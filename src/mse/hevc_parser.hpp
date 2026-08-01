#pragma once

#include "common.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tlvdemux::detail::mse {

struct NaluView {
    int type = -1;
    std::size_t offset = 0;
    std::size_t size = 0;
};

struct HevcConfiguration {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string codec;
    Bytes hvcc;
};

std::vector<NaluView> annex_b_views(const Bytes& data);
Bytes copy_nalu(const Bytes& data, const NaluView& view);
HevcConfiguration hevc_configuration(const Bytes& vps, const Bytes& sps,
                                     const Bytes& pps);

} // namespace tlvdemux::detail::mse
