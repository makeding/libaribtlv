/*
 * Minimal HEVC/AAC fragmented-MP4 muxer for the tlvdemux demo.
 * The box layout and codec parsing follow ISO BMFF, HEVC and LATM. The
 * implementation was informed by the Apache-2.0 mmts.js remuxer without
 * importing its player or demux state machine. See NOTICE.md.
 */

const textEncoder = new TextEncoder();

function bytes(...parts) {
  const size = parts.reduce((sum, part) => sum + part.byteLength, 0);
  const output = new Uint8Array(size);
  let offset = 0;
  for (const part of parts) {
    output.set(part, offset);
    offset += part.byteLength;
  }
  return output;
}

function ascii(value) { return textEncoder.encode(value); }

function u16(value) {
  return Uint8Array.of((value >>> 8) & 0xff, value & 0xff);
}

function u24(value) {
  return Uint8Array.of((value >>> 16) & 0xff, (value >>> 8) & 0xff, value & 0xff);
}

function u32(value) {
  return Uint8Array.of(
    Math.floor(value / 0x1000000) & 0xff,
    (value >>> 16) & 0xff,
    (value >>> 8) & 0xff,
    value & 0xff,
  );
}

function i32(value) { return u32(value >>> 0); }

function u64(value) {
  const bigint = BigInt(Math.max(0, Math.round(value)));
  return bytes(u32(Number(bigint >> 32n)), u32(Number(bigint & 0xffffffffn)));
}

function box(type, ...payloads) {
  const payload = bytes(...payloads);
  return bytes(u32(payload.byteLength + 8), ascii(type), payload);
}

function fullBox(type, version, flags, ...payloads) {
  return box(type, Uint8Array.of(version), u24(flags), ...payloads);
}

function fixed16_16(value) { return u32(Math.round(value * 65536)); }

const UNITY_MATRIX = bytes(
  fixed16_16(1), u32(0), u32(0),
  u32(0), fixed16_16(1), u32(0),
  u32(0), u32(0), u32(0x40000000),
);

function ftyp() {
  return box('ftyp', ascii('iso6'), u32(1), ascii('iso6'), ascii('mp41'), ascii('dash'));
}

function mvhd(timescale) {
  return fullBox('mvhd', 0, 0,
    u32(0), u32(0), u32(timescale), u32(0),
    fixed16_16(1), u16(0x0100), new Uint8Array(10), UNITY_MATRIX,
    new Uint8Array(24), u32(2));
}

function tkhd(track) {
  return fullBox('tkhd', 0, 7,
    u32(0), u32(0), u32(track.id), u32(0), u32(0),
    new Uint8Array(8), u16(0), u16(0),
    u16(track.type === 'audio' ? 0x0100 : 0), u16(0), UNITY_MATRIX,
    fixed16_16(track.width || 0), fixed16_16(track.height || 0));
}

function mdhd(track) {
  return fullBox('mdhd', 0, 0,
    u32(0), u32(0), u32(track.timescale), u32(0), u16(0x55c4), u16(0));
}

function hdlr(type) {
  const video = type === 'video';
  const name = ascii(video ? 'tlvdemux video\0' : 'tlvdemux audio\0');
  return fullBox('hdlr', 0, 0, u32(0), ascii(video ? 'vide' : 'soun'), new Uint8Array(12), name);
}

function dinf() {
  return box('dinf', box('dref', Uint8Array.of(0, 0, 0, 0), u32(1), fullBox('url ', 0, 1)));
}

function videoSampleEntry(track) {
  const compressor = new Uint8Array(32);
  compressor[0] = 8;
  compressor.set(ascii('tlvdemux'), 1);
  const header = bytes(
    new Uint8Array(6), u16(1), new Uint8Array(16),
    u16(track.width), u16(track.height), fixed16_16(72), fixed16_16(72),
    u32(0), u16(1), compressor, u16(24), u16(0xffff),
  );
  return box('hvc1', header, box('hvcC', track.hvcc));
}

function descriptor(tag, payload) {
  if (payload.byteLength >= 128) throw new Error('demo descriptor is too large');
  return bytes(Uint8Array.of(tag, payload.byteLength), payload);
}

function audioSampleEntry(track) {
  const decoderSpecific = descriptor(0x05, track.asc);
  const decoderConfig = descriptor(0x04, bytes(
    Uint8Array.of(0x40, 0x15), u24(0), u32(0), u32(0), decoderSpecific,
  ));
  const esDescriptor = descriptor(0x03, bytes(u16(track.id), Uint8Array.of(0), decoderConfig,
                                              descriptor(0x06, Uint8Array.of(2))));
  const esds = fullBox('esds', 0, 0, esDescriptor);
  const header = bytes(
    new Uint8Array(6), u16(1), new Uint8Array(8),
    u16(track.channels), u16(16), u16(0), u16(0), u32(track.sampleRate << 16),
  );
  return box('mp4a', header, esds);
}

function stbl(track) {
  const entry = track.type === 'video' ? videoSampleEntry(track) : audioSampleEntry(track);
  return box('stbl',
    fullBox('stsd', 0, 0, u32(1), entry),
    fullBox('stts', 0, 0, u32(0)),
    fullBox('stsc', 0, 0, u32(0)),
    fullBox('stsz', 0, 0, u32(0), u32(0)),
    fullBox('stco', 0, 0, u32(0)));
}

function minf(track) {
  const mediaHeader = track.type === 'video'
    ? fullBox('vmhd', 0, 1, u16(0), u16(0), u16(0), u16(0))
    : fullBox('smhd', 0, 0, u16(0), u16(0));
  return box('minf', mediaHeader, dinf(), stbl(track));
}

function moov(track) {
  const trak = box('trak', tkhd(track), box('mdia', mdhd(track), hdlr(track.type), minf(track)));
  const trex = fullBox('trex', 0, 0, u32(track.id), u32(1), u32(0), u32(0), u32(0));
  return box('moov', mvhd(track.timescale), trak, box('mvex', trex));
}

function initSegment(track) { return bytes(ftyp(), moov(track)); }

function sampleFlags(sample) {
  return sample.keyframe ? 0x02000000 : 0x01010000;
}

function mediaSegment(track, samples, baseDts, sequence) {
  const payload = bytes(...samples.map(sample => sample.data));
  const trunEntries = [];
  for (const sample of samples) {
    trunEntries.push(u32(sample.duration), u32(sample.data.byteLength),
                     u32(sampleFlags(sample)), i32(sample.pts - sample.dts));
  }
  const tfhd = fullBox('tfhd', 0, 0x020000, u32(track.id));
  const tfdt = fullBox('tfdt', 1, 0, u64(baseDts));
  const trunPayloadSize = 12 + samples.length * 16;
  const dataOffset = 8 + 16 + 8 + tfhd.byteLength + tfdt.byteLength +
    8 + trunPayloadSize + 8;
  const trun = fullBox('trun', 1, 0x000f01,
    u32(samples.length), i32(dataOffset), ...trunEntries);
  const moof = box('moof', fullBox('mfhd', 0, 0, u32(sequence)), box('traf', tfhd, tfdt, trun));
  return bytes(moof, box('mdat', payload));
}

class BitReader {
  constructor(data) { this.data = data; this.offset = 0; }
  readBits(count) {
    if (count < 0 || this.offset + count > this.data.byteLength * 8) throw new Error('truncated bitstream');
    let value = 0;
    for (let i = 0; i < count; i += 1) {
      value = value * 2 + ((this.data[this.offset >> 3] >> (7 - (this.offset & 7))) & 1);
      this.offset += 1;
    }
    return value;
  }
  readBool() { return this.readBits(1) !== 0; }
  readUE() {
    let zeros = 0;
    while (!this.readBool()) {
      zeros += 1;
      if (zeros > 31) throw new Error('invalid Exp-Golomb value');
    }
    return (2 ** zeros - 1) + (zeros ? this.readBits(zeros) : 0);
  }
  readBytesBitwise(length) {
    const output = new Uint8Array(length);
    for (let i = 0; i < length; i += 1) output[i] = this.readBits(8);
    return output;
  }
}

function rbsp(nalu) {
  const output = [];
  for (let i = 0; i < nalu.byteLength; i += 1) {
    if (i >= 2 && nalu[i] === 3 && nalu[i - 1] === 0 && nalu[i - 2] === 0) continue;
    output.push(nalu[i]);
  }
  return Uint8Array.from(output);
}

function reverseByte(value) {
  let result = 0;
  for (let i = 0; i < 8; i += 1) result |= ((value >> (7 - i)) & 1) << i;
  return result;
}

function parseSps(nalu) {
  const reader = new BitReader(rbsp(nalu));
  reader.readBits(16);
  reader.readBits(4);
  const maxSubLayersMinus1 = reader.readBits(3);
  const temporalIdNested = reader.readBool();
  const profileSpace = reader.readBits(2);
  const tier = reader.readBits(1);
  const profileIdc = reader.readBits(5);
  const compatibility = [reader.readBits(8), reader.readBits(8), reader.readBits(8), reader.readBits(8)];
  const constraints = Array.from({ length: 6 }, () => reader.readBits(8));
  const levelIdc = reader.readBits(8);
  const subProfile = [];
  const subLevel = [];
  for (let i = 0; i < maxSubLayersMinus1; i += 1) {
    subProfile.push(reader.readBool());
    subLevel.push(reader.readBool());
  }
  if (maxSubLayersMinus1 > 0) reader.readBits((8 - maxSubLayersMinus1) * 2);
  for (let i = 0; i < maxSubLayersMinus1; i += 1) {
    if (subProfile[i]) reader.readBits(88);
    if (subLevel[i]) reader.readBits(8);
  }
  reader.readUE();
  const chromaFormat = reader.readUE();
  if (chromaFormat === 3) reader.readBits(1);
  const codedWidth = reader.readUE();
  const codedHeight = reader.readUE();
  let left = 0, right = 0, top = 0, bottom = 0;
  if (reader.readBool()) {
    left = reader.readUE(); right = reader.readUE(); top = reader.readUE(); bottom = reader.readUE();
  }
  const bitDepthLumaMinus8 = reader.readUE();
  const bitDepthChromaMinus8 = reader.readUE();
  const subWidth = chromaFormat === 1 || chromaFormat === 2 ? 2 : 1;
  const subHeight = chromaFormat === 1 ? 2 : 1;
  const compatibilityValue = compatibility.reduce(
    (value, byte, index) => (value | (reverseByte(byte) << (index * 8))) >>> 0, 0);
  const profilePrefix = ['', 'A', 'B', 'C'][profileSpace];
  let codec = `hvc1.${profilePrefix}${profileIdc}.${compatibilityValue.toString(16).toUpperCase()}.${tier ? 'H' : 'L'}${levelIdc}`;
  let lastConstraint = constraints.length - 1;
  while (lastConstraint >= 0 && constraints[lastConstraint] === 0) lastConstraint -= 1;
  for (let i = 0; i <= lastConstraint; i += 1) codec += `.${constraints[i].toString(16).padStart(2, '0').toUpperCase()}`;
  return {
    width: codedWidth - subWidth * (left + right),
    height: codedHeight - subHeight * (top + bottom),
    profileSpace, tier, profileIdc, compatibility, constraints, levelIdc,
    chromaFormat, bitDepthLumaMinus8, bitDepthChromaMinus8,
    temporalLayers: maxSubLayersMinus1 + 1, temporalIdNested, codec,
  };
}

function makeHvcc(vps, sps, pps, detail) {
  const header = new Uint8Array(23);
  header[0] = 1;
  header[1] = (detail.profileSpace << 6) | (detail.tier << 5) | detail.profileIdc;
  header.set(detail.compatibility, 2);
  header.set(detail.constraints, 6);
  header[12] = detail.levelIdc;
  header[13] = 0xf0; header[14] = 0;
  header[15] = 0xfc;
  header[16] = 0xfc | detail.chromaFormat;
  header[17] = 0xf8 | detail.bitDepthLumaMinus8;
  header[18] = 0xf8 | detail.bitDepthChromaMinus8;
  header[21] = (detail.temporalLayers << 3) | (detail.temporalIdNested ? 4 : 0) | 3;
  header[22] = 3;
  const array = (type, nalu) => bytes(Uint8Array.of(0x80 | type, 0, 1), u16(nalu.byteLength), nalu);
  return bytes(header, array(32, vps), array(33, sps), array(34, pps));
}

function annexBNalus(data) {
  const starts = [];
  for (let i = 0; i + 3 < data.byteLength; i += 1) {
    if (data[i] === 0 && data[i + 1] === 0 && data[i + 2] === 1) {
      starts.push({ start: i + 3, code: i }); i += 2;
    } else if (i + 4 < data.byteLength && data[i] === 0 && data[i + 1] === 0 &&
               data[i + 2] === 0 && data[i + 3] === 1) {
      starts.push({ start: i + 4, code: i }); i += 3;
    }
  }
  return starts.map((entry, index) => {
    const end = index + 1 < starts.length ? starts[index + 1].code : data.byteLength;
    const nalu = data.subarray(entry.start, end);
    return { type: nalu.byteLength ? (nalu[0] >> 1) & 0x3f : -1, data: nalu };
  }).filter(entry => entry.data.byteLength >= 2);
}

function scaledTimestamp(value, timescale, target) {
  return Math.round(Number(value) * target / timescale);
}

class BaseMuxer {
  constructor(type, callbacks) {
    this.type = type;
    this.callbacks = callbacks;
    this.track = null;
    this.pending = null;
    this.ready = [];
    this.readyDuration = 0;
    this.sequence = 1;
    this.lastDuration = 0;
  }
  setTrack(track) {
    if (this.track) return;
    this.track = track;
    this.emitInit();
  }
  emitInit() {
    if (!this.track) return;
    const track = this.track;
    this.callbacks.onInit(this.type, {
      mime: `${this.type}/mp4; codecs="${track.codec}"`,
      data: initSegment(track),
      width: track.width,
      height: track.height,
      sampleRate: track.sampleRate,
      channels: track.channels,
    });
  }
  activate() {
    this.resetForDiscontinuity();
    this.emitInit();
  }
  resetForDiscontinuity() {
    this.pending = null;
    this.ready = [];
    this.readyDuration = 0;
    this.lastDuration = 0;
  }
  enqueue(sample) {
    if (this.pending) {
      const duration = sample.dts - this.pending.dts;
      this.pending.duration = duration > 0 ? duration : (this.lastDuration || this.defaultDuration());
      this.lastDuration = this.pending.duration;
      this.ready.push(this.pending);
      this.readyDuration += this.pending.duration;
    }
    this.pending = sample;
    if (this.readyDuration >= this.track.timescale) this.emit();
  }
  emit() {
    if (!this.ready.length || !this.track) return;
    const samples = this.ready;
    this.ready = [];
    this.readyDuration = 0;
    this.callbacks.onSegment(this.type,
      mediaSegment(this.track, samples, samples[0].dts, this.sequence++));
  }
  flush() {
    if (this.pending) {
      this.pending.duration = this.lastDuration || this.defaultDuration();
      this.ready.push(this.pending);
      this.pending = null;
    }
    this.emit();
  }
}

export class HevcMuxer extends BaseMuxer {
  constructor(callbacks) {
    super('video', callbacks);
    this.parameterSets = new Map();
    this.started = false;
    this.dropRasl = false;
  }
  hasStarted() { return this.started; }
  defaultDuration() { return 33367; }
  push(unit) {
    if (unit.discontinuity) {
      this.resetForDiscontinuity();
      this.started = false;
      this.dropRasl = false;
    }
    const nalus = annexBNalus(unit.data);
    for (const nalu of nalus) if (nalu.type >= 32 && nalu.type <= 34) this.parameterSets.set(nalu.type, nalu.data.slice());
    if (!this.track && [32, 33, 34].every(type => this.parameterSets.has(type))) {
      const detail = parseSps(this.parameterSets.get(33));
      this.setTrack({
        id: 1, type: 'video', timescale: 1000000,
        width: detail.width, height: detail.height, codec: detail.codec,
        hvcc: makeHvcc(this.parameterSets.get(32), this.parameterSets.get(33),
                       this.parameterSets.get(34), detail),
      });
    }
    if (!this.track) return;
    const vclNalus = nalus.filter(nalu => nalu.type >= 0 && nalu.type <= 31);
    if (!vclNalus.length) return;
    const irapType = vclNalus.find(nalu => nalu.type >= 16 && nalu.type <= 21)?.type;
    if (!this.started) {
      if (irapType === undefined) return;
      this.started = true;
      this.dropRasl = irapType === 21;
      this.callbacks.onStart?.('video', { nalType: irapType, signalledRandomAccess: unit.randomAccess });
    } else if (this.dropRasl) {
      if (vclNalus.some(nalu => nalu.type === 8 || nalu.type === 9)) return;
      this.dropRasl = false;
    }
    const mediaNalus = nalus.filter(nalu => nalu.type !== 32 && nalu.type !== 33 && nalu.type !== 34 && nalu.type !== 35);
    if (!mediaNalus.length) return;
    const data = bytes(...mediaNalus.map(nalu => bytes(u32(nalu.data.byteLength), nalu.data)));
    this.enqueue({
      data,
      dts: scaledTimestamp(unit.dtsValue, unit.dtsTimescale, this.track.timescale),
      pts: scaledTimestamp(unit.ptsValue, unit.ptsTimescale, this.track.timescale),
      keyframe: irapType !== undefined,
    });
  }
}

const AAC_SAMPLE_RATES = [96000, 88200, 64000, 48000, 44100, 32000, 24000,
  22050, 16000, 12000, 11025, 8000, 7350];

function latmValue(reader) {
  const bytesForValue = reader.readBits(2);
  let value = 0;
  for (let i = 0; i <= bytesForValue; i += 1) value = value * 256 + reader.readBits(8);
  return value;
}

class LatmParser {
  constructor() { this.config = null; }
  parse(data) {
    if (data.byteLength < 4 || data[0] !== 0x56 || (data[1] & 0xe0) !== 0xe0) throw new Error('invalid LOAS frame');
    const length = ((data[1] & 0x1f) << 8) | data[2];
    if (length + 3 > data.byteLength) throw new Error('truncated LOAS frame');
    const reader = new BitReader(data.subarray(3, 3 + length));
    const same = reader.readBool();
    if (!same) {
      const version = reader.readBool();
      const versionA = version && reader.readBool();
      if (versionA) throw new Error('LATM audioMuxVersionA is unsupported');
      if (version) latmValue(reader);
      if (!reader.readBool() || reader.readBits(6) !== 0 || reader.readBits(4) !== 0 || reader.readBits(3) !== 0) {
        throw new Error('unsupported LATM multiplex layout');
      }
      const ascLength = version ? latmValue(reader) : null;
      const ascStart = reader.offset;
      let objectType = reader.readBits(5);
      if (objectType === 31) objectType = 32 + reader.readBits(6);
      let sampleRateIndex = reader.readBits(4);
      let sampleRate;
      if (sampleRateIndex === 15) sampleRate = reader.readBits(24);
      else sampleRate = AAC_SAMPLE_RATES[sampleRateIndex];
      const channels = reader.readBits(4);
      reader.readBits(1);
      if (reader.readBool()) reader.readBits(14);
      reader.readBits(1);
      if (version && ascLength > reader.offset - ascStart) reader.readBits(ascLength - (reader.offset - ascStart));
      const frameLengthType = reader.readBits(3);
      if (frameLengthType !== 0) throw new Error(`LATM frameLengthType=${frameLengthType} is unsupported`);
      reader.readBits(8);
      const otherData = reader.readBool();
      if (otherData) {
        let more;
        do { more = reader.readBool(); reader.readBits(version ? latmValue(reader) : 8); } while (more);
      }
      if (reader.readBool()) reader.readBits(8);
      if (!sampleRate || !channels || objectType >= 32 || sampleRateIndex >= 15) {
        throw new Error('unsupported AAC AudioSpecificConfig');
      }
      const asc = Uint8Array.of((objectType << 3) | (sampleRateIndex >> 1),
                                ((sampleRateIndex & 1) << 7) | (channels << 3));
      this.config = { objectType, sampleRateIndex, sampleRate, channels, asc, frameLengthType };
    }
    if (!this.config) throw new Error('LATM useSameStreamMux arrived before configuration');
    let payloadLength = 0;
    let part;
    do { part = reader.readBits(8); payloadLength += part; } while (part === 255);
    return { ...this.config, data: reader.readBytesBitwise(payloadLength) };
  }
}

export class AacMuxer extends BaseMuxer {
  constructor(callbacks) {
    super('audio', callbacks);
    this.parser = new LatmParser();
  }
  defaultDuration() { return this.track ? Math.round(1024 * this.track.timescale / this.track.sampleRate) : 21333; }
  push(unit, enabled = true) {
    if (unit.discontinuity) this.resetForDiscontinuity();
    const frame = this.parser.parse(unit.data);
    if (!this.track) {
      this.setTrack({
        id: 1, type: 'audio', timescale: frame.sampleRate,
        sampleRate: frame.sampleRate, channels: frame.channels,
        asc: frame.asc, codec: `mp4a.40.${frame.objectType}`,
      });
    }
    if (!enabled) return;
    const timestamp = scaledTimestamp(unit.ptsValue, unit.ptsTimescale, this.track.timescale);
    this.enqueue({ data: frame.data.slice(), dts: timestamp, pts: timestamp, keyframe: true });
  }
}
