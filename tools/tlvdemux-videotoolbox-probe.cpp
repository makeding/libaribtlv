#include <tlvdemux/demuxer.hpp>

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
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

struct NalUnit {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::uint8_t type = 0;
    std::uint8_t layer_id = 0;
};

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

class Probe final : public tlvdemux::Sink {
public:
    Probe(const std::size_t maximum_access_units, const bool skip_leading_rasl,
          const double playback_rate, const std::size_t inflight_frames,
          const bool prepend_parameter_sets_on_irap)
        : maximum_access_units_(maximum_access_units),
          skip_leading_rasl_(skip_leading_rasl),
          playback_rate_(playback_rate),
          inflight_frames_(inflight_frames),
          prepend_parameter_sets_on_irap_(prepend_parameter_sets_on_irap) {}

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
            std::cerr << "video track packet_id=0x" << std::hex << track.packet_id << std::dec
                      << " timescale=" << track.timescale << '\n';
        }
    }

    void onAccessUnit(tlvdemux::AccessUnit&& unit) override {
        if (done_ || !video_track_.has_value() || unit.track_id != *video_track_ ||
            unit.codec != tlvdemux::Codec::Hevc) return;

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
        if (!error.recoverable) {
            std::cerr << "demux error @" << error.input_offset << ": " << error.message << '\n';
            done_ = true;
        }
    }

    bool done() const { return done_; }
    OSStatus callback_status() const { return callback_status_.load(); }
    std::size_t decoded_count() const { return decoded_count_.load(); }

    void finish() {
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
        const auto value = seconds(dts);
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

        const void* decoder_keys[] = {
            kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder,
        };
        const void* decoder_values[] = {kCFBooleanTrue};
        const auto decoder_specification = CFDictionaryCreate(
            kCFAllocatorDefault, decoder_keys, decoder_values, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        VTDecompressionOutputCallbackRecord callback{&Probe::output_callback, this};
        status = VTDecompressionSessionCreate(kCFAllocatorDefault, format_,
                                               decoder_specification, nullptr,
                                               &callback, &session_);
        CFRelease(decoder_specification);
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

    OSStatus decode(const tlvdemux::AccessUnit& unit, const std::vector<NalUnit>& nals) {
        std::vector<std::uint8_t> sample;
        const auto append_nal = [&sample](const std::uint8_t* data,
                                          const std::size_t nal_size) -> bool {
            if (nal_size > std::numeric_limits<std::uint32_t>::max()) return false;
            const auto size = static_cast<std::uint32_t>(nal_size);
            sample.push_back(static_cast<std::uint8_t>(size >> 24U));
            sample.push_back(static_cast<std::uint8_t>(size >> 16U));
            sample.push_back(static_cast<std::uint8_t>(size >> 8U));
            sample.push_back(static_cast<std::uint8_t>(size));
            sample.insert(sample.end(), data, data + nal_size);
            return true;
        };
        const bool has_irap = std::any_of(nals.begin(), nals.end(), [](const auto& nal) {
            return nal.type >= 16 && nal.type <= 23;
        });
        std::size_t first_original = 0;
        if (prepend_parameter_sets_on_irap_ && has_irap) {
            // Chromium inserts hvcC immediately after an initial AUD, then keeps
            // any in-band parameter sets already present in an hev1 sample.
            if (!nals.empty() && nals.front().type == 35) {
                if (!append_nal(nals.front().data, nals.front().size)) return paramErr;
                first_original = 1;
            }
            for (const auto& parameter_set : decoder_parameter_sets_) {
                if (!append_nal(parameter_set.data(), parameter_set.size())) return paramErr;
            }
        }
        for (std::size_t index = first_original; index < nals.size(); ++index) {
            const auto& nal = nals[index];
            if (nal.size > std::numeric_limits<std::uint32_t>::max()) return paramErr;
            if (!append_nal(nal.data, nal.size)) return paramErr;
        }

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

        const CMSampleTimingInfo timing{
            kCMTimeInvalid,
            CMTimeMake(unit.pts.value, static_cast<std::int32_t>(unit.pts.timescale)),
            CMTimeMake(unit.dts.value, static_cast<std::int32_t>(unit.dts.timescale)),
        };
        const auto sample_size = sample.size();
        CMSampleBufferRef buffer = nullptr;
        status = CMSampleBufferCreateReady(kCFAllocatorDefault, block, format_, 1, 1, &timing,
                                           1, &sample_size, &buffer);
        CFRelease(block);
        if (status != noErr) return status;

        if (!unit.random_access) {
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
                     "[--prepend-parameter-sets-on-irap]\n";
        std::exit(2);
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    const auto options = parse_options(argc, argv);
    std::ifstream input(options.path, std::ios::binary);
    if (!input) {
        std::cerr << "cannot open " << options.path << '\n';
        return 2;
    }

    Probe probe(options.maximum_access_units, options.skip_leading_rasl,
                options.playback_rate, options.inflight_frames,
                options.prepend_parameter_sets_on_irap);
    auto limits = tlvdemux::Limits{};
    limits.collect_application_resources = false;
    tlvdemux::Demuxer demuxer(probe, limits);
    if (options.offset != 0) {
        demuxer.reposition(tlvdemux::RepositionOptions{options.offset, false});
        input.seekg(static_cast<std::streamoff>(options.offset), std::ios::beg);
        if (!input) {
            std::cerr << "cannot seek to " << options.offset << '\n';
            return 2;
        }
    }
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
              << " final_status=" << probe.callback_status() << '\n';
    return probe.callback_status() == noErr ? 0 : 1;
}
