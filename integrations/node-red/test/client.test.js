'use strict';
/*
 * Tests for lib/client.js (and lib/mdns.js) with plain node: `node --test test/client.test.js`.
 *
 * Part 1 runs against tools/hub_mock.py, the repository's stand-in for the hub (started here on a free port with
 * the Python in POTLUCK_PYTHON, else `python`; skipped when it cannot start). Part 2 runs against a small server
 * in this file for what the mock does not do (POST /api/integrations, 429, a stream that stalls, the hub
 * restarting, pairing over HTTPS): that server is this reader's reading of host/potluck/HUB-API.md, not the hub.
 */

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const https = require('node:https');
const net = require('node:net');
const path = require('node:path');
const crypto = require('node:crypto');
const { spawn } = require('node:child_process');

const lib = require('../lib/client');
const mdns = require('../lib/mdns');

const { PotluckClient, SseParser, coerceValue, writeBody, pair, QUALITIES } = lib;
const ROOT = path.resolve(__dirname, '..', '..', '..');
const MOCK = path.join(ROOT, 'tools', 'hub_mock.py');
const PY = process.env.POTLUCK_PYTHON || 'python';
const LED = 'potluck://lab/node-7368/led/on';
const DIE = 'potluck://lab/node-7368/hw/die_temp';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function freePort() {
  return new Promise((resolve, reject) => {
    const s = net.createServer();
    s.listen(0, '127.0.0.1', () => {
      const { port } = s.address();
      s.close(() => resolve(port));
    });
    s.on('error', reject);
  });
}

function getJson(url) {
  return new Promise((resolve, reject) => {
    http.get(url, (res) => {
      let t = '';
      res.on('data', (c) => { t += c; });
      res.on('end', () => {
        try { resolve(JSON.parse(t)); } catch (e) { reject(e); }
      });
    }).on('error', reject);
  });
}

async function startMock(port, args = []) {
  const proc = spawn(PY, [MOCK, '--host', '127.0.0.1', '--port', String(port), ...args], { stdio: 'ignore' });
  let exited = false;
  proc.on('exit', () => { exited = true; });
  proc.on('error', () => { exited = true; });
  const url = `http://127.0.0.1:${port}/`;
  for (let i = 0; i < 100 && !exited; i += 1) {
    try {
      await getJson(`${url}api/model`);
      return { proc, url, stop: () => new Promise((r) => { if (exited) r(); else { proc.once('exit', r); proc.kill(); } }) };
    } catch (e) {
      await sleep(100);
    }
  }
  proc.kill();
  return null;
}

/** Resolves the first `event` from `em` that `pred` accepts, or rejects after `ms`. */
function waitFor(em, event, pred = () => true, ms = 15000) {
  return new Promise((resolve, reject) => {
    const t = setTimeout(() => { em.removeListener(event, on); reject(new Error(`no ${event} in ${ms} ms`)); }, ms);
    function on(x) {
      if (pred(x)) {
        clearTimeout(t);
        em.removeListener(event, on);
        resolve(x);
      }
    }
    em.on(event, on);
  });
}

/** Just enough of Node-RED's runtime API to load and run the three nodes. */
function fakeRED() {
  const { EventEmitter } = require('node:events');
  const types = {};
  const nodes = {};
  const order = [];
  const RED = {
    nodes: {
      createNode(node, config) {
        for (const k of Object.getOwnPropertyNames(EventEmitter.prototype)) {
          if (k !== 'constructor') node[k] = EventEmitter.prototype[k];
        }
        EventEmitter.call(node);
        node.id = config.id;
        node.credentials = config.credentials || {};
        node.sent = [];
        node.send = (m) => node.sent.push(m);
        node.status = (s) => { node.lastStatus = s; };
        node.error = node.warn = node.debug = node.log = () => {};
      },
      registerType(name, ctor) { types[name] = ctor; },
      getNode(id) { return nodes[id]; },
    },
    auth: { needsPermission: () => (req, res, next) => next() },
    httpAdmin: { post() {}, get() {} },
    make(type, config) {
      const n = new types[type](config);
      nodes[config.id] = n;
      order.push(n);
      return n;
    },
    async closeAll() {
      for (const n of order.reverse()) {
        for (const l of n.listeners('close')) {
          await new Promise((done) => (l.length === 2 ? l.call(n, false, done) : l.call(n, done)));
        }
      }
    },
  };
  for (const f of ['potluck-hub', 'potluck-in', 'potluck-out']) require(`../nodes/${f}.js`)(RED);
  return RED;
}

// ===================================================================== units

test('SseParser frames events across any split and line ending', () => {
  const got = [];
  const p = new SseParser((e) => got.push(e));
  const text = 'retry: 2000\r\n\r\n: a comment\nevent: model\ndata: {"a":\ndata: 1}\n\nevent: events\r\ndata: {"next":3,"events":[]}\r\n\r\n';
  for (let i = 0; i < text.length; i += 3) p.push(text.slice(i, i + 3));
  assert.deepEqual(got, [
    { event: 'model', data: '{"a":\n1}' },
    { event: 'events', data: '{"next":3,"events":[]}' },
  ]);
});

test('values are a bool or a number, and a float stays a float on the wire', () => {
  assert.equal(coerceValue('on'), true);
  assert.equal(coerceValue('OFF'), false);
  assert.equal(coerceValue(' 21.5 '), 21.5);
  assert.equal(coerceValue('abc'), undefined);
  assert.equal(coerceValue({}), undefined);
  assert.equal(coerceValue(NaN), undefined);
  assert.equal(writeBody(LED, true).body, `{"path":"${LED}","value":true}`);
  assert.equal(writeBody('p', 20, 'float').body, '{"path":"p","value":20.0}');
  assert.equal(writeBody('p', 20, 'auto').body, '{"path":"p","value":20}');
  assert.equal(writeBody('p', 20.5, 'int').refused, 'this resource takes a whole number');
  assert.equal(JSON.parse(writeBody('p', 1e21, 'float').body).value, 1e21);
});

test('a multicast DNS answer becomes a hub with its URL and fingerprint', () => {
  const fp = 'ab'.repeat(32);
  const name = (n) => mdns.encodeName(n);
  const rr = (n, type, rdata) => {
    const h = Buffer.alloc(10);
    h.writeUInt16BE(type, 0);
    h.writeUInt16BE(1, 2);
    h.writeUInt32BE(120, 4);
    h.writeUInt16BE(rdata.length, 8);
    return Buffer.concat([name(n), h, rdata]);
  };
  const inst = 'Potluck lab ef76cc2e._potluck._tcp.local';
  const srv = Buffer.concat([Buffer.from([0, 0, 0, 0, 0x20, 0xfb]), name('hubpc.local')]); // port 8443
  const txt = Buffer.concat(['cell=ef76cc2e', 'system=lab', 'api=1', 'tls=1', `fp=${fp}`].map((s) => Buffer.concat([Buffer.from([s.length]), Buffer.from(s)])));
  const header = Buffer.alloc(12);
  header.writeUInt16BE(0x8400, 2);
  header.writeUInt16BE(1, 6);
  header.writeUInt16BE(3, 10);
  const pkt = Buffer.concat([header, rr('_potluck._tcp.local', 12, name(inst)), rr(inst, 33, srv), rr(inst, 16, txt),
    rr('hubpc.local', 1, Buffer.from([192, 168, 0, 20]))]);
  const hubs = mdns.hubsFromRecords(mdns.decodeRecords(pkt));
  assert.equal(hubs.length, 1);
  assert.deepEqual({ url: hubs[0].url, cell: hubs[0].cell, system: hubs[0].system, fp: hubs[0].fp, tls: hubs[0].tls },
    { url: 'https://192.168.0.20:8443/', cell: 'ef76cc2e', system: 'lab', fp, tls: true });
});

// ===================================================================== part 1: tools/hub_mock.py

test('against the hub mock', async (t) => {
  const port = await freePort();
  const mock = await startMock(port, ['--kill', '8160@4']);
  if (!mock) {
    t.skip(`the hub mock did not start (${PY} ${MOCK})`);
    return;
  }
  t.after(() => mock.stop());

  await t.test('resources arrive with their quality; unexposed ones are not passed on', async () => {
    const c = new PotluckClient({ url: mock.url, name: 'node-red-test', reportIntervalMs: 0 });
    const seen = new Map();
    c.on('resource', (r) => seen.set(r.path, r));
    c.start();
    await waitFor(c, 'model');
    await c.stop();
    assert.ok(seen.size > 0);
    for (const r of seen.values()) {
      assert.ok(QUALITIES.includes(r.quality), `${r.path} has a quality`);
      assert.ok('unit' in r && 'age_ms' in r && 'owner' in r, `${r.path} carries its tuple`);
      assert.equal(r.exposed, true, `${r.path} is exposed`);
      if (r.quality !== 'GOOD' && r.quality !== 'STALE') assert.equal(r.value, null);
    }
    assert.ok(seen.has(DIE), 'B\'s die temperature');
    assert.equal(seen.get(DIE).unit, 'C');
    assert.equal(seen.get(DIE).board.node, '7368');
    assert.ok(!seen.has('potluck://lab/node-7368/sys/uptime'), 'an unexposed resource is not passed on');
  });

  await t.test('a board dying arrives as a board change, events, and its resources with no value', async () => {
    const c = new PotluckClient({ url: mock.url, name: 'node-red-test', includeUnexposed: true, reportIntervalMs: 0 });
    const events = [];
    c.on('event', (e) => events.push(e));
    const dead = waitFor(c, 'board', (b) => b.node === '8160' && b.state === 'dead', 20000);
    const gone = waitFor(c, 'resource', (r) => r.owner === '8160' && r.path.includes('node-8160')
      && r.quality === 'UNAVAILABLE', 20000);
    c.start();
    const b = await dead;
    const r = await gone;
    await sleep(2500);
    await c.stop();
    assert.equal(b.previous, 'alive');
    assert.equal(r.value, null, 'no old number for a dead board\'s resource');
    assert.equal(r.board.state, 'dead');
    assert.ok(events.some((e) => JSON.stringify(e).includes('8160')), 'an event names 8160');
    const seqs = events.map((e) => e.seq);
    assert.equal(new Set(seqs).size, seqs.length, 'each event once');
  });

  await t.test('a write is applied, a forbidden one is refused, and a bad value is refused here', async () => {
    const c = new PotluckClient({ url: mock.url, name: 'node-red-test', reportIntervalMs: 0 });
    c.start();
    await waitFor(c, 'model');
    const before = c.resources().find((r) => r.path === LED).value;
    const lit = waitFor(c, 'resource', (r) => r.path === LED && r.value === !before, 10000);
    const ok = await c.write('node-7368/led/on', !before); // a name, completed from the model
    assert.deepEqual([ok.ok, ok.status, ok.result, ok.path], [true, 200, 'applied', LED]);
    await lit;
    const no = await c.write(DIE, 40);
    assert.deepEqual([no.ok, no.status, no.result], [false, 403, 'not writable from outside']);
    const bad = await c.write(LED, 'maybe');
    assert.deepEqual([bad.ok, bad.status], [false, 0]);
    await c.write(LED, before);
    await c.stop();
  });

  await t.test('the state report is sent; the mock has no POST /api/integrations', async () => {
    const c = new PotluckClient({ url: mock.url, name: 'node-red-test', reportIntervalMs: 60000 });
    const rep = waitFor(c, 'report', () => true, 10000);
    c.start();
    const r = await rep;
    await c.stop();
    assert.equal(r.state, 'connected');
    // The real hub answers {"ok": true} (HUB-API.md, M18); the mock answers 404, recorded here, not hidden.
    assert.equal(r.status, 404);
    assert.equal(r.accepted, false);
  });

  await t.test('the Node-RED nodes, run in a stand-in for Node-RED: messages in, a write out', async () => {
    const RED = fakeRED();
    const hub = RED.make('potluck-hub', { id: 'h1', url: mock.url, clientName: 'node-red-test' });
    const inn = RED.make('potluck-in', { id: 'i1', hub: 'h1', paths: 'node-7368/*', boards: true, events: true, initial: true });
    const out = RED.make('potluck-out', { id: 'o1', hub: 'h1', path: '' });
    await waitFor(hub.client, 'model');
    await sleep(100);
    const die = inn.sent.find((m) => m.topic === DIE);
    assert.ok(die, 'a resource message');
    assert.equal(typeof die.payload, 'number');
    assert.equal(die.potluck.kind, 'resource');
    assert.equal(die.potluck.quality, 'GOOD');
    assert.equal(die.potluck.unit, 'C');
    assert.equal(die.potluck.board.node, '7368');
    assert.ok(inn.sent.every((m) => m.potluck.kind !== 'resource' || /node-7368/.test(m.topic)), 'the filter');
    assert.ok(inn.sent.some((m) => m.potluck.kind === 'board' && m.topic === 'potluck://lab/node-7368' && m.payload === 'alive'));
    assert.deepEqual(inn.lastStatus, { fill: 'green', shape: 'dot', text: 'connected' });
    const sent = [];
    await new Promise((resolve) => out.emit('input', { topic: DIE, payload: 30 }, (m) => sent.push(m), resolve));
    assert.equal(sent[0].potluck.result, 'not writable from outside');
    assert.equal(sent[0].potluck.ok, false);
    await new Promise((resolve) => out.emit('input', { topic: LED, payload: 'off' }, (m) => sent.push(m), resolve));
    assert.deepEqual([sent[1].potluck.result, sent[1].potluck.ok, sent[1].potluck.value], ['applied', true, false]);
    await RED.closeAll();
    assert.equal(hub.client.running, false);
  });

  await t.test('the hub going away: every value goes null; back, they return', async () => {
    const port2 = await freePort();
    const m2 = await startMock(port2);
    assert.ok(m2);
    const c = new PotluckClient({ url: m2.url, name: 'node-red-test', reportIntervalMs: 0, deadAfterMs: 1500,
      pollMs: 300, streamRetryMs: 800 });
    c.start();
    await waitFor(c, 'model');
    const lost = waitFor(c, 'resource', (r) => r.path === DIE && r.hub_unreachable && r.value === null, 10000);
    const unreachable = waitFor(c, 'state', (s) => s.state === 'unreachable', 10000);
    await m2.stop();
    await lost;
    await unreachable;
    assert.ok(c.resources().every((r) => r.value === null && r.quality === 'UNAVAILABLE'));
    const back = waitFor(c, 'resource', (r) => r.path === DIE && r.quality === 'GOOD' && typeof r.value === 'number', 15000);
    const m3 = await startMock(port2);
    assert.ok(m3);
    await back;
    await waitFor(c, 'state', (s) => s.state === 'streaming', 15000).catch(() => null);
    assert.equal(c.conn, 'streaming');
    await c.stop();
    await m3.stop();
  });
});

// ===================================================================== part 2: a server written from HUB-API.md

function model(over = {}) {
  return Object.assign({
    api: 1, at: Date.now(),
    hub: { version: 'test', up_ms: 100000, link: 'up', pull_period_ms: 2000 },
    cell: { system: 'lab', cell_id: 'ef76cc2e' },
    boards: [{ node: '7368', label: 'B', state: 'alive', since_at: 1 }],
    resources: [{ path: LED, name: 'node-7368/led/on', owner: '7368', value: false, unit: '', quality: 'GOOD',
      age_ms: 10, latency_class: 4, exposed: true, outside_writable: true, safety_relevant: false }],
  }, over);
}

/** A tiny hub: records every request; behaviour per test through `h`. */
function fakeHub(h, tls) {
  const seen = [];
  const handler = (req, res) => {
    let body = '';
    req.on('data', (c) => { body += c; });
    req.on('end', () => {
      seen.push({ method: req.method, url: req.url, headers: req.headers, body });
      const send = (status, obj, headers = {}) => {
        res.writeHead(status, Object.assign({ 'Content-Type': 'application/json' }, headers));
        res.end(JSON.stringify(obj));
      };
      if (h.handle && h.handle(req, res, body, send) !== false) return;
      send(404, { error: 'not found' });
    });
  };
  const server = tls ? https.createServer(tls, handler) : http.createServer(handler);
  return new Promise((resolve) => server.listen(0, '127.0.0.1', () => {
    const { port } = server.address();
    resolve({ server, seen, url: `${tls ? 'https' : 'http'}://127.0.0.1:${port}/`,
      close: () => new Promise((r) => { server.closeAllConnections(); server.close(r); }) });
  }));
}

test('the state report: headers, shape, and repeated while running', async () => {
  const hub = await fakeHub({
    handle(req, res, body, send) {
      if (req.url === '/api/integrations' && req.method === 'POST') return send(200, { ok: true });
      if (req.url === '/api/model') return send(200, model());
      if (req.url.startsWith('/api/events')) return send(200, { api: 1, next: 1, events: [] });
      return false;
    },
  });
  const c = new PotluckClient({ url: hub.url, name: 'Node-RED test', reportIntervalMs: 300, pollMs: 200, streamRetryMs: 60000 });
  c.start(); // no /api/stream here: the client falls back to polling, still connected
  await waitFor(c, 'report', (r) => r.accepted, 5000);
  await sleep(700);
  await c.stop('test over');
  await hub.close();
  const reports = hub.seen.filter((s) => s.url === '/api/integrations');
  assert.ok(reports.length >= 3, `reported ${reports.length} times`);
  for (const r of reports) {
    assert.equal(r.headers['x-potluck-request'], '1');
    assert.equal(r.headers['x-potluck-client'], 'Node-RED test');
    const b = JSON.parse(r.body);
    assert.equal(b.name, 'Node-RED test');
    assert.ok(['connected', 'disconnected', 'error', 'idle'].includes(b.state));
    assert.equal(typeof b.detail, 'string');
  }
  assert.equal(JSON.parse(reports[0].body).state, 'connected');
  assert.match(JSON.parse(reports[0].body).detail, /polling/);
  assert.deepEqual(JSON.parse(reports[reports.length - 1].body), { name: 'Node-RED test', state: 'disconnected', detail: 'test over' });
  assert.ok(hub.seen.filter((s) => s.method === 'GET').every((s) => s.headers['x-potluck-client'] === 'Node-RED test'));
});

test('429: the write waits Retry-After and is tried again', async () => {
  let writes = 0;
  const hub = await fakeHub({
    handle(req, res, body, send) {
      if (req.url !== '/api/write') return false;
      writes += 1;
      if (writes === 1) return send(429, { ok: false, result: 'slow down: 2 writes a second' }, { 'Retry-After': '1' });
      return send(200, { ok: true, result: 'applied' });
    },
  });
  const c = new PotluckClient({ url: hub.url, reportIntervalMs: 0 });
  const t0 = Date.now();
  const r = await c.write(LED, true);
  const took = Date.now() - t0;
  await hub.close();
  assert.equal(r.result, 'applied');
  assert.equal(r.tries, 2);
  assert.ok(took >= 950, `waited ${took} ms`);
});

test('a stream that stops sending: polling with since=next, then the stream again; a hub restart is seen', async () => {
  let streams = 0;
  let restarted = false;
  const hub = await fakeHub({
    handle(req, res, body, send) {
      if (req.url === '/api/stream') {
        streams += 1;
        res.writeHead(200, { 'Content-Type': 'text/event-stream' });
        res.write(`event: model\ndata: ${JSON.stringify(model())}\n\n`);
        res.write(`event: events\ndata: ${JSON.stringify({ api: 1, next: 11, events: [{ seq: 10, at: 1, board: 'hub', kind: 'hub_started', detail: {} }] })}\n\n`);
        return undefined; // then silence
      }
      if (req.url === '/api/model') return send(200, model(restarted ? { hub: { up_ms: 5, link: 'up', pull_period_ms: 2000 } } : {}));
      if (req.url.startsWith('/api/events')) {
        if (restarted) return send(200, { api: 1, next: 3, events: [{ seq: 1, at: 2, board: 'hub', kind: 'hub_started', detail: {} }, { seq: 2, at: 3, board: '7368', kind: 'board_dead', detail: {} }] });
        return send(200, { api: 1, next: 12, events: [{ seq: 11, at: 2, board: '7368', kind: 'board_alive', detail: {} }] });
      }
      return false;
    },
  });
  const c = new PotluckClient({ url: hub.url, reportIntervalMs: 0, deadAfterMs: 600, pollMs: 150, streamRetryMs: 1200 });
  const events = [];
  const problems = [];
  c.on('event', (e) => events.push(e));
  c.on('problem', (p) => problems.push(p.message));
  c.start();
  await waitFor(c, 'state', (s) => s.state === 'polling', 5000);
  await waitFor(c, 'event', (e) => e.seq === 11, 5000);
  const polls = hub.seen.filter((s) => s.url.startsWith('/api/events'));
  assert.equal(polls[0].url, '/api/events?since=11', 'polls from the stream\'s next');
  assert.ok(polls.slice(1).every((s) => s.url === '/api/events?since=12'));
  assert.equal(events.filter((e) => e.seq === 11).length, 1, 'each event once');
  assert.ok(!events.some((e) => e.seq === 10), 'the history held on connect is not replayed by default');
  await sleep(1300);
  assert.ok(streams >= 2, 'the stream was tried again');
  restarted = true;
  await waitFor(c, 'event', (e) => e.seq === 2 && e.kind === 'board_dead', 5000);
  await c.stop();
  await hub.close();
  assert.ok(problems.some((p) => /restarted/.test(p)));
});

test('pairing over HTTPS, the certificate checked by its fingerprint before anything is sent', async () => {
  const cert = fs.readFileSync(path.join(__dirname, 'fixtures', 'test-hub-cert.pem'));
  const key = fs.readFileSync(path.join(__dirname, 'fixtures', 'test-hub-key.pem'));
  const fp = new crypto.X509Certificate(cert).fingerprint256;
  let polls = 0;
  const hub = await fakeHub({
    handle(req, res, body, send) {
      if (req.url === '/api/pair' && req.method === 'POST') {
        if (req.headers['x-potluck-request'] !== '1') return send(403, { ok: false, result: 'missing X-Potluck-Request' });
        const b = JSON.parse(body);
        assert.deepEqual(b, { name: 'Node-RED', right: 'write' });
        return send(202, { pair_id: 'abc123', expires_in_s: 300 });
      }
      if (req.url === '/api/pair/abc123') {
        polls += 1;
        if (polls < 3) return send(200, { state: 'pending' });
        if (polls === 3) return send(200, { state: 'approved', name: 'Node-RED', right: 'write', token: 'tok-1' });
        return send(404, { error: 'not found' });
      }
      if (req.url === '/api/model') {
        if (req.headers.authorization !== 'Bearer tok-1') return send(401, { error: 'login', result: 'log in first' });
        return send(200, model());
      }
      return false;
    },
  }, { cert, key });

  const states = [];
  const got = await pair({ url: hub.url, fingerprint: fp, name: 'Node-RED', right: 'write', pollMs: 50,
    onState: (s) => states.push(s.state) });
  assert.deepEqual(got, { token: 'tok-1', name: 'Node-RED', right: 'write' });
  assert.deepEqual(states, ['pending']);

  const c = new PotluckClient({ url: hub.url, fingerprint: fp, token: got.token, reportIntervalMs: 0 });
  const m = await c.getModel();
  assert.equal(m.cell.cell_id, 'ef76cc2e');
  c.agent.destroy();

  const before = hub.seen.length;
  const wrong = new PotluckClient({ url: hub.url, fingerprint: 'cd'.repeat(32), token: 'tok-1', reportIntervalMs: 0 });
  await assert.rejects(wrong.getModel(), (e) => e.kind === 'certificate');
  wrong.agent.destroy();
  assert.equal(hub.seen.length, before, 'nothing reached a hub with another certificate');

  const nopin = new PotluckClient({ url: hub.url, token: 'tok-1', reportIntervalMs: 0 });
  await assert.rejects(nopin.getModel(), (e) => e.kind === 'network'); // self-signed, not trusted without fp or ca
  nopin.agent.destroy();
  await assert.rejects(pair({ url: 'http://127.0.0.1:8470/' }), (e) => e.kind === 'config');
  await hub.close();
});
