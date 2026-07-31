#include "mse_remuxer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <emscripten/bind.h>

namespace {

using Bytes = std::vector<std::uint8_t>;
using emscripten::val;

void append(Bytes& out, const Bytes& value) { out.insert(out.end(), value.begin(), value.end()); }
void append(Bytes& out, std::initializer_list<std::uint8_t> value) {
    out.insert(out.end(), value.begin(), value.end());
}
Bytes u16(std::uint32_t value) { return {std::uint8_t(value >> 8), std::uint8_t(value)}; }
Bytes u24(std::uint32_t value) {
    return {std::uint8_t(value >> 16), std::uint8_t(value >> 8), std::uint8_t(value)};
}
Bytes u32(std::uint64_t value) {
    return {std::uint8_t(value >> 24), std::uint8_t(value >> 16),
            std::uint8_t(value >> 8), std::uint8_t(value)};
}
Bytes u64(std::uint64_t value) {
    auto out = u32(value >> 32); append(out, u32(value)); return out;
}
Bytes ascii(const std::string& value) { return Bytes(value.begin(), value.end()); }
Bytes zeros(std::size_t count) { return Bytes(count, 0); }

template <typename... Parts>
Bytes join(const Parts&... parts) {
    Bytes out;
    (append(out, parts), ...);
    return out;
}

template <typename... Parts>
Bytes box(const char* type, const Parts&... parts) {
    auto payload = join(parts...);
    return join(u32(payload.size() + 8), ascii(type), payload);
}

template <typename... Parts>
Bytes full_box(const char* type, std::uint8_t version, std::uint32_t flags,
               const Parts&... parts) {
    return box(type, Bytes{version}, u24(flags), parts...);
}

Bytes fixed16(double value) { return u32(std::uint64_t(std::llround(value * 65536.0))); }
Bytes unity_matrix() {
    return join(fixed16(1), u32(0), u32(0), u32(0), fixed16(1), u32(0),
                u32(0), u32(0), u32(0x40000000));
}
Bytes ftyp() { return box("ftyp", ascii("iso6"), u32(1), ascii("iso6"), ascii("mp41"), ascii("dash")); }

struct Mp4Track {
    std::uint32_t id = 1;
    bool video = false;
    std::uint32_t timescale = 1000000;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;
    std::string codec;
    Bytes config;
};

Bytes mvhd(std::uint32_t timescale) {
    return full_box("mvhd", 0, 0, u32(0), u32(0), u32(timescale), u32(0),
                    fixed16(1), u16(0x100), zeros(10), unity_matrix(), zeros(24), u32(2));
}
Bytes tkhd(const Mp4Track& track) {
    return full_box("tkhd", 0, 7, u32(0), u32(0), u32(track.id), u32(0), u32(0),
                    zeros(8), u16(0), u16(0), u16(track.video ? 0 : 0x100), u16(0),
                    unity_matrix(), fixed16(track.width), fixed16(track.height));
}
Bytes mdhd(const Mp4Track& track) {
    return full_box("mdhd", 0, 0, u32(0), u32(0), u32(track.timescale), u32(0),
                    u16(0x55c4), u16(0));
}
Bytes hdlr(bool video) {
    auto name = ascii(video ? "tlvdemux video" : "tlvdemux audio"); name.push_back(0);
    return full_box("hdlr", 0, 0, u32(0), ascii(video ? "vide" : "soun"), zeros(12), name);
}
Bytes dinf() {
    return box("dinf", box("dref", Bytes{0, 0, 0, 0}, u32(1), full_box("url ", 0, 1)));
}
Bytes video_entry(const Mp4Track& track) {
    Bytes compressor(32, 0); compressor[0] = 8;
    const auto label = ascii("tlvdemux");
    std::copy(label.begin(), label.end(), compressor.begin() + 1);
    auto header = join(zeros(6), u16(1), zeros(16), u16(track.width), u16(track.height),
                       fixed16(72), fixed16(72), u32(0), u16(1), compressor, u16(24), u16(0xffff));
    return box("hvc1", header, box("hvcC", track.config));
}
Bytes descriptor(std::uint8_t tag, const Bytes& payload) {
    if (payload.size() >= 128) throw std::runtime_error("MP4 descriptor is too large");
    return join(Bytes{tag, std::uint8_t(payload.size())}, payload);
}
Bytes audio_entry(const Mp4Track& track) {
    auto decoder_specific = descriptor(0x05, track.config);
    auto decoder_config = descriptor(0x04, join(Bytes{0x40, 0x15}, u24(0), u32(0), u32(0), decoder_specific));
    auto es = descriptor(0x03, join(u16(track.id), Bytes{0}, decoder_config,
                                   descriptor(0x06, Bytes{2})));
    auto header = join(zeros(6), u16(1), zeros(8), u16(track.channels), u16(16),
                       u16(0), u16(0), u32(std::uint64_t(track.sample_rate) << 16));
    return box("mp4a", header, full_box("esds", 0, 0, es));
}
Bytes stbl(const Mp4Track& track) {
    auto entry = track.video ? video_entry(track) : audio_entry(track);
    return box("stbl", full_box("stsd", 0, 0, u32(1), entry),
               full_box("stts", 0, 0, u32(0)), full_box("stsc", 0, 0, u32(0)),
               full_box("stsz", 0, 0, u32(0), u32(0)), full_box("stco", 0, 0, u32(0)));
}
Bytes init_segment(const Mp4Track& track) {
    auto media_header = track.video
        ? full_box("vmhd", 0, 1, u16(0), u16(0), u16(0), u16(0))
        : full_box("smhd", 0, 0, u16(0), u16(0));
    auto minf = box("minf", media_header, dinf(), stbl(track));
    auto trak = box("trak", tkhd(track), box("mdia", mdhd(track), hdlr(track.video), minf));
    auto trex = full_box("trex", 0, 0, u32(track.id), u32(1), u32(0), u32(0), u32(0));
    return join(ftyp(), box("moov", mvhd(track.timescale), trak, box("mvex", trex)));
}

struct Sample {
    Bytes data;
    std::int64_t dts = 0;
    std::int64_t pts = 0;
    std::uint32_t duration = 0;
    bool keyframe = false;
};

Bytes media_segment(const Mp4Track& track, const std::vector<Sample>& samples,
                    std::uint32_t sequence) {
    Bytes payload, entries;
    for (const auto& sample : samples) {
        append(payload, sample.data);
        append(entries, u32(sample.duration)); append(entries, u32(sample.data.size()));
        append(entries, u32(sample.keyframe ? 0x02000000 : 0x01010000));
        append(entries, u32(std::uint32_t(sample.pts - sample.dts)));
    }
    auto tfhd = full_box("tfhd", 0, 0x020000, u32(track.id));
    auto tfdt = full_box("tfdt", 1, 0, u64(std::uint64_t(std::max<std::int64_t>(0, samples.front().dts))));
    const auto trun_payload_size = 12 + samples.size() * 16;
    const auto data_offset = 8 + 16 + 8 + tfhd.size() + tfdt.size() + 8 + trun_payload_size + 8;
    auto trun = full_box("trun", 1, 0x000f01, u32(samples.size()), u32(data_offset), entries);
    auto moof = box("moof", full_box("mfhd", 0, 0, u32(sequence)), box("traf", tfhd, tfdt, trun));
    return join(moof, box("mdat", payload));
}

class BitReader {
public:
    explicit BitReader(const Bytes& data) : data_(data) {}
    std::uint32_t bits(unsigned count) {
        if (offset_ + count > data_.size() * 8) throw std::runtime_error("truncated bitstream");
        std::uint32_t value = 0;
        for (unsigned i = 0; i < count; ++i) {
            value = value * 2 + ((data_[offset_ >> 3] >> (7 - (offset_ & 7))) & 1);
            ++offset_;
        }
        return value;
    }
    bool boolean() { return bits(1) != 0; }
    std::uint32_t ue() {
        unsigned zeros_count = 0;
        while (!boolean()) { if (++zeros_count > 31) throw std::runtime_error("invalid Exp-Golomb"); }
        return ((std::uint32_t{1} << zeros_count) - 1) + (zeros_count ? bits(zeros_count) : 0);
    }
    Bytes bytes(std::size_t length) {
        Bytes out(length); for (auto& value : out) value = std::uint8_t(bits(8)); return out;
    }
    std::size_t offset() const noexcept { return offset_; }
private:
    const Bytes& data_; std::size_t offset_ = 0;
};

Bytes rbsp(const Bytes& nalu) {
    Bytes out;
    for (std::size_t i = 0; i < nalu.size(); ++i) {
        if (i >= 2 && nalu[i] == 3 && nalu[i - 1] == 0 && nalu[i - 2] == 0) continue;
        out.push_back(nalu[i]);
    }
    return out;
}
std::uint8_t reverse_byte(std::uint8_t value) {
    std::uint8_t out = 0; for (unsigned i = 0; i < 8; ++i) out |= ((value >> (7 - i)) & 1) << i; return out;
}
struct SpsInfo {
    std::uint32_t width = 0, height = 0, compatibility = 0;
    std::uint8_t profile_space = 0, tier = 0, profile = 0, level = 0, chroma = 0;
    std::uint8_t bit_luma = 0, bit_chroma = 0, layers = 0, nested = 0;
    Bytes compatibility_bytes, constraints;
    std::string codec;
};
SpsInfo parse_sps(const Bytes& nalu) {
    const auto data = rbsp(nalu); BitReader r(data); SpsInfo out;
    r.bits(16); r.bits(4); const auto max_layers = r.bits(3); out.nested = r.boolean();
    out.profile_space = std::uint8_t(r.bits(2)); out.tier = std::uint8_t(r.bits(1));
    out.profile = std::uint8_t(r.bits(5));
    for (int i = 0; i < 4; ++i) out.compatibility_bytes.push_back(std::uint8_t(r.bits(8)));
    for (int i = 0; i < 6; ++i) out.constraints.push_back(std::uint8_t(r.bits(8)));
    out.level = std::uint8_t(r.bits(8));
    std::vector<bool> sub_profile(max_layers), sub_level(max_layers);
    for (std::uint32_t i = 0; i < max_layers; ++i) { sub_profile[i] = r.boolean(); sub_level[i] = r.boolean(); }
    if (max_layers > 0) r.bits((8 - max_layers) * 2);
    for (std::uint32_t i = 0; i < max_layers; ++i) { if (sub_profile[i]) r.bits(88); if (sub_level[i]) r.bits(8); }
    r.ue(); out.chroma = std::uint8_t(r.ue()); if (out.chroma == 3) r.bits(1);
    const auto coded_width = r.ue(), coded_height = r.ue();
    std::uint32_t left = 0, right = 0, top = 0, bottom = 0;
    if (r.boolean()) { left = r.ue(); right = r.ue(); top = r.ue(); bottom = r.ue(); }
    out.bit_luma = std::uint8_t(r.ue()); out.bit_chroma = std::uint8_t(r.ue());
    const auto sub_width = out.chroma == 1 || out.chroma == 2 ? 2U : 1U;
    const auto sub_height = out.chroma == 1 ? 2U : 1U;
    out.width = coded_width - sub_width * (left + right);
    out.height = coded_height - sub_height * (top + bottom);
    for (std::size_t i = 0; i < 4; ++i) out.compatibility |= std::uint32_t(reverse_byte(out.compatibility_bytes[i])) << (i * 8);
    const char prefixes[] = {'\0', 'A', 'B', 'C'};
    out.codec = "hvc1.";
    if (prefixes[out.profile_space]) out.codec += prefixes[out.profile_space];
    out.codec += std::to_string(out.profile) + ".";
    char buffer[32]; std::snprintf(buffer, sizeof(buffer), "%X.%c%u", out.compatibility, out.tier ? 'H' : 'L', out.level);
    out.codec += buffer;
    int last = int(out.constraints.size()) - 1; while (last >= 0 && out.constraints[std::size_t(last)] == 0) --last;
    for (int i = 0; i <= last; ++i) { std::snprintf(buffer, sizeof(buffer), ".%02X", out.constraints[std::size_t(i)]); out.codec += buffer; }
    out.layers = std::uint8_t(max_layers + 1); return out;
}
Bytes make_hvcc(const Bytes& vps, const Bytes& sps, const Bytes& pps, const SpsInfo& d) {
    Bytes header(23, 0); header[0] = 1; header[1] = std::uint8_t((d.profile_space << 6) | (d.tier << 5) | d.profile);
    std::copy(d.compatibility_bytes.begin(), d.compatibility_bytes.end(), header.begin() + 2);
    std::copy(d.constraints.begin(), d.constraints.end(), header.begin() + 6); header[12] = d.level;
    header[13] = 0xf0; header[15] = 0xfc; header[16] = std::uint8_t(0xfc | d.chroma);
    header[17] = std::uint8_t(0xf8 | d.bit_luma); header[18] = std::uint8_t(0xf8 | d.bit_chroma);
    header[21] = std::uint8_t((d.layers << 3) | (d.nested ? 4 : 0) | 3); header[22] = 3;
    const auto array = [](std::uint8_t type, const Bytes& nalu) {
        return join(Bytes{std::uint8_t(0x80 | type), 0, 1}, u16(nalu.size()), nalu);
    };
    return join(header, array(32, vps), array(33, sps), array(34, pps));
}
struct Nalu { int type = -1; Bytes data; };
std::vector<Nalu> annex_b(const Bytes& data) {
    struct Start { std::size_t data, code; }; std::vector<Start> starts;
    for (std::size_t i = 0; i + 3 < data.size(); ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) { starts.push_back({i + 3, i}); i += 2; }
        else if (i + 4 < data.size() && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) { starts.push_back({i + 4, i}); i += 3; }
    }
    std::vector<Nalu> out;
    for (std::size_t i = 0; i < starts.size(); ++i) {
        const auto end = i + 1 < starts.size() ? starts[i + 1].code : data.size();
        if (end < starts[i].data + 2) continue;
        Bytes nalu(data.begin() + std::ptrdiff_t(starts[i].data), data.begin() + std::ptrdiff_t(end));
        out.push_back({(nalu[0] >> 1) & 0x3f, std::move(nalu)});
    }
    return out;
}
std::int64_t scaled(std::int64_t value, std::uint32_t from, std::uint32_t to) {
    return std::int64_t(std::llround(double(value) * double(to) / double(from)));
}
val copy_bytes(const Bytes& bytes) {
    auto out = val::global("Uint8Array").new_(bytes.size());
    if (!bytes.empty()) out.call<void>("set", val(emscripten::typed_memory_view(bytes.size(), bytes.data())));
    return out;
}

class Output {
public:
    explicit Output(val callbacks) : callbacks_(std::move(callbacks)) {}
    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }
    void init(const std::string& type, const Mp4Track& track) {
        if (!enabled_ || !has("onMseInit")) return; auto event = val::object(); event.set("type", type);
        event.set("mime", std::string(track.video ? "video/mp4; codecs=\"" : "audio/mp4; codecs=\"") + track.codec + "\"");
        event.set("data", copy_bytes(init_segment(track))); event.set("width", track.width); event.set("height", track.height);
        event.set("sampleRate", track.sample_rate); event.set("channels", track.channels); emit("onMseInit", event);
    }
    void segment(const std::string& type, const Bytes& data) {
        if (!enabled_ || !has("onMseSegment")) return; auto event = val::object(); event.set("type", type);
        event.set("data", copy_bytes(data)); emit("onMseSegment", event);
    }
    void video_start(int nal_type, bool signalled) {
        if (!enabled_ || !has("onMseVideoStart")) return; auto event = val::object(); event.set("nalType", nal_type);
        event.set("signalledRandomAccess", signalled); emit("onMseVideoStart", event);
    }
    void leading_pictures_dropped(std::size_t count) {
        if (!enabled_ || count == 0 || !has("onMseLeadingPicturesDropped")) return;
        auto event = val::object(); event.set("count", count);
        emit("onMseLeadingPicturesDropped", event);
    }
private:
    bool has(const char* name) const { return callbacks_[name].typeOf().as<std::string>() == "function"; }
    void emit(const char* name, const val& event) { callbacks_[name].call<void>("call", callbacks_, event); }
    val callbacks_; bool enabled_ = true;
};

class BaseMuxer {
public:
    BaseMuxer(std::string type, Output& output) : type_(std::move(type)), output_(output) {}
    virtual ~BaseMuxer() = default;
    void reset_samples() { pending_.reset(); ready_.clear(); ready_duration_ = 0; last_duration_ = 0; }
    void activate() { reset_samples(); if (track_) output_.init(type_, *track_); }
    void flush() {
        if (pending_) { pending_->duration = last_duration_ ? last_duration_ : default_duration(); ready_.push_back(std::move(*pending_)); pending_.reset(); }
        emit();
    }
protected:
    virtual std::uint32_t default_duration() const = 0;
    void set_track(Mp4Track track) { if (track_) return; track_ = std::move(track); output_.init(type_, *track_); }
    void enqueue(Sample sample) {
        if (pending_) {
            const auto delta = sample.dts - pending_->dts;
            pending_->duration = delta > 0 ? std::uint32_t(delta) : (last_duration_ ? last_duration_ : default_duration());
            last_duration_ = pending_->duration; ready_duration_ += pending_->duration;
            ready_.push_back(std::move(*pending_));
        }
        pending_ = std::move(sample); if (track_ && ready_duration_ >= track_->timescale) emit();
    }
    void emit() {
        if (!track_ || ready_.empty()) return;
        output_.segment(type_, media_segment(*track_, ready_, sequence_++)); ready_.clear(); ready_duration_ = 0;
    }
    std::optional<Mp4Track> track_;
private:
    std::string type_; Output& output_; std::optional<Sample> pending_; std::vector<Sample> ready_;
    std::uint64_t ready_duration_ = 0; std::uint32_t sequence_ = 1, last_duration_ = 0;
};

class HevcMuxer final : public BaseMuxer {
public:
    explicit HevcMuxer(Output& output) : BaseMuxer("video", output), output_(output) {}
    bool started() const noexcept { return started_; }
    void reset() {
        reset_samples();
        parameter_sets_.clear();
        track_.reset();
        started_ = false;
        waiting_for_trailing_picture_ = false;
        leading_rasl_dropped_ = 0;
        first_output_sample_ = true;
    }
    void push(const tlvdemux::AccessUnit& unit, bool output_enabled) {
        if (unit.discontinuity) {
            reset_samples();
            started_ = false;
            waiting_for_trailing_picture_ = false;
            leading_rasl_dropped_ = 0;
            first_output_sample_ = true;
        }
        const auto nalus = annex_b(unit.data);
        for (const auto& nalu : nalus) if (nalu.type >= 32 && nalu.type <= 34) parameter_sets_[nalu.type] = nalu.data;
        if (!track_ && parameter_sets_.count(32) && parameter_sets_.count(33) && parameter_sets_.count(34)) {
            const auto d = parse_sps(parameter_sets_[33]); Mp4Track track; track.video = true;
            track.width = d.width; track.height = d.height; track.codec = d.codec;
            track.config = make_hvcc(parameter_sets_[32], parameter_sets_[33], parameter_sets_[34], d); set_track(std::move(track));
        }
        if (!track_) return;
        bool has_vcl = false, only_rasl_vcl = true; int irap = -1;
        for (const auto& nalu : nalus) if (nalu.type >= 0 && nalu.type <= 31) {
            has_vcl = true;
            only_rasl_vcl = only_rasl_vcl && (nalu.type == 8 || nalu.type == 9);
            if (nalu.type >= 16 && nalu.type <= 21 && irap < 0) irap = nalu.type;
        }
        if (!has_vcl) return;
        if (!started_) {
            if (irap < 0) return;
            started_ = true;
            // A CRA is independently decodable, but RASL pictures following it in
            // decode order can still reference pictures preceding the CRA.  At a
            // fresh MSE decode sequence those references do not exist.  Apple's
            // hardware decoder reports kVTVideoDecoderReferenceMissingErr instead
            // of silently discarding these leading pictures.
            waiting_for_trailing_picture_ = irap == 21;
            output_.video_start(irap, unit.random_access);
        } else if (waiting_for_trailing_picture_) {
            if (only_rasl_vcl) {
                ++leading_rasl_dropped_;
                return;
            }
            waiting_for_trailing_picture_ = false;
            output_.leading_pictures_dropped(leading_rasl_dropped_);
        }
        if (!output_enabled) return;
        Bytes data;
        for (const auto& nalu : nalus) if (nalu.type != 32 && nalu.type != 33 && nalu.type != 34 && nalu.type != 35) {
            append(data, u32(nalu.data.size())); append(data, nalu.data);
        }
        if (data.empty()) return;
        // MSE only gets a random-access boundary at the beginning of this decode
        // sequence.  Later CRA pictures belong to a continuous open GOP: marking
        // each of them as a sync sample makes Chromium treat their following RASL
        // pictures as unsupported leading samples even though the decoder already
        // has the required reference history.
        const bool keyframe = first_output_sample_ && irap >= 0;
        enqueue({std::move(data), scaled(unit.dts.value, unit.dts.timescale, 1000000),
                 scaled(unit.pts.value, unit.pts.timescale, 1000000), 0, keyframe});
        first_output_sample_ = false;
    }
private:
    std::uint32_t default_duration() const override { return 33367; }
    Output& output_;
    std::map<int, Bytes> parameter_sets_;
    bool started_ = false;
    bool waiting_for_trailing_picture_ = false;
    std::size_t leading_rasl_dropped_ = 0;
    bool first_output_sample_ = true;
};

constexpr std::uint32_t sample_rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
struct AacFrame { Bytes data, asc; std::uint32_t object = 0, sample_rate = 0, channels = 0; };
class LatmParser {
public:
    AacFrame parse(const Bytes& data) {
        if (data.size() < 4 || data[0] != 0x56 || (data[1] & 0xe0) != 0xe0) throw std::runtime_error("invalid LOAS frame");
        const auto length = std::size_t((data[1] & 0x1f) << 8 | data[2]);
        if (length + 3 > data.size()) throw std::runtime_error("truncated LOAS frame");
        Bytes payload(data.begin() + 3, data.begin() + std::ptrdiff_t(3 + length)); BitReader r(payload);
        if (!r.boolean()) {
            const bool version = r.boolean(), version_a = version && r.boolean(); if (version_a) throw std::runtime_error("LATM version A unsupported");
            if (version) latm_value(r);
            if (!r.boolean() || r.bits(6) != 0 || r.bits(4) != 0 || r.bits(3) != 0) throw std::runtime_error("unsupported LATM layout");
            const auto asc_length = version ? latm_value(r) : 0; const auto asc_start = r.offset();
            auto object = r.bits(5); if (object == 31) object = 32 + r.bits(6);
            const auto rate_index = r.bits(4); const auto rate = rate_index == 15 ? r.bits(24) : (rate_index < 13 ? sample_rates[rate_index] : 0);
            const auto channels = r.bits(4); r.bits(1); if (r.boolean()) r.bits(14); r.bits(1);
            if (version && asc_length > r.offset() - asc_start) r.bits(unsigned(asc_length - (r.offset() - asc_start)));
            if (r.bits(3) != 0) throw std::runtime_error("unsupported LATM frame length"); r.bits(8);
            if (r.boolean()) { bool more; do { more = r.boolean(); r.bits(version ? latm_value(r) : 8); } while (more); }
            if (r.boolean()) r.bits(8);
            if (!rate || !channels || object >= 32 || rate_index >= 15) throw std::runtime_error("unsupported AAC config");
            config_ = AacFrame{{}, Bytes{std::uint8_t((object << 3) | (rate_index >> 1)),
                                        std::uint8_t(((rate_index & 1) << 7) | (channels << 3))}, object, rate, channels};
        }
        if (!config_) throw std::runtime_error("LATM config is missing");
        std::size_t payload_length = 0; std::uint32_t part;
        do { part = r.bits(8); payload_length += part; } while (part == 255);
        auto frame = *config_; frame.data = r.bytes(payload_length); return frame;
    }
private:
    static std::uint32_t latm_value(BitReader& r) { const auto count = r.bits(2); std::uint32_t value = 0; for (std::uint32_t i = 0; i <= count; ++i) value = value * 256 + r.bits(8); return value; }
    std::optional<AacFrame> config_;
};
class AacMuxer final : public BaseMuxer {
public:
    explicit AacMuxer(Output& output) : BaseMuxer("audio", output) {}
    void discontinuity() { reset_samples(); }
    void push(const tlvdemux::AccessUnit& unit, bool enabled) {
        if (unit.discontinuity) reset_samples(); const auto frame = parser_.parse(unit.data);
        if (!track_) { Mp4Track track; track.timescale = frame.sample_rate; track.sample_rate = frame.sample_rate;
            track.channels = frame.channels; track.codec = "mp4a.40." + std::to_string(frame.object); track.config = frame.asc; set_track(std::move(track)); }
        if (!enabled) return; const auto timestamp = scaled(unit.pts.value, unit.pts.timescale, track_->timescale);
        enqueue({frame.data, timestamp, timestamp, 0, true});
    }
private:
    std::uint32_t default_duration() const override { return track_ ? std::uint32_t(std::llround(1024.0 * track_->timescale / track_->sample_rate)) : 21333; }
    LatmParser parser_;
};

} // namespace

class WasmMseRemuxer::Impl {
public:
    explicit Impl(val callbacks) : output(std::move(callbacks)), video(output) {}
    void select(tlvdemux::TrackKind kind, std::optional<std::uint64_t> id) {
        if (kind == tlvdemux::TrackKind::Video) { video_id = id; return; }
        if (kind != tlvdemux::TrackKind::Audio) return;
        if (audio_id == id && active_audio != nullptr) return;
        audio_id = id;
        if (!id) { active_audio = nullptr; return; }
        auto [it, inserted] = audio.try_emplace(*id, output); active_audio = &it->second;
        if (!inserted) active_audio->activate();
    }
    void push(const tlvdemux::AccessUnit& unit) {
        if (video_id && unit.track_id == *video_id) { if (unit.discontinuity && active_audio) active_audio->discontinuity(); video.push(unit, enabled); }
        else if (audio_id && unit.track_id == *audio_id && active_audio) active_audio->push(unit, enabled && video.started());
    }
    void flush() { video.flush(); if (active_audio) active_audio->flush(); }
    void reset() { video.reset(); audio.clear(); active_audio = nullptr; }
    void reposition() {
        video.reset();
        for (auto& entry : audio) entry.second.discontinuity();
    }
    Output output; HevcMuxer video; std::map<std::uint64_t, AacMuxer> audio; AacMuxer* active_audio = nullptr;
    std::optional<std::uint64_t> video_id, audio_id; bool enabled = true;
};

WasmMseRemuxer::WasmMseRemuxer(val callbacks) : impl_(std::make_unique<Impl>(std::move(callbacks))) {}
WasmMseRemuxer::~WasmMseRemuxer() = default;
void WasmMseRemuxer::selectTrack(tlvdemux::TrackKind kind, std::optional<std::uint64_t> id) { impl_->select(kind, id); }
void WasmMseRemuxer::setOutputEnabled(bool enabled) noexcept {
    impl_->enabled = enabled;
    impl_->output.set_enabled(enabled);
}
void WasmMseRemuxer::push(const tlvdemux::AccessUnit& unit) { impl_->push(unit); }
void WasmMseRemuxer::flush() { impl_->flush(); }
void WasmMseRemuxer::reset() { impl_->reset(); }
void WasmMseRemuxer::reposition() { impl_->reposition(); }
