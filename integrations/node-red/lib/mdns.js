'use strict';
/*
 * Finding a hub on the house network (INTEGRATIONS.md step 1; HUB-API.md, "Announced on the network"): a hub
 * started with --listen announces `_potluck._tcp.local.` by multicast DNS (RFC 6762/6763) with TXT cell=, system=,
 * api=1, tls=1 and fp= (its certificate's SHA-256). This sends one query from an ordinary port -- RFC 6762 section
 * 6.7's "legacy unicast" query, answered straight back to it -- and reads PTR, SRV, TXT, A and AAAA records.
 * Node's built-in dgram only.
 */

const dgram = require('node:dgram');
const os = require('node:os');

const SERVICE = '_potluck._tcp.local';
const T = { A: 1, PTR: 12, TXT: 16, AAAA: 28, SRV: 33 };

function encodeName(name) {
  const parts = name.replace(/\.$/, '').split('.');
  const bufs = parts.map((p) => {
    const b = Buffer.from(p, 'utf8');
    return Buffer.concat([Buffer.from([b.length]), b]);
  });
  return Buffer.concat([...bufs, Buffer.from([0])]);
}

/** A query for one name and type. */
function encodeQuery(name, type = T.PTR, id = 0) {
  const h = Buffer.alloc(12);
  h.writeUInt16BE(id, 0);
  h.writeUInt16BE(1, 4); // one question
  const q = Buffer.alloc(4);
  q.writeUInt16BE(type, 0);
  q.writeUInt16BE(1, 2); // class IN
  return Buffer.concat([h, encodeName(name), q]);
}

function readName(buf, off) {
  const labels = [];
  let jumped = false;
  let end = off;
  for (let guard = 0; guard < 128; guard += 1) {
    if (off >= buf.length) throw new Error('name runs past the packet');
    const len = buf[off];
    if (len === 0) {
      if (!jumped) end = off + 1;
      return { name: labels.join('.'), end };
    }
    if ((len & 0xc0) === 0xc0) {
      const ptr = ((len & 0x3f) << 8) | buf[off + 1];
      if (!jumped) end = off + 2;
      jumped = true;
      off = ptr;
      continue;
    }
    labels.push(buf.toString('utf8', off + 1, off + 1 + len));
    off += 1 + len;
  }
  throw new Error('name has too many labels');
}

/** Every resource record in a DNS message, answers and additional alike. */
function decodeRecords(buf) {
  const qd = buf.readUInt16BE(4);
  const total = buf.readUInt16BE(6) + buf.readUInt16BE(8) + buf.readUInt16BE(10);
  let off = 12;
  for (let i = 0; i < qd; i += 1) off = readName(buf, off).end + 4;
  const out = [];
  for (let i = 0; i < total; i += 1) {
    const n = readName(buf, off);
    off = n.end;
    const type = buf.readUInt16BE(off);
    const rdlen = buf.readUInt16BE(off + 8);
    const rd = off + 10;
    off = rd + rdlen;
    const rec = { name: n.name.toLowerCase(), rawName: n.name, type };
    if (type === T.PTR) rec.target = readName(buf, rd).name;
    else if (type === T.SRV) {
      rec.port = buf.readUInt16BE(rd + 4);
      rec.target = readName(buf, rd + 6).name;
    } else if (type === T.TXT) {
      rec.txt = {};
      let p = rd;
      while (p < rd + rdlen) {
        const l = buf[p];
        const s = buf.toString('utf8', p + 1, p + 1 + l);
        const eq = s.indexOf('=');
        if (eq > 0) rec.txt[s.slice(0, eq).toLowerCase()] = s.slice(eq + 1);
        else if (s) rec.txt[s.toLowerCase()] = '';
        p += 1 + l;
      }
    } else if (type === T.A) rec.address = [...buf.subarray(rd, rd + 4)].join('.');
    else if (type === T.AAAA) {
      const g = [];
      for (let k = 0; k < 16; k += 2) g.push(buf.readUInt16BE(rd + k).toString(16));
      rec.address = g.join(':');
    }
    out.push(rec);
  }
  return out;
}

/** Records (from any number of answers) -> hubs: {instance, host, port, addresses, cell, system, api, tls, fp, url}. */
function hubsFromRecords(records) {
  const byName = (name, type) => records.filter((r) => r.name === name.toLowerCase() && r.type === type);
  const instances = new Set(records.filter((r) => r.type === T.PTR && r.name === SERVICE).map((r) => r.target));
  const hubs = [];
  for (const inst of instances) {
    const srv = byName(inst, T.SRV)[0];
    const txt = Object.assign({}, ...byName(inst, T.TXT).map((r) => r.txt));
    if (!srv) continue;
    const addresses = [...byName(srv.target, T.A), ...byName(srv.target, T.AAAA)].map((r) => r.address);
    const host = addresses.find((a) => a.includes('.')) || (addresses[0] ? `[${addresses[0]}]` : srv.target);
    const tls = txt.tls === '1';
    hubs.push({
      instance: inst.replace(`.${SERVICE}`, ''),
      host: srv.target,
      port: srv.port,
      addresses,
      cell: txt.cell || '',
      system: txt.system || '',
      api: txt.api || '',
      tls,
      fp: txt.fp || '',
      url: `${tls ? 'https' : 'http'}://${host}:${srv.port}/`,
    });
  }
  return hubs;
}

/** This computer's IPv4 addresses, loopback left out: one per network adapter. */
function ipv4Addresses() {
  const out = [];
  for (const list of Object.values(os.networkInterfaces())) {
    for (const a of list || []) {
      if ((a.family === 'IPv4' || a.family === 4) && !a.internal) out.push(a.address);
    }
  }
  return out;
}

/**
 * Ask the network for hubs for timeoutMs; resolves the list (empty when none answered).
 *
 * The query goes out on every IPv4 adapter, one after another: a multicast query leaves by one adapter only (the
 * default one), and on a PC with a VPN, Tailscale or Hyper-V that is often not the house network's -- found
 * 2026-10-18 on the maintainer's PC (six adapters), where the default one never reached the hub on 192.168.0.7.
 */
function discover({ timeoutMs = 3000 } = {}) {
  return new Promise((resolve) => {
    const sock = dgram.createSocket({ type: 'udp4', reuseAddr: true });
    const records = [];
    let done = false;
    const finish = () => {
      if (done) return;
      done = true;
      try { sock.close(); } catch (e) { /* closed */ }
      resolve(hubsFromRecords(records));
    };
    sock.on('message', (msg) => {
      try {
        records.push(...decodeRecords(msg));
      } catch (e) { /* not a message we can read */ }
    });
    sock.on('error', finish);
    sock.bind(0, () => {
      const q = encodeQuery(SERVICE, T.PTR, Math.floor(Math.random() * 0xffff));
      // one adapter at a time: the interface is the socket's, so the next is chosen only once a send has gone
      const sendAll = (addrs) => {
        if (done || !addrs.length) return;
        try {
          sock.setMulticastInterface(addrs[0]);
        } catch (e) { /* an adapter that went away: the default one is used */ }
        sock.send(q, 5353, '224.0.0.251', () => sendAll(addrs.slice(1)));
      };
      const all = () => {
        const addrs = ipv4Addresses();
        if (addrs.length) sendAll(addrs);
        else if (!done) sock.send(q, 5353, '224.0.0.251', () => {});
      };
      all();
      setTimeout(all, 1000);
    });
    setTimeout(finish, timeoutMs);
  });
}

module.exports = { discover, decodeRecords, hubsFromRecords, encodeQuery, encodeName, ipv4Addresses, SERVICE, T };
