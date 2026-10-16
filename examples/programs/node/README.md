# potluck-watch: a program of your own, in Node.js

**Who this is for:** someone with a Potluck cell and its hub running who wants their own program to read what the
cell knows, follow it as it changes, and switch what the cell allows to be switched -- in a language other than
Python. This one is in JavaScript for Node.js; it is one file of under 600 lines, uses no packages, and uses nothing beyond
the hub's API document ([host/potluck/HUB-API.md](../../../host/potluck/HUB-API.md)).

Terms used here:

- **Cell** -- the boards that hear each other over the radio and run your apps together.
- **Hub** -- `potluck.hub`, the program on the PC that holds the cable to one board of the cell and keeps one
  picture of the whole cell, the **model**. Everything outside the cell (the dashboard, Home Assistant, this
  program) reads the model from the hub over HTTP, by default at `http://127.0.0.1:8470`.
- **Resource** -- one named value in the cell, such as `potluck://lab/node-7368/hw/die_temp` (board 7368's chip
  temperature). A resource's value always comes with its **unit**, its **quality** (`GOOD`, `STALE`,
  `UNAVAILABLE`, `NO_DATA` or `FAULTY`: whether the value can be trusted) and its **age** (how old it is). This program never prints
  a value without all three.
- **Writable from outside** -- a resource the package (the owner's signed list of what the cell runs) lets programs
  outside the cell write: `outside_writable: true` in the model. Nothing else can be written from outside, and a
  safety-relevant resource never can; the hub answers such a write with `403`, and the board that owns the
  resource checks again.
- **Stream** -- `GET /api/stream`, a connection the hub keeps open and writes to as things happen
  ([server-sent events](https://developer.mozilla.org/en-US/docs/Web/API/Server-sent_events)): the whole model once
  per pull period (every 2 s by default) and each new **event** (a board died, an actor moved, a write was made).

## What you need

- Node.js 18 or newer (`node --version`). Nothing to install: the program uses only what Node has built in.
- A hub: `python -m potluck.hub --cluster <cluster file>` from `host/potluck/`, or the one the packaged Windows
  install's tray icon starts at login (HUB-API.md, "Starting a hub"). Without a cell, use the hub's stand-in, which
  simulates three boards and speaks the same API:

```
python tools/hub_mock.py
```

(from the repository's top directory, with Python 3 and nothing else installed. It serves `http://127.0.0.1:8470` and talks to no
board.)

## Run it

From this directory:

```
node potluck-watch.mjs read                                   # the model, once
node potluck-watch.mjs watch                                  # changes and new events, until Ctrl+C
node potluck-watch.mjs                                        # both: read, then watch
node potluck-watch.mjs write potluck://lab/node-7368/led/on true
node potluck-watch.mjs --once                                 # everything below, once, and exit
```

`--hub URL` names another hub (or set `POTLUCK_HUB`). `VALUE` for `write` is `true`, `false` or a number.

`read` prints the boards with their state, and every resource:

```
Boards (3)
  A (6300)  alive   since 17:22:41  cabled  firmware 78c5305-6df6a4 #20 confirmed  package 47 confirmed
  B (7368)  alive   since 17:23:46  firmware 78c5305-6df6a4 #20 confirmed  package 47 confirmed
  C (8160)  dead    since 18:23:30, silent 20.5 s  firmware 78c5305-6df6a4 #20 confirmed  package 47 confirmed

Resources (10)
  node-7368/hw/die_temp  49.8 C, STALE, 51.5 s old
  node-7368/led/on       false, GOOD, 1.3 s old  [writable from outside]
  node-8160/rf/link      no value, UNAVAILABLE, 20.8 s old
  act/alarm/level        1 (no unit), GOOD, 600 ms old  [safety-relevant]
```

`watch` prints a line for every board whose state changes, every resource whose value or quality changes, and every
new event:

```
18:23:30  event #12  6300  board_peer_dead {"peer":"8160","a":0,"b":0,"board_ms":3725120,"board_seq":41}
18:23:32  board C (8160): alive -> dead since 18:23:30, silent 4.1 s
18:23:32  node-8160/rf/link: -36.4 dB, GOOD, 3.8 s old -> no value, UNAVAILABLE, 6.0 s old
```

Uptimes move every second, so `sys/uptime` value changes are not printed; `--ignore TEXT` hides more (any path
containing TEXT), `--ignore ""` shows everything. A quality change is always printed. `--for S` stops after S seconds.

`write` prints the hub's answer as it came:

```
write potluck://lab/node-7368/led/on = true: applied (HTTP 200, {"ok":true,"result":"applied"})
write potluck://lab/act/alarm/level = 1: refused (HTTP 403, {"ok":false,"result":"not writable from outside"})
```

`--once` reads the model, writes the first resource that is writable from outside (a true/false one is flipped),
writes one that is not and shows the `403`, watches until the hub reports the write as an event and a model shows
the new value (at most 20 s, or `--for S`), then writes the old value back, so the cell is left as it was. On a
real cell that means the resource really changes for a moment -- a board's LED, say, blinks.

Exit codes: `0` done (or a write applied), `3` a write refused (`403`), `1` anything else that went wrong, `2` a
mistake on the command line.

## When the stream drops

When the stream closes, the program reconnects once after 2 s (or after the hub's `retry:` time, if it sends one).
If that fails it does what the dashboard does: asks for `GET /api/model` and `GET /api/events?since=<n>` every 2 s
instead, and tries the stream again every 15 s (`--retry-ms`). A stream that stays open but sends no model for three pull periods (at least 10 s) counts as
dropped. Events are counted by their sequence number (`seq`), so none is printed twice across reconnects; when the
numbers start again lower, the hub has restarted, and the program says so. On connecting, the hub sends up to its
last 200 events; the program prints the last 5 of those.

## A hub behind a login

A hub opened to the house network (`potluck.hub --listen ... --tls-cert ... --tls-key ...`) is reached over
`https://` and asks for a login, made on the PC with `python -m potluck.hubauth add <name> --right look|write`
(`look` may read, `write` may also write). Give this program the name and password in the environment, never on
the command line (where other users of the PC could see it):

```
set POTLUCK_HUB_USER=<name>            (PowerShell: $env:POTLUCK_HUB_USER = "<name>")
set POTLUCK_HUB_PASSWORD=<password>
set NODE_EXTRA_CA_CERTS=C:\path\to\the-hub-cert.pem
node potluck-watch.mjs --hub https://192.168.1.20:8443 read
```

The program logs in (`POST /api/login`), keeps the session cookie it gets (`potluck_session`) for every request
after, and logs in again if the hub forgets it (a restarted hub does). It prints neither the name nor the password.
It refuses to send a password over plain `http://` to anything but this PC. `NODE_EXTRA_CA_CERTS` is Node's own
setting for trusting a certificate it does not know -- the hub's own, made for your house, is one of those.

Every POST carries the header `X-Potluck-Request: 1`, which the hub asks for on every POST, on 127.0.0.1 as behind a
login (a web page on another site cannot add it, so it cannot write through someone's browser).

## What it does not do

It never deploys: no program outside the owner's PC can, through this API or any other. It writes only what you
tell it to (or, with `--once`, one resource the package marks writable, and puts it back), and the hub decides
what is allowed -- this program only reports the answer. Fields the hub adds to the model in later versions are
ignored.

## Tests

`python host/potluck/tests/test_node_example.py` runs the program against the hub mock: reading, `--once`, a board
dying while it watches, a stream that fails (so it must poll), and a login. It is skipped when Node.js is not
installed. `tools/run_all_tests.ps1` runs it with the other suites.
