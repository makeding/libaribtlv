#include <tlvdemux/demuxer.hpp>

#include <zlib.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using Identity = std::tuple<std::uint32_t, std::uint16_t, std::uint32_t, std::uint32_t>;

struct ResourceRecord {
    Identity identity;
    std::string path;
    std::string content_type;
    std::vector<std::uint8_t> decoded;
    std::optional<std::vector<std::uint8_t>> wire;
    std::uint64_t first_offset = 0;
    std::size_t occurrences = 0;
    std::size_t duplicates = 0;
    std::uint64_t duplicate_payload_bytes = 0;
};

Identity identity(const tlvdemux::ApplicationResource& value) {
    return {value.context_id, value.component_tag, value.mpu_sequence_number, value.item_id};
}

Identity identity(const tlvdemux::DataUnit& value) {
    return {value.context_id, value.component_tag, value.mpu_sequence_number, value.item_id};
}

std::uint32_t checksum(const std::vector<std::uint8_t>& data) {
    auto value = crc32(0L, Z_NULL, 0);
    value = crc32(value, data.data(), static_cast<uInt>(data.size()));
    return static_cast<std::uint32_t>(value);
}

struct Analyzer final : tlvdemux::Sink {
    std::vector<ResourceRecord> resources;
    std::map<Identity, std::size_t> active;
    std::size_t data_units = 0;
    std::size_t duplicate_units = 0;
    std::size_t unknown_units = 0;
    std::size_t discontinuities = 0;
    std::size_t wire_changes = 0;
    std::size_t recoverable_errors = 0;

    void onService(const tlvdemux::ServiceInfo&) override {}
    void onTrack(const tlvdemux::TrackInfo&) override {}
    void onAccessUnit(tlvdemux::AccessUnit&&) override {}

    void onApplicationResource(tlvdemux::ApplicationResource&& value) override {
        ResourceRecord record;
        record.identity = identity(value);
        record.path = std::move(value.path);
        record.content_type = std::move(value.content_type);
        record.decoded = std::move(value.data);
        const auto index = resources.size();
        resources.push_back(std::move(record));
        active[resources.back().identity] = index;
    }

    void onApplicationResourceRemoved(
        const tlvdemux::ApplicationResourceRemoval& value) override {
        active.erase(Identity{value.context_id, value.component_tag,
                              value.mpu_sequence_number, value.item_id});
    }

    void onApplicationResourcesReset() override { active.clear(); }

    void onDataUnit(tlvdemux::DataUnit&& value) override {
        ++data_units;
        if (value.discontinuity) {
            ++discontinuities;
            active.clear();
            return;
        }
        const auto found = active.find(identity(value));
        if (found == active.end()) {
            ++unknown_units;
            return;
        }
        auto& record = resources.at(found->second);
        ++record.occurrences;
        if (!record.wire.has_value()) {
            record.wire = value.data;
            record.first_offset = value.input_offset;
            return;
        }
        if (*record.wire != value.data) {
            ++wire_changes;
            return;
        }
        ++record.duplicates;
        ++duplicate_units;
        record.duplicate_payload_bytes += value.data.size();
    }

    void onError(const tlvdemux::Error& error) override {
        if (!error.recoverable) throw std::runtime_error(error.message);
        ++recoverable_errors;
    }
};

void usage() { std::cerr << "usage: tlvanalyze INPUT\n"; }

} // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 2) {
            usage();
            return 2;
        }
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) throw std::runtime_error("cannot open input");
        Analyzer analyzer;
        tlvdemux::Demuxer demuxer(analyzer);
        std::array<std::uint8_t, 1024U * 1024U> buffer{};
        std::uint64_t input_bytes = 0;
        while (input) {
            input.read(reinterpret_cast<char*>(buffer.data()),
                       static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count <= 0) break;
            const auto size = static_cast<std::size_t>(count);
            demuxer.push(buffer.data(), size);
            input_bytes += static_cast<std::uint64_t>(size);
        }
        if (!input.eof()) throw std::runtime_error("failed while reading input");
        demuxer.flush();

        std::uint64_t decoded_bytes = 0;
        std::uint64_t duplicate_payload_bytes = 0;
        for (const auto& resource : analyzer.resources) {
            decoded_bytes += resource.decoded.size();
            duplicate_payload_bytes += resource.duplicate_payload_bytes;
            const auto& [context, component, mpu, item] = resource.identity;
            std::cout << "resource context=" << context
                      << " component=0x" << std::hex << component << std::dec
                      << " mpu=" << mpu << " item=" << item
                      << " size=" << resource.decoded.size()
                      << " crc32=" << std::hex << std::setw(8) << std::setfill('0')
                      << checksum(resource.decoded) << std::dec << std::setfill(' ')
                      << " occurrences=" << resource.occurrences
                      << " duplicates=" << resource.duplicates
                      << " duplicate-payload-bytes=" << resource.duplicate_payload_bytes
                      << " first-offset=" << resource.first_offset
                      << " type=" << std::quoted(resource.content_type)
                      << " path=" << std::quoted(resource.path) << '\n';
        }
        std::cout << "summary input-bytes=" << input_bytes
                  << " resources=" << analyzer.resources.size()
                  << " decoded-bytes=" << decoded_bytes
                  << " data-units=" << analyzer.data_units
                  << " duplicate-units=" << analyzer.duplicate_units
                  << " duplicate-payload-bytes=" << duplicate_payload_bytes
                  << " unknown-units=" << analyzer.unknown_units
                  << " discontinuities=" << analyzer.discontinuities
                  << " wire-changes=" << analyzer.wire_changes
                  << " recoverable-errors=" << analyzer.recoverable_errors << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "tlvanalyze: " << error.what() << '\n';
        return 1;
    }
}
