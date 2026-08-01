(function installTlvDemuxWorkerProtocol(scope) {
  scope.TlvDemuxWorkerProtocol = Object.freeze({
    init: 'tlvdemux:init',
    ready: 'tlvdemux:ready',
    create: 'tlvdemux:create',
    invoke: 'tlvdemux:invoke',
    destroy: 'tlvdemux:destroy',
    result: 'tlvdemux:result',
    event: 'tlvdemux:event',
    failure: 'tlvdemux:failure',
  });
})(globalThis);
