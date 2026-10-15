# Potluck — project context

Distributed runtime that makes a cluster of small machines behave like one: everyone brings a dish —
a sensor, some RAM, a radio — and the cluster eats together. "Kubernetes for CPU/GPU/RAM *and*
attached hardware."

**Current release: Potluck — S3 Edition.** The name of the *hardware support*, not of the project:
this edition targets ESP32-S3, with host tooling on Windows and Linux. Later editions add targets;
the architecture is deliberately hardware-agnostic (ARCHITECTURE.md §0.1) and the edition label exists
so that stays visible.

Code state: **M2 accepted** (a 13.7-minute session replays to a byte-identical digest), **half of M4
accepted** (the locality-contract checker rejects a cross-node L1 binding, naming both ends), and §7.8's
coordinator/worker pattern measured in simulation (19 workers, 18.99x, no work lost when one dies).
**M0, M1, M2 and M3 are ACCEPTED on hardware (2026-10-04); M4 on 2026-10-05; M5 on 2026-10-07; M6, M6.1 and M8 on 2026-10-08; M9, M10 and M11 on 2026-10-10; M7 on 2026-10-11.** M3: deploy to the cell through one board,
detach and power-cycle, and a broken module that all three nodes revert themselves (M0-LOG session 18). M2: a 10.05-minute three-board session over COM6
replays to a byte-identical 18-entry namespace (M0-LOG session 17). M0: a 24.52-hour soak, then a distance sweep through
the owner's two-storey house in which the kill criterion did not fire. M1: potctl on a CP2102 cabled to
board A reads board B's value; unplugged, it reads `UNAVAILABLE`. M0-LOG session 16. **M4 is ACCEPTED (2026-10-05)**, with
one caveat: the arbitration was not recorded by an instrument outside the boards (M0-LOG session 20). **M5 and M6 are
accepted too** (sessions 21-22 and 26), and so are **M6.1** (CR-6, the combined extender node, session 29) and **M8**
(host services, session 28). **M8.1 is built** (attached hardware -- B's die temperature read from A -- and a compiled-in actor
API; one acceptance line left, session 31), then **M9** (borrowing an idle core: ADR-005's revisit trigger as an experiment -- ACCEPTED 2026-10-10, 3.41x
with no host attached, ADR-005 amended) and **M10** (the PC as a placement node -- ACCEPTED 2026-10-10, session 35, caveat: the
cable "pulled" by killing the process), all scheduled 2026-10-08 from the owner's status review, and
**M11** (firmware over the cell, ACCEPTED 2026-10-10, session 36). **M7** (guest actors in a WebAssembly sandbox,
ACCEPTED 2026-10-11, session 38) is built into boards only with `CONFIG_POT_WASM` (default off; the bench runs
it). Also open is evidence -- the 24-hour soaks (M5.1 streaming, the relay, CR-6's
combined node; a 2-hour combined run is clean, session 30) and the extender's hunting window mid-run (needs the
owner to take the router away). The step-0 rerun and B's low inbound PDR are closed (session 30; the PDR was an
accounting bug, fixed in `c70756f`). One owner, one application package per cluster is a recorded scope choice (ARCHITECTURE section 0.1).
**What is left (audit, 2026-10-11, session 39, after M12):** every roadmap milestone is built and 14 of 17 accepted; left
are M5.1's two 24 h soaks, M8.1's "value moves when B is warmed", M8.2's 30 min empty room and 24 h with no PC, a
physical cable pull (M10) and a hand power cut (M11, M12) -- all the owner's to run; the flash endurance figure is verified
(min 100,000 cycles, WROOM-1 datasheet v1.8) and checkpoints now have a board-wide wear budget (~18-20 years);
section 15's questions 1 (the classes carry weight: three control/monitor apps use L0-L4) and 2 (128 stays; a full
table now answers FULL and is counted; `apply --plan` counts every board's entries and refuses over 128) were run 2026-10-15; B's power-on transmit stall a
known limitation (its trigger, a power-on, never retried). A second MCU family -- the proof of "hardware-agnostic"
beyond the PC node -- is the next edition's.
**M12 ACCEPTED (2026-10-11, M0-LOG session 39)** -- Potluck as a platform: one image, `potluck apply
<cluster.json>`, live package changes, checkpoints in NVS, inventory, catch-up, the test kit, presets, and the
developer manual rewritten for it (blind reader: a new app written, tested and deployed in ~18 min). Caveat: the
whole-cell power cycle was a reset of every chip, not a hand power cut. Every milestone through M12 is built and
14 of 17 accepted; what is left there is the list above, all the owner's to run. **NEXT: M13-M18 (planned 2026-10-11,
ARCHITECTURE section 13, from BEYOND-M12.md and its picture beyond-m12-overview.html)** -- the cell presenting its
resources to other systems: M13 the cell presents itself (presenting role, no PC; ACCEPTED 2026-10-14, M0-LOG session 42, with caveats), M13.1 a phone reaches the nearest board with no network (ACCEPTED 2026-10-14), M14 the hub (ACCEPTED 2026-10-14), M15 telemetry (ACCEPTED 2026-10-14), M16 the dashboard,
M17 Home Assistant first-class, M18 the integration layer (all three ACCEPTED 2026-10-14 evening, M0-LOG session 44, one caveat each). **M19, cells of cells (planned 2026-10-15, researched and externally reviewed, M0-LOG session 45): next to build -- groups first.** The owner is the idea-giver; the evidence for what to show and
which integrations came from primary sources (ledger).

**THE HARDWARE HAS ARRIVED — 2026-10-01, and all three boards are up.** Ordered 2026-09-21 as a shared
AliExpress order with the sibling Powersuit project (DKK 408), covering Potluck's M0/M4 and
Powersuit's Appendix A bench at once.

Board A — MAC `b8:1f:3f:da:63:00`, therefore **node 0x6300** — enumerated as **CH343 (COM3)**, and is
**flashed with Potluck and running** (now fw `e8fd0d7`, like B and C). Its factory image is backed up. The board
register and the bring-up findings are in [WHEN-THE-BOARDS-ARRIVE.md](WHEN-THE-BOARDS-ARRIVE.md).

> **§6's Wi-Fi DRAM [MEASURE] is CLOSED (2026-10-01): 32,264 B — 31.5 KiB, under the ~40 KB
> trigger, so the RX ring does not shrink.** That number had been open since the architecture was
> written, is named in §13-M0's acceptance table, and was the reason nothing past M2 was built:
> 11.8 KB of the budget sat committed on paper against a figure nobody had. It now survives contact
> with reality. Evidence: `captures/boardA-first-boot-095f21e.log`.

> ## HANDOVER, 2026-10-14 (evening): **M16-M18 shown in the owner's Home Assistant; the LED has the owner's colours** (M0-LOG session 44, end).
>
> M17: HACS installed `custom_components/potluck`, the hub was discovered and paired, an automation ("Potluck LED test
> (M17)", MQTT trigger `potluck-test/led`, written by the session at the owner's request) switched B's LED, and the PC
> off/on left the boards' entities live through C (hub entities back in 3-5 s). **LED scheme (the owner's): purple
> updating, yellow searching, red disconnected/dead/stalled, green on the network; apps blue or cyan only.** Bench:
> firmware `b384e80` (counter 36), package 69 from `keys/desk-m14-hub.json`; the hub runs resident (`--listen
> 0.0.0.0:8443`, log `captures/m17-hub-3.log`), paired with Home Assistant. Catch-up worked on the boards (~62 s).
> The homelab's clock runs 85 s behind the PC. Open: passive-sensor's magenta (drawn blue) for its own session.
>
> ## HANDOVER, 2026-10-14 (afternoon): **M13.1 and M15 ACCEPTED; M14 proposed for acceptance; M16 built** (M0-LOG session 42, end).
>
> M14's last line is built and shown: `present.hub` in a cluster file makes the PC's placement node present whenever
> it is in the cell (0.67-0.70 s to take the role, 2.33-2.46 s for a board to take it back, never two presenters,
> configs byte-identical). New on the wire: READ's describe flag (a node learns an entry it has no actor type for).
> Bench: firmware 34 (`1c7b6b6`), package 67 from `keys/desk-m14-hub.json`; C presents (no PC node, hub or agent
> running); A's relay off; A keeps the owner's pocket access point (test credentials, the owner's choice). The
> 8-node partition tests are in tests/test_reconcile.cpp [partition8]; a separate task was offered for a deploy and
> the leases during a split -- done the same afternoon (session 43,
> below; its firmware not on the boards yet). Open: the PC refuses a HELLO claiming to be from A (bad signature) every ~12 s;
> M16's real phone over the house network and Ingress; M17, M18. Windows Update rebooted the PC at 05:15-05:19.
>
> ## HANDOVER, 2026-10-14 (afternoon): **deploys and leases in a partition, host-tested; three deploy faults fixed** (M0-LOG session 43).
>
> The deploy server, push and catch-up moved from `m0_main.cpp` into `pot/package_agent.hpp` (host-tested in
> `tests/test_deploy_partition.cpp`). Fixed: a receive cut off mid-push blocked catch-up for good (abandoned after 15 s);
> two packages at one counter stayed mixed (the higher digest wins; the announce carries the digest, 13 bytes); a busy
> peer dropped asks (queued: a 4+4 heal 195 s -> 15 s). `apply` counts a deploy lease only from the cluster's lowest
> board. **Not on the boards yet** -- the next firmware rollout carries it; check catch-up on the bench then.
>
> ## HANDOVER, 2026-10-14 (03:45): **M13 ACCEPTED (with caveats); M14's and M15's bench runs done** (M0-LOG session 42, end).
>
> M13's evidence, line by line, and its caveats are at the end of M0-LOG session 42; the owner accepted. The build rule
> (aggregation off where a cell presents) costs B's hotspot about a quarter of its download and a third of its upload
> (owner's phone: 12.2 / 10.4 Mbps against 16-18 / 16-17). M15 at 10 s: heartbeat loss 0.43 % against 0.42 % without
> the pull; the slowest probe 60.2 ms. M14: M10's handover through the hub 608-660 ms. Bench: firmware `3995681`
> (counter 32), package 64 from `keys/desk-m14.json`, C presenting over TLS, A's relay off, no hub, host node or agent
> running. **Monitoring is the owner's daily manual check -- no background watchers.** Open: M14's hub presenting in
> the cell's place (not built); M15's sleeping board and no-console run; M16's login, phone layout and replay.
>
> ## HANDOVER, 2026-10-14 (00:30): **the presenting stall's cause found -- AMPDU transmit aggregation** (M0-LOG session 42, end).
>
> With `CONFIG_ESP_WIFI_AMPDU_TX_ENABLED=n` on every board (package 58 from `keys/desk-m13-noampdu.json`, firmware
> `ea28980`), C presented 13 h 19 min with no stall (120,894 messages, none failed); with it on, a stall every 10-60 min.
> The 24 h capture has ended; compress `captures/m13-soak24-*.jsonl` before committing. **Monitoring is now the owner's
> daily manual check -- no background watchers.** Next: make aggregation-off the build rule for a board that presents
> over its own station (measure the extender hotspot's cost with the owner's phone), then propose M13 for acceptance;
> TLS's runtime cost; M14's and M15's bench runs.
>
> ## HANDOVER, 2026-10-13 (night): **M13's kill line met on the bench; TLS, M14's leases and host node, M15 built** (M0-LOG session 42).
>
> **A 24-hour capture runs until 00:32 on 2026-10-14** (`captures/m13-soak24-{a,b,c}.jsonl`, three detached
> `tools/json_capture.py` processes, JSON only; broker side `/tmp/potluck-soak/m13-soak24-broker.jsonl` on the
> homelab), in phases: presenting (00:32-01:50, C stalled 3 times), control with no presenter (01:50-04:50, no
> stall), then presenting again from 05:07 on firmware `61beb74` (counter 26) with package 54 from
> `keys/desk-m13-soft.json` -- the stall's evidence in `tx_dead` and the experiment `CONFIG_POT_TX_DEAD_SOFT_S=10`
> (a Wi-Fi driver restart before the reboot) -- answered by 10:43: a full driver restart does not clear the wedge,
> only a chip reset does (boards now on `ea28980`, package 57). **Monitor it** -- the owner's rule:
> a board dying unseen ruins the run. **Finding:** a board presenting over its own station wedges its radio (ESP-NOW
> NO_MEM and the station's TCP stopped together); the station alone does not. M13's kill line is met; its fallback does
> not escape it. **Next:** read the presenting phase's `tx_soft` result; the TLS cost (`keys/desk-m13-tls.json`, the
> house broker's listener 8883); then decide M13's route -- a driver-level fix or recovery, or the hub presenting from
> the PC (M14's last line). Owed on the bench: M14's runs (M10's handover through the hub), M15's [MEASURE].
> `b332ec4` (peer_admitted noise fixed) is not on the boards yet. The house broker's TLS changes: `certs/`,
> `mosquitto.conf.pre-tls`, `docker-compose.yml.pre-tls` beside the originals.
>
> ## HANDOVER, 2026-10-12 (evening): **M13 shown end to end with Home Assistant; M14 and M16 first cuts running** (M0-LOG session 41).
>
> Home Assistant and Mosquitto run as containers on the homelab (`/home/boas/homeassistant/docker-compose.yml`, owner's
> account; MQTT broker localhost:1883 in HA). C presents (Potluck's own station, the owner's credentials), B is the
> fallback (BUSY, ranked last). HA shows 3 devices, 4 entities; its switch lights B's LED. Bench: firmware `8e7fce4`,
> package 52 from `keys/desk-m13.json` (modules sandbox, present, wifi, pocket). The hub (`python -m potluck.hub
> --cluster ../../keys/desk-m13.json` from host/potluck) was stopped at the owner's request; start it again to use the
> dashboard at http://127.0.0.1:8470/. **Next:** a long soak with C presenting (then propose M13 for acceptance), TLS's
> runtime cost (a TLS listener on Mosquitto), M14's remaining lines (agent and hostnode through the hub, a second
> resident hub refused, the deploy lock), M15 telemetry. (Everything was pushed that evening, `cc63476`.)
>
> ## HANDOVER, 2026-10-12 (morning): **M13 built and shown on the bench; M13.1 built** (M0-LOG session 41). Not accepted.
>
> **For the owner, what only you can do** (each unblocks an acceptance line): (1) a real Home Assistant with an MQTT
> broker -- or say which to install on the homelab; (2) house Wi-Fi on A or C for a second presenter:
> `<IDF python> tools/wifi_set.py COM5 <your file>` (two lines: name, password; it prints neither); (3) M13.1: a pocket
> password on a board (`POT! pocket ap <name> <password>` in its console, then reset) and your phone on it; (4) say
> whether to push to GitHub -- tonight's commits are local.
>
> **The bench now:** all three on firmware counter 20 (`78c5305`), package 47 from `keys/desk-m13.json` (desk-cell plus
> `present`, `modules: [sandbox, present]`, four resources in `outside`); B presents over the extender's station to
> **the bench broker on the homelab laptop** (`/tmp/potluck-broker/`, a stdlib script, nothing installed; restart per
> M0-RUNBOOK section 16 if the laptop rebooted). A PC-side subscriber may still be writing `captures/m13-broker-sub.jsonl`.
> `keys/desk-m13-off.json` is the same without the presenter (firmware null). A 3-hour soak (`captures/m13-soak-*.jsonl`)
> ran 04:22-07:22. **New fail-stop:** a board whose every radio send fails for 60 s restarts (`{"t":"tx_dead"}`) --
> C's radio stalled after 5.6 h tonight and B's boot stall recurred once; both recovered.
>
> **Next session:** read M0-LOG session 41. **First: compare the two 3-hour soaks** -- with presenting
> (`captures/m13-soak-*`: B's radio stalled twice, the fail-stop restarted it) and without (`captures/m13-soak-off-*`,
> 07:27-10:27, package 48; the bench is left on it, so put the presenter back with `keys/desk-m13.json`): does presenting
> make B's transmit stall likelier? That decides M13's kill line. Then
> deploy a build with `pocket` and `wifi` (both idle without credentials) and check NODE_STATUS shows them idle; then
> whatever the owner unblocked above. M14 (the hub) is next in section 13 once M13's owner-gated lines are in.
>
> ## HANDOVER, 2026-10-12: **M13-M18 and M13.1 PLANNED and externally reviewed** (M0-LOG session 40). Nothing built.
>
> **Start the next session like this:** read ARCHITECTURE section 13 from "*After M12, from the owner*" -- the preamble,
> the basic/optional table, the scenario rounds, the **binding review block (15 items)**, then M13 -- and BEYOND-M12.md
> only for the reasoning behind it. Then **start M13**, in this order: (1) the enforcement the review found missing:
> `exposed`, `outside_writable`, `safety_relevant` in manifest and namespace entry, and the owner-checked origin marker on
> proxied writes; (2) module plumbing (`requires`, per-module cost in `apply --plan`, module list in NODE_STATUS);
> (3) Potluck's own Wi-Fi station module; (4) the presenting module (the MQTT client is the managed component
> `espressif/mqtt` -- **ask the owner before downloading it**); measure heap/NO_MEM on the tightest board (extender + an
> app + presenting) with same-session A/B radio runs. The bench is as left by session 39 (counters: firmware 16, package
> 42; `apply` picks them). The owner is the idea-giver; engineering decisions are the session's (rule 1).
>
> ## HANDOVER, 2026-10-11 (late evening): **M12 ACCEPTED** (M0-LOG session 39).
>
> **Everything goes through `potluck apply` now** (from `host/potluck/`, ESP-IDF Python, any shell):
> `python -m potluck.apply ../../keys/desk-cell.json [--plan | inventory]`. `keys/desk-cell.json` (git-ignored: it
> names the private app repos by path) is the bench's whole cell -- Potluck at HEAD with the sandbox, passive-sensor
> at `73d43b8` (its own `manifests/ps-room.json`), the extender at `bed3685`, `manifests/die-and-alarm.json` (B's
> die temperature, the alarm guest). `desk-no-room.json`, `desk-c-off.json`, `desk-home.json` (cluster "home"),
> `fail-*.cluster.json` are the acceptance variants. Counters, variants and order are the tool's: never hand-pick
> them again. Builds are cached under `%LOCALAPPDATA%\potluck\cache`, keyed by the firmware tree (a docs commit
> sends no firmware). Every board runs ONE image; B is an extender because it holds credentials.
> **Do not run `tools/console_tap.py` on several ports at once from parallel jobs**: one such start reset C. For a
> whole-cell test use `tools/reset_capture.py` (JSON lines only, all ports in one process).
>
> ## THE BENCH NOW (2026-10-11) -- what the owner calls A, B and C
>
> All three ESP32-S3 boards sit on the desk, each on its own USB cable (power and console, 115200 baud), plus
> the CP2102 USB-serial adapter in a USB port of its own:
>
> | | node | MAC | console | build | notes |
> |---|---|---|---|---|---|
> | **A** | 0x6300 | b8:1f:3f:da:63:00 | COM3 | normal | **the board on the PC's frame link:** the CP2102 (HW-598A) on **COM6** (921600) is wired TXD to A's GPIO18, RXD to A's GPIO17, GND to G (`bench/m1-wiring.html`); every `potctl`, `potluck.fw`, `potluck.hostnode` command goes through it |
> | **B** | 0x7368 | b8:1f:3f:da:73:68 | COM4 | extender (poor-mans-extender's hotspot) | **holds the owner's house Wi-Fi and hotspot credentials**: its ESP-IDF log can name networks, so read it with JSON-only tools (`tools/json_capture.py`, `tools/console_tap.py` writes everything -- check before committing); never type or log credentials, and an erase is the owner's to run |
> | **C** | 0x8160 | b8:1f:3f:da:81:60 | COM5 | normal | |
>
> **Now (2026-10-11 late):** all three run the one image built by `apply` from Potluck HEAD + passive-sensor
> `73d43b8` + the extender `bed3685` + `CONFIG_POT_WASM=y` (version `<commit>-<key>`; firmware counter 16, next
> 17+ -- `apply` picks it), package from `keys/desk-cell.json` (counter 42, next 43+ -- `apply` picks it). B is the
> extender with the owner's credentials. `python -m potluck.apply ../../keys/desk-cell.json inventory` shows it all.
> Enrolled under the cluster CA (keys in the git-ignored `keys/`); the cell is on channel 1. The SN65HVD230 CAN
> modules are OFF (`bench/m4-can-wiring.html` to rewire). The full board register (CH343 serials, bring-up history)
> is in WHEN-THE-BOARDS-ARRIVE.md.
>
> ## HANDOVER, 2026-10-11 (night): **M7 ACCEPTED -- guest actors run on the boards** (M0-LOG session 38).
>
> A guest is a WebAssembly module from an author the owner certifies (role-3 certificate); the owner's package pins
> it by SHA-256 and wires its inputs and outputs. `pot/guest.hpp` holds the format, runtime and actor; host side
> `potluck.guest` (sign/verify bundles), `potluck.enrol --guest-cert`, `ctl deploy --guests ../..`; runbook section
> 12. On the bench: placed, failed over with its checkpoint (165 -> 167 on C -> 174 back on B), a never-returning
> guest quarantined, a bad author refused, 134 s of fuel burned on C with no death. One bench fault fixed
> (`fa64599`: a signed v2 image ended at body_len). Bench: firmware counter 8 (sandbox builds), package `m7-guests`
> at counter 28; author key `keys/acme.*`, bundles `keys/*.guest.json`. Follow-ups the same night: section 15's
> question 3 (AtomVM) recorded as decided by building, question 1 counted on radio apps only (all L4; the
> control-loop test stays open), and B's power-on stall recorded in the README as a known limitation -- its
> trigger, a power-on, has never been retried; reopen it only if it recurs. Session 37's note that two earlier
> attempts were stopped by safety reviews: this session judged the work acceptable (a sandbox that restricts code,
> asked for by the owner) and kept section 12's line -- a guest can write nothing. Open: as before.
>
> ## HANDOVER, 2026-10-10 (evening, owner away): **M11 ACCEPTED; M7 measured as an experiment and left dormant** (M0-LOG session 36).
>
> **Firmware over the cell:** `python -m potluck.fw --port COM6 status` and `... rollout <bin> --counter N --key
> ../../keys/deploy.key --bcert ../../keys/deploy.bcert [--image-for 7368=<extender bin>]` from `host/potluck/`
> (B runs the extender build, so it needs its own image). One board at a time, ~1.5-2 min each; a board keeps a
> new image only after 100 heartbeats with a radio peer, else rolls back. **Firmware counters: all three at 5
> (floor 5) -- the next image is 6+.** The partition table now has `otadata`/`ota_0`/`ota_1` after the module
> slots; a cable flash with `@flash_args` still works (it writes `factory` and resets otadata to boot it).
> Experiment images go out over the air and never confirm: one reset rolls the board back (used for M7's bench).
> `tools/m11_bench.py refuse`, `tools/console_tap.py` (reset-free console capture), test-only
> `CONFIG_POT_FW_TEST_PANIC` (never flash it by cable).
>
> **M7:** `firmware/components/pot_wasm` (wasm3 v0.9.0 + two patches, `PROVENANCE.md`), `CONFIG_POT_WASM`,
> `CONFIG_POT_WASM_BENCH`, `POT! wasm <samples> <runs>` in a node built with the option, `tools/m7_bench.py`,
> `tools/wasm_modules.py` (no WebAssembly compiler is installed: modules are hand-assembled, and only the
> wasm3 tarball was downloaded, with the owner's permission). ~50x native, +76 KB flash, every limit held.
>
> Bench: all three on `599260f` at firmware counter 5, A and C normal, B extender (still
> the owner's credentials); `m10-ticker` package at counter 24 (next 25+). Open as before: B's cold-boot NO_MEM
> stall; the 24 h soaks; a physical cable pull for M10; power cut by hand for M11.
>
> ## HANDOVER, 2026-10-10 (morning): **M9 and M10 ACCEPTED** (M0-LOG session 35). The roadmap's milestones are done.
>
> M10: the PC joins as `pot_hostnode` (`host/native/`, built by the host CMake into build/tests) through
> `python -m potluck.hostnode --port COM6 --package <pkg> --identity ../../keys/host-00fe.id` (enrolled under the CA;
> keys/ is git-ignored); the cabled board must relay: `POT! relay 1` on A's console. Caveat: the cable was "pulled"
> by killing the process; a physical pull is still owed. Bench: `m10-ticker` at counter 24 (next 25+); `ps-room`
> and `m9-mc` are not deployed. Open: B's cold-boot NO_MEM stall; the 24 h soaks; M7 stays gated.
>
> ## (earlier the same night) M9 ACCEPTED (M0-LOG session 35).
>
> Three boards 3.41x faster than one on a pure-function job, no host attached, identical results, B reset
> mid-run with nothing lost, lenders' duties unchanged; ADR-005 amended in the ADR. Bench: A, C normal build
> and B extender build of `c30bb85`; package `m9-mc` at counter 22 (next 23+); `ps-room` not deployed.
> Tools: `tools/m9_bench.py COM3 <out> [--lend-only] [--reset-port COM4]`, `tools/m9_report.py`.
> Gates: every Python suite now runs (three were missing), the portability gate uses Git Bash, never WSL.
>
> ## HANDOVER, 2026-10-10: M8.2's PS-0 PASSED in the room; both apps called done by the owner (M0-LOG session 34).
>
> **Back on the desk since 2026-10-10 night**, all three on USB (COM3/4/5) and the CP2102 on COM6; CAN modules
> OFF (rewire from bench/m4-can-wiring.html only if a CAN item is worked). For PS-0 they had been placed around
> the owner's room. Focus is back on Potluck itself; next per the status review: M9. Empty room read
> empty, a person walking/standing/sitting read present; kill check passed, PS-6 not needed. Bench: Potluck
> `a6feb9c` + passive-sensor `f7975d9`, `ps-room` from passive-sensor `2992b2e` (threshold 2.5 dB) at **counter 20**
> (next 21+). Left for M8.2's acceptance: 30 min empty room in place and 24 h with no PC -- the owner decides the
> 24 h run (PC on, recording method). Open: B's cold-start NO_MEM stall. The owner judges passive-sensor and
> poor-mans-extender done; both updated their own docs (passive-sensor pushed `73d43b8`; the extender committed
> `bed3685` and waits for the owner to say push). Both app repos are PRIVATE: never link them from Potluck's docs.
>
> ## HANDOVER, 2026-10-09 (evening): D13 reboot test, two Potluck fixes, one open stall (M0-LOG session 33).
>
> Fixed: a hunting extender no longer drags the cell to its boot channel (`4d2ea83`); `RxSample::channel`
> (`02b7bfe`) so an app can relearn after a channel change; sweeps no longer stop where a rebooting station's
> scan is or follow another sweeper (`55aa561`); a hunting node does not settle (`f43f19d`); `RxSample::off_home`
> (`225d801`). Bench now Potluck `a6feb9c` + passive-sensor `f7975d9`: 7 resets, no false presence, no
> relearns. **Next is PS-0 with the boards placed in the room (the owner).** **Open: B's ESP-NOW transmit stall (NO_MEM for
> ~40 min) after a cold boot, on the dynamic-TX build** -- not reproduced by 13 resets. Bench: Potluck
> `f31c56c` + passive-sensor `a4c2a8e`, `ps-room` at counter 17 (next 18+), cell on channel 1. Waiting on
> passive-sensor for a commit using `RxSample::channel`, then the 7-reset test again.
>
> ## HANDOVER, 2026-10-09 (early morning, owner asleep): M8.2 PS-1..PS-5 built and on the bench (M0-LOG session 32).
>
> All of CR2's Potluck items are built (PS-4 checkpoint `cfae645`/`4310b87`, PS-5 LED `ecd7359`, tee print gate
> `2e354a2`). Bench: A passive-sensor-tee, B extender-passive-sensor (still the owner's credentials), C passive-sensor,
> all Potluck `6090b5d` + passive-sensor `3dc9aab`, built from `git archive` exports in the scratchpad (never a live
> tree). Package `ps-room` at **counter 16** (next 17+). A second quiet night run (`captures/m82-night2-*`) is recording.
> Waiting on the owner: PS-0 (the walk-through script), the 24 h soaks (scheduled tasks), router gone mid-run, garden.
>
> ## HANDOVER, 2026-10-08 (late evening): M8.2 (CR2, passive-sensor) under way (M0-LOG session 32).
>
> RSSI tee, PS-1 (external components, -AppComponents, potluck.modules, -OutDir), PS-2 (Actor::on_rx_sample)
> and PS-3 (outputs(d,node,peer), adoption on demand, path_hash_from) are built and pushed (50118d0).
> passive-sensor writes rf_link next, then PS-0 (the owner walks a script; capture polls through A).
> Talk to it directly (SendMessage "Passive sensor ESP32-S3 project"); the owner gave a standing go-ahead.
> Bench: A, C on the appdemo build, B on extender-appdemo (code = 50118d0); package m82-demo at counter 8
> (next 9+). Never type `bash` in PowerShell: it is WSL.
>
> ## HANDOVER, 2026-10-08 (evening): M8.1 built and mostly shown on the bench (M0-LOG session 31).
>
> Actor API (`pot/actor.hpp`, one table in `main/actor_table.cpp`; `m0_main.cpp` names no actor type), the
> die-temperature actor on all three, and two fixes (replicas of other boards' pinned outputs; FAULTY now
> crosses the wire). Left for M8.1: B's value moving when warmed (needs a hand). The channel-11 split is
> FIXED (`f802ac9` re-key, `a8cbb1e` save rule, `5fc239c` search, CR-7 wired in `5fd0eab`): 15 reboots
> rejoined in 6-18 s. Bench: A, C on `f802ac9`, B extender `228cd5b`, cell on 11. Package `m81-die` at
> counter 7 (next 8+). `tools/console_send.py` sends a `POT!` line without a reset. Waiting on the owner:
> the 24 h combined soak (scheduled tasks), router gone mid-run (hunting window), garden placement.
>
> ## HANDOVER, 2026-10-08 (midday, owner away): poor-mans-extender's f1624de answered (M0-LOG session 30).
>
> Router gone: deaths of B 12x rarer with the extender's back-off, still one per attempt (each never-associated
> scan is 3.7-4.0 s off-channel), so the extender build now declares 60 x 100 ms while its station hunts
> (`e641bd4`; shown at boot, mid-run untested). Step 0 rerun on the aligned config: 16-18/16-17 Mbps, loaded
> latency 26-33 ms. A 1.96 h combined soak: 0 deaths, 0 reconnects. A late-joining board's phantom unicast loss
> fixed (`c70756f`). Bench: A, C on `4eb4f74`; B on the extender build of `cfe62aa` with the owner's credentials.
> The 24 h soak needs `tools/soak.ps1` (scheduled tasks): ask the owner first.
>
> ## HANDOVER, 2026-10-08 (late morning): M6.1 and M8 ACCEPTED; synced to GitHub. No milestone is next (M7 gated).
>
> Bench left running by the owner's choice: A, C on `7ec8229`; B on the extender build of `7ec8229`, still holding
> the owner's house Wi-Fi and hotspot credentials (the owner said keeping them is fine; clean-up when a project
> needs B is `POT! pme forget` plus an erase the owner runs). Cell on channel 11 (saved in NVS). Package `m8-svc` at
> counter 6 (next 7+). Open evidence items are listed in the code-state paragraph above.
>
> ## 2026-10-08, morning: M6.1 (CR-6) ACCEPTED on the bench (M0-LOG session 29), 14 min not 24 h.
> Combined node = 18-19/14-17 Mbps like the hotspot alone, 0 false deaths; router channel change 1 -> 11
> moved the whole cell in ~2.7 s after two fixes (`3d2333a` channel-authority sweep, `7ec8229` channel in NVS).
> Bench: A, C on `7ec8229`; B on the extender build of `7ec8229` WITH THE OWNER'S CREDENTIALS (house Wi-Fi +
> hotspot) -- clean-up per CR-6 is `POT! pme forget` then an erase the owner runs. Cell on channel 11 (saved).
>
> ## 2026-10-08, overnight: M8 ACCEPTED on the bench (M0-LOG session 28). Package `m8-svc` at counter 6 on all three
> (next: 7+). `python -m potluck.agent --port COM6 --service time`; `tools/m8_bench.py`. **OPEN, blocking M6.1 step 2:**
> ~~board B stopped transmitting~~ **run down and fixed (`28eeb1a`, M0-LOG session 28):** static TX buffers + AMPDU
> wedge ESP-NOW 2.5-5 min into a boot; the extender now keeps AMPDU with dynamic TX (Wi-Fi/lwIP out of PSRAM) -- one
> setting differs from step 0's baseline. The host now answers RTT probes (52-72 ms over the CP2102).
>
> ## 2026-10-08, later: M6.1 SCHEDULED (CR-6, "the application owns the radio"), ARCHITECTURE section 13. In progress.
>
> Run exactly as CR-6's "How to run CR-6" says. **Step 0 PASSED, step 1 BUILT** (M0-LOG session 27). **NEXT: step 2,
> which needs the owner** (credentials, phone, setup notes). Owner's morning steps: (1) close monitors; (2) run
> `& "C:\Users\tuebo\.espressif\python_env\idf6.0_py3.14_env\Scripts\python.exe" D:\Projects\potluck\tools\pme_set_wifi.py COM4 C:\Users\tuebo\pme-wifi.txt --potluck`;
> (3) in a monitor on COM4 type `POT! pme ap <hotspot name> <hotspot password>` and reset the board; (4) phone on
> the hotspot; the session then runs step 0's protocol while the M0 soak runs, and forces a router channel change.
> B runs the extender build (`build-extender`, flash with esptool `@flash_args` -- `idf.py flash` rebuilds without
> the -PmeHotspot path and fails on purpose). A, C on `f6d7fdb`.
> **Trust fix `f6d7fdb`:** the epoch fence is (certificate issue time, epoch); an erased, re-enrolled board is admitted. Erasing a board is the owner's to run (the permission
> system declines it for the session). The owner types every credential at `pme>` in their own monitor, then closes it;
> `tools/hs_capture.py` records only status lines. The `pme>` console wants CR LF and answers to `ESC[6n` -- see the
> log before scripting it. Never patch `pme_hotspot`: report bugs to the owner. Setup notes for step 2 still owed.
>
> ## HANDOVER, 2026-10-08: M6 ACCEPTED on three boards (M0-LOG session 26). PAUSED; when resumed, NEXT: M8.
>
> **Paused at the owner's request** after M6 was accepted and synced: the sibling project `poor-mans-extender`
> (`D:\Projects\poor-mans-extender`, read-only for this repo) wanted to do something after M6. Do not start M8
> until the owner says so; if that project sends a new change request, it goes into section 13 the way M5.1 did.
>
> M7 stays gated (no named workload needs WASM). M8 (host services, `potluck-agent`) was gated on M5 only, so it is
> next in section 13. Open soaks, not blocking: M5.1's 24 h streaming soak and the relay's 24 h soak.
>
> - **The bench now:** all three on **`fd1478d`**, which is the **PSRAM build** (`sdkconfig.defaults.esp32s3`
>   enables it; an S3 without PSRAM needs `CONFIG_SPIRAM=n`, which compiles the reconciler out). Enrolled, cell on
>   channel 1. Package **`m6-ticker` confirmed at counter 5 on all three** -- the next package must be **counter 6+**.
> - **M6 tooling:** `components/pot_reconcile` (node), `potluck.reconcile` (host: portability, eligibility,
>   gravity, the same HRW), `manifests/m6-ticker.json`, `tools/m6_bench.py failover|partition|analyze` (IDF
>   Python; consoles at **115200**, the COM6 frame link at **921600**). Stats lines `rec` (per actor) and `clk`
>   (board clock, printed first each period); events `actor_started`/`actor_stopped`/`actor_owner`.
> - **Internal core 49.4 KB of 64 KB** (14.6 KB headroom) since buffers that only tasks touch moved to PSRAM.
> - Design decisions go through the zero-assumption contract (memory: zero-assumption-for-design).
>
> ## HANDOVER, 2026-10-07 (late): M5 accepted, M5.1 complete except a 24 h streaming soak. (Was: NEXT: M6.)
>
> ## M5 ACCEPTED (handover, 2026-10-07)
>
> All five acceptance lines met on three boards (M0-LOG sessions 21-22, ARCHITECTURE section 13). **M5.1's code is
> complete (sessions 23-24): channel follow, runtime window, BUSY bit, PSRAM build, and the single-hop relay
> (ADR-009).** Left: step 0 (needs the owner: a repeater, his Wi-Fi password, a phone), the relay's 24 h soak. Was: **Next:
> M5.1 "sharing the radio"** (section 13, from `D:\Projects\poor-mans-extender\CR for Potluck.md`;
> read-only for us), starting with its zero-code experiment.
>
> - **The bench now (2026-10-07, after step 0):** all three on **`ff61f66`**, cell on channel 1, all three enrolled and
>   verifying each other. B was the step-0 repeater, then **flash-erased** (the owner's Wi-Fi credentials are gone), reflashed
>   and re-enrolled with a new key. The erase also reset B's deploy state: **B runs no module and its anti-downgrade floor
>   is 0; A and C run `m3-purple` at counter 4.** The next package (counter 5+) levels all three.
> - **Earlier:** all three on **`8032bc2`**, cell on channel 1, **all three enrolled** (B since session 24 --
>   it was the refused board until then). No deafness, no relay set. `POT! relay 1` on C and `POT! deaf <id>`
>   pairs recreate the relay test (`scratchpad`-style script in M0-LOG session 24). **A (COM3) and C (COM5) enrolled** (CA fp `ef76cc2e`);
>   **B (COM4) deliberately unenrolled** (the refused board). A and C run the deployed `m3-purple` at
>   **counter 4 — the next package must be counter 5+**. CP2102 on COM6 to A; CAN modules on B and C, idle.
> - **Deploy now needs the image signed:** `python -m potluck.ctl --port COM6 --node 6300 deploy
>   keys/<m>.pkg.json --ca keys/ca.pub --key keys/deploy.key --bcert keys/deploy.bcert` (`deploy.bcert`
>   made once by `python -m potluck.enrol --deploy-cert keys/deploy.pub --ca-key keys/ca.key --out ...`).
> - **Test instruments on the console** (`POT! write <node> <int>`, `POT! replay [flip]`, `POT! safe <n>`,
>   `POT! replay-ss`) and the `act/setpoint` stand-in actuator; stats lines `auth`, `act`, `ss`.
> - **Static DRAM 63.6 KB of 64 KB.** Nothing more fits. The `psram` variant exists (`-Variant psram -Extra
>   "CONFIG_SPIRAM=y","CONFIG_SPIRAM_MODE_OCT=y","CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y"`); app buffers go there.
> - **Console for M5.1:** `POT! window <ms> <misses>`, `POT! busy 0|1`, `POT! channel <n> [quiet]`, `POT! fixed 0|1`;
>   `{"t":"peers"}` line shows channel, scan state and each peer's window and busy bit.
> - **Crypto runs on the console task** (`NodeHal::run_heavy`); never call Monocypher from the link task.
>   The console serves jobs while it waits for the node mutex; keep it that way or the two deadlock.
> - **Owner constraints some evenings:** no Ollama, WSL or Docker (portability gate via Git Bash's
>   Xtensa fallback). **New design decisions go through the zero-assumption contract**
>   (github.com/Tubifix77/zero-assumption): primary sources, registered in the ledger before use.
>
> ## M4 ACCEPTED ON TWO BOARDS — READ THIS FIRST (handover, 2026-10-05)
>
> **Result** (M0-LOG session 20): B flooded a two-board CAN bus at 3,072 frames/s for 5 min while C sent
> SAFE_STATE every second. 301 consecutive SAFE_STATEs arrived; each left C within **335–683 µs**; C never
> lost arbitration (B lost 44); zero bit/form/stuff errors. The first run found SAFE_STATE queued 2.5–5.5 ms
> behind its own node's probe (the TWAI driver is FIFO), so `pot_can` now transmits in priority order,
> one frame at a time (`CanTxQueue`, `2b32274`). Evidence: `captures/m4-can-two-board-*`.
>
> **Accepted by the owner with one caveat:** the milestone says "on a scope"; the bench has no scope or
> logic analyzer and only female-female jumpers (so board A cannot tap the lines either). The trace stays
> a **[MEASURE]** for if a USB logic analyzer turns up. Also queued from this run: C's ISR receive queue overflows at ~3,000 frames/s
> (220 of 881,583, no message lost); the flood demo starves its own probes (`tx_no_slot`);
> `ss_rx_restarts` (`e8fd0d7`) is compile-checked only.
>
> **The bench now:** all three boards run the normal radio build of **`e8fd0d7`** (ELF `637eef762168…`),
> verify-flash matched on each; the cell is up (two peers alive everywhere). The SN65HVD230 modules are
> still wired to B and C (GPIO4/5; the normal build leaves those pins alone). CP2102 on COM6 for A.
>
> **Tools:** `tools/can_capture.py` (reset-free read of COM4/COM5, needs the IDF Python for pyserial),
> `tools/can_report.py <log>`. **Variant builds:** `tools\build_firmware.ps1 -Variant can-flood -Extra
> "CONFIG_POT_RADIO_DISABLE=y","CONFIG_POT_CAN=y","CONFIG_POT_M4_FLOOD=y" -Flash -Port COM4`; `can-safe`
> uses `"CONFIG_POT_M4_SAFE_STATE_MS=1000"` instead of FLOOD. Always run builds from PowerShell directly.
>
> ## M0-M3 ARE ACCEPTED. (M3 was accepted on `0dd9645`; the boards have since moved to `e8fd0d7`.)
>
> **Deploy:** keys in `keys/` (gitignored: `ca.pub`, `deploy.key`, `deploy.cert`). Sign with
> `python -m potluck.signing sign manifests/<m>.json --key keys/deploy.key --cert keys/deploy.cert
> --counter N --out keys/<m>.pkg.json`; deploy with `python -m potluck.ctl --port COM6 --node 6300 deploy
> keys/<m>.pkg.json --ca keys/ca.pub`. **The boards' anti-downgrade floor is counter 3**: sign the next
> package at 4+. **Never leave the CP2102 wired to a board unless the adapter is itself plugged in.**
>
> **The frame link:** CP2102 (HW-598A) on **COM6**, wired adapter TXD→A's GPIO18, RXD→A's GPIO17,
> GND→G (`bench/m1-wiring.html`). `python -m potluck.ctl --port COM6 --node <id> watch sys/uptime`.
> **Build from PowerShell directly** (through bash it silently did nothing), `-Clean` after a commit
> to re-stamp the version, and verify app version + ELF SHA on every flashed board.
>
> ## How M0 was accepted
>
> **The soak** (2026-10-02/03, fw `095f21e`, 24.52 h, three boards ~10 cm apart): zero gaps, reboots or
> deaths; unicast PDR 100.0000 % both ways on all six links; 264,850 RTTs, p50 4–6 ms, p99 16–22 ms,
> none above 85 ms; `free_dram` flat to the byte. M0-LOG session 13, `captures/soak-2026-10-02-report.txt`.
>
> **The sweep** (2026-10-04, fw `a652cab`, A and C fixed at the PC, B walking on mains power): eight
> spots through a ~10 × 20 m two-storey house, up to ~16 m, four walls, a floor, a dryer-and-fridge
> shadow. Unicast 100 % at six spots, worst 99.17 % / 99.80 %; worst p99 RTT 30–42 ms; no cliff. The
> indoor edge is **brief membership dropouts** (9 revivals in 14 min at the worst spot), and deaths
> only appeared where median RSSI was −73 dBm or weaker. **Orientation and obstacles outweigh
> distance**: the farthest spot was clean, the one behind the appliances was the worst. M0-LOG
> session 15, `captures/sweep-2026-10-04-report.txt` and `-notes.md`.
>
> **All three boards run `a652cab`** (ELF SHA `17437a55a…`): status LED (blue = no peer, green < 10 %
> heartbeat loss, yellow 10–50 %, red > 50 % or a peer dead; red/green/blue self-test at power-up),
> `bcast_frames` in the link record, and the `on_tx_done` race fixed. Nothing is capturing now.
>
> **Tools:** `tools\soak.ps1` (Task Scheduler captures; survives the app, not logoff/shutdown/sleep),
> `tools\soak_report.py`, `tools\sweep_report.py --walker <id>`. **Smoke-test any long run** by reading
> its first ten minutes with the report tool. **Live checks must count deaths from the event stream,
> never from the 10 s sampled state** — during the sweep that mistake hid 9 deaths.
>
> **Queued:** re-run `pot_sim --sweep` on these measured points instead of §3's borrowed farmland
> figures; the `reorder_dup` admission artefact; `peer_admitted` firing every `hello_interval_ms`.

Two entry points, depending on what is on the desk:

- **Boards have arrived** -> [WHEN-THE-BOARDS-ARRIVE.md](WHEN-THE-BOARDS-ARRIVE.md), the bench
  resumption path, written for a reader who remembers nothing. Read its unboxing notes *before*
  plugging anything in: the two USB-C sockets are visually identical and one of them is dead in this
  firmware.
- **Still waiting** -> [history/HARDWARE-FREE-PLAN.md](history/HARDWARE-FREE-PLAN.md). **All seven steps H0-H6 are
  done** (2026-08-23). M2 is accepted, half of M4 is accepted, and section 7.8's compute pattern is
  measured in simulation. What is left in the milestones is hardware and only hardware; if a session
  wants more to do without boards, the honest answer is that the queue is empty and the next thing
  is a vision decision, not an engineering one.

## The name

**The project was called dμOS until 2026-08-01.** The Greek mu broke five separate tools (Xtensa GCC's
argv, esp-idf-kconfig, QEMU's `-serial file:`, Python's stdout on CP1252, and the size checker), so it
is gone from everything except the historical documents below. µTorrent made the same retreat.

Naming now in force, all ASCII:

| surface | form |
|---|---|
| project, docs, prose | **Potluck** |
| C++ namespace, ESP-IDF components, Kconfig | `pot::`, `pot_frame`/`pot_link`/`pot_espnow`/`pot_ns`, `CONFIG_POT_*` |
| namespace URI scheme | `potluck://lab/node-1a2b/sys/uptime` |
| host Python package | `potluck` (in `host/potluck/`) |
| CLIs | `potctl` (read/write the namespace), `potluck-capture` (record a soak) |
| wire format | Potluck Frame v1 |

`pot` short-form is for identifiers; `potluck` spelled out is for anything a person types or reads.
**Do not reintroduce `dmu` anywhere** except when quoting history.

## Reading order

- **[ARCHITECTURE.md](ARCHITECTURE.md) is the single source of truth.** Decision-closed v1.
- **[M0-LOG.md](M0-LOG.md)** — the decision log, newest session last. Read its latest session before
  picking the project up; it records what is wrong as well as what is done, including withdrawn
  conclusions.
- **[BEYOND-M12.md](BEYOND-M12.md)** — the owner's next idea (2026-10-11): a hub, a dashboard and integrations, as a
  discussion draft. Not decided: discuss it with the owner before any of it becomes a milestone in ARCHITECTURE section 13.
- **[M0-RUNBOOK.md](M0-RUNBOOK.md)** — how to build, flash, emulate and measure.
- **[DEVELOPING-APPS.md](DEVELOPING-APPS.md)** — the developer manual for app authors (M12, rewritten for `potluck apply`
  2026-10-11, blind-tested twice); its worked example `examples/apps/pot_hello` is built and tested by the gates. Keep it true:
  a change to the actor API, the descriptor or the deploy steps changes the manual in the same commit.
- The three `history/dmuOS_Architecture_Vision_Document*.pdf` are **superseded history** — mine them for
  intent, never for facts. Several of their numbers are corrected in ARCHITECTURE.md §3.1. They keep
  their old filenames deliberately: they really were written about dμOS, and renaming a historical
  artefact misrepresents it.
- `history/first-analysis.md` — the external review (Fable 5 Max) that motivated the merge.
- `history/second-analysis.md` — adversarial re-read of §2/§4/§8.3. **Resolved 2026-08-01** — all twelve
  findings applied to ARCHITECTURE.md (see its Resolution section). Do not re-derive them.
- `history/duOS.png` / `history/duOS2.png` — mood boards from the old name; the second has garbled AI text layers.

## Rules for any session in this repo

1. **The owner supplies vision, not verdicts.** Sessions make engineering decisions autonomously
   *inside* the vision invariant (ARCHITECTURE.md §0.1: a hardware- and domain-agnostic
   "Kubernetes for CPU/GPU/RAM *and* attached hardware", ESP32 plus larger machines). Escalate
   exactly one thing: a knowing deviation from that invariant. Do **not** ask the owner to pick
   domains, policy defaults, or menu options — domain choices live in deploy manifests, and
   verdict-seeking questions are themselves a signal the idea got lost in processing.
   Owner tests arrive as **example systems** (ARCHITECTURE.md §1.2, "the test fleet"); the
   standing rule is falsification — if a named system is not possible under the architecture,
   that is an architecture bug to fix, never a scope answer.
2. **The ADRs are closed** (eight until 2026-10-07; ADR-009, the single-hop relay, was added for M5.1 without
   touching them). Reopen one only when its stated *revisit trigger* fires, and
   record the change in the ADR itself — never fork the decision elsewhere in the document.
3. **Every factual claim goes through the zero-assumption ledger** at
   `.claude/zero-assumption/memory.md`: live lookup → cite → register. No numbers from model
   memory. Items tagged **[MEASURE]** are bench work — do not resolve them from search.
4. **Scope discipline is the standing risk** (ADR-001, §14). ESP32 family only; ESP-NOW + UART
   before CAN; M7 and M8 stayed gated until a named need opened each (M8 on 2026-10-08, M7 on 2026-10-11). If a
   session drifts toward new MCU families, transports, or
   runtimes, stop and point at ADR-001.
5. **M0 outranks further spec work.** Two boards exchanging measured heartbeats beats any amount
   of additional documentation.
6. Design budgets and measured facts stay labelled as such (§6 pattern). A number without a
   source or a **[MEASURE]** tag is a defect.
7. **Prove the artefact under test is the one you built.** A stale QEMU flash image once made a
   working fix look broken and sent a whole session chasing a backtrace decoded against the wrong
   binary. `tools\decode_backtrace.ps1` refuses to decode on an ELF-SHA mismatch — take the refusal
   seriously. Related: one stack sample locates execution, it does not diagnose a hang; prefer a
   bisection (`tools\run_qemu.ps1 -Extra "CONFIG_..."`) that removes a suspect.

## Safety line

Potluck must never be the only thing between a motor and a person (§12). Anything involving actuated
force near humans goes to a functional-safety engineer, not into this document.
