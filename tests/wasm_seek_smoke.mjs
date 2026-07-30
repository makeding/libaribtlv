import assert from 'node:assert/strict';
import { open } from 'node:fs/promises';
import { createRequire } from 'node:module';

const [modulePath, mediaPath, targetText, durationText] = process.argv.slice(2);
assert.ok(modulePath && mediaPath && targetText && durationText,
  'usage: node tests/wasm_seek_smoke.mjs TLVDEMUX_JS SAMPLE TARGET_S DURATION_S');
const targetUs = BigInt(Math.round(Number(targetText) * 1000000));
const durationUs = BigInt(Math.round(Number(durationText) * 1000000));
const require = createRequire(import.meta.url);
const createTlvDemuxModule = require(modulePath);
const module = await createTlvDemuxModule();
const file = await open(mediaPath, 'r');
const size = BigInt((await file.stat()).size);
let phase = 'head';
let videoTrack = null;
let headVideo = false;
let firstSeekUnit = null;
let demuxer;
demuxer = new module.TlvDemuxer({
  onTrack(track) {
    if (track.kind === 'video' && videoTrack === null) {
      videoTrack = track.trackId;
      demuxer.selectTrack('video', videoTrack);
      assert.equal(demuxer.setIndexDuration(durationUs), true);
    }
  },
  onAccessUnit(unit) {
    if (unit.trackId !== videoTrack) return;
    if (phase === 'head') headVideo = true;
    else if (firstSeekUnit === null) firstSeekUnit = unit;
  },
  onError(error) {
    if (!error.recoverable) throw new Error(error.message);
  },
});
demuxer.startIndex(false);

const buffer = new Uint8Array(2 * 1024 * 1024);
let headOffset = 0;
while (!headVideo && headOffset < 64 * 1024 * 1024) {
  const { bytesRead } = await file.read(buffer, 0, buffer.byteLength, headOffset);
  assert.ok(bytesRead > 0, 'EOF while priming');
  demuxer.push(buffer.subarray(0, bytesRead));
  headOffset += bytesRead;
}
assert.ok(headVideo, 'head did not emit selected video');
assert.equal(demuxer.setIndexDuration(durationUs), true);
const estimate = demuxer.estimateOffset(targetUs, size);
assert.notEqual(estimate, null, 'estimateOffset returned null');
const start = estimate > 16n * 1024n * 1024n ? estimate - 16n * 1024n * 1024n : 0n;
demuxer.reposition(start, true);
phase = 'seek';
let offset = start;
const limit = start + 64n * 1024n * 1024n < size ? start + 64n * 1024n * 1024n : size;
while (firstSeekUnit === null && offset < limit) {
  const wanted = Number(limit - offset < BigInt(buffer.byteLength) ? limit - offset : BigInt(buffer.byteLength));
  const { bytesRead } = await file.read(buffer, 0, wanted, Number(offset));
  assert.ok(bytesRead > 0, 'EOF while seeking');
  demuxer.push(buffer.subarray(0, bytesRead));
  offset += BigInt(bytesRead);
}
await file.close();
demuxer.delete();
assert.ok(firstSeekUnit, 'seek did not emit selected video within 64 MiB');
console.log(JSON.stringify({
  targetSeconds: Number(targetUs) / 1000000,
  estimate: estimate.toString(),
  start: start.toString(),
  bytesReadAfterSeek: (offset - start).toString(),
  firstVideoSeconds: Number(firstSeekUnit.ptsValue) / firstSeekUnit.ptsTimescale,
  randomAccess: firstSeekUnit.randomAccess,
  discontinuity: firstSeekUnit.discontinuity,
  restartOffset: firstSeekUnit.restartOffset.toString(),
  inputOffset: firstSeekUnit.inputOffset.toString(),
}, null, 2));
