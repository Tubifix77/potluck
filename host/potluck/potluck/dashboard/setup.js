/* Potluck's setup wizard, "Add boards" (M20). Talks to the hub's /api/setup/*, which exists only on the PC's own
   address. Every board action is a job the page follows; nothing here keeps a secret: the passphrase and the Wi-Fi
   password go to the hub once, in the request that needs them, and are never stored by the page. */
(function () {
  "use strict";

  var cell = null;          // GET api/setup/cell
  var baseline = {};        // ports present when the page opened: never probed unless the owner says a board is new
  var boards = {};          // port -> {port, chip, el, probe, state, node, version}
  var order = [];           // ports in the order they appeared
  var build = null;         // GET api/setup/build
  var wanted = 1;
  var firstRun = false;     // a PC with no cell yet: the wizard makes one, and its first board becomes the hub's link

  function $(id) { return document.getElementById(id); }
  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    return e;
  }
  function say(text, kind) {
    var m = $("w-msg");
    m.textContent = text || "";
    m.className = "banner " + (kind || "bad");
    m.hidden = !text;
  }

  function api(method, path, body) {
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

  // A job: start it, then follow it until it is done or failed. Resolves with the job; rejects with its error.
  function job(path, body, onUpdate) {
    return api("POST", path, body).then(function (r) {
      if (r._status !== 202) throw new Error(r.result || r.error || ("HTTP " + r._status));
      return new Promise(function (resolve, reject) {
        (function poll() {
          api("GET", "api/setup/jobs/" + r.job).then(function (j) {
            if (onUpdate) onUpdate(j);
            if (j.state === "done") resolve(j);
            else if (j.state === "failed") reject(new Error(j.error || "failed"));
            else setTimeout(poll, 500);
          }, function () { setTimeout(poll, 1000); });
        })();
      });
    });
  }

  function show(step) {
    var secs = document.querySelectorAll(".step");
    for (var i = 0; i < secs.length; i++) secs[i].hidden = secs[i].id !== step;
    var lis = $("steps").children, past = true;
    for (var k = 0; k < lis.length; k++) {
      var f = lis[k].getAttribute("data-for");
      if (f === step) { lis[k].className = "now"; past = false; }
      else lis[k].className = past ? "past" : "";
    }
    say("");
  }

  // ---- step 1: how many ---------------------------------------------------------------------------------------
  // One cell holds max_boards; past that the boards become cells of cells (M19), the wizard's "Cells" step.
  // MAX_TREE is what M19 was shown with: ten cells of ten (pot_sim's 24-hour run, M0-LOG session 46).
  var MAX_TREE = 100;
  function remaining() { return MAX_TREE - cell.boards.length; }

  function checkCount() {
    var n = parseInt($("n-add").value, 10) || 0, left = remaining(), over = $("n-over"), total = cell.boards.length + n;
    over.hidden = total <= cell.max_boards && n <= left;
    over.className = n > left ? "note warn" : "note";
    over.textContent = n > left
      ? "Potluck has been shown with up to " + MAX_TREE + " boards; " + Math.max(0, left) + " more fit."
      : "With " + total + " boards the cell becomes several cells of up to ten, each with a foreman. You will choose " +
        "which boards are always powered; the rest is worked out for you.";
    $("go-plug").disabled = left <= 0 || n < 1 || n > left;
  }

  // ---- step 2: plug in ----------------------------------------------------------------------------------------
  function card(port, chip) {
    var c = el("div", "nb");
    c.setAttribute("data-port", port);
    var head = el("div", "nb-head");
    head.appendChild(el("b", "", port));
    head.appendChild(el("span", "muted small", chip || "USB serial"));
    c.appendChild(head);
    c.appendChild(el("div", "nb-what small", "Checking what it is (this restarts it)..."));
    var bar = el("div", "bar");
    bar.appendChild(el("span"));
    bar.hidden = true;
    c.appendChild(bar);
    return c;
  }
  function setWhat(b, text, kind) {
    var w = b.el.querySelector(".nb-what");
    w.textContent = text;
    w.className = "nb-what small " + (kind || "");
  }
  function setPct(b, pct) {
    var bar = b.el.querySelector(".bar");
    bar.hidden = pct === null;
    if (pct !== null) bar.firstChild.style.width = Math.max(2, pct) + "%";
  }

  function addBoard(p) {
    if (boards[p.port]) return;
    var b = { port: p.port, chip: p.chip, state: "probing" };
    b.el = card(p.port, p.chip);
    boards[p.port] = b;
    order.push(p.port);
    $("boards").appendChild(b.el);
    job("api/setup/probe", { port: p.port }).then(function (j) {
      b.probe = j.result;
      if (j.result.ok && j.result.known) {
        askKnown(b, j.result.known);
      } else if (j.result.ok) {
        b.state = "ready";
        setWhat(b, j.result.chip + ", " + j.result.flash_mb + " MB flash, " + j.result.psram_mb + " MB PSRAM: ready", "ok");
      } else {
        b.state = "unfit";
        setWhat(b, "Cannot run Potluck: " + j.result.reason, "warn");
      }
      plugReady();
    }, function (e) {
      b.state = "unfit";
      setWhat(b, "No board answered: " + e.message + ". Check the cable (some carry power only) and plug it in again.", "warn");
      plugReady();
    });
  }

  // A wiped board the cell had: its MAC gives the id of one of the cell's boards. Only the owner can say it is that
  // board (two boards' MAC tails can collide), so it waits for an answer (2026-10-23: a wiped board came back as a
  // stranger, node-0100, beside its own dead entry).
  function askKnown(b, known) {
    b.state = "asking";
    setWhat(b, "This looks like " + known.label + " (node " + known.node + "), a board of this cell that was wiped. Is it?", "");
    var row = el("div", "room-row");
    var yes = el("button", "", "Yes: put it back as " + known.label);
    var no = el("button", "", "No: it is a new board");
    yes.type = no.type = "button";
    yes.onclick = function () {
      b.replace = known; b.state = "ready"; row.remove();
      setWhat(b, "Goes back as " + known.label + (known.room ? ", in " + known.room : "") + ": ready", "ok");
      plugReady();
    };
    no.onclick = function () {
      b.replace = null; b.state = "ready"; row.remove();
      setWhat(b, "A new board (it gets an id of its own): ready", "ok");
      plugReady();
    };
    row.appendChild(yes);
    row.appendChild(no);
    b.el.appendChild(row);
  }

  function plugReady() {
    var ready = order.filter(function (p) { return boards[p].state === "ready"; }).length;
    $("plug-count").textContent = ready + " of " + wanted + " ready" + (ready > wanted ? " (more than you said: fine)" : "");
    var btn = $("go-install");
    btn.disabled = ready < 1;
    btn.textContent = ready ? "Next: install on " + ready + " board" + (ready === 1 ? "" : "s") : "Next";
  }

  function scanPorts() {
    if ($("s-plug").hidden) return;
    api("GET", "api/setup/ports").then(function (r) {
      var here = {};
      (r.ports || []).forEach(function (p) {
        here[p.port] = true;
        if (!baseline[p.port]) addBoard(p);
      });
      order.forEach(function (port) {
        var b = boards[port];
        if (!here[port] && b.state !== "gone") { b.prev = b.state; b.state = "gone"; setWhat(b, "Unplugged", "muted"); plugReady(); }
        else if (here[port] && b.state === "gone") {
          // Back after one read without it: the same card, never a new probe -- a probe restarts the board (found by the
          // Potluck Desk 2026-10-19: the card used to be removed and added again, which probed it again).
          b.state = b.prev || "plugged";
          setWhat(b, "Plugged in again (not checked again: a check restarts the board)", "muted");
          plugReady();
        }
      });
    }).finally(function () { setTimeout(scanPorts, 1500); });
  }

  function listOld(ports) {
    var ul = $("old-list");
    ports.forEach(function (p) {
      var li = el("li");
      li.appendChild(el("span", "", p.port + " (" + (p.chip || p.description || "USB serial") + ") "));
      var btn = el("button", "small", "This one is new or wiped: check it");
      btn.type = "button";
      btn.onclick = function () {
        delete baseline[p.port]; li.remove(); addBoard(p);
        $("n-old").textContent = String($("old-list").children.length);
      };
      li.appendChild(btn);
      ul.appendChild(li);
    });
    $("n-old").textContent = String(ports.length);
    $("old-ports").hidden = ports.length === 0;
  }

  // ---- step 3: install ----------------------------------------------------------------------------------------
  function readyBoards() { return order.map(function (p) { return boards[p]; }).filter(function (b) { return b.state === "ready"; }); }

  function findBuild() {
    api("GET", "api/setup/build").then(function (r) {
      if (r._status !== 200) { $("fw-what").textContent = r.result || "The cell's firmware was not found."; return; }
      build = r;
      $("fw-what").textContent = r.version ? "Potluck " + r.version + ", what the cell's boards run now."
        : "Potluck, " + (r.note || "the firmware on this PC") + ".";
      var btn = $("do-install");
      btn.disabled = false;
      btn.textContent = "Install on " + readyBoards().length + " board" + (readyBoards().length === 1 ? "" : "s");
    });
  }

  function install() {
    var list = readyBoards();
    $("do-install").disabled = true;
    var host = $("s-install");
    list.forEach(function (b) { host.appendChild(b.el); setWhat(b, "Installing..."); setPct(b, 0); });
    Promise.all(list.map(function (b) {
      return job("api/setup/flash", { port: b.port, build: build.build }, function (j) { setPct(b, j.pct); }).then(function () {
        b.state = "flashed"; setPct(b, null); setWhat(b, "Installed", "ok");
      }, function (e) {
        b.state = "unfit"; setPct(b, null); setWhat(b, "Install failed: " + e.message, "warn");
      });
    })).then(function () {
      if (list.some(function (b) { return b.state === "flashed"; })) { joinStep(); }
      else { say("No board took the firmware. Unplug them, plug them in again and start over."); }
    });
  }

  // ---- step 4: join the cell ----------------------------------------------------------------------------------
  function joinStep() {
    show("s-join");
    $("pp-row").hidden = !cell.vault;
    order.forEach(function (p) { if (boards[p].state === "flashed") $("s-join").appendChild(boards[p].el); });
  }

  function joinAll() {
    var list = order.map(function (p) { return boards[p]; }).filter(function (b) { return b.state === "flashed"; });
    var pp = $("pp").value, ssid = $("wifi-ssid").value, pw = $("wifi-pw").value;
    if (ssid && (pw.length < 8 || pw.length > 64)) { say("A Wi-Fi password is 8 to 64 characters."); return; }
    $("do-join").disabled = true;
    // one at a time: each enrolment writes the cell's node registry
    var chain = Promise.resolve();
    list.forEach(function (b) {
      chain = chain.then(function () {
        setWhat(b, "Joining...");
        var body = { port: b.port };
        if (pp) body.passphrase = pp;
        if (b.replace) { body.node_id = b.replace.node; body.replace = true; }  // the cell's board, reinstalled
        return job("api/setup/enrol", body).then(function (j) {
          b.node = j.result.node;
          return api("POST", "api/setup/board", { node: b.node });
        }).then(function (r) {
          if (!r.ok) throw new Error(r.result || "the cell file refused it");
          b.label = r.board.label;
          b.room = r.board.room || "";  // a reinstalled board keeps its room
          if (ssid) return job("api/setup/wifi", { port: b.port, ssid: ssid, password: pw });
        }).then(function () {
          return job("api/setup/reset", { port: b.port });
        }).then(function () {
          b.state = "joined";
          setWhat(b, "In the cell as " + b.label + " (node " + b.node + ")", "ok");
        }, function (e) {
          b.state = "unfit";
          setWhat(b, "Could not join: " + e.message, "warn");
        });
      });
    });
    if (firstRun) {
      chain = chain.then(function () {
        var first = list.filter(function (b) { return b.state === "joined"; })[0];
        if (!first) return;
        setWhat(first, "In the cell as " + first.label + "; connecting this PC to the cell through it...");
        return api("POST", "api/setup/attach", { port: first.port }).then(function (r) {
          if (!r.ok) throw new Error(r.result || "could not connect");
          firstRun = false;
          setWhat(first, "In the cell as " + first.label + "; this PC talks to the cell through it", "ok");
        }).catch(function (e) { say("This PC could not connect to the cell through " + first.port + ": " + e.message); });
      });
    }
    chain.then(function () {
      $("pp").value = ""; $("wifi-pw").value = "";
      if (list.some(function (b) { return b.state === "joined"; })) roomsStep();
      else { $("do-join").disabled = false; say("No board joined. If your passphrase was wrong, try again."); }
    });
  }

  // ---- step 5: rooms ------------------------------------------------------------------------------------------
  function roomsStep() {
    show("s-rooms");
    var host = $("rooms");
    // the rooms the cell already has, offered as you type: a room is where a board sits, not its name (2026-10-23)
    var dl = $("room-names") || document.body.appendChild(el("datalist"));
    dl.id = "room-names";
    dl.textContent = "";
    var seen = {};
    (cell.boards || []).forEach(function (x) {
      if (x.room && !seen[x.room]) { seen[x.room] = true; var o = el("option"); o.value = x.room; dl.appendChild(o); }
    });
    order.forEach(function (p) {
      var b = boards[p];
      if (b.state !== "joined") return;
      var row = el("div", "nb");
      var head = el("div", "nb-head");
      head.appendChild(el("b", "", b.label));
      head.appendChild(el("span", "muted small", "node " + b.node + " on " + b.port));
      row.appendChild(head);
      var line = el("div", "room-row");
      var btn = el("button", "", "Show me");
      btn.type = "button";
      btn.onclick = function () {
        btn.disabled = true;
        job("api/setup/identify", { port: b.port, seconds: 20 }).then(function () {
          btn.textContent = "Blinking blue";
          setTimeout(function () { btn.textContent = "Show me"; btn.disabled = false; }, 20000);
        }, function (e) { btn.disabled = false; say(b.label + ": " + e.message); });
      };
      var input = el("input");
      input.placeholder = "Which room is it in? e.g. Kitchen";
      input.setAttribute("list", "room-names");
      input.value = b.room || "";
      input.maxLength = 40;
      input.setAttribute("aria-label", "Room for " + b.label);
      input.onchange = function () {
        api("POST", "api/setup/board", { node: b.node, room: input.value.trim() }).then(function (r) {
          if (!r.ok) say(r.result || "not saved");
          else { b.room = input.value.trim(); say("Saved: " + b.label + (b.room ? " is in " + b.room : " has no room"), "ok"); }
        });
      };
      line.appendChild(btn);
      line.appendChild(input);
      row.appendChild(line);
      host.appendChild(row);
    });
  }

  // ---- step 6: cells (M19), only when the boards make more than one ------------------------------------------
  var tree = null;  // the last proposal: {flat, cells, foremen, notes}

  function nameOf(id) {
    var b = (cell.boards || []).filter(function (x) { return x.node === id; })[0];
    return b && b.label ? b.label : id;
  }

  function chargers() {
    var out = [], boxes = $("chargers").querySelectorAll("input[type=checkbox]");
    for (var i = 0; i < boxes.length; i++) if (boxes[i].checked) out.push(boxes[i].value);
    return out;
  }

  function propose(then) {
    $("do-tree").disabled = true;
    return api("POST", "api/setup/tree", { chargers: chargers() }).then(function (r) {
      if (!r.ok) { say(r.result || "no proposal"); return; }
      tree = r.proposal;
      var host = $("proposal");
      host.textContent = "";
      Object.keys(tree.cells).forEach(function (name) {
        var row = el("div", "nb");
        var head = el("div", "nb-head");
        head.appendChild(el("b", "", name));
        var fm = (tree.foremen[name] || []).map(nameOf);
        head.appendChild(el("span", "muted small", fm.length ? "foreman: " + fm.join(" or ") : "no board on a charger yet"));
        row.appendChild(head);
        row.appendChild(el("div", "small", tree.cells[name].map(nameOf).join(", ")));
        host.appendChild(row);
      });
      var notes = $("tree-notes");
      notes.textContent = "";
      (tree.notes || []).forEach(function (n) {  // the survey names boards by id; the owner knows their names
        notes.appendChild(el("li", "", n.replace(/\b[0-9a-f]{4}\b/g, function (id) { return nameOf(id); })));
      });
      var everyCellLed = Object.keys(tree.cells).every(function (k) { return (tree.foremen[k] || []).length > 0; });
      $("do-tree").disabled = tree.flat || !everyCellLed;
      if (then) then();
    }, function () { say("The hub does not answer."); });
  }

  function cellsStep() {
    // Fresh from the hub: the boards just added are in the cluster file now, with their rooms.
    api("GET", "api/setup/cell").then(function (c) {
      cell = c;
      return propose(function () {
        if (tree.flat) { doneStep(); return; }
        $("li-cells").hidden = false;
        show("s-cells");
        var host = $("chargers"), byRoom = {};
        host.textContent = "";
        (cell.boards || []).forEach(function (b) { (byRoom[b.room || ""] = byRoom[b.room || ""] || []).push(b); });
        Object.keys(byRoom).sort().forEach(function (room) {
          host.appendChild(el("h3", "", room || "No room"));
          var grid = el("div", "grid");
          byRoom[room].forEach(function (b) {
            var lab = el("label");
            var box = el("input");
            box.type = "checkbox";
            box.value = b.node;
            box.onchange = function () { propose(); };
            lab.appendChild(box);
            lab.appendChild(document.createTextNode(b.label || b.node));
            grid.appendChild(lab);
          });
          host.appendChild(grid);
        });
      });
    });
  }

  function writeTree() {
    $("do-tree").disabled = true;
    api("POST", "api/setup/tree", { chargers: chargers(), write: true }).then(function (r) {
      if (!r.ok) { $("do-tree").disabled = false; say(r.result || "not written"); return; }
      doneStep();
      say("The cells are in the cell's file; this step sends them out.", "ok");
    });
  }

  function doneStep() { show("s-done"); $("pp2-row").hidden = !cell.vault; }

  // ---- step 7: done -------------------------------------------------------------------------------------------
  function applyAll() {
    say("");  // a try again: the last try's refusal is not this one's (it stayed above "Done" on 2026-10-23)
    $("do-apply").disabled = true;
    var out = $("apply-out");
    out.hidden = false;
    var body = {};
    if ($("pp2").value) body.passphrase = $("pp2").value;
    $("pp2").value = "";
    job("api/setup/apply", body, function (j) {
      out.textContent = (j.lines || []).slice(-40).join("\n");
      out.scrollTop = out.scrollHeight;
    }).then(function (j) {
      var m = $("done-msg");
      m.hidden = false;
      m.textContent = "Done: " + j.result.verdict + ". ";
      var a = el("a", "", "See the cell on the dashboard");
      a.href = "./";
      m.appendChild(a);
    }, function (e) {
      $("do-apply").disabled = false;
      say("The cell did not take them yet: " + e.message + ". The lines above say why; you can try again.");
    });
  }

  // ---- start --------------------------------------------------------------------------------------------------
  function start() {
    api("GET", "api/setup/cell").then(function (c) {
      if (c._status === 404 || c._status === 401) {
        say("Adding boards is done on the PC the cell's hub runs on: open http://127.0.0.1:8470/setup.html there.", "warn");
        $("s-count").hidden = true;
        return;
      }
      if (!c.system) {  // no cell yet
        firstRun = true;
        $("li-name").hidden = false;
        show("s-name");
        return api("GET", "api/setup/ports").then(function (r) {  // never probe what was plugged in before
          (r.ports || []).forEach(function (p) { baseline[p.port] = true; });
          listOld(r.ports || []);
        });
      }
      cell = c;
      document.title = "Add boards to " + (c.system || "the cell");
      $("cell-name").textContent = c.system || "";
      var spans = document.querySelectorAll(".cell");
      for (var i = 0; i < spans.length; i++) spans[i].textContent = c.system || "the cell";
      $("n-now").textContent = String(c.boards.length);
      $("n-add").max = String(Math.max(1, remaining()));
      checkCount();
      return api("GET", "api/setup/ports").then(function (r) {
        (r.ports || []).forEach(function (p) { baseline[p.port] = true; });
        listOld(r.ports || []);
      });
    }, function () { say("The hub does not answer."); });

    $("do-cell").onclick = function () {
      var sys = $("cell-sys").value.trim(), pp = $("cell-pp").value;
      if (!/^[!-~]{1,31}$/.test(sys) || sys.indexOf("/") >= 0) { say("A name is 1 to 31 letters, digits or signs, without spaces or '/'."); return; }
      if (pp.length < 10) { say("The passphrase needs 10 characters or more."); return; }
      if (pp !== $("cell-pp2").value) { say("The two passphrases differ."); return; }
      $("do-cell").disabled = true;
      api("POST", "api/setup/newcell", { system: sys, passphrase: pp }).then(function (r) {
        $("cell-pp").value = ""; $("cell-pp2").value = "";
        if (!r.ok) { $("do-cell").disabled = false; say(r.result || "the cell was not made"); return; }
        return api("GET", "api/setup/cell").then(function (c) {
          cell = c;
          document.title = "Add boards to " + c.system;
          $("cell-name").textContent = c.system;
          var spans = document.querySelectorAll(".cell");
          for (var i = 0; i < spans.length; i++) spans[i].textContent = c.system;
          $("n-now").textContent = "0";
          $("n-add").max = String(remaining());
          checkCount();
          show("s-count");
        });
      });
    };
    $("n-add").oninput = checkCount;
    $("go-plug").onclick = function () { wanted = parseInt($("n-add").value, 10) || 1; show("s-plug"); scanPorts(); };
    $("go-install").onclick = function () { show("s-install"); findBuild(); };
    $("do-install").onclick = install;
    $("do-join").onclick = joinAll;
    $("go-done").onclick = cellsStep;
    $("do-propose").onclick = function () { propose(); };
    $("do-tree").onclick = writeTree;
    $("do-apply").onclick = applyAll;
    if (!firstRun) show("s-count");
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", start);
  else start();
})();
