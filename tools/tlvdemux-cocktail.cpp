#include <tlvdemux/demuxer.hpp>
#include <tlvdemux/playback.hpp>
#include <tlvdemux/recording.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr std::size_t read_size = 1024 * 1024;
constexpr std::uint64_t progress_interval = 512ULL * 1024ULL * 1024ULL;

std::optional<std::int64_t> timestamp_us(const tlvdemux::Timestamp timestamp) {
    if (timestamp.timescale == 0) return std::nullopt;
    const auto scale = static_cast<std::int64_t>(timestamp.timescale);
    const auto whole = timestamp.value / scale;
    const auto remainder = timestamp.value % scale;
    constexpr std::int64_t factor = 1000000;
    if (whole > std::numeric_limits<std::int64_t>::max() / factor ||
        whole < std::numeric_limits<std::int64_t>::min() / factor) {
        return std::nullopt;
    }
    return whole * factor + remainder * factor / scale;
}

std::string seconds(const std::int64_t microseconds) {
    const auto value = static_cast<long double>(microseconds) / 1000000.0L;
    std::ostringstream output;
    output << std::fixed << std::setprecision(6) << value;
    return output.str();
}

struct FrameStamp {
    std::int64_t pts_us = 0;
    std::uint64_t input_offset = 0;
    bool random_access = false;
};

class CocktailSink final : public tlvdemux::Sink {
public:
    enum class Phase { Indexing, Seeking };

    explicit CocktailSink(const std::optional<std::uint16_t> video_packet_id)
        : requested_video_packet_id_(video_packet_id) {}

    void onService(const tlvdemux::ServiceInfo&) override {}

    void onLayoutConfiguration(const tlvdemux::LayoutConfiguration&) override {
        ++layout_callbacks_[static_cast<std::size_t>(phase_)];
    }

    void onApplication(const tlvdemux::ApplicationInfo&) override {
        ++application_callbacks_[static_cast<std::size_t>(phase_)];
    }

    void onStreamEvent(const tlvdemux::StreamEvent&) override {
        ++stream_event_callbacks_[static_cast<std::size_t>(phase_)];
    }

    void onTrack(const tlvdemux::TrackInfo& track) override {
        tracks_[track.track_id] = track;
        if (!video_track_.has_value() && track.kind == tlvdemux::TrackKind::Video &&
            (!requested_video_packet_id_.has_value() ||
             track.packet_id == *requested_video_packet_id_)) {
            video_track_ = track.track_id;
            video_packet_id_ = track.packet_id;
            index_.selectVideoTrack(video_track_);
        }
    }

    void onAccessUnit(tlvdemux::AccessUnit&& unit) override {
        ++access_unit_count_;
        if (phase_ == Phase::Indexing) {
            if (video_track_ == unit.track_id) {
                if (unit.random_access) ++random_access_video_units_;
                if (!index_.observe(unit)) ++rejected_video_index_units_;
                countVideoNalTypes(unit.data);
                const auto pts = timestamp_us(unit.pts);
                if (pts.has_value()) {
                    frames_.push_back(FrameStamp{*pts, unit.input_offset, unit.random_access});
                }
            }
            return;
        }

        if (video_track_ != unit.track_id) return;
        const auto pts = timestamp_us(unit.pts);
        if (!pts.has_value()) return;
        ++seek_video_units_;
        if (!first_video_seen_) {
            first_video_seen_ = true;
            first_video_pts_us_ = *pts;
            first_video_offset_ = unit.input_offset;
            first_video_restart_offset_ = unit.restart_offset;
            first_video_random_access_ = unit.random_access;
            first_video_discontinuity_ = unit.discontinuity;
        }
        if (*pts == expected_landing_pts_us_) {
            landed_ = true;
            landing_pts_us_ = *pts;
            landing_offset_ = unit.input_offset;
        }
    }

    void onError(const tlvdemux::Error& error) override {
        ++error_counts_[static_cast<unsigned>(error.code)];
        if (!error.recoverable) ++fatal_errors_;
    }

    void beginSeek(const std::int64_t target_us, const std::int64_t expected_landing_us) {
        phase_ = Phase::Seeking;
        target_pts_us_ = target_us;
        expected_landing_pts_us_ = expected_landing_us;
        landed_ = false;
        first_video_seen_ = false;
        first_video_random_access_ = false;
        first_video_discontinuity_ = false;
        first_video_pts_us_ = 0;
        first_video_offset_ = 0;
        first_video_restart_offset_ = 0;
        landing_pts_us_ = 0;
        landing_offset_ = 0;
        seek_video_units_ = 0;
    }

    void finishIndex() {
        index_.finalize();
        std::sort(frames_.begin(), frames_.end(), [](const FrameStamp& left,
                                                      const FrameStamp& right) {
            if (left.pts_us != right.pts_us) return left.pts_us < right.pts_us;
            return left.input_offset < right.input_offset;
        });
        frames_.erase(std::unique(frames_.begin(), frames_.end(),
                                  [](const FrameStamp& left, const FrameStamp& right) {
                                      return left.pts_us == right.pts_us;
                                  }),
                      frames_.end());
    }

    std::optional<FrameStamp> frameAtOrAfter(const std::int64_t target_us,
                                             const std::int64_t duration_us) const {
        const auto found = std::lower_bound(
            frames_.begin(), frames_.end(), target_us,
            [](const FrameStamp& frame, const std::int64_t value) {
                return frame.pts_us < value;
            });
        if (found == frames_.end()) {
            if (!frames_.empty() && target_us < duration_us) return frames_.back();
            return std::nullopt;
        }
        return *found;
    }

    tlvdemux::RecordingIndex& index() noexcept { return index_; }
    const tlvdemux::RecordingIndex& index() const noexcept { return index_; }
    const std::vector<FrameStamp>& frames() const noexcept { return frames_; }
    std::optional<std::uint64_t> videoTrack() const noexcept { return video_track_; }
    std::optional<std::uint16_t> videoPacketId() const noexcept { return video_packet_id_; }
    std::uint64_t accessUnitCount() const noexcept { return access_unit_count_; }
    std::uint64_t randomAccessVideoUnits() const noexcept {
        return random_access_video_units_;
    }
    std::uint64_t rejectedVideoIndexUnits() const noexcept {
        return rejected_video_index_units_;
    }
    const std::unordered_map<unsigned, std::uint64_t>& errorCounts() const noexcept {
        return error_counts_;
    }
    const std::array<std::uint64_t, 64>& videoNalTypeCounts() const noexcept {
        return video_nal_type_counts_;
    }
    std::uint64_t layoutCallbacks(const Phase phase) const noexcept {
        return layout_callbacks_[static_cast<std::size_t>(phase)];
    }
    std::uint64_t applicationCallbacks(const Phase phase) const noexcept {
        return application_callbacks_[static_cast<std::size_t>(phase)];
    }
    std::uint64_t streamEventCallbacks(const Phase phase) const noexcept {
        return stream_event_callbacks_[static_cast<std::size_t>(phase)];
    }
    std::uint64_t fatalErrors() const noexcept { return fatal_errors_; }

    bool landed() const noexcept { return landed_; }
    bool firstVideoSeen() const noexcept { return first_video_seen_; }
    bool firstVideoRandomAccess() const noexcept { return first_video_random_access_; }
    bool firstVideoDiscontinuity() const noexcept { return first_video_discontinuity_; }
    std::int64_t firstVideoPtsUs() const noexcept { return first_video_pts_us_; }
    std::uint64_t firstVideoOffset() const noexcept { return first_video_offset_; }
    std::uint64_t firstVideoRestartOffset() const noexcept {
        return first_video_restart_offset_;
    }
    std::int64_t landingPtsUs() const noexcept { return landing_pts_us_; }
    std::uint64_t landingOffset() const noexcept { return landing_offset_; }
    std::uint64_t seekVideoUnits() const noexcept { return seek_video_units_; }

private:
    void countVideoNalTypes(const std::vector<std::uint8_t>& data) {
        for (std::size_t index = 0; index + 4 <= data.size(); ++index) {
            if (data[index] != 0 || data[index + 1] != 0 || data[index + 2] != 1) continue;
            const auto type = static_cast<std::size_t>((data[index + 3] >> 1U) & 0x3fU);
            ++video_nal_type_counts_[type];
            index += 2;
        }
    }

    Phase phase_ = Phase::Indexing;
    std::optional<std::uint16_t> requested_video_packet_id_;
    tlvdemux::RecordingIndex index_;
    std::unordered_map<std::uint64_t, tlvdemux::TrackInfo> tracks_;
    std::optional<std::uint64_t> video_track_;
    std::optional<std::uint16_t> video_packet_id_;
    std::vector<FrameStamp> frames_;
    std::unordered_map<unsigned, std::uint64_t> error_counts_;
    std::array<std::uint64_t, 64> video_nal_type_counts_{};
    std::array<std::uint64_t, 2> layout_callbacks_{};
    std::array<std::uint64_t, 2> application_callbacks_{};
    std::array<std::uint64_t, 2> stream_event_callbacks_{};
    std::uint64_t fatal_errors_ = 0;
    std::uint64_t access_unit_count_ = 0;
    std::uint64_t random_access_video_units_ = 0;
    std::uint64_t rejected_video_index_units_ = 0;
    std::int64_t target_pts_us_ = 0;
    std::int64_t expected_landing_pts_us_ = 0;
    bool landed_ = false;
    bool first_video_seen_ = false;
    bool first_video_random_access_ = false;
    bool first_video_discontinuity_ = false;
    std::int64_t first_video_pts_us_ = 0;
    std::uint64_t first_video_offset_ = 0;
    std::uint64_t first_video_restart_offset_ = 0;
    std::int64_t landing_pts_us_ = 0;
    std::uint64_t landing_offset_ = 0;
    std::uint64_t seek_video_units_ = 0;
};

struct Options {
    std::string path;
    std::size_t cases = 12;
    std::uint64_t seed = 0x746c7664656d7578ULL;
    std::uint64_t max_seek_bytes = 256ULL * 1024ULL * 1024ULL;
    std::optional<std::uint16_t> video_packet_id;
    std::optional<std::uint16_t> switch_video_packet_id;
};

void usage() {
    std::cerr << "usage: tlvdemux-cocktail [--cases N] [--seed N]"
                 " [--max-seek-bytes N] [--video-packet-id ID]"
                 " [--switch-video-packet-id ID] INPUT\n";
}

Options parse_options(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto value = [&](const char* name) -> std::string {
            if (++index >= argc) throw std::runtime_error(std::string("missing value for ") + name);
            return argv[index];
        };
        if (argument == "--cases") {
            options.cases = static_cast<std::size_t>(std::stoull(value("--cases"), nullptr, 0));
        } else if (argument == "--seed") {
            options.seed = std::stoull(value("--seed"), nullptr, 0);
        } else if (argument == "--max-seek-bytes") {
            options.max_seek_bytes = std::stoull(value("--max-seek-bytes"), nullptr, 0);
        } else if (argument == "--video-packet-id") {
            const auto parsed = std::stoull(value("--video-packet-id"), nullptr, 0);
            if (parsed > std::numeric_limits<std::uint16_t>::max()) {
                throw std::runtime_error("--video-packet-id exceeds 16 bits");
            }
            options.video_packet_id = static_cast<std::uint16_t>(parsed);
        } else if (argument == "--switch-video-packet-id") {
            const auto parsed = std::stoull(
                value("--switch-video-packet-id"), nullptr, 0);
            if (parsed > std::numeric_limits<std::uint16_t>::max()) {
                throw std::runtime_error("--switch-video-packet-id exceeds 16 bits");
            }
            options.switch_video_packet_id = static_cast<std::uint16_t>(parsed);
        } else if (argument == "-h" || argument == "--help") {
            usage();
            std::exit(0);
        } else if (!argument.empty() && argument[0] == '-') {
            throw std::runtime_error("unknown option: " + argument);
        } else if (options.path.empty()) {
            options.path = argument;
        } else {
            throw std::runtime_error("more than one input path was provided");
        }
    }
    if (options.path.empty()) throw std::runtime_error("input path is required");
    if (options.cases == 0) throw std::runtime_error("--cases must be positive");
    if (options.max_seek_bytes < read_size) {
        throw std::runtime_error("--max-seek-bytes is smaller than the read block");
    }
    return options;
}

std::uint64_t file_size(std::ifstream& file) {
    file.seekg(0, std::ios::end);
    const auto end = file.tellg();
    if (end < 0) throw std::runtime_error("cannot determine input size");
    file.seekg(0, std::ios::beg);
    return static_cast<std::uint64_t>(end);
}

bool run_seek_case(std::ifstream& file, tlvdemux::Demuxer& demuxer,
                   CocktailSink& sink, tlvdemux::PlaybackStateMachine& state,
                   const tlvdemux::SeekPoint& point, const FrameStamp expected,
                   const std::int64_t target_us, const std::uint64_t source_size,
                   const std::uint64_t max_seek_bytes, const std::size_t case_number) {
    const auto generation = state.requestSeek();
    if (!generation.has_value() || !state.beginReposition(*generation)) {
        throw std::runtime_error("state machine rejected seek request");
    }
    demuxer.reposition(tlvdemux::RepositionOptions{point.signalling_offset, true});
    if (!state.beginPriming(*generation) || !state.beginPreroll(*generation)) {
        throw std::runtime_error("state machine rejected seek priming");
    }

    file.clear();
    file.seekg(static_cast<std::streamoff>(point.signalling_offset), std::ios::beg);
    if (!file) throw std::runtime_error("cannot seek input to signalling checkpoint");
    sink.beginSeek(target_us, expected.pts_us);

    std::array<std::uint8_t, read_size> buffer{};
    std::uint64_t bytes_read = 0;
    while (!sink.landed() && bytes_read < max_seek_bytes && file) {
        const auto remaining = max_seek_bytes - bytes_read;
        const auto wanted = static_cast<std::streamsize>(
            std::min<std::uint64_t>(buffer.size(), remaining));
        file.read(reinterpret_cast<char*>(buffer.data()), wanted);
        const auto count = file.gcount();
        if (count <= 0) break;
        demuxer.push(buffer.data(), static_cast<std::size_t>(count));
        bytes_read += static_cast<std::uint64_t>(count);
    }
    if (!sink.landed() && file.eof()) demuxer.flush();

    const bool landed = sink.landed();
    const bool first_video_seen = sink.firstVideoSeen();
    const bool first_video_random_access = sink.firstVideoRandomAccess();
    const bool first_video_discontinuity = sink.firstVideoDiscontinuity();
    const bool first_pts_matches = first_video_seen &&
        sink.firstVideoPtsUs() == point.presentation_time.value;
    const bool first_offset_matches = first_video_seen &&
        sink.firstVideoOffset() == point.random_access_offset;
    const bool restart_offset_matches = first_video_seen &&
        sink.firstVideoRestartOffset() == point.signalling_offset;
    const bool landing_matches = landed && sink.landingPtsUs() == expected.pts_us;
    const bool passed = landed && first_video_seen && first_video_random_access &&
        first_video_discontinuity && first_pts_matches && first_offset_matches &&
        restart_offset_matches && landing_matches;

    const auto estimate = sink.index().estimateOffset(
        tlvdemux::Timestamp{target_us, 1000000}, source_size);
    const auto estimate_error = estimate.has_value()
        ? (*estimate > point.random_access_offset
               ? *estimate - point.random_access_offset
               : point.random_access_offset - *estimate)
        : 0;

    std::cerr << "case=" << case_number
              << " target=" << seconds(target_us)
              << " rap=" << seconds(point.presentation_time.value)
              << " checkpoint=" << point.signalling_offset
              << " landing=" << (sink.landed() ? seconds(sink.landingPtsUs()) : "missing")
              << " frames=" << sink.seekVideoUnits()
              << " read=" << bytes_read
              << " estimate-error=" << (estimate.has_value() ? std::to_string(estimate_error) : "n/a")
              << " result=" << (passed ? "PASS" : "FAIL");
    if (!passed) {
        std::cerr << " checks="
                  << "landed:" << landed
                  << ",first-video:" << first_video_seen
                  << ",rap:" << first_video_random_access
                  << ",discontinuity:" << first_video_discontinuity
                  << ",first-pts:" << first_pts_matches
                  << ",first-offset:" << first_offset_matches
                  << ",restart-offset:" << restart_offset_matches
                  << ",landing:" << landing_matches;
        if (first_video_seen) {
            std::cerr << " actual-first-pts=" << seconds(sink.firstVideoPtsUs())
                      << " expected-first-pts=" << seconds(point.presentation_time.value)
                      << " actual-first-offset=" << sink.firstVideoOffset()
                      << " expected-first-offset=" << point.random_access_offset
                      << " actual-restart-offset=" << sink.firstVideoRestartOffset()
                      << " expected-restart-offset=" << point.signalling_offset;
        }
        if (landed) {
            std::cerr << " actual-landing=" << seconds(sink.landingPtsUs())
                      << " expected-landing=" << seconds(expected.pts_us);
        }
    }
    std::cerr << '\n';

    if (passed) {
        if (!state.beginLanding(*generation) || !state.completeSeek(*generation)) {
            throw std::runtime_error("state machine rejected seek landing");
        }
    } else {
        state.failSeek(*generation);
        state.finishFailedSeek(*generation);
    }
    return passed;
}

class TrackSwitchSink final : public tlvdemux::Sink {
public:
    TrackSwitchSink(const std::uint16_t primary_packet_id,
                    const std::uint16_t secondary_packet_id)
        : primary_packet_id_(primary_packet_id),
          secondary_packet_id_(secondary_packet_id) {}

    void onService(const tlvdemux::ServiceInfo&) override {}

    void onTrack(const tlvdemux::TrackInfo& track) override {
        if (track.kind != tlvdemux::TrackKind::Video) return;
        if (track.packet_id == primary_packet_id_) primary_track_id_ = track.track_id;
        if (track.packet_id == secondary_packet_id_) secondary_track_id_ = track.track_id;
    }

    void onAccessUnit(tlvdemux::AccessUnit&& unit) override {
        if (!expected_track_id_.has_value() || unit.codec != tlvdemux::Codec::Hevc) return;
        if (unit.track_id != *expected_track_id_) {
            wrong_track_seen_ = true;
            return;
        }
        const auto pts = timestamp_us(unit.pts);
        if (!pts.has_value()) return;
        if (!first_seen_) {
            first_seen_ = true;
            first_pts_us_ = *pts;
            first_random_access_ = unit.random_access;
            first_discontinuity_ = unit.discontinuity;
        }
        latest_pts_us_ = *pts;
        ++video_units_;
    }

    void onError(const tlvdemux::Error&) override {}

    void begin(const std::uint64_t track_id) {
        expected_track_id_ = track_id;
        wrong_track_seen_ = false;
        first_seen_ = false;
        first_random_access_ = false;
        first_discontinuity_ = false;
        first_pts_us_ = 0;
        latest_pts_us_ = 0;
        video_units_ = 0;
    }

    bool readyToSwitch() const noexcept {
        return first_seen_ && video_units_ >= 16 &&
            latest_pts_us_ - first_pts_us_ >= 1000000;
    }

    bool validFirstOutput() const noexcept {
        return first_seen_ && first_random_access_ && first_discontinuity_ &&
            !wrong_track_seen_;
    }

    std::optional<std::uint64_t> primaryTrackId() const noexcept {
        return primary_track_id_;
    }
    std::optional<std::uint64_t> secondaryTrackId() const noexcept {
        return secondary_track_id_;
    }
    bool firstSeen() const noexcept { return first_seen_; }
    bool wrongTrackSeen() const noexcept { return wrong_track_seen_; }
    std::int64_t firstPtsUs() const noexcept { return first_pts_us_; }
    std::int64_t latestPtsUs() const noexcept { return latest_pts_us_; }

private:
    std::uint16_t primary_packet_id_ = 0;
    std::uint16_t secondary_packet_id_ = 0;
    std::optional<std::uint64_t> primary_track_id_;
    std::optional<std::uint64_t> secondary_track_id_;
    std::optional<std::uint64_t> expected_track_id_;
    bool wrong_track_seen_ = false;
    bool first_seen_ = false;
    bool first_random_access_ = false;
    bool first_discontinuity_ = false;
    std::int64_t first_pts_us_ = 0;
    std::int64_t latest_pts_us_ = 0;
    std::uint64_t video_units_ = 0;
};

bool run_track_switch_case(const std::string& path,
                           const std::uint16_t primary_packet_id,
                           const std::uint16_t secondary_packet_id) {
    if (primary_packet_id == secondary_packet_id) {
        throw std::runtime_error("track-switch packet IDs are identical");
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot reopen input for track switching");

    TrackSwitchSink sink(primary_packet_id, secondary_packet_id);
    tlvdemux::Demuxer demuxer(sink);
    constexpr std::size_t switch_read_size = 64 * 1024;
    std::array<std::uint8_t, switch_read_size> buffer{};
    enum class Stage { Discovering, Primary, Secondary, Returned };
    auto stage = Stage::Discovering;
    std::int64_t primary_first_us = 0;
    std::int64_t primary_last_us = 0;
    std::int64_t secondary_first_us = 0;
    std::int64_t secondary_last_us = 0;

    while (file) {
        file.read(reinterpret_cast<char*>(buffer.data()),
                  static_cast<std::streamsize>(buffer.size()));
        const auto count = file.gcount();
        if (count <= 0) break;
        demuxer.push(buffer.data(), static_cast<std::size_t>(count));

        if (sink.wrongTrackSeen()) break;
        if (stage == Stage::Discovering && sink.primaryTrackId().has_value() &&
            sink.secondaryTrackId().has_value()) {
            demuxer.selectTrack(tlvdemux::TrackKind::Video, sink.primaryTrackId());
            sink.begin(*sink.primaryTrackId());
            stage = Stage::Primary;
        } else if (stage == Stage::Primary && sink.readyToSwitch()) {
            if (!sink.validFirstOutput()) break;
            primary_first_us = sink.firstPtsUs();
            primary_last_us = sink.latestPtsUs();
            demuxer.selectTrack(tlvdemux::TrackKind::Video, sink.secondaryTrackId());
            sink.begin(*sink.secondaryTrackId());
            stage = Stage::Secondary;
        } else if (stage == Stage::Secondary && sink.readyToSwitch()) {
            if (!sink.validFirstOutput()) break;
            secondary_first_us = sink.firstPtsUs();
            secondary_last_us = sink.latestPtsUs();
            demuxer.selectTrack(tlvdemux::TrackKind::Video, sink.primaryTrackId());
            sink.begin(*sink.primaryTrackId());
            stage = Stage::Returned;
        } else if (stage == Stage::Returned && sink.firstSeen()) {
            break;
        }
    }

    const bool passed = stage == Stage::Returned && sink.validFirstOutput() &&
        primary_first_us >= 0 && secondary_first_us > 0 && sink.firstPtsUs() > 0 &&
        secondary_first_us + 1000000 >= primary_last_us &&
        sink.firstPtsUs() + 1000000 >= secondary_last_us;
    std::cerr << "track-switch primary-pid=0x" << std::hex << primary_packet_id
              << " secondary-pid=0x" << secondary_packet_id << std::dec
              << " primary-first=" << seconds(primary_first_us)
              << " secondary-first=" << seconds(secondary_first_us)
              << " return-first=" << seconds(sink.firstPtsUs())
              << " result=" << (passed ? "PASS" : "FAIL") << '\n';
    return passed;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        std::ifstream file(options.path, std::ios::binary);
        if (!file) throw std::runtime_error("cannot open input: " + options.path);
        const auto size = file_size(file);

        CocktailSink sink(options.video_packet_id);
        sink.index().begin(false);
        tlvdemux::Demuxer demuxer(sink);
        std::array<std::uint8_t, read_size> buffer{};
        std::uint64_t consumed = 0;
        std::uint64_t next_progress = progress_interval;
        const auto started = std::chrono::steady_clock::now();
        while (file) {
            file.read(reinterpret_cast<char*>(buffer.data()),
                      static_cast<std::streamsize>(buffer.size()));
            const auto count = file.gcount();
            if (count <= 0) break;
            demuxer.push(buffer.data(), static_cast<std::size_t>(count));
            consumed += static_cast<std::uint64_t>(count);
            if (consumed >= next_progress) {
                const auto percent = static_cast<long double>(consumed) * 100.0L /
                    static_cast<long double>(size);
                std::cerr << "index-progress=" << std::fixed << std::setprecision(1)
                          << percent << "% bytes=" << consumed << '\n';
                next_progress += progress_interval;
            }
        }
        demuxer.flush();
        sink.finishIndex();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        const auto duration = sink.index().duration();
        if (!sink.videoTrack().has_value() || sink.index().seekPoints().empty() ||
            sink.frames().empty() || duration.status != tlvdemux::DurationStatus::Complete) {
            throw std::runtime_error("baseline scan did not produce a complete video index");
        }

        std::cerr << "baseline bytes=" << size
                  << " duration=" << seconds(duration.value.value)
                  << " frames=" << sink.frames().size()
                  << " seek-points=" << sink.index().seekPoints().size()
                  << " random-video-aus=" << sink.randomAccessVideoUnits()
                  << " rejected-video-index=" << sink.rejectedVideoIndexUnits()
                  << " access-units=" << sink.accessUnitCount()
                  << " layouts=" << sink.layoutCallbacks(CocktailSink::Phase::Indexing)
                  << " applications=" << sink.applicationCallbacks(CocktailSink::Phase::Indexing)
                  << " stream-events=" << sink.streamEventCallbacks(CocktailSink::Phase::Indexing)
                  << " elapsed-ms=" << elapsed.count() << '\n';
        std::cerr << "video-nal-types=";
        bool first_nal_type = true;
        for (std::size_t type = 0; type < sink.videoNalTypeCounts().size(); ++type) {
            const auto count = sink.videoNalTypeCounts()[type];
            if (count == 0) continue;
            if (!first_nal_type) std::cerr << ',';
            std::cerr << type << ':' << count;
            first_nal_type = false;
        }
        std::cerr << '\n';

        tlvdemux::SourceCapabilities capabilities;
        capabilities.random_access = true;
        capabilities.size_known = true;
        capabilities.stable_identity = true;
        tlvdemux::PlaybackStateMachine state;
        const auto policy = tlvdemux::chooseSeekPolicy(capabilities, true, false);
        if (!state.beginOpen(capabilities, policy) || !state.startIndexLoading() ||
            !state.loadedIndex(true) || !state.completeOpen()) {
            throw std::runtime_error("cannot initialize playback state machine");
        }

        const auto first_time = sink.index().seekPoints().front().presentation_time.value;
        const auto last_time = duration.value.value > first_time
            ? duration.value.value - 1
            : first_time;
        std::mt19937_64 random(options.seed);
        std::uniform_int_distribution<std::int64_t> targets(first_time, last_time);
        std::vector<std::int64_t> requested;
        requested.reserve(options.cases + 4);
        requested.push_back(first_time);
        requested.push_back(
            sink.index().seekPoints()[sink.index().seekPoints().size() / 2].presentation_time.value);
        requested.push_back(sink.frames().back().pts_us);
        std::optional<std::int64_t> repeated_random_target;
        for (std::size_t index = 0; index < options.cases; ++index) {
            const auto target = targets(random);
            if (!repeated_random_target.has_value()) repeated_random_target = target;
            requested.push_back(target);
        }
        requested.push_back(*repeated_random_target); // explicit random A -> ... -> A regression

        std::size_t failures = 0;
        for (std::size_t index = 0; index < requested.size(); ++index) {
            const auto target = requested[index];
            const auto point = sink.index().previousSync(
                tlvdemux::Timestamp{target, 1000000});
            const auto expected = sink.frameAtOrAfter(target, duration.value.value);
            if (!point.has_value() || !expected.has_value()) {
                std::cerr << "case=" << index << " target=" << seconds(target)
                          << " result=FAIL reason=no-baseline-point\n";
                ++failures;
                continue;
            }
            if (!run_seek_case(file, demuxer, sink, state, *point, *expected, target,
                               size, options.max_seek_bytes, index)) {
                ++failures;
            }
        }
        if (options.switch_video_packet_id.has_value()) {
            if (!sink.videoPacketId().has_value()) {
                throw std::runtime_error("primary video packet ID was not discovered");
            }
            if (!run_track_switch_case(options.path, *sink.videoPacketId(),
                                       *options.switch_video_packet_id)) {
                ++failures;
            }
        }
        state.close();
        std::cerr << "signalling-replay layouts="
                  << sink.layoutCallbacks(CocktailSink::Phase::Seeking)
                  << " applications="
                  << sink.applicationCallbacks(CocktailSink::Phase::Seeking)
                  << " stream-events="
                  << sink.streamEventCallbacks(CocktailSink::Phase::Seeking)
                  << " fatal-errors=" << sink.fatalErrors() << '\n';
        if (sink.fatalErrors() != 0) ++failures;
        std::cerr << "cocktail cases=" << requested.size()
                  << " failures=" << failures
                  << " seed=" << options.seed << '\n';
        return failures == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "tlvdemux-cocktail: " << error.what() << '\n';
        return 2;
    }
}
