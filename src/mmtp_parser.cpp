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
                       DataAssetCallback on_data_asset,
                       DataUnitCallback on_data_unit,
                       SignallingCallback on_signalling,
                       EventCallback on_event,
                       StreamEventCallback on_stream_event,
                       ApplicationCallback on_application,
                       DataTransmissionCallback on_data_transmission,
                       DataDirectoryCallback on_data_directory,
                       DataAssetManagementCallback on_data_asset_management,
                       StateAcquireCallback acquire_state,
                       StateReleaseCallback release_state, ErrorCallback on_error)
    : context_id_(context_id), limits_(limits), on_package_(std::move(on_package)),
      on_track_(std::move(on_track)), on_access_unit_(std::move(on_access_unit)),
      on_application_service_(std::move(on_application_service)),
      on_data_asset_(std::move(on_data_asset)), on_data_unit_(std::move(on_data_unit)),
      on_signalling_(std::move(on_signalling)),
      on_event_(std::move(on_event)),
      on_stream_event_(std::move(on_stream_event)),
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
                if (extension_count == 0) application.transport_urls.push_back(base_url);
                for (std::uint16_t index = 0; index < extension_count; ++index) {
                    std::uint8_t extension_length = 0;
                    std::vector<std::uint8_t> extension;
                    if (!selector.read_u8(extension_length) ||
                        !selector.read_bytes(extension_length, extension)) {
                        return false;
                    }
                    application.transport_urls.push_back(
                        base_url + std::string(reinterpret_cast<const char*>(extension.data()),
                                               extension.size()));
                }
            }
        }
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

std::uint64_t expand_short_ntp(const std::uint32_t short_ntp,
                               const std::uint64_t reference) {
    const auto reference_seconds = static_cast<std::int64_t>(reference >> 32U);
    const auto short_seconds = static_cast<std::int64_t>(short_ntp >> 16U);
    auto seconds = (reference_seconds & ~0xffffLL) | short_seconds;
    if (seconds - reference_seconds > 32768) seconds -= 65536;
    if (reference_seconds - seconds > 32768) seconds += 65536;
    const auto fraction = static_cast<std::uint64_t>(short_ntp & 0xffffU) << 16U;
    return (static_cast<std::uint64_t>(seconds) << 32U) | fraction;
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
    (void)input_offset;
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
    (void)section_number;
    (void)last_section_number;
    const auto common_length = static_cast<std::size_t>(common_length_field & 0x0fffU);
    if (!body.skip(common_length)) return false;
    std::uint16_t loop_length_field = 0;
    if (!body.read_u16(loop_length_field)) return false;
    const auto loop_length = static_cast<std::size_t>(loop_length_field & 0x0fffU);
    const std::uint8_t* loop_data = nullptr;
    if (!body.read_view(loop_length, loop_data) || body.remaining() != 4) return false;

    ByteReader applications(loop_data, loop_length);
    while (applications.remaining() != 0) {
        if (applications.remaining() < 9) return false;
        ApplicationInfo application;
        application.context_id = context_id_;
        application.source_packet_id = packet_id;
        application.application_type = application_type;
        application.version = static_cast<std::uint8_t>((version_flags >> 1U) & 0x1fU);
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
        on_application_(std::move(application));
    }
    return true;
}

bool MmtpParser::parse_tables(const std::uint8_t* data, const std::size_t size,
                              const std::uint16_t packet_id,
                              const std::uint64_t input_offset) {
    ByteReader tables(data, size);
    while (tables.remaining() != 0) {
        std::uint8_t table_id = 0;
        if (!tables.peek_u8(table_id)) return false;
        if (table_id == 0x20 || table_id == 0x80) {
            if (tables.remaining() < 4) return false;
            const auto table_size = 4 + static_cast<std::size_t>(read_be16(tables.current() + 2));
            const std::uint8_t* table = nullptr;
            if (!tables.read_view(table_size, table)) return false;
            const bool valid = table_id == 0x20
                ? parse_mpt(table, table_size, input_offset)
                : parse_package_list(table, table_size, input_offset);
            if (!valid) return false;
        } else if (table_id >= 0x81) {
            if (tables.remaining() < 3) return false;
            const auto section_size = 3 + static_cast<std::size_t>(read_be16(tables.current() + 1) & 0x0fffU);
            const std::uint8_t* section = nullptr;
            if (!tables.read_view(section_size, section)) return false;
            if (table_id >= 0x8b && table_id <= 0x9b &&
                !parse_mh_eit(section, section_size, packet_id, input_offset)) {
                return false;
            }
            if (table_id == 0x9c &&
                !parse_mh_ait(section, section_size, packet_id, input_offset)) {
                return false;
            }
            if (table_id == 0xa6 &&
                !parse_emt(section, section_size, packet_id, input_offset)) {
                return false;
            }
        } else {
            // ARIB MMT tables in PA/M2 use a one-byte ID/version followed by a
            // 16-bit payload length. This lets v1 skip unneeded tables without
            // interpreting their contents.
            if (tables.remaining() < 4) return false;
            const auto table_size = 4 + static_cast<std::size_t>(read_be16(tables.current() + 2));
            if (!tables.skip(table_size)) return false;
        }
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

bool MmtpParser::parse_emt(const std::uint8_t* data, const std::size_t size,
                           const std::uint16_t packet_id,
                           const std::uint64_t input_offset) {
    if (size < 12 || data[0] != 0xa6) return false;
    const auto section_length = static_cast<std::size_t>(read_be16(data + 1) & 0x0fffU);
    if (section_length + 3 != size || section_length < 9) return false;

    const auto section_end = size - 4;
    const auto data_event_and_group = read_be16(data + 3);
    const auto data_event_id = static_cast<std::uint8_t>(data_event_and_group >> 12U);
    const auto table_group_id = static_cast<std::uint16_t>(data_event_and_group & 0x0fffU);
    const bool current_next = (data[5] & 0x01U) != 0;
    const auto tag = event_message_tags_.find(packet_id);
    const auto event_message_tag = tag == event_message_tags_.end()
        ? std::uint8_t{0} : tag->second;

    struct DescriptorView {
        std::uint16_t tag = 0;
        const std::uint8_t* payload = nullptr;
        std::size_t length = 0;
    };
    std::vector<DescriptorView> descriptor_views;
    ByteReader descriptors(data + 8, section_end - 8);
    while (descriptors.remaining() != 0) {
        std::uint16_t descriptor_tag = 0;
        std::uint32_t descriptor_size = 0;
        if (!descriptors.read_u16(descriptor_tag) ||
            !descriptor_length(descriptors, descriptor_tag, descriptor_size) ||
            descriptor_size > descriptors.remaining()) {
            return false;
        }
        const std::uint8_t* payload = nullptr;
        if (!descriptors.read_view(descriptor_size, payload)) return false;
        descriptor_views.push_back(
            DescriptorView{descriptor_tag, payload, static_cast<std::size_t>(descriptor_size)});
    }

    std::optional<std::uint64_t> utc_reference;
    std::optional<std::uint64_t> npt_reference;
    for (const auto& descriptor : descriptor_views) {
        if (descriptor.tag == 0x8021 && descriptor.length >= 17) {
            utc_reference = read_be64(descriptor.payload);
            npt_reference = read_be64(descriptor.payload + 8);
        }
    }

    for (const auto& descriptor : descriptor_views) {
        if (descriptor.tag != 0xf003) continue;
        if (descriptor.length < 14) return false;
        StreamEvent event;
        event.context_id = context_id_;
        event.source_packet_id = packet_id;
        event.event_message_tag = event_message_tag;
        event.data_event_id = data_event_id;
        event.message_group_id = static_cast<std::uint16_t>(
            read_be16(descriptor.payload) >> 4U);
        // All event-message descriptors in one EMT are required to use its group.
        if (event.message_group_id != table_group_id) return false;
        event.current_next = current_next;
        event.section_number = data[6];
        event.last_section_number = data[7];
        event.time_mode = descriptor.payload[2];
        event.time_value = read_be64(descriptor.payload + 3);
        event.utc_reference = utc_reference;
        event.npt_reference = npt_reference;
        event.message_type = descriptor.payload[11];
        event.raw_message_id = read_be16(descriptor.payload + 12);
        event.message_id = static_cast<std::uint8_t>(event.raw_message_id >> 8U);
        event.message_version = static_cast<std::uint8_t>(event.raw_message_id);
        event.private_data.assign(descriptor.payload + 14,
                                  descriptor.payload + descriptor.length);
        event.input_offset = input_offset;
        on_stream_event_(std::move(event));
    }
    return true;
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

void MmtpParser::install_track(TrackInfo info, AssetMetadata metadata,
                               const std::uint64_t input_offset) {
    auto state_entry = tracks_.find(info.packet_id);
    if (state_entry == tracks_.end()) {
        if (!acquire_state_()) {
            on_error_(ErrorCode::ResourceLimit, input_offset, true,
                      "global MMTP packet/track-state limit exceeded");
            return;
        }
        state_entry = tracks_.emplace(info.packet_id, TrackState{}).first;
    }
    auto& state = state_entry->second;
    const bool first_install = state.stable_track_id == 0;
    const bool codec_changed = !first_install && state.info.codec != info.codec;
    state.stable_track_id = on_track_(info);
    info.track_id = state.stable_track_id;
    state.info = std::move(info);
    state.restart_offset = input_offset;
    for (auto& entry : metadata.timestamps) {
        entry.second.restart_offset = input_offset;
        state.timestamps[entry.first] = entry.second;
        if (!latest_full_ntp_.has_value() || entry.second.ntp > *latest_full_ntp_) {
            latest_full_ntp_ = entry.second.ntp;
        }
    }
    for (auto& entry : metadata.extended_timestamps) {
        entry.second.restart_offset = input_offset;
        state.extended_timestamps[entry.first] = std::move(entry.second);
    }
    constexpr std::size_t max_timestamp_entries = 32;
    while (state.timestamps.size() > max_timestamp_entries) state.timestamps.erase(state.timestamps.begin());
    while (state.extended_timestamps.size() > max_timestamp_entries) {
        state.extended_timestamps.erase(state.extended_timestamps.begin());
    }
    if ((first_install || codec_changed) && state.info.kind == TrackKind::Video) {
        state.wait_for_rap = true;
        state.pending_hevc = {};
        state.media = {};
    }
}

bool MmtpParser::append_media(TrackState& track, const std::uint8_t* data,
                              const std::size_t size, const std::uint64_t input_offset) {
    if (track.media.data.size() > limits_.max_access_unit ||
        size > limits_.max_access_unit - track.media.data.size()) {
        track.media.data.clear();
        track.media.state = FragmentState::Skipping;
        track.discontinuity = true;
        on_error_(ErrorCode::ResourceLimit, input_offset, true,
                  "fragmented MFU exceeds configured access-unit limit");
        return false;
    }
    track.media.data.insert(track.media.data.end(), data, data + size);
    return true;
}

void MmtpParser::consume_mfu_piece(TrackState& track,
                                   const std::uint32_t packet_sequence,
                                   const std::uint32_t mpu_sequence,
                                   const bool timed, const std::uint8_t fragmentation,
                                   const bool aggregation, const bool random_access,
                                   const std::uint8_t* data, const std::size_t size,
                                   const std::uint64_t input_offset) {
    const std::size_t header_size = timed ? 14 : 4;
    if (size < header_size) {
        track.discontinuity = true;
        on_error_(ErrorCode::MalformedInput, input_offset, true,
                  "truncated timed/non-timed MFU header");
        return;
    }
    const auto sample_number = timed ? read_be32(data + 4) : read_be32(data);
    const auto* payload = data + header_size;
    const auto payload_size = size - header_size;
    auto& assembler = track.media;

    if (assembler.state != FragmentState::Initial &&
        packet_sequence != assembler.last_packet_sequence + 1U &&
        !(aggregation && fragmentation == 0 && packet_sequence == assembler.last_packet_sequence)) {
        if (packet_sequence == assembler.last_packet_sequence) {
            return; // duplicate MMTP packet
        }
        if (!assembler.data.empty()) {
            on_error_(ErrorCode::Discontinuity, input_offset, true,
                      "MMTP media sequence jump dropped an incomplete MFU");
        }
        assembler.data.clear();
        assembler.state = FragmentState::Skipping;
        track.discontinuity = true;
    }
    assembler.last_packet_sequence = packet_sequence;

    switch (fragmentation) {
    case 0:
        if (assembler.state == FragmentState::Collecting) {
            assembler.data.clear();
            track.discontinuity = true;
            on_error_(ErrorCode::Discontinuity, input_offset, true,
                      "complete MFU interrupted a fragmented MFU");
        }
        assembler.state = FragmentState::Idle;
        consume_complete_mfu(track, mpu_sequence, sample_number, random_access,
                             payload, payload_size, input_offset, track.restart_offset);
        break;
    case 1:
        assembler.data.clear();
        assembler.state = FragmentState::Collecting;
        assembler.mpu_sequence = mpu_sequence;
        assembler.sample_number = sample_number;
        assembler.input_offset = input_offset;
        assembler.restart_offset = track.restart_offset;
        assembler.random_access = random_access;
        append_media(track, payload, payload_size, input_offset);
        break;
    case 2:
        if (assembler.state == FragmentState::Skipping) return;
        if (assembler.state != FragmentState::Collecting ||
            assembler.mpu_sequence != mpu_sequence || assembler.sample_number != sample_number) {
            assembler.data.clear();
            assembler.state = FragmentState::Skipping;
            track.discontinuity = true;
            on_error_(ErrorCode::MalformedInput, input_offset, true,
                      "middle MFU fragment has no matching first fragment");
            return;
        }
        assembler.random_access = assembler.random_access || random_access;
        append_media(track, payload, payload_size, input_offset);
        break;
    case 3:
        if (assembler.state == FragmentState::Skipping) {
            assembler.state = FragmentState::Idle;
            assembler.data.clear();
            return;
        }
        if (assembler.state != FragmentState::Collecting ||
            assembler.mpu_sequence != mpu_sequence || assembler.sample_number != sample_number) {
            assembler.data.clear();
            assembler.state = FragmentState::Idle;
            track.discontinuity = true;
            on_error_(ErrorCode::MalformedInput, input_offset, true,
                      "last MFU fragment has no matching first fragment");
            return;
        }
        assembler.random_access = assembler.random_access || random_access;
        if (append_media(track, payload, payload_size, input_offset)) {
            consume_complete_mfu(track, assembler.mpu_sequence, assembler.sample_number,
                                 assembler.random_access, assembler.data.data(),
                                 assembler.data.size(), assembler.input_offset,
                                 assembler.restart_offset);
        }
        assembler.data.clear();
        assembler.state = FragmentState::Idle;
        break;
    default:
        break;
    }
}

void MmtpParser::finalize_hevc(TrackState& track) {
    auto& pending = track.pending_hevc;
    if (!pending.active) return;
    if (pending.has_vcl) {
        if (track.wait_for_rap && !pending.random_access) {
            pending = {};
            return;
        }
        if (pending.random_access) track.wait_for_rap = false;
        emit_access_unit(track, pending.mpu_sequence, std::move(pending.data),
                         pending.random_access, pending.input_offset,
                         pending.restart_offset);
    } else {
        track.discontinuity = true;
        on_error_(ErrorCode::MalformedInput, pending.input_offset, true,
                  "dropped HEVC access-unit prefix without a VCL NAL unit");
    }
    pending = {};
}

void MmtpParser::consume_complete_mfu(TrackState& track,
                                      const std::uint32_t mpu_sequence,
                                      const std::uint32_t sample_number,
                                      const bool random_access,
                                      const std::uint8_t* data, const std::size_t size,
                                      const std::uint64_t input_offset,
                                      const std::uint64_t restart_offset) {
    if (track.info.codec == Codec::Hevc) {
        if (size < 4 || static_cast<std::size_t>(read_be32(data)) != size - 4) {
            track.discontinuity = true;
            on_error_(ErrorCode::MalformedInput, input_offset, true,
                      "HEVC MFU does not contain one bounded length-prefixed NAL unit");
            return;
        }
        const auto nal_size = size - 4;
        if (nal_size < 2) {
            track.discontinuity = true;
            on_error_(ErrorCode::MalformedInput, input_offset, true,
                      "HEVC NAL unit is shorter than its header");
            return;
        }
        const auto nal_type = static_cast<std::uint8_t>((data[4] >> 1U) & 0x3fU);
        const bool is_vcl = nal_type <= 31;
        const bool is_irap = nal_type >= 16 && nal_type <= 23;
        const bool first_slice = is_vcl && nal_size >= 3 && (data[6] & 0x80U) != 0;
        auto& pending = track.pending_hevc;
        const bool begins_access_unit = nal_type == 35 ||
            (first_slice && pending.has_vcl) ||
            ((nal_type == 32 || nal_type == 33 || nal_type == 34 || nal_type == 39) &&
             pending.has_vcl);
        if (pending.active && begins_access_unit) {
            finalize_hevc(track);
        }
        if (!pending.active) {
            pending.active = true;
            pending.mpu_sequence = mpu_sequence;
            pending.sample_number = sample_number;
            pending.input_offset = input_offset;
            pending.restart_offset = restart_offset;
        }
        if (pending.data.size() > limits_.max_access_unit ||
            limits_.max_access_unit - pending.data.size() < nal_size + 3) {
            pending = {};
            track.discontinuity = true;
            on_error_(ErrorCode::ResourceLimit, input_offset, true,
                      "HEVC decoded access unit exceeds configured limit");
            return;
        }
        pending.random_access = pending.random_access || random_access || is_irap;
        pending.has_vcl = pending.has_vcl || is_vcl;
        pending.data.insert(pending.data.end(), {0x00, 0x00, 0x01});
        pending.data.insert(pending.data.end(), data + 4, data + size);
        return;
    }

    if (track.info.codec == Codec::AacLatm) {
        if (size > 0x1fff) {
            track.discontinuity = true;
            on_error_(ErrorCode::ResourceLimit, input_offset, true,
                      "AAC AudioMuxElement exceeds the 13-bit LOAS length");
            return;
        }
        std::vector<std::uint8_t> loas;
        loas.reserve(size + 3);
        loas.push_back(0x56);
        loas.push_back(static_cast<std::uint8_t>(0xe0U | (size >> 8U)));
        loas.push_back(static_cast<std::uint8_t>(size));
        loas.insert(loas.end(), data, data + size);
        emit_access_unit(track, mpu_sequence, std::move(loas), random_access, input_offset,
                         restart_offset);
        return;
    }

    if (track.info.codec != Codec::Ttml || size < 7) {
        track.discontinuity = true;
        on_error_(ErrorCode::MalformedInput, input_offset, true,
                  "truncated or unsupported TTML MFU");
        return;
    }

    const auto subtitle_sequence = data[1];
    const auto subsample_number = data[2];
    const auto last_subsample = data[3];
    const auto flags = data[4];
    const auto data_type = static_cast<std::uint8_t>(flags >> 4U);
    const bool length_extended = ((flags >> 3U) & 1U) != 0;
    const bool info_list = ((flags >> 2U) & 1U) != 0;
    if (data_type > 7 || subsample_number > last_subsample ||
        (subsample_number == 0 && data_type != 0)) {
        track.discontinuity = true;
        on_error_(ErrorCode::UnsupportedFeature, input_offset, true,
                  "unsupported TTML data type or invalid subsample number");
        return;
    }
    std::size_t cursor = 5;
    const std::size_t length_size = length_extended ? 4 : 2;
    if (size - cursor < length_size) {
        track.discontinuity = true;
        on_error_(ErrorCode::MalformedInput, input_offset, true,
                  "truncated TTML data length");
        return;
    }
    const auto data_size = length_extended
        ? static_cast<std::size_t>(read_be32(data + cursor))
        : static_cast<std::size_t>(read_be16(data + cursor));
    cursor += length_size;
    if (subsample_number == 0 && last_subsample > 0 && info_list) {
        for (std::uint16_t index = 0; index < last_subsample; ++index) {
            if (size - cursor < 1 + length_size) {
                track.discontinuity = true;
                on_error_(ErrorCode::MalformedInput, input_offset, true,
                          "truncated TTML subsample information list");
                return;
            }
            const auto listed_data_type = static_cast<std::uint8_t>(data[cursor] >> 4U);
            if (listed_data_type > 7) {
                track.discontinuity = true;
                on_error_(ErrorCode::UnsupportedFeature, input_offset, true,
                          "unsupported TTML resource data type");
                return;
            }
            cursor += 1 + length_size;
        }
    }
    if (data_size > size - cursor || data_size > limits_.max_ttml_sample) {
        track.discontinuity = true;
        on_error_(data_size > limits_.max_ttml_sample
                      ? ErrorCode::ResourceLimit : ErrorCode::MalformedInput,
                  input_offset, true, "TTML subsample length exceeds bounds");
        return;
    }

    auto& subtitle = track.subtitle;
    if (!subtitle.active || subtitle.sequence != subtitle_sequence ||
        subtitle.last_subsample != last_subsample || subtitle.mpu_sequence != mpu_sequence) {
        if (subtitle.active) {
            track.discontinuity = true;
            on_error_(ErrorCode::Discontinuity, input_offset, true,
                      "new TTML unit replaced an incomplete subsample group");
        }
        subtitle = {};
        subtitle.active = true;
        subtitle.sequence = subtitle_sequence;
        subtitle.last_subsample = last_subsample;
        subtitle.mpu_sequence = mpu_sequence;
        subtitle.input_offset = input_offset;
        subtitle.restart_offset = restart_offset;
        subtitle.random_access = random_access;
        subtitle.subsamples.resize(static_cast<std::size_t>(last_subsample) + 1);
    }
    subtitle.random_access = subtitle.random_access || random_access;
    auto& slot = subtitle.subsamples[subsample_number];
    if (!slot.has_value()) {
        slot = SubtitleAssembly::Subsample{
            data_type,
            std::vector<std::uint8_t>(data + cursor, data + cursor + data_size)};
    }
    if (!std::all_of(subtitle.subsamples.begin(), subtitle.subsamples.end(),
                     [](const auto& value) { return value.has_value(); })) {
        return;
    }
    std::size_t total_size = 0;
    for (const auto& value : subtitle.subsamples) total_size += value->data.size();
    if (total_size > limits_.max_ttml_sample) {
        subtitle = {};
        track.discontinuity = true;
        on_error_(ErrorCode::ResourceLimit, input_offset, true,
                  "reassembled TTML sample exceeds configured limit");
        return;
    }
    if (subtitle.subsamples.empty() || !subtitle.subsamples[0].has_value() ||
        subtitle.subsamples[0]->data_type != 0) {
        subtitle = {};
        track.discontinuity = true;
        on_error_(ErrorCode::MalformedInput, input_offset, true,
                  "TTML subtitle group has no document in subsample zero");
        return;
    }
    std::vector<std::uint8_t> ttml = std::move(subtitle.subsamples[0]->data);
    std::vector<SubtitleResource> resources;
    resources.reserve(subtitle.subsamples.size() - 1);
    for (std::size_t index = 1; index < subtitle.subsamples.size(); ++index) {
        auto& value = *subtitle.subsamples[index];
        resources.push_back(SubtitleResource{
            static_cast<std::uint8_t>(index), value.data_type, std::move(value.data)});
    }
    const auto output_offset = subtitle.input_offset;
    const auto output_restart_offset = subtitle.restart_offset;
    const auto output_rap = subtitle.random_access;
    subtitle = {};
    emit_access_unit(track, mpu_sequence, std::move(ttml), output_rap, output_offset,
                     output_restart_offset, std::move(resources));
}

void MmtpParser::emit_access_unit(TrackState& track, const std::uint32_t mpu_sequence,
                                  std::vector<std::uint8_t> data,
                                  const bool random_access,
                                  const std::uint64_t input_offset,
                                  const std::uint64_t restart_offset,
                                  std::vector<SubtitleResource> subtitle_resources) {
    const auto timestamp = track.timestamps.find(mpu_sequence);
    const auto extended = track.extended_timestamps.find(mpu_sequence);
    std::int64_t dts_offset = 0;
    std::int64_t pts_offset = 0;
    std::uint64_t ntp = 0;
    auto output_restart_offset = restart_offset;
    if (timestamp != track.timestamps.end() && extended != track.extended_timestamps.end() &&
        track.au_index < extended->second.dts_pts_offsets.size() &&
        track.au_index < extended->second.pts_offsets.size()) {
        output_restart_offset = std::min(
            output_restart_offset,
            std::min(timestamp->second.restart_offset,
                     extended->second.restart_offset));
        dts_offset = -static_cast<std::int64_t>(extended->second.decoding_time_offset);
        for (std::size_t index = 0; index < track.au_index; ++index) {
            dts_offset += extended->second.pts_offsets[index];
        }
        pts_offset = dts_offset + extended->second.dts_pts_offsets[track.au_index];
        ntp = timestamp->second.ntp;
        ++track.au_index;
    } else if (track.info.codec == Codec::Ttml && latest_full_ntp_.has_value()) {
        const auto delivery = track.delivery_timestamps.find(mpu_sequence);
        if (delivery == track.delivery_timestamps.end()) {
            track.discontinuity = true;
            on_error_(ErrorCode::Discontinuity, input_offset, true,
                      "dropped TTML sample without a delivery timestamp");
            return;
        }
        // B60 provides no MPU timestamp descriptors on the subtitle assets in
        // the broadcast samples. Their timed MPU is therefore anchored to the
        // MMTP short-form NTP delivery timestamp, expanded around the latest
        // full NTP mapping received for the same context.
        ntp = expand_short_ntp(delivery->second, *latest_full_ntp_);
    } else {
        track.discontinuity = true;
        ++track.au_index;
        on_error_(ErrorCode::Discontinuity, input_offset, true,
                  "dropped access unit without a matching timestamp descriptor");
        return;
    }

    const auto ntp_seconds = ntp >> 32U;
    const auto ntp_fraction = static_cast<std::uint32_t>(ntp);
    const auto ntp_microseconds = static_cast<std::int64_t>(
        ntp_seconds * 1000000ULL +
        (static_cast<std::uint64_t>(ntp_fraction) * 1000000ULL >> 32U));

    AccessUnit unit;
    unit.track_id = track.stable_track_id;
    unit.codec = track.info.codec;
    unit.component_tag = track.info.component_tag;
    if (track.info.subtitle.has_value()) {
        unit.subtitle_timing_mode = track.info.subtitle->timing_mode;
    }
    unit.data = std::move(data);
    unit.subtitle_resources = std::move(subtitle_resources);
    unit.pts = Timestamp{pts_offset, track.info.timescale};
    unit.dts = Timestamp{dts_offset, track.info.timescale};
    unit.source_ntp = Timestamp{ntp_microseconds, 1000000};
    unit.mpu_sequence_number = mpu_sequence;
    unit.restart_offset = output_restart_offset;
    unit.input_offset = input_offset;
    unit.random_access = random_access;
    unit.discontinuity = track.discontinuity;
    track.discontinuity = false;
    on_access_unit_(TimedAccessUnit{std::move(unit), ntp});
}

void MmtpParser::emit_data_unit(DataAssetState& asset,
                                const std::uint32_t mpu_sequence,
                                const std::uint32_t item_id,
                                const std::uint8_t* data, const std::size_t size,
                                const std::uint64_t input_offset,
                                const PacketExtensions& extensions) {
    DataUnit unit;
    unit.context_id = context_id_;
    unit.packet_id = asset.info.packet_id;
    unit.asset_id = asset.info.asset_id;
    unit.asset_type = asset.info.asset_type;
    unit.component_tag = asset.info.component_tag;
    unit.mpu_sequence_number = mpu_sequence;
    unit.item_id = item_id;
    unit.download_id = extensions.download_id;
    unit.item_fragment_number = extensions.item_fragment_number;
    unit.last_item_fragment_number = extensions.last_item_fragment_number;
    unit.data.assign(data, data + size);
    unit.input_offset = input_offset;
    unit.discontinuity = asset.discontinuity;
    asset.discontinuity = false;
    on_data_unit_(std::move(unit));
}

void MmtpParser::consume_data_piece(DataAssetState& asset,
                                    const std::uint32_t packet_sequence,
                                    const std::uint32_t mpu_sequence,
                                    const std::uint8_t fragmentation,
                                    const bool aggregation,
                                    const std::uint8_t* data, const std::size_t size,
                                    const std::uint64_t input_offset,
                                    const PacketExtensions& extensions) {
    if (size < 4) {
        asset.discontinuity = true;
        on_error_(ErrorCode::MalformedInput, input_offset, true,
                  "truncated non-timed MFU header");
        return;
    }
    const auto item_id = read_be32(data);
    const auto* payload = data + 4;
    const auto payload_size = size - 4;
    auto& assembler = asset.media;
    if (assembler.state != FragmentState::Initial &&
        packet_sequence != assembler.last_packet_sequence + 1U &&
        !(aggregation && fragmentation == 0 &&
          packet_sequence == assembler.last_packet_sequence)) {
        if (packet_sequence == assembler.last_packet_sequence) return;
        if (!assembler.data.empty()) {
            on_error_(ErrorCode::Discontinuity, input_offset, true,
                      "MMTP data sequence jump dropped an incomplete MFU");
        }
        assembler.data.clear();
        assembler.state = FragmentState::Skipping;
        asset.discontinuity = true;
    }
    assembler.last_packet_sequence = packet_sequence;
    auto append_data = [&]() {
        if (assembler.data.size() > limits_.max_access_unit ||
            payload_size > limits_.max_access_unit - assembler.data.size()) {
            assembler.data.clear();
            assembler.state = FragmentState::Skipping;
            asset.discontinuity = true;
            on_error_(ErrorCode::ResourceLimit, input_offset, true,
                      "fragmented data MFU exceeds configured access-unit limit");
            return false;
        }
        assembler.data.insert(assembler.data.end(), payload, payload + payload_size);
        return true;
    };
    switch (fragmentation) {
    case 0:
        if (assembler.state == FragmentState::Collecting) {
            assembler.data.clear();
            asset.discontinuity = true;
        }
        assembler.state = FragmentState::Idle;
        emit_data_unit(asset, mpu_sequence, item_id, payload, payload_size,
                       input_offset, extensions);
        break;
    case 1:
        assembler.data.clear();
        assembler.state = FragmentState::Collecting;
        assembler.mpu_sequence = mpu_sequence;
        assembler.sample_number = item_id;
        assembler.input_offset = input_offset;
        assembler.download_id = extensions.download_id;
        assembler.item_fragment_number = extensions.item_fragment_number;
        assembler.last_item_fragment_number = extensions.last_item_fragment_number;
        append_data();
        break;
    case 2:
        if (assembler.state == FragmentState::Skipping) return;
        if (assembler.state != FragmentState::Collecting ||
            assembler.mpu_sequence != mpu_sequence || assembler.sample_number != item_id) {
            assembler.data.clear();
            assembler.state = FragmentState::Skipping;
            asset.discontinuity = true;
            return;
        }
        append_data();
        break;
    case 3:
        if (assembler.state == FragmentState::Skipping) {
            assembler.state = FragmentState::Idle;
            assembler.data.clear();
            return;
        }
        if (assembler.state != FragmentState::Collecting ||
            assembler.mpu_sequence != mpu_sequence || assembler.sample_number != item_id) {
            assembler.data.clear();
            assembler.state = FragmentState::Idle;
            asset.discontinuity = true;
            return;
        }
        if (append_data()) {
            PacketExtensions collected;
            collected.download_id = assembler.download_id;
            collected.item_fragment_number = assembler.item_fragment_number;
            collected.last_item_fragment_number = assembler.last_item_fragment_number;
            emit_data_unit(asset, assembler.mpu_sequence, assembler.sample_number,
                           assembler.data.data(), assembler.data.size(),
                           assembler.input_offset, collected);
        }
        assembler.data.clear();
        assembler.state = FragmentState::Idle;
        break;
    default:
        break;
    }
}

void MmtpParser::parse_mpu(const std::uint16_t packet_id,
                           const std::uint32_t packet_sequence,
                           const std::uint32_t delivery_timestamp,
                           const bool random_access, const std::uint8_t* data,
                           const std::size_t size, const std::uint64_t input_offset,
                           const PacketExtensions& extensions) {
    if (size < 8) {
        on_error_(ErrorCode::MalformedInput, input_offset, true,
                  "truncated MMTP MPU payload");
        return;
    }
    const auto declared_size = static_cast<std::size_t>(read_be16(data));
    if (declared_size != size - 2) {
        on_error_(ErrorCode::MalformedInput, input_offset, true,
                  "MMTP MPU payload length does not match its container");
        return;
    }
    const auto fragment_type = static_cast<std::uint8_t>(data[2] >> 4U);
    if (fragment_type != 2) {
        return;
    }
    const auto flags = data[2];
    const bool timed = ((flags >> 3U) & 1U) != 0;
    const auto fragmentation = static_cast<std::uint8_t>((flags >> 1U) & 0x03U);
    const bool aggregation = (flags & 1U) != 0;
    const auto mpu_sequence = read_be32(data + 4);
    const auto data_asset_entry = data_assets_.find(packet_id);
    if (data_asset_entry != data_assets_.end()) {
        auto& asset = data_asset_entry->second;
        if (timed) {
            asset.discontinuity = true;
            on_error_(ErrorCode::UnsupportedFeature, input_offset, true,
                      "timed data-asset MFU is unsupported");
            return;
        }
        if (aggregation && fragmentation != 0) {
            asset.discontinuity = true;
            on_error_(ErrorCode::MalformedInput, input_offset, true,
                      "aggregated data MPU payload is also fragmented");
            return;
        }
        const auto* body = data + 8;
        auto body_size = size - 8;
        if (!aggregation) {
            consume_data_piece(asset, packet_sequence, mpu_sequence, fragmentation,
                               false, body, body_size, input_offset, extensions);
            return;
        }
        while (body_size != 0) {
            if (body_size < 2) return;
            const auto unit_size = static_cast<std::size_t>(read_be16(body));
            body += 2;
            body_size -= 2;
            if (unit_size > body_size) return;
            consume_data_piece(asset, packet_sequence, mpu_sequence, 0, true,
                               body, unit_size, input_offset, extensions);
            body += unit_size;
            body_size -= unit_size;
        }
        return;
    }

    const auto track_entry = tracks_.find(packet_id);
    if (track_entry == tracks_.end()) return;
    auto& track = track_entry->second;
    track.delivery_timestamps[mpu_sequence] = delivery_timestamp;
    while (track.delivery_timestamps.size() > 32) {
        track.delivery_timestamps.erase(track.delivery_timestamps.begin());
    }

    if (aggregation && fragmentation != 0) {
        track.discontinuity = true;
        on_error_(ErrorCode::MalformedInput, input_offset, true,
                  "aggregated MPU payload is also fragmented");
        return;
    }
    if (!track.current_mpu_sequence.has_value() || *track.current_mpu_sequence != mpu_sequence) {
        if (track.current_mpu_sequence.has_value()) {
            finalize_hevc(track);
            if (mpu_sequence != *track.current_mpu_sequence + 1U) track.discontinuity = true;
            if (track.subtitle.active) {
                track.subtitle = {};
                track.discontinuity = true;
            }
        }
        track.current_mpu_sequence = mpu_sequence;
        track.au_index = 0;
    }

    const auto* body = data + 8;
    auto body_size = size - 8;
    if (!aggregation) {
        consume_mfu_piece(track, packet_sequence, mpu_sequence, timed, fragmentation,
                          false, random_access, body, body_size, input_offset);
        return;
    }
    while (body_size != 0) {
        if (body_size < 2) {
            track.discontinuity = true;
            on_error_(ErrorCode::MalformedInput, input_offset, true,
                      "truncated aggregated MFU length");
            return;
        }
        const auto unit_size = static_cast<std::size_t>(read_be16(body));
        body += 2;
        body_size -= 2;
        if (unit_size > body_size) {
            track.discontinuity = true;
            on_error_(ErrorCode::MalformedInput, input_offset, true,
                      "aggregated MFU length exceeds MPU payload");
            return;
        }
        consume_mfu_piece(track, packet_sequence, mpu_sequence, timed, 0,
                          true, random_access, body, unit_size, input_offset);
        body += unit_size;
        body_size -= unit_size;
    }
}

} // namespace tlvdemux::detail
