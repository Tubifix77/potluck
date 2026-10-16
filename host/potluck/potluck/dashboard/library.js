/* The library (M20): the apps the cell runs, switched on and off, and Home Assistant as an entry beside them -- its
   broker tried from this PC, and per resource what it may see or control. Every change goes into the cluster file
   through the hub's /api/setup/library (the PC's own address only) and reaches the boards with "Apply to the cell",
   which runs `potluck apply` as a job. Elsewhere the page shows the same as a picture. */
(function () {
  "use strict";

  var L = { lib: null, busy: false, msg: "", broker: null, applying: null, lines: [], changed: false };

  function el(tag, attrs, text) {
    var e = document.createElement(tag);
    for (var k in attrs || {}) e.setAttribute(k, attrs[k]);
    if (text !== undefined) e.textContent = text;
    return e;
  }
  function call(method, path, body) {
    var opt = { method: method, cache: "no-store", headers: {} };
    if (method === "POST") {
      opt.headers["Content-Type"] = "application/json";
      opt.headers["X-Potluck-Request"] = "1";
      opt.body = JSON.stringify(body || {});
    }
    return fetch(path, opt).then(function (r) {
      return r.json().catch(function () { return {}; }).then(function (d) { d._status = r.status; return d; });
    });
  }

  var MODES = [["off", "Hidden"], ["read", "Visible"], ["read_write", "Controllable"]];

  function render(host, editable) {
    if (!editable) {
      host.innerHTML = "";
      host.appendChild(el("p", { "class": "muted" }, "The library -- which apps the cell runs and what Home Assistant " +
        "may see -- is changed on the PC the hub runs on."));
      return;
    }
    if (!L.lib && !L.busy) {
      L.busy = true;
      call("GET", "api/setup/library").then(function (d) {
        L.busy = false;
        if (d._status === 200) L.lib = d; else L.msg = d.result || d.error || "the library did not load";
        render(host, editable);
      });
    }
    if (L.applying && L.applying.state === "running") return draw(host, true);  // keep the job's lines moving
    draw(host, false);
  }

  // M20's store: apps from GitHub, run in the boards' sandbox. Off until the owner turns it on; every install is the
  // owner's, here. Looking at an app is the one request the hub makes outside; installing certifies its author for this
  // cell and writes the cluster file -- "Apply to the cell" sends it.
  function drawStore(host, running, redraw) {
    host.appendChild(el("h3", {}, "Apps from GitHub"));
    if (!L.store) {
      if (!L.storeAsked) {
        L.storeAsked = true;
        call("GET", "api/setup/store").then(function (d) { if (d._status === 200) { L.store = d; redraw(); } });
      }
      host.appendChild(el("p", { "class": "muted small" }, "Loading..."));
      return;
    }
    var st = L.store;
    var onRow = el("label", { "class": "lib-row" });
    var on = el("input", { type: "checkbox" });
    on.checked = !!st.on;
    on.disabled = running;
    on.addEventListener("change", function () {
      call("POST", "api/setup/store", { on: on.checked }).then(function (d) {
        if (d.ok) { L.store = d; L.look = null; } else L.msg = d.result || "not saved";
        redraw();
      });
    });
    onRow.appendChild(on);
    onRow.appendChild(el("span", {}, " Allow apps from GitHub"));
    host.appendChild(onRow);
    host.appendChild(el("p", { "class": "muted small" }, "An app from GitHub runs in the boards' sandbox: it reads only " +
      "the inputs you give it and writes only its own outputs. Installing one trusts its author's key for this cell; " +
      "if that key ever changes, Potluck refuses the update."));
    (st.apps || []).forEach(function (a) {
      var row = el("div", { "class": "lib-rrow" });
      row.appendChild(el("code", {}, a.name));
      row.appendChild(el("span", { "class": "muted small" }, " from " + a.repo + " (" + a.ref + ")"));
      var rm = el("button", { type: "button", "class": "small" }, "Remove");
      rm.disabled = running;
      rm.addEventListener("click", function () {
        call("POST", "api/setup/store/remove", { name: a.name }).then(function (d) {
          if (d.ok) { L.store = d; L.changed = true; L.msg = ""; } else L.msg = d.result || "not removed";
          redraw();
        });
      });
      row.appendChild(rm);
      host.appendChild(row);
    });
    if (!st.on) return;
    // the curated list: fetched on request, shown only when the store key's signature over it checks
    var fl = el("div", { "class": "lib-store" });
    var getFeed = el("button", { type: "button", "class": "small" }, L.feed ? "Refresh the list" : "Show the store's list");
    getFeed.disabled = running;
    getFeed.addEventListener("click", function () {
      getFeed.disabled = true;
      call("POST", "api/setup/store/feed", {}).then(function (d) {
        if (d.ok) { L.feed = d; L.msg = ""; } else { L.feed = null; L.msg = d.result || "the list did not load"; }
        redraw();
      });
    });
    fl.appendChild(getFeed);
    host.appendChild(fl);
    if (L.feed) {
      (L.feed.apps || []).forEach(function (a) {
        var row = el("div", { "class": "lib-rrow" });
        row.appendChild(el("code", {}, a.name));
        row.appendChild(el("span", { "class": "muted small" }, a.description || (a.repo + (a.dir ? "/" + a.dir : ""))));
        var b = el("button", { type: "button", "class": "small" }, a.installed ? "Installed -- look again" : "Look");
        b.disabled = running;
        b.addEventListener("click", function () {
          call("POST", "api/setup/store/look", { repo: a.repo, dir: a.dir, ref: a.ref }).then(function (d) {
            if (d.ok) { d.repo = a.repo; d.dir = a.dir; d.ref = a.ref; L.look = d; L.msg = ""; }
            else { L.look = null; L.msg = d.result || "no app there"; }
            redraw();
          });
        });
        row.appendChild(b);
        host.appendChild(row);
      });
      if (!(L.feed.apps || []).length) host.appendChild(el("p", { "class": "muted small" }, "The list is empty."));
    }
    host.appendChild(el("p", { "class": "muted small" }, "Or any repository:"));
    var form = el("div", { "class": "lib-store" });
    var repo = el("input", { type: "text", placeholder: "owner/repository", "aria-label": "GitHub repository" });
    var ref = el("input", { type: "text", placeholder: "release tag (optional)", "aria-label": "Release tag" });
    if (L.look) { repo.value = L.look.repo; ref.value = L.look.ref || ""; }
    var look = el("button", { type: "button", "class": "small" }, "Look");
    look.disabled = running;
    look.addEventListener("click", function () {
      look.disabled = true;
      call("POST", "api/setup/store/look", { repo: repo.value.trim(), ref: ref.value.trim() }).then(function (d) {
        if (d.ok) { d.repo = repo.value.trim(); d.ref = ref.value.trim(); L.look = d; L.msg = ""; }
        else { L.look = null; L.msg = d.result || "no app there"; }
        redraw();
      });
    });
    form.appendChild(repo);
    form.appendChild(ref);
    form.appendChild(look);
    host.appendChild(form);
    var lk = L.look;
    if (!lk) return;
    var card = el("div", { "class": "lib-list lib-card" });
    card.appendChild(el("b", {}, lk.name));
    if (lk.description) card.appendChild(el("p", {}, lk.description));
    var who = { "new": "A new author for this cell: installing trusts this key.", "pinned": "The author you trusted before.",
      "changed": "This repository's key CHANGED since you installed from it: refused." }[lk.author];
    card.appendChild(el("p", { "class": "note " + (lk.author === "changed" ? "warn" : "") },
      who + " Key " + lk.author_key.slice(0, 16) + "..."));
    var picks = [];
    (lk.inputs || []).forEach(function (inp, i) {
      var lab = el("label", {}, "Input " + inp.name + " (" + inp.type + (inp.unit ? ", " + inp.unit : "") + ") ");
      var sel = el("select", { "aria-label": "Input " + inp.name });
      sel.appendChild(el("option", { value: "" }, "-- choose --"));
      (lk.candidates[i] || []).forEach(function (p) { sel.appendChild(el("option", { value: p }, p.split("/").slice(3).join("/"))); });
      picks.push(sel);
      lab.appendChild(sel);
      card.appendChild(lab);
    });
    if (!(lk.inputs || []).length) card.appendChild(el("p", { "class": "muted small" }, "It reads nothing from the cell."));
    var pp = el("input", { type: "password", autocomplete: "current-password", "aria-label": "Your passphrase" });
    var ppl = el("label", {}, "Your passphrase (if the cell's keys are sealed) ");
    ppl.appendChild(pp);
    card.appendChild(ppl);
    var inst = el("button", { type: "button" }, "Install");
    inst.disabled = running || lk.author === "changed";
    inst.addEventListener("click", function () {
      var binds = picks.map(function (s) { return s.value; });
      if (binds.some(function (b) { return !b; })) { L.msg = "Choose a resource for every input."; redraw(); return; }
      inst.disabled = true;
      var body = { repo: lk.repo, dir: lk.dir || "", ref: lk.ref, bindings: binds };
      if (pp.value) body.passphrase = pp.value;
      pp.value = "";
      call("POST", "api/setup/store/install", body).then(function (d) {
        if (d.ok) { L.store = d; L.look = null; L.changed = true; L.msg = "Installed " + d.installed.name +
          ": apply to send it to the boards."; }
        else L.msg = d.result || "not installed";
        redraw();
      });
    });
    card.appendChild(inst);
    host.appendChild(card);
  }

  function draw(host, running) {
    host.innerHTML = "";
    var lib = L.lib;
    if (!lib) {
      host.appendChild(el("p", { "class": "muted" }, L.msg || "Loading the library..."));
      return;
    }
    var redraw = function () { draw(host, false); };
    var save = function (body) {
      call("POST", "api/setup/library", body).then(function (d) {
        if (d.ok) { L.lib = d; L.changed = true; L.msg = ""; } else L.msg = d.result || "not saved";
        redraw();
      });
    };

    // apps
    host.appendChild(el("h3", {}, "Apps on your boards"));
    var apps = el("div", { "class": "lib-list" });
    if (!lib.apps.length) apps.appendChild(el("p", { "class": "muted" }, "No apps yet."));
    lib.apps.forEach(function (a) {
      var row = el("label", { "class": "lib-row" });
      var box = el("input", { type: "checkbox" });
      box.checked = a.enabled;
      box.disabled = running;
      box.addEventListener("change", function () { save({ app: a.name, enabled: box.checked }); });
      row.appendChild(box);
      row.appendChild(el("b", {}, a.name));
      row.appendChild(el("span", { "class": "muted small" }, a.enabled ? "on" : "off -- still in the firmware, " +
        "so switching it back on needs no new firmware"));
      apps.appendChild(row);
    });
    host.appendChild(apps);

    // Home Assistant
    host.appendChild(el("h3", {}, "Home Assistant"));
    var ha = lib.home_assistant || {};
    var box = el("div", { "class": "lib-list" });
    if (!ha.on) {
      box.appendChild(el("p", {}, "Not connected: the cell presents to no MQTT broker. Home Assistant reaches the boards " +
        "through one -- its Mosquitto add-on is the usual choice. Say which broker in the cluster file's \"present\" " +
        "(the wizard will ask for it)."));
    } else {
      var p = el("p", {}, "The boards present to " + ha.broker + (ha.tls ? " over TLS" : "") + ". ");
      var test = el("button", { type: "button", "class": "small" }, "Test the broker");
      test.disabled = running;
      test.addEventListener("click", function () {
        test.disabled = true;
        L.broker = { result: "trying..." };
        redraw();
        call("POST", "api/setup/broker", {}).then(function (d) { L.broker = d; redraw(); });
      });
      p.appendChild(test);
      box.appendChild(p);
      if (L.broker) box.appendChild(el("p", { "class": "note " + (L.broker.ok ? "ok" : "warn") },
        "Broker: " + L.broker.result));
    }
    host.appendChild(box);

    // what it may see and control
    host.appendChild(el("h3", {}, "What Home Assistant may see and control"));
    host.appendChild(el("p", { "class": "muted small" }, "Only the owner opens the cell: anything controllable from " +
      "outside stays off until you turn it on, and a safety-relevant resource is never controllable."));
    var table = el("div", { "class": "lib-res" });
    lib.resources.forEach(function (r) {
      var row = el("div", { "class": "lib-rrow" + (r.missing ? " missing" : "") });
      row.appendChild(el("code", {}, r.name));
      var seg = el("span", { "class": "seg", role: "radiogroup", "aria-label": r.name });
      MODES.forEach(function (m) {
        if (m[0] === "read_write" && !r.controllable && r.mode !== "read_write") return;
        var b = el("button", { type: "button", "class": "small" + (r.mode === m[0] ? " on" : ""), role: "radio",
          "aria-checked": r.mode === m[0] ? "true" : "false" }, m[1]);
        b.disabled = running;
        b.addEventListener("click", function () {
          if (r.mode === m[0]) return;
          var o = {};
          o[r.path] = m[0];
          save({ outside: o });
        });
        seg.appendChild(b);
      });
      row.appendChild(seg);
      if (r.missing) row.appendChild(el("span", { "class": "muted small" }, "no app publishes it now"));
      table.appendChild(row);
    });
    host.appendChild(table);

    drawStore(host, running, redraw);

    // apply
    var bar = el("div", { "class": "lib-apply" });
    var go = el("button", { type: "button" }, running ? "Applying..." : "Apply to the cell");
    go.disabled = running;
    go.addEventListener("click", function () {
      L.lines = [];
      L.applying = { state: "running" };
      draw(host, true);
      call("POST", "api/setup/apply", {}).then(function (r) {
        if (r._status !== 202) { L.applying = { state: "failed", error: r.result || "not started" }; redraw(); return; }
        (function poll() {
          call("GET", "api/setup/jobs/" + r.job).then(function (j) {
            L.lines = j.lines || [];
            L.applying = j;
            if (j.state === "running") { draw(host, true); setTimeout(poll, 1000); }
            else { if (j.state === "done") L.changed = false; redraw(); }
          });
        })();
      });
    });
    bar.appendChild(go);
    bar.appendChild(el("span", { "class": "muted small" }, L.changed ? "Changes are saved in the cluster file; the boards " +
      "get them when you apply." : "The boards run what this page shows once applied."));
    host.appendChild(bar);
    if (L.applying && L.applying.state !== "running") {
      host.appendChild(el("p", { "class": "note " + (L.applying.state === "done" ? "ok" : "warn") },
        L.applying.state === "done" ? (L.applying.result || {}).verdict || "done" : "Not applied: " + L.applying.error));
    }
    if (L.lines.length) {
      var pre = el("pre", { "class": "out" }, L.lines.slice(-30).join("\n"));
      host.appendChild(pre);
      pre.scrollTop = pre.scrollHeight;
    }
    if (L.msg) host.appendChild(el("p", { "class": "note warn" }, L.msg));
  }

  window.PotluckLibrary = { render: render };
})();
