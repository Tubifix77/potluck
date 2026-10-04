# Distance sweep 2026-10-04 — field notes

Recording: `soak-sweep-2026-10-04-COM3.jsonl` (A, 0x6300) and `-COM5.jsonl` (C, 0x8160), started 13:38:18.
Walker: B (0x7368), firmware `a652cab` on all three. House ~10 x 20 m, single floor; PC on the right
wall ~3.5 m from the top (owner's floor plan).

**Fixed boards:** A and C beside the PC, unmoved for the whole sweep. **A faces toward B's spots; C
faces the wall/PC** (owner, 13:57). That orientation difference is the likely reason C barely saw the
body test that A saw.

| B boot epoch | what it was | notes |
|---|---|---|
| 7 | on the PC (COM4), before the walk, 13:38–13:45 | free desk reference, −19 dBm |
| 8 | accidental short visit to plan spot 2 (top-left corner), 13:46 | 40 s, −50 dBm |
| 9–15 | plan spot 1 on a **power bank** | B rebooted every 20–30 s from 13:49: the power bank was unstable. Not radio. Exclude from link figures |
| 16 | plan spot 1 on a wall plug, **2.5 m from A and C, clear line**, from 13:51:57 | 1 min clear, then **owner stood between B and the PC for ~60 s, ending about 13:56**, then 1 min clear. A saw −44/−48 → −52/−53 dBm from 13:55:24; C no clear change |
| 17 | plan spot 2, top-left corner of the big room, clear line, from 13:59:32 | 2 min. A −44/−50 dBm, C −40/−45; heartbeat loss ~1.8 % (A) |
| 18 | plan spot 3, inner room, from 14:28:26. **Owner's map correction: a wall divides the inner room, so this spot is 2 walls from the PC, not 1** | 2 min. A −59/−65 dBm, C −50/−58; heartbeat loss ~0.4 % (A) |
| 19 | plan spot 4, open area below the inner room, from 14:37:33 | **~9.5 min (edge rule).** A −68/−80 dBm (median −74), heartbeat loss 3.9 %, worst 10 s 14.4 %; C median −69, 1.4 %. Unicast 830/830 and 845/845 ACKed. Never dead |
| 20 | 10 s near the PC (−21 dBm) at 16:29:56, then replugged | handling, not a spot |
| 21 | plan spot 5, left side of the big room, from 16:30:16 | **113 min** (a long dwell, not planned). A median −56 dBm (−52/−69), heartbeat loss 0.79 %, worst 10 s 12.7 %; C 0.99 %. Unicast 10,103/10,103 and 10,079/10,079 ACKed. Never dead |
| 22 | plan spot 6, bottom-right room, from 18:31:56. **Behind a tumble dryer, 3 walls, the fridge possibly on the straight line to the PC** (owner) | **10 min (edge rule).** A median −77 dBm, heartbeat loss 2.4 %, unicast 896/896. **C median −80 dBm, heartbeat loss 7.9 %, worst 10 s 44.7 %, unicast 846/856 — the first un-ACKed unicast of the sweep.** Never dead |
| 23 | **upstairs, directly above spot 6**, from 18:46:44: ~3.5 m up through the floor, clear of the dryer and fridge (owner's suggestion) | 10 min. A median −78 dBm, heartbeat loss 3.0 %, worst 10 s 30.8 %, unicast 904/905. C median −73 dBm, 2.4 %, worst 10 s 69.9 %, unicast 892/895. **First deaths of the sweep, all inside the first ~11 s after B booted: A declared B dead and revived it 3 times, C 4 times; none after.** Every one was a *revival*, not a reboot, so §8.2 told them apart correctly on a real link. Cause of the early cluster not established — the owner placing B and walking away is plausible |

Body-test minute at epoch 16 was in Potluck's own plan (step 1b, "does a person in the path break
the link"); framing it as passive-sensor evidence was a mix-up, corrected with the owner. Answer for
Potluck: no, ~6 dB and the link stayed green.

From spot 2 on, every spot is at the wall socket nearest the planned spot; distances are the owner's
estimates of where B actually sat.

**Geometry from the owner's corrected floor plan (2026-10-04 14:50).** Scale: house width = 10 m,
37.5 px/m on the drawing. PC on the right wall, ~3.4 m from the top. The inner room has a dividing
wall across its middle. Straight-line distance and walls crossed, A/C to B:

| spot (epoch) | position | distance | walls on the straight line |
|---|---|---|---|
| 1 (16) | open room near the PC | 2.5 m (owner) | 0 |
| 2 (17) | top-left corner of the big room | ~7.9 m | 0 |
| 3 (18) | inner room, below the dividing wall | ~5.0 m | 2 (inner room's top wall + dividing wall) |
| 4 (19) | open area below the inner room | ~7.8 m | 3 (top wall + dividing wall + bottom wall) |
| 5 (21) | left side of the big room, mid-height | ~9.9 m | 2 (clips the inner room's top-left corner: top wall, left wall) |
| 6 (22) | bottom-right room, behind a tumble dryer | ~12.8 m | 3, plus the dryer and possibly the fridge |
| 6-up (23) | upstairs, directly above spot 6 | ~13.3 m | the floor, plus the walls on the line |

Outdoor spots dropped: no power outside the house (the power bank was the unstable one).
