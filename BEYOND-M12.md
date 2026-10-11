# Beyond M12: the hub, the dashboard and integrations

**A discussion draft (started 2026-10-11), not a decision.** Nothing here is part of the architecture until it
is agreed and moved into [ARCHITECTURE.md](ARCHITECTURE.md) section 13 as milestones with accept and kill criteria,
the way every milestone so far got there. Until then, ARCHITECTURE.md stays the single source of truth, and where
this draft and it disagree, it wins.

## 1. The idea

From the owner, after M12: two next steps that belong together -- **integrations** and **a dashboard** -- with the
dashboard as a *middleman* between two worlds:

- **Potluck underneath:** the resources the cell has, which are in use and by whom, which are shared, how many slots
  are still free; the reports it sends; how many ESP32-S3 boards and how many PCs it holds.
- **What runs on top of it:** which apps run (on the boards, and on the PCs), which PC programs talk to Potluck and
  what they read and write, where each app's logs go, and other systems only half inside it -- a Home Assistant
  installation, say, that the PC brings in as a kind of member under its control.

It will show a lot -- more than a few tabs -- and *everything that crosses between the PC world and Potluck should go
through it*, so that it knows.

**The owner's three needs (2026-10-11), and nothing more is the owner's:** *monitor everything that happens in
Potluck*; *from more than a PC*; and *Home Assistant*, the integration most in demand now. What to show, what matters
most and which other integrations to build are settled below from evidence (sections 5 and 6, each claim cited and in
the project's ledger), not from taste.

**Decided by the owner (2026-10-11):**

- **The dashboard is reachable from a phone**, not only on the PC.
- **Home Assistant reads and writes.**
- **The Home Assistant integration is a first-class, open part of Potluck**, built to be excellent, because it is
  likely where people on the internet find Potluck useful: someone who already runs Home Assistant can add a cluster of
  cheap boards to it. Section 6 is written to that standard.
- **Integrations are modules you choose (the owner, 2026-10-11):** like passive-sensor and poor-mans-extender, each
  integration a board would carry is an optional component, built into a cell's firmware only when its cluster file
  names it, because an ESP32-S3's flash and RAM are limited and not every cell wants every integration (section 6).
- **What "integration" means (the owner, 2026-10-11):** *Potluck's resources show up in Home Assistant* -- a board's
  LED, a sensor, the room sensor's presence -- as ordinary devices of the house's IoT network, whether the cell reaches
  the house through the PC that runs the hub or through a board that is already on the house Wi-Fi (an extender).
  Home Assistant does not run on Potluck, and Potluck does not present Home Assistant: the direction that matters is
  Potluck into Home Assistant, read and written there.

## 2. What is already there, and what was planned

The architecture planned for most of this from the start (all in ARCHITECTURE.md):

- **Section 7.1, the host's four daemons** (from Vision Document 2): `potluck-bridge` (the only program that touches
  the serial link, forwarding frames unchanged and recording them), `potluck-agent` (the PC as a member, offering
  named services), `potctl / pot-dashboard` ("`kubectl` for the mesh: topology, live namespace, logs, deploy") and
  `pot-app` (user programs, with the same read contract as on-node actors). "Nothing above `potluck-bridge` may open
  a serial port", and the agent is "the *only* place a second schema exists".
- **ADR-004:** one frame format everywhere, no translation in the bridge. Its revisit trigger names this case: "a
  third-party host integration genuinely requires a typed IDL below the agent."
- **Section 7.5 / M8:** named services a PC offers the boards -- integration in one direction, built and accepted.
- **Section 8.4:** a board may report to a cloud service, outbound only: "an outbound cloud path is an inbound
  attack surface."
- **Section 5 / 7.6:** "byte-identical replay from radio to dashboard" -- a dashboard was meant to show recorded
  sessions as well as live ones.

What was built instead of the four daemons: one Python tool per job, each opening the serial port itself --
`potctl`, `potluck.fw`, `potluck.apply`, `potluck.agent`, `potluck.hostnode`, the live table. **Only one of them can
hold the port at a time** (M0-RUNBOOK section 14): today the PC can be a placement node, or offer a service, or be
watched, but not all at once. That is the strongest reason for this whole step: the planned bridge-and-agent split
has become necessary, not just tidy.

## 3. The shape: one hub, many clients

```
  boards ─radio─ cabled board ─serial─ ┌─────────────── potluck-hub (one per PC) ───────────────┐
                                       │ the link: owns the port, forwards frames, records them  │
                                       │ the model: cell, resources, apps, clients, events       │
                                       │ the API: read, subscribe, write (by rule), deploy, log  │
                                       └───┬───────────┬───────────┬───────────┬───────────┬─────┘
                                           │           │           │           │           │
                                      dashboard     potctl /    PC apps     Home        exporters
                                      (browser)     apply / fw  (pot-app)   Assistant   (later)
                                                                            adapter
```

- **The hub** is sections 7.1's bridge and agent as one service on the PC: the only program on the serial port, the
  PC's identity in the cell (enrolled like a board, as `pot_hostnode` is), and the place that *knows*: every board's
  inventory, every resource and who owns it, every actor and where it runs, every client and what it uses. Every
  other program -- the existing tools included -- becomes its client.
- **The dashboard** is a client like any other, with the most to show. It is a view of the hub's model, so it can be
  closed, reloaded or opened twice without anything in the cell noticing.
- **Integrations** are clients too, each an adapter between the hub's API and another system's: Home Assistant, a
  PC program, later an exporter for someone else's charts.

So "everything goes through it" is true of the **hub**, not of the dashboard window: whatever crosses between the PC
world and the cell crosses the hub, which is how the dashboard can show all of it.

### What must not change

These are the existing rules this step lives inside; a design that breaks one is a design to change.

1. **The cell runs without the hub.** Boards place, fail over, keep their package and their checkpoints with no PC
   at all (M3, M6, M8, M12). The hub observes and brokers; it is never in the loop that keeps the cell alive. A hub
   crash, a PC asleep, a dashboard closed: the cell carries on, and the hub catches up when it comes back.
2. **Every value keeps its tuple.** What the hub shows or passes on is section 4's reading -- value, unit, age,
   quality -- never a bare number. A STALE value is shown stale; a Home Assistant entity fed from Potluck says when
   it is unavailable.
3. **Only the owner's signed package changes what the boards do.** No client, no integration, no dashboard button
   deploys anything the owner did not sign. A deploy from the dashboard is `potluck apply` with the owner's keys, on
   the owner's PC.
4. **Writes only where the package allows them.** A resource an outside system may write is declared writable in the
   package (it already has `access`), and the hub refuses the rest. The safety line holds: Potluck is never the only
   thing between a motor and a person (section 12).
5. **Boards stay outbound-only** (section 8.4). The dashboard is served by the hub on the PC, never by a board.
6. **Credentials are never the hub's to hold or show** -- Wi-Fi passwords, signing keys, Home Assistant tokens are
   typed by the owner into files the hub reads, and never displayed or logged.

## 4. What the hub has to know, and where it comes from

| what | where it comes from today | gap |
|---|---|---|
| boards and PCs in the cell, alive or dead | the cabled board's peer table, HELLO/heartbeats on the frame link | the hub sees only the cabled board's view; other boards' peer tables are on their consoles |
| each board's firmware, package, roles, actors, counters | `NODE_STATUS` (M12), proxied to every board | none: built |
| which modules each board's image carries, and each one's state (active, idle, failed) | -- | new: the list in `NODE_STATUS`, and a state per module |
| links: signal strength, delivery, round trips | each board's `link`/`peers` stats lines | on the consoles only; a board on a charger has none |
| resources: namespace entries used of 128, actor slots of 16/8/8, RAM headroom, cores lent, NVS | partly `sys/*` resources (heap, uptime, peers), the rest stats lines | most on the consoles only |
| every resource's value and quality | READ through the cabled board | none, but reads cost radio time: the hub must cache and pace them |
| where each portable actor runs, and its moves | the reconciler's claims (radio), `actor_owner` events on consoles | events on consoles only |
| apps (packages, fragments, apps' repositories at which commit) | the cluster file and `apply`'s cache | none: on the PC already |
| PC programs connected, what they read and write | -- | new: the hub's own client registry |
| logs, and where they go | each board's console; each tool's own output | new: a log path the hub owns, and a record of where each app's logs go |
| reports sent | -- (section 8.4's reporting is not built) | new |
| Home Assistant's entities | -- | new: the adapter (section 6) |

The largest gap is the first column's *consoles*: most of what a board knows about itself it prints to its USB console,
which no deployed board has. So the hub needs **telemetry over the cell**: a board's statistics, peer view, actor
stats and recent events reaching the hub over the radio, the way `NODE_STATUS` already does for the inventory. That
has a cost the radio must be able to carry: today's console output is up to ~3 KB a board every statistics period
(`firmware/main/m0_main.cpp`), and three boards' heartbeats already share a channel in a 2.4 GHz-noisy house. A
pull model (the hub asks, at a rate it is given, for the sections it shows) fits better than a push of everything;
its budget is a **[MEASURE]**: what telemetry rate leaves heartbeat round trips and delivery where M0 measured them.

## 5. The dashboard: what to show, settled

Two field methods decide the order, both chosen because they are the standard ones for exactly this question:

- **Symptoms first.** Google's SRE book: "Your monitoring system should address two questions: what's broken, and
  why?", and "it's better to spend much more effort on catching symptoms than causes"; "Dashboards should answer basic
  questions about your service, and normally include some form of the four golden signals" -- latency, traffic,
  errors, saturation ([SRE book, Monitoring Distributed Systems](https://sre.google/sre-book/monitoring-distributed-systems/)).
- **Every resource, three questions.** The USE method: "For every resource, check utilization, saturation, and
  errors" ([USE method](https://www.brendangregg.com/usemethod.html)).

So the dashboard is three layers, each a click deeper than the last.

### Layer 1: the first screen -- is anything broken? (symptoms)

One screen, readable on a phone, that answers "is my cell all right?" before anything else:

- **Every app: working or not**, judged the way `potluck apply` verifies (M12): its actors running where the package
  says, each output GOOD. An output STALE, FAULTY or UNAVAILABLE is the headline, with how long it has been so.
- **Boards and PCs: alive N of M**, and which is dead, since when.
- **The last change**: the last deploy or firmware update, its result, and any board still on trial or reverted.
- **The PC's own link**: the hub's connection to its cabled board.

These are the failure contract's events, which is the point: the README lists what can go wrong in Potluck, so that is
what the first screen watches.

### Layer 2: the cell's four golden signals

| signal | in Potluck | today's source |
|---|---|---|
| latency | heartbeat round trips per link (p50, p99); a read from the hub to each board; deploy and failover times | `link` stats; the hub's own timing; events |
| traffic | frames per second per board and per link; reads and writes per client; actor publications | `link` stats; the hub |
| errors | delivery loss per link; deaths and revivals; refused frames (authentication, downgrade); actor and guest faults; refused or reverted deploys; NVS write errors | `link`, `auth`, `rec` stats; events; `apply` |
| saturation | radio queue and refused sends (B's NO_MEM stall was this); namespace entries; actor slots; internal RAM; cores lent (M9) | stats lines |

### Layer 3: every resource -- utilization, saturation, errors

| resource | utilization | saturation | errors |
|---|---|---|---|
| the radio link (each pair) | frames per second | send queue, refused sends | delivery loss, deaths |
| the peer table | peers used of 20 (ESP-NOW's ceiling) | a refused peer | admission refusals |
| the namespace (each board) | entries used of 128 | a refused declare | `declare_err` |
| actor slots | of 16 per package, 8 pinned per board, 8 portable per cell | a refused package | actors dropped at start |
| internal RAM (each board) | static use of the 64 KB cap (60.1 KB on the full image) and free heap | largest free block | allocation failures |
| PSRAM | free | -- | -- |
| cores (M9) | units run here and lent | lenders refusing (busy) | units lost and rerun |
| flash (NVS) | entries used of 630 (M12: 107-113 on the bench) | -- | write errors |
| the serial link to the PC | frames per second | the hub's queue | CRC and framing errors |
| the hub's PC | CPU, memory | -- | client errors |

### Layer 4: the drill-down tabs

The tabs of the first draft remain the drill-downs -- **Cell** (topology, roles, channel, and every board's modules:
on and active, on and idle, or not on the board), **Resources** (layer 3
in full), **Apps** (packages, actors, where each runs, moves, checkpoints, guests), **Data** (the namespace browser with
the tuple and history), **Clients and integrations**, **Deploys**, **Logs and events** (on one clock, from every
board), **Replay** -- and every number on layers 1-3 links to the tab that explains it ("what's broken" first, then
"why").

### The ESP32-S3 as a filter

What the boards can afford decides what the dashboard may ask of them (the owner's filter, 2026-10-11):

- **Internal RAM is nearly spent:** 60.1 KB of the 64 KB cap on the full image (STATUS.md). Telemetry on a board may
  add almost nothing in internal RAM: it reuses the counters the stats lines already keep, and buffers it needs go to
  PSRAM.
- **The radio is shared with the heartbeat**, on one channel, in a 2.4 GHz-crowded house; a v1 ESP-NOW frame carries
  226 B of payload (section 5.3). So telemetry is **pulled by the hub, in small pieces, at a rate it is given** -- and
  viewers never multiply it: ten phones on the dashboard cost the cell what one hub costs, because the hub caches and
  serves them all. The rate is a **[MEASURE]** (section 4).
- **The boards are not Wi-Fi servers.** They run ESP-NOW, are outbound-only (section 8.4), and only an extender holds a
  Wi-Fi association. So unlike ESPHome, where "each ESP is its own server" for Home Assistant
  ([ESPHome native API](https://esphome.io/components/api/)), no board serves a page or an API: the hub speaks for the
  cell, or a board on the house Wi-Fi does as an outbound MQTT client (section 6) -- one board for the whole cell,
  never each board for itself.
- **No 802.15.4 radio on the S3** (ESP-IDF names the ESP32-H2, C6 and C5 as the chips with one; section 3): Thread,
  Zigbee and Matter-over-Thread are not board protocols in this edition. Section 6 reaches those ecosystems another way.

### On a phone too (decided)

The dashboard is a web page the hub serves -- any browser, nothing to install, Linux and Windows alike -- laid out for a
phone's width as well as a desk's. A phone reaches the cell three ways:

- **In Home Assistant's own app**, for Home Assistant users -- the commonest case, since 86.2 % of the installations that
  share their integrations use Home Assistant's phone app (section 6). Once Potluck's resources are Home Assistant devices
  (section 6), they are on the phone like any other device: their values, their availability, their controls, in the
  user's own Home Assistant dashboards. That covers layer 1 for most people without a page of Potluck's own.
- **The hub's page, inside Home Assistant**, for the deeper layers: when the hub runs on the Home Assistant machine
  itself (the serial adapter plugged into it), it can be packaged as a Home Assistant app and shown through Ingress,
  which "allows users to access the app web interface via the Home Assistant UI", with "authentication ... handled by
  Home Assistant" and no port forwarding ([Ingress](https://developers.home-assistant.io/docs/add-ons/presentation/));
  such an app can be given the serial device (`uart: true`, or a `devices` path;
  [app configuration](https://developers.home-assistant.io/docs/add-ons/configuration/)).
- **The hub's page on its own**, for everyone else: served on the house network behind a login the owner sets (rule 6:
  never shown, stored only as a hash), with an encrypted connection. Reaching it from outside the house is left to tools
  made for that (a VPN, Home Assistant's own remote access) -- the hub never opens a port to the internet itself.

## 6. Integrations

### PC programs (`pot-app`)

A PC program talks to the hub, not to the serial port: it registers with a name, reads and subscribes to resources,
writes where the package allows, offers named services (M8's, moved behind the hub), and can be a placement host
(M10's, moved behind the hub). The hub knows every client by name, so the dashboard can say which program reads what.
Section 7.1 named gRPC for this API; whether it is gRPC, a local WebSocket with JSON, or both, is open.

### Home Assistant: Potluck's resources as the house's devices

The bar (owner, 2026-10-11): someone who runs Home Assistant adds Potluck, and the cell's resources appear there --
read and written like any other devices of the house -- with nothing about Potluck's internals to learn. Open: the code
is in a public repository under Potluck's license, its protocol is documented, and nothing in it is reserved for the
maintainer's cell.

**How the cell reaches the house -- two ways in, the same devices:**

- **Through the PC (the hub).** The cabled board's PC runs the hub, and the hub presents the whole cell to Home
  Assistant over the house network: as a full integration, or by MQTT discovery (below). The PC may be any PC on the
  house network -- including, but not necessarily, the machine Home Assistant itself runs on.
- **Through a board on the house Wi-Fi (no PC).** An extender board is already a station on the house Wi-Fi (M6.1),
  and every board's resources reach it over the cell. So that board can present the whole cell to Home Assistant by
  itself, as an **MQTT client**: it connects out to the house's MQTT broker -- the one half of Home Assistant's users
  already run -- and publishes each resource's discovery config, its state and its availability, and receives writes
  on the resources the package declares writable. This keeps section 8.4's rule that a board holds no inbound listener:
  the connection is the board's own, outbound, to a broker the owner names. With it the house sees the cell with the PC
  switched off. Its costs are bench questions: the MQTT client is a managed ESP-IDF component in v6.0.2
  (`espressif/mqtt`), its RAM must fit an extender build whose internal RAM is nearly spent (PSRAM where it can), and
  the broker's password is the owner's to type, like the Wi-Fi's (rule 6) -- **[MEASURE]** before it is designed further.

Both routes show the **same devices with the same names**, so a cell can move from one to the other (the PC switched
off, the extender taking over) without Home Assistant's automations noticing more than a moment of "unavailable".

**What a user sees:**

- **Found, not configured:** the hub announces itself on the house network (mDNS/zeroconf), and Home Assistant offers
  to set it up -- an integration's discovery step "is invoked" when it is discovered, and setup happens in Home
  Assistant's UI through a config flow ([config flows](https://developers.home-assistant.io/docs/config_entries_config_flow_handler/)).
  Through MQTT, the devices simply appear once the broker has their discovery configs.
- **Devices and entities:** each board is a device (its node id, firmware, role, the board it is cabled through); each
  resource is an entity of the right kind -- presence a binary sensor, a temperature a sensor with its unit, the status
  LED a light, a writable setpoint a number or a switch -- named from the package's paths and labels, unavailable
  whenever the reading is not GOOD (stale values are not passed off as current: rule 2). The cell's own health --
  boards alive, the link to the house, the last deploy's result -- are entities too, so a Home Assistant automation can
  say "the room sensor is down".
- **Read and write (decided):**
  - *Home Assistant reads Potluck*: every resource the package names, live, with its age as an attribute.
  - *Home Assistant writes Potluck*: any resource the package declares writable (`access: write`) -- the LED's colour,
    a setpoint -- from an automation, a dashboard card or a voice command; the hub or the gateway board checks the rule
    (rule 4) and the owning board checks it again, as every write is checked today.
  - *Never*: deploying to the boards. A package is the owner's, signed with the owner's keys (rule 3); Home Assistant
    may show `apply`'s plan and result, nothing more.
  - *And optionally the other way*: a few Home Assistant entities the owner lists (a door contact, the electricity
    price) can be offered to board actors as resources the PC owns, `potluck://<system>/node-00fe/ha/<entity>`, read
    with an age and a quality and UNAVAILABLE while Home Assistant is gone; and a board actor may ask the hub to call a
    Home Assistant service the owner lists, as M8's named services are listed today
    ([WebSocket API](https://developers.home-assistant.io/docs/api/websocket/)). This is a convenience, not the point
    of the integration.
- **Installed the usual ways:** the integration through HACS, "a custom integration that provides a UI to manage
  custom elements" ([HACS](https://hacs.xyz/)), until it is good enough to offer to Home Assistant itself; the hub as a
  service on any PC, or packaged as a Home Assistant app for people whose PC *is* their Home Assistant machine; or no
  install at all, through MQTT.

**Two protocols, both kept open.** A full integration (the above, from the hub) is the excellent path. **MQTT
discovery** is the no-install path for people who already run a broker, and the only one a board can speak by itself: the hub publishes each resource's config on
`<discovery_prefix>/<component>/[<node_id>/]<object_id>/config` (default prefix `homeassistant`) and its availability by
birth and will messages ([Home Assistant MQTT](https://www.home-assistant.io/integrations/mqtt/)), and any MQTT tool,
not only Home Assistant, can read the cell. Both are hub clients over the same API, so neither knows more than the other.

**What Home Assistant is to the cell.** Not a node: it does not heartbeat or run actors, and the cell does not need it
-- Home Assistant is where the house looks at the cell and acts on it. The few Home Assistant entities offered back to
actors are resources the PC owns, so they carry the PC's death window (a reader sees UNAVAILABLE while the PC or Home Assistant is gone, for as long as
it is gone). An actor that needs a value in a deadline (latency classes L0-L2) cannot bind a
Home Assistant entity: the locality contract (section 4) refuses it at build time, as it refuses any binding the
transport cannot meet. That is the owner's "half-integrated member under the PC's control", stated as a contract.

### Integrations are modules, chosen per cell (decided)

The same way apps are (M8.2, M12): an integration that runs on a board is an **external component** with its own
descriptor, in its own repository or in Potluck's, and a cell's cluster file names it under `apps` -- or leaves it out.
`potluck apply` builds into the cell's one image exactly the components its cluster file names, so a cell that does
not want MQTT carries none of it. M12's "one image, roles at run time" still holds: one image per *cell*, the image
being what that cell chose; within it, a module's role runs only where it applies (the MQTT gateway only on a board
that holds Wi-Fi credentials and a broker address).

| integration | where it runs | how it is chosen |
|---|---|---|
| the MQTT gateway (a board presenting the cell to the house's broker) | one board on the house Wi-Fi | a board module, named in the cluster file; it **requires** the extender's Wi-Fi station |
| the Home Assistant integration | Home Assistant, talking to the hub | installed in Home Assistant (HACS); nothing on the boards |
| MQTT discovery from the PC | the hub | a hub plugin, switched on in the hub's configuration |
| the hub's API for PC programs | the hub | always there: Potluck's own tools use it |
| later ones (exporters, other ecosystems' bridges) | the hub, or Home Assistant | hub plugins; on the boards only if one cannot be done from the PC |

Two rules follow from the S3's limits:

- **A board module only when it must be on the board.** Anything the PC or Home Assistant can do -- most of an
  integration -- is a hub plugin, where space is not short. The board side is the part that cannot move: the radio,
  the pins, or (for the MQTT gateway) the house Wi-Fi connection when no PC is on.
- **Its cost is known before it is chosen.** A module's descriptor says what it needs (`requires`: the extender's
  Wi-Fi station, a pin, PSRAM), and `potluck apply --plan` shows what the cell's image costs with it -- flash in the 2 MB
  update slot, static internal RAM against the 64 KB cap (60.1 KB with the sandbox, the room sensor and the extender) --
  and refuses a choice that does not fit, before any board changes, as it refuses everything else (M12). The build
  already measures both (`tools/build_firmware.ps1`'s budget check); the plan has to say it per module.

**The dashboard shows every module on every board (the owner, 2026-10-11):** for each board, each module is one of

- **on, and active** -- built into the board's image and running its role there (the MQTT gateway connected to the
  broker, the extender serving its hotspot);
- **on, idle** -- built in, with nothing to do on this board (the gateway on a board with no Wi-Fi credentials), and
  why;
- **not on the board** -- not in this image; choosing it means a new firmware, which `apply` rolls out.

The board says this itself, so the dashboard shows what is running rather than what was meant to be: its inventory
(`NODE_STATUS`, M12) grows a list of the components built into its image -- today it reports only roles and running
actors -- and each module reports its own state (active, idle and why, or failed). The hub cross-checks that against
the build it made for the cell (the cluster file and the cache record which components went into which image), and a
board whose image does not match is shown as such. Hub plugins are shown the same way on the hub's own row.

### Which integrations, settled

The evidence is Home Assistant's own published usage data: 698,769 active installations, 553,881 of which share which
integrations they use (opt-in, and by the site's own note likely under a quarter of all users;
[analytics.home-assistant.io](https://analytics.home-assistant.io/), data of 2026-10-11). Among those sharing:

| integration | share of installations | what it says for Potluck |
|---|---|---|
| Home Assistant's phone app (`mobile_app`) | 86.2 % | the phone is where Home Assistant users already look: through Home Assistant, "from more than a PC" comes almost for free |
| MQTT | 48.6 % | half already run the message protocol MQTT discovery needs |
| Matter / Thread | 40.9 % / 37.7 % | large, but not board protocols on the S3 (section 5's filter) |
| ESPHome | 27.3 % | the closest neighbour: ESP boards in Home Assistant are common, and its users are Potluck's likeliest audience |
| HomeKit bridge (Home Assistant to Apple) | 16.9 % | Home Assistant already re-exports its entities to other ecosystems |
| InfluxDB / Prometheus (time-series export) | 5.1 % / 0.7 % | few export metrics; Home Assistant's own history covers most |
| Google Assistant / Alexa (direct) | 3.5 % / 1.9 % | reached through Home Assistant, not by Potluck |

And the custom integrations they install: HACS on 395,983 installations, the Node-RED companion on 18,025
([custom integration data](https://analytics.home-assistant.io/custom_integrations.json)). 80 % of installations run
Home Assistant OS (556,831 of 698,769), the edition that hosts apps (formerly add-ons), so the hub-as-an-app route
reaches most of them.

**So, in order:**

1. **Home Assistant, the full integration** (section 6 above), distributed through HACS -- most of its users have
   HACS -- with the hub as a Home Assistant app for Home Assistant OS users. Its entities follow Home Assistant's own
   conventions: board health (signal, uptime, free memory, firmware) as **diagnostic** entities, which "expose ...
   diagnostics of a device but [do] not allow changing it"; **availability** from the reading's quality; the firmware as
   an **update** entity, with `installed_version` and `latest_version`
   ([entities](https://developers.home-assistant.io/docs/core/entity/),
   [update entities](https://developers.home-assistant.io/docs/core/entity/update/)).
2. **MQTT discovery**, the no-install route: half of Home Assistant's users already run MQTT, and MQTT reaches every
   other tool that speaks it.
3. **The hub's own API for PC programs** (`pot-app`), which Potluck's own tools need anyway (section 2).
4. **Everything else through Home Assistant, not beside it**: Apple Home, Google, Alexa and Matter controllers reach
   Potluck's entities through Home Assistant's own bridges (its HomeKit bridge alone is on 16.9 %), and a user who
   wants long-term charts exports from Home Assistant. Potluck builds none of these itself until a named need asks.

**What this evidence cannot say.** It counts Home Assistant users only. People who would use Potluck without Home
Assistant -- a workshop, a car, a lab -- have no comparable published data, and for them the hub's own dashboard
(reachable from a phone, section 5) and MQTT are the integrations; nothing here ranks them further.

### Later

Exporters for other people's charts and stores; section 8.4's direct reports from a board; more than one hub (two
PCs, each cabled to its own board) -- each a decision for its own time.

## 7. Questions for the discussion

1. **The hub's language and home.** Python (the tools are Python today, and the hub would absorb them), or the
   portable C++ core that `pot_hostnode` already builds for the PC (M10), with Python as a client?
2. **The client API.** gRPC (section 7.1's plan, typed) or a local WebSocket with JSON (what browsers and Home
   Assistant-style tools speak natively)? ADR-004 keeps frames below the hub either way.
3. **Telemetry's budget.** What may the hub ask of the radio, and how is the cell protected from a dashboard left open
   on a busy tab? (A **[MEASURE]** before any design.)
4. **History.** How much the hub keeps (a resource's last day? week?), and whether that is the capture file of section
   7.6 or a store of its own.
5. ~~Who may look~~ *Decided: a phone too* -- left open: the hub's own login and encryption for users without Home
   Assistant (a self-signed certificate on a house network is awkward on phones).
6. **Writes from outside** (*decided: Home Assistant writes*). Is `access: write` in the package enough as the rule,
   or does each outside writer need to be named in it too (Home Assistant, a PC app, a phone)?
7. ~~Home Assistant first, or the dashboard first?~~ *In tandem*: the hub first, then the dashboard and the Home
   Assistant integration side by side, both its clients.
8. **Several PCs.** One hub per PC with one owning the deploys, or one hub per cell?
9. **Module dependencies.** How a descriptor says `requires` (another component, a capability such as a Wi-Fi station,
   PSRAM), and whether `apply` adds a required module itself or only refuses the cluster file that lacks it.
10. **The gateway board.** Is the extender the only board that may speak MQTT for the cell, or any board an owner gives
   Wi-Fi credentials? And when both the hub and a gateway board are up, which one presents the devices?
11. **Where the integration lives.** In Potluck's repository (one release for both) or its own public repository (the
   way HACS lists integrations), and what Home Assistant versions it supports.
12. **Entity mapping.** How a resource's kind and unit become a Home Assistant entity type, and what an app's author
    adds to their descriptor to make their outputs look right in Home Assistant (a device class, an icon, a name).
13. **The firmware update entity's "install".** Home Assistant lets a user "install an offered update directly"; for
    Potluck that would run `apply`'s firmware step with the owner's keys. Allowed only when the owner switches it on in
    the hub, or never (rule 3)?

## 8. Candidate milestones (to be argued, not yet scheduled)

Sketches in the roadmap's form, so the discussion has something to falsify:

- **M13 -- the hub.** One program owns the link; every existing tool works as its client, several at once (a
  placement host, a service, `potctl watch` and `apply` together). *Accept:* the M8 service, the M10 placement host and
  a `potctl watch` run at the same time through one cabled board; killing the hub leaves the cell untouched and its
  restart catches up. *Kill:* if a hub in the path adds latency that breaks M10's handover or M8's service deadlines.
- **M14 -- telemetry over the cell.** What a board prints to its console, available to the hub over the radio within a
  budget. *Accept:* the hub shows every board's link statistics, peer view and actor stats with no console connected,
  and the cell's heartbeat round trips and delivery stay within M0's measured spread. *Kill:* if no useful rate fits.
- **M15 -- the dashboard.** The tabs of section 5 from the hub's model, live and from a recording.
- **M16 -- Home Assistant, first-class.** *Accept:* on a fresh Home Assistant installation, the integration installed
  through HACS discovers the hub with nothing typed but a confirmation; every board appears as a device and the room
  sensor, the die temperature, a board's LED and the alarm guest as entities with the right types and units; an
  automation changes the LED and the board shows it; a board unplugged turns its entities unavailable within the death
  window and back when it returns; with the PC switched off, the same devices stay live in Home Assistant through the
  extender's MQTT connection, and come back to the hub's when the PC returns; a blind reader -- a Home Assistant
  user who has never seen Potluck -- gets from the README to their boards in Home Assistant with no other help. *Kill:*
  if a resource cannot be shown in Home Assistant with its quality intact, or writes cannot be confined to what the
  package allows.
- **M17 -- PC apps.** The client API as a documented interface, with a worked example, the way DEVELOPING-APPS.md
  documents board apps.
