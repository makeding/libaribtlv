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
let virtualFiles = []
let virtualEntry = null
let virtualEntryBytes = null
let virtualApplications = []
let virtualGeneration = 0n
let dumpedResource = null
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
    demuxer.drainApplicationResources(32)
  }
  demuxer.flush()
  demuxer.drainApplicationResources(0)
  virtualFiles = demuxer.applicationResources(null)
  virtualEntry = demuxer.applicationEntry(1)
  const entry = demuxer.applicationResource(1, virtualEntry)
  virtualEntryBytes = entry && Uint8Array.from(entry.data)
  virtualApplications = demuxer.applications()
  virtualGeneration = demuxer.applicationResourceGeneration()
  if (process.env.TLVDEMUX_DUMP_RESOURCE) {
    const dumped = demuxer.applicationResource(1, process.env.TLVDEMUX_DUMP_RESOURCE)
    dumpedResource = dumped && Uint8Array.from(dumped.data)
  }
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
if (virtualFiles.length !== resources.size ||
    virtualEntry !== 'sh4/40/001/startup/html/index.html' ||
    !virtualEntryBytes?.byteLength) {
  throw new Error('WASM virtual resource catalogue is incomplete')
}
if (!virtualApplications.some(state => state.contextId === 1 && state.entryReady) ||
    virtualGeneration <= 0n) {
  throw new Error('WASM virtual application state is incomplete')
}

console.log(`tlvdemux WASM collected ${resources.size} virtual files`)
if (process.env.TLVDEMUX_DUMP_RESOURCES === '1') {
  console.log(virtualFiles.map(resource => resource.path).sort().join('\n'))
}
if (process.env.TLVDEMUX_DUMP_RESOURCE) {
  if (!dumpedResource) throw new Error(`resource not found: ${process.env.TLVDEMUX_DUMP_RESOURCE}`)
  console.log(new TextDecoder().decode(dumpedResource))
}
