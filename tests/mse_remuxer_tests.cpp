#include <tlvdemux/mse_remuxer.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

void check(const bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

class TestSink final : public tlvdemux::MseSink {
public:
    void onMseInit(tlvdemux::MseTrackInit&& init) override {
        inits.push_back(std::move(init));
    }
    void onMseSegment(tlvdemux::MseMediaSegment&& segment) override {
        segments.push_back(std::move(segment));
    }

    std::vector<tlvdemux::MseTrackInit> inits;
    std::vector<tlvdemux::MseMediaSegment> segments;
};

class BitWriter {
public:
    void bits(const std::uint32_t value, const unsigned count) {
        for (unsigned index = 0; index < count; ++index) {
            if ((offset_ & 7U) == 0) data_.push_back(0);
            const auto shift = count - index - 1;
            data_.back() |= static_cast<std::uint8_t>(
                ((value >> shift) & 1U) << (7U - (offset_ & 7U)));
            ++offset_;
        }
    }

    std::vector<std::uint8_t> take() { return std::move(data_); }

private:
    std::vector<std::uint8_t> data_;
    unsigned offset_ = 0;
};

std::vector<std::uint8_t> loas_frame(const std::uint32_t channel_configuration) {
    BitWriter writer;
    writer.bits(0, 1);  // useSameStreamMux
    writer.bits(0, 1);  // audioMuxVersion
    writer.bits(1, 1);  // allStreamsSameTimeFraming
    writer.bits(0, 6);  // numSubFrames
    writer.bits(0, 4);  // numProgram
    writer.bits(0, 3);  // numLayer
    writer.bits(2, 5);  // AAC LC
    writer.bits(3, 4);  // 48000 Hz
    writer.bits(channel_configuration, 4);
    writer.bits(0, 1);  // frameLengthFlag
    writer.bits(0, 1);  // dependsOnCoreCoder
    writer.bits(0, 1);  // extensionFlag
    writer.bits(0, 3);  // frameLengthType
    writer.bits(0, 8);  // latmBufferFullness
    writer.bits(0, 1);  // otherDataPresent
    writer.bits(0, 1);  // crcCheckPresent
    writer.bits(1, 8);  // PayloadLengthInfo
    writer.bits(0xaa, 8);
    auto payload = writer.take();
    const auto length = payload.size();
    std::vector<std::uint8_t> result{
        0x56,
        static_cast<std::uint8_t>(0xe0U | ((length >> 8U) & 0x1fU)),
        static_cast<std::uint8_t>(length),
    };
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

tlvdemux::AccessUnit audio_unit(const std::uint32_t channel_configuration) {
    tlvdemux::AccessUnit unit;
    unit.track_id = 1;
    unit.codec = tlvdemux::Codec::AacLatm;
    unit.data = loas_frame(channel_configuration);
    unit.pts = {0, 48000};
    unit.dts = unit.pts;
    return unit;
}

// ---- Minimal fMP4 box reader: enough to pull tfdt/trun back out of a
// media segment the muxer just produced, so the tests can inspect the
// exact bytes that would reach a real MSE SourceBuffer. ----

std::uint32_t read_u32(const std::vector<std::uint8_t>& data, const std::size_t offset) {
    return (std::uint32_t(data[offset]) << 24) | (std::uint32_t(data[offset + 1]) << 16) |
           (std::uint32_t(data[offset + 2]) << 8) | std::uint32_t(data[offset + 3]);
}
std::uint64_t read_u64(const std::vector<std::uint8_t>& data, const std::size_t offset) {
    return (std::uint64_t(read_u32(data, offset)) << 32) | std::uint64_t(read_u32(data, offset + 4));
}
std::int32_t read_i32(const std::vector<std::uint8_t>& data, const std::size_t offset) {
    return static_cast<std::int32_t>(read_u32(data, offset));
}

struct BoxRange { std::size_t payload_start, payload_end; };

std::optional<BoxRange> find_box(const std::vector<std::uint8_t>& data, const std::size_t start,
                                  const std::size_t end, const char* type) {
    std::size_t offset = start;
    while (offset + 8 <= end) {
        const auto box_size = read_u32(data, offset);
        if (box_size < 8 || offset + box_size > end) break;
        if (std::equal(type, type + 4, data.begin() + static_cast<std::ptrdiff_t>(offset) + 4)) {
            return BoxRange{offset + 8, offset + box_size};
        }
        offset += box_size;
    }
    return std::nullopt;
}

struct ParsedSample {
    std::uint32_t duration = 0;
    std::int32_t composition_offset = 0;
};

struct ParsedSegment {
    std::uint64_t tfdt = 0;
    std::vector<ParsedSample> samples;
};

ParsedSegment parse_segment(const std::vector<std::uint8_t>& data) {
    const auto moof = find_box(data, 0, data.size(), "moof");
    check(moof.has_value(), "segment is missing a moof box");
    const auto traf = find_box(data, moof->payload_start, moof->payload_end, "traf");
    check(traf.has_value(), "moof is missing a traf box");
    const auto tfdt = find_box(data, traf->payload_start, traf->payload_end, "tfdt");
    check(tfdt.has_value(), "traf is missing a tfdt box");
    check(data[tfdt->payload_start] == 1, "tfdt must be version 1 (64-bit baseMediaDecodeTime)");
    const auto trun = find_box(data, traf->payload_start, traf->payload_end, "trun");
    check(trun.has_value(), "traf is missing a trun box");
    check(data[trun->payload_start] == 1, "trun must be version 1");
    const auto flags = (std::uint32_t(data[trun->payload_start + 1]) << 16) |
                        (std::uint32_t(data[trun->payload_start + 2]) << 8) |
                        std::uint32_t(data[trun->payload_start + 3]);
    check(flags == 0x000f01, "trun must carry duration/size/flags/composition-offset for every sample");

    ParsedSegment result;
    result.tfdt = read_u64(data, tfdt->payload_start + 4);
    const auto sample_count = read_u32(data, trun->payload_start + 4);
    const auto entries_start = trun->payload_start + 12;
    for (std::uint32_t index = 0; index < sample_count; ++index) {
        const auto base = entries_start + std::size_t(index) * 16;
        ParsedSample sample;
        sample.duration = read_u32(data, base);
        sample.composition_offset = read_i32(data, base + 12);
        result.samples.push_back(sample);
    }
    return result;
}

std::uint32_t mdhd_timescale(const std::vector<std::uint8_t>& init_segment) {
    const auto moov = find_box(init_segment, 0, init_segment.size(), "moov");
    check(moov.has_value(), "init segment is missing a moov box");
    const auto trak = find_box(init_segment, moov->payload_start, moov->payload_end, "trak");
    check(trak.has_value(), "moov is missing a trak box");
    const auto mdia = find_box(init_segment, trak->payload_start, trak->payload_end, "mdia");
    check(mdia.has_value(), "trak is missing a mdia box");
    const auto mdhd = find_box(init_segment, mdia->payload_start, mdia->payload_end, "mdhd");
    check(mdhd.has_value(), "mdia is missing a mdhd box");
    return read_u32(init_segment, mdhd->payload_start + 12);  // version/flags(4) + creation/modification time(8)
}

std::vector<ParsedSegment> segments_of(const std::vector<tlvdemux::MseMediaSegment>& segments,
                                       const std::string& type) {
    std::vector<ParsedSegment> out;
    for (const auto& segment : segments) if (segment.type == type) out.push_back(parse_segment(segment.data));
    return out;
}

// Reconstructs each sample's composition timestamp (tfdt + running dts sum +
// composition_offset) across a run of segments, in emission order.
std::vector<std::int64_t> composition_timestamps(const std::vector<ParsedSegment>& segments) {
    std::vector<std::int64_t> out;
    for (const auto& segment : segments) {
        std::int64_t dts = std::int64_t(segment.tfdt);
        for (const auto& sample : segment.samples) {
            out.push_back(dts + sample.composition_offset);
            dts += std::int64_t(sample.duration);
        }
    }
    return out;
}

// ---- HEVC Annex B synthesis: only what parse_sps() in mse_remuxer.cpp
// actually reads, plus the NAL headers/types the muxer inspects. ----

void write_ue(BitWriter& writer, const std::uint32_t value) {
    const auto code_num = value + 1;
    unsigned leading_zeros = 0;
    while ((code_num >> leading_zeros) > 1) ++leading_zeros;
    for (unsigned i = 0; i < leading_zeros; ++i) writer.bits(0, 1);
    writer.bits(code_num, leading_zeros + 1);
}

std::uint16_t nal_header(const unsigned type) {
    return static_cast<std::uint16_t>((type & 0x3fU) << 9 | 1U);  // layer_id 0, temporal_id_plus1 1
}
std::vector<std::uint8_t> nal_header_bytes(const unsigned type) {
    const auto value = nal_header(type);
    return {static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
}
std::vector<std::uint8_t> make_simple_nal(const unsigned type, const std::vector<std::uint8_t>& payload) {
    auto out = nal_header_bytes(type);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// Inverse of mse_remuxer.cpp's rbsp() de-escaper: inserts emulation_prevention_three_byte
// so the raw bit content survives the muxer's Annex B parsing unchanged.
std::vector<std::uint8_t> escape_rbsp(const std::vector<std::uint8_t>& raw) {
    std::vector<std::uint8_t> out;
    unsigned zero_run = 0;
    for (const auto byte : raw) {
        if (zero_run >= 2 && byte <= 3) { out.push_back(3); zero_run = 0; }
        out.push_back(byte);
        zero_run = byte == 0 ? zero_run + 1 : 0;
    }
    return out;
}

// Matches exactly the fields parse_sps() (wasm/mse_remuxer.cpp) reads, with
// sps_max_sub_layers_minus1 = 0 so its sub-layer loops are skipped.
std::vector<std::uint8_t> build_sps_nalu(const std::uint32_t width, const std::uint32_t height) {
    BitWriter writer;
    writer.bits(nal_header(33), 16);
    writer.bits(0, 4);  // sps_video_parameter_set_id
    writer.bits(0, 3);  // sps_max_sub_layers_minus1
    writer.bits(1, 1);  // sps_temporal_id_nesting_flag
    writer.bits(0, 2);  // profile_space
    writer.bits(0, 1);  // tier
    writer.bits(1, 5);  // profile_idc
    for (int i = 0; i < 4; ++i) writer.bits(0, 8);  // general_profile_compatibility_flag[32]
    for (int i = 0; i < 6; ++i) writer.bits(0, 8);  // general_constraint flags[48]
    writer.bits(93, 8);  // level_idc
    write_ue(writer, 0);       // sps_seq_parameter_set_id
    write_ue(writer, 1);       // chroma_format_idc (4:2:0)
    write_ue(writer, width);   // pic_width_in_luma_samples
    write_ue(writer, height);  // pic_height_in_luma_samples
    writer.bits(0, 1);  // conformance_window_flag
    write_ue(writer, 0);  // bit_depth_luma_minus8
    write_ue(writer, 0);  // bit_depth_chroma_minus8
    return escape_rbsp(writer.take());
}

std::vector<std::uint8_t> annex_b_wrap(const std::vector<std::uint8_t>& nalu) {
    std::vector<std::uint8_t> out{0, 0, 0, 1};
    out.insert(out.end(), nalu.begin(), nalu.end());
    return out;
}

// vcl_types gives one VCL NAL per entry (so a test can drive RASL/RADL/BLA/CRA
// mixes directly); trailing_nal optionally appends a non-VCL NAL such as
// EOS_NUT after them, and an empty vcl_types with a trailing_nal builds an
// access unit that carries only that marker NAL.
std::vector<std::uint8_t> video_access_unit_data(const bool include_parameter_sets,
                                                  const std::vector<unsigned>& vcl_types,
                                                  const std::optional<unsigned> trailing_nal = std::nullopt) {
    std::vector<std::uint8_t> out;
    if (include_parameter_sets) {
        for (const auto& nalu : {annex_b_wrap(make_simple_nal(32, {0xab, 0xcd})),   // VPS
                                 annex_b_wrap(make_simple_nal(34, {0xab, 0xcd})),   // PPS
                                 annex_b_wrap(build_sps_nalu(1920, 1080))}) {       // SPS
            out.insert(out.end(), nalu.begin(), nalu.end());
        }
    }
    for (const auto type : vcl_types) {
        const auto vcl = annex_b_wrap(make_simple_nal(type, {0x80}));
        out.insert(out.end(), vcl.begin(), vcl.end());
    }
    if (trailing_nal) {
        const auto nalu = annex_b_wrap(make_simple_nal(*trailing_nal, {}));
        out.insert(out.end(), nalu.begin(), nalu.end());
    }
    return out;
}

std::vector<std::uint8_t> video_access_unit_data(const bool include_parameter_sets, const bool keyframe) {
    return video_access_unit_data(include_parameter_sets, std::vector<unsigned>{keyframe ? 19u : 1u});  // IDR_W_RADL / TRAIL_R
}

tlvdemux::AccessUnit hevc_unit(const std::uint64_t track_id, const std::int64_t dts_value,
                               const std::int64_t pts_value, const bool keyframe,
                               const bool include_parameter_sets,
                               const std::uint32_t timescale = 1000000) {
    tlvdemux::AccessUnit unit;
    unit.track_id = track_id;
    unit.codec = tlvdemux::Codec::Hevc;
    unit.data = video_access_unit_data(include_parameter_sets, keyframe);
    unit.dts = {dts_value, timescale};
    unit.pts = {pts_value, timescale};
    unit.random_access = keyframe;
    return unit;
}

tlvdemux::AccessUnit hevc_unit(const std::uint64_t track_id, const std::int64_t dts_value,
                               const std::int64_t pts_value, const std::vector<unsigned>& vcl_types,
                               const bool include_parameter_sets,
                               const std::optional<unsigned> trailing_nal = std::nullopt,
                               const std::uint32_t timescale = 1000000) {
    tlvdemux::AccessUnit unit;
    unit.track_id = track_id;
    unit.codec = tlvdemux::Codec::Hevc;
    unit.data = video_access_unit_data(include_parameter_sets, vcl_types, trailing_nal);
    unit.dts = {dts_value, timescale};
    unit.pts = {pts_value, timescale};
    unit.random_access = std::any_of(vcl_types.begin(), vcl_types.end(),
                                      [](const unsigned type) { return type >= 16 && type <= 21; });
    return unit;
}

void test_audio_drops_non_advancing_dts() {
    TestSink sink;
    tlvdemux::MseRemuxer remuxer(sink);
    remuxer.selectTrack(tlvdemux::TrackKind::Video, 2);
    remuxer.selectTrack(tlvdemux::TrackKind::Audio, 1);
    // Audio only starts emitting once the video track has started (it supplies
    // the shared timeline offset), so prime it with a single keyframe.
    remuxer.push(hevc_unit(2, 0, 0, true, true));

    // 48000 is the AAC track's timescale here, and audio uses pts == dts, so a
    // pts.timescale of 48000 makes the muxer's internal microsecond round trip
    // exact (48000 * (1e6/48000) * (48000/1e6) == 48000) whenever the value is
    // a multiple of 6 -- no floating point slack to account for below.
    const std::int64_t step = 600;
    std::vector<std::int64_t> pts_values;
    for (int i = 0; i <= 10; ++i) pts_values.push_back(step * i);
    pts_values.push_back(step * 10);        // exact repeat: must be dropped
    pts_values.push_back(step * 10 - 300);  // goes backwards: must be dropped
    for (int i = 11; i < 40; ++i) pts_values.push_back(step * i);

    for (const auto value : pts_values) {
        auto unit = audio_unit(2);
        unit.pts = {value, 48000};
        unit.dts = unit.pts;
        remuxer.push(unit);
    }
    remuxer.flush();

    const auto segments = segments_of(sink.segments, "audio");
    check(segments.size() >= 2, "audio push should have spanned multiple fragments");

    std::int64_t expected_dts = 0;
    std::size_t total_samples = 0;
    for (std::size_t s = 0; s < segments.size(); ++s) {
        const auto& segment = segments[s];
        check(std::int64_t(segment.tfdt) == expected_dts,
              "audio fragment tfdt does not continue the previous fragment's decode timeline");
        std::uint64_t sum_durations = 0;
        for (const auto& sample : segment.samples) {
            check(sample.duration == std::uint32_t(step),
                  "a sample duration was fabricated instead of dropping the non-advancing input");
            sum_durations += sample.duration;
            ++total_samples;
        }
        expected_dts += std::int64_t(sum_durations);
    }
    check(total_samples == 40,
          "the two non-advancing samples were papered over with a fallback duration instead of dropped");
}

struct ReorderedFrame { std::int64_t dts, pts; bool keyframe; };

std::vector<ReorderedFrame> build_reordered_frames(const int groups, const std::int64_t dts_step) {
    std::vector<ReorderedFrame> frames{{0, 0, true}};
    for (int g = 0; g < groups; ++g) {
        const auto base = dts_step * (3 * g + 1);
        frames.push_back({base, base + 2 * dts_step, false});
        frames.push_back({base + dts_step, base, false});
        frames.push_back({base + 2 * dts_step, base + dts_step, false});
    }
    return frames;
}

void test_video_fragments_do_not_overlap_in_composition_time() {
    TestSink sink;
    tlvdemux::MseRemuxer remuxer(sink);
    remuxer.selectTrack(tlvdemux::TrackKind::Video, 2);

    const auto frames = build_reordered_frames(10, 100000);
    bool first = true;
    for (const auto& frame : frames) {
        remuxer.push(hevc_unit(2, frame.dts, frame.pts, frame.keyframe, first));
        first = false;
    }
    remuxer.flush();

    const auto segments = segments_of(sink.segments, "video");
    check(segments.size() >= 2, "reordered video push should have spanned multiple fragments");

    std::vector<std::pair<std::int64_t, std::int64_t>> intervals;
    for (const auto& segment : segments) {
        std::int64_t dts = std::int64_t(segment.tfdt);
        auto min_pts = std::numeric_limits<std::int64_t>::max();
        auto max_end = std::numeric_limits<std::int64_t>::min();
        for (const auto& sample : segment.samples) {
            const auto pts = dts + sample.composition_offset;
            min_pts = std::min(min_pts, pts);
            max_end = std::max(max_end, pts + std::int64_t(sample.duration));
            dts += std::int64_t(sample.duration);
        }
        intervals.emplace_back(min_pts, max_end);
    }
    for (std::size_t i = 0; i + 1 < intervals.size(); ++i) {
        check(intervals[i].second <= intervals[i + 1].first,
              "fragment " + std::to_string(i) +
                  " composition interval overlaps the next fragment's, which Firefox's "
                  "CtsComparator would misinterpret as evicting already-buffered frames");
    }
}

// Real broadcast video (see demo/bsp4k-lag-3.mmts) runs at timescale 180000
// with a 3003-tick frame interval -- 16683.33us, which does not divide the
// MP4 track timescale evenly. Pins the fix that makes the video track adopt
// unit.dts.timescale instead of hardcoding 1000000, so samples never round
// through an inexact microsecond conversion.
void test_video_fragments_do_not_overlap_with_broadcast_timescale() {
    TestSink sink;
    tlvdemux::MseRemuxer remuxer(sink);
    remuxer.selectTrack(tlvdemux::TrackKind::Video, 2);

    constexpr std::uint32_t broadcast_timescale = 180000;
    constexpr std::int64_t frame_ticks = 3003;

    const auto frames = build_reordered_frames(80, frame_ticks);
    bool first = true;
    for (const auto& frame : frames) {
        remuxer.push(hevc_unit(2, frame.dts, frame.pts, frame.keyframe, first, broadcast_timescale));
        first = false;
    }
    remuxer.flush();

    check(sink.inits.size() == 1, "video push should have produced exactly one init segment");
    check(mdhd_timescale(sink.inits.front().data) == broadcast_timescale,
          "mdhd timescale must track the stream's own timescale, not a hardcoded default");

    const auto segments = segments_of(sink.segments, "video");
    check(segments.size() >= 2, "reordered video push should have spanned multiple fragments");

    std::vector<std::pair<std::int64_t, std::int64_t>> intervals;
    for (const auto& segment : segments) {
        std::int64_t dts = std::int64_t(segment.tfdt);
        auto min_pts = std::numeric_limits<std::int64_t>::max();
        auto max_end = std::numeric_limits<std::int64_t>::min();
        for (const auto& sample : segment.samples) {
            const auto pts = dts + sample.composition_offset;
            min_pts = std::min(min_pts, pts);
            max_end = std::max(max_end, pts + std::int64_t(sample.duration));
            dts += std::int64_t(sample.duration);
        }
        intervals.emplace_back(min_pts, max_end);
    }
    for (std::size_t i = 0; i + 1 < intervals.size(); ++i) {
        check(intervals[i].second <= intervals[i + 1].first,
              "fragment " + std::to_string(i) +
                  " composition interval overlaps the next fragment's under a broadcast "
                  "timescale, which Firefox's CtsComparator would misinterpret as evicting "
                  "already-buffered frames");
    }
}

// A track with a fixed, monotonically increasing DTS step but a monotonically
// DECREASING PTS never offers safe_prefix() a cut point. Every ready sample's
// duration is the constant DTS step, so its composition end is pts_0 + step --
// the maximum over the whole run, since pts only falls after that. Meanwhile
// the most recently queued sample (pending_, or the tail of ready_) always
// holds the minimum PTS of everything still queued past any candidate cut.
// So the cut test `prefix_end <= min_from[cut]` reduces to `pts_0 + step <=
// pts_of_latest_sample`, which is false as soon as more than one sample has
// been pushed (pts_of_latest_sample < pts_0 by then). Only BaseMuxer::enqueue's
// unconditional queue-duration bound can ever emit for such a stream.
void test_video_queue_bound_forces_emit_without_safe_cut() {
    TestSink sink;
    tlvdemux::MseRemuxer remuxer(sink);
    remuxer.selectTrack(tlvdemux::TrackKind::Video, 2);

    // Video track timescale defaults to 1000000, so the periodic-emit
    // threshold is 1000000 / kFragmentDurationDivisor(4) = 250000us and the
    // forced queue-duration bound is 8x that = 2000000us.
    constexpr std::int64_t dts_step = 70000;
    constexpr std::int64_t pts_step = 70000;
    constexpr std::int64_t initial_pts = 3000000;
    constexpr int sample_count = 35;

    for (int i = 0; i < sample_count; ++i) {
        const auto dts = dts_step * i;
        const auto pts = initial_pts - pts_step * i;
        check(pts >= 0, "test setup: PTS must stay non-negative for the whole run");
        remuxer.push(hevc_unit(2, dts, pts, i == 0, i == 0));
    }

    // Captured before flush(): if the bound never fired, nothing would have
    // been emitted yet and this would be empty.
    const auto segments_before_flush = segments_of(sink.segments, "video");
    check(!segments_before_flush.empty(),
          "a stream with no safe composition cut point must still be bounded by "
          "the forced queue-duration emit, not accumulate everything until flush()");

    constexpr std::uint64_t bound_us = 2000000;
    std::uint64_t first_segment_duration = 0;
    for (const auto& sample : segments_before_flush.front().samples) {
        first_segment_duration += sample.duration;
    }
    check(first_segment_duration >= bound_us,
          "the forced emit fired before the queue reached its duration bound");
    check(first_segment_duration < bound_us + std::uint64_t(dts_step),
          "the queue grew past its duration bound by more than a single sample");

    remuxer.flush();
}

// ITU-T H.265 8.1.3 / RFC 7798 1.1.4: NoRaslOutputFlag is 1 for a mid-stream
// BLA regardless of what came before it, so its RASL cannot be decoded.
void test_mid_stream_bla_drops_rasl() {
    TestSink sink;
    tlvdemux::MseRemuxer remuxer(sink);
    remuxer.selectTrack(tlvdemux::TrackKind::Video, 2);

    constexpr std::int64_t step = 100000;
    remuxer.push(hevc_unit(2, 0, 0, std::vector<unsigned>{21}, true));              // CRA opens the sequence
    remuxer.push(hevc_unit(2, step, step, std::vector<unsigned>{1}, false));        // trailing picture closes the initial window
    remuxer.push(hevc_unit(2, 2 * step, 2 * step, std::vector<unsigned>{1}, false));
    remuxer.push(hevc_unit(2, 3 * step, 3 * step, std::vector<unsigned>{17}, false));  // mid-stream BLA
    remuxer.push(hevc_unit(2, 4 * step, 4 * step, std::vector<unsigned>{9}, false));   // its RASL: must be dropped
    remuxer.push(hevc_unit(2, 5 * step, 5 * step, std::vector<unsigned>{1}, false));   // first trailing picture closes the window
    remuxer.flush();

    const auto segments = segments_of(sink.segments, "video");
    const std::vector<std::int64_t> expected{0, step, 2 * step, 3 * step, 5 * step};
    check(composition_timestamps(segments) == expected,
          "a mid-stream BLA's RASL access unit was not dropped");
}

// A CRA only gets NoRaslOutputFlag=1 when it opens a fresh coded video
// sequence, which an EOS/EOB NAL in a preceding access unit also triggers.
void test_cra_after_eos_drops_rasl() {
    TestSink sink;
    tlvdemux::MseRemuxer remuxer(sink);
    remuxer.selectTrack(tlvdemux::TrackKind::Video, 2);

    constexpr std::int64_t step = 100000;
    remuxer.push(hevc_unit(2, 0, 0, std::vector<unsigned>{21}, true));              // CRA opens the sequence
    remuxer.push(hevc_unit(2, step, step, std::vector<unsigned>{1}, false));        // trailing picture closes the initial window
    remuxer.push(hevc_unit(2, 2 * step, 2 * step, std::vector<unsigned>{1}, false));
    remuxer.push(hevc_unit(2, 3 * step, 3 * step, std::vector<unsigned>{}, false, 36u));  // EOS_NUT-only access unit
    remuxer.push(hevc_unit(2, 4 * step, 4 * step, std::vector<unsigned>{21}, false));     // CRA following the EOS
    remuxer.push(hevc_unit(2, 5 * step, 5 * step, std::vector<unsigned>{9}, false));      // its RASL: must be dropped
    remuxer.push(hevc_unit(2, 6 * step, 6 * step, std::vector<unsigned>{1}, false));      // first trailing picture closes the window
    remuxer.flush();

    const auto segments = segments_of(sink.segments, "video");
    const std::vector<std::int64_t> expected{0, step, 2 * step, 4 * step, 6 * step};
    check(composition_timestamps(segments) == expected,
          "a CRA that follows an end-of-sequence NAL did not drop its RASL");
}

// A plain mid-stream CRA -- not the first access unit and not preceded by an
// EOS/EOB -- never gets NoRaslOutputFlag=1, so its RASL is decodable and must
// reach the sample stream. Regression guard against over-dropping.
void test_plain_mid_stream_cra_keeps_rasl() {
    TestSink sink;
    tlvdemux::MseRemuxer remuxer(sink);
    remuxer.selectTrack(tlvdemux::TrackKind::Video, 2);

    constexpr std::int64_t step = 100000;
    remuxer.push(hevc_unit(2, 0, 0, std::vector<unsigned>{21}, true));              // CRA opens the sequence
    remuxer.push(hevc_unit(2, step, step, std::vector<unsigned>{1}, false));        // trailing picture closes the initial window
    remuxer.push(hevc_unit(2, 2 * step, 2 * step, std::vector<unsigned>{1}, false));
    remuxer.push(hevc_unit(2, 3 * step, 3 * step, std::vector<unsigned>{21}, false));  // plain mid-stream CRA
    remuxer.push(hevc_unit(2, 4 * step, 4 * step, std::vector<unsigned>{9}, false));   // its RASL: must survive
    remuxer.push(hevc_unit(2, 5 * step, 5 * step, std::vector<unsigned>{1}, false));
    remuxer.flush();

    const auto segments = segments_of(sink.segments, "video");
    const std::vector<std::int64_t> expected{0, step, 2 * step, 3 * step, 4 * step, 5 * step};
    check(composition_timestamps(segments) == expected,
          "a plain mid-stream CRA's RASL was dropped even though NoRaslOutputFlag was never armed for it");
}

// RASL must precede RADL in presentation order but not necessarily in
// decoding order, so a RADL of the same IRAP can arrive first. It is leading
// and decodable, so it must not end the drop window the IRAP armed.
void test_radl_does_not_reopen_gate() {
    TestSink sink;
    tlvdemux::MseRemuxer remuxer(sink);
    remuxer.selectTrack(tlvdemux::TrackKind::Video, 2);

    constexpr std::int64_t step = 100000;
    remuxer.push(hevc_unit(2, 0, 0, std::vector<unsigned>{21}, true));               // CRA opens the sequence, arms the latch
    remuxer.push(hevc_unit(2, step, step, std::vector<unsigned>{6}, false));         // RADL: leading, must not reopen the gate
    remuxer.push(hevc_unit(2, 2 * step, 2 * step, std::vector<unsigned>{9}, false)); // RASL: still dropped
    remuxer.push(hevc_unit(2, 3 * step, 3 * step, std::vector<unsigned>{1}, false)); // first trailing picture closes the window
    remuxer.flush();

    const auto segments = segments_of(sink.segments, "video");
    const std::vector<std::int64_t> expected{0, step, 3 * step};
    check(composition_timestamps(segments) == expected,
          "a RADL access unit reopened the RASL drop window before the first trailing picture");
}

void test_audio_channel_limit() {
    TestSink sink;
    tlvdemux::MseRemuxer remuxer(sink, tlvdemux::MseOptions{6});
    remuxer.selectTrack(tlvdemux::TrackKind::Audio, 1);
    remuxer.push(audio_unit(13));
    check(sink.inits.empty(), "22.2ch AAC escaped a six-channel MSE limit");

    remuxer.push(audio_unit(6));
    check(sink.inits.size() == 1 && sink.inits.front().channels == 6,
          "5.1ch AAC was not accepted after rejecting 22.2ch");
}

void test_unlimited_22_2_channel_count() {
    TestSink sink;
    tlvdemux::MseRemuxer remuxer(sink);
    remuxer.selectTrack(tlvdemux::TrackKind::Audio, 1);
    remuxer.push(audio_unit(13));
    check(sink.inits.size() == 1 && sink.inits.front().channels == 24,
          "AAC channel_configuration 13 was not exposed as 24 channels");
}

} // namespace

int main() {
    test_audio_channel_limit();
    test_unlimited_22_2_channel_count();
    test_audio_drops_non_advancing_dts();
    test_video_fragments_do_not_overlap_in_composition_time();
    test_video_fragments_do_not_overlap_with_broadcast_timescale();
    test_video_queue_bound_forces_emit_without_safe_cut();
    test_mid_stream_bla_drops_rasl();
    test_cra_after_eos_drops_rasl();
    test_plain_mid_stream_cra_keeps_rasl();
    test_radl_does_not_reopen_gate();
    std::cout << "mse remuxer tests passed\n";
}
