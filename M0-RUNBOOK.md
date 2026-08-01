# M0 runbook — two boards, one heartbeat

**Potluck — S3 Edition.** Everything below was written and verified on **2026-08-01** with **no
hardware attached**. The firmware compiles for ESP32-S3, all 18 gates are green, and the size report is
in the repository.

More than "it compiles": the firmware has been **run** under emulation, and `potctl` has read all six
of a node's built-in resources across the frame link, so §4's read contract and the host bridge are
demonstrated rather than assumed. What has *not* been demonstrated is anything involving a radio —
which is every number §13-M0 asks for. That is what the boards are for, and it is why the acceptance
checklist in [§6](#6-the-acceptance-checklist-13-m0) is still entirely unticked.

Read [M0-LOG.md](M0-LOG.md) first — it records the decisions, the ARCHITECTURE.md corrections made
along the way, the things left open, and the conclusions that were later **withdrawn**. Its newest
session is the one that matters. Then this.

---

## 0. Ninety seconds of orientation

| | |
|---|---|
| What M0 proves | two ESP32s exchange 100 ms heartbeats over ESP-NOW and report **measured** PDR and delay |
| Accept / kill | §13-M0, copied verbatim in [§6](#6-the-acceptance-checklist-13-m0) below |
| Heartbeat timers | 100 ms period, dead at 6 misses = 600 ms (§8.2, wireless row) |
| What you flash | one binary, both boards — node ids derive from each board's MAC |
| Where the numbers land | `potluck-capture`'s summary, printed on Ctrl-C; the capture file has the raw stream |
| The one thing to remember | **there is no one-way delay figure.** M0 measures RTT. See [§7](#7-the-delay-methodology-read-before-quoting-any-number). |

```bash
tools\build_firmware.ps1 -Flash -Port COM7
```

---

## 1. Before you touch a board

Confirm the bench is still as the last session left it:

```bash
tools\run_all_tests.ps1 -Asan -Firmware
```

Expect **`ALL PASS` on all 18 lines**, and the §6 budget line reading **51.9 KB used, 12.1 KB of
headroom**. This takes a few minutes and it is worth it: it proves the toolchain, the codec, the
heartbeat state machine, the namespace, the statistics and the serial format are all still in the state
the measurements will be attributed to.

The 18 gates are: the C++ suite (158 cases, 36,536 checks), the same suite under AddressSanitizer, four
fixture/corpus regenerations, ten Python suites (124 cases — including a differential fuzz run
requiring the C++ and Python decoders to agree on all 9,226 fuzz inputs, and a namespace-wire corpus
requiring them to agree byte-for-byte on all 89 READ/WRITE/REPLY cases), a strict-GCC portability gate
over the portable core, and the firmware build with the §6 budget gate.

Without `-Asan -Firmware` you get the 16 host gates only, which is the fast loop.

If anything fails, stop. A soak run against a broken codec produces numbers about the codec.

### Prerequisites, all already installed on this machine

| | |
|---|---|
| ESP-IDF | **v6.0.2** at `D:\esp\esp-idf` (the `stable` docs channel; see M0-LOG.md) |
| Toolchain | xtensa-esp-elf gcc 15.2.0, cmake 4.0.3, ninja 1.12.1, under `~/.espressif` |
| Host C++ | MSVC 14.51 from VS 18 Build Tools |
| Python | 3.14.4. `pyserial` is needed **only** to read a live port |

If `pyserial` is missing, `tools\capture.ps1` finds ESP-IDF's own Python, which has it. Nothing else
is required — the decoder, the capture writer and the summary are standard library only.

### The ASCII mirror, and why you probably will not see it

ESP-IDF v6.0.2 cannot build from a path containing a non-ASCII character on this machine. That used to
matter enormously, because the repository lived at `D:\Projects\dμOS` and the μ broke the build in two
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
`D:\esp\potluck-fw-<target>` instead. **Never edit the mirror** — it is overwritten on every build,
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

- **PSRAM is deliberately off.** §6's [MEASURE] item is about *internal* DRAM, and with the R8's
  8 MB enabled the Wi-Fi stack could place buffers externally, making the figure unattributable.
  Measure the baseline first; enable PSRAM and measure again as a separate run if you want that
  number too.
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

Every built-in resource, each with its whole tuple:

```
heap free           213400 B  [GOOD, age 812 ms, ts 41230, class L3]
heap largest block  180224 B  [GOOD, age 812 ms, ts 41230, class L3]
uptime              41       [GOOD, age 812 ms, ts 41230, class L3]
boot epoch          27       [GOOD, age 812 ms, ts 41230, class L3]
peers alive         1        [GOOD, age 812 ms, ts 41230, class L3]
worst peer RSSI     -58      [GOOD, age 812 ms, ts 41230, class L3]
```

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

Expect `6/6 answered with a usable value`. `watch sys/peers-alive` should show `0` then `1` as the
host's own heartbeats register it as a live peer, which is the quickest confirmation that §8.2's state
machine is running and that the frame link works both ways.

**⚠ One `potctl` per QEMU run.** QEMU's `-serial tcp:…,server,nowait` accepts exactly **one** connection
for the lifetime of the VM. The second invocation against the same run fails at connect —

```
BridgeError: write to tcp 127.0.0.1:5555 failed: could not connect: timed out
```

— and that is the emulator, not the node: the console log keeps filling with statistics throughout. So
do everything in one invocation (`ls` reads all six resources over a single connection; `watch` holds
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

- [ ] 24-hour soak at a 100 ms heartbeat, both boards, uninterrupted.
- [ ] A **delay histogram** published, measured on this bench — not cited from ARCHITECTURE.md.
- [ ] A **PDR figure** published, measured on this bench, in both directions.
- [ ] **Wi-Fi-stack DRAM measured** (§6 **[MEASURE]**), and §6's ~40 KB trigger evaluated.

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

- [ ] Is PDR stable, in **both** directions, over 24 hours?
- [ ] Is the p99 RTT inside a budget that L3's 500 ms deadline (§4) can live with?
- [ ] Does the histogram show a long tail that is *bimodal* rather than merely long? §3's cliff
      between 56 m and 70 m appears as PDR oscillating between 100% and 0, not as gradual decay.

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
| `idf.py` not found | `D:\esp\esp-idf\export.ps1` — the build script does this for you |
| No `boot` line at all | wrong baud. The firmware sets 921600; `--baud 115200` if you changed `sdkconfig` |
| Boards never discover each other | different channels, or one board's radio failed to start — the firmware refuses to run a soak on a dead radio and says so on the first line |
| `espnow_ver: 1` unexpectedly | one board reports ESP-NOW v1, so the link is pinned to 226 B. Not an error, but note it: a v1 receiver silently *truncates* a longer frame (§5.3) |
| `rtt-samples` stuck at 0 | probes leaving, replies not arriving. Check `tx-pdr%` — if it is high, the peer is receiving but not answering |
| PDR high but `rx queue overruns` climbing | the link task is behind, not the radio. A Potluck problem, and never folded into PDR |
| `bad_frame` climbing | something else on the channel, or a truncated frame. `--show-frames` with the tee on will name the rejection reason |
| A `link` record missing fields | firmware/host skew. `potluck-capture` prints a warning naming unrecognised record types |
