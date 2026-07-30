import assert from 'node:assert/strict';
import { open } from 'node:fs/promises';
import { createRequire } from 'node:module';

const [modulePath, mediaPath] = process.argv.slice(2);
assert.ok(modulePath && mediaPath,
  'usage: node tests/wasm_hevc_timing_diagnostics.mjs TLVDEMUX_JS SAMPLE');

function nalTypes(data) {
  const types = [];
  for (let offset = 0; offset + 5 < data.byteLength;) {
    let prefix = 0;
    if (data[offset] === 0 && data[offset + 1] === 0 && data[offset + 2] === 1) prefix = 3;
    else if (data[offset] === 0 && data[offset + 1] === 0 &&
             data[offset + 2] === 0 && data[offset + 3] === 1) prefix = 4;
    if (!prefix) {
      offset += 1;
      continue;
    }
    types.push((data[offset + prefix] >> 1) & 0x3f);
    offset += prefix + 2;
  }
  return types;
}

const require = createRequire(import.meta.url);
const createTlvDemuxModule = require(modulePath);
const module = await createTlvDemuxModule();
const units = [];
let selectedVideo = null;
let demuxer;
demuxer = new module.TlvDemuxer({
  onTrack(track) {
    if (track.kind === 'video' && selectedVideo === null) {
      selectedVideo = track.trackId;
      demuxer.selectTrack('video', selectedVideo);
    }
  },
  onAccessUnitView(unit) {
    if (unit.trackId !== selectedVideo) return;
    const types = nalTypes(unit.data);
    if (!types.some(type => type <= 31)) return;
    units.push({
      dts: Number(unit.dtsValue) / unit.dtsTimescale,
      pts: Number(unit.ptsValue) / unit.ptsTimescale,
      types,
    });
  },
});

const file = await open(mediaPath, 'r');
const chunk = new Uint8Array(2 * 1024 * 1024);
let position = 0;
try {
  for (;;) {
    const { bytesRead } = await file.read(chunk, 0, chunk.byteLength, position);
    if (!bytesRead) break;
    assert.equal(demuxer.push(chunk.subarray(0, bytesRead)), true);
    position += bytesRead;
  }
  demuxer.flush();
} finally {
  await file.close();
  demuxer.delete();
}

let dropRasl = false;
let started = false;
let currentCra = null;
const craGroups = [];
const emitted = [];
for (const unit of units) {
  const irap = unit.types.find(type => type >= 16 && type <= 21);
  const rasl = unit.types.includes(8) || unit.types.includes(9);
  if (!started) {
    if (irap === undefined) continue;
    started = true;
  } else if (dropRasl) {
    if (rasl) {
      currentCra.dropped.push(unit);
      continue;
    }
    dropRasl = false;
  }
  if (irap === 21) {
    currentCra = { dts: unit.dts, pts: unit.pts, dropped: [] };
    craGroups.push(currentCra);
    dropRasl = true;
  }
  emitted.push(unit);
}

const presentation = emitted.toSorted((left, right) => left.pts - right.pts);
const gaps = [];
for (let index = 1; index < presentation.length; index += 1) {
  const gap = presentation[index].pts - presentation[index - 1].pts;
  if (gap > 0.025) gaps.push({ at: presentation[index - 1].pts, gap });
}

console.log(JSON.stringify({
  bytesRead: position,
  accessUnits: units.length,
  duration: units.length ? units.at(-1).dts - units[0].dts : 0,
  cra: craGroups.map(group => ({
    dts: group.dts,
    pts: group.pts,
    raslDropped: group.dropped.length,
    droppedPts: group.dropped.map(unit => unit.pts),
  })),
  largestPresentationGaps: gaps.toSorted((left, right) => right.gap - left.gap).slice(0, 20),
}, null, 2));
