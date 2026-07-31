import assert from 'node:assert/strict';
import { open } from 'node:fs/promises';
import { createRequire } from 'node:module';

const [modulePath, mediaPath] = process.argv.slice(2);
assert.ok(modulePath && mediaPath,
  'usage: node tests/wasm_mse_leading_rasl.mjs TLVDEMUX_JS SAMPLE');

const u32 = (data, offset) =>
  ((data[offset] * 0x1000000) + (data[offset + 1] << 16) +
   (data[offset + 2] << 8) + data[offset + 3]) >>> 0;
const u64 = (data, offset) => u32(data, offset) * 0x100000000 + u32(data, offset + 4);
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

function nalUnits(sample) {
  const result = [];
  for (let offset = 0; offset + 4 <= sample.byteLength;) {
    const size = u32(sample, offset);
    offset += 4;
    assert.ok(size >= 2 && offset + size <= sample.byteLength, 'invalid HEVC sample');
    result.push(sample.subarray(offset, offset + size));
    offset += size;
  }
  return result;
}

const nalTypes = sample => nalUnits(sample).map(nalu => (nalu[0] >> 1) & 0x3f);
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
    const flags = u32(segment, entry + 8);
    result.push({ data: segment.subarray(payload, payload + size), flags });
    entry += 16;
    payload += size;
  }
  return result;
}

function timing(segment) {
  const top = childBoxes(segment, 0, segment.byteLength);
  const moof = top.find(box => box.type === 'moof');
  assert.ok(moof, 'media segment lacks moof');
  const traf = childBoxes(segment, moof.offset + 8, moof.offset + moof.size)
    .find(box => box.type === 'traf');
  const children = childBoxes(segment, traf.offset + 8, traf.offset + traf.size);
  const tfdt = children.find(box => box.type === 'tfdt');
  const trun = children.find(box => box.type === 'trun');
  assert.ok(tfdt && trun, 'traf lacks tfdt or trun');
  const count = u32(segment, trun.offset + 12);
  let duration = 0;
  for (let index = 0, entry = trun.offset + 20; index < count; index += 1, entry += 16) {
    duration += u32(segment, entry);
  }
  return { start: u64(segment, tfdt.offset + 12), duration };
}

const require = createRequire(import.meta.url);
const createTlvDemuxModule = require(modulePath);
const module = await createTlvDemuxModule();
let videoTrack = null;
const videoSegments = [];
let videoMime = null;
let videoInit = null;
let demuxer;
demuxer = new module.TlvDemuxer({
  onTrack(track) {
    if (track.kind === 'video' && videoTrack === null) {
      videoTrack = track.trackId;
      demuxer.selectTrack('video', videoTrack);
    }
  },
  onMseInit(init) {
    if (init.type === 'video') {
      videoMime = init.mime;
      videoInit = init.data;
    }
  },
  onMseSegment(segment) {
    if (segment.type === 'video') videoSegments.push(segment.data);
  },
});

const file = await open(mediaPath, 'r');
const chunk = new Uint8Array(2 * 1024 * 1024);
let position = 0;
try {
  while (videoSegments.length < 4 && position < 32 * 1024 * 1024) {
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

assert.ok(videoSegments.length > 0, 'no video media segment was emitted');
assert.match(videoMime, /^video\/mp4; codecs="hvc1\./,
  `video sample entry is not hvc1: ${videoMime}`);
assert.ok(Buffer.from(videoInit).includes(Buffer.from('hvc1')),
  'video init segment does not contain an hvc1 sample entry');
assert.ok(!Buffer.from(videoInit).includes(Buffer.from('hev1')),
  'video init segment still contains an hev1 sample entry');
const firstSegmentSamples = samples(videoSegments[0]);
const emittedSamples = videoSegments.flatMap(segment => samples(segment));
const segmentTimings = videoSegments.map(timing);
assert.ok(emittedSamples.length >= 2, 'first video segment has fewer than two samples');
assert.ok(firstSegmentSamples.length <= 20,
  `video fragment still batches about one second: ${firstSegmentSamples.length} samples`);
assert.equal(segmentTimings[0].start, 0, 'fresh sequence does not start at DTS zero');
for (let index = 1; index < segmentTimings.length; index += 1) {
  const previousEnd = segmentTimings[index - 1].start + segmentTimings[index - 1].duration;
  assert.ok(segmentTimings[index].start >= previousEnd,
    `video decode timeline overlaps at segment ${index}: ${segmentTimings[index].start} < ${previousEnd}`);
}
const firstTypes = nalTypes(emittedSamples[0].data);
const secondTypes = nalTypes(emittedSamples[1].data);
assert.ok(firstTypes.includes(21), `first sample is not CRA: ${firstTypes}`);
assert.ok(!secondTypes.some(value => value === 8 || value === 9),
  `NoRaslOutputFlag leading picture leaked into the fresh sequence: ${secondTypes}`);
for (const [index, sample] of emittedSamples.entries()) {
  const types = nalTypes(sample.data);
  assert.ok(!types.some(value => value >= 32 && value <= 35),
    `hvc1 sample ${index} contains parameter set or AUD: ${types}`);
}
assert.equal(emittedSamples[0].flags, 0x02000000, 'first CRA is not a sync sample');
const laterCra = emittedSamples.slice(1)
  .map(sample => ({ sample, types: nalTypes(sample.data) }))
  .find(item => item.types.includes(21));
assert.ok(laterCra, 'first segment does not contain a later CRA');
assert.equal(laterCra.sample.flags, 0x02000000,
  `later CRA is not a sync sample: ${laterCra.sample.flags.toString(16)}`);

console.log(JSON.stringify({
  bytesRead: position,
  samples: emittedSamples.length,
  firstTypes,
  secondTypes,
  laterCraFlags: `0x${laterCra.sample.flags.toString(16)}`,
}, null, 2));
