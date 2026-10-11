# Potluck — S3 Edition

**A distributed runtime that makes a cluster of small machines behave like one.** Everyone brings a
dish — a sensor, some RAM, a radio — and the cluster eats together.

*e(SP)luribus unum — out of many, one machine.*

**Start here:** [GETTING-STARTED.md](GETTING-STARTED.md) takes you from bare ESP32-S3 boards -- two of them or
twenty -- to a working cluster in about an hour; [DEVELOPING-APPS.md](DEVELOPING-APPS.md) then takes you to apps
of your own.

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
[ESP32-S3](https://www.espressif.com/en/products/socs/esp32-s3) (tested on the ESP32-S3-DevKitC-1 N16R8 — the
standard devkit board, in its 16 MB flash / 8 MB external-RAM variant) with host tooling on **Windows and Linux**. The architecture is deliberately
hardware-agnostic — no CPU is second-class, and relative performance is a placement input, never an
exclusion rule — so later editions add targets without the name having baked one vendor's silicon
in. (It is also why the ESP32 pun lives in the tagline and not in the project name.)

## What works today

Everything below runs on real ESP32-S3 boards and has passed a test written before it was tried -- each milestone
has one, and a criterion that would have killed it. 14 of the 24 milestones so far are accepted, three more are built, and seven (M13-M18 and M13.1: the cell presenting
its resources to other systems and to a phone, a dashboard, Home Assistant) are planned; the full ledger,
with every measurement and caveat, is [STATUS.md](STATUS.md).

- **Boards that find each other and know when one is gone.** Over the radio, a missing board is declared dead
  within 600 ms. A 24.5-hour run delivered 100 % of packets, with round trips of 4-6 ms.
- **Any board's sensor, read from anywhere** -- another board or your PC -- with its age and quality, never a bare
  number. Unplug the board and the read says `UNAVAILABLE`.
- **One command sets up the whole cell**: `potluck apply <cluster file>` builds the firmware, updates the boards that
  need it, deploys one signed package to every board and reads every output back -- no counters, variants or order
  to know. Change the package and the boards take it without rebooting; a package that breaks them is reverted by
  the boards themselves.
- **Firmware updates over the radio**, board by board; a board that fails on a new image rolls itself back.
- **Work that moves.** An actor not tied to a board's pins runs on whichever board is alive and moves within
  about a second when its board dies -- carrying up to 128 bytes of saved state.
- **Your own apps** in their own repositories: native code compiled in, or untrusted code in a WebAssembly
  sandbox that cannot touch the board.
- **More than microcontrollers:** a PC can join as a member and take work while it is on, offer services the
  boards call, and boards lend each other their idle cores.
- **Tight loops stay next to their hardware** (checked when you build), and boards can share a CAN bus.
- **Potluck shares the radio**: a board can be a cluster member and a Wi-Fi extender at the same time.
- **Record and replay** a session byte for byte, to see exactly what happened.

Not there yet: a dashboard and integrations with other systems such as Home Assistant (milestones M13-M18, planned
in [BEYOND-M12.md](BEYOND-M12.md)), a few long soak tests, and the limits listed at the end of the
[getting-started guide](GETTING-STARTED.md) (up to 20 boards in a radio cell, ESP32-S3 boards and PCs only, one
package per cluster).

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
# every Python suite, portability, firmware build + memory budget.
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

A cell is any number of ESP32-S3 boards up to 20, each on a USB cable for power and console while you set
it up, plus one USB-serial adapter wired to one board's GPIO 17, 18 and ground — the *frame link*, which is how
the PC joins the cluster as a peer. That board reaches the others by radio. **To set one up:**
[GETTING-STARTED.md](GETTING-STARTED.md). The links below describe the maintainer's own three-board test cell.

- **Wiring:** [bench/m1-wiring.html](bench/m1-wiring.html) — the adapter, the board's headers from
  Espressif's own pin table, and the three wires step by step, as actually built. GitHub shows the
  file as source; open it in a browser after cloning.
- **CAN wiring (M4):** [bench/m4-can-wiring.html](bench/m4-can-wiring.html) — two boards, each with an
  SN65HVD230 transceiver module (the chip that turns logic levels into bus voltages), ten jumper
  wires, drawn as they sit on the bench.
- **The hardware, in one place:** [bench/esp32-s3-bench-datasheet.md](bench/esp32-s3-bench-datasheet.md) —
  every spec and limit known about these boards and modules, each tagged measured, datasheet or seller.
- **Your own cell from nothing:** [GETTING-STARTED.md](GETTING-STARTED.md). (The maintainer's own bring-up diary,
  with every finding along the way, is [WHEN-THE-BOARDS-ARRIVE.md](WHEN-THE-BOARDS-ARRIVE.md).)
- **Soak and measurement procedure:** [M0-RUNBOOK.md](M0-RUNBOOK.md).
- **Building an application in:** an application keeps its actors in its own repository and names them
  in a `potluck-modules.json`; a cluster file names the repository at a commit, and `potluck apply` builds it
  into the image and deploys it ([DEVELOPING-APPS.md](DEVELOPING-APPS.md)). The
  two applications built so far are the owner's `passive-sensor` (the room sensor, M8.2) and
  `poor-mans-extender` (the Wi-Fi extender, M5.1 and M6.1), both private repositories; one board runs
  both at once.

Then, from `host/potluck/`, over the adapter's port:

```
python -m potluck.apply my-cell.json                 # M12: the whole cell as the file says -- firmware where it
                                                     # differs, the package, every output read back
python -m potluck.apply my-cell.json inventory       # what every board runs, with its counters
python -m potluck.ctl --port COM6 --node 7368 watch sys/uptime        # M1: board B, read through board A
python -m potluck.ctl --port COM6 --node 6300 deploy pkg.json --ca ca.pub --key deploy.key --bcert deploy.bcert
                                                     # M3: one signed package by hand, whole cell
python -m potluck.hostnode --port COM6 --package pkg.json --identity host.id
                                                     # M10: the PC joins as a placement node
python -m potluck.fw --port COM6 rollout potluck_m0.bin --counter 4 --key deploy.key --bcert deploy.bcert --manifest my-cell.json
                                                     # M11: new firmware, board by board, over the radio
python -m potluck.guest sign module.wasm --key author.key --cert author.gcert --out module.guest.json
                                                     # M7: a guest author signs a module; the owner's
                                                     # manifest names it, ctl deploy --guests sends it
```

M9's job runs on board A and borrows B's and C's idle cores; its bench drives it from A's console:

```
python tools/m9_bench.py COM3 captures/m9-run.jsonl --runs 3
```

M4's CAN test runs on two boards built as radio-off variants, read straight off their consoles:

```
python tools/can_capture.py 300 captures/m4-can.log     # B on COM4 floods, C on COM5 sends SAFE_STATE
python tools/can_report.py captures/m4-can.log
```

## Reading order

| file | what it is |
|---|---|
| [GETTING-STARTED.md](GETTING-STARTED.md) | **Start here:** why you would use Potluck, what you need, and your own boards from bare to a working cluster -- keys, flashing, enrolment, the PC link, a first package you can watch fail over |
| [DEVELOPING-APPS.md](DEVELOPING-APPS.md) | **For people making apps for Potluck:** native actors or sandboxed guests, the actor API, descriptors, manifests, building, deploying and testing, with a worked example and what the first two apps learned |
| [WHEN-THE-BOARDS-ARRIVE.md](WHEN-THE-BOARDS-ARRIVE.md) | The maintainer's bench diary: unboxing, the test cell's board register, first contact, the soak and the distance sweep |
| [bench/m1-wiring.html](bench/m1-wiring.html) | The frame-link wiring, as built: the USB-serial adapter to one board, step by step (open in a browser) |
| [bench/m4-can-wiring.html](bench/m4-can-wiring.html) | The two-board CAN bus for M4, as built (open in a browser) |
| [STATUS.md](STATUS.md) | Every milestone, what it set out to prove, and exactly how far each claim has been measured -- the full ledger behind "What works today" |
| [ARCHITECTURE.md](ARCHITECTURE.md) | **The single source of truth.** Decision-closed v1: the namespace, the read contract, the wire format, memory budgets, the nine decision records, and milestones M0–M12 with accept/kill criteria |
| [BEYOND-M12.md](BEYOND-M12.md) | A discussion draft, not a decision: what comes after the roadmap -- one hub on the PC that owns the link, a dashboard of the whole cell, and integrations that let any other system use the cell's resources, Home Assistant first |
| [M0-LOG.md](M0-LOG.md) | The decision log, newest session last — including the conclusions that were later **withdrawn**, kept struck-through rather than deleted. The QEMU sessions are a study in how a stale flash image manufactures false evidence |
| [M0-RUNBOOK.md](M0-RUNBOOK.md) | Bench procedure: build, flash, emulate, soak, and the delay methodology to read *before quoting any number* |
| [CLAUDE.md](CLAUDE.md) | Standing rules for working on this repo (largely built with [Claude Code](https://claude.com/claude-code), which these rules keep honest) |
| [.claude/zero-assumption/memory.md](.claude/zero-assumption/memory.md) | The evidence ledger: every external fact used, with source, retrieval date and status — including the withdrawn ones |
| [history/](history/README.md) | How Potluck came to be: the original vision documents, the reviews that shaped the architecture, the first work orders. Not needed to use or build on it |

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
  Each of those boards also keeps it in flash, at most a minute old (since M12), so it survives every board
  rebooting at once. While ownership moves, the actor's outputs read NO DATA, not an old value, until the new
  holder publishes. Whatever was physically wired to the dead node does not move either. An actor
  that owns an actuator is never portable.
- **A host service can vanish at any moment** (since M8): a PC that offers services to the cluster
  is a machine that might be off. A board that calls one keeps the last answer it got, marked STALE
  with its true age, and picks up again by itself when the PC returns; nothing waits on it.
- **Lent work comes back, or runs again** (since M9): a board may hand pieces of a job to neighbours'
  idle cores, which run them below everything the node itself does and may refuse when busy. A piece
  whose answer has not come back by a deadline the lending board sets from its own measurements is
  cancelled and run again elsewhere, so a neighbour that dies or reboots costs time, never a result.
  Only pure functions built into the firmware are lent -- no code moves -- and the job's answer is the
  same wherever its pieces ran.
- **A PC in the cluster takes actors while it is on, and gives them back when it goes** (since M10): a
  PC that opts in as a placement node is chosen for portable actors like any board. Cut off without
  warning, its actors run on a board again within the death window (0.63-0.69 s after its last frame on
  the bench); back, it takes them again by a handover that never runs an actor twice, about 6 s after
  it starts. Whatever needs the PC's hardware still needs the PC.
- **A package change takes effect without a reboot** (since M12) when only actors change: an unchanged actor
  keeps running untouched; a changed or new one starts as a new instance (a changed portable actor from its
  checkpoint), and its outputs read NO DATA until it publishes; a removed one's outputs disappear. A change to a
  sandboxed guest's code reboots the boards into the package instead. Either way the boards try it before they keep
  it, and go back to the previous one by themselves if it fails.
- **A firmware update takes a board out of the cell for about 15 s** (since M11): it reboots into the
  new image, rejoins, and keeps the image only after 100 heartbeats with at least one radio peer alive.
  Its peers declare it dead and alive again meanwhile, and its portable actors move and come back like
  any other death. An image that has not passed that test 120 s after boot is rolled back by the board
  itself; one that crashes is rolled back by the bootloader at the next start. Updates go one board at a
  time and stop at the first that rolls back, so a bad image costs one board 15 s to 2 min, not the cell.
- **A guest program is contained, not trusted** (since M7, on boards built with the sandbox): code from an author the owner certified runs in a WebAssembly sandbox with a budget of work per tick, its own memory and nothing to call but its inputs, its outputs, a 128-byte checkpoint and a log. It can write nothing -- every output is read-only -- and cannot reach a pin, the radio or another node. A tick that faults publishes nothing and its outputs read FAULTY until a tick succeeds; three faults in a row quarantine it for that activation (a move to another board or a reboot is a new activation, with three more tries). Guests on one board run one at a time, so a guest can hold up another on the same board for its whole budget. They run below everything the node itself does and are never placed on a PC.
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
