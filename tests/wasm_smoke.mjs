import assert from 'node:assert/strict';
import { createRequire } from 'node:module';

const modulePath = process.argv[2];
assert.ok(modulePath, 'missing generated tlvdemux-wasm module path');

const require = createRequire(import.meta.url);
const createTlvDemuxModule = require(modulePath);
const module = await createTlvDemuxModule();
const errors = [];
const demuxer = new module.TlvDemuxer({
    onError: error => errors.push(error),
});

assert.equal(demuxer.push(new Uint8Array()), true);
assert.equal(demuxer.pushFromHeap(0, 0), true);
assert.equal(demuxer.pushFromHeap(module.HEAPU8.byteLength, 1), false);
demuxer.selectService(undefined);
demuxer.selectTrack('video', undefined);
demuxer.reposition(0n, true);
demuxer.reset();
demuxer.flush();
demuxer.delete();

assert.deepEqual(errors, []);
console.log('tlvdemux WASM smoke test passed');
