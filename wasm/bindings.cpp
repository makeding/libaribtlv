#include <tlvdemux/demuxer.hpp>

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
    void reset() { demuxer_.reset(); }

    void reposition(const std::uint64_t input_offset, const bool preserve_timeline) {
        demuxer_.reposition(tlvdemux::RepositionOptions{input_offset, preserve_timeline});
    }

    void selectService(const val& context_id) {
        demuxer_.selectService(optional_number<std::uint32_t>(context_id));
    }

    void selectTrack(const std::string& kind, const val& track_id) {
        std::optional<tlvdemux::TrackKind> parsed_kind;
        if (kind == "video") parsed_kind = tlvdemux::TrackKind::Video;
        if (kind == "audio") parsed_kind = tlvdemux::TrackKind::Audio;
        if (kind == "subtitle") parsed_kind = tlvdemux::TrackKind::Subtitle;
        if (!parsed_kind.has_value()) return;
        demuxer_.selectTrack(*parsed_kind, optional_number<std::uint64_t>(track_id));
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
        auto event = val::object();
        event.set("trackId", unit.track_id);
        event.set("codec", std::string(codec_name(unit.codec)));
        event.set("data", copy_bytes(unit.data));
        event.set("ptsValue", unit.pts.value);
        event.set("ptsTimescale", unit.pts.timescale);
        event.set("dtsValue", unit.dts.value);
        event.set("dtsTimescale", unit.dts.timescale);
        event.set("restartOffset", unit.restart_offset);
        event.set("inputOffset", unit.input_offset);
        event.set("randomAccess", unit.random_access);
        event.set("discontinuity", unit.discontinuity);
        emit("onAccessUnit", event);
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
    template <typename T>
    static std::optional<T> optional_number(const val& value) {
        if (value.isNull() || value.isUndefined()) return std::nullopt;
        return value.as<T>();
    }

    void emit(const char* name, const val& event) {
        if (callbacks_.isNull() || callbacks_.isUndefined()) return;
        const auto callback = callbacks_[name];
        if (callback.typeOf().as<std::string>() != "function") return;
        callback.call<void>("call", callbacks_, event);
    }

    val callbacks_;
    tlvdemux::Demuxer demuxer_;
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
        .function("selectTrack", &WasmDemuxer::selectTrack);
}

int main() {
    return 0;
}
