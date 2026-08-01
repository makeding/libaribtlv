import assert from 'node:assert/strict';
import { mkdir, open, writeFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { resolve } from 'node:path';

const [modulePath, mediaPath, outputDirectory] = process.argv.slice(2);
assert.ok(modulePath && mediaPath,
  'usage: node tests/demo_playback_smoke.mjs TLVDEMUX_JS SAMPLE');

const require = createRequire(import.meta.url);
const createTlvDemuxModule = require(resolve(modulePath));
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
