import assert from 'node:assert/strict';
import { closeSync, openSync, readSync } from 'node:fs';
import { performance } from 'node:perf_hooks';
import { createRequire } from 'node:module';
import { resolve } from 'node:path';

const [modulePath, mediaPath, maximumBytesText, chunkBytesText] = process.argv.slice(2);
assert.ok(modulePath && mediaPath,
  'usage: node tests/wasm_benchmark.mjs TLVDEMUX_JS SAMPLE [MAX_BYTES] [CHUNK_BYTES]');

const maximumBytes = Number(maximumBytesText ?? 256 * 1024 * 1024);
assert.ok(Number.isSafeInteger(maximumBytes) && maximumBytes > 0, 'invalid MAX_BYTES');
const chunkBytes = Number(chunkBytesText ?? 2 * 1024 * 1024);
assert.ok(Number.isSafeInteger(chunkBytes) && chunkBytes > 0, 'invalid CHUNK_BYTES');

const require = createRequire(import.meta.url);
const createTlvDemuxModule = require(resolve(modulePath));
const module = await createTlvDemuxModule();

function benchmark(remux) {
  let demuxer;
  let videoTrack = null;
  let audioTrack = null;
  let accessUnits = 0;
  let accessUnitBytes = 0;
  let mediaSegments = 0;
  let mediaSegmentBytes = 0;
  let maximumWasmHeap = module.HEAPU8.byteLength;
  const callbacks = {
    onTrack(track) {
      if (track.kind === 'video' && videoTrack === null) {
        videoTrack = track.trackId;
        demuxer.selectTrack('video', videoTrack);
      } else if (track.kind === 'audio' && audioTrack === null) {
        audioTrack = track.trackId;
        demuxer.selectTrack('audio', audioTrack);
      }
    },
    onAccessUnitView(unit) {
      accessUnits += 1;
      accessUnitBytes += unit.data.byteLength;
    },
  };
  if (remux) {
    callbacks.onMseInit = () => {};
    callbacks.onMseSegment = segment => {
      mediaSegments += 1;
      mediaSegmentBytes += segment.data.byteLength;
    };
  }
  demuxer = new module.TlvDemuxer(callbacks);

  const descriptor = openSync(mediaPath, 'r');
  const chunk = new Uint8Array(chunkBytes);
  const inputAddress = module._malloc(chunk.byteLength);
  assert.ok(inputAddress, `cannot allocate ${chunk.byteLength} bytes of WASM input memory`);
  let position = 0;
  const started = performance.now();
  try {
    while (position < maximumBytes) {
      const length = Math.min(chunk.byteLength, maximumBytes - position);
      const count = readSync(descriptor, chunk, 0, length, position);
      if (count === 0) break;
      module.HEAPU8.set(chunk.subarray(0, count), inputAddress);
      assert.equal(demuxer.pushFromHeap(inputAddress, count), true);
      while (demuxer.drainApplicationResources(256)) {}
      position += count;
      maximumWasmHeap = Math.max(maximumWasmHeap, module.HEAPU8.byteLength);
    }
    demuxer.flush();
    while (demuxer.drainApplicationResources(256)) {}
  } finally {
    closeSync(descriptor);
    demuxer.delete();
    module._free(inputAddress);
  }
  const elapsedMilliseconds = performance.now() - started;
  return {
    mode: remux ? 'demux+mse' : 'demux',
    inputBytes: position,
    elapsedMilliseconds,
    inputMiBPerSecond: position / 1024 / 1024 / (elapsedMilliseconds / 1000),
    accessUnits,
    accessUnitBytes,
    mediaSegments,
    mediaSegmentBytes,
    maximumWasmHeapBytes: maximumWasmHeap,
  };
}

console.log(JSON.stringify({
  sample: mediaPath,
  maximumBytes,
  chunkBytes,
  inputMode: 'reusable-wasm-heap',
  results: [benchmark(false), benchmark(true)],
}, null, 2));
