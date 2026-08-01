#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>

#include "mmtp_parser.hpp"
#include "tlv_parser.hpp"

namespace tlvdemux::detail {

class CompressedIpParser {
public:
    using ServiceCallback = std::function<void(ServiceInfo)>;
    using TrackCallback = std::function<std::uint64_t(TrackInfo)>;
    using AccessUnitCallback = std::function<void(TimedAccessUnit)>;
    using ApplicationServiceCallback = std::function<void(ApplicationServiceInfo)>;
    using LayoutCallback = std::function<void(LayoutConfiguration)>;
    using DataAssetCallback = std::function<void(DataAssetInfo)>;
    using DataUnitCallback = std::function<void(DataUnit)>;
    using SignallingCallback = std::function<void(SignallingMessage)>;
    using EventCallback = std::function<void(EventInfo)>;
    using StreamEventCallback = std::function<void(StreamEvent)>;
    using ApplicationCallback = std::function<void(ApplicationInfo)>;
    using DataTransmissionCallback = std::function<void(DataTransmissionTable)>;
    using DataDirectoryCallback = std::function<void(DataDirectoryTable)>;
    using DataAssetManagementCallback = std::function<void(DataAssetManagementTable)>;

    CompressedIpParser(const Limits&, ServiceCallback, TrackCallback,
                       AccessUnitCallback, ApplicationServiceCallback,
                       LayoutCallback, DataAssetCallback, DataUnitCallback, SignallingCallback, EventCallback,
                       StreamEventCallback,
                       ApplicationCallback,
                       DataTransmissionCallback, DataDirectoryCallback,
                       DataAssetManagementCallback, ErrorCallback);

    void consume(const TlvPacketView&);
    void flush();
    void reset();
    void select_service(std::optional<std::uint32_t> context_id);

private:
    MmtpParser* context(std::uint32_t context_id, std::uint64_t input_offset);
    void parse_ipv6(const TlvPacketView&);
    void parse_compressed(const TlvPacketView&);

    Limits limits_;
    ServiceCallback on_service_;
    TrackCallback on_track_;
    AccessUnitCallback on_access_unit_;
    ApplicationServiceCallback on_application_service_;
    LayoutCallback on_layout_;
    DataAssetCallback on_data_asset_;
    DataUnitCallback on_data_unit_;
    SignallingCallback on_signalling_;
    EventCallback on_event_;
    StreamEventCallback on_stream_event_;
    ApplicationCallback on_application_;
    DataTransmissionCallback on_data_transmission_;
    DataDirectoryCallback on_data_directory_;
    DataAssetManagementCallback on_data_asset_management_;
    ErrorCallback on_error_;
    std::optional<std::uint32_t> selected_service_;
    std::size_t active_packet_states_ = 0;
    std::unordered_map<std::uint32_t, std::unique_ptr<MmtpParser>> contexts_;
};

} // namespace tlvdemux::detail
