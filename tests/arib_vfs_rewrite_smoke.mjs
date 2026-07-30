import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';

const source = await readFile(new URL('../demo/arib-vfs-sw.js', import.meta.url), 'utf8');
const context = {
  Map, Response, TextDecoder, TextEncoder, URL, Uint8Array, setTimeout,
  self: {
    location: { origin: 'http://127.0.0.1:8000' },
    clients: { claim() {} },
    skipWaiting() {},
    addEventListener() {},
  },
};
vm.runInNewContext(`${source}\nthis.rewrite = rewriteBroadcastObjects; this.defer = deferRomSounds;`, context);

for (const html of [
  '<object id="video" type="video/x-arib2-broadcast" data=""></object>',
  "<object data='' TYPE='video/x-arib2-broadcast'><param name='video_src'></object>",
  '<object type=video/x-arib2-broadcast/>',
]) {
  const rewritten = context.rewrite(html);
  assert.match(rewritten, /<object\b/i);
  assert.doesNotMatch(rewritten, /\stype\s*=/i);
  assert.match(rewritten, /data-arib-type="video\/x-arib2-broadcast"/);
}

const sound = context.defer('<audio src="romsound://9"></audio>');
assert.doesNotMatch(sound, /\ssrc=/i);
assert.match(sound, /data-arib-romsound="romsound:\/\/9"/);

console.log('ARIB VFS HTML rewrite smoke test passed');
