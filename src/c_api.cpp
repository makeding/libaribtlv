#include <aribtlv/aribtlv.h>

#include <aribtlv/demuxer.hpp>

#include <algorithm>
#include <cstring>
#include <exception>
#include <new>
#include <optional>
#include <string>
#include <utility>

namespace {

aribtlv_codec codec(const aribtlv::Codec value) noexcept {
    switch (value) {
    case aribtlv::Codec::Hevc: return ARIBTLV_CODEC_HEVC;
    case aribtlv::Codec::AacLatm: return ARIBTLV_CODEC_AAC_LATM;
    case aribtlv::Codec::Ttml: return ARIBTLV_CODEC_TTML;
    }
    return ARIBTLV_CODEC_HEVC;
}

aribtlv_track_kind track_kind(const aribtlv::TrackKind value) noexcept {
    switch (value) {
    case aribtlv::TrackKind::Video: return ARIBTLV_TRACK_VIDEO;
    case aribtlv::TrackKind::Audio: return ARIBTLV_TRACK_AUDIO;
    case aribtlv::TrackKind::Subtitle: return ARIBTLV_TRACK_SUBTITLE;
    }
    return ARIBTLV_TRACK_VIDEO;
}

std::optional<aribtlv::TrackKind> track_kind(const aribtlv_track_kind value) noexcept {
    switch (value) {
    case ARIBTLV_TRACK_VIDEO: return aribtlv::TrackKind::Video;
    case ARIBTLV_TRACK_AUDIO: return aribtlv::TrackKind::Audio;
    case ARIBTLV_TRACK_SUBTITLE: return aribtlv::TrackKind::Subtitle;
    }
    return std::nullopt;
}

aribtlv_error_code error_code(const aribtlv::ErrorCode value) noexcept {
    switch (value) {
    case aribtlv::ErrorCode::MalformedInput: return ARIBTLV_ERROR_MALFORMED_INPUT;
    case aribtlv::ErrorCode::UnsupportedFeature: return ARIBTLV_ERROR_UNSUPPORTED_FEATURE;
    case aribtlv::ErrorCode::Discontinuity: return ARIBTLV_ERROR_DISCONTINUITY;
    case aribtlv::ErrorCode::ResourceLimit: return ARIBTLV_ERROR_RESOURCE_LIMIT;
    }
    return ARIBTLV_ERROR_MALFORMED_INPUT;
}

aribtlv_timestamp timestamp(const aribtlv::Timestamp value) noexcept {
    return {value.value, value.timescale};
}

aribtlv_track_info track_info(const aribtlv::TrackInfo& source) noexcept {
    aribtlv_track_info result{};
    result.track_id = source.track_id;
    result.context_id = source.context_id;
    result.packet_id = source.packet_id;
    result.component_tag = source.component_tag;
    result.kind = track_kind(source.kind);
    result.codec = codec(source.codec);
    result.timescale = source.timescale;
    result.language = source.language.c_str();
    if (source.audio) {
        result.has_audio = 1;
        result.audio_main_component = source.audio->main_component ? 1 : 0;
        result.audio_sample_rate = source.audio->sample_rate;
        result.audio_channels = aribtlv::audio_channel_count(source.audio->channel_layout);
    }
    return result;
}

class CallbackSink final : public aribtlv::Sink {
public:
    CallbackSink(aribtlv_callbacks callbacks, void* opaque)
        : callbacks_(callbacks), opaque_(opaque) {}

    void beginCall() {
        fatal_error_ = false;
        last_error_.clear();
    }

    bool fatalError() const noexcept { return fatal_error_; }
    const std::string& lastError() const noexcept { return last_error_; }
    void setLastError(std::string message) { last_error_ = std::move(message); }

    void onService(const aribtlv::ServiceInfo& source) override {
        if (!callbacks_.on_service) return;
        const aribtlv_service_info event{
            source.context_id,
            source.package_id.empty() ? nullptr : source.package_id.data(),
            source.package_id.size(),
        };
        callbacks_.on_service(opaque_, &event);
    }

    void onTrack(const aribtlv::TrackInfo& source) override {
        if (!callbacks_.on_track) return;
        const auto event = track_info(source);
        callbacks_.on_track(opaque_, &event);
    }

    void onTrackRemoved(const aribtlv::TrackInfo& source) override {
        if (!callbacks_.on_track_removed) return;
        const auto event = track_info(source);
        callbacks_.on_track_removed(opaque_, &event);
    }

    void onAccessUnit(aribtlv::AccessUnit&& source) override {
        if (!callbacks_.on_access_unit) return;
        const aribtlv_access_unit event{
            source.track_id,
            codec(source.codec),
            source.component_tag,
            source.data.empty() ? nullptr : source.data.data(),
            source.data.size(),
            timestamp(source.pts),
            timestamp(source.dts),
            source.restart_offset,
            source.input_offset,
            static_cast<std::uint8_t>(source.random_access ? 1 : 0),
            static_cast<std::uint8_t>(source.discontinuity ? 1 : 0),
        };
        callbacks_.on_access_unit(opaque_, &event);
    }

    void onError(const aribtlv::Error& source) override {
        if (!source.recoverable) {
            fatal_error_ = true;
            last_error_ = source.message;
        }
        if (!callbacks_.on_error) return;
        const aribtlv_error event{
            error_code(source.code),
            source.input_offset,
            static_cast<std::uint8_t>(source.recoverable ? 1 : 0),
            source.message.c_str(),
        };
        callbacks_.on_error(opaque_, &event);
    }

private:
    aribtlv_callbacks callbacks_{};
    void* opaque_ = nullptr;
    bool fatal_error_ = false;
    std::string last_error_;
};

} // namespace

struct aribtlv_demuxer {
    aribtlv_demuxer(aribtlv_callbacks callbacks, void* opaque, aribtlv::Limits limits)
        : sink(callbacks, opaque), implementation(sink, limits) {}

    CallbackSink sink;
    aribtlv::Demuxer implementation;
};

namespace {

template <typename Operation>
int invoke(aribtlv_demuxer* demuxer, Operation operation) noexcept {
    if (!demuxer) return ARIBTLV_ERROR_INVALID_ARGUMENT;
    demuxer->sink.beginCall();
    try {
        operation(demuxer->implementation);
        return demuxer->sink.fatalError() ? ARIBTLV_ERROR_DEMUX : ARIBTLV_OK;
    } catch (const std::bad_alloc&) {
        demuxer->sink.setLastError("out of memory");
        return ARIBTLV_ERROR_OUT_OF_MEMORY;
    } catch (const std::exception& error) {
        demuxer->sink.setLastError(error.what());
        return ARIBTLV_ERROR_INTERNAL;
    } catch (...) {
        demuxer->sink.setLastError("unknown C++ exception");
        return ARIBTLV_ERROR_INTERNAL;
    }
}

} // namespace

extern "C" {

uint32_t aribtlv_version(void) { return ARIBTLV_VERSION_INT; }

const char* aribtlv_version_string(void) { return ARIBTLV_VERSION_STRING; }

void aribtlv_callbacks_init(aribtlv_callbacks* callbacks) {
    if (!callbacks) return;
    std::memset(callbacks, 0, sizeof(*callbacks));
    callbacks->struct_size = sizeof(*callbacks);
}

void aribtlv_config_init(aribtlv_config* config) {
    if (!config) return;
    std::memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->collect_application_resources = 1;
}

aribtlv_demuxer* aribtlv_demuxer_create(
    const aribtlv_config* config, const aribtlv_callbacks* callbacks, void* opaque) {
    try {
        aribtlv_callbacks copied_callbacks{};
        if (callbacks) {
            if (callbacks->struct_size < sizeof(callbacks->struct_size)) return nullptr;
            std::memcpy(&copied_callbacks, callbacks,
                        std::min(callbacks->struct_size, sizeof(copied_callbacks)));
        }
        aribtlv::Limits limits;
        if (config) {
            const auto required = offsetof(aribtlv_config, collect_application_resources) +
                sizeof(config->collect_application_resources);
            if (config->struct_size < required) return nullptr;
            limits.collect_application_resources = config->collect_application_resources != 0;
        }
        return new aribtlv_demuxer(copied_callbacks, opaque, limits);
    } catch (...) {
        return nullptr;
    }
}

void aribtlv_demuxer_destroy(aribtlv_demuxer* demuxer) { delete demuxer; }

int aribtlv_demuxer_push(aribtlv_demuxer* demuxer, const uint8_t* data, const size_t size) {
    if (!data && size != 0) return ARIBTLV_ERROR_INVALID_ARGUMENT;
    return invoke(demuxer, [&](aribtlv::Demuxer& value) { value.push(data, size); });
}

int aribtlv_demuxer_flush(aribtlv_demuxer* demuxer) {
    return invoke(demuxer, [](aribtlv::Demuxer& value) { value.flush(); });
}

int aribtlv_demuxer_reset(aribtlv_demuxer* demuxer) {
    return invoke(demuxer, [](aribtlv::Demuxer& value) { value.reset(); });
}

int aribtlv_demuxer_reposition(aribtlv_demuxer* demuxer, const uint64_t input_offset,
                               const uint8_t preserve_timeline) {
    return invoke(demuxer, [&](aribtlv::Demuxer& value) {
        value.reposition({input_offset, preserve_timeline != 0});
    });
}

int aribtlv_demuxer_select_service(aribtlv_demuxer* demuxer, const uint32_t context_id) {
    return invoke(demuxer, [&](aribtlv::Demuxer& value) { value.selectService(context_id); });
}

int aribtlv_demuxer_clear_service(aribtlv_demuxer* demuxer) {
    return invoke(demuxer, [](aribtlv::Demuxer& value) { value.selectService(std::nullopt); });
}

int aribtlv_demuxer_select_track(aribtlv_demuxer* demuxer, const aribtlv_track_kind kind,
                                 const uint64_t track_id) {
    const auto converted = track_kind(kind);
    if (!converted) return ARIBTLV_ERROR_INVALID_ARGUMENT;
    return invoke(demuxer, [&](aribtlv::Demuxer& value) {
        value.selectTrack(*converted, track_id);
    });
}

int aribtlv_demuxer_clear_track(aribtlv_demuxer* demuxer, const aribtlv_track_kind kind) {
    const auto converted = track_kind(kind);
    if (!converted) return ARIBTLV_ERROR_INVALID_ARGUMENT;
    return invoke(demuxer, [&](aribtlv::Demuxer& value) {
        value.selectTrack(*converted, std::nullopt);
    });
}

int aribtlv_demuxer_set_subtitle_passthrough(aribtlv_demuxer* demuxer,
                                             const uint8_t enabled) {
    return invoke(demuxer, [&](aribtlv::Demuxer& value) {
        value.setSubtitlePassthroughEnabled(enabled != 0);
    });
}

const char* aribtlv_demuxer_last_error(const aribtlv_demuxer* demuxer) {
    return demuxer ? demuxer->sink.lastError().c_str() : "invalid demuxer";
}

} // extern "C"
