# libaribtlv

English | [日本語](README.ja.md)

`libaribtlv` is a C++20 library for incrementally decoding already-descrambled
ARIB MMT/TLV streams. It owns the transport and broadcast-domain core shared by
native receivers, command-line tools, and WebAssembly adapters:

- TLV resynchronization, compressed-IP contexts, and MMTP fragmentation;
- PA/M2/MPT, MH-AIT, EIT, SDT, TOT, EMT, and B60 data-transmission signalling;
- HEVC, AAC-LATM/LOAS, and ARIB STD-B62 TTML access units;
- ARIB-HTML5 application-resource assembly and an in-memory resource store;
- recording indexes, seek points, duration tracking, and range-based probing.

Player session policy, MSE/fMP4 remuxing, JavaScript bindings, command-line
programs, and browser demos live in the sibling `tlvdemux` project.

## Build and test

```sh
nix-shell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Shared libraries are enabled by default and produce `libaribtlv.so.0` or
`libaribtlv.0.dylib`. Set `-DBUILD_SHARED_LIBS=OFF` for `libaribtlv.a`.
Zlib is the only non-standard runtime dependency. Emscripten builds use its
built-in zlib port.

Install the library, headers, and CMake package with:

```sh
cmake --install build --prefix /desired/prefix
```

Consumers use `find_package(aribtlv CONFIG REQUIRED)` and link
`aribtlv::aribtlv`.

## Usage

```cpp
#include <aribtlv/demuxer.hpp>

class Receiver final : public aribtlv::Sink {
public:
    void onService(const aribtlv::ServiceInfo&) override;
    void onTrack(const aribtlv::TrackInfo&) override;
    void onAccessUnit(aribtlv::AccessUnit&&) override;
    void onError(const aribtlv::Error&) override;
};

Receiver receiver;
aribtlv::Demuxer demuxer(receiver);
demuxer.push(data, size);
demuxer.flush();
```

`push()` consumes arbitrary chunk boundaries synchronously and does not retain
the input pointer. Use `reposition()` for a seek in the same source, `reset()`
when replacing the source, and `flush()` at a true input boundary or EOF.

Application resources can be received directly through `Sink`, or assembled
explicitly with `ApplicationResourceAssembler`. `Limits` bounds parser and
resource memory, and can disable built-in application-resource collection when
an adapter needs to drain that work asynchronously.

The public ABI is C++20. Dynamically linked consumers must use a compatible
compiler and C++ standard library.

## License

MIT
