# The hub's API, version 1 (M14, M16-M18)

`potluck-hub` (M14) holds the cable to the cell and keeps **one model** of it. Everything else reads that model through
this API: the dashboard (M16), Home Assistant's integration (M17), the owner's own programs (M18). It is served on the
PC, by default on `127.0.0.1:8470` only; reaching it from the house network or a phone is a switch the owner turns on,
behind a login, encrypted (ARCHITECTURE section 13, M16): `potluck.hub --listen 0.0.0.0:8443 --tls-cert <pem>
--tls-key <pem>`, with logins made by `python -m potluck.hubauth add <name> --right look|write`. The hub refuses
`--listen` without both. On 127.0.0.1 there is no login: that is the PC's own user.

Three rights, kept apart: **look** (every GET), **write** (`POST /api/write`, only what the package lets the outside
write, never a safety-relevant resource -- the owning board checks again), **deploy** (never through this API: only
`potluck apply` on the PC that holds the owner's keys).

Every number carries what section 4 says it must: a value comes with its unit, quality and age; a board's state with
since when. Times are the hub's clock, milliseconds since 1970 (`*_at`), and ages are milliseconds (`*_ms`); the hub
maps each board's uptime to its own clock at every pull, with the round trip as the stated error (`clock_error_ms`).

## GET /api/model

The whole model, one JSON object. Fields may be added; none in version 1 will be removed or change meaning.

```json
{
  "api": 1,
  "at": 1791780000000,
  "hub": {"version": "e61b92c", "up_ms": 5400000, "port": "COM6", "link": "up", "link_detail": "",
          "frames_rx": 120345, "frames_tx": 30211, "pull_period_ms": 2000, "clock_error_ms": 35},
  "cell": {"system": "lab", "cell_id": "ef76cc2e", "channel": 1, "package": 47, "firmware": "78c5305-6df6a4",
           "other_cell_frames": 0, "presenter": "7368"},
  "boards": [
    {"node": "7368", "label": "B", "state": "alive", "since_at": 1791775000000, "silent_ms": 0, "rssi": -34,
     "cabled": false,
     "firmware": {"version": "78c5305-6df6a4", "counter": 20, "state": "confirmed"},
     "package": {"counter": 47, "state": "confirmed", "live_applies": 1},
     "roles": ["extender", "sandbox", "reconciler", "relay", "enrolled"],
     "modules": [{"name": "present", "state": "active", "reason": ""},
                 {"name": "wifi", "state": "idle", "reason": "another module is the connection"}],
     "actors": ["rf_link", "rf_indicator", "die_temp", "led_switch", "present"],
     "heap": {"internal_free": 152831, "internal_min": 141379, "internal_largest": 77824, "psram_free": 8100000},
     "up_ms": 9540000,
     "links": [{"peer": "6300", "state": "alive", "delivery": 0.9952, "rtt_p50_ms": 5, "rtt_p99_ms": 16,
                "deaths": 0, "rssi": -34}]}
  ],
  "apps": [
    {"name": "die_b", "module": "builtin:die_temp", "placement": "pinned", "nodes": ["7368"], "running_on": ["7368"],
     "working": true, "detail": "", "outputs": ["potluck://lab/node-7368/hw/die_temp"]},
    {"name": "rf_fusion", "module": "ext:passive_sensor/rf_fusion", "placement": "portable",
     "nodes": ["6300", "7368", "8160"], "running_on": ["8160"], "working": true, "detail": "",
     "outputs": ["potluck://lab/act/rf_fusion/out"]}
  ],
  "resources": [
    {"path": "potluck://lab/node-7368/hw/die_temp", "name": "node-7368/hw/die_temp", "owner": "7368",
     "value": 51.5, "unit": "C", "quality": "GOOD", "age_ms": 2685, "latency_class": 4,
     "exposed": true, "outside_writable": false, "safety_relevant": false}
  ],
  "integrations": [
    {"name": "presenting (MQTT)", "kind": "board", "where": "7368", "state": "connected", "since_at": 1791775000000,
     "detail": "192.168.0.77:1883, 4 entities"}
  ],
  "deploys": [
    {"at": 1791771300000, "kind": "firmware", "counter": 20, "result": "confirmed", "detail": "confirmed on 3 board(s)"},
    {"at": 1791771600000, "kind": "package", "counter": 47, "result": "verified", "detail": "live on 3 board(s)"}
  ],
  "signals": {"latency_p50_ms": 5, "latency_p99_ms": 16, "traffic_frames_s": 62.0, "errors_min": 0.4,
              "saturation": {"peers": 0.15, "namespace": 0.31, "actor_slots": 0.38, "internal_ram": 0.24,
                             "airtime": 0.26}}
}
```

- `hub.link`: `up` | `down` (the cable to the cell). `hub.tls_fp`: with `--listen`, the SHA-256 of its certificate.
- `board.state`: `alive` | `dead` | `asleep` (a declared long window, M5.1) | `unknown` (never heard since the hub started).
- `board.room` and `board.position` (M20): the cluster file's room name and `[x, y]` on the floor plan (metres from its
  top-left corner, x right, y down), each `null` when it gives none. `cell.plan`: the floor plan, `{"width", "height",
  "rooms": [{"name", "x", "y", "w", "h"}]}` in the same metres, or `null`.
- `board.heap`, `board.links`, `board.rssi` and `board.telemetry` come from M15's telemetry, pulled over the radio every
  `hub.telemetry_period_ms` (`--telemetry-ms`, default 10000; 0 never): `heap` adds `internal_total_kib` and `psram_min` (`signals.saturation.internal_ram` is the worst
  board's internal heap used at its low-water mark); each link adds `lost`
  (sequence gaps), `misses`, `cable` (the PC on that board's cable) and `window_ms` (the peer's declared death window);
  `delivery` is `null` on the cable, where nothing is acknowledged. `telemetry` holds the board's counters --
  `{at, channel, flags, up_ms, epoch, peers_alive, peers_dead, deaths, revivals, reboots_seen, rx_total, rx_rejected,
  rx_queue_dropped, tx_total, tx_enqueue_err, events_dropped, writes_served, writes_rejected, actors_pinned,
  actors_portable_here, link_stack_free, actors: [{key, running_here, owner, assigned, activations, fenced, released}]}`;
  `flags` names `no_radio`, `busy`, `connected`, `long_window`, `enrolled`. A board that never answers telemetry (firmware
  before M15) has `heap: null`, `links: []` and no `telemetry`, and an event `telemetry_unsupported`.
  `hub.telemetry` counts `{pulls, replies, failures, events}`.
- A board is `asleep`, not `dead`, when it stops answering after its telemetry said `long_window`.
- `module.state`: `active` | `idle` | `failed` (NODE_STATUS version 2), or absent from the list when the image does not carry it.
- `app.working`: as `potluck apply` verifies -- running where the package says (portable: exactly once in the cell),
  and every output GOOD. `detail` says why not.
- `resource.value`: a number, a bool, or `null` when it has none to show (strict and not GOOD, or no value at all).
- `resource.quality` (section 4 rule 2; never omitted): `GOOD` (fresh), `STALE` (older than its staleness bound, still
  shown), `UNAVAILABLE` (its owner is dead or unreachable: no value), `NO_DATA` (never published yet), `FAULTY` (the
  owner says its sensor is broken: no value). `resource.age_ms` is the value's age at the model's `at`.
- `integration.state`: `connected` | `disconnected` | `idle` | `off` | `error` (a program's own report, M18: `error` --
  it runs but cannot do its job, say its login was refused; `disconnected` -- it cannot reach the hub or the other
  system; `idle` -- nothing to do; the cell's presenter is `off` when no board carries it).
- `resource.owner`: the board that owns it now -- for a portable actor's output, the board running the actor, which
  changes when the actor moves. `resource.type`: `bool` | `i32` | `u32` | `f32` | ... -- what it holds, `null` until it
  first has a value. The model holds every resource the package names, for the owner's dashboard; an integration shows
  only those with `exposed: true`. A strict resource (section 4) that is not `GOOD` arrives with `value: null` -- the hub
  has already withheld the number; a client never needs to know which are strict.
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
  {"seq": 5510, "at": 1791775001234, "board": "8160", "kind": "peer_dead", "detail": {"peer": "7368"}},
  {"seq": 5511, "at": 1791775062000, "board": "7368", "kind": "tx_dead", "detail": {"failing_ms": 60007}}
]}
```

Kinds include every node event the boards keep, pulled by M15's telemetry and prefixed `board_` (`board_peer_dead`,
`board_peer_rebooted`, `board_actor_started`, `board_channel_changed`, ...; detail `{peer, a, b, board_ms, board_seq}`,
`at` the hub's clock when pulled), the hub's own (`hub_started`, `board_alive`, `board_dead`, `board_asleep`,
`board_rebooted`, `relay_client`, `host_node`, `hub_lease_lost`, `telemetry_unsupported`), deploys (`deploy`), and
outside writes (`outside_write`, detail `{resource, value, result}`; `path` carries the same as `resource`, and a
write refused by the login's right adds `by`).

## GET /api/stream

Server-sent events (`text/event-stream`, each message one `event:` line and one `data:` line holding one JSON object,
then a blank line; on connect the model comes first): `event: model` with the full model once per pull period, `event: events` with
new events as they arrive, in the same shape as `GET /api/events` (`{api, next, events}`; at most the last 200 on connect).
A dashboard needs nothing else; ten open streams cost the cell no more radio than one, since every viewer is served from
the hub's one pull (M15).

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
owning board refuses a type that is not the resource's. The write reaches
the owning board marked as from outside, which checks the package again. Every answer is `{"ok", "result"}`:

| status | result | why |
|---|---|---|
| 200 | `applied` | the owning board applied it |
| 400 | `expected {"path": ..., "value": ...}` / `a bool or a number` / `only a board's own resource can be written from outside` | the request |
| 403 | `not writable from outside` | the package does not allow it, or it is safety-relevant |
| 403 | `this login may look, not write` | the login's or token's right is `look` |
| 403 | the board's refusal, lowercased (`type_mismatch`, `not_outside_writable`, ...) | the owning board checked and refused |
| 504 | `the owning board did not answer` | no answer in 2 s |

## Logins (M16): POST /api/login, POST /api/logout, GET /api/whoami

Only on `--listen`. `POST /api/login {"name", "password"}` -> `{"ok": true, "name", "right"}` and a session cookie
(`potluck_session`, HttpOnly, Secure, SameSite=Strict, 12 hours, in the hub's memory: a restarted hub asks again), or
`403` with the reason; five failures from one address in five minutes refuse it for a minute. Without a session every
request but the page's own files, the login and `whoami` is `401`. Every POST must carry `X-Potluck-Request: 1`, which
a page on another site cannot add, so it cannot write through a logged-in browser -- on 127.0.0.1 too, where there is
no login to protect it (since 2026-10-15); and 127.0.0.1 answers only a request whose `Host` is 127.0.0.1 or localhost
(`403` otherwise), so a page elsewhere cannot reach it through a name of its own that resolves there. `GET /api/whoami` ->
`{"api": 1, "login": true|false, "name", "right"}` -- `login: false` on 127.0.0.1; `name` and `right` are `null` when
nobody is logged in. Passwords are kept as scrypt (N=2^17, r=8, p=1, a random salt each) in
`%LOCALAPPDATA%/potluck/hub-users.json`. There is no deploy right to log in with.

What the refusals look like: no session, `401 {"error": "login", "result": "log in first"}`; a wrong login,
`403 {"ok": false, "result": "<reason>"}`; a POST without the header (the login's own included),
`403 {"ok": false, "result": "missing X-Potluck-Request"}`; an unknown path, `404 {"error": "not found"}`. The cookie
is set as `potluck_session=<token>; Path=/; HttpOnly; Secure; SameSite=Strict; Max-Age=43200`. The hub's certificate is
its own (self-signed): a program trusts it by the file given to `--tls-cert` (Python: `ssl.create_default_context(
cafile=...)`; Node: `NODE_EXTRA_CA_CERTS`), or by its SHA-256 fingerprint, which the hub announces (below).

## Retention (M16): `hub.retention` in the model

`{"events_kept": 5000, "events_held": <n>, "oldest_event_at": <at>, "recording": null | {"recording": <dir>,
"hours": 24, "files": <n>, "bytes": <n>, "since_file": <name>}}` -- how much history this hub keeps: the last 5000
events in memory, and with `--record <dir>` every pull period's model and the new events in hourly files, the last
`--record-hours` (default 24) kept. The dashboard shows it on the hub's card.

## Replay (M16): POST /api/replay

`potluck.hub --replay <dir>/hub-<date>.jsonl.gz` serves a recording through this same API instead of a cell, so every
tab works on it unchanged; the model then carries `hub.replay` = `{"file", "from_at", "to_at", "position_at",
"speed", "paused", "models"}`, times on the recording's clock. `POST /api/replay {"seek_at": <at>, "speed": <x>,
"paused": <bool>}` moves it (any login; it changes nothing in a cell). Writes are `403`: there is no cell behind it.
The recording is the model the dashboard showed, not the radio's frames -- those are section 7.6's capture, which
replays the cell byte for byte.

## Announced on the network, and paired (M17)

With `--listen`, the hub announces itself by multicast DNS (RFC 6762/6763), so Home Assistant finds it with nothing
typed: service type `_potluck._tcp.local.`, instance `Potluck <system> <cell id>`, port the `--listen` port, and TXT
records `txtvers=1`, `cell=<cell id>` (8 hex digits), `system=<name>`, `api=1`, `tls=1`, `fp=<SHA-256 of the hub's
certificate, DER, 64 lowercase hex digits>`. It answers a querier on port 5353 by multicast and a one-shot query from
any other port by unicast (RFC 6762 6.7). Where multicast does not reach (another network, a container), the same
fingerprint is on the dashboard's hub card on the PC and in `GET /api/hello`; compare it there before trusting it, and
the owner sees the asking address (`from`) before clicking Allow. A client checks the certificate it is served against `fp` -- the hub's certificate is
self-signed, so the announcement is what it is checked against -- and the owner's approval below is what makes it
trusted.

`GET /api/hello` (no login, on both addresses) -> `{"api": 1, "cell_id", "system", "version", "tls_fp"}`: who this
hub is, before pairing -- nothing a client cannot also read from the announcement.

Pairing gives a program a token, approved by the owner on the PC, with nothing typed on the program's side:

- `POST /api/pair` (on `--listen`, no session needed, with `X-Potluck-Request: 1`) `{"name": "Home Assistant",
  "right": "look"}` -> `202 {"pair_id": "<id>", "expires_in_s": 300}`. At most 4 pending requests; more are `429`.
- `GET /api/pair/<pair_id>` (no session needed either) -> `{"state": "pending"}`, then `{"state": "approved", "name",
  "right", "token"}` (the token is shown once), or `{"state": "denied"}`; `404` once it expired or was fetched. Pairing
  exists only on `--listen`: on 127.0.0.1 there is nothing to pair (no login at all). A second program asking under a
  name already paired gets that name with a number added (`Home Assistant 2`), so `hubauth revoke` can tell them apart.
- The owner sees each pending request in the model (`hub.pairing`: `[{"pair_id", "name", "right", "from", "at"}]`) and
  on the dashboard's hub card, and answers on the PC: `POST /api/pairing {"pair_id": "<id>", "approve": true|false}` --
  on 127.0.0.1 only, never on `--listen`.
- A paired program sends `Authorization: Bearer <token>` instead of a session; the token carries the right it was
  approved with (`look` or `write`; never deploy). Tokens are kept hashed (SHA-256) in the logins file and listed and
  removed with `python -m potluck.hubauth tokens` / `python -m potluck.hubauth revoke <name>`.

## The hub's own entities in Home Assistant (M17)

When the cluster file names a broker (`present`) the resident hub also publishes, by MQTT discovery and on the same
devices the cell's presenting role creates (identifiers `potluck_<cell>_<board>` and `potluck_<cell>_cell`), what only
it knows: each board's state (`alive`/`asleep`/`dead`), internal RAM free, uptime, signal strength and worst heartbeat
delivery as diagnostic sensors, and its firmware as an update entity (installed = the board's, latest = the newest in
the cell; no command topic, so Home Assistant installs nothing); and on the cell's device, apps working and a problem
binary sensor per app. They are available while the hub is (`potluck/<cell>/hub`, `online`/`offline`, its will
`offline`): with the PC off they go unavailable and the boards' resources stay live through the cell's own presenter.

## Integrations report their state, and each program has a share (M18)

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

A writable resource may name the programs that may write it, in the cluster file:
`"outside": {"node-7368/led/on": {"mode": "read_write", "writers": ["Home Assistant"]}}`. Through the hub, any other
program gets `403 {"ok": false, "result": "not writable by this program"}`; the owner on the PC (127.0.0.1) always
may. The cell's own presenter (M13) writes over MQTT, which the boards cannot tell from the hub's writes, so a writers
list is the hub's rule, not the boards'.

## The setup wizard (M20): /api/setup/*

The owner's actions on boards plugged into the PC, served **only on 127.0.0.1** (on `--listen` every one is 404) and,
like every POST there, only with `X-Potluck-Request` and a `Host` of 127.0.0.1, localhost or ::1. The page is
`setup.html` ("Add boards" on the dashboard). Board actions are jobs: a POST answers `202 {"ok": true, "job": <id>}`
and `GET /api/setup/jobs/<id>` follows it -- `{"id", "kind", "port", "state": "running" | "done" | "failed", "pct",
"result", "error", "lines"}`; one job at a time per port (else 409). `GET /api/setup/jobs` lists them.

| call | what |
|---|---|
| `GET /api/setup/cell` | `{"system", "boards": [{"node", "label", "room"}], "max_boards": 20, "vault": <keys sealed>}` |
| `GET /api/setup/ports` | USB serial ports, `[{"port", "vid", "pid", "chip", "description"}]`, without the hub's own |
| `GET /api/setup/build` | `{"version", "build"}`: the firmware most boards run and its build on this PC (409 if none) |
| `POST /api/setup/probe {"port"}` | job: chip, flash, PSRAM, MAC, and whether it can run Potluck -- **restarts the board** |
| `POST /api/setup/flash {"port", "build"}` | job: writes that build (only one under the hub's build cache; else 403) |
| `POST /api/setup/enrol {"port", "passphrase"?}` | job: a key signed by the cell's CA and a node id from its registry; `result.node` |
| `POST /api/setup/wifi {"port", "ssid", "password"}` | job: the house Wi-Fi into the board (kept only there; never logged) |
| `POST /api/setup/reset {"port"}` | job: restarts the board (a new id and a saved Wi-Fi take effect then) |
| `POST /api/setup/identify {"port", "seconds"?}` | job: the board's light blinks blue (default 20 s, at most 300) |
| `POST /api/setup/board {"node", "label"?, "room"?, "position"?}` | adds the board to the cluster file or sets its room (`""` clears it) or its `[x, y]` on the floor plan (`[]` clears it); the file's previous version is kept as `<file>.bak`, and a change `apply` would refuse is never written (409). With `{"plan": {...} | null}` instead, sets the floor plan |
| `POST /api/setup/apply` | job: `potluck apply <cluster file>`; its output in `lines`; done on VERIFIED, failed otherwise |

The hub's own serial port is refused (409). The passphrase unlocks the cell's sealed keys for that one enrolment and
they are locked again after it.
