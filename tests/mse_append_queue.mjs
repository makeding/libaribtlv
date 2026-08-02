import assert from 'node:assert/strict';

class FakeTimeRanges {
  constructor(ranges = []) {
    this.ranges = ranges;
  }
  get length() { return this.ranges.length; }
  start(index) { return this.ranges[index][0]; }
  end(index) { return this.ranges[index][1]; }
}

class FakeSourceBuffer extends EventTarget {
  constructor(ranges = []) {
    super();
    this.mode = 'segments';
    this.updating = false;
    this.buffered = new FakeTimeRanges(ranges);
    this.appendFailures = [];
    this.removeCalls = [];
  }
  appendBuffer() {
    const failure = this.appendFailures.shift();
    if (failure) throw failure;
    this.updating = true;
  }
  remove(start, end) {
    if (!(end > start)) throw new TypeError(`invalid remove range ${start}..${end}`);
    this.removeCalls.push([start, end]);
    this.updating = true;
  }
  complete() {
    this.updating = false;
    this.dispatchEvent(new Event('updateend'));
  }
}

class FakeMediaSource extends EventTarget {
  static isTypeSupported() { return true; }
  constructor(sourceBuffer) {
    super();
    this.readyState = 'open';
    this.sourceBuffers = [];
    this.sourceBuffer = sourceBuffer;
  }
  addSourceBuffer() {
    this.sourceBuffers.push(this.sourceBuffer);
    return this.sourceBuffer;
  }
}

globalThis.MediaSource = FakeMediaSource;
const {MseAppendQueue} = await import('../mse-append-queue.mjs');

async function tick() {
  await new Promise(resolve => setTimeout(resolve, 5));
}

{
  const sourceBuffer = new FakeSourceBuffer();
  const mediaSource = new FakeMediaSource(sourceBuffer);
  const media = {currentTime: 0, error: null, buffered: new FakeTimeRanges()};
  const queue = new MseAppendQueue(mediaSource, media, 'video/mp4', null, {
    forwardBufferHighSeconds: Infinity,
  });
  queue.append(new Uint8Array(5 * 1024 * 1024));
  const unblocked = queue.waitBelow(4 * 1024 * 1024);

  // Model an abort/remove updateend after ownership of the previous append was
  // cleared. Subtracting currentBytes here would leave a phantom 5 MiB queued.
  queue.currentBytes = 0;
  sourceBuffer.complete();
  await unblocked;
  assert.equal(queue.queuedBytes, 0);
  assert.equal(queue.queue.length, 0);
}

{
  const sourceBuffer = new FakeSourceBuffer([[17.218, 30]]);
  sourceBuffer.appendFailures.push(new DOMException('quota', 'QuotaExceededError'));
  const mediaSource = new FakeMediaSource(sourceBuffer);
  const media = {currentTime: 17.73278, error: null, buffered: sourceBuffer.buffered};
  const queue = new MseAppendQueue(mediaSource, media, 'video/mp4', null, {
    backBufferSeconds: 8,
    forwardBufferHighSeconds: Infinity,
    retryDelayMilliseconds: 0,
  });

  queue.append(new Uint8Array(1024));
  await tick();
  assert.equal(queue.error, null);
  assert.deepEqual(sourceBuffer.removeCalls, []);
  assert.equal(sourceBuffer.updating, true);
  sourceBuffer.complete();
  await queue.waitIdle();
  assert.equal(queue.queuedBytes, 0);
}

console.log('mse append queue regression tests passed');
