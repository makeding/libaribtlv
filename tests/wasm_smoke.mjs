import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { resolve } from 'node:path';

const modulePathArgument = process.argv[2];
assert.ok(modulePathArgument, 'missing generated tlvdemux-wasm module path');
const modulePath = resolve(modulePathArgument);

const require = createRequire(import.meta.url);
const createTlvDemuxModule = require(modulePath);
const module = await createTlvDemuxModule();
const errors = [];
const demuxer = new module.TlvDemuxer({
    onError: error => errors.push(error),
});

demuxer.startIndex(false);
assert.equal(demuxer.indexState(), 'building');
assert.equal(demuxer.setIndexDuration(1000000n), true);
assert.deepEqual(demuxer.indexDuration(), {
    value: 1000000n,
    timescale: 1000000,
    status: 'provisional',
});
assert.equal(demuxer.push(new Uint8Array()), true);
assert.equal(demuxer.pushFromHeap(0, 0), true);
assert.equal(demuxer.pushFromHeap(module.HEAPU8.byteLength, 1), false);
demuxer.selectService(undefined);
demuxer.selectTrack('video', undefined);
demuxer.setSubtitlePassthroughEnabled(true);
demuxer.setSubtitlePassthroughEnabled(false);
assert.equal(demuxer.indexDuration(), null);
assert.equal(demuxer.setIndexDuration(1000000n), true);
demuxer.reposition(0n, true);
demuxer.reset();
assert.equal(demuxer.indexDuration(), null);
assert.equal(demuxer.setIndexDuration(1000000n), true);
demuxer.flush();
assert.equal(demuxer.finalizeIndex(), true);
assert.equal(demuxer.indexState(), 'complete');
assert.deepEqual(demuxer.indexDuration(), {
    value: 1000000n,
    timescale: 1000000,
    status: 'complete',
});
assert.equal(demuxer.seekPointCount(), 0);
assert.equal(demuxer.previousSync(0n), null);
assert.equal(demuxer.seekPointsFor(0n), null);
assert.equal(demuxer.estimateOffset(0n, 1n), 0n);
demuxer.delete();

assert.deepEqual(errors, []);

const probe = new module.DurationProbe();
assert.equal(probe.begin(16n, { initialRangeSize: 4n, maxRangeSize: 8n }), true);
let range = probe.nextRange();
assert.deepEqual(
    { offset: range.offset, length: range.length },
    { offset: 0n, length: 4n }
);
assert.equal(probe.pushRange(range.requestId, range.offset, new Uint8Array(4), true), true);
range = probe.nextRange();
assert.deepEqual(
    { offset: range.offset, length: range.length },
    { offset: 4n, length: 4n }
);
assert.equal(probe.pushRange(range.requestId, range.offset, new Uint8Array(4), true), true);
assert.equal(probe.state(), 'unknown');
assert.equal(probe.failure(), 'no-video');
assert.equal(probe.duration(), null);
assert.equal(probe.transferredBytes(), 8n);
probe.delete();

console.log('tlvdemux WASM smoke test passed');
