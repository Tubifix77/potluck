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
