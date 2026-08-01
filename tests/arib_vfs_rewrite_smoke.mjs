import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';

const controllerSource = await readFile(
  new URL('../demo/data-broadcast.js', import.meta.url),
  'utf8',
);
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
this.usesWebKitMediaPlaneFallback = usesWebKitMediaPlaneFallback;
this.broadcastClockChanged = DataBroadcastController.prototype.broadcastClockChanged;
this.visibleApplication = DataBroadcastController.prototype.visibleApplication;
this.maintenanceApplication = DataBroadcastController.prototype.maintenanceApplication;
this.ensureResourceAvailable = DataBroadcastController.prototype.ensureResourceAvailable;
this.recoverApplicationFailure = DataBroadcastController.prototype.recoverApplicationFailure;
this.loadApplication = DataBroadcastController.prototype.loadApplication;
`, controllerContext);
assert.equal(controllerContext.usesWebKitMediaPlaneFallback(
  'Mozilla/5.0 (Macintosh) AppleWebKit/605.1.15 Version/18.6 Safari/605.1.15',
), true);
assert.equal(controllerContext.usesWebKitMediaPlaneFallback(
  'Mozilla/5.0 (Macintosh) AppleWebKit/537.36 Chrome/140.0.0.0 Safari/537.36',
), false);
assert.equal(controllerContext.usesWebKitMediaPlaneFallback(
  'Mozilla/5.0 (iPad) AppleWebKit/605.1.15 CriOS/140.0 Mobile/15E148 Safari/604.1',
), true);
const now = Date.UTC(2026, 6, 31, 12, 0, 0);
const program = controllerContext.createDemoProgramInfo(now);
assert.equal(program.duration, 30 * 60 * 1000);
assert.equal(program.f_duration, program.duration);
assert.equal(program.start_time.getTime(), now - 60 * 1000);
assert.equal(
  program.f_start_time.getTime(),
  program.start_time.getTime() + program.duration,
);
assert.equal(program.name, 'データ放送デモ');
assert.equal(program.f_name, 'データ放送デモ NEXT');
assert.equal(program.service_id, 0);

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

let ensuredPath = null;
const recoveryContext = {
  sessionGeneration: 3,
  vfsSession: {
    async ensure(path) { ensuredPath = path; },
  },
};
await controllerContext.ensureResourceAvailable.call(recoveryContext, 'sh8/index8k.html', 3);
assert.equal(ensuredPath, 'sh8/index8k.html');

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

console.log('data-broadcast controller smoke test passed');
