#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <tlvdemux/types.hpp>

namespace tlvdemux {

enum class ApplicationCollectionState {
    Discovered,
    Collecting,
    Ready,
};

struct ApplicationState {
    ApplicationInfo application;
    ApplicationCollectionState state = ApplicationCollectionState::Discovered;
    std::size_t resource_count = 0;
    bool entry_ready = false;
};

struct ApplicationResource {
    std::uint32_t context_id = 0;
    std::uint16_t component_tag = 0;
    std::uint32_t transaction_id = 0;
    std::uint32_t download_id = 0;
    std::uint32_t mpu_sequence_number = 0;
    std::uint32_t item_id = 0;
    std::uint8_t version = 0;
    std::string path;
    std::string content_type;
    std::vector<std::uint8_t> data;
};

class ApplicationResourceSink {
public:
    virtual ~ApplicationResourceSink() = default;
    virtual void onApplicationState(const ApplicationState&) {}
    virtual void onApplicationResource(ApplicationResource&&) {}
    virtual void onApplicationResourcesReset() {}
    virtual void onApplicationResourceError(const Error&) {}
};

// Reassembles the directory, asset-management and data-unit signalling into
// browser-ready files. It is synchronous and does not create worker threads;
// callers may run the owning Demuxer on their own worker/thread.
class ApplicationResourceAssembler {
public:
    ApplicationResourceAssembler(ApplicationResourceSink&, Limits = {});
    ~ApplicationResourceAssembler();

    ApplicationResourceAssembler(ApplicationResourceAssembler&&) noexcept;
    ApplicationResourceAssembler& operator=(ApplicationResourceAssembler&&) noexcept;
    ApplicationResourceAssembler(const ApplicationResourceAssembler&) = delete;
    ApplicationResourceAssembler& operator=(const ApplicationResourceAssembler&) = delete;

    void onApplication(const ApplicationInfo&);
    void onDataDirectoryTable(const DataDirectoryTable&);
    void onDataAssetManagementTable(const DataAssetManagementTable&);
    void onDataUnit(const DataUnit&);
    void reset();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tlvdemux
