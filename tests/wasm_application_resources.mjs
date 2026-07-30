import fs from 'node:fs'
import path from 'node:path'
import { pathToFileURL } from 'node:url'

const [moduleFile, inputFile] = process.argv.slice(2)
if (!moduleFile || !inputFile) {
  throw new Error('usage: node wasm_application_resources.mjs TLVDEMUX-JS INPUT.tlv')
}

const imported = await import(pathToFileURL(path.resolve(moduleFile)).href)
const createModule = imported.default ?? imported.createTlvDemuxModule
const module = await createModule()
const resources = new Map()
const states = []
const demuxer = new module.TlvDemuxer({
  onApplicationResourceView(resource) {
    if (resource.dataLifetime !== 'callback') {
      throw new Error('resource view did not declare callback lifetime')
    }
    resources.set(resource.path, {
      size: resource.data.byteLength,
      version: resource.version,
      contentType: resource.contentType,
    })
  },
  onApplicationState(state) {
    states.push(state)
  },
})

const input = fs.openSync(inputFile, 'r')
const buffer = Buffer.allocUnsafe(1024 * 1024)
try {
  for (;;) {
    const count = fs.readSync(input, buffer, 0, buffer.length, null)
    if (count === 0) break
    demuxer.push(new Uint8Array(buffer.buffer, buffer.byteOffset, count))
  }
  demuxer.flush()
} finally {
  fs.closeSync(input)
  demuxer.delete()
}

if (!resources.has('sh4/40/001/startup/html/index.html')) {
  throw new Error('startup entry resource was not collected')
}
if (!states.some((state) => state.entryReady && state.state === 'ready')) {
  throw new Error('application never reached ready state')
}

console.log(`tlvdemux WASM collected ${resources.size} virtual files`)
