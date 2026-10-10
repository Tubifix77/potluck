# Potluck — S3 Edition

**A distributed runtime that makes a cluster of small machines behave like one.** Everyone brings a
dish — a sensor, some RAM, a radio — and the cluster eats together.

*e(SP)luribus unum — out of many, one machine.*

---

## The idea

An [ESP32](https://en.wikipedia.org/wiki/ESP32) — a Wi-Fi microcontroller that costs a few euros —
is far too powerful to spend its life watching a flowerpot. But there it sits, because its pins are
wired to the flowerpot. That is **stranded compute**, and every home, car and workshop is full of it.

Potluck federates those machines into one. It is
[Kubernetes](https://kubernetes.io/) — the tool that pools a datacenter's servers into one big
computer — for CPU/GPU/RAM ***and* attached hardware**: an application is written against the
cluster, never against a board. It refers to

```
potluck://car/lights/rear/left
```

not to "analog input 1 on the board behind the utility cupboard". Ten ESP32s, a
[Raspberry Pi](https://en.wikipedia.org/wiki/Raspberry_Pi), twenty lights and a dash screen ship as **one signed
application package for the whole car**. The application is composed of *actors* — small isolated
units of code, in the [actor-model](https://en.wikipedia.org/wiki/Actor_model) sense: the ones that
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
that returns the number alone; in Python, `float(reading)` raises on purpose; and `potctl` — the
cluster's command-line tool — deliberately has **no flag to print just the value**, because such a
flag ends up in a script that has lost the age.

## What "S3 Edition" means

It names the **hardware support, not the project**. This edition targets the
[ESP32-S3](https://www.espressif.com/en/products/socs/esp32-s3) (bench fleet:
3 × ESP32-S3-DevKitC-1 N16R8 — the standard devkit board, in its 16 MB flash / 8 MB external-RAM
variant) with host tooling on **Windows and Linux**. The architecture is deliberately
hardware-agnostic — no CPU is second-class, and relative performance is a placement input, never an
exclusion rule — so later editions add targets without the name having baked one vendor's silicon
in. (It is also why the ESP32 pun lives in the tagline and not in the project name.)

## Honest status — read this before judging anything else

Work is organised as **falsifiable milestones, M0–M10** — every "M-number" in this README is one of
them; the ".1" and ".2" steps were added when the owner's own applications asked for something the
platform could not yet do. Each has an acceptance test that can fail and a kill criterion (both spelled
out in [ARCHITECTURE.md](ARCHITECTURE.md)); a milestone counts as *built* when the code exists, and
*accepted* only when its test has passed. Where they stand as of **2026-10-10**, after ten days on real
hardware — three ESP32-S3 boards on a desk, walked through a house, and placed around a room:

| milestone | in one line | state |
|---|---|---|
| **M0** — two boards, one heartbeat | the wire format; membership (who is in the cluster and alive); measured packet-delivery ratio and round-trip delay | **accepted, 2026-10-04.** A 24.5-hour *soak* (a long unattended measured run) on three ESP32-S3 boards: no gaps, no reboots, no node wrongly declared dead, **100 % of packets** delivered both ways on all six links, round trip **4–6 ms typical, 16–22 ms at the 99th percentile**, memory flat to the byte. Then the *kill criterion* (the test that could have sent the radio choice back to the drawing board): a distance sweep through a real two-storey house, up to about 16 m, through four walls, a floor and a fridge. The link held everywhere; the worst spot still delivered 99 % of packets with round trips under 42 ms. The radio choice stands |
| **M1** — one remote read | the namespace; typed, staleness-checked reads | **accepted on hardware, 2026-10-04.** The PC, cabled to one board, read another board's value across the radio: value, unit, age and class. Unplug that board and the read says `UNAVAILABLE` with no number at all; plug it back in and it recovers by itself. Getting there found three bugs no emulator could show, including a value's age being measured from when it arrived rather than from when it was measured |
| **M2** — host in the loop | `potctl`; recording sessions to a capture file and replaying them | **accepted**, first under emulation (a 13.7-minute session, 11,444 frames) and **on hardware, 2026-10-04**: a 10-minute session over the real serial cable, covering all three boards, was recorded as raw frames and replayed on its own into a namespace identical to the byte (checked by SHA-256, and a wrong checksum is refused). The first attempt crashed after 70 seconds on a real bug: the PC tool could write two messages to the cable at once |
| **M3** — deploy and detach | A/B module slots (two copies, so a bad deployment falls back by itself); signed deployment packages | **accepted on hardware, 2026-10-04.** A signed package sent once, to one board, reached all three: that board passed it on to the others over the radio. Every board changed behaviour (the status light's healthy colour), and kept the new behaviour after the PC was unplugged and all three were power-cycled on a mains charger. Then a deliberately broken package: each board crashed three times on it and went back to the previous version by itself |
| **M4** — locality contract enforced | the rule that tight control loops stay pinned to the node wired to the hardware, rejected at build time otherwise; plus [CAN](https://en.wikipedia.org/wiki/CAN_bus), the automotive wired bus | **accepted (2026-10-05), with one caveat: no instrument outside the boards has recorded the arbitration.** The build-time half passes: a manifest that binds a tight loop to a sensor on another node is rejected with an error naming the actor, the resource and both nodes. On a two-board CAN bus, one board flooded at about 3,000 frames a second while the other sent a safety message every second: all 301 arrived in order, each left within 335–683 µs, and the safety sender never lost arbitration. The first run found the safety message waiting 2.5–5.5 ms behind its own board's traffic, so the transport now sends in priority order. The milestone asked for this on a scope; the bench has none, so the evidence is the CAN controllers' own counters. A logic-analyzer trace would close that gap |
| **M5** — signed everything | a cluster certificate authority, node enrolment, authenticated frames | **accepted on three boards (2026-10-07).** Each board makes its own key and is enrolled over its own USB cable; an unenrolled board is refused and logged by the others. Every frame between enrolled boards carries a tag under a per-pair key, and a recorded command re-sent over the air is rejected. A safety message is signed, and one recorded before the whole cell restarts is still refused afterwards. Boards check deployment packages themselves and refuse unsigned ones, older ones, and ones signed outside the cluster. Signatures are [Ed25519](https://en.wikipedia.org/wiki/EdDSA), chosen because a board verifies one in 27 ms against 442 ms for the alternative |
| **M5.1** — sharing the radio | a node can be a Potluck member and a phone hotspot on the house Wi-Fi at once (from the owner's `poor-mans-extender`) | **all five parts built (2026-10-07).** The cell follows a channel change and a lost node finds it again by scanning (about 5 s on the bench); a node can loosen or tighten its own death window while running without being declared dead; a node can declare itself busy; and a PSRAM build is measured (PSRAM copies at 11–31 MiB/s, ample for the extender). And a node out of range of the others joins through a single relay (a new design decision, ADR-009): reads, writes and death detection all work through it, still authenticated end to end, and a session captured through it replays byte-identical. And the real-world test: next to a working phone hotspot carrying a video stream, the cell declared no false death in 16 minutes; the stream costs round-trip jitter (worst 157 ms), not liveness (death window 600 ms). A 24-hour version is still to run |
| **M6** — reconciler | failure-driven actor re-placement | **accepted on three boards (2026-10-08).** A "portable" actor -- one not tied to any board's hardware -- runs on one board and moves when that board dies: with its board held in reset, reads were answered by another board 0.46-0.69 s later, every time. When the dead board comes back, the actor is handed back without ever running in two places. Cut the cell in two and both halves run it; when the cut heals, the extra copy is stopped within half a second, and no reader ever accepts a value from the losing copy. Where an actor runs is worked out by every board independently from the same inputs ([rendezvous hashing](https://en.wikipedia.org/wiki/Rendezvous_hashing)), so there is no coordinator to lose. The boards now use their 8 MB of extra RAM ([PSRAM](https://en.wikipedia.org/wiki/Pseudostatic_RAM)) for buffers that are not time-critical |
| **M6.1** — the application owns the radio | the same garden node as one chip: a Potluck member, a station on the house Wi-Fi, and a phone hotspot at once (CR-6 from the owner's `poor-mans-extender`) | **accepted on the bench (2026-10-08).** One board is a cluster member, joined to the house Wi-Fi, and a phone hotspot at the same time, at the same speed as the hotspot alone (18-19 Mbps down); the cluster never mistook it for dead. When the house router changed channel, the whole cluster followed it within about 3 seconds -- after a fix the test itself prompted. Since then that board has also run the room sensor's actors (M8.2), and running both found three faults in how a rebooting extender rejoins -- it sent the others to the channel it had saved, they followed each other while looking for it, and it took over work before its Wi-Fi had connected -- all fixed (2026-10-09). **Open:** once, after a cold start, its radio stopped sending for about 40 minutes; not reproduced since. The 24-hour version is still to run |
| **M7** — [WebAssembly](https://webassembly.org/) tier | running **untrusted** code -- a third party's -- sandboxed, so it cannot take over the boards | gated: opens when someone needs to run code they do not fully trust. Updates of the owner's own code are M11's, without an interpreter |
| *background compute* — on hardware it is **M9** (below) | a coordinator handing units of work to nodes that would otherwise sit idle, so a chip is not limited to watching one sensor. It is the answer to why an ESP32-S3 is worth clustering at all | **measured in simulation** (19 workers reach 18.99× the throughput of one, killing a worker mid-job loses no work) **and, since 2026-10-10, on hardware: three boards 3.41× faster than one** (M9). Listed here because it is the point of the project rather than a numbered step toward it |
| **M8** — host services | `potluck-agent`, letting a PC offer services (say, speech-to-text) into the cluster's namespace | **accepted on the bench (2026-10-08).** A PC running `potluck-agent` offers named services to the cluster -- only the ones its owner lists -- and a board calls one (the time of day, which a board cannot know by itself) every second. Pull the PC's cable and the board carries on: the value it last got is still readable, clearly marked as old and getting older, never passed off as fresh; plug it back and it picks up again by itself. |
| **M8.1** — attached hardware and the actor API | a real sensor's value in the namespace, read from another board with its age and quality; actors written against a documented interface instead of inside the firmware's main file (still compiled into the image) | **built (2026-10-08).** The chip's own temperature sensor on board B is read from the PC through board A: a value with its age, `FAULTY` with no number when the sensor fails, `UNAVAILABLE` when B is gone. Actors are now written against a documented interface, and the four existing ones were moved onto it. Still to show: the value moving when the chip is warmed |
| **M8.2** — the room that knows it's occupied | three boards publish presence and motion from how a person in the room weakens their own heartbeat signals; no extra hardware (from the owner's `passive-sensor`). The first application whose code lives in its own repository | **built, and the experiment that could have stopped it passed (2026-10-10).** Potluck now builds an application's own actors in from its own repository, hands them every received frame's signal strength and channel, moves its detector to another board when one dies and resumes it from a checkpoint, and shows its verdict on every status light without ever hiding a failing link. On the desk: 3 hours of empty desk with no false alarm once the detector was tuned, and seven board reboots with none. In a real room, boards placed around it: nobody there read empty (0 of 370 readings); a person walking, standing and sitting read present (100 %), and walking was told apart from standing still (99 % against 11 % "moving"). It senses the open room beyond the three boards, but patchily. **Not yet accepted:** 30 minutes of empty room, and 24 hours with no PC |
| **M9** — borrowing an idle core | a board hands self-contained pieces of a job to neighbours with spare cores, and does a piece itself if a neighbour dies; the experiment that decides whether moving work between live boards is worth it | **accepted on three boards (2026-10-10), with no PC attached.** A job on one board -- an estimate of pi from a billion random points, in 960 pieces -- took 3 min 53 s on that board alone and 68 s with the other two lending their idle cores: **3.41x faster**, with the same answer to the last digit wherever each piece ran. Pressing reset on a lending board mid-job lost nothing: its pieces were run again. The lenders kept doing their own jobs as before. The board doing the work decides for itself, from its own measurements, whether lending a piece pays; a lender can say no. This reopened one of the design's closed decisions, as the experiment was set up to do, and only as far as the measurements reach |
| **M10** — the PC as a placement node | a PC runs cluster workloads while it is on, and the boards take them back when it is not | **accepted on the bench (2026-10-11), with one caveat: the cable was "pulled" by stopping the PC's program without warning, which is all a pulled cable shows the boards; a physical pull is still to do.** The PC joins as a full member, from the boards' own code built for Windows and enrolled like a board, and reaches the boards it is not cabled to through the one it is. A job that prefers the PC moved there each time it joined -- handed over, never running in two places -- and back to a board within 0.63-0.69 s each time it vanished, five times in a row; with the PC on for 30 minutes it stayed put. Every board read the job's values from the PC |
| **M11** — firmware over the cell | a signed firmware update sent once, through one board, reaches every board over the radio, each trying it and rolling back by itself if it fails | planned (2026-10-11), from `passive-sensor`: two boards on wall chargers around a room could not take a new command without being carried back to the PC |

Beneath the milestones, the standing figures:

| | |
|---|---|
| Architecture | decision-closed v1, with eight [Architecture Decision Records](https://adr.github.io/) and the trigger that would reopen each ([ARCHITECTURE.md](ARCHITECTURE.md)) |
| Test gates | **29 green (2026-10-10):** 325 C++ cases / 104,701 checks, run plain and under [AddressSanitizer](https://github.com/google/sanitizers/wiki/AddressSanitizer); 273 Python cases in 22 suites; three independent wire-format implementations agreeing byte-for-byte over generated corpora; a strict-GCC portability gate; plus the firmware build with its memory-budget check and on-target self-tests |
| Static memory | **49.8 KB** of the 64 KB core cap in the room-sensor builds, measured per build; 53.2 KB with the per-frame signal log on. Buffers that only tasks touch live in the boards' 8 MB of extra RAM |
| Measured on hardware | A **24.5-hour** three-board soak: no reboots, no false deaths, **100 %** packet delivery on all six links, round trip **4–6 ms** typical and 16–22 ms at the 99th percentile, memory flat to the byte. A distance sweep through a two-storey house (up to ~16 m, four walls, a floor, a fridge): every spot held, the worst at 99 %. The Wi-Fi stack costs **31.5 KiB** with [ESP-NOW](https://www.espressif.com/en/solutions/low-power-solutions/esp-now) active. All in a home thick with competing 2.4 GHz traffic, so these are a floor, not a best case |

Nothing above claims a measurement that was not made. Emulated runs stamp `"no_radio":1` on every
statistics line precisely so they can never be mistaken for a measured one; the hardware figures come
from runs stamped `"no_radio":0`. And the simulator earned some credit on the same day — it had
predicted the round trip at 5.57 ms and the per-node frame rate at ~12/s from published figures alone,
and measured reality landed at 4–6 ms and 13.6/s.

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
      │                         round-robin unicast probe → round-trip histogram
      ├── UART / USB-serial     COBS + CRC-16/CCITT-FALSE; the host joins as an
      │                         ordinary peer, not a special case
      └── CAN                   single-frame profile via 29-bit extended ID (M4)
```

The transports, for the unacquainted:
[ESP-NOW](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html)
is Espressif's connectionless Wi-Fi messaging (no router, no TCP/IP);
[UART](https://en.wikipedia.org/wiki/Universal_asynchronous_receiver-transmitter) is the classic
serial port, here framed with
[COBS](https://en.wikipedia.org/wiki/Consistent_Overhead_Byte_Stuffing) and a
[CRC](https://en.wikipedia.org/wiki/Cyclic_redundancy_check) checksum so frames survive a raw byte
stream.

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

## What you would build with it

Nothing in this section exists yet. These are design targets — and they are also *architecture
tests*, under a standing rule: if a named system cannot be built under this design, the design is
wrong and gets fixed, rather than the example quietly dropped. So each one below states what it
costs and what it refuses to do.

**The house.** A watering node in the garden, moisture sensors indoors, a voice box, a screen by the
door, and a PC that is sometimes on. One signed package for the whole house. The watering rule is
pinned to the valve it owns and keeps working with the PC off; the anomaly detector is portable and
lands wherever there is headroom; the voice box keeps wake-word detection on its own microphone and
ships only the utterance to the PC's speech service, degrading to a handful of canned local intents
when the PC is off. You address the house, and no application ever names a board.
*What it costs:* that speech service is best-effort by construction, so nothing time-critical may
ever be built on top of it.

**The production line.** A test *fixture* — the lidded box a finished circuit board is dropped into,
so spring-loaded pins can touch its bare test pads and check it works before it ships — is a
microcontroller bolted permanently beside hardware at one fixed spot. A factory floor has dozens,
each running its own standalone program, each updated by carrying a USB stick over to it. Under
Potluck the fixture's chip runs the runtime and the test is an actor above it: every fixture's
readings are addressable by name from anywhere and arrive stamped with their age, a new test version
ships as one signed package to every fixture at once and reverts itself if it is bad, and a recorded
session replays frame-for-frame when a customer disputes a unit six months later.
*What it costs:* forty fixtures exceed the ~20-node ceiling of a single v1 radio cell, so this is a
wired deployment or it waits for the routed profile named as the post-v1 path.
*What it refuses:* anything that clamps, presses or moves near an operator's hands. Potluck
instruments the fixture; the interlock is hardware, and it belongs to a functional-safety engineer.

**The workshop cluster.** A Raspberry Pi owns the only screen, keyboard and mouse; a dozen ESP32s
around the room each watch one sensor and are idle the rest of the time. A coordinator on the Pi
scatters latency-indifferent work — indexing, compression, batch analysis — across every chip at
background priority, below every pinned duty, consuming only the cycles nothing else wanted. The
Pi's keyboard and screen are namespace entries like any pins. Kill a worker mid-job and no work is
lost. This is the answer to why an ESP32-S3 is worth clustering at all, and it is the one scenario
here with numbers behind it: 19 workers reaching 18.99× the throughput of one, in simulation.
*What it costs:* harvesting idle cycles denies sleep, so battery and solar nodes stay out of
background pools unless a manifest opts them in.

## Try it with no hardware at all

Prerequisites: [ESP-IDF v6.0.2](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/) —
Espressif's development framework for these chips, which also provides
[the QEMU fork](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/tools/qemu.html)
via `idf_tools.py install qemu-xtensa` — plus Python ≥ 3.10 and a host C++ toolchain. The scripts
are PowerShell-first; `tools/run_all_tests.sh` exists for Linux but the PowerShell path is the one
exercised daily.

```powershell
# The gates: C++ suite, AddressSanitizer, three differential corpora,
# eighteen Python suites, portability, firmware build + memory budget.
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

Every line is the full read tuple. `L4` is the loosest of the five latency classes (L0 is
pinned-to-hardware, L4 is best-effort), and the zeros are honest ones: *peers alive* and *RSSI*
(radio signal strength) read zero because QEMU emulates no radio. `watch sys/peers-alive` shows
`0 → 1` as your own host registers as a live peer — the membership state machine running over a
wire, with the firmware unaware the peer is a laptop. Add `--capture session.jsonl` and the session
prints the digest command that verifies its own capture. One `potctl` per emulated run: QEMU's
socket serial accepts a single connection per VM lifetime ([M0-RUNBOOK.md](M0-RUNBOOK.md) has the
details and the other emulator sharp edges).

## On real hardware

The bench is three ESP32-S3-DevKitC-1 boards, each on its own USB cable for power and console, plus
one USB-serial adapter wired to one board's GPIO 17, 18 and ground — the *frame link*, which is how
the PC joins the cluster as a peer. That board reaches the others by radio.

- **Wiring:** [bench/m1-wiring.html](bench/m1-wiring.html) — the adapter, the board's headers from
  Espressif's own pin table, and the three wires step by step, as actually built. GitHub shows the
  file as source; open it in a browser after cloning.
- **CAN wiring (M4):** [bench/m4-can-wiring.html](bench/m4-can-wiring.html) — two boards, each with an
  SN65HVD230 transceiver module (the chip that turns logic levels into bus voltages), ten jumper
  wires, drawn as they sit on the bench.
- **The hardware, in one place:** [bench/esp32-s3-bench-datasheet.md](bench/esp32-s3-bench-datasheet.md) —
  every spec and limit known about these boards and modules, each tagged measured, datasheet or seller.
- **Bring-up from nothing:** [WHEN-THE-BOARDS-ARRIVE.md](WHEN-THE-BOARDS-ARRIVE.md).
- **Soak and measurement procedure:** [M0-RUNBOOK.md](M0-RUNBOOK.md).
- **Building an application in:** an application keeps its actors in its own repository and names them
  in a `potluck-modules.json`; `tools\build_firmware.ps1 -AppComponents <its components folder>` builds
  them into the image, and `potctl ... deploy --modules <that file>` checks a manifest against it. The
  two applications built so far are the owner's `passive-sensor` (the room sensor, M8.2) and
  `poor-mans-extender` (the Wi-Fi extender, M5.1 and M6.1), both private repositories; one board runs
  both at once.

Then, from `host/potluck/`, the same tool as above over the adapter's port:

```
python -m potluck.ctl --port COM6 --node 7368 watch sys/uptime        # M1: board B, read through board A
python -m potluck.ctl --port COM6 --node 6300 deploy pkg.json --ca ca.pub   # M3: a signed package, whole cell
```

M4's CAN test runs on two boards built as radio-off variants, read straight off their consoles:

```
python tools/can_capture.py 300 captures/m4-can.log     # B on COM4 floods, C on COM5 sends SAFE_STATE
python tools/can_report.py captures/m4-can.log
```

## Reading order

| file | what it is |
|---|---|
| [WHEN-THE-BOARDS-ARRIVE.md](WHEN-THE-BOARDS-ARRIVE.md) | The bench guide: unboxing, the board register, first contact, the soak and the distance sweep, written for a reader who remembers nothing |
| [bench/m1-wiring.html](bench/m1-wiring.html) | The frame-link wiring, as built: the USB-serial adapter to one board, step by step (open in a browser) |
| [bench/m4-can-wiring.html](bench/m4-can-wiring.html) | The two-board CAN bus for M4, as built (open in a browser) |
| [ARCHITECTURE.md](ARCHITECTURE.md) | **The single source of truth.** Decision-closed v1: the namespace, the read contract, the wire format, memory budgets, the eight decision records, and milestones M0–M8 with accept/kill criteria |
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
`tools/decode_backtrace.ps1` refuses to decode a crash against a compiled binary
([ELF](https://en.wikipedia.org/wiki/Executable_and_Linkable_Format)) whose hash disagrees with the
log, a rule that exists because breaking it once cost a full day chasing a fix that had already
worked.

## The name

The project was **dμOS** until 2026-08-01. The Greek mu broke five separate tools — the ESP32
compiler's argument handling, `esp-idf-kconfig`, QEMU's `-serial file:`, Python's console output on
[CP1252](https://en.wikipedia.org/wiki/Windows-1252) Windows, and a size checker — each in a
different way, none with a useful error. µTorrent made the same retreat. "Potluck" says what the
system does in one word, and every character of it survives every toolchain. The workaround code is
still in the build script, self-disabling, for whoever clones this under a path with an accent in
it.

## The failure contract

Potluck injects behaviours into every system built on it, and an application author inherits them
without seeing them — so whether they are acceptable for a given machine is not decidable unless the
runtime states them. Here they are:

- **A dead node is declared dead after 600 ms** — six missed heartbeats at the default 100 ms period
  (both configurable). Until that window closes, the cluster still believes the node is alive.
- **Each node declares its own death window, and may change it while running** (since M5.1): the
  period is carried in one byte of centiseconds, so **10 ms to 2.55 s** in 10 ms steps, times a miss
  limit of 1 to 255. A node that loosens its window keeps the old rate until a periodic greeting has
  told its peers; a node that tightens it applies the change at once. Peers always judge a node by
  the window it declared, not by their own.
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
  wired to the hardware; the build-time checker that enforces this has been in place since M4.
- **A portable actor re-activates on another node after its host is declared dead** (since M6):
  0.46-0.69 s on the bench, for the default 600 ms death window. The actor restarts from its initial
  state -- nothing it held in memory moves -- unless it saved a checkpoint (since M8.2): up to 128
  bytes, kept by every other board that may run it, which the new holder starts from with its age.
  A checkpoint lives in memory only: if every such board reboots, it is gone, and the actor starts
  fresh. While ownership moves, the actor's outputs read NO DATA, not an old value, until the new
  holder publishes. Whatever was physically wired to the dead node does not move either. An actor
  that owns an actuator is never portable.
- **A host service can vanish at any moment** (since M8): a PC that offers services to the cluster
  is a machine that might be off. A board that calls one keeps the last answer it got, marked STALE
  with its true age, and picks up again by itself when the PC returns; nothing waits on it.
- **Activation is at-least-once, actuation exactly-once.** While a partition lasts, both sides may run
  the same portable actor; when it heals, the copy with the older claim stops (0.38 s on the bench),
  and readers never accept a value from it once they have seen the newer one. A node that has just
  booted starts nothing for up to 6 s, until it has heard who runs what.

None of the above is a certified safety function — `SAFE_STATE` (the reserved everyone-freeze
broadcast), the staleness rules and the locality contract are availability and honesty features, not
a safety case.

## License

[Apache License 2.0](LICENSE) with a [NOTICE](NOTICE) file. Slightly stricter than
[MIT](https://en.wikipedia.org/wiki/MIT_License) in exactly one direction: if you redistribute this
or build on it, the attribution in NOTICE travels with your distribution (§4(d)), you state
significant changes (§4(b)), and you get an explicit patent grant in return. Acknowledge the cook;
otherwise help yourself.
