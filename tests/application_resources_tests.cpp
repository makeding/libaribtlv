#include <tlvdemux/application_resources.hpp>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <zlib.h>

namespace {

struct TestSink final : tlvdemux::ApplicationResourceSink {
    std::vector<tlvdemux::ApplicationState> states;
    std::vector<tlvdemux::ApplicationResource> resources;
    std::vector<tlvdemux::Error> errors;
    std::size_t resets = 0;

    void onApplicationState(const tlvdemux::ApplicationState& value) override {
        states.push_back(value);
    }
    void onApplicationResource(tlvdemux::ApplicationResource&& value) override {
        resources.push_back(std::move(value));
    }
    void onApplicationResourcesReset() override { ++resets; }
    void onApplicationResourceError(const tlvdemux::Error& value) override {
        errors.push_back(value);
    }
};

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void check(const bool condition, const std::string& message) {
    if (!condition) fail(message);
}

void append_u16(std::vector<std::uint8_t>& value, const std::uint32_t number) {
    value.push_back(static_cast<std::uint8_t>(number >> 8U));
    value.push_back(static_cast<std::uint8_t>(number));
}

void append_u32(std::vector<std::uint8_t>& value, const std::uint32_t number) {
    value.push_back(static_cast<std::uint8_t>(number >> 24U));
    value.push_back(static_cast<std::uint8_t>(number >> 16U));
    value.push_back(static_cast<std::uint8_t>(number >> 8U));
    value.push_back(static_cast<std::uint8_t>(number));
}

std::vector<std::uint8_t> index_item(const std::uint32_t item_id,
                                     const std::uint32_t stored_size,
                                     const std::uint8_t version,
                                     const std::string& filename,
                                     const std::string& content_type,
                                     const std::uint8_t compression_type = 0xff,
                                     const std::uint32_t original_size = 0) {
    std::vector<std::uint8_t> value;
    append_u16(value, 1);
    append_u32(value, item_id);
    append_u32(value, stored_size);
    value.push_back(version);
    value.push_back(static_cast<std::uint8_t>(filename.size()));
    value.insert(value.end(), filename.begin(), filename.end());
    value.push_back(0);
    value.push_back(static_cast<std::uint8_t>(content_type.size()));
    value.insert(value.end(), content_type.begin(), content_type.end());
    value.push_back(compression_type);
    if (compression_type != 0xff) append_u32(value, original_size);
    return value;
}

tlvdemux::DataDirectoryTable directory(const std::uint32_t context) {
    tlvdemux::DataDirectoryTable value;
    value.context_id = context;
    value.base_path = "/sh4/60/001";
    tlvdemux::DataDirectoryNode node;
    node.node_tag = 7;
    node.path = "top/source";
    value.directories.push_back(std::move(node));
    return value;
}

tlvdemux::DataAssetManagementTable management(const std::uint32_t context,
                                               const std::uint32_t transaction,
                                               const std::uint32_t download) {
    tlvdemux::DataAssetManagementTable value;
    value.context_id = context;
    value.transaction_id = transaction;
    value.component_tag = 0x40;
    value.download_id = download;
    tlvdemux::DataAssetMpu mpu;
    mpu.sequence_number = 9;
    mpu.index_item = true;
    mpu.index_item_id = 99;
    tlvdemux::DataAssetItem item;
    item.node_tag = 7;
    item.item_id = 5;
    mpu.items.push_back(std::move(item));
    value.mpus.push_back(std::move(mpu));
    return value;
}

tlvdemux::DataUnit unit(const std::uint32_t context, const std::uint32_t item,
                        std::vector<std::uint8_t> bytes) {
    tlvdemux::DataUnit value;
    value.context_id = context;
    value.component_tag = 0x40;
    value.mpu_sequence_number = 9;
    value.item_id = item;
    value.data = std::move(bytes);
    return value;
}

tlvdemux::ApplicationInfo application(const std::uint32_t context) {
    tlvdemux::ApplicationInfo value;
    value.context_id = context;
    value.application_type = 1;
    value.organization_id = 2;
    value.application_id = 3;
    value.version = 1;
    value.entry_path = "top/source/index.html";
    value.transport_urls.push_back("sh4/60/001/");
    return value;
}

void test_out_of_order_collection_and_update() {
    TestSink sink;
    tlvdemux::ApplicationResourceAssembler assembler(sink);
    const std::vector<std::uint8_t> first{'o', 'n', 'e'};

    assembler.onApplication(application(1));
    assembler.onDataUnit(unit(1, 5, first));
    assembler.onDataUnit(unit(1, 99, index_item(5, first.size(), 1,
        "index.html", "text/html")));
    assembler.onDataAssetManagementTable(management(1, 10, 20));
    check(sink.resources.empty(), "resource was published before its directory arrived");
    assembler.onDataDirectoryTable(directory(1));

    check(sink.resources.size() == 1, "out-of-order resource was not published");
    check(sink.resources[0].path == "sh4/60/001/top/source/index.html",
          "resource path was not normalized");
    check(sink.resources[0].data == first, "resource bytes changed during assembly");
    check(!sink.states.empty() && sink.states.back().entry_ready &&
              sink.states.back().state == tlvdemux::ApplicationCollectionState::Ready,
          "entry resource did not make the application ready");

    assembler.onDataUnit(unit(1, 5, first));
    check(sink.resources.size() == 1, "carousel duplicate was emitted twice");

    const std::vector<std::uint8_t> second{'t', 'w', 'o'};
    assembler.onDataAssetManagementTable(management(1, 11, 21));
    assembler.onDataUnit(unit(1, 99, index_item(5, second.size(), 2,
        "index.html", "text/html")));
    assembler.onDataUnit(unit(1, 5, second));
    check(sink.resources.size() == 2 && sink.resources.back().version == 2 &&
              sink.resources.back().data == second,
          "new application resource version was not published");

    assembler.reset();
    check(sink.resets == 1, "reset was not propagated to the resource store");
}

void test_virtual_resource_store() {
    tlvdemux::ApplicationResourceStore store;
    tlvdemux::ApplicationResource resource;
    resource.context_id = 4;
    resource.path = "sh4/60/001/top/source/index.html";
    resource.content_type = "text/html";
    resource.version = 3;
    resource.data = {'o', 'k'};
    store.onApplicationResource(std::move(resource));

    const auto found = store.get(4, "/sh4/60/001/top/source/index.html");
    check(found && found->data == std::vector<std::uint8_t>({'o', 'k'}),
          "virtual resource lookup failed");
    const auto files = store.list(4);
    check(files.size() == 1 && files[0].size == 2 && files[0].version == 3,
          "virtual resource listing omitted metadata");
    check(store.waitFor(4, files[0].path, std::chrono::milliseconds(1)) != nullptr,
          "waitFor did not return an available resource");
    check(store.get(4, "../escape") == nullptr,
          "virtual resource store accepted path traversal");
    store.clear();
    check(store.list().empty(), "virtual resource store did not clear its session");
}

void test_compression_and_limit() {
    const std::vector<std::uint8_t> source{'c', 'o', 'm', 'p', 'r', 'e', 's', 's'};
    std::vector<std::uint8_t> compressed(compressBound(source.size()));
    auto compressed_size = static_cast<uLongf>(compressed.size());
    check(compress(compressed.data(), &compressed_size, source.data(), source.size()) == Z_OK,
          "test fixture compression failed");
    compressed.resize(compressed_size);

    TestSink sink;
    tlvdemux::ApplicationResourceAssembler assembler(sink);
    assembler.onDataDirectoryTable(directory(2));
    assembler.onDataAssetManagementTable(management(2, 1, 1));
    assembler.onDataUnit(unit(2, 99, index_item(5, compressed.size(), 1,
        "index.html", "text/html", 0, source.size())));
    assembler.onDataUnit(unit(2, 5, compressed));
    check(sink.resources.size() == 1 && sink.resources[0].data == source,
          "zlib application resource was not decompressed");

    tlvdemux::Limits limits;
    limits.max_application_resource = 3;
    TestSink limited_sink;
    tlvdemux::ApplicationResourceAssembler limited(limited_sink, limits);
    limited.onDataDirectoryTable(directory(3));
    limited.onDataAssetManagementTable(management(3, 1, 1));
    limited.onDataUnit(unit(3, 99, index_item(5, source.size(), 1,
        "index.html", "text/html")));
    limited.onDataUnit(unit(3, 5, source));
    check(limited_sink.resources.empty() && !limited_sink.errors.empty() &&
              limited_sink.errors.back().code == tlvdemux::ErrorCode::ResourceLimit,
          "resource size limit was not enforced");
}

} // namespace

int main() {
    test_out_of_order_collection_and_update();
    test_virtual_resource_store();
    test_compression_and_limit();
    std::cout << "application resource tests passed\n";
    return 0;
}
