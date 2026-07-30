import fs from 'node:fs'
import path from 'node:path'
import { pathToFileURL } from 'node:url'

const [moduleFile, inputFile] = process.argv.slice(2)
if (!moduleFile || !inputFile) {
  throw new Error('usage: node wasm_event_info.mjs TLVDEMUX-JS INPUT.mmts')
}

const imported = await import(pathToFileURL(path.resolve(moduleFile)).href)
const createModule = imported.default ?? imported.createTlvDemuxModule
const module = await createModule()
const events = new Map()
const demuxer = new module.TlvDemuxer({
  onEventInfo(event) {
    if (event.tableId === 0x8b && event.currentNext &&
        (event.sectionNumber === 0 || event.sectionNumber === 1)) {
      events.set(event.sectionNumber, event)
    }
  },
})

const input = fs.openSync(inputFile, 'r')
const buffer = Buffer.allocUnsafe(1024 * 1024)
let transferred = 0
try {
  while (events.size < 2 && transferred < 64 * 1024 * 1024) {
    const count = fs.readSync(input, buffer, 0, buffer.length, null)
    if (count === 0) break
    transferred += count
    demuxer.push(new Uint8Array(buffer.buffer, buffer.byteOffset, count))
  }
  demuxer.flush()
} finally {
  fs.closeSync(input)
  demuxer.delete()
}

const present = events.get(0)
const following = events.get(1)
if (!present?.title || !following?.title ||
    !Number.isFinite(present.startTimeUnixMilliseconds) ||
    !Number.isFinite(following.startTimeUnixMilliseconds) ||
    !(present.durationSeconds > 0) || !(following.durationSeconds > 0)) {
  throw new Error('WASM did not expose complete MH-EIT present/following events')
}

console.log(`present:   ${present.title}`)
console.log(`following: ${following.title}`)
console.log(`read ${transferred} bytes before both MH-EIT events were available`)
