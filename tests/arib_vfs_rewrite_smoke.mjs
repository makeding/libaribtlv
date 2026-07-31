import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';

const persistentCacheEntries = new Map();
const cacheKey = request => typeof request === 'string' ? request : request.url;
const fakeCache = {
  async put(request, response) {
    persistentCacheEntries.set(cacheKey(request), response.clone());
  },
  async match(request) {
    return persistentCacheEntries.get(cacheKey(request))?.clone();
  },
  async keys() {
    return [...persistentCacheEntries.keys()].map(url => new Request(url));
  },
};
const fakeCaches = {
  async open() { return fakeCache; },
  async delete() {
    const existed = persistentCacheEntries.size > 0;
    persistentCacheEntries.clear();
    return existed;
  },
};

const source = await readFile(new URL('../demo/arib-vfs-sw.js', import.meta.url), 'utf8');
const controllerSource = await readFile(
  new URL('../demo/data-broadcast.js', import.meta.url),
  'utf8',
);
const context = {
  Map, Request, Response, TextDecoder, TextEncoder, URL, Uint8Array, setTimeout,
  caches: fakeCaches,
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
this.prefixRootAttributes = prefixRootAttributes;
this.prefixCssRootUrls = prefixCssRootUrls;
this.normalizePath = normalizePath;
this.putPath = path => resources.set(path, {});
this.uniqueBasenameMatch = uniqueBasenameMatch;
this.hasBroadcastRoot = hasBroadcastRoot;
this.hasResourceCandidate = hasResourceCandidate;
this.waitForPath = waitFor;
this.putResource = (path, resource) => {
  resources.set(path, resource);
  wake(path, resource);
};
this.beginPersistentSession = beginPersistentSession;
this.persistResource = persistResource;
this.restorePersistentResources = restorePersistentResources;
this.simulateWorkerRestart = () => {
  resources.clear();
  enabled = false;
  restorePromise = null;
};
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

assert.equal(
  context.prefixRootAttributes('<script src="/sh4/common.js"></script><a href=/40/top.html>'),
  '<script src="/data-broadcast/sh4/common.js"></script><a href=/data-broadcast/40/top.html>',
);
assert.equal(
  context.prefixRootAttributes('<a href="/data-broadcast/sh4/top.html">'),
  '<a href="/data-broadcast/sh4/top.html">',
);
assert.equal(
  context.prefixCssRootUrls('a{background:url("/sh4/a.png")} @import "/40/base.css";'),
  'a{background:url("/data-broadcast/sh4/a.png")} @import "/data-broadcast/40/base.css";',
);
assert.equal(context.normalizePath('/data-broadcast/sh4/a.html'), 'sh4/a.html');
assert.equal(context.normalizePath('/sh4/a.html'), 'sh4/a.html');

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

const pendingElimination = context.waitForPath(
  'sh8/60/001/top/source/elimination.html',
  1000,
);
context.putResource('sh8/70/001/msgerase/source/elimination.html', { marker: 1 });
const resolvedElimination = await pendingElimination;
assert.equal(resolvedElimination.path, 'sh8/70/001/msgerase/source/elimination.html');
assert.equal(resolvedElimination.resource.marker, 1);
assert.equal(
  context.hasResourceCandidate('/sh4/70/001/msgerase/source/elimination.html'),
  true,
);
assert.equal(context.hasResourceCandidate('/demo/demo.js'), false);

await context.beginPersistentSession();
await context.persistResource('sh8/60/001/top/source/index8k.html', {
  data: new Uint8Array(new TextEncoder().encode('<html>8K</html>')),
  contentType: 'text/html; charset=utf-8',
});
context.simulateWorkerRestart();
assert.equal(await context.restorePersistentResources(), true);
const restoredIndex = await context.waitForPath('sh8/60/001/top/source/index8k.html');
assert.equal(new TextDecoder().decode(restoredIndex.resource.data), '<html>8K</html>');
assert.equal(restoredIndex.resource.contentType, 'text/html; charset=utf-8');

const controllerContext = {
  Date,
  URL,
  location: {
    href: 'http://127.0.0.1:8000/demo/',
    origin: 'http://127.0.0.1:8000',
  },
};
vm.runInNewContext(`${controllerSource.replace(/^export /gm, '')}
this.createDemoProgramInfo = createDemoProgramInfo;
this.createProgramInfoFromEvents = createProgramInfoFromEvents;
this.broadcastClockChanged = DataBroadcastController.prototype.broadcastClockChanged;
this.visibleApplication = DataBroadcastController.prototype.visibleApplication;
this.maintenanceApplication = DataBroadcastController.prototype.maintenanceApplication;
this.ensureResourceAvailable = DataBroadcastController.prototype.ensureResourceAvailable;
this.recoverApplicationFailure = DataBroadcastController.prototype.recoverApplicationFailure;
this.loadApplication = DataBroadcastController.prototype.loadApplication;
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
  readyContextId: 1,
  resourceSequenceByPath: new Map([
    ['1:sh4/40/001/startup/html/index.html', 1],
    ['1:sh8/60/001/top/source/index8k.html', 2],
    ['1:sh4/70/001/msgerase/source/index.html', 3],
    ['1:sh4/60/001/top/source/index4k.html', 4],
  ]),
});
assert.equal(visibleApplication.contextId, 1);
assert.equal(visibleApplication.path, 'sh4/40/001/startup/html/index.html');

const maintenanceApplication = controllerContext.maintenanceApplication.call({
  readyContextId: 1,
  resourceSequenceByPath: new Map([
    ['2:sh8/60/001/top/maintenance/maintenance.html', 1],
    ['1:sh4/60/001/top/source/index4k.html', 2],
    ['1:sh4/60/001/top/maintenance/maintenance.html', 3],
  ]),
});
assert.equal(maintenanceApplication.contextId, 1);
assert.equal(maintenanceApplication.path, 'sh4/60/001/top/maintenance/maintenance.html');
assert.equal(maintenanceApplication.sequence, 3);
assert.equal(controllerContext.maintenanceApplication.call({
  readyContextId: 1,
  resourceSequenceByPath: new Map([
    ['1:sh4/60/001/top/source/index4k.html', 1],
  ]),
}), null);

let loadedApplication = null;
controllerContext.loadApplication.call({
  readyContextId: 1,
  resourceSequenceByPath: new Map([
    ['1:sh4/60/001/top/maintenance/maintenance.html', 3],
  ]),
  host: { loadApplication(value) { loadedApplication = value; } },
  viewport: { classList: { toggle() {} } },
  detail: { textContent: '' },
  updateProgramInfo() {},
  setStatus() {},
}, '/sh4/60/001/top/maintenance/maintenance.html', true, 1);
assert.equal(
  loadedApplication,
  'http://127.0.0.1:8000/data-broadcast/sh4/60/001/top/maintenance/maintenance.html',
);

const replayed = [];
let probeCount = 0;
const resourceMirror = new Map([
  ['sh8/index8k.html', { path: 'sh8/index8k.html', data: new Uint8Array([1]) }],
  ['sh8/top.js', { path: 'sh8/top.js', data: new Uint8Array([2]) }],
]);
const recoveryContext = {
  sessionGeneration: 3,
  pendingWrites: Promise.resolve(),
  resourceMirror,
  bridge: {
    async canRead() { return ++probeCount > 1; },
    async begin() { replayed.push('begin'); },
    async put(resource) { replayed.push(resource.path); },
  },
};
await controllerContext.ensureResourceAvailable.call(recoveryContext, 'sh8/index8k.html', 3);
assert.deepEqual(replayed, ['begin', 'sh8/index8k.html', 'sh8/top.js']);

let recoveredVisibility = null;
let recoveredStatus = null;
controllerContext.recoverApplicationFailure.call({
  applicationLoadTimer: null,
  showRequested: true,
  loadedEntry: 'sh8/index8k.html',
  setVisible(value) { recoveredVisibility = value; },
  setStatus(status, detail) { recoveredStatus = { status, detail }; },
}, 'VFS missing');
assert.equal(recoveredVisibility, false);
assert.equal(recoveredStatus.status, 'アプリケーション起動失敗');
assert.match(recoveredStatus.detail, /dデータで再試行できます/);

console.log('ARIB VFS HTML rewrite smoke test passed');
