import assert from 'node:assert/strict';

import { coalesceReadableStream } from '../demo/live-stream.mjs';

function bytes(length, value) {
  return new Uint8Array(length).fill(value);
}

function arrayReader(values) {
  let index = 0;
  return {
    cancelled: false,
    released: false,
    async read() {
      if (index >= values.length) return { done: true, value: undefined };
      return { done: false, value: values[index++] };
    },
    async cancel() { this.cancelled = true; },
    releaseLock() { this.released = true; },
  };
}

const batchedReader = arrayReader([bytes(100, 1), bytes(100, 2), bytes(100, 3), bytes(50, 4)]);
const batched = [];
for await (const chunk of coalesceReadableStream(batchedReader, {
  targetBytes: 250,
  maxDelayMilliseconds: 1000,
})) {
  batched.push(chunk);
}
assert.deepEqual(batched.map(chunk => chunk.byteLength), [300, 50]);
assert.deepEqual([...batched[0].slice(95, 105)], [1, 1, 1, 1, 1, 2, 2, 2, 2, 2]);
assert.equal(batchedReader.cancelled, true);
assert.equal(batchedReader.released, true);

const large = bytes(300, 7);
const largeReader = arrayReader([large]);
const largeOutput = [];
for await (const chunk of coalesceReadableStream(largeReader, {
  targetBytes: 250,
  maxDelayMilliseconds: 1000,
})) {
  largeOutput.push(chunk);
}
assert.equal(largeOutput.length, 1);
assert.equal(largeOutput[0], large);

let resolveSecondRead;
const delayedReader = {
  reads: 0,
  cancelled: false,
  released: false,
  read() {
    this.reads += 1;
    if (this.reads === 1) return Promise.resolve({ done: false, value: bytes(64, 9) });
    if (this.reads === 2) {
      return new Promise(resolve => { resolveSecondRead = resolve; });
    }
    return Promise.resolve({ done: true, value: undefined });
  },
  async cancel() { this.cancelled = true; },
  releaseLock() { this.released = true; },
};
const delayed = coalesceReadableStream(delayedReader, {
  targetBytes: 256,
  maxDelayMilliseconds: 5,
});
const first = await delayed.next();
assert.equal(first.done, false);
assert.equal(first.value.byteLength, 64);
resolveSecondRead({ done: false, value: bytes(32, 8) });
const second = await delayed.next();
assert.equal(second.done, false);
assert.equal(second.value.byteLength, 32);
const end = await delayed.next();
assert.equal(end.done, true);
assert.equal(delayedReader.cancelled, true);
assert.equal(delayedReader.released, true);

await assert.rejects(
  async () => {
    for await (const _ of coalesceReadableStream(arrayReader([]), { targetBytes: 0 })) {}
  },
  /targetBytes/,
);

console.log('live stream coalescing tests passed');
