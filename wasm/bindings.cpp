#include <tlvdemux/demuxer.hpp>
#include <tlvdemux/duration_probe.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <emscripten/bind.h>
#include <emscripten/heap.h>
#include <emscripten/val.h>

namespace {

using emscripten::val;

val copy_bytes(const std::vector<std::uint8_t>& source) {
    auto result = val::global("Uint8Array").new_(source.size());
    if (!source.empty()) {
        result.call<void>("set", val(emscripten::typed_memory_view(source.size(), source.data())));
    }
    return result;
}

val view_bytes(const std::vector<std::uint8_t>& source) {
    if (source.empty()) return val::global("Uint8Array").new_(0);
    return val(emscripten::typed_memory_view(source.size(), source.data()));
}

const char* codec_name(const tlvdemux::Codec codec) noexcept {
    switch (codec) {
    case tlvdemux::Codec::Hevc: return "hevc";
    case tlvdemux::Codec::AacLatm: return "aac-latm";
    case tlvdemux::Codec::Ttml: return "ttml";
    }
    return "unknown";
}

const char* track_kind_name(const tlvdemux::TrackKind kind) noexcept {
    switch (kind) {
    case tlvdemux::TrackKind::Video: return "video";
    case tlvdemux::TrackKind::Audio: return "audio";
    case tlvdemux::TrackKind::Subtitle: return "subtitle";
    }
    return "unknown";
}

const char* error_code_name(const tlvdemux::ErrorCode code) noexcept {
    switch (code) {
    case tlvdemux::ErrorCode::MalformedInput: return "malformed-input";
    case tlvdemux::ErrorCode::UnsupportedFeature: return "unsupported-feature";
    case tlvdemux::ErrorCode::Discontinuity: return "discontinuity";
    case tlvdemux::ErrorCode::ResourceLimit: return "resource-limit";
    }
    return "unknown";
}

const char* duration_probe_state_name(const tlvdemux::DurationProbeState state) noexcept {
    switch (state) {
    case tlvdemux::DurationProbeState::Idle: return "idle";
    case tlvdemux::DurationProbeState::NeedRange: return "need-range";
    case tlvdemux::DurationProbeState::Complete: return "complete";
    case tlvdemux::DurationProbeState::Unknown: return "unknown";
    case tlvdemux::DurationProbeState::Failed: return "failed";
    case tlvdemux::DurationProbeState::Cancelled: return "cancelled";
    }
    return "unknown";
}

const char* duration_probe_failure_name(const tlvdemux::DurationProbeFailure failure) noexcept {
    switch (failure) {
    case tlvdemux::DurationProbeFailure::None: return "none";
    case tlvdemux::DurationProbeFailure::InvalidSource: return "invalid-source";
    case tlvdemux::DurationProbeFailure::InvalidResponse: return "invalid-response";
    case tlvdemux::DurationProbeFailure::SourceError: return "source-error";
    case tlvdemux::DurationProbeFailure::NoVideo: return "no-video";
    case tlvdemux::DurationProbeFailure::NoTailTimestamp: return "no-tail-timestamp";
    case tlvdemux::DurationProbeFailure::RangeLimit: return "range-limit";
    case tlvdemux::DurationProbeFailure::ParseError: return "parse-error";
    }
    return "unknown";
}

const char* index_state_name(const tlvdemux::IndexState state) noexcept {
    switch (state) {
    case tlvdemux::IndexState::Absent: return "absent";
    case tlvdemux::IndexState::Loading: return "loading";
    case tlvdemux::IndexState::Building: return "building";
    case tlvdemux::IndexState::Partial: return "partial";
    case tlvdemux::IndexState::Following: return "following";
    case tlvdemux::IndexState::Complete: return "complete";
    case tlvdemux::IndexState::Stale: return "stale";
    case tlvdemux::IndexState::Failed: return "failed";
    }
    return "unknown";
}

val duration_value(const tlvdemux::DurationInfo duration) {
    if (duration.status == tlvdemux::DurationStatus::Unknown) return val::null();
    auto result = val::object();
    result.set("value", duration.value.value);
    result.set("timescale", duration.value.timescale);
    result.set("status", duration.status == tlvdemux::DurationStatus::Complete
                             ? std::string("complete")
                             : std::string("provisional"));
    return result;
}

val seek_point_value(const tlvdemux::SeekPoint& point) {
    auto result = val::object();
    result.set("presentationTimeUs", point.presentation_time.value);
    result.set("signallingOffset", point.signalling_offset);
    result.set("randomAccessOffset", point.random_access_offset);
    result.set("videoTrackId", point.video_track_id);
    result.set("bootstrapId", point.bootstrap_id);
    return result;
}

class WasmDurationProbe final {
public:
    bool begin(const std::uint64_t source_size, const val& js_options) {
        tlvdemux::DurationProbeOptions options;
        if (!js_options.isNull() && !js_options.isUndefined()) {
            assign_if_present(js_options, "initialRangeSize", options.initial_range_size);
            assign_if_present(js_options, "maxRangeSize", options.max_range_size);
            assign_optional_if_present(js_options, "serviceContextId",
                                       options.service_context_id);
            assign_optional_if_present(js_options, "videoPacketId", options.video_packet_id);
        }
        return probe_.begin(source_size, options);
    }

    val nextRange() const {
        const auto request = probe_.nextRange();
        if (!request.has_value()) return val::null();
        auto result = val::object();
        result.set("generation", request->generation);
        result.set("requestId", request->request_id);
        result.set("offset", request->offset);
        result.set("length", request->length);
        return result;
    }

    bool pushRange(const std::uint64_t request_id, const std::uint64_t absolute_offset,
                   const val& bytes, const bool end_of_range) {
        if (bytes.isNull() || bytes.isUndefined()) return false;
        const auto byte_length = bytes["byteLength"].as<std::size_t>();
        std::vector<std::uint8_t> copy(byte_length);
        if (byte_length != 0) {
            val(emscripten::typed_memory_view(copy.size(), copy.data())).call<void>("set", bytes);
        }
        return probe_.pushRange(request_id, absolute_offset, copy.data(), copy.size(), end_of_range);
    }

    bool pushRangeFromHeap(const std::uint64_t request_id,
                           const std::uint64_t absolute_offset,
                           const std::uintptr_t address, const std::size_t size,
                           const bool end_of_range) {
        const auto heap_size = static_cast<std::uintptr_t>(emscripten_get_heap_size());
        if (address > heap_size || size > heap_size - address) return false;
        return probe_.pushRange(request_id, absolute_offset,
                                reinterpret_cast<const std::uint8_t*>(address), size,
                                end_of_range);
    }

    bool failRange(const std::uint64_t request_id) { return probe_.failRange(request_id); }
    void cancel() { probe_.cancel(); }
    std::string state() const { return duration_probe_state_name(probe_.state()); }
    std::string failure() const { return duration_probe_failure_name(probe_.failure()); }
    std::uint64_t generation() const { return probe_.generation(); }
    std::uint64_t transferredBytes() const { return probe_.transferredBytes(); }

    val duration() const {
        return duration_value(probe_.duration());
    }

private:
    template <typename T>
    static void assign_if_present(const val& object, const char* name, T& destination) {
        const auto value = object[name];
        if (!value.isNull() && !value.isUndefined()) destination = value.as<T>();
    }

    template <typename T>
    static void assign_optional_if_present(const val& object, const char* name,
                                           std::optional<T>& destination) {
        const auto value = object[name];
        if (!value.isNull() && !value.isUndefined()) destination = value.as<T>();
    }

    tlvdemux::DurationProbe probe_;
};

class WasmDemuxer final : public tlvdemux::Sink {
public:
    explicit WasmDemuxer(val callbacks)
        : callbacks_(std::move(callbacks)), demuxer_(*this) {}

    bool push(const val& bytes) {
        if (bytes.isNull() || bytes.isUndefined()) return false;
        const auto byte_length = bytes["byteLength"].as<std::size_t>();
        std::vector<std::uint8_t> copy(byte_length);
        if (byte_length != 0) {
            val(emscripten::typed_memory_view(copy.size(), copy.data())).call<void>("set", bytes);
        }
        demuxer_.push(copy.data(), copy.size());
        return true;
    }

    bool pushFromHeap(const std::uintptr_t address, const std::size_t size) {
        const auto heap_size = static_cast<std::uintptr_t>(emscripten_get_heap_size());
        if (address > heap_size || size > heap_size - address) return false;
        demuxer_.push(reinterpret_cast<const std::uint8_t*>(address), size);
        return true;
    }

    void flush() { demuxer_.flush(); }
    void reset() {
        demuxer_.reset();
        if (index_active_) recording_index_.begin(index_growing_);
    }

    void reposition(const std::uint64_t input_offset, const bool preserve_timeline) {
        demuxer_.reposition(tlvdemux::RepositionOptions{input_offset, preserve_timeline});
    }

    void selectService(const val& context_id) {
        demuxer_.selectService(optional_number<std::uint32_t>(context_id));
        if (index_active_) recording_index_.begin(index_growing_);
    }

    void selectTrack(const std::string& kind, const val& track_id) {
        std::optional<tlvdemux::TrackKind> parsed_kind;
        if (kind == "video") parsed_kind = tlvdemux::TrackKind::Video;
        if (kind == "audio") parsed_kind = tlvdemux::TrackKind::Audio;
        if (kind == "subtitle") parsed_kind = tlvdemux::TrackKind::Subtitle;
        if (!parsed_kind.has_value()) return;
        const auto selected = optional_number<std::uint64_t>(track_id);
        demuxer_.selectTrack(*parsed_kind, selected);
        if (*parsed_kind == tlvdemux::TrackKind::Video && index_active_ &&
            recording_index_.state() == tlvdemux::IndexState::Building) {
            recording_index_.selectVideoTrack(selected);
        }
    }

    void startIndex(const bool growing) {
        recording_index_.begin(growing);
        index_active_ = true;
        index_growing_ = growing;
    }

    bool finalizeIndex() {
        return index_active_ && recording_index_.finalize();
    }

    std::string indexState() const {
        return index_state_name(recording_index_.state());
    }

    val indexDuration() const { return duration_value(recording_index_.duration()); }
    std::size_t seekPointCount() const { return recording_index_.seekPoints().size(); }
    val indexedVideoTrack() const {
        const auto track = recording_index_.selectedVideoTrack();
        return track.has_value() ? val(*track) : val::null();
    }

    val previousSync(const std::int64_t target_us) const {
        const auto point = recording_index_.previousSync(
            tlvdemux::Timestamp{target_us, 1000000});
        return point.has_value() ? seek_point_value(*point) : val::null();
    }

    val seekPointsFor(const std::int64_t target_us) const {
        const auto points = recording_index_.seekPointsFor(
            tlvdemux::Timestamp{target_us, 1000000});
        if (!points.has_value()) return val::null();
        auto result = val::object();
        result.set("first", seek_point_value(points->first));
        result.set("second", points->second.has_value()
                                 ? seek_point_value(*points->second)
                                 : val::null());
        return result;
    }

    val estimateOffset(const std::int64_t target_us,
                       const std::uint64_t source_size) const {
        const auto offset = recording_index_.estimateOffset(
            tlvdemux::Timestamp{target_us, 1000000}, source_size);
        return offset.has_value() ? val(*offset) : val::null();
    }

    void onService(const tlvdemux::ServiceInfo& info) override {
        auto event = val::object();
        event.set("contextId", info.context_id);
        event.set("packageId", copy_bytes(info.package_id));
        emit("onService", event);
    }

    void onTrack(const tlvdemux::TrackInfo& info) override {
        auto event = val::object();
        event.set("trackId", info.track_id);
        event.set("contextId", info.context_id);
        event.set("packetId", info.packet_id);
        event.set("kind", std::string(track_kind_name(info.kind)));
        event.set("codec", std::string(codec_name(info.codec)));
        event.set("language", info.language);
        event.set("componentTag", info.component_tag);
        event.set("timescale", info.timescale);
        if (info.audio.has_value()) {
            auto audio = val::object();
            audio.set("channelLayout", static_cast<unsigned>(info.audio->channel_layout));
            audio.set("sampleRate", info.audio->sample_rate);
            audio.set("mainComponent", info.audio->main_component);
            event.set("audio", audio);
        }
        if (info.subtitle.has_value()) {
            auto subtitle = val::object();
            subtitle.set("operationMode", info.subtitle->operation_mode);
            subtitle.set("timingMode", info.subtitle->timing_mode);
            event.set("subtitle", subtitle);
        }
        emit("onTrack", event);
    }

    void onAccessUnit(tlvdemux::AccessUnit&& unit) override {
        if (index_active_) recording_index_.observe(unit);
        if (has_callback("onAccessUnitView")) {
            auto event = access_unit_event(unit, view_bytes(unit.data));
            event.set("dataLifetime", std::string("callback"));
            emit("onAccessUnitView", event);
            return;
        }
        emit("onAccessUnit", access_unit_event(unit, copy_bytes(unit.data)));
    }

    void onError(const tlvdemux::Error& error) override {
        auto event = val::object();
        event.set("code", std::string(error_code_name(error.code)));
        event.set("inputOffset", error.input_offset);
        event.set("recoverable", error.recoverable);
        event.set("message", error.message);
        emit("onError", event);
    }

private:
    static val access_unit_event(const tlvdemux::AccessUnit& unit, const val& data) {
        auto event = val::object();
        event.set("trackId", unit.track_id);
        event.set("codec", std::string(codec_name(unit.codec)));
        event.set("data", data);
        event.set("ptsValue", unit.pts.value);
        event.set("ptsTimescale", unit.pts.timescale);
        event.set("dtsValue", unit.dts.value);
        event.set("dtsTimescale", unit.dts.timescale);
        event.set("restartOffset", unit.restart_offset);
        event.set("inputOffset", unit.input_offset);
        event.set("randomAccess", unit.random_access);
        event.set("discontinuity", unit.discontinuity);
        return event;
    }

    bool has_callback(const char* name) const {
        if (callbacks_.isNull() || callbacks_.isUndefined()) return false;
        return callbacks_[name].typeOf().as<std::string>() == "function";
    }

    template <typename T>
    static std::optional<T> optional_number(const val& value) {
        if (value.isNull() || value.isUndefined()) return std::nullopt;
        return value.as<T>();
    }

    void emit(const char* name, const val& event) {
        if (!has_callback(name)) return;
        const auto callback = callbacks_[name];
        callback.call<void>("call", callbacks_, event);
    }

    val callbacks_;
    tlvdemux::Demuxer demuxer_;
    tlvdemux::RecordingIndex recording_index_;
    bool index_active_ = false;
    bool index_growing_ = false;
};

} // namespace

EMSCRIPTEN_BINDINGS(tlvdemux_wasm) {
    emscripten::class_<WasmDemuxer>("TlvDemuxer")
        .constructor<val>()
        .function("push", &WasmDemuxer::push)
        .function("pushFromHeap", &WasmDemuxer::pushFromHeap)
        .function("flush", &WasmDemuxer::flush)
        .function("reset", &WasmDemuxer::reset)
        .function("reposition", &WasmDemuxer::reposition)
        .function("selectService", &WasmDemuxer::selectService)
        .function("selectTrack", &WasmDemuxer::selectTrack)
        .function("startIndex", &WasmDemuxer::startIndex)
        .function("finalizeIndex", &WasmDemuxer::finalizeIndex)
        .function("indexState", &WasmDemuxer::indexState)
        .function("indexDuration", &WasmDemuxer::indexDuration)
        .function("seekPointCount", &WasmDemuxer::seekPointCount)
        .function("indexedVideoTrack", &WasmDemuxer::indexedVideoTrack)
        .function("previousSync", &WasmDemuxer::previousSync)
        .function("seekPointsFor", &WasmDemuxer::seekPointsFor)
        .function("estimateOffset", &WasmDemuxer::estimateOffset);

    emscripten::class_<WasmDurationProbe>("DurationProbe")
        .constructor<>()
        .function("begin", &WasmDurationProbe::begin)
        .function("nextRange", &WasmDurationProbe::nextRange)
        .function("pushRange", &WasmDurationProbe::pushRange)
        .function("pushRangeFromHeap", &WasmDurationProbe::pushRangeFromHeap)
        .function("failRange", &WasmDurationProbe::failRange)
        .function("cancel", &WasmDurationProbe::cancel)
        .function("state", &WasmDurationProbe::state)
        .function("failure", &WasmDurationProbe::failure)
        .function("duration", &WasmDurationProbe::duration)
        .function("generation", &WasmDurationProbe::generation)
        .function("transferredBytes", &WasmDurationProbe::transferredBytes);
}

int main() {
    return 0;
}
