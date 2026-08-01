importScripts('./demux-worker-protocol.js');

const protocol = globalThis.TlvDemuxWorkerProtocol;
const applicationDrainBatch = 32;
const objects = new Map();
let modulePromise = null;
let operationQueue = Promise.resolve();

function serializeError(error) {
  return {
    name: error?.name || 'Error',
    message: error?.message || String(error),
    stack: error?.stack || '',
  };
}

function sendFailure(requestId, error) {
  postMessage({ type: protocol.failure, requestId, error: serializeError(error) });
}

function sendEvent(objectId, name, value, transfer = []) {
  postMessage({ type: protocol.event, objectId, name, value }, transfer);
}

function copyBytes(source) {
  const output = new Uint8Array(source?.byteLength || 0);
  if (output.byteLength) output.set(source);
  return output;
}

function transferAccessUnit(objectId, unit) {
  if (unit.codec === 'hevc' && !unit.randomAccess && !unit.discontinuity) return;
  if (unit.codec === 'aac-latm' && !unit.discontinuity) return;
  const data = unit.codec === 'ttml' ? copyBytes(unit.data) : new Uint8Array(0);
  const resources = (unit.subtitleResources || []).map(resource => ({
    ...resource,
    data: copyBytes(resource.data),
  }));
  const value = { ...unit, data, subtitleResources: resources };
  const transfer = [data.buffer, ...resources.map(resource => resource.data.buffer)];
  sendEvent(objectId, 'onAccessUnitView', value, transfer);
}

function automaticSelection(record, track) {
  const selection = record.selection;
  if (track.kind === 'video') {
    if (selection.videoTrack !== null) return;
    if (selection.videoPacketId !== null && track.packetId !== selection.videoPacketId) return;
    selection.videoTrack = track.trackId;
    record.instance.selectTrack('video', track.trackId);
    return;
  }
  if (track.kind === 'audio') {
    const channels = Number(track.audio?.channels || 0);
    if (selection.maxAudioChannels > 0 && channels > selection.maxAudioChannels) return;
    const preferred = selection.audioPacketId;
    if (selection.audioTrack !== null && (preferred === null || track.packetId !== preferred)) {
      return;
    }
    selection.audioTrack = track.trackId;
    record.instance.selectTrack('audio', track.trackId);
    return;
  }
  if (track.kind === 'subtitle' && track.codec === 'ttml') {
    const preferred = selection.subtitlePacketId;
    if (selection.subtitleTrack !== null &&
        (preferred === null || track.packetId !== preferred)) return;
    selection.subtitleTrack = track.trackId;
    record.instance.selectTrack('subtitle', track.trackId);
  }
}

function createDemuxer(module, objectId, options) {
  const record = {
    type: 'demuxer',
    module,
    instance: null,
    inputAddress: 0,
    inputCapacity: 0,
    applicationDrainScheduled: false,
    applicationDrainError: null,
    selection: {
      videoPacketId: options.videoPacketId ?? null,
      audioPacketId: options.audioPacketId ?? null,
      subtitlePacketId: options.subtitlePacketId ?? null,
      maxAudioChannels: Number(options.mseMaxAudioChannels || 0),
      videoTrack: null,
      audioTrack: null,
      subtitleTrack: null,
    },
  };
  const event = (name, transform = value => value) => value => {
    sendEvent(objectId, name, transform(value));
  };
  record.instance = new module.TlvDemuxer({
    mseMaxAudioChannels: record.selection.maxAudioChannels,
    onMseVideoStart: event('onMseVideoStart'),
    onMseInit(init) {
      sendEvent(objectId, 'onMseInit', init, [init.data.buffer]);
    },
    onMseSegment(segment) {
      sendEvent(objectId, 'onMseSegment', segment, [segment.data.buffer]);
    },
    onService: event('onService'),
    onTrack(track) {
      automaticSelection(record, track);
      sendEvent(objectId, 'onTrack', track);
    },
    onLayoutConfiguration: event('onLayoutConfiguration'),
    onApplicationService: event('onApplicationService'),
    onDataAsset: event('onDataAsset'),
    onSignallingMessage: event('onSignallingMessage'),
    onBroadcastClock: event('onBroadcastClock'),
    onEventInfo: event('onEventInfo'),
    onStreamEvent: event('onStreamEvent'),
    onApplicationResourceView(resource) {
      const data = copyBytes(resource.data);
      sendEvent(objectId, 'onApplicationResourceView', { ...resource, data }, [data.buffer]);
    },
    onApplicationState(state) {
      sendEvent(objectId, 'onApplicationState', {
        ...state,
        applicationEntry: record.instance.applicationEntry(state.contextId),
      });
    },
    onApplicationResourcesReset: event('onApplicationResourcesReset'),
    onPlaybackAccessUnitView(unit) {
      transferAccessUnit(objectId, unit);
    },
    onError: event('onError'),
  });
  return record;
}

function ensureInputCapacity(record, byteLength) {
  if (byteLength <= record.inputCapacity) return;
  const allocationUnit = 64 * 1024;
  const capacity = Math.ceil(byteLength / allocationUnit) * allocationUnit;
  if (!Number.isSafeInteger(capacity) || capacity <= 0) {
    throw new RangeError(`invalid demux input size: ${byteLength}`);
  }
  const address = record.module._malloc(capacity);
  if (!address) throw new RangeError(`cannot allocate ${capacity} bytes of WASM input memory`);
  if (record.inputAddress) record.module._free(record.inputAddress);
  record.inputAddress = address;
  record.inputCapacity = capacity;
}

function pushDemuxBytes(record, bytes) {
  const byteLength = Number(bytes?.byteLength || 0);
  if (byteLength === 0) return record.instance.pushFromHeap(0, 0);
  ensureInputCapacity(record, byteLength);
  record.module.HEAPU8.set(bytes, record.inputAddress);
  return record.instance.pushFromHeap(record.inputAddress, byteLength);
}

function scheduleApplicationDrain(record) {
  if (record.applicationDrainScheduled) return;
  record.applicationDrainScheduled = true;
  setTimeout(() => {
    operationQueue = operationQueue.then(() => {
      record.applicationDrainScheduled = false;
      if (objects.get(record.objectId) !== record) return;
      try {
        drainApplications(record, false);
      } catch (error) {
        record.applicationDrainError = error;
      }
    });
  }, 0);
}

function drainApplications(record, exhaustive) {
  if (record.type !== 'demuxer') return;
  let more = record.instance.drainApplicationResources(applicationDrainBatch);
  if (exhaustive) {
    while (more) {
      more = record.instance.drainApplicationResources(applicationDrainBatch);
    }
  } else if (more) {
    // Application decompression remains in the worker, but yields between
    // batches so a large carousel cannot indefinitely delay media input.
    scheduleApplicationDrain(record);
  }
}

async function initialize(message) {
  if (!modulePromise) {
    importScripts(message.wasmUrl);
    if (typeof createTlvDemuxModule !== 'function') {
      throw new Error(`WASM factory was not exported by ${message.wasmUrl}`);
    }
    modulePromise = createTlvDemuxModule();
  }
  await modulePromise;
  postMessage({ type: protocol.ready, requestId: message.requestId });
}

async function createObject(message) {
  const module = await modulePromise;
  let record;
  if (message.objectType === 'duration-probe') {
    record = { type: 'duration-probe', instance: new module.DurationProbe() };
  } else if (message.objectType === 'demuxer') {
    record = createDemuxer(module, message.objectId, message.options || {});
  } else {
    throw new Error(`unknown worker object type: ${message.objectType}`);
  }
  record.objectId = message.objectId;
  objects.set(message.objectId, record);
  postMessage({ type: protocol.result, requestId: message.requestId, value: true });
}

function configureSelection(record, options) {
  const selection = record.selection;
  if ('videoPacketId' in options) selection.videoPacketId = options.videoPacketId ?? null;
  if ('audioPacketId' in options) selection.audioPacketId = options.audioPacketId ?? null;
  if ('subtitlePacketId' in options) {
    selection.subtitlePacketId = options.subtitlePacketId ?? null;
  }
  return true;
}

async function invokeObject(message) {
  const record = objects.get(message.objectId);
  if (!record) throw new Error(`worker object ${message.objectId} does not exist`);
  if (record.applicationDrainError) throw record.applicationDrainError;
  let value;
  if (message.method === 'configureTrackSelection') {
    if (record.type !== 'demuxer') throw new Error('track selection requires a demuxer');
    value = configureSelection(record, message.args[0] || {});
  } else {
    if (record.type === 'demuxer' && message.method === 'push') {
      value = pushDemuxBytes(record, message.args?.[0]);
    } else {
      const method = record.instance[message.method];
      if (typeof method !== 'function') {
        throw new Error(`unknown ${record.type} method: ${message.method}`);
      }
      value = method.apply(record.instance, message.args || []);
    }
    if (message.method === 'push' || message.method === 'flush') {
      drainApplications(record, message.method === 'flush');
    }
  }
  postMessage({ type: protocol.result, requestId: message.requestId, value });
}

async function destroyObject(message) {
  const record = objects.get(message.objectId);
  if (record) {
    if (record.inputAddress) record.module._free(record.inputAddress);
    record.instance.delete();
    objects.delete(message.objectId);
  }
  postMessage({ type: protocol.result, requestId: message.requestId, value: true });
}

async function dispatch(message) {
  try {
    if (message.type === protocol.init) await initialize(message);
    else if (message.type === protocol.create) await createObject(message);
    else if (message.type === protocol.invoke) await invokeObject(message);
    else if (message.type === protocol.destroy) await destroyObject(message);
  } catch (error) {
    sendFailure(message.requestId, error);
  }
}

self.onmessage = event => {
  operationQueue = operationQueue.then(() => dispatch(event.data));
};
