#pragma once

#include "common.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tlvdemux::detail::mse {

struct Mp4Track {
    std::uint32_t id = 1;
    bool video = false;
    std::uint32_t timescale = 1000000;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;
    std::string codec;
    Bytes config;
};

struct Sample {
    Bytes data;
    std::int64_t dts = 0;
    std::int64_t pts = 0;
    std::uint32_t duration = 0;
    bool keyframe = false;
};

Bytes init_segment(const Mp4Track& track);
Bytes media_segment(const Mp4Track& track, const std::vector<Sample>& samples,
                    std::uint32_t sequence);

} // namespace tlvdemux::detail::mse
