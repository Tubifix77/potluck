# M6.1 step 0 rerun -- the hotspot alone, on poor-mans-extender's aligned config (2026-10-08)

Board B (COM4) on poor-mans-extender `f1624de` firmware, built from that repo unchanged and flashed with esptool
`@flash_args` over Potluck's extender build. The pme baseline sdkconfig now mirrors
`firmware/sdkconfig.defaults.variant-extender`, including dynamic TX buffers. The credentials carried over in NVS. B is
not a cell member during this run. `m61-step0b-hs.jsonl` holds only the `{"t":"hs"}` status lines (via
`tools/hs_capture.py`), so it contains no names or passwords.

Joined at boot: station associated with an address, channel 1, RSSI -50 dBm, reconnects 0.

## fast.com, owner's phone on the hotspot, about 09:55

| run | latency unloaded / loaded | upload | download |
|---|---|---|---|
| 1 | 11 / 33 ms | 17 Mbps | 16 Mbps |
| 2 | 7 / 26 ms | 16 Mbps | 18 Mbps |
| 3 | 8 / 29 ms | 16 Mbps | 18 Mbps |

## 10-minute video stream, started 09:59:45 (up_s ~865 to ~1465)

60 status lines in the window: no reboot, associated and addressed throughout, channel 1, RSSI -54..-43 dBm,
one client throughout, reconnects 0, lowest internal free 198,179 B. 153 lines in all (up_s 10 to 1631);
no line names a network.

## Against the earlier runs

| | step 0 (old baseline, static TX) | step 0 rerun (aligned, dynamic TX) | step 2 (hotspot + Potluck) |
|---|---|---|---|
| download / upload | 18-19 / 14-15 Mbps | 16-18 / 16-17 Mbps | 18-19 / 14-17 Mbps |
| latency unloaded / loaded | 8-12 / 43-47 ms | 7-11 / 26-33 ms | 10-12 / 20-27 ms |
| 10-minute stream | clean | clean | clean |

The loaded-latency gain step 2 showed is mostly the buffer setting, now in the baseline too. Against the
aligned baseline, Potluck on the same chip costs no throughput and no loaded latency that three runs each
can resolve.

Afterwards B was flashed back to Potluck's extender build (`c87c9e1`, the size files uncommitted, hence
`-dirty`), verify-flash matched; the cell re-formed on channel 1 within 43 s of boot, hotspot up again.
