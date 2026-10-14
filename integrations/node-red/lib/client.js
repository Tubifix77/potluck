'use strict';
/*
 * A client for the Potluck hub's API, version 1 -- the logic of node-red-contrib-potluck, usable without Node-RED.
 *
 * Built from INTEGRATIONS.md ("A program on the hub's API, step by step") and host/potluck/HUB-API.md only.
 * Node's built-in modules only.
 *
 *   const { PotluckClient } = require('./lib/client');
 *   const c = new PotluckClient({ url: 'http://127.0.0.1:8470/', name: 'Node-RED' });
 *   c.on('resource', (r) => console.log(r.path, r.value, r.quality));
 *   c.start();
 *
 * Events emitted by a PotluckClient:
 *   'resource'  a resource changed (value, quality, owner or its board's state), or was seen for the first time:
 *               {path, name, value, unit, quality, age_ms, owner, board, ...}. `value` is null whenever quality is
 *               UNAVAILABLE, NO_DATA or FAULTY, and for every resource while the hub cannot be reached.
 *   'board'     a board's state changed: {node, label, state, previous, since_at}
 *   'event'     one event from the hub (GET /api/events shape), each seq once
 *   'state'     the connection changed: {state: connecting|streaming|polling|unreachable|refused, detail}
 *   'report'    a state report was sent: {state, detail, status, accepted, result}
 *   'model'     the whole model, each time one arrives
 *   'problem'   something worth logging that is not fatal: {kind, message}
 */

const http = require('node:http');
const https = require('node:https');
const { EventEmitter } = require('node:events');

const QUALITIES = ['GOOD', 'STALE', 'UNAVAILABLE', 'NO_DATA', 'FAULTY'];
const NO_VALUE = new Set(['UNAVAILABLE', 'NO_DATA', 'FAULTY']);
const REPORT_STATES = ['connected', 'disconnected', 'error', 'idle'];

class PotluckError extends Error {
  constructor(kind, message, extra) {
    super(message);
    this.name = 'PotluckError';
    this.kind = kind; // config | network | timeout | auth | share | hub | certificate | stream | pair
    if (extra) Object.assign(this, extra);
  }
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

/** "34:A5:..." or "34a5..." -> 64 lowercase hex digits; '' when none given. */
function normaliseFingerprint(fp) {
  if (fp == null || String(fp).trim() === '') return '';
  const s = String(fp).replace(/[:\s]/g, '').toLowerCase();
  if (!/^[0-9a-f]{64}$/.test(s)) {
    throw new PotluckError('config', 'a fingerprint is the SHA-256 of the hub\'s certificate: 64 hex digits');
  }
  return s;
}

function bareHost(hostname) {
  return hostname.replace(/^\[/, '').replace(/\]$/, '');
}

function isLoopback(hostname) {
  const h = bareHost(hostname);
  return h === '127.0.0.1' || h === '::1' || h === 'localhost';
}

function readAll(res) {
  return new Promise((resolve, reject) => {
    const parts = [];
    res.on('data', (c) => parts.push(c));
    res.on('end', () => resolve(Buffer.concat(parts).toString('utf8')));
    res.on('error', reject);
    res.on('aborted', () => reject(new PotluckError('network', 'the answer was cut off')));
  });
}

/** Retry-After is seconds (HUB-API.md); an HTTP date is accepted too. Milliseconds, at least 100. */
function retryAfterMs(header) {
  if (header == null) return 1000;
  const s = Number(header);
  if (Number.isFinite(s)) return Math.max(100, Math.ceil(s * 1000));
  const t = Date.parse(header);
  if (Number.isFinite(t)) return Math.max(100, t - Date.now());
  return 1000;
}

/**
 * Server-sent events, as the HTML standard frames them: lines end in CRLF, LF or CR; a blank line ends an event;
 * `data:` lines join with LF; `:` starts a comment. Feed it text chunks in any split.
 */
class SseParser {
  constructor(onEvent) {
    this.onEvent = onEvent;
    this.buf = '';
    this._reset();
  }

  _reset() {
    this.event = '';
    this.data = [];
  }

  push(text) {
    this.buf += text;
    for (;;) {
      const i = this.buf.search(/[\r\n]/);
      if (i < 0) return;
      let len = 1;
      if (this.buf[i] === '\r') {
        if (i + 1 >= this.buf.length) return; // a CR at the end may be the first half of CRLF
        if (this.buf[i + 1] === '\n') len = 2;
      }
      const line = this.buf.slice(0, i);
      this.buf = this.buf.slice(i + len);
      this._line(line);
    }
  }

  _line(line) {
    if (line === '') {
      if (this.data.length) this.onEvent({ event: this.event || 'message', data: this.data.join('\n') });
      this._reset();
      return;
    }
    if (line[0] === ':') return;
    const c = line.indexOf(':');
    let field = line;
    let value = '';
    if (c >= 0) {
      field = line.slice(0, c);
      value = line.slice(c + 1);
      if (value[0] === ' ') value = value.slice(1);
    }
    if (field === 'event') this.event = value;
    else if (field === 'data') this.data.push(value);
    // `id` and `retry` are not used: HUB-API.md says the stream sends no retry and takes no since.
  }
}

/** A bool or a number, from what a flow is likely to hold. undefined when it is neither. */
function coerceValue(v) {
  if (typeof v === 'boolean') return v;
  if (typeof v === 'number') return Number.isFinite(v) ? v : undefined;
  if (typeof v === 'string') {
    const s = v.trim().toLowerCase();
    if (s === 'true' || s === 'on') return true;
    if (s === 'false' || s === 'off') return false;
    if (/^[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?$/.test(s)) return Number(s);
  }
  return undefined;
}

/**
 * The body of POST /api/write. HUB-API.md: an integer is written as a 32-bit integer, a number with a fraction as a
 * 32-bit float, and the board refuses the wrong type. JavaScript writes 20.0 as "20", so numberType 'float' forces a
 * fraction into the text; 'int' refuses a fraction here rather than at the board. Returns {body} or {refused}.
 */
function writeBody(path, value, numberType = 'auto') {
  let text;
  if (typeof value === 'boolean') {
    text = value ? 'true' : 'false';
  } else if (typeof value === 'number' && Number.isFinite(value)) {
    if (numberType === 'float') {
      text = Number.isInteger(value) ? value.toFixed(1) : String(value);
    } else if (numberType === 'int') {
      if (!Number.isInteger(value)) return { refused: 'this resource takes a whole number' };
      if (value < -2147483648 || value > 2147483647) return { refused: 'a whole number in 32 bits' };
      text = String(value);
    } else {
      text = JSON.stringify(value);
    }
  } else {
    return { refused: 'a bool or a number' };
  }
  return { body: `{"path":${JSON.stringify(path)},"value":${text}}` };
}

function statusError(status, json, text) {
  const said = (json && (json.result || json.error)) || (text || '').slice(0, 120) || `HTTP ${status}`;
  if (status === 401) return new PotluckError('auth', `the hub wants a login or a token: ${said}`, { status });
  if (status === 403) return new PotluckError('auth', `refused: ${said}`, { status });
  if (status === 429) return new PotluckError('share', said, { status });
  return new PotluckError('hub', `HTTP ${status}: ${said}`, { status });
}

class PotluckClient extends EventEmitter {
  /**
   * @param {object} o
   * @param {string} o.url              http://127.0.0.1:8470/ on the hub's PC; https://<host>:<port>/ elsewhere
   * @param {string} [o.token]          a paired token (sent only over https)
   * @param {string} [o.fingerprint]    the hub's certificate SHA-256 (its `fp=` announcement); pins the certificate
   * @param {string|Buffer} [o.ca]      or: the hub's certificate file's contents, to trust it the usual way
   * @param {string} [o.name]           this program's name: X-Potluck-Client, the state report, the writers list
   * @param {boolean} [o.includeUnexposed]  also pass on resources the package does not expose (default: no)
   * @param {boolean} [o.replayHistory] emit the events the hub already held when first connected (default: no)
   * @param {number} [o.reportIntervalMs]   state report period, 0 for none (default 30000)
   * @param {string} [o.numberType]     'auto' | 'int' | 'float' for writes (default 'auto')
   */
  constructor(o = {}) {
    super();
    if (!o.url) throw new PotluckError('config', 'the hub\'s URL is needed, e.g. http://127.0.0.1:8470/');
    let u;
    try {
      u = new URL(o.url);
    } catch (e) {
      throw new PotluckError('config', `not a URL: ${o.url}`);
    }
    if (u.protocol !== 'http:' && u.protocol !== 'https:') {
      throw new PotluckError('config', 'the hub\'s URL starts with http:// or https://');
    }
    this.base = u;
    this.tls = u.protocol === 'https:';
    this.loopback = isLoopback(u.hostname);
    this.token = o.token || '';
    this.fingerprint = normaliseFingerprint(o.fingerprint);
    this.ca = o.ca || null;
    this.name = String(o.name || 'Node-RED');
    this.includeUnexposed = !!o.includeUnexposed;
    this.replayHistory = !!o.replayHistory;
    this.reportIntervalMs = o.reportIntervalMs ?? 30000;
    this.pollMs = o.pollMs ?? 2000;
    this.streamRetryMs = o.streamRetryMs ?? 15000;
    this.deadAfterMs = o.deadAfterMs ?? null; // null: three pull periods, at least 10 s (HUB-API.md, the stream)
    this.requestTimeoutMs = o.requestTimeoutMs ?? 10000;
    this.writeRetries = o.writeRetries ?? 3;
    this.numberType = o.numberType || 'auto';
    this.agent = new (this.tls ? https : http).Agent({ keepAlive: true, maxSockets: 6 });
    this.setMaxListeners(100);

    this.running = false;
    this.conn = 'idle';
    this.connDetail = '';
    this.model = null;
    this.lastReport = null;
    this._slowUntil = 0;
    this._stream = null;
    this._polling = false;
    this._pollTimer = null;
    this._streamRetryTimer = null;
    this._reportTimer = null;
    this._lastModelAt = 0;
    this._lastUpMs = null;
    this._lastSeq = null;
    this._next = null;
    this._resKey = new Map(); // path -> change key
    this._resLast = new Map(); // path -> last record emitted
    this._boardState = new Map(); // node -> state
    this._reported = null;
  }

  // ---------------------------------------------------------------- HTTP

  _open(method, path, { body, stream, timeoutMs } = {}) {
    return new Promise((resolve, reject) => {
      const url = new URL(path, this.base);
      const headers = {
        'X-Potluck-Client': this.name,
        Accept: stream ? 'text/event-stream' : 'application/json',
      };
      if (this.token && this.tls) headers.Authorization = `Bearer ${this.token}`;
      let payload = null;
      if (method === 'POST') {
        headers['X-Potluck-Request'] = '1';
        payload = Buffer.from(body == null ? '{}' : body, 'utf8');
        headers['Content-Type'] = 'application/json';
        headers['Content-Length'] = payload.length;
      }
      const opt = {
        method,
        hostname: bareHost(url.hostname),
        port: url.port || (this.tls ? 443 : 80),
        path: url.pathname + url.search,
        headers,
        agent: this.agent,
      };
      if (this.tls) {
        if (this.fingerprint) opt.rejectUnauthorized = false; // checked below, before anything is sent
        else if (this.ca) opt.ca = this.ca;
      }
      const req = (this.tls ? https : http).request(opt, (res) => resolve({ req, res }));
      req.on('error', (e) => reject(e instanceof PotluckError ? e : new PotluckError('network', e.message, { code: e.code })));
      if (timeoutMs) {
        req.setTimeout(timeoutMs, () => req.destroy(new PotluckError('timeout', `no answer from the hub in ${timeoutMs} ms`)));
      }
      const send = () => req.end(payload || undefined);
      if (this.tls && this.fingerprint) this._pin(req, send);
      else send();
    });
  }

  /** Nothing is written to a socket whose certificate is not the announced one: the request is ended only after. */
  _pin(req, send) {
    req.once('socket', (s) => {
      const check = () => {
        const cert = s.getPeerCertificate ? s.getPeerCertificate() : null;
        const fp = cert && cert.fingerprint256 ? cert.fingerprint256.replace(/:/g, '').toLowerCase() : '';
        if (fp === this.fingerprint) {
          send();
        } else {
          req.destroy(new PotluckError('certificate',
            `the hub's certificate is not the announced one (got ${fp || 'none'}, expected ${this.fingerprint})`));
        }
      };
      const cert = s.getPeerCertificate ? s.getPeerCertificate() : null;
      if (cert && cert.fingerprint256) check();
      else s.once('secureConnect', check);
    });
  }

  async _respectShare() {
    const wait = this._slowUntil - Date.now();
    if (wait > 0) await sleep(wait);
  }

  _noteRetryAfter(header) {
    const ms = retryAfterMs(header);
    this._slowUntil = Math.max(this._slowUntil, Date.now() + ms);
    this.emit('problem', { kind: 'share', message: `the hub asked this program to slow down for ${ms} ms` });
    return ms;
  }

  /** One request, answered as {status, headers, json, text}. Waits out a Retry-After first. */
  async call(method, path, body, { timeoutMs } = {}) {
    await this._respectShare();
    const { res } = await this._open(method, path, { body, timeoutMs: timeoutMs ?? this.requestTimeoutMs });
    const text = await readAll(res);
    let json = null;
    try {
      json = text ? JSON.parse(text) : null;
    } catch (e) {
      json = null;
    }
    if (res.statusCode === 429) this._noteRetryAfter(res.headers['retry-after']);
    return { status: res.statusCode, headers: res.headers, json, text };
  }

  async getModel() {
    const r = await this.call('GET', '/api/model');
    if (r.status !== 200 || !r.json) throw statusError(r.status, r.json, r.text);
    return r.json;
  }

  async getEvents(since) {
    const q = since == null ? '' : `?since=${encodeURIComponent(since)}`;
    const r = await this.call('GET', `/api/events${q}`);
    if (r.status !== 200 || !r.json) throw statusError(r.status, r.json, r.text);
    return r.json;
  }

  /** The last record of every resource passed on, as 'resource' sent it. */
  resources() {
    return [...this._resLast.values()];
  }

  /** A path as the model writes it; "node-7368/led/on" (a resource's name) is completed from the cell's system. */
  resolvePath(p) {
    const s = String(p || '').trim();
    if (s.startsWith('potluck://')) return s;
    const system = this.model && this.model.cell && this.model.cell.system;
    if (system && s) return `potluck://${system}/${s.replace(/^\/+/, '')}`;
    return s;
  }

  /**
   * POST /api/write. Waits out a 429's Retry-After and tries again, up to writeRetries times.
   * Answers {ok, status, result, path, value}: status 0 when refused here, before the hub was asked.
   */
  async write(path, value, { numberType } = {}) {
    const full = this.resolvePath(path);
    const v = coerceValue(value);
    if (!full) return { ok: false, status: 0, result: 'no resource path (msg.topic or the node\'s path)', path: full, value };
    if (v === undefined) return { ok: false, status: 0, result: 'a bool or a number', path: full, value };
    const w = writeBody(full, v, numberType || this.numberType);
    if (w.refused) return { ok: false, status: 0, result: w.refused, path: full, value: v };
    let attempt = 0;
    for (;;) {
      const r = await this.call('POST', '/api/write', w.body);
      if (r.status === 429 && attempt < this.writeRetries) {
        attempt += 1;
        continue; // call() waits out the Retry-After before the next try
      }
      const result = (r.json && (r.json.result || r.json.error)) || `HTTP ${r.status}`;
      return { ok: r.status === 200 && !!(r.json && r.json.ok), status: r.status, result, path: full, value: v, tries: attempt + 1 };
    }
  }

  /** POST /api/integrations: this program's state on the hub's dashboard. */
  async report(state, detail = '') {
    if (!REPORT_STATES.includes(state)) throw new PotluckError('config', `a state is one of ${REPORT_STATES.join(', ')}`);
    const body = JSON.stringify({ name: this.name, state, detail: String(detail).slice(0, 200) });
    const r = await this.call('POST', '/api/integrations', body, { timeoutMs: 5000 });
    const accepted = r.status === 200 && !!(r.json && r.json.ok === true);
    this.lastReport = {
      at: Date.now(), state, detail, status: r.status, accepted,
      result: (r.json && (r.json.result || r.json.error)) || '',
    };
    this.emit('report', this.lastReport);
    return this.lastReport;
  }

  // ---------------------------------------------------------------- the stream, and the fallback

  start() {
    if (this.running) return;
    this.running = true;
    this._startedAt = Date.now();
    this._setConn('connecting', 'opening the stream');
    this._connectStream();
    if (this.reportIntervalMs > 0) {
      this._reportTimer = setInterval(() => this._reportNow(true), this.reportIntervalMs);
    }
  }

  async stop(detail = 'stopped') {
    const wasConnected = this.conn === 'streaming' || this.conn === 'polling';
    this.running = false;
    clearInterval(this._reportTimer);
    clearTimeout(this._streamRetryTimer);
    this._stopPolling();
    if (this._stream) {
      const s = this._stream;
      this._stream = null;
      clearTimeout(s.watchdog);
      if (s.req) s.req.destroy();
    }
    if (this.reportIntervalMs > 0 && wasConnected) {
      try {
        await this.report('disconnected', detail);
      } catch (e) { /* the hub may be the thing that went */ }
    }
    this._setConn('idle', detail);
    this.agent.destroy();
    this.agent = new (this.tls ? https : http).Agent({ keepAlive: true, maxSockets: 6 });
    // A later start() begins afresh: every resource and board is sent again as first seen.
    this._resKey.clear();
    this._resLast.clear();
    this._boardState.clear();
    this._lastModelAt = 0;
    this._reported = null;
  }

  _deadAfter() {
    if (this.deadAfterMs != null) return this.deadAfterMs;
    const pull = (this.model && this.model.hub && this.model.hub.pull_period_ms) || 2000;
    return Math.max(3 * pull, 10000);
  }

  async _connectStream() {
    if (!this.running || this._stream) return;
    clearTimeout(this._streamRetryTimer);
    const attempt = { req: null, watchdog: null };
    this._stream = attempt;
    let req;
    let res;
    try {
      await this._respectShare();
      // A socket silent for the dead time is dropped too, before or after the answer's headers.
      ({ req, res } = await this._open('GET', '/api/stream', { stream: true, timeoutMs: this._deadAfter() }));
    } catch (e) {
      if (this._stream === attempt) this._stream = null;
      this._streamFailed(e);
      return;
    }
    if (!this.running || this._stream !== attempt) {
      req.destroy();
      return;
    }
    if (res.statusCode !== 200) {
      const text = await readAll(res).catch(() => '');
      let json = null;
      try { json = JSON.parse(text); } catch (e) { /* not JSON */ }
      if (res.statusCode === 429) this._noteRetryAfter(res.headers['retry-after']);
      this._stream = null;
      this._streamFailed(statusError(res.statusCode, json, text));
      return;
    }
    attempt.req = req;
    res.setEncoding('utf8');
    const parser = new SseParser((ev) => this._onSse(ev, attempt));
    const end = (why) => {
      if (this._stream !== attempt) return;
      this._stream = null;
      clearTimeout(attempt.watchdog);
      req.destroy();
      this._streamFailed(new PotluckError('stream', why));
    };
    attempt.end = end;
    this._armWatchdog(attempt);
    res.on('data', (chunk) => {
      try {
        parser.push(chunk);
      } catch (e) {
        this.emit('problem', { kind: 'stream', message: e.message });
      }
    });
    res.on('error', (e) => end(e.message));
    res.on('close', () => end('the stream closed'));
  }

  _armWatchdog(attempt) {
    clearTimeout(attempt.watchdog);
    const ms = this._deadAfter();
    attempt.watchdog = setTimeout(() => attempt.end && attempt.end(`no model on the stream for ${ms} ms`), ms);
  }

  _onSse(ev, attempt) {
    if (this._stream !== attempt) return;
    let data;
    try {
      data = JSON.parse(ev.data);
    } catch (e) {
      this.emit('problem', { kind: 'stream', message: `a ${ev.event} event that is not JSON` });
      return;
    }
    if (ev.event === 'model') {
      this._armWatchdog(attempt);
      this._stopPolling();
      this._setConn('streaming', 'the stream');
      this._handleModel(data);
    } else if (ev.event === 'events') {
      this._handleEvents(data);
    }
  }

  _streamFailed(err) {
    if (!this.running) return;
    if (err && err.kind === 'auth') {
      this._setConn('refused', err.message);
    } else {
      this.emit('problem', { kind: 'stream', message: err ? err.message : 'the stream dropped' });
      this._startPolling();
      this._checkFresh(err ? err.message : 'the stream dropped');
    }
    clearTimeout(this._streamRetryTimer);
    this._streamRetryTimer = setTimeout(() => this._connectStream(), this.streamRetryMs);
  }

  _startPolling() {
    if (this._polling || !this.running) return;
    this._polling = true;
    const tick = async () => {
      if (!this._polling) return;
      try {
        const m = await this.getModel();
        if (this._polling) {
          this._setConn('polling', 'the stream is down; reading every 2 s');
          this._handleModel(m);
          const e = await this.getEvents(this._next);
          if (this._polling) this._handleEvents(e);
        }
      } catch (err) {
        if (this._polling) {
          if (err.kind === 'auth') this._setConn('refused', err.message);
          else this._checkFresh(err.message);
        }
      } finally {
        if (this._polling) this._pollTimer = setTimeout(tick, this.pollMs);
      }
    };
    tick();
  }

  _stopPolling() {
    this._polling = false;
    clearTimeout(this._pollTimer);
    this._pollTimer = null;
  }

  /** No model for the dead time: the values held are no longer known, so none is shown (INTEGRATIONS.md, 2). */
  _checkFresh(why) {
    if (this.conn === 'unreachable' || this.conn === 'refused') return;
    // Since the last model, or since start(): a hub gets the same dead time to send its first.
    if (Date.now() - Math.max(this._lastModelAt, this._startedAt || 0) < this._deadAfter()) return;
    this._hubLost(why);
  }

  _hubLost(why) {
    this._setConn('unreachable', why);
    for (const [path, last] of this._resLast) {
      const rec = { ...last, value: null, quality: 'UNAVAILABLE', hub_unreachable: true,
        board: last.board ? { ...last.board, state: 'unknown' } : null };
      this._resKey.set(path, this._key(rec));
      this._resLast.set(path, rec);
      this.emit('resource', { ...rec, initial: false });
    }
    for (const [node, state] of this._boardState) {
      if (state !== 'unknown') {
        this._boardState.set(node, 'unknown');
        this.emit('board', { node, label: null, state: 'unknown', previous: state, since_at: null, hub_unreachable: true });
      }
    }
  }

  _hubRestarted(why) {
    this._lastSeq = 0;
    this.emit('problem', { kind: 'hub', message: `the hub restarted (${why})` });
  }

  // ---------------------------------------------------------------- the model -> changes

  _key(rec) {
    return JSON.stringify([rec.value, rec.quality, rec.owner, rec.board ? rec.board.state : null]);
  }

  _record(r, boards, cell, at) {
    let quality = QUALITIES.includes(r.quality) ? r.quality : null;
    let value = typeof r.value === 'number' || typeof r.value === 'boolean' ? r.value : null;
    let note;
    if (!quality) {
      note = `the hub sent no known quality (${r.quality})`;
      quality = 'UNAVAILABLE';
    }
    if (NO_VALUE.has(quality)) value = null;
    const b = boards.get(String(r.owner));
    const rec = {
      path: r.path,
      name: r.name,
      value,
      unit: r.unit == null ? '' : r.unit,
      quality,
      age_ms: typeof r.age_ms === 'number' ? r.age_ms : null,
      owner: r.owner == null ? null : String(r.owner),
      latency_class: r.latency_class,
      exposed: r.exposed === true,
      outside_writable: r.outside_writable === true,
      safety_relevant: r.safety_relevant === true,
      board: b ? { node: String(b.node), label: b.label, state: b.state, since_at: b.since_at } : null,
      cell: cell.cell_id,
      system: cell.system,
      at,
    };
    if (note) rec.note = note;
    return rec;
  }

  _handleModel(m) {
    if (!m || typeof m !== 'object') return;
    if (m.api !== 1 && !this._warnedApi) {
      this._warnedApi = true;
      this.emit('problem', { kind: 'hub', message: `the hub speaks API ${m.api}; this client was written for 1` });
    }
    const upMs = m.hub && m.hub.up_ms;
    if (typeof upMs === 'number' && this._lastUpMs != null && upMs < this._lastUpMs) {
      this._hubRestarted('its uptime went down');
      this._next = null; // the events check would see the same restart again
    }
    if (typeof upMs === 'number') this._lastUpMs = upMs;
    this.model = m;
    this._lastModelAt = Date.now();
    const cell = m.cell || {};
    const boards = new Map();
    for (const b of m.boards || []) {
      const node = String(b.node);
      boards.set(node, b);
      const prev = this._boardState.get(node);
      if (prev !== b.state) {
        this._boardState.set(node, b.state);
        this.emit('board', { node, label: b.label, state: b.state, previous: prev === undefined ? null : prev,
          since_at: b.since_at, initial: prev === undefined });
      }
    }
    const seen = new Set();
    for (const r of m.resources || []) {
      if (!r || !r.path) continue;
      if (!this.includeUnexposed && r.exposed !== true) continue;
      const rec = this._record(r, boards, cell, m.at);
      const key = this._key(rec);
      const prev = this._resKey.get(rec.path);
      seen.add(rec.path);
      this._resLast.set(rec.path, rec);
      if (prev !== key) {
        this._resKey.set(rec.path, key);
        this.emit('resource', { ...rec, initial: prev === undefined });
      }
    }
    for (const [path, last] of [...this._resLast]) {
      if (seen.has(path)) continue;
      this._resLast.delete(path);
      this._resKey.delete(path);
      this.emit('resource', { ...last, value: null, quality: 'UNAVAILABLE', removed: true, initial: false });
    }
    this.emit('model', m);
  }

  _handleEvents(e) {
    if (!e || !Array.isArray(e.events)) return;
    const next = typeof e.next === 'number' ? e.next : null;
    if (next != null && this._next != null && next < this._next) {
      this._hubRestarted('its event numbers went back');
      this._lastUpMs = null; // the uptime check would see the same restart again
    }
    const first = this._lastSeq == null;
    const evs = e.events.filter((x) => x && typeof x.seq === 'number').sort((a, b) => a.seq - b.seq);
    let last = this._lastSeq == null ? -Infinity : this._lastSeq;
    for (const ev of evs) {
      if (ev.seq <= last) continue;
      last = ev.seq;
      if (!first || this.replayHistory) this.emit('event', ev);
    }
    if (last !== -Infinity) this._lastSeq = last;
    else if (first && next != null) this._lastSeq = next - 1;
    if (next != null) this._next = next;
  }

  // ---------------------------------------------------------------- the state report

  _setConn(state, detail) {
    const changed = state !== this.conn || detail !== this.connDetail;
    const stateChanged = state !== this.conn;
    this.conn = state;
    this.connDetail = detail;
    if (changed) this.emit('state', { state, detail });
    if (stateChanged && this.running) this._reportNow(false);
  }

  /** What INTEGRATIONS.md step 5 is told, from the connection's state. null: nothing to say yet. */
  reportState() {
    const n = this._resLast.size;
    const link = this.model && this.model.hub && this.model.hub.link;
    const linkNote = link && link !== 'up' ? `; the hub's link to the cell is ${link}` : '';
    switch (this.conn) {
      case 'streaming': return { state: 'connected', detail: `stream, ${n} resource(s)${linkNote}` };
      case 'polling': return { state: 'connected', detail: `polling every 2 s (stream down), ${n} resource(s)${linkNote}` };
      case 'refused': return { state: 'error', detail: this.connDetail };
      case 'unreachable': return { state: 'disconnected', detail: this.connDetail };
      default: return null;
    }
  }

  _reportNow(periodic) {
    if (this.reportIntervalMs <= 0 || !this.running) return;
    const s = this.reportState();
    if (!s) return;
    const key = `${s.state}|${s.detail}`;
    if (!periodic && key === this._reported) return;
    this._reported = key;
    this.report(s.state, s.detail).catch((e) => {
      this.lastReport = { at: Date.now(), state: s.state, detail: s.detail, status: 0, accepted: false, result: e.message };
      this.emit('report', this.lastReport);
    });
  }
}

// ---------------------------------------------------------------- pairing (INTEGRATIONS.md step 2)

/**
 * Ask the hub for a token and wait while the owner clicks Allow on the hub's dashboard. Nothing is typed here.
 * Resolves {token, name, right}; rejects with a PotluckError (kind 'pair') when denied or expired.
 * onState({state: 'pending', pair_id, expires_in_s}) is called once the hub has the request.
 */
async function pair({ url, fingerprint, ca, name = 'Node-RED', right = 'look', pollMs = 2000, onState, signal } = {}) {
  if (right !== 'look' && right !== 'write') throw new PotluckError('config', 'a right is "look" or "write"');
  const c = new PotluckClient({ url, fingerprint, ca, name, reportIntervalMs: 0 });
  try {
    if (!c.tls) {
      throw new PotluckError('config',
        'pairing is only on a hub started with --listen (https); on 127.0.0.1 no token is needed');
    }
    if (!c.fingerprint && !c.ca) {
      throw new PotluckError('config', 'give the hub\'s certificate fingerprint (announced as fp=) before pairing');
    }
    const r = await c.call('POST', '/api/pair', JSON.stringify({ name, right }));
    if (r.status === 429) throw new PotluckError('pair', 'the hub already has as many pairing requests waiting as it takes; try again in a few minutes');
    if (r.status !== 202 || !r.json || !r.json.pair_id) throw statusError(r.status, r.json, r.text);
    const id = String(r.json.pair_id);
    const expiresS = Number(r.json.expires_in_s) || 300;
    if (onState) onState({ state: 'pending', pair_id: id, expires_in_s: expiresS });
    const deadline = Date.now() + expiresS * 1000 + 5000;
    while (Date.now() < deadline) {
      if (signal && signal.aborted) throw new PotluckError('pair', 'pairing cancelled');
      await sleep(pollMs);
      const p = await c.call('GET', `/api/pair/${encodeURIComponent(id)}`);
      if (p.status === 404) throw new PotluckError('pair', 'the request expired (or was already fetched) before it was approved');
      if (p.status === 429) continue;
      if (p.status !== 200 || !p.json) throw statusError(p.status, p.json, p.text);
      if (p.json.state === 'approved' && p.json.token) {
        return { token: String(p.json.token), name: p.json.name || name, right: p.json.right || right };
      }
      if (p.json.state === 'denied') throw new PotluckError('pair', 'the owner denied the request');
    }
    throw new PotluckError('pair', 'nobody answered the request on the hub before it expired');
  } finally {
    c.agent.destroy();
  }
}

module.exports = {
  PotluckClient,
  PotluckError,
  SseParser,
  pair,
  coerceValue,
  writeBody,
  normaliseFingerprint,
  retryAfterMs,
  QUALITIES,
  REPORT_STATES,
};
