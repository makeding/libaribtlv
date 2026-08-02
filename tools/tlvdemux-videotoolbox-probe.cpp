#include <tlvdemux/demuxer.hpp>
#include <tlvdemux/mse_remuxer.hpp>

#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

struct NalUnit {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::uint8_t type = 0;
    std::uint8_t layer_id = 0;
};

struct Mp4Box {
    std::size_t offset = 0;
    std::size_t size = 0;
    std::size_t payload = 0;
    std::string type;
};

std::uint32_t be32(const std::uint8_t* data) {
    return (std::uint32_t(data[0]) << 24U) | (std::uint32_t(data[1]) << 16U) |
           (std::uint32_t(data[2]) << 8U) | std::uint32_t(data[3]);
}

std::uint64_t be64(const std::uint8_t* data) {
    return (std::uint64_t(be32(data)) << 32U) | be32(data + 4);
}

std::vector<Mp4Box> child_boxes(const std::vector<std::uint8_t>& data,
                                std::size_t begin, std::size_t end) {
    std::vector<Mp4Box> result;
    while (begin + 8 <= end) {
        std::uint64_t size = be32(data.data() + begin);
        std::size_t header = 8;
        if (size == 1) {
            if (begin + 16 > end) return {};
            size = be64(data.data() + begin + 8);
            header = 16;
        } else if (size == 0) {
            size = end - begin;
        }
        if (size < header || size > end - begin) return {};
        result.push_back({begin, static_cast<std::size_t>(size), begin + header,
                          std::string(reinterpret_cast<const char*>(data.data() + begin + 4), 4)});
        begin += static_cast<std::size_t>(size);
    }
    return begin == end ? result : std::vector<Mp4Box>{};
}

std::optional<Mp4Box> box_of_type(const std::vector<Mp4Box>& boxes,
                                  const std::string& type) {
    const auto found = std::find_if(boxes.begin(), boxes.end(), [&](const auto& box) {
        return box.type == type;
    });
    if (found == boxes.end()) return std::nullopt;
    return *found;
}

std::optional<Mp4Box> find_box_signature(const std::vector<std::uint8_t>& data,
                                         const std::string& type) {
    if (type.size() != 4) return std::nullopt;
    for (std::size_t offset = 4; offset + 4 <= data.size(); ++offset) {
        if (!std::equal(type.begin(), type.end(), data.begin() +
                        static_cast<std::ptrdiff_t>(offset))) continue;
        const auto size = be32(data.data() + offset - 4);
        if (size >= 8 && offset - 4 + size <= data.size()) {
            return Mp4Box{offset - 4, size, offset + 4, type};
        }
    }
    return std::nullopt;
}

std::vector<NalUnit> split_length_prefixed(const std::uint8_t* data, std::size_t size,
                                           std::uint8_t length_size) {
    std::vector<NalUnit> result;
    std::size_t offset = 0;
    while (offset < size) {
        if (length_size == 0 || length_size > 4 || offset + length_size > size) return {};
        std::uint32_t nal_size = 0;
        for (std::uint8_t index = 0; index < length_size; ++index) {
            nal_size = (nal_size << 8U) | data[offset++];
        }
        if (nal_size < 2 || nal_size > size - offset) return {};
        result.push_back({data + offset, nal_size,
                          static_cast<std::uint8_t>((data[offset] >> 1U) & 0x3fU),
                          static_cast<std::uint8_t>(((data[offset] & 1U) << 5U) |
                                                    (data[offset + 1] >> 3U))});
        offset += nal_size;
    }
    return result;
}

std::vector<NalUnit> split_annex_b(const std::vector<std::uint8_t>& bytes) {
    std::vector<NalUnit> result;
    const auto start_code = [&bytes](const std::size_t offset) -> std::size_t {
        if (offset + 3 <= bytes.size() && bytes[offset] == 0 && bytes[offset + 1] == 0 &&
            bytes[offset + 2] == 1) return 3;
        if (offset + 4 <= bytes.size() && bytes[offset] == 0 && bytes[offset + 1] == 0 &&
            bytes[offset + 2] == 0 && bytes[offset + 3] == 1) return 4;
        return 0;
    };

    std::size_t cursor = 0;
    while (cursor < bytes.size()) {
        const auto prefix = start_code(cursor);
        if (prefix == 0) {
            ++cursor;
            continue;
        }
        const auto nal_start = cursor + prefix;
        auto nal_end = nal_start;
        while (nal_end < bytes.size() && start_code(nal_end) == 0) ++nal_end;
        if (nal_end > nal_start && nal_end - nal_start >= 2) {
            result.push_back({
                bytes.data() + nal_start,
                nal_end - nal_start,
                static_cast<std::uint8_t>((bytes[nal_start] >> 1U) & 0x3fU),
                static_cast<std::uint8_t>(((bytes[nal_start] & 1U) << 5U) |
                                          (bytes[nal_start + 1] >> 3U)),
            });
        }
        cursor = nal_end;
    }
    return result;
}

std::string nal_types(const std::vector<NalUnit>& nals) {
    std::string result;
    for (const auto& nal : nals) {
        if (!result.empty()) result += ',';
        result += std::to_string(nal.type);
        result += '@';
        result += std::to_string(nal.layer_id);
    }
    return result;
}

class Probe final : public tlvdemux::Sink, public tlvdemux::MseSink {
public:
    Probe(const std::size_t maximum_access_units, const bool skip_leading_rasl,
          const double playback_rate, const std::size_t inflight_frames,
          const bool prepend_parameter_sets_on_irap, const bool mse_pipeline,
          const bool timeline_only, const std::optional<std::uint16_t> audio_packet_id,
          const bool require_hardware)
        : wanted_audio_packet_id_(audio_packet_id),
          maximum_access_units_(maximum_access_units),
          skip_leading_rasl_(skip_leading_rasl),
          playback_rate_(playback_rate),
          inflight_frames_(inflight_frames),
          prepend_parameter_sets_on_irap_(prepend_parameter_sets_on_irap),
          mse_pipeline_(mse_pipeline), timeline_only_(timeline_only),
          require_hardware_(require_hardware),
          mse_remuxer_(*this) {}

    ~Probe() override {
        finish();
        if (session_ != nullptr) CFRelease(session_);
        if (format_ != nullptr) CFRelease(format_);
    }

    void onService(const tlvdemux::ServiceInfo&) override {}

    void onTrack(const tlvdemux::TrackInfo& track) override {
        if (!video_track_.has_value() && track.kind == tlvdemux::TrackKind::Video &&
            track.codec == tlvdemux::Codec::Hevc) {
            video_track_ = track.track_id;
            if (mse_pipeline_) mse_remuxer_.selectTrack(tlvdemux::TrackKind::Video,
                                                        track.track_id);
            std::cerr << "video track packet_id=0x" << std::hex << track.packet_id << std::dec
                      << " timescale=" << track.timescale << '\n';
        } else if (mse_pipeline_ && !audio_track_.has_value() &&
                   track.kind == tlvdemux::TrackKind::Audio && track.audio.has_value() &&
                   tlvdemux::audio_channel_count(track.audio->channel_layout) <= 6 &&
                   (!wanted_audio_packet_id_.has_value() ||
                    track.packet_id == *wanted_audio_packet_id_)) {
            audio_track_ = track.track_id;
            mse_remuxer_.selectTrack(tlvdemux::TrackKind::Audio, track.track_id);
            std::cerr << "audio track packet_id=0x" << std::hex << track.packet_id << std::dec
                      << " timescale=" << track.timescale << '\n';
        }
    }

    void onAccessUnit(tlvdemux::AccessUnit&& unit) override {
        if (done_) return;
        if (mse_pipeline_ && audio_track_.has_value() && unit.track_id == *audio_track_ &&
            unit.codec == tlvdemux::Codec::AacLatm) {
            mse_remuxer_.push(unit);
            return;
        }
        if (!video_track_.has_value() || unit.track_id != *video_track_ ||
            unit.codec != tlvdemux::Codec::Hevc) return;

        if (unit.discontinuity) {
            std::cerr << "video discontinuity pts=" << seconds(unit.pts)
                      << " dts=" << seconds(unit.dts)
                      << " rap=" << (unit.random_access ? 1 : 0)
                      << " input_offset=" << unit.input_offset << '\n';
        }

        if (mse_pipeline_) {
            mse_remuxer_.push(unit);
            return;
        }

        const auto nals = split_annex_b(unit.data);
        remember_parameter_sets(nals);
        if (session_ == nullptr && !create_session()) return;

        const bool has_vcl = std::any_of(nals.begin(), nals.end(),
                                         [](const auto& nal) { return nal.type <= 31; });
        const bool has_irap = std::any_of(nals.begin(), nals.end(),
                                          [](const auto& nal) {
                                              return nal.type >= 16 && nal.type <= 23;
                                          });
        const bool only_rasl_vcl = has_vcl && std::all_of(
            nals.begin(), nals.end(), [](const auto& nal) {
                return nal.type > 31 || nal.type == 8 || nal.type == 9;
            });
        if (skip_leading_rasl_ && waiting_for_first_trailing_picture_ && only_rasl_vcl) {
            std::cerr << "skip leading RASL pts=" << seconds(unit.pts)
                      << " dts=" << seconds(unit.dts)
                      << " nal=" << nal_types(nals) << '\n';
            return;
        }
        if (waiting_for_first_trailing_picture_ && has_vcl && !has_irap) {
            waiting_for_first_trailing_picture_ = false;
        }

        pace(unit.dts);

        ++access_unit_count_;
        const auto status = decode(unit, nals);
        std::cerr << "au=" << access_unit_count_
                  << " pts=" << seconds(unit.pts)
                  << " dts=" << seconds(unit.dts)
                  << " rap=" << (unit.random_access ? 1 : 0)
                  << " nal=" << nal_types(nals)
                  << " submit=" << status << '\n';
        if (status != noErr || callback_status_.load() != noErr ||
            access_unit_count_ >= maximum_access_units_) {
            done_ = true;
        }
        if (access_unit_count_ == 1 && has_irap) waiting_for_first_trailing_picture_ = true;
    }

    void onError(const tlvdemux::Error& error) override {
        if (error.code == tlvdemux::ErrorCode::Discontinuity) {
            std::cerr << "demux discontinuity @" << error.input_offset
                      << ": " << error.message << '\n';
        }
        if (!error.recoverable) {
            std::cerr << "demux error @" << error.input_offset << ": " << error.message << '\n';
            pipeline_ok_ = false;
            done_ = true;
        }
    }

    void onMseInit(tlvdemux::MseTrackInit&& init) override {
        if (init.type == "audio") {
            audio_timescale_ = init.sample_rate;
            std::cerr << "mse init " << init.mime << " sample_rate=" << init.sample_rate
                      << " channels=" << init.channels << '\n';
            return;
        }
        if (init.type != "video") return;
        const auto hvcc = find_box_signature(init.data, "hvcC");
        if (!hvcc || hvcc->payload + 23 > hvcc->offset + hvcc->size) {
            fail_pipeline("video init segment has no valid hvcC");
            return;
        }
        const auto* payload = init.data.data() + hvcc->payload;
        const auto payload_size = hvcc->size - (hvcc->payload - hvcc->offset);
        mse_nal_length_size_ = static_cast<std::uint8_t>((payload[21] & 3U) + 1U);
        const auto array_count = payload[22];
        std::size_t offset = 23;
        std::array<std::vector<std::uint8_t>, 3> parsed_parameter_sets;
        for (std::uint8_t array_index = 0; array_index < array_count; ++array_index) {
            if (offset + 3 > payload_size) {
                fail_pipeline("truncated hvcC array header");
                return;
            }
            const auto type = static_cast<std::uint8_t>(payload[offset++] & 0x3fU);
            const auto count = static_cast<std::uint16_t>(
                (std::uint16_t(payload[offset]) << 8U) | payload[offset + 1]);
            offset += 2;
            for (std::uint16_t index = 0; index < count; ++index) {
                if (offset + 2 > payload_size) {
                    fail_pipeline("truncated hvcC NAL length");
                    return;
                }
                const auto size = static_cast<std::uint16_t>(
                    (std::uint16_t(payload[offset]) << 8U) | payload[offset + 1]);
                offset += 2;
                if (size < 2 || offset + size > payload_size) {
                    fail_pipeline("invalid hvcC NAL payload");
                    return;
                }
                if (type >= 32 && type <= 34 && parsed_parameter_sets[type - 32].empty()) {
                    parsed_parameter_sets[type - 32].assign(payload + offset,
                                                             payload + offset + size);
                }
                offset += size;
            }
        }
        if (std::any_of(parsed_parameter_sets.begin(), parsed_parameter_sets.end(),
                        [](const auto& value) { return value.empty(); })) {
            fail_pipeline("hvcC does not contain VPS/SPS/PPS");
            return;
        }
        reset_decoder();
        parameter_sets_ = parsed_parameter_sets;
        mse_config_parameter_sets_ = std::move(parsed_parameter_sets);
        std::cerr << "mse init " << init.mime << " length_size="
                  << unsigned(mse_nal_length_size_) << " size=" << init.width << 'x'
                  << init.height << '\n';
    }

    void onMseSegment(tlvdemux::MseMediaSegment&& segment) override {
        if (done_) return;
        try {
            if (segment.type == "video") decode_mse_segment(segment.data);
            else if (segment.type == "audio") validate_audio_mse_segment(segment.data);
        } catch (const std::exception& error) {
            fail_pipeline(error.what());
        }
    }

    void onMseVideoStart(const tlvdemux::MseVideoStart& start) override {
        std::cerr << "mse video start nal=" << start.nal_type
                  << " signalled_rap=" << (start.signalled_random_access ? 1 : 0) << '\n';
    }

    bool done() const { return done_; }
    bool ok() const {
        return pipeline_ok_ && callback_status_.load() == noErr &&
            (timeline_only_ ? access_unit_count_ != 0 : decoded_count_.load() != 0);
    }
    OSStatus callback_status() const { return callback_status_.load(); }
    std::size_t decoded_count() const { return decoded_count_.load(); }
    std::size_t timeline_gap_count() const { return timeline_gap_count_; }
    std::uint64_t largest_timeline_gap_us() const { return largest_timeline_gap_us_; }
    std::size_t audio_sample_count() const { return audio_sample_count_; }
    std::size_t audio_timeline_gap_count() const { return audio_timeline_gap_count_; }
    std::uint64_t largest_audio_timeline_gap() const { return largest_audio_timeline_gap_; }
    std::size_t mse_pps_sample_count() const { return mse_pps_sample_count_; }
    std::size_t mse_pps_variant_count() const { return mse_pps_variants_.size(); }

    void finish() {
        if (mse_pipeline_ && !mse_flushed_) {
            mse_flushed_ = true;
            mse_remuxer_.flush();
        }
        if (session_ != nullptr) VTDecompressionSessionWaitForAsynchronousFrames(session_);
    }

private:
    static double seconds(const tlvdemux::Timestamp timestamp) {
        if (timestamp.timescale == 0) return 0.0;
        return static_cast<double>(timestamp.value) / static_cast<double>(timestamp.timescale);
    }

    void remember_parameter_sets(const std::vector<NalUnit>& nals) {
        for (const auto& nal : nals) {
            if (nal.type < 32 || nal.type > 34) continue;
            auto& target = parameter_sets_[nal.type - 32];
            target.assign(nal.data, nal.data + nal.size);
        }
    }

    void pace(const tlvdemux::Timestamp dts) {
        if (playback_rate_ <= 0.0 || dts.timescale == 0) return;
        pace_seconds(seconds(dts));
    }

    void pace_seconds(const double value) {
        if (playback_rate_ <= 0.0) return;
        if (!first_dts_seconds_.has_value()) {
            first_dts_seconds_ = value;
            pacing_started_ = std::chrono::steady_clock::now();
            return;
        }
        const auto media_seconds = value - *first_dts_seconds_;
        if (media_seconds <= 0.0) return;
        const auto target = pacing_started_ + std::chrono::duration_cast<
            std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(media_seconds / playback_rate_));
        std::this_thread::sleep_until(target);
    }

    void fail_pipeline(const std::string& message) {
        pipeline_ok_ = false;
        done_ = true;
        std::cerr << "mse pipeline error: " << message << '\n';
    }

    void reset_decoder() {
        if (session_ != nullptr) {
            VTDecompressionSessionWaitForAsynchronousFrames(session_);
            VTDecompressionSessionInvalidate(session_);
            CFRelease(session_);
            session_ = nullptr;
        }
        if (format_ != nullptr) {
            CFRelease(format_);
            format_ = nullptr;
        }
        frames_since_wait_ = 0;
        decoder_parameter_sets_ = {};
    }

    bool create_session() {
        for (const auto& parameter_set : parameter_sets_) {
            if (parameter_set.empty()) return false;
        }
        std::array<const std::uint8_t*, 3> pointers{};
        std::array<std::size_t, 3> sizes{};
        for (std::size_t index = 0; index < parameter_sets_.size(); ++index) {
            pointers[index] = parameter_sets_[index].data();
            sizes[index] = parameter_sets_[index].size();
        }
        auto status = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
            kCFAllocatorDefault, pointers.size(), pointers.data(), sizes.data(), 4, nullptr,
            &format_);
        if (status != noErr) {
            std::cerr << "CMVideoFormatDescriptionCreateFromHEVCParameterSets: " << status << '\n';
            done_ = true;
            return false;
        }
        decoder_parameter_sets_ = parameter_sets_;

        CFDictionaryRef decoder_specification = nullptr;
        if (require_hardware_) {
            const void* decoder_keys[] = {
                kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder,
            };
            const void* decoder_values[] = {kCFBooleanTrue};
            decoder_specification = CFDictionaryCreate(
                kCFAllocatorDefault, decoder_keys, decoder_values, 1,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        }
        VTDecompressionOutputCallbackRecord callback{&Probe::output_callback, this};
        status = VTDecompressionSessionCreate(kCFAllocatorDefault, format_,
                                               decoder_specification, nullptr,
                                               &callback, &session_);
        if (decoder_specification != nullptr) CFRelease(decoder_specification);
        if (status != noErr) {
            std::cerr << "VTDecompressionSessionCreate: " << status << '\n';
            done_ = true;
            return false;
        }
        CFTypeRef hardware_value = nullptr;
        const auto property_status = VTSessionCopyProperty(
            session_, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,
            kCFAllocatorDefault, &hardware_value);
        const bool hardware = property_status == noErr && hardware_value == kCFBooleanTrue;
        if (hardware_value != nullptr) CFRelease(hardware_value);
        std::cerr << "VideoToolbox session created (hardware="
                  << (hardware ? "yes" : "unknown") << ")\n";
        return true;
    }

    static bool append_nal(std::vector<std::uint8_t>& sample, const std::uint8_t* data,
                           const std::size_t nal_size) {
        if (nal_size > std::numeric_limits<std::uint32_t>::max()) return false;
        const auto size = static_cast<std::uint32_t>(nal_size);
        sample.push_back(static_cast<std::uint8_t>(size >> 24U));
        sample.push_back(static_cast<std::uint8_t>(size >> 16U));
        sample.push_back(static_cast<std::uint8_t>(size >> 8U));
        sample.push_back(static_cast<std::uint8_t>(size));
        sample.insert(sample.end(), data, data + nal_size);
        return true;
    }

    OSStatus submit_sample(const std::vector<std::uint8_t>& sample, const CMTime pts,
                           const CMTime dts, const bool random_access) {
        if (sample.empty() || session_ == nullptr || format_ == nullptr) return paramErr;
        CMBlockBufferRef block = nullptr;
        auto status = CMBlockBufferCreateWithMemoryBlock(
            kCFAllocatorDefault, nullptr, sample.size(), kCFAllocatorDefault, nullptr, 0,
            sample.size(), 0, &block);
        if (status != noErr) return status;
        status = CMBlockBufferReplaceDataBytes(sample.data(), block, 0, sample.size());
        if (status != noErr) {
            CFRelease(block);
            return status;
        }

        const CMSampleTimingInfo timing{kCMTimeInvalid, pts, dts};
        const auto sample_size = sample.size();
        CMSampleBufferRef buffer = nullptr;
        status = CMSampleBufferCreateReady(kCFAllocatorDefault, block, format_, 1, 1, &timing,
                                           1, &sample_size, &buffer);
        CFRelease(block);
        if (status != noErr) return status;

        if (!random_access) {
            const auto attachments = CMSampleBufferGetSampleAttachmentsArray(buffer, true);
            if (attachments != nullptr && CFArrayGetCount(attachments) != 0) {
                auto dictionary = reinterpret_cast<CFMutableDictionaryRef>(
                    const_cast<void*>(CFArrayGetValueAtIndex(attachments, 0)));
                CFDictionarySetValue(dictionary, kCMSampleAttachmentKey_NotSync,
                                     kCFBooleanTrue);
            }
        }

        VTDecodeInfoFlags flags = 0;
        status = VTDecompressionSessionDecodeFrame(
            session_, buffer, kVTDecodeFrame_EnableAsynchronousDecompression, nullptr, &flags);
        CFRelease(buffer);
        if (status == noErr) {
            ++frames_since_wait_;
            if (frames_since_wait_ >= inflight_frames_) {
                status = VTDecompressionSessionWaitForAsynchronousFrames(session_);
                frames_since_wait_ = 0;
            }
        }
        return status;
    }

    OSStatus decode(const tlvdemux::AccessUnit& unit, const std::vector<NalUnit>& nals) {
        std::vector<std::uint8_t> sample;
        const bool has_irap = std::any_of(nals.begin(), nals.end(), [](const auto& nal) {
            return nal.type >= 16 && nal.type <= 23;
        });
        std::size_t first_original = 0;
        if (prepend_parameter_sets_on_irap_ && has_irap) {
            // Make the hvcC parameter sets available at the random-access
            // sample while retaining any in-band updates in the access unit.
            if (!nals.empty() && nals.front().type == 35) {
                if (!append_nal(sample, nals.front().data, nals.front().size)) return paramErr;
                first_original = 1;
            }
            for (const auto& parameter_set : decoder_parameter_sets_) {
                if (!append_nal(sample, parameter_set.data(), parameter_set.size())) return paramErr;
            }
        }
        for (std::size_t index = first_original; index < nals.size(); ++index) {
            const auto& nal = nals[index];
            if (nal.size > std::numeric_limits<std::uint32_t>::max()) return paramErr;
            if (!append_nal(sample, nal.data, nal.size)) return paramErr;
        }
        return submit_sample(
            sample,
            CMTimeMake(unit.pts.value, static_cast<std::int32_t>(unit.pts.timescale)),
            CMTimeMake(unit.dts.value, static_cast<std::int32_t>(unit.dts.timescale)),
            unit.random_access);
    }

    void decode_mse_segment(const std::vector<std::uint8_t>& data) {
        const auto top = child_boxes(data, 0, data.size());
        const auto moof = box_of_type(top, "moof");
        const auto mdat = box_of_type(top, "mdat");
        if (!moof || !mdat) throw std::runtime_error("media segment requires moof and mdat");
        const auto moof_children = child_boxes(data, moof->payload,
                                               moof->offset + moof->size);
        const auto traf = box_of_type(moof_children, "traf");
        if (!traf) throw std::runtime_error("moof has no traf");
        const auto traf_children = child_boxes(data, traf->payload,
                                               traf->offset + traf->size);
        const auto tfdt = box_of_type(traf_children, "tfdt");
        const auto trun = box_of_type(traf_children, "trun");
        if (!tfdt || !trun) throw std::runtime_error("traf requires tfdt and trun");
        if (tfdt->payload + 8 > tfdt->offset + tfdt->size ||
            trun->payload + 12 > trun->offset + trun->size) {
            throw std::runtime_error("truncated tfdt/trun");
        }

        const auto tfdt_version = data[tfdt->payload];
        const auto tfdt_data = tfdt->payload + 4;
        std::uint64_t decode_time = 0;
        if (tfdt_version == 1) {
            if (tfdt_data + 8 > tfdt->offset + tfdt->size)
                throw std::runtime_error("truncated 64-bit tfdt");
            decode_time = be64(data.data() + tfdt_data);
        } else if (tfdt_version == 0) {
            if (tfdt_data + 4 > tfdt->offset + tfdt->size)
                throw std::runtime_error("truncated 32-bit tfdt");
            decode_time = be32(data.data() + tfdt_data);
        } else {
            throw std::runtime_error("unsupported tfdt version");
        }

        const auto trun_version = data[trun->payload];
        const auto trun_flags = (std::uint32_t(data[trun->payload + 1]) << 16U) |
                                (std::uint32_t(data[trun->payload + 2]) << 8U) |
                                std::uint32_t(data[trun->payload + 3]);
        constexpr std::uint32_t required_flags = 0x000f01;
        if ((trun_flags & required_flags) != required_flags)
            throw std::runtime_error("trun lacks per-sample duration/size/flags/cto");
        const auto sample_count = be32(data.data() + trun->payload + 4);
        const auto signed_data_offset = static_cast<std::int32_t>(
            be32(data.data() + trun->payload + 8));
        const auto payload_position_signed = static_cast<std::int64_t>(moof->offset) +
                                             signed_data_offset;
        if (payload_position_signed < 0)
            throw std::runtime_error("negative trun data offset");
        auto payload_position = static_cast<std::size_t>(payload_position_signed);
        if (payload_position < mdat->payload || payload_position > mdat->offset + mdat->size)
            throw std::runtime_error("trun data offset does not point into mdat");

        auto entry = trun->payload + 12;
        std::uint64_t dts = decode_time;
        if (previous_mse_decode_end_.has_value() && dts < *previous_mse_decode_end_) {
            throw std::runtime_error(
                "MSE decode timeline overlap: tfdt=" + std::to_string(dts) +
                " previous_end=" + std::to_string(*previous_mse_decode_end_));
        }
        if (previous_mse_decode_end_.has_value() && dts > *previous_mse_decode_end_) {
            const auto gap = dts - *previous_mse_decode_end_;
            ++timeline_gap_count_;
            largest_timeline_gap_us_ = std::max(largest_timeline_gap_us_, gap);
            std::cerr << "mse timeline gap fragment=" << (mse_fragment_count_ + 1)
                      << " previous_end="
                      << (static_cast<double>(*previous_mse_decode_end_) / 1000000.0)
                      << " next_dts=" << (static_cast<double>(dts) / 1000000.0)
                      << " gap=" << (static_cast<double>(gap) / 1000000.0) << '\n';
        }
        ++mse_fragment_count_;
        for (std::uint32_t sample_index = 0; sample_index < sample_count; ++sample_index) {
            if (entry + 16 > trun->offset + trun->size)
                throw std::runtime_error("truncated trun sample table");
            const auto duration = be32(data.data() + entry);
            const auto size = be32(data.data() + entry + 4);
            const auto flags = be32(data.data() + entry + 8);
            const auto cto_bits = be32(data.data() + entry + 12);
            const auto composition_offset = trun_version == 1
                ? static_cast<std::int64_t>(static_cast<std::int32_t>(cto_bits))
                : static_cast<std::int64_t>(cto_bits);
            entry += 16;
            if (duration == 0) throw std::runtime_error("zero-duration video sample");
            if (size == 0 || size > data.size() - payload_position)
                throw std::runtime_error("invalid mdat sample size");
            const auto nals = split_length_prefixed(data.data() + payload_position, size,
                                                     mse_nal_length_size_);
            if (nals.empty()) throw std::runtime_error("invalid length-prefixed HEVC sample");
            bool has_inband_pps = false;
            for (const auto& nal : nals) {
                if (nal.type != 34) continue;
                has_inband_pps = true;
                const bool inserted =
                    mse_pps_variants_.emplace(nal.data, nal.data + nal.size).second;
                if (inserted) {
                    std::cerr << "mse pps variant=" << mse_pps_variants_.size()
                              << " first_dts="
                              << (static_cast<double>(dts) / 1000000.0)
                              << " size=" << nal.size << '\n';
                }
            }
            if (has_inband_pps) ++mse_pps_sample_count_;

            const bool metadata_sync = (flags & 0x00010000U) == 0;
            const auto depends_on = static_cast<std::uint8_t>((flags >> 24U) & 3U);
            const bool metadata_keyframe = metadata_sync && depends_on != 1;
            const bool bitstream_keyframe = std::any_of(nals.begin(), nals.end(),
                [](const auto& nal) { return nal.type >= 16 && nal.type <= 23; });
            if (metadata_keyframe != bitstream_keyframe) {
                std::cerr << "mse keyframe mismatch fragment=" << mse_fragment_count_
                          << " sample=" << sample_index
                          << " metadata=" << (metadata_keyframe ? 1 : 0)
                          << " bitstream=" << (bitstream_keyframe ? 1 : 0) << '\n';
            }

            if (timeline_only_) {
                ++access_unit_count_;
                payload_position += size;
                dts += duration;
                if (access_unit_count_ >= maximum_access_units_) {
                    done_ = true;
                    previous_mse_decode_end_ = dts;
                    return;
                }
                continue;
            }

            // Model decoder input for hvc1: make the hvcC parameter sets
            // available at a coded keyframe while retaining any in-band
            // updates from the media sample.
            std::vector<std::vector<std::uint8_t>> injected;
            std::vector<NalUnit> converted;
            std::size_t first_original = 0;
            if (bitstream_keyframe && !nals.empty() && nals.front().type == 35) {
                converted.push_back(nals.front());
                first_original = 1;
            }
            if (bitstream_keyframe) {
                for (std::size_t index = 0; index < mse_config_parameter_sets_.size(); ++index) {
                    injected.push_back(mse_config_parameter_sets_[index]);
                    const auto& bytes = injected.back();
                    converted.push_back({bytes.data(), bytes.size(),
                                         static_cast<std::uint8_t>(32 + index), 0});
                }
            }
            converted.insert(converted.end(), nals.begin() +
                             static_cast<std::ptrdiff_t>(first_original), nals.end());
            remember_parameter_sets(converted);
            if (session_ == nullptr && !create_session()) {
                if (done_) return;
                throw std::runtime_error("cannot create VideoToolbox session without parameter sets");
            }
            std::vector<std::uint8_t> vt_sample;
            for (const auto& nal : converted) {
                if (nal.type != 35 && !append_nal(vt_sample, nal.data, nal.size))
                    throw std::runtime_error("HEVC NAL is too large");
            }
            if (vt_sample.empty()) throw std::runtime_error("HEVC sample is empty");

            const auto pts_signed = static_cast<std::int64_t>(dts) + composition_offset;
            if (pts_signed < 0) throw std::runtime_error("negative MSE presentation timestamp");
            pace_seconds(static_cast<double>(dts) / 1000000.0);
            ++access_unit_count_;
            const auto status = submit_sample(
                vt_sample, CMTimeMake(pts_signed, 1000000), CMTimeMake(dts, 1000000),
                bitstream_keyframe);
            std::cerr << "mse au=" << access_unit_count_
                      << " fragment=" << mse_fragment_count_
                      << " dts=" << (static_cast<double>(dts) / 1000000.0)
                      << " pts=" << (static_cast<double>(pts_signed) / 1000000.0)
                      << " metadata_key=" << (metadata_keyframe ? 1 : 0)
                      << " bitstream_key=" << (bitstream_keyframe ? 1 : 0)
                      << " nal=" << nal_types(nals) << " submit=" << status << '\n';
            if (status != noErr || callback_status_.load() != noErr) {
                pipeline_ok_ = false;
                done_ = true;
                return;
            }
            payload_position += size;
            dts += duration;
            if (access_unit_count_ >= maximum_access_units_) {
                done_ = true;
                previous_mse_decode_end_ = dts;
                return;
            }
        }
        previous_mse_decode_end_ = dts;
    }

    void validate_audio_mse_segment(const std::vector<std::uint8_t>& data) {
        const auto top = child_boxes(data, 0, data.size());
        const auto moof = box_of_type(top, "moof");
        const auto mdat = box_of_type(top, "mdat");
        if (!moof || !mdat) throw std::runtime_error("audio segment requires moof and mdat");
        const auto moof_children = child_boxes(data, moof->payload,
                                               moof->offset + moof->size);
        const auto traf = box_of_type(moof_children, "traf");
        if (!traf) throw std::runtime_error("audio moof has no traf");
        const auto traf_children = child_boxes(data, traf->payload,
                                               traf->offset + traf->size);
        const auto tfdt = box_of_type(traf_children, "tfdt");
        const auto trun = box_of_type(traf_children, "trun");
        if (!tfdt || !trun) throw std::runtime_error("audio traf requires tfdt and trun");
        if (tfdt->payload + 12 > tfdt->offset + tfdt->size ||
            trun->payload + 12 > trun->offset + trun->size) {
            throw std::runtime_error("truncated audio tfdt/trun");
        }
        if (data[tfdt->payload] != 1) {
            throw std::runtime_error("audio tfdt is not version 1");
        }
        auto dts = be64(data.data() + tfdt->payload + 4);
        const auto sample_count = be32(data.data() + trun->payload + 4);
        auto entry = trun->payload + 12;
        const auto timescale = audio_timescale_ == 0 ? 1U : audio_timescale_;
        if (previous_audio_decode_end_.has_value()) {
            if (dts < *previous_audio_decode_end_) {
                throw std::runtime_error(
                    "audio MSE timeline overlap: tfdt=" + std::to_string(dts) +
                    " previous_end=" + std::to_string(*previous_audio_decode_end_));
            }
            if (dts > *previous_audio_decode_end_) {
                const auto gap = dts - *previous_audio_decode_end_;
                ++audio_timeline_gap_count_;
                largest_audio_timeline_gap_ = std::max(largest_audio_timeline_gap_, gap);
                std::cerr << "audio timeline gap fragment=" << (audio_fragment_count_ + 1)
                          << " previous_end="
                          << (static_cast<double>(*previous_audio_decode_end_) / timescale)
                          << " next_dts=" << (static_cast<double>(dts) / timescale)
                          << " gap=" << (static_cast<double>(gap) / timescale) << '\n';
            }
        }
        ++audio_fragment_count_;
        for (std::uint32_t index = 0; index < sample_count; ++index) {
            if (entry + 16 > trun->offset + trun->size) {
                throw std::runtime_error("truncated audio trun sample table");
            }
            const auto duration = be32(data.data() + entry);
            const auto size = be32(data.data() + entry + 4);
            if (duration == 0 || size == 0) {
                throw std::runtime_error("invalid audio sample duration or size");
            }
            entry += 16;
            dts += duration;
            ++audio_sample_count_;
        }
        previous_audio_decode_end_ = dts;
    }

    static void output_callback(void* context, void*, const OSStatus status,
                                VTDecodeInfoFlags flags, CVImageBufferRef,
                                CMTime presentation_time, CMTime) {
        auto& probe = *static_cast<Probe*>(context);
        if (status != noErr) probe.callback_status_.store(status);
        if (status == noErr) ++probe.decoded_count_;
        std::cerr << "  callback status=" << status << " flags=0x" << std::hex << flags
                  << std::dec << " pts=" << CMTimeGetSeconds(presentation_time) << '\n';
    }

    std::optional<std::uint64_t> video_track_;
    std::optional<std::uint64_t> audio_track_;
    std::optional<std::uint16_t> wanted_audio_packet_id_;
    std::array<std::vector<std::uint8_t>, 3> parameter_sets_;
    std::array<std::vector<std::uint8_t>, 3> decoder_parameter_sets_;
    CMVideoFormatDescriptionRef format_ = nullptr;
    VTDecompressionSessionRef session_ = nullptr;
    std::atomic<OSStatus> callback_status_{noErr};
    std::atomic<std::size_t> decoded_count_{0};
    std::size_t access_unit_count_ = 0;
    std::size_t maximum_access_units_ = 300;
    bool skip_leading_rasl_ = false;
    bool waiting_for_first_trailing_picture_ = false;
    double playback_rate_ = 0.0;
    std::size_t inflight_frames_ = 1;
    std::size_t frames_since_wait_ = 0;
    bool prepend_parameter_sets_on_irap_ = false;
    std::optional<double> first_dts_seconds_;
    std::chrono::steady_clock::time_point pacing_started_{};
    bool mse_pipeline_ = false;
    bool timeline_only_ = false;
    bool require_hardware_ = true;
    bool mse_flushed_ = false;
    bool pipeline_ok_ = true;
    std::uint8_t mse_nal_length_size_ = 4;
    std::array<std::vector<std::uint8_t>, 3> mse_config_parameter_sets_;
    std::optional<std::uint64_t> previous_mse_decode_end_;
    std::size_t mse_fragment_count_ = 0;
    std::size_t timeline_gap_count_ = 0;
    std::uint64_t largest_timeline_gap_us_ = 0;
    std::size_t mse_pps_sample_count_ = 0;
    std::set<std::vector<std::uint8_t>> mse_pps_variants_;
    std::uint32_t audio_timescale_ = 0;
    std::optional<std::uint64_t> previous_audio_decode_end_;
    std::size_t audio_fragment_count_ = 0;
    std::size_t audio_sample_count_ = 0;
    std::size_t audio_timeline_gap_count_ = 0;
    std::uint64_t largest_audio_timeline_gap_ = 0;
    tlvdemux::MseRemuxer mse_remuxer_;
    bool done_ = false;
};

struct Options {
    std::string path;
    std::size_t maximum_access_units = 300;
    std::uint64_t offset = 0;
    double playback_rate = 0.0;
    std::size_t inflight_frames = 1;
    bool prepend_parameter_sets_on_irap = false;
    bool skip_leading_rasl = false;
    bool mse_pipeline = false;
    bool timeline_only = false;
    bool require_hardware = true;
    std::optional<std::uint16_t> audio_packet_id;
    std::size_t random_seeks = 0;
    std::uint64_t seed = 0x544c564d5345ULL;
};

Options parse_options(const int argc, char** argv) {
    Options options;
    bool legacy_maximum_seen = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto value = [&](const char* name) -> std::string {
            if (++index >= argc) {
                std::cerr << "missing value for " << name << '\n';
                std::exit(2);
            }
            return argv[index];
        };
        if (argument == "--skip-leading-rasl") {
            options.skip_leading_rasl = true;
        } else if (argument == "--mse") {
            options.mse_pipeline = true;
        } else if (argument == "--timeline-only") {
            options.timeline_only = true;
        } else if (argument == "--allow-software") {
            options.require_hardware = false;
        } else if (argument == "--audio-packet-id") {
            options.audio_packet_id = static_cast<std::uint16_t>(
                std::strtoul(value("--audio-packet-id").c_str(), nullptr, 0));
        } else if (argument == "--random-seeks") {
            options.random_seeks = static_cast<std::size_t>(
                std::strtoull(value("--random-seeks").c_str(), nullptr, 0));
        } else if (argument == "--seed") {
            options.seed = std::strtoull(value("--seed").c_str(), nullptr, 0);
        } else if (argument == "--prepend-parameter-sets-on-irap") {
            options.prepend_parameter_sets_on_irap = true;
        } else if (argument == "--max-au") {
            options.maximum_access_units = static_cast<std::size_t>(
                std::strtoull(value("--max-au").c_str(), nullptr, 0));
        } else if (argument == "--offset") {
            options.offset = std::strtoull(value("--offset").c_str(), nullptr, 0);
        } else if (argument == "--rate") {
            options.playback_rate = std::strtod(value("--rate").c_str(), nullptr);
        } else if (argument == "--inflight") {
            options.inflight_frames = static_cast<std::size_t>(
                std::strtoull(value("--inflight").c_str(), nullptr, 0));
        } else if (!argument.empty() && argument[0] == '-') {
            std::cerr << "unknown option: " << argument << '\n';
            std::exit(2);
        } else if (options.path.empty()) {
            options.path = argument;
        } else if (!legacy_maximum_seen) {
            options.maximum_access_units = static_cast<std::size_t>(
                std::strtoull(argument.c_str(), nullptr, 0));
            legacy_maximum_seen = true;
        } else {
            std::cerr << "unexpected argument: " << argument << '\n';
            std::exit(2);
        }
    }
    if (options.path.empty() || options.maximum_access_units == 0 ||
        options.inflight_frames == 0 ||
        options.playback_rate < 0.0) {
        std::cerr << "usage: tlvdemux-videotoolbox-probe FILE.mmts [MAX_AU] "
                     "[--max-au N] [--offset BYTES] [--rate X] "
                     "[--inflight N] [--skip-leading-rasl] "
                     "[--prepend-parameter-sets-on-irap] [--mse] "
                     "[--timeline-only] [--audio-packet-id ID] [--allow-software] "
                     "[--random-seeks N] [--seed N]\n";
        std::exit(2);
    }
    if (options.timeline_only && !options.mse_pipeline) {
        std::cerr << "--timeline-only requires --mse\n";
        std::exit(2);
    }
    return options;
}

} // namespace

bool run_probe(const Options& options, const std::uint64_t offset,
               const std::size_t case_index) {
    std::ifstream input(options.path, std::ios::binary);
    if (!input) {
        std::cerr << "cannot open " << options.path << '\n';
        return false;
    }

    Probe probe(options.maximum_access_units, options.skip_leading_rasl,
                options.playback_rate, options.inflight_frames,
                options.prepend_parameter_sets_on_irap, options.mse_pipeline,
                options.timeline_only, options.audio_packet_id,
                options.require_hardware);
    auto limits = tlvdemux::Limits{};
    limits.collect_application_resources = false;
    tlvdemux::Demuxer demuxer(probe, limits);
    if (offset != 0) {
        demuxer.reposition(tlvdemux::RepositionOptions{offset, false});
        input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!input) {
            std::cerr << "cannot seek to " << offset << '\n';
            return false;
        }
    }
    std::cerr << "case=" << case_index << " offset=" << offset << '\n';
    std::array<std::uint8_t, 1024 * 1024> chunk{};
    while (!probe.done() && input) {
        input.read(reinterpret_cast<char*>(chunk.data()),
                   static_cast<std::streamsize>(chunk.size()));
        const auto count = input.gcount();
        if (count > 0) demuxer.push(chunk.data(), static_cast<std::size_t>(count));
    }
    if (!probe.done()) demuxer.flush();
    probe.finish();
    std::cerr << "decoded=" << probe.decoded_count()
              << " final_status=" << probe.callback_status()
              << " timeline_gaps=" << probe.timeline_gap_count()
              << " largest_gap_us=" << probe.largest_timeline_gap_us()
              << " audio_samples=" << probe.audio_sample_count()
              << " audio_timeline_gaps=" << probe.audio_timeline_gap_count()
              << " largest_audio_gap=" << probe.largest_audio_timeline_gap()
              << " pps_samples=" << probe.mse_pps_sample_count()
              << " pps_variants=" << probe.mse_pps_variant_count() << '\n';
    return probe.ok();
}

int main(int argc, char** argv) {
    const auto options = parse_options(argc, argv);
    std::ifstream size_input(options.path, std::ios::binary | std::ios::ate);
    if (!size_input) {
        std::cerr << "cannot open " << options.path << '\n';
        return 2;
    }
    const auto end = size_input.tellg();
    if (end <= 0) {
        std::cerr << "empty input " << options.path << '\n';
        return 2;
    }
    const auto file_size = static_cast<std::uint64_t>(end);
    std::vector<std::uint64_t> offsets{options.offset};
    std::mt19937_64 random(options.seed);
    // Keep at least 4 MiB after a landing point so a short tail does not turn
    // into a false decoder failure merely because it contains no following RAP.
    const auto random_limit = file_size > 4U * 1024U * 1024U
        ? file_size - 4U * 1024U * 1024U : file_size - 1;
    for (std::size_t index = 0; index < options.random_seeks; ++index) {
        offsets.push_back(std::uniform_int_distribution<std::uint64_t>(0, random_limit)(random));
    }
    bool passed = true;
    for (std::size_t index = 0; index < offsets.size(); ++index) {
        if (!run_probe(options, offsets[index], index)) passed = false;
    }
    std::cerr << "cocktail cases=" << offsets.size() << " result="
              << (passed ? "PASS" : "FAIL") << '\n';
    return passed ? 0 : 1;
}
