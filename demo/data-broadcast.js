const KEYBOARD_KEYS = {
  ArrowLeft: 37, ArrowUp: 38, ArrowRight: 39, ArrowDown: 40,
  Enter: 13, Backspace: 461, d: 457, D: 457,
  r: 403, R: 403, g: 404, G: 404,
  y: 405, Y: 405, b: 406, B: 406,
  0: 48, 1: 49, 2: 50, 3: 51, 4: 52,
  5: 53, 6: 54, 7: 55, 8: 56, 9: 57,
};
const VFS_PREFIX = '/data-broadcast/';

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

export function createProgramInfoFromEvents(present, following = null) {
  if (!present) return null;
  const startTime = Number(present.startTimeUnixMilliseconds);
  const durationSeconds = Number(present.durationSeconds);
  if (!Number.isFinite(startTime) || !(durationSeconds > 0)) return null;
  const result = {
    original_network_id: Number(present.originalNetworkId),
    transport_stream_id: Number(present.tlvStreamId),
    service_id: Number(present.serviceId),
    event_id: Number(present.eventId),
    name: String(present.title || ''),
    event_name: String(present.title || ''),
    start_time: new Date(startTime),
    duration: durationSeconds * 1000,
    desc: String(present.description || ''),
    event_text: String(present.description || ''),
    running_status: Number(present.runningStatus),
    free_ca_mode: Boolean(present.freeCaMode),
  };
  if (following) {
    const followingStartTime = Number(following.startTimeUnixMilliseconds);
    const followingDurationSeconds = Number(following.durationSeconds);
    if (Number.isFinite(followingStartTime) && followingDurationSeconds > 0) {
      Object.assign(result, {
        f_event_id: Number(following.eventId),
        f_name: String(following.title || ''),
        f_start_time: new Date(followingStartTime),
        f_duration: followingDurationSeconds * 1000,
        f_desc: String(following.description || ''),
      });
    }
  }
  return result;
}

function isBackgroundHoleApplication(pathname) {
  return /\/(?:caption)\/source\//.test(String(pathname));
}

export class DataBroadcastController {
  constructor({ viewport, videoSurface, mediaPlane, video, iframe, remote, status, detail, url }) {
    this.viewport = viewport;
    this.videoSurface = videoSurface;
    this.video = video;
    this.iframe = iframe;
    this.remote = remote;
    this.maintenanceButton = remote.querySelector('[data-maintenance-page]');
    this.status = status;
    this.detail = detail;
    this.url = url;
    if (!window.ARIBHTML5?.ServiceWorkerBroadcastVfs) {
      throw new Error('libaribhtml5 Service Worker VFS が読み込まれていません');
    }
    this.bridge = new window.ARIBHTML5.ServiceWorkerBroadcastVfs({
      workerUrl: '/libaribhtml5/arib-vfs-sw.js',
      baseUrl: VFS_PREFIX,
      uniqueBasenameFallback: true,
    });
    this.pendingWrites = Promise.resolve();
    this.resourceQueue = [];
    this.resourceDrainScheduled = false;
    this.resourceSequence = 0;
    this.resourceSequenceByPath = new Map();
    this.resourceMirror = new Map();
    this.completedResourceSequence = 0;
    this.resourceWaiters = [];
    this.sessionGeneration = 0;
    this.applicationLoadTimer = null;
    this.readyResourceCount = 0;
    this.htmlCatalogueSignatures = new Map();
    this.visible = false;
    this.readyEntry = null;
    this.readyContextId = null;
    this.loadedEntry = null;
    this.is8k = false;
    this.programEvents = new Map();
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
        if (value === 'ランタイム導入済み' && this.applicationLoadTimer !== null) {
          clearTimeout(this.applicationLoadTimer);
          this.applicationLoadTimer = null;
        } else if (value === 'ページ遷移中') {
          this.armApplicationLoadTimer('ページ遷移先のランタイムを確認できません');
        }
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
    window.__ARIB_HTML5_INSTALL__ = target => this.installRuntime(target);
    window.addEventListener('keydown', this.handleKeyboard);
    this.remote.querySelectorAll('[data-arib-key]').forEach(button => {
      button.addEventListener('click', () => this.dispatchKey(Number(button.dataset.aribKey)));
    });
    this.maintenanceButton?.addEventListener('click', () => this.showMaintenance());
    this.setVisible(false);
    this.bridge.initialize().then(
      () => this.setStatus('データ放送待機中', 'WASM 仮想ファイル未生成'),
      error => this.setStatus('VFS 初期化失敗', error.message),
    );
  }

  setLogger(callback) {
    this.log = typeof callback === 'function' ? callback : () => {};
  }

  installRuntime(target) {
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
    this.host.installRuntime(target);
  }

  beginSession() {
    this.sessionGeneration += 1;
    if (this.applicationLoadTimer !== null) clearTimeout(this.applicationLoadTimer);
    this.applicationLoadTimer = null;
    this.resourceQueue = [];
    this.resourceSequenceByPath.clear();
    this.resourceMirror.clear();
    this.resourceDrainScheduled = false;
    this.completedResourceSequence = this.resourceSequence;
    this.resolveResourceWaiters();
    this.readyEntry = null;
    this.readyContextId = null;
    this.loadedEntry = null;
    this.host.clearBroadcastClock();
    this.programEvents.clear();
    this.readyResourceCount = 0;
    this.htmlCatalogueSignatures.clear();
    this.showRequested = false;
    this.updateMaintenanceButton();
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
    if (this.loadedEntry) this.updateProgramInfo();
  }

  eventInformationChanged(event) {
    if (Number(event.tableId) !== 0x8b || !event.currentNext) return;
    const sectionNumber = Number(event.sectionNumber);
    if (sectionNumber !== 0 && sectionNumber !== 1) return;
    this.programEvents.set(sectionNumber, { ...event });
    this.updateProgramInfo();
  }

  updateProgramInfo() {
    const actual = createProgramInfoFromEvents(
      this.programEvents.get(0),
      this.programEvents.get(1),
    );
    this.host.setProgramInfo(
      actual ?? createDemoProgramInfo(this.is8k, this.host.getBroadcastTime()),
    );
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
    this.updateMaintenanceButton();
    const resource = {
      path: notification.path,
      contentType: notification.contentType,
      data,
    };
    this.resourceMirror.set(notification.path, resource);
    this.resourceQueue.push({
      sequence,
      sessionGeneration: this.sessionGeneration,
      resource,
    });
    if (notification.contextId === this.readyContextId && /\.html?$/i.test(notification.path)) {
      this.logHtmlCatalogue(notification.contextId, this.readyResourceCount);
    }
    this.scheduleResourceDrain();
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
    this.updateMaintenanceButton();
    this.logHtmlCatalogue(state.contextId, state.resourceCount);
    this.status.textContent = 'データ放送準備完了';
    if (this.showRequested) {
      const application = this.visibleApplication();
      if (application) this.showApplication(application.path, application.contextId);
    }
  }

  resourcesReset() {
    this.beginSession();
  }

  logHtmlCatalogue(contextId, resourceCount) {
    const prefix = `${contextId}:`;
    const paths = [...this.resourceSequenceByPath.keys()]
      .filter(key => key.startsWith(prefix))
      .map(key => key.slice(prefix.length))
      .filter(path => /\.html?$/i.test(path))
      .sort();
    const signature = paths.join('\n');
    if (this.htmlCatalogueSignatures.get(contextId) === signature) return;
    this.htmlCatalogueSignatures.set(contextId, signature);
    this.log(`VFS HTML context ${contextId}: ${paths.length} 件 / 全 ${resourceCount} ファイル`);
    for (const path of paths) this.log(`  /${path}`);
  }

  loadApplication(path, visible, contextId = this.readyContextId) {
    const requestedPath = String(path).replace(/^\/+/, '');
    const resolved = new URL(`${VFS_PREFIX}${requestedPath}`, location.origin);
    let decodedPath = '';
    try {
      decodedPath = decodeURIComponent(resolved.pathname);
    } catch {
      // Keep the empty value so malformed URL escapes fail the check below.
    }
    if (!decodedPath.startsWith(VFS_PREFIX)) {
      throw new Error(`許可されていないデータ放送 URL: ${resolved.href}`);
    }
    const resourcePath = decodedPath.slice(VFS_PREFIX.length);
    const resourceKey = `${contextId}:${resourcePath}`;
    if (resolved.origin !== location.origin || !this.resourceSequenceByPath.has(resourceKey)) {
      throw new Error(`許可されていないデータ放送 URL: ${resolved.href}`);
    }
    const is8k = resourcePath.startsWith('sh8/');
    this.is8k = is8k;
    this.updateProgramInfo();
    this.host.loadApplication(resolved.href);
    this.visible = visible;
    this.viewport.classList.toggle('data-broadcast-visible', visible);
    this.setStatus(visible ? 'データ放送へ移動中' : 'データ放送準備完了', this.detail.textContent);
  }

  visibleApplication() {
    if (!this.readyEntry || this.readyContextId === null) return null;
    const sequence = this.resourceSequenceByPath.get(
      `${this.readyContextId}:${this.readyEntry}`,
    );
    if (sequence === undefined) return null;
    // Start from the MH-AIT entry. Broadcaster startup code owns receiver-state
    // initialization (for example NHK's ureg63 sh4/sh8 media selection) and
    // then performs the transition to its visible top page.
    return {
      contextId: this.readyContextId,
      path: this.readyEntry,
      sequence,
    };
  }

  maintenanceApplication() {
    if (this.readyContextId === null) return null;
    const prefix = `${this.readyContextId}:`;
    const path = [...this.resourceSequenceByPath.keys()]
      .filter(key => key.startsWith(prefix))
      .map(key => key.slice(prefix.length))
      .filter(candidate => /(?:^|\/)maintenance\/maintenance\.html$/i.test(candidate))
      .sort((left, right) => left.length - right.length || left.localeCompare(right))[0];
    if (!path) return null;
    return {
      contextId: this.readyContextId,
      path,
      sequence: this.resourceSequenceByPath.get(`${this.readyContextId}:${path}`),
    };
  }

  updateMaintenanceButton() {
    if (this.maintenanceButton) {
      this.maintenanceButton.disabled = this.maintenanceApplication() === null;
    }
  }

  showMaintenance() {
    const application = this.maintenanceApplication();
    if (!application) {
      this.setStatus('メンテナンスページなし', '現在のデータ放送には収録されていません');
      return;
    }
    const sessionGeneration = this.sessionGeneration;
    if (this.applicationLoadTimer !== null) {
      clearTimeout(this.applicationLoadTimer);
      this.applicationLoadTimer = null;
    }
    this.setStatus('メンテナンスを準備中', this.detail.textContent);
    this.waitForResources(application.sequence).then(async () => {
      if (sessionGeneration !== this.sessionGeneration) return;
      await this.ensureResourceAvailable(application.path, sessionGeneration);
      if (sessionGeneration !== this.sessionGeneration) return;
      this.loadApplication(`/${application.path}`, true, application.contextId);
      this.loadedEntry = application.path;
      this.showRequested = false;
      this.armApplicationLoadTimer(`/${application.path} のランタイムを確認できません`, {
        sessionGeneration,
        loadedEntry: application.path,
      });
      this.log(`データ放送 メンテナンス /${application.path}`);
    }).catch(error => {
      if (sessionGeneration !== this.sessionGeneration) return;
      this.setVisible(false);
      this.setStatus('メンテナンス起動失敗', error.message);
    });
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
    this.waitForResources(barrier).then(async () => {
      if (!this.showRequested || sessionGeneration !== this.sessionGeneration) return;
      await this.ensureResourceAvailable(entry, sessionGeneration);
      if (!this.showRequested || sessionGeneration !== this.sessionGeneration) return;
      this.loadApplication(`/${entry}`, true, contextId);
      this.loadedEntry = entry;
      this.showRequested = false;
      this.armApplicationLoadTimer(`/${entry} のランタイムを確認できません`, {
        sessionGeneration,
        loadedEntry: entry,
      });
      this.log(`データ放送 表示ページ /${entry}`);
    }).catch(error => this.recoverApplicationFailure(error.message));
  }

  async ensureResourceAvailable(entry, sessionGeneration) {
    this.pendingWrites = this.pendingWrites
      .catch(() => undefined)
      .then(async () => {
        if (sessionGeneration !== this.sessionGeneration) return;
        if (await this.bridge.canRead(entry)) return;

        // A Service Worker VFS is memory-backed and may disappear when its
        // worker is replaced or reclaimed.  Rehydrate the current session from
        // the page-owned mirror before allowing the iframe to navigate.
        await this.bridge.begin();
        for (const resource of this.resourceMirror.values()) {
          if (sessionGeneration !== this.sessionGeneration) return;
          await this.bridge.put(resource);
        }
        if (sessionGeneration !== this.sessionGeneration) return;
        if (!await this.bridge.canRead(entry)) {
          throw new Error(`VFS に /${entry} がありません`);
        }
      });
    await this.pendingWrites;
  }

  recoverApplicationFailure(message) {
    if (this.applicationLoadTimer !== null) clearTimeout(this.applicationLoadTimer);
    this.applicationLoadTimer = null;
    this.showRequested = false;
    this.loadedEntry = null;
    this.setVisible(false);
    this.setStatus('アプリケーション起動失敗', `${message} · dデータで再試行できます`);
  }

  armApplicationLoadTimer(message, expected = {}) {
    if (this.applicationLoadTimer !== null) clearTimeout(this.applicationLoadTimer);
    const sessionGeneration = expected.sessionGeneration ?? this.sessionGeneration;
    const loadedEntry = expected.loadedEntry ?? this.loadedEntry;
    this.applicationLoadTimer = setTimeout(() => {
      this.applicationLoadTimer = null;
      if (sessionGeneration !== this.sessionGeneration || loadedEntry !== this.loadedEntry) return;
      this.recoverApplicationFailure(message);
    }, 5000);
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
