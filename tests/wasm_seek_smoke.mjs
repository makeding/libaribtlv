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
let audioTrack = null;
let headVideo = false;
let firstSeekUnit = null;
let probeRap = null;
const mseSegments = { video: 0, audio: 0 };
let demuxer;
demuxer = new module.TlvDemuxer({
  onMseSegment(segment) { mseSegments[segment.type] += 1; },
  onTrack(track) {
    if (track.kind === 'video' && videoTrack === null) {
      videoTrack = track.trackId;
      demuxer.selectTrack('video', videoTrack);
      assert.equal(demuxer.setIndexDuration(durationUs), true);
    } else if (track.kind === 'audio' && audioTrack === null) {
      audioTrack = track.trackId;
      demuxer.selectTrack('audio', audioTrack);
    }
  },
  onAccessUnit(unit) {
    if (unit.trackId !== videoTrack) return;
    if (phase === 'head') headVideo = true;
    else if (phase === 'probe' && unit.randomAccess && probeRap === null) {
      probeRap = {
        seconds: Number(unit.ptsValue) / unit.ptsTimescale,
        restartOffset: BigInt(unit.restartOffset),
      };
    } else if (phase === 'seek' && firstSeekUnit === null) firstSeekUnit = unit;
  },
  onError(error) {
    if (!error.recoverable) throw new Error(error.message);
  },
});
demuxer.setMseOutputEnabled(false);
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
const minPreroll = 16n * 1024n * 1024n;
const maxPreroll = 128n * 1024n * 1024n;
const estimatedPreroll = size * 8000000n / durationUs;
let preroll = estimatedPreroll < minPreroll ? minPreroll
  : estimatedPreroll > maxPreroll ? maxPreroll : estimatedPreroll;
let candidate = 0n;
let attempts = 0;
for (;;) {
  candidate = estimate > preroll ? estimate - preroll : 0n;
  demuxer.reposition(candidate, true);
  phase = 'probe';
  probeRap = null;
  let probeOffset = candidate;
  const probeLimit = candidate + 64n * 1024n * 1024n < size
    ? candidate + 64n * 1024n * 1024n : size;
  while (probeRap === null && probeOffset < probeLimit) {
    const wanted = Number(probeLimit - probeOffset < BigInt(buffer.byteLength)
      ? probeLimit - probeOffset : BigInt(buffer.byteLength));
    const { bytesRead } = await file.read(buffer, 0, wanted, Number(probeOffset));
    assert.ok(bytesRead > 0, 'EOF while probing seek restart');
    demuxer.push(buffer.subarray(0, bytesRead));
    probeOffset += BigInt(bytesRead);
  }
  if (probeRap !== null && probeRap.seconds <= Number(targetText) + 0.05) break;
  assert.ok(candidate !== 0n && ++attempts < 5,
    'could not find a RAP before the requested time');
  preroll *= 2n;
  if (preroll > estimate) preroll = estimate;
}
const start = probeRap.restartOffset;
demuxer.reposition(start, true);
demuxer.setMseOutputEnabled(true);
phase = 'seek';
let offset = start;
const limit = start + 64n * 1024n * 1024n < size ? start + 64n * 1024n * 1024n : size;
while ((firstSeekUnit === null || mseSegments.video === 0 || mseSegments.audio === 0) && offset < limit) {
  const wanted = Number(limit - offset < BigInt(buffer.byteLength) ? limit - offset : BigInt(buffer.byteLength));
  const { bytesRead } = await file.read(buffer, 0, wanted, Number(offset));
  assert.ok(bytesRead > 0, 'EOF while seeking');
  demuxer.push(buffer.subarray(0, bytesRead));
  offset += BigInt(bytesRead);
}
await file.close();
assert.ok(firstSeekUnit, 'seek did not emit selected video within 64 MiB');
console.log(JSON.stringify({
  targetSeconds: Number(targetUs) / 1000000,
  estimate: estimate.toString(),
  start: start.toString(),
  preroll: preroll.toString(),
  probeAttempts: attempts + 1,
  probeRapSeconds: probeRap.seconds,
  bytesReadAfterSeek: (offset - start).toString(),
  firstVideoSeconds: Number(firstSeekUnit.ptsValue) / firstSeekUnit.ptsTimescale,
  randomAccess: firstSeekUnit.randomAccess,
  discontinuity: firstSeekUnit.discontinuity,
  restartOffset: firstSeekUnit.restartOffset.toString(),
  inputOffset: firstSeekUnit.inputOffset.toString(),
  mseSegments,
}, null, 2));
demuxer.delete();
assert.ok(Number(firstSeekUnit.ptsValue) / firstSeekUnit.ptsTimescale <= Number(targetText) + 0.25,
  'seek restart RAP is later than the requested time');
assert.ok(mseSegments.video > 0, 'seek did not produce an MSE video segment within 64 MiB');
assert.ok(mseSegments.audio > 0, 'seek did not produce an MSE audio segment within 64 MiB');
