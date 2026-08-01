#pragma once

#include "common.hpp"

#include <cstdint>
#include <optional>

namespace tlvdemux::detail::mse {

struct AacFrame {
    Bytes data;
    Bytes asc;
    std::uint32_t object = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;
};

class LatmParser {
public:
    AacFrame parse(const Bytes& data);

private:
    static std::uint32_t latm_value(BitReader& reader);
    std::optional<AacFrame> config_;
};

} // namespace tlvdemux::detail::mse
