# Potluck -- a guide for AI assistants working in this repository

Potluck is a distributed runtime that makes a cluster of small machines behave like one system: ESP32-S3 boards (and,
optionally, a PC) share their sensors, compute and radios as one "cell". Think "Kubernetes for CPU, RAM *and* attached
hardware". Version 1.0.0, the S3 Edition: the tested hardware is the ESP32-S3 with 16 MB flash and 8 MB PSRAM; the host
tools are tested on Windows 11. Licence: Apache-2.0 (NOTICE lists third-party code).

## Where things are

| need | read |
|---|---|
| what it is, what works, the failure contract | [README.md](README.md) |
| first cell, from boards to a running app | [GETTING-STARTED.md](GETTING-STARTED.md) |
| writing an app (actors, descriptor, manifests, guests, tests) | [DEVELOPING-APPS.md](DEVELOPING-APPS.md) |
| the hub's HTTP API, integrations, Home Assistant | [host/potluck/HUB-API.md](host/potluck/HUB-API.md), [INTEGRATIONS.md](INTEGRATIONS.md), [HOME-ASSISTANT.md](HOME-ASSISTANT.md) |
| the design, its decisions (ADRs) and the milestone record | [ARCHITECTURE.md](ARCHITECTURE.md) |
| what is accepted, with measurements and caveats | [STATUS.md](STATUS.md) |
| build, flash, emulate, measure (operator's manual) | [M0-RUNBOOK.md](M0-RUNBOOK.md) |
| the decision log, newest last | [M0-LOG.md](M0-LOG.md) |

`history/`, `BEYOND-M12.md` and `WHEN-THE-BOARDS-ARRIVE.md` are history: mine them for intent, never for current facts.

## Layout

- `firmware/` -- the ESP-IDF project (ESP-IDF v6.0.2): `components/pot_*` (frame, link, namespace, deploy, trust,
  reconcile, actors, ...), `main/` (the node program and its actor table).
- `host/potluck/` -- the Python package (3.10+, tested on 3.14): `apply` (build, roll out, deploy, verify), `ctl`
  (potctl), `hub` (the PC's resident program and dashboard), `setup` (the wizard), `store`, `release`, ...
- `tests/` -- the C++ host tests (`pot_tests`); `host/potluck/tests/` -- the Python tests; `sim/` -- `pot_sim`.
- `custom_components/potluck/` -- the Home Assistant integration (HACS); `integrations/node-red/` -- Node-RED nodes.
- `examples/` -- an example app (`examples/apps/pot_hello`), example cluster files, an example host program.

## Build and test

- Host tests (C++ and Python, no hardware): `tools\run_host_tests.ps1` and `tools\run_all_tests.ps1` (PowerShell 7).
- Firmware: `tools\build_firmware.ps1` (finds ESP-IDF from `-IdfPath`, else `IDF_PATH`). Normally `potluck apply`
  builds it: `python -m potluck.apply <cluster file> --plan` from `host/potluck/`.
- Without boards: `tools\run_qemu.ps1` runs a node under QEMU; `pot_sim` simulates cells.

## Conventions

- Names: `pot` for identifiers (`pot::`, `pot_link`, `CONFIG_POT_*`), `potluck` spelled out for anything a person
  types or reads. The project was called dμOS until 2026-08-01; do not reintroduce that name or `dmu`.
- Many files are CRLF and some LF: keep each file's line endings when editing.
- ARCHITECTURE.md's ADRs are closed decisions: reopen one only when its stated revisit trigger fires, and record the
  change inside that ADR.
- A number in the design documents is either measured (with its source) or a labelled budget / **[MEASURE]**.
- A change to the actor API, the module descriptor or the deploy steps updates DEVELOPING-APPS.md in the same commit.
- Prove the binary under test is the one you built (versions and ELF hashes are checked; `tools\decode_backtrace.ps1`
  refuses an ELF mismatch).
- Never commit keys, credentials or Wi-Fi settings: `keys/` is git-ignored, and a board's console log can name the
  networks it uses.

## Safety line

Potluck must never be the only thing between a motor and a person (ARCHITECTURE.md section 12). Anything involving
actuated force near people belongs with a functional-safety engineer, not in this repository.
