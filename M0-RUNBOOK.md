# Potluck runbook — building, flashing, emulating, measuring

**Potluck — S3 Edition.** The operator's manual: how to build and flash the firmware, run it under
emulation, and measure and test a cell on a bench. It is a living runbook, kept with the code; it was
**last verified on 2026-10-16 on the reference bench** (three ESP32-S3-DevKitC-1 N16R8 boards and a CP2102
USB-serial adapter, on Windows 11). The host tools are tested on Windows 11 only; Linux is untested.

It began as the M0 runbook ("two boards, one heartbeat", sections 0-9, whose acceptance checklist in
[§6](#6-the-acceptance-checklist-13-m0) is ticked), and each later milestone added its bench procedure as a
section of its own. **For setting up a cell, start with [GETTING-STARTED.md](GETTING-STARTED.md)**: today
`potluck apply` builds, flashes and deploys a whole cell from one cluster file (section 15).

**On the reference bench**, used in the examples below: board **A** (node `6300`) carries the frame-link
adapter (`COM6` in the examples), **B** (`7368`) runs the Wi-Fi extender app, **C** (`8160`) is a plain board;
their consoles were `COM3`, `COM4` and `COM5`. Substitute your own ports (`/dev/ttyUSB0` on Linux) and node
ids (`python -m potluck.apply <cluster file> inventory` lists them).

[M0-LOG.md](M0-LOG.md) is the project's decision log, newest session last: the reasoning behind what is
here, including conclusions that were later **withdrawn**.

---

## 0. Ninety seconds of orientation

| | |
|---|---|
| What M0 proves | two ESP32s exchange 100 ms heartbeats over ESP-NOW and report **measured** PDR and delay |
| Accept / kill | §13-M0, copied verbatim in [§6](#6-the-acceptance-checklist-13-m0) below |
| Heartbeat timers | 100 ms period, dead at 6 misses = 600 ms (§8.2, wireless row) |
| What you flash | one binary, both boards — node ids derive from each board's MAC (enrolment keeps that id unless another board has it) |
| Where the numbers land | `potluck-capture`'s summary, printed on Ctrl-C; the capture file has the raw stream |
| The one thing to remember | **there is no one-way delay figure.** M0 measures RTT. See [§7](#7-the-delay-methodology--read-before-quoting-any-number). |

```bash
tools\build_firmware.ps1 -Flash -Port COM7
```

---

## 1. Before you touch a board

Confirm the bench is still as the last session left it:

```bash
tools\run_all_tests.ps1 -Asan -Firmware
```

Expect **`ALL PASS` on every line**, and the §6 budget line under the 64 KB cap of static internal RAM.
This takes a few minutes and it is worth it: it proves the toolchain, the codec, the heartbeat state
machine, the namespace, the statistics and the serial format are all still in the state the
measurements will be attributed to.

The gates are: the C++ suite (479 test cases on 2026-10-16), the same suite under AddressSanitizer, the
fixture and corpus regenerations, every Python test file (516 tests in 48 files — including a differential
fuzz run requiring the C++ and Python decoders to agree on every fuzz input, and a namespace-wire corpus
requiring them to agree byte for byte on every READ/WRITE/REPLY case), a strict-GCC portability gate over
the portable core, and the firmware build with the §6 budget gate.

Without `-Asan -Firmware` you get the host gates only, which is the fast loop.

If anything fails, stop. A soak run against a broken codec produces numbers about the codec.

### Prerequisites (the versions on the reference bench)

| | |
|---|---|
| ESP-IDF | **v6.0.2**, installed anywhere, with `IDF_PATH` set (ESP-IDF's installer does) or `-IdfPath` passed to the scripts |
| Toolchain | xtensa-esp-elf gcc 15.2.0, cmake 4.0.3, ninja 1.12.1, as ESP-IDF's tools installer puts them under `~/.espressif` |
| Shell | PowerShell 7 (`pwsh`); Windows PowerShell 5.1 fails in ESP-IDF's `export.ps1` |
| Host C++ | MSVC 14.51 from VS 18 Build Tools |
| Python | 3.14 (ESP-IDF's own environment). `pyserial` is needed **only** to read a live port |

If `pyserial` is missing, `tools\capture.ps1` finds ESP-IDF's own Python, which has it. Nothing else
is required — the decoder, the capture writer and the summary are standard library only.

### The ASCII mirror, and why you probably will not see it

ESP-IDF v6.0.2 cannot build from a path containing a non-ASCII character on Windows. That used to
matter enormously, because the repository's folder was named `dμOS` and the μ broke the build in two
independent ways — Xtensa GCC's argv handling and `esp-idf-kconfig`, each with a different and
unhelpful error. A junction and a `subst` drive were tried and rejected; CMake resolves both back to
the real path. Full measurements are in M0-LOG.md, decision 12.

**The rename to Potluck removed the cause.** On an all-ASCII path `tools\build_firmware.ps1` builds
`firmware/` in place and prints `building in place … (all-ASCII path, no mirror needed)`.

The mirror code is still there, and it triggers itself if the *whole* path is not ASCII — which covers
the case that has nothing to do with the old name: a user account with an accent in it. If you see

```
path contains a non-ASCII character; building through an ASCII mirror
```

then something above the repository has a non-ASCII character in it, and the build is running from
`<system drive>\potluck-fw-<target>` (for example `C:\potluck-fw-esp32s3`) instead. **Never edit the mirror** — it is overwritten on every build,
and only size reports are copied back. `-ForceMirror` exercises that path deliberately.

The host tests always build in place. MSVC never had the problem.

---

## 2. Flash the boards

Target hardware is **ESP32-S3-DevKitC-1 N16R8**. Build with the default target:

```bash
tools\build_firmware.ps1 -Target esp32s3
```

Classic ESP32 also still builds (`-Target esp32`) and is useful for comparison, since §6's budget is
written against it.

### ⚠ Plug into the UART port, not the USB port

The DevKitC-1 has two USB-C sockets. Use the one labelled **UART**.

The S3's USB PHY interferes with Wi-Fi — ESP-IDF sets `SOC_WIFI_PHY_NEEDS_USB_WORKAROUND` for this
chip, and stock `CONFIG_ESP_PHY_ENABLE_USB=y` accepts a known RF penalty to keep native USB alive.
M0 exists to measure PDR and delay, so `sdkconfig.defaults.esp32s3` turns that off
(`CONFIG_ESP_PHY_ENABLE_USB=n`) to get the radio's real behaviour. The consequence is that the
native **USB** port will not work while Wi-Fi is running. The **UART** port is a separate bridge
chip and is unaffected — everything, including flashing and the console, goes through it.

If a board appears dead over USB, this is why. It is not a fault.

Two further notes on this hardware:

- **PSRAM is on** (since M6, `sdkconfig.defaults.esp32s3`): buffers that only tasks touch live in the R8's
  8 MB, and section 6's gate counts internal RAM only. (Until M6 it was deliberately off, so the M0
  measurement of the Wi-Fi stack's internal DRAM was unambiguous.) A board without PSRAM needs
  `CONFIG_SPIRAM=n` (with `potluck apply`, in the cluster file's `"firmware": {"extra": [...]}`).
- **2.4 GHz only.** The merchant listing claims dual-band; it is wrong, no ESP32 has a 5 GHz radio.
  So the soak shares the band with every access point around it, and the channel you pick matters.
  Record it (§8).

```bash
tools\capture.ps1 --list-ports
```

Then, for each board in turn:

```bash
tools\build_firmware.ps1 -Flash -Port COM7
```

```bash
tools\build_firmware.ps1 -Flash -Port COM9
```

The same binary goes to both. Node ids are derived from the low two bytes of each board's station
MAC, so the boards differ without two builds existing — a soak where the two boards run different
images can differ in ways nobody intended.

Sanity-check one board before starting the soak:

```bash
tools\capture.ps1 --port COM7 --interval 5 --show-logs --duration 60
```

Within a few seconds you should see, in this order:

1. a `boot` line carrying the node id, epoch, MAC, **ESP-NOW version** and the §6 DRAM readings;
2. `peer_discovered` / `peer_alive` for the other board;
3. `version_pinned`, showing the negotiated payload cap — **1446 B** if both boards report ESP-NOW
   v2, **226 B** otherwise (§5.3);
4. a table with growing `rtt-samples` and a `tx-pdr%` near 100.

If `rtt-samples` stays at 0 while frames flow, probes are going out and replies are not coming back
— check both boards are on channel 1 (`CONFIG_POT_CHANNEL`) and that both show the same channel in
their `boot` lines.

---

## 2b. Sanity-check the cell size before you commit 24 hours to it

The heartbeat is O(N) now (broadcast beacon plus round-robin probe), but check the arithmetic for
whatever fleet size you are actually running:

```bash
build\tests\pot_sim.exe --sweep
```

And simulate the fleet, using the same `pot::Node` the boards run:

```bash
build\tests\pot_sim.exe --nodes 7 --mode broadcast --minutes 10 --link 52m_fringe
```

`membership: 0 death declarations` is the thing to look for. Any deaths on a modelled link mean the
policy will not hold at that size and geometry, and finding that out here costs ten seconds instead
of a day. `--mode unicast` reproduces the pre-fix behaviour if you want to see it saturate.

## 2c. The namespace, without a second board

Two boards are needed for §13-M0's link numbers, but the namespace and §4's read contract need only
one — plus the host, which is an ordinary peer at MAC `02:00:00:00:00:FE`.

Build with `CONFIG_POT_SERIAL_LINK=y` (on by default), wire the two GPIOs to a USB-serial adapter,
and then:

```bash
python -m potluck.ctl --port COM11 --node 1a2b ls
```

Every built-in resource (seven on every board), each with its whole tuple; your numbers will differ:

```
free internal DRAM   213400 B  [GOOD, age 812 ms, ts 41230, class L4]
largest free block   180224 B  [GOOD, age 812 ms, ts 41230, class L4]
uptime               41 s      [GOOD, age 812 ms, ts 41230, class L4]
boot epoch           27        [GOOD, age 812 ms, ts 41230, class L4]
peers alive          1         [GOOD, age 812 ms, ts 41230, class L4]
worst peer RSSI      -58       [GOOD, age 812 ms, ts 41230, class L4]
why it last started  1         [GOOD, age 812 ms, ts 41230, class L4]
```

`L4` is the loosest latency class (these are diagnostics). *Why it last started* is ESP-IDF's reset-reason
code (`sys/reset`): 1 power-on, 3 a software restart, 6 the task watchdog.

There is no flag that prints just the number, on purpose — see [§7](#7-the-delay-methodology--read-before-quoting-any-number).

```bash
python -m potluck.ctl --port COM11 --node 1a2b watch sys/heap-free --interval 1
```

```bash
python -m potluck.ctl --port COM11 --node 1a2b --verbose info
```

`--capture frames.jsonl` tees every frame in both directions into §7.6's format, the same format
`potluck-capture` writes, so a `potctl` session is replayable afterwards.

**The pin defaults are `CONFIG_POT_SERIAL_TX_GPIO=17` and `CONFIG_POT_SERIAL_RX_GPIO=18`,** on UART1
— deliberately not the console UART, so binary frames and log lines cannot corrupt each other. Cross
them over to the adapter: board TX to adapter RX.

If `ls` prints `TIMEOUT` for everything but `info` shows a heartbeat, the link is fine and the
namespace is not. If nothing arrives at all, it is the wiring or the baud rate, and `potctl` says
which of the two it thinks it is.

## 2d. Under emulation, with no boards at all

`tools\run_qemu.ps1` runs the firmware under Espressif's QEMU fork. **Read `M0-LOG.md`'s QEMU section
before trusting anything it prints.** In summary:

- There is no Wi-Fi device, so the build sets `CONFIG_POT_RADIO_DISABLE` and every statistics line
  carries `"no_radio":1`. **No run from it can stand in for §13-M0's soak.**
- GPIO input is not emulated, so `CONFIG_POT_BYE_BUTTON_GPIO=-1` is set; otherwise every node reads
  its BYE button as held down and departs the mesh immediately.
- The eFuse MAC is blank, so pass `-NodeId` or every emulated node claims `0x0001`.

Within those limits it runs the real firmware properly: boot, NVS, the boot epoch across runs, task
scheduling, the namespace and §4's read contract end to end over the frame link, and the static memory
figures.

```bash
tools\run_qemu.ps1 -Seconds 30 -NodeId 4097 -FrameLinkPort 5555
```

That exposes UART1 as a TCP socket, and the identical `potctl` drives it — same code that talks to a
board:

```bash
python -m potluck.ctl --tcp 127.0.0.1:5555 --node 1001 ls
```

Expect `7/7 answered with a usable value`. `watch sys/peers-alive` should show `0` then `1` as the
host's own heartbeats register it as a live peer, which is the quickest confirmation that §8.2's state
machine is running and that the frame link works both ways.

**⚠ One `potctl` per QEMU run.** QEMU's `-serial tcp:…,server,nowait` accepts exactly **one** connection
for the lifetime of the VM. The second invocation against the same run fails at connect —

```
BridgeError: write to tcp 127.0.0.1:5555 failed: could not connect: timed out
```

— and that is the emulator, not the node: the console log keeps filling with statistics throughout. So
do everything in one invocation (`ls` reads all seven resources over a single connection; `watch` holds
the connection open), or restart `run_qemu.ps1` between commands. A real board over a real serial port
has no such limit; this is purely an artefact of the socket chardev.

`-Extra "CONFIG_..."` applies extra Kconfig lines for a single run, which is how to bisect a
misbehaviour by turning one feature off:

```bash
tools\run_qemu.ps1 -Seconds 25 -NodeId 4097 -Extra "CONFIG_POT_SERIAL_LINK=n"
```

If the firmware panics, decode the backtrace with the ELF it actually came from:

```bash
tools\decode_backtrace.ps1
```

That script compares the panic's `ELF file SHA256` against the ELF and **refuses to decode on a
mismatch**. Take the refusal seriously: decoding against a stale binary once cost a whole session and
named a function this firmware never calls.

## 3. Run the 24-hour soak

One terminal per board. Both must run for the whole window: PDR is measured in both directions, and
one board's outbound figure is not the other's inbound figure.

```bash
tools\capture.ps1 --port COM7 --capture captures\node1.jsonl --interval 60
```

```bash
tools\capture.ps1 --port COM9 --capture captures\node2.jsonl --interval 60
```

Then leave them alone for 24 hours.

- **Geometry matters, and there is a number for it: keep the boards well inside 56 m.** §3 measured
  PDR above 99% below 56 m, and "fluctuating between 100% and zero" from 56 to 70 m. The beacon is a
  broadcast and so gets no MAC-layer retries, which means six consecutive losses is just
  `(1 − PDR)^6`: at 99% that is no false deaths at all, at §3's measured 58 m figure of 83.2% it is
  about **one false death per hour per link**, and at 70% it is 26. A soak run at the edge measures
  the edge, not the protocol. Write down the distance, obstructions and room in
  [§8](#8-record-these-with-the-numbers) — a PDR figure without a geometry is not a measurement.
- `--interval 60` prints the table each minute. The record stream is independent of this; the
  interval only controls the display.
- Ctrl-C at the end prints the summary. **That summary is the deliverable.** Interrupting is how a
  soak ends, not a failure — the tool exits 0.
- The capture file lets you re-derive everything afterwards without the boards:

  ```bash
  tools\capture.ps1 --replay captures\node1.jsonl
  ```

### Optional, and off by default: the frame tee

`CONFIG_POT_FRAME_TEE=y` makes each board print every frame in both directions, which `potluck-capture`
records in §7.6's capture format. **Do not enable it for the acceptance soak.** At a 100 ms heartbeat
with a probe and a reply each way it is roughly 40 lines per second, and the console UART's cost
lands inside the timing you are trying to measure. Turn it on to debug the wire, off to measure the
link.

---

## 4. Exercise the failure paths

Do these **after** the soak, or on a separate short run. Each one takes a minute and each produces
evidence for a specific claim in §8.2.

### Death declaration at 600 ms

Unplug board 2. Board 1 should log, within ~600 ms:

```
peer 0x…. declared DEAD after 6 misses (600 ms silent)
```

and `potluck-capture` should surface `! peer_dead … a=6 b=600`. `a` is the miss count and `b` the
milliseconds of silence. §8.2 chose 600 ms because "any wireless liveness timeout below ~150 ms is
provably wrong" — a declaration noticeably earlier than 600 ms means the timers are not what the
document says.

### Revival, and reboot told apart from revival

Plug board 2 back in. Board 1 reports **`peer_rebooted`**, not `peer_revived`, because the boot epoch
changed — the peer lost its sequence numbers, so its old ones are cleared rather than counted as
tens of thousands of losses. To see a plain `peer_revived` instead, break the link without power-
cycling: put a hand or a metal plate between the boards until death is declared, then remove it.

### Intentional departure: BYE

Press the **BOOT button (GPIO0)** on board 2. It broadcasts `BYE` and stops heartbeating. Board 1
should report `peer_left` and then *not* declare it dead — a peer that left on purpose is not a
failure, and counting it as one would corrupt the failure statistics. Press again to rejoin.

### The peer table is bounded

Not testable with two boards. §3 caps ESP-NOW at 20 paired devices *including* the broadcast entry,
so Potluck admits 19 unicast peers and refuses the twentieth with a `table_full` count. Worth knowing
before someone tries a 20-node cluster (M0-LOG.md, finding 4).

---

## 5. Read the numbers

The summary has four parts.

**Boot, per node — §6's [MEASURE] item.** Free internal DRAM at each bring-up step, plus the derived
figures. `Wi-Fi stack cost` is the number §6 asked for. §6's own trigger is evaluated for you:

> If it exceeds ~40 KB, the RX ring shrinks first.

If the summary says `ABOVE`, that is the action — reduce `kRxRingSlots` in `espnow_port.hpp` and
record the new figure. Do not leave it above and proceed.

**Per link — PDR.** Two independent figures, and they measure different things:

| Figure | Source | What it means |
|---|---|---|
| outbound PDR | ESP-NOW send callback | the 802.11 MAC-layer ACK came back |
| inbound PDR | `seq` gaps (§5.1) | frames the peer sent that never arrived |

`enqueue_err` is deliberately **excluded** from the outbound denominator: a frame `esp_now_send()`
refused was never transmitted, and counting it would blame the radio for a local queue overflow.
`rx queue overruns` is likewise ours, not the radio's.

An unmeasured PDR prints `unmeasured`, never 0 and never 100.

**Per link — the delay histogram.** 16 buckets, counts and percentages. p50 and p99 are reported as
the **bucket interval** containing them (`2.0-3.0 ms`), because that is all a histogram knows;
interpolating inside a bucket would invent precision no measurement supports. Exact min and max are
tracked separately and reported as single numbers, because those are real samples.

**Node counters.** Bad frames, unknown peers, wrong destinations, short payloads, unknown opcodes,
membership transitions, and the queue overruns above. On a healthy two-board bench most of these
should be 0; `bad_frame` climbing steadily means something else is on the channel.

---

## 6. The acceptance checklist (§13-M0)

Copied from ARCHITECTURE.md §13. Tick each line with a number, not an impression.

**Accept**

- [x] 24-hour soak at a 100 ms heartbeat, both boards, uninterrupted. **24.52 h, three boards, no
      capture gap longer than the 10 s sample interval, zero reboots** (2026-10-02/03, M0-LOG session 13).
- [x] A **delay histogram** published, measured on this bench — not cited from ARCHITECTURE.md.
      **264,850 round trips: p50 4–6 ms, p99 16–22 ms, p99.9 42–60 ms, none above 85 ms.**
- [x] A **PDR figure** published, measured on this bench, in both directions. **100.0000 % outbound
      (791,324 frames, 0 `cb_fail`) and inbound (6,337,757 frames, 0 seq gaps) on all six links.**
- [x] **Wi-Fi-stack DRAM measured** (§6 **[MEASURE]**), and §6's ~40 KB trigger evaluated.
      **32,264 B, under the trigger** (2026-10-01).

Supporting evidence the overnight session already produced:

- [x] Potluck Frame codec matching §5.1 byte for byte — golden-byte tests, plus a second independent
      decoder in `host/potluck` agreeing on the same bytes.
- [x] `HELLO` / `HELLO_ACK` / `HEARTBEAT` / `BYE` / `ERR` implemented, and only those.
- [x] ESP-NOW v2 profile with per-peer v1 fallback pinned via `HELLO` (§5.3).
- [x] Heartbeat at 100 ms, death at 6 misses (§8.2), with revival and reboot distinguished.
- [x] `auth_tag` bytes reserved in the MTU arithmetic and zeroed, not enforced (M5 enforces).
- [x] Static allocation; core static DRAM **29,136 B of the 64 KB cap** (`firmware/size/`).
- [x] Host tests green, including under AddressSanitizer.

**Kill**

> If a stable link at your intended geometry cannot be achieved, the transport decision reopens
> **before** anything else is built.

Concretely, on the geometry you actually intend to deploy at:

- [x] Is PDR stable, in **both** directions, over 24 hours? **Yes: 24.52 h at desk range; across the
      house, worst spot 99.17 % out / 99.80 % in, 100 % at six of eight spots (2026-10-04).**
- [x] Is the p99 RTT inside a budget that L3's 500 ms deadline (§4) can live with? **Yes: worst p99
      bucket anywhere in the house 30–42 ms.**
- [x] Does the histogram show a long tail that is *bimodal* rather than merely long? §3's cliff
      between 56 m and 70 m appears as PDR oscillating between 100% and 0, not as gradual decay.
      **No. The indoor edge appears as brief membership dropouts, not a cliff: 9 revivals in 14 min
      at the worst spot while unicast still delivered 99 %.**

**Answered on the intended geometry, 2026-10-04: the kill criterion does not fire.** The distance
sweep covered a two-storey house, up to ~16 m, four walls, a floor, and the shadow of large appliances.
M0 is accepted. Per-spot figures are in M0-LOG session 15 and `captures/sweep-2026-10-04-report.txt`.

If any answer is no, that is the kill criterion firing. **Stop and reopen the transport decision.**
Do not start M1. §14 lists "scope expansion back toward the full matrix" as the highest-likelihood,
fatal risk; the honest response to a failed M0 is to reopen ADR-001, not to build M1 on a link that
does not work.

---

## 7. The delay methodology — read before quoting any number

**M0 measures round-trip time. There is no one-way figure, and there will not be one.**

A one-way delay needs either synchronised clocks, which two ESP32s do not have, or an assumption of
path symmetry, which ESP-NOW's retry machinery does not provide — a frame may sit in up to 31
retransmissions in one direction and none in the other (§3). §3's cited 2782.85 µs figure comes from
an instrumented experiment with equipment M0 does not have; it is a citation, not something this
firmware can reproduce, and §13-M0 asks for numbers "measured on your bench, not cited from this
document".

What is measured, and on whose clock:

| Quantity | Computed as | Clock |
|---|---|---|
| `rtt_us` | reply arrival − probe submit | node A's, both ends |
| `txq_us` | send-callback − probe submit | node A's, both ends |
| `turnaround_us` | reply submit − probe arrival | node B's, both ends |

Every one is a **duration on a single clock**. Durations are comparable across unsynchronised clocks,
which differ in rate by parts per million; instants are not, because they differ by an unknown
offset. That is why `turnaround_us` travels in the `HEARTBEAT` payload as a duration and why no
timestamp does.

The mechanism: each periodic heartbeat sets `ACKREQ` and carries a fresh `msg_id`; the peer answers
*immediately* with a heartbeat carrying that `msg_id` back, `IS_REPLY` set, `ACKREQ` clear, and its
own measured turnaround. Replying immediately rather than piggybacking on the next beacon is what
keeps the turnaround a small measured quantity instead of an artefact of the 100 ms period.

`rtt_us − turnaround_us` is honest — it subtracts one measured duration from another and is named
for exactly that. **Dividing it by two is not**, and does not appear anywhere in this codebase.
`tests/test_stats_json.cpp` and `host/potluck/tests/test_records.py` both assert that no
one-way field exists, so adding one means arguing with a test first.

§13-M0's description line was corrected overnight from "one-way delay histogram" to "round-trip
delay histogram" for this reason; the *Accept* line already said only "delay histogram".

---

## 8. Record these with the numbers

A measurement without its conditions cannot be compared to the next one.

```
date, time, duration
board model and revision, ×2
firmware git revision, and `idf` string from the boot line
distance between boards, in metres
line of sight? obstructions? which room, which floor
Wi-Fi channel, and what else is on it (2.4 GHz congestion, other APs)
mains or battery; USB hub or direct
ambient temperature
```

Then the results:

```
outbound PDR, node 1 -> node 2
inbound  PDR, node 1 <- node 2
RTT min / p50 interval / p99 interval / max, each direction
the 16 histogram bucket counts
Wi-Fi-stack DRAM, and whether it exceeded §6's ~40 KB
deaths declared, revivals, reboots seen
bad frames, unknown peers, queue overruns
```

Paste them into M0-LOG.md under a new `## Bench results` heading, tagged as **measured** with the
date — §6's convention, and the difference between a fact and a design budget.

---

## 9. If something goes wrong

| Symptom | Where to look |
|---|---|
| `cc1.exe: No such file or directory` on a file that exists | the non-ASCII path. Use `tools\build_firmware.ps1`, never bare `idf.py` in `firmware/` |
| `idf.py` not found | ESP-IDF's `export.ps1` (under `IDF_PATH`) has not run — the build script does this for you |
| No `boot` line at all | wrong baud. The console runs at 115200 (`potluck-capture`'s default); 921600 is the frame link's rate, on the adapter |
| Boards never discover each other | different channels, or one board's radio failed to start — the firmware refuses to run a soak on a dead radio and says so on the first line |
| `espnow_ver: 1` unexpectedly | one board reports ESP-NOW v1, so the link is pinned to 226 B. Not an error, but note it: a v1 receiver silently *truncates* a longer frame (§5.3) |
| `rtt-samples` stuck at 0 | probes leaving, replies not arriving. Check `tx-pdr%` — if it is high, the peer is receiving but not answering |
| PDR high but `rx queue overruns` climbing | the link task is behind, not the radio. A Potluck problem, and never folded into PDR |
| `bad_frame` climbing | something else on the channel, or a truncated frame. `--show-frames` with the tee on will name the rejection reason |
| A `link` record missing fields | firmware/host skew. `potluck-capture` prints a warning naming unrecognised record types |

## 10. M4: CAN on two boards

> **Sections 10-14 are the manual path.** They were written before `potluck apply` (section 15), and they pick
> build variants, firmware and package counters, and the order of boards by hand. For a normal cell, `apply`
> now does all of that from the cluster file: it reads every board's floors and goes one above, builds the one
> image the cell needs, and updates the cabled board last. These sections are kept for the experiments that need
> a variant of their own (CAN, a bench image) and as a description of what `apply` does underneath. Their board
> names, ports and node ids are the reference bench's (see the top of this runbook); the keys they name live in
> the git-ignored `keys/` folder.

Wiring: [bench/m4-can-wiring.html](bench/m4-can-wiring.html). Each board runs a **radio-off variant**
(v1 has no router, so a node on two transports would appear as two peers), built in its own
`build-<variant>` directory so it can never be mistaken for the normal build:

```
tools\build_firmware.ps1 -Clean -Variant can-flood -Extra "CONFIG_POT_RADIO_DISABLE=y","CONFIG_POT_CAN=y","CONFIG_POT_M4_FLOOD=y" -Flash -Port COM4
tools\build_firmware.ps1 -Clean -Variant can-safe  -Extra "CONFIG_POT_RADIO_DISABLE=y","CONFIG_POT_CAN=y","CONFIG_POT_M4_SAFE_STATE_MS=1000" -Flash -Port COM5
```

Run builds from PowerShell directly. `-Clean` after a commit re-stamps the version. Check the
variant's `build-<variant>\sdkconfig` for the `CONFIG_POT_*` lines before trusting a run.

**One board alone:** add `"CONFIG_POT_CAN_LOOPBACK=y"` for the controller's self-test (no
transceiver needed; RX is put on the TX pin).

**Read, without resetting the boards** (DTR/RTS held low; needs pyserial, which the ESP-IDF Python
environment has):

```
python tools\can_capture.py 300 captures\m4-can-<date>.log
python tools\can_report.py captures\m4-can-<date>.log
```

Each board prints a `{"t":"can"}` line every 10 s. What a pass looks like (session 20):

- B: `ss_rx` climbs with C's `ss_sent`; `ss_rx_gaps` 0 (`ss_rx_restarts` counts C rebooting under B);
  `safe_state_rx` events in the stream carry consecutive counters.
- C: `ss_min_us`/`ss_max_us`, submit to on-the-wire. Loopback baseline ~320 µs (one frame on an idle
  bus); under flood 335–683 µs. C's `arb_lost` stays 0.
- Both: zero `bit_err`, `form_err`, `stuff_err`, `tx_fail`. `ack_err` only while the other board is
  absent or being flashed.

**Afterwards, put every board back on the normal build** and prove it:

```
tools\build_firmware.ps1 -Clean -Flash -Port COM4      # then the same with -Port COM5 and -Port COM3
python -m esptool --chip esp32s3 -p COM4 verify-flash 0x540000 firmware\build\potluck_m0.bin
```

`0x540000` is the `factory` app partition (`firmware/partitions.csv`; it was at 0x10000 until 2026-10-11).
`firmware\build\flash_args` lists every offset a cable flash writes.

Flash all three from one build: beacon formats must agree across the cell.

## 11. M11: firmware over the cell

The boards' partition table has two app slots for updates (`ota_0`, `ota_1`) after `factory` and the module
slots, and a bootloader built with ESP-IDF's app rollback. Both need one cable flash (`@flash_args`, which
also resets `otadata` so the board boots `factory`); after that, updates go over the radio. From
`host/potluck/`, with the CP2102 on COM6:

```
python -m potluck.fw --port COM6 status --targets 6300,7368,8160
python -m potluck.fw --port COM6 rollout ../../firmware/build/potluck_m0.bin --counter N --key ../../keys/deploy.key --bcert ../../keys/deploy.bcert --targets 7368,8160,6300 --image-for 7368=../../firmware/build-extender/potluck_m0.bin
```

- `N` must be above every board's floor (`status` shows it). The default order is B, C, then A (the cable's own
  board last); `--targets` changes it. A board that does not confirm stops the rollout.
- Each board: ~90 s of transfer at ~9 KiB/s, a reboot, then 100 heartbeats with a radio peer before it keeps the
  image (~15 s). Unconfirmed 120 s after boot (`CONFIG_POT_FW_TRIAL_TIMEOUT_MS`), it rolls back by itself; an
  image that crashes is rolled back by the bootloader.
- Record the consoles meanwhile without resetting them: `python tools/console_tap.py COM5 600 captures/x.log`.
  Count deaths from the `peer_dead` events, never from sampled state.
- Refusals: `python tools/m11_bench.py --port COM6 --target 8160 refuse --key ... --bcert ...` (seven checks,
  4 KB images, a few seconds each).
- An experiment image (a bench that never joins the cell) can go out the same way: it never confirms, and one
  reset (`tools/console_tap.py` does not reset; esptool's or RTS's does) puts the board back on its image.

## 12. M7: guest actors in the WebAssembly sandbox (off by default)

A guest is a WebAssembly module from an author the owner certifies but does not trust with a node. It runs
on boards built with `CONFIG_POT_WASM` (variants `m7-node`, and `extender-m7` for B), as a portable actor in
the sandbox: fuel per tick, its own memory, nine imports (`guests/potluck_guest.rs`), and read-only outputs
under `act/<actor>/`. A board without the option refuses an image that carries guests. (B's image needs the
Wi-Fi extender app's sources, which live in a private repository; the release image carries the app compiled
in.) From `host/potluck/`:

```
tools\build_firmware.ps1 -Clean -Variant m7-node -Extra "CONFIG_POT_WASM=y"
tools\build_firmware.ps1 -Clean -Variant extender-m7 -Extra "CONFIG_POT_WASM=y" -PmeHotspot <the Wi-Fi extender app's firmware\components>
python -m potluck.fw --port COM6 rollout ../../firmware/build-m7-node/potluck_m0.bin --counter N --key ../../keys/deploy.key --bcert ../../keys/deploy.bcert --targets 7368,8160,6300 --image-for 7368=../../firmware/build-extender-m7/potluck_m0.bin

# once per author (the owner): a key for the author, certified by the cluster CA as role 3
python -m potluck.signing keygen --role guest --label acme --out ../../keys/acme
python -m potluck.enrol --guest-cert ../../keys/acme.pub --author 7 --ca-key ../../keys/ca.key --out ../../keys/acme.gcert
# per module (the author): sign it into a bundle
python -m potluck.guest sign ../../guests/build/overheat_alarm.wasm --key ../../keys/acme.key --cert ../../keys/acme.gcert --out ../../keys/overheat_alarm.guest.json
python -m potluck.guest verify ../../keys/overheat_alarm.guest.json --ca ../../keys/ca.pub
# the owner: the manifest names the guest by its sha256 (manifests/m7-guests.json), then sign and deploy as usual
python -m potluck.signing sign ../../manifests/m7-guests.json --key ../../keys/deploy.key --cert ../../keys/deploy.cert --counter N --out ../../keys/m7-guests-N.pkg.json
python -m potluck.ctl --port COM6 --node 6300 deploy ../../keys/m7-guests-N.pkg.json --ca ../../keys/ca.pub --key ../../keys/deploy.key --bcert ../../keys/deploy.bcert --guests ../..
```

- Bundle paths in a manifest are relative to `--guests` (here the repository root). `ctl deploy` checks every
  author signature against the CA before sending; `--skip-guest-check` sends anyway, only to test that the
  nodes refuse it (they check again before committing, and again before each guest first runs).
- Consoles: `{"t":"guest"}` per running guest each stats period, `{"t":"guest_fault"}` on every failed tick.
  Record them with `tools/json_capture.py` (B holds credentials), read the outputs from A with
  `python tools/m7_guests.py watch --port COM6 --seconds 900 --out captures/<name>-watch.jsonl`, and
  summarise with `python tools/m7_guests.py report captures/<name>`.
- Example guests: `guests/overheat_alarm.rs` (a vendor's alarm with a checkpoint), `guests/endless_tick.rs`
  (never returns). `python tools/build_guests.py` rebuilds them with `rustc` for `wasm32-unknown-unknown`; the
  compiled files are checked in.

### The sandbox experiment (2026-10-10)


```
tools\build_firmware.ps1 -Variant m7-wasm -Extra "CONFIG_POT_WASM=y","CONFIG_POT_WASM_BENCH=y"   # the bench
tools\build_firmware.ps1 -Variant m7-node -Extra "CONFIG_POT_WASM=y"                             # a node with it
python tools/m7_bench.py duty --target COM5 --samples 2000000 --runs 6 --out captures/m7-duty    # a guest in a running cell
python tools/wasm_modules.py                                                                     # regenerate the test modules
```

The bench prints `{"t":"wasm_bench"}` lines and stops; deliver it with `potluck.fw rollout --targets 8160` and
reset the board afterwards to roll it back. `POT! wasm <samples> <runs>` runs M9's kernel in the sandbox on a
node built with `CONFIG_POT_WASM`. The host tests (`tests/test_wasm.cpp`) run the same interpreter and modules.

## 13. M9: borrowing an idle core

The job (`McJobActor`) runs on A and lends pieces to `McLenderActor` on B and C; both are in
`manifests/m9-mc.json`. Sign and deploy that package at the next counter, from `host/potluck/`:

```
python -m potluck.signing sign ../../manifests/m9-mc.json --key ../../keys/deploy.key --cert ../../keys/deploy.cert --counter N --out ../../keys/m9-mc-N.pkg.json
python -m potluck.ctl --port COM6 --node 6300 deploy ../../keys/m9-mc-N.pkg.json --ca ../../keys/ca.pub --key ../../keys/deploy.key --bcert ../../keys/deploy.bcert
```

Then drive the job from A's console, with the IDF Python -- no host joins the cell, so nothing else may hold
COM6 -- while B's and C's consoles are recorded, and compare the windows:

```
python tools/json_capture.py COM4 captures/m9-run-COM4.jsonl 900                     # and COM5, in two other terminals
python tools/m9_bench.py COM3 captures/m9-run-COM3.jsonl --runs 3                    # alone, then lending
python tools/m9_report.py captures/m9-run-COM3.jsonl captures/m9-run-COM4.jsonl captures/m9-run-COM5.jsonl
```

`--reset-port COM4` on the bench resets B in the middle of every lending run (M9's "a lender held in reset
loses no result"); the report compares the lenders' heartbeat delivery, round trips and die_temp output
across the idle, alone and lending windows.

## 14. M10: the PC as a placement node

Once, enrol the PC as node 0x00fe (the identity file stays in the git-ignored `keys/`):

```
build\tests\pot_hostnode.exe --keygen keys\host-00fe.id
python -m potluck.enrol --host-cert keys\host-00fe.id --pub <the printed public key> --node 0x00fe --ca-key keys\ca.key
```

Then deploy a package whose manifest makes the host eligible (`"placement": true`; `manifests/m10-ticker.json`),
switch on A's relay (`POT! relay 1` on A's console; saved across reboots since M12 -- `POT! relay 0` when done, since
one relay per cell is kept, the lower id's, and A's would take the role from B the extender) and join, from `host/potluck/`:

```
python -m potluck.hostnode --port COM6 --package ../../keys/m10-ticker-N.pkg.json --identity ../../keys/host-00fe.id --out ../../captures/m10-host.jsonl
python tools/m10_report.py captures/<name>        # from the repo root: who ran the actor when, on one clock
```

`pot_hostnode` is built by the host test build (`tools\run_host_tests.ps1`). Only one program may hold COM6:
not `potctl` or `potluck-agent` at the same time. Stopping the program without warning is what a pulled
cable looks like to the boards; the actor should be back on a board within ~0.7 s.

## 15. M12: the whole cell from one file

From `host/potluck/`, with the ESP-IDF Python, in any shell. `<cluster file>` is your cell's file -- start from
a preset in `examples/cells/` (`minimal.json`: Potluck and the sandbox, no package; `hello.json`: the worked example of
DEVELOPING-APPS.md), copied beside your keys with your boards' node ids and adapter port in it (GETTING-STARTED.md,
"Your first package", shows one in full). From here on the runbook's examples write `my-cell.json`.

```
python -m potluck.apply <cluster file> --plan         # what it would change; changes nothing
python -m potluck.apply <cluster file>                # firmware where it differs, the package, verify
python -m potluck.apply <cluster file> inventory      # every board: firmware, package, counters, actors
python -m potluck.deploy decl <manifest> --modules <descriptor>  # an actor's declaration bytes, for a test
```

Counters, variants and the order are the tool's: it reads every board's floors and goes one above. Builds come
from `git archive` exports into a cache (`%LOCALAPPDATA%\potluck\cache` on Windows, `~/.cache/potluck` elsewhere;
`--cache` names another), keyed by the firmware tree and the build script, so a commit that changes neither sends
nothing to the boards (a first build is ~7-9 min, a board's update ~2 min). For acceptance tests, keep variants of
the file beside it -- an app switched off (a live package change), one board left out (the catch-up test), another
cluster name -- and files that must be refused before any change.

- **A whole-cell reset with every console recorded:** `python tools/reset_capture.py captures/<name>.log 25 75`
  (JSON lines only, so safe for B; all ports in one process). Do not start several `console_tap.py` jobs at once:
  one such start reset C.
- **Checkpoints in flash:** the `rec` line carries `ck_persisted`, `ck_restored`, `ck_nvs_writes`, `ck_nvs_errors`,
  `ck_nvs_last_err`, `nvs_used` and `nvs_available`; each restored copy prints `{"t":"ck_restored",...}` at boot.
- **Live changes:** a board prints `{"t":"package_live",...}` with what it kept, stopped and started.

## 16. M13: the cell presents itself

What the owner adds to a cluster file (section 15), and what each line does:

```json
"firmware": {"potluck": {...}, "modules": ["sandbox", "present", "wifi"]},
"present": {"broker": "192.168.1.50:1883", "keepalive_s": 10, "period_ms": 2000},
"outside": {"node-7368/hw/die_temp": "read", "node-7368/led/on": "read_write", "act/<actor>/<output>": "read"}
```

- `present` needs a network connection in the same image: Potluck's own station (`"wifi"`; give a board the
  network with `tools/wifi_set.py <port> <file>`, which the owner runs and which prints neither line), or the Wi-Fi
  extender app on a board whose hotspot has credentials. The role goes to one board that is on the network (its
  HELLO carries a `connected` bit) and moves when it dies.
- `outside` is what the cell presents over MQTT; `read_write` needs a resource declared writable in some
  node's `owns` and never one marked `safety_relevant`. The owning board checks again. Anyone who can publish on
  the broker can write every `read_write` resource: give the broker a login and use TLS (section 18).
- A broker with a login: `POT! present login <user> <password>` on each board that may present (the owner types
  it; it prints only the result).
- `builtin:led_switch` (pinned, `r` `g` `b`, `blink_ms`) is B's status LED as `node-<id>/led/on`, the bench's
  stand-in for a lightbulb (`manifests/m13-present.json`).

**A bench broker.** For a real installation use your own broker (HOME-ASSISTANT.md). For a run that wants every
message logged with its own clock, `tools/mqtt_bench_broker.py` is a small MQTT 3.1.1 broker in one file
(standard library only, nothing to install); run it on any machine the boards can reach, here `192.168.1.50`:

```
python3 tools/mqtt_bench_broker.py --port 1883 --log broker.jsonl                    # on the broker machine
python tools/mqtt_tool.py --host 192.168.1.50 sub '#' --out captures/<name>.jsonl     # what Home Assistant would receive
python tools/mqtt_tool.py --host 192.168.1.50 pub potluck/<cell>/<hash>/set ON        # what an automation sends
```

`<cell>` is the CA fingerprint's 8 hex digits (in every topic); `<hash>` the resource's path hash, FNV-1a/32 of
its full URI (`python -c "from potluck.paths import path_hash; print('%08x' % path_hash('potluck://lab/node-7368/led/on'))"`
from `host/potluck/`). The presenting board prints `{"t":"present",...}` every stats period: holding, connected,
messages published, outside writes applied, sent and refused, and its internal heap (free, largest block, lowest).

**M13.1: the pocket page.** Add `"pocket"` to `firmware.modules`. On each board that should answer a phone, the owner
types `POT! pocket ap <name> <password>` (WPA2, 8-63 characters; it prints only the result) and resets it -- or runs
`<IDF python> tools/pocket_set.py <port> <file>` with the two in a file, which does both and prints neither: the board
then makes that access point on the cell's channel, beside the station ESP-NOW rides on, and serves
`http://192.168.4.1/` (the page, refreshing itself every 5 s) and `/status.json`. Every other method is answered 405,
every other path 404. Without a name and password there is no access point; on an extender whose hotspot is up the
module stays idle (the extender owns the access point). The board prints `{"t":"pocket",...}`: phones on it, pages
served, requests refused, its internal heap.

**M26: writes from the pocket page.** Off by default. The owner turns them on per board with
`<IDF python> tools/pocket_set.py <port> --login <file>` (one line, the page's login password, 12-63 characters; only a
keyed BLAKE2b hash is kept, it takes effect at once) and off with `--login-off`. A phone that logs in gets a session
(an HttpOnly, SameSite=Strict cookie) and a separate form token; then each value the package marks writable from
outside -- never a safety-relevant one -- has a small form. The board writes its own resource and passes another
board's to its owner as an outside write, checked again there. Refused: no session, a wrong token, a Host or Origin that
is not the board's own address, a value with no reading yet, more than 2 writes a second (bursts of 4). Five wrong
passwords lock the login for a minute, each lockout after that twice as long (to 16 minutes) until a right one; a
session ends after 15 idle minutes. **The limit, plainly:** the page is plain HTTP over the access point's WPA2, so
anyone who knows the access point's password can read what crosses it -- the login password and the cookie included;
keep that password as private as the login. `{"t":"pocket",...}` adds writes_on, logins, bad passwords, lockouts,
bad sessions, tokens and origins, rate-limited and applied writes, and the auth state's bytes (about 450; M26's kill
line is 4 KB). A security review before it shipped found and closed: replies sent while holding the page's lock (the
node's task could stall), a long Origin read as none, a shared note, guessed value types, and a short-password and
lockout weakness. A switch takes on/off, true/false or 1/0 in any case (a phone's keyboard capitalises the first
word); after **set** the page shows the board's answer -- `applied`, `sent to its board`, or why it was refused.

## 17. M16: the dashboard, and the mock

The dashboard is three static files in `host/potluck/potluck/dashboard/` (`index.html`, `app.js`, `style.css`; no
framework, nothing fetched from the internet) that the hub (M14) serves at `/` (with `--listen`, over HTTPS behind a login). It reads only the hub's API
version 1 (`host/potluck/HUB-API.md`): `/api/stream`, falling back to `/api/model` and `/api/events` every 2 s when
the stream fails. For work without boards, `tools/hub_mock.py` serves the same files and the
same API from a simulated cell of three boards (A 6300 cabled, B 7368 extender and presenter, C 8160). The mock opens
no serial port and touches no board; any Python 3.10+ runs it, from the repository root:

```
python tools/hub_mock.py                       # then open http://127.0.0.1:8470/ (desk or phone browser on this PC)
python tools/hub_mock.py --kill 8160@20        # C goes silent after 20 s, dead one death window (3 s) later
python tools/hub_mock.py --stale --reverted --other-cell --link-down --asleep 6300
```

Faults can also be set while it runs: `http://127.0.0.1:8470/mock/fault?kill=8160&after=5`, `?revive=8160`,
`?stale=1`, `?reverted=1`, `?other_cell=1`, `?link_down=1`, `?asleep=6300`, `?wake=6300` (`=0` clears a switch).
`/mock/...` is the mock's own and not part of the API. Only `potluck://lab/node-7368/led/on` is writable from outside
(the Data tab shows it as a switch); any other write gets 403, as the contract says. There is no deploy control.

The page: the first screen answers "is anything broken?" (problems first, then tiles for boards, apps,
integrations, the last deploy, the hub's link and the channel, each linking to the tab that explains it), then the
four golden signals, then utilisation and saturation per resource (cell-wide saturation, heap per board, every
radio link), then the tabs Cell, Apps, Data, Integrations, Deploys, Events. Links are `#<tab>/<item>`, e.g.
`#cell/8160` or `#data/potluck://lab/node-7368/hw/die_temp`. Tests: `python -m pytest -q
host/potluck/tests/test_dashboard.py` (the mock against the contract, the page's landmarks; no browser needed).

## 18. M13-M15 (2026-10-13): TLS to the broker, the cell's leases, the host node behind the hub, telemetry

**TLS to the broker.** The cluster file's `present` block takes `"tls_ca": "<file>"` (the broker's CA, PEM or DER,
relative to the cluster file) and a broker on its TLS port (8883 when `"broker"` names no port); `apply` puts the CA's
DER in the signed manifest and the image carries it (version 4). On the broker's side that is a TLS listener with a
certificate signed by that CA -- for Mosquitto, a `listener 8883` block with `cafile`, `certfile` and `keyfile`.
The present status line shows `tls`, PSRAM free and low-water, and `sender_stack_free`.

**Aggregation.** A cell that presents is built with `CONFIG_ESP_WIFI_AMPDU_TX_ENABLED=n` on every board; `apply --plan`
says so. With aggregation on, a board presenting over its own station wedged its radio every 10-60 minutes (ESP-NOW
NO_MEM and the station's TCP stopped until a chip reset); with it off, 13 h 19 min clean (M0-LOG session 42).

**Leases.** Nothing to run: `apply` takes the cell's deploy lease after planning and says
`lease: the cell's deploy lease is ours (board 0x6300 holds it)`; a second `apply` meanwhile, from any PC, is
`REFUSED, nothing changed: the cell's deploy lease is held by "apply on <PC>" ...`. A resident hub takes the hub lease;
a second hub on the same PC is refused from `%LOCALAPPDATA%\potluck\hub.json` (`~/.cache/potluck/hub.json` elsewhere),
one on another PC by the lease. A cell
on firmware before M14 answers no LEASE: noted, not refused.

**The placement host behind the hub.** With a hub holding COM6, `python -m potluck.hostnode --port COM6 --package
../../keys/<m>.pkg.json --identity ../../keys/host-00fe.id` goes through the hub by itself (`--hub [host:port]` to name
it); potctl, the agent and apply run beside it. It refuses an old `pot_hostnode` (`pot_hostnode --caps`): rebuild with
`tools\run_host_tests.ps1`.

**Telemetry.** The hub pulls every board's health, links, actors and events every `--telemetry-ms` (default 10000;
0 never): `python -m potluck.hub --cluster <cluster file> --telemetry-ms 10000`. The model's `boards[].heap`,
`links`, `rssi` and `telemetry` fill from it (HUB-API.md); the dashboard's heap and radio-link tables show it. From a
script: `from potluck import telemetry as tm; tm.health(bridge, 0x7368); tm.links(bridge, 0x7368)`.
`build/tests/pot_sim.exe --nodes 20 --scenario telemetry --tel-ms 10000` is the pull's airtime at 20 boards.

## 19. M20: the setup wizard, the console link, the floor plan and the library

Everything here is on the hub's own address, http://127.0.0.1:8470/ -- never on `--listen`.

- **The setup wizard:** `setup.html` ("Add boards" in the dashboard's header). On a PC with no cell the hub starts with
  no port and no cluster file (`python -m potluck.hub` from `host/potluck/`) and the page opens on "Make your cell": a
  name and a passphrase; the first board plugged in then becomes the hub's link. On a cell, Add boards: plug the new
  boards in **after** the page has opened -- a port that was there before is never probed, because checking a board
  (`esptool flash-id`) restarts it.
- **The frame link over a board's own USB console:** in the cluster file, `"port"` is that board's COM port and `"baud":
  115200` (the console's rate; the adapter on GPIO17/18 stays at 921600). One host per board: a board answers on the UART
  it last heard from, so never run the hub on the adapter and a tool on the same board's console at once.
- **Seeing a board's log on its own cable:** `POTLUCK_CONSOLE_TEXT=<file>` makes `Bridge` (the hub, `apply`,
  `potluck.fw`, `potctl`) append the log text between frames to that file; add `POTLUCK_CONSOLE_TEXT_SAFE=1` to keep only
  update and crash lines -- use it for any board that holds Wi-Fi credentials, whose log can name networks.
- **The floor plan:** the Plan tab. Make a plan (100 x 100 m by default), add rooms, drag boards; each drag is saved at
  once in the cluster file (`<file>.bak` keeps the version before), and the tab names the boards whose place is not on
  the board yet -- "Apply to the cell" sends it (a package change, about 20 s, no restart).
- **The library:** the Library tab -- apps on and off, "Test the broker", per resource Hidden / Visible / Controllable,
  then "Apply to the cell".
- **pot_sim for an app's read load:** `build/tests/pot_sim.exe --nodes 20 --scenario d20` (a room-presence app's design: a
  fusion per room) or `--scenario perlink` (the design before it), against `--scenario none` on the same seed.
- **The packaged app and the tray icon:** `tools\build_package.ps1` builds `dist\Potluck\` (`potluck.exe`: hub, apply,
  setup, keystore, ctl...; `potluck-tray.exe`: the tray icon) with PyInstaller from `%LOCALAPPDATA%\potluck\build-env`
  (made once; the script's top says how). The owner installs it with `packaging\install.ps1` -- per user, so run it
  in your own PowerShell as the user who will use it -- and removes it with `packaging\uninstall.ps1`. The tray starts
  a hub at login when none runs (the cell in `%LOCALAPPDATA%\potluck\tray.json`'s "cluster", else the one the wizard
  made, else setup mode), and starts one again whenever the hub stops answering for three polls (9 s), at most once a
  minute. Unpackaged: `python -m potluck.tray --start-hub`.
- **esptool in the package** is `Potluck\esptool\esptool.exe`, a program of its own (GPLv2+, with its `LICENSE` and
  `SOURCE.txt`), never imported by the Apache-2.0 hub: `potluck.setup` runs it (`python -m esptool` from source,
  `POTLUCK_ESPTOOL` to name another).
- **The release firmware in the package:** `tools\build_package.ps1` also runs `python -m potluck.release
  dist\Potluck\firmware` -- with `-FromCluster <cluster file>`, the image `apply` builds for that cell, with the built-in
  apps it names (the published release is built this way); without it, Potluck and the sandbox only -- its flashed
  files and `release.json`, so the wizard on a PC with no ESP-IDF flashes that (`-NoFirmware` leaves it out). Building
  it needs ESP-IDF on the packaging PC, once.
- **A release (what HACS updates from):** `python tools/make_release.py X.Y.Z` sets the integration's manifest version,
  commits and tags `vX.Y.Z` locally and prints the two publishing commands; `--publish` runs them (push, then `gh release
  create`, attaching `dist\Potluck` as a zip when it is built from this firmware tree). HACS reads the newest *release*;
  with none it offers every commit on main as an update.
- **The store (M20):** Library tab, "Apps from GitHub" -- off until ticked (the cell's `store.json`). "Look" fetches a
  repository's `potluck-app.json` and module (DEVELOPING-APPS.md, "Publishing a guest for any cell"); "Install" pins
  the author's key to the repository, certifies it (role 3, numbers from 100; the passphrase unlocks a sealed cell's
  CA for that one action), writes `store/` beside the cluster file and its fragment into the package; "Apply to the
  cell" sends it. A guest change reboots the boards into it, as any guest change does. "Show the store's list" fetches
  [Tubifix77/potluck-store](https://github.com/Tubifix77/potluck-store)'s `feed.json` and shows it only if `feed.sig`
  verifies under `potluck.store.FEED_KEY`. **Maintaining a store list** needs the store's private key, whose public
  half is `FEED_KEY`: keep the store key out of the repository and backed up (a new one means a Potluck release with
  its public half). Edit `feed.json`, run `python tools/store_feed.py sign feed.json --key <the store key>`, and
  commit both.
- **The wizard's Cells step (M19):** after Rooms, the wizard asks the hub for a tree (`POST /api/setup/tree`); twenty
  boards or fewer that all hear each other skip it. Otherwise: tick the boards on a charger, see the proposed cells and
  their foremen, "Use these cells" (once every cell has one), then Done applies.
- **Potluck's mark:** `python -m potluck.brandmark` regenerates every copy of it -- the dashboard's SVGs, Home
  Assistant's `custom_components/potluck/brand/icon*.png` -- from one geometry; the tray draws it at run time.

## 20. M19: cells of cells

A tree is a cluster file with `cells` (DEVELOPING-APPS.md's table: each cell's `parent`, `window_ms`, `export`,
`period_ms`; a board's `cells` and `"foreman": true`). `apply --plan` prints every board's peers against 19 and
refuses a tree that does not fit, naming the board; one package carries the whole tree, and each board keeps only its
cells' actors (image version 5).

- **The foreman:** `builtin:foreman`, one per cell with a parent. Its stats line `{"t":"foreman",...}` (cell, parent,
  alive, exports, groups) shows on whichever board holds it; that board's `groups` names both cells. The standby -- the
  board ranked second -- stays in its own cell and joins the parent when it takes the role.
- **Reading across cells:** on a board, `POT! read <path hash>` shows what it holds for a path (owner, quality, the
  value as u32 and f32, unit, age) and asks the owning board again; `POT! refer <via hex> <path hash>` asks for a path that
  lives in another cell as a referral through that cell's foreman, then `POT! read` shows it. Path hashes:
  `python -c "from potluck.paths import path_hash; print(path_hash('potluck://lab/cell-bench/warmest'))"`.
- **pot_sim:** `build/tests/pot_sim.exe --scenario tree2 --cells 10 --minutes 10` -- ten cells of ten in their own
  radio spaces under one upper cell, real reconcilers and foremen; `--kill-at <s>` puts cell 0's foreman to sleep for
  two minutes. It reports deaths, handovers, cell-seconds with no foreman or two, each radio space's airtime (and how
  much of it the reconciler's CASTs), and the sibling summaries read. A day takes about five hours.
- **Stub cells on the bench** (a tree tested with three real boards): the cluster file names stub boards
  (`"stub": true`, ids f000..fdff), which `apply` composes into the tree and never contacts. Build the
  host program optimised -- `tools\run_host_tests.ps1 -Release -BuildDir build/release -Target pot_stubs` (a Debug
  build fell behind its own heartbeats with nine stubs checking signatures) -- make sure the cabled board relays
  (`POT! relay 1`, kept across reboots), then from `host/potluck/`:
  `python -m potluck.stubs --port COM6 --package <the tree's signed package> [--watch f301:cell-bench/warmest]
  [--refer f101:8160:node-7368/hw/die_temp] [--sleep f102@120+90] [--out <file.jsonl>]`. The package is the one
  `apply` kept (`packages\<system>-<counter>.pkg.json` in its cache, section 15); each stub's identity is made
  under the cell's CA the first time (`keys/stubs/`). Its stderr lines: `stub` (groups, peers, roles and the summary
  each reads), `watch`, `event`.
- **The wizard's tree:** `POST /api/setup/tree {"chargers": ["0100", ...]}` on the hub proposes cells from the boards'
  link reports (potluck.survey); `"write": true` puts them into the cluster file, then Apply.
- **Beyond the cable:** in a tree the cable reaches the cabled board's cells only. `apply` names the boards of other cells
  ("is in a cell the cable does not reach") and verifies what the cabled board can resolve; those boards take the
  package and the firmware by catch-up. `apply --partial` lets boards that do not answer catch up (for example when
  folding a tree back into one cell: apply the one-cell file with `--partial`).
- **Firmware catch-up:** a confirmed board announces its firmware counter every minute (first 45 s after boot); a peer
  with an older one asks and takes the holder's own image (1.2 MB in about 33 s, one peer at a time, the asker waits
  three minutes before asking again). The board on the PC's cable leaves its firmware to the PC (it carries the PC's
  rollout to the others); a receive cut off half-way is dropped after 15 s. Its log lines are `fw: ... asked for it`,
  `fw: catch-up to 0x....`, then the receiver's usual `begin`, `commit`, `trial passed`. A board that took its firmware
  before 2026-10-16 (no trailer kept) cannot serve until it takes one over the cell.
- **Radio spaces on the bench:** `POT! deaf <last two MAC bytes>` on two boards, both ways, keeps them out of each other's
  range (`POT! deaf 0` clears; the list is not kept across a reset).

## 21. The task watchdog resets a wedged board

Since `27994a5` (2026-10-16) a task that keeps a core from its idle task for 5 s resets the chip
(`CONFIG_ESP_TASK_WDT_PANIC=y`), where it used to print every 5 s -- C's Wi-Fi task once spun for 6 hours. The boot line
says why a board last started: `fw: running ... ; reset: task watchdog` (or `power-on`, `software`, `panic`, `brownout`,
`usb`...). `POT! hog <ms>` on a console spins core 0 above every task but Wi-Fi's: 3000 must not reset the board, 8000 must
(the watchdog names `pot_hog`, then the board restarts on its confirmed image). Work on the background workers and in the
guest runtime yields every few tens of ms; new long work must too.
