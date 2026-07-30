#include <tlvdemux/demuxer.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <zlib.h>

namespace {

std::uint16_t be16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8U) | data[1]);
}

std::uint32_t be32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
           (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) | data[3];
}

struct Cursor {
    const std::vector<std::uint8_t>& data;
    std::size_t at = 0;

    bool u8(std::uint8_t& value) {
        if (at >= data.size()) return false;
        value = data[at++];
        return true;
    }
    bool u16(std::uint16_t& value) {
        if (data.size() - at < 2) return false;
        value = be16(data.data() + at);
        at += 2;
        return true;
    }
    bool u32(std::uint32_t& value) {
        if (data.size() - at < 4) return false;
        value = be32(data.data() + at);
        at += 4;
        return true;
    }
    bool text(const std::size_t size, std::string& value) {
        if (size > data.size() - at) return false;
        value.assign(reinterpret_cast<const char*>(data.data() + at), size);
        at += size;
        return true;
    }
    bool skip(const std::size_t size) {
        if (size > data.size() - at) return false;
        at += size;
        return true;
    }
};

struct MpuKey {
    std::uint32_t context = 0;
    std::uint16_t component = 0;
    std::uint32_t sequence = 0;
    bool operator<(const MpuKey& other) const {
        return std::tie(context, component, sequence) <
               std::tie(other.context, other.component, other.sequence);
    }
};

struct ItemKey {
    MpuKey mpu;
    std::uint32_t item = 0;
    bool operator<(const ItemKey& other) const {
        return std::tie(mpu.context, mpu.component, mpu.sequence, item) <
               std::tie(other.mpu.context, other.mpu.component, other.mpu.sequence, other.item);
    }
};

struct MpuMap {
    std::uint32_t index_item_id = 0;
    std::vector<std::uint16_t> node_tags;
    std::optional<std::uint16_t> mpu_node_tag;
};

struct FileRecord {
    std::uint32_t item_id = 0;
    std::uint32_t item_size = 0;
    std::uint8_t version = 0;
    std::string filename;
    std::string content_type;
    std::uint8_t compression_type = 0xff;
    std::optional<std::uint32_t> original_size;
};

struct FileTarget {
    std::filesystem::path path;
    std::uint8_t compression_type = 0xff;
    std::optional<std::uint32_t> original_size;
};

struct Extractor final : tlvdemux::Sink {
    explicit Extractor(std::filesystem::path root) : root(std::move(root)) {}

    std::filesystem::path root;
    std::map<std::pair<std::uint32_t, std::uint16_t>, std::filesystem::path> node_paths;
    std::map<MpuKey, MpuMap> mpu_maps;
    std::map<ItemKey, FileTarget> item_paths;
    std::map<ItemKey, tlvdemux::DataUnit> pending;
    std::set<ItemKey> diagnosed;
    std::set<ItemKey> completed;
    std::size_t written = 0;

    void onService(const tlvdemux::ServiceInfo&) override {}
    void onTrack(const tlvdemux::TrackInfo&) override {}
    void onAccessUnit(tlvdemux::AccessUnit&&) override {}

    void onDataDirectoryTable(const tlvdemux::DataDirectoryTable& table) override {
        for (const auto& directory : table.directories) {
            const auto relative = safe_relative(table.base_path) / safe_relative(directory.path);
            node_paths[{table.context_id, directory.node_tag}] = relative;
        }
        retry();
    }

    void onDataAssetManagementTable(const tlvdemux::DataAssetManagementTable& table) override {
        for (const auto& mpu : table.mpus) {
            MpuMap map;
            map.index_item_id = mpu.index_item_id.value_or(0);
            for (const auto& item : mpu.items) map.node_tags.push_back(item.node_tag);
            for (std::size_t at = 0; at + 5 <= mpu.info.size();) {
                const auto length = static_cast<std::size_t>(mpu.info[at + 2]);
                if (length > mpu.info.size() - at - 3) break;
                if (length == 2) map.mpu_node_tag = be16(mpu.info.data() + at + 3);
                at += 3 + length;
            }
            mpu_maps[{table.context_id, table.component_tag, mpu.sequence_number}] =
                std::move(map);
        }
        retry();
    }

    void onDataUnit(tlvdemux::DataUnit&& unit) override {
        ItemKey key{{unit.context_id, unit.component_tag, unit.mpu_sequence_number}, unit.item_id};
        if (completed.find(key) != completed.end()) return;
        pending[key] = std::move(unit);
        retry();
    }

    void onError(const tlvdemux::Error& error) override {
        if (!error.recoverable) {
            throw std::runtime_error(error.message);
        }
    }

private:
    static std::filesystem::path safe_relative(const std::string& value) {
        std::filesystem::path path(value);
        if (path.is_absolute()) throw std::runtime_error("broadcast path is absolute");
        std::filesystem::path result;
        for (const auto& part : path) {
            if (part == "..") throw std::runtime_error("broadcast path escapes output directory");
            if (part != "." && !part.empty()) result /= part;
        }
        return result;
    }

    static bool parse_index(const std::vector<std::uint8_t>& data,
                            std::vector<FileRecord>& records) {
        Cursor cursor{data};
        std::uint16_t count = 0;
        if (!cursor.u16(count)) return false;
        records.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            FileRecord record;
            std::uint8_t filename_size = 0;
            std::uint8_t checksum = 0;
            std::uint8_t type_size = 0;
            if (!cursor.u32(record.item_id) || !cursor.u32(record.item_size) ||
                !cursor.u8(record.version) || !cursor.u8(filename_size) ||
                !cursor.text(filename_size, record.filename) || !cursor.u8(checksum)) {
                return false;
            }
            if ((checksum & 0x80U) != 0 && !cursor.skip(4)) return false;
            if (!cursor.u8(type_size) || !cursor.text(type_size, record.content_type) ||
                !cursor.u8(record.compression_type)) {
                return false;
            }
            if (record.compression_type != 0xff) {
                std::uint32_t original_size = 0;
                if (!cursor.u32(original_size)) return false;
                record.original_size = original_size;
            }
            records.push_back(std::move(record));
        }
        return cursor.at == data.size();
    }

    void retry() {
        bool progressed = true;
        while (progressed) {
            progressed = false;
            for (auto it = pending.begin(); it != pending.end();) {
                const auto map_it = mpu_maps.find(it->first.mpu);
                if (map_it == mpu_maps.end()) {
                    ++it;
                    continue;
                }
                if (it->first.item == map_it->second.index_item_id) {
                    std::vector<FileRecord> records;
                    if (!parse_index(it->second.data, records)) {
                        if (diagnosed.insert(it->first).second) {
                            std::cerr << "cannot parse index component=0x" << std::hex
                                      << it->first.mpu.component << std::dec
                                      << " mpu=" << it->first.mpu.sequence
                                      << " bytes=" << it->second.data.size() << " data=";
                            for (const auto byte : it->second.data) {
                                std::cerr << std::hex << static_cast<unsigned>(byte) << ',';
                            }
                            std::cerr << std::dec << '\n';
                        }
                        ++it;
                        continue;
                    }
                    auto node_tags = map_it->second.node_tags;
                    if (node_tags.empty() && map_it->second.mpu_node_tag.has_value()) {
                        node_tags.assign(records.size(), *map_it->second.mpu_node_tag);
                    }
                    if (records.size() != node_tags.size()) {
                        if (diagnosed.insert(it->first).second) {
                            std::cerr << "index mapping count mismatch component=0x" << std::hex
                                  << it->first.mpu.component << std::dec
                                  << " mpu=" << it->first.mpu.sequence
                                  << " records=" << records.size()
                                  << " nodes=" << map_it->second.node_tags.size();
                        for (const auto& record : records) {
                            std::cerr << " file=" << record.filename;
                        }
                            std::cerr << '\n';
                        }
                        ++it;
                        continue;
                    }
                    bool have_paths = true;
                    for (const auto node : node_tags) {
                        if (node_paths.find({it->first.mpu.context, node}) == node_paths.end()) {
                            have_paths = false;
                            break;
                        }
                    }
                    if (!have_paths) {
                        ++it;
                        continue;
                    }
                    for (std::size_t index = 0; index < records.size(); ++index) {
                        const auto directory = node_paths.at(
                            {it->first.mpu.context, node_tags[index]});
                        item_paths[{it->first.mpu, records[index].item_id}] = FileTarget{
                            directory / safe_relative(records[index].filename),
                            records[index].compression_type, records[index].original_size};
                    }
                    it = pending.erase(it);
                    progressed = true;
                    continue;
                }
                const auto path_it = item_paths.find(it->first);
                if (path_it == item_paths.end()) {
                    ++it;
                    continue;
                }
                const auto output = root / path_it->second.path;
                std::filesystem::create_directories(output.parent_path());
                const std::vector<std::uint8_t>* bytes = &it->second.data;
                std::vector<std::uint8_t> decompressed;
                if (path_it->second.compression_type != 0xff) {
                    if (path_it->second.compression_type != 0 ||
                        !path_it->second.original_size.has_value()) {
                        throw std::runtime_error("unsupported broadcast compression type");
                    }
                    decompressed.resize(*path_it->second.original_size);
                    auto output_size = static_cast<uLongf>(decompressed.size());
                    const auto status = uncompress(
                        decompressed.data(), &output_size, it->second.data.data(),
                        static_cast<uLong>(it->second.data.size()));
                    if (status != Z_OK || output_size != decompressed.size()) {
                        throw std::runtime_error("cannot decompress broadcast item: " +
                                                 output.string());
                    }
                    bytes = &decompressed;
                }
                std::ofstream file(output, std::ios::binary);
                if (!file) throw std::runtime_error("cannot create output: " + output.string());
                file.write(reinterpret_cast<const char*>(bytes->data()),
                           static_cast<std::streamsize>(bytes->size()));
                if (!file) throw std::runtime_error("cannot write output: " + output.string());
                ++written;
                completed.insert(it->first);
                it = pending.erase(it);
                progressed = true;
            }
        }
    }
};

void usage() {
    std::cerr << "usage: tlvdemux-extract OUTPUT-DIR INPUT\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3) {
            usage();
            return 2;
        }
        Extractor extractor(argv[1]);
        std::ifstream input(argv[2], std::ios::binary);
        if (!input) throw std::runtime_error("cannot open input");
        tlvdemux::Demuxer demuxer(extractor);
        std::array<std::uint8_t, 64 * 1024> buffer{};
        while (input) {
            input.read(reinterpret_cast<char*>(buffer.data()),
                       static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count > 0) demuxer.push(buffer.data(), static_cast<std::size_t>(count));
        }
        demuxer.flush();
        std::cerr << "extracted " << extractor.written << " files\n";
        return extractor.written == 0 ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "tlvdemux-extract: " << error.what() << '\n';
        return 2;
    }
}
