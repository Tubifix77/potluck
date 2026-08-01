# M0 log

Append-only. Newest session at the bottom. The morning session starts by reading this.

---

## Session 1 — 2026-08-01, overnight (no hardware attached)

**Status: code-complete.** Firmware compiles for ESP32 under ESP-IDF v6.0.2, host tests green
(including under AddressSanitizer), size report in `firmware/size/`, core static DRAM
**29,136 B of §6's 64 KB cap**. Next step is flash-and-measure — see [M0-RUNBOOK.md](M0-RUNBOOK.md).

Owner went to bed mid-session and delegated the remaining decisions, so everything below was decided
autonomously inside the §0.1 invariant. Nothing here reopens an ADR. Two factual corrections were
made to ARCHITECTURE.md and both are recorded in [§Corrections](#corrections-to-architecturemd).

### What exists now

| Deliverable | Where | State |
|---|---|---|
| Potluck Frame codec, §5.1 byte-for-byte | `firmware/components/pot_frame/` | done, golden-byte tested |
| M0 payload schemas | `firmware/components/pot_frame/include/pot/payloads.hpp` | done, defined here (§5 does not specify bodies) |
| Heartbeat state machine, §8.2 | `firmware/components/pot_link/` | done, injected clock, host-tested |
| Per-peer link stats + RTT histogram | `firmware/components/pot_link/` | done, inside §6's budget |
| Serial stats format | `firmware/components/pot_link/src/stats_json.cpp` | done, golden-string tested |
| ESP-NOW transport, §5.3 profiles | `firmware/components/pot_espnow/` | done, **compiles only, never run on hardware** |
| §6 [MEASURE] DRAM probe | `firmware/components/pot_espnow/include/pot/dram_probe.hpp` | coded; numbers land when hardware does |
| M0 node application | `firmware/main/m0_main.cpp` | done |
| Host tooling | `host/potluck/` | done, exercised end-to-end against synthetic input |
| Host tests | `tests/` | 91 cases, 35,368 checks, green; green under ASan |
| Python tests | `host/potluck/tests/` | 47 cases, green, incl. differential fuzzing |
| Size report + §6 budget gate | `firmware/size/`, `tools/check_size_budget.py` | done, passing |
| Runbook | `M0-RUNBOOK.md` | done |

Roughly 5,900 lines of C++ and 2,650 of Python. `tools/run_all_tests.ps1 -Asan -Firmware` runs
everything and prints `ALL PASS`.

### Environment as found, and as left

The bench had **no C++ compiler on PATH, no CMake, no ESP-IDF**. Found and used:

- MSVC 14.51 (`cl.exe` 19.51) from **VS 18 Build Tools**, with CMake 4.0.3 and Ninja 1.12.1 bundled.
  VS 2022 Community is also present (MSVC 14.44) but ships no CMake.
- Installed **ESP-IDF v6.0.2** to `D:\esp\esp-idf` (shallow clone, 654 MB) plus the esp32 toolchain
  to `~/.espressif`. `install.ps1` succeeded against the system Python 3.14.4 — IDF's floor is 3.10
  and 3.14 was a real risk, so this is worth knowing.
- After install, ESP-IDF's own CMake and Ninja are used for the host tests too, so a machine set up
  to build the firmware needs nothing more to run the tests.

### Decisions

1. **ESP-IDF v6.0.2, not v5.5.5.** `docs.espressif.com/.../en/stable/` renders as "v6.0.2", and every
   citation in the ledger and in ARCHITECTURE.md points at `/stable/` — so v6.0.2 *is* the version
   the architecture is written against. v5.5.5 is a newer *patch* (2026-07-17) of an older line.
   Checked that the ESP-NOW constants and both callback signatures are byte-identical between the
   two tags, so nothing here is version-locked. Two v6.0 differences did bite: GPIO now lives in
   `esp_driver_gpio` rather than `driver`, and a `bool` Kconfig is *undefined* rather than 0 when
   unset.

2. **RTT, decomposed — never a one-way figure.** The single most important decision in the session.
   Clocks are unsynchronised, so nothing converts between them. Each heartbeat sets `ACKREQ` with a
   fresh `msg_id`; the peer answers *immediately* with `IS_REPLY` and that `msg_id`, plus
   `turnaround_us` measured entirely on its own clock. Reported: `rtt_us` (submit → reply, our
   clock), `txq_us` (submit → send-callback, our clock), `turnaround_us` (the peer's, as a duration).
   Durations are comparable across unsynced clocks; instants are not. `rtt_us − turnaround_us` is
   honest and named for what it is. **Dividing by two is not, and appears nowhere.** Two tests assert
   the absence of a one-way field so that adding one means arguing with a test first.
   Replying immediately rather than piggybacking on the next beacon is deliberate: piggybacking
   would make turnaround up to 100 ms and quantisation-limited by the beacon period.

3. **Percentiles as intervals.** p50 and p99 are emitted as `[lo, hi]` bucket bounds, because a
   16-bucket histogram knows an interval and not a number. Exact min and max are separate, since
   those are real samples. Interpolating inside a bucket would be exactly the invented precision
   §13-M0 rules out.

4. **Unmeasured is `null`, never 0.** A fresh link's PDR is `null` in JSON and `unmeasured` in the
   summary. A default of 0 claims total loss; a default of 100% claims perfection. §13-M0 asks for a
   *measured* figure.

5. **Local queue errors are never charged to the radio.** `esp_now_send()` refusing a frame
   (`ESP_ERR_ESPNOW_NO_MEM`) means it was never transmitted, so `tx_enqueue_err` is excluded from the
   PDR denominator. Likewise the RX-queue overrun counter is Potluck's own and is reported separately.
   Conflating either would make the PDR figure describe the wrong thing.

6. **Two independent codec implementations, on purpose.** `host/potluck/potluck/frame.py` is
   a second decoder written from §5, not a port of the C++. Both are checked against the *same*
   golden byte arrays. One implementation can only be tested for self-consistency — a header field
   swapped with its neighbour round-trips perfectly. §14 asks for the wire format to be "specified
   and versioned independently of the implementation"; two agreeing implementations is the cheapest
   way to mean it. Same reasoning for the histogram bucket edges, which the node does not transmit:
   `test_records.py` recomputes the node's own p50/p99 from its bucket counts and fails if the
   host's edges have drifted.

7. **The serial format is a tested contract.** `stats_json.cpp` lives in the *portable* component so
   the C++ tests assert its exact bytes, and `pot_tests --emit-fixtures` writes those bytes to
   `host/potluck/tests/fixtures/stats_sample.jsonl`, which the Python tests parse with a real
   JSON parser. A format change fails a test on both sides rather than showing up as a missing
   column in a 24-hour soak report.

8. **Two tasks, one owner of the peer table.** `link_task` (priority 6) is the only writer.
   `stats_task` (priority 3) takes a mutex just long enough to *copy* one peer, then formats and
   prints from the copy. Printing ~700 bytes of JSON on a blocking console UART takes milliseconds;
   doing it on `link_task` would delay heartbeats and manufacture the very misses §8.2 counts.

9. **ESP-NOW callbacks run in task context, and must not block.** Confirmed from the reference
   example, which calls `malloc()` and `ESP_LOGE()` inside them. So the queue calls are `xQueueSend`,
   not `xQueueSendFromISR` — but with a **zero timeout**, unlike the reference example's
   `portMAX_DELAY`. Blocking there would stall the Wi-Fi task and delay every other frame in flight,
   corrupting the timing M0 exists to measure. A full queue drops and counts.

10. **Zero-configuration discovery; one binary for both boards.** Node ids derive from the low two
    bytes of the station MAC, and peers are found by broadcast `HELLO`. No MAC addresses compiled in,
    no per-board build. A soak where the two boards run different images can differ in ways nobody
    intended.

11. **`AUTH` stays clear at M0, but the eight bytes are reserved in the MTU arithmetic.** §5.3's 1446
    and 226 are link MTU − 16 − 8, so enabling frame auth at M5 cannot shrink a payload that already
    shipped. The codec supports the flag and zeroes the tag (tested), but M0 traffic does not set it:
    emitting a zeroed tag would look like an authenticated frame to a future receiver that had
    started checking. This is the reading of §14's "the *bytes* are reserved from day one".

12. **The build goes through a mirror, because ESP-IDF v6.0.2 cannot build from a non-ASCII path on
    this machine.** `tools/build_firmware.ps1` mirrors `firmware/` to `D:\esp\potluck-fw` and builds
    there, copying the size reports back. Never edit the mirror.

    Tried and **rejected**: a directory junction with an ASCII name, and a `subst` virtual drive.
    CMake resolves both back to the real path, so neither helps.

    **Corrected 2026-08-01, later the same session — my first explanation was wrong and so was the
    remedy I proposed.** I originally wrote that the problem was CP1252 having no U+03BC, and
    suggested renaming the directory to use U+00B5 MICRO SIGN instead. Both claims were reasoned
    from a codepage table rather than measured. Measured:

    - The system ANSI codepage is 1252. `WideCharToMultiByte`-style best-fit maps **both** U+03BC
      and U+00B5 to the same byte **0xB5**, so "CP1252 cannot represent it" was simply false.
    - The actual mechanism is a **round-trip mismatch**: the path on disk is U+03BC, argv reaches
      GCC as 0xB5, and the CRT converts that back to U+00B5 — a *different* character — so the file
      genuinely is not found. That predicted a U+00B5 rename would work.
    - So I tested it, by copying `firmware/` to `D:\esp\dµOS-probe` (U+00B5) and running
      `idf.py set-target esp32`. **It fails too**, just differently: `esp-idf-kconfig` dies with
      `FileNotFoundError: 'D:/esp/dÂµOS-probe/build/kconfigs.in'` — `Â µ` being the UTF-8 bytes of
      µ read back as CP1252. A second encoding bug, in the Python tooling rather than in GCC.

    Conclusion: **any non-ASCII character in the project path breaks this toolchain**, in at least
    two independent places. Renaming the directory is not a fix and is not worth doing. The
    remaining candidate is Windows' *"Use Unicode UTF-8 for worldwide language support"*, which is
    a system-wide setting needing a reboot; it is untested here and is the owner's call, not a
    build script's. Given that the mirror is one line of robocopy and already automated, there is
    no reason to touch a system setting for it.

    The host tests are unaffected either way — MSVC handles the real path fine, which is why
    `tools/run_host_tests.ps1` builds in place.

13. **`FREERTOS_HZ=1000` even though M0 has no L1 actors.** §4 mandates it for the reference
    configuration. Measuring M0 under a tick rate the rest of the system will not use would make the
    M0 numbers useless as a baseline.

14. **`WIFI_PS_NONE`.** Modem sleep would add a wake latency that has nothing to do with the link, so
    the figure would be about our power policy. §7.8 makes power the honest constraint for battery
    nodes; that is a later, deliberate decision, not a default to inherit silently.

15. **Wi-Fi buffers trimmed in `sdkconfig.defaults`.** ESP-NOW never associates, so buffers sized for
    a TCP/IP workload are dead DRAM. This is the largest saving available and it serves §6's
    [MEASURE] item directly — if the Wi-Fi stack exceeds ~40 KB, §6 says the RX ring shrinks first,
    and the firmware evaluates that trigger itself and warns on the boot line.

16. **A BOOT-button `BYE`.** `CONFIG_POT_BYE_BUTTON_GPIO` (default GPIO0) makes a node announce an
    intentional departure and stop heartbeating. Without it, "left" versus "dead" is untestable
    without unplugging a board, and §5.2 distinguishes them for a reason: counting a planned shutdown
    as a failure would corrupt the failure statistics.

17. **The frame tee is off by default.** `CONFIG_POT_FRAME_TEE` gives §7.6's capture content now
    rather than at M2, but at ~40 lines/second the console UART's cost lands inside the timing being
    measured. On to debug the wire, off to measure the link.

### Bugs found by the tests

- **`pdr_ppm` reported 0 for every link.** `s.kv_opt("pdr_ppm", p.pdr_tx_ppm(ppm), ppm)` — C++ leaves
  function-argument evaluation order unspecified, and MSVC read `ppm` before `pdr_tx_ppm()` wrote it.
  Caught by the golden-string test, which is precisely the class of bug that would otherwise have
  produced a confident, wrong PDR figure for the whole 24-hour soak and been believed. Fixed by
  computing before passing.
- One test had wrong arithmetic of its own (asserted that fragment offset 90 + 8 bytes > 100). Fixed
  the test, and added the exact-fit case it should have had.

### Two bugs found by re-reading `link_task`, which has never run

Found by reading rather than by testing, because neither is reachable without hardware. Both are
recorded here so that if either symptom appears on the bench, it is already ruled out.

- **Heartbeat starvation under a busy RX queue.** The loop originally handled one frame and then
  `continue`d to the top "to drain the queue before doing timer work". Once a deadline has passed the
  computed wait is zero, so a sender fast enough to keep the queue non-empty would have starved the
  heartbeat indefinitely — and a starved heartbeat means the *peer* declares *us* dead while we are
  busy listening to it, which would have looked exactly like a link problem in the statistics. Now
  the loop drains a bounded number of frames (the queue depth) and then always runs the timer work.
  Not reachable with two boards at 10 Hz; very reachable at M1 with real traffic.

- **The discovering `HELLO` was never counted.** `handle_rx()` only accounts a frame against a peer
  it already knows, so the HELLO that *creates* a peer was missing from `rx_frames` — permanently
  off by one per peer. Small, but a counter that is knowably wrong by a fixed amount is worse than a
  correct one, because every figure derived from it inherits the error. Now accounted where the peer
  is created.

### Findings worth carrying forward

1. **ESP-NOW v1/v2 interop is length-qualified, and the failure mode is truncation.** The docs' summary
   sentence says v1.0 devices can only receive from v1.0, but the *next* sentence says a v1.0 device
   *does* receive v2.0 packets at or below 250 B, and above that "will either truncate the data to
   the first 250 bytes or discard the packet entirely". Truncation is the dangerous branch — a
   silently shortened frame is worse than a dropped one. This makes §5.3's version pinning load-
   bearing rather than a courtesy, and it is why the codec's `total_len` check matters: a v2 frame
   truncated to 250 B fails `length_mismatch` rather than being parsed. There is a test for exactly
   that scenario at exactly that boundary, in both implementations. **ARCHITECTURE.md §5.3 corrected.**

2. **`ESP_NOW_MAX_ENCRYPT_PEER_NUM` is 6 in the header but the real ceiling is 17.** The header
   constant is stale in both v5.5.5 and v6.0.2; the limit is `CONFIG_ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM`
   (`range 0 17`, default 7). §3's table was already right. Not on M0's path — M0 runs unencrypted,
   link crypto is M5 — but it is a trap for anyone sizing a table from the header.

3. **`esp_now_send()`'s documented length cap contradicts the same page.** "Attention 3" still says
   "less than `ESP_NOW_MAX_DATA_LEN`" (250) while the Frame Format section says 1470 for v2.0. The
   official example ships `ESPNOW_SEND_LEN` with `range 10 1470`, so the attention note is stale. M0
   never relies on it — every M0 frame is ≤ 64 B — but the v2 profile's 1446 B payload does.
   **[MEASURE]** on the bench before anything depends on a >250 B v2 send.

4. **ESP-NOW's 20-peer ceiling includes the broadcast entry, so Potluck admits 19 unicast peers.**
   Broadcasting requires the broadcast MAC to be added as a peer, and that consumes one of the twenty.
   §3 and §6 both say "20" without noting this. Enforced in `m0_main.cpp` with a `table_full` count
   rather than left to be discovered by a twentieth node failing to join. Not worth an ARCHITECTURE
   edit on its own — it becomes relevant when §4's "one radio cell of ~20 nodes" is tested for real.

5. **The RX ring is 96 bytes over §6's line, and that is deliberate.** §6 budgets "RX ring, 8 × 1470 B
   = 11.5 KB". Each slot also carries source MAC, length, RSSI and an arrival timestamp — 14 bytes of
   metadata — so eight slots are 11,872 B against a budgeted 11,776 B. Sizing the slot to the profile
   MTU rather than to today's 64-byte traffic is what makes the budget mean anything, and the
   timestamp is load-bearing for the RTT measurement. Flagged rather than fudged.

6. **The 1 KB JSON line buffer is M0 instrumentation, not §6 core.** §6 has no line for it. It is
   static rather than on `stats_task`'s stack because a stack overflow nineteen hours into a soak is
   the worst way to lose a measurement. M2's bridge tees binary frames and this buffer goes away with
   it. Counted in the 29,136 B figure, so it is inside the cap either way.

### Corrections to ARCHITECTURE.md

Both are factual corrections with sources, not decisions. Neither touches an ADR.

1. **§5.3** — the transport-profile table and the version-negotiation paragraph said a v1.0 device
   "cannot receive v2.0 packets". Corrected to the length-qualified rule, with the truncation hazard
   named and the reason §5.4's `total_len` check exists made explicit. See finding 1.

2. **§13-M0** — the description line said "one-way delay histogram". Corrected to "round-trip", with
   the reasoning inline: a one-way figure needs synchronised clocks or path symmetry, and M0 has
   neither. The *Accept* line already said only "delay histogram", so the acceptance criterion did
   not change. See decision 2.

The zero-assumption ledger gained 19 rows and a "Contradictions resolved" section covering findings
1–3. One prior row (`ESP-NOW v1/v2 interop`) is marked **superseded**.

### Deferred, with the reason

Nothing below is a gap in M0. Each is either M1+ by the roadmap or blocked on hardware.

| Deferred | Why | Owner |
|---|---|---|
| Namespace, `READ`/`WRITE`, staleness | M1 | §13-M1 |
| `potluck-bridge`, binary frame tee, `potctl replay` | M2. The capture *format* is written now so a capture taken today is readable then | §13-M2 |
| Fragmentation and reassembly | codec supports `FRAG` and is tested; no M0 opcode needs it, and §5.4 forbids it for the three that matter | §5.4 |
| CAN / TWAI, COBS-framed UART | M4, and ADR-001 says ESP-NOW + UART before CAN | ADR-001 |
| `SAFE_STATE`'s non-fragmentation siblings — broadcast-only, priority 31, `ACKREQ` always off | the opcode is not implemented at M0; only §5.4's FRAG rule is enforced, which needs the constant | M4 |
| `auth_tag` verification, session keys, enrolment | M5. Bytes reserved, never checked | §13-M5 |
| Wi-Fi-stack DRAM **numbers** | probe coded, needs a board | §6 [MEASURE] |
| A >250 B ESP-NOW v2 send | finding 3 | [MEASURE] |
| Coverage-guided fuzzing (libFuzzer/AFL) against `pot::parse` | the in-process fuzzer covers the same entry point deterministically and runs on every build; a guided run is strictly additional | post-M0 |
| RSSI-vs-PDR correlation across §3's 56–70 m cliff | RSSI is captured per peer; the analysis needs a field trial, not a bench | post-M0 |

---

## Session 2 — 2026-08-01, target hardware changed to ESP32-S3

Owner is waiting on **7 × ESP32-S3-DevKitC-1 N16R8** (16 MB flash, 8 MB octal PSRAM), not two
classic ESP32s. Both changes — the chip and the count — invalidate assumptions in session 1's code.
No ADR is reopened: ADR-001 scopes the project to the ESP32 *family*, and the S3 is in it.

### The S3 is not a bigger classic ESP32

Verified against the locally installed ESP-IDF v6.0.2 tree, which is the same source the build uses.

| | classic ESP32 | ESP32-S3 |
|---|---|---|
| DRAM window | `0x3FFAE000–0x40000000` = 328 KB | `0x3FC88000–0x3FD00000` = 480 KB |
| IRAM window | `0x40080000–0x400AA000` = 168 KB | `0x40370000–0x403E0000` = 448 KB |
| Relationship | separate physical banks | **one unified internal SRAM** |
| Static DRAM ceiling | 160 KB, fixed | none fixed — IRAM and DRAM trade against each other |
| Bluetooth | BLE **and** Classic | **BLE only** (`SOC_BT_CLASSIC_SUPPORTED` absent) |
| esp-idf-size memory row | `DRAM` | **`DIRAM`** |

Consequences, in order of how much they cost to miss:

1. **§6's premise does not hold.** §6 is written for "ESP32 classic … Bluetooth disabled (reclaiming
   the 64 KB the BT stack would cost)" and reasons "against the 160 KB static DRAM ceiling". On the
   S3 there is no such ceiling, and the 64 KB BT figure is a classic-part number that does not
   transfer to a BLE-only chip. The **64 KB core cap still applies** — it is a Potluck design limit, not
   a silicon one — but the surrounding headroom arithmetic in §6 needs restating for the S3 before
   anyone quotes it. Not corrected in ARCHITECTURE.md yet: it wants a rewrite of §6's preamble, not
   a one-line fix, and it should be done with the measured S3 numbers in hand rather than twice.

2. **The size checker silently failed on S3.** `esp-idf-size` reports `DIRAM` rather than `DRAM` for
   a unified-SRAM target, so `check_size_budget.py` parsed zero archives and reported
   `COULD NOT DETERMINE`. It now accepts either name and prints which it used, and says explicitly
   that §6's 160 KB ceiling is a classic-part figure when it sees `DIRAM`. Worth noting that the
   failure mode was correct-by-design: it refused rather than passing on an empty sum.

3. **The merchant listing is wrong about the radio.** It claims "dual-band WiFi". The S3 has **no
   5 GHz capability** — no ESP32 variant does. Practical consequence: the M0 soak shares 2.4 GHz
   with every other access point in the building, so §3's PDR figures are the relevant ones and
   channel choice matters. Nothing to fix in code; recorded so nobody plans around 5 GHz.

4. **The USB PHY degrades Wi-Fi, and the default accepts that.** The S3 sets
   `SOC_WIFI_PHY_NEEDS_USB_WORKAROUND`, and `CONFIG_ESP_PHY_ENABLE_USB` **defaults to `y`** on this
   target: "the USB PHY can interfere with WiFi thus lowering WiFi performance… This option can be
   disabled to increase WiFi performance."
   This is the one that would have quietly ruined M0. The whole milestone is a PDR and delay
   measurement, and the stock configuration takes a known RF penalty to keep native USB alive. A bad
   enough result would have fired §13-M0's kill criterion — *"the transport decision reopens"* —
   over a configuration default rather than over ESP-NOW.
   `sdkconfig.defaults.esp32s3` now sets `CONFIG_ESP_PHY_ENABLE_USB=n`. The cost is that the
   DevKitC-1's native **USB** port is unusable while Wi-Fi runs; the console goes over the board's
   other USB-C port, the **UART** one, which is a separate bridge chip and unaffected.

5. **PSRAM stays off for the baseline.** The R8's 8 MB is disabled by default and left disabled.
   §6's [MEASURE] item is about *internal* DRAM; with PSRAM enabled the Wi-Fi stack may place
   buffers externally, and free-internal-DRAM would rise for reasons unrelated to the Wi-Fi stack's
   real cost, making the number unattributable. Enable it and measure again *second*, as a separate
   run, once the baseline exists. `free_internal_dram()` already uses
   `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`, so it excludes PSRAM correctly either way.

6. **ADR-003 has a revisit trigger pending, not fired.** §3.1's Correction 3 rejects WASM as a
   per-node default because wasm3 measures ~156 KB "against a 160 KB static DRAM ceiling". On a
   board with 480 KB of unified SRAM and 8 MB of PSRAM that argument needs re-testing. **Not acted
   on** — M7 is gated on M0–M6 shipping (§13), and ADR-001 names scope creep as the standing risk.
   Logged so the trigger is on the record.

### Build changes

- `firmware/sdkconfig.defaults.esp32s3` — S3 overrides, every line annotated with its reason.
- `tools/build_firmware.ps1` now takes `-Target esp32s3|esp32`, defaulting to **esp32s3**. Separate
  mirror and separate `firmware/size/<target>/` per target, so the two stay comparable and switching
  does not force a rebuild.
- Both targets build clean. Core static data is **29,136 B on both** — unsurprising, since it is our
  own statically allocated arrays, and a useful confirmation that nothing target-specific crept in.

### Architecture bug: the heartbeat is O(N²) and does not fit the channel

Found when the fleet went from 2 nodes to 7. It is not a 7-node problem — it is a hole in §4 and
§8.2 that two boards could never have exposed.

**What the firmware does today.** Every node unicasts a heartbeat probe to every peer every 100 ms,
and every probe gets an immediate unicast reply. That is `2·N·(N−1)` frames per period.

**Airtime per frame**, from inputs already in the ledger. The ESP-NOW vendor-specific action frame is
`24` (MAC header) `+ 1` (category) `+ 3` (OUI) `+ 4` (random) `+ 8` (vendor element header) `+ 4`
(FCS) `= 44 B` of overhead, and a Potluck heartbeat is `16 + 48 = 64 B`, so **108 B on air**. At the
documented default ESP-NOW rate of **1 Mbit/s** that is **864 µs** of data airtime. This is a hard
**floor**: it excludes the PHY preamble, SIFS, the MAC-layer ACK, DIFS and contention backoff, which
together typically more than double it.

| Nodes | Frames per 100 ms | Frames/s | Airtime floor | Verdict |
|---|---|---|---|---|
| 2 | 4 | 40 | 3.5 % | fine — the only case session 1 could test |
| **7** | **84** | **840** | **73 %** | **saturated once real PHY overhead is added** |
| 20 | 760 | 7,600 | 657 % | impossible by 6.6× at the floor |

**Why this is an architecture bug and not a tuning knob.** §4 states that a v1 wireless cluster is
"one radio cell of ~20 nodes", and §1.2 repeats it. §8.2 mandates a 100 ms heartbeat. Those two
statements cannot both hold with unicast full-mesh heartbeating, and nothing in the document
notices. CLAUDE.md rule 1: a named system that is not possible under the architecture is an
architecture bug to fix, never a scope answer. §1.2's test fleet contains systems well past 7 nodes.

**The fix, which is boring.** Split liveness from measurement:

- **Liveness rides a broadcast beacon.** One frame per node per period reaches every peer, so cost
  is O(N) rather than O(N²): 20 nodes → 200 frames/s → 17 % floor. §8.2's period, miss count and
  600 ms death declaration are all **unchanged** — only the transport of the beacon changes.
- **RTT and PDR ride a round-robin unicast probe**, one peer per node per probe interval, and that
  interval need not be 100 ms because RTT is a diagnostic rather than a liveness signal. At one
  probe per second per node: 20 nodes → 40 frames/s → 3.5 %. Each *link* still collects 4,320 RTT
  samples over a 24-hour soak, which is ample for a 16-bucket histogram.
- Combined at 20 nodes: roughly 20 % of the channel at the floor, against 657 % today.

**One consequence to handle honestly.** A broadcast has no MAC-layer ACK, so the send callback stops
being an outbound-PDR signal for beacons. Outbound delivery then comes from each peer's *reported*
`rx_frames` / `rx_lost_seqgap`, which the HEARTBEAT payload already carries. That is arguably the
better measurement — it counts what actually arrived rather than what the link layer acknowledged —
but it is a different quantity and the JSON must name it as such rather than quietly reusing
`pdr_ppm`. The round-robin unicast probes keep a true ACK-based figure alongside it.

**Implemented, and verified in the simulator before the boards arrive.** The order taken was:
simulator first, reproduce the saturation, then fix, then show the simulator agrees.

`pot_sim --sweep` prints the channel load bracketed by two figures, because the true cost per frame
lies between them and neither alone would be honest — the 864 µs floor (data bits only) and §3's
2800 µs modelled initial-transmission delay (which includes channel access, so across contending
nodes it double-counts and is an upper bound):

| Nodes | unicast frames/s | floor | meas | broadcast frames/s | floor | meas |
|---|---|---|---|---|---|---|
| 2 | 40 | 3% | 11% | 24 | 2% | 7% |
| 5 | 400 | 35% | **112%** | 60 | 5% | 17% |
| **7** | **840** | **73%** | **235%** | **84** | **7%** | **24%** |
| 10 | 1,800 | **156%** | **504%** | 120 | 10% | 34% |
| 20 | 7,600 | **657%** | **2128%** | 240 | 21% | 67% |

N=20 is impossible on either bound. N=7 — the fleet on order — is impossible on the measured bound
and marginal on the floor.

Simulated, 2 minutes of cell time, perfect link, both modes running the *same* `pot::Node`:

| | unicast, 7 nodes | broadcast, 7 nodes | broadcast, 20 nodes |
|---|---|---|---|
| frames offered | 865/s | **109/s** | 443/s |
| airtime (floor) | 74.0% | **8.7%** | 32.5% |
| death declarations | 0 | 0 | 0 |
| RTT samples per node | 7,194 | 119 | — |

Eight times less traffic at seven nodes, the cell still fully connected, and every link still
measured. The RTT sample count drops with the probe interval, as designed: 119 samples in two
minutes extrapolates to about 86,000 over a 24-hour soak per link, which is far more than a
16-bucket histogram needs.

### Second architecture bug: `SAFE_STATE` and `HEARTBEAT` cannot exist on CAN as specified

Found by applying CLAUDE.md rule 1's standing rule — falsify the architecture against §1.2's named
systems — to **the car**: ten ESP32s and a Pi on a wired CAN spine. §1.2 says it "fits with room to
spare". It does not fit at all, and the reason is arithmetic the document already contains.

**The hard contradiction.** §5.1 fixes the header at 16 bytes. §5.3.1 gives CAN 7 Potluck bytes per
frame. So even a **zero-payload** Potluck Frame is ⌈16/7⌉ = 3 CAN frames, and a 64-byte `HEARTBEAT` is 10.
Against that:

- §5.2 claimed "`SAFE_STATE` is deliberately constrained to fit in a single frame on every
  transport, **including one CAN frame**." Impossible — 16 bytes of header cannot fit in 7.
- §5.4 requires "no fragmentation for `SAFE_STATE`, `HEARTBEAT`, or `HELLO`." On CAN this is
  unsatisfiable for *every* opcode, including a payloadless one.

Two sections asserting something a third makes impossible. Nothing detected it because CAN is M4 and
nobody had done the division.

**The capacity problem, on top.** CAN is a broadcast bus, so a heartbeat needs sending once per node
per period rather than once per peer — the O(N²) trap does not apply here. It still does not fit:

| Bus rate | Per heartbeat | 11 nodes × that, per §8.2's 20 ms wired period |
|---|---|---|
| 500 kbit/s | 2.22 ms | **24.4 ms = 122%** — oversubscribed before a single setpoint moves |
| 1 Mbit/s | 1.11 ms | 12.2 ms = 61% — the control traffic fights the liveness traffic |

Inputs are §3.1 Correction 2's own worked example (64 B → 10 frames → 1110 bits) and §8.2's wired
row. Note this is the *same underlying error* as the wireless one, wearing different clothes: a
48-byte `HEARTBEAT` conflates liveness with link statistics, and liveness is the thing with the hard
constraints.

**Resolution, written into §5.3.1.** A single-frame CAN profile carrying the routing header in the
29-bit extended ID — priority in the high bits (so CAN arbitration *is* Potluck priority, which §5.3.1
already wanted), opcode, 6-bit segment-local src/dst aliases, a SINGLE flag and a segment index.
When SINGLE is set the 8 data bytes are the whole payload, so `SAFE_STATE`, a beacon and a `HELLO`
each occupy exactly one CAN frame.

This does **not** fork the wire format. §5's promise is that the *Potluck Frame* is the same everywhere —
one parser, one fuzz target, one capture, byte-identical replay. The CAN profile was already an
*encoding* of that frame rather than a copy of its bytes; the segmentation byte is not part of any
Potluck Frame either. The extended ID is the same kind of encoding and reconstructs a byte-identical
16-byte header, so everything above the transport is untouched.

**Deliberately not implemented.** The consequence is that a liveness beacon must fit 8 bytes, which
means splitting `HEARTBEAT` into a compact beacon and a separate statistics report. That is the
right correction and it is **M4's**, not M0's: ESP-NOW carries the 64-byte frame in one piece, and
rewriting the wire format for a transport nobody has built is exactly the scope creep ADR-001 and
§14 name as the standing, fatal risk. Recorded in §5.3.1 so M4 starts from the answer.

### Third bug, found by the reboot scenario: an epoch going backwards was treated as a reboot

The simulator gained adversarial scenarios — `--scenario reboot | partition | chaos` — because a
bench will not produce fifty power-cycles or a clean partition on demand, and those are where the
subtle logic lives.

The reboot scenario immediately disagreed with arithmetic: **24 reboots injected, 324 observed** by
peers, when 24 × 6 peers = 144 is the ceiling. Roughly 2.25 detections per peer per reboot.

**Cause.** `peer_on_frame()` treated *any* epoch difference as a reboot. ESP-NOW retries for up to
~104 ms (§3), so a frame sent just before a peer rebooted can legitimately arrive *after* one sent
just after it. The sequence was: new epoch arrives → Rebooted, epoch := 2; straggler with the old
epoch arrives → "Rebooted" again, epoch dragged back to 1; next live frame → "Rebooted" a third
time. Each spurious detection also cleared `seq_rx_valid` and `seq_rx_last`, so **genuine inbound
loss detection was suppressed for as long as the flapping continued** — and inbound PDR is one of
the two numbers §13-M0 exists to produce.

**Fix.** Only an epoch *increase* is a reboot. One going backwards is a ghost of a superseded
incarnation: counted as `rx_stale_epoch`, and it changes nothing — not the epoch, not the sequence
expectations, not liveness. This also makes the membership layer agree with two things the document
already assumed: §7.7's consumers "fence by accepting the highest `(epoch, assignment)`", and §8.3's
safety receivers "reject epochs older than the last heard from that source". Monotonic epochs were
always the intent; the membership code just did not implement it.

After the fix: **24 injected, 138 observed** — short by exactly the final reboot, which has not
propagated before the run ends. Two regression tests in `test_heartbeat.cpp` pin it.

Room for `rx_stale_epoch` came from deleting three dead fields in `NodeCounters`
(`free_dram_at_boot` / `_after_wifi` / `_after_espnow`), which duplicated `DramProfile` and were
read by nobody. The struct stays exactly 64 B against §6.

**A fourth bug, in the scenario itself, found on the way.** The first reboot run reported 24
injected / 35 observed — an *under*-count. That was the instrumentation, not the code: rebooting a
node destroys the `NodeCounters` being summed at the end. The sim now harvests counters before
destroying a node. Worth recording because it is the failure mode a simulator is most prone to —
measuring itself rather than the system.

### All four scenarios converge

7 nodes, 2 simulated minutes each, on §3's 52 m fringe link. "Converged" means no peer is left
marked dead once the perturbation stops.

| Scenario | Deaths | Revivals | Reboots seen | Still dead at end |
|---|---|---|---|---|
| none | 0 | 0 | — | 0 ✓ |
| reboot (one node every 5 s) | 0 | 0 | 138 of 144 | 0 ✓ |
| partition (3 s split / 7 s heal) | 264 | 264 | — | 0 ✓ |
| chaos (both at once) | 248 | 190 | 173 | 0 ✓ |

Deaths under `partition` are correct, not a failure: 3 s of split far exceeds §8.2's 600 ms window,
and 12 cycles × 24 cross-group links ≈ 264. Deaths and revivals balancing exactly is the property
worth having. Under `chaos` they differ by 58 because a peer that reboots while dead returns as
*Rebooted* rather than *Revived* — which is the distinction §7.7 will need, working as intended.

### Fifth architecture bug: L4's reassembly timeout is undefined, and four lost fragments wedge a peer

Falsifying §1.2's **ESP32 supercomputer** — the one system whose traffic is large, latency-indifferent
messages rather than small control frames. Two rules collide:

- §4: **L4 is "best effort, no deadline".**
- §5.4: reassembly has "a timeout of 3× the class deadline and a hard cap of 4 concurrent
  reassemblies per peer".

`3 × (no deadline)` is undefined, for exactly the class that fragments most: the supercomputer
scatters work units as L4 `CALL`s, and anything over 1446 B fragments. A partial message with no
timeout is never reclaimed, so **four lost fragments from one peer wedge that peer's four reassembly
slots permanently** — after which nothing from that peer can ever be reassembled again. That is a
denial of service reached by arithmetic rather than by malice, and it needs no attacker: §3's
measured 83.2% PDR at 58 m gets there on its own.

Fixed in §5.4: **L4's reassembly timeout is a fixed 30 s**, not a multiple of anything.

**And a second hole in the same paragraph.** §5.4's ACK policy names L2 and L3 and stops. L4 —
again, the supercomputer's entire traffic — had no stated policy. "Best effort" reads like "no
ACK", but best-effort means *no deadline*, not *no delivery*: at 83.2% PDR that is 17% of throughput
vanishing with nobody informed, and a coordinator that cannot distinguish a slow worker from a lost
job. §5.4 now says L4 sets ACKREQ, with the reasoning inline — loss must be *visible* even where it
is tolerable, because §7.8's coordinator can only resubmit what it knows was lost.

Neither is implemented: M0 has no fragmentation and no L4 traffic. Both are M1/M2 work, recorded
where those milestones will look.

### The remaining named systems: the suit and the harvester add nothing new

Checked for completeness. Both are CAN-spine machines — §1.2 calls the harvester "the car vignette in
a harsher suit" — so both inherit the single-frame contradiction and the 122% bus load already
recorded above, and neither introduces a mechanism the car did not. The suit's genuinely hard part
(sub-millisecond loops around a human) is already answered honestly by §3.1 and §12: it is pinned to
local silicon and Potluck is never the only thing between a motor and a person. Nothing further to fix.

### The broadcast beacon has no retries, and that sets a PDR floor §8.2 never derived

The broadcast beacon fixes the airtime problem but gives up something real: a broadcast draws no
802.11 ACK, so it gets none of ESP-NOW's up-to-31 retransmissions. Six consecutive beacon losses is
simply `(1 − PDR)^6`, and §8.2's 6-miss limit was derived from the ~104 ms retry window, not from a
loss-probability argument. At a 100 ms beacon there are 36,000 opportunities per link per hour:

| Beacon PDR | False deaths per link |
|---|---|
| 99% | ~0 |
| 95% | 0.01/day |
| 90% | 0.9/day |
| **83.2%** | **0.8/hour**  ← §3's measured figure at 58 m |
| 80% | 2/hour |
| 70% | 26/hour |
| 50% | 562/hour |

Simulated at §3's measured 58 m link (83.2% PDR), 7 nodes, 5 minutes: **1 death declaration, 1
revival, converged** — which matches the table.

Two things follow. **For the soak: keep the boards well inside 56 m.** §3 puts PDR above 99% below
56 m, where false deaths are effectively zero; 58 m already costs about one an hour per link. That is
now in the runbook's geometry advice rather than left to be discovered in the results.

**For the design:** the miss limit and the beacon's delivery probability are coupled, and §8.2 only
ever reasoned about the former. The knee is around 83% — comfortable above it, bad below. Raising
the miss limit is the obvious lever if a deployment genuinely needs to live near the cliff, and it
trades detection latency for false-positive rate. Not changed: 6 misses is right for the geometry
M0 will actually be run at, and §8.3 is emphatic that false trips destroy trust.

### The range cliff behaves correctly

`--scenario cliff` reproduces §3's 56–70 m band, where PDR "fluctuates between 100% and zero"
instead of decaying: 4 s of perfect link, 4 s of nothing. Seven nodes, 5 minutes: **1,554 deaths,
1,554 revivals, converged, nobody stuck.** Deaths and revivals balancing exactly across 1,554 events
is the property worth having — the cell tracks a link that keeps vanishing and never loses count.
This is the condition §13-M0's kill criterion is written about, and the honest reading is that the
*membership layer* handles it correctly while the *link* remains unusable for anything with a
deadline.

### Fourth architecture bug: a sleeping node is indistinguishable from a dead one

Falsifying §1.2's **home**, which has a garden watering node. §7.8: "Battery and solar nodes are
excluded from background pools by default and opt in via manifest — the §1.2 home's garden node
stays sleepy unless told otherwise." A sleeping node's radio is off. §8.2 declares a peer dead after
600 ms of silence and has no notion of a peer that is silent on purpose. Nothing anywhere in the
document mentions sleep in connection with liveness.

Simulated (`--scenario sleepy`): five nodes, one duty-cycling awake 2 s / asleep 8 s — deliberately
*conservative*, since a real solar node wakes for seconds every few minutes. Five simulated minutes:

**240 death declarations, 232 revivals, 8 peers still marked dead at the end. The cell never
settles.** That is 48 deaths a minute produced by one node doing exactly what §7.8 tells it to do,
and it buries the death-declaration count — the signal that something is actually wrong — in noise.

**Resolution written into §8.2:** sleep is announced rather than inferred. `HELLO` carries the
sender's wake schedule; a peer with a declared schedule is measured against that rather than the
beacon period; inside its sleep window its state is `DOZING` — expected to be silent, not a failure,
and not counted as one. A dozing peer that misses its *own* announced wake still goes to `DEAD`.
This costs nothing elsewhere: §4 already delivers reads from a silent node as `STALE` with an exact
age, and a dozing peer's staleness is bounded by its advertised interval.

**Not implemented.** It needs a `HELLO` field for the wake schedule — the third wire-format change
now queued behind the same gate as the CAN beacon split and the `uptime_ms` overflow. M0 has no
sleeping nodes; all seven boards are mains-powered for the soak.

### The 49.7-day millisecond wrap: liveness survives it, `uptime_ms` does not

§1.2's environmental monitor "runs from flash indefinitely", so the counter wraps are a falsification
target in their own right. `last_rx_ms`, `hb_period_ms`, `Event.at_ms` and the payload's `uptime_ms`
are all `uint32` **milliseconds** — 49.71 days.

Tested by starting the simulator's virtual clock at 49.709 days (`--start-days`) and running a
7-node cell across the boundary: **26,082 frames, zero loss, zero death declarations, converged.**
The liveness arithmetic is wrap-safe because every comparison is an unsigned difference over an
interval far shorter than the wrap — `silent_ms = now_ms - p.last_rx_ms` and the `int32_t` deadline
comparisons in `Node::tick`. That was a comment; it is now a test.

**What does break is the reported value.** `uptime_ms` is informational — nothing computes with it —
but a monitor watching a node that has been up for 50 days sees its uptime jump backwards to zero.
For a node §1.2 expects to run indefinitely that is wrong, if cosmetic.

**Not fixed, deliberately, and for the same reason as the CAN case.** The fix is to report uptime in
seconds (`uint32` seconds is 136 years), which is a wire-format change. M4 already has to split
`HEARTBEAT` into a compact beacon and a statistics report to fit one CAN frame — that is the moment
to change this payload, once, rather than twice. Changing the bytes now for a display field that
misbehaves after seven weeks does not clear the bar, and M0's acceptance run is 24 hours.
Recorded against M4 alongside the beacon split.

### Long-run simulation: the 71.6-minute wrap is survivable

`probe_submit_us`, `probe_sendcb_us` and `owed_recv_us` are `uint32_t` microseconds, which wrap every
71.6 minutes. Unsigned subtraction stays correct across a wrap provided every measured interval is
shorter than that, which every one of them is — but that was a comment in `link_stats.hpp`, not a
tested fact.

Now tested: 7 nodes, 130 simulated minutes on §3's 52 m fringe link, straddling the wrap. **846,105
frames, 5,104 lost to the modelled link, zero death declarations and zero revivals.** The RTT
histogram and the liveness timers both survive the wrap.

### Still to do before the boards arrive

- **The heartbeat does not scale to 7 nodes.** See the next section — this is an architecture bug,
  not a tuning problem.
- Restate §6's preamble for the S3 memory model, once there are measured numbers.
- A host-side node simulator, so the 7-node fix can be verified before hardware exists.

### For the morning session

1. `tools\run_all_tests.ps1 -Firmware` — expect `ALL PASS` and 28.5 KB of 64 KB.
2. Follow `M0-RUNBOOK.md` from §2.
3. Record the geometry. §3's PDR "fluctuates between 100% and zero" between 56 m and 70 m, so a PDR
   figure without a distance is not a measurement.
4. Paste results into a new `## Bench results` section here, tagged **measured** with the date —
   §6's convention, and the difference between a fact and a design budget.
5. If the kill criterion fires, reopen ADR-001 rather than starting M1. §14 lists scope expansion as
   the highest-likelihood fatal risk.

### Not started: M1

The brief says leftover hours go to codec fuzz cases rather than M1, and they did.

**In-process fuzzer** (`tests/test_frame_fuzz.cpp`), deterministic and run on every build, clean
under AddressSanitizer. Seven generators against `pot::parse`:

| Generator | Inputs | What it is for |
|---|---|---|
| seed corpus | 13 valid frames | a bit-flipper essentially never builds a valid 16-byte header by chance, so without these only the rejection paths get exercised |
| random mutation | 13 × 400 = 5,200 | bit flips, byte replacement, truncation, extension, interesting-value substitution, byte swaps |
| pure random | 4,000 | lengths clustered around the header size, half with a valid magic byte so they get past the first check |
| length boundary sweep | 2 × 41 | every total length 0–40 with and without `AUTH`, verdict derived from §5 rather than from the code — 16, 17, 23, 24, 25 are where an off-by-one in the overhead arithmetic lives |
| length-field targeted | 2 × 400 | `frag_off` × `total_len` over 20 boundary values each, with the expected verdict computed from §5.4 independently. These two fields are where a wrong answer is a memory-safety bug rather than a parse failure |
| dictionary splice | 13 × 200 = 2,600 | whole tokens (a valid header, the v1/v2 payload caps, all-flags-set) spliced or overwritten at random offsets, so a second valid header appearing mid-payload actually gets explored |
| structured header sweep | 256 × 37 = 9,472 | every `ver_flags` byte × every seventh `lclass_pri` |

Three properties are checked per input: no access outside the buffer (guard bands either side, not
trusted to ASan alone), internal consistency of anything accepted, and byte-exact re-encode.

**Differential fuzzing** (`host/potluck/tests/test_differential.py`) — the more valuable half,
and what makes decision 6's second implementation earn its keep. `pot_tests --emit-fuzz-corpus`
writes all **9,226** generated inputs plus the C++ parser's verdict; the Python decoder replays them
and must produce an identical verdict, field for field, on every one. Currently: **agreement on all
9,226**, across both §5.3 transport caps, reaching all ten rejection reasons, with 1,882 inputs
accepted — so field-level agreement is genuinely exercised and not just agreement on saying no.

The distinction matters. Golden-byte tests prove the two implementations agree on frames someone
thought to write down, which catches a field at the wrong offset. Differential fuzzing catches a
*rule* one side enforces and the other does not — a length check one author forgot, a wrap boundary,
a flag combination nobody considered. And a disagreement would mean §5 is ambiguous, which is a
documentation bug and the most useful kind to find at design stage.

The corpus is ~4 MB and gitignored: it is fully deterministic (fixed seeds), so
`tools/run_all_tests.ps1` regenerates it and the test says how if it is absent. `stats_sample.jsonl`
in the same directory *is* checked in — it is the serial-format contract and belongs in the history.

Still worth doing later, and not a gap now: a coverage-guided run (libFuzzer or AFL) against the same
`pot::parse` entry point. It would explore paths a fixed-seed generator cannot, but it is strictly
additional — it cannot be run on every build, and it does not check the Python side at all.

---

## Session 3 — 2026-08-01, M1, M2 groundwork, QEMU

Written before a context compaction. **Read this section first if you are picking the project up.**

### Where things stand

`tools\run_all_tests.ps1 -Asan -Firmware` was green at 14 gates before the QEMU work started
(151 C++ cases / 75 Python). The firmware builds for esp32s3 and esp32. Core static data 53.1 KB of
§6's 64 KB cap — up from 28.6 KB because M1's namespace and M2's serial link both landed.

### M1 — done, minus the CLI

- **`Reading`** (§4 rule 2): value, unit, timestamp, age, class, quality — with **no accessor that
  returns the bare number**. `classify()` is the only place the staleness rule lives.
- **Namespace**: 128 entries × 40 B, FNV-1a/32 path hashes, linear probing, `worst_probe()` measured
  against §14's lookup-cost risk.
- **READ / WRITE / REPLY** on the wire (0x10, 0x11, 0x21). REPLY answers both rather than minting two
  opcodes — its §5.2 definition was never actor-specific.
- **`publish()` vs `write_local()`**: `Access` is the *wire* policy, not a lock the owning driver has
  to pick. Nearly every resource is read-only to the cluster and written constantly by its owner.
- **Six built-in resources per node**, so a fresh fleet has something real to read on day one.
- **M1's acceptance test passes** node-to-node in simulation, including the sentence that matters:
  unplug the owner and the read returns `UNAVAILABLE`, never a cached number presented as fresh.

Remaining: `potctl read <path>` (needs the bridge), SUBSCRIBE/PUBLISH/LIST, event-queue semantics,
the bind table for logical paths, manifests.

### M2 — framing done, bridge not started

- **COBS + CRC-16/CCITT-FALSE**, both sides, agreeing byte-for-byte over a 430-frame differential
  corpus. Both assert `0x29B1` over `"123456789"`; "CRC-16" alone names a dozen incompatible
  algorithms and a mismatch presents as a link that looks *dead* rather than wrong.
- **Firmware UART transport** (`pot_espnow/serial_port.cpp`): UART1, separate from the console so
  binary frames and logs cannot corrupt each other. The host is an ordinary peer at a reserved MAC
  (`kHostMac`), routed in `hal_send` — §8.1 says a mode transition is a non-event, which only holds
  if a host is not a special case above the transport.

**THE LOOSE END: `potluck-bridge` on the host is not written.** That is the next task. It needs to own
the serial port, speak the framing above, tee to §7.6's capture format (already implemented in
`host/potluck/potluck/capture.py`), and let `potctl` issue READ/WRITE. §7.1 forbids anything
above the bridge opening a serial port; for M2 the bridge can be a library `potctl` uses in-process,
with the socket boundary deferred until `potluck-agent` exists at M8.

### QEMU — runs, but boot-loops after the serial link starts. UNRESOLVED.

> **Superseded by Session 4. Two of the conclusions below are wrong.** There *was* a panic on every
> boot, and the `espnow_up()` guards *had* fixed it — QEMU was booting a stale flash image. Read
> Session 4 before acting on anything in this subsection.

The owner's suggestion, and a good one: it removes "the firmware has never executed" from the blocked
list. Installed with `idf_tools.py install qemu-xtensa`; `tools\run_qemu.ps1` drives it.

**What was established:**

- QEMU's `esp32s3` machine emulates CPU, memory, timers, flash and UARTs. `-device help` lists **no
  Wi-Fi or WLAN device**; the machine is handed an Ethernet device instead. So ESP-NOW cannot be
  emulated, and `esp_wifi_start()` enters PHY calibration and **never returns** — a normal build
  hangs at boot rather than failing.
- Hence `CONFIG_POT_RADIO_DISABLE` and `firmware/sdkconfig.qemu`. A node built that way reports
  `"no_radio":1` on **every** statistics line, so no run from it can be mistaken for §13-M0's
  measured soak.
- With that build the node **boots, runs `app_main`, initialises NVS, increments the boot epoch
  across runs (proving flash persistence works), declares its 6 resources, and brings up the frame
  link on UART1** — `frame link on UART1, tx=17 rx=18, 921600 baud`.

**Then it resets, in a loop.** Roughly 9 boots in 30 seconds, the boot epoch counting up as the only
evidence — no panic message on the console.

**Ruled out:** the ESP-NOW entry points. Every `espnow_*` function is now guarded by `espnow_up()`,
because on a build with no Wi-Fi driver those calls reset the chip instead of returning
`ESP_ERR_ESPNOW_NOT_INIT` as documented. That guard is correct and worth keeping regardless — but it
did **not** stop the loop.

**Still to check, in order of likelihood:**
1. `bye_button_init()` — `gpio_config` on GPIO0 under QEMU. Try `CONFIG_POT_BYE_BUTTON_GPIO=-1` in
   `sdkconfig.qemu` first; it is a one-line test.
2. Task creation, or something in `link_task`/`stats_task` on their static stacks. The 3 KB
   `pot_serial_rx` stack is the newest and least exercised.
3. `esp_read_mac(ESP_MAC_WIFI_STA)` in the radio-less path — the efuse MAC block may be unset in
   QEMU's generated `qemu_efuse.bin`.
4. Get a panic backtrace: run `idf.py qemu monitor` instead of raw QEMU, which decodes backtraces,
   or `idf.py qemu gdb`.

The step-trace `ESP_LOGD` calls left in `serial_port_start()` are debugging scaffolding; remove them
once this is closed.

### Two path/naming facts worth carrying forward

The repository path's **μ** (U+03BC) has now broken four separate tools: Xtensa GCC's argv handling,
`esp-idf-kconfig`, and QEMU's `-serial file:` — each in a different way, none with a useful error.
Only MSVC and Python handle it. `tools\build_firmware.ps1` and `tools\run_qemu.ps1` both mirror to
ASCII paths under `D:\esp\` to work around it, and the QEMU console log is written there and copied
back.

**The owner has proposed dropping the Greek mu outside the logo** — `dmicroOS`, `duOS`, or dropping
"OS" altogether on the grounds that this is closer to "Kubernetes on top of an OS" than to an
operating system, which §7.1 already concedes ("On a PC, Potluck is deliberately not the operating
system… Potluck rides on FreeRTOS… the name is ambition"). µTorrent reportedly made the same retreat.

> **Done in Session 5. The project is now Potluck.** The estimate below — "mostly a directory rename
> plus prose" — was **wrong in one important way**: it missed that the namespace URI scheme `dmu://`
> is hashed into every path hash on the wire. See Session 5.

**Not acted on** *(at the time of writing)*. It touches every filename, identifier prefix, document
heading and path string in the project, so it wants doing in one deliberate pass rather than
piecemeal — and it should be the owner's final call on which name. Recorded here so it is not lost.

---

## Session 4 — 2026-08-01, QEMU root-caused, potluck-bridge landed

Picking up the three threads Session 3 left open. Two are closed; one is characterised and bounded.

### First: Session 3's QEMU conclusion was wrong, and the reason matters more than the bug

Session 3 recorded "the `espnow_up()` guards did **not** stop the loop" and "no panic message on the
console". Both were false.

There **was** a panic, every time: `Guru Meditation Error: Core 0 panicked (LoadProhibited)`,
`EXCVADDR=0x4c`, the same backtrace on all nine boots. And the guards **had** fixed it.

What went wrong is a methodology failure worth more than the defect. `idf.py build` produces
`potluck_m0.elf`; `qemu_flash.bin` is a *separate* merge of bootloader, partition table and
application. `tools\run_qemu.ps1` generated that image only when it was missing — so after the guards
were added, the build produced a new ELF while **QEMU went on booting the old application**. The fix
appeared not to work because it was never running.

Worse, decoding that panic's addresses against the new ELF gave a confident, plausible, entirely
wrong answer: `__esp_system_init_fn_mbedtls_psa_crypto_init_fn`, a function this firmware never
calls. Nothing in `addr2line`'s output hints at a mismatch. ESP-IDF prints
`ELF file SHA256: <9 hex>` immediately before `Rebooting...` for exactly this reason.

Two changes so this cannot recur:

- **`run_qemu.ps1` always regenerates the flash image.** A few seconds per run against a whole
  session lost.
- **`tools\decode_backtrace.ps1`** decodes a panic and **refuses when the ELF's SHA-256 does not
  match the log's**. It resolves each frame twice — `addr2line` for file:line and the symbol table
  for the enclosing symbol — and prints both, because `addr2line` attributes a PC just past a
  function's end to its neighbour. When the two disagree, believe the symbol table.

The general rule: **an address is only as good as the binary it is decoded against, and that pairing
has to be checked by a machine, not remembered by a person.**

### Two real firmware findings, both from QEMU, both matter on hardware

1. **GPIO input is not emulated** (Espressif's own feature table lists GPIO matrix/IOMUX as
   unsupported), so `gpio_get_level()` returns 0 on every pin — which an active-low button reads as
   held down. Every emulated node announced a BYE ~6 ms after the frame link came up.
   `CONFIG_POT_BYE_BUTTON_GPIO=-1` in `sdkconfig.qemu`.
2. **The eFuse MAC block is blank under QEMU**, so `esp_read_mac()` returns all zeros, the derived
   node id lands on §5.1's reserved `0x0000`, gets nudged to `0x0001`, and **every** such node claims
   the same id — a fleet whose peers appear and vanish for no visible reason. This is not only an
   emulator problem: an unprogrammed efuse block does the same on real silicon. The firmware now
   **says so explicitly** rather than quietly inventing an identity, and `run_qemu.ps1 -NodeId` pins
   one so more than one emulated node can run.

### The QEMU hang: FOUND, and it was our bug, not the emulator's

**Read the resolution at the end of this subsection before the narrative — the narrative records two
wrong turns that are worth not repeating.**

**Root cause: `link_task` was a busy loop at priority 6 whenever the radio was down.** The loop's only
sleep lived inside `espnow_rx_pop()`, and that function returns `false` *immediately* when
`g_espnow_up` is false rather than blocking for its timeout. So on any radio-less build the task spun
at 100% CPU at priority 6, starving `stats_task` at priority 3 — permanently. Nothing crashed, nothing
logged, and the node went silent for ever after a flawless boot.

The log timestamps appeared frozen for a mundane reason: **`ESP_LOG` is not used again after
`app_main` on a radio-disabled build.** The statistics stream goes out through `fputs`, and it was the
starved task that produced it. There was never a stopped clock.

Fixed by making the sleep the loop's own responsibility rather than a side effect of whichever
transport happens to exist: an explicit `vTaskDelay` of at least one tick when nothing below could
have blocked. After the fix, a 30-second emulated run produces **144 statistics records** — boot
record, node counters, and all six resources reporting §4 rule 2's whole tuple with real ages and
rising update counts.

**This is a hardware bug, found under emulation.** §8.1 makes a radio-less node a *degraded* node
that keeps running, and `espnow_start()` failing on a real board takes exactly the same path. A board
whose radio does not come up would have starved the one task that reports anything — silent at
precisely the moment the diagnostics matter. That is the emulator paying for itself.

Two wrong turns on the way, both recorded because each cost real time:

**Wrong turn 1: reading a single sample as a deadlock.** One QEMU-monitor sample found CPU0 in
`xPortEnterCriticalTimeout` with `SCOMPARE1 = 0xB33FFFFF` (ESP-IDF's `SPINLOCK_FREE`) and interrupts
masked at level 3, and that was written up as a spinlock deadlock. It was not. A busy loop that takes
and releases a mutex every iteration spends a good fraction of its time inside `portENTER_CRITICAL`,
so a single sample landing there is exactly what a spin *looks* like. **One stack sample is not a
diagnosis** — it needs either a second sample showing no progress, or a bisection.

**Wrong turn 2: trusting `CONFIG_FREERTOS_UNICORE` as a fix.** It was applied
(`CONFIG_FREERTOS_NUMBER_OF_CORES=1` confirmed in the generated sdkconfig) and changed nothing, which
was correct evidence — but it was chased because the deadlock story predicted it, not because
anything pointed at core count.

**Retested afterwards, and removed.** With the spin fixed, a dual-core emulated build runs correctly:
`Multicore app`, **172 statistics records in 30 s, no panic, one POWERON**. So `sdkconfig.qemu` is
dual-core again, and it now carries a comment saying *not* to add the flag back and why. This is not
tidiness: a single-core emulated run exercises different concurrency from the firmware that ships,
so every task-priority and mutex interaction would be the wrong one — and those interactions are most
of what running the real firmware under emulation is good for. A workaround left in place after its
premise collapsed would have quietly degraded the emulator to testing a configuration nobody flashes.

`CONFIG_LOG_DEFAULT_LEVEL_DEBUG` came out at the same time. It was on to watch the frame link come up;
ESP-IDF's own components are chatty at that level, and on a slow emulated run the console becomes the
bottleneck — which changes the timing of the thing being observed.

**What actually resolved it was the cheapest possible experiment**: turn one feature off
(`CONFIG_POT_SERIAL_LINK=n`) and see whether the symptom survives. It did, which cleared UART1 and left
only the two tasks — at which point reading `link_task` against `espnow_rx_pop`'s guard took a minute.
`tools\run_qemu.ps1 -Extra "CONFIG_..."` exists now to make that kind of one-flag bisection free.

### The QEMU hang: how it was characterised before the cause was found

With the flash image fixed, the firmware **boots cleanly and completely**: NVS up, boot epoch
incremented across runs (flash persistence proven), six resources declared, UART1 frame link up, and
the identity line printed —

```
I (239) pot.serial: frame link on UART1, tx=17 rx=18, 921600 baud
I (239) pot.m0: Potluck M0 node 0x1001 epoch 5, heartbeat 100 ms x 6 misses = 600 ms
```

That last line is the final statement of `app_main`. **Then the FreeRTOS tick stops.** Every
subsequent log timestamp would be identical, so nothing more is printed; no statistics line ever
appears; the node does not answer UART1.

What was established about it, in order:

- **Not a crash.** QEMU's monitor reports `VM status: running` and `CCOUNT` keeps advancing. No
  panic, no reset — one `POWERON` and nothing else.
- ~~**The tick is what died.**~~ **WRONG, and it was the inference that cost the most.** The reasoning
  was: ESP-IDF's log timestamp is `xTaskGetTickCount()`, so a frozen timestamp means a frozen tick.
  The premise is right and the conclusion does not follow — a frozen timestamp equally means *nothing
  called `ESP_LOG` again*, which is exactly what happens on a radio-disabled build where the
  statistics go out through `fputs` from a task that has been starved. **A signal that stops can mean
  its source stopped or its producer stopped, and those need telling apart before anything is built
  on top.**
- ~~**CPU0 was found in `xPortEnterCriticalTimeout`**~~ — real observation, wrong reading. See "wrong
  turn 1" above: a loop that takes and releases a mutex every iteration is inside
  `portENTER_CRITICAL` a good fraction of the time, so a single sample landing there is what a *spin*
  looks like, not what a deadlock proves.
- **Not cross-core atomics.** `CONFIG_FREERTOS_UNICORE=y` was the obvious suspect and it changed
  nothing. Verified applied (`CONFIG_FREERTOS_NUMBER_OF_CORES=1` in the generated sdkconfig).
- **GDB's backtrace is not usable here.** `xtensa-esp32s3-elf-gdb` against QEMU's gdbstub produced
  frames that cannot be real — `app_main` called from `serial_port_send`, `xTickCount` of
  1,852,141,683, locals contradicting the console. Xtensa's windowed ABI does not unwind reliably
  from a live target, so **that output must not be quoted as evidence.** Recorded here so the next
  session does not spend the time again.
- The lead that looked strongest and was also wrong: `D (230) intr_alloc: Connected src 28 to int 9`
  — UART1's interrupt, allocated in the last few milliseconds before the freeze. An interrupt
  asserting permanently under emulation would produce this signature exactly. It did not:
  `-Extra "CONFIG_POT_SERIAL_LINK=n"` reproduced the hang with no UART1 at all, which cleared the
  serial link and left only the two tasks. **That negative result is what actually solved it** — with
  UART1 out of the picture there was nowhere left to look but `link_task`, and reading it against
  `espnow_rx_pop`'s guard took a minute.

**What emulation is good for, now that it works:** boot, NVS, the boot epoch, flash persistence, task
scheduling, the namespace and §4's read contract **end to end over the frame link**, and the static
memory figures. **Not** the radio, and therefore not one number §13-M0 asks for. Every statistics line
from this build carries `"no_radio":1` so a capture can never be mistaken for the soak.

### THE LOOSE END IS CLOSED: `potluck-bridge` and `potctl` exist

The host is now an ordinary peer, not a console: MAC `02:00:00:00:00:FE`, node id `0x00FE` derived
from it by the firmware's own rule, HELLO to be admitted, and a heartbeat so it stays Alive in the
node's peer table. §8.1 says a mode transition is a non-event, and that only holds if the firmware
never needs to ask whether a frame came from a host — it does not.

New on the host:

| file | what it is |
|---|---|
| `transport.py` | serial, TCP (QEMU's emulated UART), and an in-process loopback pair |
| `bridge.py` | owns the transport, framing and msg_id correlation; READ/WRITE with real timeouts |
| `value.py` | §4 rule 2's `Value` and `Reading`, host side |
| `ns_payloads.py` | READ/WRITE/REPLY, second implementation of §5.2's namespace payloads |
| `fake_node.py` | a known far end, so a bridge bug and a firmware bug stop looking identical |
| `ctl.py` | `potctl`: `ls`, `read`, `write`, `watch`, `info` |

Three decisions worth keeping:

- **§4 rule 2 is enforced in the host's type system too.** `Reading.number()` returns
  `(number, quality)` and there is no accessor for the number alone; `float(reading)` raises with an
  explanation. Python would otherwise happily invent the conversion this rule exists to prevent, and
  `potctl` deliberately has **no flag to print just the value** — such a flag ends up in a script
  that has lost the age.
- **A timeout raises rather than returning an empty `Reading`.** A `Reading` is a statement about a
  resource; fabricating one for a request that was never answered *is* the unmarked value §4 forbids.
  `UNAVAILABLE` means the node said so, and that distinction has to survive.
- **`write()` returns the REPLY, not a bool.** `NOT_WRITABLE`, `WRONG_OWNER` and `TYPE_MISMATCH` are
  three different conversations, and the one that says the manifest is wrong is the one a bool loses.

A hazard avoided by having met it before: the pending request is registered **before** `send_frame`,
never after. The reply can arrive on the reader thread in that window. The same bug bit the
firmware's own request path in Session 3, and its fix is commented there.

### Tests: the base suite goes from 12 gates to 16

- **`tests/test_ns_wire.cpp`** — 8 cases. Pins READ/WRITE/REPLY against byte literals typed from
  §5.2's table rather than produced by the encoder, and emits an 89-case corpus.
- **`tests/test_ns_diff.py`** — 10 cases replaying that corpus. The failure it exists for: swap
  `quality` (offset 16) and `latency_class` (offset 17) in *both* implementations and every
  self-consistency test on both sides still passes, while the fleet reports the wrong staleness for
  ever. One test asserts the corpus actually contains a case where those two differ, because
  otherwise the guard guards nothing.
- **`tests/test_value.py`** — 22 cases on §4 rule 2's host guarantees, including that a stale reading
  still hands back its number and that a truncated value returns `None` rather than zero-padding.
- **`tests/test_bridge.py`** — 17 cases, bridge against `FakeNode` over a loopback, pumped
  deterministically rather than threaded (plus one threaded end-to-end case). Everything between the
  two is shipping code.

All green: **151 → 181 C++ cases, 75 → 124 Python.** The namespace wire corpus agrees byte-for-byte
across all 89 cases, first run — the C++ and the Python were written from §5.2 independently.

One self-inflicted failure worth recording: `test_ns_diff` initially asserted `len(corpus) > 100` as a
did-it-shrink guard. The emitter produces exactly 89, so the gate failed on a magic number of mine
rather than on any disagreement. Replaced with exact per-opcode counts, which is what a shrink guard
should have been in the first place.

### M1 AND M2 NOW VERIFIED AGAINST REAL FIRMWARE

Not the simulator, not `FakeNode` — the actual ESP32-S3 binary, with the host bridge on the far end of
UART1. This is the first time the two halves of the project have spoken to each other.

`potctl --tcp 127.0.0.1:5555 --node 1001 info`:

```
# bridge 0x00fe on tcp 127.0.0.1:5555, peer unknown
# node 0x1001 epoch 2 admitted; its death window is 100 ms x 6
bridge 0x00fe on tcp 127.0.0.1:5555, peer 0x1001
```

The firmware admitted the host as a peer and told it the death window it announced, rather than the
host assuming one. Then `ls`:

```
free internal DRAM  359736 B  [GOOD, age 530 ms, ts 26885, class L4]
largest free block  294912 B  [GOOD, age 636 ms, ts 26885, class L4]
uptime              26 s      [GOOD, age 702 ms, ts 26885, class L4]
boot epoch          2         [GOOD, age 773 ms, ts 26885, class L4]
peers alive         0         [GOOD, age 854 ms, ts 26885, class L4]
worst peer RSSI     0         [GOOD, age 925 ms, ts 26885, class L4]

6/6 answered with a usable value
```

Six resources, six READ/REPLY round trips over COBS-framed Potluck Frames, each answer carrying §4 rule 2's
whole tuple. The ages differ per line because they are real — each is measured when that resource was
answered, not stamped once for the batch.

Then the two things that prove it is a real membership relationship rather than a request/response toy:

```
14:32:43  0  [GOOD, age 1130 ms, ts 43135, class L4]
14:32:45  1  [GOOD, age 1172 ms, ts 45165, class L4]
14:32:48  1  [GOOD, age 1210 ms, ts 47195, class L4]
```

`sys/peers-alive` goes **0 → 1**: the host's own heartbeats reached the node over the serial link and
the node promoted it from Known to Alive. §8.2's state machine ran against a host peer, over a
transport that is not the radio, without the firmware knowing the difference. That is §8.1's claim
made good.

And a write the node must refuse:

```
$ potctl ... write sys/heap-free 1 --type u32
REFUSED: NOT_WRITABLE
```

Refused **by name**, with a non-zero exit code. `NOT_WRITABLE` rather than a generic failure is what
tells a manifest error apart from a permissions error.

What this does **not** show, stated plainly: one node, no radio, no second board, and the values are
this node's own. Remote reads across a radio, PDR, RTT and everything else §13-M0 asks for still need
hardware.

### Gates: 18 green

`tools\run_all_tests.ps1 -Asan -Firmware` — all 18 pass, including AddressSanitizer and the
esp32s3 firmware build. Static data **53,118 B against §6's 64 KB core cap**, 12.1 KB of headroom,
which is committed rather than free (§6's table allots the rest to M1+ features not yet built).

The simulator is unaffected by the session's firmware changes: 7 nodes on the 52 m fringe link,
**0 death declarations, 8.7% airtime, 109 frames/s, converged.**

### One host-side gap found and fixed along the way

`records.py` did not list `ns` as a known record kind, so a real capture would have warned
*"unrecognised record types (firmware newer than this tool?): {'ns': …}"* about its own node's
namespace. Missed when M1 added `emit_ns_records()`. The known-kind list is now a single named
constant with a comment explaining that it is the version-skew boundary — a false alarm there is worse
than no warning, because it teaches people to ignore the real one.

`potluck-capture`'s live monitor now also folds `ns` records in: it keeps the latest state per
(node, path) rather than the history, prints a line the moment a resource leaves `GOOD`, and reports
the full namespace in the end-of-soak summary. A resource going STALE mid-soak is precisely what §4
rule 2 wants surfaced rather than averaged away.

### Also done

- `stats_task`'s core is now chosen from `portNUM_PROCESSORS` instead of hard-coded to 1. Pinning to
  core 1 on a single-core build is an assertion failure, not a graceful fallback.
- The `ESP_LOGD` step-traces are out of `serial_port_start()`, as Session 3 asked.
- `M0-RUNBOOK.md` gains §2c (the namespace over a real serial link, one board) and §2d (under
  emulation, no boards), both with the caveats stated rather than implied.
- `.claude/zero-assumption/memory.md` gains a QEMU section, and the dual-core "deadlock" row in it is
  **struck through and marked withdrawn** rather than deleted. Two method rows were added beside it:
  that GDB cannot be trusted to unwind a live QEMU Xtensa target, and that one stack sample is not a
  diagnosis. Keeping a withdrawn claim visible is the point — a ledger that only ever records
  conclusions that held teaches nothing about how the wrong ones got in.

### Still open

1. **The rename.** Still the owner's call, still cheap: the code is already ASCII `pot` throughout,
   so it is a directory rename plus prose, and it would delete three workarounds.
2. **Everything needing two boards or a radio.** §13-M0's whole acceptance table — PDR, RTT, airtime,
   the Wi-Fi DRAM `[MEASURE]`, the range cliff — plus remote reads across a link and the two-board
   serial bridge. Unchanged, and unchangeable until the hardware arrives.
3. **M3+**: SUBSCRIBE/PUBLISH, event-queue semantics, the bind table for logical paths, manifests.
   Not started, and correctly so — M0's acceptance outranks them.

Nothing is blocked on a decision except the rename.

---

## Session 5 — 2026-08-01, dμOS becomes Potluck (S3 Edition)

The owner picked the name. **Potluck**: everyone brings a dish — a sensor, some RAM, a radio — and the
cluster eats together. It is the most literal word in English for what §7.2's namespace does, and it
is all ASCII, which was the entire point.

This first release is documented as **Potluck — S3 Edition**. That labels the *hardware support*, not
the project: ESP32-S3 plus host tooling on Windows and Linux. §0.1's invariant is that the
architecture is hardware-agnostic, and an edition label keeps that visible instead of letting today's
silicon quietly become the definition.

### Why not the ESP32-S3 wordplay the owner asked about

Asked for a name that plays on ESP32-S3. Declined, and the reason is the vision invariant itself: a
name built on `esp` brands the project as an Espressif accessory — shelved next to ESPHome, ESP-NOW and
esptool — and bakes one vendor's silicon into the name permanently, which is precisely what §0.1 says
this is not. The wink went into the tagline instead, where it costs nothing:
**e(SP)luribus unum — out of many, one machine.**

Also, every good esp-name is occupied, including by a company that does almost exactly this: Pluribus
Networks ships a "distributed network fabric". Recorded with the other collision checks in
`.claude/zero-assumption/memory.md`.

### The names that were rejected, and why that is written down

Seven candidates were checked by live search rather than by intuition, and the results are in the
ledger so nobody re-runs them. Disqualified for **in-domain** collisions: `muster` (LLNL's "Massively
Scalable Clustering" *and* a Giant Swarm control plane), `krill` (a production RPKI certificate
authority *and* a robotics process orchestrator with health monitoring and restarts — uncomfortably
close to what this does), `pluribus`, `smelt` (nine unrelated projects). `murmur` was the runner-up and
genuinely good — Mumble has officially vacated it — and `planktos` was third.

### The one thing the earlier estimate got wrong

Session 3 recorded that a rename would be "mostly a *directory* rename plus prose, far cheaper than it
looks". That was nearly right and missed one load-bearing detail:

**The namespace URI scheme `dmu://` is hashed into every path hash on the wire.** §7.2 hashes the
canonical path with FNV-1a/32 and the node stores only the 32-bit result, so changing the scheme
changes every hash in the system. That is not prose; it is the wire.

It was safe to do **exactly once**, and now was that once: no fleet is deployed, and paths are declared
at boot rather than persisted. Checked before committing to it — no test hardcodes the hash of a scheme
path (the literals in the tests are arbitrary values like `0xAABBCCDD`, or FNV conformance vectors like
`path_hash("") == 2166136261`), and both implementations compute from the string, so they move together.
Also checked that the longest built-in path grows from 34 to 40 characters against
`sys_resource_path()`'s 64-byte buffer — headroom that matters, because `snprintf` would truncate
silently and a truncated path hashes to something plausible and wrong.

Had this been found after boards were in the field, it would have been a fleet-wide flag day.

### The mapping

| surface | before | after |
|---|---|---|
| project, prose | dμOS | **Potluck** |
| C++ namespace | `dmu::` | `pot::` |
| ESP-IDF components | `dmu_frame` … | `pot_frame`, `pot_link`, `pot_espnow`, `pot_ns` |
| include paths | `dmu/frame.hpp` | `pot/frame.hpp` |
| Kconfig | `CONFIG_DMU_*` | `CONFIG_POT_*` |
| URI scheme | `dmu://lab/…` | `potluck://lab/…` |
| host package | `dmu_capture` in `host/dmu-capture/` | `potluck` in `host/potluck/` |
| CLIs | `dmu-capture`, `dmu-ctl` | `potluck-capture`, **`potctl`** |
| wire format | dμ-Frame v1 | Potluck Frame v1 |
| ESP-IDF project | `dmuos_m0` | `potluck_m0` |
| M8 daemon (unwritten) | `dmu-agent` | `potluck-agent` |

`pot` is for identifiers; `potluck` spelled out is for anything a person types or reads — same split as
`http://` never being shortened to `h://`.

Done as one ordered, auditable script (993 substitutions across 112 files) rather than by hand, with
the rule order arranged longest-first so a broad rule could not eat a name a narrower rule owned. A
backup was taken first, because this repository is not under version control.

**The three vision PDFs keep their `dmuOS_` filenames.** They really were written about dμOS, and
renaming a historical artefact misrepresents it. CLAUDE.md says so explicitly, so no future session
"tidies" them.

### One bug the rename exposed, which is an argument for doing renames

`tests/test_namespace.cpp` asserted that the NUL-terminated and explicit-length hash overloads agree:

```cpp
CHECK_EQ(path_hash("dmu://lab/n2/adc/0"), path_hash_n("dmu://lab/n2/adc/0", 18));
```

That `18` was the string's length written out as a literal. Under `potluck://` the string is 22
characters, so the test failed — **not because anything it tests broke**, but because a magic number
had silently encoded the old scheme's length. Now derived with `sizeof(kPath) - 1`, so it cannot rot
again. A mechanical rename is quite good at finding this class of thing.

### The workaround that deleted itself

The ASCII mirror in `tools\build_firmware.ps1` existed only because of the μ. Rather than delete it,
it is now **conditional**: it fires when the *whole* path contains a non-ASCII character, and prints
`building in place … (all-ASCII path, no mirror needed)` otherwise. Kept because it costs one `if` and
still covers the version of the bug that has nothing to do with the old name — a user account with an
accent in it. `-ForceMirror` exercises it deliberately.

Note that during this session it correctly still *used* the mirror, because the directory is renamed
last (Windows holds a lock on the working directory of a running process). That is the conditional
working, not failing.

### Verified after the rename

- **All 16 host gates green**, unchanged: 158 C++ cases / 36,536 checks, 124 Python cases. The
  namespace wire corpus still agrees byte-for-byte across all 89 cases, with every hash in it different
  from yesterday's.
- **Clean firmware build from scratch** for esp32s3, with `libpot_frame.a` / `libpot_link.a` /
  `libpot_espnow.a` and `CONFIG_POT_*`. Static data **53,118 B — byte-identical** to before, which is
  the expected result and worth stating: a rename that changed the memory figures would mean something
  else changed too.

### Left for the owner: the directory itself

`D:\Projects\dμOS` → `D:\Projects\potluck` cannot be done from inside a session, because Windows holds
a lock on a running process's working directory. Instructions are in the handoff; the only non-obvious
part is that Claude Code keys its per-project state (memory, transcripts) to the directory path, so
`C:\Users\tuebo\.claude\projects\D--Projects-d-OS` has to move alongside it or the project's memory
looks empty afterwards.

### Postscript: one emulator limitation found while verifying the rename

`potctl ls` worked against the renamed firmware first try — all six resources, whole tuple, `6/6
answered with a usable value` — and so did a `write` refusal (`REFUSED: NOT_WRITABLE`). A third command
then failed to connect at all.

That is **QEMU, not the node**: `-serial tcp:…,server,nowait` accepts exactly one connection for the
lifetime of the VM, and the console log went on filling with statistics throughout. Confirmed twice; the
second attempt fails at *connect* with a timeout, which is unambiguous.

So: one `potctl` invocation per emulated run. `ls` does all six reads on one connection and `watch`
holds it open, so this is a smaller constraint than it sounds — and it does not exist on a real serial
port. Registered in the ledger and flagged in the runbook, because it presents as "the node stopped
answering" and would cost the next person an hour.

Worth noting what made this diagnosable in seconds rather than an hour: `potctl`'s own failure message
already says *"Nothing at all means the link, not the namespace"* and prints the byte counts. Error text
that names the likely layer is worth writing.
---

## Session 6 — 2026-08-01, M2's acceptance test, and the bug it found

The owner asked the right question: *is there anything left worth doing before the boards arrive?* The
honest answer was "one thing", and doing it found a data-loss bug that had been silently corrupting
capture files.

### Why this and nothing else

Of the nine milestones, **zero are accepted**. Of the acceptance tests still outstanding, exactly one
does not need hardware:

> **§13-M2** — *"a captured 10-minute session replays and produces byte-identical namespace state."*

Everything else on the list needs two boards, three boards, a radio, or a scope. And building *past*
M0 is the specific thing §13's preamble and the M0 kill criterion warn against — with a concrete
reason, not a procedural one: static data sits at **51.9 KB of §6's 64 KB cap and the remaining
12.1 KB is already allocated on paper** to M1+ features, while the Wi-Fi stack's real DRAM cost is
still a `[MEASURE]` item. Everything worth building next allocates statically into a budget nobody has
measured. §8.2's timers and the broadcast-beacon fix are likewise validated only in simulation against
§3's *cited* PDR rather than PDR measured at this fleet's geometry.

So: close M2, harden the tool the M0 soak will be analysed with, and stop.

### What "byte-identical" had to be made to mean

A digest, not an eyeball. `Monitor.namespace_canonical()` serialises every namespace entry's last known
state with a fixed entry order and a fixed field order, and **excludes every host-side quantity** —
host timestamps, wall clock, arrival order. A digest that moved because the host was busier on the
replay would test nothing. `potluck-capture` and `potctl` both print the SHA-256 of what they observed
plus the command that checks it:

```
python -m potluck --replay session.jsonl --expect-digest <sha256>
```

Mismatch exits 6. A capture is only evidence once something has confirmed it is sufficient to rebuild
the state it claims to record.

### THE BUG: every `ns` and `event` record ever written to a capture was corrupt

`CaptureWriter.write_record` wrote `{"host_ts": …, "kind": <record type>, **data}`. Two record types
carry their own field called `kind` — `ns` has the resource kind (`sampled` / `event`) and `event` has
the event kind (`peer_dead` …) — so `**data` **silently overwrote the envelope**. The line became
unidentifiable as a record of any type, and nothing anywhere said so.

Found because a capture full of `ns` records replayed to an empty namespace. Not found earlier because
nothing had ever replayed one and checked.

The console stream never had the problem: there the envelope key is `t`. The collision was created
purely by renaming `t` to a name a payload might also use. Fixed by making the envelope key **`rec`**,
which no record emits, and by writing the envelope **after** the payload so a future collision cannot
win either. §7.6 now specifies the line format explicitly, including this reasoning, because a capture
format whose rules live only in one function is a format that will grow another collision.

A second, smaller one from the same work: `int(r.get("updates", 0))` crashed on entries learnt from
REPLY frames, because `.get(key, default)` returns `None` when the key is *present and* None. Those
entries legitimately have no update count — the wire carries the reading, not the declaration — so they
now print `?` rather than `0`. Reporting an unmeasured quantity as zero is the same class of mistake as
serving a stale value unmarked.

### Verified

`tests/test_replay.py`, 9 cases, in the gate list — **17 host gates now**:

- a frame capture (potctl-style) replays to a byte-identical namespace;
- a console capture (`ns` records) does the same;
- the digest is stable across repeated replays;
- **the digest notices a single flipped bit** in a captured REPLY's value — a digest that could not
  fail would be decoration;
- a replayed namespace is not empty, which is the guard against the acceptance test passing by
  comparing nothing to nothing;
- a payload naming *every* envelope key cannot overwrite or impersonate the envelope, and its own
  colliding field survives intact.

**And on real firmware.** A `potctl ls` session against the ESP32-S3 binary under QEMU, captured over
the emulated UART, replayed to a byte-identical digest — with the gate then proven to fail on a wrong
one:

```
# capture -> captures/m2-ls.jsonl  (6 namespace entries observed)
namespace state: 6 entries, sha256 6b215de827f95f5357a19fa89dc62e3dc564e938b56d22db1c4bd03d6af2c841
digest matches: the replay reproduced byte-identical namespace state          → exit 0
DIGEST MISMATCH  expected 000…000  got 6b215de8…                             → exit 6
```

All six resources rebuilt **from raw frames alone** — no console records in that file — each printing
`bound ? ms, ? updates`, which is the honest rendering: a REPLY carries §4 rule 2's reading, not the
declaration around it. The first run of this used `watch`, which reads one path and therefore observed
one entry; a one-entry digest would have been a weak thing to call acceptance, so it was redone with
`ls`. Both captures are kept in `captures/`.

### M2 is *mechanically* proven, not yet formally accepted

Stated precisely, because the difference matters: the mechanism is verified end to end against real
firmware, and the unit tests cover multi-entry namespaces including both capture shapes. What has not
happened is §13-M2's sentence literally — **a ten-minute session**. The runs here were tens of seconds,
and QEMU's socket serial port accepts one connection per VM lifetime, which caps how much a single
emulated session can do.

That last mile is a hardware activity anyway: a ten-minute capture belongs to the same bench session as
M0's soak, over a real serial port with no one-connection limit. The tooling will not need changing —
`--expect-digest` is the whole test.

### Also corrected

**§13-M1's acceptance sentence was wrong**, and would have made a correct implementation look broken at
acceptance time. It said unplugging node 2 makes the read return `STALE`. §4's own definitions assign
`STALE` to a value past its bound that is *still delivered with its exact age*, and `UNAVAILABLE` to a
resource whose owner is dead — no value at all. An unplugged node is the second case: its last reading
is not merely old, it is unattributable, since nothing can say whether the resource still exists. The
code returns `UNAVAILABLE` and is right; §13's line now says so, with a note on how to actually observe
`STALE` (stop publishing while leaving the owner alive).

---

## Session 7 — 2026-08-01, published

**https://github.com/Tubifix77/potluck** — public, Apache-2.0 + NOTICE, 13 topics, README.

The license choice, since "acknowledge me, a bit stricter than MIT" has several wrong answers: the
old BSD advertising clause is deprecated and GPL-incompatible, CC-BY is not a software license, and a
custom license gets *less* acknowledgement because serious users skip anything unbadged. Apache-2.0's
section 4(d) is the mechanism wanted: the NOTICE file — which names the author — must travel with every
redistribution or derivative, 4(b) requires stating significant changes, and section 3 adds a patent
grant MIT lacks. Text fetched from apache.org rather than typed from memory, per rule 3.

Tracked deliberately: the zero-assumption ledger (CLAUDE.md rule 3 makes it method, not setup) and the
two small M2 acceptance captures (evidence you cannot open is a claim). Untracked deliberately: build
trees, generated sdkconfig, the two regenerable multi-MB corpora, and local Claude settings.
`.gitattributes` pins `* -text` — no line-ending normalisation ever, because letting git rewrite files
per-platform is the artefact-under-test mistake as a config option.

README states the S3 Edition framing (the label names the hardware support, not the project) and the
project's own standing caveat, verbatim in spirit: **two physical boards have never exchanged a
heartbeat.** Nothing published claims a measurement that was not made.
