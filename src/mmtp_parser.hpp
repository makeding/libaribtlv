#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "parser_common.hpp"

namespace tlvdemux::detail {

struct TimestampMapping {
    std::uint64_t ntp = 0;
    std::uint64_t restart_offset = 0;
};

struct ExtendedTimestampMapping {
    std::uint16_t decoding_time_offset = 0;
    std::vector<std::uint16_t> dts_pts_offsets;
    std::vector<std::uint16_t> pts_offsets;
    std::uint64_t restart_offset = 0;
};

struct AssetMetadata {
    std::string language;
    std::uint16_t component_tag = 0;
    std::uint32_t timescale = 1;
    std::optional<AudioInfo> audio;
    std::optional<SubtitleInfo> subtitle;
    std::vector<MpuPresentationRegion> presentation_regions;
    bool aac_latm = false;
    bool ttml = false;
    std::map<std::uint32_t, TimestampMapping> timestamps;
    std::map<std::uint32_t, ExtendedTimestampMapping> extended_timestamps;
};

struct TimedAccessUnit {
    AccessUnit unit;
    std::uint64_t source_ntp_raw = 0;
};

class MmtpParser {
public:
    using PackageCallback = std::function<void(std::uint32_t, std::vector<std::uint8_t>)>;
    using TrackCallback = std::function<std::uint64_t(TrackInfo)>;
    using AccessUnitCallback = std::function<void(TimedAccessUnit)>;
    using ApplicationServiceCallback = std::function<void(ApplicationServiceInfo)>;
    using LayoutCallback = std::function<void(LayoutConfiguration)>;
    using DataAssetCallback = std::function<void(DataAssetInfo)>;
    using DataUnitCallback = std::function<void(DataUnit)>;
    using SignallingCallback = std::function<void(SignallingMessage)>;
    using EventCallback = std::function<void(EventInfo)>;
    using StreamEventCallback = std::function<void(StreamEvent)>;
    using ViewerParticipationCallback =
        std::function<void(ViewerParticipationNotification)>;
    using ApplicationCallback = std::function<void(ApplicationInfo)>;
    using DataTransmissionCallback = std::function<void(DataTransmissionTable)>;
    using DataDirectoryCallback = std::function<void(DataDirectoryTable)>;
    using DataAssetManagementCallback = std::function<void(DataAssetManagementTable)>;
    using StateAcquireCallback = std::function<bool()>;
    using StateReleaseCallback = std::function<void()>;

    MmtpParser(std::uint32_t context_id, const Limits&, PackageCallback,
               TrackCallback, AccessUnitCallback, ApplicationServiceCallback,
               LayoutCallback, DataAssetCallback, DataUnitCallback, SignallingCallback, EventCallback,
               StreamEventCallback, ViewerParticipationCallback,
               ApplicationCallback,
               DataTransmissionCallback, DataDirectoryCallback,
               DataAssetManagementCallback, StateAcquireCallback,
               StateReleaseCallback, ErrorCallback);
    ~MmtpParser();

    void push(const std::uint8_t* data, std::size_t size, std::uint64_t input_offset);
    void flush();
    void reset();

    struct PacketExtensions {
        std::optional<std::size_t> authenticated_payload_size;
        std::optional<std::uint32_t> download_id;
        std::optional<std::uint32_t> item_fragment_number;
        std::optional<std::uint32_t> last_item_fragment_number;
    };

private:
    enum class FragmentState { Initial, Idle, Collecting, Skipping };

    struct SignallingAssembler {
        FragmentState state = FragmentState::Initial;
        std::uint32_t last_sequence = 0;
        std::uint64_t input_offset = 0;
        std::vector<std::uint8_t> data;
    };

    struct MediaAssembler {
        FragmentState state = FragmentState::Initial;
        std::uint32_t last_packet_sequence = 0;
        std::uint32_t mpu_sequence = 0;
        std::uint32_t sample_number = 0;
        std::uint64_t input_offset = 0;
        std::uint64_t restart_offset = 0;
        bool random_access = false;
        std::optional<std::uint32_t> download_id;
        std::optional<std::uint32_t> item_fragment_number;
        std::optional<std::uint32_t> last_item_fragment_number;
        std::vector<std::uint8_t> data;
    };

    struct DataAssetState {
        DataAssetInfo info;
        MediaAssembler media;
        bool discontinuity = false;
    };

    struct PendingHevc {
        bool active = false;
        std::uint32_t mpu_sequence = 0;
        std::uint32_t sample_number = 0;
        std::uint64_t input_offset = 0;
        std::uint64_t restart_offset = 0;
        bool random_access = false;
        bool has_vcl = false;
        std::vector<std::uint8_t> data;
    };

    struct SubtitleAssembly {
        struct Subsample {
            std::uint8_t data_type = 0;
            std::vector<std::uint8_t> data;
        };

        bool active = false;
        std::uint8_t sequence = 0;
        std::uint8_t last_subsample = 0;
        std::uint32_t mpu_sequence = 0;
        std::uint64_t input_offset = 0;
        std::uint64_t restart_offset = 0;
        bool random_access = false;
        std::vector<std::optional<Subsample>> subsamples;
    };

    struct TrackState {
        TrackInfo info;
        std::uint64_t stable_track_id = 0;
        std::map<std::uint32_t, TimestampMapping> timestamps;
        std::map<std::uint32_t, ExtendedTimestampMapping> extended_timestamps;
        std::map<std::uint32_t, std::uint32_t> delivery_timestamps;
        std::uint64_t restart_offset = 0;
        std::optional<std::uint32_t> current_mpu_sequence;
        std::size_t au_index = 0;
        std::int64_t dts_offset_accumulator = 0;
        bool wait_for_rap = false;
        bool discontinuity = false;
        MediaAssembler media;
        PendingHevc pending_hevc;
        SubtitleAssembly subtitle;
    };

    void parse_signalling(std::uint16_t packet_id, std::uint32_t sequence,
                          const std::uint8_t* data, std::size_t size,
                          std::uint64_t input_offset);
    void parse_mpu(std::uint16_t packet_id, std::uint32_t packet_sequence,
                   std::uint32_t delivery_timestamp, bool random_access,
                   const std::uint8_t* data,
                   std::size_t size, std::uint64_t input_offset,
                   const PacketExtensions&);
    void consume_data_piece(DataAssetState&, std::uint32_t packet_sequence,
                            std::uint32_t mpu_sequence, std::uint8_t fragmentation,
                            bool aggregation, const std::uint8_t*, std::size_t,
                            std::uint64_t input_offset, const PacketExtensions&);
    void emit_data_unit(DataAssetState&, std::uint32_t mpu_sequence,
                        std::uint32_t item_id, const std::uint8_t*, std::size_t,
                        std::uint64_t input_offset, const PacketExtensions&);
    void consume_mfu_piece(TrackState&, std::uint32_t packet_sequence,
                           std::uint32_t mpu_sequence, bool timed,
                           std::uint8_t fragmentation, bool aggregation,
                           bool random_access,
                           const std::uint8_t* data, std::size_t size,
                           std::uint64_t input_offset);
    void consume_complete_mfu(TrackState&, std::uint32_t mpu_sequence,
                              std::uint32_t sample_number, bool random_access,
                              const std::uint8_t* data, std::size_t size,
                              std::uint64_t input_offset, std::uint64_t restart_offset);
    bool append_media(TrackState&, const std::uint8_t*, std::size_t,
                      std::uint64_t input_offset);
    void emit_access_unit(TrackState&, std::uint32_t mpu_sequence,
                          std::vector<std::uint8_t>, bool random_access,
                          std::uint64_t input_offset, std::uint64_t restart_offset,
                          std::vector<SubtitleResource> subtitle_resources = {});
    void finalize_hevc(TrackState&);
    void install_track(TrackInfo, AssetMetadata, std::uint64_t input_offset);
    void release_all_states();
    void accept_signalling_unit(std::uint16_t packet_id,
                                const std::uint8_t* data, std::size_t size,
                                std::uint64_t input_offset);
    bool parse_pa_message(std::uint16_t packet_id,
                          const std::uint8_t* data, std::size_t size,
                          std::uint64_t input_offset);
    bool parse_m2_message(std::uint16_t packet_id,
                          const std::uint8_t* data, std::size_t size,
                          std::uint64_t input_offset);
    bool parse_data_transmission_message(std::uint16_t packet_id,
                                         const std::uint8_t* data, std::size_t size,
                                         std::uint64_t input_offset);
    bool parse_tables(const std::uint8_t* data, std::size_t size,
                      std::uint16_t packet_id,
                      std::uint64_t input_offset);
    bool parse_mpt(const std::uint8_t* data, std::size_t size,
                   std::uint64_t input_offset);
    bool parse_package_list(const std::uint8_t* data, std::size_t size,
                            std::uint64_t input_offset);
    bool parse_lct(const std::uint8_t* data, std::size_t size,
                   std::uint16_t packet_id, std::uint64_t input_offset);
    bool parse_mh_ait(const std::uint8_t* data, std::size_t size,
                      std::uint16_t packet_id, std::uint64_t input_offset);
    bool parse_mh_eit(const std::uint8_t* data, std::size_t size,
                      std::uint16_t packet_id, std::uint64_t input_offset);
    bool parse_emt(const std::uint8_t* data, std::size_t size,
                   std::uint16_t packet_id, std::uint64_t input_offset);
    bool parse_data_directory_table(const DataTransmissionTable&);
    bool parse_data_asset_management_table(const DataTransmissionTable&);
    bool append(SignallingAssembler&, const std::uint8_t*, std::size_t,
                std::uint64_t input_offset);

    std::uint32_t context_id_;
    Limits limits_;
    PackageCallback on_package_;
    TrackCallback on_track_;
    AccessUnitCallback on_access_unit_;
    ApplicationServiceCallback on_application_service_;
    LayoutCallback on_layout_;
    DataAssetCallback on_data_asset_;
    DataUnitCallback on_data_unit_;
    SignallingCallback on_signalling_;
    EventCallback on_event_;
    StreamEventCallback on_stream_event_;
    ViewerParticipationCallback on_viewer_participation_;
    ApplicationCallback on_application_;
    DataTransmissionCallback on_data_transmission_;
    DataDirectoryCallback on_data_directory_;
    DataAssetManagementCallback on_data_asset_management_;
    StateAcquireCallback acquire_state_;
    StateReleaseCallback release_state_;
    ErrorCallback on_error_;
    std::unordered_map<std::uint16_t, SignallingAssembler> signalling_;
    std::unordered_map<std::uint16_t, TrackState> tracks_;
    std::unordered_map<std::uint16_t, DataAssetState> data_assets_;
    std::unordered_map<std::uint16_t, std::uint8_t> event_message_tags_;
    std::optional<std::uint64_t> latest_full_ntp_;
};

} // namespace tlvdemux::detail
