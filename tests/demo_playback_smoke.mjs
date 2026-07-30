import assert from 'node:assert/strict';
import { mkdir, open, writeFile } from 'node:fs/promises';
import { createRequire } from 'node:module';

const [modulePath, mediaPath, outputDirectory] = process.argv.slice(2);
assert.ok(modulePath && mediaPath,
  'usage: node tests/demo_playback_smoke.mjs TLVDEMUX_JS SAMPLE');

const require = createRequire(import.meta.url);
const createTlvDemuxModule = require(modulePath);
const module = await createTlvDemuxModule();
const initSegments = new Map();
const mediaSegments = new Map([['video', []], ['audio', []]]);
let videoTrack = null;
let audioTrack = null;
const fatalErrors = [];
let demuxer;
demuxer = new module.TlvDemuxer({
  onMseInit(init) { initSegments.set(init.type, init); },
  onMseSegment(segment) { mediaSegments.get(segment.type).push(segment.data); },
  onTrack(track) {
    if (track.kind === 'video' && videoTrack === null) {
      videoTrack = track.trackId;
      demuxer.selectTrack('video', videoTrack);
    } else if (track.kind === 'audio' && audioTrack === null) {
      audioTrack = track.trackId;
      demuxer.selectTrack('audio', audioTrack);
    }
  },
  onError(error) { if (!error.recoverable) fatalErrors.push(error); },
});

const file = await open(mediaPath, 'r');
const chunk = new Uint8Array(2 * 1024 * 1024);
let position = 0;
try {
  while (position < 16 * 1024 * 1024 &&
         (!initSegments.has('video') || !initSegments.has('audio') ||
          mediaSegments.get('video').length === 0 || mediaSegments.get('audio').length === 0)) {
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

function boxType(data, offset = 0) {
  return String.fromCharCode(data[offset + 4], data[offset + 5], data[offset + 6], data[offset + 7]);
}

function uint32(data, offset) {
  return new DataView(data.buffer, data.byteOffset, data.byteLength).getUint32(offset);
}

function int32(data, offset) {
  return new DataView(data.buffer, data.byteOffset, data.byteLength).getInt32(offset);
}

function childBox(data, start, end, wanted) {
  for (let offset = start; offset + 8 <= end;) {
    const size = uint32(data, offset);
    assert.ok(size >= 8 && offset + size <= end, `invalid MP4 box at ${offset}`);
    if (boxType(data, offset) === wanted) return { start: offset, end: offset + size };
    offset += size;
  }
  return null;
}

function assertNoLeadingPicturesAfterRap(segments) {
  let latestRapPts = null;
  let rapCount = 0;
  const presentationPts = [];
  const sampleDurations = [];
  for (const data of segments) {
    const moof = childBox(data, 0, data.byteLength, 'moof');
    const traf = moof && childBox(data, moof.start + 8, moof.end, 'traf');
    const tfdt = traf && childBox(data, traf.start + 8, traf.end, 'tfdt');
    const trun = traf && childBox(data, traf.start + 8, traf.end, 'trun');
    assert.ok(tfdt && trun, 'video fragment is missing tfdt/trun');
    const tfdtVersion = data[tfdt.start + 8];
    const baseDts = tfdtVersion === 1
      ? uint32(data, tfdt.start + 12) * 0x100000000 + uint32(data, tfdt.start + 16)
      : uint32(data, tfdt.start + 12);
    const trunVersion = data[trun.start + 8];
    const flags = (data[trun.start + 9] << 16) | (data[trun.start + 10] << 8) |
      data[trun.start + 11];
    const sampleCount = uint32(data, trun.start + 12);
    let cursor = trun.start + 16;
    if (flags & 0x000001) cursor += 4;
    if (flags & 0x000004) cursor += 4;
    let dts = baseDts;
    for (let index = 0; index < sampleCount; index += 1) {
      const duration = flags & 0x000100 ? uint32(data, cursor) : 0;
      if (flags & 0x000100) cursor += 4;
      if (flags & 0x000200) cursor += 4;
      const sampleFlags = flags & 0x000400 ? uint32(data, cursor) : 0;
      if (flags & 0x000400) cursor += 4;
      const compositionOffset = flags & 0x000800
        ? (trunVersion === 1 ? int32(data, cursor) : uint32(data, cursor)) : 0;
      if (flags & 0x000800) cursor += 4;
      const pts = dts + compositionOffset;
      presentationPts.push(pts);
      if (duration > 0) sampleDurations.push(duration);
      const keyframe = (sampleFlags & 0x00010000) === 0;
      if (keyframe) {
        latestRapPts = pts;
        rapCount += 1;
      } else if (latestRapPts !== null) {
        assert.ok(pts >= latestRapPts,
          `leading picture PTS ${pts} follows later RAP PTS ${latestRapPts}`);
      }
      dts += duration;
    }
  }
  assert.ok(rapCount >= 1, 'sample did not contain an initial HEVC RAP');
  presentationPts.sort((left, right) => left - right);
  sampleDurations.sort((left, right) => left - right);
  const nominalDuration = sampleDurations[Math.floor(sampleDurations.length / 2)];
  for (let index = 1; index < presentationPts.length; index += 1) {
    const gap = presentationPts[index] - presentationPts[index - 1];
    // VideoToolbox treats each CRA as a decoder restart, so the RASL pictures
    // associated with it cannot safely be submitted through MSE on macOS.
    // Seven leading pictures plus the normal frame interval form this gap.
    assert.ok(gap <= nominalDuration * 9 + 1,
      `video presentation gap ${gap} at ${presentationPts[index - 1]} -> ` +
      `${presentationPts[index]} exceeds the CRA recovery window (${nominalDuration * 9})`);
  }
}

assert.deepEqual(fatalErrors, []);
for (const type of ['video', 'audio']) {
  const init = initSegments.get(type);
  assert.ok(init, `missing ${type} init segment`);
  assert.equal(boxType(init.data), 'ftyp');
  assert.ok(init.data.some((_, index) => index + 8 <= init.data.length && boxType(init.data, index) === 'moov'),
    `missing ${type} moov`);
  assert.ok(mediaSegments.get(type).length > 0, `missing ${type} media segment`);
  assert.equal(boxType(mediaSegments.get(type)[0]), 'moof');
}
assertNoLeadingPicturesAfterRap(mediaSegments.get('video'));

if (outputDirectory) {
  await mkdir(outputDirectory, { recursive: true });
  for (const type of ['video', 'audio']) {
    const init = initSegments.get(type).data;
    const segments = mediaSegments.get(type);
    const size = init.byteLength + segments.reduce((sum, segment) => sum + segment.byteLength, 0);
    const output = new Uint8Array(size);
    output.set(init, 0);
    let offset = init.byteLength;
    for (const segment of segments) {
      output.set(segment, offset);
      offset += segment.byteLength;
    }
    await writeFile(`${outputDirectory}/${type}.mp4`, output);
  }
}

console.log(JSON.stringify({
  bytesRead: position,
  video: {
    mime: initSegments.get('video').mime,
    width: initSegments.get('video').width,
    height: initSegments.get('video').height,
    segments: mediaSegments.get('video').length,
  },
  audio: {
    mime: initSegments.get('audio').mime,
    sampleRate: initSegments.get('audio').sampleRate,
    channels: initSegments.get('audio').channels,
    segments: mediaSegments.get('audio').length,
  },
}, null, 2));
