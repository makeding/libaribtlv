# tlvdemux

`tlvdemux` is a C++20 incremental demultiplexer for
already-descrambled ARIB MMT/TLV streams. It is designed to emit player-ready
HEVC, AAC-LATM/LOAS and ARIB STD-B62 TTML access units without converting the
stream to MPEG-TS or exposing FFmpeg ABI types.

The current implementation provides the stable public callback API, bounded
incremental TLV resynchronization, compressed-IP context isolation, MMTP
fragment/aggregation handling, PA/M2/MPT track discovery, descriptor-driven
timelines, and HEVC/AAC-LATM/TTML access-unit output.

## Build and test

```sh
nix-shell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Zlib is the only non-standard runtime dependency. It is used to turn compressed
data-broadcast items into browser-ready virtual files; Emscripten links its zlib
port into the single-file WASM build.

Shared-library builds are enabled by default. Linux produces
`libtlvdemux.so.0` (with the versioned implementation file), while macOS
produces the corresponding `libtlvdemux.0.dylib`. Use
`-DBUILD_SHARED_LIBS=OFF` when a static `libtlvdemux.a` is preferred.
The exported interface is a C++20 ABI, so dynamically linked consumers should
use a compatible compiler and C++ standard library.

When embedding the project with `add_subdirectory()`, the diagnostic executable
can be disabled with `-DTLVDEMUX_BUILD_TOOLS=OFF`. Tests follow CMake's standard
`BUILD_TESTING` option.

On macOS, the VideoToolbox probe can exercise the complete browser-facing MSE
path without launching a browser. It feeds MMTS through the production
`MseRemuxer`, validates `tfdt`/`trun` continuity and HEVC sample flags, applies
Chromium-style `hvc1` conversion, and submits the result to hardware decoding.
The following also repeats the run at sixteen deterministic random byte
landings while pacing samples at 3x:

```sh
./build/tlvdemux-videotoolbox-probe demo/8k.mmts \
  --mse --rate 3 --inflight 4 --max-au 90 \
  --random-seeks 16 --seed 20260731
```

Install the library, public headers and CMake target export with:

```sh
cmake --install build --prefix /desired/prefix
```

The install includes the shared or static library, public headers, the
`tlvdemux::tlvdemux` CMake package target, the diagnostic tool when enabled,
and the MIT license.

## Library usage

Implement `tlvdemux::Sink`, keep it alive for the lifetime of the demuxer, and
feed arbitrary-sized chunks synchronously:

```cpp
#include <tlvdemux/demuxer.hpp>

class PlayerSink final : public tlvdemux::Sink {
public:
    void onService(const tlvdemux::ServiceInfo& service) override;
    void onTrack(const tlvdemux::TrackInfo& track) override;
    void onAccessUnit(tlvdemux::AccessUnit&& unit) override;
    void onError(const tlvdemux::Error& error) override;
};

PlayerSink sink;
tlvdemux::Demuxer demuxer(sink);
demuxer.push(data, size);
demuxer.flush();
```

`push()` does not retain the input pointer. Callback payloads own their data,
and malformed stream data is reported through `onError()` while parsing
continues where recovery is possible. Call `reset()` when replacing the input
stream; service and track selection policies are retained.

Audio tracks expose their MH audio component metadata through
`TrackInfo::audio`, including the signalled channel layout, component type,
main-component flag and sampling rate. Select tracks from this metadata rather
than assuming packet IDs remain fixed between programmes.

## Data-broadcast applications and virtual files

Application-resource collection is enabled by default. While media access units
are emitted, the demuxer also combines application signalling, data-directory
tables, asset-management tables and out-of-order data units into complete files.
`Sink` receives `onApplicationState`, `onApplicationResource`, and
`onApplicationResourcesReset` events. An application becomes `Ready` when its
AIT entry path is present; other referenced files may continue arriving from the
broadcast carousel.

Completed bytes are moved to the sink rather than retained indefinitely by the
demuxer. Native hosts can keep them in the thread-safe
`ApplicationResourceStore`, whose `get`, `list`, and `waitFor` methods are
intended for a loopback HTTP/WebView adapter:

```cpp
class ReceiverSink final : public tlvdemux::Sink {
public:
    tlvdemux::ApplicationResourceStore files;

    void onApplicationState(const tlvdemux::ApplicationState& state) override {
        files.onApplicationState(state);
    }
    void onApplicationResource(tlvdemux::ApplicationResource&& resource) override {
        files.onApplicationResource(std::move(resource));
    }
    void onApplicationResourcesReset() override {
        files.onApplicationResourcesReset();
    }
    // Implement the four required media/error callbacks as usual.
};
```

The store contains no socket or HTTP dependency. A native application may bind
a separate server to `127.0.0.1` and answer a request with
`files.waitFor(context_id, path, timeout)`. WASM callers normally keep the same
events in a JavaScript `Map` and expose them through a Service Worker instead.

Resource collection can be disabled with
`Limits::collect_application_resources`. `Limits` also bounds pending item
count/bytes, catalogue size, and decompressed file size so a malformed carousel
cannot grow memory without limit.

## Inspect a stream

```sh
./build/tlvdemux-inspect --list test.tlv
./build/tlvdemux-inspect --trace-au test.tlv
./build/tlvdemux-inspect --video video.hevc --audio audio.loas \
  --subtitle subtitle.ttml test.tlv
./build/tlvdemux-inspect --audio secondary.loas \
  --audio-packet-id 0xf311 test.tlv
```

Use Mirakurun's raw 4K path with `decode=0` when capturing validation input:

```sh
curl 'http://MIRAKURUN/api/services/SERVICE_ID/stream?decode=0' > test.tlv
```

When more than one track of a kind is present, the diagnostic dumper writes the
first discovered supported track of that kind. `--trace-au` still reports every
emitted track.

The library assumes any required B61 descrambling has already happened before
the bytes reach `Demuxer::push()`. In the validation setup, Mirakurun
`decode=0` preserves the MMT/TLV stream while the tuner/frontend path supplies
already-usable media payloads. B61 message-authentication metadata is parsed so
an appended authentication code is not exposed as part of the media payload;
cryptographic verification itself remains the caller's responsibility.

## WebAssembly

Install the prebuilt single-file WebAssembly package from npm:

```sh
npm install tlvdemux
```

The package works with CommonJS directly and with the usual default-import
interop in ESM-aware bundlers:

```js
import createTlvDemuxModule from "tlvdemux";

const module = await createTlvDemuxModule();
const demuxer = new module.TlvDemuxer({
  onTrack: track => console.log(track),
  onAccessUnitView: unit => consumeSynchronously(unit),
  onError: error => console.warn(error),
});
```

For MSE players, `mseMaxAudioChannels` can reject layouts above the browser's
chosen limit without rewriting the AAC configuration. For example, a value of
`6` keeps mono through 5.1 tracks and suppresses a 22.2-channel AAC init
segment. Use `track.audio.channels` in `onTrack` to select a compatible
alternative track; omitted or zero leaves the remuxer unlimited.

#### BS8K の 22.2ch 音声を除外する

BS8K の番組によっては、AAC の `channel_configuration=13` で表される
22.2ch（24 チャンネル）音声が送出されます。Chromium 系ブラウザーはこの構成を
MSE で受け付けず、音声の `appendBuffer()` が MediaError になることがあります。
ブラウザー再生では次のように上限を 6 チャンネルにすると、モノラルから 5.1ch
までは通し、22.2ch の MSE init segment は出力しません。

```js
let selectedAudio = false;
const demuxer = new module.TlvDemuxer({
  mseMaxAudioChannels: 6,
  onTrack(track) {
    const channels = track.audio?.channels ?? 0;
    if (!selectedAudio && track.kind === "audio" &&
        (channels === 0 || channels <= 6)) {
      selectedAudio = true;
      demuxer.selectTrack("audio", track.trackId);
    }
  },
  onMseInit: init => appendInitSegment(init),
  onMseSegment: segment => appendMediaSegment(segment),
});
```

この設定は 22.2ch を 5.1ch にダウンミックスするものではなく、非対応の音声を
MSE に渡さないための安全策です。複数の音声トラックがある場合は
`track.audio.channels` を見て 5.1ch またはステレオの代替トラックを選択して
ください。省略時または `0` の場合、チャンネル数による制限は行いません。

TypeScript declarations for the module, callbacks, events, duration probe and
recording index are included. The npm package contains the generated wrapper
with its WebAssembly binary embedded, so consumers do not need Emscripten and
do not make a separate `.wasm` request.

### iOS and iPadOS Safari

The WASM demuxer itself runs on current iOS Safari, including the `BigInt`
values used by the public API. Player integrations must not assume that the
standard `MediaSource` constructor exists, however: iOS exposes the compatible
`ManagedMediaSource` API instead. Select the constructor once and use it for
both capability checks and construction:

```js
const BrowserMediaSource = globalThis.ManagedMediaSource || globalThis.MediaSource;
if (!BrowserMediaSource?.isTypeSupported(mime)) throw new Error(`Unsupported: ${mime}`);
const mediaSource = new BrowserMediaSource();
```

Register the `sourceopen` listener before assigning the object URL to the video
element, then attach it and begin playback. `demo/demo.js` implements this
path. `demo/ios-compat.html` is a small feature and end-to-end diagnostic page;
it reports WASM, HEVC/AAC, MSE/MMS and SourceBuffer results separately.

Do not use the iOS Simulator as the final ManagedMediaSource playback verdict.
WebKit bug 266764 documents that the simulator can expose the API but never
open the source. Confirm the SourceBuffer stage on physical iPhone/iPad
hardware. See also WebKit's ManagedMediaSource integration example:

- https://webkit.org/blog/15036/how-to-use-media-source-extensions-with-airplay/
- https://bugs.webkit.org/show_bug.cgi?id=266764

### Build the npm package

Build the browser/worker wrapper with Emscripten:

```sh
nix-shell
emcmake cmake -S . -B build-wasm -G Ninja \
  -DBUILD_SHARED_LIBS=OFF -DTLVDEMUX_BUILD_TOOLS=OFF
cmake --build build-wasm --target tlvdemux-wasm
```

From `nix-shell`, `npm run build` performs the same release build and copies the
result to `dist/tlvdemux.js`. `npm pack --dry-run` runs the release build and
WASM smoke test before showing the exact files that would be published.

The result is a single self-contained `build-wasm/tlvdemux.js`; the WebAssembly
binary is embedded and no separate `.wasm` request is made. Load it as a normal
script and create a demuxer asynchronously:

```js
const module = await createTlvDemuxModule();
const demuxer = new module.TlvDemuxer({
  onTrack: track => console.log(track),
  onEventInfo: event => console.log(event.title, event.startTimeUnixMilliseconds),
  onStreamEvent: event => console.log(event.eventMessageTag, event.messageId),
  onAccessUnitView: unit => consumeSynchronously(unit),
  onApplicationState: application => console.log(application.state),
  onApplicationResourceView: resource => console.log(resource.path),
  onError: error => console.warn(error),
});

demuxer.push(chunk); // Uint8Array; copied into WASM memory
demuxer.flush();
demuxer.delete();
```

For loaders that already manage buffers, `_malloc`, `_free`, `HEAPU8`, and
`pushFromHeap(address, size)` provide a reusable heap-buffer path. JavaScript
receives 64-bit offsets, timestamps, and track IDs as `BigInt` values.
MH-EIT current/following and schedule entries are reported through
`onEventInfo`; `tableId === 0x8b` with `sectionNumber` 0/1 identifies the
present/following event for the service.
ARIB STD-B60 EMT messages are reported through `onStreamEvent`. The event
contains the MPT-signalled EMT tag, group/id/version, private bytes, and the raw
time-mode fields so the receiver can ignite timed messages against its playback
clock instead of the demux/read-ahead clock. `rawMessageId` preserves B60's
16-bit descriptor field; its high octet is exposed as `messageId` and its low
octet as `messageVersion` to the B62 application.
`onAccessUnitView` avoids copying media output, but its `data` view is valid only
for the duration of the callback and must be consumed synchronously. Use
`onAccessUnit` instead when the callback needs an owned `Uint8Array` copy.
`onApplicationResourceView` has the same callback-only lifetime; use
`onApplicationResource` for an owned copy.

`TlvDemuxer` also owns an `ApplicationResourceStore`. `applicationResources()`
lists its files, `applicationResource(contextId, path)` returns an owned file,
`applications()` reports current application states, and
`applicationEntry(contextId)` resolves the ready entry document. This keeps
path validation, version replacement, and entry resolution in C++/WASM rather
than duplicating those rules in each browser loader.

Run the application-resource WASM integration test against a captured stream
with:

```sh
node tests/wasm_application_resources.mjs build-wasm/tlvdemux.js test.tlv
```

`DurationProbe` drives fast head/tail reads without owning a file or HTTP
client. Start it with the known file size, fulfill each object returned by
`nextRange()`, and pass the exact bytes to `pushRange()`. A successful
`duration()` has `status: "complete"`; failure remains explicit through
`state()` and `failure()` and never falls back to downloading the whole file.
The native `tlvdemux-probe INPUT` tool exercises the same protocol.

For precise recorded seek, call `startIndex(false)` before feeding the full
stream and `finalizeIndex()` at its real EOF. `seekPointsFor(targetUs)` returns
the surrounding RAP checkpoints. Reposition to `first.signallingOffset`, feed
from there, decode from the emitted RAP, and present the first frame at or after
the requested time.

### Browser demo

Build the sibling `libaribhtml5` receiver SDK and `build-wasm/tlvdemux.js`, then
serve the repository root and open `/demo/`:

```sh
(cd ../libaribhtml5 && pnpm build:sdk)
node demo/server.mjs
```

The bundled development server supports the `206` and `Content-Range`
responses required by duration probing and recorded seek. Python's basic
`python3 -m http.server` is not suitable for this demo because it does not
provide the required Range behavior.

The demo accepts either a local MMTS file or an HTTP URL, probes its duration,
then plays the selected HEVC and AAC tracks through Media Source Extensions.
Application resources collected by WASM are exposed to a sandboxed data-
broadcast iframe through the same-origin Service Worker VFS shipped by
`libaribhtml5`. The receiver API, video-plane handling, document preparation,
built-in ROM sounds, and remote-control behavior also come from
`libaribhtml5`; external application URLs remain blocked.
Local files use `Blob.slice()`; remote files require validated `206` and
`Content-Range` responses. Live mode skips duration probing and seeking, uses a
normal streaming `GET`, and exposes the Media Source as an unbounded timeline.
HTTP URLs that do not return a valid Range response automatically fall back to
Live mode.
The demo contains a deliberately small fMP4/MSE layer and does not depend on
mmts.js at runtime. Browser HEVC MSE support is still required.

Demuxing and fMP4 remuxing run in `demo/demux-worker-runtime.js`. The main
thread sends input chunks as transferable buffers and receives only MSE init
segments, media segments, subtitle payloads, application files, and small
control events. `demo/worker-tlvdemux.js` owns the RPC facade, while
`demo/demux-worker-protocol.js` contains the shared message names. Keeping these
three responsibilities separate makes it possible to change the player UI,
the transport protocol, or the worker-side demux lifecycle independently.

Run the repeatable WASM throughput benchmark with:

```sh
npm run benchmark:wasm -- build-wasm/tlvdemux.js test.tlv 268435456
```

It reports demux-only and demux-plus-MSE throughput, callback/segment counts,
output bytes, and maximum observed WASM heap size. See
[`docs/performance.md`](docs/performance.md) for the hot-path ownership map,
measurement guidance, and regression checklist.

## Current scope

Version 0.1 supports the ARIB broadcast subset exercised by the validation
streams: all four HCfB compressed-IP modes (`0x20`, `0x21`, `0x60`, `0x61`),
MMTP signalling and fragmented media, HEVC Annex B, AAC-LATM/LOAS, and ARIB
STD-B62 TTML. The recording helpers provide bounded duration probing, sparse RAP
indexing, and recording-relative repositioning. CAS/descrambling, decoder and
TTML rendering, persistent index serialization, and general-purpose ISO MMT are
outside the library's current scope.
