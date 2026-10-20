# Contributing to Potluck

Potluck is a small project with a strong method. This page is the short version: what must stay true, how a change is
judged, and how to build and test one. The design itself is [ARCHITECTURE.md](ARCHITECTURE.md); what is accepted, with
its measurements and caveats, is [STATUS.md](STATUS.md).

## What must stay true

These are the project's invariants. Each points at where the design states it; a change that breaks one is a design
change, discussed first, not a pull request.

1. **A value never travels without its quality.** Every read returns the whole tuple -- value, unit, timestamp, age,
   latency class and quality (ARCHITECTURE.md section 4, rule 2; section 7.2). A stale or unavailable reading is
   delivered *as* stale or unavailable, never as a bare number. Uncertainty is shown, not hidden.
2. **Cost is explicit.** Naming is location-transparent, cost is not: a binding that crosses a hop its latency class
   does not budget for is refused at build time, naming both ends (section 4, the Locality Contract; ADR-002).
3. **Contracts stay stable.** The wire format (Potluck Frame v1, section 5), the namespace paths a board derives from
   its node id (`node-<id>/sys/...`), the module descriptor and the hub's API version are contracts other code relies
   on. Extend them; do not change what an existing field or path means. A change to the actor API, the descriptor or
   the deploy steps updates [DEVELOPING-APPS.md](DEVELOPING-APPS.md) in the same commit.
4. **Deployment stays reversible.** A package or firmware image runs on trial and is kept only after it proves itself;
   otherwise the board reverts by itself (section 7.4, steps 4-6). Anti-downgrade counters only move forward. Nothing
   ships that a board cannot back out of.
5. **Evidence beats assumption.** A number in the design documents is either measured, with its source, or labelled a
   budget or **[MEASURE]** (section 6's pattern). Prove the binary under test is the one you built: versions and ELF
   hashes are checked, and `tools\decode_backtrace.ps1` refuses an ELF mismatch.
6. **Scope is a decision.** The ESP32 family first, ESP-NOW and UART before CAN, no new runtime or transport without a
   named need (ADR-001, section 14). The closed decisions (section 10) are reopened only when their stated revisit
   trigger fires, and the change is recorded inside that decision.
7. **The safety line.** Potluck must never be the only thing between a motor and a person (section 12). Anything
   involving actuated force near people belongs with a functional-safety engineer, not in this repository.

## How a change is judged

- **A named need.** New work answers a real system that cannot be built today (section 1.2's test fleet), not an
  architecture for its own sake.
- **A test that would have failed without it.** Bugs get a regression test written from the failure; features get tests
  of their contract, including the refusals.
- **The bench, where it matters.** Radio, timing and memory claims are measured on boards and written down with the
  capture they came from (M0-LOG.md is the decision log, newest session last).
- **Honest status.** STATUS.md says what is accepted and with which caveat; a fix in code that has not been shown on the
  bench says so.

## Build and test

Everything below runs without boards (Windows 11, PowerShell 7; ESP-IDF v6.0.2 for the firmware):

| what | how |
|---|---|
| C++ host tests (the firmware's portable code, on the PC) | `tools\run_host_tests.ps1` |
| every suite, C++ and Python | `tools\run_all_tests.ps1` |
| the firmware | `tools\build_firmware.ps1` (or `python -m potluck.apply <cluster file> --plan` from `host/potluck/`) |
| a node without a board | `tools\run_qemu.ps1`; cells of many nodes: `pot_sim` |

Conventions: `pot` for identifiers (`pot::`, `pot_link`, `CONFIG_POT_*`), `potluck` spelled out for anything a person
types or reads. Many files are CRLF and some LF: keep each file's line endings. Never commit keys, credentials or Wi-Fi
settings -- `keys/` is git-ignored, and a board's console log can name the networks it uses.

## Limits worth knowing before you design

One cell is at most 20 boards (a hardware peer limit, section 3; ESP-NOW's 20 peers less the broadcast entry), a board's
namespace holds 128 entries (`potluck apply --plan` counts them and refuses more), and larger systems are cells of cells
(M19). STATUS.md has the measurements behind each.

Licence: Apache-2.0 ([LICENSE](LICENSE), [NOTICE](NOTICE)); a contribution is made under it (its section 5).
