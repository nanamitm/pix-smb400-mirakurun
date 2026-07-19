#!/usr/bin/env node
'use strict';

// Minimal mDNS responder for the constrained PIX-SMB400 runtime.
// Advertises pix-smb400.local and the Mirakurun HTTP service without Avahi/dbus.

const dgram = require('node:dgram');
const os = require('node:os');

const MDNS_ADDRESS = '224.0.0.251';
const MDNS_PORT = 5353;
const TTL = 120;
const hostname = normalizeLabel(process.env.MDNS_HOSTNAME || 'pix-smb400');
const hostFqdn = `${hostname}.local`;
const serviceType = '_http._tcp.local';
const serviceInstance = `${process.env.MDNS_INSTANCE || 'PIX-SMB400 Mirakurun'}.${serviceType}`;
const serviceEnumeration = '_services._dns-sd._udp.local';
const servicePort = parsePort(process.env.MDNS_PORT || '40772');

function normalizeLabel(value) {
  const label = String(value).toLowerCase().replace(/\.local\.?$/i, '')
    .replace(/[^a-z0-9-]/g, '-').replace(/^-+|-+$/g, '');
  if (!label || label.length > 63) throw new Error('invalid MDNS_HOSTNAME');
  return label;
}

function parsePort(value) {
  const port = Number(value);
  if (!Number.isInteger(port) || port < 1 || port > 65535) {
    throw new Error('invalid MDNS_PORT');
  }
  return port;
}

function ipv4Address() {
  const interfaces = os.networkInterfaces();
  const preferred = interfaces.eth0 || [];
  const candidates = preferred.concat(...Object.entries(interfaces)
    .filter(([name]) => name !== 'eth0')
    .map(([, addresses]) => addresses || []));
  const found = candidates.find((address) =>
    address.family === 'IPv4' && !address.internal && !address.address.startsWith('169.254.'));
  return found && found.address;
}

function encodeName(name) {
  const chunks = String(name).replace(/\.$/, '').split('.').map((label) => {
    const bytes = Buffer.from(label, 'utf8');
    if (bytes.length > 63) throw new Error(`DNS label too long: ${label}`);
    return Buffer.concat([Buffer.from([bytes.length]), bytes]);
  });
  return Buffer.concat([...chunks, Buffer.from([0])]);
}

function decodeName(packet, initialOffset) {
  const labels = [];
  let offset = initialOffset;
  let nextOffset = initialOffset;
  let jumped = false;
  let hops = 0;
  while (offset < packet.length && hops++ < 128) {
    const length = packet[offset];
    if ((length & 0xc0) === 0xc0) {
      if (offset + 1 >= packet.length) throw new Error('truncated DNS pointer');
      if (!jumped) nextOffset = offset + 2;
      offset = ((length & 0x3f) << 8) | packet[offset + 1];
      jumped = true;
      continue;
    }
    offset += 1;
    if (length === 0) {
      if (!jumped) nextOffset = offset;
      return { name: labels.join('.').toLowerCase(), offset: nextOffset };
    }
    if (offset + length > packet.length) throw new Error('truncated DNS name');
    labels.push(packet.subarray(offset, offset + length).toString('utf8'));
    offset += length;
    if (!jumped) nextOffset = offset;
  }
  throw new Error('invalid DNS name');
}

function parseQuestions(packet) {
  if (packet.length < 12) return [];
  const count = packet.readUInt16BE(4);
  let offset = 12;
  const questions = [];
  for (let i = 0; i < count; i += 1) {
    const decoded = decodeName(packet, offset);
    offset = decoded.offset;
    if (offset + 4 > packet.length) throw new Error('truncated DNS question');
    questions.push({
      name: decoded.name,
      type: packet.readUInt16BE(offset),
      unicast: Boolean(packet.readUInt16BE(offset + 2) & 0x8000),
    });
    offset += 4;
  }
  return questions;
}

function record(name, type, data, cacheFlush = false) {
  const header = Buffer.alloc(10);
  header.writeUInt16BE(type, 0);
  header.writeUInt16BE(cacheFlush ? 0x8001 : 1, 2);
  header.writeUInt32BE(TTL, 4);
  header.writeUInt16BE(data.length, 8);
  return Buffer.concat([encodeName(name), header, data]);
}

function ptr(name, target) {
  return record(name, 12, encodeName(target));
}

function a(name, address) {
  const octets = address.split('.').map(Number);
  return record(name, 1, Buffer.from(octets), true);
}

function srv(name, target) {
  const fixed = Buffer.alloc(6);
  fixed.writeUInt16BE(servicePort, 4);
  return record(name, 33, Buffer.concat([fixed, encodeName(target)]), true);
}

function txt(name) {
  const value = Buffer.from('path=/', 'utf8');
  return record(name, 16, Buffer.concat([Buffer.from([value.length]), value]), true);
}

function response(records) {
  const header = Buffer.alloc(12);
  header.writeUInt16BE(0x8400, 2); // response + authoritative answer
  header.writeUInt16BE(records.length, 6);
  return Buffer.concat([header, ...records]);
}

function recordsFor(questions, address) {
  const records = [];
  const add = (value) => {
    if (!records.some((existing) => existing.equals(value))) records.push(value);
  };
  for (const question of questions) {
    const any = question.type === 255;
    if (question.name === hostFqdn && (question.type === 1 || any)) add(a(hostFqdn, address));
    if (question.name === serviceEnumeration && (question.type === 12 || any)) {
      add(ptr(serviceEnumeration, serviceType));
    }
    if (question.name === serviceType && (question.type === 12 || any)) {
      add(ptr(serviceType, serviceInstance));
      add(srv(serviceInstance, hostFqdn));
      add(txt(serviceInstance));
      add(a(hostFqdn, address));
    }
    if (question.name === serviceInstance.toLowerCase()) {
      if (question.type === 33 || any) add(srv(serviceInstance, hostFqdn));
      if (question.type === 16 || any) add(txt(serviceInstance));
      add(a(hostFqdn, address));
    }
  }
  return records;
}

const socket = dgram.createSocket({ type: 'udp4', reuseAddr: true });

socket.on('error', (error) => {
  console.error(`[mdns] ${error.stack || error.message}`);
  process.exitCode = 1;
  socket.close();
});

socket.on('message', (packet, remote) => {
  try {
    const address = ipv4Address();
    if (!address) return;
    const questions = parseQuestions(packet);
    const records = recordsFor(questions, address);
    if (!records.length) return;
    const destination = questions.some((question) => question.unicast)
      ? { port: remote.port, address: remote.address }
      : { port: MDNS_PORT, address: MDNS_ADDRESS };
    socket.send(response(records), destination.port, destination.address);
  } catch (error) {
    console.error(`[mdns] ignored malformed query: ${error.message}`);
  }
});

socket.bind(MDNS_PORT, '0.0.0.0', () => {
  socket.addMembership(MDNS_ADDRESS);
  socket.setMulticastTTL(255);
  const address = ipv4Address();
  if (!address) {
    console.error('[mdns] no usable IPv4 address; waiting for queries');
    return;
  }
  const announcement = response([
    ptr(serviceEnumeration, serviceType),
    ptr(serviceType, serviceInstance),
    srv(serviceInstance, hostFqdn),
    txt(serviceInstance),
    a(hostFqdn, address),
  ]);
  socket.send(announcement, MDNS_PORT, MDNS_ADDRESS);
  setTimeout(() => socket.send(announcement, MDNS_PORT, MDNS_ADDRESS), 1000);
  console.log(`[mdns] ${hostFqdn} -> ${address}; ${serviceInstance} port ${servicePort}`);
});

for (const signal of ['SIGINT', 'SIGTERM']) {
  process.on(signal, () => socket.close(() => process.exit(0)));
}
