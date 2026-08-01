# Potluck — S3 Edition

**A distributed runtime that makes a cluster of small machines behave like one.** Everyone brings a
dish — a sensor, some RAM, a radio — and the cluster eats together.

*e(SP)luribus unum — out of many, one machine.*

---

## The idea

An ESP32 is far too powerful to spend its life watching a flowerpot — but there it sits, because its
pins are wired to the flowerpot. That is **stranded compute**, and every home, car and workshop is
full of it.

Potluck federates those machines into one. It is Kubernetes for CPU/GPU/RAM ***and* attached
hardware**: an application is written against the cluster, never against a board. It refers to

```
potluck://car/lights/rear/left
```

not to "ADC channel 1 on the board behind the utility cupboard". Ten ESP32s, a Raspberry Pi, twenty
lights and a dash screen ship as **one signed application package for the whole car**; actors that
must sit next to their hardware pin themselves there, and everything portable is placed by a
constraint solver at build time. If the node behind the rear bumper dies, its portable actors
re-activate elsewhere within seconds. The lamp itself is gone until the node returns — physics — and
every read of it *says so, loudly*.

That last sentence is the design's centre of gravity:

> **A read never returns a bare value.** Every read yields
> `(value, unit, timestamp, age, class, quality)` — and past a resource's staleness bound the value
> is still delivered, marked `STALE`, with its exact age. The one banned act is handing back old data
> *unmarked*: a location-transparent read that silently serves a 400 ms-old sensor value is how
> distributed control systems hurt people.

The rule is enforced in the type system on both sides of the wire. The C++ `Reading` has no accessor
that returns the number alone; in Python, `float(reading)` raises on purpose; and `potctl`
deliberately has **no flag to print just the value**, because such a flag ends up in a script that
has lost the age.

## What "S3 Edition" means

It names the **hardware support, not the project**. This edition targets the **ESP32-S3**
(bench fleet: 7 × ESP32-S3-DevKitC-1 N16R8) with host tooling on **Windows and Linux**. The
architecture is deliberately hardware-agnostic — no CPU is second-class, and relative performance is
a placement input, never an exclusion rule — so later editions add targets without the name having
baked one vendor's silicon in. (It is also why the ESP32 pun lives in the tagline and not in the
project name.)

## Honest status — read this before judging anything else

This project's standing rule is that a number without a source is a defect, so here is exactly where
it stands as of **2026-08-01**:

| | state |
|---|---|
| Architecture | decision-closed v1, eight ADRs with revisit triggers ([ARCHITECTURE.md](ARCHITECTURE.md)) |
| M0 firmware (frame codec, heartbeats, membership, link stats) | **built**, runs under QEMU emulation |
| M1 namespace (`READ`/`WRITE`/`REPLY`, staleness, six built-in resources) | **built and exercised against the real firmware** under emulation |
| M2 host bridge (`potctl`, capture, replay) | **built**; replay proven to rebuild byte-identical namespace state from a captured session, gate and all |
| Test gates | **19 green**: 158 C++ cases / 36,536 checks, 133 Python cases, three independent wire-format implementations agreeing byte-for-byte over generated corpora, AddressSanitizer, a strict-GCC portability gate, and the firmware build with its memory-budget check |
| Static memory | **53,118 B** of the 64 KB core cap, measured per build |
| **M0 *accepted*** | **No. Two physical boards have never exchanged a heartbeat.** The acceptance table (24-hour soak, measured PDR, round-trip delay histogram, Wi-Fi DRAM) needs boards and a radio, and QEMU emulates neither. The boards are in the mail. |

Nothing above claims a measurement that was not made. Emulated runs stamp `"no_radio":1` on every
statistics line precisely so they can never be mistaken for the soak.

## Ninety seconds of architecture

```
  application actors            one signed package per system,
      │                         placement frozen at build
      ▼
  namespace  potluck://…        typed, timestamped, staleness-checked reads
      │                         (value, unit, timestamp, age, class, quality)
      ▼
  Potluck Frame v1              16-byte header, auth bytes reserved from day one
      │
      ├── ESP-NOW               100 ms broadcast heartbeat beacon, dead at 6 misses;
      │                         round-robin unicast probe → RTT histogram
      ├── UART / USB-serial     COBS + CRC-16/CCITT-FALSE; the host joins as an
      │                         ordinary peer, not a special case
      └── CAN                   single-frame profile via 29-bit extended ID (M4)
```

Design choices with teeth:

- **No one-way latency figures, ever.** Two boards have unsynchronised clocks, and ESP-NOW may retry
  a frame 31 times in one direction and none in the other. Potluck measures round trips and never
  divides by two.
- **The heartbeat is O(N), not O(N²).** A broadcast beacon plus a round-robin probe: at 7 nodes the
  naive full mesh needs 865 frames/s and ~74% airtime; the beacon needs 109 and ~8.7%, with zero
  false death declarations in simulation.
- **A dead node's value is `UNAVAILABLE`, not "stale".** Old-but-attributable and gone are different
  facts, and conflating them is how a cached number gets trusted.
- **The host is not special.** `potctl` says HELLO, heartbeats, and is admitted like any peer —
  a mode transition is a non-event, which only holds if nothing above the transport can tell who is
  who.
- **Capture and replay are load-bearing.** Distributed embedded bugs are not reproducible by hand,
  so every frame can be teed to a capture file, and a replayed capture must rebuild **byte-identical
  namespace state**, checked by SHA-256 digest with a gate that exits non-zero on mismatch.

## Try it with no hardware at all

Prerequisites: [ESP-IDF v6.0.2](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/)
(includes the QEMU fork via `idf_tools.py install qemu-xtensa`), Python ≥ 3.10, and a host C++
toolchain. The scripts are PowerShell-first; `tools/run_all_tests.sh` exists for Linux but the
PowerShell path is the one exercised daily.

```powershell
# The gates: C++ suite, ASan, three differential corpora, eleven Python suites,
# portability, firmware build + memory budget.
tools\run_all_tests.ps1 -Asan -Firmware

# Boot the real firmware under QEMU with its frame link exposed as a TCP socket:
tools\run_qemu.ps1 -Seconds 60 -NodeId 4097 -FrameLinkPort 5555
```

then, from `host/potluck/`, talk to it:

```
> python -m potluck.ctl --tcp 127.0.0.1:5555 --node 1001 ls
# node 0x1001, boot epoch 2, death window 100 ms x 6 = 600 ms
free internal DRAM  349296 B  [GOOD, age 1576 ms, ts 10531, class L4]
largest free block  286720 B  [GOOD, age 1680 ms, ts 10531, class L4]
uptime              10 s      [GOOD, age 1790 ms, ts 10531, class L4]
boot epoch          2         [GOOD, age 1894 ms, ts 10531, class L4]
peers alive         0         [GOOD, age 1961 ms, ts 10531, class L4]
worst peer RSSI     0         [GOOD, age 2016 ms, ts 10531, class L4]

6/6 answered with a usable value
```

Every line is the full read tuple. `watch sys/peers-alive` shows `0 → 1` as your own host registers
as a live peer — the membership state machine running over a wire, with the firmware unaware the
peer is a laptop. Add `--capture session.jsonl` and the session prints the digest command that
verifies its own capture. One `potctl` per emulated run: QEMU's socket serial accepts a single
connection per VM lifetime ([M0-RUNBOOK.md](M0-RUNBOOK.md) has the details and the other emulator
sharp edges).

With boards on the bench, the same tool talks over `--port COM7` with none of those limits, and
[M0-RUNBOOK.md](M0-RUNBOOK.md) is the step-by-step for the acceptance soak.

## Reading order

| file | what it is |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | **The single source of truth.** Decision-closed v1: the namespace, the read contract, the wire format, memory budgets, eight ADRs, falsifiable milestones M0–M8 with accept/kill criteria |
| [M0-LOG.md](M0-LOG.md) | The decision log, newest session last — including the conclusions that were later **withdrawn**, kept struck-through rather than deleted. The QEMU sessions are a study in how a stale flash image manufactures false evidence |
| [M0-RUNBOOK.md](M0-RUNBOOK.md) | Bench procedure: build, flash, emulate, soak, and the delay methodology to read *before quoting any number* |
| [CLAUDE.md](CLAUDE.md) | Standing rules for working on this repo (largely built with [Claude Code](https://claude.com/claude-code), which these rules keep honest) |
| [.claude/zero-assumption/memory.md](.claude/zero-assumption/memory.md) | The evidence ledger: every external fact used, with source, retrieval date and status — including the withdrawn ones |
| `first-analysis.md`, `second-analysis.md` | External and adversarial reviews; all findings resolved into ARCHITECTURE.md |
| `dmuOS_Architecture_Vision_Document*.pdf` | Superseded history — mine for intent, never for facts. They keep their old filenames because they really were written about dμOS, and renaming a historical artefact misrepresents it |

## Method, briefly

The repo runs on a few rules that shaped everything in it: every external fact is looked up live,
cited and registered in the ledger — no numbers from model memory or anyone else's; design budgets
and measured facts are labelled as which they are, and bench work is tagged `[MEASURE]` rather than
resolved from search; and the artefact under test must be *proven* to be the artefact you built —
`tools/decode_backtrace.ps1` refuses to decode a panic against an ELF whose hash disagrees with the
log, a rule that exists because breaking it once cost a full day chasing a fix that had already
worked.

## The name

The project was **dμOS** until 2026-08-01. The Greek mu broke five separate tools — Xtensa GCC's
argv handling, `esp-idf-kconfig`, QEMU's `-serial file:`, Python's stdout on CP1252, and a size
checker — each in a different way, none with a useful error. µTorrent made the same retreat.
"Potluck" says what the system does in one word, and every character of it survives every toolchain.
The renamed workaround code is still in the build script, self-disabling, for whoever clones this
under a path with an accent in it.

## The failure contract

Potluck injects behaviours into every system built on it, and an application author inherits them
without seeing them — so whether they are acceptable for a given machine is not decidable unless the
runtime states them. Here they are:

- **A dead node is declared dead after 600 ms** — six missed heartbeats at the default 100 ms period
  (both configurable). Until that window closes, the cluster still believes the node is alive.
- **Stale values are delivered, marked.** Past a resource's staleness bound a read still returns the
  last value, with its exact age and quality `STALE` (the default "informative" policy; a resource
  declared "strict" withholds the value instead). A dead owner's resources read `UNAVAILABLE` — no
  value at all, because a dead node's last number is not merely old, it is unattributable.
- **The radio drops and delays frames as a matter of course.** A frame may legitimately spend
  ~100 ms in ESP-NOW's retry machinery before being abandoned, and delivery falls off a cliff with
  distance rather than degrading gracefully. The measured figures and their sources are in
  [ARCHITECTURE.md](ARCHITECTURE.md).
- **No one-way latency is ever reported.** Clocks across nodes are unsynchronised, so Potluck
  measures round trips and never divides by two.
- **Sub-millisecond loops cannot span the network.** The locality contract pins them to the node
  wired to the hardware; the build-time checker that enforces this lands at M4.
- **Planned (M6): portable actors re-activate on another node within ~2 s** of their host dying.
  The actor moves; whatever was physically wired to the dead node does not.

None of the above is a certified safety function — `SAFE_STATE`, the staleness rules and the
locality contract are availability and honesty features, not a safety case.

## License

[Apache License 2.0](LICENSE) with a [NOTICE](NOTICE) file. Slightly stricter than MIT in exactly
one direction: if you redistribute this or build on it, the attribution in NOTICE travels with your
distribution (§4(d)), you state significant changes (§4(b)), and you get an explicit patent grant in
return. Acknowledge the cook; otherwise help yourself.
