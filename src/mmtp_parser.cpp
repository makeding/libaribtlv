#include "mmtp_parser.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

#include "byte_reader.hpp"

namespace tlvdemux::detail {

namespace {

bool parse_packet_extensions(const std::uint16_t extension_type,
                             const std::uint8_t* extension,
                             const std::size_t extension_size,
                             MmtpParser::PacketExtensions& result) {
    if (extension_type != 0x0000) return true;

    std::size_t cursor = 0;
    while (cursor < extension_size) {
        if (extension_size - cursor < 4) return false;
        const auto header = read_be16(extension + cursor);
        const auto type = static_cast<std::uint16_t>(header & 0x7fffU);
        const bool end = (header & 0x8000U) != 0;
        const auto size = static_cast<std::size_t>(read_be16(extension + cursor + 2));
        cursor += 4;
        if (size > extension_size - cursor) return false;

        if (type == 0x0001) {
            if (size < 1) return false;
            const auto flags = extension[cursor];
            const bool scramble_system_present = (flags & 0x04U) != 0;
            const bool authentication_present = (flags & 0x02U) != 0;
            std::size_t field_cursor = 1;
            if (scramble_system_present) {
                if (field_cursor >= size) return false;
                ++field_cursor;
            }
            if (authentication_present) {
                if (size - field_cursor < 2) return false;
                result.authenticated_payload_size = read_be16(extension + cursor + field_cursor);
            }
        } else if (type == 0x0002 && size == 4) {
            result.download_id = read_be32(extension + cursor);
        } else if (type == 0x0003 && size == 8) {
            result.item_fragment_number = read_be32(extension + cursor);
            result.last_item_fragment_number = read_be32(extension + cursor + 4);
        }

        cursor += size;
        if (end) return cursor == extension_size;
    }
    return true;
}

} // namespace

MmtpParser::MmtpParser(const std::uint32_t context_id, const Limits& limits,
                       PackageCallback on_package, TrackCallback on_track,
                       AccessUnitCallback on_access_unit,
                       ApplicationServiceCallback on_application_service,
                       LayoutCallback on_layout,
                       DataAssetCallback on_data_asset,
                       DataUnitCallback on_data_unit,
                       SignallingCallback on_signalling,
                       EventCallback on_event,
                       StreamEventCallback on_stream_event,
                       ViewerParticipationCallback on_viewer_participation,
                       ApplicationCallback on_application,
                       DataTransmissionCallback on_data_transmission,
                       DataDirectoryCallback on_data_directory,
                       DataAssetManagementCallback on_data_asset_management,
                       StateAcquireCallback acquire_state,
                       StateReleaseCallback release_state, ErrorCallback on_error)
    : context_id_(context_id), limits_(limits), on_package_(std::move(on_package)),
      on_track_(std::move(on_track)), on_access_unit_(std::move(on_access_unit)),
      on_application_service_(std::move(on_application_service)),
      on_layout_(std::move(on_layout)),
      on_data_asset_(std::move(on_data_asset)), on_data_unit_(std::move(on_data_unit)),
      on_signalling_(std::move(on_signalling)),
      on_event_(std::move(on_event)),
      on_stream_event_(std::move(on_stream_event)),
      on_viewer_participation_(std::move(on_viewer_participation)),
      on_application_(std::move(on_application)),
      on_data_transmission_(std::move(on_data_transmission)),
      on_data_directory_(std::move(on_data_directory)),
      on_data_asset_management_(std::move(on_data_asset_management)),
      acquire_state_(std::move(acquire_state)), release_state_(std::move(release_state)),
      on_error_(std::move(on_error)) {}

MmtpParser::~MmtpParser() {
    release_all_states();
}

void MmtpParser::release_all_states() {
    const auto count = signalling_.size() + tracks_.size() + data_assets_.size();
    for (std::size_t index = 0; index < count; ++index) release_state_();
}

void MmtpParser::reset() {
    release_all_states();
    signalling_.clear();
    tracks_.clear();
    data_assets_.clear();
    event_message_tags_.clear();
    latest_full_ntp_.reset();
}

void MmtpParser::flush() {
    for (auto& entry : signalling_) {
        auto& assembler = entry.second;
        if (assembler.state == FragmentState::Collecting && !assembler.data.empty()) {
            on_error_(ErrorCode::MalformedInput, 0, true,
                      "dropped incomplete MMTP signalling fragment at end of input");
        }
    }
    for (auto& entry : tracks_) {
        auto& track = entry.second;
        if (track.media.state == FragmentState::Collecting && !track.media.data.empty()) {
            on_error_(ErrorCode::MalformedInput, track.media.input_offset, true,
                      "dropped incomplete MMTP media fragment at end of input");
        }
        finalize_hevc(track);
        if (track.subtitle.active) {
            track.discontinuity = true;
            on_error_(ErrorCode::MalformedInput, track.subtitle.input_offset, true,
                      "dropped incomplete TTML subsample group at end of input");
        }
        track.media = {};
        track.subtitle = {};
        track.current_mpu_sequence.reset();
        track.au_index = 0;
        track.dts_offset_accumulator = 0;
        track.discontinuity = true;
        if (track.info.kind == TrackKind::Video) track.wait_for_rap = true;
    }
    for (auto& entry : data_assets_) {
        auto& asset = entry.second;
        if (asset.media.state == FragmentState::Collecting && !asset.media.data.empty()) {
            on_error_(ErrorCode::MalformedInput, asset.media.input_offset, true,
                      "dropped incomplete non-timed MFU fragment at end of input");
        }
        asset.media = {};
        asset.discontinuity = true;
    }
    for (std::size_t index = 0; index < signalling_.size(); ++index) release_state_();
    signalling_.clear();
}

void MmtpParser::push(const std::uint8_t* data, const std::size_t size,
                      const std::uint64_t input_offset) {
    if (size < 12) {
        on_error_(ErrorCode::MalformedInput, input_offset, true,
                  "MMTP packet is shorter than its fixed header");
        return;
    }

    const auto first = data[0];
    const auto version = static_cast<std::uint8_t>(first >> 6U);
    if (version != 0) {
        on_error_(ErrorCode::UnsupportedFeature, input_offset, true,
                  "unsupported MMTP version");
        return;
    }
    const bool packet_counter_flag = ((first >> 5U) & 1U) != 0;
    const bool extension_header_flag = ((first >> 1U) & 1U) != 0;
    const bool random_access = (first & 1U) != 0;
    const auto payload_type = static_cast<std::uint8_t>(data[1] & 0x3fU);
    const auto packet_id = read_be16(data + 2);
    const auto delivery_timestamp = read_be32(data + 4);
    const auto sequence = read_be32(data + 8);

    std::size_t cursor = 12;
    if (packet_counter_flag) {
        if (size - cursor < 4) {
            on_error_(ErrorCode::MalformedInput, input_offset, true,
                      "truncated MMTP packet counter");
            return;
        }
        cursor += 4;
    }
    PacketExtensions extensions;
    if (extension_header_flag) {
        if (size - cursor < 4) {
            on_error_(ErrorCode::MalformedInput, input_offset, true,
                      "truncated MMTP extension header");
            return;
        }
        const auto extension_type = read_be16(data + cursor);
        const auto extension_size = static_cast<std::size_t>(read_be16(data + cursor + 2));
        cursor += 4;
        if (extension_size > size - cursor) {
            on_error_(ErrorCode::MalformedInput, input_offset, true,
                      "MMTP extension length exceeds packet bounds");
            return;
        }
        if (!parse_packet_extensions(extension_type, data + cursor, extension_size,
                                     extensions)) {
            on_error_(ErrorCode::MalformedInput, input_offset, true,
                      "malformed MMTP multi-type extension header");
            return;
        }
        cursor += extension_size;
    }

    const auto* payload = data + cursor;
    auto payload_size = size - cursor;
    if (extensions.authenticated_payload_size.has_value()) {
        if (*extensions.authenticated_payload_size > payload_size) {
            on_error_(ErrorCode::MalformedInput, input_offset, true,
                      "authenticated MMTP payload length exceeds packet bounds");
            return;
        }
        payload_size = *extensions.authenticated_payload_size;
    }
    if (payload_type == 0x02) {
        parse_signalling(packet_id, sequence, payload, payload_size, input_offset);
    } else if (payload_type == 0x00) {
        parse_mpu(packet_id, sequence, delivery_timestamp, random_access,
                  payload, payload_size, input_offset, extensions);
    } else {
        on_error_(ErrorCode::UnsupportedFeature, input_offset, true,
                  "unsupported MMTP payload type in context " + std::to_string(context_id_));
    }
}

bool MmtpParser::append(SignallingAssembler& assembler, const std::uint8_t* data,
                        const std::size_t size, const std::uint64_t input_offset) {
    if (size > limits_.max_signalling_message - assembler.data.size()) {
        assembler.data.clear();
        assembler.input_offset = 0;
        assembler.state = FragmentState::Skipping;
        on_error_(ErrorCode::ResourceLimit, input_offset, true,
                  "MMTP signalling message exceeds configured limit");
        return false;
    }
    assembler.data.insert(assembler.data.end(), data, data + size);
    return true;
}

void MmtpParser::accept_signalling_unit(const std::uint16_t packet_id,
                                        const std::uint8_t* data, const std::size_t size,
                                        const std::uint64_t input_offset) {
    if (size < 2) {
        on_error_(ErrorCode::MalformedInput, input_offset, true,
                  "signalling message is too short for a message ID");
        return;
    }
    const auto message_id = read_be16(data);
    bool valid = true;
    if (message_id == 0x0000) {
        valid = parse_pa_message(packet_id, data, size, input_offset);
    } else if (message_id == 0x8000) {
        valid = parse_m2_message(packet_id, data, size, input_offset);
    } else if (message_id == 0x8003) {
        valid = parse_data_transmission_message(packet_id, data, size, input_offset);
    }
    if (!valid) {
        on_error_(ErrorCode::MalformedInput, input_offset, true,
                  "malformed MMTP signalling message or nested table");
        return;
    }
    SignallingMessage message;
    message.context_id = context_id_;
    message.packet_id = packet_id;
    message.message_id = message_id;
    message.data.assign(data, data + size);
    message.input_offset = input_offset;
    on_signalling_(std::move(message));
}

namespace {

bool skip_general_location(ByteReader& reader, std::optional<std::uint16_t>& packet_id) {
    std::uint8_t type = 0;
    if (!reader.read_u8(type)) return false;
    switch (type) {
    case 0x00: {
        std::uint16_t value = 0;
        if (!reader.read_u16(value)) return false;
        if (!packet_id.has_value()) packet_id = value;
        return true;
    }
    case 0x01: return reader.skip(12);
    case 0x02: return reader.skip(36);
    case 0x03: return reader.skip(6);
    case 0x04: return reader.skip(38);
    case 0x05: {
        std::uint8_t length = 0;
        return reader.read_u8(length) && reader.skip(length);
    }
    default:
        return false;
    }
}

bool descriptor_length(ByteReader& reader, const std::uint16_t tag, std::uint32_t& length) {
    if (tag <= 0x3fff || (tag >= 0x8000 && tag <= 0xefff)) {
        std::uint8_t value = 0;
        if (!reader.read_u8(value)) return false;
        length = value;
        return true;
    }
    if (tag <= 0x6fff || tag >= 0xf000) {
        std::uint16_t value = 0;
        if (!reader.read_u16(value)) return false;
        length = value;
        return true;
    }
    return reader.read_u32(length);
}

AudioChannelLayout audio_channel_layout(const std::uint8_t component_type) {
    switch (component_type & 0x1fU) {
    case 0x01: return AudioChannelLayout::Mono;
    case 0x02: return AudioChannelLayout::DualMono;
    case 0x03: return AudioChannelLayout::Stereo;
    case 0x04: return AudioChannelLayout::Channels2_1;
    case 0x05: return AudioChannelLayout::Channels3_0;
    case 0x06: return AudioChannelLayout::Channels2_2;
    case 0x07: return AudioChannelLayout::Channels4_0;
    case 0x08: return AudioChannelLayout::Channels5_0;
    case 0x09: return AudioChannelLayout::Channels5_1;
    case 0x0a: return AudioChannelLayout::Channels3_3_1;
    case 0x0b: return AudioChannelLayout::Channels6_1;
    case 0x0c:
    case 0x0d:
    case 0x0e:
    case 0x0f: return AudioChannelLayout::Channels7_1;
    case 0x10: return AudioChannelLayout::Channels10_2;
    case 0x11: return AudioChannelLayout::Channels22_2;
    default: return AudioChannelLayout::Unknown;
    }
}

std::uint32_t audio_sample_rate(const std::uint8_t code) {
    switch (code) {
    case 0x01: return 16000;
    case 0x02: return 22050;
    case 0x03: return 24000;
    case 0x05: return 32000;
    case 0x06: return 44100;
    case 0x07: return 48000;
    default: return 0;
    }
}

bool parse_descriptors(ByteReader& reader, AssetMetadata& metadata) {
    while (reader.remaining() != 0) {
        std::uint16_t tag = 0;
        std::uint32_t length = 0;
        if (!reader.read_u16(tag) || !descriptor_length(reader, tag, length) ||
            length > reader.remaining()) {
            return false;
        }
        const std::uint8_t* payload = nullptr;
        if (!reader.read_view(length, payload)) return false;

        if (tag == 0x0001) {
            if (length % 12 != 0) return false;
            ByteReader values(payload, length);
            while (values.remaining() != 0) {
                std::uint32_t sequence = 0;
                const std::uint8_t* ntp = nullptr;
                if (!values.read_u32(sequence) || !values.read_view(8, ntp)) return false;
                metadata.timestamps[sequence] = TimestampMapping{read_be64(ntp)};
            }
        } else if (tag == 0x8003) {
            ByteReader values(payload, length);
            while (values.remaining() != 0) {
                MpuPresentationRegion region;
                std::uint8_t reserved_length = 0;
                if (!values.read_u32(region.mpu_sequence_number) ||
                    !values.read_u8(region.layout_number) ||
                    !values.read_u8(region.region_number) ||
                    !values.read_u8(reserved_length) ||
                    !values.skip(reserved_length)) {
                    return false;
                }
                metadata.presentation_regions.push_back(region);
            }
        } else if (tag == 0x8011 && length >= 2) {
            metadata.component_tag = read_be16(payload);
        } else if (tag == 0x8010 && length >= 8) {
            if (metadata.component_tag == 0) metadata.component_tag = read_be16(payload + 2);
            metadata.language.assign(reinterpret_cast<const char*>(payload + 5), 3);
        } else if (tag == 0x8014 && length >= 10) {
            const auto stream_content = static_cast<std::uint8_t>(payload[0] & 0x0fU);
            const auto stream_type = payload[4];
            const auto flags = payload[6];
            if (metadata.component_tag == 0) {
                metadata.component_tag = read_be16(payload + 2);
            }
            metadata.language.assign(reinterpret_cast<const char*>(payload + 7), 3);
            AudioInfo audio;
            audio.stream_content = stream_content;
            audio.component_type = payload[1];
            audio.component_tag = read_be16(payload + 2);
            audio.channel_layout = audio_channel_layout(audio.component_type);
            audio.stream_type = stream_type;
            audio.simulcast_group_tag = payload[5];
            audio.es_multi_lingual = (flags & 0x80U) != 0;
            audio.main_component = (flags & 0x40U) != 0;
            audio.quality_indicator = static_cast<std::uint8_t>((flags >> 4U) & 0x03U);
            audio.sampling_rate_code = static_cast<std::uint8_t>((flags >> 1U) & 0x07U);
            audio.sample_rate = audio_sample_rate(audio.sampling_rate_code);
            if (audio.es_multi_lingual) {
                if (length < 13) return false;
                audio.secondary_language.assign(reinterpret_cast<const char*>(payload + 10), 3);
            }
            metadata.audio = std::move(audio);
            metadata.aac_latm = stream_content == 0x03 && stream_type == 0x11;
        } else if (tag == 0x8020 && length >= 10 && read_be16(payload) == 0x0020) {
            const auto* additional = payload + 2;
            const auto additional_size = length - 2;
            if (additional_size < 8) return false;
            metadata.language.assign(reinterpret_cast<const char*>(additional + 2), 3);
            SubtitleInfo subtitle;
            subtitle.tag = additional[0];
            subtitle.info_version = static_cast<std::uint8_t>((additional[1] >> 4U) & 0x0fU);
            const bool has_start_mpu_sequence_number = (additional[1] & 0x08U) != 0;
            subtitle.type = static_cast<std::uint8_t>((additional[5] >> 6U) & 0x03U);
            subtitle.format = static_cast<std::uint8_t>((additional[5] >> 2U) & 0x0fU);
            subtitle.operation_mode = static_cast<std::uint8_t>(additional[5] & 0x03U);
            subtitle.timing_mode = static_cast<std::uint8_t>((additional[6] >> 4U) & 0x0fU);
            subtitle.display_mode = static_cast<std::uint8_t>(additional[6] & 0x0fU);
            subtitle.resolution = static_cast<std::uint8_t>((additional[7] >> 4U) & 0x0fU);
            subtitle.compression_type = static_cast<std::uint8_t>(additional[7] & 0x0fU);
            std::size_t offset = 8;
            if (has_start_mpu_sequence_number) {
                if (additional_size < offset + 4) return false;
                subtitle.start_mpu_sequence_number = read_be32(additional + offset);
                offset += 4;
            }
            if (subtitle.timing_mode == 0x02) {
                if (additional_size < offset + 8) return false;
                subtitle.reference_start_ntp = read_be64(additional + offset);
            }
            metadata.ttml = subtitle.format == 0;
            metadata.subtitle = subtitle;
        } else if (tag == 0x8026 && length >= 1) {
            ByteReader values(payload, length);
            std::uint8_t flags = 0;
            if (!values.read_u8(flags)) return false;
            const auto pts_offset_type = static_cast<std::uint8_t>((flags >> 1U) & 0x03U);
            const bool timescale_present = (flags & 1U) != 0;
            if (timescale_present) {
                std::uint32_t timescale = 0;
                if (!values.read_u32(timescale) || timescale == 0) return false;
                metadata.timescale = timescale;
            }
            std::uint16_t default_pts_offset = 0;
            if (pts_offset_type == 1 && !values.read_u16(default_pts_offset)) return false;
            if (pts_offset_type == 0) continue;
            while (values.remaining() != 0) {
                std::uint32_t sequence = 0;
                std::uint8_t leap_and_reserved = 0;
                std::uint16_t decoding_offset = 0;
                std::uint8_t au_count = 0;
                if (!values.read_u32(sequence) || !values.read_u8(leap_and_reserved) ||
                    !values.read_u16(decoding_offset) || !values.read_u8(au_count)) {
                    return false;
                }
                (void)leap_and_reserved;
                ExtendedTimestampMapping timing;
                timing.decoding_time_offset = decoding_offset;
                timing.dts_pts_offsets.reserve(au_count);
                timing.pts_offsets.reserve(au_count);
                for (std::uint16_t index = 0; index < au_count; ++index) {
                    std::uint16_t dts_pts = 0;
                    std::uint16_t pts = default_pts_offset;
                    if (!values.read_u16(dts_pts)) return false;
                    if (pts_offset_type == 2 && !values.read_u16(pts)) return false;
                    timing.dts_pts_offsets.push_back(dts_pts);
                    timing.pts_offsets.push_back(pts);
                }
                metadata.extended_timestamps[sequence] = std::move(timing);
            }
        }
    }
    return true;
}

bool parse_application_service_descriptor(const std::uint32_t context_id,
                                          const std::uint8_t* payload,
                                          const std::size_t length,
                                          ApplicationServiceInfo& info) {
    if (length < 3) return false;
    info.context_id = context_id;
    info.application_format = static_cast<std::uint8_t>(payload[0] >> 4U);
    info.document_resolution = static_cast<std::uint8_t>(payload[1] >> 4U);
    info.default_ait = (payload[2] & 0x80U) != 0;
    info.has_data_transmission_messages = (payload[2] & 0x40U) != 0;
    const auto emt_count = static_cast<std::uint8_t>(payload[2] & 0x0fU);
    ByteReader locations(payload + 3, length - 3);
    if (!skip_general_location(locations, info.ait_packet_id)) return false;
    if (info.has_data_transmission_messages &&
        !skip_general_location(locations, info.data_transmission_packet_id)) {
        return false;
    }
    info.event_message_locations.reserve(emt_count);
    for (std::uint16_t index = 0; index < emt_count; ++index) {
        ApplicationServiceInfo::EventMessageLocation location;
        if (!locations.read_u8(location.event_message_tag) ||
            !skip_general_location(locations, location.packet_id)) {
            return false;
        }
        info.event_message_locations.push_back(std::move(location));
    }
    return true;
}

bool parse_program_descriptors(ByteReader& reader, const std::uint32_t context_id,
                               const MmtpParser::ApplicationServiceCallback& callback) {
    while (reader.remaining() != 0) {
        std::uint16_t tag = 0;
        std::uint32_t length = 0;
        if (!reader.read_u16(tag) || !descriptor_length(reader, tag, length) ||
            length > reader.remaining()) {
            return false;
        }
        const std::uint8_t* payload = nullptr;
        if (!reader.read_view(length, payload)) return false;
        if (tag == 0x8034) {
            ApplicationServiceInfo info;
            if (!parse_application_service_descriptor(context_id, payload, length, info)) {
                return false;
            }
            callback(std::move(info));
        }
    }
    return true;
}

bool parse_application_descriptors(ByteReader& reader, ApplicationInfo& application) {
    while (reader.remaining() != 0) {
        std::uint16_t tag = 0;
        std::uint32_t length = 0;
        if (!reader.read_u16(tag) || !descriptor_length(reader, tag, length) ||
            length > reader.remaining()) {
            return false;
        }
        const std::uint8_t* payload = nullptr;
        if (!reader.read_view(length, payload)) return false;
        if (tag == 0x8029) {
            ByteReader descriptor(payload, length);
            std::uint8_t profiles_length = 0;
            if (!descriptor.read_u8(profiles_length) || profiles_length % 5 != 0 ||
                profiles_length > descriptor.remaining()) {
                return false;
            }
            ByteReader profiles(payload + 1, profiles_length);
            while (profiles.remaining() != 0) {
                ApplicationInfo::Profile profile;
                if (!profiles.read_u16(profile.application_profile) ||
                    !profiles.read_u8(profile.version_major) ||
                    !profiles.read_u8(profile.version_minor) ||
                    !profiles.read_u8(profile.version_micro)) {
                    return false;
                }
                application.profiles.push_back(profile);
            }
            if (!descriptor.skip(profiles_length)) return false;
            std::uint8_t flags = 0;
            if (!descriptor.read_u8(flags) ||
                !descriptor.read_u8(application.application_priority)) {
                return false;
            }
            application.application_descriptor_present = true;
            application.service_bound = (flags & 0x80U) != 0;
            application.visibility = static_cast<std::uint8_t>((flags >> 5U) & 0x03U);
            application.present_application_priority = (flags & 0x01U) != 0;
            while (descriptor.remaining() != 0) {
                std::uint8_t label = 0;
                if (!descriptor.read_u8(label)) return false;
                application.transport_protocol_labels.push_back(label);
            }
        } else if (tag == 0x802b) {
            application.entry_path.assign(reinterpret_cast<const char*>(payload), length);
        } else if (tag == 0x802a && length >= 3) {
            const auto protocol_id = read_be16(payload);
            if (protocol_id != 0x0003 && protocol_id != 0x0005) continue;
            ApplicationInfo::Transport transport;
            transport.protocol_id = protocol_id;
            transport.label = payload[2];
            ByteReader selector(payload + 3, length - 3);
            while (selector.remaining() != 0) {
                std::uint8_t base_length = 0;
                std::vector<std::uint8_t> base;
                std::uint8_t extension_count = 0;
                if (!selector.read_u8(base_length) ||
                    !selector.read_bytes(base_length, base) ||
                    !selector.read_u8(extension_count)) {
                    return false;
                }
                const std::string base_url(reinterpret_cast<const char*>(base.data()), base.size());
                if (extension_count == 0) transport.urls.push_back(base_url);
                for (std::uint16_t index = 0; index < extension_count; ++index) {
                    std::uint8_t extension_length = 0;
                    std::vector<std::uint8_t> extension;
                    if (!selector.read_u8(extension_length) ||
                        !selector.read_bytes(extension_length, extension)) {
                        return false;
                    }
                    transport.urls.push_back(
                        base_url + std::string(reinterpret_cast<const char*>(extension.data()),
                                               extension.size()));
                }
            }
            application.transports.erase(
                std::remove_if(application.transports.begin(), application.transports.end(),
                               [label = transport.label](const auto& existing) {
                                   return existing.label == label;
                               }),
                application.transports.end());
            application.transports.push_back(std::move(transport));
        }
    }
    application.transport_urls.clear();
    for (const auto& transport : application.transports) {
        if (!application.transport_protocol_labels.empty() &&
            std::find(application.transport_protocol_labels.begin(),
                      application.transport_protocol_labels.end(), transport.label) ==
                application.transport_protocol_labels.end()) {
            continue;
        }
        application.transport_urls.insert(application.transport_urls.end(),
                                          transport.urls.begin(), transport.urls.end());
    }
    return true;
}

std::optional<std::uint8_t> decode_bcd(const std::uint8_t value) {
    const auto high = static_cast<std::uint8_t>(value >> 4U);
    const auto low = static_cast<std::uint8_t>(value & 0x0fU);
    if (high > 9 || low > 9) return std::nullopt;
    return static_cast<std::uint8_t>(high * 10 + low);
}

std::optional<std::int64_t> parse_mjd_time(const std::uint8_t* data) {
    if (std::all_of(data, data + 5, [](const std::uint8_t value) { return value == 0xff; })) {
        return std::nullopt;
    }
    const auto hour = decode_bcd(data[2]);
    const auto minute = decode_bcd(data[3]);
    const auto second = decode_bcd(data[4]);
    if (!hour.has_value() || !minute.has_value() || !second.has_value() ||
        *hour > 23 || *minute > 59 || *second > 59) {
        return std::nullopt;
    }
    // MH-EIT expresses the MJD calendar fields in JST. MJD 40587 is
    // 1970-01-01, so subtract nine hours to obtain a Unix UTC timestamp.
    const auto days = static_cast<std::int64_t>(read_be16(data)) - 40587;
    const auto local_seconds = days * 86400 + static_cast<std::int64_t>(*hour) * 3600 +
        static_cast<std::int64_t>(*minute) * 60 + *second;
    return (local_seconds - 9 * 3600) * 1000;
}

std::optional<std::uint32_t> parse_bcd_duration(const std::uint8_t* data) {
    if (data[0] == 0xff && data[1] == 0xff && data[2] == 0xff) return std::nullopt;
    const auto hour = decode_bcd(data[0]);
    const auto minute = decode_bcd(data[1]);
    const auto second = decode_bcd(data[2]);
    if (!hour.has_value() || !minute.has_value() || !second.has_value() ||
        *minute > 59 || *second > 59) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*hour) * 3600U +
        static_cast<std::uint32_t>(*minute) * 60U + *second;
}

bool parse_short_event_descriptor(const std::uint8_t* payload, const std::size_t length,
                                  EventInfo& event) {
    if (length < 6) return false;
    event.language.assign(reinterpret_cast<const char*>(payload), 3);
    std::size_t offset = 3;
    const auto title_length = static_cast<std::size_t>(payload[offset++]);
    if (title_length > length - offset) return false;
    event.title.assign(reinterpret_cast<const char*>(payload + offset), title_length);
    offset += title_length;
    if (length - offset < 2) return false;
    const auto text_length = static_cast<std::size_t>(read_be16(payload + offset));
    offset += 2;
    if (text_length > length - offset) return false;
    event.description.assign(reinterpret_cast<const char*>(payload + offset), text_length);
    return true;
}

} // namespace

bool MmtpParser::parse_pa_message(const std::uint16_t packet_id,
                                  const std::uint8_t* data, const std::size_t size,
                                  const std::uint64_t input_offset) {
    if (size < 7 || read_be16(data) != 0x0000) return false;
    const auto length = static_cast<std::size_t>(read_be32(data + 3));
    if (length > size - 7) return false;
    ByteReader body(data + 7, length);
    std::uint8_t table_count = 0;
    if (!body.read_u8(table_count) || !body.skip(static_cast<std::size_t>(table_count) * 4)) {
        return false;
    }
    return parse_tables(body.current(), body.remaining(), packet_id, input_offset);
}

bool MmtpParser::parse_m2_message(const std::uint16_t packet_id,
                                  const std::uint8_t* data, const std::size_t size,
                                  const std::uint64_t input_offset) {
    if (size < 5 || read_be16(data) != 0x8000) return false;
    const auto length = static_cast<std::size_t>(read_be16(data + 3));
    if (length > size - 5) return false;
    return parse_tables(data + 5, length, packet_id, input_offset);
}

bool MmtpParser::parse_data_transmission_message(const std::uint16_t packet_id,
                                                 const std::uint8_t* data,
                                                 const std::size_t size,
                                                 const std::uint64_t input_offset) {
    if (size < 7 || read_be16(data) != 0x8003) return false;
    const auto length = static_cast<std::size_t>(read_be32(data + 3));
    if (length != size - 7 || length < 12) return false;
    const auto* table = data + 7;
    const auto section_size = 3 + static_cast<std::size_t>(read_be16(table + 1) & 0x0fffU);
    if (section_size != length || section_size < 12) return false;

    DataTransmissionTable result;
    result.context_id = context_id_;
    result.source_packet_id = packet_id;
    result.table_id = table[0];
    result.session_id = table[3];
    result.version = static_cast<std::uint8_t>((table[5] >> 1U) & 0x1fU);
    result.section_number = table[6];
    result.last_section_number = table[7];
    result.data.assign(table, table + section_size);
    result.input_offset = input_offset;
    if (result.table_id == 0xa3 && !parse_data_directory_table(result)) return false;
    if (result.table_id == 0xa4 && !parse_data_asset_management_table(result)) return false;
    on_data_transmission_(std::move(result));
    return true;
}

bool MmtpParser::parse_data_directory_table(const DataTransmissionTable& table) {
    if (table.data.size() < 12) return false;
    ByteReader body(table.data.data() + 8, table.data.size() - 12);
    DataDirectoryTable result;
    result.context_id = table.context_id;
    result.session_id = table.session_id;
    result.version = table.version;
    result.section_number = table.section_number;
    result.last_section_number = table.last_section_number;

    std::uint8_t base_path_length = 0;
    std::vector<std::uint8_t> base_path;
    std::uint8_t directory_count = 0;
    if (!body.read_u8(base_path_length) || !body.read_bytes(base_path_length, base_path) ||
        !body.read_u8(directory_count)) {
        return false;
    }
    result.base_path.assign(reinterpret_cast<const char*>(base_path.data()), base_path.size());
    result.directories.reserve(directory_count);
    for (std::uint16_t directory_index = 0; directory_index < directory_count;
         ++directory_index) {
        DataDirectoryNode directory;
        std::uint8_t path_length = 0;
        std::vector<std::uint8_t> path;
        std::uint16_t file_count = 0;
        if (!body.read_u16(directory.node_tag) || !body.read_u8(directory.version) ||
            !body.read_u8(path_length) || !body.read_bytes(path_length, path) ||
            !body.read_u16(file_count)) {
            return false;
        }
        directory.path.assign(reinterpret_cast<const char*>(path.data()), path.size());
        directory.files.reserve(file_count);
        for (std::uint32_t file_index = 0; file_index < file_count; ++file_index) {
            DataDirectoryFile file;
            std::uint8_t name_length = 0;
            std::vector<std::uint8_t> name;
            if (!body.read_u16(file.node_tag) || !body.read_u8(name_length) ||
                !body.read_bytes(name_length, name)) {
                return false;
            }
            file.name.assign(reinterpret_cast<const char*>(name.data()), name.size());
            directory.files.push_back(std::move(file));
        }
        result.directories.push_back(std::move(directory));
    }
    if (body.remaining() != 0) return false;
    on_data_directory_(std::move(result));
    return true;
}

bool MmtpParser::parse_data_asset_management_table(const DataTransmissionTable& table) {
    if (table.data.size() < 23) return false;
    ByteReader body(table.data.data() + 8, table.data.size() - 12);
    DataAssetManagementTable result;
    result.context_id = table.context_id;
    result.session_id = table.session_id;
    result.version = table.version;
    result.section_number = table.section_number;
    result.last_section_number = table.last_section_number;
    std::uint8_t mpu_count = 0;
    if (!body.read_u32(result.transaction_id) || !body.read_u16(result.component_tag) ||
        !body.read_u32(result.download_id) || !body.read_u8(mpu_count)) {
        return false;
    }
    result.mpus.reserve(mpu_count);
    for (std::uint16_t mpu_index = 0; mpu_index < mpu_count; ++mpu_index) {
        DataAssetMpu mpu;
        std::uint8_t flags = 0;
        if (!body.read_u32(mpu.sequence_number) || !body.read_u32(mpu.size) ||
            !body.read_u8(flags)) {
            return false;
        }
        mpu.index_item = (flags & 0x80U) != 0;
        const bool index_item_id_present = (flags & 0x40U) != 0;
        mpu.index_item_compression_type = static_cast<std::uint8_t>((flags >> 4U) & 0x03U);
        if (mpu.index_item && index_item_id_present) {
            std::uint32_t item_id = 0;
            if (!body.read_u32(item_id)) return false;
            mpu.index_item_id = item_id;
        }
        std::uint16_t item_count = 0;
        if (!body.read_u16(item_count)) return false;
        mpu.items.reserve(item_count);
        for (std::uint32_t item_index = 0; item_index < item_count; ++item_index) {
            DataAssetItem item;
            if (!body.read_u16(item.node_tag)) return false;
            if (!mpu.index_item) {
                std::uint32_t item_id = 0;
                std::uint32_t item_size = 0;
                std::uint8_t item_version = 0;
                std::uint8_t item_flags = 0;
                if (!body.read_u32(item_id) || !body.read_u32(item_size) ||
                    !body.read_u8(item_version) || !body.read_u8(item_flags)) {
                    return false;
                }
                item.item_id = item_id;
                item.size = item_size;
                item.version = item_version;
                if ((item_flags & 0x80U) != 0) {
                    std::uint32_t checksum = 0;
                    if (!body.read_u32(checksum)) return false;
                    item.checksum = checksum;
                }
                std::uint8_t info_length = 0;
                if (!body.read_u8(info_length) || !body.read_bytes(info_length, item.info)) {
                    return false;
                }
            }
            mpu.items.push_back(std::move(item));
        }
        std::uint8_t mpu_info_length = 0;
        if (!body.read_u8(mpu_info_length) || !body.read_bytes(mpu_info_length, mpu.info)) {
            return false;
        }
        result.mpus.push_back(std::move(mpu));
    }
    std::uint8_t component_info_length = 0;
    if (!body.read_u8(component_info_length) ||
        !body.read_bytes(component_info_length, result.component_info) ||
        body.remaining() != 0) {
        return false;
    }
    on_data_asset_management_(std::move(result));
    return true;
}

bool MmtpParser::parse_mh_ait(const std::uint8_t* data, const std::size_t size,
                              const std::uint16_t packet_id,
                              const std::uint64_t input_offset) {
    if (size < 12 || data[0] != 0x9c) return false;
    const auto declared_size = 3 + static_cast<std::size_t>(read_be16(data + 1) & 0x0fffU);
    if (declared_size != size) return false;
    ByteReader body(data + 3, size - 3);
    std::uint16_t application_type = 0;
    std::uint8_t version_flags = 0;
    std::uint8_t section_number = 0;
    std::uint8_t last_section_number = 0;
    std::uint16_t common_length_field = 0;
    if (!body.read_u16(application_type) || !body.read_u8(version_flags) ||
        !body.read_u8(section_number) || !body.read_u8(last_section_number) ||
        !body.read_u16(common_length_field)) {
        return false;
    }
    const auto common_length = static_cast<std::size_t>(common_length_field & 0x0fffU);
    const std::uint8_t* common_descriptors = nullptr;
    if (!body.read_view(common_length, common_descriptors)) return false;
    ApplicationInfo common;
    ByteReader common_reader(common_descriptors, common_length);
    if (!parse_application_descriptors(common_reader, common)) return false;
    std::uint16_t loop_length_field = 0;
    if (!body.read_u16(loop_length_field)) return false;
    const auto loop_length = static_cast<std::size_t>(loop_length_field & 0x0fffU);
    const std::uint8_t* loop_data = nullptr;
    if (!body.read_view(loop_length, loop_data) || body.remaining() != 4) return false;

    ByteReader applications(loop_data, loop_length);
    while (applications.remaining() != 0) {
        if (applications.remaining() < 9) return false;
        ApplicationInfo application = common;
        application.context_id = context_id_;
        application.source_packet_id = packet_id;
        application.application_type = application_type;
        application.version = static_cast<std::uint8_t>((version_flags >> 1U) & 0x1fU);
        application.current_next = (version_flags & 0x01U) != 0;
        application.section_number = section_number;
        application.last_section_number = last_section_number;
        application.input_offset = input_offset;
        std::uint16_t descriptor_length_field = 0;
        if (!applications.read_u16(application.organization_id) ||
            !applications.read_u32(application.application_id) ||
            !applications.read_u8(application.control_code) ||
            !applications.read_u16(descriptor_length_field)) {
            return false;
        }
        const auto descriptor_length =
            static_cast<std::size_t>(descriptor_length_field & 0x0fffU);
        const std::uint8_t* descriptors = nullptr;
        if (!applications.read_view(descriptor_length, descriptors)) return false;
        ByteReader descriptor_reader(descriptors, descriptor_length);
        if (!parse_application_descriptors(descriptor_reader, application)) return false;
        if (application.current_next) on_application_(std::move(application));
    }
    return true;
}

bool MmtpParser::parse_mh_eit(const std::uint8_t* data, const std::size_t size,
                              const std::uint16_t packet_id,
                              const std::uint64_t input_offset) {
    if (size < 18 || data[0] < 0x8b || data[0] > 0x9b) return false;
    const auto section_length = static_cast<std::size_t>(read_be16(data + 1) & 0x0fffU);
    if (section_length + 3 != size || section_length < 15) return false;

    const auto section_end = size - 4;
    std::size_t offset = 14;
    while (offset < section_end) {
        if (section_end - offset < 12) return false;
        EventInfo event;
        event.context_id = context_id_;
        event.source_packet_id = packet_id;
        event.table_id = data[0];
        event.version = static_cast<std::uint8_t>((data[5] >> 1U) & 0x1fU);
        event.current_next = (data[5] & 0x01U) != 0;
        event.section_number = data[6];
        event.last_section_number = data[7];
        event.service_id = read_be16(data + 3);
        event.tlv_stream_id = read_be16(data + 8);
        event.original_network_id = read_be16(data + 10);
        event.event_id = read_be16(data + offset);
        event.start_time_unix_milliseconds = parse_mjd_time(data + offset + 2);
        event.duration_seconds = parse_bcd_duration(data + offset + 7);
        event.running_status = static_cast<std::uint8_t>(data[offset + 10] >> 5U);
        event.free_ca_mode = (data[offset + 10] & 0x10U) != 0;
        event.input_offset = input_offset;
        const auto descriptors_length =
            static_cast<std::size_t>(read_be16(data + offset + 10) & 0x0fffU);
        offset += 12;
        if (descriptors_length > section_end - offset) return false;

        ByteReader descriptors(data + offset, descriptors_length);
        while (descriptors.remaining() != 0) {
            std::uint16_t tag = 0;
            std::uint32_t length = 0;
            if (!descriptors.read_u16(tag) || !descriptor_length(descriptors, tag, length) ||
                length > descriptors.remaining()) {
                return false;
            }
            const std::uint8_t* payload = nullptr;
            if (!descriptors.read_view(length, payload)) return false;
            if (tag == 0xf001 && !parse_short_event_descriptor(payload, length, event)) {
                return false;
            }
        }
        offset += descriptors_length;
        on_event_(std::move(event));
    }
    return offset == section_end;
}

bool MmtpParser::parse_mpt(const std::uint8_t* data, const std::size_t size,
                           const std::uint64_t input_offset) {
    (void)input_offset;
    if (size < 4 || data[0] != 0x20) return false;
    const auto declared_size = static_cast<std::size_t>(read_be16(data + 2));
    if (declared_size != size - 4) return false;
    ByteReader body(data + 4, declared_size);

    std::uint8_t mode = 0;
    std::uint8_t package_length = 0;
    std::vector<std::uint8_t> package_id;
    std::uint16_t program_descriptors_length = 0;
    std::uint8_t asset_count = 0;
    if (!body.read_u8(mode) || !body.read_u8(package_length) ||
        !body.read_bytes(package_length, package_id) ||
        !body.read_u16(program_descriptors_length)) {
        return false;
    }
    const std::uint8_t* program_descriptors = nullptr;
    if (!body.read_view(program_descriptors_length, program_descriptors)) return false;
    ByteReader program_descriptor_reader(program_descriptors, program_descriptors_length);
    const ApplicationServiceCallback application_service = [this](ApplicationServiceInfo info) {
        for (const auto& location : info.event_message_locations) {
            if (location.packet_id.has_value()) {
                event_message_tags_[*location.packet_id] = location.event_message_tag;
            }
        }
        on_application_service_(std::move(info));
    };
    if (!parse_program_descriptors(program_descriptor_reader, context_id_,
                                   application_service) ||
        !body.read_u8(asset_count)) {
        return false;
    }
    (void)mode;
    on_package_(context_id_, std::move(package_id));

    for (std::uint16_t asset_index = 0; asset_index < asset_count; ++asset_index) {
        std::uint8_t identifier_type = 0;
        std::uint8_t asset_id_length = 0;
        std::vector<std::uint8_t> asset_id;
        const std::uint8_t* asset_type_data = nullptr;
        std::uint8_t clock_flags = 0;
        std::uint8_t location_count = 0;
        if (!body.read_u8(identifier_type) || !body.skip(4) ||
            !body.read_u8(asset_id_length) || !body.read_bytes(asset_id_length, asset_id) ||
            !body.read_view(4, asset_type_data) || !body.read_u8(clock_flags) ||
            !body.read_u8(location_count)) {
            return false;
        }
        (void)identifier_type;
        (void)clock_flags;

        std::optional<std::uint16_t> packet_id;
        for (std::uint16_t location_index = 0; location_index < location_count; ++location_index) {
            if (!skip_general_location(body, packet_id)) return false;
        }

        std::uint16_t descriptors_length = 0;
        const std::uint8_t* descriptors = nullptr;
        if (!body.read_u16(descriptors_length) ||
            !body.read_view(descriptors_length, descriptors)) {
            return false;
        }
        AssetMetadata metadata;
        ByteReader descriptor_reader(descriptors, descriptors_length);
        if (!parse_descriptors(descriptor_reader, metadata)) return false;
        if (!packet_id.has_value()) continue;

        const std::string asset_type(reinterpret_cast<const char*>(asset_type_data), 4);
        TrackInfo track;
        track.context_id = context_id_;
        track.packet_id = *packet_id;
        track.asset_id = std::move(asset_id);
        track.language = metadata.language;
        track.component_tag = metadata.component_tag;
        track.timescale = metadata.timescale;
        track.audio = metadata.audio;
        track.subtitle = metadata.subtitle;
        track.presentation_regions = metadata.presentation_regions;

        bool supported = true;
        if (asset_type == "hev1") {
            track.kind = TrackKind::Video;
            track.codec = Codec::Hevc;
        } else if (asset_type == "mp4a" && metadata.aac_latm) {
            track.kind = TrackKind::Audio;
            track.codec = Codec::AacLatm;
        } else if (asset_type == "stpp" && metadata.ttml) {
            track.kind = TrackKind::Subtitle;
            track.codec = Codec::Ttml;
            if (track.timescale == 1) track.timescale = 65536;
        } else {
            supported = false;
        }
        if (supported) {
            install_track(std::move(track), std::move(metadata), input_offset);
        } else if (asset_type == "aapp" || asset_type == "asgd" || asset_type == "aagd") {
            DataAssetInfo info;
            info.context_id = context_id_;
            info.packet_id = *packet_id;
            info.asset_id = std::move(track.asset_id);
            info.asset_type = asset_type;
            info.component_tag = metadata.component_tag;
            info.presentation_regions = metadata.presentation_regions;
            auto state_entry = data_assets_.find(*packet_id);
            if (state_entry == data_assets_.end()) {
                if (!acquire_state_()) {
                    on_error_(ErrorCode::ResourceLimit, input_offset, true,
                              "global MMTP packet/track-state limit exceeded");
                    continue;
                }
                state_entry = data_assets_.emplace(*packet_id, DataAssetState{}).first;
            }
            state_entry->second.info = info;
            on_data_asset_(std::move(info));
        }
    }
    return body.remaining() == 0;
}

bool MmtpParser::parse_package_list(const std::uint8_t* data, const std::size_t size,
                                    const std::uint64_t input_offset) {
    (void)input_offset;
    if (size < 4 || data[0] != 0x80) return false;
    const auto declared_size = static_cast<std::size_t>(read_be16(data + 2));
    if (declared_size != size - 4) return false;
    ByteReader body(data + 4, declared_size);
    std::uint8_t package_count = 0;
    if (!body.read_u8(package_count)) return false;
    for (std::uint16_t index = 0; index < package_count; ++index) {
        std::uint8_t package_length = 0;
        std::vector<std::uint8_t> package_id;
        std::optional<std::uint16_t> ignored_packet_id;
        if (!body.read_u8(package_length) || !body.read_bytes(package_length, package_id) ||
            !skip_general_location(body, ignored_packet_id)) {
            return false;
        }
        on_package_(context_id_, std::move(package_id));
    }
    std::uint8_t ip_delivery_count = 0;
    if (!body.read_u8(ip_delivery_count)) return false;
    if (ip_delivery_count != 0) {
        on_error_(ErrorCode::UnsupportedFeature, input_offset, true,
                  "package-list IP delivery alternatives are not supported");
    }
    return true;
}

void MmtpParser::parse_signalling(const std::uint16_t packet_id,
                                  const std::uint32_t sequence,
                                  const std::uint8_t* data, const std::size_t size,
                                  const std::uint64_t input_offset) {
    if (size < 2) {
        on_error_(ErrorCode::MalformedInput, input_offset, true,
                  "truncated MMTP signalling payload header");
        return;
    }
    auto assembler_entry = signalling_.find(packet_id);
    if (assembler_entry == signalling_.end()) {
        if (!acquire_state_()) {
            on_error_(ErrorCode::ResourceLimit, input_offset, true,
                      "global MMTP packet/track-state limit exceeded");
            return;
        }
        assembler_entry = signalling_.emplace(packet_id, SignallingAssembler{}).first;
    }
    auto& assembler = assembler_entry->second;
    const auto flags = data[0];
    const auto fragmentation = static_cast<std::uint8_t>(flags >> 6U);
    const bool length_extension = ((flags >> 1U) & 1U) != 0;
    const bool aggregation = (flags & 1U) != 0;
    const auto* body = data + 2;
    auto body_size = size - 2;

    if (assembler.state != FragmentState::Initial && sequence == assembler.last_sequence) {
        return; // duplicate signalling packet
    }
    if (assembler.state != FragmentState::Initial && sequence != assembler.last_sequence + 1U) {
        if (!assembler.data.empty()) {
            on_error_(ErrorCode::Discontinuity, input_offset, true,
                      "MMTP signalling sequence jump dropped an incomplete unit");
        }
        assembler.data.clear();
        assembler.input_offset = 0;
        assembler.state = FragmentState::Skipping;
    }
    assembler.last_sequence = sequence;

    if (aggregation) {
        if (fragmentation != 0) {
            on_error_(ErrorCode::MalformedInput, input_offset, true,
                      "aggregated signalling payload is also fragmented");
            return;
        }
        const std::size_t length_size = length_extension ? 4 : 2;
        while (body_size != 0) {
            if (body_size < length_size) {
                on_error_(ErrorCode::MalformedInput, input_offset, true,
                          "truncated aggregated signalling length");
                return;
            }
            const auto unit_size = length_extension
                ? static_cast<std::size_t>(read_be32(body))
                : static_cast<std::size_t>(read_be16(body));
            body += length_size;
            body_size -= length_size;
            if (unit_size > body_size || unit_size > limits_.max_signalling_message) {
                on_error_(unit_size > limits_.max_signalling_message
                              ? ErrorCode::ResourceLimit
                              : ErrorCode::MalformedInput,
                          input_offset, true,
                          "aggregated signalling unit length exceeds bounds");
                return;
            }
            accept_signalling_unit(packet_id, body, unit_size, input_offset);
            body += unit_size;
            body_size -= unit_size;
        }
        assembler.state = FragmentState::Idle;
        return;
    }

    switch (fragmentation) {
    case 0:
        if (assembler.state == FragmentState::Collecting) {
            on_error_(ErrorCode::MalformedInput, input_offset, true,
                      "complete signalling unit interrupted a fragmented unit");
        }
        assembler.data.clear();
        assembler.input_offset = 0;
        assembler.state = FragmentState::Idle;
        accept_signalling_unit(packet_id, body, body_size, input_offset);
        break;
    case 1:
        assembler.data.clear();
        assembler.input_offset = input_offset;
        assembler.state = FragmentState::Collecting;
        append(assembler, body, body_size, input_offset);
        break;
    case 2:
        if (assembler.state == FragmentState::Skipping) {
            return;
        }
        if (assembler.state != FragmentState::Collecting) {
            on_error_(ErrorCode::MalformedInput, input_offset, true,
                      "middle signalling fragment has no first fragment");
            assembler.state = FragmentState::Skipping;
            return;
        }
        append(assembler, body, body_size, input_offset);
        break;
    case 3:
        if (assembler.state == FragmentState::Skipping) {
            assembler.state = FragmentState::Idle;
            assembler.data.clear();
            assembler.input_offset = 0;
            return;
        }
        if (assembler.state != FragmentState::Collecting) {
            on_error_(ErrorCode::MalformedInput, input_offset, true,
                      "last signalling fragment has no first fragment");
            return;
        }
        if (append(assembler, body, body_size, input_offset)) {
            accept_signalling_unit(packet_id, assembler.data.data(), assembler.data.size(),
                                   assembler.input_offset);
        }
        assembler.data.clear();
        assembler.input_offset = 0;
        assembler.state = FragmentState::Idle;
        break;
    default:
        break;
    }
}

} // namespace tlvdemux::detail
