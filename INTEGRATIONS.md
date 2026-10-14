# Integrations: how another system uses the cell (M18)

An **integration** connects Potluck to another system -- Home Assistant, Node-RED, a metrics store, a program of your
own -- so that system sees the cell's resources (a board's sensor, an app's output) in its own terms. This page is the
interface every integration is built on, so that the next one is an adapter, not a design. It assumes you have read
the cell's [README](README.md); the hub's API, field by field, is [host/potluck/HUB-API.md](host/potluck/HUB-API.md).

## The five things every integration does

1. **Names the cell's resources in the other system's terms.** A board is a device; a resource is a sensor, a switch,
   a number -- whatever the other system calls them. Keep the cell's identities: the cell id (`ef76cc2e`, eight hex
   digits from its certificate authority), the board's node id (`7368`), the resource's path
   (`potluck://lab/node-7368/hw/die_temp`). They never change while the cell and the package stay the same.
2. **Carries the tuple across.** A value never travels alone: it has a **unit**, an **age** and a **quality** (`GOOD`,
   `STALE`, `UNAVAILABLE`, `NO_DATA`, `FAULTY` -- HUB-API.md, the model). Show the quality; when a resource is strict and
   not `GOOD`, there is no value to show (`null`), and the integration says "unavailable", never an old number.
3. **Reads only what the package exposes, writes only what it allows, and never deploys.** The cluster file's `outside`
   block says what systems outside the cell may read and write (DEVELOPING-APPS.md); nothing else reaches an integration.
   A write goes to the owning board, which checks again. There is no way to deploy through an integration: that is
   `potluck apply`, on the PC that holds the owner's keys.
4. **Is a module the owner chooses.** Nothing runs unless the owner turns it on: a program they start, a board module
   named in `firmware.modules`.
5. **Reports its own state to the hub**, so the dashboard shows whether it is connected (below).

## Two kinds

| | a program on the hub's API (hub side) | a board module (board side) |
|---|---|---|
| runs on | any computer that reaches the hub | the boards, in the one image |
| needs the PC | yes -- the hub runs on it | no |
| examples | the Home Assistant integration (`custom_components/potluck`), the Node.js example (`examples/programs/node`) | the presenting role (`pot_present`, M13): MQTT to Home Assistant with no PC |
| built with | HTTP and JSON, any language | C++ on ESP-IDF, the actor API (DEVELOPING-APPS.md) |

Prefer a program on the hub's API: it costs the boards nothing. Build a board module only when the integration must
work with no PC, and measure what it costs the tightest board, as M13 did.

## A program on the hub's API, step by step

**1. Find the hub.** On the hub's own PC: `http://127.0.0.1:8470/`, no login (that is the PC's owner). Elsewhere, the
hub serves the house network only when started with `--listen` (HTTPS and a login); it then announces itself by
multicast DNS as `_potluck._tcp.local.` with TXT `cell=`, `system=`, `api=1`, `fp=` (the SHA-256 of its certificate).

**2. Get a token, with nothing typed.** `POST /api/pair {"name": "<your program>", "right": "look"}` (`"write"` only if
you write), then poll `GET /api/pair/<pair_id>` every 2 s while the owner clicks **Allow** on the hub's dashboard on
the PC. Send `Authorization: Bearer <token>` from then on, and `X-Potluck-Request: 1` on every POST. Trust the hub's
certificate by the `fp` it announced (it signs its own). A person can log in instead (`POST /api/login`).

**3. Read.** `GET /api/model` is everything at once; `GET /api/stream` sends it again every pull period (2 s) and the
events as they happen. Use the stream: it counts once against your share, however long it stays open. Each resource:
`{path, name, owner, value, unit, quality, age_ms, latency_class, exposed, outside_writable, safety_relevant}`; each
board: `{node, label, state (alive | asleep | dead | unknown), since_at, ...}`. A board that is `dead` makes its
resources `UNAVAILABLE`; one `asleep` declared a long window and is expected back.

**4. Write** (only `outside_writable: true`): `POST /api/write {"path", "value"}`, a bool or a number. HUB-API.md lists
every answer; show the `result` to the person who asked. The cluster file may name which programs may write a resource
(`"writers": [...]`) -- by the name you paired with.

**5. Report your state** at least every 30 s: `POST /api/integrations {"name", "state": "connected" | "disconnected" |
"error" | "idle", "detail"}`. Stop reporting and the dashboard shows you `disconnected` after 60 s.

**6. Stay within your share.** Requests 20 a second, writes 2 a second, per program. Over it you get `429` with
`Retry-After`: wait that long. Only you are slowed; the cell and the other programs are not.

That is the whole interface: the hub needs no change for a new integration (M18's test).

## The MQTT payload, for any consumer

When the cell presents itself (M13, the cluster file's `present` block), the presenting board -- or the PC's node, with
`present.hub` -- publishes to the owner's MQTT broker. Home Assistant reads it through MQTT discovery, but nothing in it
is Home Assistant's alone. QoS 0, nothing retained; `<cell>` is the cell id, `<hash>` a resource's path hash
(8 lowercase hex digits, FNV-1a of its path, the same as the hub's).

| topic | payload |
|---|---|
| `potluck/<cell>/<hash>/state` | `{"value": <number, true/false, or null>, "unit": "C", "quality": "GOOD", "age_ms": 1234, "latency_class": 4, "sampled_ms": <ms since 1970, when the presenter knows the time>}` -- every 2 s by default (`present.period_ms`) |
| `potluck/<cell>/presenter/<node>` | `{"online": 1, "node": "8160", "nodes": [<boards alive>], "down": [<hashes with no value to show>]}`, and the will `{"online": 0, ...}` when that presenter goes |
| `potluck/<cell>/<hash>/set` | a write: `ON` / `OFF` for a bool, a decimal integer or number otherwise -- only for what the package lets the outside write |
| `potluck/<cell>/event` | `{"what": "outside_write", "resource": "node-7368/led/on", "hash": "<hash>", "value": "<the payload as sent>", "result": "applied" | "sent to its owner" | "refused: ..."}` |
| `homeassistant/<component>/potluck_<cell>/<hash>/config` | Home Assistant's discovery config, with the cell id in every unique id |

A consumer that is not Home Assistant subscribes to `potluck/<cell>/+/state` and the presenter topics: an entity is
available when some presenter is `online`, lists its board in `nodes`, and does not list its hash in `down`.

## A board module

A board-side integration is an optional firmware module, like `pot_present`: an ESP-IDF component, built in only when
the cluster file names it in `firmware.modules`, declared to `potluck apply` with what it `requires` and what it costs,
reporting its state in NODE_STATUS (`active`, `idle` and why, `failed`). It reads through the cell as any reader does
(a replica, refreshed by READ from its owner), turns an outside write into the cell's authenticated WRITE marked from
outside -- only for what the package allows -- and runs as a portable actor if any board may carry it. DEVELOPING-APPS.md
documents the actor API; `firmware/components/pot_present` is the worked example. If a board lacks an actor type whose
output it presents, it learns the entry's declaration from a board that has it (READ's describe flag, M14).

## Which integration next

Chosen as Home Assistant was: by published evidence of what people running systems like Potluck use, filtered by what
an ESP32-S3 can carry for any board-side part. On 2026-10-14 that evidence (the ledger) put **Node-RED** first: on 13.0 %
of Home Assistant installations that report add-ons and 17.3 % of ioBroker installations, used for home automation
and, by its own survey, professionally by about half its users -- and a program on the hub's API, with nothing on the
boards. Metrics (InfluxDB, Grafana) come next; Matter does not fit a board (Espressif's own light example takes about
1.5 MB of flash and 195 KB of RAM on an ESP32-C3).
