# Changelog

Potluck's releases. The full record -- every milestone with its measurements and caveats -- is
[STATUS.md](STATUS.md); the design and its decisions are [ARCHITECTURE.md](ARCHITECTURE.md).

## 1.0.0 -- 2026-10-16

The S3 Edition: ESP32-S3 boards (16 MB flash, 8 MB PSRAM) working as one cell, with a PC as an optional member.
Host tools tested on Windows 11.

- **Documentation for people new to Potluck.** Every manual checked against the code and rewritten for readers who
  were not there: the getting-started guide, the app developer's manual, the hub's API, integrations, Home Assistant.
  Personal and bench-specific details removed; one current figure where numbers disagreed.
- **The store's signed list**: [Tubifix77/potluck-store](https://github.com/Tubifix77/potluck-store). The dashboard
  shows it only when the store key's signature over it verifies; its first app is `overheat_alarm`.
- **esptool runs as a program of its own** beside the Apache-2.0 tools (it is GPLv2+); the Windows package carries
  Potluck's LICENSE and NOTICE and every bundled component's licence text in `licenses/`.
- **A `potluck` command** after `pip install -e host/potluck` (`potluck apply`, `potluck hub`, ...), as in the
  Windows package.
- Fixes: with TLS (`present.tls_ca`) the boards connect to port 8883 by default, as the hub does; the build scripts
  find ESP-IDF through `IDF_PATH`; the packaged hub reports the commit it was built from; the mock hub sends board
  events as a real hub does (`board_` + kind).
- Corrected: the board console runs at 115200 baud (the configuration had claimed 921600; the image never did).

## 0.2.0 -- 2026-10-16

The first tagged release. M0-M19 accepted; M20 (Potluck for everyone) built: the setup wizard with a step for cells
of cells, the library, the floor plan, app panels, the store (off until the owner turns it on), the release firmware
carried by the Windows package, and a tray icon that keeps the hub running. A board whose task hogs a core is reset
by the task watchdog, and each board reports why it last started (`node-<id>/sys/reset`).

Known limitation: on a busy 2.4 GHz band a board's radio can stop accepting sends; it restarts itself after 60 s and
the cell carries its jobs meanwhile.

Earlier tags: `first-silicon` (2026-10-01, the firmware's first run on a real ESP32-S3) and `deploy-and-detach`
(2026-10-04, M0-M3 accepted on hardware).
