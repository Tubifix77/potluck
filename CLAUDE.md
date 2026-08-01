# Potluck — project context

Distributed runtime that makes a cluster of small machines behave like one: everyone brings a dish —
a sensor, some RAM, a radio — and the cluster eats together. "Kubernetes for CPU/GPU/RAM *and*
attached hardware."

**Current release: Potluck — S3 Edition.** The name of the *hardware support*, not of the project:
this edition targets ESP32-S3, with host tooling on Windows and Linux. Later editions add targets;
the architecture is deliberately hardware-agnostic (ARCHITECTURE.md §0.1) and the edition label exists
so that stays visible.

Code state: M0 firmware complete and M1/M2 verified against real firmware under emulation. **M0 is not
yet *accepted*** — §13-M0's acceptance table needs two boards and a radio, which is the only thing
now blocked on hardware.

**The project is paused (2026-08-01) until at least one ESP32-S3 is on a USB cable.** If hardware
has arrived, start at **[WHEN-THE-BOARDS-ARRIVE.md](WHEN-THE-BOARDS-ARRIVE.md)** — it is the
resumption path, written for a reader who remembers nothing. Do not start M3+ in the meantime; the
reasons are in that file and in M0-LOG.md Session 6.

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
