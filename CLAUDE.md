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
**M0's accept line is met (2026-10-03)** — a 24.52-hour soak on three boards; its kill line waits on
the distance sweep.

**THE HARDWARE HAS ARRIVED — 2026-10-01, and all three boards are up.** Ordered 2026-09-21 as a shared
AliExpress order with the sibling Powersuit project (DKK 408), covering Potluck's M0/M4 and
Powersuit's Appendix A bench at once.

Board A — MAC `b8:1f:3f:da:63:00`, therefore **node 0x6300** — enumerated as **CH343 (COM3)**, and is
**flashed with Potluck and running** (fw `095f21e`). Its factory image is backed up. The board
register and the bring-up findings are in [WHEN-THE-BOARDS-ARRIVE.md](WHEN-THE-BOARDS-ARRIVE.md).

> **§6's Wi-Fi DRAM [MEASURE] is CLOSED (2026-10-01): 32,264 B — 31.5 KiB, under the ~40 KB
> trigger, so the RX ring does not shrink.** That number had been open since the architecture was
> written, is named in §13-M0's acceptance table, and was the reason nothing past M2 was built:
> 11.8 KB of the budget sat committed on paper against a figure nobody had. It now survives contact
> with reality. Evidence: `captures/boardA-first-boot-095f21e.log`.

> ## THE 24-HOUR SOAK IS DONE. M0'S ACCEPT LINE IS MET. NEXT: THE DISTANCE SWEEP.
>
> Run 2: 2026-10-02 00:31 to 2026-10-03 01:03, **24.52 h**, three boards in a row ~10 cm apart in
> the order **C B A**, firmware `095f21e`. Report: `captures/soak-2026-10-02-report.txt`; write-up in
> M0-LOG session 13. Tasks stopped and removed; nothing is capturing now.
>
> | | |
> |---|---|
> | integrity | zero gaps longer than the 10 s sample, zero reboots, zero deaths |
> | PDR | **100.0000 %** outbound (791,324, MAC ACK) and inbound (6,337,757, seq gaps), all six links |
> | delay | 264,850 RTTs: p50 4–6 ms, p99 16–22 ms, p99.9 42–60 ms, none above 85 ms, not bimodal |
> | heartbeat | 99.69–99.84 % per link, *reconstructed* (the firmware never emits `rx_bcast_frames`) |
> | memory | `free_dram` ends on the same byte it started on, all three nodes |
>
> **The sweep is the kill criterion** — the only test that can reopen the transport decision — and it
> runs on **the same binary** as the soak, so the two differ only in geometry. Record orientation as
> well as distance and hold orientation fixed (at 10 cm it outweighed distance). See
> WHEN-THE-BOARDS-ARRIVE.md. **Smoke-test it:** read its first minutes with `tools\soak_report.py`
> before trusting a long dwell.
>
> **Four firmware changes are queued behind the sweep, not ahead of it:** emit `rx_bcast_frames`;
> the `on_tx_done` race that credits an earlier frame's completion to the probe (makes `txq_max_us`
> wrap to ~4.29e9; RTT unaffected); the `reorder_dup` admission artefact (~20 per earlier-booted
> peer, never recurs); and `peer_admitted` firing every `hello_interval_ms` for an admitted peer.
>
> **Run 1 died at 3 h 27 m** because `Start-Process` left the captures inside the desktop app's job
> object, and the app restarted. `tools\soak.ps1` now uses Task Scheduler. Survives the app and this
> session; does **not** survive logging off, shutdown or sleep. Closing one of its console windows
> kills that capture.

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
2. **The eight ADRs are closed.** Reopen one only when its stated *revisit trigger* fires, and
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
