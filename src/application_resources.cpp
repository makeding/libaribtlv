#include <tlvdemux/application_resources.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <utility>

#include <zlib.h>

namespace tlvdemux {
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
    std::uint32_t transaction_id = 0;
    std::uint32_t download_id = 0;
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
    std::string path;
    std::string content_type;
    std::uint32_t item_size = 0;
    std::uint8_t version = 0;
    std::uint8_t compression_type = 0xff;
    std::optional<std::uint32_t> original_size;
};

struct PublishedItem {
    std::uint32_t transaction_id = 0;
    std::uint32_t download_id = 0;
    std::uint8_t version = 0;
    std::string path;
};

std::optional<std::string> safe_path(const std::string& value) {
    if (value.empty()) return std::string{};
    std::string normalized;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find('/', begin);
        const auto part = value.substr(begin, end == std::string::npos
            ? std::string::npos : end - begin);
        if (part == "..") return std::nullopt;
        if (!part.empty() && part != ".") {
            if (!normalized.empty()) normalized.push_back('/');
            normalized += part;
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return normalized;
}

std::optional<std::string> join_path(const std::string& first, const std::string& second,
                                     const std::string& third = {}) {
    const auto a = safe_path(first);
    const auto b = safe_path(second);
    const auto c = safe_path(third);
    if (!a.has_value() || !b.has_value() || !c.has_value()) return std::nullopt;
    std::string result;
    for (const auto* part : {&*a, &*b, &*c}) {
        if (part->empty()) continue;
        if (!result.empty()) result.push_back('/');
        result += *part;
    }
    return result;
}

bool parse_index(const std::vector<std::uint8_t>& data, std::vector<FileRecord>& records) {
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

std::string application_key(const ApplicationInfo& info) {
    return std::to_string(info.context_id) + ':' + std::to_string(info.application_type) + ':' +
        std::to_string(info.organization_id) + ':' + std::to_string(info.application_id);
}

} // namespace

class ApplicationResourceAssembler::Impl {
public:
    Impl(ApplicationResourceSink& sink, Limits limits)
        : sink_(sink), limits_(std::move(limits)) {}

    void application(const ApplicationInfo& info) {
        if (!limits_.collect_application_resources) return;
        auto& state = applications_[application_key(info)];
        state.application = info;
        state.state = ApplicationCollectionState::Collecting;
        state.resource_count = resource_count(info.context_id);
        state.entry_ready = entry_ready(info);
        if (state.entry_ready) state.state = ApplicationCollectionState::Ready;
        sink_.onApplicationState(state);
    }

    void directory(const DataDirectoryTable& table) {
        if (!limits_.collect_application_resources) return;
        for (const auto& directory : table.directories) {
            const auto path = join_path(table.base_path, directory.path);
            if (!path.has_value()) {
                report(ErrorCode::MalformedInput, 0, true,
                       "broadcast directory path escapes its virtual root");
                continue;
            }
            node_paths_[{table.context_id, directory.node_tag}] = *path;
        }
        retry();
    }

    void management(const DataAssetManagementTable& table) {
        if (!limits_.collect_application_resources) return;
        for (const auto& mpu : table.mpus) {
            const MpuKey key{table.context_id, table.component_tag, mpu.sequence_number};
            MpuMap map;
            map.transaction_id = table.transaction_id;
            map.download_id = table.download_id;
            map.index_item_id = mpu.index_item_id.value_or(0);
            for (const auto& item : mpu.items) map.node_tags.push_back(item.node_tag);
            for (std::size_t at = 0; at + 5 <= mpu.info.size();) {
                const auto length = static_cast<std::size_t>(mpu.info[at + 2]);
                if (length > mpu.info.size() - at - 3) break;
                if (length == 2) map.mpu_node_tag = be16(mpu.info.data() + at + 3);
                at += 3 + length;
            }
            const auto previous = mpu_maps_.find(key);
            if (previous != mpu_maps_.end() &&
                (previous->second.transaction_id != map.transaction_id ||
                 previous->second.download_id != map.download_id)) {
                erase_mpu_targets(key);
            }
            mpu_maps_[key] = std::move(map);
        }
        retry();
    }

    void unit(const DataUnit& value) {
        if (!limits_.collect_application_resources) return;
        const ItemKey key{{value.context_id, value.component_tag, value.mpu_sequence_number},
                          value.item_id};
        if (try_consume(key, value)) return;

        const auto existing = pending_.find(key);
        const auto previous_size = existing == pending_.end() ? 0U : existing->second.data.size();
        if (pending_.size() + (existing == pending_.end() ? 1U : 0U) >
                limits_.max_application_pending_units ||
            value.data.size() > limits_.max_application_pending_bytes -
                std::min(pending_bytes_, limits_.max_application_pending_bytes) + previous_size) {
            report(ErrorCode::ResourceLimit, value.input_offset, true,
                   "application resource pending-data limit exceeded");
            return;
        }
        pending_bytes_ -= previous_size;
        pending_[key] = value;
        pending_bytes_ += value.data.size();
        retry();
    }

    void reset() {
        node_paths_.clear();
        mpu_maps_.clear();
        item_paths_.clear();
        pending_.clear();
        published_.clear();
        published_paths_.clear();
        applications_.clear();
        pending_bytes_ = 0;
        sink_.onApplicationResourcesReset();
    }

private:
    void retry() {
        bool progressed = true;
        while (progressed) {
            progressed = false;
            for (auto it = pending_.begin(); it != pending_.end();) {
                if (!try_consume(it->first, it->second)) {
                    ++it;
                    continue;
                }
                pending_bytes_ -= it->second.data.size();
                it = pending_.erase(it);
                progressed = true;
            }
        }
    }

    bool try_consume(const ItemKey& key, const DataUnit& unit) {
        const auto map_it = mpu_maps_.find(key.mpu);
        if (map_it == mpu_maps_.end()) return false;
        if (key.item == map_it->second.index_item_id) {
            return consume_index(key.mpu, unit, map_it->second);
        }
        const auto target = item_paths_.find(key);
        if (target == item_paths_.end()) return false;
        return publish(key, unit, map_it->second, target->second);
    }

    bool consume_index(const MpuKey& key, const DataUnit& unit, const MpuMap& map) {
        std::vector<FileRecord> records;
        if (!parse_index(unit.data, records)) {
            report(ErrorCode::MalformedInput, unit.input_offset, true,
                   "cannot parse application resource index item");
            return true;
        }
        auto node_tags = map.node_tags;
        if (node_tags.empty() && map.mpu_node_tag.has_value()) {
            node_tags.assign(records.size(), *map.mpu_node_tag);
        }
        if (records.size() != node_tags.size()) {
            report(ErrorCode::MalformedInput, unit.input_offset, true,
                   "application resource index mapping count mismatch");
            return true;
        }
        for (const auto node : node_tags) {
            if (node_paths_.find({key.context, node}) == node_paths_.end()) return false;
        }
        if (item_paths_.size() + records.size() > limits_.max_application_resources) {
            report(ErrorCode::ResourceLimit, unit.input_offset, true,
                   "application resource catalogue limit exceeded");
            return true;
        }
        for (std::size_t index = 0; index < records.size(); ++index) {
            const auto directory = node_paths_.at({key.context, node_tags[index]});
            const auto path = join_path(directory, records[index].filename);
            if (!path.has_value()) {
                report(ErrorCode::MalformedInput, unit.input_offset, true,
                       "broadcast file path escapes its virtual root");
                continue;
            }
            item_paths_[{key, records[index].item_id}] = FileTarget{
                *path, records[index].content_type, records[index].item_size,
                records[index].version, records[index].compression_type,
                records[index].original_size};
        }
        return true;
    }

    bool publish(const ItemKey& key, const DataUnit& unit, const MpuMap& map,
                 const FileTarget& target) {
        const PublishedItem signature{map.transaction_id, map.download_id,
                                      target.version, target.path};
        const auto emitted = published_.find(key);
        if (emitted != published_.end() &&
            emitted->second.transaction_id == signature.transaction_id &&
            emitted->second.download_id == signature.download_id &&
            emitted->second.version == signature.version &&
            emitted->second.path == signature.path) {
            return true;
        }
        if (unit.data.size() != target.item_size) {
            report(ErrorCode::MalformedInput, unit.input_offset, true,
                   "application resource size does not match its index");
            return true;
        }

        std::vector<std::uint8_t> bytes;
        if (target.compression_type == 0xff) {
            if (unit.data.size() > limits_.max_application_resource) {
                report(ErrorCode::ResourceLimit, unit.input_offset, true,
                       "application resource exceeds size limit");
                return true;
            }
            bytes = unit.data;
        } else {
            if (target.compression_type != 0 || !target.original_size.has_value()) {
                report(ErrorCode::UnsupportedFeature, unit.input_offset, true,
                       "unsupported application resource compression type");
                return true;
            }
            if (*target.original_size > limits_.max_application_resource) {
                report(ErrorCode::ResourceLimit, unit.input_offset, true,
                       "decompressed application resource exceeds size limit");
                return true;
            }
            bytes.resize(*target.original_size);
            auto output_size = static_cast<uLongf>(bytes.size());
            const auto status = uncompress(bytes.data(), &output_size, unit.data.data(),
                                           static_cast<uLong>(unit.data.size()));
            if (status != Z_OK || output_size != bytes.size()) {
                report(ErrorCode::MalformedInput, unit.input_offset, true,
                       "cannot decompress application resource");
                return true;
            }
        }

        published_[key] = signature;
        published_paths_[{key.mpu.context, target.path}] = target.version;
        ApplicationResource resource;
        resource.context_id = key.mpu.context;
        resource.component_tag = key.mpu.component;
        resource.transaction_id = map.transaction_id;
        resource.download_id = map.download_id;
        resource.mpu_sequence_number = key.mpu.sequence;
        resource.item_id = key.item;
        resource.version = target.version;
        resource.path = target.path;
        resource.content_type = target.content_type;
        resource.data = std::move(bytes);
        sink_.onApplicationResource(std::move(resource));
        update_application_states(key.mpu.context);
        return true;
    }

    bool entry_ready(const ApplicationInfo& info) const {
        const auto entry = safe_path(info.entry_path);
        return entry.has_value() &&
            published_paths_.find({info.context_id, *entry}) != published_paths_.end();
    }

    std::size_t resource_count(const std::uint32_t context) const {
        return static_cast<std::size_t>(std::count_if(
            published_paths_.begin(), published_paths_.end(), [context](const auto& item) {
                return item.first.first == context;
            }));
    }

    void update_application_states(const std::uint32_t context) {
        for (auto& item : applications_) {
            auto& state = item.second;
            if (state.application.context_id != context) continue;
            const auto ready = entry_ready(state.application);
            const auto count = resource_count(context);
            if (state.entry_ready == ready && state.resource_count == count) continue;
            state.entry_ready = ready;
            state.resource_count = count;
            state.state = ready ? ApplicationCollectionState::Ready
                                : ApplicationCollectionState::Collecting;
            sink_.onApplicationState(state);
        }
    }

    void erase_mpu_targets(const MpuKey& key) {
        for (auto it = item_paths_.begin(); it != item_paths_.end();) {
            if (!(it->first.mpu < key) && !(key < it->first.mpu)) it = item_paths_.erase(it);
            else ++it;
        }
        for (auto it = published_.begin(); it != published_.end();) {
            if (!(it->first.mpu < key) && !(key < it->first.mpu)) it = published_.erase(it);
            else ++it;
        }
    }

    void report(const ErrorCode code, const std::uint64_t offset, const bool recoverable,
                std::string message) {
        sink_.onApplicationResourceError(Error{code, offset, recoverable, std::move(message)});
    }

    ApplicationResourceSink& sink_;
    Limits limits_;
    std::map<std::pair<std::uint32_t, std::uint16_t>, std::string> node_paths_;
    std::map<MpuKey, MpuMap> mpu_maps_;
    std::map<ItemKey, FileTarget> item_paths_;
    std::map<ItemKey, DataUnit> pending_;
    std::map<ItemKey, PublishedItem> published_;
    std::map<std::pair<std::uint32_t, std::string>, std::uint8_t> published_paths_;
    std::unordered_map<std::string, ApplicationState> applications_;
    std::size_t pending_bytes_ = 0;
};

ApplicationResourceAssembler::ApplicationResourceAssembler(ApplicationResourceSink& sink,
                                                           Limits limits)
    : impl_(std::make_unique<Impl>(sink, std::move(limits))) {}

ApplicationResourceAssembler::~ApplicationResourceAssembler() = default;
ApplicationResourceAssembler::ApplicationResourceAssembler(ApplicationResourceAssembler&&) noexcept =
    default;
ApplicationResourceAssembler& ApplicationResourceAssembler::operator=(
    ApplicationResourceAssembler&&) noexcept = default;

void ApplicationResourceAssembler::onApplication(const ApplicationInfo& value) {
    impl_->application(value);
}
void ApplicationResourceAssembler::onDataDirectoryTable(const DataDirectoryTable& value) {
    impl_->directory(value);
}
void ApplicationResourceAssembler::onDataAssetManagementTable(
    const DataAssetManagementTable& value) {
    impl_->management(value);
}
void ApplicationResourceAssembler::onDataUnit(const DataUnit& value) {
    impl_->unit(value);
}
void ApplicationResourceAssembler::reset() { impl_->reset(); }

} // namespace tlvdemux
