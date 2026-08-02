import assert from 'node:assert/strict';

const [debugBase = 'http://127.0.0.1:9335', bundleUrl, sourceUrl,
  targetSecondsText = '60', playbackRateText = '1', startTimeText] = process.argv.slice(2);
assert.ok(bundleUrl && sourceUrl, 'bundle and source URLs are required');
const targetSeconds = Number(targetSecondsText);
const playbackRate = Number(playbackRateText);
const startTime = startTimeText === undefined ? null : Number(startTimeText);

const targets = await (await fetch(`${debugBase}/json`)).json();
const target = targets.find(item => item.type === 'page');
assert.ok(target?.webSocketDebuggerUrl, 'browser page is not available through CDP');
const socket = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((resolve, reject) => {
  socket.addEventListener('open', resolve, {once: true});
  socket.addEventListener('error', reject, {once: true});
});

let nextId = 1;
const pending = new Map();
socket.addEventListener('message', event => {
  const message = JSON.parse(event.data);
  if (!message.id || !pending.has(message.id)) return;
  const handler = pending.get(message.id);
  pending.delete(message.id);
  if (message.error) handler.reject(new Error(message.error.message));
  else handler.resolve(message.result);
});
const call = (method, params = {}) => new Promise((resolve, reject) => {
  const id = nextId++;
  pending.set(id, {resolve, reject});
  socket.send(JSON.stringify({id, method, params}));
});
const evaluate = async (expression, awaitPromise = false) => {
  const result = await call('Runtime.evaluate', {expression, awaitPromise, returnByValue: true});
  if (result.exceptionDetails) throw new Error(result.exceptionDetails.text);
  return result.result.value;
};

await evaluate(`(async () => {
  globalThis.__dplayerTlv?.destroy();
  document.body.innerHTML = '<div id="dplayer-tlv-smoke" style="width:1280px;height:720px"></div>';
  await new Promise((resolve, reject) => {
    const script = document.createElement('script');
    script.src = ${JSON.stringify(bundleUrl)};
    script.onload = resolve;
    script.onerror = reject;
    document.head.append(script);
  });
  globalThis.__dplayerTlv = new DPlayer({
    container: document.getElementById('dplayer-tlv-smoke'),
    autoplay: true,
    video: {url: ${JSON.stringify(sourceUrl)}, type: 'tlv'},
    pluginOptions: {tlv: {forwardBufferSeconds: 15, backBufferSeconds: 8}},
  });
  globalThis.__dplayerTlv.video.muted = true;
  void globalThis.__dplayerTlv.video.play();
  await new Promise(resolve => setTimeout(resolve, 2000));
  ${startTime === null ? '' : `globalThis.__dplayerTlv.seek(${startTime}, true);`}
  return true;
})()`, true);

const deadline = Date.now() + Math.max(90000, Math.abs(targetSeconds - (startTime ?? 0)) * 3000);
let state;
while (Date.now() < deadline) {
  await new Promise(resolve => setTimeout(resolve, 1000));
  state = await evaluate(`(() => {
    const video = globalThis.__dplayerTlv.video;
    video.playbackRate = ${playbackRate};
    if (video.paused && !video.ended && !video.error) void video.play();
    const ranges = [];
    for (let i = 0; i < video.buffered.length; i++) ranges.push([video.buffered.start(i), video.buffered.end(i)]);
    return {
      currentTime: video.currentTime,
      duration: video.duration,
      ended: video.ended,
      paused: video.paused,
      error: video.error ? {code: video.error.code, message: video.error.message} : null,
      ranges,
    };
  })()`);
  console.log(JSON.stringify(state));
  if (state.error) throw new Error(`DPlayer media error at ${state.currentTime}s: ${state.error.message}`);
  if (state.currentTime >= targetSeconds || state.ended) break;
}
socket.close();
assert.ok(state && (state.currentTime >= targetSeconds || state.ended),
  `DPlayer playback did not reach ${targetSeconds}s: ${JSON.stringify(state)}`);
