#include <aribtlv/demuxer.hpp>
#include <aribtlv/recording.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

struct TestSink final : aribtlv::Sink {
    std::vector<aribtlv::ServiceInfo> services;
    std::vector<aribtlv::TrackInfo> tracks;
    std::vector<aribtlv::AccessUnit> access_units;
    std::vector<aribtlv::ApplicationServiceInfo> application_services;
    std::vector<aribtlv::LayoutConfiguration> layouts;
    std::vector<aribtlv::DataAssetInfo> data_assets;
    std::vector<aribtlv::TrackInfo> removed_tracks;
    std::vector<aribtlv::ApplicationServiceInfo> removed_application_services;
    std::vector<aribtlv::DataAssetInfo> removed_data_assets;
    std::vector<aribtlv::SignallingMessage> signalling_messages;
    std::vector<aribtlv::EventInfo> events;
    std::vector<aribtlv::MhSdtSnapshot> mh_sdt_snapshots;
    std::vector<aribtlv::MhTotInfo> mh_tot;
    std::vector<aribtlv::StreamEvent> stream_events;
    std::vector<aribtlv::ViewerParticipationNotification>
        viewer_participation_notifications;
    std::vector<aribtlv::ApplicationInfo> applications;
    std::vector<aribtlv::ApplicationInfo> removed_applications;
    std::vector<aribtlv::MptSnapshot> mpt_snapshots;
    std::vector<aribtlv::MhAitSnapshot> mh_ait_snapshots;
    std::vector<aribtlv::ServiceStateReset> service_resets;
    std::vector<aribtlv::DataTransmissionTable> data_transmission_tables;
    std::vector<aribtlv::Error> errors;
    std::vector<aribtlv::DamageSpan> damage_spans;
    void onService(const aribtlv::ServiceInfo& value) override { services.push_back(value); }
    void onTrack(const aribtlv::TrackInfo& value) override { tracks.push_back(value); }
    void onTrackRemoved(const aribtlv::TrackInfo& value) override {
        removed_tracks.push_back(value);
    }
    void onAccessUnit(aribtlv::AccessUnit&& value) override {
        access_units.push_back(std::move(value));
    }
    void onApplicationService(const aribtlv::ApplicationServiceInfo& value) override {
        application_services.push_back(value);
    }
    void onApplicationServiceRemoved(
        const aribtlv::ApplicationServiceInfo& value) override {
        removed_application_services.push_back(value);
    }
    void onLayoutConfiguration(const aribtlv::LayoutConfiguration& value) override {
        layouts.push_back(value);
    }
    void onDataAsset(const aribtlv::DataAssetInfo& value) override {
        data_assets.push_back(value);
    }
    void onDataAssetRemoved(const aribtlv::DataAssetInfo& value) override {
        removed_data_assets.push_back(value);
    }
    void onSignallingMessage(aribtlv::SignallingMessage&& value) override {
        signalling_messages.push_back(std::move(value));
    }
    void onEventInfo(const aribtlv::EventInfo& value) override { events.push_back(value); }
    void onMhSdtSnapshot(const aribtlv::MhSdtSnapshot& value) override {
        mh_sdt_snapshots.push_back(value);
    }
    void onMhTot(const aribtlv::MhTotInfo& value) override { mh_tot.push_back(value); }
    void onStreamEvent(const aribtlv::StreamEvent& value) override {
        stream_events.push_back(value);
    }
    void onViewerParticipationNotification(
        const aribtlv::ViewerParticipationNotification& value) override {
        viewer_participation_notifications.push_back(value);
    }
    void onApplication(const aribtlv::ApplicationInfo& value) override {
        applications.push_back(value);
    }
    void onApplicationRemoved(const aribtlv::ApplicationInfo& value) override {
        removed_applications.push_back(value);
    }
    void onMptSnapshot(const aribtlv::MptSnapshot& value) override {
        mpt_snapshots.push_back(value);
    }
    void onMhAitSnapshot(const aribtlv::MhAitSnapshot& value) override {
        mh_ait_snapshots.push_back(value);
    }
    void onServiceStateReset(const aribtlv::ServiceStateReset& value) override {
        service_resets.push_back(value);
    }
    void onDataTransmissionTable(aribtlv::DataTransmissionTable&& value) override {
        data_transmission_tables.push_back(std::move(value));
    }
    void onError(const aribtlv::Error& value) override { errors.push_back(value); }
    void onDamage(const aribtlv::DamageSpan& value) override {
        damage_spans.push_back(value);
    }
};

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void check(const bool condition, const std::string& message) {
    if (!condition) fail(message);
}

std::vector<std::uint8_t> mmtp_signalling(const std::uint16_t packet_id,
                                          const std::uint32_t sequence) {
    return {
        0x00, 0x02,
        static_cast<std::uint8_t>(packet_id >> 8U), static_cast<std::uint8_t>(packet_id),
        0, 0, 0, 0,
        static_cast<std::uint8_t>(sequence >> 24U), static_cast<std::uint8_t>(sequence >> 16U),
        static_cast<std::uint8_t>(sequence >> 8U), static_cast<std::uint8_t>(sequence),
        0x00, 0x00, 0x00, 0x00,
    };
}

std::vector<std::uint8_t> compressed(const std::uint16_t context_id,
                                     const std::uint16_t packet_id,
                                     const std::uint32_t sequence) {
    auto mmtp = mmtp_signalling(packet_id, sequence);
    std::vector<std::uint8_t> result{
        static_cast<std::uint8_t>((context_id << 4U) >> 8U),
        static_cast<std::uint8_t>(context_id << 4U),
        0x61,
    };
    result.insert(result.end(), mmtp.begin(), mmtp.end());
    return result;
}

std::vector<std::uint8_t> tlv(const std::uint8_t type,
                              const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> result{
        0x7f, type,
        static_cast<std::uint8_t>(payload.size() >> 8U),
        static_cast<std::uint8_t>(payload.size()),
    };
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

std::vector<std::uint8_t> stream_for_contexts(const std::uint16_t first,
                                              const std::uint16_t second) {
    auto result = tlv(0x03, compressed(first, 0x8000, 1));
    const auto tail = tlv(0x03, compressed(second, 0x8000, 1));
    result.insert(result.end(), tail.begin(), tail.end());
    return result;
}

void test_split_at_every_boundary() {
    const auto data = stream_for_contexts(1, 2);
    for (std::size_t split = 0; split <= data.size(); ++split) {
        TestSink sink;
        aribtlv::Demuxer demuxer(sink);
        demuxer.push(data.data(), split);
        demuxer.push(data.data() + split, data.size() - split);
        demuxer.flush();
        check(sink.services.size() == 2, "TLV split changed discovered context count");
    }
}

void test_one_byte_input() {
    const auto data = stream_for_contexts(7, 8);
    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    for (const auto byte : data) demuxer.push(&byte, 1);
    demuxer.flush();
    check(sink.services.size() == 2, "one-byte pushes did not match whole-stream parsing");
}

void test_garbage_recovery() {
    auto data = stream_for_contexts(1, 2);
    const auto third = stream_for_contexts(3, 4);
    data.insert(data.end(), {0xde, 0xad, 0x7f, 0x03, 0xff, 0xff, 0xbe, 0xef});
    data.insert(data.end(), third.begin(), third.end());

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(data.data(), data.size());
    demuxer.flush();
    check(sink.services.size() == 4, "parser did not recover after middle garbage");
    check(std::any_of(sink.errors.begin(), sink.errors.end(), [](const auto& error) {
        return error.code == aribtlv::ErrorCode::MalformedInput;
    }), "garbage recovery did not report a recoverable error");
}

void test_service_selection_and_reset() {
    const auto data = stream_for_contexts(10, 11);
    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.selectService(11);
    demuxer.push(data.data(), data.size());
    demuxer.flush();
    check(sink.services.size() == 1 && sink.services[0].context_id == 11,
          "service selection leaked another context");

    demuxer.reset();
    demuxer.push(data.data(), data.size());
    demuxer.flush();
    check(sink.services.size() == 2, "reset did not make selected service discoverable again");
}

void test_incomplete_flush() {
    auto data = stream_for_contexts(1, 2);
    data.resize(data.size() - 3);
    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(data.data(), data.size());
    demuxer.flush();
    check(!sink.errors.empty(), "flush did not report incomplete trailing data");
}

void test_mode_60_and_resource_limit() {
    auto mmtp = mmtp_signalling(0x8000, 1);
    std::vector<std::uint8_t> stream;
    for (const auto mode_and_size : std::vector<std::pair<std::uint8_t, std::size_t>>{
             {0x20, 20}, {0x21, 2}, {0x60, 42}, {0x61, 0}}) {
        std::vector<std::uint8_t> payload{0x12, 0x30, mode_and_size.first};
        payload.insert(payload.end(), mode_and_size.second, 0);
        payload.insert(payload.end(), mmtp.begin(), mmtp.end());
        const auto packet = tlv(0x03, payload);
        stream.insert(stream.end(), packet.begin(), packet.end());
    }

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(stream.data(), stream.size());
    demuxer.flush();
    check(sink.services.size() == 1 && sink.services[0].context_id == 0x123,
          "compressed-IP modes did not preserve their shared context ID");

    aribtlv::Limits limits;
    limits.max_resync_buffer = 16;
    TestSink limited_sink;
    aribtlv::Demuxer limited(limited_sink, limits);
    std::vector<std::uint8_t> garbage(128, 0x55);
    limited.push(garbage.data(), garbage.size());
    limited.flush();
    check(std::any_of(limited_sink.errors.begin(), limited_sink.errors.end(), [](const auto& error) {
        return error.code == aribtlv::ErrorCode::ResourceLimit;
    }), "TLV resynchronization buffer limit was not enforced");

    const auto unsupported_packet = tlv(0x03, {0x00, 0x10, 0x22});
    std::vector<std::uint8_t> noisy_stream;
    for (int index = 0; index < 100; ++index) {
        noisy_stream.insert(noisy_stream.end(), unsupported_packet.begin(), unsupported_packet.end());
    }
    TestSink noisy_sink;
    aribtlv::Demuxer noisy(noisy_sink);
    noisy.push(noisy_stream.data(), noisy_stream.size());
    noisy.flush();
    const auto unsupported_callbacks = std::count_if(
        noisy_sink.errors.begin(), noisy_sink.errors.end(), [](const auto& error) {
            return error.code == aribtlv::ErrorCode::MalformedInput;
        });
    check(unsupported_callbacks > 0 && unsupported_callbacks < 10,
          "identical recoverable errors were not rate-limited");
}

void append_u16(std::vector<std::uint8_t>& value, const std::size_t number) {
    value.push_back(static_cast<std::uint8_t>(number >> 8U));
    value.push_back(static_cast<std::uint8_t>(number));
}

void append_u32(std::vector<std::uint8_t>& value, const std::size_t number) {
    value.push_back(static_cast<std::uint8_t>(number >> 24U));
    value.push_back(static_cast<std::uint8_t>(number >> 16U));
    value.push_back(static_cast<std::uint8_t>(number >> 8U));
    value.push_back(static_cast<std::uint8_t>(number));
}

void descriptor(std::vector<std::uint8_t>& value, const std::uint16_t tag,
                const std::vector<std::uint8_t>& payload) {
    append_u16(value, tag);
    value.push_back(static_cast<std::uint8_t>(payload.size()));
    value.insert(value.end(), payload.begin(), payload.end());
}

void append_u64(std::vector<std::uint8_t>& value, const std::uint64_t number) {
    append_u32(value, static_cast<std::uint32_t>(number >> 32U));
    append_u32(value, static_cast<std::uint32_t>(number));
}

void timing_descriptors(std::vector<std::uint8_t>& value,
                        const std::uint32_t mpu_sequence,
                        const std::uint32_t timescale,
                        const std::uint8_t au_count = 1,
                        const std::uint64_t mpu_presentation_time = 100ULL << 32U,
                        const std::uint8_t leap_indicator = 0) {
    std::vector<std::uint8_t> timestamp;
    append_u32(timestamp, mpu_sequence);
    append_u64(timestamp, mpu_presentation_time);
    descriptor(value, 0x0001, timestamp);

    std::vector<std::uint8_t> extended{0x03};
    append_u32(extended, timescale);
    append_u16(extended, 3000);
    append_u32(extended, mpu_sequence);
    extended.push_back(static_cast<std::uint8_t>(leap_indicator << 6U));
    append_u16(extended, 0);
    extended.push_back(au_count);
    for (std::uint16_t index = 0; index < au_count; ++index) append_u16(extended, 0);
    descriptor(value, 0x8026, extended);
}

// Unlike timing_descriptors() above, this carries a distinct dts_pts_offset per
// access unit (pts_offset_type == 2), so tests can prove the descriptor is indexed
// by sample_number rather than by emission order. pts_offsets must be uniform across
// entries unless a test is specifically exercising the non-uniform rejection, since
// emit_access_unit() only accumulates it correctly when it is constant across the MPU.
void per_au_timing_descriptors(std::vector<std::uint8_t>& value,
                               const std::uint32_t mpu_sequence,
                               const std::uint32_t timescale,
                               const std::vector<std::uint16_t>& dts_pts_offsets,
                               const std::vector<std::uint16_t>& pts_offsets) {
    std::vector<std::uint8_t> timestamp;
    append_u32(timestamp, mpu_sequence);
    append_u64(timestamp, 100ULL << 32U);
    descriptor(value, 0x0001, timestamp);

    std::vector<std::uint8_t> extended{0x05}; // timescale present, pts_offset_type == 2
    append_u32(extended, timescale);
    append_u32(extended, mpu_sequence);
    extended.push_back(0);
    append_u16(extended, 0);
    extended.push_back(static_cast<std::uint8_t>(dts_pts_offsets.size()));
    for (std::size_t index = 0; index < dts_pts_offsets.size(); ++index) {
        append_u16(extended, dts_pts_offsets[index]);
        append_u16(extended, pts_offsets[index]);
    }
    descriptor(value, 0x8026, extended);
}

// pts_offset_type == 3 is reserved by TR-B39 Table 34.1-72: only dts_pts_offset is
// present per access unit, with no pts_offset field at all.
void reserved_pts_offset_type_descriptors(std::vector<std::uint8_t>& value,
                                          const std::uint32_t mpu_sequence,
                                          const std::uint32_t timescale,
                                          const std::vector<std::uint16_t>& dts_pts_offsets) {
    std::vector<std::uint8_t> timestamp;
    append_u32(timestamp, mpu_sequence);
    append_u64(timestamp, 100ULL << 32U);
    descriptor(value, 0x0001, timestamp);

    std::vector<std::uint8_t> extended{0x07}; // timescale present, pts_offset_type == 3
    append_u32(extended, timescale);
    append_u32(extended, mpu_sequence);
    extended.push_back(0);
    append_u16(extended, 0);
    extended.push_back(static_cast<std::uint8_t>(dts_pts_offsets.size()));
    for (const auto dts_pts : dts_pts_offsets) append_u16(extended, dts_pts);
    descriptor(value, 0x8026, extended);
}

void asset(std::vector<std::uint8_t>& body, const std::uint16_t packet_id,
           const std::string& type, const std::vector<std::uint8_t>& descriptors) {
    body.push_back(0);
    append_u32(body, 0);
    body.push_back(2);
    append_u16(body, packet_id);
    body.insert(body.end(), type.begin(), type.end());
    body.push_back(0xfe);
    body.push_back(1);
    body.push_back(0);
    append_u16(body, packet_id);
    append_u16(body, descriptors.size());
    body.insert(body.end(), descriptors.begin(), descriptors.end());
}

std::vector<std::uint8_t> layout_configuration_table() {
    std::vector<std::uint8_t> body{
        1,       // number_of_loop
        2, 0, 2, // layout 2, main device, two regions
        0, 0, 0, 100, 100, 0,
        1, 10, 20, 90, 80, 3,
    };
    descriptor(body, 0x8abc, {0xde, 0xad});
    descriptor(body, 0x8002, {0x12, 0x34, 0x56});
    std::vector<std::uint8_t> table{0x81, 7};
    append_u16(table, body.size());
    table.insert(table.end(), body.begin(), body.end());
    return table;
}

std::vector<std::uint8_t> discovery_message() {
    std::vector<std::uint8_t> program_descriptors;
    descriptor(program_descriptors, 0x8034,
               {0x1f, 0x1f, 0xf1, 0x00, 0xff, 0x02, 0x00, 0xff, 0x03,
                40, 0x00, 0xff, 0x04});

    std::vector<std::uint8_t> video_descriptors;
    descriptor(video_descriptors, 0x8011, {0x00, 0x00});
    descriptor(video_descriptors, 0x8000, {0x00, 0x01});
    descriptor(video_descriptors, 0x8abc, {0xde, 0xad, 0xbe});
    descriptor(video_descriptors, 0x800a,
               {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02});
    descriptor(video_descriptors, 0x8010, {0, 0, 0, 0, 0x50, 'j', 'p', 'n'});
    descriptor(video_descriptors, 0x8003,
               {0, 0, 0, 1, 2, 1, 0,
                0, 0, 0, 2, 3, 4, 2, 0xaa, 0xbb});
    timing_descriptors(video_descriptors, 1, 180000);

    std::vector<std::uint8_t> audio_descriptors;
    descriptor(audio_descriptors, 0x8011, {0x01, 0x10});
    descriptor(audio_descriptors, 0x8000, {0x10, 0x00});
    descriptor(audio_descriptors, 0x8000, {0x11, 0x01});
    descriptor(audio_descriptors, 0x8014,
               {0xf3, 0x03, 0x01, 0x10, 0x11, 0xff, 0x5f, 'j', 'p', 'n'});
    timing_descriptors(audio_descriptors, 1, 180000, 2);

    std::vector<std::uint8_t> subtitle_descriptors;
    descriptor(subtitle_descriptors, 0x8011, {0x12, 0x30});
    descriptor(subtitle_descriptors, 0x8020,
               {0x00, 0x20, 0x30, 0x08, 'j', 'p', 'n', 0x02, 0x2a, 0x10,
                0x00, 0x00, 0x00, 0x05,
                0x00, 0x00, 0x00, 0x64, 0x80, 0x00, 0x00, 0x00, 0x7f});

    std::vector<std::uint8_t> application_descriptors;
    descriptor(application_descriptors, 0x8011, {0x12, 0x40});
    descriptor(application_descriptors, 0x8003, {0, 0, 0, 9, 2, 1, 0});

    std::vector<std::uint8_t> mpt_body{0xfc, 2, 0x00, 0x65};
    append_u16(mpt_body, program_descriptors.size());
    mpt_body.insert(mpt_body.end(), program_descriptors.begin(), program_descriptors.end());
    mpt_body.push_back(4);
    asset(mpt_body, 0xf300, "hev1", video_descriptors);
    asset(mpt_body, 0xf310, "mp4a", audio_descriptors);
    asset(mpt_body, 0xf330, "stpp", subtitle_descriptors);
    asset(mpt_body, 0xf340, "aapp", application_descriptors);
    std::vector<std::uint8_t> mpt{0x20, 8};
    append_u16(mpt, mpt_body.size());
    mpt.insert(mpt.end(), mpt_body.begin(), mpt_body.end());

    const auto lct = layout_configuration_table();
    std::vector<std::uint8_t> pa{0x00, 0x00, 0x00};
    append_u32(pa, 1 + lct.size() + mpt.size());
    pa.push_back(0);
    pa.insert(pa.end(), lct.begin(), lct.end());
    pa.insert(pa.end(), mpt.begin(), mpt.end());
    return pa;
}

std::vector<std::uint8_t> video_discovery_message(
    const std::optional<std::uint32_t> mpu_sequence) {
    std::vector<std::uint8_t> descriptors;
    descriptor(descriptors, 0x8011, {0x00, 0x00});
    descriptor(descriptors, 0x8010, {0, 0, 0, 0, 0, 'j', 'p', 'n'});
    if (mpu_sequence.has_value()) {
        timing_descriptors(descriptors, *mpu_sequence, 180000);
    }

    std::vector<std::uint8_t> mpt_body{0xfc, 2, 0x00, 0x65, 0x00, 0x00, 1};
    asset(mpt_body, 0xf300, "hev1", descriptors);
    std::vector<std::uint8_t> mpt{0x20, 8};
    append_u16(mpt, mpt_body.size());
    mpt.insert(mpt.end(), mpt_body.begin(), mpt_body.end());

    std::vector<std::uint8_t> pa{0x00, 0x00, 0x00};
    append_u32(pa, 1 + mpt.size());
    pa.push_back(0);
    pa.insert(pa.end(), mpt.begin(), mpt.end());
    return pa;
}

// Sibling of video_discovery_message() that lets a fixture pick the pts_offset_type
// == 1 descriptor's access-unit count, for comparison against the pts_offset_type
// == 2 descriptors below.
std::vector<std::uint8_t> video_discovery_message_with_au_count(
    const std::uint32_t mpu_sequence, const std::uint8_t au_count) {
    std::vector<std::uint8_t> descriptors;
    descriptor(descriptors, 0x8011, {0x00, 0x00});
    descriptor(descriptors, 0x8010, {0, 0, 0, 0, 0, 'j', 'p', 'n'});
    timing_descriptors(descriptors, mpu_sequence, 180000, au_count);

    std::vector<std::uint8_t> mpt_body{0xfc, 2, 0x00, 0x65, 0x00, 0x00, 1};
    asset(mpt_body, 0xf300, "hev1", descriptors);
    std::vector<std::uint8_t> mpt{0x20, 8};
    append_u16(mpt, mpt_body.size());
    mpt.insert(mpt.end(), mpt_body.begin(), mpt_body.end());

    std::vector<std::uint8_t> pa{0x00, 0x00, 0x00};
    append_u32(pa, 1 + mpt.size());
    pa.push_back(0);
    pa.insert(pa.end(), mpt.begin(), mpt.end());
    return pa;
}

std::vector<std::uint8_t> video_discovery_message_with_offsets(
    const std::uint32_t mpu_sequence, const std::vector<std::uint16_t>& dts_pts_offsets,
    const std::vector<std::uint16_t>& pts_offsets) {
    std::vector<std::uint8_t> descriptors;
    descriptor(descriptors, 0x8011, {0x00, 0x00});
    descriptor(descriptors, 0x8010, {0, 0, 0, 0, 0, 'j', 'p', 'n'});
    per_au_timing_descriptors(descriptors, mpu_sequence, 180000, dts_pts_offsets, pts_offsets);

    std::vector<std::uint8_t> mpt_body{0xfc, 2, 0x00, 0x65, 0x00, 0x00, 1};
    asset(mpt_body, 0xf300, "hev1", descriptors);
    std::vector<std::uint8_t> mpt{0x20, 8};
    append_u16(mpt, mpt_body.size());
    mpt.insert(mpt.end(), mpt_body.begin(), mpt_body.end());

    std::vector<std::uint8_t> pa{0x00, 0x00, 0x00};
    append_u32(pa, 1 + mpt.size());
    pa.push_back(0);
    pa.insert(pa.end(), mpt.begin(), mpt.end());
    return pa;
}

std::vector<std::uint8_t> video_discovery_message_with_reserved_pts_offset_type(
    const std::uint32_t mpu_sequence, const std::vector<std::uint16_t>& dts_pts_offsets) {
    std::vector<std::uint8_t> descriptors;
    descriptor(descriptors, 0x8011, {0x00, 0x00});
    descriptor(descriptors, 0x8010, {0, 0, 0, 0, 0, 'j', 'p', 'n'});
    reserved_pts_offset_type_descriptors(descriptors, mpu_sequence, 180000, dts_pts_offsets);

    std::vector<std::uint8_t> mpt_body{0xfc, 2, 0x00, 0x65, 0x00, 0x00, 1};
    asset(mpt_body, 0xf300, "hev1", descriptors);
    std::vector<std::uint8_t> mpt{0x20, 8};
    append_u16(mpt, mpt_body.size());
    mpt.insert(mpt.end(), mpt_body.begin(), mpt_body.end());

    std::vector<std::uint8_t> pa{0x00, 0x00, 0x00};
    append_u32(pa, 1 + mpt.size());
    pa.push_back(0);
    pa.insert(pa.end(), mpt.begin(), mpt.end());
    return pa;
}

std::vector<std::uint8_t> audio_discovery_message_with_offsets(
    const std::uint32_t mpu_sequence, const std::vector<std::uint16_t>& dts_pts_offsets,
    const std::vector<std::uint16_t>& pts_offsets) {
    std::vector<std::uint8_t> descriptors;
    descriptor(descriptors, 0x8011, {0x01, 0x10});
    descriptor(descriptors, 0x8014,
               {0xf3, 0x03, 0x01, 0x10, 0x11, 0xff, 0x5f, 'j', 'p', 'n'});
    per_au_timing_descriptors(descriptors, mpu_sequence, 180000, dts_pts_offsets, pts_offsets);

    std::vector<std::uint8_t> mpt_body{0xfc, 2, 0x00, 0x66, 0x00, 0x00, 1};
    asset(mpt_body, 0xf310, "mp4a", descriptors);
    std::vector<std::uint8_t> mpt{0x20, 8};
    append_u16(mpt, mpt_body.size());
    mpt.insert(mpt.end(), mpt_body.begin(), mpt_body.end());

    std::vector<std::uint8_t> pa{0x00, 0x00, 0x00};
    append_u32(pa, 1 + mpt.size());
    pa.push_back(0);
    pa.insert(pa.end(), mpt.begin(), mpt.end());
    return pa;
}

// Sibling of audio_discovery_message_with_offsets() that lets a fixture pick
// the MPU's mpu_presentation_time and mpu_presentation_time_leap_indicator
// directly, for exercising the leap-second correction in emit_access_unit().
std::vector<std::uint8_t> audio_discovery_message_with_leap(
    const std::uint32_t mpu_sequence, const std::uint64_t mpu_presentation_time,
    const std::uint8_t leap_indicator) {
    std::vector<std::uint8_t> descriptors;
    descriptor(descriptors, 0x8011, {0x01, 0x10});
    descriptor(descriptors, 0x8014,
               {0xf3, 0x03, 0x01, 0x10, 0x11, 0xff, 0x5f, 'j', 'p', 'n'});
    timing_descriptors(descriptors, mpu_sequence, 180000, 1, mpu_presentation_time,
                       leap_indicator);

    std::vector<std::uint8_t> mpt_body{0xfc, 2, 0x00, 0x66, 0x00, 0x00, 1};
    asset(mpt_body, 0xf310, "mp4a", descriptors);
    std::vector<std::uint8_t> mpt{0x20, 8};
    append_u16(mpt, mpt_body.size());
    mpt.insert(mpt.end(), mpt_body.begin(), mpt_body.end());

    std::vector<std::uint8_t> pa{0x00, 0x00, 0x00};
    append_u32(pa, 1 + mpt.size());
    pa.push_back(0);
    pa.insert(pa.end(), mpt.begin(), mpt.end());
    return pa;
}

std::vector<std::uint8_t> audio_discovery_message() {
    auto audio_descriptors = [](const std::uint8_t component_type,
                                const std::uint16_t component_tag,
                                const bool main_component,
                                const bool multilingual = false) {
        std::vector<std::uint8_t> descriptors;
        std::vector<std::uint8_t> audio{
            0xf3,
            component_type,
            static_cast<std::uint8_t>(component_tag >> 8U),
            static_cast<std::uint8_t>(component_tag),
            0x11,
            0xff,
            static_cast<std::uint8_t>((multilingual ? 0x80U : 0U) |
                                      (main_component ? 0x40U : 0U) | 0x1fU),
            'j', 'p', 'n',
        };
        if (multilingual) audio.insert(audio.end(), {'e', 'n', 'g'});
        descriptor(descriptors, 0x8014, audio);
        timing_descriptors(descriptors, 1, 180000, 2);
        return descriptors;
    };

    std::vector<std::uint8_t> mpt_body{0xfc, 2, 0x00, 0x66, 0x00, 0x00, 3};
    asset(mpt_body, 0xe210, "mp4a", audio_descriptors(0x11, 0x0110, true));
    asset(mpt_body, 0xe275, "mp4a", audio_descriptors(0x09, 0x0011, false));
    asset(mpt_body, 0xe2aa, "mp4a", audio_descriptors(0x03, 0x0012, false, true));

    std::vector<std::uint8_t> mpt{0x20, 8};
    append_u16(mpt, mpt_body.size());
    mpt.insert(mpt.end(), mpt_body.begin(), mpt_body.end());

    std::vector<std::uint8_t> pa{0x00, 0x00, 0x00};
    append_u32(pa, 1 + mpt.size());
    pa.push_back(0);
    pa.insert(pa.end(), mpt.begin(), mpt.end());
    return pa;
}

std::vector<std::uint8_t> application_control_message(
    const std::uint8_t section_number = 0,
    const std::uint8_t last_section_number = 0,
    const std::uint8_t version = 3,
    const bool include_application = true,
    const std::uint16_t application_type = 0x0011,
    const std::uint8_t control_code = 0x01) {
    std::vector<std::uint8_t> descriptors;
    descriptor(descriptors, 0x8029,
               {0x05, 0x00, 0x01, 0x01, 0x02, 0x03,
                0xe1, 0x7f, 0x05});
    descriptor(descriptors, 0x802b,
               {'i', 'n', 'd', 'e', 'x', '.', 'h', 't', 'm', 'l'});

    std::vector<std::uint8_t> common_descriptors;
    std::vector<std::uint8_t> transport{0x00, 0x05, 0x05, 0x05};
    transport.insert(transport.end(), {'/', 'a', 'p', 'p', '/'});
    transport.push_back(0);
    descriptor(common_descriptors, 0x802a, transport);
    std::vector<std::uint8_t> unreferenced_transport{0x00, 0x05, 0x06, 0x07};
    unreferenced_transport.insert(unreferenced_transport.end(),
                                  {'/', 'i', 'g', 'n', 'o', 'r', 'e'});
    unreferenced_transport.push_back(0);
    descriptor(common_descriptors, 0x802a, unreferenced_transport);

    std::vector<std::uint8_t> applications;
    append_u16(applications, 0x1234);
    append_u32(applications, 0x01020304);
    applications.push_back(control_code);
    append_u16(applications, 0xf000U | descriptors.size());
    applications.insert(applications.end(), descriptors.begin(), descriptors.end());

    if (!include_application) applications.clear();
    std::vector<std::uint8_t> section{0x9c, 0x00, 0x00};
    append_u16(section, application_type);
    section.push_back(static_cast<std::uint8_t>(0xc1U | ((version & 0x1fU) << 1U)));
    section.push_back(section_number);
    section.push_back(last_section_number);
    append_u16(section, 0xf000U | common_descriptors.size());
    section.insert(section.end(), common_descriptors.begin(), common_descriptors.end());
    append_u16(section, 0xf000U | applications.size());
    section.insert(section.end(), applications.begin(), applications.end());
    append_u32(section, 0);
    const auto section_length = section.size() - 3;
    section[1] = static_cast<std::uint8_t>(0xf0U | (section_length >> 8U));
    section[2] = static_cast<std::uint8_t>(section_length);

    std::vector<std::uint8_t> message{0x80, 0x00, 0x00};
    append_u16(message, section.size());
    message.insert(message.end(), section.begin(), section.end());
    return message;
}

std::vector<std::uint8_t> data_transmission_message() {
    std::vector<std::uint8_t> table{0xa3, 0x00, 0x00, 0x2a, 0xff, 0xc9, 0x00, 0x00,
                                    0x01, '/', 0x00};
    append_u32(table, 0);
    const auto section_length = table.size() - 3;
    table[1] = static_cast<std::uint8_t>(0xf0U | (section_length >> 8U));
    table[2] = static_cast<std::uint8_t>(section_length);
    std::vector<std::uint8_t> message{0x80, 0x03, 0x00};
    append_u32(message, table.size());
    message.insert(message.end(), table.begin(), table.end());
    return message;
}

std::vector<std::uint8_t> signalling_mmtp(const std::uint32_t sequence,
                                          const std::uint8_t flags,
                                          const std::vector<std::uint8_t>& body,
                                          const std::uint16_t packet_id = 0xff02) {
    auto mmtp = mmtp_signalling(packet_id, 1);
    mmtp.resize(12);
    mmtp[8] = static_cast<std::uint8_t>(sequence >> 24U);
    mmtp[9] = static_cast<std::uint8_t>(sequence >> 16U);
    mmtp[10] = static_cast<std::uint8_t>(sequence >> 8U);
    mmtp[11] = static_cast<std::uint8_t>(sequence);
    mmtp.push_back(flags);
    mmtp.push_back(0);
    mmtp.insert(mmtp.end(), body.begin(), body.end());
    return mmtp;
}

std::vector<std::uint8_t> discovery_stream() {
    const auto pa = discovery_message();
    const auto mmtp = signalling_mmtp(1, 0, pa);
    std::vector<std::uint8_t> compressed_payload{0x00, 0x10, 0x61};
    compressed_payload.insert(compressed_payload.end(), mmtp.begin(), mmtp.end());
    const auto packet = tlv(0x03, compressed_payload);
    auto stream = packet;
    stream.insert(stream.end(), packet.begin(), packet.end());
    return stream;
}

std::vector<std::uint8_t> signalling_tlv(const std::uint32_t sequence,
                                         const std::uint8_t flags,
                                         const std::vector<std::uint8_t>& body,
                                         const std::uint16_t packet_id = 0xff02) {
    const auto mmtp = signalling_mmtp(sequence, flags, body, packet_id);
    std::vector<std::uint8_t> compressed_payload{0x00, 0x10, 0x61};
    compressed_payload.insert(compressed_payload.end(), mmtp.begin(), mmtp.end());
    return tlv(0x03, compressed_payload);
}

std::vector<std::uint8_t> mh_eit_message() {
    const std::string title = "録画された番組";
    const std::string description = "番組概要";
    std::vector<std::uint8_t> short_event{'j', 'p', 'n',
        static_cast<std::uint8_t>(title.size())};
    short_event.insert(short_event.end(), title.begin(), title.end());
    append_u16(short_event, description.size());
    short_event.insert(short_event.end(), description.begin(), description.end());

    std::vector<std::uint8_t> descriptors;
    append_u16(descriptors, 0xf001);
    append_u16(descriptors, short_event.size());
    descriptors.insert(descriptors.end(), short_event.begin(), short_event.end());

    std::vector<std::uint8_t> extended{0x00, 'j', 'p', 'n'};
    std::vector<std::uint8_t> extended_items{0x04, 'C', 'a', 's', 't'};
    append_u16(extended_items, 5);
    extended_items.insert(extended_items.end(), {'A', 'l', 'i', 'c', 'e'});
    append_u16(extended, extended_items.size());
    extended.insert(extended.end(), extended_items.begin(), extended_items.end());
    append_u16(extended, 4);
    extended.insert(extended.end(), {'M', 'o', 'r', 'e'});
    append_u16(descriptors, 0xf002);
    append_u16(descriptors, extended.size());
    descriptors.insert(descriptors.end(), extended.begin(), extended.end());
    descriptor(descriptors, 0x8012, {0x12, 0x34});
    descriptor(descriptors, 0x8013, {'J', 'P', 'N', 0x04});
    descriptor(descriptors, 0x8014,
               {0x03, 0x03, 0x00, 0x10, 0x11, 0xff, 0x4e,
                'j', 'p', 'n', 'M', 'a', 'i', 'n'});
    descriptor(descriptors, 0x8016,
               {0x12, 0x34, 0x25, 0x9e, 0x8c, 0x00, 0x10, 0x0c,
                'S', 'e', 'r', 'i', 'e', 's'});

    std::vector<std::uint8_t> section{
        0x8b, 0xf0, 0x00,
        0x00, 0x65, // service_id 101
        0xc7,       // version 3, current_next=1
        0x00, 0x01,
        0x00, 0x0b, // tlv_stream_id 11
        0x00, 0x04, // original_network_id 4
        0x01, 0x8b,
        0x12, 0x34,
        0xc0, 0x79, 0x12, 0x45, 0x00, // 1993-10-13 12:45:00 JST
        0x01, 0x45, 0x30,
        static_cast<std::uint8_t>(0x80U | (descriptors.size() >> 8U)),
        static_cast<std::uint8_t>(descriptors.size()),
    };
    section.insert(section.end(), descriptors.begin(), descriptors.end());
    append_u32(section, 0);
    const auto section_length = section.size() - 3;
    section[1] = static_cast<std::uint8_t>(0xf0U | (section_length >> 8U));
    section[2] = static_cast<std::uint8_t>(section_length);

    std::vector<std::uint8_t> message{0x80, 0x00, 0x00};
    append_u16(message, section.size());
    message.insert(message.end(), section.begin(), section.end());
    return message;
}

std::vector<std::uint8_t> mh_sdt_message() {
    std::vector<std::uint8_t> service_descriptor{0x01, 0x03, 'N', 'H', 'K',
                                                  0x03, 'B', 'S', '4'};
    std::vector<std::uint8_t> descriptors;
    descriptor(descriptors, 0x8019, service_descriptor);
    std::vector<std::uint8_t> section{0x9f, 0x00, 0x00};
    append_u16(section, 11);
    section.push_back(0xc7);
    section.push_back(0);
    section.push_back(0);
    append_u16(section, 4);
    section.push_back(0xff);
    append_u16(section, 101);
    section.push_back(0xff);
    append_u16(section, 0x8000U | descriptors.size());
    section.insert(section.end(), descriptors.begin(), descriptors.end());
    append_u32(section, 0);
    const auto section_length = section.size() - 3;
    section[1] = static_cast<std::uint8_t>(0xf0U | (section_length >> 8U));
    section[2] = static_cast<std::uint8_t>(section_length);
    std::vector<std::uint8_t> message{0x80, 0x00, 0x00};
    append_u16(message, section.size());
    message.insert(message.end(), section.begin(), section.end());
    return message;
}

std::vector<std::uint8_t> mh_tot_message() {
    std::vector<std::uint8_t> local_offset{
        'J', 'P', 'N', 0x05, 0x01, 0x00,
        0x9e, 0x8c, 0x13, 0x00, 0x00, 0x02, 0x00,
    };
    std::vector<std::uint8_t> descriptors;
    descriptor(descriptors, 0x8023, local_offset);
    std::vector<std::uint8_t> section{0xa1, 0x00, 0x00,
                                      0x9e, 0x8c, 0x12, 0x34, 0x56};
    append_u16(section, 0xf000U | descriptors.size());
    section.insert(section.end(), descriptors.begin(), descriptors.end());
    append_u32(section, 0);
    const auto section_length = section.size() - 3;
    section[1] = static_cast<std::uint8_t>(0x70U | (section_length >> 8U));
    section[2] = static_cast<std::uint8_t>(section_length);
    std::vector<std::uint8_t> message{0x80, 0x02, 0x00};
    append_u16(message, section.size());
    message.insert(message.end(), section.begin(), section.end());
    return message;
}

void test_independent_m2_sdt_and_tot() {
    auto stream = signalling_tlv(1, 0, mh_sdt_message(), 0x8004);
    const auto tot = signalling_tlv(1, 0, mh_tot_message(), 0x8005);
    stream.insert(stream.end(), tot.begin(), tot.end());
    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(stream.data(), stream.size());
    demuxer.flush();
    check(sink.mh_sdt_snapshots.size() == 1 &&
              sink.mh_sdt_snapshots[0].tlv_stream_id == 11 &&
              sink.mh_sdt_snapshots[0].original_network_id == 4 &&
              sink.mh_sdt_snapshots[0].services.size() == 1 &&
              sink.mh_sdt_snapshots[0].services[0].service_id == 101 &&
              sink.mh_sdt_snapshots[0].services[0].provider_name == "NHK" &&
              sink.mh_sdt_snapshots[0].services[0].service_name == "BS4",
          "independent M2 MH-SDT did not produce a complete service snapshot");
    const auto expected_time =
        static_cast<std::int64_t>(86400 + 12 * 3600 + 34 * 60 + 56 - 9 * 3600) * 1000;
    check(sink.mh_tot.size() == 1 &&
              sink.mh_tot[0].time_unix_milliseconds == expected_time &&
              sink.mh_tot[0].local_time_offsets.size() == 1 &&
              sink.mh_tot[0].local_time_offsets[0].offset_minutes == 60 &&
              sink.mh_tot[0].local_time_offsets[0].next_offset_minutes == 120,
          "independent M2-short MH-TOT did not expose JST/local-offset state");
    check(sink.signalling_messages.size() == 2 &&
              sink.signalling_messages[0].message_id == 0x8000 &&
              sink.signalling_messages[1].message_id == 0x8002,
          "independent M2/M2-short messages were not exposed");
}

void test_mh_eit_program_events() {
    const auto message = mh_eit_message();
    auto stream = signalling_tlv(1, 0, message);
    const auto repeated = signalling_tlv(2, 0, message);
    stream.insert(stream.end(), repeated.begin(), repeated.end());

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(stream.data(), stream.size());
    demuxer.flush();
    check(sink.events.size() == 1, "repeated MH-EIT event was not deduplicated");
    const auto& event = sink.events[0];
    check(event.table_id == 0x8b && event.current_next && event.section_number == 0 &&
              event.service_id == 101 && event.tlv_stream_id == 11 &&
              event.original_network_id == 4 && event.event_id == 0x1234,
          "MH-EIT event identity was not parsed");
    check(event.start_time_unix_milliseconds == std::optional<std::int64_t>{750483900000LL} &&
              event.duration_seconds == std::optional<std::uint32_t>{6330},
          "MH-EIT MJD/BCD time was not converted from JST");
    check(event.running_status == 4 && !event.free_ca_mode && event.language == "jpn" &&
              event.title == "録画された番組" && event.description == "番組概要",
          "MH short-event descriptor was not parsed");
    check(event.extended_description == "More" && event.extended_items.size() == 1 &&
              event.extended_items[0].description == "Cast" &&
              event.extended_items[0].value == "Alice" &&
              event.genres.size() == 1 && event.genres[0].level1 == 1 &&
              event.genres[0].level2 == 2 && event.parental_ratings.size() == 1 &&
              event.parental_ratings[0].rating == 4,
          "MH extended/content/parental event descriptors were not parsed");
    check(event.audio_components.size() == 1 &&
              event.audio_components[0].audio.component_tag == 0x10 &&
              event.audio_components[0].audio.sample_rate == 48000 &&
              event.audio_components[0].text == "Main" && event.series.has_value() &&
              event.series->series_id == 0x1234 && event.series->episode_number == 1 &&
              event.series->last_episode_number == 12 && event.series->name == "Series",
          "MH audio-component/series event descriptors were not parsed");
}

std::vector<std::uint8_t> emt_message(const std::uint8_t version,
                                      const std::uint16_t message_id) {
    std::vector<std::uint8_t> descriptors;
    append_u16(descriptors, 0x8021);
    descriptors.push_back(17);
    append_u64(descriptors, 200ULL << 32U);
    append_u64(descriptors, 10ULL << 32U);
    descriptors.push_back(0x30); // leap=0, NPT advances at the UTC rate

    std::vector<std::uint8_t> event_payload;
    append_u16(event_payload, 0x001f); // group 1 + reserved nibble
    event_payload.push_back(0);        // immediate
    append_u64(event_payload, 0);
    event_payload.push_back(2);
    append_u16(event_payload, message_id);
    event_payload.insert(event_payload.end(), {0xde, 0xad});
    append_u16(descriptors, 0xf003);
    append_u16(descriptors, event_payload.size());
    descriptors.insert(descriptors.end(), event_payload.begin(), event_payload.end());

    std::vector<std::uint8_t> section{
        0xa6, 0xf0, 0x00,
        0x30, 0x01, // data_event_id 3, group 1
        static_cast<std::uint8_t>(0xc1U | ((version & 0x1fU) << 1U)),
        0x00, 0x00,
    };
    section.insert(section.end(), descriptors.begin(), descriptors.end());
    append_u32(section, 0);
    const auto section_length = section.size() - 3;
    section[1] = static_cast<std::uint8_t>(0xf0U | (section_length >> 8U));
    section[2] = static_cast<std::uint8_t>(section_length);

    std::vector<std::uint8_t> message{0x80, 0x00, 0x00};
    append_u16(message, section.size());
    message.insert(message.end(), section.begin(), section.end());
    return message;
}

std::vector<std::uint8_t> viewer_participation_emt(
    const std::uint8_t version, const bool current_next = true) {
    std::vector<std::uint8_t> section{
        0xa6, 0xf0, 0x09,
        0xff, 0x00, // data_event_id 0xF, event_msg_group_id 0xF00
        static_cast<std::uint8_t>(0xc0U | ((version & 0x1fU) << 1U) |
                                  (current_next ? 1U : 0U)),
        0x00, 0x00,
    };
    append_u32(section, 0);
    std::vector<std::uint8_t> message{0x80, 0x00, 0x00};
    append_u16(message, section.size());
    message.insert(message.end(), section.begin(), section.end());
    return message;
}

void test_emt_stream_events() {
    auto stream = discovery_stream();
    const auto first = signalling_tlv(1, 0, emt_message(7, 0xb007), 0xff04);
    const auto repeated = signalling_tlv(2, 0, emt_message(7, 0xb007), 0xff04);
    const auto updated = signalling_tlv(3, 0, emt_message(7, 0xb008), 0xff04);
    stream.insert(stream.end(), first.begin(), first.end());
    stream.insert(stream.end(), repeated.begin(), repeated.end());
    stream.insert(stream.end(), updated.begin(), updated.end());

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(stream.data(), stream.size());
    demuxer.flush();
    check(sink.stream_events.size() == 2,
          "EMT messages were not deduplicated by identity and version");
    const auto& event = sink.stream_events.front();
    check(event.event_message_tag == 40 && event.data_event_id == 3 &&
              event.message_group_id == 1 && event.message_version == 7 &&
              event.time_mode == 0 && event.message_type == 2 &&
              event.raw_message_id == 0xb007 && event.message_id == 176,
          "EMT identity was not parsed with its MPT-signalled tag");
    check(event.utc_reference == std::optional<std::uint64_t>{200ULL << 32U} &&
              event.npt_reference == std::optional<std::uint64_t>{10ULL << 32U} &&
              event.private_data == std::vector<std::uint8_t>({0xde, 0xad}),
          "EMT timing reference or private data was not parsed");
}

void test_viewer_participation_notifications() {
    auto stream = signalling_tlv(1, 0, viewer_participation_emt(7), 0xff04);
    const auto repeated = signalling_tlv(2, 0, viewer_participation_emt(7), 0xff04);
    const auto updated = signalling_tlv(3, 0, viewer_participation_emt(8), 0xff04);
    const auto not_current = signalling_tlv(
        4, 0, viewer_participation_emt(9, false), 0xff04);
    stream.insert(stream.end(), repeated.begin(), repeated.end());
    stream.insert(stream.end(), updated.begin(), updated.end());
    stream.insert(stream.end(), not_current.begin(), not_current.end());

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(stream.data(), stream.size());
    demuxer.flush();
    check(sink.viewer_participation_notifications.size() == 2,
          "viewer-participation EMT was not deduplicated by table version");
    const auto& notification = sink.viewer_participation_notifications.front();
    check(notification.context_id == 1 && notification.source_packet_id == 0xff04 &&
              notification.event_message_tag == 0xff &&
              notification.data_event_id == 0x0f &&
              notification.message_group_id == 0x0f00 &&
              notification.version == 7 && notification.current_next &&
              notification.section_number == 0 &&
              notification.last_section_number == 0,
          "descriptor-less viewer-participation EMT identity was not exposed");
    check(sink.stream_events.empty(),
          "viewer-participation notification leaked into application StreamEvent");

    demuxer.reset();
    demuxer.push(stream.data(), stream.size());
    demuxer.flush();
    check(sink.viewer_participation_notifications.size() == 4,
          "full reset retained viewer-participation deduplication state");
}

void test_global_packet_state_budget() {
    const auto data = discovery_stream();
    aribtlv::Limits limits;
    limits.max_packet_states = 3; // one signalling PID plus two track states
    TestSink sink;
    aribtlv::Demuxer demuxer(sink, limits);
    demuxer.push(data.data(), data.size());
    demuxer.flush();
    check(sink.tracks.size() == 2 &&
              std::any_of(sink.errors.begin(), sink.errors.end(), [](const auto& error) {
                  return error.code == aribtlv::ErrorCode::ResourceLimit;
              }),
          "global MMTP packet/track-state budget was not shared by signalling and tracks");
}

void test_signalling_fragmentation_aggregation_and_m2() {
    const auto pa = discovery_message();
    const auto first_end = pa.size() / 3;
    const auto middle_end = first_end * 2;
    auto first = signalling_tlv(10, 0x40,
        std::vector<std::uint8_t>(pa.begin(), pa.begin() + static_cast<std::ptrdiff_t>(first_end)));
    const auto middle = signalling_tlv(11, 0x80,
        std::vector<std::uint8_t>(pa.begin() + static_cast<std::ptrdiff_t>(first_end),
                                  pa.begin() + static_cast<std::ptrdiff_t>(middle_end)));
    const auto last = signalling_tlv(12, 0xc0,
        std::vector<std::uint8_t>(pa.begin() + static_cast<std::ptrdiff_t>(middle_end), pa.end()));
    first.insert(first.end(), middle.begin(), middle.end());
    first.insert(first.end(), last.begin(), last.end());
    first.insert(first.end(), last.begin(), last.end()); // duplicate is ignored
    TestSink fragmented_sink;
    aribtlv::Demuxer fragmented(fragmented_sink);
    fragmented.push(first.data(), first.size());
    fragmented.flush();
    check(fragmented_sink.tracks.size() == 3,
          "first/middle/last signalling fragments did not reassemble exactly once");

    std::vector<std::uint8_t> aggregate;
    append_u16(aggregate, pa.size());
    aggregate.insert(aggregate.end(), pa.begin(), pa.end());
    append_u16(aggregate, 4);
    aggregate.insert(aggregate.end(), {0x80, 0x03, 0x00, 0x00});
    auto aggregated_stream = signalling_tlv(20, 0x01, aggregate);
    const auto aggregate_tail = signalling_tlv(21, 0x01, aggregate);
    aggregated_stream.insert(aggregated_stream.end(), aggregate_tail.begin(), aggregate_tail.end());
    TestSink aggregated_sink;
    aribtlv::Demuxer aggregated(aggregated_sink);
    aggregated.push(aggregated_stream.data(), aggregated_stream.size());
    aggregated.flush();
    check(aggregated_sink.tracks.size() == 3,
          "aggregated signalling messages were not length-delimited and deduplicated");

    const auto mpt_start = static_cast<std::size_t>(8);
    std::vector<std::uint8_t> m2{0x80, 0x00, 0x00};
    append_u16(m2, pa.size() - mpt_start);
    m2.insert(m2.end(), pa.begin() + static_cast<std::ptrdiff_t>(mpt_start), pa.end());
    auto m2_stream = signalling_tlv(30, 0, m2);
    const auto m2_tail = signalling_tlv(31, 0, m2);
    m2_stream.insert(m2_stream.end(), m2_tail.begin(), m2_tail.end());
    TestSink m2_sink;
    aribtlv::Demuxer m2_demuxer(m2_sink);
    m2_demuxer.push(m2_stream.data(), m2_stream.size());
    m2_demuxer.flush();
    check(m2_sink.tracks.size() == 3, "M2 section message did not carry its MPT");

    auto gap_stream = signalling_tlv(40, 0x40,
        std::vector<std::uint8_t>(pa.begin(), pa.begin() + static_cast<std::ptrdiff_t>(first_end)));
    const auto gap_last = signalling_tlv(42, 0xc0,
        std::vector<std::uint8_t>(pa.begin() + static_cast<std::ptrdiff_t>(first_end), pa.end()));
    const auto recovered = signalling_tlv(43, 0, pa);
    gap_stream.insert(gap_stream.end(), gap_last.begin(), gap_last.end());
    gap_stream.insert(gap_stream.end(), recovered.begin(), recovered.end());
    TestSink gap_sink;
    aribtlv::Demuxer gap_demuxer(gap_sink);
    gap_demuxer.push(gap_stream.data(), gap_stream.size());
    gap_demuxer.flush();
    check(gap_sink.tracks.size() == 3 &&
              std::any_of(gap_sink.errors.begin(), gap_sink.errors.end(), [](const auto& error) {
                  return error.code == aribtlv::ErrorCode::Discontinuity;
              }),
          "signalling sequence gap did not discard the fragment and recover at a complete message");

    auto malformed_pa = pa;
    malformed_pa[10] = 0xff;
    malformed_pa[11] = 0xff;
    auto malformed_stream = signalling_tlv(50, 0, malformed_pa);
    const auto valid_after_malformed = signalling_tlv(51, 0, pa);
    malformed_stream.insert(malformed_stream.end(),
                            valid_after_malformed.begin(), valid_after_malformed.end());
    TestSink malformed_sink;
    aribtlv::Demuxer malformed_demuxer(malformed_sink);
    malformed_demuxer.push(malformed_stream.data(), malformed_stream.size());
    malformed_demuxer.flush();
    check(malformed_sink.tracks.size() == 3 &&
              std::any_of(malformed_sink.errors.begin(), malformed_sink.errors.end(), [](const auto& error) {
                  return error.code == aribtlv::ErrorCode::MalformedInput;
              }),
          "malformed nested MPT length damaged later signalling recovery");
}

void test_track_discovery_and_deduplication() {
    const auto data = discovery_stream();
    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(data.data(), data.size());
    demuxer.flush();
    check(sink.tracks.size() == 3, "MPT did not discover exactly three supported tracks");
    check(sink.layouts.size() == 1 && sink.layouts[0].context_id == 1 &&
              sink.layouts[0].source_packet_id == 0xff02 &&
              sink.layouts[0].version == 7 &&
              sink.layouts[0].background_color_rgb ==
                  std::optional<std::uint32_t>{0x123456} &&
              sink.layouts[0].devices.size() == 1 &&
              sink.layouts[0].devices[0].layout_number == 2 &&
              sink.layouts[0].devices[0].device_id == 0 &&
              sink.layouts[0].devices[0].regions.size() == 2 &&
              sink.layouts[0].devices[0].regions[1].region_number == 1 &&
              sink.layouts[0].devices[0].regions[1].left_top_pos_x == 10 &&
              sink.layouts[0].devices[0].regions[1].left_top_pos_y == 20 &&
              sink.layouts[0].devices[0].regions[1].right_down_pos_x == 90 &&
              sink.layouts[0].devices[0].regions[1].right_down_pos_y == 80 &&
              sink.layouts[0].devices[0].regions[1].layer_order == 3,
          "LCT layout regions or background color were not exposed");
    check(sink.application_services.size() == 1 &&
              sink.application_services[0].application_format == 1 &&
              sink.application_services[0].document_resolution == 1 &&
              sink.application_services[0].default_ait &&
              sink.application_services[0].has_data_transmission_messages &&
              sink.application_services[0].ait_packet_id == 0xff02 &&
              sink.application_services[0].data_transmission_packet_id == 0xff03,
          "ARIB-HTML5 application service metadata was not parsed from the MPT");
    check(sink.application_services[0].event_message_locations.size() == 1 &&
              sink.application_services[0].event_message_locations[0].event_message_tag == 40 &&
              sink.application_services[0].event_message_locations[0].packet_id == 0xff04,
          "EMT tag/location metadata was not parsed from the MPT");
    check(sink.data_assets.size() == 1 &&
              sink.data_assets[0].packet_id == 0xf340 &&
              sink.data_assets[0].asset_type == "aapp" &&
              sink.data_assets[0].component_tag == 0x1240 &&
              sink.data_assets[0].presentation_regions ==
                  std::vector<aribtlv::MpuPresentationRegion>{{9, 2, 1}},
          "MMT application data asset was not exposed");
    check(sink.signalling_messages.size() == 1 &&
              sink.signalling_messages[0].message_id == 0x0000 &&
              sink.signalling_messages[0].packet_id == 0xff02,
          "completed MMTP signalling message was not exposed");
    check(sink.tracks[0].codec == aribtlv::Codec::Hevc && sink.tracks[0].timescale == 180000,
          "HEVC metadata was not parsed from MPT descriptors");
    check(sink.tracks[0].video.has_value() &&
              sink.tracks[0].video->hdr_wcg_idc == 2 &&
              sink.tracks[0].video->video_transfer_characteristics == 5,
          "HEVC colour signalling was not parsed from MPT descriptors");
    check(sink.tracks[0].presentation_regions ==
              std::vector<aribtlv::MpuPresentationRegion>{{1, 2, 1}, {2, 3, 4}},
          "MPU presentation-region descriptor was not exposed on the track");
    check(sink.tracks[0].asset_groups ==
              std::vector<aribtlv::AssetGroupInfo>{{0x00, 0x01}},
          "asset group metadata was not exposed on the video track");
    check(sink.tracks[1].codec == aribtlv::Codec::AacLatm && sink.tracks[1].language == "jpn",
          "AAC-LATM metadata was not parsed from MPT descriptors");
    check(sink.tracks[1].audio.has_value() &&
              sink.tracks[1].audio->channel_layout == aribtlv::AudioChannelLayout::Stereo &&
              sink.tracks[1].component_tag == 0x0110 &&
              sink.tracks[1].audio->component_tag == 0x0110 &&
              sink.tracks[1].audio->main_component &&
              sink.tracks[1].audio->sample_rate == 48000,
          "MH audio component metadata was not exposed on the audio track");
    check(sink.tracks[1].asset_groups ==
              std::vector<aribtlv::AssetGroupInfo>{{0x10, 0x00}, {0x11, 0x01}},
          "multiple asset group descriptors were not preserved on the audio track");
    check(sink.tracks[2].codec == aribtlv::Codec::Ttml &&
              sink.tracks[2].component_tag == 0x1230,
          "TTML metadata was not parsed from MPT descriptors");
    check(sink.tracks[2].timescale == 65536,
          "TTML without a timestamp descriptor did not use short-NTP timescale");
    check(sink.tracks[2].subtitle.has_value() &&
              sink.tracks[2].subtitle->operation_mode == 2 &&
              sink.tracks[2].subtitle->timing_mode == 2 &&
              sink.tracks[2].subtitle->display_mode == 10 &&
              sink.tracks[2].subtitle->resolution == 1 &&
              sink.tracks[2].subtitle->start_mpu_sequence_number == 5 &&
              sink.tracks[2].subtitle->reference_start_ntp ==
                  std::optional<std::uint64_t>{(100ULL << 32U) | 0x80000000ULL} &&
              sink.tracks[2].subtitle->reference_start_time_leap_indicator == 1,
          "ARIB B62 subtitle timing metadata was not exposed on the subtitle track");

    const auto stable_id = sink.tracks[0].track_id;
    demuxer.reset();
    demuxer.push(data.data(), data.size());
    demuxer.flush();
    check(sink.tracks.size() == 6 && sink.tracks[3].track_id == stable_id &&
              sink.layouts.size() == 2,
          "reset changed a track's Demuxer-lifetime stable identity");
}

// ARIB STD-B60 Table 9-3's TMD == 0010 branch of Additional_Arib_Subtitle_Info()
// is 9 bytes (reference_start_time plus the leap indicator/reserved byte); here
// it is cut back to 8, one byte short of the standard.
std::vector<std::uint8_t> truncated_subtitle_message() {
    std::vector<std::uint8_t> subtitle_descriptors;
    descriptor(subtitle_descriptors, 0x8011, {0x12, 0x30});
    descriptor(subtitle_descriptors, 0x8020,
               {0x00, 0x20, 0x30, 0x08, 'j', 'p', 'n', 0x02, 0x2a, 0x10,
                0x00, 0x00, 0x00, 0x05,
                0x00, 0x00, 0x00, 0x64, 0x80, 0x00, 0x00, 0x00});

    std::vector<std::uint8_t> mpt_body{0xfc, 2, 0x00, 0x65};
    append_u16(mpt_body, 0);
    mpt_body.push_back(1);
    asset(mpt_body, 0xf330, "stpp", subtitle_descriptors);
    std::vector<std::uint8_t> mpt{0x20, 8};
    append_u16(mpt, mpt_body.size());
    mpt.insert(mpt.end(), mpt_body.begin(), mpt_body.end());

    std::vector<std::uint8_t> pa{0x00, 0x00, 0x00};
    append_u32(pa, 1 + mpt.size());
    pa.push_back(0);
    pa.insert(pa.end(), mpt.begin(), mpt.end());
    return pa;
}

std::vector<std::uint8_t> truncated_subtitle_stream() {
    const auto pa = truncated_subtitle_message();
    const auto mmtp = signalling_mmtp(1, 0, pa);
    std::vector<std::uint8_t> compressed_payload{0x00, 0x10, 0x61};
    compressed_payload.insert(compressed_payload.end(), mmtp.begin(), mmtp.end());
    return tlv(0x03, compressed_payload);
}

void test_truncated_subtitle_reference_start_time_is_rejected() {
    const auto data = truncated_subtitle_stream();
    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(data.data(), data.size());
    demuxer.flush();
    // parse_mpt() rejects the whole MPT on a malformed asset descriptor, so the
    // subtitle track is never installed and the PA message never reaches onSignallingMessage.
    check(sink.tracks.empty(),
          "subtitle track was exposed despite a truncated reference_start_time block");
    check(sink.signalling_messages.empty(),
          "MPT with a truncated subtitle descriptor was still reported as a valid signalling "
          "message");
    check(!sink.errors.empty() && sink.errors[0].code == aribtlv::ErrorCode::MalformedInput &&
              sink.errors[0].recoverable,
          "truncated reference_start_time block did not raise a recoverable parse error");
}

void test_service_selection_clears_layout_state() {
    const auto data = discovery_stream();
    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(data.data(), data.size());
    demuxer.flush();
    demuxer.selectService(1);
    demuxer.push(data.data(), data.size());
    demuxer.flush();
    check(sink.layouts.size() == 2,
          "service selection retained stale layout deduplication state");
}

void test_application_and_data_transmission_signalling() {
    auto stream = signalling_tlv(1, 0, application_control_message());
    const auto data_message = signalling_tlv(2, 0, data_transmission_message());
    stream.insert(stream.end(), data_message.begin(), data_message.end());

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(stream.data(), stream.size());
    demuxer.flush();
    check(sink.applications.size() == 1 &&
              sink.applications[0].application_type == 0x0011 &&
              sink.applications[0].organization_id == 0x1234 &&
              sink.applications[0].application_id == 0x01020304 &&
              sink.applications[0].control_code == 0x01 &&
              sink.applications[0].version == 3 &&
              sink.applications[0].current_next &&
              sink.applications[0].section_number == 0 &&
              sink.applications[0].last_section_number == 0 &&
              sink.applications[0].application_descriptor_present &&
              sink.applications[0].profiles.size() == 1 &&
              sink.applications[0].profiles[0].application_profile == 0x0001 &&
              sink.applications[0].profiles[0].version_major == 1 &&
              sink.applications[0].profiles[0].version_minor == 2 &&
              sink.applications[0].profiles[0].version_micro == 3 &&
              sink.applications[0].service_bound &&
              sink.applications[0].visibility == 0x03 &&
              sink.applications[0].present_application_priority &&
              sink.applications[0].application_priority == 0x7f &&
              sink.applications[0].transport_protocol_labels ==
                  std::vector<std::uint8_t>{0x05} &&
              sink.applications[0].entry_path == "index.html" &&
              sink.applications[0].transports.size() == 2 &&
              sink.applications[0].transports[0].label == 0x05 &&
              sink.applications[0].transport_urls.size() == 1 &&
              sink.applications[0].transport_urls[0] == "/app/",
          "MH-AIT application identity, control, and location were not parsed");
    check(sink.data_transmission_tables.size() == 1 &&
              sink.data_transmission_tables[0].table_id == 0xa3 &&
              sink.data_transmission_tables[0].session_id == 0x2a &&
              sink.data_transmission_tables[0].version == 4 &&
              sink.data_transmission_tables[0].data.size() == 15,
          "data transmission table metadata was not exposed");
    check(sink.signalling_messages.size() == 2 &&
              sink.signalling_messages[0].message_id == 0x8000 &&
              sink.signalling_messages[1].message_id == 0x8003,
          "application signalling messages were not exposed after typed parsing");
}

void test_mpt_snapshot_removes_missing_service_state() {
    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    const auto initial = discovery_stream();
    demuxer.push(initial.data(), initial.size());
    const auto ait = signalling_tlv(2, 0, application_control_message());
    demuxer.push(ait.data(), ait.size());
    check(sink.mpt_snapshots.size() == 1 && sink.applications.size() == 1,
          "initial MPT/MH-AIT snapshots were not committed once");

    const auto replacement = signalling_tlv(3, 0, video_discovery_message(1));
    demuxer.push(replacement.data(), replacement.size());
    check(sink.mpt_snapshots.size() == 2 && sink.removed_tracks.size() == 2 &&
              sink.removed_data_assets.size() == 1 &&
              sink.removed_application_services.size() == 1 &&
              sink.removed_applications.size() == 1,
          "complete MPT replacement did not retire every missing item atomically");

    const auto stale_ait = signalling_tlv(
        4, 0, application_control_message(0, 0, 4, true, 0x0011, 0x02));
    demuxer.push(stale_ait.data(), stale_ait.size());
    check(sink.mh_ait_snapshots.size() == 1 && sink.applications.size() == 1,
          "MPT descriptor removal did not stop the old MH-AIT route");
}

void test_mh_ait_snapshot_completion_empty_and_reposition() {
    TestSink sink;
    aribtlv::Demuxer demuxer(sink);

    auto arib_single = signalling_tlv(
        1, 0, application_control_message(1, 1, 10));
    const auto arib_repeat = signalling_tlv(
        2, 0, application_control_message(1, 1, 10));
    arib_single.insert(arib_single.end(), arib_repeat.begin(), arib_repeat.end());
    demuxer.push(arib_single.data(), arib_single.size());
    check(sink.mh_ait_snapshots.size() == 1 && sink.applications.size() == 1 &&
              sink.mh_ait_snapshots.back().applications.size() == 1,
          "ARIB-HTML5 section 1/1 was incorrectly left waiting for section 0 (snapshots=" +
              std::to_string(sink.mh_ait_snapshots.size()) + ", applications=" +
              std::to_string(sink.applications.size()) + ", errors=" +
              std::to_string(sink.errors.size()) + ", signalling=" +
              std::to_string(sink.signalling_messages.size()) + ")");

    const auto empty = signalling_tlv(
        3, 0, application_control_message(1, 1, 11, false));
    demuxer.push(empty.data(), empty.size());
    check(sink.mh_ait_snapshots.size() == 2 &&
              sink.mh_ait_snapshots.back().applications.empty() &&
              sink.removed_applications.size() == 1,
          "empty MH-AIT snapshot did not retire the preceding application");

    const auto generic_last = signalling_tlv(
        4, 0, application_control_message(1, 1, 12, true, 0x0010));
    demuxer.push(generic_last.data(), generic_last.size());
    check(sink.mh_ait_snapshots.size() == 2,
          "generic multi-section MH-AIT committed without section 0");
    const auto generic_first = signalling_tlv(
        5, 0, application_control_message(0, 1, 12, false, 0x0010));
    demuxer.push(generic_first.data(), generic_first.size());
    check(sink.mh_ait_snapshots.size() == 3 &&
              sink.mh_ait_snapshots.back().applications.size() == 1,
          "out-of-order complete MH-AIT sub-table was not committed atomically");

    demuxer.reposition(aribtlv::RepositionOptions{0, true});
    auto historical = signalling_tlv(
        1, 0, application_control_message(1, 1, 2, true, 0x0011, 0x02));
    const auto historical_repeat = signalling_tlv(
        2, 0, application_control_message(1, 1, 2, true, 0x0011, 0x02));
    historical.insert(historical.end(), historical_repeat.begin(), historical_repeat.end());
    demuxer.push(historical.data(), historical.size());
    check(sink.mh_ait_snapshots.size() == 4 &&
              sink.mh_ait_snapshots.back().version == 2 &&
              sink.applications.back().control_code == 0x02,
          "first complete snapshot after reposition rejected a historical version rollback");
}

void test_service_state_reset_notifications() {
    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.selectService(1);
    demuxer.reset();
    check(sink.service_resets.size() == 2 &&
              sink.service_resets[0].reason ==
                  aribtlv::ServiceStateResetReason::ServiceSelection &&
              sink.service_resets[1].reason ==
                  aribtlv::ServiceStateResetReason::FullReset,
          "service selection/full reset did not expose explicit reset ownership");
}

void test_dynamic_audio_layout_metadata() {
    const auto pa = audio_discovery_message();
    auto data = signalling_tlv(1, 0, pa);
    auto updated_pa = pa;
    const std::vector<std::uint8_t> first_audio_descriptor{0x80, 0x14, 0x0a, 0xf3, 0x11};
    const auto updated_component = std::search(updated_pa.begin(), updated_pa.end(),
                                               first_audio_descriptor.begin(),
                                               first_audio_descriptor.end());
    check(updated_component != updated_pa.end(), "audio update fixture has no 22.2ch descriptor");
    updated_component[4] = 0x09;
    const auto update = signalling_tlv(2, 0, updated_pa);
    data.insert(data.end(), update.begin(), update.end());

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(data.data(), data.size());
    demuxer.flush();
    check(sink.tracks.size() == 4,
          "three signalled audio tracks plus one metadata update were not reported");

    const auto find_layout = [&](const aribtlv::AudioChannelLayout layout) {
        return std::find_if(sink.tracks.begin(), sink.tracks.end(), [&](const auto& track) {
            return track.audio.has_value() && track.audio->channel_layout == layout;
        });
    };
    const auto surround22 = find_layout(aribtlv::AudioChannelLayout::Channels22_2);
    const auto surround51 = find_layout(aribtlv::AudioChannelLayout::Channels5_1);
    const auto stereo = find_layout(aribtlv::AudioChannelLayout::Stereo);
    check(surround22 != sink.tracks.end() && surround22->packet_id == 0xe210 &&
              surround22->component_tag == 0x0110 &&
              surround22->audio->component_tag == 0x0110 &&
              surround22->audio->main_component,
          "22.2ch track was not identified from its descriptor metadata");
    check(surround51 != sink.tracks.end() && surround51->packet_id == 0xe275 &&
              !surround51->audio->main_component,
          "5.1ch track was not identified independently of its packet ID");
    check(stereo != sink.tracks.end() && stereo->packet_id == 0xe2aa &&
              stereo->audio->es_multi_lingual &&
              stereo->audio->secondary_language == "eng",
          "stereo/multilingual track metadata was not parsed completely");
    check(sink.tracks.back().packet_id == 0xe210 &&
              sink.tracks.back().track_id == surround22->track_id &&
              sink.tracks.back().audio->channel_layout ==
                  aribtlv::AudioChannelLayout::Channels5_1,
          "audio descriptor update did not preserve track identity and emit replacement metadata");
}

std::vector<std::uint8_t> mmtp_packet(const std::uint16_t packet_id,
                                      const std::uint32_t packet_sequence,
                                      const std::uint32_t delivery_timestamp,
                                      const bool random_access,
                                      const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> result{
        static_cast<std::uint8_t>(random_access ? 1 : 0), 0x00,
        static_cast<std::uint8_t>(packet_id >> 8U), static_cast<std::uint8_t>(packet_id),
    };
    append_u32(result, delivery_timestamp);
    append_u32(result, packet_sequence);
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

std::vector<std::uint8_t> authenticated_mmtp_packet(
    const std::uint16_t packet_id, const std::uint32_t packet_sequence,
    const std::uint32_t delivery_timestamp, const bool random_access,
    const std::vector<std::uint8_t>& payload, const std::uint16_t declared_payload_size) {
    auto plain = mmtp_packet(packet_id, packet_sequence, delivery_timestamp,
                             random_access, payload);
    std::vector<std::uint8_t> result(plain.begin(), plain.begin() + 12);
    result[0] = static_cast<std::uint8_t>(result[0] | 0x02U);
    result.insert(result.end(), {
        0x00, 0x00, 0x00, 0x07, // multi-type extension
        0x80, 0x01, 0x00, 0x03, // final B61 extension, three-byte field
        0x02,                    // message authentication present
        static_cast<std::uint8_t>(declared_payload_size >> 8U),
        static_cast<std::uint8_t>(declared_payload_size),
    });
    result.insert(result.end(), payload.begin(), payload.end());
    result.insert(result.end(), {0xaa, 0xbb, 0xcc, 0xdd});
    return result;
}

std::vector<std::uint8_t> mpu_payload(const std::uint32_t mpu_sequence,
                                      const std::vector<std::uint8_t>& mfu,
                                      const std::uint32_t sample_number = 0) {
    std::vector<std::uint8_t> result;
    append_u16(result, 6 + 14 + mfu.size());
    result.push_back(0x28);
    result.push_back(0);
    append_u32(result, mpu_sequence);
    append_u32(result, 0);
    append_u32(result, sample_number);
    append_u32(result, 0);
    result.push_back(0);
    result.push_back(0);
    result.insert(result.end(), mfu.begin(), mfu.end());
    return result;
}

// Non-timed MFU: fragment_type=2, timed=0. Its 4-byte header carries an opaque
// item_id, not an MMT sample_number, per the MMTP/MPU wire format.
std::vector<std::uint8_t> non_timed_mpu_payload(const std::uint32_t mpu_sequence,
                                                const std::vector<std::uint8_t>& mfu,
                                                const std::uint32_t item_id = 0) {
    std::vector<std::uint8_t> result;
    append_u16(result, 6 + 4 + mfu.size());
    result.push_back(0x20);
    result.push_back(0);
    append_u32(result, mpu_sequence);
    append_u32(result, item_id);
    result.insert(result.end(), mfu.begin(), mfu.end());
    return result;
}

std::vector<std::uint8_t> fragmented_mpu_payload(const std::uint32_t mpu_sequence,
                                                 const std::uint8_t fragmentation,
                                                 const std::vector<std::uint8_t>& piece) {
    auto result = mpu_payload(mpu_sequence, piece);
    result[2] = static_cast<std::uint8_t>(0x28U | (fragmentation << 1U));
    return result;
}

std::vector<std::uint8_t> tlv_for_mmtp(const std::uint16_t context_id,
                                       const std::vector<std::uint8_t>& mmtp) {
    std::vector<std::uint8_t> payload{
        static_cast<std::uint8_t>((context_id << 4U) >> 8U),
        static_cast<std::uint8_t>(context_id << 4U), 0x61,
    };
    payload.insert(payload.end(), mmtp.begin(), mmtp.end());
    return tlv(0x03, payload);
}

void test_authenticated_mmtp_payload_bounds() {
    const auto media = mpu_payload(1, {0x11, 0x22});
    auto valid_stream = discovery_stream();
    const auto valid_packet = tlv_for_mmtp(
        1, authenticated_mmtp_packet(0xf310, 1, 100U << 16U, true,
                                     media, static_cast<std::uint16_t>(media.size())));
    valid_stream.insert(valid_stream.end(), valid_packet.begin(), valid_packet.end());

    TestSink valid_sink;
    aribtlv::Demuxer valid_demuxer(valid_sink);
    valid_demuxer.push(valid_stream.data(), valid_stream.size());
    valid_demuxer.flush();
    check(std::any_of(valid_sink.access_units.begin(), valid_sink.access_units.end(),
                      [](const auto& unit) {
                          return unit.codec == aribtlv::Codec::AacLatm;
                      }),
          "B61 message-authentication code was treated as MMTP media payload");

    auto invalid_stream = discovery_stream();
    const auto invalid_packet = tlv_for_mmtp(
        1, authenticated_mmtp_packet(0xf310, 1, 100U << 16U, true,
                                     media, static_cast<std::uint16_t>(media.size() + 32)));
    invalid_stream.insert(invalid_stream.end(), invalid_packet.begin(), invalid_packet.end());
    TestSink invalid_sink;
    aribtlv::Demuxer invalid_demuxer(invalid_sink);
    invalid_demuxer.push(invalid_stream.data(), invalid_stream.size());
    invalid_demuxer.flush();
    check(std::any_of(invalid_sink.errors.begin(), invalid_sink.errors.end(),
                      [](const auto& error) {
                          return error.code == aribtlv::ErrorCode::MalformedInput;
                      }),
          "out-of-bounds authenticated MMTP payload length was accepted");
}

void append_video_access_unit(std::vector<std::uint8_t>& stream,
                              const std::uint32_t first_packet_sequence) {
    const auto add_video = [&](const std::uint32_t sequence, const bool rap,
                               const std::vector<std::uint8_t>& mfu) {
        const auto packet = tlv_for_mmtp(
            1, mmtp_packet(0xf300, sequence, 100U << 16U, rap, mpu_payload(1, mfu)));
        stream.insert(stream.end(), packet.begin(), packet.end());
    };
    add_video(first_packet_sequence, true, {0, 0, 0, 2, 0x46, 0x01});
    add_video(first_packet_sequence + 1, false, {0, 0, 0, 3, 0x02, 0x01, 0x80});
    add_video(first_packet_sequence + 2, false, {0, 0, 0, 2, 0x46, 0x01});
}

void test_recording_scanner_uses_demux_metadata_and_bounds_time() {
    auto stream = discovery_stream();
    append_video_access_unit(stream, 1);

    aribtlv::RecordingScanner scanner;
    check(scanner.push(stream.data(), stream.size()),
          "recording scanner rejected a valid recording");
    const auto& result = scanner.finish();
    check(result.complete() && result.video_packet_id == 0xf300 &&
              result.first_presentation_time.has_value() &&
              result.last_presentation_time.has_value() &&
              result.seek_points.size() == 1,
          "recording scanner did not expose the selected video timeline and RAP");
    check(scanner.seekFromStart({0, 1000000}).has_value(),
          "recording scanner could not locate the recording start RAP");
    check(!scanner.seekFromStart({1, 1000000}).has_value(),
          "recording scanner returned the final RAP for a target beyond the recording end");

    aribtlv::RecordingScanOptions options;
    options.video_packet_id = 0xf301;
    aribtlv::RecordingScanner wrong_video(options);
    check(wrong_video.push(stream.data(), stream.size()) &&
              wrong_video.finish().failure == aribtlv::RecordingScanFailure::NoVideo,
          "recording scanner ignored the requested video packet id");
}

void test_codec_output_and_timeline() {
    auto stream = discovery_stream();
    auto add_media = [&](const std::uint16_t packet_id, const std::uint32_t packet_sequence,
                         const bool rap, const std::vector<std::uint8_t>& mfu) {
        const auto packet = tlv_for_mmtp(
            1, mmtp_packet(packet_id, packet_sequence, 100U << 16U, rap,
                           mpu_payload(1, mfu)));
        stream.insert(stream.end(), packet.begin(), packet.end());
    };

    add_media(0xf300, 1, true, {0, 0, 0, 2, 0x46, 0x01});
    add_media(0xf300, 2, false, {0, 0, 0, 3, 0x02, 0x01, 0x80});
    add_media(0xf300, 3, false, {0, 0, 0, 2, 0x46, 0x01});
    add_media(0xf310, 1, true, {0x11, 0x22});
    add_media(0xf310, 2, false, {0x33, 0x44});
    add_media(0xf330, 1, true, {0x30, 0x01, 0x00, 0x01, 0x04, 0x00, 0x03,
                                0x10, 0x00, 0x03, 'a', 'b', 'c'});
    add_media(0xf330, 2, false, {0x30, 0x01, 0x01, 0x01, 0x10, 0x00, 0x03,
                                 'd', 'e', 'f'});

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(stream.data(), stream.size());
    demuxer.flush();

    check(sink.access_units.size() >= 4, "supported codec MFUs did not produce access units");
    const auto video = std::find_if(sink.access_units.begin(), sink.access_units.end(), [](const auto& unit) {
        return unit.codec == aribtlv::Codec::Hevc;
    });
    const auto audio = std::find_if(sink.access_units.begin(), sink.access_units.end(), [](const auto& unit) {
        return unit.codec == aribtlv::Codec::AacLatm;
    });
    const auto subtitle = std::find_if(sink.access_units.begin(), sink.access_units.end(), [](const auto& unit) {
        return unit.codec == aribtlv::Codec::Ttml;
    });
    check(video != sink.access_units.end() &&
              video->data == std::vector<std::uint8_t>({0, 0, 1, 0x46, 0x01,
                                                        0, 0, 1, 0x02, 0x01, 0x80}),
          "HEVC MFUs were not assembled into one Annex-B access unit");
    check(audio != sink.access_units.end() &&
              audio->data == std::vector<std::uint8_t>({0x56, 0xe0, 0x02, 0x11, 0x22}),
          "AAC MFU was not wrapped in a valid LOAS header");
    const auto second_audio = std::find_if(audio + 1, sink.access_units.end(), [](const auto& unit) {
        return unit.codec == aribtlv::Codec::AacLatm;
    });
    check(second_audio != sink.access_units.end() && second_audio->pts.value == 3000,
          "multi-AU timestamp offsets were not applied in presentation order");
    check(subtitle != sink.access_units.end() &&
              subtitle->data == std::vector<std::uint8_t>({'a', 'b', 'c'}),
          "TTML document was not separated from its resource subsamples");
    check(subtitle != sink.access_units.end() && subtitle->component_tag == 0x1230 &&
              subtitle->subtitle_timing_mode == std::optional<std::uint8_t>{2} &&
              subtitle->subtitle_operation_mode == std::optional<std::uint8_t>{2} &&
              subtitle->subtitle_display_mode == std::optional<std::uint8_t>{10} &&
              subtitle->subtitle_compression_type == std::optional<std::uint8_t>{0},
          "TTML access unit omitted component, timing, or B60 control metadata");
    check(subtitle != sink.access_units.end() && subtitle->subtitle_resources.size() == 1 &&
              subtitle->subtitle_resources[0].subsample_number == 1 &&
              subtitle->subtitle_resources[0].data_type == 1 &&
              subtitle->subtitle_resources[0].data == std::vector<std::uint8_t>({'d', 'e', 'f'}),
          "TTML resource subsample metadata was not preserved");
    check(video->pts.value == 0 && video->dts.value == 0,
          "first selected media timestamp was not normalized to zero");

    TestSink passthrough_sink;
    aribtlv::Demuxer passthrough_demuxer(passthrough_sink);
    passthrough_demuxer.selectTrack(aribtlv::TrackKind::Subtitle,
                                    std::numeric_limits<std::uint64_t>::max());
    passthrough_demuxer.setSubtitlePassthroughEnabled(true);
    passthrough_demuxer.push(stream.data(), stream.size());
    passthrough_demuxer.flush();
    check(std::any_of(passthrough_sink.access_units.begin(),
                      passthrough_sink.access_units.end(), [](const auto& unit) {
                          return unit.codec == aribtlv::Codec::Ttml;
                      }),
          "subtitle passthrough did not bypass selected-track filtering");
}

void test_timestamp_overflow_rejection() {
    auto pa = discovery_message();
    const std::vector<std::uint8_t> timestamp_pattern{0x00, 0x01, 0x0c, 0, 0, 0, 1};
    const auto first_timestamp = std::search(pa.begin(), pa.end(),
                                             timestamp_pattern.begin(), timestamp_pattern.end());
    check(first_timestamp != pa.end(), "test fixture has no video timestamp descriptor");
    const auto second_timestamp = std::search(first_timestamp + 1, pa.end(),
                                              timestamp_pattern.begin(), timestamp_pattern.end());
    check(second_timestamp != pa.end(), "test fixture has no audio timestamp descriptor");
    const auto timestamp_index = static_cast<std::size_t>(second_timestamp - pa.begin());
    for (std::size_t index = 0; index < 4; ++index) pa[timestamp_index + 7 + index] = 0xff;
    for (std::size_t index = 4; index < 8; ++index) pa[timestamp_index + 7 + index] = 0x00;

    const std::vector<std::uint8_t> extended_tag{0x80, 0x26};
    const auto audio_extended = std::search(second_timestamp, pa.end(),
                                            extended_tag.begin(), extended_tag.end());
    check(audio_extended != pa.end(), "test fixture has no audio extended timestamp descriptor");
    const auto extended_index = static_cast<std::size_t>(audio_extended - pa.begin());
    for (std::size_t index = 0; index < 4; ++index) pa[extended_index + 4 + index] = 0xff;

    auto stream = signalling_tlv(1, 0, pa);
    const auto repeated_signalling = signalling_tlv(2, 0, pa);
    stream.insert(stream.end(), repeated_signalling.begin(), repeated_signalling.end());
    auto add_media = [&](const std::uint16_t packet_id, const std::uint32_t sequence,
                         const bool rap, const std::vector<std::uint8_t>& mfu) {
        const auto packet = tlv_for_mmtp(
            1, mmtp_packet(packet_id, sequence, 100U << 16U, rap, mpu_payload(1, mfu)));
        stream.insert(stream.end(), packet.begin(), packet.end());
    };
    add_media(0xf300, 1, true, {0, 0, 0, 2, 0x46, 0x01});
    add_media(0xf300, 2, false, {0, 0, 0, 3, 0x02, 0x01, 0x80});
    add_media(0xf300, 3, false, {0, 0, 0, 2, 0x46, 0x01});
    add_media(0xf310, 1, true, {0x11, 0x22});

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(stream.data(), stream.size());
    demuxer.flush();
    check(std::count_if(sink.access_units.begin(), sink.access_units.end(), [](const auto& unit) {
              return unit.codec == aribtlv::Codec::AacLatm;
          }) == 0 &&
              std::any_of(sink.errors.begin(), sink.errors.end(), [](const auto& error) {
                  return error.code == aribtlv::ErrorCode::Discontinuity;
              }),
          "timestamp normalization overflow was not rejected recoverably");
}

void test_track_selection_clears_incomplete_media() {
    const auto discovery = discovery_stream();
    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(discovery.data(), discovery.size());
    demuxer.flush();
    const auto video_track = sink.tracks[0].track_id;

    const auto first_fragment = tlv_for_mmtp(
        1, mmtp_packet(0xf300, 1, 100U << 16U, true,
                       fragmented_mpu_payload(1, 1, {0, 0, 0, 3, 0x02})));
    const auto boundary = tlv(0xff, {});
    auto partial_stream = first_fragment;
    partial_stream.insert(partial_stream.end(), boundary.begin(), boundary.end());
    demuxer.push(partial_stream.data(), partial_stream.size());

    demuxer.selectTrack(aribtlv::TrackKind::Video, video_track);

    const auto stale_last = tlv_for_mmtp(
        1, mmtp_packet(0xf300, 2, 100U << 16U, false,
                       fragmented_mpu_payload(1, 3, {0x01, 0x80})));
    auto stale_stream = stale_last;
    stale_stream.insert(stale_stream.end(), boundary.begin(), boundary.end());
    demuxer.push(stale_stream.data(), stale_stream.size());

    const auto next_mpu_signalling = signalling_tlv(
        10, 0, video_discovery_message(2));
    demuxer.push(next_mpu_signalling.data(), next_mpu_signalling.size());
    auto add_video = [&](const std::uint32_t sequence, const bool rap,
                         const std::vector<std::uint8_t>& mfu) {
        const auto packet = tlv_for_mmtp(
            1, mmtp_packet(0xf300, sequence, 100U << 16U, rap, mpu_payload(2, mfu)));
        demuxer.push(packet.data(), packet.size());
    };
    add_video(10, true, {0, 0, 0, 2, 0x46, 0x01});
    add_video(11, false, {0, 0, 0, 3, 0x02, 0x01, 0x80});
    add_video(12, false, {0, 0, 0, 2, 0x46, 0x01});
    demuxer.flush();

    check(std::count_if(sink.access_units.begin(), sink.access_units.end(), [](const auto& unit) {
              return unit.codec == aribtlv::Codec::Hevc;
          }) == 1,
          "track selection retained stale fragmented media or failed to resume at a fresh RAP");
}

void test_fragmented_signalling_restart_offset() {
    const auto pa = discovery_message();
    const auto first_end = pa.size() / 3;
    const auto middle_end = first_end * 2;
    const auto prefix = tlv(0xff, {});
    auto stream = prefix;
    const auto first = signalling_tlv(
        10, 0x40,
        std::vector<std::uint8_t>(pa.begin(),
                                  pa.begin() + static_cast<std::ptrdiff_t>(first_end)));
    const auto middle = signalling_tlv(
        11, 0x80,
        std::vector<std::uint8_t>(pa.begin() + static_cast<std::ptrdiff_t>(first_end),
                                  pa.begin() + static_cast<std::ptrdiff_t>(middle_end)));
    const auto last = signalling_tlv(
        12, 0xc0,
        std::vector<std::uint8_t>(pa.begin() + static_cast<std::ptrdiff_t>(middle_end),
                                  pa.end()));
    stream.insert(stream.end(), first.begin(), first.end());
    stream.insert(stream.end(), middle.begin(), middle.end());
    stream.insert(stream.end(), last.begin(), last.end());

    auto add_video = [&](const std::uint32_t sequence, const bool rap,
                         const std::vector<std::uint8_t>& mfu) {
        const auto packet = tlv_for_mmtp(
            1, mmtp_packet(0xf300, sequence, 100U << 16U, rap, mpu_payload(1, mfu)));
        stream.insert(stream.end(), packet.begin(), packet.end());
    };
    add_video(1, true, {0, 0, 0, 2, 0x46, 0x01});
    add_video(2, false, {0, 0, 0, 3, 0x02, 0x01, 0x80});
    add_video(3, false, {0, 0, 0, 2, 0x46, 0x01});

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(stream.data(), stream.size());
    demuxer.flush();
    const auto video = std::find_if(sink.access_units.begin(), sink.access_units.end(),
                                    [](const auto& unit) {
                                        return unit.codec == aribtlv::Codec::Hevc;
                                    });
    check(video != sink.access_units.end() && video->restart_offset == prefix.size() &&
              video->input_offset > video->restart_offset,
          "AU restart offset did not retain the first fragmented signalling packet");
}

void test_reposition_preserves_timeline_and_absolute_offsets() {
    auto initial = discovery_stream();
    append_video_access_unit(initial, 1);

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(initial.data(), initial.size());
    demuxer.flush();
    const auto initial_video = std::find_if(
        sink.access_units.begin(), sink.access_units.end(), [](const auto& unit) {
            return unit.codec == aribtlv::Codec::Hevc;
        });
    check(initial_video != sink.access_units.end() && initial_video->pts.value == 0,
          "initial video did not establish the recording timeline");
    const auto initial_video_index =
        static_cast<std::size_t>(initial_video - sink.access_units.begin());
    const auto original_track_callbacks = sink.tracks.size();

    auto shifted_pa = discovery_message();
    const std::vector<std::uint8_t> timestamp_pattern{0x00, 0x01, 0x0c, 0, 0, 0, 1};
    const auto video_timestamp = std::search(shifted_pa.begin(), shifted_pa.end(),
                                             timestamp_pattern.begin(), timestamp_pattern.end());
    check(video_timestamp != shifted_pa.end(), "shifted fixture has no video timestamp");
    const auto ntp_index = static_cast<std::size_t>(video_timestamp - shifted_pa.begin()) + 7;
    shifted_pa[ntp_index + 0] = 0;
    shifted_pa[ntp_index + 1] = 0;
    shifted_pa[ntp_index + 2] = 0;
    shifted_pa[ntp_index + 3] = 101;
    shifted_pa[ntp_index + 4] = 0;
    shifted_pa[ntp_index + 5] = 0;
    shifted_pa[ntp_index + 6] = 0;
    shifted_pa[ntp_index + 7] = 0;

    auto shifted = signalling_tlv(100, 0, shifted_pa);
    const auto repeated = signalling_tlv(101, 0, shifted_pa);
    const auto latest_checkpoint_offset = static_cast<std::uint64_t>(shifted.size());
    shifted.insert(shifted.end(), repeated.begin(), repeated.end());
    append_video_access_unit(shifted, 1000);

    constexpr std::uint64_t source_offset = 500000;
    demuxer.reposition(aribtlv::RepositionOptions{source_offset, true});
    demuxer.push(shifted.data(), shifted.size());
    demuxer.flush();

    const auto second_video = std::find_if(
        sink.access_units.begin() + static_cast<std::ptrdiff_t>(initial_video_index + 1),
        sink.access_units.end(), [](const auto& unit) {
            return unit.codec == aribtlv::Codec::Hevc;
        });
    check(second_video != sink.access_units.end() && second_video->pts.value == 180000,
          "reposition reset the recording timeline instead of preserving it");
    check(second_video->restart_offset == source_offset + latest_checkpoint_offset &&
              second_video->input_offset > second_video->restart_offset,
          "reposition did not preserve absolute source offsets");
    check(second_video->discontinuity,
          "first access unit after reposition was not marked discontinuous");
    check(second_video->discontinuity_reasons ==
              aribtlv::DiscontinuityReason::Reposition &&
              sink.damage_spans.empty(),
          "reposition was incorrectly reported as damaged source media");
    check(sink.tracks.size() == original_track_callbacks,
          "reposition re-emitted unchanged track metadata");
}

void test_track_selection_preserves_timeline_and_waits_for_rap() {
    auto initial = discovery_stream();
    append_video_access_unit(initial, 1);

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(initial.data(), initial.size());
    demuxer.flush();
    const auto initial_video = std::find_if(
        sink.access_units.begin(), sink.access_units.end(), [](const auto& unit) {
            return unit.codec == aribtlv::Codec::Hevc;
        });
    check(initial_video != sink.access_units.end() && initial_video->pts.value == 0,
          "initial video did not establish the track-switch timeline");
    const auto initial_video_index =
        static_cast<std::size_t>(initial_video - sink.access_units.begin());
    const auto video_track = initial_video->track_id;

    demuxer.selectTrack(aribtlv::TrackKind::Video, video_track);

    auto shifted_pa = discovery_message();
    const std::vector<std::uint8_t> timestamp_pattern{0x00, 0x01, 0x0c, 0, 0, 0, 1};
    const auto video_timestamp = std::search(shifted_pa.begin(), shifted_pa.end(),
                                             timestamp_pattern.begin(), timestamp_pattern.end());
    check(video_timestamp != shifted_pa.end(), "track-switch fixture has no video timestamp");
    const auto ntp_index = static_cast<std::size_t>(video_timestamp - shifted_pa.begin()) + 7;
    shifted_pa[ntp_index + 0] = 0;
    shifted_pa[ntp_index + 1] = 0;
    shifted_pa[ntp_index + 2] = 0;
    shifted_pa[ntp_index + 3] = 101;
    shifted_pa[ntp_index + 4] = 0;
    shifted_pa[ntp_index + 5] = 0;
    shifted_pa[ntp_index + 6] = 0;
    shifted_pa[ntp_index + 7] = 0;

    auto shifted = signalling_tlv(100, 0, shifted_pa);
    append_video_access_unit(shifted, 1000);
    demuxer.push(shifted.data(), shifted.size());
    demuxer.flush();

    const auto selected_video = std::find_if(
        sink.access_units.begin() + static_cast<std::ptrdiff_t>(initial_video_index + 1),
        sink.access_units.end(), [](const auto& unit) {
            return unit.codec == aribtlv::Codec::Hevc;
        });
    check(selected_video != sink.access_units.end() &&
              selected_video->track_id == video_track &&
              selected_video->pts.value == 180000 &&
              selected_video->random_access && selected_video->discontinuity,
          "video track selection reset the timeline or did not resume at a discontinuous RAP");
    check(aribtlv::hasDiscontinuityReason(
              selected_video->discontinuity_reasons,
              aribtlv::DiscontinuityReason::TrackSelection),
          "track selection did not retain its controlled discontinuity reason");
}

void test_hevc_irap_detection_without_mmtp_rap() {
    auto stream = discovery_stream();
    const auto add_video = [&](const std::uint32_t sequence,
                               const std::vector<std::uint8_t>& mfu) {
        const auto packet = tlv_for_mmtp(
            1, mmtp_packet(0xf300, sequence, 100U << 16U, false, mpu_payload(1, mfu)));
        stream.insert(stream.end(), packet.begin(), packet.end());
    };
    add_video(1, {0, 0, 0, 2, 0x46, 0x01});
    add_video(2, {0, 0, 0, 3, 0x26, 0x01, 0x80});
    add_video(3, {0, 0, 0, 2, 0x46, 0x01});

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(stream.data(), stream.size());
    demuxer.flush();
    const auto video = std::find_if(
        sink.access_units.begin(), sink.access_units.end(), [](const auto& unit) {
            return unit.codec == aribtlv::Codec::Hevc;
        });
    check(video != sink.access_units.end() && video->random_access,
          "HEVC IRAP NAL was not exposed as a random-access AU without MMTP RAP");
}

void test_reposition_drops_orphan_hevc_irap_continuation() {
    auto initial = discovery_stream();
    append_video_access_unit(initial, 1);

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(initial.data(), initial.size());
    demuxer.flush();
    const auto initial_access_unit_count = sink.access_units.size();

    auto restarted = discovery_stream();
    const auto add_video = [&](const std::uint32_t sequence, const bool rap,
                               const std::vector<std::uint8_t>& mfu) {
        const auto packet = tlv_for_mmtp(
            1, mmtp_packet(0xf300, sequence, 100U << 16U, rap,
                           mpu_payload(1, mfu)));
        restarted.insert(restarted.end(), packet.begin(), packet.end());
    };

    // A checkpoint may be inside a large IRAP picture. This type-21 CRA NAL is
    // a continuation slice, so neither its NAL type nor an MMTP RAP flag makes
    // it the head of a decodable random-access point.
    add_video(1, true, {0, 0, 0, 3, 0x2a, 0x01, 0x00});
    add_video(2, false, {0, 0, 0, 2, 0x50, 0x01}); // suffix SEI of old picture
    add_video(3, false, {0, 0, 0, 3, 0x2a, 0x01, 0x00});
    const auto complete_picture_offset = static_cast<std::uint64_t>(restarted.size());
    add_video(4, false, {0, 0, 0, 2, 0x4e, 0x01}); // prefix SEI of new picture
    add_video(5, false, {0, 0, 0, 3, 0x2a, 0x01, 0x80});

    constexpr std::uint64_t source_offset = 500000;
    demuxer.reposition(aribtlv::RepositionOptions{source_offset, true});
    demuxer.push(restarted.data(), restarted.size());
    demuxer.flush();

    const auto first_restarted = sink.access_units.begin() +
        static_cast<std::ptrdiff_t>(initial_access_unit_count);
    const auto restarted_video_count = std::count_if(
        first_restarted, sink.access_units.end(), [](const auto& unit) {
            return unit.codec == aribtlv::Codec::Hevc;
        });
    const auto video = std::find_if(
        first_restarted, sink.access_units.end(), [](const auto& unit) {
            return unit.codec == aribtlv::Codec::Hevc;
        });
    check(restarted_video_count == 1 && video != sink.access_units.end(),
          "reposition emitted an orphan HEVC IRAP continuation as an access unit");
    check(video->data == std::vector<std::uint8_t>(
              {0, 0, 1, 0x4e, 0x01, 0, 0, 1, 0x2a, 0x01, 0x80}) &&
              video->random_access && video->discontinuity &&
              video->input_offset == source_offset + complete_picture_offset,
          "reposition did not resume at the first complete HEVC IRAP picture");
}

void test_access_unit_restart_offset_is_snapshotted() {
    const auto pa = discovery_message();
    auto stream = signalling_tlv(1, 0, pa);
    const auto first_checkpoint = static_cast<std::uint64_t>(0);
    const auto add_video = [&](const std::uint32_t sequence,
                               const std::vector<std::uint8_t>& mfu) {
        const auto packet = tlv_for_mmtp(
            1, mmtp_packet(0xf300, sequence, 100U << 16U, false, mpu_payload(1, mfu)));
        stream.insert(stream.end(), packet.begin(), packet.end());
    };
    add_video(1, {0, 0, 0, 2, 0x46, 0x01});
    add_video(2, {0, 0, 0, 3, 0x26, 0x01, 0x80});
    const auto later_signalling_offset = static_cast<std::uint64_t>(stream.size());
    const auto later_signalling = signalling_tlv(2, 0, pa);
    stream.insert(stream.end(), later_signalling.begin(), later_signalling.end());
    add_video(3, {0, 0, 0, 2, 0x46, 0x01});

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(stream.data(), stream.size());
    demuxer.flush();
    const auto video = std::find_if(
        sink.access_units.begin(), sink.access_units.end(), [](const auto& unit) {
            return unit.codec == aribtlv::Codec::Hevc;
        });
    check(video != sink.access_units.end() &&
              video->restart_offset == first_checkpoint &&
              video->input_offset < later_signalling_offset,
          "AU used signalling received after the AU began as its restart checkpoint");
}

void test_restart_offset_includes_timestamp_mapping_origin() {
    const auto timing_signalling = signalling_tlv(
        1, 0, video_discovery_message(1));
    const auto metadata_only_signalling = signalling_tlv(
        2, 0, video_discovery_message(std::nullopt));
    auto stream = timing_signalling;
    const auto later_signalling_offset = static_cast<std::uint64_t>(stream.size());
    stream.insert(stream.end(), metadata_only_signalling.begin(),
                  metadata_only_signalling.end());

    const auto add_video = [&](const std::uint32_t sequence,
                               const std::vector<std::uint8_t>& mfu) {
        const auto packet = tlv_for_mmtp(
            1, mmtp_packet(0xf300, sequence, 100U << 16U, false,
                           mpu_payload(1, mfu)));
        stream.insert(stream.end(), packet.begin(), packet.end());
    };
    add_video(1, {0, 0, 0, 2, 0x46, 0x01});
    add_video(2, {0, 0, 0, 3, 0x26, 0x01, 0x80});
    add_video(3, {0, 0, 0, 2, 0x46, 0x01});

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(stream.data(), stream.size());
    demuxer.flush();
    const auto video = std::find_if(
        sink.access_units.begin(), sink.access_units.end(), [](const auto& unit) {
            return unit.codec == aribtlv::Codec::Hevc;
        });
    check(video != sink.access_units.end() && video->random_access &&
              video->restart_offset == 0 &&
              video->restart_offset < later_signalling_offset,
          "AU restart offset omitted the earlier timestamp mapping origin");

    TestSink restarted_sink;
    aribtlv::Demuxer restarted(restarted_sink);
    restarted.reposition(aribtlv::RepositionOptions{video->restart_offset, true});
    restarted.push(stream.data() + video->restart_offset,
                   stream.size() - static_cast<std::size_t>(video->restart_offset));
    restarted.flush();
    check(std::any_of(restarted_sink.access_units.begin(),
                      restarted_sink.access_units.end(), [](const auto& unit) {
                          return unit.codec == aribtlv::Codec::Hevc &&
                              unit.random_access;
                      }),
          "timestamp-origin restart checkpoint could not reproduce its RAP");
}

void test_extended_timestamp_indexed_by_sample_number() {
    // dts_pts_offsets[0] == 0 keeps the first AU's own PTS at the Demuxer's
    // presentation-timeline origin, so the remaining assertions can compare
    // directly against the raw per-AU descriptor offsets below. pts_offsets is
    // uniform, as required for pts_offset_type == 2 (see emit_access_unit()).
    const std::vector<std::uint16_t> dts_pts_offsets{0, 20, 30, 40};
    const std::vector<std::uint16_t> pts_offsets{111, 111, 111, 111};
    auto stream = signalling_tlv(
        1, 0, video_discovery_message_with_offsets(1, dts_pts_offsets, pts_offsets));

    std::uint32_t sequence = 1;
    const auto add_mfu = [&](const std::uint32_t sample_number, const bool rap,
                             const std::vector<std::uint8_t>& mfu) {
        const auto packet = tlv_for_mmtp(
            1, mmtp_packet(0xf300, sequence++, 100U << 16U, rap,
                           mpu_payload(1, mfu, sample_number)));
        stream.insert(stream.end(), packet.begin(), packet.end());
    };
    add_mfu(1, true, {0, 0, 0, 2, 0x46, 0x01});
    add_mfu(1, false, {0, 0, 0, 3, 0x02, 0x01, 0x80});
    add_mfu(2, false, {0, 0, 0, 2, 0x46, 0x01});
    add_mfu(2, false, {0, 0, 0, 3, 0x02, 0x01, 0x80});
    // sample_number 3 is intentionally never delivered, leaving a hole.
    add_mfu(4, false, {0, 0, 0, 2, 0x46, 0x01});
    add_mfu(4, false, {0, 0, 0, 3, 0x02, 0x01, 0x80});

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(stream.data(), stream.size());
    demuxer.flush();

    std::vector<const aribtlv::AccessUnit*> video;
    for (const auto& unit : sink.access_units) {
        if (unit.codec == aribtlv::Codec::Hevc) video.push_back(&unit);
    }
    check(video.size() == 3, "expected exactly three HEVC access units around the dropped AU");
    check(video[0]->dts.value == 0 && video[0]->pts.value == 0,
          "first access unit did not use its own sample_number offsets");
    check(video[1]->dts.value == 111 && video[1]->pts.value == 131,
          "second access unit did not use its own sample_number offsets");
    check(video[2]->dts.value == 333 && video[2]->pts.value == 373,
          "access unit after the dropped sample_number was shifted onto the wrong "
          "descriptor entry");
}

void test_mpu_au_count_mismatch_flags_discontinuity() {
    const std::vector<std::uint16_t> dts_pts_offsets{10, 20};
    const std::vector<std::uint16_t> pts_offsets{111, 222};
    auto stream = signalling_tlv(
        1, 0, video_discovery_message_with_offsets(1, dts_pts_offsets, pts_offsets));
    const auto next_mpu_signalling = signalling_tlv(2, 0, video_discovery_message(2));

    std::uint32_t sequence = 1;
    const auto add_mfu = [&](const std::uint32_t mpu_sequence, const std::uint32_t sample_number,
                             const bool rap, const std::vector<std::uint8_t>& mfu) {
        const auto packet = tlv_for_mmtp(
            1, mmtp_packet(0xf300, sequence++, 100U << 16U, rap,
                           mpu_payload(mpu_sequence, mfu, sample_number)));
        stream.insert(stream.end(), packet.begin(), packet.end());
    };
    // MPU 1's descriptor declares two access units, but only one is delivered
    // before MPU 2 begins.
    add_mfu(1, 1, true, {0, 0, 0, 2, 0x46, 0x01});
    add_mfu(1, 1, false, {0, 0, 0, 3, 0x02, 0x01, 0x80});
    stream.insert(stream.end(), next_mpu_signalling.begin(), next_mpu_signalling.end());
    add_mfu(2, 1, true, {0, 0, 0, 2, 0x46, 0x01});
    add_mfu(2, 1, false, {0, 0, 0, 3, 0x02, 0x01, 0x80});

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(stream.data(), stream.size());
    demuxer.flush();

    check(std::any_of(sink.errors.begin(), sink.errors.end(), [](const auto& error) {
              return error.code == aribtlv::ErrorCode::Discontinuity && error.recoverable;
          }),
          "short MPU access-unit count did not raise a recoverable discontinuity error");
    const auto second_mpu_video = std::find_if(
        sink.access_units.begin(), sink.access_units.end(), [](const auto& unit) {
            return unit.codec == aribtlv::Codec::Hevc &&
                unit.mpu_sequence_number == std::optional<std::uint32_t>{2};
        });
    check(second_mpu_video != sink.access_units.end() && second_mpu_video->discontinuity,
          "MPU access-unit count mismatch did not mark the next access unit discontinuous");
    check(second_mpu_video->discontinuity_reasons ==
              aribtlv::DiscontinuityReason::SourceDamage,
          "source damage was not distinguished from a controlled discontinuity");
    check(sink.damage_spans.size() == 1 && sink.damage_spans[0].recovered &&
              sink.damage_spans[0].recovery_random_access &&
              sink.damage_spans[0].track_id == second_mpu_video->track_id &&
              sink.damage_spans[0].recovery_time ==
                  std::optional<aribtlv::Timestamp>{second_mpu_video->pts},
          "source damage did not report the next random-access recovery point");
}

void test_non_timed_media_mfu_ignores_opaque_header_as_sample_number() {
    const std::vector<std::uint16_t> dts_pts_offsets{0, 20};
    const std::vector<std::uint16_t> pts_offsets{111, 111};
    auto stream = signalling_tlv(
        1, 0, video_discovery_message_with_offsets(1, dts_pts_offsets, pts_offsets));

    std::uint32_t sequence = 1;
    const auto add_mfu = [&](const std::uint32_t item_id, const bool rap,
                             const std::vector<std::uint8_t>& mfu) {
        const auto packet = tlv_for_mmtp(
            1, mmtp_packet(0xf300, sequence++, 100U << 16U, rap,
                           non_timed_mpu_payload(1, mfu, item_id)));
        stream.insert(stream.end(), packet.begin(), packet.end());
    };
    // Non-timed MFUs carry an opaque item_id in the same header slot a timed
    // MFU's sample_number occupies. Large/out-of-range values here must not
    // be treated as descriptor indices; the parser must fall back to the
    // emission counter exactly as it did before sample_number indexing.
    add_mfu(99, true, {0, 0, 0, 2, 0x46, 0x01});
    add_mfu(99, false, {0, 0, 0, 3, 0x02, 0x01, 0x80});
    add_mfu(1, false, {0, 0, 0, 2, 0x46, 0x01});
    add_mfu(1, false, {0, 0, 0, 3, 0x02, 0x01, 0x80});

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(stream.data(), stream.size());
    demuxer.flush();

    std::vector<const aribtlv::AccessUnit*> video;
    for (const auto& unit : sink.access_units) {
        if (unit.codec == aribtlv::Codec::Hevc) video.push_back(&unit);
    }
    check(video.size() == 2,
          "non-timed media MFUs were misindexed by their opaque header field");
    check(video[0]->dts.value == 0 && video[0]->pts.value == 0,
          "first non-timed access unit did not fall back to the emission counter");
    check(video[1]->dts.value == 111 && video[1]->pts.value == 131,
          "second non-timed access unit did not fall back to the emission counter");
}

void test_aac_extended_timestamp_indexed_by_sample_number() {
    const std::vector<std::uint16_t> dts_pts_offsets{0, 20, 30, 40};
    const std::vector<std::uint16_t> pts_offsets{111, 111, 111, 111};
    auto stream = signalling_tlv(
        1, 0, audio_discovery_message_with_offsets(1, dts_pts_offsets, pts_offsets));

    std::uint32_t sequence = 1;
    const auto add_mfu = [&](const std::uint32_t sample_number,
                             const std::vector<std::uint8_t>& mfu) {
        const auto packet = tlv_for_mmtp(
            1, mmtp_packet(0xf310, sequence++, 100U << 16U, false,
                           mpu_payload(1, mfu, sample_number)));
        stream.insert(stream.end(), packet.begin(), packet.end());
    };
    add_mfu(1, {0x11, 0x22});
    add_mfu(2, {0x33, 0x44});
    // sample_number 3 is intentionally never delivered, leaving a hole.
    add_mfu(4, {0x55, 0x66});

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(stream.data(), stream.size());
    demuxer.flush();

    std::vector<const aribtlv::AccessUnit*> audio;
    for (const auto& unit : sink.access_units) {
        if (unit.codec == aribtlv::Codec::AacLatm) audio.push_back(&unit);
    }
    check(audio.size() == 3, "expected exactly three AAC access units around the dropped AU");
    check(audio[0]->dts.value == 0 && audio[0]->pts.value == 0,
          "first AAC access unit did not use its own sample_number offsets");
    check(audio[1]->dts.value == 111 && audio[1]->pts.value == 131,
          "second AAC access unit did not use its own sample_number offsets");
    check(audio[2]->dts.value == 333 && audio[2]->pts.value == 373,
          "AAC access unit after the dropped sample_number was shifted onto the wrong "
          "descriptor entry");
}

void test_out_of_order_sample_number_is_dropped() {
    // dts_offset is au_index * pts_offsets[0] (see emit_access_unit()), which
    // strictly increases with au_index; pts_offsets must stay uniform across
    // the MPU or emit_access_unit() rejects the whole descriptor entry instead.
    // Delivering sample_number 4 before 2 therefore produces a decreasing
    // dts_offset that the guard must reject; sample_number 5 afterward proves
    // the following access unit is still emitted, marked discontinuous.
    const std::vector<std::uint16_t> dts_pts_offsets{0, 20, 30, 40, 50};
    const std::vector<std::uint16_t> pts_offsets{111, 111, 111, 111, 111};
    auto stream = signalling_tlv(
        1, 0, video_discovery_message_with_offsets(1, dts_pts_offsets, pts_offsets));

    std::uint32_t sequence = 1;
    const auto add_mfu = [&](const std::uint32_t sample_number, const bool rap,
                             const std::vector<std::uint8_t>& mfu) {
        const auto packet = tlv_for_mmtp(
            1, mmtp_packet(0xf300, sequence++, 100U << 16U, rap,
                           mpu_payload(1, mfu, sample_number)));
        stream.insert(stream.end(), packet.begin(), packet.end());
    };
    add_mfu(1, true, {0, 0, 0, 2, 0x46, 0x01});
    add_mfu(1, false, {0, 0, 0, 3, 0x02, 0x01, 0x80});
    add_mfu(4, false, {0, 0, 0, 2, 0x46, 0x01});
    add_mfu(4, false, {0, 0, 0, 3, 0x02, 0x01, 0x80});
    add_mfu(2, false, {0, 0, 0, 2, 0x46, 0x01});
    add_mfu(2, false, {0, 0, 0, 3, 0x02, 0x01, 0x80});
    add_mfu(5, false, {0, 0, 0, 2, 0x46, 0x01});
    add_mfu(5, false, {0, 0, 0, 3, 0x02, 0x01, 0x80});

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(stream.data(), stream.size());
    demuxer.flush();

    std::vector<const aribtlv::AccessUnit*> video;
    for (const auto& unit : sink.access_units) {
        if (unit.codec == aribtlv::Codec::Hevc) video.push_back(&unit);
    }
    check(video.size() == 3, "out-of-order sample_number 2 access unit was emitted");
    check(video[0]->dts.value == 0 && video[1]->dts.value == 333 && video[2]->dts.value == 444,
          "surviving access units did not use their own sample_number offsets");
    check(video[2]->discontinuity,
          "access unit after the dropped decode timestamp was not marked discontinuous");
    check(std::any_of(sink.errors.begin(), sink.errors.end(), [](const auto& error) {
              return error.code == aribtlv::ErrorCode::MalformedInput && error.recoverable &&
                  error.message ==
                      "dropped access unit with a decreasing decode timestamp inside an MPU";
          }),
          "out-of-order sample_number did not raise a recoverable decreasing-DTS error");
}

void test_sample_number_change_starts_a_new_access_unit() {
    // Neither AU carries an AUD (NAL 35) or a parameter-set/prefix-SEI NAL,
    // and the second AU's VCL NAL has first_slice_segment_in_pic_flag CLEAR
    // (the top bit of its third byte is 0), so the other three boundary
    // terms all stay false. Only the sample_number change can split them.
    const std::vector<std::uint16_t> dts_pts_offsets{0, 20};
    const std::vector<std::uint16_t> pts_offsets{111, 111};
    auto stream = signalling_tlv(
        1, 0, video_discovery_message_with_offsets(1, dts_pts_offsets, pts_offsets));

    std::uint32_t sequence = 1;
    const auto add_mfu = [&](const std::uint32_t sample_number, const bool rap,
                             const std::vector<std::uint8_t>& mfu) {
        const auto packet = tlv_for_mmtp(
            1, mmtp_packet(0xf300, sequence++, 100U << 16U, rap,
                           mpu_payload(1, mfu, sample_number)));
        stream.insert(stream.end(), packet.begin(), packet.end());
    };
    // A freshly-installed video track waits for a RAP before emitting, so
    // the first AU must be delivered as MMTP random-access.
    add_mfu(1, true, {0, 0, 0, 3, 0x02, 0x01, 0x80}); // pending empty, first_slice irrelevant
    add_mfu(2, false, {0, 0, 0, 3, 0x02, 0x01, 0x00}); // first_slice_segment_in_pic_flag CLEAR

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(stream.data(), stream.size());
    demuxer.flush();

    std::vector<const aribtlv::AccessUnit*> video;
    for (const auto& unit : sink.access_units) {
        if (unit.codec == aribtlv::Codec::Hevc) video.push_back(&unit);
    }
    check(video.size() == 2,
          "sample_number change did not split two plain VCL NAL units into separate "
          "access units");
    check(video[0]->dts.value == 0 && video[0]->pts.value == 0,
          "first access unit did not carry its own descriptor entry");
    check(video[1]->dts.value == 111 && video[1]->pts.value == 131,
          "second access unit did not carry its own descriptor entry");
}

void test_pts_offset_type_2_uniform_matches_pts_offset_type_1() {
    // A pts_offset_type == 2 descriptor whose per-AU pts_offset is uniform must
    // produce identical timestamps to the equivalent pts_offset_type == 1
    // descriptor: TR-B39 fixes pts_offset_type at '01' and replicates a single
    // default_pts_offset across the MPU, so accumulating either one over a
    // decode-order prefix sums the same constant regardless of order (see the
    // citation above MmtpParser::emit_access_unit).
    const std::uint8_t au_count = 3;
    const std::vector<std::uint16_t> dts_pts_offsets(au_count, 0);
    const std::vector<std::uint16_t> pts_offsets(au_count, 3000);

    struct Emitted {
        std::int64_t dts = 0;
        std::int64_t pts = 0;
    };
    const auto run = [](std::vector<std::uint8_t> discovery) {
        auto stream = signalling_tlv(1, 0, std::move(discovery));
        std::uint32_t sequence = 1;
        const auto add_mfu = [&](const std::uint32_t sample_number, const bool rap,
                                 const std::vector<std::uint8_t>& mfu) {
            const auto packet = tlv_for_mmtp(
                1, mmtp_packet(0xf300, sequence++, 100U << 16U, rap,
                               mpu_payload(1, mfu, sample_number)));
            stream.insert(stream.end(), packet.begin(), packet.end());
        };
        add_mfu(1, true, {0, 0, 0, 2, 0x46, 0x01});
        add_mfu(1, false, {0, 0, 0, 3, 0x02, 0x01, 0x80});
        add_mfu(2, false, {0, 0, 0, 2, 0x46, 0x01});
        add_mfu(2, false, {0, 0, 0, 3, 0x02, 0x01, 0x80});
        add_mfu(3, false, {0, 0, 0, 2, 0x46, 0x01});
        add_mfu(3, false, {0, 0, 0, 3, 0x02, 0x01, 0x80});

        TestSink sink;
        aribtlv::Demuxer demuxer(sink);
        demuxer.push(stream.data(), stream.size());
        demuxer.flush();

        std::vector<Emitted> emitted;
        for (const auto& unit : sink.access_units) {
            if (unit.codec == aribtlv::Codec::Hevc) {
                emitted.push_back(Emitted{unit.dts.value, unit.pts.value});
            }
        }
        return emitted;
    };

    const auto type1 = run(video_discovery_message_with_au_count(1, au_count));
    const auto type2 = run(video_discovery_message_with_offsets(1, dts_pts_offsets, pts_offsets));

    check(type1.size() == 3 && type2.size() == 3,
          "expected three access units from both the pts_offset_type 1 and 2 streams");
    for (std::size_t index = 0; index < type1.size(); ++index) {
        check(type1[index].dts == type2[index].dts && type1[index].pts == type2[index].pts,
              "uniform pts_offset_type == 2 produced different timestamps than the "
              "equivalent pts_offset_type == 1 descriptor");
    }
}

void test_pts_offset_type_2_non_uniform_is_rejected() {
    // Once an MPU's pts_offset stops being constant across access units,
    // emit_access_unit() can no longer recover presentation order from a
    // decode-order prefix sum, and must drop the access units instead of
    // silently reusing the (wrong) pts_offset_type == 1 arithmetic.
    const std::vector<std::uint16_t> dts_pts_offsets{0, 10, 20};
    const std::vector<std::uint16_t> pts_offsets{111, 222, 333};
    auto stream = signalling_tlv(
        1, 0, video_discovery_message_with_offsets(1, dts_pts_offsets, pts_offsets));

    std::uint32_t sequence = 1;
    const auto add_mfu = [&](const std::uint32_t sample_number, const bool rap,
                             const std::vector<std::uint8_t>& mfu) {
        const auto packet = tlv_for_mmtp(
            1, mmtp_packet(0xf300, sequence++, 100U << 16U, rap,
                           mpu_payload(1, mfu, sample_number)));
        stream.insert(stream.end(), packet.begin(), packet.end());
    };
    add_mfu(1, true, {0, 0, 0, 2, 0x46, 0x01});
    add_mfu(1, false, {0, 0, 0, 3, 0x02, 0x01, 0x80});
    add_mfu(2, false, {0, 0, 0, 2, 0x46, 0x01});
    add_mfu(2, false, {0, 0, 0, 3, 0x02, 0x01, 0x80});
    add_mfu(3, false, {0, 0, 0, 2, 0x46, 0x01});
    add_mfu(3, false, {0, 0, 0, 3, 0x02, 0x01, 0x80});

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(stream.data(), stream.size());
    demuxer.flush();

    check(std::none_of(sink.access_units.begin(), sink.access_units.end(),
                       [](const auto& unit) { return unit.codec == aribtlv::Codec::Hevc; }),
          "non-uniform pts_offset_type == 2 still emitted an access unit for the MPU");
    check(std::any_of(sink.errors.begin(), sink.errors.end(), [](const auto& error) {
              return error.code == aribtlv::ErrorCode::UnsupportedFeature && error.recoverable &&
                  error.message ==
                      "dropped access unit: pts_offset_type 2 supplied a non-uniform "
                      "pts_offset, which needs the bitstream presentation order that "
                      "this parser does not derive";
          }),
          "non-uniform pts_offset_type == 2 did not raise the expected recoverable error");
}

void test_pts_offset_type_3_is_rejected() {
    // pts_offset_type == 3 is reserved by TR-B39 Table 34.1-72 and must not be
    // silently treated as though every access unit shares one decode timestamp.
    const std::vector<std::uint16_t> dts_pts_offsets{0, 20};
    auto stream = signalling_tlv(
        1, 0, video_discovery_message_with_reserved_pts_offset_type(1, dts_pts_offsets));

    std::uint32_t sequence = 1;
    const auto add_mfu = [&](const std::uint32_t sample_number, const bool rap,
                             const std::vector<std::uint8_t>& mfu) {
        const auto packet = tlv_for_mmtp(
            1, mmtp_packet(0xf300, sequence++, 100U << 16U, rap,
                           mpu_payload(1, mfu, sample_number)));
        stream.insert(stream.end(), packet.begin(), packet.end());
    };
    add_mfu(1, true, {0, 0, 0, 2, 0x46, 0x01});
    add_mfu(1, false, {0, 0, 0, 3, 0x02, 0x01, 0x80});
    add_mfu(2, false, {0, 0, 0, 2, 0x46, 0x01});
    add_mfu(2, false, {0, 0, 0, 3, 0x02, 0x01, 0x80});

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(stream.data(), stream.size());
    demuxer.flush();

    check(std::none_of(sink.access_units.begin(), sink.access_units.end(),
                       [](const auto& unit) { return unit.codec == aribtlv::Codec::Hevc; }),
          "reserved pts_offset_type == 3 still built a timestamp mapping and emitted "
          "access units");
    check(std::any_of(sink.errors.begin(), sink.errors.end(), [](const auto& error) {
              return error.code == aribtlv::ErrorCode::UnsupportedFeature && error.recoverable &&
                  error.message ==
                      "mpu_extended_timestamp_descriptor: pts_offset_type 3 is reserved by "
                      "TR-B39 Table 34.1-72 and defines no pts_offset semantics; skipping it";
          }),
          "reserved pts_offset_type == 3 did not raise the expected recoverable error");
}

void test_leap_second_insertion_corrects_presentation_timeline() {
    // MPU1 is normal (100s, indicator 0). MPU2 enters the leap window (101s,
    // indicator 1, "the day before"). MPU3 repeats MPU2's wire
    // mpu_presentation_time (101s again, the inserted duplicate second) but
    // is where the indicator switches 1->0, so it is where TR-B39 Appendix 1
    // section 2.1 says the +1s correction begins. MPU4 (102s, indicator 0) proves
    // the correction persists past the transition MPU.
    auto stream =
        signalling_tlv(1, 0, audio_discovery_message_with_leap(1, 100ULL << 32U, 0));
    const auto mpu2 =
        signalling_tlv(2, 0, audio_discovery_message_with_leap(2, 101ULL << 32U, 1));
    const auto mpu3 =
        signalling_tlv(3, 0, audio_discovery_message_with_leap(3, 101ULL << 32U, 0));
    const auto mpu4 =
        signalling_tlv(4, 0, audio_discovery_message_with_leap(4, 102ULL << 32U, 0));

    std::uint32_t sequence = 1;
    const auto add_mfu = [&](const std::uint32_t mpu_sequence,
                             const std::vector<std::uint8_t>& mfu) {
        const auto packet = tlv_for_mmtp(
            1, mmtp_packet(0xf310, sequence++, 100U << 16U, false, mpu_payload(mpu_sequence, mfu)));
        stream.insert(stream.end(), packet.begin(), packet.end());
    };
    add_mfu(1, {0x11, 0x22});
    stream.insert(stream.end(), mpu2.begin(), mpu2.end());
    add_mfu(2, {0x33, 0x44});
    stream.insert(stream.end(), mpu3.begin(), mpu3.end());
    add_mfu(3, {0x55, 0x66});
    stream.insert(stream.end(), mpu4.begin(), mpu4.end());
    add_mfu(4, {0x77, 0x88});

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(stream.data(), stream.size());
    demuxer.flush();

    std::vector<const aribtlv::AccessUnit*> audio;
    for (const auto& unit : sink.access_units) {
        if (unit.codec == aribtlv::Codec::AacLatm) audio.push_back(&unit);
    }
    check(audio.size() == 4, "leap-second insertion test did not produce four AAC access units");
    check(audio[0]->pts.value == 0 && audio[0]->dts.value == 0,
          "first access unit was not normalized to the presentation-timeline origin");
    check(audio[1]->pts.value == 180000 && audio[1]->dts.value == 180000,
          "step before the transition was not the normal one-second inter-MPU step");
    check(audio[2]->pts.value == 360000 && audio[2]->dts.value == 360000,
          "leap-second insertion was not corrected away at the 1->0 transition MPU");
    check(audio[3]->pts.value == 540000 && audio[3]->dts.value == 540000,
          "the +1s leap correction did not persist past the transition MPU");
}

void test_leap_second_deletion_corrects_presentation_timeline() {
    // MPU1 is normal (100s, indicator 0). MPU2 enters the deletion window
    // (101s, indicator 2). MPU3 jumps straight to 103s -- 102s is the
    // deleted wire second -- and is where the indicator switches 2->0, so
    // TR-B39 Appendix 1 section 2.2 says the -1s correction begins there. MPU4
    // (104s, indicator 0) proves the correction persists.
    auto stream =
        signalling_tlv(1, 0, audio_discovery_message_with_leap(1, 100ULL << 32U, 0));
    const auto mpu2 =
        signalling_tlv(2, 0, audio_discovery_message_with_leap(2, 101ULL << 32U, 2));
    const auto mpu3 =
        signalling_tlv(3, 0, audio_discovery_message_with_leap(3, 103ULL << 32U, 0));
    const auto mpu4 =
        signalling_tlv(4, 0, audio_discovery_message_with_leap(4, 104ULL << 32U, 0));

    std::uint32_t sequence = 1;
    const auto add_mfu = [&](const std::uint32_t mpu_sequence,
                             const std::vector<std::uint8_t>& mfu) {
        const auto packet = tlv_for_mmtp(
            1, mmtp_packet(0xf310, sequence++, 100U << 16U, false, mpu_payload(mpu_sequence, mfu)));
        stream.insert(stream.end(), packet.begin(), packet.end());
    };
    add_mfu(1, {0x11, 0x22});
    stream.insert(stream.end(), mpu2.begin(), mpu2.end());
    add_mfu(2, {0x33, 0x44});
    stream.insert(stream.end(), mpu3.begin(), mpu3.end());
    add_mfu(3, {0x55, 0x66});
    stream.insert(stream.end(), mpu4.begin(), mpu4.end());
    add_mfu(4, {0x77, 0x88});

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(stream.data(), stream.size());
    demuxer.flush();

    std::vector<const aribtlv::AccessUnit*> audio;
    for (const auto& unit : sink.access_units) {
        if (unit.codec == aribtlv::Codec::AacLatm) audio.push_back(&unit);
    }
    check(audio.size() == 4, "leap-second deletion test did not produce four AAC access units");
    check(audio[0]->pts.value == 0 && audio[0]->dts.value == 0,
          "first access unit was not normalized to the presentation-timeline origin");
    check(audio[1]->pts.value == 180000 && audio[1]->dts.value == 180000,
          "step before the transition was not the normal one-second inter-MPU step");
    check(audio[2]->pts.value == 360000 && audio[2]->dts.value == 360000,
          "leap-second deletion's two-second jump was not corrected at the 2->0 transition MPU");
    check(audio[3]->pts.value == 540000 && audio[3]->dts.value == 540000,
          "the -1s leap correction did not persist past the transition MPU");
}

void test_leap_indicator_zero_is_inert() {
    // The indicator stays 0 throughout, so the correction must never engage:
    // the emitted timing must match plain, unadjusted mpu_presentation_time
    // arithmetic exactly.
    auto stream =
        signalling_tlv(1, 0, audio_discovery_message_with_leap(1, 100ULL << 32U, 0));
    const auto mpu2 =
        signalling_tlv(2, 0, audio_discovery_message_with_leap(2, 101ULL << 32U, 0));
    const auto mpu3 =
        signalling_tlv(3, 0, audio_discovery_message_with_leap(3, 102ULL << 32U, 0));

    std::uint32_t sequence = 1;
    const auto add_mfu = [&](const std::uint32_t mpu_sequence,
                             const std::vector<std::uint8_t>& mfu) {
        const auto packet = tlv_for_mmtp(
            1, mmtp_packet(0xf310, sequence++, 100U << 16U, false, mpu_payload(mpu_sequence, mfu)));
        stream.insert(stream.end(), packet.begin(), packet.end());
    };
    add_mfu(1, {0x11, 0x22});
    stream.insert(stream.end(), mpu2.begin(), mpu2.end());
    add_mfu(2, {0x33, 0x44});
    stream.insert(stream.end(), mpu3.begin(), mpu3.end());
    add_mfu(3, {0x55, 0x66});

    TestSink sink;
    aribtlv::Demuxer demuxer(sink);
    demuxer.push(stream.data(), stream.size());
    demuxer.flush();

    std::vector<const aribtlv::AccessUnit*> audio;
    for (const auto& unit : sink.access_units) {
        if (unit.codec == aribtlv::Codec::AacLatm) audio.push_back(&unit);
    }
    check(audio.size() == 3, "leap-indicator-zero test did not produce three AAC access units");
    check(audio[0]->pts.value == 0 && audio[0]->dts.value == 0,
          "leap indicator 0 changed the first access unit's timing");
    check(audio[1]->pts.value == 180000 && audio[1]->dts.value == 180000,
          "leap indicator 0 changed the second access unit's timing");
    check(audio[2]->pts.value == 360000 && audio[2]->dts.value == 360000,
          "leap indicator 0 changed the third access unit's timing");
}

} // namespace

int main() {
    test_split_at_every_boundary();
    test_one_byte_input();
    test_garbage_recovery();
    test_service_selection_and_reset();
    test_incomplete_flush();
    test_mode_60_and_resource_limit();
    test_signalling_fragmentation_aggregation_and_m2();
    test_global_packet_state_budget();
    test_track_discovery_and_deduplication();
    test_truncated_subtitle_reference_start_time_is_rejected();
    test_service_selection_clears_layout_state();
    test_application_and_data_transmission_signalling();
    test_mpt_snapshot_removes_missing_service_state();
    test_mh_ait_snapshot_completion_empty_and_reposition();
    test_service_state_reset_notifications();
    test_independent_m2_sdt_and_tot();
    test_mh_eit_program_events();
    test_emt_stream_events();
    test_viewer_participation_notifications();
    test_dynamic_audio_layout_metadata();
    test_authenticated_mmtp_payload_bounds();
    test_codec_output_and_timeline();
    test_recording_scanner_uses_demux_metadata_and_bounds_time();
    test_timestamp_overflow_rejection();
    test_track_selection_clears_incomplete_media();
    test_fragmented_signalling_restart_offset();
    test_reposition_preserves_timeline_and_absolute_offsets();
    test_track_selection_preserves_timeline_and_waits_for_rap();
    test_hevc_irap_detection_without_mmtp_rap();
    test_reposition_drops_orphan_hevc_irap_continuation();
    test_access_unit_restart_offset_is_snapshotted();
    test_restart_offset_includes_timestamp_mapping_origin();
    test_extended_timestamp_indexed_by_sample_number();
    test_mpu_au_count_mismatch_flags_discontinuity();
    test_non_timed_media_mfu_ignores_opaque_header_as_sample_number();
    test_aac_extended_timestamp_indexed_by_sample_number();
    test_out_of_order_sample_number_is_dropped();
    test_sample_number_change_starts_a_new_access_unit();
    test_pts_offset_type_2_uniform_matches_pts_offset_type_1();
    test_pts_offset_type_2_non_uniform_is_rejected();
    test_pts_offset_type_3_is_rejected();
    test_leap_second_insertion_corrects_presentation_timeline();
    test_leap_second_deletion_corrects_presentation_timeline();
    test_leap_indicator_zero_is_inert();
    std::cout << "all tests passed\n";
    return 0;
}
