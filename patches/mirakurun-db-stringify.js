'use strict'
// Chunked native JSON serializer for Mirakurun's DB saves.
//
// Why: Mirakurun's db.js serializes programs.json with yieldable-json's
// stringifyAsync, which yields to the event loop but is ~26x slower than
// JSON.stringify. On the SMB400 a 15.8 MB programs.json (23751 programs) took
// 18.4 s (native: 0.7 s). During an 8K (~100 Mbps TLV) stream that save
// starved the TLV processing on the single JS thread for ~26 s, so the tuner
// pipeline overflowed and the client stuttered.
//
// This serializes the array element by element with the native serializer,
// CHUNK elements per event-loop turn. The output is byte-identical to
// JSON.stringify(data): an array serializes as its elements' JSON joined by
// commas, with undefined / function / symbol elements written as null.
//
// `make deploy-mirakurun` copies this file to
//   $MIRAKURUN/lib/Mirakurun/db-stringify.js
// and points db.js's stringifyAsync at stringifyChunked.

const CHUNK = 500

function stringifyChunked (data) {
  if (!Array.isArray(data)) {
    return Promise.resolve(JSON.stringify(data))
  }
  return new Promise((resolve, reject) => {
    const parts = new Array(data.length)
    let i = 0
    const step = () => {
      try {
        const end = Math.min(i + CHUNK, data.length)
        for (; i < end; i++) {
          const s = JSON.stringify(data[i])
          parts[i] = s === undefined ? 'null' : s
        }
        if (i < data.length) {
          setImmediate(step)
        } else {
          resolve('[' + parts.join(',') + ']')
        }
      } catch (e) {
        reject(e)
      }
    }
    step()
  })
}

module.exports = { stringifyChunked }
