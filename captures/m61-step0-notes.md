# M6.1 step 0 -- hotspot baseline, board B running pme_hotspot alone (no Potluck)

Firmware: poor-mans-extender `eaf29d6`, built with ESP-IDF v6.0.2, 824,272 B, 0 warnings.
Station joined the house Wi-Fi on channel 1, RSSI -49..-51 dBm, 0 reconnects (status lines in
`m61-step0-hs.jsonl`; setup attempts with a mistyped password in `m61-step0-hs-setup-attempts.jsonl`,
reason 15 = 4-way handshake timeout). Phone joined the hotspot; cnn.com loaded through it.

## Speed tests (fast.com on the owner's phone, three in a row, 2026-10-08 01:47)

| run | download ("internet speed") | upload | latency unloaded | latency loaded |
|---|---|---|---|---|
| 1 | 18 Mbps | 14 Mbps | 8 ms | 43 ms |
| 2 | 18 Mbps | 15 Mbps | 12 ms | 47 ms |
| 3 | 19 Mbps | 15 Mbps | 12 ms | 47 ms |

## Setup (to repeat exactly in step 2)

- board position and orientation: (owner to give)
- phone distance from B: (owner to give)
- house access point B joined: (owner to give, if the Home app shows it)

## 10-minute video stream (YouTube on the phone, through the hotspot), started 01:47:41

From the 62 status lines in the window (`m61-step0-hs.jsonl`, between the two `mark` records):
no reboot (`up_s` 370 -> 980 continuous); station associated and addressed throughout (`sta` 1,
`has_ip` 1); channel 1 throughout; RSSI -50..-47 dBm; one client throughout; reconnects 0 -> 0;
no `router lost` event; lowest `int_free` 242,507 B; lowest `psram_free` 8,212,624 B.
Whole run (96 lines, 16 min): the same, RSSI -52..-47 dBm.

**Accept met:** `has_ip` 1; the phone reached the internet through the hotspot; the 10-minute stream
ran with no reboot. **Kill not fired.**

Credentials check: this capture keeps only `{"t":"hs"}` lines, the reason number of `router lost`
warnings, and marks; every other console line was dropped unread (`tools/hs_capture.py`).
