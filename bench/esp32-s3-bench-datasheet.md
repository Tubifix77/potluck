# Bench hardware datasheet — the owner's ESP32-S3 kit

Everything known about the hardware on this bench, compiled 2026-10-06 from the Potluck repository:
the owner's own readings, the zero-assumption ledger (`.claude/zero-assumption/memory.md`), and
measurements taken on these exact boards (`M0-LOG.md`, `captures/`). Written to be handed to another
project. **Every row says how it is known:**

| tag | meaning |
|---|---|
| **measured** | observed on these boards or modules, on this bench |
| **owner** | read off the physical part by the owner |
| **datasheet** | from a primary vendor document (Espressif, TI, Silicon Labs) or the ESP-IDF source tree |
| **seller** | from the AliExpress listing or its photos/schematic, not a datasheet |
| **unknown** | not established; do not assume |

Toolchain everything below was measured with: **ESP-IDF v6.0.2**, Xtensa GCC 15.2.0
(`esp-15.2.0_20251204`), mbedTLS 4.1.0, Windows 11. A different IDF version can change memory and
timing figures.

---

## 1. What is on the bench

Ordered 2026-09-21 from AliExpress (DKK 408 all-in), arrived 2026-10-01.

| item | qty | notes |
|---|---|---|
| ESP32-S3-DevKitC-1 **N16R8**, two USB-C sockets, headers pre-soldered | 3 | a clone: no Espressif seal on the listing |
| SN65HVD230 CAN transceiver module | 2 | |
| HW-598A CP2102 USB-to-TTL dongle (USB-A male, red module) | 1 | |
| Dupont jumpers, **female-female** only, 20 cm | 40 | no M-M, no M-F, **no breadboard** |
| 6-port USB mains charger (power only) | 1 | |

**Not on the bench:** multimeter, oscilloscope, logic analyzer, breadboard, soldering iron,
resistors, M-F jumpers. Anything that needs one of these has not been done.

---

## 2. The boards: ESP32-S3-DevKitC-1 N16R8

### 2.1 Chip, as the boards report themselves (measured, `esptool chip-id` / `flash-id`, board A)

| property | value |
|---|---|
| chip | ESP32-S3 (QFN56), revision **v0.2** |
| cores | dual-core Xtensa LX7 + LP core, **240 MHz** maximum |
| flash | **16 MB**, quad mode, 3.3 V (eFuse) |
| PSRAM | **8 MB embedded, octal, AP_3v3** |
| radio | Wi-Fi **2.4 GHz only** (the listing's "dual-band" claim is false — no ESP32 has 5 GHz; datasheet: ESP-IDF `soc_caps.h`) |
| Bluetooth | **BLE only, no Bluetooth Classic** (datasheet: `soc_caps.h`) |
| hardware crypto | SHA, AES, MPI (bignum/RSA) accelerators; **no ECC/ECDSA peripheral** (datasheet: `soc_caps.h`) |

### 2.2 The three boards

Node id is derived from the MAC, `(mac[4] << 8) | mac[5]`, if you want the same naming.

| label | MAC | id | USB bridge serial | console port on the owner's PC |
|---|---|---|---|---|
| A | `b8:1f:3f:da:63:00` | 0x6300 | `5CBC414102` | COM3 |
| B | `b8:1f:3f:da:73:68` | 0x7368 | `5C93086589` | COM4 |
| C | `b8:1f:3f:da:81:60` | 0x8160 | `5C93086538` | COM5 |

COM numbers are what Windows assigned on this PC; the bridge serial is stable and identifies each
board without unplugging anything (measured). Factory images are backed up under
`D:/esp/board-backups/` (A: full 16 MB; B, C: first 2 MB), named by MAC.

### 2.3 USB and serial

| fact | value | how known |
|---|---|---|
| USB-to-UART bridge | **CH343** (WCH), `VID_1A86 PID_55D3`, "USB-Enhanced-SERIAL CH343" — **not** a CP210x. Windows 11 had the driver; no install needed | measured |
| **Two USB-C sockets that look identical** | one is the CH343 UART (console, flashing, power); the other is the S3's native USB (GPIO19/20). Only the silkscreen tells them apart — tape-label them | owner, measured |
| Native USB socket is dead while Wi-Fi runs **if** `CONFIG_ESP_PHY_ENABLE_USB=n` | Potluck sets it `n` because the USB PHY degrades Wi-Fi on the S3 (IDF's own Kconfig help says so). That is a firmware choice, not the hardware: with the default `y` the native port works but Wi-Fi suffers | datasheet (`esp_phy/Kconfig`), this repo's config |
| Auto-reset | DTR/RTS reset is wired: esptool resets into the bootloader without holding BOOT | measured |
| **Opening the console port with pyserial defaults reboots the board** | pyserial's defaults toggle DTR/RTS. To read without resetting: set `dtr=False, rts=False` **before** `open()` | measured (bisected on board A) |
| Console UART | UART0 on **GPIO43 (TX) / GPIO44 (RX)**, which are also the `TX`/`RX` labels on header J3. Potluck runs it at 115200 | datasheet (DevKitC-1 guide) + this repo's config |
| Console input works | the firmware reads command lines from UART0 through the CH343 | measured |
| Power, flashing, console and logging | all over the one CH343 USB cable; no separate supply needed | measured |
| Charge-only cables | a power-only cable lights the board and never makes a COM port; swap the cable first | general bench experience, recorded in the repo |

### 2.4 Headers (verified against Espressif's DevKitC-1 **v1.1** table, and checked pin-for-pin on board A by the owner)

```
J1, pin 1 -> 22:  3V3, 3V3, RST, 4, 5, 6, 7, 15, 16, 17, 18, 8, 3, 46, 9, 10, 11, 12, 13, 14, 5V, G
J3, pin 1 -> 22:  G, TX(43), RX(44), 1, 2, 42, 41, 40, 39, 38, 37, 36, 35, 0, 45, 48, 47, 21, 20, 19, G, G
```

Identify a pin by its **printed label**, never by counting positions — clones are not guaranteed to
copy the order, even though these do.

**Which DevKitC-1 revision these are: unknown.** The header tables match v1.1. The onboard RGB LED is on
GPIO48 on v1.0 and GPIO38 on v1.1; Potluck drives both and the LED works, but which pin carries it
on these boards is **not established**.

### 2.5 Pins

**Never use** (datasheet: ESP-IDF GPIO reference, hardware design guidelines):

| GPIO | why |
|---|---|
| **35, 36, 37** | octal PSRAM on the R8 part. **The silkscreen offers them as ordinary GPIOs. They are not.** |
| 26–32 | SPI flash / PSRAM |
| 19, 20 | native USB D−/D+ (the second USB-C socket) |
| 43, 44 | UART0 console (the CH343) — usable only if you give up the console |

**Care needed:**

| GPIO | why |
|---|---|
| 0, 3, 45, 46 | strapping pins. GPIO0 is the BOOT button — fine as a button input |
| 47, 48 | on the 1.8 V PSRAM SPI rail, not the normal 3.3 V GPIO supply (why Espressif moved the LED to 38 on v1.1) |
| 38 / 48 | the RGB LED, by board revision |

**Used and working on this bench** (measured): GPIO17/18 as UART1 (921600 baud to the CP2102), GPIO4/5
as TWAI (CAN) TX/RX at 500 kbit/s, GPIO0 as a button input, GPIO48+38 as the WS2812-style LED.

**Electrical:** I/O is **3.3 V and not 5 V tolerant** — absolute maximum about VDD + 0.3 V (≈3.6 V)
(datasheet). Never drive a GPIO from a 5 V signal.

### 2.6 The onboard RGB LED (measured)

Addressable pixel, **GRB byte order**, driven from the RMT peripheral. Lights correctly when both
GPIO48 and GPIO38 are driven (see 2.4 on which one it really is).

### 2.7 Power

| fact | value | how known |
|---|---|---|
| peak current, ESP32-S3-WROOM-1 transmitting | **330 mA at 3.3 V** (802.11b, 1 Mbps, 20.5 dBm, 100 % duty) → budget **~350 mA per board** from USB 5 V | datasheet (Espressif ESP32-S3) |
| three boards on one **bus-powered** hub | not enough (500 mA shared against ~1050 mA of peaks); use separate PC ports or a mains-powered hub | derived from the row above |
| failure mode of a weak supply | random brownout resets mid-transmit, which look like firmware or protocol faults rather than a power fault | derived, recorded as a bench rule |
| the 6-port mains charger | runs all three boards; used for the distance sweep and the M3 power-cycle test | measured |
| a small power bank | was too unstable for a board during the distance sweep; mains was fine | owner |
| measured current draw of these boards | **unknown** (no multimeter) | — |

### 2.8 Memory, flash and CPU, as Potluck uses them

| fact | value | how known |
|---|---|---|
| internal SRAM windows | DRAM `0x3FC88000–0x3FD00000` (480 KB) and IRAM `0x40370000–0x403E0000` (448 KB) overlap **one unified pool**; static IRAM use shrinks the DRAM available | datasheet (ESP-IDF `soc.h`, memory-types guide) |
| **Wi-Fi stack cost, ESP-NOW active** | **32,264 B** of internal DRAM (`esp_wifi_init` + `esp_wifi_start`); ESP-NOW itself adds only 152 B | measured (board A) |
| free internal DRAM after the radio is up | 249,936 B free, largest block 204,800 B (early firmware, one node, no peers) | measured |
| PSRAM | 8 MB present; **Potluck does not enable it** (`CONFIG_SPIRAM` unset), so nothing here was measured with PSRAM on | this repo's config |
| default CPU clock | Potluck runs at **160 MHz** (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160`); 240 MHz is available | this repo's config |
| Potluck's partition table | `nvs` 0x9000 (24 KB), `phy_init` 0xF000 (4 KB), `factory` app 0x10000 (1 MB), two 64 KB data slots at 0x110000 and 0x120000. The 16 MB flash is mostly unused | `firmware/partitions.csv` |
| a full Potluck radio image | ~756 KB (fits the 1 MB app partition) | measured (build output) |
| `uxTaskGetStackHighWaterMark()` | returns **bytes** on ESP-IDF, not words | ESP-IDF behaviour, recorded from the sibling Powersuit project |

### 2.9 Random numbers (datasheet: ESP-IDF `esp_random.h`, `bootloader_random.h`)

`esp_random()` / `esp_fill_random()` return **true random numbers only while Wi-Fi or Bluetooth is
running**. With the radio off, call `bootloader_random_enable()` around the read (and
`bootloader_random_disable()` after), and never while the radio is in use.

### 2.10 Cryptography speed (measured on board B, 160 MHz, size-optimised build, 16 runs each)

| operation | time | library |
|---|---|---|
| **Ed25519 verify** | **27.0 ms** | Monocypher 4.0.3 (vendored in `firmware/components/monocypher`) |
| Ed25519 sign | 9.3 ms | Monocypher |
| X25519 shared secret | 19.0 ms | Monocypher |
| P-256 ECDSA verify | 441.5 ms | mbedTLS 4.1 (PSA), hardware MPI |
| P-256 ECDSA sign | 222.8 ms | mbedTLS |
| X25519 shared secret | 209.3 ms | mbedTLS |
| P-256 ECDH | 205.4 ms | mbedTLS |
| HMAC-SHA256 over 64 bytes | 0.17 ms | mbedTLS, hardware SHA |

With speed optimisation (`-O2`), mbedTLS gets faster (P-256 verify 325.9 ms) and Monocypher slower
(Ed25519 verify 39.0 ms). **mbedTLS 4.1 in IDF v6.0.2 has no EdDSA at all** (datasheet: the IDF tree).
Stack per Ed25519 verify: about 2.3 KB. Evidence: `captures/m5-crypto-bench-*-2e69038.log`.

---

## 3. Radio: ESP-NOW on these boards

### 3.1 Limits (datasheet: ESP-IDF ESP-NOW guide and `soc_caps.h`)

| fact | value |
|---|---|
| band | 2.4 GHz only |
| peers | **20** maximum, of which the broadcast entry takes one; **17** encrypted peers maximum (default 7) |
| encryption | link-layer CCMP for unicast only; **broadcast/multicast cannot be encrypted** |
| payload | v1: 250 B per frame; **v2: 1470 B** (measured: two boards negotiated v2 with a 1446-byte usable profile) |
| channel | every node must be on the same channel; a node joined to a Wi-Fi router is forced onto the router's channel, which then binds the whole cell |
| ESP-NOW + Wi-Fi station on one chip | supported |
| sleep with ESP-NOW | needs `esp_wifi_connectionless_module_set_wake_interval()`; station mode only |

### 3.2 Measured performance (owner's house, channel 1, PCB antennas)

**Standing condition:** the house is RF-noisy (much Wi-Fi and Bluetooth, and channel 1 is crowded).
Every figure below includes that contention.

| test | result |
|---|---|
| **24.52 h soak**, three boards ~10 cm apart | unicast delivery **100.0000 %** both ways on all six links (791,324 out / 6,337,757 in); round trip **p50 4–6 ms, p99 16–22 ms, p99.9 42–60 ms, none above 85 ms**; zero reboots, zero deaths; free DRAM identical at start and end |
| **distance sweep**, ~10 × 20 m two-storey house, up to ~16 m, up to four walls and a floor | unicast 100 % at six of eight spots, worst 99.17 % out / 99.80 % in; worst p99 round trip 30–42 ms; **no cliff** |
| where links start dropping | brief dropouts (each recovered within seconds) only where median RSSI was **−73 dBm or weaker**; none at −63 dBm or better |
| what matters most indoors | **orientation and obstacles, more than distance**: the farthest spot (~16 m) was clean; one 3 m closer but behind a tumble dryer and fridge was the worst. Hold orientation fixed when comparing |
| close range | RSSI −13 to −32 dBm at desk distances |

Evidence: `captures/soak-2026-10-02-report.txt`, `captures/sweep-2026-10-04-report.txt` and
`-notes.md`.

**Not measured:** raw throughput (Potluck sends small frames), range outdoors, long-range (LR) mode,
any channel other than 1, through a human body (a ~20 dB penalty is cited in the ledger, not measured).

---

## 4. CAN (TWAI) with the SN65HVD230 modules

### 4.1 The modules

| fact | value | how known |
|---|---|---|
| 4-pin control header | **TX, RX, GND, 3.3V** reading top to bottom with the pins pointing left (the seller lists the same pins as 3.3V / GND / RX / TX, from the other end) | owner + seller |
| bus header | **CANL, CANH** top to bottom, in that same orientation; CANH/CANL are also on a blue screw terminal, in parallel | owner + seller |
| wiring to the ESP32 | **straight, not crossed**: ESP32 CAN-TX → module TX (the transceiver's driver input D), ESP32 CAN-RX → module RX (its receiver output R) | seller's schematic + measured working |
| supply | **3.3 V** (3.0–3.6 V), from the board's 3V3 pin | seller; measured working |
| logic thresholds | input low ≤ 0.8 V, high ≥ 2 V: the S3's 3.3 V logic drives it fine | seller |
| termination | **120 Ω on board** (R2 across CANH/CANL on the schematic) → two modules terminate a two-node bus correctly; a third module would over-terminate (no jumper to remove it is known) | seller's schematic |
| slope control | `Rs` tied to GND through 10 kΩ (slope-control mode, ~15 V/µs), not changeable without soldering. **It does not cap the bit rate**: TI's datasheet states no rate limit for slope control (the tradeoff is bus length). An earlier "250 kbit/s cap" in the repo was withdrawn | datasheet (TI SLOS346O, read from the PDF) |
| standby current | 370 µA (the listing graphic's "370 MA" is a typo) | seller |

### 4.2 Measured on two boards (B and C, 500 kbit/s, ~20 cm, ten jumpers)

| test | result |
|---|---|
| one board alone | **cannot transmit**: every frame needs an ACK from another node (use the TWAI loopback self-test instead, with RX on the TX pin) |
| self-test (loopback) on one board | 294/294 frames, one 8-byte extended frame in ~320 µs submit-to-sent |
| two boards, one flooding at **3,072 frames/s** | zero bit, form or stuff errors over 5 minutes; a high-priority frame left within 335–683 µs every time |
| ground | the bus ran **without** a module-to-module ground wire, the boards sharing ground through the PC's USB. Add a GND wire whenever the boards are on separate supplies |
| receive under load | the ISR queue dropped 220 of 881,583 frames at ~3,000 frames/s — drain faster, or filter, for a busy bus |

### 4.3 ESP-IDF v6 TWAI driver facts that bite (datasheet: the IDF source)

- `twai_node_transmit()` queues a **pointer** to your frame, not a copy: the frame must stay valid until
  `on_tx_done` names it.
- The driver's transmit queue is **first-in, first-out**: a high-priority frame waits behind everything
  queued before it on the same node. To get priority, hand the driver one frame at a time in your own
  priority order.
- Received frames can only be read inside the `on_rx_done` callback.
- Lost arbitration is reported through `on_error` (`arb_lost`), which the driver does enable.

---

## 5. The CP2102 adapter (HW-598A)

| fact | value | how known |
|---|---|---|
| pins, top to bottom | **3V3, TXD, RXD, GND, +5V** (the seller's text lists them in a different order) | owner |
| chip | reports as an original **CP2102**, `USB\VID_10C4&PID_EA60&REV_0100` | measured (Device Manager) |
| driver | needed Silicon Labs' **CP210x Universal Windows Driver v11.6.0**, installed through Device Manager. The package's `.bat` said success but installed nothing (device code 28) | measured |
| logic level | the CP2102's UART runs from its own 3.0–3.6 V regulator, so **TXD cannot exceed 3.6 V**: safe to wire straight to an S3 GPIO | datasheet (CP2102 Rev 1.2) |
| speed | 921600 baud works (loopback, 4,096 random bytes exact); rated to 1 Mbit/s | measured + seller |
| its 3V3 pin | under 40 mA: **cannot power a board**. Do not connect 3V3 or 5V to a board that has its own USB | seller |
| wiring to a board | three wires: adapter TXD → board RX pin, adapter RXD → board TX pin, GND → GND | measured working |
| **rule learned the hard way** | **never leave the adapter wired to a board while the adapter itself is unplugged**: the board back-powers it through the signal lines (its red LED lights) | measured |

---

## 6. What is NOT known about this hardware

- Current consumption of the boards (no meter).
- Which DevKitC-1 revision the boards are (v1.0 or v1.1), and so which GPIO the LED is on.
- Wi-Fi throughput, outdoor range, any channel but 1, long-range mode.
- Anything about the 8 MB PSRAM in use (Potluck leaves it disabled).
- Native USB (the second socket) in use: never exercised, because Potluck disables its PHY.
- CAN with more than two nodes, longer cables, or rates other than 500 kbit/s.
- An external recording (scope or logic analyzer) of any signal on this bench.

Where a figure here matters to a design, re-measure it on the target rather than inheriting it: these
numbers belong to one house, one PC, one toolchain and these three boards.
