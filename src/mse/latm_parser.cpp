#include "latm_parser.hpp"

#include <cstddef>
#include <stdexcept>

namespace tlvdemux::detail::mse {
namespace {

constexpr std::uint32_t sample_rates[] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000,
    22050, 16000, 12000, 11025, 8000, 7350,
};

std::uint32_t aac_channel_count(const std::uint32_t configuration) {
    switch (configuration) {
    case 1: return 1;
    case 2: return 2;
    case 3: return 3;
    case 4: return 4;
    case 5: return 5;
    case 6: return 6;
    case 7: return 8;
    case 11: return 7;
    case 12: return 8;
    case 13: return 24;
    default: return 0;
    }
}

} // namespace

AacFrame LatmParser::parse(const Bytes& data) {
    if (data.size() < 4 || data[0] != 0x56 || (data[1] & 0xe0U) != 0xe0U) {
        throw std::runtime_error("invalid LOAS frame");
    }
    const auto length = static_cast<std::size_t>((data[1] & 0x1fU) << 8U | data[2]);
    if (length + 3 > data.size()) throw std::runtime_error("truncated LOAS frame");
    Bytes payload(data.begin() + 3,
                  data.begin() + static_cast<std::ptrdiff_t>(3 + length));
    BitReader reader(payload);
    if (!reader.boolean()) {
        const bool version = reader.boolean();
        const bool version_a = version && reader.boolean();
        if (version_a) throw std::runtime_error("LATM version A unsupported");
        if (version) latm_value(reader);
        if (!reader.boolean() || reader.bits(6) != 0 || reader.bits(4) != 0 ||
            reader.bits(3) != 0) {
            throw std::runtime_error("unsupported LATM layout");
        }
        const auto asc_length = version ? latm_value(reader) : 0;
        const auto asc_start = reader.offset();
        auto object = reader.bits(5);
        if (object == 31) object = 32 + reader.bits(6);
        const auto rate_index = reader.bits(4);
        const auto rate = rate_index == 15
            ? reader.bits(24)
            : (rate_index < 13 ? sample_rates[rate_index] : 0);
        const auto channel_configuration = reader.bits(4);
        const auto channels = aac_channel_count(channel_configuration);
        reader.bits(1);
        if (reader.boolean()) reader.bits(14);
        reader.bits(1);
        if (version && asc_length > reader.offset() - asc_start) {
            reader.bits(static_cast<unsigned>(asc_length - (reader.offset() - asc_start)));
        }
        if (reader.bits(3) != 0) {
            throw std::runtime_error("unsupported LATM frame length");
        }
        reader.bits(8);
        if (reader.boolean()) {
            bool more = false;
            do {
                more = reader.boolean();
                reader.bits(version ? latm_value(reader) : 8);
            } while (more);
        }
        if (reader.boolean()) reader.bits(8);
        if (rate == 0 || channels == 0 || object >= 32 || rate_index >= 15) {
            throw std::runtime_error("unsupported AAC config");
        }
        config_ = AacFrame{
            {},
            Bytes{static_cast<std::uint8_t>((object << 3U) | (rate_index >> 1U)),
                  static_cast<std::uint8_t>(((rate_index & 1U) << 7U) |
                                            (channel_configuration << 3U))},
            object,
            rate,
            channels,
        };
    }
    if (!config_) throw std::runtime_error("LATM config is missing");
    std::size_t payload_length = 0;
    std::uint32_t part = 0;
    do {
        part = reader.bits(8);
        payload_length += part;
    } while (part == 255);
    auto frame = *config_;
    frame.data = reader.bytes(payload_length);
    return frame;
}

std::uint32_t LatmParser::latm_value(BitReader& reader) {
    const auto count = reader.bits(2);
    std::uint32_t value = 0;
    for (std::uint32_t index = 0; index <= count; ++index) {
        value = value * 256U + reader.bits(8);
    }
    return value;
}

} // namespace tlvdemux::detail::mse
