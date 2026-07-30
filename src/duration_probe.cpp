#include <tlvdemux/duration_probe.hpp>

#include <tlvdemux/demuxer.hpp>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace tlvdemux {
namespace {

constexpr std::uint32_t microsecond_timescale = 1000000;

bool timestamp_microseconds(const Timestamp timestamp, std::int64_t& output) noexcept {
    if (timestamp.timescale == 0) return false;
    const auto scale = static_cast<std::int64_t>(timestamp.timescale);
    const auto whole = timestamp.value / scale;
    const auto remainder = timestamp.value % scale;
    constexpr auto factor = static_cast<std::int64_t>(microsecond_timescale);
    if (whole > std::numeric_limits<std::int64_t>::max() / factor ||
        whole < std::numeric_limits<std::int64_t>::min() / factor) {
        return false;
    }
    const auto scaled_whole = whole * factor;
    const auto fractional = remainder * factor / scale;
    if ((fractional > 0 && scaled_whole > std::numeric_limits<std::int64_t>::max() - fractional) ||
        (fractional < 0 && scaled_whole < std::numeric_limits<std::int64_t>::min() - fractional)) {
        return false;
    }
    output = scaled_whole + fractional;
    return true;
}

std::optional<std::int64_t> timestamp_distance(const std::int64_t first,
                                               const std::int64_t second) noexcept {
    if (first == second) return std::nullopt;
    const auto difference = first > second
        ? static_cast<std::uint64_t>(first) - static_cast<std::uint64_t>(second)
        : static_cast<std::uint64_t>(second) - static_cast<std::uint64_t>(first);
    if (difference > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(difference);
}

} // namespace

class DurationProbe::Impl final : public Sink {
public:
    Impl() : demuxer_(*this) {}

    bool begin(const std::uint64_t source_size, DurationProbeOptions options) {
        ++generation_;
        if (generation_ == 0) ++generation_;
        state_ = DurationProbeState::Idle;
        failure_ = DurationProbeFailure::None;
        duration_ = {};
        source_size_ = source_size;
        options_ = std::move(options);
        transferred_bytes_ = 0;
        request_.reset();
        request_received_ = 0;
        head_end_ = 0;
        head_video_count_ = 0;
        head_previous_pts_us_.reset();
        head_maximum_pts_us_.reset();
        head_frame_duration_us_ = 0;
        selected_video_track_.reset();
        tail_window_ = 0;
        reset_tail_statistics();
        demuxer_.reset();

        if (source_size_ == 0 || options_.initial_range_size == 0 ||
            options_.max_range_size < options_.initial_range_size) {
            unknown(DurationProbeFailure::InvalidSource);
            return false;
        }
        demuxer_.selectService(options_.service_context_id);
        phase_ = Phase::Head;
        issue_request(0, std::min(source_size_, options_.initial_range_size));
        return true;
    }

    std::optional<RangeRequest> next_range() const noexcept {
        if (state_ != DurationProbeState::NeedRange) return std::nullopt;
        return request_;
    }

    bool push_range(const std::uint64_t request_id, const std::uint64_t absolute_offset,
                    const std::uint8_t* data, const std::size_t size,
                    const bool end_of_range) {
        if (state_ != DurationProbeState::NeedRange || !request_.has_value() ||
            request_->request_id != request_id) {
            return false;
        }
        const auto expected_offset = request_->offset + request_received_;
        const auto remaining = request_->length - request_received_;
        if (absolute_offset != expected_offset || size > remaining ||
            (data == nullptr && size != 0)) {
            fail(DurationProbeFailure::InvalidResponse);
            return false;
        }

        if (size != 0) demuxer_.push(data, size);
        if (state_ != DurationProbeState::NeedRange) return false;
        request_received_ += static_cast<std::uint64_t>(size);
        transferred_bytes_ += static_cast<std::uint64_t>(size);
        if (!end_of_range) return true;
        if (request_received_ != request_->length) {
            fail(DurationProbeFailure::InvalidResponse);
            return false;
        }
        finish_request();
        return true;
    }

    bool fail_range(const std::uint64_t request_id) {
        if (state_ != DurationProbeState::NeedRange || !request_.has_value() ||
            request_->request_id != request_id) {
            return false;
        }
        fail(DurationProbeFailure::SourceError);
        return true;
    }

    void cancel() noexcept {
        if (state_ == DurationProbeState::Complete || state_ == DurationProbeState::Unknown ||
            state_ == DurationProbeState::Failed) {
            return;
        }
        ++generation_;
        if (generation_ == 0) ++generation_;
        request_.reset();
        state_ = DurationProbeState::Cancelled;
        failure_ = DurationProbeFailure::None;
    }

    DurationProbeState state() const noexcept { return state_; }
    DurationProbeFailure failure() const noexcept { return failure_; }
    DurationInfo duration() const noexcept { return duration_; }
    std::uint64_t generation() const noexcept { return generation_; }
    std::uint64_t transferred_bytes() const noexcept { return transferred_bytes_; }

    void onService(const ServiceInfo&) override {}

    void onTrack(const TrackInfo& info) override {
        if (info.kind != TrackKind::Video || info.codec != Codec::Hevc) return;
        if (options_.video_packet_id.has_value() &&
            info.packet_id != *options_.video_packet_id) {
            return;
        }
        if (!selected_video_track_.has_value()) {
            selected_video_track_ = info.track_id;
            demuxer_.selectTrack(TrackKind::Video, info.track_id);
        }
    }

    void onAccessUnit(AccessUnit&& unit) override {
        if (!selected_video_track_.has_value() || unit.track_id != *selected_video_track_ ||
            unit.codec != Codec::Hevc) {
            return;
        }
        std::int64_t pts_us = 0;
        if (!timestamp_microseconds(unit.pts, pts_us)) return;
        if (phase_ == Phase::Head || phase_ == Phase::SequentialTail) {
            ++head_video_count_;
            observe_timestamp(pts_us, head_previous_pts_us_, head_maximum_pts_us_,
                              head_frame_duration_us_);
        } else if (phase_ == Phase::Tail) {
            ++tail_video_count_;
            observe_timestamp(pts_us, tail_previous_pts_us_, tail_maximum_pts_us_,
                              tail_frame_duration_us_);
        }
    }

    void onError(const Error& error) override {
        if (!error.recoverable && state_ == DurationProbeState::NeedRange) {
            fail(DurationProbeFailure::ParseError);
        }
    }

private:
    enum class Phase { Head, SequentialTail, Tail };

    static void observe_timestamp(const std::int64_t pts_us,
                                  std::optional<std::int64_t>& previous,
                                  std::optional<std::int64_t>& maximum,
                                  std::int64_t& frame_duration) noexcept {
        if (previous.has_value()) {
            const auto distance = timestamp_distance(pts_us, *previous);
            if (distance.has_value() &&
                (frame_duration == 0 || *distance < frame_duration)) {
                frame_duration = *distance;
            }
        }
        previous = pts_us;
        if (!maximum.has_value() || pts_us > *maximum) maximum = pts_us;
    }

    void issue_request(const std::uint64_t offset, const std::uint64_t length) {
        ++next_request_id_;
        if (next_request_id_ == 0) ++next_request_id_;
        request_ = RangeRequest{generation_, next_request_id_, offset, length};
        request_received_ = 0;
        state_ = DurationProbeState::NeedRange;
    }

    void finish_request() {
        const auto completed = *request_;
        request_.reset();
        if (phase_ == Phase::Head) {
            head_end_ = completed.offset + completed.length;
            finish_head_range();
            return;
        }
        if (phase_ == Phase::SequentialTail) {
            demuxer_.flush();
            if (state_ != DurationProbeState::NeedRange) return;
            complete_from_statistics(head_video_count_, head_maximum_pts_us_,
                                     head_frame_duration_us_);
            return;
        }

        demuxer_.flush();
        if (state_ != DurationProbeState::NeedRange) return;
        if (tail_video_count_ >= 2 && tail_maximum_pts_us_.has_value() &&
            tail_frame_duration_us_ > 0) {
            complete_from_statistics(tail_video_count_, tail_maximum_pts_us_,
                                     tail_frame_duration_us_);
            return;
        }
        widen_tail();
    }

    void finish_head_range() {
        if (selected_video_track_.has_value() && head_video_count_ != 0) {
            if (head_end_ == source_size_) {
                demuxer_.flush();
                if (state_ != DurationProbeState::NeedRange) return;
                complete_from_statistics(head_video_count_, head_maximum_pts_us_,
                                         head_frame_duration_us_);
                return;
            }
            const auto remaining = source_size_ - head_end_;
            if (remaining <= options_.initial_range_size) {
                phase_ = Phase::SequentialTail;
                issue_request(head_end_, remaining);
                return;
            }
            tail_window_ = std::min(options_.initial_range_size, source_size_);
            issue_tail_request();
            return;
        }

        const auto current_window = head_end_;
        const auto doubled = current_window > options_.max_range_size / 2
            ? options_.max_range_size
            : current_window * 2;
        const auto target = std::min(source_size_, std::min(options_.max_range_size, doubled));
        if (target <= head_end_) {
            unknown(DurationProbeFailure::NoVideo);
            return;
        }
        issue_request(head_end_, target - head_end_);
    }

    void issue_tail_request() {
        reset_tail_statistics();
        const auto offset = source_size_ - std::min(source_size_, tail_window_);
        demuxer_.reposition(RepositionOptions{offset, true});
        phase_ = Phase::Tail;
        issue_request(offset, source_size_ - offset);
    }

    void widen_tail() {
        if (tail_window_ >= options_.max_range_size || tail_window_ >= source_size_) {
            unknown(tail_video_count_ == 0
                        ? DurationProbeFailure::NoTailTimestamp
                        : DurationProbeFailure::RangeLimit);
            return;
        }
        tail_window_ = tail_window_ > options_.max_range_size / 2
            ? options_.max_range_size
            : std::min(options_.max_range_size, tail_window_ * 2);
        issue_tail_request();
    }

    void complete_from_statistics(const std::uint64_t count,
                                  const std::optional<std::int64_t> maximum,
                                  const std::int64_t frame_duration) {
        if (count < 2 || !maximum.has_value() || frame_duration <= 0) {
            unknown(DurationProbeFailure::NoTailTimestamp);
            return;
        }
        auto value = std::max<std::int64_t>(0, *maximum);
        if (value <= std::numeric_limits<std::int64_t>::max() - frame_duration) {
            value += frame_duration;
        }
        duration_ = DurationInfo{Timestamp{value, microsecond_timescale},
                                 DurationStatus::Complete};
        state_ = DurationProbeState::Complete;
        failure_ = DurationProbeFailure::None;
    }

    void reset_tail_statistics() noexcept {
        tail_video_count_ = 0;
        tail_previous_pts_us_.reset();
        tail_maximum_pts_us_.reset();
        tail_frame_duration_us_ = 0;
    }

    void unknown(const DurationProbeFailure failure) noexcept {
        request_.reset();
        duration_ = {};
        state_ = DurationProbeState::Unknown;
        failure_ = failure;
    }

    void fail(const DurationProbeFailure failure) noexcept {
        request_.reset();
        duration_ = {};
        state_ = DurationProbeState::Failed;
        failure_ = failure;
    }

    Demuxer demuxer_;
    DurationProbeOptions options_;
    DurationProbeState state_ = DurationProbeState::Idle;
    DurationProbeFailure failure_ = DurationProbeFailure::None;
    DurationInfo duration_;
    Phase phase_ = Phase::Head;
    std::uint64_t source_size_ = 0;
    std::uint64_t generation_ = 0;
    std::uint64_t next_request_id_ = 0;
    std::optional<RangeRequest> request_;
    std::uint64_t request_received_ = 0;
    std::uint64_t transferred_bytes_ = 0;
    std::uint64_t head_end_ = 0;
    std::uint64_t head_video_count_ = 0;
    std::optional<std::uint64_t> selected_video_track_;
    std::optional<std::int64_t> head_previous_pts_us_;
    std::optional<std::int64_t> head_maximum_pts_us_;
    std::int64_t head_frame_duration_us_ = 0;
    std::uint64_t tail_window_ = 0;
    std::uint64_t tail_video_count_ = 0;
    std::optional<std::int64_t> tail_previous_pts_us_;
    std::optional<std::int64_t> tail_maximum_pts_us_;
    std::int64_t tail_frame_duration_us_ = 0;
};

DurationProbe::DurationProbe() : impl_(std::make_unique<Impl>()) {}
DurationProbe::~DurationProbe() = default;
DurationProbe::DurationProbe(DurationProbe&&) noexcept = default;
DurationProbe& DurationProbe::operator=(DurationProbe&&) noexcept = default;

bool DurationProbe::begin(const std::uint64_t source_size, DurationProbeOptions options) {
    return impl_->begin(source_size, std::move(options));
}

std::optional<RangeRequest> DurationProbe::nextRange() const noexcept {
    return impl_->next_range();
}

bool DurationProbe::pushRange(const std::uint64_t request_id,
                              const std::uint64_t absolute_offset,
                              const std::uint8_t* data, const std::size_t size,
                              const bool end_of_range) {
    return impl_->push_range(request_id, absolute_offset, data, size, end_of_range);
}

bool DurationProbe::failRange(const std::uint64_t request_id) {
    return impl_->fail_range(request_id);
}

void DurationProbe::cancel() noexcept { impl_->cancel(); }
DurationProbeState DurationProbe::state() const noexcept { return impl_->state(); }
DurationProbeFailure DurationProbe::failure() const noexcept { return impl_->failure(); }
DurationInfo DurationProbe::duration() const noexcept { return impl_->duration(); }
std::uint64_t DurationProbe::generation() const noexcept { return impl_->generation(); }
std::uint64_t DurationProbe::transferredBytes() const noexcept {
    return impl_->transferred_bytes();
}

} // namespace tlvdemux
