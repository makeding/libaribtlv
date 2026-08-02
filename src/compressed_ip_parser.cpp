#include "compressed_ip_parser.hpp"

#include <utility>

namespace tlvdemux::detail {

CompressedIpParser::CompressedIpParser(const Limits& limits, ServiceCallback on_service,
                                       TrackCallback on_track, AccessUnitCallback on_access_unit,
                                       ApplicationServiceCallback on_application_service,
                                       LayoutCallback on_layout,
                                       DataAssetCallback on_data_asset,
                                       DataUnitCallback on_data_unit,
                                       SignallingCallback on_signalling,
                                       EventCallback on_event,
                                       MhSdtCallback on_mh_sdt,
                                       MhTotCallback on_mh_tot,
                                       StreamEventCallback on_stream_event,
                                       ViewerParticipationCallback on_viewer_participation,
                                       ApplicationCallback on_application,
                                       MptSnapshotCallback on_mpt_snapshot,
                                       MhAitSnapshotCallback on_mh_ait_snapshot,
                                       DataTransmissionCallback on_data_transmission,
                                       DataDirectoryCallback on_data_directory,
                                       DataAssetManagementCallback on_data_asset_management,
                                       ErrorCallback on_error)
    : limits_(limits), on_service_(std::move(on_service)), on_track_(std::move(on_track)),
      on_access_unit_(std::move(on_access_unit)),
      on_application_service_(std::move(on_application_service)),
      on_layout_(std::move(on_layout)),
      on_data_asset_(std::move(on_data_asset)), on_data_unit_(std::move(on_data_unit)),
      on_signalling_(std::move(on_signalling)),
      on_event_(std::move(on_event)),
      on_mh_sdt_(std::move(on_mh_sdt)),
      on_mh_tot_(std::move(on_mh_tot)),
      on_stream_event_(std::move(on_stream_event)),
      on_viewer_participation_(std::move(on_viewer_participation)),
      on_application_(std::move(on_application)),
      on_mpt_snapshot_(std::move(on_mpt_snapshot)),
      on_mh_ait_snapshot_(std::move(on_mh_ait_snapshot)),
      on_data_transmission_(std::move(on_data_transmission)),
      on_data_directory_(std::move(on_data_directory)),
      on_data_asset_management_(std::move(on_data_asset_management)),
      on_error_(std::move(on_error)) {}

void CompressedIpParser::reset() {
    contexts_.clear();
    active_packet_states_ = 0;
}

void CompressedIpParser::select_service(std::optional<std::uint32_t> context_id) {
    if (selected_service_ == context_id) return;
    selected_service_ = context_id;
    reset();
}

void CompressedIpParser::flush() {
    for (auto& entry : contexts_) {
        entry.second->flush();
    }
}

MmtpParser* CompressedIpParser::context(const std::uint32_t context_id,
                                        const std::uint64_t input_offset) {
    const auto found = contexts_.find(context_id);
    if (found != contexts_.end()) {
        return found->second.get();
    }
    if (contexts_.size() >= limits_.max_contexts) {
        on_error_(ErrorCode::ResourceLimit, input_offset, true,
                  "compressed-IP context limit exceeded");
        return nullptr;
    }
    auto parser = std::make_unique<MmtpParser>(
        context_id, limits_,
        [this](const std::uint32_t id, std::vector<std::uint8_t> package_id) {
            on_service_(ServiceInfo{id, std::move(package_id)});
        },
        on_track_, on_access_unit_, on_application_service_, on_layout_,
        on_data_asset_, on_data_unit_,
        on_signalling_, on_event_, on_mh_sdt_, on_mh_tot_,
        on_stream_event_, on_viewer_participation_,
        on_application_, on_mpt_snapshot_, on_mh_ait_snapshot_, on_data_transmission_,
        on_data_directory_, on_data_asset_management_,
        [this]() {
            if (active_packet_states_ >= limits_.max_packet_states) return false;
            ++active_packet_states_;
            return true;
        },
        [this]() {
            if (active_packet_states_ != 0) --active_packet_states_;
        },
        on_error_);
    auto* result = parser.get();
    contexts_.emplace(context_id, std::move(parser));
    on_service_(ServiceInfo{context_id, {}});
    return result;
}

void CompressedIpParser::consume(const TlvPacketView& packet) {
    switch (packet.type) {
    case 0x02:
        parse_ipv6(packet);
        break;
    case 0x03:
        parse_compressed(packet);
        break;
    case 0xfe:
    case 0xff:
        break;
    default:
        on_error_(ErrorCode::UnsupportedFeature, packet.input_offset, true,
                  "unsupported TLV packet type");
        break;
    }
}

void CompressedIpParser::parse_ipv6(const TlvPacketView& packet) {
    if (packet.size < 48) {
        on_error_(ErrorCode::MalformedInput, packet.input_offset, true,
                  "truncated IPv6/UDP TLV payload");
        return;
    }
    if ((packet.payload[0] >> 4U) != 6 || packet.payload[6] != 17) {
        on_error_(ErrorCode::UnsupportedFeature, packet.input_offset, true,
                  "unsupported IPv6 next-header in TLV payload");
        return;
    }
    const auto ipv6_payload_size = static_cast<std::size_t>(read_be16(packet.payload + 4));
    if (ipv6_payload_size != packet.size - 40) {
        on_error_(ErrorCode::MalformedInput, packet.input_offset, true,
                  "IPv6 payload length does not match TLV payload");
        return;
    }
    const auto* udp = packet.payload + 40;
    const auto udp_size = static_cast<std::size_t>(read_be16(udp + 4));
    if (udp_size < 8 || udp_size != packet.size - 40) {
        on_error_(ErrorCode::MalformedInput, packet.input_offset, true,
                  "UDP length does not match IPv6 payload");
        return;
    }
    const auto destination_port = read_be16(udp + 2);
    if (destination_port != 123) {
        on_error_(ErrorCode::UnsupportedFeature, packet.input_offset, true,
                  "unsupported IPv6 UDP payload (only NTP is recognized in v1)");
    }
}

void CompressedIpParser::parse_compressed(const TlvPacketView& packet) {
    if (packet.size < 3) {
        on_error_(ErrorCode::MalformedInput, packet.input_offset, true,
                  "truncated compressed-IP header");
        return;
    }
    const auto context_id = static_cast<std::uint32_t>(read_be16(packet.payload) >> 4U);
    if (selected_service_.has_value() && *selected_service_ != context_id) return;
    const auto mode = packet.payload[2];
    std::size_t cursor = 3;
    std::size_t compressed_header_size = 0;
    if (mode == 0x20) {
        // ARIB STD-B32 Part 3 3.7: partial IPv4 (16 bytes) and
        // partial UDP (4 bytes).
        compressed_header_size = 16 + 4;
    } else if (mode == 0x21) {
        compressed_header_size = 2; // IPv4 identifier
    } else if (mode == 0x60) {
        compressed_header_size = 38 + 4; // partial IPv6 and partial UDP
    } else if (mode != 0x61) {
        on_error_(ErrorCode::MalformedInput, packet.input_offset, true,
                  "unsupported compressed-IP context identification mode");
        return;
    }
    if (packet.size - cursor < compressed_header_size) {
        on_error_(ErrorCode::MalformedInput, packet.input_offset, true,
                  "truncated compressed-IP context header");
        return;
    }
    cursor += compressed_header_size;

    auto* parser = context(context_id, packet.input_offset);
    if (parser == nullptr) {
        return;
    }
    parser->push(packet.payload + cursor, packet.size - cursor, packet.input_offset);
}

} // namespace tlvdemux::detail
