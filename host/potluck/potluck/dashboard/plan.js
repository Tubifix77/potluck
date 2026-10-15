/* The cell's floor plan (M20): rooms, the boards where the owner put them, and anything an app says it has placed
   -- drawn as the zone the app gives, never as a point. On the PC's own address it can be edited: make a plan, add
   rooms, drag boards onto it; every change goes to the cluster file through the hub's /api/setup/board, and a
   board's position reaches the board itself with the next `potluck apply`. Elsewhere it is a picture only. */
(function () {
  "use strict";

  var NS = "http://www.w3.org/2000/svg";
  var KIND = ["state", "x", "y", "a", "b", "theta", "conf"];  // DEVELOPING-APPS.md: the position kind
  var P = { dragging: null, placing: null, editRoom: null, msg: "", saving: false };

  function el(tag, attrs, text) {
    var e = document.createElement(tag);
    for (var k in attrs || {}) e.setAttribute(k, attrs[k]);
    if (text !== undefined) e.textContent = text;
    return e;
  }
  function sv(tag, attrs, text) {
    var e = document.createElementNS(NS, tag);
    for (var k in attrs || {}) e.setAttribute(k, attrs[k]);
    if (text !== undefined) e.textContent = text;
    return e;
  }
  function post(body) {
    P.saving = true;
    return fetch("api/setup/board", {
      method: "POST", headers: { "Content-Type": "application/json", "X-Potluck-Request": "1" },
      body: JSON.stringify(body)
    }).then(function (r) { return r.json().then(function (d) { d._status = r.status; return d; }); })
      .then(function (d) { P.saving = false; P.msg = d.ok ? "" : (d.result || "not saved"); return d; },
            function () { P.saving = false; P.msg = "the hub does not answer"; return { ok: false }; });
  }

  // Every actor with all seven position outputs, from the model's resources.
  function placed(m) {
    var by = {};
    (m.resources || []).forEach(function (r) {
      var k = /\/act\/([^/]+)\/([a-z]+)$/.exec(r.path || "");
      if (!k || KIND.indexOf(k[2]) < 0) return;
      (by[k[1]] = by[k[1]] || {})[k[2]] = r;
    });
    var out = [];
    Object.keys(by).sort().forEach(function (name) {
      var g = by[name];
      if (KIND.every(function (f) { return g[f]; })) out.push({ name: name, g: g });
    });
    return out;
  }
  function num(r) { return r && typeof r.value === "number" && isFinite(r.value) ? r.value : null; }

  function stateColour(s) { return { alive: "var(--ok)", dead: "var(--bad)", asleep: "var(--sleep)" }[s] || "var(--idle)"; }

  function toPlan(svg, evt) {
    var pt = svg.createSVGPoint();
    pt.x = evt.clientX; pt.y = evt.clientY;
    var p = pt.matrixTransform(svg.getScreenCTM().inverse());
    return { x: p.x, y: p.y };
  }
  function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }
  function r1(v) { return Math.round(v * 10) / 10; }

  function render(host, m, editable, refresh) {
    if (P.dragging || P.saving) return;  // the model arrives every few seconds: never redraw under the owner's hand
    host.innerHTML = "";
    var plan = (m.cell || {}).plan;
    if (!plan) {
      host.appendChild(el("p", { "class": "muted" }, "No floor plan yet." +
        (editable ? " Give its size in metres; rooms and boards come next." : " It is made on the PC the hub runs on.")));
      if (editable) host.appendChild(newPlanForm(refresh));
      return;
    }
    var W = plan.width, H = plan.height;
    var svg = sv("svg", { viewBox: "0 0 " + W + " " + H, "class": "plan", role: "img",
      "aria-label": "Floor plan, " + W + " by " + H + " m" });
    for (var gx = 1; gx < W; gx++) svg.appendChild(sv("line", { x1: gx, y1: 0, x2: gx, y2: H, "class": "grid" }));
    for (var gy = 1; gy < H; gy++) svg.appendChild(sv("line", { x1: 0, y1: gy, x2: W, y2: gy, "class": "grid" }));
    svg.appendChild(sv("rect", { x: 0, y: 0, width: W, height: H, "class": "outline" }));
    plan.rooms.forEach(function (r, i) {
      var g = sv("g", { "class": "room" + (P.editRoom === i ? " sel" : "") });
      g.appendChild(sv("rect", { x: r.x, y: r.y, width: r.w, height: r.h }));
      g.appendChild(sv("text", { x: r.x + 0.2, y: r.y + 0.55 }, r.name));
      if (editable) g.addEventListener("click", function () {
        if (P.placing) return;  // that click is a board's new place, not a choice of room
        P.editRoom = i;
        render(host, m, editable, refresh);
      });
      svg.appendChild(g);
    });
    var unplaced = [];
    var msgs = [];
    placed(m).forEach(function (p) {
      var st = num(p.g.state);
      if (st === 1) msgs.push(p.name + ": someone is here, place unknown");
      if (st !== 2) return;
      var x = num(p.g.x), y = num(p.g.y), a = num(p.g.a), b = num(p.g.b), t = num(p.g.theta) || 0;
      if (x === null || y === null || a === null || b === null) return;
      var e = sv("ellipse", { cx: x, cy: y, rx: Math.max(a, 0.1), ry: Math.max(b, 0.1), "class": "zone",
        transform: "rotate(" + (t * 180 / Math.PI) + " " + x + " " + y + ")" });
      e.appendChild(sv("title", {}, p.name + ": estimated zone, " + (2 * a).toFixed(1) + " x " + (2 * b).toFixed(1) +
        " m" + (num(p.g.conf) !== null ? ", confidence " + Math.round(num(p.g.conf) * 100) + " %" : "")));
      svg.appendChild(e);
      msgs.push(p.name + ": estimated zone shown");
    });
    (m.boards || []).forEach(function (bd) {
      if (!bd.position) { unplaced.push(bd); return; }
      var g = sv("g", { "class": "board" + (editable ? " movable" : ""), "data-node": bd.node });
      var c = sv("circle", { cx: bd.position[0], cy: bd.position[1], r: 0.28, fill: stateColour(bd.state) });
      g.appendChild(c);
      var t = sv("text", { x: bd.position[0] + 0.38, y: bd.position[1] + 0.15 }, bd.label || bd.node);
      g.appendChild(t);
      g.appendChild(sv("title", {}, (bd.label || bd.node) + " (" + bd.node + "), " + bd.state + ", at " +
        bd.position[0] + ", " + bd.position[1] + " m"));
      if (editable) {
        g.addEventListener("pointerdown", function (evt) {
          evt.preventDefault();
          g.setPointerCapture(evt.pointerId);
          P.dragging = { node: bd.node, g: g, c: c, t: t, x: bd.position[0], y: bd.position[1] };
        });
        g.addEventListener("pointermove", function (evt) {
          var d = P.dragging;
          if (!d || d.node !== bd.node) return;
          var q = toPlan(svg, evt);
          d.x = r1(clamp(q.x, 0, W)); d.y = r1(clamp(q.y, 0, H));
          c.setAttribute("cx", d.x); c.setAttribute("cy", d.y);
          t.setAttribute("x", d.x + 0.38); t.setAttribute("y", d.y + 0.15);
        });
        g.addEventListener("pointerup", function () {
          var d = P.dragging;
          if (!d) return;
          P.dragging = null;
          bd.position = [d.x, d.y];
          post({ node: bd.node, position: [d.x, d.y] }).then(refresh);
        });
      }
      svg.appendChild(g);
    });
    if (editable && P.placing) {
      svg.classList.add("placing");
      svg.addEventListener("click", function (evt) {
        if (!P.placing) return;
        var q = toPlan(svg, evt), node = P.placing;
        P.placing = null;
        post({ node: node, position: [r1(clamp(q.x, 0, W)), r1(clamp(q.y, 0, H))] }).then(refresh);
      });
    }
    host.appendChild(svg);
    var info = el("div", { "class": "plan-info small" });
    msgs.forEach(function (t) { info.appendChild(el("p", { "class": "muted" }, t)); });
    if (unplaced.length) {
      var p = el("p", {}, "Not on the plan: ");
      unplaced.forEach(function (bd) {
        if (editable) {
          var b = el("button", { type: "button", "class": "small" + (P.placing === bd.node ? " on" : "") }, bd.label || bd.node);
          b.addEventListener("click", function () {
            P.placing = P.placing === bd.node ? null : bd.node;
            render(host, m, editable, refresh);
          });
          p.appendChild(b);
        } else p.appendChild(document.createTextNode((bd.label || bd.node) + " "));
      });
      if (editable) p.appendChild(el("span", { "class": "muted" }, P.placing ? " -- now click where it is on the plan" :
        " -- pick one, then click where it is"));
      info.appendChild(p);
    }
    if (editable) {
      info.appendChild(el("p", { "class": "muted" }, "Drag a board to move it. A board learns its place with the next " +
        "\"potluck apply\"; the plan itself is only for you. A rough plan is enough."));
      info.appendChild(roomForm(plan, refresh, host, m, editable));
    }
    if (P.msg) info.appendChild(el("p", { "class": "note warn" }, P.msg));
    host.appendChild(info);
  }

  function newPlanForm(refresh) {
    var f = el("form", { "class": "plan-form" });
    var w = el("input", { type: "number", min: "1", max: "1000", step: "0.5", value: "12", "aria-label": "Width in metres" });
    var h = el("input", { type: "number", min: "1", max: "1000", step: "0.5", value: "8", "aria-label": "Depth in metres" });
    f.appendChild(el("label", {}, "Width (m) ")).appendChild(w);
    f.appendChild(el("label", {}, "Depth (m) ")).appendChild(h);
    f.appendChild(el("button", { type: "submit" }, "Make the plan"));
    f.addEventListener("submit", function (evt) {
      evt.preventDefault();
      post({ plan: { width: Number(w.value), height: Number(h.value), rooms: [] } }).then(refresh);
    });
    return f;
  }

  function roomForm(plan, refresh, host, m, editable) {
    var i = P.editRoom, r = i !== null && plan.rooms[i] ? plan.rooms[i] : null;
    var f = el("form", { "class": "plan-form" });
    f.appendChild(el("b", {}, r ? "Room \"" + r.name + "\"" : "Add a room"));
    var fields = {};
    [["name", "Name", "text"], ["x", "Left (m)", "number"], ["y", "Top (m)", "number"], ["w", "Width (m)", "number"],
     ["h", "Depth (m)", "number"]].forEach(function (d) {
      var inp = el("input", { type: d[2], "aria-label": d[1] });
      if (d[2] === "number") { inp.step = "0.1"; inp.min = "0"; }
      else inp.maxLength = 40;
      inp.value = r ? r[d[0]] : (d[0] === "name" ? "" : d[0] === "w" || d[0] === "h" ? "3" : "0");
      fields[d[0]] = inp;
      f.appendChild(el("label", {}, d[1] + " ")).appendChild(inp);
    });
    f.appendChild(el("button", { type: "submit" }, r ? "Save" : "Add"));
    var rooms = plan.rooms.slice();
    if (r) {
      var del = el("button", { type: "button" }, "Remove");
      del.addEventListener("click", function () {
        rooms.splice(i, 1);
        P.editRoom = null;
        post({ plan: { width: plan.width, height: plan.height, rooms: rooms } }).then(refresh);
      });
      f.appendChild(del);
      var cancel = el("button", { type: "button" }, "Done");
      cancel.addEventListener("click", function () { P.editRoom = null; render(host, m, editable, refresh); });
      f.appendChild(cancel);
    }
    f.addEventListener("submit", function (evt) {
      evt.preventDefault();
      var nr = { name: fields.name.value.trim(), x: Number(fields.x.value), y: Number(fields.y.value),
                 w: Number(fields.w.value), h: Number(fields.h.value) };
      if (!nr.name) { P.msg = "a room needs a name"; render(host, m, editable, refresh); return; }
      if (r) rooms[i] = nr; else rooms.push(nr);
      P.editRoom = null;
      post({ plan: { width: plan.width, height: plan.height, rooms: rooms } }).then(refresh);
    });
    return f;
  }

  window.PotluckPlan = { render: render, placed: placed };
})();
