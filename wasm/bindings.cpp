#include <tlvdemux/demuxer.hpp>
#include <tlvdemux/duration_probe.hpp>
#include <tlvdemux/application_resources.hpp>

#include "mse_remuxer.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <variant>
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

const char* application_collection_state_name(
    const tlvdemux::ApplicationCollectionState state) noexcept {
    switch (state) {
    case tlvdemux::ApplicationCollectionState::Discovered: return "discovered";
    case tlvdemux::ApplicationCollectionState::Collecting: return "collecting";
    case tlvdemux::ApplicationCollectionState::Ready: return "ready";
    }
    return "discovered";
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

val broadcast_clock_value(const tlvdemux::BroadcastClock& clock) {
    auto result = val::object();
    result.set("mediaTimeValue", clock.media_time.value);
    result.set("mediaTimeTimescale", clock.media_time.timescale);
    result.set("broadcastTimeValue", clock.broadcast_time.value);
    result.set("broadcastTimeTimescale", clock.broadcast_time.timescale);
    result.set("inputOffset", clock.input_offset);
    result.set("discontinuity", clock.discontinuity);
    return result;
}

val event_info_value(const tlvdemux::EventInfo& info) {
    auto result = val::object();
    result.set("contextId", info.context_id);
    result.set("sourcePacketId", info.source_packet_id);
    result.set("tableId", info.table_id);
    result.set("version", info.version);
    result.set("currentNext", info.current_next);
    result.set("sectionNumber", info.section_number);
    result.set("lastSectionNumber", info.last_section_number);
    result.set("serviceId", info.service_id);
    result.set("tlvStreamId", info.tlv_stream_id);
    result.set("originalNetworkId", info.original_network_id);
    result.set("eventId", info.event_id);
    if (info.start_time_unix_milliseconds.has_value()) {
        result.set("startTimeUnixMilliseconds",
                   static_cast<double>(*info.start_time_unix_milliseconds));
    } else {
        result.set("startTimeUnixMilliseconds", val::null());
    }
    if (info.duration_seconds.has_value()) {
        result.set("durationSeconds", *info.duration_seconds);
    } else {
        result.set("durationSeconds", val::null());
    }
    result.set("runningStatus", info.running_status);
    result.set("freeCaMode", info.free_ca_mode);
    result.set("language", info.language);
    result.set("title", info.title);
    result.set("description", info.description);
    result.set("inputOffset", info.input_offset);
    return result;
}

val stream_event_value(const tlvdemux::StreamEvent& event) {
    auto result = val::object();
    result.set("contextId", event.context_id);
    result.set("sourcePacketId", event.source_packet_id);
    result.set("eventMessageTag", event.event_message_tag);
    result.set("dataEventId", event.data_event_id);
    result.set("messageGroupId", event.message_group_id);
    result.set("messageVersion", event.message_version);
    result.set("currentNext", event.current_next);
    result.set("sectionNumber", event.section_number);
    result.set("lastSectionNumber", event.last_section_number);
    result.set("timeMode", event.time_mode);
    result.set("timeValue", event.time_value);
    result.set("utcReference", event.utc_reference.has_value()
        ? val(*event.utc_reference) : val::null());
    result.set("nptReference", event.npt_reference.has_value()
        ? val(*event.npt_reference) : val::null());
    result.set("messageType", event.message_type);
    result.set("rawMessageId", event.raw_message_id);
    result.set("messageId", event.message_id);
    result.set("privateData", copy_bytes(event.private_data));
    result.set("inputOffset", event.input_offset);
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

class WasmDemuxer final : public tlvdemux::Sink,
                          public tlvdemux::ApplicationResourceSink {
public:
    explicit WasmDemuxer(val callbacks)
        : callbacks_(std::move(callbacks)), application_assembler_(*this),
          demuxer_(*this, media_limits()),
          mse_remuxer_(callbacks_, mse_max_audio_channels(callbacks_)) {
        mse_enabled_ = has_callback("onMseInit") || has_callback("onMseSegment");
    }

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

    void flush() {
        demuxer_.flush();
        if (mse_enabled_) mse_remuxer_.flush();
    }
    void reset() {
        reset_application_resources();
        demuxer_.reset();
        if (mse_enabled_) mse_remuxer_.reset();
        if (index_active_) recording_index_.begin(index_growing_);
    }

    void reposition(const std::uint64_t input_offset, const bool preserve_timeline) {
        // A media seek is not a service change. Drop only incomplete carousel
        // assembly state so fragments from both byte positions cannot mix,
        // while retaining files already published into the VFS. Receivers can
        // therefore open data broadcasting immediately after a seek and keep
        // refreshing it from later carousel cycles.
        restart_application_assembly();
        demuxer_.reposition(tlvdemux::RepositionOptions{input_offset, preserve_timeline});
        if (mse_enabled_) mse_remuxer_.reposition();
    }

    void selectService(const val& context_id) {
        reset_application_resources();
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
        if (mse_enabled_) mse_remuxer_.selectTrack(*parsed_kind, selected);
        if (*parsed_kind == tlvdemux::TrackKind::Video && index_active_ &&
            recording_index_.state() == tlvdemux::IndexState::Building) {
            recording_index_.selectVideoTrack(selected);
        }
    }

    void setMseOutputEnabled(const bool enabled) {
        mse_remuxer_.setOutputEnabled(enabled);
    }

    void setSubtitlePassthroughEnabled(const bool enabled) {
        demuxer_.setSubtitlePassthroughEnabled(enabled);
    }

    bool drainApplicationResources(std::size_t max_events) {
        if (max_events == 0) max_events = application_events_.size();
        while (max_events-- != 0 && !application_events_.empty()) {
            auto event = std::move(application_events_.front());
            application_events_.pop_front();
            std::visit([this](auto&& value) { consume_application_event(std::move(value)); },
                       std::move(event));
        }
        return !application_events_.empty();
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
    bool setIndexDuration(const std::int64_t duration_us) {
        if (!index_active_ || duration_us < 0) return false;
        return recording_index_.updateDuration(tlvdemux::DurationInfo{
            tlvdemux::Timestamp{duration_us, 1000000},
            tlvdemux::DurationStatus::Provisional,
        });
    }
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

    val applicationResources(const val& context_id) const {
        const auto resources = application_resources_.list(
            optional_number<std::uint32_t>(context_id));
        auto result = val::array();
        for (std::size_t index = 0; index < resources.size(); ++index) {
            result.set(index, application_resource_metadata_event(resources[index]));
        }
        return result;
    }

    val applicationResource(const std::uint32_t context_id,
                            const std::string& path) const {
        const auto resource = application_resources_.get(context_id, path);
        if (!resource) return val::null();
        return application_resource_event(*resource, copy_bytes(resource->data));
    }

    val applicationEntry(const std::uint32_t context_id) const {
        const auto path = application_resources_.entryPath(context_id);
        return path.has_value() ? val(*path) : val::null();
    }

    val applications() const {
        const auto applications = application_resources_.applications();
        auto result = val::array();
        for (std::size_t index = 0; index < applications.size(); ++index) {
            result.set(index, application_state_event(applications[index]));
        }
        return result;
    }

    std::uint64_t applicationResourceGeneration() const {
        return application_resources_.generation();
    }

    val broadcastClock() const {
        const auto clock = demuxer_.broadcastClock();
        return clock.has_value() ? broadcast_clock_value(*clock) : val::null();
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
            audio.set("componentType", info.audio->component_type);
            audio.set("componentTag", info.audio->component_tag);
            audio.set("channelLayout", static_cast<unsigned>(info.audio->channel_layout));
            audio.set("channels", tlvdemux::audio_channel_count(info.audio->channel_layout));
            audio.set("streamType", info.audio->stream_type);
            audio.set("simulcastGroupTag", info.audio->simulcast_group_tag);
            audio.set("multilingual", info.audio->es_multi_lingual);
            audio.set("sampleRate", info.audio->sample_rate);
            audio.set("mainComponent", info.audio->main_component);
            audio.set("secondaryLanguage", info.audio->secondary_language);
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
        if (mse_enabled_) mse_remuxer_.push(unit);
        if (has_callback("onAccessUnitView")) {
            auto event = access_unit_event(unit, view_bytes(unit.data));
            event.set("dataLifetime", std::string("callback"));
            emit("onAccessUnitView", event);
            return;
        }
        emit("onAccessUnit", access_unit_event(unit, copy_bytes(unit.data)));
    }

    void onApplication(const tlvdemux::ApplicationInfo& info) override {
        application_events_.emplace_back(info);
    }

    void onDataDirectoryTable(const tlvdemux::DataDirectoryTable& table) override {
        application_events_.emplace_back(table);
    }

    void onDataAssetManagementTable(const tlvdemux::DataAssetManagementTable& table) override {
        application_events_.emplace_back(table);
    }

    void onDataUnit(tlvdemux::DataUnit&& unit) override {
        application_events_.emplace_back(std::move(unit));
    }

    void onError(const tlvdemux::Error& error) override {
        auto event = val::object();
        event.set("code", std::string(error_code_name(error.code)));
        event.set("inputOffset", error.input_offset);
        event.set("recoverable", error.recoverable);
        event.set("message", error.message);
        emit("onError", event);
    }

    void onBroadcastClock(const tlvdemux::BroadcastClock& clock) override {
        emit("onBroadcastClock", broadcast_clock_value(clock));
    }

    void onEventInfo(const tlvdemux::EventInfo& info) override {
        emit("onEventInfo", event_info_value(info));
    }

    void onStreamEvent(const tlvdemux::StreamEvent& event) override {
        emit("onStreamEvent", stream_event_value(event));
    }

    void onApplicationState(const tlvdemux::ApplicationState& state) override {
        application_resources_.onApplicationState(state);
        emit("onApplicationState", application_state_event(state));
    }

    void onApplicationResource(tlvdemux::ApplicationResource&& resource) override {
        const auto context_id = resource.context_id;
        const auto path = resource.path;
        application_resources_.onApplicationResource(std::move(resource));
        const auto stored = application_resources_.get(context_id, path);
        if (!stored) return;
        if (has_callback("onApplicationResourceView")) {
            auto event = application_resource_event(*stored, view_bytes(stored->data));
            event.set("dataLifetime", std::string("callback"));
            emit("onApplicationResourceView", event);
            return;
        }
        if (has_callback("onApplicationResource")) {
            emit("onApplicationResource",
                 application_resource_event(*stored, copy_bytes(stored->data)));
        }
    }

    void onApplicationResourcesReset() override {
        application_resources_.onApplicationResourcesReset();
        emit("onApplicationResourcesReset", val::object());
    }

private:
    static std::uint32_t mse_max_audio_channels(const val& options) {
        if (options.isNull() || options.isUndefined()) return 0;
        const auto value = options["mseMaxAudioChannels"];
        if (value.typeOf().as<std::string>() != "number") return 0;
        const auto channels = value.as<double>();
        if (!std::isfinite(channels) || channels <= 0 || channels > 24 ||
            std::floor(channels) != channels) return 0;
        return static_cast<std::uint32_t>(channels);
    }

    using ApplicationEvent = std::variant<tlvdemux::ApplicationInfo,
                                          tlvdemux::DataDirectoryTable,
                                          tlvdemux::DataAssetManagementTable,
                                          tlvdemux::DataUnit>;

    static tlvdemux::Limits media_limits() {
        auto limits = tlvdemux::Limits{};
        limits.collect_application_resources = false;
        return limits;
    }

    void reset_application_resources() {
        application_events_.clear();
        application_assembler_.reset();
    }

    void restart_application_assembly() {
        application_events_.clear();
        // Replacing the assembler clears partial tables/units without sending
        // onApplicationResourcesReset(), which would erase the completed VFS.
        application_assembler_ = tlvdemux::ApplicationResourceAssembler(*this);
    }

    void consume_application_event(tlvdemux::ApplicationInfo info) {
        application_assembler_.onApplication(info);
    }
    void consume_application_event(tlvdemux::DataDirectoryTable table) {
        application_assembler_.onDataDirectoryTable(table);
    }
    void consume_application_event(tlvdemux::DataAssetManagementTable table) {
        application_assembler_.onDataAssetManagementTable(table);
    }
    void consume_application_event(tlvdemux::DataUnit unit) {
        application_assembler_.onDataUnit(unit);
    }

    static val access_unit_event(const tlvdemux::AccessUnit& unit, const val& data) {
        auto event = val::object();
        event.set("trackId", unit.track_id);
        event.set("codec", std::string(codec_name(unit.codec)));
        event.set("componentTag", unit.component_tag);
        event.set("subtitleTimingMode", unit.subtitle_timing_mode.has_value()
                                            ? val(*unit.subtitle_timing_mode)
                                            : val::null());
        event.set("data", data);
        event.set("ptsValue", unit.pts.value);
        event.set("ptsTimescale", unit.pts.timescale);
        event.set("dtsValue", unit.dts.value);
        event.set("dtsTimescale", unit.dts.timescale);
        event.set("mpuSequenceNumber", unit.mpu_sequence_number.has_value()
                                           ? val(*unit.mpu_sequence_number)
                                           : val::null());
        if (unit.subtitle_reference_start_pts.has_value()) {
            event.set("subtitleReferenceStartPtsValue",
                      unit.subtitle_reference_start_pts->value);
            event.set("subtitleReferenceStartPtsTimescale",
                      unit.subtitle_reference_start_pts->timescale);
        } else {
            event.set("subtitleReferenceStartPtsValue", val::null());
            event.set("subtitleReferenceStartPtsTimescale", val::null());
        }
        auto resources = val::array();
        for (std::size_t index = 0; index < unit.subtitle_resources.size(); ++index) {
            const auto& source = unit.subtitle_resources[index];
            auto resource = val::object();
            resource.set("subsampleNumber", source.subsample_number);
            resource.set("dataType", source.data_type);
            // B62RendererStateMachine keeps a resource scope beyond this
            // callback, so resource payloads cannot borrow the AccessUnit.
            resource.set("data", copy_bytes(source.data));
            resources.set(index, resource);
        }
        event.set("subtitleResources", resources);
        event.set("restartOffset", unit.restart_offset);
        event.set("inputOffset", unit.input_offset);
        event.set("randomAccess", unit.random_access);
        event.set("discontinuity", unit.discontinuity);
        return event;
    }

    static val application_resource_event(const tlvdemux::ApplicationResource& resource,
                                          const val& data) {
        auto event = val::object();
        event.set("contextId", resource.context_id);
        event.set("componentTag", resource.component_tag);
        event.set("transactionId", resource.transaction_id);
        event.set("downloadId", resource.download_id);
        event.set("mpuSequenceNumber", resource.mpu_sequence_number);
        event.set("itemId", resource.item_id);
        event.set("version", resource.version);
        event.set("path", resource.path);
        event.set("contentType", resource.content_type);
        event.set("data", data);
        return event;
    }

    static val application_resource_metadata_event(
        const tlvdemux::ApplicationResourceMetadata& resource) {
        auto event = val::object();
        event.set("contextId", resource.context_id);
        event.set("componentTag", resource.component_tag);
        event.set("transactionId", resource.transaction_id);
        event.set("downloadId", resource.download_id);
        event.set("mpuSequenceNumber", resource.mpu_sequence_number);
        event.set("itemId", resource.item_id);
        event.set("version", resource.version);
        event.set("path", resource.path);
        event.set("contentType", resource.content_type);
        event.set("size", resource.size);
        event.set("generation", resource.generation);
        return event;
    }

    static val application_state_event(const tlvdemux::ApplicationState& state) {
        auto event = val::object();
        event.set("contextId", state.application.context_id);
        event.set("sourcePacketId", state.application.source_packet_id);
        event.set("applicationType", state.application.application_type);
        event.set("organizationId", state.application.organization_id);
        event.set("applicationId", state.application.application_id);
        event.set("controlCode", state.application.control_code);
        event.set("version", state.application.version);
        event.set("entryPath", state.application.entry_path);
        auto urls = val::array();
        for (std::size_t index = 0;
             index < state.application.transport_urls.size(); ++index) {
            urls.set(index, state.application.transport_urls[index]);
        }
        event.set("transportUrls", urls);
        event.set("state", std::string(application_collection_state_name(state.state)));
        event.set("entryReady", state.entry_ready);
        event.set("resourceCount", state.resource_count);
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
    tlvdemux::ApplicationResourceAssembler application_assembler_;
    std::deque<ApplicationEvent> application_events_;
    tlvdemux::Demuxer demuxer_;
    tlvdemux::ApplicationResourceStore application_resources_;
    WasmMseRemuxer mse_remuxer_;
    tlvdemux::RecordingIndex recording_index_;
    bool index_active_ = false;
    bool index_growing_ = false;
    bool mse_enabled_ = false;
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
        .function("setMseOutputEnabled", &WasmDemuxer::setMseOutputEnabled)
        .function("setSubtitlePassthroughEnabled", &WasmDemuxer::setSubtitlePassthroughEnabled)
        .function("drainApplicationResources", &WasmDemuxer::drainApplicationResources)
        .function("startIndex", &WasmDemuxer::startIndex)
        .function("finalizeIndex", &WasmDemuxer::finalizeIndex)
        .function("indexState", &WasmDemuxer::indexState)
        .function("indexDuration", &WasmDemuxer::indexDuration)
        .function("setIndexDuration", &WasmDemuxer::setIndexDuration)
        .function("seekPointCount", &WasmDemuxer::seekPointCount)
        .function("indexedVideoTrack", &WasmDemuxer::indexedVideoTrack)
        .function("previousSync", &WasmDemuxer::previousSync)
        .function("seekPointsFor", &WasmDemuxer::seekPointsFor)
        .function("estimateOffset", &WasmDemuxer::estimateOffset)
        .function("applicationResources", &WasmDemuxer::applicationResources)
        .function("applicationResource", &WasmDemuxer::applicationResource)
        .function("applicationEntry", &WasmDemuxer::applicationEntry)
        .function("applications", &WasmDemuxer::applications)
        .function("applicationResourceGeneration",
                  &WasmDemuxer::applicationResourceGeneration)
        .function("broadcastClock", &WasmDemuxer::broadcastClock);

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
