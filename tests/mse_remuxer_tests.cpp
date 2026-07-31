#include <tlvdemux/mse_remuxer.hpp>

#include <cstdlib>
#include <iostream>
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
    void onMseSegment(tlvdemux::MseMediaSegment&&) override {}

    std::vector<tlvdemux::MseTrackInit> inits;
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
    std::cout << "mse remuxer tests passed\n";
}
