# Beyond M12: the hub, the dashboard and integrations

**A discussion draft (started 2026-10-11, rewritten the same day), not a decision.** Nothing here is part of the
architecture until it is agreed and moved into [ARCHITECTURE.md](ARCHITECTURE.md) section 13 as milestones with
accept and kill criteria, the way every milestone so far got there. Until then, ARCHITECTURE.md stays the single
source of truth, and where this draft and it disagree, it wins.

## 1. The idea

From the owner, after M12: two next steps that belong together -- **a dashboard** and **integrations** -- with one
place on the PC that knows everything about the cell, standing between two worlds:

- **Potluck underneath:** the resources the cell has, which are in use and by what, which are shared, how many slots are
  still free; the reports it sends; how many ESP32-S3 boards and how many PCs it holds.
- **What runs on top of it and around it:** which apps run, on the boards and on the PCs; which programs and other
  systems use the cell's resources, and what they read and write; where each app's logs go.

It will show a lot -- more than a few tabs -- and everything that crosses between the cell and anything outside it
should go through that one place, so that it knows.

**What the owner supplies (2026-10-11):** three needs -- *monitor everything that happens in Potluck*; *from more than a
PC*; and *integrations, Home Assistant being the one most in demand now* -- and the decisions below. What to show, what
matters most and which integrations to build after Home Assistant are settled from evidence (sections 5 and 6, each
claim cited and in the project's ledger), not from taste.

**Decided by the owner (2026-10-11):**

- **An integration lets another system use Potluck's resources.** Anything that could benefit from the cell's resources
  -- a board's LED, a sensor, the room sensor's presence, a portable actor's output -- gets a way to read them and,
  where the package allows, to write them, in that system's own terms. Home Assistant is the **first** integration,
  because it is the one most in demand now; it is not the only one, nor the shape of all of them. An integration reaches
  the other system through the PC that runs the hub, or through a board that is itself connected to it (a board on a
  Wi-Fi network). Potluck does not run the other system, and does not present it.
- **The Home Assistant integration is first-class and open**, built to be excellent: it is likely where people on the
  internet first find Potluck useful -- someone who runs Home Assistant adds a cluster of cheap boards to it.
- **Home Assistant reads and writes** Potluck's resources.
- **Integrations are modules you choose**, like passive-sensor and poor-mans-extender: what an integration needs on a
  board is an optional component, built into a cell's firmware only when its cluster file names it, because an
  ESP32-S3's flash and RAM are limited and not every cell wants every integration.
- **The dashboard shows every integration on every board**: on and active, on and idle, or not on the board.
- **The dashboard is reachable from a phone**, not only on the PC.

### The core: one presentation of the whole cell's resources

*As a picture:* [beyond-m12-overview.html](beyond-m12-overview.html) -- the boards and their resources, the hub's presentation,
the integration layer, and the programs on top (open it in a browser after cloning; GitHub shows it as source).

It comes down to the original idea -- *Kubernetes for CPU/GPU/RAM and attached hardware* (ARCHITECTURE section 0.1).
What this step adds is the part of Kubernetes Potluck does not have yet: **one presentation of every resource on all
the cell's boards and PCs**, the cell seen as one machine, that everything else is built on.

In Kubernetes that presentation is the API server: "The core of Kubernetes' control plane is the API server. The API
server exposes an HTTP API that lets end users, different parts of your cluster, and external components communicate
with one another", and kubectl and its other tools "in turn use the API"
([Kubernetes API](https://kubernetes.io/docs/concepts/overview/kubernetes-api/)). Its dashboard is one client of that
API, "a web-based Kubernetes user interface" -- and a replaceable one: the page now says "Kubernetes Dashboard is
deprecated and unmaintained" and points new installations to another UI
([Dashboard](https://kubernetes.io/docs/tasks/access-application-cluster/web-ui-dashboard/)). The API stayed; the UI
changed.

So, for Potluck (decided by the draft, on that evidence):

- **The presentation belongs to the cell, not to a PC.** Potluck runs without a PC (rule 1), and a board on the house
  Wi-Fi is part of the house's network itself: so the cell presents its resources by itself, through that board -- Home
  Assistant switches a lightbulb on a board with no PC anywhere (6.3). **A PC is an addition, never the way in:** cabled
  to a board, its hub presents the same resources under the same names, and adds what a small board cannot hold -- the
  full model (every node, resource with its tuple, actor, app, module, program, integration and event), its history,
  the dashboard and the API for the owner's programs (section 3). The two presenters can come and go; what another
  system sees stays the same.
- **The integration layer sits on that API, beside the dashboard, not inside it.** An integration presents the same
  resources to another system that the dashboard presents to a person. Built into the dashboard, an integration would
  stop when the dashboard page closes and would have to be rebuilt with every dashboard; on the API it runs on its own,
  and the dashboard *shows* it like everything else.
- **The dashboard is the first client**, and a replaceable one -- the way Kubernetes' was.

## 2. What is already there, and what was planned

The architecture planned for most of this from the start (all in ARCHITECTURE.md):

- **Section 7.1, the host's four daemons** (from Vision Document 2): `potluck-bridge` (the only program that touches
  the serial link, forwarding frames unchanged and recording them), `potluck-agent` (the PC as a member, offering
  named services), `potctl / pot-dashboard` ("`kubectl` for the mesh: topology, live namespace, logs, deploy") and
  `pot-app` (user programs, with the same read contract as on-node actors). "Nothing above `potluck-bridge` may open
  a serial port", and the agent is "the *only* place a second schema exists".
- **ADR-004:** one frame format everywhere, no translation in the bridge. Its revisit trigger names this case: "a
  third-party host integration genuinely requires a typed IDL below the agent."
- **Section 7.5 / M8:** named services a PC offers the boards -- built and accepted.
- **Section 8.4:** a board may report to a cloud service, outbound only: "an outbound cloud path is an inbound
  attack surface."
- **Section 5 / 7.6:** "byte-identical replay from radio to dashboard" -- a dashboard was meant to show recorded
  sessions as well as live ones.

What was built instead of the four daemons: one Python tool per job, each opening the serial port itself --
`potctl`, `potluck.fw`, `potluck.apply`, `potluck.agent`, `potluck.hostnode`, the live table. **Only one of them can
hold the port at a time** (M0-RUNBOOK section 14): today the PC can be a placement node, or offer a service, or be
watched, but not all at once. That is the strongest reason for this whole step: the planned bridge-and-agent split has
become necessary, not just tidy.

## 3. The shape: the presentation, its clients, and board modules

Two presenters of the one presentation: **the gateway board** (a board on the house Wi-Fi with the gateway module: the
cell's resources, their availability and the writes the package allows, to any system that speaks MQTT -- lean, because
it lives on an ESP32-S3) and **the hub** (a PC's full model and API, when a PC is there). The picture below is the hub's
side; the gateway board is at its bottom left, and works without the rest.

```
                       ┌────────────────────── potluck-hub (one per PC) ──────────────────────┐
  boards ─radio─ cabled board ─serial─ │ the link: owns the port, forwards frames, records them        │
     │                                 │ the model: cell, resources, apps, modules, clients, events    │
     │                                 │ the API: read, subscribe, write (by rule), deploy, log        │
     │                                 └──┬──────────┬──────────┬──────────┬────────────┬─────────────┘
     │                                    │          │          │          │            │
     │                               dashboard   potctl /   your own   integration   integration
     │                               (any        apply /    programs   plugins:      plugins:
     │                               browser,    fw         (pot-app)  Home          later ones
     │                               a phone)                          Assistant,
     │                                                                 MQTT
     │
     └─ a board with an integration module (e.g. the MQTT gateway on a board on the house Wi-Fi):
        presents the cell to another system by itself, with no PC on
```

- **The hub** is section 7.1's bridge and agent as one service on the PC -- the only program on the serial port, the PC's
  identity in the cell (enrolled like a board, as `pot_hostnode` is) -- and it holds **the presentation**: the place
  that *knows*: every board's
  inventory and modules, every resource and who owns it, every actor and where it runs, every client and every
  integration and what each uses. Every other program on the PC -- the existing tools included -- becomes its client.
- **The dashboard** is a client like any other, with the most to show. It is a view of the presentation, so it can be
  closed, reloaded or opened on several phones at once without anything in the cell -- or any integration -- noticing.
- **Integrations** are adapters to other systems. Most live in the hub as **plugins** (where space is not short);
  the part that must be on a board -- a board's own connection to another system, for when no PC is on -- is a **board
  module** chosen per cell (section 6.2). Either way an integration reports its state to the hub, so the dashboard can
  show it.

So "everything goes through it" is true of the **hub**, not of the dashboard window: whatever crosses between the cell
and the outside crosses the hub -- or, for a board module, is reported to it -- which is how the dashboard can show all
of it.

### What the presentation holds

Kubernetes presents a cluster as kinds of objects (nodes, pods, services ...); the cell's presentation has its own,
each already a thing Potluck has -- only now in one model, with one API to read and watch them:

| kind | one of them is | what the presentation says about it |
|---|---|---|
| **Node** | a board or a PC | role (cabled, extender, relay, placement host), firmware, uptime, alive or dead, its links |
| **Resource** | a namespace entry: a sensor, an LED, an actor's output | owner, value with section 4's tuple, writable or not, who reads it |
| **Capacity** | what a node offers and how much is used | namespace entries, actor slots, RAM, cores, flash -- used and free |
| **App** | a package's part from one repository | its actors, where each runs, its checkpoints, its version |
| **Actor** | a running actor | its node, its moves, its faults |
| **Module** | a component built into a board's image | on and active, on and idle, or not on the board |
| **Integration** | an adapter to another system | connected or not, what it presents, what it was asked |
| **Client** | a program using the hub | what it reads and writes |
| **Event** | a death, a move, a fault, a deploy, a refused write | when, where, on one clock |

The dashboard draws these; an integration translates them into another system's terms; `apply` and the owner's
programs read and change them through the same API, under section 3's rules.

### Basic Potluck, or added by choice (decided 2026-10-11)

Much of this would be unwanted in some cells -- like the Wi-Fi extender, an app a cell chooses, not a feature of every
installation. So every part is either **basic** (in every installation) or **optional**, because it is a **security
liability** someone may not want (it opens the cell to something outside it) or **filler** someone will never use.
Optional parts are added like apps -- named in the cluster file, built into a board's firmware or switched on as a hub
plugin -- and are off by default. A part is basic only if every cell needs it and it opens nothing to the outside.

| part | where | class | why |
|---|---|---|---|
| module plumbing: a descriptor's `requires`, each module's cost in `apply --plan`, the module list and states in `NODE_STATUS` (M13) | host tools; a few bytes on a board | **basic** | it is what makes everything below optional: without it nobody can see or choose what a board carries |
| telemetry responder (M15) | a board, when the cell has a hub | **optional -- filler** without a hub | the minimal case (below) never asks for it; it answers only the cell's own enrolled hub, at the rate the hub asks. Basic only if its built-in cost is measured near zero |
| the hub (M14), listening on the PC itself only | a PC | **basic** for anyone with a PC (Potluck's own tools become its clients) | costs the boards nothing; reachable from nowhere but the PC by default |
| the integration interface (M18) | the hub | **basic** | an interface, not an integration: it does nothing until an integration is added |
| the gateway module (M13) | one board on a Wi-Fi network | **optional -- a security liability** | it opens a path from a house network into the cell and needs Wi-Fi and broker credentials on a board; costs internal RAM the board barely has |
| writes from outside (M13) | the package, per resource | **optional -- a security liability** | read-only unless the owner declares a resource writable from outside, one by one |
| the LED-as-a-resource actor (M13) | a board | **optional -- filler** for most | an actor in the package like any other: present only if the package names it |
| the dashboard (M16) | a hub plugin | **optional -- filler** for some | not every cell is watched; some owners only ever use `apply` and Home Assistant |
| the dashboard and the API reachable from the house network or a phone (M16) | the hub | **optional -- a security liability** | anything that answers beyond the PC is an attack surface: off until the owner switches it on, behind a login, encrypted |
| the Home Assistant integration (M17) | a hub plugin, and a Home Assistant integration | **optional -- filler** without Home Assistant | of no use to a cell without Home Assistant |
| every later integration (M18) | a hub plugin, or a board module | **optional** | each one a liability, a filler, or both, for the cells that do not want it |

Already true before M13, and kept: the extender (poor-mans-extender) and the room sensor (passive-sensor) are apps a
cell chooses, and M7's sandbox is a firmware option (`CONFIG_POT_WASM`, off by default) -- the same pattern.

**The min/max test (the owner, 2026-10-11).** Each part was checked against two real cells. *Minimal:* two boards on the
home network, both detecting whether plants are watered, one of them also the Wi-Fi extender. *Maximal:* a
multifunction platform -- three detectors, often idle and lending their cores to Monte Carlo jobs (M9), lightbulbs in
Home Assistant, large reports to the dashboard, surveillance software reading the cell. The classes above are the
result; three things changed because of it. **Telemetry** is optional, not basic: the minimal cell has no hub to ask for
it. **The gateway's cost** is measured on the minimal cell's tightest board -- the extender, an app and the gateway
together -- since that is where a PC-less owner sees "plant dry" on a phone (M13). **The maximal cell needs two things
nothing else settled:** how much history and how large a report the hub keeps (M16), and how much an outside program --
surveillance software, say -- may pull from the cell, so one client cannot load the radio for everyone (M18). Note too
what the minimal cell shows about a PC: it needs one to be set up (`apply` deploys through a PC's cable), never to run.

### What must not change

These are the existing rules this step lives inside; a design that breaks one is a design to change.

1. **The cell runs without the hub.** Boards place, fail over, keep their package and their checkpoints with no PC
   at all (M3, M6, M8, M12). The hub observes and brokers; it is never in the loop that keeps the cell alive. A hub
   crash, a PC asleep, a dashboard closed: the cell carries on, and the hub catches up when it comes back.
2. **Every value keeps its tuple.** Whatever the hub or an integration shows or passes on is section 4's reading --
   value, unit, age, quality -- never a bare number. A STALE value is shown stale; in another system it is shown as
   unavailable rather than passed off as current.
3. **Only the owner's signed package changes what the boards do.** No client, no integration, no dashboard button
   deploys anything the owner did not sign. A deploy from the dashboard is `potluck apply` with the owner's keys, on
   the owner's PC.
4. **Writes only where the package allows them.** A resource another system may write is declared writable in the
   package (it already has `access`); the hub, or the gateway board, refuses the rest, and the owning board checks
   again, as every write is checked today. The safety line holds: Potluck is never the only thing between a motor and a
   person (section 12).
5. **Boards stay outbound-only** (section 8.4). A board serves no page and holds no listener; a board module connects
   out to a system the owner names.
6. **Credentials are never shown or logged** -- Wi-Fi passwords, signing keys, the tokens and passwords of other
   systems are typed by the owner into files the hub (or a board) reads, and never displayed.

## 4. What the hub has to know, and where it comes from

| what | where it comes from today | gap |
|---|---|---|
| boards and PCs in the cell, alive or dead | the cabled board's peer table, HELLO/heartbeats on the frame link | the hub sees only the cabled board's view; other boards' peer tables are on their consoles |
| each board's firmware, package, roles, actors, counters | `NODE_STATUS` (M12), proxied to every board | none: built |
| which modules each board's image carries, and each one's state (active, idle, failed) | -- | new: a component list in `NODE_STATUS`, and a state per module |
| links: signal strength, delivery, round trips | each board's `link`/`peers` stats lines | on the consoles only; a board on a charger has none |
| resources: namespace entries used of 128, actor slots of 16/8/8, RAM headroom, cores lent, NVS | partly `sys/*` resources (heap, uptime, peers), the rest stats lines | most on the consoles only |
| every resource's value and quality | READ through the cabled board | none, but reads cost radio time: the hub must cache and pace them |
| where each portable actor runs, and its moves | the reconciler's claims (radio), `actor_owner` events on consoles | events on consoles only |
| apps (packages, fragments, apps' repositories at which commit) | the cluster file and `apply`'s cache | none: on the PC already |
| programs on the PC that use the cell, and what they read and write | -- | new: the hub's own client registry |
| integrations: each one's state, what it offers the other system, what it was asked | -- | new: reported by each plugin and each board module |
| logs, and where they go | each board's console; each tool's own output | new: a log path the hub owns, and a record of where each app's logs go |
| reports sent | -- (section 8.4's reporting is not built) | new |

The largest gap is the *consoles*: most of what a board knows about itself it prints to its USB console, which no
deployed board has. So the hub needs **telemetry over the cell**: a board's statistics, peer view, actor stats, module
states and recent events reaching the hub over the radio, the way `NODE_STATUS` already does for the inventory. That has
a cost the radio must be able to carry: today's console output is up to ~3 KB a board every statistics period
(`firmware/main/m0_main.cpp`), and three boards' heartbeats already share a channel in a 2.4 GHz-noisy house. A pull
model (the hub asks, at a rate it is given, for the sections it shows) fits better than a push of everything; its
budget is a **[MEASURE]**: what telemetry rate leaves heartbeat round trips and delivery where M0 measured them.

## 5. The dashboard: what to show, settled

Two field methods decide the order, both chosen because they are the standard ones for exactly this question:

- **Symptoms first.** Google's SRE book: "Your monitoring system should address two questions: what's broken, and
  why?", and "it's better to spend much more effort on catching symptoms than causes"; "Dashboards should answer basic
  questions about your service, and normally include some form of the four golden signals" -- latency, traffic,
  errors, saturation ([SRE book, Monitoring Distributed Systems](https://sre.google/sre-book/monitoring-distributed-systems/)).
- **Every resource, three questions.** The USE method: "For every resource, check utilization, saturation, and
  errors" ([USE method](https://www.brendangregg.com/usemethod.html)).

So the dashboard is layered, each layer a click deeper than the last.

### Layer 1: the first screen -- is anything broken? (symptoms)

One screen, readable on a phone, that answers "is my cell all right?" before anything else:

- **Every app: working or not**, judged the way `potluck apply` verifies (M12): its actors running where the package
  says, each output GOOD. An output STALE, FAULTY or UNAVAILABLE is the headline, with how long it has been so.
- **Boards and PCs: alive N of M**, and which is dead, since when.
- **Every integration: connected or not** -- the other system reachable, the board module or plugin that serves it up.
- **The last change**: the last deploy or firmware update, its result, and any board still on trial or reverted.
- **The PC's own link**: the hub's connection to its cabled board.

These are the failure contract's events, which is the point: the README lists what can go wrong in Potluck, so that is
what the first screen watches.

### Layer 2: the cell's four golden signals

| signal | in Potluck | today's source |
|---|---|---|
| latency | heartbeat round trips per link (p50, p99); a read from the hub to each board; deploy and failover times | `link` stats; the hub's own timing; events |
| traffic | frames per second per board and per link; reads and writes per client and per integration; actor publications | `link` stats; the hub |
| errors | delivery loss per link; deaths and revivals; refused frames (authentication, downgrade); actor and guest faults; refused or reverted deploys; refused writes from outside; NVS write errors | `link`, `auth`, `rec` stats; events; `apply`; the hub |
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
| each integration's link to its system | messages per second | its queue | disconnects, refused writes |
| the hub's PC | CPU, memory | -- | client errors |

### Layer 4: the drill-down tabs

**Cell** (topology, roles, channel, and every board's modules: on and active, on and idle, or not on the board),
**Resources** (layer 3 in full), **Apps** (packages, actors, where each runs, moves, checkpoints, guests), **Data** (the
namespace browser with the tuple and history), **Programs and integrations** (every client of the hub and every
integration, plugin or board module: what it offers, what it reads and writes, how often, its state), **Deploys**
(`apply` runs, trials, reverts), **Logs and events** (on one clock, from every board), **Replay** (a recording through
the same tabs). Every number on layers 1-3 links to the tab that explains it -- "what's broken" first, then "why".

### The ESP32-S3 as a filter

What the boards can afford decides what the dashboard -- and any integration -- may ask of them (the owner's filter,
2026-10-11):

- **Internal RAM is nearly spent:** 60.1 KB of the 64 KB cap on the full image (STATUS.md). Telemetry or a board module
  may add almost nothing in internal RAM: telemetry reuses the counters the stats lines already keep, and buffers go to
  PSRAM.
- **The radio is shared with the heartbeat**, on one channel, in a 2.4 GHz-crowded house; a v1 ESP-NOW frame carries
  226 B of payload (section 5.3). So telemetry is **pulled by the hub, in small pieces, at a rate it is given** -- and
  neither viewers nor integrations multiply it: ten phones and three integrations cost the cell what one hub costs,
  because the hub caches and serves them all. The rate is a **[MEASURE]** (section 4).
- **The boards are not servers.** They run ESP-NOW, are outbound-only (section 8.4), and only an extender holds a Wi-Fi
  association. So unlike ESPHome, where "each ESP is its own server" for Home Assistant
  ([ESPHome native API](https://esphome.io/components/api/)), no board serves a page or an API: the hub speaks for the
  cell, or one board with an integration module connects out for the whole cell -- never each board for itself.
- **No 802.15.4 radio on the S3** (ESP-IDF names the ESP32-H2, C6 and C5 as the chips with one; section 3): Thread,
  Zigbee and Matter-over-Thread cannot be board-side integrations in this edition; a system that speaks only those is
  reached from the hub's side, or through another system that bridges to it.

### On a phone too (decided)

The dashboard is a web page the hub serves -- any browser, nothing to install, Linux and Windows alike -- laid out for a
phone's width as well as a desk's. A phone reaches the cell:

- **Through the hub's page**, for everyone: served on the house network behind a login the owner sets (rule 6: never
  shown, stored only as a hash), with an encrypted connection. Reaching it from outside the house is left to tools made
  for that (a VPN, or another system's own remote access) -- the hub never opens a port to the internet itself.
- **Through an integration's own app**, for people who already use one: once the cell's resources are another system's
  devices, that system's phone app shows them. For Home Assistant this is the common case -- 86.2 % of the installations
  that share their integrations use its phone app (section 6.3). And when the hub runs on the Home Assistant machine
  itself, the hub's page can appear inside Home Assistant through Ingress, which "allows users to access the app web
  interface via the Home Assistant UI", with "authentication ... handled by Home Assistant" and no port forwarding
  ([Ingress](https://developers.home-assistant.io/docs/add-ons/presentation/)); such a packaged hub can be given the
  serial device (`uart: true`, or a `devices` path; [app configuration](https://developers.home-assistant.io/docs/add-ons/configuration/)).

## 6. Integrations

### 6.1 What an integration is

An adapter between Potluck and another system, so that system can use the cell's resources. Whatever the system is -- a
home-automation hub, a message broker, a data or charting tool, a factory or robot controller, a cloud service, a
program of the owner's own -- an integration does the same five things, and the hub (or the board module's runtime)
gives it all five, so each new one is an adapter, not a new design:

1. **Names Potluck's resources in the other system's terms** (an entity, a topic, a tag, a metric), from the package's
   paths and labels, and the cell's boards as that system's devices where it has them;
2. **Carries section 4's tuple across** -- the value with its age and quality, unavailable when it is not GOOD, never a
   bare number (rule 2);
3. **Writes only what the package declares writable** (rule 4), and never deploys (rule 3);
4. **Is a module the owner chooses** -- a hub plugin, or a board module where it must be on a board (6.2) -- with its
   cost known before it is chosen;
5. **Reports its own state** to the hub -- connected or not, what it offers, what it was asked -- so the dashboard can
   show it.

An integration may also, optionally, offer the other system's data *to* the cell -- a value from that system as a
resource the PC owns, read by board actors with an age and a quality and UNAVAILABLE while the system is gone -- and let
an actor ask the hub to call a service the owner lists there, as M8's named services are listed today. That is a
convenience some integrations will want; it is not what makes them integrations. Such a resource carries the PC's death
window, so an actor that needs a value within a deadline (latency classes L0-L2) cannot bind one: the locality contract
(section 4) refuses it at build time, as it refuses any binding the transport cannot meet.

### 6.2 Integrations are modules, chosen per cell (decided)

The same way apps are (M8.2, M12): what an integration needs on a board is an **external component** with its own
descriptor, in its own repository or in Potluck's, and a cell's cluster file names it under `apps` -- or leaves it out.
`potluck apply` builds into the cell's one image exactly the components its cluster file names, so a cell that wants
no board-side integration carries none. M12's "one image, roles at run time" still holds: one image per *cell*, the
image being what that cell chose; within it, a module's role runs only where it applies (the MQTT gateway only on a
board that holds Wi-Fi credentials and a broker address).

| integration part | where it runs | how it is chosen |
|---|---|---|
| a hub plugin (Home Assistant's, MQTT from the PC, later ones) | the hub | switched on in the hub's configuration |
| a board module (the MQTT gateway: a board presenting the cell to a broker) | one board connected to that system | named in the cluster file; it **requires** what it runs on (for the gateway, the extender's Wi-Fi station) |
| the other system's own side (Home Assistant's integration) | that system | installed there the usual way (for Home Assistant, HACS) |
| the hub's API for the owner's own programs | the hub | always there: Potluck's own tools use it |

Two rules follow from the S3's limits:

- **A board module only when it must be on the board.** Anything the PC can do -- most of an integration -- is a hub
  plugin. The board side is the part that cannot move: a board's own connection to another system, so the cell is seen
  with no PC on.
- **Its cost is known before it is chosen.** A module's descriptor says what it needs (`requires`: the extender's Wi-Fi
  station, a pin, PSRAM), and `potluck apply --plan` shows what the cell's image costs with it -- flash in the 2 MB
  update slot, static internal RAM against the 64 KB cap (60.1 KB with the sandbox, the room sensor and the extender) --
  and refuses a choice that does not fit, before any board changes, as it refuses everything else (M12). The build
  already measures both (`tools/build_firmware.ps1`'s budget check); the plan has to say it per module.

**The dashboard shows every module on every board (decided):** for each board, each module is

- **on, and active** -- built into the board's image and running its role there (the MQTT gateway connected to its
  broker, the extender serving its hotspot);
- **on, idle** -- built in, with nothing to do on this board (the gateway on a board with no Wi-Fi credentials), and why;
- **not on the board** -- not in this image; choosing it means a new firmware, which `apply` rolls out.

The board says this itself, so the dashboard shows what is running rather than what was meant to be: its inventory
(`NODE_STATUS`, M12) grows a list of the components built into its image -- today it reports only roles and running
actors -- and each module reports its own state (active, idle and why, or failed). The hub cross-checks that against the
build it made for the cell (the cluster file and the cache record which components went into which image), and a board
whose image does not match is shown as such. Hub plugins are shown the same way, on the hub's own row.

### 6.3 The first integration: Home Assistant

**Why first.** Home Assistant publishes its own usage data: 698,769 active installations, 553,881 of which share which
integrations they use (opt-in, and by the site's own note likely under a quarter of all users;
[analytics.home-assistant.io](https://analytics.home-assistant.io/), data of 2026-10-11). It is the system the owner
named, and its data says how to do the integration well:

| among Home Assistant installations sharing their integrations | share | what it says for this integration |
|---|---|---|
| its phone app (`mobile_app`) | 86.2 % | the cell's devices in Home Assistant are on its users' phones with nothing more to build |
| MQTT | 48.6 % | half already run a broker: the no-install route (MQTT discovery) reaches them, and a board can speak it by itself |
| ESPHome | 27.3 % | ESP boards in Home Assistant are common: Potluck's likeliest early users, and the conventions they expect |
| its HomeKit bridge | 16.9 % | Home Assistant re-exports its devices to Apple Home (and to Google and Alexa through other integrations), so the cell reaches those too, through Home Assistant |
| InfluxDB / Prometheus (time-series export) | 5.1 % / 0.7 % | few of its users export metrics; its own history covers most |
| custom integrations: HACS | 395,983 installations ([data](https://analytics.home-assistant.io/custom_integrations.json)) | most install community integrations through HACS: that is where Potluck's goes first |
| installation type: Home Assistant OS | 556,831 of 698,769 (80 %) | most can run a packaged hub on the same machine, if they want |

**The bar (decided).** Someone who runs Home Assistant adds Potluck, and the cell's resources appear there -- read and
written like any other devices -- with nothing about Potluck's internals to learn. Open: the code is in a public
repository under Potluck's license, its protocol documented, and nothing in it is reserved for the maintainer's cell.

**Two ways in, the same devices:**

- **Through the hub (a hub plugin, and an integration installed in Home Assistant).** The PC on the cable runs the hub;
  the hub announces itself on the house network (mDNS/zeroconf) and Home Assistant offers to set it up -- an
  integration's discovery step "is invoked" when it is discovered, and setup happens in Home Assistant's UI through a
  config flow ([config flows](https://developers.home-assistant.io/docs/config_entries_config_flow_handler/)). The PC
  may be any PC on the house network, including, but not necessarily, the one Home Assistant runs on.
- **Through a board, with no PC (a board module: the MQTT gateway).** An extender board is already a station on the
  house Wi-Fi (M6.1), and every board's resources reach it over the cell. So it can present the whole cell by itself, as
  an **MQTT client**: it connects out to the broker the owner names and publishes each resource's discovery config, its
  state and its availability, and receives writes on the resources the package declares writable. MQTT discovery is
  how a device announces itself to Home Assistant over MQTT: "the configuration is done on the device itself and the
  topic used by the device", on `<discovery_prefix>/<component>/[<node_id>/]<object_id>/config` (default prefix
  `homeassistant`), with availability by birth and will messages
  ([Home Assistant MQTT](https://www.home-assistant.io/integrations/mqtt/)). This keeps rule 5: the connection is the
  board's own, outbound. Its costs are bench questions: the MQTT client is a managed ESP-IDF component in v6.0.2
  (`espressif/mqtt`), its RAM must fit an extender build whose internal RAM is nearly spent (PSRAM where it can), and the
  broker's password is the owner's to type, like the Wi-Fi's (rule 6) -- **[MEASURE]** before it is designed further.
  The same discovery messages can come from the hub as a plugin, for cells whose PC stays on.

Both show the **same devices with the same names**, so a cell can move from one to the other (the PC switched off, the
gateway board taking over) without Home Assistant's automations noticing more than a moment of "unavailable".

**What a Home Assistant user sees:**

- **Devices and entities:** each board a device (its node id, firmware, role); each resource an entity of the right
  kind -- presence a binary sensor, a temperature a sensor with its unit, the status LED a light, a writable setpoint a
  number or a switch -- named from the package's paths and labels, unavailable whenever the reading is not GOOD (rule 2).
  Board health -- signal, uptime, free memory -- as **diagnostic** entities, which "expose ... diagnostics of a device but
  [do] not allow changing it"; the firmware as an **update** entity with `installed_version` and `latest_version`
  ([entities](https://developers.home-assistant.io/docs/core/entity/),
  [update entities](https://developers.home-assistant.io/docs/core/entity/update/)); and the cell's own health -- boards
  alive, the link, the last deploy's result -- so an automation can say "the room sensor is down".
- **Reads and writes (decided):** Home Assistant reads every resource the package names, live, with its age as an
  attribute; it writes any resource the package declares writable -- the LED's colour, a setpoint -- from an automation,
  a dashboard card or a voice command. It never deploys (rule 3); it may show `apply`'s plan and result.
- **Installed the usual ways:** the Home Assistant side through HACS, until it is good enough to offer to Home Assistant
  itself; the hub as a service on any PC, or packaged as a Home Assistant app on the Home Assistant machine; or, through
  MQTT, nothing installed in Home Assistant at all.

### 6.4 MQTT, for every other system that speaks it

The MQTT side of 6.3 is not Home Assistant's only: the same messages reach any tool that reads MQTT, so it is the second
integration by construction -- a hub plugin and a board module that serve Home Assistant and any other MQTT user at
once. What topic layout and payloads it uses beyond Home Assistant's discovery convention is open (question 12).

### 6.5 The owner's own programs (`pot-app`)

A program on a PC talks to the hub, not to the serial port: it registers with a name, reads and subscribes to
resources, writes where the package allows, offers named services (M8's, moved behind the hub), and can be a placement
host (M10's, moved behind the hub). The hub knows every client by name, so the dashboard can say which program reads
what. Section 7.1 named gRPC for this API; whether it is gRPC, a local WebSocket with JSON, or both, is open.

### 6.6 Which integrations next

Decided the way Home Assistant was: by evidence of what people running systems like Potluck use, measured where it is
published, and by the S3 filter for any part that would have to live on a board. The data above counts Home Assistant
users only, and ranks nothing outside that world. People who would use Potluck without Home Assistant -- a workshop, a
car, a lab -- have no comparable published data yet; for them the hub's own dashboard (from a phone, section 5), MQTT
(6.4) and the program API (6.5) are what exists, and the next integration is chosen when evidence names it (question 9).

### Later

Section 8.4's direct reports from a board; more than one hub (two PCs, each cabled to its own board) -- each a decision
for its own time.

## 7. Questions for the discussion

1. **The hub's language and home.** Python (the tools are Python today, and the hub would absorb them), or the
   portable C++ core that `pot_hostnode` already builds for the PC (M10), with Python as a client?
2. **The client and plugin API.** gRPC (section 7.1's plan, typed) or a local WebSocket with JSON (what browsers speak
   natively)? ADR-004 keeps frames below the hub either way.
3. **Telemetry's budget.** What may the hub ask of the radio, and how is the cell protected from a dashboard left open
   on a busy tab? (A **[MEASURE]** before any design.)
4. **History.** How much the hub keeps (a resource's last day? week?), and whether that is the capture file of section
   7.6 or a store of its own.
5. **The hub's own login** on a house network, for phones (a self-signed certificate is awkward on phones).
6. **Writes from outside.** Is `access: write` in the package enough as the rule, or does each outside writer need to
   be named in it too (an integration, a program, a phone)?
7. **Several PCs.** One hub per PC with one owning the deploys, or one hub per cell?
8. **The gateway board.** Is the extender the only board that may carry a board-side integration, or any board an owner
   gives a network's credentials? And when both the hub and a gateway board are up, which one presents the devices?
9. **Which integrations next.** Gather the evidence the way 6.3 did -- what people running systems like Potluck use,
   measured where it is published -- for the candidate families: message brokers and flow tools; data, metrics and
   charting; industrial and robot controllers; cloud IoT services. Rank by that, filtered by the S3 for any board-side part.
10. **Module dependencies.** How a descriptor says `requires` (another component, a capability such as a Wi-Fi station,
    PSRAM), and whether `apply` adds a required module itself or only refuses the cluster file that lacks it.
11. **Where each integration lives.** In Potluck's repository (one release) or its own public repository (the way HACS
    lists Home Assistant integrations), and which versions of the other system it supports.
12. **Resource mapping.** How a resource's kind and unit become the other system's types (a Home Assistant entity, an
    MQTT topic and payload), and what an app's author adds to their descriptor so their outputs look right there (a
    device class, an icon, a name).
13. **Home Assistant's update "install".** Home Assistant lets a user "install an offered update directly"; for Potluck
    that would run `apply`'s firmware step with the owner's keys. Allowed only when the owner switches it on in the hub,
    or never (rule 3)?

## 8. Milestones: scheduled as M13-M18 (2026-10-11)

The owner agreed the shape ("looks right now, make the milestones from it"), and the milestones are now in
[ARCHITECTURE.md](ARCHITECTURE.md) section 13, which is where they are kept from here on: **M13** the cell presents
itself (the gateway module, no PC), **M14** the hub, **M15** telemetry over the cell, **M16** the dashboard, **M17**
Home Assistant first-class, **M18** the integration layer and the owner's programs. Two of this draft's questions were
settled on the way: writes from outside are declared **per resource** in the package (question 6), and one presenter at
a time -- the gateway with no PC, the hub when a PC is on -- so no system sees a device twice (question 8). The sketches
below are the draft's first versions, kept for the record; section 13 supersedes them.

- **M13 -- the hub.** One program owns the link; every existing tool works as its client, several at once (a placement
  host, a service, `potctl watch` and `apply` together). *Accept:* the M8 service, the M10 placement host and a `potctl
  watch` run at the same time through one cabled board; killing the hub leaves the cell untouched and its restart
  catches up. *Kill:* if a hub in the path adds latency that breaks M10's handover or M8's service deadlines.
- **M14 -- telemetry over the cell.** What a board prints to its console -- and its modules' states -- available to the
  hub over the radio within a budget. *Accept:* the hub shows every board's link statistics, peer view, actor stats and
  modules with no console connected, and the cell's heartbeat round trips and delivery stay within M0's measured
  spread. *Kill:* if no useful rate fits.
- **M15 -- the dashboard.** Section 5's layers from the hub's model, live and from a recording, on a desk and on a
  phone.
- **M16 -- integrations, and Home Assistant first.** The five things of 6.1 built once, as the hub's plugin interface
  and a board-module interface, with Home Assistant as their first user. *Accept:* on a fresh Home Assistant
  installation, the integration installed through HACS discovers the hub with nothing typed but a confirmation; every
  board appears as a device and the room sensor, the die temperature, a board's LED and the alarm guest as entities with
  the right types and units; an automation changes the LED and the board shows it; a board unplugged turns its entities
  unavailable within the death window and back when it returns; with the PC switched off, the same devices stay live
  through the gateway board's MQTT connection, and come back to the hub's when the PC returns; the dashboard shows the
  gateway module on, active, on its board and not on the others; a blind reader -- a Home Assistant user who has never
  seen Potluck -- gets from the README to their boards in Home Assistant with no other help. *Kill:* if a resource
  cannot be shown with its quality intact, or writes cannot be confined to what the package allows.
- **M17 -- the owner's programs.** The client API as a documented interface, with a worked example, the way
  DEVELOPING-APPS.md documents board apps.
