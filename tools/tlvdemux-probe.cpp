#include <tlvdemux/duration_probe.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

const char* failure_name(const tlvdemux::DurationProbeFailure failure) noexcept {
    switch (failure) {
    case tlvdemux::DurationProbeFailure::None: return "none";
    case tlvdemux::DurationProbeFailure::InvalidSource: return "invalid-source";
    case tlvdemux::DurationProbeFailure::InvalidResponse: return "invalid-response";
    case tlvdemux::DurationProbeFailure::SourceError: return "source-error";
    case tlvdemux::DurationProbeFailure::NoVideo: return "no-video";
    case tlvdemux::DurationProbeFailure::NoTailTimestamp: return "no-tail-timestamp";
    case tlvdemux::DurationProbeFailure::RangeLimit: return "range-limit";
    case tlvdemux::DurationProbeFailure::ParseError: return "parse-error";
    }
    return "unknown";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            std::cerr << "usage: tlvdemux-probe INPUT\n";
            return 2;
        }
        const std::string path = argv[1];
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) throw std::runtime_error("cannot open input: " + path);
        const auto end = input.tellg();
        if (end <= 0) throw std::runtime_error("input is empty or its size is unavailable");
        const auto unsigned_end = static_cast<std::uint64_t>(end);
        if (unsigned_end > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
            throw std::runtime_error("input is too large for this file adapter");
        }

        tlvdemux::DurationProbe probe;
        if (!probe.begin(unsigned_end)) throw std::runtime_error("cannot start duration probe");
        std::array<std::uint8_t, 512 * 1024> buffer{};
        while (const auto request = probe.nextRange()) {
            input.clear();
            input.seekg(static_cast<std::streamoff>(request->offset), std::ios::beg);
            if (!input) {
                probe.failRange(request->request_id);
                break;
            }
            std::uint64_t remaining = request->length;
            std::uint64_t offset = request->offset;
            while (remaining != 0) {
                const auto wanted = static_cast<std::streamsize>(
                    std::min<std::uint64_t>(remaining, buffer.size()));
                input.read(reinterpret_cast<char*>(buffer.data()), wanted);
                const auto count = input.gcount();
                if (count <= 0) {
                    probe.failRange(request->request_id);
                    break;
                }
                const auto size = static_cast<std::size_t>(count);
                remaining -= static_cast<std::uint64_t>(size);
                const bool complete = remaining == 0;
                if (!probe.pushRange(request->request_id, offset, buffer.data(), size, complete)) {
                    break;
                }
                offset += static_cast<std::uint64_t>(size);
            }
        }

        if (probe.state() != tlvdemux::DurationProbeState::Complete) {
            std::cerr << "duration=unknown failure=" << failure_name(probe.failure())
                      << " bytes-read=" << probe.transferredBytes() << '\n';
            return 1;
        }
        const auto duration = probe.duration();
        const auto seconds = static_cast<long double>(duration.value.value) /
            static_cast<long double>(duration.value.timescale);
        std::cout << "duration=" << std::fixed << std::setprecision(6) << seconds
                  << " bytes-read=" << probe.transferredBytes() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "tlvdemux-probe: " << error.what() << '\n';
        return 2;
    }
}
