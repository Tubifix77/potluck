# When the boards arrive

The project was **deliberately paused on 2026-08-01** — not stalled: everything buildable without
hardware is built, verified, and public, and the next line of work *requires* a physical ESP32-S3 on
a USB cable. This file is the resumption path. It assumes months may have passed and nobody
remembers anything.

**State at pause:** 19 test gates green (158 C++ cases / 36,536 checks, 133 Python). Firmware builds
in place for esp32s3. M0/M1/M2 built — M1 and M2 exercised against the real firmware under QEMU
emulation, M2's replay mechanism digest-proven. **Zero milestones accepted**, because every
acceptance test left needs silicon. Static data 53,118 B of the 64 KB cap, and the remaining
headroom is committed on paper to later milestones — which is exactly why nothing past M2 was built.

---

## Step zero: what to buy

Nothing here needs soldering. Everything is push-fit.

| # | Item | Qty | Why |
|---|---|---|---|
| 1 | **ESP32-S3-DevKitC-1 N16R8**, headers pre-soldered, **two USB sockets** | 3 | 2 is the minimum for M0; the 3rd is a spare against a dead-on-arrival board *and* makes the broadcast-beacon comparison meaningful (at 2 nodes broadcast and unicast are indistinguishable) |
| 2 | USB **data** cables matching the sockets | 3–4 | 3 for the boards; a 4th only if the serial adapter is the socket kind rather than a USB-A dongle. Check the listing photos — connector type varies by board revision. See the charge-only warning below |
| 3 | USB hub — **only if short of PC ports, and only a mains-powered one** | 0–1 | three free ports on the PC is the better setup; see the power note below |
| 4 | SN65HVD230 CAN transceiver module | 2 | M4's CAN transport — and the same two modules serve the sibling Powersuit project |
| 5 | Dupont jumper **female-female** pack (a mixed M-M/M-F/F-F set is fine and no dearer) | 1 | ~10 wires total; a 40-pack is plenty |
| 6 | ~~Breadboard~~ | **0** | **Not needed, and one will not fit three boards.** See below |
| 7 | USB-to-TTL adapter, **3.3 V logic** — prefer **CP2102** over CH340 | 0–1 | optional, and *not* for flashing or power — it is the second serial channel reaching the UART1 frame link. See below for why CP2102. The free alternative is a rebuild with the frame link on native USB, at the cost of RF quality during that test |

**Do not buy:** 120 Ω resistors (each SN65HVD230 module carries its own, which is exactly right for a
two-node bus), LEDs, screw-terminal shields, a soldering iron, or a breadboard.

**Why no breadboard.** The devkit and the transceiver module both have male header pins, so
female-female jumpers join them directly: 3V3, GND, TX and RX from each board to its own module, then
CANH→CANH and CANL→CANL between the two modules. Ten wires, nothing in between. A breadboard would
only add a place for a wire to fall out of.

It would not fit anyway, which is worth knowing before buying one hopefully: a half-size (400-point)
breadboard is about 82 × 54 mm and a DevKitC-1 is about 70 × 28 mm, so one small breadboard holds
**one** board. Three would want ~210 mm of length; even a full-size 830-point board (~165 mm) takes
only two.

That situation never arises regardless: **the three boards are never all wired at once.** Only two
ever carry CAN transceivers — a two-node bus is what both acceptance tests need, and a third module
would put three terminators on a bus that wants exactly two. The third board is for the Wi-Fi mesh
test, which uses no wires at all, just USB power.

**Check before clicking buy:** the photo shows **two** USB sockets — a board with only the native USB
port cannot run this firmware at all, because `CONFIG_ESP_PHY_ENABLE_USB=n` makes that port unusable
while Wi-Fi is on. Also confirm the listing says headers are already soldered.

**A 4th cable, or not, depends on the adapter's shape.** A USB-to-TTL adapter comes either as a
dongle with a USB-A plug moulded on — plugs straight into the PC, no cable — or as a module with a
Micro-USB/USB-C socket, which needs its own. Check the listing photo before assuming.

**⚠ Charge-only cables are the trap here.** Many cheap USB cables carry power but no data lines. Such
a cable powers the board perfectly and it never enumerates as a COM port — which presents as a driver
problem, or a dead board, and costs an hour. Prefer cables that shipped with real data devices.
**If a board lights up but no COM port appears, swap the cable before suspecting anything else.**

**One cable per board does everything:** 5 V power, flashing, the serial console, and the JSON
statistics stream `potluck-capture` records. No separate power supply is needed anywhere.

### What the USB-serial adapter is actually for

Not flashing, and not power — both of those already arrive on the board's own USB cable. It exists to
reach the **frame link**, and only that.

The board's built-in USB-UART bridge is wired to **UART0**, the console: log lines, the JSON
statistics stream, and flashing. Potluck's frame link is **UART1 on GPIO 17/18** (`CONFIG_POT_SERIAL_TX_GPIO`
/ `_RX_GPIO`), which are bare header pins with nothing on the board joining them to USB. The adapter
is the wire from those two pins to the PC.

Two channels rather than one because the console is human-readable text and the frame link is binary
COBS-framed frames; sharing a wire, each corrupts the other. That is why `serial_port.cpp` puts the
link on UART1 in the first place.

**It is a PC-to-board link, not a board-to-board one, and its far end is pins rather than a socket.**
The adapter connects to the PC by USB and to the board by *jumper wires on the GPIO header* — the
board's own USB-C sockets play no part. Nothing here replaces the radio: `hal_send()` routes a frame
to the serial link only when its destination is `kHostMac`, and everything else goes to ESP-NOW. The
frame link is how the host joins the cluster as a peer; boards always reach each other by radio.

```
                        ┌───────────┐
                        │  your PC  │
                        └─┬───────┬─┘
              USB cable   │       │   USB (serial adapter)
        power/flash/JSON  │       │   ...then 3 jumper wires
                          │       │   to GPIO 17, 18, GND
                          ▼       ▼
                     ┌──────────────┐
                     │   board 1    │   the only board with two connections
                     └──────┬───────┘
                            ┊
                       ESP-NOW radio — no wires, ever
                            ┊
        ┌──────────────┐    ┊    ┌──────────────┐
        │   board 2    │┈┈┈┈┴┈┈┈┈│   board 3    │
        └──────┬───────┘         └──────┬───────┘
               │ USB (console)          │ USB (console)
               ▼                        ▼
             to PC                    to PC
```

Three wires, crossed, and **no power line**:

| board | adapter |
|---|---|
| GPIO 17 (TX) | RX |
| GPIO 18 (RX) | TX |
| GND | GND |

Leave 3V3 and 5V unconnected — the board is already powered by its own USB cable, and tying two
supplies together invites trouble. GND alone provides the shared reference the signals need.

**The adapter must be 3.3 V logic.** An ESP32 GPIO's absolute maximum is VDD+0.3 V (~3.6 V), and 5 V
drives overcurrent through the internal clamping diodes.

**Which chip: prefer CP2102 over CH340.** Both work, but the CP2102 signals at 3.3 V *natively* —
there is no jumper to set, therefore none to set wrongly, and mis-setting it is the only action in
this build that can damage a board. It also needs no additional driver, since CP210x is already
required for the boards themselves, and its on-chip EEPROM serial number keeps its COM port stable
across replugs where a CH340 typically has none. The CH340's one advantage is a 2 Mbps ceiling
against the CP2102's 1 Mbps, which does not matter here: `CONFIG_POT_SERIAL_BAUD` defaults to
921 600.

The cost of choosing CP2102 is that every device then enumerates as "Silicon Labs CP210x". That is
already true of three identical boards, so a way to tell ports apart is needed regardless: run
`tools\capture.ps1 --list-ports`, unplug one thing, and see which entry disappears. Attach the
adapter last and it is the newest COM number.

One adapter is enough, on one board: M1's acceptance is *"`potctl` asks node 1 to read node 2's
resource, then node 2 is unplugged"*, so only node 1 needs a frame link. During that test one board
wears two cables — its own USB, plus the adapter — and the others keep one each.

### Power: the one bench mistake that would fake a protocol failure

The ESP32-S3-WROOM-1 draws a **peak of 330 mA at 3.3 V transmitting 802.11b at 20.5 dBm**
([datasheet](https://documentation.espressif.com/esp32-s3_datasheet_en.html)) — an LDO passes that
through nearly one-for-one from the 5 V rail. So per board, budget ~350 mA of peak USB current.

- **Three separate PC ports** — 500 mA each on USB 2.0, 900 mA on USB 3.0. Fine, and the best setup.
- **A mains-powered hub** — fine.
- **A bus-powered hub** (no wall wart) — **not fine**: 500 mA total shared, against roughly 1050 mA
  of peak demand from three boards.

Why this deserves its own section rather than a footnote: the 330 mA figure is a 100 %-duty-cycle
measurement, while heartbeat traffic is short bursts every 100 ms. An undersized supply therefore
does not fail cleanly — it sags **occasionally**, mid-transmit, at random times across 24 hours. Each
sag resets a board, which bumps its boot epoch and stops its heartbeat, and the peer declares it
dead. §13-M0's acceptance criterion is *zero false death declarations*, so a marginal hub would
manufacture precisely the failures the soak exists to rule out, and they would read as a protocol
bug. Related: stop the PC sleeping, or USB suspend ends the capture mid-soak.

Stay with **N16R8** unless you have a reason not to: it is what every memory figure in this repo is
attributed to, and deviating means re-measuring rather than comparing.

Sources and the wider pin-exclusion list are in `.claude/zero-assumption/memory.md` under
*Bench hardware*.

### Do this while the parcel is in transit: install two drivers

"The board does not appear as a COM port" is the classic first-hour blocker, and it is entirely
avoidable — **both drivers install with no hardware present.** Note they are different chips:

| chip | on what | driver |
|---|---|---|
| **CP2102N** | the three DevKitC-1 boards' USB-to-UART port | [Silicon Labs CP210x VCP](https://www.silabs.com/developer-tools/usb-to-uart-bridge-vcp-drivers) |
| **CH340 / CH340G** | *only* if the frame-link adapter uses CH340 rather than CP2102 | [WCH CH341SER](https://www.wch-ic.com/downloads/CH341SER_EXE.html) |

Espressif's [Establish Serial Connection](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/establish-serial-connection.html)
page is the reference if a port still fails to enumerate. Confirm afterwards with
`tools\capture.ps1 --list-ports`, which prints every port it can see and needs no board attached.

---

## Hour one, regardless of how many boards came

```powershell
tools\run_all_tests.ps1 -Asan -Firmware
```

Expect **ALL PASS on 19 lines** and the budget line `51.9 KB used, 12.1 KB of headroom`. This is not
ritual — it proves the toolchain, the three wire-format implementations and the firmware are still
in the state the coming measurements will be attributed to.

If it fails after months of drift: the toolchain is pinned (ESP-IDF **v6.0.2** at `D:\esp\esp-idf`,
Xtensa GCC 15.2.0 — versions and sources in `.claude/zero-assumption/memory.md`). Diagnose against
the pin; do not "upgrade to fix" before the soak, or the soak measures the upgrade.

## With ONE board

One board cannot measure a link, but it retires real risk:

1. **Flash through the socket labelled UART, not USB.** The USB-C socket marked USB will play dead —
   by configuration, not fault: this build turns the USB PHY off because it degrades the radio, and
   the whole point of M0 is measuring the radio. Details: [M0-RUNBOOK.md](M0-RUNBOOK.md) §2.

   ```powershell
   tools\build_firmware.ps1 -Flash -Port COM?
   tools\capture.ps1 --port COM? --interval 5 --show-logs --duration 60
   ```

2. **Read the boot record. The first number that matters in the whole project is on it:** the
   Wi-Fi-stack DRAM figures, step by step. This is the architecture's open `[MEASURE]` item — if the
   stack eats more than ~40 KB, the memory budget's RX ring shrinks first, and that verdict shapes
   everything after M2. The firmware warns on the boot line itself if the threshold is crossed.
   Register the measured figure in the ledger and resolve the `[MEASURE]` tag.

3. **Sanity-check identity.** The node id must derive from the board's real MAC. If the log shows
   the `MAC efuse reads 00:00:00:00:00:00` error, stop — that path was only ever seen under QEMU,
   and on real silicon it means an unprogrammed efuse.

4. **The serial frame link on real wires.** UART1, default pins TX=17 / RX=18, through any
   USB-serial adapter (crossed: board TX → adapter RX):

   ```powershell
   python -m potluck.ctl --port COM? --node <id> ls
   ```

   Expect `6/6 answered with a usable value` — the same result QEMU gave, now on silicon, with no
   one-connection-per-run limit.

5. **Formally close M2.** Its acceptance line asks for a *ten-minute* captured session replaying to
   byte-identical namespace state. The mechanism is already digest-proven; the duration was the only
   missing piece, and it needed a real port:

   ```powershell
   python -m potluck.ctl --port COM? --node <id> --capture captures/m2-10min.jsonl watch sys/heap-free --interval 1 --count 600
   python -m potluck --replay captures/m2-10min.jsonl --expect-digest <the digest the session prints>
   ```

   Exit 0 → tick M2 in the README's milestone table: **accepted**.

## With TWO boards

Now the part no emulator could touch — the first heartbeat this project ever exchanges over a radio:

1. Flash both (same binary; ids derive from each MAC), then the short sanity run from
   [M0-RUNBOOK.md](M0-RUNBOOK.md) §2: `peer_discovered`, `version_pinned` (1446 B if both report
   ESP-NOW v2), `tx-pdr%` near 100, `rtt-samples` climbing.
2. Cross-check the cell against the simulator's prediction (`pot_sim.exe --nodes 2 …`) — the
   simulator ran on cited link figures; this is the first chance to see how wrong they were at your
   geometry.
3. **The 24-hour soak** — [M0-RUNBOOK.md](M0-RUNBOOK.md) §3, then the acceptance checklist in §6.
   Keep the boards well inside 56 m, write down the geometry, leave the frame tee off. The summary
   `potluck-capture` prints on Ctrl-C **is** the M0 deliverable: packet-delivery ratio both ways,
   round-trip histogram, zero false deaths expected at bench distance.
4. **The failure drills** (§4 of the runbook): BYE button = *left* ≠ *dead*; power-cycle =
   reboot detected by epoch, not a revival; table-full refusal.
5. **M1's acceptance, exactly as written:** read a resource of node 2 from node 1, then unplug
   node 2 — the read must return **`UNAVAILABLE`**. Note: the architecture doc originally said
   `STALE` here and was corrected; the code is right. Do not "fix" the firmware to match old prose.

## After acceptance

- Tick the checklist in the runbook, move M0 (and M1/M2) from *built* to **accepted** in the
  README's milestone table — the one table this repo promises to keep honest.
- **Read the parked finding from the pause** — [M0-LOG.md](M0-LOG.md), final section, *"During the
  pause — a candidate M9 from Powersuit"*. Short version: the owner's Powersuit project was thrown
  at the architecture as a falsification test and passed — advisories model cleanly on the existing
  event resource kind — but it exposed that SUBSCRIBE/PUBLISH and the event queues are **fully
  specified and never scheduled by any milestone**, while the M6 demo and the home wake-word both
  consume them. When milestone scoping resumes, that entry is standing input: it carries a
  ready-made acceptance test (seq-gap count vs. independently captured drops) and the
  whitelist-becomes-manifest mapping. It is input, not a decision — no M9 exists.
- Register every measured number in `.claude/zero-assumption/memory.md` with source "this bench" and
  the geometry; resolve the `[MEASURE]` tags.
- Log the session in [M0-LOG.md](M0-LOG.md) — including anything that went wrong, and anything you
  concluded and later withdrew. The log keeps its mistakes; that is its value.
- Commit and push. The public status table is a promise.

## If you build the LED demo

Making the board's onboard RGB LED a writable cluster resource — `potctl write` on the laptop, the
*other* board's LED changes — is the vision in miniature and needs no extra hardware. One thing to
get right on the first try: **the LED's GPIO differs by board revision (48 on v1.0, 38 on v1.1)**,
because GPIO47/48 sit on the 1.8 V SPI rail used by PSRAM. Both revisions are sold and listings
rarely say which. So the pin is a **Kconfig value with no default that is silently wrong** — a
hardcoded 48 on a v1.1 board is a dead LED that looks exactly like a broken driver. Sources and the
wider pin-exclusion list are in `.claude/zero-assumption/memory.md` under *Bench hardware*.

## Sharp edges, one line each

- USB socket dead while Wi-Fi runs → by design, use the UART socket ([runbook §2](M0-RUNBOOK.md)).
- Both boards must share `CONFIG_POT_CHANNEL`; the boot record prints it.
- Packet delivery falls off a cliff at 56–70 m — a soak at the edge measures the edge.
- Frame tee (`CONFIG_POT_FRAME_TEE`) off during any measurement run; its console cost lands inside
  the timing being measured.
- One `potctl` per **QEMU** run (socket accepts one connection per VM lifetime); real ports have no
  such limit.
- A backtrace decoded against the wrong binary looks completely plausible —
  `tools\decode_backtrace.ps1` refuses on hash mismatch; take the refusal seriously.
- GDB backtraces from a live QEMU target are not evidence (windowed-register unwinding lies);
  the QEMU monitor's single-frame `info registers` is trustworthy, nothing built on top of it is.

## What NOT to do while the soak runs

Do not start M3+. The remaining 12.1 KB of the static budget is already allocated on paper, the
Wi-Fi DRAM figure that validates the whole budget is the very thing being measured, and the
membership timers under everything else are being validated against real packet loss for the first
time. Two boards exchanging measured heartbeats outrank any amount of new code — that sentence has
been the project's rule since the first session, and the pause exists to honour it.
