#!/usr/bin/env node
// potluck-watch.mjs -- reads, watches and writes a Potluck cell through the hub's API version 1.
//
// The specification is host/potluck/HUB-API.md and nothing else. Node 18 or newer, no packages: the built-in
// fetch, web streams and TextDecoder. See README.md beside this file.
//
//   node potluck-watch.mjs [--hub URL]                    read the model, then watch until Ctrl+C
//   node potluck-watch.mjs [--hub URL] read               read the model once
//   node potluck-watch.mjs [--hub URL] watch [--for S]    watch: changes and new events
//   node potluck-watch.mjs [--hub URL] write PATH VALUE   write one resource; VALUE is true, false or a number
//   node potluck-watch.mjs [--hub URL] --once             read, write, be refused, watch the write land, restore
//
// The hub is --hub, else the environment variable POTLUCK_HUB, else http://127.0.0.1:8470 (the hub on this PC,
// which needs no login). A hub reached over https:// asks for a login: the name and password come from the
// environment variables POTLUCK_HUB_USER and POTLUCK_HUB_PASSWORD, never from the command line, and are never
// printed.

import process from "node:process";

const API_VERSION = 1;
const DEFAULT_HUB = "http://127.0.0.1:8470";
const POLL_MS = 2000; // while the stream is down, GET /api/model and /api/events this often (as the dashboard does)

const USAGE = `usage:
  node potluck-watch.mjs [--hub URL]                    read the model, then watch until Ctrl+C
  node potluck-watch.mjs [--hub URL] read               read the model once
  node potluck-watch.mjs [--hub URL] watch [--for S]    print changes and new events (for S seconds)
  node potluck-watch.mjs [--hub URL] write PATH VALUE   write a resource; VALUE is true, false or a number
  node potluck-watch.mjs [--hub URL] --once             read, write, be refused, watch the write land, restore

options:
  --hub URL          the hub (default: $POTLUCK_HUB, else ${DEFAULT_HUB})
  --for S            stop watching after S seconds
  --ignore TEXT      do not print value changes of resources whose path contains TEXT; repeatable.
                     sys/uptime (it moves every second) is ignored from the start; --ignore "" clears the list
  --retry-ms MS      while polling, try the stream again this often (default 15000)

A login (https:// hubs): POTLUCK_HUB_USER and POTLUCK_HUB_PASSWORD in the environment.`;

// ---- the command line ---------------------------------------------------------------------------------------------

function parseArgs(argv) {
  const o = { hub: process.env.POTLUCK_HUB || DEFAULT_HUB, cmd: null, rest: [], forS: null, ignore: ["sys/uptime"],
              retryMs: 15000, once: false };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const val = () => {
      if (i + 1 >= argv.length) throw new UsageError(`${a} needs a value`);
      return argv[++i];
    };
    if (a === "--hub") o.hub = val();
    else if (a === "--for") o.forS = Number(val());
    else if (a === "--ignore") { const t = val(); o.ignore = t === "" ? [] : [...o.ignore, t]; }
    else if (a === "--retry-ms") o.retryMs = Number(val());
    else if (a === "--once") o.once = true;
    else if (a === "-h" || a === "--help") { console.log(USAGE); process.exit(0); }
    else if (a.startsWith("--")) throw new UsageError(`unknown option ${a}`);
    else if (o.cmd === null) o.cmd = a;
    else o.rest.push(a);
  }
  if (o.cmd !== null && !["read", "watch", "write"].includes(o.cmd)) throw new UsageError(`unknown command ${o.cmd}`);
  if (o.cmd === "write" && o.rest.length !== 2) throw new UsageError("write needs PATH and VALUE");
  if (o.cmd !== "write" && o.rest.length) throw new UsageError(`unexpected ${o.rest[0]}`);
  if (o.once && o.cmd !== null) throw new UsageError("--once is a command of its own");
  if (o.forS !== null && !(o.forS > 0)) throw new UsageError("--for needs a number of seconds");
  if (!(o.retryMs > 0)) throw new UsageError("--retry-ms needs a number of milliseconds");
  return o;
}

class UsageError extends Error {}

// ---- talking to the hub -------------------------------------------------------------------------------------------

class HttpError extends Error {
  constructor(status, body) {
    super(`HTTP ${status}${body && body.result ? ": " + body.result : ""}`);
    this.status = status;
    this.body = body;
  }
}

class Hub {
  constructor(base) {
    this.url = new URL(base);
    this.base = this.url.href.replace(/\/+$/, "");
    this.cookie = null; // "potluck_session=<token>" once logged in
  }

  // One request. A POST always carries X-Potluck-Request: 1 (the hub wants it on every POST behind a login, and it
  // costs nothing without one). A 401 means "log in first": log in once and ask again.
  async request(method, path, { body, signal } = {}) {
    for (let attempt = 0; ; attempt++) {
      const headers = { Accept: path === "/api/stream" ? "text/event-stream" : "application/json" };
      if (this.cookie) headers.Cookie = this.cookie;
      if (method === "POST") {
        headers["Content-Type"] = "application/json";
        headers["X-Potluck-Request"] = "1";
      }
      const res = await fetch(this.base + path, {
        method, headers, signal, body: body === undefined ? undefined : JSON.stringify(body),
      });
      if (res.status === 401 && attempt === 0 && path !== "/api/login") {
        await res.body?.cancel().catch(() => {});
        this.cookie = null;
        await this.login();
        continue;
      }
      return res;
    }
  }

  async json(method, path, opts) {
    const res = await this.request(method, path, opts);
    const body = await res.json().catch(() => null);
    if (!res.ok) throw new HttpError(res.status, body);
    return body;
  }

  model() { return this.json("GET", "/api/model"); }

  events(since) { return this.json("GET", "/api/events" + (since == null ? "" : `?since=${since}`)); }

  // POST /api/write: the hub's answer and its status, whatever it is (a refusal is an answer, not an error).
  async write(path, value) {
    const res = await this.request("POST", "/api/write", { body: { path, value } });
    const body = await res.json().catch(() => ({}));
    return { status: res.status, body };
  }

  async login() {
    const name = process.env.POTLUCK_HUB_USER;
    const password = process.env.POTLUCK_HUB_PASSWORD;
    if (!name || !password) {
      throw new Error("the hub asks for a login: set POTLUCK_HUB_USER and POTLUCK_HUB_PASSWORD in the environment");
    }
    const loopback = ["127.0.0.1", "localhost", "[::1]"].includes(this.url.hostname);
    if (this.url.protocol !== "https:" && !loopback) {
      throw new Error("the hub asks for a login over plain http: refusing to send a password unencrypted");
    }
    const res = await fetch(this.base + "/api/login", {
      method: "POST",
      headers: { "Content-Type": "application/json", "X-Potluck-Request": "1", Accept: "application/json" },
      body: JSON.stringify({ name, password }),
    });
    const body = await res.json().catch(() => ({}));
    if (res.status !== 200 || !body.ok) {
      throw new Error(`login refused (HTTP ${res.status}${body.result ? ": " + body.result : ""})`);
    }
    const token = sessionCookie(res.headers);
    if (!token) throw new Error("login accepted but the hub set no potluck_session cookie");
    this.cookie = `potluck_session=${token}`;
    say(`logged in, right: ${body.right}`); // the name is the user's own and is not printed
  }
}

function sessionCookie(headers) {
  const all = typeof headers.getSetCookie === "function" ? headers.getSetCookie() : [headers.get("set-cookie") || ""];
  for (const line of all) {
    const m = /(?:^|[,;]\s*)potluck_session=([^;,\s]*)/.exec(line);
    if (m && m[1]) return m[1];
  }
  return null;
}

// ---- formatting: a value never travels without its unit, quality and age ------------------------------------------

function say(line) { console.log(line); }

function clock(at) {
  if (typeof at !== "number") return "--:--:--";
  const d = new Date(at);
  return [d.getHours(), d.getMinutes(), d.getSeconds()].map((x) => String(x).padStart(2, "0")).join(":");
}

function dur(ms) {
  if (typeof ms !== "number") return "?";
  if (ms < 1000) return `${Math.round(ms)} ms`;
  const s = ms / 1000;
  if (s < 60) return `${s.toFixed(1)} s`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m} min ${Math.floor(s % 60)} s`;
  const h = Math.floor(m / 60);
  if (h < 48) return `${h} h ${m % 60} min`;
  return `${Math.floor(h / 24)} d ${h % 24} h`;
}

// "51.5 C, GOOD, 2.7 s old" -- or "no value, UNAVAILABLE, 12.0 s old". A number with no unit says so.
function tuple(r) {
  let v;
  if (r.value === null || r.value === undefined) v = "no value";
  else if (typeof r.value === "boolean") v = String(r.value);
  else v = `${r.value} ${r.unit ? r.unit : "(no unit)"}`;
  return `${v}, ${r.quality ?? "quality ?"}, ${dur(r.age_ms)} old`;
}

function boardName(b) { return b.label ? `${b.label} (${b.node})` : b.node; }

function resName(r) { return r.name || r.path; }

function byKey(list, key) { return new Map((list || []).map((x) => [x[key], x])); }

function printModel(m, hub) {
  if (m.api !== API_VERSION) say(`note: the hub speaks API ${m.api}; this program was written for ${API_VERSION}`);
  const h = m.hub || {}, c = m.cell || {};
  say(`Hub ${hub.base} -- version ${h.version}, API ${m.api}, model at ${clock(m.at)}, ` +
      `pull every ${dur(h.pull_period_ms)}, clock error ${dur(h.clock_error_ms)}`);
  say(`  link to the cell: ${h.link}${h.port ? " on " + h.port : ""}${h.link_detail ? " -- " + h.link_detail : ""}`);
  say(`Cell ${c.system} / ${c.cell_id}, channel ${c.channel}, package ${c.package}, firmware ${c.firmware}` +
      (c.presenter ? `, presented by ${c.presenter}` : "") +
      (c.other_cell_frames ? `, another cell heard (${c.other_cell_frames} frames)` : ""));
  say("");
  const boards = m.boards || [];
  say(`Boards (${boards.length})`);
  const bw = Math.max(0, ...boards.map((b) => boardName(b).length));
  for (const b of boards) {
    const fw = b.firmware || {}, pk = b.package || {};
    say(`  ${boardName(b).padEnd(bw)}  ${String(b.state).padEnd(7)} since ${clock(b.since_at)}` +
        (b.silent_ms ? `, silent ${dur(b.silent_ms)}` : "") +
        `  ${b.cabled ? "cabled  " : ""}firmware ${fw.version} #${fw.counter} ${fw.state}` +
        `  package ${pk.counter} ${pk.state}`);
  }
  say("");
  const res = m.resources || [];
  say(`Resources (${res.length})`);
  const rw = Math.max(0, ...res.map((r) => resName(r).length));
  for (const r of res) {
    const flags = [r.outside_writable ? "writable from outside" : "", r.safety_relevant ? "safety-relevant" : ""]
      .filter(Boolean).join(", ");
    say(`  ${resName(r).padEnd(rw)}  ${tuple(r)}${flags ? "  [" + flags + "]" : ""}`);
  }
}

function eventLine(e) {
  const detail = e.detail && Object.keys(e.detail).length ? " " + JSON.stringify(e.detail) : "";
  return `${clock(e.at)}  event #${e.seq}  ${e.board ?? "-"}  ${e.kind}${detail}`;
}

// What changed between two models: the hub's link, boards' states, resources' values and qualities.
function changes(prev, cur, ignore) {
  const out = [];
  const t = clock(cur.at);
  const ph = prev.hub || {}, ch = cur.hub || {};
  if (ph.link !== ch.link) {
    out.push(`${t}  hub link to the cell: ${ph.link} -> ${ch.link}${ch.link_detail ? " (" + ch.link_detail + ")" : ""}`);
  }
  const pb = byKey(prev.boards, "node");
  const cb = byKey(cur.boards, "node");
  for (const [n, b] of cb) {
    const p = pb.get(n);
    if (!p) out.push(`${t}  board ${boardName(b)} appeared: ${b.state} since ${clock(b.since_at)}`);
    else if (p.state !== b.state) {
      out.push(`${t}  board ${boardName(b)}: ${p.state} -> ${b.state} since ${clock(b.since_at)}` +
               (b.silent_ms ? `, silent ${dur(b.silent_ms)}` : ""));
    }
  }
  for (const [, p] of pb) if (!cb.has(p.node)) out.push(`${t}  board ${boardName(p)} is no longer in the model`);
  const pr = byKey(prev.resources, "path");
  const cr = byKey(cur.resources, "path");
  for (const [path, r] of cr) {
    const p = pr.get(path);
    if (!p) { out.push(`${t}  new resource ${resName(r)}: ${tuple(r)}`); continue; }
    const qualityChanged = p.quality !== r.quality;
    const valueChanged = p.value !== r.value;
    if (qualityChanged || (valueChanged && !ignore.some((s) => path.includes(s)))) {
      out.push(`${t}  ${resName(r)}: ${tuple(p)} -> ${tuple(r)}`);
    }
  }
  for (const [path, p] of pr) if (!cr.has(path)) out.push(`${t}  resource ${resName(p)} is no longer in the model`);
  return out;
}

// ---- watching: the stream, with polling while it is down -----------------------------------------------------------

class Watcher {
  constructor(hub, opts) {
    this.hub = hub;
    this.ignore = opts.ignore;
    this.retryMs = opts.retryMs;
    this.onModel = opts.onModel || (() => {});
    this.onEvent = opts.onEvent || (() => {});
    this.model = opts.model || null; // the last model seen: changes are printed against it
    this.next = null; // the next event sequence number to ask for
    this.seen = new Set();
    this.firstBatch = true;
    this.stopper = new AbortController();
    this.done = new Promise((resolve) => { this.finish = resolve; });
  }

  stop() { this.stopper.abort(); this.finish(); }

  get stopped() { return this.stopper.signal.aborted; }

  takeModel(m) {
    if (this.model) for (const line of changes(this.model, m, this.ignore)) say(line);
    this.model = m;
    this.onModel(m);
  }

  takeEvents(obj) {
    const evs = (obj && obj.events) || [];
    if (this.next != null && obj.next != null && obj.next < this.next) {
      say("hub restarted: its event numbers began again");
      this.seen.clear();
      this.firstBatch = true;
    }
    const fresh = evs.filter((e) => !this.seen.has(e.seq));
    for (const e of fresh) this.seen.add(e.seq);
    let shown = fresh;
    if (this.firstBatch) {
      // The first batch is the hub's backlog (up to the last 200): print its tail, not all of it.
      this.firstBatch = false;
      if (fresh.length > 5) {
        say(`(${fresh.length} earlier events; the last 5:)`);
        shown = fresh.slice(-5);
      }
    }
    for (const e of shown) say(eventLine(e));
    for (const e of fresh) this.onEvent(e);
    if (obj && obj.next != null) this.next = obj.next;
  }

  // Run until stop(): the stream while it works; when it drops, try it again at once, then poll every 2 s and try the
  // stream again every retryMs.
  async run() {
    let retryNow = true;
    while (!this.stopped) {
      const r = await this.streamOnce();
      if (this.stopped) break;
      if (r.gotModel) retryNow = true; // it worked for a while: one quick retry before falling back
      if (retryNow) {
        say(`stream dropped (${r.why}); reconnecting in ${dur(r.retryMs)}`);
        retryNow = false;
        await this.sleep(r.retryMs);
        continue;
      }
      say(`stream unavailable (${r.why}); polling every ${dur(POLL_MS)}, trying the stream again in ${dur(this.retryMs)}`);
      await this.poll(this.retryMs);
      if (!this.stopped) say("trying the stream again");
    }
  }

  async streamOnce() {
    const ctl = new AbortController();
    const onStop = () => ctl.abort();
    this.stopper.signal.addEventListener("abort", onStop);
    let retryMs = 2000, gotModel = false, why = "closed by the hub", quiet = null;
    // A stream that stops sending models without closing is as good as dropped: three pull periods, at least 10 s.
    const quietMs = () => Math.max(10000, 3 * ((this.model && this.model.hub && this.model.hub.pull_period_ms) || 2000));
    const arm = () => {
      clearTimeout(quiet);
      quiet = setTimeout(() => { why = `no model for ${dur(quietMs())}`; ctl.abort(); }, quietMs());
    };
    try {
      arm();
      const res = await this.hub.request("GET", "/api/stream", { signal: ctl.signal });
      if (!res.ok) {
        await res.body?.cancel().catch(() => {});
        why = `HTTP ${res.status}`;
        return { gotModel, why, retryMs };
      }
      if (!gotModel && !this.announced) { say(`watching ${this.hub.base}/api/stream`); this.announced = true; }
      const dec = new TextDecoder();
      let buf = "", event = "message", data = [];
      for await (const chunk of res.body) {
        buf += dec.decode(chunk, { stream: true });
        let nl;
        while ((nl = buf.indexOf("\n")) >= 0) {
          let line = buf.slice(0, nl);
          buf = buf.slice(nl + 1);
          if (line.endsWith("\r")) line = line.slice(0, -1);
          if (line === "") { // a blank line ends one server-sent event
            if (data.length) {
              const text = data.join("\n");
              let obj = null;
              try { obj = JSON.parse(text); } catch { say(`(an unreadable ${event} event was skipped)`); }
              if (obj && event === "model") { gotModel = true; arm(); this.takeModel(obj); }
              else if (obj && event === "events") this.takeEvents(obj);
            }
            event = "message"; data = [];
            continue;
          }
          if (line.startsWith(":")) continue; // a comment
          const colon = line.indexOf(":");
          const field = colon < 0 ? line : line.slice(0, colon);
          let value = colon < 0 ? "" : line.slice(colon + 1);
          if (value.startsWith(" ")) value = value.slice(1);
          if (field === "event") event = value;
          else if (field === "data") data.push(value);
          else if (field === "retry" && /^\d+$/.test(value)) retryMs = Number(value);
        }
      }
    } catch (e) {
      if (!ctl.signal.aborted) why = e.cause ? `${e.message}: ${e.cause.code || e.cause.message}` : e.message;
      else if (this.stopped) why = "stopped";
    } finally {
      clearTimeout(quiet);
      this.stopper.signal.removeEventListener("abort", onStop);
    }
    return { gotModel, why, retryMs };
  }

  async poll(forMs) {
    const until = Date.now() + forMs;
    let failing = false;
    while (!this.stopped && Date.now() < until) {
      try {
        this.takeModel(await this.hub.model());
        this.takeEvents(await this.hub.events(this.next));
        if (failing) say("the hub answers again");
        failing = false;
      } catch (e) {
        if (!failing) say(`no answer from the hub (${e.cause ? e.cause.code || e.cause.message : e.message})`);
        failing = true;
      }
      await this.sleep(POLL_MS);
    }
  }

  sleep(ms) {
    return new Promise((resolve) => {
      const t = setTimeout(done, ms);
      const sig = this.stopper.signal;
      function done() { clearTimeout(t); sig.removeEventListener("abort", done); resolve(); }
      sig.addEventListener("abort", done);
    });
  }
}

// ---- the commands -------------------------------------------------------------------------------------------------

function parseValue(text) {
  let v;
  try { v = JSON.parse(text); } catch { v = undefined; }
  if (typeof v !== "boolean" && typeof v !== "number") throw new UsageError("VALUE is true, false or a number");
  return v;
}

function writeLine(path, value, r) {
  const verdict = r.status === 200 && r.body.ok ? "applied" : r.status === 403 ? "refused" : "failed";
  return `write ${path} = ${JSON.stringify(value)}: ${verdict} (HTTP ${r.status}, ${JSON.stringify(r.body)})`;
}

async function cmdWrite(hub, path, value) {
  const r = await hub.write(path, value);
  say(writeLine(path, value, r));
  return r.status === 200 && r.body.ok ? 0 : r.status === 403 ? 3 : 1;
}

async function cmdWatch(hub, opts, model) {
  const w = new Watcher(hub, { ignore: opts.ignore, retryMs: opts.retryMs, model });
  process.once("SIGINT", () => w.stop());
  if (opts.forS) setTimeout(() => w.stop(), opts.forS * 1000).unref?.();
  await Promise.race([w.run(), w.done]);
  w.stop();
  return 0;
}

// --once: everything the program can do, once, against whatever cell the hub has -- and the cell left as it was.
async function cmdOnce(hub, opts) {
  const ok = [];
  const fail = (why) => { say(`FAILED: ${why}`); return 1; };
  const m = await hub.model();
  printModel(m, hub);
  say("");
  const res = m.resources || [];
  const target = res.find((r) => r.outside_writable && !r.safety_relevant &&
                                 (typeof r.value === "boolean" || typeof r.value === "number"));
  const closed = res.find((r) => !r.outside_writable && !r.safety_relevant);
  if (!target) return fail("no resource with a value is writable from outside in this package");
  if (!closed) return fail("every resource is writable from outside: nothing to be refused");

  const before = target.value;
  const wanted = typeof before === "boolean" ? !before : before; // a number is written back unchanged
  const w1 = await hub.write(target.path, wanted);
  say(writeLine(target.path, wanted, w1));
  if (w1.status !== 200 || !w1.body.ok) return fail("the writable resource was not written");
  ok.push("write applied");

  const refusedValue = typeof closed.value === "boolean" || typeof closed.value === "number" ? closed.value : 0;
  const w2 = await hub.write(closed.path, refusedValue);
  say(writeLine(closed.path, refusedValue, w2));
  if (w2.status !== 403) return fail(`a write to ${closed.path} was not refused with 403`);
  ok.push("write refused (403)");
  say("");

  // Watch until the hub reports the write (an outside_write event) and a model shows the new value.
  let sawEvent = false, sawValue = false;
  const w = new Watcher(hub, {
    ignore: opts.ignore, retryMs: opts.retryMs, model: m,
    onEvent: (e) => {
      const d = e.detail || {};
      if (e.kind === "outside_write" && (d.resource === target.path || d.path === target.path) &&
          d.result === "applied") sawEvent = true;
      if (sawEvent && sawValue) w.stop();
    },
    onModel: (mm) => {
      const r = (mm.resources || []).find((x) => x.path === target.path);
      if (r && r.value === wanted) sawValue = true;
      if (sawEvent && sawValue) w.stop();
    },
  });
  const limit = setTimeout(() => w.stop(), (opts.forS || 20) * 1000);
  process.once("SIGINT", () => w.stop());
  await Promise.race([w.run(), w.done]);
  clearTimeout(limit);
  w.stop();
  say("");
  if (sawValue) ok.push("the model shows the new value");
  if (sawEvent) ok.push("the hub reported the write as an event");

  if (wanted !== before) {
    const w3 = await hub.write(target.path, before);
    say(writeLine(target.path, before, w3) + "  (put back as it was)");
    if (w3.status !== 200 || !w3.body.ok) return fail("could not put the resource back");
    ok.push("put back");
  }
  if (!sawValue) return fail(`no model showed ${target.path} = ${JSON.stringify(wanted)}`);
  if (!sawEvent) return fail(`no outside_write event for ${target.path}`);
  say(`OK: ${ok.join("; ")}`);
  return 0;
}

async function main(argv) {
  let opts;
  try {
    opts = parseArgs(argv);
  } catch (e) {
    if (e instanceof UsageError) { console.error(`${e.message}\n\n${USAGE}`); return 2; }
    throw e;
  }
  const hub = new Hub(opts.hub);
  if (hub.url.protocol === "https:" && process.env.POTLUCK_HUB_USER) await hub.login();
  if (opts.once) return cmdOnce(hub, opts);
  if (opts.cmd === "write") return cmdWrite(hub, opts.rest[0], parseValue(opts.rest[1]));
  if (opts.cmd === "watch") return cmdWatch(hub, opts, null);
  const m = await hub.model();
  printModel(m, hub);
  if (opts.cmd === "read") return 0;
  say("");
  return cmdWatch(hub, opts, m);
}

// Output piped into something that stops reading (`| head`): stop quietly.
process.stdout.on("error", (e) => { if (e.code === "EPIPE") process.exit(0); throw e; });

main(process.argv.slice(2)).then(
  (code) => { process.exitCode = code; },
  (e) => {
    if (e instanceof UsageError) { console.error(`${e.message}\n\n${USAGE}`); process.exitCode = 2; return; }
    const code = e.cause && e.cause.code ? String(e.cause.code) : "";
    const why = e.cause ? `${e.message}: ${code || e.cause.message}` : e.message;
    console.error(`potluck-watch: ${why}`);
    if (/CERT|SELF_SIGNED|UNABLE_TO_VERIFY/.test(code)) {
      console.error("  the hub's certificate is not one Node trusts: start with NODE_EXTRA_CA_CERTS=<the hub's certificate>");
    }
    process.exitCode = 1;
  },
);
