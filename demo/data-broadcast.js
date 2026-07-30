const KEYBOARD_KEYS = {
  ArrowLeft: 37, ArrowUp: 38, ArrowRight: 39, ArrowDown: 40,
  Enter: 13, Backspace: 461, d: 457, D: 457,
  r: 403, R: 403, g: 404, G: 404,
  y: 405, Y: 405, b: 406, B: 406,
  0: 48, 1: 49, 2: 50, 3: 51, 4: 52,
  5: 53, 6: 54, 7: 55, 8: 56, 9: 57,
};

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
  constructor({ viewport, videoSurface, video, iframe, remote, status, detail, url }) {
    this.viewport = viewport;
    this.videoSurface = videoSurface;
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
    this.completedResourceSequence = 0;
    this.resourceWaiters = [];
    this.sessionGeneration = 0;
    this.applicationLoadTimer = null;
    this.readyResourceCount = 0;
    this.visible = false;
    this.readyEntry = null;
    this.loadedEntry = null;
    this.log = () => {};

    if (!window.ARIBHTML5?.AribReceiverHost) {
      throw new Error('libaribhtml5 SDK が読み込まれていません');
    }
    this.host = new window.ARIBHTML5.AribReceiverHost({
      iframe,
      viewport,
      videoSurface,
      keepVideoVisible: true,
      onStatus: value => this.setStatus(value),
      onUrlChange: value => { this.url.textContent = value; },
    });
    this.host.attachVideo(video);
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
    this.resourceDrainScheduled = false;
    this.completedResourceSequence = this.resourceSequence;
    this.resolveResourceWaiters();
    this.readyEntry = null;
    this.loadedEntry = null;
    this.readyResourceCount = 0;
    this.url.textContent = '';
    this.iframe.src = 'about:blank';
    this.setVisible(false);
    this.pendingWrites = this.bridge.begin();
    this.pendingWrites.catch(error => this.setStatus('VFS エラー', error.message));
    this.setStatus('データ放送を収集中', '0 ファイル');
  }

  resourceChanged(demuxer, notification) {
    const sequence = ++this.resourceSequence;
    this.resourceQueue.push({
      sequence,
      sessionGeneration: this.sessionGeneration,
      demuxer,
      contextId: notification.contextId,
      path: notification.path,
    });
    this.scheduleResourceDrain();
    if (this.readyEntry && this.loadedEntry !== this.readyEntry) {
      this.scheduleApplicationLoad(this.readyEntry);
    }
  }

  scheduleResourceDrain() {
    if (this.resourceDrainScheduled || !this.resourceQueue.length) return;
    this.resourceDrainScheduled = true;
    const run = () => {
      this.resourceDrainScheduled = false;
      return this.drainOneResource();
    };
    if (typeof globalThis.requestIdleCallback === 'function') {
      globalThis.requestIdleCallback(run, { timeout: 1000 });
    } else if (globalThis.scheduler?.postTask) {
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
        const resource = item.demuxer.applicationResource(item.contextId, item.path);
        if (resource) {
          this.pendingWrites = this.pendingWrites
            .catch(() => undefined)
            .then(() => this.bridge.put(resource));
          await this.pendingWrites;
        }
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
    this.readyResourceCount = state.resourceCount;
    this.scheduleApplicationLoad(entry);
  }

  scheduleApplicationLoad(entry) {
    if (this.loadedEntry === entry) return;
    if (this.applicationLoadTimer !== null) clearTimeout(this.applicationLoadTimer);
    const sessionGeneration = this.sessionGeneration;
    this.applicationLoadTimer = setTimeout(() => {
      this.applicationLoadTimer = null;
      const resourceBarrier = this.resourceSequence;
      this.waitForResources(resourceBarrier).then(() => this.pendingWrites).then(() => {
        if (sessionGeneration !== this.sessionGeneration || this.readyEntry !== entry) return;
        this.loadApplication(`/${entry}`);
        this.loadedEntry = entry;
        this.log(`データ放送 entry /${entry} (${this.readyResourceCount} files)`);
      }).catch(error => this.setStatus('アプリケーション起動失敗', error.message));
    }, 300);
  }

  resourcesReset() {
    this.beginSession();
  }

  loadApplication(path) {
    const resolved = new URL(path, location.href);
    if (resolved.origin !== location.origin ||
        !/^\/(?:sh[48]|[4567][012])\//.test(resolved.pathname)) {
      throw new Error(`許可されていないデータ放送 URL: ${resolved.href}`);
    }
    const is8k = resolved.pathname.startsWith('/sh8/');
    this.host.setProgramInfo({
      original_network_id: 4,
      transport_stream_id: 11,
      service_id: is8k ? 102 : 101,
      event_id: 1,
      event_name: is8k ? 'BS8K' : 'BS4K',
    });
    this.host.loadApplication(resolved.href);
    this.setVisible(true);
    this.setStatus('アプリケーション読込中', this.detail.textContent);
  }

  setVisible(visible) {
    this.visible = visible;
    this.viewport.classList.toggle('data-broadcast-visible', visible);
    this.remote.classList.toggle('disabled', !visible);
    this.remote.querySelectorAll('button').forEach(button => { button.disabled = !visible; });
    if (!visible) {
      Object.assign(this.videoSurface.style, {
        display: 'grid', left: '0%', top: '0%', width: '100%', height: '100%',
      });
    }
  }

  dispatchKey(code) {
    if (code === 457 && !this.visible && this.readyEntry) {
      this.setVisible(true);
    }
    if (!this.visible) return;
    this.host.dispatchKey(code);
  }

  handleKeyboard = event => {
    if (!this.visible || event.target instanceof HTMLInputElement ||
        event.target instanceof HTMLSelectElement || event.target instanceof HTMLTextAreaElement) return;
    const code = KEYBOARD_KEYS[event.key];
    if (code === undefined) return;
    event.preventDefault();
    this.dispatchKey(code);
  };

  setStatus(status, detail) {
    this.status.textContent = status;
    if (detail !== undefined) this.detail.textContent = detail;
  }
}
