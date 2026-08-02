import assert from 'node:assert/strict';

const [debugBase = 'http://127.0.0.1:9335', targetSecondsText = '70', sourceUrl,
  playbackRateText, startTimeText] = process.argv.slice(2);
const targetSeconds = Number(targetSecondsText);
assert.ok(Number.isFinite(targetSeconds) && targetSeconds > 0, 'invalid target seconds');
const requestedPlaybackRate = playbackRateText === undefined ? null : Number(playbackRateText);
assert.ok(requestedPlaybackRate === null ||
  (Number.isFinite(requestedPlaybackRate) && requestedPlaybackRate > 0), 'invalid playback rate');
const requestedStartTime = startTimeText === undefined ? null : Number(startTimeText);
assert.ok(requestedStartTime === null ||
  (Number.isFinite(requestedStartTime) && requestedStartTime >= 0), 'invalid start time');

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

if (sourceUrl) {
  await evaluate(`localStorage.setItem('tlvdemux.demo.httpUrl', ${JSON.stringify(sourceUrl)})`);
}
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
if (requestedPlaybackRate !== null) {
  await evaluate(`document.getElementById('video').playbackRate = ${requestedPlaybackRate}`);
}
if (requestedStartTime !== null) {
  await evaluate(`document.getElementById('video').currentTime = ${requestedStartTime}`);
}

const deadline = Date.now() + Math.max(90000, targetSeconds * 2000);
let state;
while (Date.now() < deadline) {
  await new Promise(resolve => setTimeout(resolve, 1000));
  state = await evaluate(`(() => {
    const video = document.getElementById('video');
    ${requestedPlaybackRate === null ? '' : `video.playbackRate = ${requestedPlaybackRate};`}
    const error = video.error;
    const ranges = [];
    for (let i = 0; i < video.buffered.length; i++) ranges.push([video.buffered.start(i), video.buffered.end(i)]);
    return {
      currentTime: video.currentTime,
      paused: video.paused,
      playbackRate: video.playbackRate,
      error: error ? { code: error.code, message: error.message } : null,
      ranges,
      status: document.getElementById('probeState')?.textContent,
      queues: [...(globalThis.__tlvdemuxDebugQueues?.entries() || [])].map(([type, queue]) => ({
        type,
        state: queue.state,
        updating: queue.sourceBuffer.updating,
        queuedBytes: queue.queuedBytes,
        currentBytes: queue.currentBytes,
        queueLength: queue.queue.length,
        waiters: queue.waiters.length,
        retryPending: queue.retryTimer !== null,
        ahead: queue.bufferedAhead(),
        error: queue.error?.message || null,
      })),
      logTail: document.getElementById('log')?.textContent.split('\\n').slice(-8),
    };
  })()`);
  console.log(JSON.stringify(state));
  if (state.error) throw new Error(`media error at ${state.currentTime}s: ${state.error.message}`);
  if (state.currentTime >= targetSeconds) break;
}
socket.close();
assert.ok(state && state.currentTime >= targetSeconds,
  `playback did not reach ${targetSeconds}s: ${JSON.stringify(state)}`);
