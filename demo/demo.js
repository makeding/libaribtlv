import { B62TTMLRenderer } from '/aribb62.js/src/index.js';
import { DataBroadcastController } from './data-broadcast.js?v=mh-eit-v1';

const MiB = 1024n * 1024n;
const PLAYBACK_CHUNK = 2n * MiB;
const FORWARD_BUFFER_HIGH_SECONDS = 15;
const FORWARD_BUFFER_LOW_SECONDS = 8;
const BACK_BUFFER_SECONDS = 8;
const SOURCE_QUEUE_HIGH_BYTES = 4 * 1024 * 1024;
const MIN_SEEK_PREROLL_BYTES = 16n * MiB;
const MAX_SEEK_PREROLL_BYTES = 128n * MiB;
const SEEK_PREROLL_US = 8000000n;
const SEEK_PROBE_BYTES = 64n * MiB;
const MAX_SEEK_PROBE_ATTEMPTS = 5;
const DEFAULT_PLAYBACK_RATE = 2;
const LIVE_PLAYBACK_RATE = 1;
const URL_STORAGE_KEY = 'tlvdemux.demo.httpUrl';
const AUDIO_STORAGE_KEY = 'tlvdemux.demo.audioPacketId';
const SUBTITLE_STORAGE_KEY = 'tlvdemux.demo.subtitlePacketId';
const elements = Object.fromEntries([
  'wasmStatus', 'fileInput', 'urlInput', 'initialRange', 'maxRange',
  'videoPacketId', 'probeButton', 'cancelButton', 'clearButton',
  'probeState', 'duration', 'sourceSize', 'transferred', 'log',
  'video', 'mediaInfo', 'liveMode', 'audioTrack', 'subtitleTrack', 'subtitleOverlay',
  'broadcastViewport', 'broadcastVideoSurface', 'broadcastMediaPlane', 'broadcastFrame', 'dataRemote',
  'dataStatus', 'dataDetail', 'dataUrl',
].map(id => [id, document.getElementById(id)]));

const dataBroadcast = new DataBroadcastController({
  viewport: elements.broadcastViewport,
  videoSurface: elements.broadcastVideoSurface,
  mediaPlane: elements.broadcastMediaPlane,
  video: elements.video,
  iframe: elements.broadcastFrame,
  remote: elements.dataRemote,
  status: elements.dataStatus,
  detail: elements.dataDetail,
  url: elements.dataUrl,
});
dataBroadcast.setLogger(appendLog);

let wasmModule = null;
let activeProbe = null;
let activeDemuxer = null;
let activeApplicationDrainState = null;
let activeController = null;
let activeMediaSource = null;
let activeObjectUrl = null;
let activeQueues = [];
let activeQueueByType = new Map();
let runGeneration = 0;
let cachedProbe = null;
let seekTimer = null;
let internalSeekTarget = null;
let currentLiveMode = false;
let activeAudioSwitch = null;
let activeSubtitleSwitch = null;
let activeSubtitleRenderer = null;
let selectedAudioPacketId = null;
let preferredAudioPacketId = null;
let knownAudioTracks = new Map();
let selectedSubtitlePacketId = null;
let preferredSubtitlePacketId = null;
let knownSubtitleTracks = new Map();
let playbackQualityTimer = null;

elements.video.defaultPlaybackRate = DEFAULT_PLAYBACK_RATE;
elements.video.playbackRate = DEFAULT_PLAYBACK_RATE;

try {
  const savedUrl = localStorage.getItem(URL_STORAGE_KEY);
  if (savedUrl !== null) elements.urlInput.value = savedUrl;
  const savedAudio = localStorage.getItem(AUDIO_STORAGE_KEY);
  if (savedAudio !== null && /^\d+$/.test(savedAudio)) preferredAudioPacketId = Number(savedAudio);
  const savedSubtitle = localStorage.getItem(SUBTITLE_STORAGE_KEY);
  if (savedSubtitle !== null && /^\d+$/.test(savedSubtitle)) {
    preferredSubtitlePacketId = Number(savedSubtitle);
  }
} catch (_) {
  // localStorage may be unavailable for restricted or opaque origins.
}

elements.urlInput.addEventListener('input', () => {
  try { localStorage.setItem(URL_STORAGE_KEY, elements.urlInput.value); }
  catch (_) { /* Keep the demo usable when storage is unavailable. */ }
});

const AUDIO_LAYOUTS = [
  '不明', 'モノラル', 'デュアルモノ', 'ステレオ', '2.1ch', '3.0ch', '2.2ch',
  '4.0ch', '5.0ch', '5.1ch', '3.3.1ch', '6.1ch', '7.1ch', '10.2ch', '22.2ch',
];

function audioTrackLabel(track) {
  const parts = [`0x${track.packetId.toString(16)}`];
  if (track.language) parts.push(track.language);
  if (track.audio) {
    parts.push(AUDIO_LAYOUTS[track.audio.channelLayout] || `${track.audio.channelLayout}ch`);
    if (track.audio.sampleRate) parts.push(`${track.audio.sampleRate}Hz`);
    if (track.audio.mainComponent) parts.push('メイン');
    if (track.audio.multilingual) parts.push('二か国語');
  }
  return parts.join(' · ');
}

function renderAudioTracks() {
  elements.audioTrack.replaceChildren();
  const automatic = document.createElement('option');
  automatic.value = '';
  automatic.textContent = '自動';
  elements.audioTrack.append(automatic);
  for (const track of [...knownAudioTracks.values()].sort((a, b) => a.packetId - b.packetId)) {
    const option = document.createElement('option');
    option.value = String(track.packetId);
    option.textContent = audioTrackLabel(track);
    elements.audioTrack.append(option);
  }
  const desired = preferredAudioPacketId ?? selectedAudioPacketId;
  elements.audioTrack.value = desired !== null && knownAudioTracks.has(desired) ? String(desired) : '';
  elements.audioTrack.disabled = knownAudioTracks.size === 0;
}

function subtitleTrackLabel(track) {
  const parts = [`0x${track.packetId.toString(16)}`];
  if (track.language) parts.push(track.language);
  if (track.subtitle) {
    parts.push(`mode=${track.subtitle.operationMode}`);
    parts.push(`timing=${track.subtitle.timingMode}`);
  }
  return parts.join(' · ');
}

function renderSubtitleTracks() {
  elements.subtitleTrack.replaceChildren();
  const automatic = document.createElement('option');
  automatic.value = '';
  automatic.textContent = '自動';
  elements.subtitleTrack.append(automatic);
  for (const track of [...knownSubtitleTracks.values()].sort((a, b) => a.packetId - b.packetId)) {
    const option = document.createElement('option');
    option.value = String(track.packetId);
    option.textContent = subtitleTrackLabel(track);
    elements.subtitleTrack.append(option);
  }
  const desired = preferredSubtitlePacketId ?? selectedSubtitlePacketId;
  elements.subtitleTrack.value = desired !== null && knownSubtitleTracks.has(desired)
    ? String(desired) : '';
  elements.subtitleTrack.disabled = knownSubtitleTracks.size === 0;
}

function appendLog(message) {
  if (elements.log.textContent === '読み込み待ち…') elements.log.textContent = '';
  elements.log.textContent += `${message}\n`;
  elements.log.scrollTop = elements.log.scrollHeight;
}

function mediaErrorMessage(error = elements.video.error) {
  if (!error) return null;
  const names = { 1: '中断', 2: 'ネットワーク', 3: 'デコード', 4: '非対応ソース' };
  return `MediaError ${names[error.code] || error.code}${error.message ? `: ${error.message}` : ''}`;
}

class RangeUnsupportedError extends Error {}

function formatBytes(value) {
  const byteCount = typeof value === 'bigint' ? value : BigInt(value);
  if (byteCount < 1024n) return `${byteCount} B`;
  const units = ['KiB', 'MiB', 'GiB', 'TiB'];
  let scaled = Number(byteCount);
  let unit = -1;
  do { scaled /= 1024; unit += 1; } while (scaled >= 1024 && unit < units.length - 1);
  return `${scaled.toFixed(scaled >= 100 ? 0 : scaled >= 10 ? 1 : 2)} ${units[unit]}`;
}

function durationSeconds(duration) { return Number(duration.value) / duration.timescale; }

function seekPrerollBytes(sourceSize, durationUs) {
  if (durationUs <= 0n) return MIN_SEEK_PREROLL_BYTES;
  const estimated = sourceSize * SEEK_PREROLL_US / durationUs;
  return estimated < MIN_SEEK_PREROLL_BYTES ? MIN_SEEK_PREROLL_BYTES
    : estimated > MAX_SEEK_PREROLL_BYTES ? MAX_SEEK_PREROLL_BYTES : estimated;
}

function formatDuration(duration) {
  const seconds = durationSeconds(duration);
  const whole = Math.max(0, Math.floor(seconds));
  const hours = Math.floor(whole / 3600);
  const minutes = Math.floor((whole % 3600) / 60);
  const rest = whole % 60;
  const clock = hours > 0
    ? `${hours}:${String(minutes).padStart(2, '0')}:${String(rest).padStart(2, '0')}`
    : `${minutes}:${String(rest).padStart(2, '0')}`;
  return `${clock} (${seconds.toFixed(6)}s)`;
}

function toSafeNumber(value, label) {
  if (value < 0n || value > BigInt(Number.MAX_SAFE_INTEGER)) {
    throw new Error(`${label} がブラウザーの安全な整数範囲を超えています`);
  }
  return Number(value);
}

function parsePacketId() {
  const text = elements.videoPacketId.value.trim();
  if (!text) return undefined;
  const value = Number(text);
  if (!Number.isInteger(value) || value < 0 || value > 0xffff) {
    throw new Error('映像 packet_id は 0..0xffff で指定してください');
  }
  return value;
}

function localSource(file) {
  return {
    identity: file,
    label: file.name,
    size: BigInt(file.size),
    async read(offset, length) {
      const start = toSafeNumber(offset, 'Range 開始位置');
      const end = toSafeNumber(offset + length, 'Range 終了位置');
      return new Uint8Array(await file.slice(start, end).arrayBuffer());
    },
  };
}

function parseContentRange(value) {
  const match = /^bytes (\d+)-(\d+)\/(\d+)$/.exec(value || '');
  if (!match) return null;
  return { start: BigInt(match[1]), end: BigInt(match[2]), size: BigInt(match[3]) };
}

async function discoverRemoteSize(url, signal) {
  const response = await fetch(url, { headers: { Range: 'bytes=0-0' }, signal });
  const range = parseContentRange(response.headers.get('Content-Range'));
  if (response.status !== 206 || !range || range.start !== 0n || range.end !== 0n) {
    await response.body?.cancel();
    throw new RangeUnsupportedError('サーバーは HTTP Range に対応していません');
  }
  await response.arrayBuffer();
  return range.size;
}

async function remoteSource(rawUrl, signal) {
  const url = new URL(rawUrl, window.location.href).href;
  const size = await discoverRemoteSize(url, signal);
  return {
    identity: url,
    label: url,
    size,
    async read(offset, length) {
      const end = offset + length - 1n;
      const response = await fetch(url, {
        headers: { Range: `bytes=${offset}-${end}` }, signal,
      });
      const returned = parseContentRange(response.headers.get('Content-Range'));
      if (response.status !== 206 || !returned || returned.start !== offset ||
          returned.end !== end || returned.size !== size) {
        await response.body?.cancel();
        throw new RangeUnsupportedError(`不正な Range 応答です: bytes ${offset}-${end}/${size} が必要です`);
      }
      const data = new Uint8Array(await response.arrayBuffer());
      if (BigInt(data.byteLength) !== length) {
        throw new Error(`Range の長さが一致しません: 期待値 ${length}、実際 ${data.byteLength}`);
      }
      return data;
    },
  };
}

function liveRemoteSource(rawUrl, signal) {
  const url = new URL(rawUrl, window.location.href).href;
  return {
    identity: `live:${url}`,
    label: url,
    size: null,
    async *stream() {
      const response = await fetch(url, { signal });
      if (!response.ok || !response.body) {
        throw new Error(`Live HTTP リクエストに失敗しました: ${response.status}`);
      }
      const reader = response.body.getReader();
      try {
        while (true) {
          const { done, value } = await reader.read();
          if (done) break;
          if (value?.byteLength) yield value;
        }
      } finally {
        reader.releaseLock();
      }
    },
  };
}

async function selectedSource(signal, liveMode) {
  const file = elements.fileInput.files[0];
  if (file) return localSource(file);
  const url = elements.urlInput.value.trim();
  if (url) return liveMode ? liveRemoteSource(url, signal) : remoteSource(url, signal);
  throw new Error('ローカル MMTS ファイルまたは HTTP URL を指定してください');
}

function snapshotTimeRanges(ranges) {
  const result = [];
  for (let index = 0; index < ranges.length; index += 1) {
    result.push({ start: ranges.start(index), end: ranges.end(index) });
  }
  return result;
}

function intersectBufferedRanges(left, right) {
  const result = [];
  let leftIndex = 0;
  let rightIndex = 0;
  while (leftIndex < left.length && rightIndex < right.length) {
    const start = Math.max(left[leftIndex].start, right[rightIndex].start);
    const end = Math.min(left[leftIndex].end, right[rightIndex].end);
    if (end > start) result.push({ start, end });
    if (left[leftIndex].end < right[rightIndex].end) leftIndex += 1;
    else rightIndex += 1;
  }
  return result;
}

class AppendQueue {
  constructor(mediaSource, mediaElement, mime, onUpdateEnd = null) {
    if (!MediaSource.isTypeSupported(mime)) throw new Error(`このブラウザーは ${mime} に対応していません`);
    this.mediaElement = mediaElement;
    this.mediaSource = mediaSource;
    this.mime = mime;
    this.sourceBuffer = mediaSource.addSourceBuffer(mime);
    this.queue = [];
    this.queuedBytes = 0;
    this.currentBytes = 0;
    this.waiters = [];
    this.error = null;
    this.retryTimer = null;
    this.trimBeforeTime = null;
    this.state = 'running';
    this.onUpdateEnd = onUpdateEnd;
    this.sourceBuffer.addEventListener('updateend', () => {
      this.queuedBytes -= this.currentBytes;
      this.currentBytes = 0;
      try {
        if (this.state === 'quiescing') this.state = 'idle';
        else this.pump();
        this.onUpdateEnd?.();
      } catch (error) {
        this.error = error;
      } finally {
        this.resolveWaiters();
      }
    });
    this.sourceBuffer.addEventListener('error', () => {
      this.error = new Error(mediaErrorMessage(this.mediaElement.error) || `SourceBuffer エラー: ${mime}`);
      this.resolveWaiters();
    });
  }
  append(data) {
    if (this.error) throw this.error;
    if (this.state !== 'running') {
      throw new DOMException(`SourceBuffer キューは ${this.state} 状態です`, 'InvalidStateError');
    }
    this.queue.push(data);
    this.queuedBytes += data.byteLength;
    this.pump();
  }
  pump() {
    if (this.error || this.state !== 'running' || this.sourceBuffer.updating) return;
    const mediaFailure = mediaErrorMessage(this.mediaElement.error);
    if (mediaFailure) {
      this.error = new Error(mediaFailure);
      this.resolveWaiters();
      return;
    }
    const ranges = this.bufferedRanges();
    if (this.trimBeforeTime !== null && ranges.length) {
      const removeEnd = this.trimBeforeTime;
      const start = ranges[0].start;
      if (removeEnd > start + 0.25) {
        this.trimBeforeTime = null;
        this.sourceBuffer.remove(start, removeEnd);
        return;
      }
      this.trimBeforeTime = null;
    }
    if (!this.queue.length) return;
    if (this.bufferedAhead() >= FORWARD_BUFFER_HIGH_SECONDS) {
      this.scheduleRetry();
      return;
    }
    const data = this.queue.shift();
    this.currentBytes = data.byteLength;
    try {
      this.sourceBuffer.appendBuffer(data);
    } catch (error) {
      this.queue.unshift(data);
      this.currentBytes = 0;
      if (error.name === 'QuotaExceededError') {
        this.trimBefore(this.mediaElement.currentTime - BACK_BUFFER_SECONDS);
        this.scheduleRetry();
      } else {
        this.error = error;
        this.resolveWaiters();
      }
    }
  }
  bufferedAhead() {
    const ranges = this.bufferedRanges();
    const time = this.mediaElement.currentTime;
    for (let index = 0; index < ranges.length; index += 1) {
      if (ranges[index].start <= time + 0.1 && ranges[index].end >= time) {
        return ranges[index].end - time;
      }
    }
    return 0;
  }
  bufferedRanges() {
    try {
      return snapshotTimeRanges(this.sourceBuffer.buffered);
    } catch (error) {
      if (error.name !== 'InvalidStateError') throw error;
      return snapshotTimeRanges(this.mediaElement.buffered);
    }
  }
  trimBefore(time) {
    if (time <= 0 || this.state !== 'running') return;
    this.trimBeforeTime = this.trimBeforeTime === null
      ? time : Math.max(this.trimBeforeTime, time);
    this.scheduleRetry();
  }
  scheduleRetry() {
    if (this.retryTimer !== null) return;
    this.retryTimer = setTimeout(() => {
      this.retryTimer = null;
      this.pump();
    }, 250);
  }
  waitBelow(limit) {
    if (this.error) return Promise.reject(this.error);
    if (this.queuedBytes <= limit) return Promise.resolve();
    return new Promise((resolve, reject) => this.waiters.push({ limit, idle: false, resolve, reject }));
  }
  isIdle() {
    return !this.sourceBuffer.updating && this.queuedBytes === 0 &&
      this.queue.length === 0 && this.trimBeforeTime === null;
  }
  waitIdle() {
    if (this.error) return Promise.reject(this.error);
    if (this.isIdle()) return Promise.resolve();
    return new Promise((resolve, reject) => this.waiters.push({ idle: true, resolve, reject }));
  }
  async quiesce() {
    if (this.error) throw this.error;
    this.state = 'quiescing';
    if (this.retryTimer !== null) clearTimeout(this.retryTimer);
    this.retryTimer = null;
    this.trimBeforeTime = null;
    this.queue = [];
    this.queuedBytes = this.currentBytes;
    if (!this.sourceBuffer.updating) {
      this.currentBytes = 0;
      this.queuedBytes = 0;
      this.state = 'idle';
    }
    this.resolveWaiters();
    await this.waitIdle();
  }
  resume() {
    if (this.error || this.state === 'destroyed') return;
    this.state = 'running';
    this.pump();
  }
  destroy() {
    if (this.retryTimer !== null) clearTimeout(this.retryTimer);
    this.retryTimer = null;
    this.state = 'destroyed';
    this.error = this.error || new Error('SourceBuffer キューを停止しました');
    this.queue = [];
    this.queuedBytes = 0;
    this.currentBytes = 0;
    this.resolveWaiters();
  }
  resolveWaiters() {
    const pending = this.waiters;
    this.waiters = [];
    for (const waiter of pending) {
      if (this.error) waiter.reject(this.error);
      else if (waiter.idle ? this.isIdle() : this.queuedBytes <= waiter.limit) waiter.resolve();
      else this.waiters.push(waiter);
    }
  }
}

function once(target, event) {
  return new Promise((resolve, reject) => {
    const done = () => { cleanup(); resolve(); };
    const failed = () => { cleanup(); reject(new Error(`${event} に失敗しました`)); };
    const cleanup = () => {
      target.removeEventListener(event, done);
      target.removeEventListener('error', failed);
    };
    target.addEventListener(event, done, { once: true });
    target.addEventListener('error', failed, { once: true });
  });
}

function setRunning(running) {
  elements.probeButton.disabled = running || !wasmModule;
  elements.cancelButton.disabled = !running;
  elements.fileInput.disabled = running;
  elements.urlInput.disabled = running;
  elements.liveMode.disabled = running;
}

function createSubtitleRenderer(liveMode) {
  activeSubtitleRenderer?.destroy();
  activeSubtitleRenderer = new B62TTMLRenderer({
    mediaElement: elements.video,
    overlayElement: elements.subtitleOverlay,
    isLive: liveMode,
    maxCues: liveMode ? 300 : 100000,
    normalFont: '"Rounded M+ 1m for ARIB", "Hiragino Maru Gothic Pro", "BIZ UDGothic", "Yu Gothic Medium", sans-serif',
    forceStrokeColor: '#000000',
    strokeWidthInPlane: 4,
    backgroundPadding: '0 0.08em',
    lineBackground: true,
  });
}

function timestampMilliseconds(value, timescale) {
  if (value === null || value === undefined || !Number.isFinite(timescale) || timescale <= 0) {
    return undefined;
  }
  return Number(value) * 1000 / timescale;
}

function releaseMedia() {
  if (playbackQualityTimer !== null) clearInterval(playbackQualityTimer);
  playbackQualityTimer = null;
  activeMediaSource = null;
  activeAudioSwitch = null;
  activeSubtitleSwitch = null;
  activeSubtitleRenderer?.destroy();
  activeSubtitleRenderer = null;
  internalSeekTarget = null;
  for (const queue of activeQueues) queue.destroy();
  elements.video.pause();
  elements.video.removeAttribute('src');
  elements.video.load();
  if (activeObjectUrl) URL.revokeObjectURL(activeObjectUrl);
  activeObjectUrl = null;
  activeQueues = [];
  activeQueueByType = new Map();
}

function stopPlayback(quiet = false, preserveMedia = false) {
  runGeneration += 1;
  activeController?.abort();
  activeProbe?.cancel();
  activeProbe?.delete();
  if (activeApplicationDrainState) activeApplicationDrainState.alive = false;
  activeApplicationDrainState = null;
  activeDemuxer?.delete();
  activeController = null;
  activeProbe = null;
  activeDemuxer = null;
  activeAudioSwitch = null;
  activeSubtitleSwitch = null;
  activeSubtitleRenderer?.reset();
  if (!preserveMedia) releaseMedia();
  setRunning(false);
  if (!quiet) {
    elements.probeState.textContent = '停止しました';
    elements.mediaInfo.textContent = '停止しました';
    appendLog('停止しました');
  }
}

async function probeDuration(source, generation) {
  const initialRangeSize = BigInt(elements.initialRange.value) * MiB;
  const maxRangeSize = BigInt(elements.maxRange.value) * MiB;
  if (maxRangeSize < initialRangeSize) throw new Error('最大 Range は初期 Range 以上にしてください');
  const options = { initialRangeSize, maxRangeSize };
  const videoPacketId = parsePacketId();
  if (videoPacketId !== undefined) options.videoPacketId = videoPacketId;
  const probe = new wasmModule.DurationProbe();
  activeProbe = probe;
  if (!probe.begin(source.size, options)) throw new Error(`再生時間の検出を開始できません: ${probe.failure()}`);
  let number = 0;
  while (probe.state() === 'need-range') {
    const request = probe.nextRange();
    if (!request) throw new Error('検出器から Range リクエストが返されませんでした');
    number += 1;
    const end = request.offset + request.length - 1n;
    elements.probeState.textContent = `Range 検出 ${number}`;
    appendLog(`検出 #${number} bytes=${request.offset}-${end} (${formatBytes(request.length)})`);
    let data;
    try { data = await source.read(request.offset, request.length); }
    catch (error) {
      if (generation === runGeneration) probe.failRange(request.requestId);
      throw error;
    }
    if (generation !== runGeneration) return null;
    if (!probe.pushRange(request.requestId, request.offset, data, true)) {
      throw new Error(`Range #${number} は検出器に拒否されました`);
    }
    elements.transferred.textContent = formatBytes(probe.transferredBytes());
  }
  if (probe.state() !== 'complete') throw new Error(`検出未完了: ${probe.state()} / ${probe.failure()}`);
  const result = { duration: probe.duration(), transferred: probe.transferredBytes() };
  probe.delete();
  activeProbe = null;
  return result;
}

function bufferedAhead() {
  const ranges = elements.video.buffered;
  if (!ranges.length) return 0;
  for (let index = 0; index < ranges.length; index += 1) {
    if (ranges.start(index) <= elements.video.currentTime + 0.1 &&
        ranges.end(index) >= elements.video.currentTime) {
      return ranges.end(index) - elements.video.currentTime;
    }
  }
  return 0;
}

function monitorPlaybackQuality(generation) {
  if (playbackQualityTimer !== null) clearInterval(playbackQualityTimer);
  if (typeof elements.video.getVideoPlaybackQuality !== 'function') return;
  let previous = elements.video.getVideoPlaybackQuality();
  playbackQualityTimer = setInterval(() => {
    if (generation !== runGeneration) {
      clearInterval(playbackQualityTimer);
      playbackQualityTimer = null;
      return;
    }
    const current = elements.video.getVideoPlaybackQuality();
    const dropped = current.droppedVideoFrames - previous.droppedVideoFrames;
    const total = current.totalVideoFrames - previous.totalVideoFrames;
    if (dropped > 0) {
      const ahead = bufferedAhead();
      const cause = ahead < 0.5 ? 'MSE 供給不足' : 'デコード/描画負荷';
      appendLog(`映像品質 ${cause}: 5秒で ${dropped}/${total} フレーム落ち、バッファ=${ahead.toFixed(1)}s`);
    }
    previous = current;
  }, 5000);
}

function isTimeBuffered(time) {
  const ranges = elements.video.buffered;
  for (let index = 0; index < ranges.length; index += 1) {
    if (ranges.start(index) <= time && ranges.end(index) >= time + 0.1) return true;
  }
  return false;
}

async function playbackBackpressure(generation) {
  for (const queue of activeQueues) {
    queue.trimBefore(elements.video.currentTime - BACK_BUFFER_SECONDS);
  }
  await Promise.all(activeQueues.map(queue => queue.waitBelow(SOURCE_QUEUE_HIGH_BYTES)));
  if (generation !== runGeneration) return;
  if (bufferedAhead() < FORWARD_BUFFER_HIGH_SECONDS) return;
  elements.probeState.textContent = elements.video.paused ? '再生待ち' : 'バッファ十分';
  while (generation === runGeneration && bufferedAhead() > FORWARD_BUFFER_LOW_SECONDS) {
    for (const queue of activeQueues) {
      queue.trimBefore(elements.video.currentTime - BACK_BUFFER_SECONDS);
    }
    await new Promise(resolve => setTimeout(resolve, 250));
  }
  if (generation === runGeneration) elements.probeState.textContent = 'バッファリング中';
}

async function playSource(source, probeResult, generation, startTimeSeconds = 0,
                          liveMode = false, reuseMedia = false) {
  const playbackRate = liveMode ? LIVE_PLAYBACK_RATE : DEFAULT_PLAYBACK_RATE;
  elements.video.defaultPlaybackRate = playbackRate;
  elements.video.playbackRate = playbackRate;
  let mediaSource;
  const openFreshMediaSource = async () => {
    activeQueueByType = new Map();
    const fresh = new MediaSource();
    fresh.tlvdemuxQueues = activeQueueByType;
    activeMediaSource = fresh;
    activeObjectUrl = URL.createObjectURL(fresh);
    elements.video.src = activeObjectUrl;
    await once(fresh, 'sourceopen');
    return fresh;
  };
  if (reuseMedia && (!activeMediaSource || !activeObjectUrl)) reuseMedia = false;
  if (reuseMedia) {
    mediaSource = activeMediaSource;
    const registry = mediaSource.tlvdemuxQueues;
    if (mediaSource.readyState !== 'open' || !(registry instanceof Map) ||
        registry.size !== mediaSource.sourceBuffers.length) {
      appendLog(`MediaSource を再構築します (状態=${mediaSource.readyState})`);
      releaseMedia();
      reuseMedia = false;
    } else {
      activeQueueByType = registry;
      activeQueues = [...registry.values()];
      await Promise.all(activeQueues.map(queue => queue.quiesce()));
    }
  }
  if (!reuseMedia) {
    mediaSource = await openFreshMediaSource();
  }
  if (generation !== runGeneration) return;
  if (mediaSource.readyState !== 'open') {
    appendLog(`シーク準備中に MediaSource が ${mediaSource.readyState} になったため再構築します`);
    if (mediaSource === activeMediaSource) releaseMedia();
    reuseMedia = false;
    mediaSource = await openFreshMediaSource();
    if (generation !== runGeneration) return;
  }
  const mediaDuration = liveMode ? Infinity : durationSeconds(probeResult.duration);
  try {
    mediaSource.duration = mediaDuration;
  } catch (error) {
    if (error.name !== 'InvalidStateError') throw error;
    appendLog(`MediaSource 設定競合 (${error.message}) のため再構築します`);
    if (mediaSource === activeMediaSource) releaseMedia();
    reuseMedia = false;
    mediaSource = await openFreshMediaSource();
    if (generation !== runGeneration) return;
    mediaSource.duration = mediaDuration;
  }
  for (const queue of activeQueues) queue.resume();
  createSubtitleRenderer(liveMode);

  const queues = reuseMedia ? new Map(activeQueueByType) : new Map();
  const tracks = new Map();
  let selectedVideo = null;
  let selectedAudio = null;
  let selectedSubtitle = null;
  let subtitleEventCount = 0;
  let callbackError = null;
  let recoverableErrors = 0;
  let played = reuseMedia && !elements.video.paused;
  let suppressOutput = startTimeSeconds > 0;
  let headVideoSeen = false;
  let seekProbeActive = false;
  let seekProbeRap = null;
  const pendingInits = new Map();
  const pendingSegments = new Map([['video', []], ['audio', []]]);
  const mseSegmentTypes = new Set();
  const externalDurationUs = liveMode ? null : BigInt(Math.round(
    durationSeconds(probeResult.duration) * 1000000));

  const maybeStartPlayback = () => {
    if (played || generation !== runGeneration || queues.size < 2) return;
    let commonRanges = null;
    for (const queue of queues.values()) {
      const ranges = queue.bufferedRanges();
      if (!ranges.length) return;
      commonRanges = commonRanges === null
        ? ranges : intersectBufferedRanges(commonRanges, ranges);
      if (!commonRanges.length) return;
    }
    const currentTime = elements.video.currentTime;
    const range = commonRanges.find(item => item.end > currentTime + 0.001);
    if (!range) return;
    const commonAhead = range.end - Math.max(currentTime, range.start);
    if (currentTime < range.start - 0.001) {
      internalSeekTarget = range.start;
      elements.video.currentTime = range.start;
      appendLog(`再生開始を共通バッファ先頭 ${range.start.toFixed(6)}s に合わせます`);
    }
    played = true;
    monitorPlaybackQuality(generation);
    elements.probeState.textContent = liveMode ? 'Live 再生中' : '再生中';
    if (liveMode) appendLog(`Live 共通バッファ ${commonAhead.toFixed(1)}s で再生開始 (1×)`);
    elements.video.play().catch(() => {
      appendLog('自動再生がブロックされました。再生ボタンを押してください');
    });
  };
  for (const queue of activeQueues) queue.onUpdateEnd = maybeStartPlayback;

  const appendSegment = (type, data) => {
    const queue = queues.get(type);
    if (!queue) {
      const pending = pendingSegments.get(type);
      pending.push(data);
      if (pending.reduce((sum, item) => sum + item.byteLength, 0) > SOURCE_QUEUE_HIGH_BYTES) {
        throw new Error(`${type} の初期化待ちが長すぎます`);
      }
      return;
    }
    queue.append(data);
  };

  const installPairedInits = () => {
    if (!pendingInits.has('video') || !pendingInits.has('audio') || queues.size) return;
    for (const type of ['video', 'audio']) {
      const init = pendingInits.get(type);
      let queue = activeQueueByType.get(type);
      if (queue && queue.mime !== init.mime) {
        throw new Error(`シーク中に ${type} codec が変化しました: ${queue.mime} -> ${init.mime}`);
      }
      if (!queue) {
        queue = new AppendQueue(mediaSource, elements.video, init.mime, maybeStartPlayback);
        activeQueueByType.set(type, queue);
        activeQueues.push(queue);
      }
      queues.set(type, queue);
    }
    for (const type of ['video', 'audio']) {
      const init = pendingInits.get(type);
      queues.get(type).append(init.data);
      const details = type === 'video'
        ? `${init.width}x${init.height}`
        : `${init.sampleRate}Hz ${init.channels}ch`;
      elements.mediaInfo.textContent += ` · ${type} ${details}`;
    }
    for (const type of ['video', 'audio']) {
      for (const data of pendingSegments.get(type)) appendSegment(type, data);
      pendingSegments.get(type).length = 0;
    }
  };

  const onMseInit = init => {
      const type = init.type;
      try {
        appendLog(`${type} 初期化 ${init.mime}`);
        if (queues.size) {
          const queue = queues.get(type);
          if (!queue || queue.mime !== init.mime) {
            throw new Error(`${type} codec を切り替えられません: ${init.mime}`);
          }
          queue.append(init.data);
          if (type === 'audio') {
            elements.mediaInfo.textContent = elements.mediaInfo.textContent.replace(
              / · audio [^·]+$/, ` · audio ${init.sampleRate}Hz ${init.channels}ch`);
          }
          return;
        }
        pendingInits.set(type, init);
        installPairedInits();
      } catch (error) { callbackError = error; }
  };
  const onMseSegment = segment => {
      try {
        if (!mseSegmentTypes.has(segment.type)) {
          mseSegmentTypes.add(segment.type);
          appendLog(`${segment.type} media segment 開始`);
        }
        appendSegment(segment.type, segment.data);
      }
      catch (error) { callbackError = error; }
  };
  const wantedVideoPacketId = parsePacketId();

  const selectAudioTrack = track => {
    selectedAudio = track.trackId;
    selectedAudioPacketId = track.packetId;
    demuxer.selectTrack('audio', selectedAudio);
    renderAudioTracks();
  };

  const selectSubtitleTrack = track => {
    selectedSubtitle = track.trackId;
    selectedSubtitlePacketId = track.packetId;
    demuxer.selectTrack('subtitle', selectedSubtitle);
    activeSubtitleRenderer?.reset();
    renderSubtitleTracks();
  };

  let demuxer;
  demuxer = new wasmModule.TlvDemuxer({
    onMseVideoStart(detail) {
      appendLog(`映像開始 HEVC NAL=${detail.nalType} シグナルRAP=${detail.signalledRandomAccess}`);
    },
    onMseInit,
    onMseSegment,
    onTrack(track) {
      tracks.set(track.trackId, track);
      appendLog(`トラック ${track.kind} packet_id=0x${track.packetId.toString(16)} codec=${track.codec}`);
      if (track.kind === 'video' && selectedVideo === null &&
          (wantedVideoPacketId === undefined || track.packetId === wantedVideoPacketId)) {
        selectedVideo = track.trackId;
        demuxer.selectTrack('video', selectedVideo);
        if (externalDurationUs !== null && typeof demuxer.setIndexDuration === 'function') {
          demuxer.setIndexDuration(externalDurationUs);
        }
      } else if (track.kind === 'audio') {
        knownAudioTracks.set(track.packetId, track);
        renderAudioTracks();
        const desired = preferredAudioPacketId ?? selectedAudioPacketId;
        if (selectedAudio === null || track.packetId === desired) selectAudioTrack(track);
      } else if (track.kind === 'subtitle' && track.codec === 'ttml') {
        knownSubtitleTracks.set(track.packetId, track);
        renderSubtitleTracks();
        const desired = preferredSubtitlePacketId ?? selectedSubtitlePacketId;
        if (selectedSubtitle === null || track.packetId === desired) selectSubtitleTrack(track);
      }
    },
    onBroadcastClock(clock) {
      try { dataBroadcast.broadcastClockChanged(clock); }
      catch (error) { callbackError = error; }
    },
    onEventInfo(event) {
      try { dataBroadcast.eventInformationChanged(event); }
      catch (error) { callbackError = error; }
    },
    onApplicationResourceView(resource) {
      try { dataBroadcast.resourceChanged(resource); }
      catch (error) { callbackError = error; }
    },
    onApplicationState(state) {
      try { dataBroadcast.applicationStateChanged(demuxer, state); }
      catch (error) { callbackError = error; }
    },
    onApplicationResourcesReset() {
      dataBroadcast.resourcesReset();
    },
    onAccessUnitView(unit) {
      try {
        if (unit.trackId === selectedVideo) {
          if (unit.randomAccess) headVideoSeen = true;
          if (seekProbeActive && unit.randomAccess && seekProbeRap === null) {
            seekProbeRap = {
              seconds: Number(unit.ptsValue) / unit.ptsTimescale,
              restartOffset: BigInt(unit.restartOffset),
            };
          }
        } else if (unit.trackId === selectedSubtitle && !suppressOutput) {
          const track = tracks.get(unit.trackId);
          if (!track || !activeSubtitleRenderer) return;
          if (unit.discontinuity) activeSubtitleRenderer.reset();
          const subtitleData = {
            packetId: track.packetId,
            mpuSequenceNumber: unit.mpuSequenceNumber ?? undefined,
            pts: timestampMilliseconds(unit.ptsValue, unit.ptsTimescale),
            dts: timestampMilliseconds(unit.dtsValue, unit.dtsTimescale),
            subtitleTimingMode: track.subtitle?.timingMode,
            subtitleReferenceStartMediaTime: timestampMilliseconds(
              unit.subtitleReferenceStartPtsValue,
              unit.subtitleReferenceStartPtsTimescale,
            ),
            data: unit.data,
            len: unit.data.byteLength,
            resources: (unit.subtitleResources || []).map(resource => ({
              index: resource.subsampleNumber,
              dataType: resource.dataType,
              data: resource.data,
            })),
          };
          const result = activeSubtitleRenderer.push(subtitleData);
          subtitleEventCount += 1;
          if (subtitleEventCount <= 8) {
            appendLog(`字幕 #${subtitleEventCount} packet_id=0x${track.packetId.toString(16)}` +
              ` cues=${result.cueCount} resources=${result.resourceCount}` +
              ` pts=${subtitleData.pts?.toFixed(3) ?? '—'}ms`);
          }
        }
      } catch (error) { callbackError = error; }
    },
    onError(error) {
      if (!error.recoverable) callbackError = new Error(error.message);
      else if (recoverableErrors++ < 8) appendLog(`分離警告 @${error.inputOffset}: ${error.message}`);
    },
  });
  demuxer.setMseOutputEnabled(!suppressOutput);
  activeDemuxer = demuxer;
  const applicationDrainState = { alive: true, scheduled: false };
  activeApplicationDrainState = applicationDrainState;
  const scheduleApplicationDrain = () => {
    if (applicationDrainState.scheduled || !applicationDrainState.alive) return;
    applicationDrainState.scheduled = true;
    const run = () => {
      applicationDrainState.scheduled = false;
      if (!applicationDrainState.alive || generation !== runGeneration) return;
      try {
        if (demuxer.drainApplicationResources(32)) scheduleApplicationDrain();
      } catch (error) {
        callbackError = error;
      }
    };
    if (globalThis.scheduler?.postTask) {
      globalThis.scheduler.postTask(run, { priority: 'background' }).catch(error => {
        callbackError = error;
      });
    } else {
      setTimeout(run, 0);
    }
  };
  activeAudioSwitch = async packetId => {
    const track = [...tracks.values()].find(item => item.kind === 'audio' && item.packetId === packetId);
    if (!track) throw new Error(`音声 packet_id=0x${packetId.toString(16)} は利用できません`);
    if (track.trackId === selectedAudio) return;
    if (generation !== runGeneration) return;
    const restartTime = liveMode ? 0 : Math.max(0, elements.video.currentTime);
    const label = liveMode
      ? `音声切替 packet_id=0x${packetId.toString(16)}、Live を再接続します`
      : `音声切替 packet_id=0x${packetId.toString(16)}、${restartTime.toFixed(3)}s から MSE を再構築します`;
    stopPlayback(true, false);
    await loadAndPlay(restartTime, false, label);
  };
  activeSubtitleSwitch = packetId => {
    const track = [...tracks.values()].find(
      item => item.kind === 'subtitle' && item.packetId === packetId,
    );
    if (!track) throw new Error(`字幕 packet_id=0x${packetId.toString(16)} は利用できません`);
    if (track.trackId === selectedSubtitle) return;
    selectSubtitleTrack(track);
    appendLog(`字幕切替 packet_id=0x${packetId.toString(16)}`);
  };
  demuxer.startIndex(liveMode);
  if (typeof demuxer.setIndexDuration !== 'function' && startTimeSeconds > 0) {
    throw new Error('現在の tlvdemux.js は Range シークに未対応です。WASM を再ビルドしてください');
  }
  elements.mediaInfo.textContent = 'tlvdemux';
  elements.probeState.textContent = 'バッファリング中';

  let offset = 0n;
  let playbackBytes = 0n;
  let lastReported = 0n;
  if (startTimeSeconds > 0) {
    let headEnd = 0n;
    const maximumHead = 64n * MiB < source.size ? 64n * MiB : source.size;
    while ((!selectedVideo || !headVideoSeen) && headEnd < maximumHead) {
      const length = maximumHead - headEnd < PLAYBACK_CHUNK
        ? maximumHead - headEnd : PLAYBACK_CHUNK;
      const data = await source.read(headEnd, length);
      if (generation !== runGeneration) return;
      if (!demuxer.push(data)) throw new Error(`先頭解析に失敗しました: ${headEnd}`);
      scheduleApplicationDrain();
      if (callbackError) throw callbackError;
      headEnd += length;
      playbackBytes += length;
    }
    if (!selectedVideo || !headVideoSeen) throw new Error('シーク準備中に選択した映像を検出できませんでした');
    const targetUs = BigInt(Math.round(startTimeSeconds * 1000000));
    demuxer.setIndexDuration(externalDurationUs);
    const estimate = demuxer.estimateOffset(targetUs, source.size);
    if (estimate === null) throw new Error('シーク先のバイト位置を推定できませんでした');
    let preroll = seekPrerollBytes(source.size, externalDurationUs);
    let candidate = 0n;
    let attempt = 0;
    for (;;) {
      candidate = estimate > preroll ? estimate - preroll : 0n;
      demuxer.reposition(candidate, true);
      seekProbeRap = null;
      seekProbeActive = true;
      let probeOffset = candidate;
      const probeLimit = candidate + SEEK_PROBE_BYTES < source.size
        ? candidate + SEEK_PROBE_BYTES : source.size;
      while (seekProbeRap === null && probeOffset < probeLimit) {
        const length = probeLimit - probeOffset < PLAYBACK_CHUNK
          ? probeLimit - probeOffset : PLAYBACK_CHUNK;
        const data = await source.read(probeOffset, length);
        if (generation !== runGeneration) return;
        if (!demuxer.push(data)) throw new Error(`シーク位置の検証に失敗しました: ${probeOffset}`);
        scheduleApplicationDrain();
        if (callbackError) throw callbackError;
        probeOffset += length;
        playbackBytes += length;
      }
      seekProbeActive = false;
      if (seekProbeRap !== null && seekProbeRap.seconds <= startTimeSeconds + 0.05) break;
      if (candidate === 0n || ++attempt >= MAX_SEEK_PROBE_ATTEMPTS) {
        const found = seekProbeRap === null ? 'RAP なし' : `RAP ${seekProbeRap.seconds.toFixed(3)}s`;
        throw new Error(`シーク先より前の映像開始点を検出できませんでした (${found})`);
      }
      const found = seekProbeRap === null ? 'RAP なし' : `RAP ${seekProbeRap.seconds.toFixed(3)}s`;
      appendLog(`シーク再探索 ${found} > ${startTimeSeconds.toFixed(3)}s、preroll を拡大します`);
      preroll *= 2n;
      if (preroll > estimate) preroll = estimate;
    }
    offset = seekProbeRap.restartOffset;
    demuxer.reposition(offset, true);
    suppressOutput = false;
    demuxer.setMseOutputEnabled(true);
    if (!reuseMedia) {
      internalSeekTarget = startTimeSeconds;
      elements.video.currentTime = startTimeSeconds;
    }
    appendLog(`シーク ${startTimeSeconds.toFixed(3)}s -> 推定 ${formatBytes(estimate)}、RAP ${seekProbeRap.seconds.toFixed(3)}s @ ${formatBytes(offset)}、preroll ${formatBytes(preroll)}`);
  }
  if (liveMode && source.stream) {
    for await (const data of source.stream()) {
      if (generation !== runGeneration) return;
      if (!demuxer.push(data)) throw new Error(`Live 分離入力に失敗しました: ${playbackBytes}`);
      scheduleApplicationDrain();
      if (callbackError) throw callbackError;
      playbackBytes += BigInt(data.byteLength);
      elements.transferred.textContent = `${formatBytes(playbackBytes)} / ${bufferedAhead().toFixed(1)}s`;
      if (playbackBytes - lastReported >= 32n * MiB) {
        appendLog(`Live ${formatBytes(playbackBytes)}、バッファ=${bufferedAhead().toFixed(1)}s`);
        lastReported = playbackBytes;
      }
      await playbackBackpressure(generation);
    }
  }
  while ((!liveMode || !source.stream) && offset < source.size && generation === runGeneration) {
    const length = source.size - offset < PLAYBACK_CHUNK ? source.size - offset : PLAYBACK_CHUNK;
    const data = await source.read(offset, length);
    if (generation !== runGeneration) return;
    if (!demuxer.push(data)) throw new Error(`分離入力に失敗しました: ${offset}`);
    scheduleApplicationDrain();
    if (callbackError) throw callbackError;
    offset += length;
    playbackBytes += length;
    elements.transferred.textContent = `${formatBytes(probeResult.transferred + playbackBytes)} / ${bufferedAhead().toFixed(1)}s`;
    if (playbackBytes - lastReported >= 32n * MiB || offset === source.size) {
      appendLog(`再生 ${formatBytes(offset)} / ${formatBytes(source.size)}、バッファ=${bufferedAhead().toFixed(1)}s`);
      lastReported = playbackBytes;
    }
    await playbackBackpressure(generation);
  }
  if (generation !== runGeneration) return;
  demuxer.flush();
  while (demuxer.drainApplicationResources(256)) {
    await new Promise(resolve => setTimeout(resolve, 0));
    if (generation !== runGeneration) return;
  }
  if (callbackError) throw callbackError;
  if (!liveMode) demuxer.finalizeIndex();
  appendLog(`索引 RAP ${demuxer.seekPointCount()} 点、状態=${demuxer.indexState()}`);
  applicationDrainState.alive = false;
  if (activeApplicationDrainState === applicationDrainState) activeApplicationDrainState = null;
  demuxer.delete();
  activeDemuxer = null;
  activeAudioSwitch = null;
  activeSubtitleSwitch = null;
  await Promise.all(activeQueues.map(queue => queue.waitIdle()));
  if (generation !== runGeneration) return;
  if (mediaSource.readyState === 'open') mediaSource.endOfStream();
  elements.probeState.textContent = liveMode ? 'Live 終了' : '読み込み完了';
  appendLog(liveMode ? 'Live ストリームが終了しました' : 'ストリーム終端です');
}

async function loadAndPlay(startTimeSeconds = 0, reuseMedia = false, operationLabel = null) {
  if (!reuseMedia && startTimeSeconds === 0) dataBroadcast.beginSession();
  if (!reuseMedia) {
    releaseMedia();
    knownAudioTracks = new Map();
    selectedAudioPacketId = null;
    renderAudioTracks();
    knownSubtitleTracks = new Map();
    selectedSubtitlePacketId = null;
    renderSubtitleTracks();
  }
  const generation = ++runGeneration;
  const controller = new AbortController();
  activeController = controller;
  setRunning(true);
  if (startTimeSeconds === 0) elements.duration.textContent = '—';
  elements.sourceSize.textContent = '—';
  elements.transferred.textContent = '—';
  elements.probeState.textContent = '入力情報を確認中';
  elements.mediaInfo.textContent = '準備中';
  elements.log.textContent = '';
  if (operationLabel) appendLog(operationLabel);
  try {
    let liveMode = elements.liveMode.checked;
    currentLiveMode = liveMode;
    if (liveMode && startTimeSeconds > 0) throw new Error('Live mode ではシークできません');
    let source;
    try {
      source = await selectedSource(controller.signal, liveMode);
    } catch (error) {
      if (!(error instanceof RangeUnsupportedError) || liveMode || elements.fileInput.files[0]) throw error;
      liveMode = true;
      currentLiveMode = true;
      elements.liveMode.checked = true;
      appendLog('Range 非対応のため Live mode に切り替えました');
      source = await selectedSource(controller.signal, true);
    }
    if (generation !== runGeneration) return;
    elements.sourceSize.textContent = liveMode && source.size === null
      ? 'Live' : formatBytes(source.size);
    appendLog(`入力 ${source.label}`);
    appendLog(liveMode ? 'モード Live ストリーム' : `サイズ ${source.size} (${formatBytes(source.size)})`);
    let probeResult;
    if (liveMode) {
      probeResult = { duration: null, transferred: 0n };
      elements.duration.textContent = 'Live';
    } else if (cachedProbe && cachedProbe.identity === source.identity && cachedProbe.size === source.size) {
      probeResult = cachedProbe.result;
      appendLog(`再生時間キャッシュ ${durationSeconds(probeResult.duration).toFixed(6)}s`);
    } else {
      probeResult = await probeDuration(source, generation);
      if (probeResult) cachedProbe = { identity: source.identity, size: source.size, result: probeResult };
    }
    if (!probeResult || generation !== runGeneration) return;
    if (!liveMode) {
      elements.duration.textContent = formatDuration(probeResult.duration);
      appendLog(`再生時間 ${durationSeconds(probeResult.duration).toFixed(6)}s、検出読み込み ${formatBytes(probeResult.transferred)}`);
    }
    await playSource(source, probeResult, generation, startTimeSeconds, liveMode, reuseMedia);
  } catch (error) {
    if (generation !== runGeneration || error.name === 'AbortError') return;
    elements.probeState.textContent = '失敗';
    elements.mediaInfo.textContent = error.message || String(error);
    appendLog(`エラー ${error.message || error}`);
    console.error(error);
  } finally {
    if (generation === runGeneration) {
      activeProbe?.delete();
      activeProbe = null;
      if (activeApplicationDrainState) activeApplicationDrainState.alive = false;
      activeApplicationDrainState = null;
      activeDemuxer?.delete();
      activeDemuxer = null;
      activeController = null;
      setRunning(false);
    }
  }
}

elements.probeButton.addEventListener('click', () => loadAndPlay(0));
elements.cancelButton.addEventListener('click', stopPlayback);
elements.clearButton.addEventListener('click', () => { elements.log.textContent = ''; });
elements.audioTrack.addEventListener('change', () => {
  const value = elements.audioTrack.value;
  preferredAudioPacketId = value === '' ? null : Number(value);
  try {
    if (preferredAudioPacketId === null) localStorage.removeItem(AUDIO_STORAGE_KEY);
    else localStorage.setItem(AUDIO_STORAGE_KEY, String(preferredAudioPacketId));
  } catch (_) { /* Keep track switching available without storage. */ }
  const target = preferredAudioPacketId === null
    ? [...knownAudioTracks.values()].find(track => track.audio?.mainComponent) || knownAudioTracks.values().next().value
    : knownAudioTracks.get(preferredAudioPacketId);
  if (target && activeAudioSwitch) {
    activeAudioSwitch(target.packetId).catch(error => {
      appendLog(`音声切替エラー ${error.message || error}`);
      console.error(error);
    });
  }
});
elements.subtitleTrack.addEventListener('change', () => {
  const value = elements.subtitleTrack.value;
  preferredSubtitlePacketId = value === '' ? null : Number(value);
  try {
    if (preferredSubtitlePacketId === null) localStorage.removeItem(SUBTITLE_STORAGE_KEY);
    else localStorage.setItem(SUBTITLE_STORAGE_KEY, String(preferredSubtitlePacketId));
  } catch (_) { /* Keep track switching available without storage. */ }
  const target = preferredSubtitlePacketId === null
    ? knownSubtitleTracks.values().next().value
    : knownSubtitleTracks.get(preferredSubtitlePacketId);
  if (target && activeSubtitleSwitch) {
    try { activeSubtitleSwitch(target.packetId); }
    catch (error) {
      appendLog(`字幕切替エラー ${error.message || error}`);
      console.error(error);
    }
  }
});
elements.video.addEventListener('error', () => {
  const message = mediaErrorMessage();
  appendLog(`映像エラー ${message || '不明'}`);
  elements.probeState.textContent = 'デコード失敗';
  elements.mediaInfo.textContent = message || 'MediaElement エラー';
  activeController?.abort();
});
elements.video.addEventListener('seeking', () => {
  if (currentLiveMode || !activeMediaSource) return;
  const target = elements.video.currentTime;
  if (internalSeekTarget !== null && Math.abs(target - internalSeekTarget) < 0.1) return;
  internalSeekTarget = null;
  if (isTimeBuffered(target)) return;
  clearTimeout(seekTimer);
  seekTimer = setTimeout(() => {
    if (!activeMediaSource) return;
    appendLog(`ユーザーシーク ${target.toFixed(3)}s`);
    stopPlayback(true, true);
    loadAndPlay(target, true);
  }, 120);
});
elements.video.addEventListener('seeked', () => {
  if (internalSeekTarget !== null &&
      Math.abs(elements.video.currentTime - internalSeekTarget) < 0.1) {
    internalSeekTarget = null;
  }
});

if (typeof createTlvDemuxModule !== 'function') {
  elements.wasmStatus.textContent = 'WASM がありません';
  elements.wasmStatus.className = 'badge error';
  elements.probeState.textContent = 'build-wasm/tlvdemux.js を先にビルドしてください';
  appendLog('../build-wasm/tlvdemux.js が見つかりません');
} else {
  createTlvDemuxModule().then(module => {
    wasmModule = module;
    elements.wasmStatus.textContent = 'WASM 準備完了';
    elements.wasmStatus.className = 'badge';
    setRunning(false);
  }).catch(error => {
    elements.wasmStatus.textContent = 'WASM 読み込み失敗';
    elements.wasmStatus.className = 'badge error';
    elements.probeState.textContent = '読み込み失敗';
    appendLog(`WASM エラー ${error.message || error}`);
  });
}
