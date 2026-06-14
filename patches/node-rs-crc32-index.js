'use strict'
// Pure-JS replacement for @node-rs/crc32 (deployed over node_modules on device).
//
// Why: @node-rs/crc32 ships no linux-arm musl build — only glibc gnueabihf,
// which aborts under Alpine/musl + gcompat on the SMB400. @chinachu/aribts
// uses only `crc32` here (PNG logo chunk CRCs); TS/TLV CRC uses aribts' own
// JS implementation. crc32c is provided for API parity.
//
// `make deploy-mirakurun` copies this file over
//   $MIRAKURUN/node_modules/@node-rs/crc32/index.js
// after pushing node_modules, so a fresh `npm install` never reintroduces the
// broken native binding on the device.

function makeTable(poly) {
  const t = new Int32Array(256)
  for (let n = 0; n < 256; n++) {
    let c = n
    for (let k = 0; k < 8; k++) c = (c & 1) ? (poly ^ (c >>> 1)) : (c >>> 1)
    t[n] = c
  }
  return t
}

const TABLE_IEEE = makeTable(0xEDB88320) // CRC-32 (zlib/PNG)
const TABLE_C = makeTable(0x82F63B78)    // CRC-32C (Castagnoli)

function compute(table, buf, prev) {
  let c = prev === undefined || prev === null ? 0xFFFFFFFF : (~prev) >>> 0
  for (let i = 0; i < buf.length; i++) {
    c = table[(c ^ buf[i]) & 0xff] ^ (c >>> 8)
  }
  // Return a signed int32 (matches Buffer.writeInt32BE usage in aribts).
  return (c ^ 0xFFFFFFFF) | 0
}

module.exports.crc32 = (buf, prev) => compute(TABLE_IEEE, buf, prev)
module.exports.crc32c = (buf, prev) => compute(TABLE_C, buf, prev)
