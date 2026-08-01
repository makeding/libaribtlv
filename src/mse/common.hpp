#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace tlvdemux::detail::mse {

using Bytes = std::vector<std::uint8_t>;

inline void append(Bytes& output, const Bytes& value) {
    output.insert(output.end(), value.begin(), value.end());
}

inline void append(Bytes& output, const std::uint8_t* data, const std::size_t size) {
    output.insert(output.end(), data, data + size);
}

class BitReader {
public:
    explicit BitReader(const Bytes& data) : data_(data) {}

    std::uint32_t bits(const unsigned count) {
        if (offset_ + count > data_.size() * 8) {
            throw std::runtime_error("truncated bitstream");
        }
        std::uint32_t value = 0;
        for (unsigned index = 0; index < count; ++index) {
            value = value * 2 +
                ((data_[offset_ >> 3U] >> (7U - (offset_ & 7U))) & 1U);
            ++offset_;
        }
        return value;
    }

    bool boolean() { return bits(1) != 0; }

    std::uint32_t ue() {
        unsigned zeros = 0;
        while (!boolean()) {
            if (++zeros > 31) throw std::runtime_error("invalid Exp-Golomb");
        }
        return ((std::uint32_t{1} << zeros) - 1U) +
            (zeros != 0 ? bits(zeros) : 0U);
    }

    Bytes bytes(const std::size_t length) {
        Bytes output(length);
        for (auto& value : output) value = static_cast<std::uint8_t>(bits(8));
        return output;
    }

    std::size_t offset() const noexcept { return offset_; }

private:
    const Bytes& data_;
    std::size_t offset_ = 0;
};

inline std::int64_t scaled(const std::int64_t value, const std::uint32_t from,
                           const std::uint32_t to) {
    return static_cast<std::int64_t>(std::llround(
        static_cast<double>(value) * static_cast<double>(to) /
        static_cast<double>(from)));
}

} // namespace tlvdemux::detail::mse
