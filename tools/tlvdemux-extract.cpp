#include <tlvdemux/demuxer.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

struct Extractor final : tlvdemux::Sink {
    explicit Extractor(std::filesystem::path root) : root(std::move(root)) {}

    std::filesystem::path root;
    std::size_t written = 0;

    void onService(const tlvdemux::ServiceInfo&) override {}
    void onTrack(const tlvdemux::TrackInfo&) override {}
    void onAccessUnit(tlvdemux::AccessUnit&&) override {}

    void onApplicationResource(tlvdemux::ApplicationResource&& resource) override {
        const auto relative = safe_relative(resource.path);
        const auto output = root / relative;
        std::filesystem::create_directories(output.parent_path());
        std::ofstream file(output, std::ios::binary);
        if (!file) throw std::runtime_error("cannot create output: " + output.string());
        file.write(reinterpret_cast<const char*>(resource.data.data()),
                   static_cast<std::streamsize>(resource.data.size()));
        if (!file) throw std::runtime_error("cannot write output: " + output.string());
        ++written;
    }

    void onError(const tlvdemux::Error& error) override {
        if (!error.recoverable) throw std::runtime_error(error.message);
        std::cerr << "recoverable error at " << error.input_offset << ": "
                  << error.message << '\n';
    }

private:
    static std::filesystem::path safe_relative(const std::string& value) {
        std::filesystem::path path(value);
        if (path.is_absolute()) throw std::runtime_error("broadcast path is absolute");
        std::filesystem::path result;
        for (const auto& part : path) {
            if (part == "..") throw std::runtime_error("broadcast path escapes output directory");
            if (part != "." && !part.empty()) result /= part;
        }
        if (result.empty()) throw std::runtime_error("broadcast path is empty");
        return result;
    }
};

void usage() {
    std::cerr << "usage: tlvdemux-extract OUTPUT-DIR INPUT\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3) {
            usage();
            return 2;
        }
        Extractor extractor(argv[1]);
        std::ifstream input(argv[2], std::ios::binary);
        if (!input) throw std::runtime_error("cannot open input");
        tlvdemux::Demuxer demuxer(extractor);
        std::array<std::uint8_t, 64 * 1024> buffer{};
        while (input) {
            input.read(reinterpret_cast<char*>(buffer.data()),
                       static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count > 0) demuxer.push(buffer.data(), static_cast<std::size_t>(count));
        }
        demuxer.flush();
        std::cerr << "extracted " << extractor.written << " files\n";
        return extractor.written == 0 ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "tlvdemux-extract: " << error.what() << '\n';
        return 2;
    }
}
