import assert from 'node:assert/strict';
import { open } from 'node:fs/promises';
import { createRequire } from 'node:module';

const [modulePath, mediaPath, maximumAccessUnitsArgument] = process.argv.slice(2);
assert.ok(modulePath && mediaPath,
  'usage: node tests/wasm_hevc_timing_diagnostics.mjs TLVDEMUX_JS SAMPLE [MAX_AU]');
const maximumAccessUnits = maximumAccessUnitsArgument === undefined
  ? Number.POSITIVE_INFINITY
  : Number.parseInt(maximumAccessUnitsArgument, 10);
assert.ok(maximumAccessUnits > 0, 'MAX_AU must be positive');

function nalUnits(data) {
  const starts = [];
  for (let offset = 0; offset + 3 < data.byteLength;) {
    if (data[offset] === 0 && data[offset + 1] === 0 && data[offset + 2] === 1) {
      starts.push({ code: offset, data: offset + 3 });
      offset += 3;
    } else if (offset + 4 < data.byteLength && data[offset] === 0 && data[offset + 1] === 0 &&
               data[offset + 2] === 0 && data[offset + 3] === 1) {
      starts.push({ code: offset, data: offset + 4 });
      offset += 4;
    } else offset += 1;
  }
  const units = [];
  for (let index = 0; index < starts.length; index += 1) {
    const start = starts[index].data;
    const end = index + 1 < starts.length ? starts[index + 1].code : data.byteLength;
    if (end - start < 2) continue;
    const nalu = data.subarray(start, end);
    const first = nalu[0];
    const second = nalu[1];
    const type = (first >> 1) & 0x3f;
    const rbsp = [];
    for (let cursor = 2, zeros = 0; cursor < nalu.length; cursor += 1) {
      if (zeros >= 2 && nalu[cursor] === 3) { zeros = 0; continue; }
      rbsp.push(nalu[cursor]);
      zeros = nalu[cursor] === 0 ? zeros + 1 : 0;
    }
    let bit = 0;
    const readBit = () => (rbsp[bit >> 3] >> (7 - (bit++ & 7))) & 1;
    const readUe = () => {
      let zeros = 0;
      while (readBit() === 0) zeros += 1;
      let value = 1;
      while (zeros-- > 0) value = (value << 1) | readBit();
      return value - 1;
    };
    let ppsId = null;
    if (type === 34) ppsId = readUe();
    else if (type <= 31) {
      readBit();
      if (type >= 16 && type <= 23) readBit();
      ppsId = readUe();
    }
    let hash = 2166136261;
    for (const byte of nalu) hash = Math.imul(hash ^ byte, 16777619) >>> 0;
    let payloadHash = 2166136261;
    for (const byte of nalu.subarray(2)) {
      payloadHash = Math.imul(payloadHash ^ byte, 16777619) >>> 0;
    }
    units.push({
      type,
      layer: ((first & 1) << 5) | (second >> 3),
      temporalId: (second & 7) - 1,
      ppsId,
      hash: hash.toString(16).padStart(8, '0'),
      payloadHash: payloadHash.toString(16).padStart(8, '0'),
      ...(type === 34 ? { hex: Buffer.from(nalu).toString('hex') } : {}),
    });
  }
  return units;
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
    if (unit.trackId !== selectedVideo || units.length >= maximumAccessUnits) return;
    const nalus = nalUnits(unit.data);
    const types = nalus.map(nalu => nalu.type);
    if (!types.some(type => type <= 31)) return;
    units.push({
      dts: Number(unit.dtsValue) / unit.dtsTimescale,
      pts: Number(unit.ptsValue) / unit.ptsTimescale,
      types,
      nalus,
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
    if (units.length >= maximumAccessUnits) break;
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
  firstAccessUnits: units.slice(0, 40).map(unit => ({
    dts: unit.dts,
    pts: unit.pts,
    nalus: unit.nalus.map(({ type, temporalId, ppsId, hash, payloadHash, hex }) =>
      ({ type, temporalId, ppsId, hash, payloadHash, hex })),
  })),
  accessUnitLayouts: [...new Set(units.map(unit => JSON.stringify(unit.nalus)))].map(JSON.parse),
}, null, 2));
