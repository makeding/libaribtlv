#pragma once

#include <cstdint>
#include <optional>

namespace aribtlv {

// ISO/IEC 23091-2 (CICP) transfer-characteristics values used by ARIB
// broadcast video. These describe the coded signal; they are not a tone-map
// decision by themselves.
enum class VideoTransferCharacteristics : std::uint8_t {
    Unknown = 0,
    Bt709 = 1,
    Bt2020_10 = 11,
    Bt2020_12 = 14,
    Smpte2084 = 16,
    AribHlg = 18,
};

constexpr std::optional<VideoTransferCharacteristics>
cicp_transfer_from_b60(const std::uint8_t value) noexcept {
    switch (value) {
    case 1: return VideoTransferCharacteristics::Bt709;
    case 2: return VideoTransferCharacteristics::Bt2020_10;
    case 3: return VideoTransferCharacteristics::Bt2020_12;
    case 4: return VideoTransferCharacteristics::Smpte2084;
    case 5: return VideoTransferCharacteristics::AribHlg;
    default: return std::nullopt;
    }
}

constexpr bool is_hdr_transfer(const VideoTransferCharacteristics value) noexcept {
    return value == VideoTransferCharacteristics::Smpte2084 ||
        value == VideoTransferCharacteristics::AribHlg;
}

constexpr bool is_hlg_transfer(const VideoTransferCharacteristics value) noexcept {
    return value == VideoTransferCharacteristics::AribHlg;
}

constexpr bool is_pq_transfer(const VideoTransferCharacteristics value) noexcept {
    return value == VideoTransferCharacteristics::Smpte2084;
}

} // namespace aribtlv
