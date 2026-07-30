import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';

const source = await readFile(new URL('../demo/arib-vfs-sw.js', import.meta.url), 'utf8');
const controllerSource = await readFile(
  new URL('../demo/data-broadcast.js', import.meta.url),
  'utf8',
);
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

for (const html of [
  '<audio src="romsound://9"></audio>',
  "<source src='romsound://7' type='audio/X-arib-romsound'>",
  '<source src=romsound://9 type=audio/X-arib-romsound>',
  '<SOURCE SRC = ROMSOUND://13/>',
]) {
  const sound = context.defer(html);
  assert.doesNotMatch(sound, /\ssrc\s*=/i);
  assert.match(sound, /data-arib-romsound="romsound:\/\/\d+"/i);
}
assert.equal(
  context.defer('<img src="https://example.test/romsound://9">'),
  '<img src="https://example.test/romsound://9">',
);

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

const controllerContext = { Date };
vm.runInNewContext(`${controllerSource.replace(/^export /gm, '')}
this.createDemoProgramInfo = createDemoProgramInfo;
this.createProgramInfoFromEvents = createProgramInfoFromEvents;
this.broadcastClockChanged = DataBroadcastController.prototype.broadcastClockChanged;
this.visibleApplication = DataBroadcastController.prototype.visibleApplication;
`, controllerContext);
const now = Date.UTC(2026, 6, 31, 12, 0, 0);
const program = controllerContext.createDemoProgramInfo(false, now);
assert.equal(program.duration, 30 * 60 * 1000);
assert.equal(program.f_duration, program.duration);
assert.equal(program.start_time.getTime(), now - 60 * 1000);
assert.equal(
  program.f_start_time.getTime(),
  program.start_time.getTime() + program.duration,
);
assert.equal(program.name, 'BS4K');
assert.equal(program.f_name, 'BS4K NEXT');

const actualProgram = controllerContext.createProgramInfoFromEvents({
  originalNetworkId: 4,
  tlvStreamId: 11,
  serviceId: 101,
  eventId: 2401,
  title: '録画された番組',
  description: '番組概要',
  startTimeUnixMilliseconds: Date.UTC(2026, 5, 26, 12, 0, 0),
  durationSeconds: 7200,
  runningStatus: 4,
  freeCaMode: false,
}, {
  eventId: 2402,
  title: '次の番組',
  description: '次番組概要',
  startTimeUnixMilliseconds: Date.UTC(2026, 5, 26, 14, 0, 0),
  durationSeconds: 3600,
});
assert.equal(actualProgram.event_name, '録画された番組');
assert.equal(actualProgram.start_time.getTime(), Date.UTC(2026, 5, 26, 12, 0, 0));
assert.equal(actualProgram.duration, 7200 * 1000);
assert.equal(actualProgram.f_name, '次の番組');
assert.equal(actualProgram.f_duration, 3600 * 1000);

let projectedClock = null;
controllerContext.broadcastClockChanged.call({
  host: {
    setBroadcastClock(value) { projectedClock = value; },
  },
  video: { currentTime: 12 },
  loadedEntry: null,
}, {
  broadcastTimeValue: 2208988800 + now / 1000,
  broadcastTimeTimescale: 1,
  mediaTimeValue: 10,
  mediaTimeTimescale: 1,
});
assert.equal(projectedClock.epochMilliseconds, now);
assert.equal(projectedClock.mediaTimeSeconds, 10);
assert.equal(projectedClock.currentMediaTimeSeconds(), 12);

const visibleApplication = controllerContext.visibleApplication.call({
  readyEntry: 'sh4/40/001/startup/html/index.html',
  resourceSequenceByPath: new Map([
    ['1:sh4/40/001/startup/html/index.html', 1],
    ['1:sh8/60/001/top/source/index8k.html', 2],
    ['1:sh4/70/001/msgerase/source/index.html', 3],
    ['1:sh4/60/001/top/source/index4k.html', 4],
  ]),
});
assert.equal(visibleApplication.contextId, 1);
assert.equal(visibleApplication.path, 'sh4/60/001/top/source/index4k.html');

console.log('ARIB VFS HTML rewrite smoke test passed');
