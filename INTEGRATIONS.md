# Integrations: how another system uses the cell

An **integration** connects Potluck to another system -- Home Assistant, Node-RED, a metrics store, a program of your
own -- so that system sees the cell's resources (a board's sensor, an app's output) in its own terms. This page is the
interface every integration is built on, so that the next one is an adapter, not a design. It assumes you have read
the cell's [README](README.md); the hub's API, field by field, is [host/potluck/HUB-API.md](host/potluck/HUB-API.md).

## The five things every integration does

1. **Names the cell's resources in the other system's terms.** A board is a device; a resource is a sensor, a switch,
   a number -- whatever the other system calls them. Keep the cell's identities: the cell id (`ef76cc2e`, eight hex
   digits from its certificate authority), the board's node id (`7368`), the resource's path
   (`potluck://lab/node-7368/hw/die_temp`). They never change while the cell and the package stay the same.
2. **Carries a value's unit, age and quality across.** A value never travels alone: it has a **unit**, an **age**
   and a **quality** (`GOOD`, `STALE`, `UNAVAILABLE`, `NO_DATA`, `FAULTY` -- HUB-API.md, the model). Show the quality;
   when a resource is strict (an old value of it must not be shown) and not `GOOD`, there is no value to show
   (`null`), and the integration says "unavailable", never an old number.
3. **Shows what the owner exposes, writes only what the owner allows, and never deploys.** The cluster file's
   `outside` block says what the cell presents to other systems and what they may write (DEVELOPING-APPS.md). Over
   MQTT the cell presents only what `outside` names. The hub's model holds every resource the package names, and a
   paired program with the **look** right can read all of it: pairing is the owner's approval. By default an
   integration shows only the resources with `exposed: true`; offering the rest is an option the owner turns on
   knowingly (Node-RED's "also read resources the cell does not expose" is one). It writes only those with
   `outside_writable: true`; a write goes to the owning board, which checks again. There is no way to deploy through an
   integration: that is `python -m potluck.apply`, on the PC that holds the owner's keys.
4. **Is a module the owner chooses.** Nothing runs unless the owner turns it on: a program they start, a board module
   named in `firmware.modules`.
5. **Reports its own state to the hub**, so the dashboard shows whether it is connected (below).

## Two kinds

| | a program on the hub's API (hub side) | a board module (board side) |
|---|---|---|
| runs on | any computer that reaches the hub | the boards, in the one image |
| needs the PC | yes -- the hub runs on it | no |
| examples | the Home Assistant integration (`custom_components/potluck`), the Node-RED integration (`integrations/node-red`), the Node.js example program (`examples/programs/node`, a program rather than an integration: it skips duty 5) | the presenting role (`firmware/components/pot_present`): MQTT to Home Assistant with no PC |
| built with | HTTP and JSON, any language | C++ on ESP-IDF, the actor API (DEVELOPING-APPS.md) |

Prefer a program on the hub's API: it costs the boards nothing. Build a board module only when the integration must
work with no PC, and measure what it costs the tightest board in RAM, flash and radio time.

## A program on the hub's API, step by step

Every request below needs a running hub: HUB-API.md, "Starting a hub". Every POST, `/api/pair` included, must carry
the header `X-Potluck-Request: 1`, or the hub answers `403`.

**1. Find the hub.** On the hub's own PC: `http://127.0.0.1:8470/`, no login (whoever uses the PC is the owner).
Elsewhere, the hub serves the house network only when started with `--listen` (HTTPS and a login); it then announces
itself by multicast DNS as `_potluck._tcp.local.` with TXT `txtvers=1`, `cell=`, `system=`, `api=1`, `tls=1` and `fp=`
(the SHA-256 of its certificate).

**2. Get a token, with nothing typed.** `POST /api/pair {"name": "<your program>", "right": "look"}` (`"write"` only if
you write), then poll `GET /api/pair/<pair_id>` every 2 s while the owner clicks **Allow** on the hub's dashboard on
the PC. Send `Authorization: Bearer <token>` from then on. The hub signs its own certificate, so pin the `fp` you paired
with and refuse any other certificate. The announcement is not authenticated: to know `fp` is the hub's, compare it with
the hub card's **Certificate** on the PC's dashboard, and the owner should allow only a request whose asking address
(shown with it) is your program's. A person can log in instead (`POST /api/login`).

**3. Read.** `GET /api/model` is everything at once; `GET /api/stream` sends it again every pull period (2 s) and the
events as they happen. Use the stream: it counts once against your share, however long it stays open. Each resource:
`{path, name, owner, value, unit, quality, age_ms, latency_class, exposed, outside_writable, safety_relevant, type}`;
each board: `{node, label, state (alive | asleep | dead | unknown), since_at, ...}`. A board that is `dead` makes its
resources `UNAVAILABLE`; one `asleep` declared a long window and is expected back.

**4. Write** (only `outside_writable: true`): `POST /api/write {"path", "value"}`, a bool or a number -- the hub writes
a number as the resource's own `type` once it knows it. HUB-API.md lists every answer; show the `result` to the person
who asked. The cluster file may name which programs may write a resource through the hub (`"writers": [...]`, by the
name each paired with). That list governs the hub's API only, never writes over MQTT (below).

**5. Report your state** at least every 30 s, under the name you paired with: `POST /api/integrations {"name", "state":
"connected" | "disconnected" | "error" | "idle", "detail"}` -- `error` when you run but cannot do your job (a refused
login), `disconnected` when you cannot reach the hub or your own system, `idle` when there is nothing to do. Stop
reporting and the dashboard shows you `disconnected` after 60 s.

**6. Stay within your share.** Requests 20 a second, writes 2 a second, per program. Over it you get `429` with
`Retry-After`: wait that long. Only you are slowed; the cell and the other programs are not.

**7. Publish values of your own** (2026-10-20). A program can bring resources to the cell -- a phone's readings, a
desktop app's state -- through an **inbox**: a built-in actor, pinned to one board, that holds named values for the
whole cell. In a manifest the cluster file's package names:

```json
{"name": "phone", "module": "builtin:inbox", "latency_class": 4, "pin": "A",
 "config": {"outputs": "heart_rate:f32:hertz steps:u32 moving:bool", "stale_ms": 10000}}
```

and in the cluster file `"outside": {"act/phone/heart_rate": "read_write", ...}` for each value, with `"writers"` naming
your program. Write them with step 4's `POST /api/write {"path": "potluck://<system>/act/phone/heart_rate", "value":
72.5}`. Every board reads them like any actor's output; the hub, Home Assistant and the dashboard show them. Pin it to the
board on the PC's cable (one hop). Until you write a value it reads `NO_DATA`; write each more often than `stale_ms`, or
it ages into `STALE` -- that is how the cell knows your program stopped. Values are numbers or a bool of at most eight
bytes; at most 12 per inbox. Mind step 6: two writes a second in all, so a program with many values writes each less
often, and sets `stale_ms` to match (three values every 5 s is 0.6 writes a second; `stale_ms` 15000).

That is the whole interface: a new integration needs no change to the hub.

## The MQTT payload, for any consumer

When the cell presents itself (the cluster file's `present` block), the presenting board -- or the PC's node, with
`present.hub` -- publishes to the owner's MQTT broker: what the cluster file's `outside` names, nothing else. Home
Assistant reads it through MQTT discovery, but nothing in it is Home Assistant's alone. QoS 0, nothing retained;
`<cell>` is the cell id, `<hash>` a resource's path hash: 8 lowercase hex digits, the 32-bit FNV-1a hash of the full
path including its scheme and cell name (`potluck://lab/node-7368/hw/die_temp`, not `node-7368/hw/die_temp`), the same
as the hub uses. The broker is reached on port 1883, or 8883 with TLS when the cluster file gives `present.tls_ca` (the
broker's CA certificate, a PEM or DER file), unless `present.broker` names a port.

| topic | payload |
|---|---|
| `potluck/<cell>/<hash>/state` | `{"value": <number, true/false, or null>, "unit": "C", "quality": "GOOD", "age_ms": 1234, "latency_class": 4, "sampled_ms": <ms since 1970, when the presenter knows the time>}` -- every 2 s by default (`present.period_ms`) |
| `potluck/<cell>/presenter/<node>` | `{"online": 1, "node": "8160", "nodes": [<boards alive>], "down": [<hashes with no value to show>]}`, and the will `{"online": 0, "node": "8160", "nodes": [], "down": []}` when that presenter goes |
| `potluck/<cell>/<hash>/set` | a write, which the presenting board takes: `ON` / `OFF` for a bool, a decimal number otherwise -- only for what `outside` marks `read_write` and is not safety-relevant |
| `potluck/<cell>/event` | `{"what": "outside_write", "resource": "node-7368/led/on", "hash": "<hash>", "value": "<the payload as sent>", "result": ...}`; `result` is `applied` (the presenting board owns it), `sent to its owner` (passed on, marked from outside; the owner checks again), `refused by the owner`, or `refused: not writable from outside` / `not known on the presenting board` / `not a value of its type` / `its owner is not reachable` |
| `homeassistant/<component>/potluck_<cell>/<hash>/config` | Home Assistant's discovery config, with the cell id in every unique id |

The presenting board subscribes to `potluck/<cell>/+/set` and to `homeassistant/status`: when Home Assistant
announces `online` there (it restarted), the board sends every config again.

When the hub runs with a cluster file that has `present`, it publishes what only it knows on the same broker, also
QoS 0 and not retained (HUB-API.md, "The hub's own entities in Home Assistant"):

| topic | payload |
|---|---|
| `potluck/<cell>/hub` | `online`, and the will `offline` when the hub goes |
| `potluck/<cell>/hub/<node>/state` | `{"state": "alive", "internal_free": 152831, "up_s": 9540, "rssi": -34, "delivery_pct": 99.52}` per board, every 10 s; `null` where the hub does not know |
| `potluck/<cell>/hub/<node>/firmware` | `{"installed_version": "<the board's>", "latest_version": "<the newest in the cell>", "title": "Potluck firmware"}` |
| `potluck/<cell>/hub/cell/state` | `{"apps_working": 2, "problems": {"<app>": false, ...}, "attrs": {"apps_total", "not_working", "boards_alive", "boards_total"}}` |
| `homeassistant/sensor/...`, `homeassistant/update/...`, `homeassistant/binary_sensor/...` `/potluck_<cell>/hub_.../config` | the discovery configs for those, at each connect, when Home Assistant announces `online` on `homeassistant/status`, and every 5 minutes |

A consumer that is not Home Assistant subscribes to `potluck/<cell>/+/state` and the presenter topics: an entity is
available when some presenter is `online`, lists its board in `nodes`, and does not list its hash in `down`.

**Who can write.** The presenting board checks a write from the broker against `outside` (`read_write`, not
safety-relevant), and the owning board checks again; nothing more. The hub's `writers` lists do not apply here: the
boards never see them. So anyone who can publish on your broker can write every resource you made writable from
outside. Give the broker a login (the boards take one with `POT! present login <user> <password>`, HOME-ASSISTANT.md
step A.6) and TLS (`present.tls_ca`, port 8883), and make only what must be controlled `read_write`.

## A board module

A board-side integration is an optional firmware module, like `pot_present`: an ESP-IDF component, built in only when
the cluster file names it in `firmware.modules`, declared to `python -m potluck.apply` with what it `requires` and what
it costs, reporting its state in the board's status report (`active`, `idle` and why, `failed`; the hub shows it as
`board.modules`). It reads a resource as any board in the cell does -- a local copy, refreshed from the board that owns
it -- turns an outside write into a write marked from outside, authenticated like every frame between the cell's boards
and sent to the owning board, which checks it again -- only for what the package allows -- and runs as a portable actor if any board may carry it. DEVELOPING-APPS.md
documents the actor API; `firmware/components/pot_present` is the worked example. A board that lacks the actor type
behind a resource it presents asks a board that has it for the resource's declaration (its unit and type).

## Which integration next

Chosen as Home Assistant was: by published evidence of what people running systems like Potluck use, filtered by what
an ESP32-S3 can carry for any board-side part. That evidence put **Node-RED** first, and it is built
(`integrations/node-red`): on 13.0 % of Home Assistant installations that report add-ons and 17.3 % of ioBroker
installations, used for home automation and, by its own survey, professionally by about half its users -- and a
program on the hub's API, with nothing on the boards. Metrics (InfluxDB, Grafana) come next; Matter does not fit a
board (Espressif's own light example takes about 1.5 MB of flash and 195 KB of RAM on an ESP32-C3).
