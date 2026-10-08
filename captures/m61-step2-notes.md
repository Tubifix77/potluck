# M6.1 step 2 -- the combined node: board B = Potluck member + router station + hotspot (+ BUSY, RELAY)

Firmware: B on the extender build of `28eeb1a` (dynamic TX buffers, AMPDU on -- M0-LOG session 28); A, C on
`db91dc6`. Package `m8-svc` (counter 6) on all three. Credentials set by the owner with
`tools/pme_set_wifi.py --potluck` (four-line file). B: running 1, sta 1, has_ip 1, channel 1, RSSI -51..-53 dBm.

## Speed tests (fast.com, owner's phone on the hotspot, three in a row, 2026-10-08 07:38)

| run | download | upload | latency unloaded | latency loaded |
|---|---|---|---|---|
| 1 | 18 Mbps | 17 Mbps | 12 ms | 20 ms |
| 2 | 19 Mbps | 14 Mbps | 10 ms | 21 ms |
| 3 | 19 Mbps | 16 Mbps | 11 ms | 27 ms |

Step 0 (hotspot alone, `m61-step0-notes.md`): 18-19 down, 14-15 up, 8-12 / 43-47 ms.

## 10-minute video stream, started 07:38:40

57 status lines in the window: no reboot, associated and addressed throughout, channel 1, RSSI -53..-46 dBm,
one client throughout, reconnects 0, lowest internal free 168,371 B. Potluck meanwhile: A and C declared
B dead **0 times** over the whole 14-minute session. B's own ESP-NOW sends met a full TX buffer pool
(`ESP_ERR_ESPNOW_NO_MEM`) 202 times in 14,849 (1.4 %), in bursts of 0.1-10 s that cleared on their own,
mostly during the speed tests. Heartbeat delivery on B's links 98.4-99.4 %; RTT p50 4-6 ms, p99 22-60 ms
(`m61-step2-report.txt`).

## The house mesh itself (owner, fast.com, phone on the Google Wifi directly)

640 Mbps down, 240 up, latency 10 / 44 ms. The extender's 18-19 / 14-17 is the S3, not the house.

## Router channel change

Run 1 (08:14, firmware `28eeb1a`): the router restarted on channel 11 (was 1). B went with it and announced
it on 11; **A and C stayed on 1** -- they still heard each other, and CR-1's scan starts only when every peer
is lost. Fail. Fixed (`3d2333a`, channel-authority sweep) and the cell's channel made to survive a reboot
(`7ec8229`).

Run 2 (08:36, firmware `7ec8229`): the router again came back on channel 11. B re-associated and announced
at 08:36:20; A and C each lost B, swept, and found it on 11 **about 2.7 s later**; all three alive on 11.
While the router was down, B's station hunted across channels and the others chased it (4-5 sweeps each);
B read dead meanwhile, which is what the acceptance line asks for.
