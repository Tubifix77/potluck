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

- **PSRAM is on** (since M6, `sdkconfig.defaults.esp32s3`): buffers that only tasks touch live in the R8's
  8 MB, and section 6's gate counts internal RAM only. (Until M6 it was deliberately off, so the M0
  measurement of the Wi-Fi stack's internal DRAM was unambiguous.) A board without PSRAM needs
  `CONFIG_SPIRAM=n`, which compiles the reconciler out.
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
sweep covered a two-storey house, up to ~16 m, four walls, a floor, and a dryer-and-fridge shadow.
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
| `idf.py` not found | `D:\esp\esp-idf\export.ps1` — the build script does this for you |
| No `boot` line at all | wrong baud. The firmware sets 921600; `--baud 115200` if you changed `sdkconfig` |
| Boards never discover each other | different channels, or one board's radio failed to start — the firmware refuses to run a soak on a dead radio and says so on the first line |
| `espnow_ver: 1` unexpectedly | one board reports ESP-NOW v1, so the link is pinned to 226 B. Not an error, but note it: a v1 receiver silently *truncates* a longer frame (§5.3) |
| `rtt-samples` stuck at 0 | probes leaving, replies not arriving. Check `tx-pdr%` — if it is high, the peer is receiving but not answering |
| PDR high but `rx queue overruns` climbing | the link task is behind, not the radio. A Potluck problem, and never folded into PDR |
| `bad_frame` climbing | something else on the channel, or a truncated frame. `--show-frames` with the tee on will name the rejection reason |
| A `link` record missing fields | firmware/host skew. `potluck-capture` prints a warning naming unrecognised record types |

## 10. M4: CAN on two boards

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
tools\build_firmware.ps1 -Clean -Flash -Port COM4      # then idf.py -p COM5 flash, -p COM3 flash
python -m esptool --chip esp32s3 -p COM4 verify-flash 0x10000 firmware\build\potluck_m0.bin
```

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
under `act/<actor>/`. A board without the option refuses an image that carries guests. From `host/potluck/`:

```
tools\build_firmware.ps1 -Clean -Variant m7-node -Extra "CONFIG_POT_WASM=y"
tools\build_firmware.ps1 -Clean -Variant extender-m7 -Extra "CONFIG_POT_WASM=y" -PmeHotspot D:\Projects\poor-mans-extender\firmware\components
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

From `host/potluck/`, with the ESP-IDF Python, in any shell. The bench's cell is `keys/desk-cell.json` (git-ignored:
it names the private app repositories by path); `examples/cells/` holds the public presets.

```
python -m potluck.apply ../../keys/desk-cell.json --plan       # what it would change; changes nothing
python -m potluck.apply ../../keys/desk-cell.json              # firmware where it differs, the package, verify
python -m potluck.apply ../../keys/desk-cell.json inventory    # every board: firmware, package, counters, actors
python -m potluck.deploy decl <manifest> --modules <descriptor>  # an actor's declaration bytes, for a test
```

Counters, variants and the order are the tool's: it reads every board's floors and goes one above. Builds come
from `git archive` exports into `%LOCALAPPDATA%\potluck\cache`, keyed by the firmware tree and the build script,
so a commit that changes neither sends nothing to the boards (a first build is ~7-9 min, a board's update ~2 min).
The acceptance variants beside it: `desk-no-room.json` (the room sensor off, live), `desk-c-off.json` (C left out,
for the catch-up test), `desk-home.json` (cluster "home"), `fail-*.cluster.json` (refused before any change).

- **A whole-cell reset with every console recorded:** `python tools/reset_capture.py captures/<name>.log 25 75`
  (JSON lines only, so safe for B; all ports in one process). Do not start several `console_tap.py` jobs at once:
  one such start reset C.
- **Checkpoints in flash:** the `rec` line carries `ck_persisted`, `ck_restored`, `ck_nvs_writes`, `ck_nvs_errors`,
  `ck_nvs_last_err`, `nvs_used` and `nvs_available`; each restored copy prints `{"t":"ck_restored",...}` at boot.
- **Live changes:** a board prints `{"t":"package_live",...}` with what it kept, stopped and started.

## 16. M13: the cell presents itself

What the owner adds to a cluster file (section 15), and what each line does:

```json
"firmware": {"potluck": {...}, "modules": ["sandbox", "present"], "extender": {...}},
"present": {"broker": "192.168.0.77:1883", "keepalive_s": 10, "period_ms": 2000},
"outside": {"node-7368/hw/die_temp": "read", "node-7368/led/on": "read_write", "act/rf_fusion/out": "read"}
```

- `present` needs a network connection in the same image: the extender (a board whose hotspot has credentials)
  or Potluck's own station (`"wifi"`; give a board the network with `tools/wifi_set.py <port> <file>`, run by the
  owner). The role goes to one board that is on the network (HELLO's `connected` bit) and moves when it dies.
- `outside` is everything another system may see; `read_write` needs a resource declared writable in some
  node's `owns` and never one marked `safety_relevant`. The owning board checks again.
- A broker with a login: `POT! present login <user> <password>` on each board that may present (the owner types
  it; it prints only the result).
- `builtin:led_switch` (pinned, `r` `g` `b`, `blink_ms`) is B's status LED as `node-<id>/led/on`, the bench's
  stand-in for a lightbulb (`manifests/m13-present.json`).

**The bench broker.** Home Assistant and Mosquitto run on the homelab since 2026-10-12 (HOME-ASSISTANT.md); for a
run that wants every message logged with its own clock, the bench also has a small MQTT 3.1.1 broker, run from `/tmp` on the homelab laptop (nothing
installed):

```
scp tools/mqtt_bench_broker.py homelab:/tmp/potluck-broker/
ssh homelab 'cd /tmp/potluck-broker && setsid nohup python3 mqtt_bench_broker.py --port 1883 --log broker.jsonl > broker.out 2>&1 < /dev/null &'
python tools/mqtt_tool.py --host 192.168.0.77 sub '#' --out captures/<name>.jsonl     # what Home Assistant would receive
python tools/mqtt_tool.py --host 192.168.0.77 pub potluck/<cell>/<hash>/set ON        # what an automation sends
```

`<cell>` is the CA fingerprint's 8 hex digits (in every topic); `<hash>` the resource's path hash
(`python -c "from potluck.paths import path_hash; print('%08x' % path_hash('potluck://lab/node-7368/led/on'))"`
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
relative to the cluster file) and a broker on its TLS port; `apply` puts the CA's DER in the signed manifest and the
image carries it (version 4). `keys/desk-m13-tls.json` is the bench's: the house broker's listener 8883 and
`keys/bench-broker-ca.crt` (the homelab's `/home/boas/mosquitto/config/certs/ca.crt`; the listener's lines are at the end
of `mosquitto.conf`, the file before them is `mosquitto.conf.pre-tls`, and the port is published in the compose file).
The present status line shows `tls`, PSRAM free and low-water, and `sender_stack_free`.

**Aggregation.** A cell that presents is built with `CONFIG_ESP_WIFI_AMPDU_TX_ENABLED=n` on every board; `apply --plan`
says so. With aggregation on, a board presenting over its own station wedged its radio every 10-60 minutes (ESP-NOW
NO_MEM and the station's TCP stopped until a chip reset); with it off, 13 h 19 min clean (M0-LOG session 42).

**Leases.** Nothing to run: `apply` takes the cell's deploy lease after planning and says
`lease: the cell's deploy lease is ours (board 0x6300 holds it)`; a second `apply` meanwhile, from any PC, is
`REFUSED, nothing changed: the cell's deploy lease is held by "apply on <PC>" ...`. A resident hub takes the hub lease;
a second hub on the same PC is refused from `%LOCALAPPDATA%\potluck\hub.json`, one on another PC by the lease. A cell
on firmware before M14 answers no LEASE: noted, not refused.

**The placement host behind the hub.** With a hub holding COM6, `python -m potluck.hostnode --port COM6 --package
../../keys/<m>.pkg.json --identity ../../keys/host-00fe.id` goes through the hub by itself (`--hub [host:port]` to name
it); potctl, the agent and apply run beside it. It refuses an old `pot_hostnode` (`pot_hostnode --caps`): rebuild with
`tools\run_host_tests.ps1`.

**Telemetry.** The hub pulls every board's health, links, actors and events every `--telemetry-ms` (default 10000;
0 never): `python -m potluck.hub --cluster ../../keys/desk-m13.json --telemetry-ms 10000`. The model's `boards[].heap`,
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
  update and crash lines -- required for B and C, whose logs can name networks.
- **The floor plan:** the Plan tab. Make a plan (100 x 100 m by default), add rooms, drag boards; each drag is saved at
  once in the cluster file (`<file>.bak` keeps the version before), and the tab names the boards whose place is not on
  the board yet -- "Apply to the cell" sends it (a package change, about 20 s, no restart).
- **The library:** the Library tab -- apps on and off, "Test the broker", per resource Hidden / Visible / Controllable,
  then "Apply to the cell".
- **pot_sim for an app's read load:** `build/tests/pot_sim.exe --nodes 20 --scenario d20` (passive-sensor's design: a
  fusion per room) or `--scenario perlink` (the design before it), against `--scenario none` on the same seed.
- **The packaged app and the tray icon:** `tools\build_package.ps1` builds `dist\Potluck\` (`potluck.exe`: hub, apply,
  setup, keystore, ctl...; `potluck-tray.exe`: the tray icon) with PyInstaller from `%LOCALAPPDATA%\potluck\build-env`
  (made once; the script's top says how). The owner installs it with `packaging\install.ps1` -- run it yourself, not
  from a Claude session, whose AppData is redirected -- and removes it with `packaging\uninstall.ps1`. The tray starts
  a hub at login when none runs (the cell in `%LOCALAPPDATA%\potluck\tray.json`'s "cluster", else the one the wizard
  made, else setup mode). Unpackaged: `python -m potluck.tray`.
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
  value as u32 and f32, unit, age) and asks the owner again; `POT! refer <via hex> <path hash>` asks for a path that
  lives in another cell as a referral through that cell's foreman, then `POT! read` shows it. Path hashes:
  `python -c "from potluck.paths import path_hash; print(path_hash('potluck://lab/cell-bench/warmest'))"`.
- **pot_sim:** `build/tests/pot_sim.exe --scenario tree2 --cells 10 --minutes 10` -- ten cells of ten in their own
  radio spaces under one upper cell, real reconcilers and foremen; `--kill-at <s>` puts cell 0's foreman to sleep for
  two minutes. It reports deaths, handovers, cell-seconds with no foreman or two, each radio space's airtime (and how
  much of it the reconciler's CASTs), and the sibling summaries read. A day takes about five hours.
- **Stub cells on the bench** (the owner buys no more boards): the cluster file names stub boards (`"stub": true`,
  ids f000..fdff, e.g. `keys/desk-m19-tree.json`), which `apply` composes into the tree and never contacts. Build the
  host program optimised -- `tools\run_host_tests.ps1 -Release -BuildDir build/release -Target pot_stubs` (a Debug
  build fell behind its own heartbeats with nine stubs checking signatures) -- make sure the cabled board relays
  (`POT! relay 1`, kept across reboots), then from `host/potluck/`:
  `python -m potluck.stubs --port COM6 --package <the tree's signed package> [--watch f301:cell-bench/warmest]
  [--refer f101:8160:node-7368/hw/die_temp] [--sleep f102@120+90] [--out <file.jsonl>]`. The package is the one
  `apply` kept (`%LOCALAPPDATA%\potluck\cache\packages\<system>-<counter>.pkg.json`); each stub's identity is made
  under the cell's CA the first time (`keys/stubs/`). Its stderr lines: `stub` (groups, peers, roles and the summary
  each reads), `watch`, `event`.
- **The wizard's tree:** `POST /api/setup/tree {"chargers": ["0100", ...]}` on the hub proposes cells from the boards'
  link reports (potluck.survey); `"write": true` puts them into the cluster file, then Apply.
- **Beyond the cable:** in a tree the cable reaches the cabled board's cells only. `apply` names the boards of other cells
  ("is in a cell the cable does not reach") and verifies what the cabled board can resolve; those boards take the
  package and the firmware by catch-up. `apply --partial` lets boards that do not answer catch up (folding a tree back
  into one cell: `keys/desk-m19-spaces.json` -> `keys/desk-m14-hub.json`).
- **Firmware catch-up:** a confirmed board announces its firmware counter every minute (first 45 s after boot); a peer
  with an older one asks and takes the holder's own image (1.2 MB in about 33 s, one peer at a time, the asker waits
  three minutes before asking again). The board on the PC's cable leaves its firmware to the PC (it carries the PC's
  rollout to the others); a receive cut off half-way is dropped after 15 s. Its log lines are `fw: ... asked for it`,
  `fw: catch-up to 0x....`, then the receiver's usual `begin`, `commit`, `trial passed`. A board that took its firmware
  before 2026-10-16 (no trailer kept) cannot serve until it takes one over the cell.
- **Radio spaces on the bench:** `POT! deaf <last two MAC bytes>` on two boards, both ways, keeps them out of each other's
  range (`POT! deaf 0` clears; the list is not kept across a reset).
