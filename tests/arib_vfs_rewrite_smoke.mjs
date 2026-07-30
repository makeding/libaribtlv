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
vm.runInNewContext(`${source}
this.rewrite = rewriteBroadcastObjects;
this.defer = deferRomSounds;
this.putPath = path => resources.set(path, {});
this.uniqueBasenameMatch = uniqueBasenameMatch;
this.hasBroadcastRoot = hasBroadcastRoot;
`, context);

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

context.putPath('sh4/60/001/top/source/index4k.html');
assert.equal(
  context.uniqueBasenameMatch('sh4/70/001/msgerase/source/index4k.html'),
  'sh4/60/001/top/source/index4k.html',
);
context.putPath('sh4/40/001/startup/html/index.html');
context.putPath('sh4/40/002/startup/html/index.html');
assert.equal(context.uniqueBasenameMatch('sh4/70/001/msgerase/source/index.html'), null);

context.putPath('bsfuji4k/40/0000/html/index.html');
assert.equal(context.hasBroadcastRoot('/bsfuji4k/40/0000/html/index.html'), true);
assert.equal(context.hasBroadcastRoot('/bsfuji4k/40/0000/css/main.css'), true);
assert.equal(context.hasBroadcastRoot('/demo/demo.js'), false);

console.log('ARIB VFS HTML rewrite smoke test passed');
