import assert from 'node:assert/strict';
import { open } from 'node:fs/promises';
import { createRequire } from 'node:module';

const [modulePath, mediaPath] = process.argv.slice(2);
assert.ok(modulePath && mediaPath,
  'usage: node tests/wasm_mse_leading_rasl.mjs TLVDEMUX_JS SAMPLE');

const u32 = (data, offset) =>
  ((data[offset] * 0x1000000) + (data[offset + 1] << 16) +
   (data[offset + 2] << 8) + data[offset + 3]) >>> 0;
const type = (data, offset) =>
  String.fromCharCode(data[offset + 4], data[offset + 5], data[offset + 6], data[offset + 7]);

function childBoxes(data, start, end) {
  const boxes = [];
  for (let offset = start; offset + 8 <= end;) {
    const size = u32(data, offset);
    assert.ok(size >= 8 && offset + size <= end, `invalid MP4 box at ${offset}`);
    boxes.push({ offset, size, type: type(data, offset) });
    offset += size;
  }
  return boxes;
}

function nalTypes(sample) {
  const result = [];
  for (let offset = 0; offset + 4 <= sample.byteLength;) {
    const size = u32(sample, offset);
    offset += 4;
    assert.ok(size >= 2 && offset + size <= sample.byteLength, 'invalid HEVC sample');
    result.push((sample[offset] >> 1) & 0x3f);
    offset += size;
  }
  return result;
}

function samples(segment) {
  const top = childBoxes(segment, 0, segment.byteLength);
  const moof = top.find(box => box.type === 'moof');
  const mdat = top.find(box => box.type === 'mdat');
  assert.ok(moof && mdat, 'media segment lacks moof/mdat');
  const traf = childBoxes(segment, moof.offset + 8, moof.offset + moof.size)
    .find(box => box.type === 'traf');
  assert.ok(traf, 'moof lacks traf');
  const trun = childBoxes(segment, traf.offset + 8, traf.offset + traf.size)
    .find(box => box.type === 'trun');
  assert.ok(trun, 'traf lacks trun');

  const count = u32(segment, trun.offset + 12);
  let entry = trun.offset + 20;
  let payload = mdat.offset + 8;
  const result = [];
  for (let index = 0; index < count; index += 1) {
    const size = u32(segment, entry + 4);
    result.push(segment.subarray(payload, payload + size));
    entry += 16;
    payload += size;
  }
  return result;
}

const require = createRequire(import.meta.url);
const createTlvDemuxModule = require(modulePath);
const module = await createTlvDemuxModule();
let videoTrack = null;
let firstVideoSegment = null;
let demuxer;
demuxer = new module.TlvDemuxer({
  onTrack(track) {
    if (track.kind === 'video' && videoTrack === null) {
      videoTrack = track.trackId;
      demuxer.selectTrack('video', videoTrack);
    }
  },
  onMseSegment(segment) {
    if (segment.type === 'video' && firstVideoSegment === null) firstVideoSegment = segment.data;
  },
});

const file = await open(mediaPath, 'r');
const chunk = new Uint8Array(2 * 1024 * 1024);
let position = 0;
try {
  while (firstVideoSegment === null && position < 32 * 1024 * 1024) {
    const { bytesRead } = await file.read(chunk, 0, chunk.byteLength, position);
    if (bytesRead === 0) break;
    assert.equal(demuxer.push(chunk.subarray(0, bytesRead)), true);
    position += bytesRead;
  }
  demuxer.flush();
} finally {
  await file.close();
  demuxer.delete();
}

assert.ok(firstVideoSegment, 'no video media segment was emitted');
const emittedSamples = samples(firstVideoSegment);
assert.ok(emittedSamples.length >= 2, 'first video segment has fewer than two samples');
const firstTypes = nalTypes(emittedSamples[0]);
const secondTypes = nalTypes(emittedSamples[1]);
assert.ok(firstTypes.includes(21), `first sample is not CRA: ${firstTypes}`);
assert.ok(!secondTypes.some(value => value === 8 || value === 9),
  `leading RASL leaked into the fresh decode sequence: ${secondTypes}`);

console.log(JSON.stringify({
  bytesRead: position,
  samples: emittedSamples.length,
  firstTypes,
  secondTypes,
}, null, 2));
