#include <tlvdemux/mse_remuxer.hpp>

#include "mse/hevc_parser.hpp"
#include "mse/latm_parser.hpp"
#include "mse/mp4_builder.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifdef __EMSCRIPTEN__
#include "mse_remuxer.hpp"
#include <emscripten/bind.h>
#endif

namespace {

using tlvdemux::detail::mse::AacFrame;
using tlvdemux::detail::mse::Bytes;
using tlvdemux::detail::mse::HevcConfiguration;
using tlvdemux::detail::mse::LatmParser;
using tlvdemux::detail::mse::Mp4Track;
using tlvdemux::detail::mse::NaluView;
using tlvdemux::detail::mse::Sample;
using tlvdemux::detail::mse::annex_b_views;
using tlvdemux::detail::mse::append;
using tlvdemux::detail::mse::copy_nalu;
using tlvdemux::detail::mse::hevc_configuration;
using tlvdemux::detail::mse::init_segment;
using tlvdemux::detail::mse::media_segment;
using tlvdemux::detail::mse::scaled;

#ifdef __EMSCRIPTEN__
using emscripten::val;
#endif

constexpr std::uint32_t kFragmentDurationDivisor = 4;

Bytes u32(const std::uint64_t value) {
    return {static_cast<std::uint8_t>(value >> 24U),
            static_cast<std::uint8_t>(value >> 16U),
            static_cast<std::uint8_t>(value >> 8U),
            static_cast<std::uint8_t>(value)};
}

#ifdef __EMSCRIPTEN__
val copy_bytes(const Bytes& bytes) {
    auto output = val::global("Uint8Array").new_(bytes.size());
    if (!bytes.empty()) {
        output.call<void>("set", emscripten::typed_memory_view(bytes.size(), bytes.data()));
    }
    return output;
}
#endif

class Output {
public:
    explicit Output(tlvdemux::MseSink& sink) : sink_(sink) {}

    void set_enabled(const bool enabled) noexcept { enabled_ = enabled; }

    void init(const std::string& type, const Mp4Track& track) {
        if (!enabled_) return;
        tlvdemux::MseTrackInit event;
        event.type = type;
        event.mime = std::string(track.video ? "video/mp4; codecs=\"" :
                                               "audio/mp4; codecs=\"") +
            track.codec + "\"";
        event.data = init_segment(track);
        event.width = track.width;
        event.height = track.height;
        event.sample_rate = track.sample_rate;
        event.channels = track.channels;
        sink_.onMseInit(std::move(event));
    }

    void segment(const std::string& type, Bytes&& data) {
        if (!enabled_) return;
        sink_.onMseSegment(tlvdemux::MseMediaSegment{type, std::move(data)});
    }

    void video_start(const int nal_type, const bool signalled) {
        if (!enabled_) return;
        sink_.onMseVideoStart(tlvdemux::MseVideoStart{nal_type, signalled});
    }

private:
    tlvdemux::MseSink& sink_;
    bool enabled_ = true;
};

class BaseMuxer {
public:
    BaseMuxer(std::string type, Output& output)
        : type_(std::move(type)), output_(output) {}
    virtual ~BaseMuxer() = default;

    void reset_samples() {
        pending_.reset();
        ready_.clear();
        ready_duration_ = 0;
        last_duration_ = 0;
    }

    void activate() {
        reset_samples();
        if (track_) output_.init(type_, *track_);
    }

    void flush() {
        if (pending_) {
            pending_->duration = last_duration_ != 0 ? last_duration_ : default_duration();
            ready_.push_back(std::move(*pending_));
            pending_.reset();
        }
        emit();
    }

protected:
    virtual std::uint32_t default_duration() const = 0;

    void set_track(Mp4Track track) {
        if (track_) return;
        track_ = std::move(track);
        output_.init(type_, *track_);
    }

    void enqueue(Sample sample) {
        if (pending_) {
            // A trun sample duration is the delta to the next decode timestamp;
            // a non-advancing DTS has no representable duration, so drop it
            // rather than fabricate one (see Firefox CtsComparator background).
            const auto delta = sample.dts - pending_->dts;
            if (delta <= 0) return;
            pending_->duration = static_cast<std::uint32_t>(delta);
            last_duration_ = pending_->duration;
            ready_duration_ += pending_->duration;
            ready_.push_back(std::move(*pending_));
        }
        pending_ = std::move(sample);
        if (track_ && ready_duration_ >=
                std::max<std::uint32_t>(1, track_->timescale / kFragmentDurationDivisor)) {
            emit();
        }
    }

    void emit() {
        if (!track_ || ready_.empty()) return;
        auto segment = media_segment(*track_, ready_, sequence_++);
        output_.segment(type_, std::move(segment));
        ready_.clear();
        ready_duration_ = 0;
    }

    std::optional<Mp4Track> track_;

private:
    std::string type_;
    Output& output_;
    std::optional<Sample> pending_;
    std::vector<Sample> ready_;
    std::uint64_t ready_duration_ = 0;
    std::uint32_t sequence_ = 1;
    std::uint32_t last_duration_ = 0;
};

class HevcMuxer final : public BaseMuxer {
public:
    explicit HevcMuxer(Output& output) : BaseMuxer("video", output), output_(output) {}

    bool started() const noexcept { return started_; }
    std::optional<std::int64_t> timeline_offset_us() const noexcept {
        return timeline_offset_us_;
    }

    void reset() {
        reset_samples();
        parameter_sets_.clear();
        track_.reset();
        started_ = false;
        no_rasl_output_ = false;
        timeline_offset_us_.reset();
    }

    void push(const tlvdemux::AccessUnit& unit, const bool output_enabled) {
        if (unit.discontinuity) {
            reset_samples();
            started_ = false;
            no_rasl_output_ = false;
            timeline_offset_us_.reset();
        }
        const auto nalus = annex_b_views(unit.data);
        for (const auto& nalu : nalus) {
            if (nalu.type >= 32 && nalu.type <= 34) {
                parameter_sets_[nalu.type] = copy_nalu(unit.data, nalu);
            }
        }
        if (!track_ && parameter_sets_.count(32) != 0 &&
            parameter_sets_.count(33) != 0 && parameter_sets_.count(34) != 0) {
            const HevcConfiguration config = hevc_configuration(
                parameter_sets_[32], parameter_sets_[33], parameter_sets_[34]);
            Mp4Track track;
            track.video = true;
            track.width = config.width;
            track.height = config.height;
            track.codec = config.codec;
            track.config = config.hvcc;
            set_track(std::move(track));
        }
        if (!track_) return;

        bool has_vcl = false;
        bool only_rasl_vcl = true;
        int irap = -1;
        for (const auto& nalu : nalus) {
            if (nalu.type < 0 || nalu.type > 31) continue;
            has_vcl = true;
            only_rasl_vcl = only_rasl_vcl && (nalu.type == 8 || nalu.type == 9);
            if (nalu.type >= 16 && nalu.type <= 21 && irap < 0) irap = nalu.type;
        }
        if (!has_vcl) return;
        if (!started_) {
            if (irap < 0) return;
            started_ = true;
            const auto first_dts_us = scaled(unit.dts.value, unit.dts.timescale, 1000000);
            timeline_offset_us_ = std::max<std::int64_t>(0, -first_dts_us);
            // The first CRA starts a fresh decode sequence, so its leading RASL
            // pictures cannot reference pre-CRA pictures that were not emitted.
            no_rasl_output_ = irap == 21;
            output_.video_start(irap, unit.random_access);
        } else if (no_rasl_output_) {
            if (only_rasl_vcl) return;
            no_rasl_output_ = false;
        }
        if (!output_enabled) return;

        std::size_t output_size = 0;
        for (const auto& nalu : nalus) {
            if (included_in_sample(nalu)) output_size += 4U + nalu.size;
        }
        Bytes data;
        data.reserve(output_size);
        for (const auto& nalu : nalus) {
            if (!included_in_sample(nalu)) continue;
            append(data, u32(nalu.size));
            append(data, unit.data.data() + nalu.offset, nalu.size);
        }
        if (data.empty()) return;
        const auto offset = timeline_offset_us_.value_or(0);
        const auto dts = scaled(unit.dts.value, unit.dts.timescale, 1000000) + offset;
        const auto pts = scaled(unit.pts.value, unit.pts.timescale, 1000000) + offset;
        if (dts < 0) return;
        enqueue({std::move(data), dts, pts, 0, irap >= 0});
    }

private:
    static bool included_in_sample(const NaluView& nalu) noexcept {
        return nalu.type != 32 && nalu.type != 33 && nalu.type != 34 && nalu.type != 35;
    }

    std::uint32_t default_duration() const override { return 33367; }

    Output& output_;
    std::map<int, Bytes> parameter_sets_;
    bool started_ = false;
    bool no_rasl_output_ = false;
    std::optional<std::int64_t> timeline_offset_us_;
};

class AacMuxer final : public BaseMuxer {
public:
    explicit AacMuxer(Output& output, const std::uint32_t max_channels)
        : BaseMuxer("audio", output), max_channels_(max_channels) {}

    void discontinuity() { reset_samples(); }

    void push(const tlvdemux::AccessUnit& unit, const bool enabled,
              const std::optional<std::int64_t> timeline_offset_us) {
        if (unit.discontinuity) reset_samples();
        auto frame = parser_.parse(unit.data);
        if (max_channels_ != 0 && frame.channels > max_channels_) return;
        if (!track_) {
            Mp4Track track;
            track.timescale = frame.sample_rate;
            track.sample_rate = frame.sample_rate;
            track.channels = frame.channels;
            track.codec = "mp4a.40." + std::to_string(frame.object);
            track.config = frame.asc;
            set_track(std::move(track));
        }
        if (!enabled || !timeline_offset_us.has_value()) return;
        const auto shifted_us = scaled(unit.pts.value, unit.pts.timescale, 1000000) +
            *timeline_offset_us;
        if (shifted_us < 0) return;
        const auto timestamp = scaled(shifted_us, 1000000, track_->timescale);
        enqueue({std::move(frame.data), timestamp, timestamp, 0, true});
    }

private:
    std::uint32_t default_duration() const override {
        return track_ ? static_cast<std::uint32_t>(std::llround(
                            1024.0 * track_->timescale / track_->sample_rate))
                      : 21333;
    }

    LatmParser parser_;
    std::uint32_t max_channels_ = 0;
};

} // namespace

class tlvdemux::MseRemuxer::Impl {
public:
    explicit Impl(MseSink& sink, const MseOptions options)
        : output(sink), video(output), options(options) {}

    void select(const tlvdemux::TrackKind kind, std::optional<std::uint64_t> id) {
        if (kind == tlvdemux::TrackKind::Video) {
            video_id = id;
            return;
        }
        if (kind != tlvdemux::TrackKind::Audio) return;
        if (audio_id == id && active_audio != nullptr) return;
        audio_id = id;
        if (!id) {
            active_audio = nullptr;
            return;
        }
        auto [iterator, inserted] =
            audio.try_emplace(*id, output, options.max_audio_channels);
        active_audio = &iterator->second;
        if (!inserted) active_audio->activate();
    }

    void push(const tlvdemux::AccessUnit& unit) {
        if (video_id && unit.track_id == *video_id) {
            if (unit.discontinuity && active_audio) active_audio->discontinuity();
            video.push(unit, enabled);
        } else if (audio_id && unit.track_id == *audio_id && active_audio) {
            active_audio->push(unit, enabled && video.started(),
                               video.timeline_offset_us());
        }
    }

    void flush() {
        video.flush();
        if (active_audio) active_audio->flush();
    }

    void reset() {
        video.reset();
        audio.clear();
        active_audio = nullptr;
    }

    void reposition() {
        video.reset();
        for (auto& entry : audio) entry.second.discontinuity();
    }

    Output output;
    HevcMuxer video;
    std::map<std::uint64_t, AacMuxer> audio;
    AacMuxer* active_audio = nullptr;
    MseOptions options;
    std::optional<std::uint64_t> video_id;
    std::optional<std::uint64_t> audio_id;
    bool enabled = true;
};

tlvdemux::MseRemuxer::MseRemuxer(MseSink& sink, const MseOptions options)
    : impl_(std::make_unique<Impl>(sink, options)) {}
tlvdemux::MseRemuxer::~MseRemuxer() = default;

void tlvdemux::MseRemuxer::selectTrack(const TrackKind kind,
                                       std::optional<std::uint64_t> id) {
    impl_->select(kind, id);
}

void tlvdemux::MseRemuxer::setOutputEnabled(const bool enabled) noexcept {
    impl_->enabled = enabled;
    impl_->output.set_enabled(enabled);
}

void tlvdemux::MseRemuxer::push(const AccessUnit& unit) { impl_->push(unit); }
void tlvdemux::MseRemuxer::flush() { impl_->flush(); }
void tlvdemux::MseRemuxer::reset() { impl_->reset(); }
void tlvdemux::MseRemuxer::reposition() { impl_->reposition(); }

#ifdef __EMSCRIPTEN__
class WasmMseRemuxer::Impl final : public tlvdemux::MseSink {
public:
    explicit Impl(val callbacks, const std::uint32_t max_audio_channels)
        : callbacks_(std::move(callbacks)),
          remuxer_(*this, tlvdemux::MseOptions{max_audio_channels}) {}

    void onMseInit(tlvdemux::MseTrackInit&& init) override {
        if (!has("onMseInit")) return;
        auto event = val::object();
        event.set("type", init.type);
        event.set("mime", init.mime);
        event.set("data", copy_bytes(init.data));
        event.set("width", init.width);
        event.set("height", init.height);
        event.set("sampleRate", init.sample_rate);
        event.set("channels", init.channels);
        emit("onMseInit", event);
    }

    void onMseSegment(tlvdemux::MseMediaSegment&& segment) override {
        if (!has("onMseSegment")) return;
        auto event = val::object();
        event.set("type", segment.type);
        event.set("data", copy_bytes(segment.data));
        emit("onMseSegment", event);
    }

    void onMseVideoStart(const tlvdemux::MseVideoStart& start) override {
        if (!has("onMseVideoStart")) return;
        auto event = val::object();
        event.set("nalType", start.nal_type);
        event.set("signalledRandomAccess", start.signalled_random_access);
        emit("onMseVideoStart", event);
    }

    tlvdemux::MseRemuxer& remuxer() noexcept { return remuxer_; }

private:
    bool has(const char* name) const {
        return callbacks_[name].typeOf().as<std::string>() == "function";
    }

    void emit(const char* name, const val& event) {
        callbacks_[name].call<void>("call", callbacks_, event);
    }

    val callbacks_;
    tlvdemux::MseRemuxer remuxer_;
};

WasmMseRemuxer::WasmMseRemuxer(val callbacks,
                               const std::uint32_t max_audio_channels)
    : impl_(std::make_unique<Impl>(std::move(callbacks), max_audio_channels)) {}
WasmMseRemuxer::~WasmMseRemuxer() = default;

void WasmMseRemuxer::selectTrack(const tlvdemux::TrackKind kind,
                                 std::optional<std::uint64_t> id) {
    impl_->remuxer().selectTrack(kind, id);
}

void WasmMseRemuxer::setOutputEnabled(const bool enabled) noexcept {
    impl_->remuxer().setOutputEnabled(enabled);
}

void WasmMseRemuxer::push(const tlvdemux::AccessUnit& unit) {
    impl_->remuxer().push(unit);
}
void WasmMseRemuxer::flush() { impl_->remuxer().flush(); }
void WasmMseRemuxer::reset() { impl_->remuxer().reset(); }
void WasmMseRemuxer::reposition() { impl_->remuxer().reposition(); }
#endif
