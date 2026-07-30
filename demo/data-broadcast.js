const KEYBOARD_KEYS = {
  ArrowLeft: 37, ArrowUp: 38, ArrowRight: 39, ArrowDown: 40,
  Enter: 13, Backspace: 461, d: 457, D: 457,
  r: 403, R: 403, g: 404, G: 404,
  y: 405, Y: 405, b: 406, B: 406,
  0: 48, 1: 49, 2: 50, 3: 51, 4: 52,
  5: 53, 6: 54, 7: 55, 8: 56, 9: 57,
};

export function createDemoProgramInfo(is8k, now = Date.now()) {
  const startTime = new Date(now - 60 * 1000);
  // MH-EIT is not decoded yet. Keep this clearly synthetic, but anchor it to
  // the recording clock and avoid the misleading same-time 24-hour range.
  const duration = 30 * 60 * 1000;
  const followingStartTime = new Date(startTime.getTime() + duration);
  const eventName = is8k ? 'BS8K' : 'BS4K';
  return {
    original_network_id: 4,
    transport_stream_id: 11,
    service_id: is8k ? 102 : 101,
    event_id: 1,
    name: eventName,
    event_name: eventName,
    start_time: startTime,
    duration,
    desc: '',
    event_text: '',
    event_extended_text: '',
    f_event_id: 2,
    f_name: `${eventName} NEXT`,
    f_start_time: followingStartTime,
    f_duration: duration,
    f_desc: '',
  };
}

function isBackgroundHoleApplication(pathname) {
  return /\/(?:caption)\/source\//.test(String(pathname));
}

class ServiceWorkerResourceBridge {
  constructor(url) {
    this.url = url;
    this.registration = null;
    this.ready = null;
  }

  initialize() {
    if (this.ready) return this.ready;
    this.ready = (async () => {
      if (!('serviceWorker' in navigator)) {
        throw new Error('このブラウザーは Service Worker に対応していません');
      }
      this.registration = await navigator.serviceWorker.register(this.url, {
        scope: '/',
        updateViaCache: 'none',
      });
      await this.registration.update();
      const pending = this.registration.installing || this.registration.waiting;
      if (pending && pending.state !== 'activated') {
        await new Promise((resolve, reject) => {
          const timeout = setTimeout(
            () => reject(new Error('VFS worker の更新が完了しません')),
            5000,
          );
          pending.addEventListener('statechange', () => {
            if (pending.state !== 'activated') return;
            clearTimeout(timeout);
            resolve();
          });
        });
      }
      await navigator.serviceWorker.ready;
      return this.registration;
    })();
    return this.ready;
  }

  async request(message, transfer = []) {
    const registration = await this.initialize();
    const worker = registration.active || registration.waiting || registration.installing;
    if (!worker) throw new Error('データ放送 VFS worker を開始できません');
    return new Promise((resolve, reject) => {
      const channel = new MessageChannel();
      const timeout = setTimeout(() => reject(new Error('VFS worker が応答しません')), 5000);
      channel.port1.onmessage = event => {
        clearTimeout(timeout);
        if (event.data?.ok) resolve(event.data);
        else reject(new Error(event.data?.error || 'VFS worker エラー'));
      };
      worker.postMessage(message, [channel.port2, ...transfer]);
    });
  }

  begin() {
    return this.request({ type: 'arib-vfs-begin' });
  }

  put(resource) {
    const bytes = resource.data instanceof Uint8Array
      ? resource.data
      : new Uint8Array(resource.data);
    const owned = bytes.byteOffset === 0 && bytes.byteLength === bytes.buffer.byteLength
      ? bytes : bytes.slice();
    return this.request({
      type: 'arib-vfs-put',
      path: resource.path,
      contentType: resource.contentType,
      data: owned.buffer,
    }, [owned.buffer]);
  }

  reset() {
    return this.request({ type: 'arib-vfs-reset' });
  }
}

export class DataBroadcastController {
  constructor({ viewport, videoSurface, mediaPlane, video, iframe, remote, status, detail, url }) {
    this.viewport = viewport;
    this.videoSurface = videoSurface;
    this.video = video;
    this.iframe = iframe;
    this.remote = remote;
    this.status = status;
    this.detail = detail;
    this.url = url;
    this.bridge = new ServiceWorkerResourceBridge('./arib-vfs-sw.js');
    this.pendingWrites = Promise.resolve();
    this.resourceQueue = [];
    this.resourceDrainScheduled = false;
    this.resourceSequence = 0;
    this.resourceSequenceByPath = new Map();
    this.completedResourceSequence = 0;
    this.resourceWaiters = [];
    this.sessionGeneration = 0;
    this.applicationLoadTimer = null;
    this.readyResourceCount = 0;
    this.visible = false;
    this.readyEntry = null;
    this.readyContextId = null;
    this.loadedEntry = null;
    this.is8k = false;
    this.showRequested = false;
    this.log = () => {};

    if (!window.ARIBHTML5?.AribReceiverHost) {
      throw new Error('libaribhtml5 SDK が読み込まれていません');
    }
    if (!window.ARIBHTML5?.BehindIframeMediaPlaneAdapter) {
      throw new Error('libaribhtml5 media-plane adapter が読み込まれていません');
    }
    Object.assign(mediaPlane.style, { position: 'relative', overflow: 'hidden' });
    Object.assign(video.style, {
      display: 'block', width: '100%', height: '100%', objectFit: 'contain', background: '#080b09',
    });
    const subtitleOverlay = mediaPlane.querySelector('.subtitle-overlay');
    if (subtitleOverlay) Object.assign(subtitleOverlay.style, {
      position: 'absolute', zIndex: '1', inset: '0', pointerEvents: 'none', overflow: 'hidden',
    });
    this.mediaPlaneAdapter = new window.ARIBHTML5.BehindIframeMediaPlaneAdapter({
      surface: videoSurface,
      keepVisible: false,
    });
    this.host = new window.ARIBHTML5.AribReceiverHost({
      iframe,
      viewport,
      mediaPlaneAdapter: this.mediaPlaneAdapter,
      onStatus: value => {
        this.setStatus(value);
        if (value === 'アプリケーション終了') this.setVisible(false);
      },
      onUrlChange: value => this.applicationUrlChanged(value),
      onMediaPlane: plane => {
        videoSurface.classList.toggle('broadcast-video-plane-visible', plane.visible);
        if (plane.visible && videoSurface.classList.contains('broadcast-background-hole')) {
          viewport.style.backgroundColor = '#000';
        }
      },
    });
    window.__ARIB_HTML5_INSTALL__ = target => this.installRuntimeCooperatively(target);
    window.addEventListener('keydown', this.handleKeyboard);
    this.remote.querySelectorAll('[data-arib-key]').forEach(button => {
      button.addEventListener('click', () => this.dispatchKey(Number(button.dataset.aribKey)));
    });
    this.setVisible(false);
    this.bridge.initialize().then(
      () => this.setStatus('データ放送待機中', 'WASM 仮想ファイル未生成'),
      error => this.setStatus('VFS 初期化失敗', error.message),
    );
  }

  setLogger(callback) {
    this.log = typeof callback === 'function' ? callback : () => {};
  }

  installRuntimeCooperatively(target) {
    if (isBackgroundHoleApplication(target.location.pathname)) {
      const style = target.document.createElement('style');
      style.dataset.tlvdemuxBackgroundHole = '';
      style.textContent = `
        html, body, #backscreen, #container, #vstream,
        #vstream object, #vstream [data-arib-type="video/x-arib2-broadcast"] {
          background: transparent !important;
        }
      `;
      (target.document.head || target.document.documentElement).append(style);
    }
    const NativeMutationObserver = target.MutationObserver;
    const nativeSetInterval = target.setInterval;
    const callSetInterval = nativeSetInterval.bind(target);
    target.MutationObserver = class CooperativeMutationObserver extends NativeMutationObserver {
      constructor(callback) {
        let timer = null;
        let latestRecords = [];
        let latestObserver = null;
        super((records, observer) => {
          latestRecords.push(...records);
          latestObserver = observer;
          if (timer !== null) return;
          timer = target.setTimeout(() => {
            timer = null;
            const pendingRecords = latestRecords;
            latestRecords = [];
            callback(pendingRecords, latestObserver);
          }, 250);
        });
      }

      observe(node, options = {}) {
        // libaribhtml5 used to poll getBoundingClientRect() every 100 ms to
        // notice video-plane changes. Observe layout-affecting attributes
        // instead, so the 4K decoder is not interrupted by forced reflows.
        if (options.childList && options.subtree && !options.attributes) {
          super.observe(node, {
            ...options,
            attributes: true,
            attributeFilter: [
              'style', 'class', 'hidden', 'type', 'data', 'value', 'width', 'height',
            ],
          });
          return;
        }
        super.observe(node, options);
      }
    };
    target.setInterval = (callback, delay, ...args) => {
      if (Number(delay) === 100) return 0;
      return callSetInterval(callback, delay, ...args);
    };
    try {
      this.host.installRuntime(target);
    } finally {
      target.MutationObserver = NativeMutationObserver;
      target.setInterval = nativeSetInterval;
    }
  }

  beginSession() {
    this.sessionGeneration += 1;
    if (this.applicationLoadTimer !== null) clearTimeout(this.applicationLoadTimer);
    this.applicationLoadTimer = null;
    this.resourceQueue = [];
    this.resourceSequenceByPath.clear();
    this.resourceDrainScheduled = false;
    this.completedResourceSequence = this.resourceSequence;
    this.resolveResourceWaiters();
    this.readyEntry = null;
    this.readyContextId = null;
    this.loadedEntry = null;
    this.host.clearBroadcastClock();
    this.readyResourceCount = 0;
    this.showRequested = false;
    this.url.textContent = '';
    this.setVisible(false);
    this.pendingWrites = this.bridge.begin();
    this.pendingWrites.catch(error => this.setStatus('VFS エラー', error.message));
    this.setStatus('データ放送を収集中', '0 ファイル');
  }

  broadcastClockChanged(clock) {
    const broadcastTimescale = Number(clock.broadcastTimeTimescale);
    const mediaTimescale = Number(clock.mediaTimeTimescale);
    if (!(broadcastTimescale > 0) || !(mediaTimescale > 0)) return;
    const ntpMilliseconds = Number(clock.broadcastTimeValue) * 1000 / broadcastTimescale;
    const unixMilliseconds = ntpMilliseconds - 2208988800 * 1000;
    const mediaTimeSeconds = Number(clock.mediaTimeValue) / mediaTimescale;
    if (!Number.isFinite(unixMilliseconds) || !Number.isFinite(mediaTimeSeconds)) return;
    this.host.setBroadcastClock({
      epochMilliseconds: unixMilliseconds,
      mediaTimeSeconds,
      currentMediaTimeSeconds: () => this.video.currentTime,
    });
    if (this.loadedEntry) {
      this.host.setProgramInfo(createDemoProgramInfo(this.is8k, this.host.getBroadcastTime()));
    }
  }

  resourceChanged(notification) {
    const sequence = ++this.resourceSequence;
    // onApplicationResourceView is callback-lifetime data. Copy it before the
    // callback returns; never retain or call back into the WASM demuxer from
    // the asynchronous VFS drain.
    const data = Uint8Array.from(notification.data);
    this.resourceSequenceByPath.set(
      `${notification.contextId}:${notification.path}`,
      sequence,
    );
    this.resourceQueue.push({
      sequence,
      sessionGeneration: this.sessionGeneration,
      resource: {
        path: notification.path,
        contentType: notification.contentType,
        data,
      },
    });
    this.scheduleResourceDrain();
    // entryReady can precede delivery of the entry resource. Start the
    // broadcast startup application as soon as its VFS write barrier exists.
    if (this.readyEntry === notification.path &&
        this.readyContextId === notification.contextId) {
      this.scheduleApplicationLoad(this.readyEntry, this.readyContextId, true);
    }
    if (this.showRequested) {
      const application = this.visibleApplication();
      if (application) this.showApplication(application.path, application.contextId);
    }
  }

  scheduleResourceDrain() {
    if (this.resourceDrainScheduled || !this.resourceQueue.length) return;
    this.resourceDrainScheduled = true;
    const run = () => {
      this.resourceDrainScheduled = false;
      return this.drainOneResource();
    };
    if (globalThis.scheduler?.postTask) {
      globalThis.scheduler.postTask(run, { priority: 'background' }).catch(error => {
        this.setStatus('VFS タスク失敗', error.message);
      });
    } else {
      setTimeout(run, 0);
    }
  }

  async drainOneResource() {
    const item = this.resourceQueue.shift();
    if (!item) return;
    try {
      if (item.sessionGeneration === this.sessionGeneration) {
        this.pendingWrites = this.pendingWrites
          .catch(() => undefined)
          .then(() => this.bridge.put(item.resource));
        await this.pendingWrites;
      }
    } catch (error) {
      this.setStatus('VFS 書き込み失敗', error.message);
    } finally {
      this.completedResourceSequence = Math.max(this.completedResourceSequence, item.sequence);
      this.resolveResourceWaiters();
      this.scheduleResourceDrain();
    }
  }

  waitForResources(sequence) {
    if (this.completedResourceSequence >= sequence) return Promise.resolve();
    return new Promise(resolve => this.resourceWaiters.push({ sequence, resolve }));
  }

  resolveResourceWaiters() {
    const pending = this.resourceWaiters;
    this.resourceWaiters = [];
    for (const waiter of pending) {
      if (this.completedResourceSequence >= waiter.sequence) waiter.resolve();
      else this.resourceWaiters.push(waiter);
    }
  }

  applicationStateChanged(demuxer, state) {
    this.detail.textContent = `context ${state.contextId} · ${state.resourceCount} ファイル`;
    if (!state.entryReady) {
      this.status.textContent = state.state === 'discovered'
        ? 'アプリケーション検出' : 'データ放送を収集中';
      return;
    }
    const entry = demuxer.applicationEntry(state.contextId);
    if (!entry) return;
    this.readyEntry = entry;
    this.readyContextId = state.contextId;
    this.readyResourceCount = state.resourceCount;
    this.status.textContent = 'データ放送準備完了';
    this.scheduleApplicationLoad(entry, state.contextId, true);
    if (this.showRequested) {
      const application = this.visibleApplication();
      if (application) this.showApplication(application.path, application.contextId);
    }
  }

  scheduleApplicationLoad(entry, contextId, immediate = false) {
    if (this.loadedEntry === entry) return;
    if (this.applicationLoadTimer !== null) {
      if (!immediate) return;
      clearTimeout(this.applicationLoadTimer);
    }
    const sessionGeneration = this.sessionGeneration;
    this.applicationLoadTimer = setTimeout(() => {
      this.applicationLoadTimer = null;
      const entryBarrier = this.resourceSequenceByPath.get(`${contextId}:${entry}`);
      if (entryBarrier === undefined) return;
      this.waitForResources(entryBarrier).then(() => {
        if (sessionGeneration !== this.sessionGeneration || this.readyEntry !== entry) return;
        this.loadApplication(`/${entry}`, false, contextId);
        this.loadedEntry = entry;
        this.log(`データ放送 バックグラウンド起動 /${entry} (${this.readyResourceCount} files)`);
      }).catch(error => this.setStatus('アプリケーション起動失敗', error.message));
    }, immediate ? 0 : 300);
  }

  resourcesReset() {
    this.beginSession();
  }

  loadApplication(path, visible, contextId = this.readyContextId) {
    const resolved = new URL(path, location.href);
    let decodedPath = '';
    try {
      decodedPath = decodeURIComponent(resolved.pathname);
    } catch {
      // Keep the empty value so malformed URL escapes fail the check below.
    }
    const resourcePath = decodedPath.slice(1);
    const resourceKey = `${contextId}:${resourcePath}`;
    if (resolved.origin !== location.origin || !this.resourceSequenceByPath.has(resourceKey)) {
      throw new Error(`許可されていないデータ放送 URL: ${resolved.href}`);
    }
    const is8k = resolved.pathname.startsWith('/sh8/');
    this.is8k = is8k;
    this.host.setProgramInfo(createDemoProgramInfo(is8k, this.host.getBroadcastTime()));
    this.host.loadApplication(resolved.href);
    this.visible = visible;
    this.viewport.classList.toggle('data-broadcast-visible', visible);
    this.setStatus(visible ? 'データ放送へ移動中' : 'データ放送準備完了', this.detail.textContent);
  }

  visibleApplication() {
    if (!this.readyEntry) return null;
    const serviceRoot = this.readyEntry.split('/', 1)[0];
    const candidates = [];
    for (const [key, sequence] of this.resourceSequenceByPath) {
      const separator = key.indexOf(':');
      const contextId = Number(key.slice(0, separator));
      const path = key.slice(separator + 1);
      if (!Number.isInteger(contextId) || !path.startsWith(`${serviceRoot}/`)) continue;
      if (!/\/top\/source\/index[^/]*\.html$/i.test(path)) continue;
      candidates.push({ contextId, path, sequence });
    }
    candidates.sort((left, right) => {
      const leftPreferred = /\/60\/[^/]+\/top\/source\//.test(left.path) ? 0 : 1;
      const rightPreferred = /\/60\/[^/]+\/top\/source\//.test(right.path) ? 0 : 1;
      return leftPreferred - rightPreferred || left.sequence - right.sequence;
    });
    return candidates[0] || null;
  }

  showApplication(entry, contextId = this.readyContextId) {
    if (!entry || contextId === null) return;
    const barrier = this.resourceSequenceByPath.get(`${contextId}:${entry}`);
    if (barrier === undefined) {
      this.setStatus('データ放送を準備中', this.detail.textContent);
      return;
    }
    if (this.applicationLoadTimer !== null) {
      clearTimeout(this.applicationLoadTimer);
      this.applicationLoadTimer = null;
    }
    const sessionGeneration = this.sessionGeneration;
    this.waitForResources(barrier).then(() => {
      if (!this.showRequested || sessionGeneration !== this.sessionGeneration) return;
      this.loadApplication(`/${entry}`, true, contextId);
      this.loadedEntry = entry;
      this.showRequested = false;
      this.log(`データ放送 表示ページ /${entry}`);
    }).catch(error => this.setStatus('アプリケーション起動失敗', error.message));
  }

  setVisible(visible) {
    this.visible = visible;
    if (!visible) this.host.exitApplication();
    this.viewport.classList.toggle('data-broadcast-visible', visible);
    if (!visible) {
      this.video.controls = true;
      this.videoSurface.classList.remove(
        'broadcast-video-plane-visible', 'broadcast-background-hole',
      );
    }
  }

  applicationUrlChanged(value) {
    this.url.textContent = value;
    const backgroundHole = isBackgroundHoleApplication(value);
    this.videoSurface.classList.toggle('broadcast-background-hole', backgroundHole);
    this.video.controls = !backgroundHole;
    if (backgroundHole) this.viewport.style.backgroundColor = '#000';
  }

  dispatchKey(code) {
    if (code === 457 && !this.visible) {
      this.showRequested = true;
      if (!this.readyEntry) {
        this.setStatus('データ放送を準備中', this.detail.textContent);
        return;
      }
      const application = this.visibleApplication();
      if (application) this.showApplication(application.path, application.contextId);
      else this.setStatus('データ放送を準備中', this.detail.textContent);
      return;
    }
    if (!this.visible) return;
    this.host.dispatchKey(code);
  }

  handleKeyboard = event => {
    if (event.target instanceof HTMLInputElement ||
        event.target instanceof HTMLSelectElement || event.target instanceof HTMLTextAreaElement) return;
    const code = KEYBOARD_KEYS[event.key];
    if (code === undefined || (!this.visible && code !== 457)) return;
    event.preventDefault();
    this.dispatchKey(code);
  };

  setStatus(status, detail) {
    this.status.textContent = status;
    if (detail !== undefined) this.detail.textContent = detail;
  }
}
