const protocol = globalThis.TlvDemuxWorkerProtocol;

function remoteError(value) {
  const error = new Error(value?.message || 'tlvdemux worker failed');
  error.name = value?.name || 'Error';
  if (value?.stack) error.stack = value.stack;
  return error;
}

class WorkerClient {
  constructor({ workerUrl, wasmUrl }) {
    this.worker = new Worker(workerUrl);
    this.nextRequestId = 1;
    this.nextObjectId = 1;
    this.pending = new Map();
    this.callbacks = new Map();
    this.entries = new Map();
    this.worker.onmessage = event => this.receive(event.data);
    this.worker.onerror = event => this.failAll(
      new Error(event.message || 'tlvdemux worker crashed'));
    this.ready = this.request(protocol.init, { wasmUrl });
  }

  receive(message) {
    if (message.type === protocol.event) {
      if (message.name === 'onApplicationState' && message.value?.applicationEntry) {
        this.entries.set(`${message.objectId}:${message.value.contextId}`,
          message.value.applicationEntry);
      }
      const callback = this.callbacks.get(message.objectId)?.[message.name];
      if (typeof callback === 'function') callback(message.value);
      return;
    }
    const pending = this.pending.get(message.requestId);
    if (!pending) return;
    this.pending.delete(message.requestId);
    if (message.type === protocol.failure) pending.reject(remoteError(message.error));
    else pending.resolve(message.value ?? true);
  }

  failAll(error) {
    for (const pending of this.pending.values()) pending.reject(error);
    this.pending.clear();
  }

  request(type, fields = {}, transfer = []) {
    const requestId = this.nextRequestId++;
    const promise = new Promise((resolve, reject) => {
      this.pending.set(requestId, { resolve, reject });
    });
    this.worker.postMessage({ type, requestId, ...fields }, transfer);
    return promise;
  }

  async create(objectType, callbacks = null, options = {}) {
    await this.ready;
    const objectId = this.nextObjectId++;
    if (callbacks) this.callbacks.set(objectId, callbacks);
    await this.request(protocol.create, { objectId, objectType, options });
    return objectId;
  }

  async invoke(objectId, method, args = [], transfer = []) {
    await this.ready;
    return this.request(protocol.invoke, { objectId, method, args }, transfer);
  }

  destroy(objectId) {
    this.callbacks.delete(objectId);
    for (const key of this.entries.keys()) {
      if (key.startsWith(`${objectId}:`)) this.entries.delete(key);
    }
    void this.request(protocol.destroy, { objectId });
  }

  applicationEntry(objectId, contextId) {
    return this.entries.get(`${objectId}:${contextId}`) ?? null;
  }

  close() {
    this.worker.terminate();
    this.failAll(new DOMException('tlvdemux worker closed', 'AbortError'));
  }
}

class WorkerObject {
  constructor(client, objectType, callbacks = null, options = {}) {
    this.client = client;
    this.objectId = null;
    this.closed = false;
    this.ready = client.create(objectType, callbacks, options).then(objectId => {
      this.objectId = objectId;
      return objectId;
    });
  }

  async call(method, args = [], transfer = []) {
    const objectId = await this.ready;
    if (this.closed) throw new DOMException('worker object is closed', 'InvalidStateError');
    return this.client.invoke(objectId, method, args, transfer);
  }

  delete() {
    if (this.closed) return;
    this.closed = true;
    void this.ready.then(objectId => this.client.destroy(objectId));
  }
}

class WorkerDurationProbe extends WorkerObject {
  constructor(client) { super(client, 'duration-probe'); }
  begin(size, options) { return this.call('begin', [size, options]); }
  nextRange() { return this.call('nextRange'); }
  failRange(requestId) { return this.call('failRange', [requestId]); }
  cancel() { return this.call('cancel'); }
  state() { return this.call('state'); }
  failure() { return this.call('failure'); }
  duration() { return this.call('duration'); }
  transferredBytes() { return this.call('transferredBytes'); }
  pushRange(requestId, offset, bytes, endOfRange) {
    const data = bytes.byteOffset === 0 && bytes.byteLength === bytes.buffer.byteLength
      ? bytes : bytes.slice();
    return this.call('pushRange', [requestId, offset, data, endOfRange], [data.buffer]);
  }
}

class WorkerDemuxer extends WorkerObject {
  constructor(client, callbacks) {
    super(client, 'demuxer', callbacks, {
      mseMaxAudioChannels: callbacks?.mseMaxAudioChannels || 0,
    });
  }

  configureTrackSelection(options) {
    return this.call('configureTrackSelection', [options]);
  }
  push(bytes) {
    const data = bytes.byteOffset === 0 && bytes.byteLength === bytes.buffer.byteLength
      ? bytes : bytes.slice();
    return this.call('push', [data], [data.buffer]);
  }
  flush() { return this.call('flush'); }
  reset() { return this.call('reset'); }
  reposition(offset, preserveTimeline) {
    return this.call('reposition', [offset, preserveTimeline]);
  }
  selectService(contextId) { return this.call('selectService', [contextId]); }
  selectTrack(kind, trackId) { return this.call('selectTrack', [kind, trackId]); }
  setMseOutputEnabled(enabled) { return this.call('setMseOutputEnabled', [enabled]); }
  setSubtitlePassthroughEnabled(enabled) {
    return this.call('setSubtitlePassthroughEnabled', [enabled]);
  }
  startIndex(growing) { return this.call('startIndex', [growing]); }
  finalizeIndex() { return this.call('finalizeIndex'); }
  setIndexDuration(duration) { return this.call('setIndexDuration', [duration]); }
  estimateOffset(target, sourceSize) {
    return this.call('estimateOffset', [target, sourceSize]);
  }
  seekPointCount() { return this.call('seekPointCount'); }
  indexState() { return this.call('indexState'); }
  applicationEntry(contextId) {
    return this.objectId === null ? null :
      this.client.applicationEntry(this.objectId, contextId);
  }
}

export async function createWorkerTlvDemuxModule(options = {}) {
  if (!protocol) throw new Error('demux-worker-protocol.js was not loaded');
  const client = new WorkerClient({
    workerUrl: options.workerUrl || new URL('./demux-worker-runtime.js', import.meta.url),
    wasmUrl: options.wasmUrl || new URL('../build-wasm/tlvdemux.js', import.meta.url).href,
  });
  await client.ready;
  return {
    DurationProbe: class extends WorkerDurationProbe {
      constructor() { super(client); }
    },
    TlvDemuxer: class extends WorkerDemuxer {
      constructor(callbacks) { super(client, callbacks); }
    },
    close: () => client.close(),
  };
}
