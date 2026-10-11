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

Open: whether it is a web page served by the hub on `localhost` (any browser, nothing to install, works on Linux and
Windows alike) or a desktop program; the draft's leaning is the first.

## 6. Integrations

### PC programs (`pot-app`)

A PC program talks to the hub, not to the serial port: it registers with a name, reads and subscribes to resources,
writes where the package allows, offers named services (M8's, moved behind the hub), and can be a placement host
(M10's, moved behind the hub). The hub knows every client by name, so the dashboard can say which program reads what.
Section 7.1 named gRPC for this API; whether it is gRPC, a local WebSocket with JSON, or both, is open.

### Home Assistant, both ways

- **Potluck into Home Assistant:** Potluck's resources appear as Home Assistant entities -- the room sensor's presence
  as a binary sensor, B's chip temperature as a sensor -- with their availability following the reading's quality.
  One route needs no Home Assistant code: MQTT discovery, where "the configuration is done on the device itself and
  the topic used by the device", on topics `<discovery_prefix>/<component>/[<node_id>/]<object_id>/config` with the
  default prefix `homeassistant`, and availability announced by birth and will messages
  ([Home Assistant MQTT](https://www.home-assistant.io/integrations/mqtt/)). It needs an MQTT broker, which a Home
  Assistant installation often already runs.
- **Home Assistant into Potluck:** Home Assistant's entities appear as resources the PC owns --
  `potluck://<system>/node-00fe/ha/<entity>` -- so a board's actor can read a door sensor or the weather the way it
  reads a board's temperature, with an age and a quality, and a dead Home Assistant reads UNAVAILABLE. Home
  Assistant's WebSocket API at `/api/websocket` authenticates with an access token and streams `state_changed`
  events (`subscribe_events`), and `call_service` acts on an entity
  ([Home Assistant WebSocket API](https://developers.home-assistant.io/docs/api/websocket/)).
- **Commands, either way, only by rule (section 3, rules 3-4):** a Home Assistant automation may write a Potluck
  resource the package declares writable; a Potluck actor may ask the hub to call a Home Assistant service the owner
  lists, as M8's named services are listed today. Nothing in Home Assistant can deploy to the boards.

This is the owner's "half-integrated member under the PC's control": Home Assistant is not a node (it does not
heartbeat or run actors), but its entities are resources in the namespace, owned by the PC that bridges them, and its
devices are as reachable from an actor as a board's pins -- with the PC's death window, not a board's.

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
5. **Who may look.** The dashboard on `localhost` only, or reachable from a phone on the house network -- and if the
   latter, with what login, given rule 6.
6. **Writes from outside.** Is `access: write` in the package enough as the rule, or does each outside writer need to
   be named in it too?
7. **Home Assistant first, or the dashboard first?** In tandem is possible: the hub comes first either way, and both
   are its clients.
8. **Several PCs.** One hub per PC with one owning the deploys, or one hub per cell?

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
- **M16 -- Home Assistant.** Both directions of section 6, under rules 3-4.
- **M17 -- PC apps.** The client API as a documented interface, with a worked example, the way DEVELOPING-APPS.md
  documents board apps.
