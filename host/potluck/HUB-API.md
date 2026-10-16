# The hub's API, version 1

The **hub** (`potluck.hub`) is the program on the PC that holds the cable to the cell and keeps **one model** of it.
Everything else reads that model through this API: the hub's dashboard, the Home Assistant integration
([HOME-ASSISTANT.md](../../HOME-ASSISTANT.md)) and your own programs ([INTEGRATIONS.md](../../INTEGRATIONS.md)). By
default it answers on `127.0.0.1:8470` only, with no login: whoever uses the PC is the owner. Serving the house network
or a phone is a switch the owner turns on, behind a login and encrypted: `--listen 0.0.0.0:8443 --tls-cert <pem>
--tls-key <pem>`, with logins made by `python -m potluck.hubauth add <name> --right look|write`. The hub refuses
`--listen` without both files.

## Starting a hub

From `host/potluck/`, with ESP-IDF's Python ([GETTING-STARTED.md](../../GETTING-STARTED.md)):

```
python -m potluck.hub --cluster <cluster file>
```

The hub opens the serial port the cluster file names (`port`); `--port COM6` (Linux: `/dev/ttyUSB0`) names another, or
starts a hub with no cluster file -- it then knows no package, so it shows the board on its cable and no resources.
With neither, it starts in setup mode: only the setup wizard, `http://127.0.0.1:8470/setup.html`, which makes a cell.
With the package installed (`pip install -e host/potluck`) the same command is `potluck hub ...`; the packaged Windows
build's is `potluck.exe hub ...`. The host tools are tested on Windows 11 only.

| option | default | what |
|---|---|---|
| `--cluster <file>` | none | the cell: its boards and labels, the package, what the outside may see |
| `--port <port>` | the cluster file's `port` | the serial port to the cell |
| `--baud <n>` | the cluster file's `baud` for its port, else 921600 | the port's rate: 921600 for a USB-serial adapter on a board's frame-link pins, 115200 for a board's own USB cable |
| `--tcp <host:port>` | | instead of a port, a TCP endpoint carrying the frame link (an emulator) |
| `--api <addr>` | `127.0.0.1:8470` | this API, with no login |
| `--relay <addr>` | `127.0.0.1:8471` | where the PC's other tools reach the cell through the hub |
| `--pull-ms <ms>` | 2000 | the pull period: how often the model is refreshed |
| `--telemetry-ms <ms>` | 10000 | how often each board's telemetry is pulled over the radio (0: never) |
| `--telemetry-log <file>` | | append every telemetry pull to this file, as JSON lines |
| `--listen <addr>`, `--tls-cert <pem>`, `--tls-key <pem>` | | serve the house network too (below) |
| `--record <dir>`, `--record-hours <h>` | 24 h | keep a recording (Retention, below) |
| `--replay <file>`, `--speed <x>` | 1.0 | serve a recording instead of a cell (Replay, below) |
| `--broker-login <file>` | | two lines, the broker's user name and password, for the hub's own MQTT entities |
| `--no-ha-entities` | | do not publish the hub's own entities to the cluster's broker |

**One hub per PC, one per cell.** A second hub on the same PC is refused (`REFUSED, nothing changed: a hub already runs
on this PC ...`), and so is a hub on another PC while this one holds the cell's hub lease. While a hub runs, the PC's
other tools (`python -m potluck.apply`, `python -m potluck.ctl`) go through it by themselves. Ctrl+C stops it.

**The packaged Windows install** (`tools\build_package.ps1`, then `packaging\install.ps1`) starts a hub for you: its
tray icon starts at login, starts a hub if none answers, and starts one again whenever the hub stops answering. It takes
the cell from `%LOCALAPPDATA%\potluck\tray.json` -- `{"cluster": "<cluster file>", "hub_args": ["--listen",
"0.0.0.0:8443", ...]}`, both optional; relative paths in `hub_args` are from the cluster file's folder -- else the one
cell the setup wizard made, else setup mode. The hub's output goes to `%LOCALAPPDATA%\potluck\logs\hub.log`. To change
its options, edit `tray.json` and end the running `potluck.exe` (Task Manager): the tray starts a new hub within a
minute. Quitting the tray icon leaves the hub running.

## Rights, and what a program sees

Three rights, kept apart: **look** (every GET), **write** (`POST /api/write`: only what the cluster file's `outside`
marks `read_write`, never a safety-relevant resource -- the owning board checks again), **deploy** (never through this
API: only `python -m potluck.apply` on the PC that holds the owner's keys).

A login or a paired program with **look** reads the hub's whole model: every resource the package names, whether or
not the cluster file's `outside` exposes it (`exposed` says which). Making a login, or approving a pairing, is the
owner's approval to read all of it. `outside` decides what the cell presents over MQTT (INTEGRATIONS.md) and what may
be written from outside; it does not hide anything from the hub's API.

Every value comes with its unit, quality and age, and a board's state with since when. Times are the hub's clock,
milliseconds since 1970 (`*_at`), and ages are milliseconds (`*_ms`); the hub maps each board's uptime to its own clock
at every pull, with half the median round trip as the stated error (`clock_error_ms`).

## GET /api/model

The whole model, one JSON object. Fields may be added; none in version 1 will be removed or change meaning.

```json
{
  "api": 1,
  "at": 1791780000000,
  "hub": {"version": "1ee95c3", "up_ms": 5400000, "port": "COM6", "link": "up", "link_detail": "",
          "frames_rx": 120345, "frames_tx": 30211, "pull_period_ms": 2000, "clock_error_ms": 35,
          "telemetry_period_ms": 10000, "telemetry": {"pulls": 540, "replies": 538, "failures": 2, "events": 61},
          "pairing": [], "clients": [], "tls_fp": null},
  "cell": {"system": "lab", "cell_id": "ef76cc2e", "channel": null, "package": 47, "firmware": "78c5305-6df6a4",
           "other_cell_frames": null, "presenter": "7368", "plan": null},
  "boards": [
    {"node": "7368", "label": "B", "room": "Kitchen", "position": [4.5, 2.0], "state": "alive",
     "since_at": 1791775000000, "silent_ms": 1210, "rssi": -34, "cabled": false,
     "firmware": {"version": "78c5305-6df6a4", "counter": 20, "state": "confirmed"},
     "package": {"counter": 47, "state": "confirmed", "live_applies": 1},
     "roles": ["sandbox", "reconciler", "enrolled"],
     "modules": [{"name": "present", "state": "active", "reason": ""},
                 {"name": "wifi", "state": "idle", "reason": "another module is the connection"}],
     "actors": ["die_temp", "led_switch", "present"],
     "up_ms": 9540000, "reset": "power-on", "cluster": "lab",
     "heap": {"internal_free": 152831, "internal_min": 141379, "internal_largest": 77824, "internal_total_kib": null,
              "psram_free": 8100000, "psram_min": 8050000},
     "links": [{"peer": "6300", "state": "alive", "delivery": 0.9952, "rtt_p50_ms": 5, "rtt_p99_ms": 16, "rssi": -34,
                "deaths": 0, "lost": 2, "misses": 0, "cable": false, "window_ms": 600}]}
  ],
  "apps": [
    {"name": "die_b", "module": "builtin:die_temp", "placement": "pinned", "nodes": ["7368"], "running_on": ["7368"],
     "working": true, "detail": "", "outputs": ["potluck://lab/node-7368/hw/die_temp"]},
    {"name": "tick", "module": "builtin:ticker", "placement": "portable",
     "nodes": ["6300", "7368", "8160"], "running_on": ["8160"], "working": true, "detail": "",
     "outputs": ["potluck://lab/act/tick/out"]}
  ],
  "panels": [
    {"kind": "room", "app": "presence_kitchen", "room": "Kitchen", "resource": "potluck://lab/act/presence_kitchen/out",
     "label": "Presence", "on_text": "Someone is here", "off_text": "Empty", "value": true, "quality": "GOOD", "on": true}
  ],
  "resources": [
    {"path": "potluck://lab/node-7368/hw/die_temp", "name": "node-7368/hw/die_temp", "owner": "7368",
     "value": 51.5, "unit": "C", "quality": "GOOD", "age_ms": 2685, "latency_class": 4,
     "exposed": true, "outside_writable": false, "safety_relevant": false, "type": "f32"}
  ],
  "integrations": [
    {"name": "presenting (MQTT)", "kind": "board", "where": "7368", "state": "connected", "since_at": 1791775000000,
     "detail": ""}
  ],
  "deploys": [
    {"at": 1791771300000, "kind": "firmware", "counter": 20, "result": "confirmed", "detail": "confirmed on 3 board(s)"},
    {"at": 1791771600000, "kind": "package", "counter": 47, "result": "verified", "detail": "live on 3 board(s)"}
  ],
  "signals": {"latency_p50_ms": 5.2, "latency_p99_ms": 16.0, "traffic_frames_s": 62.0, "errors_min": 0,
              "saturation": {"peers": 0.14, "namespace": null, "actor_slots": 0.38, "internal_ram": 0.24,
                             "airtime": null}},
  "last_event_seq": 5511
}
```

The example leaves out `hub.relay`, `hub.retention` and `board.telemetry`, described below.

- `hub.version`: the commit the hub was built from. `hub.port`: the serial port it holds. `hub.link`: `up` | `down`
  (the cable to the cell). `hub.tls_fp`: with `--listen`, the SHA-256 of its certificate; `null` without.
  `hub.relay`: counts for the PC's other tools that go through the hub, `{clients, requests, replies, unmatched,
  fanned_out, node_frames_out, node_frames_in}`. `hub.telemetry` counts `{pulls, replies, failures, events}`.
- `cell.channel`, `cell.other_cell_frames`, `signals.saturation.namespace` and `signals.saturation.airtime` are
  reserved: version 1 always sends `null`. `cell.presenter`: the board running the presenting actor, or `null`.
  `signals.saturation.peers` is the boards the hub knows against 21 (a board and the 20 peers it can hold);
  `actor_slots` the busiest board's actors against 8.
- `last_event_seq`: the newest event's `seq` (0 before the first), so a client knows where `GET /api/events` stands.
- `board.state`: `alive` | `dead` | `asleep` (it declared a long window: expected back) | `unknown` (not heard since the
  hub started). A board is `asleep`, not `dead`, when it stops answering after its telemetry said `long_window`; it
  turns `dead` once the cabled board's own view has declared it dead. The model calls a board dead after 3 unanswered
  pulls in a row. `board.silent_ms`: since its last answer.
- `board.label`: the cluster file's label for it. `board.cluster`: the cell name its package carries. `board.cabled`:
  the board on the hub's cable.
- `board.firmware.state`: `confirmed` | `on_trial` (a new image not yet kept) | `factory`. `board.package.state`:
  `confirmed` | `on_trial`.
- `board.roles`: what the board is and does, from `extender-build`, `extender`, `sandbox`, `reconciler`, `relay`,
  `enrolled`, `cabled`.
- `board.modules`: each optional firmware module the image carries (`present`, `wifi`, `sandbox`, `pocket`,
  `extender`), with `state` `active` | `idle` | `failed` and the `reason` when it is not active (`no credentials`, `no
  network`, `standby`, `nothing here uses it`, `out of memory`, `refused`, `error`, `another module is the connection`).
  A module the image does not carry is not listed.
- `board.actors`: the actor types running on the board now.
- `board.reset`: why the board last started, from its `sys/reset` resource -- `power-on`, `software` (an update, a
  reset command, the radio fail-stop), `task watchdog` (a wedge the watchdog cleared), `panic`, `brownout`, `usb`, ...;
  `null` until the hub has read it, and from firmware that does not have it. Read once a boot.
- `board.room` and `board.position`: the cluster file's room name and `[x, y]` on the floor plan (metres from its
  top-left corner, x right, y down), each `null` when it gives none. `cell.plan`: the floor plan, `{"width", "height",
  "rooms": [{"name", "x", "y", "w", "h"}]}` in the same metres, or `null`.
- `board.heap`, `board.links`, `board.rssi` and `board.telemetry` come from the boards' telemetry, pulled over the
  radio every `hub.telemetry_period_ms` (`--telemetry-ms`, default 10000; 0 never). `heap`: internal RAM free, its
  low-water mark and largest block, the internal total in KiB (`null` when the board does not say), PSRAM free and its
  low-water mark; `signals.saturation.internal_ram` is the worst board's internal heap used at its low-water mark. Each
  link: the peer, its `state`, `delivery` (the share of heartbeats acknowledged; `null` on the cable, where nothing is
  acknowledged), round-trip percentiles, `rssi` (`null` when not measured), `deaths` (peer deaths the hub has pulled
  since it started), `lost` (sequence gaps), `misses`, `cable` (the PC on that board's cable) and `window_ms` (the
  peer's declared death window). `board.rssi`: the strongest radio link's. `telemetry` holds the board's counters --
  `{at, channel, flags, up_ms, epoch, peers_alive, peers_dead, deaths, revivals, reboots_seen, rx_total, rx_rejected,
  rx_queue_dropped, tx_total, tx_enqueue_err, events_dropped, writes_served, writes_rejected, actors_pinned,
  actors_portable_here, link_stack_free, actors: [{key, running_here, owner, assigned, activations, fenced, released}]}`;
  `flags` names `no_radio`, `busy`, `connected`, `long_window`, `enrolled`. A board that does not answer telemetry
  (older firmware) has `heap: null`, `links: []` and no `telemetry`, and an event `telemetry_unsupported`; the hub
  asks it again every 5 minutes.
- `panels`: the cards apps ask for in their module descriptors (DEVELOPING-APPS.md, "panel"), one per actor -- per
  room for a per_room module -- and only for exposed resources. `kind`: `room` | `value`; `on`: true, false, or `null`
  while `quality` is not GOOD. Their words are text: show them as text, never as markup.
- `app.placement`: `pinned` | `portable` | `every`. `app.working`: as `python -m potluck.apply` verifies -- running
  where the package says (portable: exactly once in the cell), and every output GOOD. `detail` says why not.
- `resource.value`: a number, a bool, or `null` when it has none to show (strict and not GOOD, or no value at all).
- `resource.quality` (never omitted): `GOOD` (fresh), `STALE` (older than its staleness bound, still shown),
  `UNAVAILABLE` (its owner is dead or unreachable: no value), `NO_DATA` (never published yet), `FAULTY` (the owner says
  its sensor is broken: no value). `resource.age_ms` is the value's age at the model's `at`.
- `resource.settled_quality`: the quality that has held for two reads in a row. Another board's value reaches the hub
  through the cabled board's copy, which is refreshed at the hub's previous read, so a value published every second can
  read `STALE` once and `GOOD` the next time. `quality` shows each read as it is; `app.working`, the dashboard's
  problems and the `quality_changed` event follow `settled_quality`, so one late read is not reported as a fault.
- `resource.owner`: the board whose resource it is, for a board's own resource (`node-XXXX/...`); `null` for an
  actor's output (`act/...`), which can move between boards. `resource.type`: `bool` | `i32` | `u32` | `f32` | `i64` |
  `u64` | `f64` | `bytes` -- what it holds, `null` until it first has a value. A strict resource (its staleness policy
  says an old value must not be shown) that is not `GOOD` arrives with `value: null` -- the hub has already withheld
  the number; a client never needs to know which are strict.
- `integrations`: one entry per board whose image carries the `present` module, `kind: "board"`, with `state`
  `connected` (the module is active) or the module's own `idle` | `failed`, and its reason in `detail`; then each
  program's own report (below), `kind: "program"`, with `state` `connected` | `disconnected` | `error` | `idle`.
- `deploys`: the last 20, oldest first, learned from the boards' firmware and package counters moving -- whoever
  deployed (`apply` through the hub, or a board catching up from a peer); the hub sends none itself. `result`:
  `running` (`detail`: `N of M board(s)`), then `verified` (a package) or `confirmed` (firmware) once every board
  it hears runs it; `reverted`, `refused` and `failed` are reserved, not reported yet.

## GET /api/events?since=<seq>

Events in order, on the hub's clock, with a sequence number for paging. `since` omitted: the last 200. `since` is
inclusive (events with `seq >= since`): pass the previous answer's `next` to get only what is new. `board` is a node id,
or `"hub"` for the hub's own events. A restarted hub numbers its events from 1 again -- a `next` lower than the last one
seen, or `hub.up_ms` going down, means the hub restarted; start again from what it sends.

```json
{"api": 1, "next": 5512, "events": [
  {"seq": 5509, "at": 1791775001234, "board": "8160", "kind": "board_peer_dead",
   "detail": {"peer": "7368", "a": 0, "b": 0, "board_ms": 9533120, "board_seq": 41}},
  {"seq": 5510, "at": 1791775002010, "board": "7368", "kind": "board_dead", "detail": {"was": "alive"}},
  {"seq": 5511, "at": 1791775031870, "board": "7368", "kind": "board_restarted",
   "detail": {"reason": "software", "epoch": 18}}
]}
```

Kinds, in three groups:

- **The boards' own events**, pulled with their telemetry and named `board_` + the board's kind: `board_peer_discovered`,
  `board_peer_admitted`, `board_peer_alive`, `board_peer_dead`, `board_peer_revived`, `board_peer_rebooted`,
  `board_peer_left`, `board_bad_frame`, `board_tx_error`, `board_probe_timeout`, `board_version_pinned`,
  `board_safe_state_rx`, `board_peer_refused`, `board_frame_rejected`, `board_safe_state_rejected`,
  `board_channel_changed`, `board_actor_started`, `board_actor_stopped`, `board_actor_owner`. `board` is the board that
  logged it; detail `{peer, a, b, board_ms, board_seq}`: the other node it concerns, two numbers whose meaning depends
  on the kind, the board's uptime clock when it happened and its own sequence number. `at` is the hub's clock when the
  hub pulled it, not when it happened.
- **What the hub saw**: `board_alive`, `board_dead`, `board_asleep`, `board_unknown` (a board's state changed; detail
  `{was}`), `board_rebooted` (its uptime went back; `{up_s}`), `board_restarted` (a new boot seen in its telemetry while
  the hub watched; `{reason, epoch}`, with `board.reset`'s reason), `telemetry_unsupported` (`{why}`),
  `quality_changed` (`{path, from, to}`), `cabled_board_rebooted`, `deploy` (a `deploys` entry settled).
- **The hub's own**, `board: "hub"`: `hub_started`, `hub_lease_lost`, `relay_client`, `host_node`, `host_node_refused`,
  `login` (`{name, from, result}`), `pair_request`, `pair_answer`, `integration_forgotten`, and outside writes
  (`outside_write`, detail `{resource, value, result}` -- `path` carries the same as `resource`, and a write refused for
  its login adds `by`; a write that reached a board is logged under that board's id).

The names of the first two groups share the `board_` prefix: `board_dead` is the hub's view of a board, `board_peer_dead`
a board's view of a peer.

## GET /api/stream

Server-sent events (`text/event-stream`, each message one `event:` line and one `data:` line holding one JSON object,
then a blank line; on connect the model comes first): `event: model` with the full model once per pull period, `event: events` with
new events as they arrive, in the same shape as `GET /api/events` (`{api, next, events}`; at most the last 200 on connect).
A dashboard needs nothing else; ten open streams cost the cell no more radio than one, since every viewer is served from
the hub's one pull.

The stream takes no `since`: every reconnect sends up to the last 200 events again, so a client drops the ones it has by
`seq`. No `retry:` field is sent. What the dashboard does, and a client should: treat a stream that sends no model for
three pull periods (at least 10 s) as dead; on a drop, poll `GET /api/model` and `GET /api/events?since=<next>` every
2 s, and try the stream again every 15 s.

## POST /api/write

`{"path": "potluck://lab/node-7368/led/on", "value": true}` -> `{"ok": true, "result": "applied"}`. Only resources with
`outside_writable: true`, and only a board's own resource (`.../node-XXXX/...`; an actor's output that moves between
boards cannot be written from outside). `value` is a JSON bool or number -- an integer is written as a 32-bit integer, a
number with a fraction as a 32-bit float -- unless the hub knows the resource's `type`, when a number is written as
that (20 to an `f32` resource is 20.0, 20.0 to an `i32` one is 20), so a client never has to tell them apart; the
owning board refuses a type that is not the resource's. Until the hub has read a value of the resource (`type: null`),
a whole number goes as a 32-bit integer, which a float resource refuses. The write reaches the owning board marked as
from outside, which checks the package again. Every answer is `{"ok", "result"}`:

| status | result | why |
|---|---|---|
| 200 | `applied` | the owning board applied it |
| 400 | `expected {"path": ..., "value": ...}` / `a bool or a number` / `only a board's own resource can be written from outside` | the request |
| 403 | `not writable from outside` | the cluster file's `outside` does not mark it `read_write`, or it is safety-relevant |
| 403 | `not writable by this program` | the cluster file names other programs as its writers (below) |
| 403 | `this login may look, not write` | the login's or token's right is `look` |
| 403 | the board's refusal, lowercased (`type_mismatch`, `not_outside_writable`, ...) | the owning board checked and refused |
| 429 | `slow down: ...` | over this program's share of writes (below) |
| 504 | `the owning board did not answer` | no answer in 2 s |

## Logins: POST /api/login, POST /api/logout, GET /api/whoami

Only on `--listen`. `POST /api/login {"name", "password"}` -> `{"ok": true, "name", "right"}` and a session cookie
(`potluck_session`, HttpOnly, Secure, SameSite=Strict, 12 hours, in the hub's memory: a restarted hub asks again), or
`403` with the reason; five failures from one address in five minutes refuse it for a minute. Without a session every
request but the page's own files, the login and `whoami` is `401`. Every POST must carry `X-Potluck-Request: 1`, which
a page on another site cannot add, so it cannot write through a logged-in browser -- on 127.0.0.1 too, where there is
no login to protect it; and 127.0.0.1 answers only a request whose `Host` is 127.0.0.1, localhost or ::1 (`403`
otherwise), so a page elsewhere cannot reach it through a name of its own that resolves there. `GET /api/whoami` ->
`{"api": 1, "login": true|false, "name", "right"}` -- `login: false` on 127.0.0.1, where `name` is `local` and `right`
`write`; on `--listen`, `name` and `right` are `null` when nobody is logged in. Passwords are kept as scrypt (N=2^17,
r=8, p=1, a random salt each) in `%LOCALAPPDATA%\potluck\hub-users.json` (elsewhere `~/.cache/potluck/hub-users.json`);
a login added while the hub runs counts from its next start. There is no deploy right to log in with.

What the refusals look like: no session, `401 {"error": "login", "result": "log in first"}`; a wrong login,
`403 {"ok": false, "result": "<reason>"}`; a POST without the header (the login's own included),
`403 {"ok": false, "result": "missing X-Potluck-Request"}`; an unknown path, `404 {"error": "not found"}`. The cookie
is set as `potluck_session=<token>; Path=/; HttpOnly; Secure; SameSite=Strict; Max-Age=43200`. The hub's certificate is
its own (self-signed): a program trusts it by the file given to `--tls-cert` (Python: `ssl.create_default_context(
cafile=...)`; Node: `NODE_EXTRA_CA_CERTS`), or by its SHA-256 fingerprint, which the hub announces (below).

## Retention: `hub.retention` in the model

`{"events_kept": 5000, "events_held": <n>, "oldest_event_at": <at>, "recording": null | {"recording": <dir>,
"hours": 24, "files": <n>, "bytes": <n>, "since_file": <name>}}` -- how much history this hub keeps: the last 5000
events in memory, and with `--record <dir>` every pull period's model and the new events in hourly files, the last
`--record-hours` (default 24) kept. The dashboard shows it on the hub's card.

## Replay: POST /api/replay

`python -m potluck.hub --replay <dir>/hub-<date>.jsonl.gz` serves a recording through this same API instead of a cell, so every
tab works on it unchanged; the model then carries `hub.replay` = `{"file", "from_at", "to_at", "position_at",
"speed", "paused", "models"}`, times on the recording's clock. `POST /api/replay {"seek_at": <at>, "speed": <x>,
"paused": <bool>}` moves it (any login; it changes nothing in a cell). Writes are `403`: there is no cell behind it.
The recording is the model the dashboard showed, not the radio's frames.

## Announced on the network, and paired

With `--listen`, the hub announces itself by multicast DNS (RFC 6762/6763), so Home Assistant finds it with nothing
typed: service type `_potluck._tcp.local.`, instance `Potluck <system> <cell id>`, port the `--listen` port, and TXT
records `txtvers=1`, `cell=<cell id>` (8 hex digits), `system=<name>`, `api=1`, `tls=1`, `fp=<SHA-256 of the hub's
certificate, DER, 64 lowercase hex digits>`. It answers a querier on port 5353 by multicast and a one-shot query from
any other port by unicast (RFC 6762 6.7).

What the announcement is worth: it is not authenticated -- anyone on the network can announce the same cell id with a
certificate of their own. A client pins the `fp` it paired with, so its token only ever goes to that certificate, and
the hub's certificate is self-signed, so `fp` is all there is to check it against. To know the `fp` is your hub's,
compare it with the fingerprint on the dashboard's hub card on the PC (**Certificate**) or with `openssl x509 -in
<the hub's certificate> -noout -fingerprint -sha256`; Home Assistant's discovery does not show it, so there the check
that counts is the owner's: the pairing request shows the asking address (`from`), and the owner allows only a request
from the address the program really has. Where multicast does not reach (another network, a container), the
fingerprint is also in `GET /api/hello` and on the first TLS connection.

`GET /api/hello` (no login, on both addresses) -> `{"api": 1, "cell_id", "system", "version", "tls_fp"}`: who this
hub is, before pairing -- nothing a client cannot also read from the announcement.

Pairing gives a program a token, approved by the owner on the PC, with nothing typed on the program's side:

- `POST /api/pair` (on `--listen`, no session needed, and like every POST with `X-Potluck-Request: 1`)
  `{"name": "Home Assistant", "right": "look"}` -> `202 {"pair_id": "<id>", "expires_in_s": 300}`. At most 4 pending requests; more are `429`.
- `GET /api/pair/<pair_id>` (no session needed either) -> `{"state": "pending"}`, then `{"state": "approved", "name",
  "right", "token"}` (the token is shown once), or `{"state": "denied"}`; `404` once it expired or was fetched. Pairing
  exists only on `--listen`: on 127.0.0.1 there is nothing to pair (no login at all). A second program asking under a
  name already paired gets that name with a number added (`Home Assistant 2`), so `hubauth revoke` can tell them apart.
- The owner sees each pending request in the model (`hub.pairing`: `[{"pair_id", "name", "right", "from", "at"}]`) and
  on the dashboard's hub card, and answers on the PC: `POST /api/pairing {"pair_id": "<id>", "approve": true|false}` --
  on 127.0.0.1 only, never on `--listen`.
- A paired program sends `Authorization: Bearer <token>` instead of a session; the token carries the right it was
  approved with (`look` or `write`; never deploy). Tokens are kept hashed (SHA-256) in the logins file and listed and
  removed with `python -m potluck.hubauth tokens` / `python -m potluck.hubauth revoke <name>`; a revoke counts from the
  token's next request, without restarting the hub.

## The hub's own entities in Home Assistant

When the cluster file names a broker (`present`) the hub also connects to it and publishes, by MQTT discovery and on
the same devices the cell's presenting role creates (identifiers `potluck_<cell>_<board>` and `potluck_<cell>_cell`),
what only it knows: each board's state (`alive`/`asleep`/`dead`/`unknown`), internal RAM free, uptime, signal strength
and worst heartbeat delivery as diagnostic sensors, and its firmware as an update entity (installed = the board's,
latest = the newest in the cell; no command topic, so Home Assistant installs nothing); and on the cell's device, apps
working and a problem binary sensor per app. It needs no Home Assistant integration: any MQTT consumer can read the same
topics (INTEGRATIONS.md lists them). It connects as client `potluck-<cell>-hub`, to port 8883 with TLS when the
cluster file gives `present.tls_ca` and 1883 without, unless the broker names a port; `--broker-login` gives it a
broker login, and `--no-ha-entities` turns all of it off. They are available while the hub is (`potluck/<cell>/hub`,
`online`/`offline`, its will `offline`): with the PC off they go unavailable and the boards' resources stay live
through the cell's own presenter.

## Integrations report their state, and each program has a share

`POST /api/integrations {"name": "Node-RED", "state": "connected" | "disconnected" | "error" | "idle", "detail": "..."}`
-> `{"ok": true}` (any login or token; on 127.0.0.1 no login). Report under one name, the one you paired with; a second
report under the same name replaces the first. The model then lists it in `integrations` beside the
cell's own presenter: `{"name", "kind": "program", "where": <who sent it>, "state", "since_at", "detail", "last_at"}`,
and a report older than 60 s reads `disconnected` ("silent for N s"). Report at least every 30 s while running.
A program that stopped for good stays listed as down until the owner forgets it: `POST /api/integrations/forget
{"name": ...}` on 127.0.0.1 only (the dashboard's Integrations tab has a Forget button there) -> 200, or 409 for a
program still connected; it comes back the next time it reports.

Every program has a share of what the hub may ask of the cell, by who it is -- a login or token by name; on 127.0.0.1
the `X-Potluck-Client: <name>` header it sends (send one), else its address: requests 20 a second (bursts of 40),
writes 2 a second (bursts of 4). Over its share it gets `429 {"ok": false, "result": "slow down: ..."}` with
`Retry-After: <whole seconds>` -- opening the stream included -- and only it is slowed: every read is answered from the hub's one model and never reaches the
cell, and writes, which do, cannot crowd it. The open stream counts once, when it opens -- use it rather than polling.
`hub.clients` in the model: `[{"name", "requests", "writes", "refused", "last_at"}]`, every program seen in the last
10 minutes.

A writable resource may name the programs that may write it **through this API**, in the cluster file:
`"outside": {"node-7368/led/on": {"mode": "read_write", "writers": ["Node-RED"]}}` -- by the name each paired or logged
in. Through the hub, any other program gets `403 {"ok": false, "result": "not writable by this program"}`; the owner on
the PC (127.0.0.1) always may. A writers list is the hub's rule only: the boards never see it. **It does not cover
writes over MQTT.** The cell's presenting board takes writes from the broker (`potluck/<cell>/<hash>/set`) and checks
only what the boards know -- `read_write` in `outside`, and not safety-relevant -- so anyone who can publish on the
broker can write every resource the owner made writable from outside. Home Assistant writes that way (its Potluck
integration pairs with `look` and never writes through the hub), so a writers list does not apply to it. Protect the
broker: give it a login (HOME-ASSISTANT.md, step A.6) and TLS (`present.tls_ca`, port 8883).

## The setup wizard: /api/setup/*

The owner's actions on boards plugged into the PC, served **only on 127.0.0.1** (on `--listen` every one is 404) and,
like every POST there, only with `X-Potluck-Request` and a `Host` of 127.0.0.1, localhost or ::1. The page is
`setup.html` ("Add boards" on the dashboard). Board actions are jobs: a POST answers `202 {"ok": true, "job": <id>}`
and `GET /api/setup/jobs/<id>` follows it -- `{"id", "kind", "port", "state": "running" | "done" | "failed", "pct",
"result", "error", "lines"}`; one job at a time per port (else 409). `GET /api/setup/jobs` lists them.

| call | what |
|---|---|
| `GET /api/setup/cell` | `{"system", "boards": [{"node", "label", "room"}], "max_boards": 20, "vault": <keys sealed>}` |
| `GET /api/setup/ports` | USB serial ports, `[{"port", "vid", "pid", "chip", "description"}]`, without the hub's own |
| `GET /api/setup/build` | `{"version", "build"}`: the firmware most boards run and its build on this PC (409 if there is none); with no board yet, `{"version": null, "build", "note"}`, the newest firmware built on this PC |
| `POST /api/setup/probe {"port"}` | job: chip, flash, PSRAM, MAC, and whether it can run Potluck -- **restarts the board** |
| `POST /api/setup/flash {"port", "build"}` | job: writes that build (only one under the hub's build cache; else 403) |
| `POST /api/setup/enrol {"port", "passphrase"?, "node_id"?}` | job: a key signed by the cell's CA and a node id from its registry -- the lowest free one, or `node_id` (hex) if given; `result.node` |
| `POST /api/setup/wifi {"port", "ssid", "password"}` | job: the house Wi-Fi into the board (kept only there; never logged) |
| `POST /api/setup/reset {"port"}` | job: restarts the board (a new id and a saved Wi-Fi take effect then) |
| `POST /api/setup/identify {"port", "seconds"?}` | job: the board's light blinks blue (default 20 s, at most 300) |
| `POST /api/setup/board {"node", "label"?, "room"?, "position"?}` | adds the board to the cluster file or sets its room (`""` clears it) or its `[x, y]` on the floor plan (`[]` clears it); the file's previous version is kept as `<file>.bak`, and a change `apply` would refuse is never written (409). With `{"plan": {...} | null}` instead, sets the floor plan |
| `POST /api/setup/tree {"chargers": ["<hex id>", ...], "write"?: true}` | for more than twenty boards: proposes a tree of cells from the boards' measured links (potluck.survey: who hears whom, rooms as hints, ten boards a cell, foreman candidates from the boards on a charger) -- `{"proposal": {"flat", "cells", "foremen", "notes"}, "written"}`; twenty boards or fewer that all hear each other stay one cell. With `"write": true` the proposal goes into the cluster file (refused, 409, while a cell has no board on a charger) |
| `POST /api/setup/apply {"passphrase"?}` | job: `python -m potluck.apply <cluster file>`, the passphrase unlocking sealed keys for it; its output in `lines`; done on VERIFIED, failed otherwise |
| `POST /api/setup/newcell {"system", "passphrase"}` | a PC with no cell (the hub started without a port or cluster file): makes the cell -- its keys sealed under the passphrase, an empty cluster file -- under `%LOCALAPPDATA%\potluck\cells\<system>`; `{"ok", "cluster"}` names the file |
| `POST /api/setup/attach {"port"}` | once, after the first board is flashed and enrolled: its own USB port goes into the cluster file at 115200 and the hub attaches to it; `{"ok", "node"}`, the node that answered (`null` if none did) |
| `GET /api/setup/library` | `{"apps": [{"name", "enabled", "built_in"}], "home_assistant": {"on", "broker", "tls"}, "resources": [{"path", "name", "mode": "off" \| "read" \| "read_write", "controllable", "missing"?}]}` -- `controllable`: writable and not safety-relevant, so it may be offered as controllable; `missing`: a rule for a resource the package no longer names |
| `POST /api/setup/library {"app", "enabled"}` or `{"outside": {path: "off" \| "read" \| "read_write"}}` | an app on or off (its manifests in or out of the package; the image unchanged), or what systems outside the cell may see and control -- the cluster file's `outside`; the next `apply` carries either |
| `GET /api/setup/store`, `POST /api/setup/store {"on"}` | the app store: `{"on", "apps": [{"name", "repo", "ref", "sha256", "author"}]}`; off until the owner turns it on (kept in the cell's `store.json`) |
| `POST /api/setup/store/feed` | the store's list (Tubifix77/potluck-store's `feed.json`), its signature checked against the store key Potluck carries: `{"apps": [{"repo", "dir", "ref", "name", "description", "installed"}], "removed"}`; 409 while the store is off or the signature fails |
| `POST /api/setup/store/look {"repo", "dir"?, "ref"?}` | fetches `potluck-app.json` and its module from that GitHub repository and checks the author's signature (the store's calls are the only ones in which the hub reaches outside the house): `{"name", "description", "author_key", "sha256", "author": "new" \| "pinned" \| "changed", "inputs", "outputs", "candidates": [[path, ...] per input]}`; 409 while the store is off |
| `POST /api/setup/store/install {"repo", "dir"?, "ref"?, "bindings": [path per input], "passphrase"?}` | fetches and checks it again, pins the author's key to the repository (a changed key is refused), certifies it under the cell's CA (role 3, author numbers from 100), writes `store/<name>.guest.json` and `store/<name>.json` and adds the fragment to the cluster file; `{"installed": {"name", "author", "fragment"}}` -- Apply sends it |
| `POST /api/setup/store/remove {"name"}` | takes the app's fragment out of the cluster file (its author stays pinned) |
| `POST /api/setup/broker {"broker"?, "tls"?}` | tries the broker from this PC (the cluster's by default, with its CA when it has one): `{"ok", "result"}` -- `connected`, a login wanted or refused, TLS wanted, no answer |

The hub's own serial port is refused (409). A passphrase unlocks the cell's sealed keys for that one action (an
enrolment, an apply, an install), and they are locked again after it.
