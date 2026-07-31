import assert from 'node:assert/strict';
import { closeSync, mkdirSync, openSync, readSync, writeSync } from 'node:fs';
import { createRequire } from 'node:module';

const [modulePath, mediaPath, outputDirectory, maximumBytesText] = process.argv.slice(2);
assert.ok(modulePath && mediaPath && outputDirectory,
  'usage: node tests/wasm_mse_dump.mjs TLVDEMUX_JS SAMPLE OUTPUT_DIR [MAX_BYTES]');

const maximumBytes = Number(maximumBytesText ?? 640 * 1024 * 1024);
assert.ok(Number.isSafeInteger(maximumBytes) && maximumBytes > 0, 'invalid MAX_BYTES');
mkdirSync(outputDirectory, { recursive: true });

const descriptors = new Map([
  ['video', openSync(`${outputDirectory}/video.mp4`, 'w')],
  ['audio', openSync(`${outputDirectory}/audio.mp4`, 'w')],
]);
const initialized = new Set();
const segmentCounts = new Map([['video', 0], ['audio', 0]]);

const write = (type, data) => {
  const descriptor = descriptors.get(type);
  assert.notEqual(descriptor, undefined, `unexpected MSE track: ${type}`);
  let offset = 0;
  while (offset < data.byteLength) offset += writeSync(descriptor, data, offset);
};

const require = createRequire(import.meta.url);
const createTlvDemuxModule = require(modulePath);
const module = await createTlvDemuxModule();
let videoTrack = null;
let audioTrack = null;
const fatalErrors = [];
let demuxer;
demuxer = new module.TlvDemuxer({
  onTrack(track) {
    if (track.kind === 'video' && videoTrack === null) {
      videoTrack = track.trackId;
      demuxer.selectTrack('video', videoTrack);
    } else if (track.kind === 'audio' && audioTrack === null) {
      audioTrack = track.trackId;
      demuxer.selectTrack('audio', audioTrack);
    }
  },
  onMseInit(init) {
    if (initialized.has(init.type)) return;
    initialized.add(init.type);
    write(init.type, init.data);
  },
  onMseSegment(segment) {
    write(segment.type, segment.data);
    segmentCounts.set(segment.type, segmentCounts.get(segment.type) + 1);
  },
  onError(error) {
    if (!error.recoverable) fatalErrors.push(error);
  },
});

const input = openSync(mediaPath, 'r');
const chunk = new Uint8Array(2 * 1024 * 1024);
let position = 0;
try {
  while (position < maximumBytes) {
    const length = Math.min(chunk.byteLength, maximumBytes - position);
    const bytesRead = readSync(input, chunk, 0, length, position);
    if (bytesRead === 0) break;
    assert.equal(demuxer.push(chunk.subarray(0, bytesRead)), true);
    position += bytesRead;
  }
  demuxer.flush();
} finally {
  closeSync(input);
  demuxer.delete();
  for (const descriptor of descriptors.values()) closeSync(descriptor);
}

assert.deepEqual(fatalErrors, []);
assert.ok(segmentCounts.get('video') > 0, 'no video media segments');
assert.ok(segmentCounts.get('audio') > 0, 'no audio media segments');
console.log(JSON.stringify({ bytesRead: position, segments: Object.fromEntries(segmentCounts) }));
