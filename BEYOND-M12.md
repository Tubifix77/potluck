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

**Decided by the owner (2026-10-11):**

- **The dashboard is reachable from a phone**, not only on the PC.
- **Home Assistant reads and writes.**
- **The Home Assistant integration is a first-class, open part of Potluck**, built to be excellent, because it is
  likely where people on the internet find Potluck useful: someone who already runs Home Assistant can add a cluster of
  cheap boards to it. Section 6 is written to that standard.

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

## 5. The dashboard

A first cut of its tabs -- each is a view of the hub's model, and none needs anything a later tab adds:

| tab | shows |
|---|---|
| **Cell** | every board and PC: role (cabled, extender, relay, placement host), firmware, uptime, alive or dead; the radio topology -- who hears whom, how well; the channel |
| **Resources** | per node and in total: namespace entries used and free, actor slots used and free (16 per package, 8 pinned per node, 8 portable per cell), RAM and PSRAM headroom, cores lent and borrowed (M9), checkpoint storage, flash wear |
| **Apps** | the package: each app, its repository and commit, its actors, where each runs, its moves over time, its checkpoints' age; guests with their author, fuel and faults |
| **Data** | the namespace as a browser: every resource with its owner, value, unit, age and quality; a resource's history as a chart; who reads it |
| **Clients and integrations** | every program connected to the hub -- dashboard sessions, `potctl`, PC apps, the Home Assistant adapter -- what each reads and writes, and how often |
| **Deploys** | `apply` runs: plan, firmware per board, package counter, live or rebooted, verify result, timings; trials and reverts |
| **Logs and events** | deaths and revivals, actor moves, faults, refused frames, trial outcomes -- from every board, on one clock -- and where each app's logs go |
| **Replay** | a recorded session (section 7.6) shown through the same tabs, so a fault can be looked at after the fact |

**On a phone too (decided).** So it is a web page the hub serves -- any browser, nothing to install, Linux and Windows
alike -- laid out for a phone's width as well as a desk's. Two ways in, one page:

- **Inside Home Assistant**, for its users: the hub packaged as a Home Assistant app (formerly "add-on") shows the
  dashboard through Ingress, which "allows users to access the app web interface via the Home Assistant UI", with
  "authentication ... handled by Home Assistant" and no port forwarding
  ([Ingress](https://developers.home-assistant.io/docs/add-ons/presentation/)) -- so the Home Assistant phone app
  carries Potluck's dashboard with no login of Potluck's own. Such an app can be given the serial adapter
  (`uart: true`, or a `devices` path; [app configuration](https://developers.home-assistant.io/docs/add-ons/configuration/)),
  so the hub can run on the Home Assistant machine itself, the adapter plugged into it.
- **On its own**, for everyone else: the hub serves the page on the house network behind a login the owner sets
  (rule 6: never shown, stored only as a hash), with an encrypted connection. Reaching it from outside the house is
  left to tools made for that (a VPN, Home Assistant's own remote access) -- the hub never opens a port to the
  internet itself.

## 6. Integrations

### PC programs (`pot-app`)

A PC program talks to the hub, not to the serial port: it registers with a name, reads and subscribes to resources,
writes where the package allows, offers named services (M8's, moved behind the hub), and can be a placement host
(M10's, moved behind the hub). The hub knows every client by name, so the dashboard can say which program reads what.
Section 7.1 named gRPC for this API; whether it is gRPC, a local WebSocket with JSON, or both, is open.

### Home Assistant: first-class, open, both ways

The bar (owner, 2026-10-11): someone who runs Home Assistant installs Potluck's integration, and their boards appear,
read and written like any other devices, with nothing about Potluck's internals to learn. Open: the integration's code
is in a public repository under Potluck's license, its protocol is documented, and nothing in it is reserved for the
maintainer's cell.

**What a user sees:**

- **Found, not configured:** the hub announces itself on the house network (mDNS/zeroconf), and Home Assistant offers
  to set it up -- an integration's discovery step "is invoked" when it is discovered, and setup happens in Home
  Assistant's UI through a config flow ([config flows](https://developers.home-assistant.io/docs/config_entries_config_flow_handler/)).
- **Devices and entities:** each board is a device (its node id, firmware, role, the board it is cabled through); each
  resource is an entity of the right kind -- presence a binary sensor, a temperature a sensor with its unit, a
  writable setpoint a number or a switch -- named from the package's paths and labels, unavailable whenever the reading
  is not GOOD (stale values are not passed off as current: rule 2). The cell's own health -- boards alive, the cabled
  link, the last deploy's result -- are entities too, so a Home Assistant automation can say "the room sensor is
  down".
- **Read and write (decided):**
  - *Home Assistant reads Potluck*: every resource the package names, live, with its age as an attribute.
  - *Home Assistant writes Potluck*: any resource the package declares writable (`access: write`), from an automation,
    a dashboard card or a voice command; the hub checks the rule (rule 4) and the board checks it again, as every
    write is checked today.
  - *Potluck reads Home Assistant*: entities the owner lists become resources the PC owns,
    `potluck://<system>/node-00fe/ha/<entity>`, read by board actors with an age and a quality -- a door contact, the
    weather, the electricity price -- and UNAVAILABLE while Home Assistant is gone.
  - *Potluck writes Home Assistant*: a board actor asks the hub to call a Home Assistant service the owner lists, as
    M8's named services are listed today; the hub calls it through Home Assistant's WebSocket API (`call_service`,
    [WebSocket API](https://developers.home-assistant.io/docs/api/websocket/)).
  - *Never*: deploying to the boards. A package is the owner's, signed with the owner's keys (rule 3); the
    integration may show `apply`'s plan and result, and the owner may run `apply` from the hub's own page.
- **Installed the usual ways:** the integration through HACS, "a custom integration that provides a UI to manage
  custom elements" ([HACS](https://hacs.xyz/)), until it is good enough to offer to Home Assistant itself; the hub as a
  Home Assistant app for Home Assistant OS users, or as a service on any PC.

**Two routes, both kept open.** A full integration (the above) is the excellent path. **MQTT discovery** is the
no-install path for people who already run a broker: the hub publishes each resource's config on
`<discovery_prefix>/<component>/[<node_id>/]<object_id>/config` (default prefix `homeassistant`) and its availability by
birth and will messages ([Home Assistant MQTT](https://www.home-assistant.io/integrations/mqtt/)), and any MQTT tool,
not only Home Assistant, can read the cell. Both are hub clients over the same API, so neither knows more than the other.

**What Home Assistant is to the cell.** Not a node: it does not heartbeat or run actors. Its entities are resources the
PC owns, so they carry the PC's death window (a reader sees UNAVAILABLE while the PC or Home Assistant is gone, for as long as
it is gone). An actor that needs a value in a deadline (latency classes L0-L2) cannot bind a
Home Assistant entity: the locality contract (section 4) refuses it at build time, as it refuses any binding the
transport cannot meet. That is the owner's "half-integrated member under the PC's control", stated as a contract.

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
9. **Where the integration lives.** In Potluck's repository (one release for both) or its own public repository (the
   way HACS lists integrations), and what Home Assistant versions it supports.
10. **Entity mapping.** How a resource's kind and unit become a Home Assistant entity type, and what an app's author
    adds to their descriptor to make their outputs look right in Home Assistant (a device class, an icon, a name).

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
  sensor, the die temperature and the alarm guest as entities with the right types and units; an automation writes a
  writable Potluck resource and a board acts on it; a board actor reads a Home Assistant entity; a board unplugged
  turns its entities unavailable within the death window and back when it returns; a blind reader -- a Home Assistant
  user who has never seen Potluck -- gets from the README to their boards in Home Assistant with no other help. *Kill:*
  if a resource cannot be shown in Home Assistant with its quality intact, or writes cannot be confined to what the
  package allows.
- **M17 -- PC apps.** The client API as a documented interface, with a worked example, the way DEVELOPING-APPS.md
  documents board apps.
