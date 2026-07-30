#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include <tlvdemux/types.hpp>

namespace tlvdemux {

struct RepositionOptions {
    std::uint64_t input_offset = 0;
    bool preserve_timeline = true;
};

class Sink {
public:
    virtual ~Sink() = default;
    virtual void onService(const ServiceInfo&) = 0;
    virtual void onTrack(const TrackInfo&) = 0;
    virtual void onAccessUnit(AccessUnit&&) = 0;
    virtual void onError(const Error&) = 0;
    virtual void onApplicationService(const ApplicationServiceInfo&) {}
    virtual void onDataAsset(const DataAssetInfo&) {}
    virtual void onDataUnit(DataUnit&&) {}
    virtual void onSignallingMessage(SignallingMessage&&) {}
    virtual void onApplication(const ApplicationInfo&) {}
    virtual void onDataTransmissionTable(DataTransmissionTable&&) {}
    virtual void onDataDirectoryTable(const DataDirectoryTable&) {}
    virtual void onDataAssetManagementTable(const DataAssetManagementTable&) {}
};

class Demuxer {
public:
    explicit Demuxer(Sink&);
    Demuxer(Sink&, Limits);
    ~Demuxer();

    Demuxer(Demuxer&&) noexcept;
    Demuxer& operator=(Demuxer&&) noexcept;
    Demuxer(const Demuxer&) = delete;
    Demuxer& operator=(const Demuxer&) = delete;

    void push(const std::uint8_t* data, std::size_t size);
    void flush();
    void reset();
    void reposition(RepositionOptions);
    void selectService(std::optional<std::uint32_t> context_id);
    void selectTrack(TrackKind kind, std::optional<std::uint64_t> track_id);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tlvdemux
