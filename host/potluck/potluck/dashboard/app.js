/* Potluck's dashboard (M16), first cut. Reads the hub's API version 1 (host/potluck/HUB-API.md).
 *
 * One source: GET /api/stream (server-sent events: `model` once per pull period, `events` as they arrive). If the
 * stream fails, GET /api/model and /api/events every 2 s, and try the stream again every 15 s.
 * Rights on this page: look, and write what the package lets the outside write (`outside_writable`). No deploy.
 */
"use strict";
(function () {
  var POLL_MS = 2000, RETRY_STREAM_MS = 15000, MAX_EVENTS = 2000;
  var S = {
    model: null, recvAt: 0, mode: "connecting", es: null, pollTimer: null, retryTimer: null,
    events: [], seen: {}, next: null, writes: {}, tab: "cell", focus: "", evDirty: true, evOptions: "",
    who: { login: false, name: "local", right: "write" }  // M16: who this page is, from api/whoami
  };

  // ---- helpers ------------------------------------------------------------------------------------------------
  function $(id) { return document.getElementById(id); }
  function esc(s) {
    return String(s == null ? "" : s).replace(/[&<>"']/g, function (c) {
      return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c];
    });
  }
  // "3 s", "4 min", "2 h 5 min", "3 d"
  function dur(ms) {
    if (ms == null || !isFinite(ms)) return "-";
    var s = Math.floor(Math.max(0, ms) / 1000);
    if (s < 60) return s + " s";
    var m = Math.floor(s / 60);
    if (m < 60) return m + " min";
    var h = Math.floor(m / 60);
    if (h < 24) return m % 60 ? h + " h " + (m % 60) + " min" : h + " h";
    return Math.floor(h / 24) + " d";
  }
  // The hub's clock now: the model's `at` plus what has passed here since it arrived.
  function replay() { return S.model && S.model.hub && S.model.hub.replay; }
  function hubNow() {
    var r = replay();
    if (r) return r.position_at + (r.paused ? 0 : (Date.now() - S.recvAt) * r.speed);  // the recording's clock
    return S.model ? S.model.at + (Date.now() - S.recvAt) : Date.now();
  }
  // A span showing how long ago a hub-clock time was; the ticker keeps it current.
  function ago(at, pre, post) {
    if (at == null) return "-";
    return '<span class="age" data-at="' + at + '" data-pre="' + esc(pre || "") + '" data-post="' + esc(post || "") +
      '">' + esc(pre || "") + dur(hubNow() - at) + esc(post || "") + "</span>";
  }
  function pad(n) { return n < 10 ? "0" + n : "" + n; }
  function clock(at) {
    var d = new Date(at), t = pad(d.getHours()) + ":" + pad(d.getMinutes()) + ":" + pad(d.getSeconds());
    var today = new Date(hubNow());
    return d.toDateString() === today.toDateString() ? t : d.getFullYear() + "-" + pad(d.getMonth() + 1) + "-" +
      pad(d.getDate()) + " " + t;
  }
  function kb(n) { return n == null ? "-" : (n / 1024).toFixed(1) + " KB"; }
  function mb(n) { return n == null ? "-" : (n / 1048576).toFixed(1) + " MB"; }
  function pct(x, digits) { return x == null ? "-" : (x * 100).toFixed(digits == null ? 0 : digits) + " %"; }
  function num(x, unit) { return x == null ? "-" : x + (unit ? " " + unit : ""); }
  function badge(text, cls) { return '<span class="badge ' + cls + '">' + esc(text) + "</span>"; }
  function boardsBy(m) { var o = {}; (m.boards || []).forEach(function (b) { o[b.node] = b; }); return o; }
  function bname(m, node) {
    if (node === "hub") return "hub";
    var b = boardsBy(m)[node];
    return b && b.label ? b.label + " " + node : node;
  }
  function blink(m, node) {
    if (!node || node === "hub") return esc(node || "-");
    return '<a href="#cell/' + esc(node) + '">' + esc(bname(m, node)) + "</a>";
  }
  function fid(s) { return "f-" + String(s).replace(/[^A-Za-z0-9_-]/g, "_"); }
  function td(label, html, cls) { return '<td data-label="' + esc(label) + '"' + (cls ? ' class="' + cls + '"' : "") + "><div>" + html + "</div></td>"; }
  function table(cols, rows, cls) {
    return '<table class="rt ' + (cls || "") + '"><thead><tr>' + cols.map(function (c) { return "<th>" + esc(c) + "</th>"; }).join("") +
      "</tr></thead><tbody>" + rows.join("") + "</tbody></table>";
  }
  var STATE_CLS = { alive: "ok", dead: "bad", asleep: "sleep", unknown: "warn", connected: "ok", disconnected: "bad",
    idle: "idle", off: "idle", active: "ok", failed: "bad", verified: "ok", confirmed: "ok", reverted: "bad",
    refused: "bad", running: "warn", up: "ok", down: "bad", GOOD: "ok", STALE: "warn", FAULTY: "bad",
    UNAVAILABLE: "bad" };
  function sbadge(state) { return badge(state, STATE_CLS[state] || "idle"); }
  function period() { return (S.model && S.model.hub && S.model.hub.pull_period_ms) || POLL_MS; }
  function modelAge() { return S.model ? Date.now() - S.recvAt : Infinity; }
  function modelLate() { return modelAge() > 3 * period() + 2000; }

  // ---- layer 1: is anything broken? ---------------------------------------------------------------------------
  function problems(m) {
    var P = [], bb = boardsBy(m);
    var add = function (sev, html, href) { P.push({ sev: sev, html: html, href: href }); };
    if (m.hub && m.hub.link !== "up")
      add("bad", "The hub's link to the cell is " + esc(m.hub.link) + (m.hub.link_detail ? ": " + esc(m.hub.link_detail) : "") +
        " -- board states below are as last heard", "#cell/hub");
    (m.boards || []).forEach(function (b) {
      if (b.state === "dead") add("bad", "Board " + esc(bname(m, b.node)) + " dead " + ago(b.since_at, "for "), "#cell/" + b.node);
      else if (b.state === "unknown") add("warn", "Board " + esc(bname(m, b.node)) + " never heard since the hub started", "#cell/" + b.node);
      if (b.firmware && b.firmware.state && b.firmware.state !== "confirmed" && b.state !== "dead")
        add("warn", "Board " + esc(bname(m, b.node)) + ": firmware " + esc(b.firmware.counter) + " " + esc(b.firmware.state), "#cell/" + b.node);
      if (b.package && b.package.state && b.package.state !== "confirmed" && b.state !== "dead")
        add("warn", "Board " + esc(bname(m, b.node)) + ": package " + esc(b.package.counter) + " " + esc(b.package.state), "#cell/" + b.node);
      (b.modules || []).forEach(function (md) {
        if (md.state === "failed") add("bad", "Module " + esc(md.name) + " failed on " + esc(bname(m, b.node)) +
          (md.reason ? ": " + esc(md.reason) : ""), "#cell/" + b.node);
      });
    });
    var res = {};
    (m.resources || []).forEach(function (r) { res[r.path] = r; });
    var shown = {};
    (m.apps || []).forEach(function (a) {
      var badOut = (a.outputs || []).map(function (p) { return res[p]; })
        .filter(function (r) { return r && r.quality !== "GOOD" && !shown[r.path]; })
        .filter(function (r) { return !(bb[r.owner] && bb[r.owner].state === "dead"); });  // the dead board is the headline
      var placed = a.placement === "portable" ? (a.running_on || []).length === 1 :
        (a.nodes || []).every(function (n) { return (a.running_on || []).indexOf(n) >= 0; });
      if (!a.working && placed && badOut.length) {  // the outputs are the reason: say so, with an age that ticks
        var r = badOut[0];
        shown[r.path] = 1;
        add("bad", "App " + esc(a.name) + " not working: output " + esc(r.name) + " " + esc(r.quality) + " " +
          ago(m.at - r.age_ms, "for "), "#data/" + r.path);
      } else if (!a.working) add("bad", "App " + esc(a.name) + " not working" + (a.detail ? ": " + esc(a.detail) : ""), "#apps/" + a.name);
      badOut.forEach(function (r) {
        if (shown[r.path]) return;
        shown[r.path] = 1;
        add(r.quality === "STALE" ? "warn" : "bad", "Output " + esc(r.name) + " " + esc(r.quality) + " " +
          ago(m.at - r.age_ms, "for "), "#data/" + r.path);
      });
    });
    (m.integrations || []).forEach(function (i) {
      if (i.state === "disconnected") add("bad", "Integration " + esc(i.name) + " disconnected " + ago(i.since_at, "for ") +
        (i.detail ? ": " + esc(i.detail) : ""), "#integrations/" + i.name);
    });
    var d = lastDeploy(m);
    if (d && ["reverted", "refused", "failed"].indexOf(d.result) >= 0)
      add("bad", "Last deploy (" + esc(d.kind) + " " + esc(d.counter) + ") " + esc(d.result) + " " + ago(d.at, "", " ago") +
        (d.detail ? ": " + esc(d.detail) : ""), "#deploys/" + d.at);
    else if (d && d.result === "running")
      add("warn", "Deploy in progress (" + esc(d.kind) + " " + esc(d.counter) + ")", "#deploys/" + d.at);
    if (m.cell && m.cell.other_cell_frames > 0)
      add("warn", "Another cell shares channel " + esc(m.cell.channel) + " (" + esc(m.cell.other_cell_frames) +
        " frames heard)", "#cell/channel");
    P.sort(function (a, b) { return (a.sev === "bad" ? 0 : 1) - (b.sev === "bad" ? 0 : 1); });
    return P;
  }
  function lastDeploy(m) {
    var d = (m.deploys || []).slice().sort(function (a, b) { return b.at - a.at; });
    return d[0];
  }
  function setTile(id, cls, value, sub) {
    var el = $(id);
    el.className = "tile " + cls;
    el.querySelector(".tile-v").innerHTML = value;
    el.querySelector(".tile-s").innerHTML = sub;
  }
  function renderStatus(m) {
    var P = problems(m), bad = P.filter(function (p) { return p.sev === "bad"; }).length, warn = P.length - bad;
    var boards = m.boards || [], apps = m.apps || [], ints = m.integrations || [];
    var alive = boards.filter(function (b) { return b.state === "alive"; }).length;
    var asleep = boards.filter(function (b) { return b.state === "asleep"; });
    var dead = boards.filter(function (b) { return b.state === "dead"; });
    var unknown = boards.filter(function (b) { return b.state === "unknown"; });
    var working = apps.filter(function (a) { return a.working; }).length;
    // A role a board holds (presenting) is listed per board, but it is one integration: connected when any board
    // holds it; the others standing by are not "not connected".
    var byName = {};
    ints.forEach(function (i) {
      var k = i.kind === "board" ? "board:" + i.name : "program:" + i.name, c = byName[k];
      if (!c) { byName[k] = { name: i.name, state: i.state }; return; }
      if (i.state === "connected" || (i.state === "disconnected" && c.state !== "connected")) c.state = i.state;
    });
    var roles = Object.keys(byName).map(function (k) { return byName[k]; });
    var inUse = roles.filter(function (i) { return i.state !== "off"; });
    var conn = roles.filter(function (i) { return i.state === "connected"; }).length;
    var disc = roles.filter(function (i) { return i.state === "disconnected"; });
    var d = lastDeploy(m), hub = m.hub || {}, cell = m.cell || {};

    var v = $("verdict");
    v.className = "verdict " + (bad ? "bad" : warn ? "warn" : "ok");
    var nb = bad === 1 ? "1 problem" : bad + " problems", nw = warn === 1 ? "1 warning" : warn + " warnings";
    $("verdict-title").textContent = bad && warn ? nb + ", " + nw : bad ? nb : warn ? nw : "All is well";
    $("verdict-text").innerHTML = bad || warn ? esc(alive + " of " + boards.length + " boards alive" + (asleep.length ? ", " + asleep.length + " asleep" : "") + ", " + working + " of " +
      apps.length + " apps working.") :
      esc(alive + " of " + boards.length + " boards alive" + (asleep.length ? ", " + asleep.length + " asleep" : "") +
        ", all " + apps.length + " apps working, every integration in use connected, the last deploy " +
        (d ? d.result : "none") + ", the hub's link up" + (cell.channel != null ? ", no other cell on channel " + cell.channel : "") + ".");
    $("problems").innerHTML = P.map(function (p) {
      return '<li class="' + p.sev + '"><a href="' + esc(p.href) + '"><span class="pmark" aria-hidden="true"></span><span>' +
        p.html + "</span></a></li>";
    }).join("");

    var bsub = dead.map(function (b) { return esc(b.label || b.node) + " dead " + ago(b.since_at, "", ""); })
      .concat(unknown.map(function (b) { return esc(b.label || b.node) + " never heard"; }))
      .concat(asleep.map(function (b) { return esc(b.label || b.node) + " asleep " + ago(b.since_at, "", ""); }));
    setTile("sum-boards", dead.length ? "bad" : unknown.length ? "warn" : "ok",
      alive + "<small>/" + boards.length + "</small>", bsub.join(", ") || "all alive");
    var notw = apps.filter(function (a) { return !a.working; }).map(function (a) { return esc(a.name); });
    setTile("sum-apps", notw.length ? "bad" : "ok", working + "<small>/" + apps.length + "</small>",
      notw.length ? "not: " + notw.join(", ") : "all working");
    var isub = disc.length ? disc.map(function (i) { return esc(i.name) + " down"; }).join(", ") :
      roles.filter(function (i) { return i.state !== "connected"; }).map(function (i) { return esc(i.name) + " " + esc(i.state); }).join(", ");
    setTile("sum-integrations", disc.length ? "bad" : "ok", conn + "<small>/" + inUse.length + " connected</small>", isub || "all connected");
    if (d) {
      var dcls = ["reverted", "refused", "failed"].indexOf(d.result) >= 0 ? "bad" : d.result === "running" ? "warn" : "ok";
      setTile("sum-deploy", dcls, esc(d.result), esc(d.kind) + " " + esc(d.counter) + ", " + ago(d.at, "", " ago"));
    } else setTile("sum-deploy", "idle", "none", "");
    setTile("sum-hub", hub.link === "up" ? "ok" : "bad", esc(hub.link || "-"),
      esc(hub.port || "") + (hub.clock_error_ms != null ? ", clock &plusmn;" + esc(hub.clock_error_ms) + " ms" : ""));
    var oc = cell.other_cell_frames || 0;
    setTile("sum-channel", oc > 0 ? "warn" : "ok", esc(cell.channel),
      oc > 0 ? "another cell: " + esc(oc) + " frames" : "no other cell heard");
  }

  // ---- layer 2: golden signals --------------------------------------------------------------------------------
  function renderSignals(m) {
    var g = m.signals || {}, sat = g.saturation || {}, top = null;
    Object.keys(sat).forEach(function (k) { if (top === null || sat[k] > sat[top]) top = k; });
    var sc = top === null ? "idle" : sat[top] >= 0.8 ? "bad" : sat[top] >= 0.6 ? "warn" : "ok";
    var t = function (href, cls, k, v, s) {
      return '<a class="tile ' + cls + '" href="' + href + '"><span class="tile-k">' + k + '</span><span class="tile-v">' + v +
        '</span><span class="tile-s">' + s + "</span></a>";
    };
    $("signals-tiles").innerHTML =
      t("#use-links", "ok", "Latency", num(g.latency_p50_ms) + "<small> / " + num(g.latency_p99_ms, "ms") + "</small>", "round trip of the hub's reads, p50 / p99") +
      t("#use-links", "ok", "Traffic", num(g.traffic_frames_s) + "<small> frames/s</small>", "the whole cell") +
      t("#events", g.errors_min > 5 ? "bad" : g.errors_min > 1 ? "warn" : "ok", "Errors", num(g.errors_min) + "<small> /min</small>",
        "losses, deaths, refusals") +
      t("#use", sc, "Saturation", top === null ? "-" : pct(sat[top]), top === null ? "" : "highest: " + esc(top.replace(/_/g, " ")));
  }

  // ---- layer 3: per resource ----------------------------------------------------------------------------------
  function bar(frac) {
    var c = frac >= 0.8 ? "bad" : frac >= 0.6 ? "warn" : "ok";
    return '<span class="bar"><span class="fill ' + c + '" style="width:' + Math.min(100, Math.max(0, frac * 100)).toFixed(1) + '%"></span></span>';
  }
  function renderUse(m) {
    var sat = (m.signals || {}).saturation || {};
    $("use-sat").innerHTML = Object.keys(sat).map(function (k) {
      return '<div class="barrow"><span class="bar-k">' + esc(k.replace(/_/g, " ")) + "</span>" + bar(sat[k]) +
        '<span class="bar-v">' + pct(sat[k]) + "</span></div>";
    }).join("") || '<p class="muted">No saturation figures.</p>';
    $("use-heap").innerHTML = table(["Board", "Internal free", "Lowest", "Largest block", "PSRAM free"],
      (m.boards || []).map(function (b) {
        var h = b.heap || {}, snap = b.state !== "alive" ? ' <span class="muted small">(as last heard)</span>' : "";
        return "<tr>" + td("Board", blink(m, b.node) + snap) + td("Internal free", kb(h.internal_free)) +
          td("Lowest", kb(h.internal_min)) + td("Largest block", kb(h.internal_largest)) + td("PSRAM free", mb(h.psram_free)) + "</tr>";
      }));
    var rows = [];
    (m.boards || []).forEach(function (b) {
      (b.links || []).forEach(function (l) {
        var snap = b.state !== "alive" ? ' <span class="muted small">(as last heard)</span>' : "";
        rows.push("<tr>" + td("Link", blink(m, b.node) + " &rarr; " + blink(m, l.peer) + snap) + td("State", sbadge(l.state)) +
          td("Delivery", l.delivery == null ? "-" : pct(l.delivery, 2)) +
          td("RTT p50 / p99", num(l.rtt_p50_ms) + " / " + num(l.rtt_p99_ms, "ms")) + td("Lost", num(l.lost)) +
          td("RSSI", num(l.rssi, "dBm")) + "</tr>");
      });
    });
    var hub = m.hub || {};
    rows.push("<tr>" + td("Link", '<a href="#cell/hub">hub</a> &rarr; ' + esc(hub.port || "cable") + ' <span class="muted small">(serial: ' +
      esc(num(hub.frames_rx)) + " rx, " + esc(num(hub.frames_tx)) + " tx frames)</span>") + td("State", sbadge(hub.link || "unknown")) +
      td("Delivery", "-") + td("RTT p50 / p99", "-") + td("Lost", "-") + td("RSSI", "-") + "</tr>");
    $("use-links").innerHTML = table(["Link", "State", "Delivery", "RTT p50 / p99", "Lost", "RSSI"], rows);
  }

  // ---- layer 4: tabs ------------------------------------------------------------------------------------------
  // M18: every program using the hub, with what it asked and what was refused (each has a share).
  function clientRows(cs) {
    if (!cs || !cs.length) return "";
    return "<dt>Programs</dt><dd>" + cs.map(function (c) {
      return esc(c.name) + " " + esc(num(c.requests)) + " asks" + (c.refused ? ", " + badge(c.refused + " slowed", "warn") : "");
    }).join("<br>") + "</dd>";
  }
  // M17: a program asking to be paired (Home Assistant); only the PC's own page may answer.
  function pairingBlock(ps) {
    if (!ps || !ps.length) return "";
    var local = !S.who.login;
    return '<div class="pairing"><h4>Asking to connect</h4>' + ps.map(function (p) {
      return '<div class="pair-req"><span>' + esc(p.name) + ' <span class="muted small">(' + esc(p.right) + ", from " +
        esc(p.from) + ")</span></span>" + (local ? ' <button type="button" data-pair="' + esc(p.pair_id) +
        '" data-approve="1">Allow</button> <button type="button" data-pair="' + esc(p.pair_id) +
        '" data-approve="0">Refuse</button>' : ' <span class="muted small">answer on the hub\'s PC</span>') + "</div>";
    }).join("") + "</div>";
  }
  document.addEventListener("click", function (e) {
    var b = e.target;
    if (b.matches && b.matches("button[data-forget]")) {
      b.disabled = true;
      post("api/integrations/forget", { name: b.getAttribute("data-forget") });
      return;
    }
    if (!b.matches || !b.matches("button[data-pair]")) return;
    b.disabled = true;
    post("api/pairing", { pair_id: b.getAttribute("data-pair"), approve: b.getAttribute("data-approve") === "1" });
  });
  function retentionRows(r) {  // M16: how much history the hub keeps, said where the hub is described
    if (!r) return "";
    var h = "<dt>History</dt><dd>" + esc(num(r.events_held)) + " of " + esc(num(r.events_kept)) + " events kept" +
      (r.oldest_event_at ? ", since " + clock(r.oldest_event_at) : "") + "</dd>";
    var rec = r.recording;
    h += "<dt>Recording</dt><dd>" + (rec ? "the last " + esc(rec.hours) + " h, " + mb(rec.bytes) + " in " + esc(rec.files) +
      " file" + (rec.files === 1 ? "" : "s") : "off (start the hub with --record to replay later)") + "</dd>";
    return h;
  }
  function renderCell(m) {
    var cell = m.cell || {}, hub = m.hub || {}, all = [];
    (m.boards || []).forEach(function (b) { (b.modules || []).forEach(function (md) { if (all.indexOf(md.name) < 0) all.push(md.name); }); });
    var h = '<div class="cards">';
    h += '<div class="card" id="' + fid("channel") + '"><h3>Cell ' + esc(cell.system) + "</h3><dl>" +
      "<dt>Cell id</dt><dd><code>" + esc(cell.cell_id) + "</code></dd><dt>Channel</dt><dd>" + esc(cell.channel) +
      (cell.other_cell_frames > 0 ? " " + badge("another cell: " + cell.other_cell_frames + " frames", "warn") : " " + badge("alone", "ok")) +
      "</dd><dt>Firmware</dt><dd><code>" + esc(cell.firmware) + "</code></dd><dt>Package</dt><dd>" + esc(cell.package) +
      "</dd><dt>Presenter</dt><dd>" + (cell.presenter ? blink(m, cell.presenter) : "none") + "</dd></dl></div>";
    h += '<div class="card" id="' + fid("hub") + '"><h3>Hub ' + sbadge(hub.link || "unknown") + "</h3><dl>" +
      "<dt>Link</dt><dd>" + esc(hub.port) + (hub.link_detail ? " -- " + esc(hub.link_detail) : "") + "</dd>" +
      "<dt>Version</dt><dd><code>" + esc(hub.version) + "</code></dd><dt>Up</dt><dd>" + dur(hub.up_ms) + "</dd>" +
      "<dt>Frames</dt><dd>" + esc(num(hub.frames_rx)) + " rx, " + esc(num(hub.frames_tx)) + " tx</dd>" +
      "<dt>Pull period</dt><dd>" + num(hub.pull_period_ms, "ms") + "</dd><dt>Clock error</dt><dd>&plusmn;" +
      num(hub.clock_error_ms, "ms") + "</dd>" + retentionRows(hub.retention) + clientRows(hub.clients) +
      (hub.tls_fp ? "<dt>Certificate</dt><dd><code class=\"fp\">" + esc(hub.tls_fp.match(/../g).join(":").toUpperCase()) + "</code></dd>" : "") + "</dl>" +
      pairingBlock(hub.pairing) + "</div>";
    (m.boards || []).forEach(function (b) {
      var fw = b.firmware || {}, pk = b.package || {}, hp = b.heap || {}, mods = {};
      (b.modules || []).forEach(function (md) { mods[md.name] = md; });
      var since = b.state === "dead" ? ago(b.since_at, "dead for ") : b.state === "asleep" ? ago(b.since_at, "asleep for ") :
        b.state === "alive" ? ago(b.since_at, "up since ", " ago") : "never heard";
      h += '<div class="card board ' + (STATE_CLS[b.state] || "") + '" id="' + fid(b.node) + '"><h3>' + esc(b.label || "") +
        " <code>" + esc(b.node) + "</code> " + sbadge(b.state) + (b.cabled ? " " + badge("cabled", "idle") : "") +
        (b.room ? ' <span class="small muted">' + esc(b.room) + "</span>" : "") + "</h3>" +
        '<p class="small muted">' + since + (b.silent_ms ? ", silent " + dur(b.silent_ms) + " at the snapshot" : "") + "</p><dl>" +
        "<dt>Firmware</dt><dd><code>" + esc(fw.version) + "</code> #" + esc(fw.counter) + " " + sbadge(fw.state || "unknown") + "</dd>" +
        "<dt>Package</dt><dd>#" + esc(pk.counter) + " " + sbadge(pk.state || "unknown") +
        (pk.live_applies != null ? ' <span class="small muted">' + esc(pk.live_applies) + " live apply(s)</span>" : "") + "</dd>" +
        "<dt>Roles</dt><dd>" + (b.roles || []).map(function (r) { return badge(r, "role"); }).join(" ") + "</dd>" +
        "<dt>Modules</dt><dd>" + all.map(function (n) {
          var md = mods[n];
          if (!md) return '<span class="badge absent" title="not on this board\'s image">' + esc(n) + " not here</span>";
          return '<span class="badge ' + (STATE_CLS[md.state] || "idle") + '" title="' + esc(md.reason || md.state) + '">' + esc(n) +
            " " + esc(md.state) + "</span>";
        }).join(" ") + "</dd>" +
        "<dt>Actors</dt><dd>" + ((b.actors || []).map(esc).join(", ") || "none") + "</dd>" +
        "<dt>Heap</dt><dd>" + kb(hp.internal_free) + " free, lowest " + kb(hp.internal_min) + ", largest " + kb(hp.internal_largest) +
        "; PSRAM " + mb(hp.psram_free) + "</dd>" +
        "<dt>Up</dt><dd>" + dur(b.up_ms) + "</dd><dt>RSSI</dt><dd>" + num(b.rssi, "dBm") + "</dd>" +
        "<dt>Links</dt><dd>" + ((b.links || []).map(function (l) {
          return blink(m, l.peer) + " " + sbadge(l.state) + " " + (l.delivery == null ? "-" : pct(l.delivery, 2)) + ", " +
            num(l.rtt_p50_ms) + "/" + num(l.rtt_p99_ms, "ms") + (l.lost ? ", " + l.lost + " lost" : "");
        }).join("<br>") || "none") + "</dd></dl></div>";
    });
    $("tab-cell").innerHTML = h + "</div>";
  }

  function renderApps(m) {
    var res = {};
    (m.resources || []).forEach(function (r) { res[r.path] = r; });
    $("tab-apps").innerHTML = table(["App", "Module", "Placement", "Runs on", "Working", "Outputs"],
      (m.apps || []).map(function (a) {
        var where = (a.running_on || []).map(function (n) { return blink(m, n); }).join(", ") || '<span class="bad-t">nowhere</span>';
        var of = a.placement === "portable" ? " (one of " + (a.nodes || []).map(esc).join(", ") + ")" :
          " (of " + (a.nodes || []).map(esc).join(", ") + ")";
        return '<tr id="' + fid(a.name) + '">' + td("App", "<b>" + esc(a.name) + "</b>") + td("Module", "<code>" + esc(a.module) + "</code>") +
          td("Placement", esc(a.placement)) + td("Runs on", where + '<span class="small muted">' + of + "</span>") +
          td("Working", a.working ? badge("working", "ok") : badge("not working", "bad") +
            (a.detail ? '<div class="small">' + esc(a.detail) + "</div>" : "")) +
          td("Outputs", (a.outputs || []).map(function (p) {
            var r = res[p];
            return '<a class="path" href="#data/' + esc(p) + '">' + esc(r ? r.name : p) + "</a>" + (r ? " " + sbadge(r.quality) : "");
          }).join("<br>")) + "</tr>";
      }));
  }

  function writeControl(r) {
    var w = S.writes[r.path], res = "";
    if (w) res = '<span class="wres ' + (w.pending ? "idle" : w.ok ? "ok" : "bad") + '">' + esc(w.pending ? "sending" : w.text) +
      (w.pending ? "" : " " + '<span class="age-local" data-t="' + w.t + '">' + dur(Date.now() - w.t) + "</span> ago") + "</span>";
    if (replay()) return '<span class="muted small">writable; this is a replay</span>';
    if (S.who.right !== "write") return '<span class="muted small">writable; this login may only look</span>';
    var dis = w && w.pending ? " disabled" : "";
    var p = esc(r.path), c;
    if (typeof r.value === "boolean")
      c = '<label class="switch"><input type="checkbox" role="switch" data-write="' + p + '" data-type="bool"' +
        (r.value ? " checked" : "") + dis + '><span class="track"></span><span class="small">' + (r.value ? "on" : "off") + "</span></label>";
    else if (typeof r.value === "number")
      c = '<form class="wform" data-write="' + p + '" data-type="number"><input type="number" step="any" inputmode="decimal" value="' +
        esc(r.value) + '" aria-label="New value"' + dis + '><button type="submit"' + dis + ">Set</button></form>";
    else  // no value to show: the type is unknown, so take true, false or a number
      c = '<form class="wform" data-write="' + p + '" data-type="any"><input type="text" placeholder="true, false or a number" aria-label="New value"' +
        dis + '><button type="submit"' + dis + ">Set</button></form>";
    return '<div class="wctl">' + c + res + "</div>";
  }

  function renderData(m) {
    var act = document.activeElement;
    if (act && $("data-body").contains(act) && act.tagName === "INPUT" && act.type !== "checkbox") return;  // typing
    var f = ($("data-filter").value || "").toLowerCase();
    var rows = (m.resources || []).slice().sort(function (a, b) { return a.path < b.path ? -1 : 1; })
      .filter(function (r) {
        return !f || (r.path + " " + r.owner + " " + bname(m, r.owner) + " " + r.quality).toLowerCase().indexOf(f) >= 0;
      }).map(function (r) {
        var at = m.at - r.age_ms;  // when the value was produced, on the hub's clock
        var val = r.value === null || r.value === undefined ? '<span class="muted">-</span>' :
          typeof r.value === "boolean" ? (r.value ? "true" : "false") : esc(r.value) + (r.unit ? " " + esc(r.unit) : "");
        var flags = [r.exposed ? badge("exposed", "role") : "", r.outside_writable ? badge("writable", "warn") : "",
          r.safety_relevant ? badge("safety", "bad") : "", badge("L" + r.latency_class, "idle")].join(" ");
        return '<tr id="' + fid(r.path) + '">' + td("Resource", '<span class="path" title="' + esc(r.path) + '">' + esc(r.name) + "</span>") +
          td("Owner", blink(m, r.owner), "nw") + td("Value", '<span class="val">' + val + "</span>", "nw") +
          td("Quality", sbadge(r.quality)) + td("Age", ago(at), "nw") + td("Flags", flags) +
          td("Write", r.outside_writable ? writeControl(r) : '<span class="muted small">read only</span>') + "</tr>";
      });
    $("data-body").innerHTML = rows.length ? table(["Resource", "Owner", "Value", "Quality", "Age", "Flags", "Write"], rows) :
      '<p class="muted">No resource matches.</p>';
  }

  function renderIntegrations(m) {
    $("tab-integrations").innerHTML = table(["Integration", "Kind", "Where", "State", "Since", "Detail"],
      (m.integrations || []).map(function (i) {
        return '<tr id="' + fid(i.name) + '">' + td("Integration", "<b>" + esc(i.name) + "</b>") + td("Kind", esc(i.kind)) +
          td("Where", blink(m, i.where)) + td("State", sbadge(i.state)) + td("Since", ago(i.since_at, "", " ago")) +
          td("Detail", esc(i.detail || "") + (i.kind === "program" && i.state !== "connected" && !S.who.login ?
            ' <button type="button" data-forget="' + esc(i.name) + '">Forget</button>' : "")) + "</tr>";
      })) + '<p class="muted small">A program that stopped for good, or a test, can be forgotten here on the hub\'s PC; ' +
        'it comes back the next time it reports.</p>';
  }

  function renderDeploys(m) {
    var d = (m.deploys || []).slice().sort(function (a, b) { return b.at - a.at; });
    $("deploys-body").innerHTML = d.length ? table(["When", "Kind", "Counter", "Result", "Detail"], d.map(function (x) {
      return '<tr id="' + fid(x.at) + '">' + td("When", clock(x.at) + ' <span class="small muted">' + ago(x.at, "", " ago") + "</span>") +
        td("Kind", esc(x.kind)) + td("Counter", esc(x.counter)) + td("Result", sbadge(x.result)) + td("Detail", esc(x.detail || "")) + "</tr>";
    })) : '<p class="muted">No deploy recorded.</p>';
  }

  function fmtDetail(d) {
    if (d == null) return "";
    if (typeof d !== "object") return esc(d);
    return Object.keys(d).map(function (k) {
      var v = d[k];
      return '<span class="kv"><span class="muted">' + esc(k) + "</span>=" + esc(typeof v === "object" ? JSON.stringify(v) : v) + "</span>";
    }).join(" ");
  }
  function renderEvents() {
    var m = S.model || { boards: [] };
    var boards = {}, kinds = {};
    S.events.forEach(function (e) { boards[e.board] = 1; kinds[e.kind] = 1; });
    var opts = Object.keys(boards).sort().join(",") + "|" + Object.keys(kinds).sort().join(",");
    if (opts !== S.evOptions) {  // rebuild the selects only when their choices change, keeping the selection
      S.evOptions = opts;
      [["ev-board", boards, function (b) { return bname(m, b); }], ["ev-kind", kinds, function (k) { return k; }]].forEach(function (x) {
        var sel = $(x[0]), cur = sel.value;
        sel.innerHTML = '<option value="">all</option>' + Object.keys(x[1]).sort().map(function (v) {
          return '<option value="' + esc(v) + '"' + (v === cur ? " selected" : "") + ">" + esc(x[2](v)) + "</option>";
        }).join("");
      });
    }
    var fb = $("ev-board").value, fk = $("ev-kind").value;
    var list = S.events.filter(function (e) { return (!fb || e.board === fb) && (!fk || e.kind === fk); });
    var shown = list.slice(-300).reverse();
    $("ev-count").textContent = list.length + " event(s)" + (list.length > shown.length ? ", newest " + shown.length + " shown" : "");
    $("events-body").innerHTML = shown.map(function (e) {
      var cls = /dead|down|fail|refused|revert/.test(e.kind) || (e.detail && /refused|reverted|failed/.test(e.detail.result || "")) ? "bad" :
        /alive|up|started/.test(e.kind) ? "ok" : "";
      return '<li class="' + cls + '"><span class="ev-t" title="' + esc(new Date(e.at).toISOString()) + '">' + clock(e.at) + "</span>" +
        '<span class="ev-b">' + (e.board === "hub" ? "hub" : blink(m, e.board)) + '</span><span class="ev-k">' + esc(e.kind) +
        '</span><span class="ev-d">' + fmtDetail(e.detail) + "</span></li>";
    }).join("") || '<li class="muted">No events.</li>';
    S.evDirty = false;
  }

  // M20: the floor plan (plan.js). Editable on the PC's own address only, where the hub has /api/setup/*.
  function renderPlan(m) {
    if (!window.PotluckPlan) return;
    window.PotluckPlan.render($("tab-plan"), m, !S.who.login, function () {
      getJSON("api/model").then(function (fresh) { onModel(fresh); }).catch(function () {});
    });
  }

  // M20: the library (library.js) -- the PC's own address only; it keeps its own state, so the model's arrival
  // every few seconds does not redraw it.
  var libDrawn = false;
  function renderLibrary() {
    if (!window.PotluckLibrary || (libDrawn && S.tab === "library")) return;
    libDrawn = true;
    window.PotluckLibrary.render($("tab-library"), !S.who.login);
  }

  // ---- routing --------------------------------------------------------------------------------------------------
  var TABS = ["cell", "plan", "library", "apps", "data", "integrations", "deploys", "events"];
  function route(scroll) {
    var h = decodeURIComponent((location.hash || "").slice(1)), i = h.indexOf("/");
    var tab = i < 0 ? h : h.slice(0, i), focus = i < 0 ? "" : h.slice(i + 1);
    if (TABS.indexOf(tab) < 0) return;  // a plain anchor (#use, #signals...): the browser scrolls to it
    if (S.tab !== tab) libDrawn = false;  // the library is drawn again each time it is opened
    S.tab = tab; S.focus = focus;
    TABS.forEach(function (t) {
      $("tab-" + t).hidden = t !== tab;
      var a = document.querySelector('#tabs [data-tab="' + t + '"]');
      a.setAttribute("aria-selected", t === tab ? "true" : "false");
      a.classList.toggle("active", t === tab);
    });
    if (tab === "data" && focus) $("data-filter").value = "";
    renderTab();
    if (scroll) {
      var el = focus ? document.getElementById(fid(focus)) : null;
      document.querySelectorAll(".focus").forEach(function (x) { x.classList.remove("focus"); });
      if (el) { el.classList.add("focus"); el.scrollIntoView({ block: "center" }); }
      else $("tabs").scrollIntoView({ block: "start" });
    }
  }
  function renderTab() {
    var m = S.model;
    if (S.tab === "events") return renderEvents();
    if (!m) return;
    ({ cell: renderCell, plan: renderPlan, library: renderLibrary, apps: renderApps, data: renderData,
       integrations: renderIntegrations, deploys: renderDeploys })[S.tab](m);
    if (S.focus) { var el = document.getElementById(fid(S.focus)); if (el) el.classList.add("focus"); }
  }

  // ---- data in --------------------------------------------------------------------------------------------------
  function onModel(m) {
    if (!m || m.api !== 1) { console.warn("unexpected model", m); }
    S.model = m; S.recvAt = Date.now();
    var c = m.cell || {};
    $("cell-name").textContent = (c.system || "") + (c.cell_id ? " / " + c.cell_id : "");
    $("hub-meta").textContent = "Hub " + ((m.hub || {}).version || "?") + ", API " + m.api + ".";
    document.title = "Potluck " + (c.system || "cell");
    var first = !S.rendered;
    S.rendered = true;
    renderReplay(m); renderStatus(m); renderSignals(m); renderUse(m); renderTab(); tick();
    if (first && S.focus) route(true);  // a link with a focus, opened fresh: scroll to it once the model is here
  }
  function onEvents(obj) {
    if (S.next != null && obj.next != null && obj.next < S.next) {  // the hub restarted: its sequence began again
      S.events = []; S.seen = {}; S.evOptions = "";
      if (!(obj.events || []).length) { S.next = null; return; }  // polling asked past the new end: start over
    }
    (obj.events || []).forEach(function (e) {
      if (S.seen[e.seq]) return;
      S.seen[e.seq] = 1; S.events.push(e);
    });
    S.events.sort(function (a, b) { return a.seq - b.seq; });
    while (S.events.length > MAX_EVENTS) delete S.seen[S.events.shift().seq];
    if (obj.next != null) S.next = obj.next;
    if (S.tab === "events") renderEvents(); else S.evDirty = true;
  }
  function setMode(mode) {
    S.mode = mode; tick();
  }
  function startStream() {
    clearTimeout(S.retryTimer);
    if (!window.EventSource) { startPolling(); return; }
    if (S.es) S.es.close();
    var es = new EventSource("api/stream");
    S.es = es;
    es.addEventListener("model", function (e) {
      try { onModel(JSON.parse(e.data)); } catch (err) { console.error(err); return; }
      stopPolling(); setMode("live");
    });
    es.addEventListener("events", function (e) { try { onEvents(JSON.parse(e.data)); } catch (err) { console.error(err); } });
    es.onerror = function () {
      es.close(); if (S.es === es) S.es = null;
      startPolling();
      S.retryTimer = setTimeout(startStream, RETRY_STREAM_MS);
    };
  }
  function getJSON(url) {
    return fetch(url, { cache: "no-store" }).then(function (r) {
      if (r.status === 401) { showLogin("Your session ended: log in again."); throw new Error("login"); }
      if (!r.ok) throw new Error(url + ": " + r.status); return r.json();
    });
  }
  function pollOnce() {
    getJSON("api/model").then(function (m) {
      onModel(m); setMode("polling");
      return getJSON("api/events" + (S.next != null ? "?since=" + S.next : "")).then(onEvents);
    }).catch(function () { setMode("lost"); });
  }
  function startPolling() {
    if (S.pollTimer) return;
    setMode(S.model ? "polling" : "lost");
    pollOnce();
    S.pollTimer = setInterval(pollOnce, POLL_MS);
  }
  function stopPolling() { if (S.pollTimer) { clearInterval(S.pollTimer); S.pollTimer = null; } }

  // ---- writes ---------------------------------------------------------------------------------------------------
  function sendWrite(path, value) {
    S.writes[path] = { pending: true, t: Date.now() };
    if (S.model && S.tab === "data") renderData(S.model);
    fetch("api/write", { method: "POST", headers: { "Content-Type": "application/json", "X-Potluck-Request": "1" }, body: JSON.stringify({ path: path, value: value }) })
      .then(function (r) {
        return r.json().catch(function () { return {}; }).then(function (b) {
          S.writes[path] = { ok: r.ok && b.ok === true, text: b.result || (r.ok ? "done" : "HTTP " + r.status), t: Date.now() };
        });
      }).catch(function (e) { S.writes[path] = { ok: false, text: "no answer from the hub", t: Date.now() }; })
      .then(function () { if (S.model && S.tab === "data") { if (document.activeElement) document.activeElement.blur(); renderData(S.model); } });
  }
  document.addEventListener("change", function (e) {
    var t = e.target;
    if (t.matches && t.matches('input[type="checkbox"][data-write]')) sendWrite(t.getAttribute("data-write"), t.checked);
    if (t.id === "ev-board" || t.id === "ev-kind") renderEvents();
  });
  document.addEventListener("submit", function (e) {
    var f = e.target;
    if (!f.matches || !f.matches("form[data-write]")) return;
    e.preventDefault();
    var raw = f.querySelector("input").value.trim(), type = f.getAttribute("data-type"), v;
    if (type === "any" && (raw === "true" || raw === "false")) v = raw === "true";
    else { v = Number(raw); if (raw === "" || !isFinite(v)) { S.writes[f.getAttribute("data-write")] = { ok: false, text: "not a number", t: Date.now() }; renderData(S.model); return; } }
    sendWrite(f.getAttribute("data-write"), v);
  });
  $("data-filter").addEventListener("input", function () { if (S.model) renderData(S.model); });

  // ---- the clock ------------------------------------------------------------------------------------------------
  function tick() {
    var now = hubNow();
    document.querySelectorAll(".age").forEach(function (el) {
      el.textContent = el.getAttribute("data-pre") + dur(now - Number(el.getAttribute("data-at"))) + el.getAttribute("data-post");
    });
    document.querySelectorAll(".age-local").forEach(function (el) { el.textContent = dur(Date.now() - Number(el.getAttribute("data-t"))); });
    var late = S.model && modelLate();
    var mode = S.mode === "live" && late ? "late" : S.mode;
    var text = { connecting: "connecting", live: "live", polling: "polling every 2 s", lost: "no answer from the hub", late: "stream quiet" }[mode];
    if (replay() && (mode === "live" || mode === "polling")) { text = "replay, not live"; mode = "polling"; }
    $("conn").className = "conn " + ({ live: "ok", polling: "warn", lost: "bad", late: "bad", connecting: "idle" }[mode]);
    $("conn-text").textContent = text + (S.model ? ", model " + dur(modelAge()) + " old" : "");
    var b = $("stale-banner");
    if (late) {
      b.hidden = false;
      b.textContent = "No model from the hub for " + dur(modelAge()) + ": everything below is as of then, not now.";
    } else b.hidden = true;
  }

  // ---- replay (M16): a recording served through the same API, moved from here --------------------------------------
  function renderReplay(m) {
    var r = m.hub && m.hub.replay, bar = $("replay-bar");
    bar.hidden = !r;
    if (!r) return;
    var span = Math.max(1, r.to_at - r.from_at);
    $("replay-text").textContent = "Replay of " + r.file + ": " + clock(r.position_at) + " (" + dur(r.position_at - r.from_at) +
      " of " + dur(span) + ")" + (r.paused ? ", paused" : "");
    $("replay-play").textContent = r.paused ? "Play" : "Pause";
    if (document.activeElement !== $("replay-seek")) $("replay-seek").value = Math.round(1000 * (r.position_at - r.from_at) / span);
    if (document.activeElement !== $("replay-speed")) $("replay-speed").value = String(r.speed);
  }
  $("replay-play").addEventListener("click", function () { var r = replay(); if (r) post("api/replay", { paused: !r.paused }); });
  $("replay-speed").addEventListener("change", function () { post("api/replay", { speed: Number($("replay-speed").value) }); });
  $("replay-seek").addEventListener("change", function () {
    var r = replay(); if (!r) return;
    var at = r.from_at + Math.round((r.to_at - r.from_at) * Number($("replay-seek").value) / 1000);
    post("api/replay", { seek_at: at }).then(function () { location.reload(); });  // the events start again from there
  });

  // ---- the login (M16): only when the hub serves this page beyond its own PC --------------------------------------
  function showLogin(msg) {
    if (S.es) { S.es.close(); S.es = null; }
    stopPolling(); clearTimeout(S.retryTimer);
    $("main").hidden = true; $("login").hidden = false; $("who").hidden = true;
    $("login-msg").textContent = msg || "";
    $("conn").className = "conn idle"; $("conn-text").textContent = "log in to see the cell";
  }
  function post(url, body) {
    return fetch(url, { method: "POST", headers: { "Content-Type": "application/json", "X-Potluck-Request": "1" }, body: JSON.stringify(body) })
      .then(function (r) { return r.json().catch(function () { return {}; }).then(function (b) { b.status = r.status; return b; }); });
  }
  $("login-form").addEventListener("submit", function (e) {
    e.preventDefault();
    $("login-msg").textContent = "checking";
    post("api/login", { name: $("login-name").value, password: $("login-password").value }).then(function (b) {
      $("login-password").value = "";
      if (b.ok) location.reload(); else $("login-msg").textContent = b.result || "refused";
    }).catch(function () { $("login-msg").textContent = "no answer from the hub"; });
  });
  $("logout").addEventListener("click", function () { post("api/logout", {}).then(function () { location.reload(); }); });
  function start() {
    window.addEventListener("hashchange", function () { route(true); });
    if (!location.hash) history.replaceState(null, "", "#cell");
    route(false);
    startStream();
    setInterval(tick, 1000);
  }
  fetch("api/whoami", { cache: "no-store" }).then(function (r) { return r.ok ? r.json() : {}; }).catch(function () { return {}; })
    .then(function (w) {
      if (w.login) {
        if (!w.name) { showLogin(""); return; }
        S.who = w;
        $("who").hidden = false;
        $("who-text").textContent = w.name + " (" + (w.right === "write" ? "look and write" : "look only") + ")";
      } else if (w.api) {
        $("add-boards").hidden = false;  // M20: the setup wizard lives on the PC's own address only
      }
      start();  // a hub before M16 has no api/whoami: the page runs as before
    });
})();
