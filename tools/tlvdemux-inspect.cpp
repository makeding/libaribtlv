#include <tlvdemux/demuxer.hpp>

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {

const char* codec_name(const tlvdemux::Codec codec) {
    switch (codec) {
    case tlvdemux::Codec::Hevc: return "hevc";
    case tlvdemux::Codec::AacLatm: return "aac-latm";
    case tlvdemux::Codec::Ttml: return "ttml";
    }
    return "unknown";
}

const char* error_name(const tlvdemux::ErrorCode code) {
    switch (code) {
    case tlvdemux::ErrorCode::MalformedInput: return "malformed-input";
    case tlvdemux::ErrorCode::UnsupportedFeature: return "unsupported-feature";
    case tlvdemux::ErrorCode::Discontinuity: return "discontinuity";
    case tlvdemux::ErrorCode::ResourceLimit: return "resource-limit";
    }
    return "unknown";
}

const char* audio_layout_name(const tlvdemux::AudioChannelLayout layout) {
    switch (layout) {
    case tlvdemux::AudioChannelLayout::Unknown: return "unknown";
    case tlvdemux::AudioChannelLayout::Mono: return "mono";
    case tlvdemux::AudioChannelLayout::DualMono: return "dual-mono";
    case tlvdemux::AudioChannelLayout::Stereo: return "stereo";
    case tlvdemux::AudioChannelLayout::Channels2_1: return "2/1";
    case tlvdemux::AudioChannelLayout::Channels3_0: return "3ch";
    case tlvdemux::AudioChannelLayout::Channels2_2: return "2/2";
    case tlvdemux::AudioChannelLayout::Channels4_0: return "4ch";
    case tlvdemux::AudioChannelLayout::Channels5_0: return "5ch";
    case tlvdemux::AudioChannelLayout::Channels5_1: return "5.1ch";
    case tlvdemux::AudioChannelLayout::Channels3_3_1: return "3/3.1ch";
    case tlvdemux::AudioChannelLayout::Channels6_1: return "6.1ch";
    case tlvdemux::AudioChannelLayout::Channels7_1: return "7.1ch";
    case tlvdemux::AudioChannelLayout::Channels10_2: return "10.2ch";
    case tlvdemux::AudioChannelLayout::Channels22_2: return "22.2ch";
    }
    return "unknown";
}

struct Inspector final : tlvdemux::Sink {
    bool list = false;
    bool trace = false;
    std::unordered_map<std::uint64_t, tlvdemux::TrackInfo> tracks;
    std::unordered_set<std::string> signalling;
    std::optional<std::uint64_t> video_track;
    std::optional<std::uint64_t> audio_track;
    std::optional<std::uint64_t> subtitle_track;
    std::optional<std::uint16_t> wanted_video_packet_id;
    std::optional<std::uint16_t> wanted_audio_packet_id;
    std::optional<std::uint16_t> wanted_subtitle_packet_id;
    std::ofstream video;
    std::ofstream audio;
    std::ofstream subtitle;

    void onService(const tlvdemux::ServiceInfo& info) override {
        if (list) {
            std::cerr << "service context=" << info.context_id
                      << " package-id-bytes=" << info.package_id.size() << '\n';
        }
    }

    void onTrack(const tlvdemux::TrackInfo& info) override {
        tracks[info.track_id] = info;
        if (info.kind == tlvdemux::TrackKind::Video && !video_track.has_value() &&
            (!wanted_video_packet_id.has_value() || *wanted_video_packet_id == info.packet_id)) {
            video_track = info.track_id;
        }
        if (info.kind == tlvdemux::TrackKind::Audio && !audio_track.has_value() &&
            (!wanted_audio_packet_id.has_value() || *wanted_audio_packet_id == info.packet_id)) {
            audio_track = info.track_id;
        }
        if (info.kind == tlvdemux::TrackKind::Subtitle && !subtitle_track.has_value() &&
            (!wanted_subtitle_packet_id.has_value() || *wanted_subtitle_packet_id == info.packet_id)) {
            subtitle_track = info.track_id;
        }
        if (list) {
            std::cerr << "track id=" << info.track_id << " context=" << info.context_id
                      << " packet-id=0x" << std::hex << info.packet_id << std::dec
                      << " codec=" << codec_name(info.codec)
                      << " language=" << info.language << " timescale=" << info.timescale;
            if (info.audio.has_value()) {
                std::cerr << " audio-layout=" << audio_layout_name(info.audio->channel_layout)
                          << " component-type=0x" << std::hex
                          << static_cast<unsigned>(info.audio->component_type) << std::dec
                          << " component-tag=0x" << std::hex
                          << info.audio->component_tag << std::dec
                          << " main=" << info.audio->main_component
                          << " sample-rate=" << info.audio->sample_rate;
            }
            std::cerr << '\n';
        }
    }

    void onApplicationService(const tlvdemux::ApplicationServiceInfo& info) override {
        if (!list) return;
        std::cerr << "application-service context=" << info.context_id
                  << " format=0x" << std::hex
                  << static_cast<unsigned>(info.application_format)
                  << " resolution=0x" << static_cast<unsigned>(info.document_resolution)
                  << std::dec << " default-ait=" << info.default_ait
                  << " data-transmission=" << info.has_data_transmission_messages;
        if (info.ait_packet_id.has_value()) {
            std::cerr << " ait-packet-id=0x" << std::hex << *info.ait_packet_id << std::dec;
        }
        if (info.data_transmission_packet_id.has_value()) {
            std::cerr << " data-packet-id=0x" << std::hex
                      << *info.data_transmission_packet_id << std::dec;
        }
        std::cerr << '\n';
    }

    void onDataAsset(const tlvdemux::DataAssetInfo& info) override {
        if (!list) return;
        std::cerr << "data-asset context=" << info.context_id
                  << " packet-id=0x" << std::hex << info.packet_id << std::dec
                  << " type=" << info.asset_type
                  << " component-tag=0x" << std::hex << info.component_tag << std::dec
                  << " asset-id-bytes=" << info.asset_id.size() << '\n';
    }

    void onDataUnit(tlvdemux::DataUnit&& unit) override {
        if (!list) return;
        std::cerr << "data-unit context=" << unit.context_id
                  << " packet-id=0x" << std::hex << unit.packet_id << std::dec
                  << " component-tag=0x" << std::hex << unit.component_tag << std::dec
                  << " mpu=" << unit.mpu_sequence_number
                  << " item=" << unit.item_id << " size=" << unit.data.size();
        if (unit.download_id.has_value()) {
            std::cerr << " download-id=0x" << std::hex << *unit.download_id << std::dec;
        }
        if (unit.item_fragment_number.has_value()) {
            std::cerr << " fragment=" << *unit.item_fragment_number;
            if (unit.last_item_fragment_number.has_value()) {
                std::cerr << '/' << *unit.last_item_fragment_number;
            }
        }
        std::cerr << " discontinuity=" << unit.discontinuity << '\n';
    }

    void onSignallingMessage(tlvdemux::SignallingMessage&& message) override {
        if (!list) return;
        const auto key = std::to_string(message.context_id) + ':' +
            std::to_string(message.packet_id) + ':' + std::to_string(message.message_id);
        if (!signalling.insert(key).second) return;
        std::cerr << "signalling context=" << message.context_id
                  << " packet-id=0x" << std::hex << message.packet_id
                  << " message-id=0x" << message.message_id << std::dec
                  << " size=" << message.data.size() << '\n';
    }

    void onEventInfo(const tlvdemux::EventInfo& info) override {
        if (!list) return;
        std::cerr << "event context=" << info.context_id
                  << " service-id=" << info.service_id
                  << " event-id=" << info.event_id
                  << " table-id=0x" << std::hex << static_cast<unsigned>(info.table_id)
                  << std::dec << " section=" << static_cast<unsigned>(info.section_number)
                  << " title=" << info.title;
        if (info.start_time_unix_milliseconds.has_value()) {
            std::cerr << " start-ms=" << *info.start_time_unix_milliseconds;
        }
        if (info.duration_seconds.has_value()) {
            std::cerr << " duration=" << *info.duration_seconds;
        }
        std::cerr << '\n';
    }

    void onApplication(const tlvdemux::ApplicationInfo& info) override {
        if (!list) return;
        std::cerr << "application context=" << info.context_id
                  << " source-packet-id=0x" << std::hex << info.source_packet_id
                  << " type=0x" << info.application_type
                  << " organization-id=0x" << info.organization_id
                  << " application-id=0x" << info.application_id
                  << " control=0x" << static_cast<unsigned>(info.control_code)
                  << std::dec << " version=" << static_cast<unsigned>(info.version)
                  << " entry=" << info.entry_path;
        for (const auto& url : info.transport_urls) std::cerr << " transport=" << url;
        std::cerr << '\n';
    }

    void onDataTransmissionTable(tlvdemux::DataTransmissionTable&& table) override {
        if (!list) return;
        std::cerr << "data-table context=" << table.context_id
                  << " source-packet-id=0x" << std::hex << table.source_packet_id
                  << " table-id=0x" << static_cast<unsigned>(table.table_id)
                  << std::dec << " session=" << static_cast<unsigned>(table.session_id)
                  << " version=" << static_cast<unsigned>(table.version)
                  << " section=" << static_cast<unsigned>(table.section_number)
                  << '/' << static_cast<unsigned>(table.last_section_number)
                  << " size=" << table.data.size() << '\n';
    }

    void onDataDirectoryTable(const tlvdemux::DataDirectoryTable& table) override {
        if (!list) return;
        std::size_t file_count = 0;
        for (const auto& directory : table.directories) file_count += directory.files.size();
        std::cerr << "data-directory context=" << table.context_id
                  << " session=" << static_cast<unsigned>(table.session_id)
                  << " version=" << static_cast<unsigned>(table.version)
                  << " section=" << static_cast<unsigned>(table.section_number)
                  << '/' << static_cast<unsigned>(table.last_section_number)
                  << " base=" << table.base_path
                  << " directories=" << table.directories.size()
                  << " files=" << file_count << '\n';
        for (const auto& directory : table.directories) {
            std::cerr << "  directory node=0x" << std::hex << directory.node_tag << std::dec
                      << " version=" << static_cast<unsigned>(directory.version)
                      << " path=" << directory.path
                      << " files=" << directory.files.size() << '\n';
            for (const auto& file : directory.files) {
                std::cerr << "    file node=0x" << std::hex << file.node_tag << std::dec
                          << " name=" << file.name << '\n';
            }
        }
    }

    void onDataAssetManagementTable(const tlvdemux::DataAssetManagementTable& table) override {
        if (!list) return;
        std::size_t item_count = 0;
        for (const auto& mpu : table.mpus) item_count += mpu.items.size();
        std::cerr << "data-asset-map context=" << table.context_id
                  << " session=" << static_cast<unsigned>(table.session_id)
                  << " version=" << static_cast<unsigned>(table.version)
                  << " section=" << static_cast<unsigned>(table.section_number)
                  << '/' << static_cast<unsigned>(table.last_section_number)
                  << " component-tag=0x" << std::hex << table.component_tag
                  << " download-id=0x" << table.download_id << std::dec
                  << " mpus=" << table.mpus.size()
                  << " items=" << item_count << '\n';
        for (const auto& mpu : table.mpus) {
            std::cerr << "  mpu sequence=" << mpu.sequence_number
                      << " size=" << mpu.size << " index=" << mpu.index_item;
            if (mpu.index_item_id.has_value()) {
                std::cerr << " index-item-id=0x" << std::hex << *mpu.index_item_id << std::dec;
            }
            std::cerr << " items=" << mpu.items.size() << '\n';
            for (const auto& item : mpu.items) {
                std::cerr << "    item node=0x" << std::hex << item.node_tag << std::dec;
                if (item.item_id.has_value()) std::cerr << " id=" << *item.item_id;
                if (item.size.has_value()) std::cerr << " size=" << *item.size;
                std::cerr << '\n';
            }
        }
    }

    void onAccessUnit(tlvdemux::AccessUnit&& unit) override {
        const auto track = tracks.find(unit.track_id);
        if (trace) {
            std::cerr << "au offset=" << unit.input_offset
                      << " restart-offset=" << unit.restart_offset
                      << " track=" << unit.track_id;
            if (track != tracks.end()) {
                std::cerr << " context=" << track->second.context_id
                          << " packet-id=0x" << std::hex << track->second.packet_id << std::dec;
            }
            std::cerr << " codec=" << codec_name(unit.codec) << " size=" << unit.data.size()
                      << " pts=" << unit.pts.value << '/' << unit.pts.timescale
                      << " dts=" << unit.dts.value << '/' << unit.dts.timescale
                      << " rap=" << unit.random_access
                      << " discontinuity=" << unit.discontinuity << '\n';
        }
        std::ofstream* output = nullptr;
        if (unit.codec == tlvdemux::Codec::Hevc && video_track == unit.track_id) output = &video;
        if (unit.codec == tlvdemux::Codec::AacLatm && audio_track == unit.track_id) output = &audio;
        if (unit.codec == tlvdemux::Codec::Ttml && subtitle_track == unit.track_id) output = &subtitle;
        if (output != nullptr && output->is_open()) {
            output->write(reinterpret_cast<const char*>(unit.data.data()),
                          static_cast<std::streamsize>(unit.data.size()));
        }
    }

    void onError(const tlvdemux::Error& error) override {
        std::cerr << error_name(error.code) << " offset=" << error.input_offset
                  << " recoverable=" << error.recoverable << ": " << error.message << '\n';
    }
};

void usage() {
    std::cerr << "usage: tlvdemux-inspect [--list] [--trace-au] [--service ID]"
                 " [--video FILE] [--video-packet-id ID]"
                 " [--audio FILE] [--audio-packet-id ID]"
                 " [--subtitle FILE] [--subtitle-packet-id ID] INPUT\n";
}

std::uint16_t parse_packet_id(const std::string& value) {
    const auto parsed = std::stoul(value, nullptr, 0);
    if (parsed > 0xffffU) throw std::runtime_error("packet ID is outside the 16-bit range");
    return static_cast<std::uint16_t>(parsed);
}

} // namespace

int main(int argc, char** argv) {
    try {
        Inspector inspector;
        std::optional<std::uint32_t> service;
        std::string input_path;

        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            auto value = [&](const char* option) -> std::string {
                if (++index >= argc) throw std::runtime_error(std::string("missing value for ") + option);
                return argv[index];
            };
            if (argument == "--list") inspector.list = true;
            else if (argument == "--trace-au") inspector.trace = true;
            else if (argument == "--service") service = static_cast<std::uint32_t>(std::stoul(value("--service"), nullptr, 0));
            else if (argument == "--video-packet-id") inspector.wanted_video_packet_id = parse_packet_id(value("--video-packet-id"));
            else if (argument == "--audio-packet-id") inspector.wanted_audio_packet_id = parse_packet_id(value("--audio-packet-id"));
            else if (argument == "--subtitle-packet-id") inspector.wanted_subtitle_packet_id = parse_packet_id(value("--subtitle-packet-id"));
            else if (argument == "--video") {
                const auto path = value("--video");
                inspector.video.open(path, std::ios::binary);
                if (!inspector.video) throw std::runtime_error("cannot open video output: " + path);
            } else if (argument == "--audio") {
                const auto path = value("--audio");
                inspector.audio.open(path, std::ios::binary);
                if (!inspector.audio) throw std::runtime_error("cannot open audio output: " + path);
            } else if (argument == "--subtitle") {
                const auto path = value("--subtitle");
                inspector.subtitle.open(path, std::ios::binary);
                if (!inspector.subtitle) throw std::runtime_error("cannot open subtitle output: " + path);
            }
            else if (argument == "-h" || argument == "--help") { usage(); return 0; }
            else if (!argument.empty() && argument[0] == '-' && argument != "-") throw std::runtime_error("unknown option: " + argument);
            else if (input_path.empty()) input_path = argument;
            else throw std::runtime_error("more than one input path was provided");
        }
        if (input_path.empty()) {
            usage();
            return 2;
        }
        if (!inspector.list && !inspector.trace && !inspector.video.is_open() &&
            !inspector.audio.is_open() && !inspector.subtitle.is_open()) {
            inspector.list = true;
        }

        std::ifstream file;
        std::istream* input = &std::cin;
        if (input_path != "-") {
            file.open(input_path, std::ios::binary);
            if (!file) throw std::runtime_error("cannot open input: " + input_path);
            input = &file;
        }

        tlvdemux::Demuxer demuxer(inspector);
        demuxer.selectService(service);
        std::array<std::uint8_t, 64 * 1024> buffer{};
        while (*input) {
            input->read(reinterpret_cast<char*>(buffer.data()),
                        static_cast<std::streamsize>(buffer.size()));
            const auto count = input->gcount();
            if (count > 0) demuxer.push(buffer.data(), static_cast<std::size_t>(count));
        }
        demuxer.flush();
        if (inspector.video.is_open() && !inspector.video_track.has_value()) {
            throw std::runtime_error("requested video packet ID was not discovered");
        }
        if (inspector.audio.is_open() && !inspector.audio_track.has_value()) {
            throw std::runtime_error("requested audio packet ID was not discovered");
        }
        if (inspector.subtitle.is_open() && !inspector.subtitle_track.has_value()) {
            throw std::runtime_error("requested subtitle packet ID was not discovered");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "tlvdemux-inspect: " << error.what() << '\n';
        return 2;
    }
}
