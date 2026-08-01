# Performance architecture

The browser path keeps parsing and fMP4 construction off the main thread. The
main thread remains responsible for fetching, `MediaSource`/`SourceBuffer`, the
subtitle renderer, and the data-broadcast document. This boundary is the most
important performance property: a large MMTS chunk must not turn into a burst
of per-access-unit callbacks on the UI thread.

## Ownership map

| Area | Owner | Responsibility |
| --- | --- | --- |
| Worker message constants | `demo/demux-worker-protocol.js` | Stable RPC and event names shared by both sides. |
| Main-thread facade | `demo/worker-tlvdemux.js` | Promise correlation, transferable input, callback dispatch, and application-entry cache. |
| Worker lifecycle | `demo/demux-worker-runtime.js` | One WASM module, probe/demux instances, track selection, resource draining, and event filtering. |
| MSE orchestration | `wasm/mse_remuxer.cpp` | Per-track state, timestamps, discontinuities, and segment emission. |
| MP4 construction | `src/mse/mp4_builder.*` | Init/media boxes and single-pass `moof`/`mdat` output assembly. |
| HEVC parsing | `src/mse/hevc_parser.*` | Annex-B NAL views, SPS metadata, and `hvcC`. |
| AAC parsing | `src/mse/latm_parser.*` | LATM/LOAS framing and AudioSpecificConfig. |
| Shared byte utilities | `src/mse/common.hpp` | Bounded byte/bit readers and append helpers. |

The worker receives each input `ArrayBuffer` by transfer, so the main thread
does not retain or clone a full MMTS chunk. Small live-network chunks are
coalesced up to 512 KiB with a 25 ms latency bound before transfer. Input is
copied into a reusable, grow-only WASM staging allocation and passed through
`pushFromHeap()`, avoiding a fresh C++ input vector allocation for every push.
Once TLV synchronization is established, the parser reads complete packets
directly from that staging allocation and retains only the packet tail crossing
a push boundary; malformed input still falls back to the bounded resync buffer.
Media access units normally stay inside WASM: HEVC and AAC data are converted
into fMP4 in the worker, and only complete MSE segments cross back. TTML access units and
application files are the exceptions because their consumers run on the main
thread. The sparse `onPlaybackAccessUnitView` callback forwards HEVC RAP and
discontinuity metadata and AAC discontinuity metadata without their
elementary-stream payload, avoiding a JS object for every ordinary media AU.

Application-resource assembly drains in bounded worker batches between media
pushes and exhaustively at flush. Non-seekable live playback does not build a
recording index; otherwise its RAP vector would grow for the lifetime of the
stream without providing a usable seek target.

Within the remuxer, Annex-B parsing uses non-owning NAL views. The final sample
buffer is reserved once and filled directly. Media-segment construction also
reserves the final size and writes `moof`, `mdat`, and sample bytes into one
buffer. Avoid reintroducing intermediate per-NAL, `mdat` payload, or complete
segment copies.

## Benchmark

Build the release WASM artifact and run the same byte range for both modes:

```sh
npm run build:wasm
npm run benchmark:wasm -- \
  build-wasm/tlvdemux.js demo/bsp4k-lag-1.mmts 162000000
```

The benchmark intentionally consumes callbacks, drains application resources
like the worker, but performs no decoding or `SourceBuffer.appendBuffer()`. It
isolates demux/remux cost from the browser and prints JSON suitable for saving
and comparing in CI. On the 162 MB BSP4K sample
used during the worker/remux split, repeated release runs measured 655--839
MiB/s for demux and 465--502 MiB/s for demux plus MSE. The same machine measured
about 400 MiB/s for the pre-split MSE path, a roughly 16--25% improvement.
Absolute numbers depend on CPU state and the host; compare multiple runs from
both builds on the same machine and sample.

For browser profiling, inspect both the Worker and main-thread tracks. A fast
Node benchmark does not prove smooth playback: check for long main-thread tasks,
`SourceBuffer` queue growth, dropped frames, and memory growth during a long
recording or live stream.

## Regression checklist

Run the native and WASM suites, compare remuxed bytes when changing MP4 code,
then exercise real seek and track switching:

```sh
ctest --test-dir build --output-on-failure
npm run build
npm test
node tests/demo_playback_smoke.mjs \
  build-wasm/tlvdemux.js demo/bsp4k-lag-1.mmts
./build/tlvdemux-cocktail --cases 256 --seed 20260801 \
  demo/bsp4k-lag-1.mmts
```

For dual-video validation, pass packet IDs that were actually enumerated from
the chosen recording. A `secondary-not-found` result means the requested asset
is absent from that file; it is not evidence of a parser or switching failure.
The browser demo remains the end-to-end check for Worker startup, MSE playback,
subtitles, and data-broadcast resource delivery.
