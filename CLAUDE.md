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
**M0, M1, M2 and M3 are ACCEPTED on hardware (2026-10-04); M4 on 2026-10-05; M5 on 2026-10-07; M6 on 2026-10-08.** M3: deploy to the cell through one board,
detach and power-cycle, and a broken module that all three nodes revert themselves (M0-LOG session 18). M2: a 10.05-minute three-board session over COM6
replays to a byte-identical 18-entry namespace (M0-LOG session 17). M0: a 24.52-hour soak, then a distance sweep through
the owner's two-storey house in which the kill criterion did not fire. M1: potctl on a CP2102 cabled to
board A reads board B's value; unplugged, it reads `UNAVAILABLE`. M0-LOG session 16. **M4 is ACCEPTED (2026-10-05)**, with
one caveat: the arbitration was not recorded by an instrument outside the boards (M0-LOG session 20). **M5 and M6 are
accepted too** (sessions 21-22 and 26). **PAUSED after M6 (2026-10-08)**: the owner's `poor-mans-extender` project
wanted to do something once M6 was done, so Potluck waits for the owner. When it resumes, the roadmap's next item is M8.

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
- **Still waiting** -> [HARDWARE-FREE-PLAN.md](HARDWARE-FREE-PLAN.md). **All seven steps H0-H6 are
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
- **[M0-RUNBOOK.md](M0-RUNBOOK.md)** — how to build, flash, emulate and measure.
- The three `dmuOS_Architecture_Vision_Document*.pdf` are **superseded history** — mine them for
  intent, never for facts. Several of their numbers are corrected in ARCHITECTURE.md §3.1. They keep
  their old filenames deliberately: they really were written about dμOS, and renaming a historical
  artefact misrepresents it.
- `first-analysis.md` — the external review (Fable 5 Max) that motivated the merge.
- `second-analysis.md` — adversarial re-read of §2/§4/§8.3. **Resolved 2026-08-01** — all twelve
  findings applied to ARCHITECTURE.md (see its Resolution section). Do not re-derive them.
- `duOS.png` / `duOS2.png` — mood boards from the old name; the second has garbled AI text layers.

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
   before CAN; M7/M8 are gated. If a session drifts toward new MCU families, transports, or
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
