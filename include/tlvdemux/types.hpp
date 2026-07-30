#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tlvdemux {

enum class Codec { Hevc, AacLatm, Ttml };
enum class TrackKind { Video, Audio, Subtitle };

enum class AudioChannelLayout {
    Unknown,
    Mono,
    DualMono,
    Stereo,
    Channels2_1,
    Channels3_0,
    Channels2_2,
    Channels4_0,
    Channels5_0,
    Channels5_1,
    Channels3_3_1,
    Channels6_1,
    Channels7_1,
    Channels10_2,
    Channels22_2,
};

struct AudioInfo {
    std::uint8_t stream_content = 0;
    std::uint8_t component_type = 0;
    std::uint16_t component_tag = 0;
    AudioChannelLayout channel_layout = AudioChannelLayout::Unknown;
    std::uint8_t stream_type = 0;
    std::uint8_t simulcast_group_tag = 0;
    bool es_multi_lingual = false;
    bool main_component = false;
    std::uint8_t quality_indicator = 0;
    std::uint8_t sampling_rate_code = 0;
    std::uint32_t sample_rate = 0;
    std::string secondary_language;
};

struct Timestamp {
    std::int64_t value = 0;
    std::uint32_t timescale = 1;
};

struct SubtitleInfo {
    std::uint8_t tag = 0;
    std::uint8_t info_version = 0;
    std::uint8_t type = 0;
    std::uint8_t format = 0;
    std::uint8_t operation_mode = 0;
    std::uint8_t timing_mode = 0;
    std::uint8_t display_mode = 0;
    std::uint8_t resolution = 0;
    std::uint8_t compression_type = 0;
    std::optional<std::uint32_t> start_mpu_sequence_number;
    // ARIB STD-B60 reference_start_time in unsigned 64-bit NTP format.
    std::optional<std::uint64_t> reference_start_ntp;
};

struct ServiceInfo {
    std::uint32_t context_id = 0;
    std::vector<std::uint8_t> package_id;
};

struct ApplicationServiceInfo {
    std::uint32_t context_id = 0;
    std::uint8_t application_format = 0;
    std::uint8_t document_resolution = 0;
    bool default_ait = false;
    bool has_data_transmission_messages = false;
    std::optional<std::uint16_t> ait_packet_id;
    std::optional<std::uint16_t> data_transmission_packet_id;
};

struct DataAssetInfo {
    std::uint32_t context_id = 0;
    std::uint16_t packet_id = 0;
    std::vector<std::uint8_t> asset_id;
    std::string asset_type;
    std::uint16_t component_tag = 0;
};

struct DataUnit {
    std::uint32_t context_id = 0;
    std::uint16_t packet_id = 0;
    std::vector<std::uint8_t> asset_id;
    std::string asset_type;
    std::uint16_t component_tag = 0;
    std::uint32_t mpu_sequence_number = 0;
    std::uint32_t item_id = 0;
    std::optional<std::uint32_t> download_id;
    std::optional<std::uint32_t> item_fragment_number;
    std::optional<std::uint32_t> last_item_fragment_number;
    std::vector<std::uint8_t> data;
    std::uint64_t input_offset = 0;
    bool discontinuity = false;
};

struct SignallingMessage {
    std::uint32_t context_id = 0;
    std::uint16_t packet_id = 0;
    std::uint16_t message_id = 0;
    std::vector<std::uint8_t> data;
    std::uint64_t input_offset = 0;
};

struct ApplicationInfo {
    std::uint32_t context_id = 0;
    std::uint16_t source_packet_id = 0;
    std::uint16_t application_type = 0;
    std::uint16_t organization_id = 0;
    std::uint32_t application_id = 0;
    std::uint8_t control_code = 0;
    std::uint8_t version = 0;
    std::string entry_path;
    std::vector<std::string> transport_urls;
};

struct DataTransmissionTable {
    std::uint32_t context_id = 0;
    std::uint16_t source_packet_id = 0;
    std::uint8_t table_id = 0;
    std::uint8_t session_id = 0;
    std::uint8_t version = 0;
    std::uint8_t section_number = 0;
    std::uint8_t last_section_number = 0;
    std::vector<std::uint8_t> data;
    std::uint64_t input_offset = 0;
};

struct DataDirectoryFile {
    std::uint16_t node_tag = 0;
    std::string name;
};

struct DataDirectoryNode {
    std::uint16_t node_tag = 0;
    std::uint8_t version = 0;
    std::string path;
    std::vector<DataDirectoryFile> files;
};

struct DataDirectoryTable {
    std::uint32_t context_id = 0;
    std::uint8_t session_id = 0;
    std::uint8_t version = 0;
    std::uint8_t section_number = 0;
    std::uint8_t last_section_number = 0;
    std::string base_path;
    std::vector<DataDirectoryNode> directories;
};

struct DataAssetItem {
    std::uint16_t node_tag = 0;
    std::optional<std::uint32_t> item_id;
    std::optional<std::uint32_t> size;
    std::optional<std::uint8_t> version;
    std::optional<std::uint32_t> checksum;
    std::vector<std::uint8_t> info;
};

struct DataAssetMpu {
    std::uint32_t sequence_number = 0;
    std::uint32_t size = 0;
    bool index_item = false;
    std::optional<std::uint32_t> index_item_id;
    std::uint8_t index_item_compression_type = 0;
    std::vector<DataAssetItem> items;
    std::vector<std::uint8_t> info;
};

struct DataAssetManagementTable {
    std::uint32_t context_id = 0;
    std::uint8_t session_id = 0;
    std::uint8_t version = 0;
    std::uint8_t section_number = 0;
    std::uint8_t last_section_number = 0;
    std::uint32_t transaction_id = 0;
    std::uint16_t component_tag = 0;
    std::uint32_t download_id = 0;
    std::vector<DataAssetMpu> mpus;
    std::vector<std::uint8_t> component_info;
};

struct TrackInfo {
    std::uint64_t track_id = 0;
    std::uint32_t context_id = 0;
    std::uint16_t packet_id = 0;
    std::vector<std::uint8_t> asset_id;
    TrackKind kind = TrackKind::Video;
    Codec codec = Codec::Hevc;
    std::string language;
    std::uint16_t component_tag = 0;
    std::uint32_t timescale = 1;
    std::optional<AudioInfo> audio;
    std::optional<SubtitleInfo> subtitle;
};

struct SubtitleResource {
    std::uint8_t subsample_number = 0;
    std::uint8_t data_type = 0;
    std::vector<std::uint8_t> data;
};

struct AccessUnit {
    std::uint64_t track_id = 0;
    Codec codec = Codec::Hevc;
    std::vector<std::uint8_t> data;
    std::vector<SubtitleResource> subtitle_resources;
    Timestamp pts;
    Timestamp dts;
    std::optional<Timestamp> source_ntp;
    std::optional<std::uint32_t> mpu_sequence_number;
    // Media-timeline position corresponding to SubtitleInfo::reference_start_ntp.
    std::optional<Timestamp> subtitle_reference_start_pts;
    std::uint64_t restart_offset = 0;
    std::uint64_t input_offset = 0;
    bool random_access = false;
    bool discontinuity = false;
};

enum class ErrorCode {
    MalformedInput,
    UnsupportedFeature,
    Discontinuity,
    ResourceLimit,
};

struct Error {
    ErrorCode code = ErrorCode::MalformedInput;
    std::uint64_t input_offset = 0;
    bool recoverable = true;
    std::string message;
};

struct Limits {
    std::size_t max_tlv_payload = 65535;
    std::size_t max_resync_buffer = 1024 * 1024;
    std::size_t max_signalling_message = 1024 * 1024;
    std::size_t max_access_unit = 16 * 1024 * 1024;
    std::size_t max_ttml_sample = 4 * 1024 * 1024;
    std::size_t max_contexts = 64;
    std::size_t max_packet_states = 256;
};

} // namespace tlvdemux
