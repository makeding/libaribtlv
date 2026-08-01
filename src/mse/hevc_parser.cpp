#include "hevc_parser.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace tlvdemux::detail::mse {
namespace {

Bytes u16(const std::uint32_t value) {
    return {static_cast<std::uint8_t>(value >> 8U),
            static_cast<std::uint8_t>(value)};
}

template <typename... Parts>
Bytes join(const Parts&... parts) {
    const std::size_t size = (parts.size() + ... + 0U);
    Bytes output;
    output.reserve(size);
    (append(output, parts), ...);
    return output;
}

Bytes rbsp(const Bytes& nalu) {
    Bytes output;
    output.reserve(nalu.size());
    for (std::size_t index = 0; index < nalu.size(); ++index) {
        if (index >= 2 && nalu[index] == 3 && nalu[index - 1] == 0 &&
            nalu[index - 2] == 0) {
            continue;
        }
        output.push_back(nalu[index]);
    }
    return output;
}

std::uint8_t reverse_byte(const std::uint8_t value) {
    std::uint8_t output = 0;
    for (unsigned index = 0; index < 8; ++index) {
        output |= static_cast<std::uint8_t>(
            ((value >> (7U - index)) & 1U) << index);
    }
    return output;
}

struct SpsInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t compatibility = 0;
    std::uint8_t profile_space = 0;
    std::uint8_t tier = 0;
    std::uint8_t profile = 0;
    std::uint8_t level = 0;
    std::uint8_t chroma = 0;
    std::uint8_t bit_luma = 0;
    std::uint8_t bit_chroma = 0;
    std::uint8_t layers = 0;
    std::uint8_t nested = 0;
    Bytes compatibility_bytes;
    Bytes constraints;
    std::string codec;
};

SpsInfo parse_sps(const Bytes& nalu) {
    const auto data = rbsp(nalu);
    BitReader reader(data);
    SpsInfo output;
    reader.bits(16);
    reader.bits(4);
    const auto max_layers = reader.bits(3);
    output.nested = static_cast<std::uint8_t>(reader.boolean());
    output.profile_space = static_cast<std::uint8_t>(reader.bits(2));
    output.tier = static_cast<std::uint8_t>(reader.bits(1));
    output.profile = static_cast<std::uint8_t>(reader.bits(5));
    for (int index = 0; index < 4; ++index) {
        output.compatibility_bytes.push_back(static_cast<std::uint8_t>(reader.bits(8)));
    }
    for (int index = 0; index < 6; ++index) {
        output.constraints.push_back(static_cast<std::uint8_t>(reader.bits(8)));
    }
    output.level = static_cast<std::uint8_t>(reader.bits(8));
    std::vector<bool> sub_profile(max_layers);
    std::vector<bool> sub_level(max_layers);
    for (std::uint32_t index = 0; index < max_layers; ++index) {
        sub_profile[index] = reader.boolean();
        sub_level[index] = reader.boolean();
    }
    if (max_layers > 0) reader.bits((8U - max_layers) * 2U);
    for (std::uint32_t index = 0; index < max_layers; ++index) {
        if (sub_profile[index]) reader.bits(88);
        if (sub_level[index]) reader.bits(8);
    }
    reader.ue();
    output.chroma = static_cast<std::uint8_t>(reader.ue());
    if (output.chroma == 3) reader.bits(1);
    const auto coded_width = reader.ue();
    const auto coded_height = reader.ue();
    std::uint32_t left = 0;
    std::uint32_t right = 0;
    std::uint32_t top = 0;
    std::uint32_t bottom = 0;
    if (reader.boolean()) {
        left = reader.ue();
        right = reader.ue();
        top = reader.ue();
        bottom = reader.ue();
    }
    output.bit_luma = static_cast<std::uint8_t>(reader.ue());
    output.bit_chroma = static_cast<std::uint8_t>(reader.ue());
    const auto sub_width = output.chroma == 1 || output.chroma == 2 ? 2U : 1U;
    const auto sub_height = output.chroma == 1 ? 2U : 1U;
    output.width = coded_width - sub_width * (left + right);
    output.height = coded_height - sub_height * (top + bottom);
    for (std::size_t index = 0; index < 4; ++index) {
        output.compatibility |= static_cast<std::uint32_t>(
            reverse_byte(output.compatibility_bytes[index])) << (index * 8U);
    }
    const char prefixes[] = {'\0', 'A', 'B', 'C'};
    output.codec = "hvc1.";
    if (prefixes[output.profile_space] != '\0') {
        output.codec += prefixes[output.profile_space];
    }
    output.codec += std::to_string(output.profile) + ".";
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%X.%c%u", output.compatibility,
                  output.tier != 0 ? 'H' : 'L', output.level);
    output.codec += buffer;
    auto last = static_cast<int>(output.constraints.size()) - 1;
    while (last >= 0 && output.constraints[static_cast<std::size_t>(last)] == 0) --last;
    for (int index = 0; index <= last; ++index) {
        std::snprintf(buffer, sizeof(buffer), ".%02X",
                      output.constraints[static_cast<std::size_t>(index)]);
        output.codec += buffer;
    }
    output.layers = static_cast<std::uint8_t>(max_layers + 1U);
    return output;
}

Bytes make_hvcc(const Bytes& vps, const Bytes& sps, const Bytes& pps,
                const SpsInfo& info) {
    Bytes header(23, 0);
    header[0] = 1;
    header[1] = static_cast<std::uint8_t>(
        (info.profile_space << 6U) | (info.tier << 5U) | info.profile);
    std::copy(info.compatibility_bytes.begin(), info.compatibility_bytes.end(),
              header.begin() + 2);
    std::copy(info.constraints.begin(), info.constraints.end(), header.begin() + 6);
    header[12] = info.level;
    header[13] = 0xf0;
    header[15] = 0xfc;
    header[16] = static_cast<std::uint8_t>(0xfcU | info.chroma);
    header[17] = static_cast<std::uint8_t>(0xf8U | info.bit_luma);
    header[18] = static_cast<std::uint8_t>(0xf8U | info.bit_chroma);
    header[21] = static_cast<std::uint8_t>(
        (static_cast<unsigned>(info.layers) << 3U) |
        (info.nested != 0 ? 4U : 0U) | 3U);
    header[22] = 3;
    const auto array = [](const std::uint8_t type, const Bytes& nalu) {
        return join(Bytes{static_cast<std::uint8_t>(0x80U | type), 0, 1},
                    u16(static_cast<std::uint32_t>(nalu.size())), nalu);
    };
    return join(header, array(32, vps), array(33, sps), array(34, pps));
}

} // namespace

std::vector<NaluView> annex_b_views(const Bytes& data) {
    struct Start {
        std::size_t data = 0;
        std::size_t code = 0;
    };
    std::vector<Start> starts;
    starts.reserve(16);
    for (std::size_t index = 0; index + 3 < data.size(); ++index) {
        if (data[index] == 0 && data[index + 1] == 0 && data[index + 2] == 1) {
            starts.push_back({index + 3, index});
            index += 2;
        } else if (index + 4 < data.size() && data[index] == 0 &&
                   data[index + 1] == 0 && data[index + 2] == 0 &&
                   data[index + 3] == 1) {
            starts.push_back({index + 4, index});
            index += 3;
        }
    }
    std::vector<NaluView> output;
    output.reserve(starts.size());
    for (std::size_t index = 0; index < starts.size(); ++index) {
        const auto end = index + 1 < starts.size() ? starts[index + 1].code : data.size();
        if (end < starts[index].data + 2) continue;
        output.push_back({static_cast<int>((data[starts[index].data] >> 1U) & 0x3fU),
                          starts[index].data, end - starts[index].data});
    }
    return output;
}

Bytes copy_nalu(const Bytes& data, const NaluView& view) {
    if (view.offset > data.size() || view.size > data.size() - view.offset) {
        throw std::runtime_error("HEVC NAL view exceeds access unit");
    }
    return Bytes(data.begin() + static_cast<std::ptrdiff_t>(view.offset),
                 data.begin() + static_cast<std::ptrdiff_t>(view.offset + view.size));
}

HevcConfiguration hevc_configuration(const Bytes& vps, const Bytes& sps,
                                     const Bytes& pps) {
    const auto info = parse_sps(sps);
    return {info.width, info.height, info.codec, make_hvcc(vps, sps, pps, info)};
}

} // namespace tlvdemux::detail::mse
