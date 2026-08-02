import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';

const [debugBase = 'http://127.0.0.1:9335', sourceUrl = '8k.mmts',
  outputDir = '/tmp/tlvdemux-edge-color'] = process.argv.slice(2);
const captureTimes = [0.5, 1, 2, 4, 6, 8, 10];

await mkdir(outputDir, { recursive: true });

const targets = await (await fetch(`${debugBase}/json`)).json();
const target = targets.find(item => item.type === 'page' && item.url.includes('/demo/'));
assert.ok(target?.webSocketDebuggerUrl, 'demo page is not available through CDP');

const socket = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((resolve, reject) => {
  socket.addEventListener('open', resolve, { once: true });
  socket.addEventListener('error', reject, { once: true });
});

let nextId = 1;
const pending = new Map();
socket.addEventListener('message', event => {
  const message = JSON.parse(event.data);
  if (!message.id) return;
  const handler = pending.get(message.id);
  if (!handler) return;
  pending.delete(message.id);
  if (message.error) handler.reject(new Error(message.error.message));
  else handler.resolve(message.result);
});
const call = (method, params = {}) => new Promise((resolve, reject) => {
  const id = nextId++;
  pending.set(id, { resolve, reject });
  socket.send(JSON.stringify({ id, method, params }));
});
const evaluate = async (expression, awaitPromise = false) => {
  const result = await call('Runtime.evaluate', {
    expression, awaitPromise, returnByValue: true,
  });
  if (result.exceptionDetails) throw new Error(result.exceptionDetails.text);
  return result.result.value;
};

await evaluate(`localStorage.setItem('tlvdemux.demo.httpUrl', ${JSON.stringify(sourceUrl)})`);
await evaluate(`(() => {
  const url = new URL(location.href);
  url.searchParams.set('tlvdemuxDebug', '1');
  history.replaceState(null, '', url);
})()`);
await call('Page.enable');
await call('Page.reload', { ignoreCache: true });
await new Promise(resolve => setTimeout(resolve, 500));
await evaluate(`new Promise((resolve, reject) => {
  const deadline = performance.now() + 15000;
  const start = () => {
    const button = document.getElementById('probeButton');
    if (button && !button.disabled) { button.click(); resolve(true); return; }
    if (performance.now() >= deadline) { reject(new Error('play button stayed disabled')); return; }
    setTimeout(start, 50);
  };
  start();
})`, true);

const captures = [];
for (const captureTime of captureTimes) {
  const deadline = Date.now() + 30000;
  let state;
  while (Date.now() < deadline) {
    await new Promise(resolve => setTimeout(resolve, 40));
    state = await evaluate(`(() => {
      const video = document.getElementById('video');
      video.playbackRate = 1;
      const quality = video.getVideoPlaybackQuality?.();
      return {
        currentTime: video.currentTime,
        readyState: video.readyState,
        paused: video.paused,
        error: video.error ? { code: video.error.code, message: video.error.message } : null,
        totalVideoFrames: quality?.totalVideoFrames ?? null,
        droppedVideoFrames: quality?.droppedVideoFrames ?? null,
      };
    })()`);
    assert.equal(state.error, null, `media error before ${captureTime}s`);
    if (state.currentTime >= captureTime && state.readyState >= 2) break;
  }
  assert.ok(state?.currentTime >= captureTime,
    `playback did not reach ${captureTime}s: ${JSON.stringify(state)}`);

  const frame = await evaluate(`(() => {
    const video = document.getElementById('video');
    video.pause();
    const rect = video.getBoundingClientRect();
    return {
      currentTime: video.currentTime,
      x: rect.x,
      y: rect.y,
      width: rect.width,
      height: rect.height,
      videoWidth: video.videoWidth,
      videoHeight: video.videoHeight,
    };
  })()`);
  const screenshot = await call('Page.captureScreenshot', {
    format: 'png',
    fromSurface: true,
    captureBeyondViewport: true,
    clip: { x: frame.x, y: frame.y, width: frame.width, height: frame.height, scale: 1 },
  });
  const filename = `frame-${String(captureTime).replace('.', '_')}s.png`;
  await writeFile(path.join(outputDir, filename), Buffer.from(screenshot.data, 'base64'));
  captures.push({ requestedTime: captureTime, filename, ...state, ...frame });
  await evaluate(`document.getElementById('video').play()` , true);
}

const finalState = await evaluate(`(() => {
  const video = document.getElementById('video');
  const quality = video.getVideoPlaybackQuality?.();
  return {
    currentTime: video.currentTime,
    error: video.error ? { code: video.error.code, message: video.error.message } : null,
    totalVideoFrames: quality?.totalVideoFrames ?? null,
    droppedVideoFrames: quality?.droppedVideoFrames ?? null,
    logTail: document.getElementById('log')?.textContent.split('\\n').slice(-10),
  };
})()`);
assert.equal(finalState.error, null, 'media error after the 10-second capture');

const report = { sourceUrl, captureTimes, captures, finalState };
await writeFile(path.join(outputDir, 'report.json'), `${JSON.stringify(report, null, 2)}\n`);
console.log(JSON.stringify(report));
socket.close();
