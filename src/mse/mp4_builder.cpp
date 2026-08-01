#include "mp4_builder.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace tlvdemux::detail::mse {
namespace {

Bytes u16(const std::uint32_t value) {
    return {static_cast<std::uint8_t>(value >> 8U),
            static_cast<std::uint8_t>(value)};
}

Bytes u24(const std::uint32_t value) {
    return {static_cast<std::uint8_t>(value >> 16U),
            static_cast<std::uint8_t>(value >> 8U),
            static_cast<std::uint8_t>(value)};
}

Bytes u32(const std::uint64_t value) {
    return {static_cast<std::uint8_t>(value >> 24U),
            static_cast<std::uint8_t>(value >> 16U),
            static_cast<std::uint8_t>(value >> 8U),
            static_cast<std::uint8_t>(value)};
}

Bytes u64(const std::uint64_t value) {
    auto output = u32(value >> 32U);
    append(output, u32(value));
    return output;
}

Bytes ascii(const std::string& value) { return Bytes(value.begin(), value.end()); }
Bytes zeros(const std::size_t count) { return Bytes(count, 0); }

template <typename... Parts>
Bytes join(const Parts&... parts) {
    const std::size_t size = (parts.size() + ... + 0U);
    Bytes output;
    output.reserve(size);
    (append(output, parts), ...);
    return output;
}

template <typename... Parts>
Bytes box(const char* type, const Parts&... parts) {
    auto payload = join(parts...);
    return join(u32(payload.size() + 8U), ascii(type), payload);
}

template <typename... Parts>
Bytes full_box(const char* type, const std::uint8_t version,
               const std::uint32_t flags, const Parts&... parts) {
    return box(type, Bytes{version}, u24(flags), parts...);
}

Bytes fixed16(const double value) {
    return u32(static_cast<std::uint64_t>(std::llround(value * 65536.0)));
}

Bytes unity_matrix() {
    return join(fixed16(1), u32(0), u32(0), u32(0), fixed16(1), u32(0),
                u32(0), u32(0), u32(0x40000000));
}

Bytes ftyp() {
    return box("ftyp", ascii("iso6"), u32(1), ascii("iso6"), ascii("mp41"),
               ascii("dash"));
}

Bytes mvhd(const std::uint32_t timescale) {
    return full_box("mvhd", 0, 0, u32(0), u32(0), u32(timescale), u32(0),
                    fixed16(1), u16(0x100), zeros(10), unity_matrix(), zeros(24),
                    u32(2));
}

Bytes tkhd(const Mp4Track& track) {
    return full_box("tkhd", 0, 7, u32(0), u32(0), u32(track.id), u32(0), u32(0),
                    zeros(8), u16(0), u16(0), u16(track.video ? 0 : 0x100),
                    u16(0), unity_matrix(), fixed16(track.width),
                    fixed16(track.height));
}

Bytes mdhd(const Mp4Track& track) {
    return full_box("mdhd", 0, 0, u32(0), u32(0), u32(track.timescale), u32(0),
                    u16(0x55c4), u16(0));
}

Bytes hdlr(const bool video) {
    auto name = ascii(video ? "tlvdemux video" : "tlvdemux audio");
    name.push_back(0);
    return full_box("hdlr", 0, 0, u32(0), ascii(video ? "vide" : "soun"),
                    zeros(12), name);
}

Bytes dinf() {
    return box("dinf", box("dref", Bytes{0, 0, 0, 0}, u32(1),
                            full_box("url ", 0, 1)));
}

Bytes video_entry(const Mp4Track& track) {
    Bytes compressor(32, 0);
    compressor[0] = 8;
    const auto label = ascii("tlvdemux");
    std::copy(label.begin(), label.end(), compressor.begin() + 1);
    auto header = join(zeros(6), u16(1), zeros(16), u16(track.width),
                       u16(track.height), fixed16(72), fixed16(72), u32(0), u16(1),
                       compressor, u16(24), u16(0xffff));
    const auto* sample_entry = track.codec.rfind("hvc1.", 0) == 0 ? "hvc1" : "hev1";
    return box(sample_entry, header, box("hvcC", track.config));
}

Bytes descriptor(const std::uint8_t tag, const Bytes& payload) {
    if (payload.size() >= 128) throw std::runtime_error("MP4 descriptor is too large");
    return join(Bytes{tag, static_cast<std::uint8_t>(payload.size())}, payload);
}

Bytes audio_entry(const Mp4Track& track) {
    auto decoder_specific = descriptor(0x05, track.config);
    auto decoder_config = descriptor(
        0x04, join(Bytes{0x40, 0x15}, u24(0), u32(0), u32(0), decoder_specific));
    auto es = descriptor(0x03, join(u16(track.id), Bytes{0}, decoder_config,
                                   descriptor(0x06, Bytes{2})));
    auto header = join(zeros(6), u16(1), zeros(8), u16(track.channels), u16(16),
                       u16(0), u16(0),
                       u32(static_cast<std::uint64_t>(track.sample_rate) << 16U));
    return box("mp4a", header, full_box("esds", 0, 0, es));
}

Bytes stbl(const Mp4Track& track) {
    auto entry = track.video ? video_entry(track) : audio_entry(track);
    return box("stbl", full_box("stsd", 0, 0, u32(1), entry),
               full_box("stts", 0, 0, u32(0)), full_box("stsc", 0, 0, u32(0)),
               full_box("stsz", 0, 0, u32(0), u32(0)),
               full_box("stco", 0, 0, u32(0)));
}

} // namespace

Bytes init_segment(const Mp4Track& track) {
    auto media_header = track.video
        ? full_box("vmhd", 0, 1, u16(0), u16(0), u16(0), u16(0))
        : full_box("smhd", 0, 0, u16(0), u16(0));
    auto minf = box("minf", media_header, dinf(), stbl(track));
    auto trak = box("trak", tkhd(track),
                    box("mdia", mdhd(track), hdlr(track.video), minf));
    auto trex = full_box("trex", 0, 0, u32(track.id), u32(1), u32(0), u32(0),
                         u32(0));
    return join(ftyp(), box("moov", mvhd(track.timescale), trak, box("mvex", trex)));
}

Bytes media_segment(const Mp4Track& track, const std::vector<Sample>& samples,
                    const std::uint32_t sequence) {
    if (samples.empty()) return {};
    if (samples.front().dts < 0) {
        throw std::runtime_error("negative MSE decode timestamp");
    }

    std::size_t media_size = 0;
    Bytes entries;
    entries.reserve(samples.size() * 16U);
    for (const auto& sample : samples) {
        if (sample.data.size() > std::numeric_limits<std::uint32_t>::max() ||
            media_size > std::numeric_limits<std::uint32_t>::max() - sample.data.size()) {
            throw std::runtime_error("MSE media segment is too large");
        }
        media_size += sample.data.size();
        append(entries, u32(sample.duration));
        append(entries, u32(sample.data.size()));
        append(entries, u32(sample.keyframe ? 0x02000000 : 0x01010000));
        append(entries, u32(static_cast<std::uint32_t>(sample.pts - sample.dts)));
    }

    auto tfhd = full_box("tfhd", 0, 0x020000, u32(track.id));
    auto tfdt = full_box("tfdt", 1, 0, u64(static_cast<std::uint64_t>(samples.front().dts)));
    const auto trun_payload_size = 12U + samples.size() * 16U;
    const auto data_offset = 8U + 16U + 8U + tfhd.size() + tfdt.size() + 8U +
        trun_payload_size + 8U;
    auto trun = full_box("trun", 1, 0x000f01, u32(samples.size()),
                         u32(data_offset), entries);
    auto moof = box("moof", full_box("mfhd", 0, 0, u32(sequence)),
                    box("traf", tfhd, tfdt, trun));

    Bytes output;
    output.reserve(moof.size() + 8U + media_size);
    append(output, moof);
    append(output, u32(media_size + 8U));
    append(output, ascii("mdat"));
    for (const auto& sample : samples) append(output, sample.data);
    return output;
}

} // namespace tlvdemux::detail::mse
