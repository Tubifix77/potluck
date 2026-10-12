# The hub's API, version 1 (M14, M16) -- a contract, written before either side is built

`potluck-hub` (M14) holds the cable to the cell and keeps **one model** of it. Everything else reads that model through
this API: the dashboard (M16), Home Assistant's integration (M17), the owner's own programs (M18). It is served on the
PC, by default on `127.0.0.1:8470` only; reaching it from the house network or a phone is a switch the owner turns on,
behind a login, encrypted (ARCHITECTURE section 13, M16 -- not in version 1's mock).

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
    {"at": 1791771600000, "kind": "package", "counter": 47, "result": "verified", "detail": "live on 3 board(s)"},
    {"at": 1791771300000, "kind": "firmware", "counter": 20, "result": "confirmed", "detail": "3 board(s)"}
  ],
  "signals": {"latency_p50_ms": 5, "latency_p99_ms": 16, "traffic_frames_s": 62.0, "errors_min": 0.4,
              "saturation": {"peers": 0.15, "namespace": 0.31, "actor_slots": 0.38, "internal_ram": 0.24,
                             "airtime": 0.26}}
}
```

- `board.state`: `alive` | `dead` | `asleep` (a declared long window, M5.1) | `unknown` (never heard since the hub started).
- `board.heap`, `board.links`, `board.rssi` and `board.telemetry` come from M15's telemetry, pulled over the radio every
  `hub.telemetry_period_ms` (`--telemetry-ms`, default 10000; 0 never): `heap` adds `psram_min`; each link adds `lost`
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
- `integration.state`: `connected` | `disconnected` | `idle` | `off`.
- `deploy.result`: `verified` | `confirmed` | `reverted` | `refused` | `failed` | `running`.

## GET /api/events?since=<seq>

Events in order, on the hub's clock, with a sequence number for paging. `since` omitted: the last 200.

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
outside writes (`outside_write`, with resource, value and result).

## GET /api/stream

Server-sent events (`text/event-stream`): `event: model` with the full model once per pull period, `event: events` with
new events as they arrive, in the same shape as `GET /api/events` (`{api, next, events}`; at most the last 200 on connect).
A write to a path that is not writable from outside, or unknown, is `403`; malformed, `400`; an owning board that does not
answer, `504`. A dashboard needs nothing else; ten open streams cost the cell no more radio than one,
since every viewer is served from the hub's one pull (M15).

## POST /api/write

`{"path": "potluck://lab/node-7368/led/on", "value": true}` -> `{"ok": true, "result": "applied"}`, or `403` with
`{"ok": false, "result": "not writable from outside"}`. Only resources with `outside_writable: true`; a login with the
look right only gets `403`. The write reaches the owning board marked as from outside, which checks the package again.

## GET /api/replay?from=<at>&to=<at> (M16, later)

The model as it was, from the hub's recording (section 7.6). Not in version 1's mock.
