# Changelog

Potluck's releases. The full record -- every milestone with its measurements and caveats -- is
[STATUS.md](STATUS.md); the design and its decisions are [ARCHITECTURE.md](ARCHITECTURE.md).

## Unreleased

- **The hub goes on the house network with one request**: `POST /api/setup/listen {"on": true}` on the PC makes the
  hub's certificate (its own CA, naming the PC's address; one that already names it is kept, so Home Assistant's pairing
  holds), writes `--listen` into the tray's `tray.json` and has the tray restart the hub -- no openssl, no Task Manager.
  HOME-ASSISTANT.md part B. Asked for by the Potluck Desk.
- **A program can publish values of its own: `builtin:inbox`.** Pinned to one board, it declares `act/<actor>/<name>`
  as writable values (1-12, scalar types, a unit, a staleness bound); a program on the hub's API writes them with
  `POST /api/write` where the cluster file's `outside` allows, and every board reads them like any actor's output.
  NO_DATA until written, STALE once the program stops; `apply`'s verify does not wait for them. INTEGRATIONS.md step 7.
  Asked for by the Potluck Desk for the owner's phone app. A firmware change.
- **Wiring, for a wiring view**: an app's module descriptor may list the **parts** it needs wired (`"parts"`: part,
  bus, pins by the config field that carries each GPIO, a guide); `potluck apply --plan` refuses a GPIO used twice on a
  board or one the firmware drives; the hub's model lists each app's parts with their GPIOs (`apps[].parts`) and the
  pins the boards' build drives (`cell.pins`, from the build's own configuration; a release carries them in
  release.json). Asked for by the Potluck Desk. No firmware change.
- **A board whose pocket page is up publishes its figures**: `node-<id>/sys/pocket/phones` (stations on its access
  point), `sys/pocket/pages` and `sys/pocket/refused` (since boot), until now only on its console. Declared only where
  the page is up, read for the PC through the cabled board like `sys/ext/*`. Asked for by the Potluck Desk. A firmware
  change.
- **Board telemetry carries the identity and frame-tag counters** (`telemetry.auth` in the hub's model, and in
  `--telemetry-log`): after a move on 2026-10-19 the extender stayed out of the cell for ~4.5 minutes while the board
  on the cable rejected its frames under an old key, and nothing on the PC could say why its new HELLOs were not
  taken. A firmware change; older boards report `null`.
- **On-demand reads in the hub's API.** `POST /api/interest` asks the hub to read, for as long as a program keeps
  asking, what its model leaves out: a board's per-link outputs (`{peer}`), outputs published only sometimes (`?`) and
  a board's built-in `sys/` resources. They arrive in the model marked `"on_demand": true`; at most 32 per program and
  64 in all (HUB-API.md). Asked for by the Potluck Desk, whose per-link view of passive-sensor stayed empty.
- The hub's MQTT problem sensors in Home Assistant ("`<app>` problem") no longer switch on for apps the hub has not
  seen yet while it starts, matching the dashboard and the integration.
- **An extender board publishes its router figures**: `node-<id>/sys/ext/router-rssi` (dBm, not published while its
  station is not associated), `sys/ext/clients` (stations on its hotspot) and `sys/ext/reconnects` (since boot),
  until now only on its console. Declared only on a board whose extender started; readable through the hub's
  `POST /api/interest`. Asked for by the Potluck Desk. A firmware change.
- **A cell split by a house mesh's channels finds itself again.** A board with its own Wi-Fi connection that
  misses a board its package names for a minute probes the network's other channels (30 s each, backing off from 2
  to 10 minutes); an extender waits 3 minutes, and needs an extender app that can choose its access point at run time. Found 2026-10-18,
  when a firmware rollout restarted the extender onto the mesh's other channel and the cell stayed in two halves.
- `potluck apply` says a board that took a firmware image and restarted but has not answered since "took firmware N
  and restarted", instead of quoting the board's last reply as if nothing had been sent.
- A board reads an extender board's `sys/ext/*` for the PC as it reads the built-ins (they read UNAVAILABLE
  through the cabled board before).
- **The hub's `POST /api/write` reaches every board, not only the one on the cable.** The board on the cable passes an
  outside write for another board's resource to its owner and returns the owner's answer; the owner still checks the
  package. Before, every write to another board timed out (504) -- found by the Potluck Desk on B's LED. Needs the
  firmware and the hub of this change.
- **The dashboard sends a Content-Security-Policy** (its own scripts only, nothing from another site, not framed by one)
  and `X-Content-Type-Options: nosniff`; the mock sends the same policy, checked in a browser.
- **The tray's menu opens the Potluck Desk** ("Open Potluck Desk", and its Add boards behind "Add boards") when
  `tray.json` names it -- `packaging\install.ps1 -Desk <its folder>` writes that, keeping the cell's settings.
- **Logins last 30 days and survive a restart of the hub**, so a reinstall or a PC restart no longer logs a phone out
  (the session's hash is kept beside the logins). The hub offers its own certificate at `/hub-certificate.crt`, linked
  from the login page: a phone that trusts it once lets its browser save the password, which Chrome refuses on a page
  with a certificate error.
- **Two apps on one board's LED no longer take turns.** The board keeps one app state per app (`led_claim`, a new board
  service; `led_app` is slot 0): on the owner's cell, B's switched-on LED went blue and green every few seconds
  because passive-sensor's presence light said "off" in between. The LED switch uses its own slot.
- The setup wizard no longer checks a new board again when its port drops out of one look and comes back -- a check
  restarts the board (found by the Potluck Desk, which follows the wizard's page).
- The hub reads a request's body before refusing it (an unknown path, too large, not logged in): unread, it was taken
  for the next request on a kept-alive connection, and on Windows the client was often reset instead of answered.
- The Node-RED nodes look for hubs on every network adapter: on a PC with a VPN, Tailscale or Hyper-V, the one query
  they sent left by the default adapter and never reached the house network.

## 1.0.1 -- 2026-10-16

Fixes found running a three-board cell every day, with the dashboard, Home Assistant and the tray.

- **A hub start is quiet.** Until the hub has heard every board and each value has read GOOD twice (never longer than
  30 s after its first look at the cell), the dashboard says "Starting up" in grey and no app, board or value is a
  problem; `hub.starting`, `resource.settled_quality` (`STARTING`) and `app.working: null` in the hub's API. The Home
  Assistant integration's problems follow the same rules.
- **A board's kept history is history.** The events a board logged before the hub was watching arrive marked
  `"earlier": true`, stamped with when they happened; the dashboard dims them and sorts events by time.
- **One late read is not a fault.** Another board's value reaches the hub through the cabled board's copy, so it can
  read STALE once; app health and the problems follow a quality that held for two reads.
- **An app's health counts its own outputs**, not every value on its board.
- Fixes: `install.ps1` stops the running hub before copying (a running hub held its files); a `tray.json` saved with a
  byte-order mark no longer starts the hub with no cell.
- Recorded: on ESP-IDF 6.0.2 a CAN bus-off stops Potluck's CAN transmit until a reboot (STATUS.md, M4).

## 1.0.0 -- 2026-10-16

The S3 Edition: ESP32-S3 boards (16 MB flash, 8 MB PSRAM) working as one cell, with a PC as an optional member.
Host tools tested on Windows 11.

- **Documentation for people new to Potluck.** Every manual checked against the code and rewritten for readers who
  were not there: the getting-started guide, the app developer's manual, the hub's API, integrations, Home Assistant.
  Personal and bench-specific details removed; one current figure where numbers disagreed.
- **The store's signed list**: [Tubifix77/potluck-store](https://github.com/Tubifix77/potluck-store). The dashboard
  shows it only when the store key's signature over it verifies; its first app is `overheat_alarm`.
- **esptool runs as a program of its own** beside the Apache-2.0 tools (it is GPLv2+); the Windows package carries
  Potluck's LICENSE and NOTICE and every bundled component's licence text in `licenses/`.
- **A `potluck` command** after `pip install -e host/potluck` (`potluck apply`, `potluck hub`, ...), as in the
  Windows package.
- Fixes: with TLS (`present.tls_ca`) the boards connect to port 8883 by default, as the hub does; the build scripts
  find ESP-IDF through `IDF_PATH`; the packaged hub reports the commit it was built from; the mock hub sends board
  events as a real hub does (`board_` + kind).
- Corrected: the board console runs at 115200 baud (the configuration had claimed 921600; the image never did).

## 0.2.0 -- 2026-10-16

The first tagged release. M0-M19 accepted; M20 (Potluck for everyone) built: the setup wizard with a step for cells
of cells, the library, the floor plan, app panels, the store (off until the owner turns it on), the release firmware
carried by the Windows package, and a tray icon that keeps the hub running. A board whose task hogs a core is reset
by the task watchdog, and each board reports why it last started (`node-<id>/sys/reset`).

Known limitation: on a busy 2.4 GHz band a board's radio can stop accepting sends; it restarts itself after 60 s and
the cell carries its jobs meanwhile.

Earlier tags: `first-silicon` (2026-10-01, the firmware's first run on a real ESP32-S3) and `deploy-and-detach`
(2026-10-04, M0-M3 accepted on hardware).
