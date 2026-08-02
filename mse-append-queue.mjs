const DEFAULT_RETRY_DELAY_MILLISECONDS = 250;
const DEFAULT_BACK_BUFFER_SECONDS = 8;
const DEFAULT_FORWARD_BUFFER_HIGH_SECONDS = 15;
const DEFAULT_TRIM_GRANULARITY_SECONDS = 2;

function snapshotTimeRanges(ranges) {
  const result = [];
  for (let index = 0; index < ranges.length; index += 1) {
    result.push({ start: ranges.start(index), end: ranges.end(index) });
  }
  return result;
}

function defaultMediaError(media) {
  if (!media.error) return '';
  return `MediaError code=${media.error.code}`;
}

/**
 * Serializes SourceBuffer mutations and applies byte/time backpressure.
 *
 * queuedBytes deliberately includes the append currently owned by the
 * SourceBuffer. updateend recomputes it from the remaining queue because the
 * event can also be emitted by abort() and remove(), where subtracting the
 * last append size would make the accounting drift.
 */
export class MseAppendQueue {
  constructor(mediaSource, mediaElement, mime, onUpdateEnd = null, options = {}) {
    const MediaSourceClass = globalThis.ManagedMediaSource ?? globalThis.MediaSource;
    if (!MediaSourceClass?.isTypeSupported(mime)) {
      throw new Error(`Unsupported MSE type: ${mime}`);
    }

    this.mediaElement = mediaElement;
    this.mediaSource = mediaSource;
    this.mime = mime;
    this.sourceBuffer = mediaSource.addSourceBuffer(mime);
    this.sourceBuffer.mode = 'segments';
    this.queue = [];
    this.queuedBytes = 0;
    this.currentBytes = 0;
    this.waiters = [];
    this.error = null;
    this.retryTimer = null;
    this.trimBeforeTime = null;
    this.forceTrim = false;
    this.state = 'running';
    this.onUpdateEnd = onUpdateEnd;
    this.retryDelayMilliseconds = options.retryDelayMilliseconds ?? DEFAULT_RETRY_DELAY_MILLISECONDS;
    this.backBufferSeconds = options.backBufferSeconds ?? DEFAULT_BACK_BUFFER_SECONDS;
    this.forwardBufferHighSeconds = options.forwardBufferHighSeconds ?? DEFAULT_FORWARD_BUFFER_HIGH_SECONDS;
    this.trimGranularitySeconds = options.trimGranularitySeconds ?? DEFAULT_TRIM_GRANULARITY_SECONDS;
    this.getMediaError = options.getMediaError ?? defaultMediaError;

    this.sourceBuffer.addEventListener('updateend', () => {
      this.currentBytes = 0;
      this.recountQueuedBytes();
      try {
        if (this.state === 'quiescing') this.state = 'idle';
        else this.pump();
        this.onUpdateEnd?.();
      } catch (error) {
        this.fail(error);
      } finally {
        this.resolveWaiters();
      }
    });
    this.sourceBuffer.addEventListener('error', () => {
      this.fail(new Error(this.getMediaError(this.mediaElement) || `SourceBuffer error: ${mime}`));
    });
    this.mediaSource.addEventListener('sourceclose', () => {
      if (this.state !== 'destroyed') this.destroy(new Error('MediaSource closed'));
    });
  }

  append(data) {
    if (this.error) throw this.error;
    if (this.state !== 'running') {
      throw new DOMException(`SourceBuffer queue is ${this.state}`, 'InvalidStateError');
    }
    this.queue.push(data);
    this.queuedBytes += data.byteLength;
    this.pump();
  }

  pump() {
    if (this.error || this.state !== 'running' || this.sourceBuffer.updating) return;
    const mediaFailure = this.getMediaError(this.mediaElement);
    if (mediaFailure) {
      this.fail(new Error(mediaFailure));
      return;
    }
    if (this.mediaSource.readyState !== 'open' ||
        !Array.from(this.mediaSource.sourceBuffers).includes(this.sourceBuffer)) {
      this.destroy(new Error('SourceBuffer is detached'));
      return;
    }

    const ranges = this.bufferedRanges();
    if (this.trimBeforeTime !== null && ranges.length) {
      const requestedEnd = this.trimBeforeTime;
      const start = ranges[0].start;
      const removeEnd = Math.min(requestedEnd, ranges[ranges.length - 1].end);
      const force = this.forceTrim;
      this.trimBeforeTime = null;
      this.forceTrim = false;
      const minimumEnd = force ? start : start + this.trimGranularitySeconds;
      if (removeEnd > minimumEnd) {
        this.sourceBuffer.remove(start, removeEnd);
        return;
      }
    }
    if (!this.queue.length) return;
    if (this.bufferedAhead() >= this.forwardBufferHighSeconds) {
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
      this.recountQueuedBytes();
      if (error?.name === 'QuotaExceededError') {
        this.trimBefore(this.mediaElement.currentTime - this.backBufferSeconds, true);
        this.scheduleRetry();
      } else {
        this.fail(error);
      }
    }
  }

  bufferedAhead() {
    const ranges = this.bufferedRanges();
    const time = this.mediaElement.currentTime;
    for (const range of ranges) {
      if (range.start <= time + 0.1 && range.end >= time) return range.end - time;
    }
    return 0;
  }

  bufferedRanges() {
    try {
      return snapshotTimeRanges(this.sourceBuffer.buffered);
    } catch (error) {
      if (error?.name !== 'InvalidStateError') throw error;
      return snapshotTimeRanges(this.mediaElement.buffered);
    }
  }

  trimBefore(time, force = false) {
    if (!(time > 0) || this.state !== 'running') return;
    if (!force && this.trimBeforeTime === null) {
      const ranges = this.bufferedRanges();
      if (!ranges.length || time <= ranges[0].start + this.trimGranularitySeconds) return;
    }
    this.trimBeforeTime = this.trimBeforeTime === null ? time : Math.max(this.trimBeforeTime, time);
    this.forceTrim ||= force;
    this.scheduleRetry();
  }

  scheduleRetry() {
    if (this.retryTimer !== null || this.state !== 'running') return;
    this.retryTimer = setTimeout(() => {
      this.retryTimer = null;
      try {
        this.pump();
      } catch (error) {
        this.fail(error);
      }
    }, this.retryDelayMilliseconds);
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
    this.forceTrim = false;
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

  stop() {
    this.destroy();
  }

  destroy(error = new Error('SourceBuffer queue stopped')) {
    if (this.retryTimer !== null) clearTimeout(this.retryTimer);
    this.retryTimer = null;
    this.state = 'destroyed';
    this.error ||= error;
    this.queue = [];
    this.queuedBytes = 0;
    this.currentBytes = 0;
    this.trimBeforeTime = null;
    this.forceTrim = false;
    this.resolveWaiters();
  }

  recountQueuedBytes() {
    this.queuedBytes = this.currentBytes +
      this.queue.reduce((sum, item) => sum + item.byteLength, 0);
  }

  fail(error) {
    this.error = error instanceof Error ? error : new Error(String(error));
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

/** Waits for all queues and closes a MediaSource on a stable A/V boundary. */
export async function finalizeMseMediaSource(mediaSource, queues, options = {}) {
  await Promise.all(queues.map(queue => queue.waitIdle()));
  let truncatedTo = null;
  if (options.truncateToCommonEnd && queues.length > 1) {
    let common = queues[0].bufferedRanges();
    for (let index = 1; index < queues.length && common.length; index += 1) {
      common = intersectBufferedRanges(common, queues[index].bufferedRanges());
    }
    const commonEnd = common.at(-1)?.end;
    const minimumTruncationSeconds = options.minimumTruncationSeconds ?? 0.05;
    if (Number.isFinite(commonEnd) && Number.isFinite(mediaSource.duration) &&
        mediaSource.duration - commonEnd > minimumTruncationSeconds) {
      // Chromium rejects duration reduction while any coded frame still ends
      // beyond the requested value. Remove those tails first, then commit the
      // shorter duration once every SourceBuffer has completed its mutation.
      for (const queue of queues) {
        const ranges = queue.bufferedRanges();
        const bufferedEnd = ranges.at(-1)?.end;
        if (Number.isFinite(bufferedEnd) && bufferedEnd > commonEnd) {
          queue.sourceBuffer.remove(commonEnd, bufferedEnd);
        }
      }
      await Promise.all(queues.map(queue => queue.waitIdle()));
      mediaSource.duration = commonEnd;
      truncatedTo = commonEnd;
    }
  }
  if (mediaSource.readyState === 'open') mediaSource.endOfStream();
  return { truncatedTo };
}

export default MseAppendQueue;
