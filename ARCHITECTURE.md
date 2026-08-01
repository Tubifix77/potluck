# Potluck — Architecture

**A distributed runtime that makes a cluster of microcontrollers behave like one machine.**

| | |
|---|---|
| **Author** | Tue Wincentz Boas |
| **Status** | Decision-closed v1 architecture, amended 2026-08-01 after adversarial review (second-analysis.md). Supersedes Vision Documents 1, 2 and 3. |
| **Document date** | 2026-08-01 |
| **v1 target hardware** | ESP32 family via ESP-IDF (Xtensa + RISC-V), plus one host machine |
| **Document type** | Architecture description with ADRs, wire spec, threat model, and falsifiable milestones |

---

## 0. How to read this document, and what changed

The three vision documents were deliberately written as *open decision spaces* — every hard question was presented as Option A / Option B / OPEN, so the documents could be fed to a model and expanded. That format did its job: the concept space is now well mapped. But it has a structural problem. **A document whose format is "here are the options" produces more options every time you feed it to anything.** The output of that loop is breadth. What this project needs next is depth: one set of decisions, made, with reasons, that two physical boards can be built against.

So this document does the opposite. Every decision space is **closed**. Each closure is recorded as an ADR with a stated rationale and an explicit **revisit trigger** — the observation that would justify reopening it. Nothing is left open to be helpful.

Four substantive things changed in the merge, and they are changes, not restatements:

1. **Compute pooling is re-grounded, not deleted.** Load-driven MCU↔MCU work-stealing is cut from v1 — §2 shows the arithmetic — but the vision's Kubernetes property survives as deploy-time placement plus failure-driven reconciliation (§7.7): every declared job runs somewhere, never twice. The product is the namespace, deploy-and-detach, and a mesh that keeps its declared state true.
2. **Location transparency is bounded by a Locality Contract** (§4). Transparent naming, *explicit* cost. This is the load-bearing new idea in the document.
3. **The latency numbers in the vision documents were optimistic** and several architectural conclusions rested on them. §3 replaces them with measured, cited figures, and the corrected numbers invalidate the original heartbeat design (§8) and the original wireless real-time claims (§4).
4. **Security exists.** It was absent from all three source documents. §9 is a threat model, not a paragraph.

Numbers in this document are cited to a source retrieved on 2026-08-01. Numbers that are *design budgets* rather than measured facts are labelled as such. Where a number should be measured on your own bench before it is trusted, it is marked **[MEASURE]** and appears as a milestone deliverable.

### 0.1 Vision invariant and known deviations

The owner's invariant, restated 2026-08-01: **a hardware- and domain-agnostic "Kubernetes for CPU/GPU/RAM *and* attached hardware", spanning ESP32-class nodes and larger machines** — motivated by stranded compute (chips far too powerful for their local job, yet required at that physical spot by their pins), and concretised as the car and the home in §1.2.

A corollary the owner has made explicit, binding every section below — **no second-class CPUs**: the system is built for uses nobody has foreseen, so hyper-flexibility is the requirement itself. Relative performance is an optimisation input for placement, never an exclusion rule. That a Pi out-computes the fleet is beside the point; the fleet's idle cycles are capacity, everything can be used anywhere as a coherent whole, and no CPU is ever ruled out because a faster one exists elsewhere (§7.8).

Engineering decisions in this document are made autonomously under that invariant; only *deviations from it* are escalated. Two knowing narrowings exist, disclosed here:

1. **ESP32-first is sequencing, not deviation (ADR-001).** Agnosticism is a property of the seams — the VHAL (§7.0 L2), the platform-neutral wire format (§5), the cross-platform host daemons (§7.1) — and a seam is proven by the *second* platform, cheaply, once the first works. v1 builds the seams on one family; v2's first STM32 or RP2040 port is the proof, not the promise. If sequencing itself is unacceptable, this is the line to object to.
2. **Work-stealing is reinterpreted, not deleted (§2, §7.7).** Kubernetes does not steal work between live nodes; it places workloads declaratively and re-places them when nodes fail. Potluck does the same. What is cut is only the load-driven variant the measured numbers kill. GPU/NPU capacity participates through host named services (§7.5), exactly as Doc 2's `cuda: true` capability advertisement intended.

Domain agnosticism is *enforced in the mechanisms*: wherever behaviour could legitimately differ by domain — staleness handling (§4), host-loss response (§8.3) — Potluck ships the mechanism menu and the deploy manifest picks the policy. The OS never assumes a robot or a building.

---

## 1. What Potluck is

Start from the observation the whole project rests on: an ESP32 is absurdly oversized for the job it usually gets. Two cores and half a megabyte of SRAM sit in a flowerpot, waking occasionally to check whether the soil is dry and ping a server. But it has to be *that* chip and it has to be *there* — it is cheap, and its pins are wired to the sensor, the valve, the lamp, the camera. The compute is stranded by physics, not by need. A house or a machine full of these is a cluster nobody is using.

Potluck exists to un-strand it. It is a middleware layer that presents every chip on the network — plus optionally one or more host machines — as **one machine**: compute, memory, screens, sensors and actuators in a single addressable system. An application refers to `potluck://house/vents/kitchen/temp`, not to "ADC channel 1 on the board behind the utility cupboard" — and it is written against the cluster, never against a board (§1.2, §7.4).

Three lineages, and precisely what is taken from each:

**Plan 9 from Bell Labs** — "Resources are named and accessed like files in a hierarchical file system", with "a standard protocol, called 9P, for accessing these resources", and each context getting "a separate name space" ([Pike, Presotto, Dorward, Flandrena, Thompson, Trickey & Winterbottom, *Plan 9 from Bell Labs*](https://9p.io/sys/doc/9.html)). Potluck takes the uniform namespace and per-node bind tables. It does **not** take 9P itself — 9P's message set assumes a stream transport, and the wire budget here is 250 bytes on a lossy radio (§5).

**Erlang/OTP** — isolated processes, message passing, supervision, let-it-crash. Potluck takes the *supervision and isolation model*. It does not take the BEAM (see §11 — something already implements the BEAM on ESP32, and it is not this project).

**Kubernetes** — declarative desired state, a control plane that reconciles toward it, and workload placement. Potluck takes declarative placement and reconciliation. It does **not** take the scheduler: k8s reschedules freely because moving a pod is cheap and pods are not wired to a specific piece of copper. Here they are.

### 1.1 The three things Potluck actually sells

1. **A location-transparent, typed, timestamped peripheral namespace.** Any node can read any peripheral by logical name and gets back a value *with its age and its latency class* — never a bare number.
2. **Deploy-and-detach.** Develop tethered to a host with full observability; deploy; unplug; the mesh runs the same code, autonomously, from flash, indefinitely.
3. **Heterogeneity.** An ESP32 and a workstation are the same kind of thing to the runtime — differing in capability, not in kind.

### 1.2 What this looks like

Two systems the owner has named; they are the canonical pictures for every abstraction below.

**The car.** Ten ESP32s, a Linux Pi, twenty lights, front and rear sensors, a dash screen, a central tablet. Potluck on every one of them; they cluster over wired CAN (§5.3.1) with the Pi as mega-node (§7.1). The product ships as **one application package for the whole car** (§7.4): lamp actors pin themselves to the nodes wired to the lamps — their L0/L1 bindings leave no choice — while fusion, trip logic and UI actors declare constraints and let the build tool place them, which lands the heavy ones on the Pi. From anywhere in the car, `potluck://car/lights/rear/left` *is* the lamp. If the node behind the rear bumper dies, its portable actors re-activate elsewhere within seconds (§7.7); the lamp itself is gone until the node returns — physics — and every read of it says so, loudly (§4).

**The home.** A watering node in the garden, moisture nodes inside, a voice box, a screen by the door, a PC that is sometimes on. One package for the house: the watering rule is pinned to the valve it owns and keeps working with the PC off (Mode C, §8.1); the anomaly detector is portable and lands wherever headroom is; the voice box pins its wake-word actor to its own mic and ships utterances to `potluck://home/svc/speech-to-text` (§7.5) when the PC is up, degrading to canned local intents when it isn't — L4 by construction, so nothing real-time ever depended on it. The Star Trek ship computer, minus the pretense: you address the *house*, and no application ever names a board.

Two feasibility notes, so the pictures stay honest. Potluck pools memory the way Kubernetes does — as a **placement resource** (an actor goes where the RAM is), never as remote swap. And screens join the namespace at *state* level, not pixel level: one 320×240×16 bpp frame is ~150 KB — over twice the §6 core budget — so the render actor pins to the screen's node and subscribes to small typed state, which is the right design anyway. v1's wireless scope is one ESP-NOW cell of ~20 nodes (§4): the car fits with room to spare; the fully IoT'ed home is why the wire format is transport-agnostic and why a routed profile is named as the post-v1 growth path (§5.3).

**The test fleet.** The owner supplies example systems as architecture tests, under a standing rule that is now doctrine: *if a named setup is not possible, the architecture is wrong.* Four are on file beyond the car and the home:

| System | What it stresses | Verdict under this architecture |
|---|---|---|
| **Ironman suit** | dense actuation around a human; onboard host; HUD; voice | Possible as a fabric: loops tighter than L1 pin to the node wired to actuator+IMU, setpoints distribute over the wired CAN spine (L2), the suit's Pi-class node is a Mode B host that flies with the mesh (§8.1), HUD renders at state level (§7.2), the JARVIS role is `svc/*` (§7.5) with `on_host_loss` declared per actor (§8.3). Cross-node sub-millisecond loops remain impossible — physics, §3.1 — and pinning is the answer, as in every real machine. Non-negotiable: §12. A suit is *entirely* motors wrapped around a person; Potluck is never the only thing between them. |
| **Environmental monitoring, daily LLM mail to admin** | Mode C longevity; outbound cloud | Already explicit in §8.4: on-chip summarisation plus outbound-only direct-to-cloud reporting, TLS via the platform's mbedTLS (§9.3). No host, no inbound listener, runs from flash indefinitely. |
| **ESP32 supercomputer** — a Pi owns the only screen, keyboard and mouse | pure compute placement; human I/O as cluster resources | Possible: one application package, N portable L4 worker actors spread across the fleet at `background` priority (§7.8), a coordinator on the Pi scattering work via `CALL` and gathering `REPLY`s. The Pi's screen/keyboard/mouse are namespace entries like any pins (§7.2) — this test exposed and forced the `event` resource kind. That the Pi alone out-computes the fleet is beside the point (§0.1: no second-class CPUs): this is the canonical form of the background-compute pattern — latency-indifferent work parallelised across cycles nobody else wants — and the best conceivable M6 demo: kill a worker mid-job and watch the reconciler re-place it. |
| **Combine harvester** | EMI, vibration, detached field operation, operator cab | The car vignette in a harsher suit: wired CAN backbone for EMI resilience (§3), cab Pi as onboard Mode B host (§8.1), cab display at state level, auto-steer actors declare `on_host_loss: controlled_stop` (§8.3), and §12 governs everything near the operator or the header. |

### 1.3 Non-goals for v1 (explicit)

- Not a hard real-time RTOS. FreeRTOS/ESP-IDF is that; Potluck sits above it.
- Not a safety system. See §12.
- Not a general compute grid. See §2.
- Not a new programming language, and not a new virtual machine.
- Not multi-vendor in v1. No STM32, no RP2040, no Zephyr port. See ADR-001.
- No consensus algorithm. See ADR-006.
- No cloud service, no fleet backend, no accounts.

---

## 2. The question that had to be answered first

The vision documents lead with compute pooling: work-stealing between microcontrollers. Everything downstream — the WASM runtime, the CRDT capability registry, the dynamic scheduler — exists to serve it. So it has to justify itself before it earns that much machinery.

For MCU→MCU work-stealing to pay, a task must be **simultaneously**: (a) too heavy for the local node, (b) small enough to ship over the link, (c) latency-tolerant enough to survive the round trip, and (d) not better served by the host mega-node. Here is what the measured numbers do to that set.

**Shipping the code.** ESP-NOW v2.0 raises the maximum payload to 1470 bytes (`ESP_NOW_MAX_DATA_LEN_V2`) from v1.0's 250 bytes (`ESP_NOW_MAX_DATA_LEN`) ([ESP-IDF ESP-NOW reference](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html)) — genuinely better than the source documents assumed. At the v2 profile's 1446-byte Potluck payload (§5.3) a 10 KB module is ⌈10240/1446⌉ = 8 frames, against ⌈10240/226⌉ = 46 at the v1 profile. But frames are not free: on an open-farmland line-of-sight link at 54 m, measured mean one-way delay was **2782.85 µs with σ = 108.72 µs** at PDR 1.0; at 52 m, PDR 99.85% with mean 3461.65 µs, σ 2079.06 µs and a **maximum of 25628 µs**; at 58 m, PDR 83.2%, mean 7851.15 µs, σ 8033.23 µs, **max 59192 µs** ([Becker, Oberli, Zobel, Steinmetz & Meuser, *ESP-NOW Performance in Outdoor Environments*, IEEE/IFIP WONS 2025](https://dl.ifip.org/db/conf/wons/wons2025/1571077625.pdf)). Eight frames on a good link is ~22–28 ms. On an 83%-PDR link it is a lottery with a tail in the hundreds of milliseconds.

**Running the code.** A receiving node needs a runtime. Measured on ESP32-C6 for a 100-integer bubble sort: wasm3 used **~156 KB** and WAMR **~480 KB** of memory, and wasm3 executed in 6,358 µs against native C's 577.5 µs — roughly **11× slower** ([*WebAssembly on Resource-Constrained IoT Devices*, arXiv 2512.00035, Figs. 3–5](https://arxiv.org/html/2512.00035v1)). The ESP32 has 520 KB of SRAM total, split 320 KB DRAM / 200 KB IRAM, and "the maximum statically allocated DRAM usage is 160 KB" ([ESP-IDF memory types](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/memory-types.html)).

**The conclusion follows mechanically.** A node with 156 KB free to host a stolen task is not a node with spare capacity you discovered — it is a node you deliberately over-provisioned. And when it runs the task it runs it ~11× slower than the node that was "too busy" would have run it natively. The transfer costs tens of milliseconds and can cost hundreds. Every task that survives (a) through (c) is a task the host — with orders of magnitude more RAM and an AOT-compiled runtime — serves *faster*, which is (d).

Read bar (d) precisely, because it is easy to over-read — this document initially did. It tests whether **runtime stealing machinery** earns its complexity; it is not a rule about where work may live. Per §0.1's *no second-class CPUs*: "the host would be faster" never disqualifies a node — latency-indifferent work is a first-class citizen on any CPU in the cluster, placed at background priority (§7.8). What is cut is the machinery that moves work between live nodes at runtime; the right to run work anywhere is untouched.

Two honest scopings before the decision. First, the numbers above price the *ship-code-at-steal-time* variant; actors pre-compiled into every node's image (Doc 1's own Option B) dodge them entirely — for those, moving work is a state handoff, not a code transfer. Second, bar (d) assumes a host, and Mode C — the flagship mode — has none. So the arithmetic kills dynamic code-shipping steals; it does not kill the vision's underlying demand, which the owner has made explicit: **every declared job must be running somewhere in the network, and running it twice is waste.**

That demand is not work-stealing. It is Kubernetes' actual property — declarative placement plus failure-driven reconciliation — and k8s itself never steals between live nodes. **Decision: load-driven MCU↔MCU work-stealing is cut; the demand is met by three mechanisms:**

- **Placement** — where an actor lives is decided at deploy time, declaratively, by manifest.
- **Reconciliation (§7.7)** — when a node dies, its *portable* actors are re-activated on surviving nodes from pre-provisioned flash. Code is distributed to every eligible node at deploy time; only *activation* moves at failure time — which is why the WASM arithmetic above never applies to it.
- **Migration (§7.4)** — planned, checkpointed movement for graceful events (hot-unplug of a robot module, battery load-shedding), initiated by manifest or by the reconciler.

Opportunistic offload to the host mega-node stays, but as **named services** (§7.5), not generic stealing.

> **Revisit trigger (ADR-005), restated so it can actually fire:** a profiled workload where moving work between two *live* MCUs beats both local execution and host offload. The interpretation penalty no longer appears in the trigger, because pre-provisioned native actors pay none.

---

## 3. Physical reality: what the hardware will and will not do

Everything in §4 onward is derived from this table. These are the constraints, cited.

| Property | Measured / specified value | Source |
|---|---|---|
| ESP32 SRAM | 520 KB total = 320 KB DRAM + 200 KB IRAM | [ESP-IDF memory types](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/memory-types.html) |
| ESP32 static DRAM ceiling | 160 KB static max; remaining 160 KB heap-only at runtime | [ESP-IDF memory types](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/memory-types.html) |
| DRAM lost to Bluetooth | −64 KB if the BT stack is used (−16–32 KB for trace memory) | [ESP-IDF memory types](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/memory-types.html) |
| ESP-NOW payload, v1.0 / v2.0 | 250 B / 1470 B | [ESP-IDF ESP-NOW](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html) |
| ESP-NOW peers | 20 total; encrypted peers ≤ 17, default 7 | [ESP-IDF ESP-NOW](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html) |
| ESP-NOW crypto | CCMP on the action frame; PMK and LMK both 16 B; **encrypted multicast not supported** | [ESP-IDF ESP-NOW](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html) |
| ESP-NOW channel | "The channel must be set as the channel that the local device is on" | [ESP-IDF ESP-NOW](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html) |
| ESP-NOW best-case one-way delay | 2782.85 µs mean, σ 108.72 µs, PDR 1.0 (54 m LOS, 250 B, 802.11b/g @ 1 Mbit/s) | [WONS 2025](https://dl.ifip.org/db/conf/wons/wons2025/1571077625.pdf) |
| ESP-NOW delay tail | up to 25.6 ms at 99.85% PDR; up to 59.2 ms at 83.2% PDR | [WONS 2025](https://dl.ifip.org/db/conf/wons/wons2025/1571077625.pdf) |
| ESP-NOW retry behaviour | slotted p-persistent access; modelled init tx 2800 µs, retx 3350 µs, slot 481 µs, **retransmission limit 31** | [WONS 2025](https://dl.ifip.org/db/conf/wons/wons2025/1571077625.pdf) |
| ESP-NOW range cliff | PDR >99% below 56 m, **zero above 70 m**, and between 56–70 m it oscillates between 100% and 0 rather than degrading smoothly | [WONS 2025](https://dl.ifip.org/db/conf/wons/wons2025/1571077625.pdf) |
| ESP32 TWAI (CAN) | ISO 11898-1 frame structure, 11-bit and 29-bit IDs | [ESP-IDF TWAI](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/twai.html) |
| ESP32 TWAI — no CAN FD | "not compatible with FD format frames and will interpret such frames as errors" | [ESP-IDF TWAI](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/twai.html) |
| ESP32 TWAI transceiver | No internal transceiver; external one required (e.g. TJA105x for ISO 11898-2) | [ESP-IDF TWAI](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/twai.html) |
| CAN 2.0A frame length | 47–111 bits for 0–8 data bytes, excluding stuff bits; 47–111 µs at 1 Mbit/s | [Copperhill CAN tutorial](https://copperhilltech.com/blog/controller-area-network-can-bus-tutorial-message-frame-format/) — *vendor blog, single source; treat as indicative* |
| FreeRTOS tick (ESP-IDF) | `FREERTOS_HZ` **default 100**, range 1–1000 | [esp-idf/components/freertos/Kconfig](https://raw.githubusercontent.com/espressif/esp-idf/master/components/freertos/Kconfig) |
| NVS limits | key ≤ 15 chars; strings ≤ 4000 B; blobs ≤ 508,000 B or 97.6% of partition − 4000 B | [ESP-IDF NVS](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/nvs_flash.html) |
| NVS guidance | "NVS works best for storing many small values, rather than a few large values"; use FAT + wear levelling for large blobs | [ESP-IDF NVS](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/nvs_flash.html) |
| Secure Boot v2 | RSA-PSS (RSA-3072); SHA-256 digest of the public key in eFuse; on ESP32 **one** public key; verified on every boot **and on each OTA update** | [ESP-IDF Secure Boot v2](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/security/secure-boot-v2.html) |
| 802.15.4 / Thread chips | "chips with 15.4 radio such as ESP32-H2, ESP32-C6 and ESP32-C5" | [ESP-IDF OpenThread](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-guides/openthread.html) |
| Wasm3 minimum | "~64Kb for code and ~10Kb RAM" | [wasm3](https://github.com/wasm3/wasm3) |
| WAMR binary size | ~58.9 K fast interp / ~56.3 K classic interp / ~29.4 K AOT runtime | [WAMR](https://github.com/bytecodealliance/wasm-micro-runtime) |
| Measured Wasm on ESP32-C6 | wasm3 ~156 KB / WAMR ~480 KB memory; wasm3 ~11× slower than native C; wasm3 1.12 mJ vs native 0.1 mJ | [arXiv 2512.00035](https://arxiv.org/html/2512.00035v1) |

### 3.1 Three corrections to the vision documents

**Correction 1 — wireless cannot carry a sub-millisecond loop, and it is not close.** Vision Document 1 lists ESP-NOW at "~2–5 ms" and Document 3 assigns "<1ms response" safety loops to exosuits on a wireless mesh. The measured floor is 2.78 ms one-way on a *perfect* link, and a single frame may legitimately remain in the protocol's retry machinery for **31 × 3350 µs ≈ 104 ms** before it is abandoned ([WONS 2025](https://dl.ifip.org/db/conf/wons/wons2025/1571077625.pdf)). Separately, the local scheduler's default granularity is 10 ms, since `FREERTOS_HZ` defaults to 100 ([Kconfig](https://raw.githubusercontent.com/espressif/esp-idf/master/components/freertos/Kconfig)) — so even *on-chip*, a sub-millisecond deadline is an ISR or hardware-timer job, not a task job. This is the origin of the Locality Contract in §4.

**Correction 2 — "wired CAN, therefore <1 ms" does not hold for Potluck messages.** ESP32 TWAI cannot do CAN FD, so the payload is 8 bytes per frame. A Potluck message therefore needs a segmentation sublayer over CAN, and the "<1 ms" figure applies to one 8-byte frame, not one message. Arithmetic, using 7 payload bytes per frame after the 1-byte segmentation header (§5.3) and the 111-bit maximum frame length:

> 64-byte message (16-byte header + 48-byte payload) → ⌈64/7⌉ = **10 frames** → 10 × 111 = **1110 bits**
> at 500 kbit/s → **2.22 ms**; at 1 Mbit/s → **1.11 ms**
> — excluding stuff bits, arbitration delay and interframe spacing, so treat as a floor.

Wired is still an order of magnitude better than wireless in both latency *and* variance. It is not sub-millisecond for anything but the smallest messages.

**Correction 3 — the WASM overhead figure was off by roughly 4×.** Document 1 budgets "~30–60KB per runtime instance." Independent measurement puts wasm3 at ~156 KB and WAMR at ~480 KB on ESP32-C6 for a trivial workload ([arXiv 2512.00035](https://arxiv.org/html/2512.00035v1)); the vendors' own floors are "~64Kb code and ~10Kb RAM" for [wasm3](https://github.com/wasm3/wasm3) and ~56–59 KB of interpreter *binary* for [WAMR](https://github.com/bytecodealliance/wasm-micro-runtime). The vendor floors and the measured totals are not in conflict — the vendor numbers exclude the module's own linear memory and stacks, which is most of the cost — but the number that matters for provisioning is the measured one. Against a 160 KB static DRAM ceiling, WASM is not a per-node default. It is a tier (ADR-003).

---

## 4. The Locality Contract

*This is the central architectural idea in Potluck and the part that is genuinely new relative to its three ancestors.*

Plan 9 made remote resources look local. That was the right call on a wired LAN of workstations, where the cost of the lie was small and bounded. On a mesh whose delay distribution has a 59 ms tail and a PDR that falls off a cliff between 56 and 70 m, the same lie is a latent field failure: someone writes a 100 Hz control loop against `potluck://arm/elbow/angle`, it works on the bench with both boards on the same desk, and it fails in a building.

Kubernetes has affinity rules; Plan 9 has no notion of cost at all. Neither gives you what an embedded distributed system needs, which is:

> **Transparent naming. Explicit cost. Checked before it ships.**

Every resource, every actor, and every binding in Potluck carries a **latency class**. The class is part of the type. Binding across an incompatible class is a **build-time error**, not a runtime surprise.

| Class | Deadline budget (p99) | May cross | Implemented as | Example |
|---|---|---|---|---|
| **L0** | < 100 µs | nothing — same silicon | ISR / hardware timer / RMT / MCPWM | PWM commutation, quadrature decode, e-stop input |
| **L1** | < 10 ms | nothing — same node | FreeRTOS task on the owning node | PID loop, current limit, local sensor fusion |
| **L2** | < 20 ms | one wired hop (CAN / UART) | Potluck Frame over TWAI or serial | joint setpoint distribution, wired telemetry |
| **L3** | < 500 ms | one wireless hop (ESP-NOW) | Potluck Frame over ESP-NOW | room temperature, occupancy, vent position |
| **L4** | best effort, no deadline | anything, incl. host and internet | any transport, may queue | SLAM, model inference, logging, reporting |

Four rules, and they are the whole contract:

1. **An actor declares its class.** An L1 actor may only bind resources of class L1 or tighter, *and* only on its own node. The toolchain rejects the deploy manifest otherwise.
2. **A read never returns a bare value — and never a silent one.** Every read yields `(value, unit, timestamp, age, class, quality)`. Past the resource's staleness bound, `quality` becomes `STALE` and the value is *still delivered* — an estimator predicting through a dropout legitimately wants the last measurement and its exact age. Each resource declares a `staleness_policy`: **`informative`** (default: deliver, marked) or **`strict`** (deliver an error, no value — for feedback where consuming old data is worse than none). The one banned act is handing back old data *unmarked*: a location-transparent read that silently serves a 400 ms-old sensor value is how distributed control systems hurt people. Mechanism in the OS, policy in the manifest.
3. **Class is transport-derived, not aspirational.** The class ceiling of a binding is the worst class of any transport on its path, computed by the router from the actual topology, not declared by the author.
4. **Demotion is loud, and detected by cheap signals.** N consecutive deadline misses, transport retry exhaustion, or heartbeat loss on the binding's path put it into `DEGRADED` with a `CLASS_VIOLATION` event — signals the node already has, and fast enough for links whose PDR drops from 100% to zero in seconds (§3). Percentile auditing (observed p99 vs class) runs *host-side* in Modes A/B against the capture stream (§7.6), where histogram memory is free. A binding never quietly gets slower.

Two boundary conditions, stated so they stop hiding in the table. **v1 paths are single-hop**: rule 3's worst-transport arithmetic is only correct when a path has one transport — two L3 hops at <500 ms each compose to <1 s, which is not L3 — so forwarding is v2 work and requires budget-*summing*, not worst-of. With ESP-NOW's [20-peer ceiling](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html), single-hop means a v1 wireless cluster is one radio cell of ~20 nodes; that scope statement belongs in the open, not in a table cell. And **L1's 10 ms budget sits exactly on the default scheduler tick** — `FREERTOS_HZ` defaults to 100, i.e. 10 ms per tick ([Kconfig](https://raw.githubusercontent.com/espressif/esp-idf/master/components/freertos/Kconfig)) — zero margin, so the Potluck reference configuration mandates `FREERTOS_HZ=1000` (the Kconfig range allows it).

The payoff, stated precisely: class **ceilings** are static — a linter on your laptop proves a binding never crosses a hop type its class doesn't budget for. Class **satisfaction** is environmental — the same manifest can pass at 3 m and fail at 60 m across §3's PDR cliff — and that is what rule 4 watches at runtime. Build time checks topology; runtime checks reality; both are loud. The contract must not commit the lie it was built to kill. A compile-time error is still worth more than any amount of runtime cleverness — it just isn't the whole job.

**ADR-002 records this decision.** It also settles the source documents' Decision Area 3 on task scheduling (Doc 2): there is no scheduling heuristic, because the contract makes placement explicit.

---

## 5. The wire: Potluck Frame v1

One frame format. The **same bytes** on ESP-NOW, on CAN, on USB-serial, and on the host's local IPC socket. This is a deliberate, load-bearing simplification: the bridge daemon (§7.1) forwards frames, it does not translate them into a second schema. One format means one parser, one fuzz target, one capture format, and byte-identical replay from radio to dashboard.

### 5.1 Header (16 bytes, little-endian)

```
 offset  size  field       notes
 ------  ----  ----------  ---------------------------------------------------
   0      1    magic       0xD9
   1      1    ver_flags   [7:4] version = 1
                           [3]   FRAG   — this is a fragment
                           [2]   ACKREQ — sender wants an ACK
                           [1]   AUTH   — auth tag present (see 5.4)
                           [0]   reserved, must be 0
   2      2    src         node id (0x0000 = unprovisioned)
   4      2    dst         node id (0xFFFF = broadcast)
   6      1    opcode      see 5.2
   7      1    lclass_pri  [7:5] latency class L0..L4
                           [4:0] priority 0..31 (higher wins arbitration)
   8      2    seq         per (src,dst) monotonic, wraps
  10      2    msg_id      correlation id; reply carries the request's msg_id
  12      2    frag_off    byte offset of this fragment within the message
  14      2    total_len   total payload length across all fragments
 ------  ----  ----------  ---------------------------------------------------
  16      N    payload
  16+N    8    auth_tag    present iff AUTH set — truncated HMAC-SHA256 (5.4)
```

Fixed 16 bytes, no options, no TLVs in the header. Parsing is a struct cast on every target and there is no variable-length header arithmetic to get wrong.

### 5.2 Opcodes

| Range | Opcode | Meaning |
|---|---|---|
| Membership | `0x01 HELLO` | node announce: id, capabilities, epoch, pubkey fingerprint |
| | `0x02 HELLO_ACK` | admission decision |
| | `0x03 HEARTBEAT` | liveness + link stats (§8) |
| | `0x04 BYE` | intentional departure |
| Namespace | `0x10 READ` | path → typed value |
| | `0x11 WRITE` | path ← typed value |
| | `0x12 SUBSCRIBE` | path + min interval + max staleness |
| | `0x13 PUBLISH` | value push to subscribers |
| | `0x14 UNSUBSCRIBE` | |
| | `0x15 LIST` | enumerate a namespace subtree |
| Actors | `0x20 CALL` | request to a named actor |
| | `0x21 REPLY` | response, carries request `msg_id` |
| | `0x22 CAST` | fire and forget |
| Lifecycle | `0x30 DEPLOY_BEGIN` | manifest + signature + total size |
| | `0x31 DEPLOY_CHUNK` | payload chunk |
| | `0x32 DEPLOY_COMMIT` | activate slot |
| | `0x33 DEPLOY_ABORT` | |
| | `0x40 MIGRATE_PREPARE` | quiesce + checkpoint |
| | `0x41 MIGRATE_COMMIT` | ownership transfer |
| Safety | `0x50 SAFE_STATE` | broadcast, priority 31, unfragmented, always ACKREQ off |
| Errors | `0x7F ERR` | code + optional detail; never silently dropped |

`SAFE_STATE` must fit a single frame on every transport, because a safety broadcast that requires reassembly is not a safety broadcast.

> **This requirement was stated here but not met, and the failure is arithmetic.** §5.1 fixes the header at 16 bytes and §5.3.1 gives CAN 7 Potluck bytes per frame, so even a *zero-payload* Potluck Frame is ⌈16/7⌉ = **3 CAN frames**. A 64-byte `HEARTBEAT` is 10. As originally written, this section claimed something §5.1 and §5.3.1 together forbid, and §5.4's "no fragmentation for `SAFE_STATE`, `HEARTBEAT`, or `HELLO`" was unsatisfiable on CAN for every opcode. §5.3.1 now carries the resolution; the requirement above stands and the CAN profile changes to meet it.

### 5.3 Transport profiles

| Transport | Link MTU | Potluck payload/frame | Notes |
|---|---|---|---|
| ESP-NOW v2.0 | 1470 B | 1446 B | preferred; only usable at full MTU when every peer is v2.0 — a v1.0 receiver [truncates a longer v2.0 packet to its first 250 bytes or discards it](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html) |
| ESP-NOW v1.0 | 250 B | 226 B | compatibility floor; assume this for any node you did not build. A v1.0 receiver *does* accept a v2.0 sender's packets at or below 250 B — which is what makes the pinned profile below work |
| USB-serial / UART | — | 1446 B | COBS framing + CRC-16; same payload cap as ESP-NOW v2 so behaviour matches |
| CAN (TWAI, classic) | 8 B | 7 B | segmentation sublayer, §5.3.1 |
| Host IPC | — | 1446 B | length-prefixed frames over a named pipe (Windows) or Unix socket |

**Version negotiation matters, and the failure it prevents is worse than a dropped frame.** A v1.0 receiver accepts a v2.0 sender's packet only while it stays at or below 250 B; above that it will "either truncate the data to the first 250 bytes or discard the packet entirely" ([ESP-IDF ESP-NOW](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html)). Truncation is the dangerous branch: a silently shortened frame is a parse against attacker-shaped-by-accident bytes, not a loss the link stats would show. So `HELLO` carries the ESP-NOW version, the router pins a peer to the 226-byte profile unless *both* ends advertise v2.0, and the codec's `total_len` check (§5.4) rejects a truncated frame rather than parsing it. Do not discover this in the field.

One deliberate absence: a routed or infrastructure-Wi-Fi UDP profile — same Potluck Frame, no 20-peer ceiling — is **not** in v1. It is the named post-v1 path for the >20-node home (§1.2), and because it changes nothing above L1, deferring it costs the architecture nothing.

#### 5.3.1 CAN segmentation, and the single-frame profile

**Bulk messages segment.** One segmentation byte per frame: `[7] FIRST, [6] LAST, [5:0] index`, giving 7 Potluck bytes per CAN frame. The header occupies the first 16 payload bytes of the sequence, so a 16-byte control frame is 3 CAN frames and a 64-byte message is 10 (§3.1, Correction 2).

**Control messages must not**, and segmentation alone cannot give them what §5.2 and §5.4 require. Two things break at once:

- *Correctness.* A `SAFE_STATE` spread over 3 CAN frames is a safety broadcast that depends on reassembly, which §5.2 forbids for good reason.
- *Capacity.* §1.2's car is ten ESP32s and a Pi on a CAN spine, and §8.2's wired row is a 20 ms heartbeat. CAN is a broadcast bus, so one heartbeat per node per period suffices — but at 10 CAN frames each and §3.1's 1110 bits, eleven nodes need **24.4 ms of bus per 20 ms period at 500 kbit/s: 122%, before a single setpoint moves.** At 1 Mbit/s it is 61%, which leaves the actual control traffic fighting the liveness traffic. §1.2 says the car "fits with room to spare"; under segmentation-only it does not fit at all.

**Resolution: a single-frame CAN profile that carries the routing header in the 29-bit extended ID.** ESP32 TWAI supports 29-bit extended identifiers (§3), and this section already wanted the ID to encode priority. Carrying the rest of the routing fields there costs nothing extra and buys the single-frame property:

```
 CAN 29-bit extended ID              8 data bytes
 ------------------------------      -------------
 [28:24]  priority   (5)  §5.1 lclass_pri[4:0]; high bits, so CAN arbitration
                          *is* Potluck priority — SAFE_STATE wins the bus by construction
 [23:17]  opcode     (7)  §5.2 opcodes are all ≤ 0x7F
 [16:11]  src alias  (6)  segment-local node alias, assigned at enrolment
 [10:5]   dst alias  (6)  0x3F = broadcast
 [4]      SINGLE     (1)  1 = complete message, no segmentation sublayer
 [3:0]    index      (4)  segment index when SINGLE is 0
```

A 64-node alias space is ample for one CAN segment and for §1.2's eleven-node car. When `SINGLE` is set the 8 data bytes are the whole Potluck payload, so `SAFE_STATE`, a liveness beacon and a `HELLO` each occupy exactly one CAN frame — arbitrated by Potluck priority, never reassembled. Bulk traffic clears `SINGLE` and uses the segmentation sublayer above.

**This does not fork the wire format.** §5's promise is that the *Potluck Frame* is the same everywhere — one parser, one fuzz target, one capture format, byte-identical replay. The CAN profile was already an encoding of that frame rather than a copy of its bytes: the segmentation byte is not part of any Potluck Frame either. The extended ID is the same kind of encoding, and the decoder reconstructs a byte-identical 16-byte header, so everything above the transport is unchanged.

**Consequence for the beacon.** A single CAN frame carries 8 payload bytes, so a liveness beacon must fit 8 — which the current 48-byte `HEARTBEAT` does not. That payload conflates two things with different requirements: liveness (tiny, must never fragment, must fit every transport) and link statistics (fat, diagnostic, may fragment and may be slow). **M4 splits them**, and the split is the same correction §8.2 makes for the wireless case — a liveness signal is not a measurement channel. Not done at M0: ESP-NOW carries the 64-byte frame in one piece, and rewriting the wire format for a transport nobody has built yet is precisely the scope creep ADR-001 and §14 name as the standing risk. It is M4's first task, recorded here so M4 does not rediscover it.

### 5.4 Framing rules

- **Fragmentation is per-message, reassembly is per-`(src, msg_id)`**, with a reassembly timeout of 3× the class deadline and a hard cap of 4 concurrent reassemblies per peer (memory bound, §6). **L4 has no deadline** (§4), so "3× the class deadline" is undefined for exactly the class that fragments most — §1.2's supercomputer scatters large work units as L4 `CALL`s. Four lost fragments from one peer would then wedge that peer's four reassembly slots permanently, and a peer that can never again reassemble anything is a denial of service arrived at by arithmetic rather than by malice. **L4's reassembly timeout is therefore a fixed 30 s**, not a multiple of anything, and the cap stays at 4.
- **No fragmentation for `SAFE_STATE`, `HEARTBEAT`, or `HELLO`.** Liveness and safety must never depend on reassembly.
- **`auth_tag` is a truncated HMAC-SHA256 over header+payload** with the per-link session key, or an Ed25519 signature for the frames listed in §9.3. Truncation to 64 bits is a deliberate trade against a 226-byte MTU; §9.5 states what that does and does not buy.
- **ACK policy is per-class.** L2/L3 request frames set ACKREQ; PUBLISH does not (subscribers detect gaps by `seq`). Retries are bounded at 3 with jittered backoff, on top of ESP-NOW's own retransmission machinery — do not stack an unbounded retry loop on a protocol that already retries up to 31 times. **L4 sets ACKREQ too**, and the reason is worth stating because "best effort" invites the opposite conclusion: best-effort means *no deadline*, not *no delivery*. At §3's measured 83.2% PDR a work unit dropped without notification is 17% of a supercomputer's throughput lost silently, and the coordinator would have no way to tell a slow worker from a lost job. Loss must be *visible* to the application even where it is tolerable; §7.8's coordinator resubmits, which it can only do if it is told.

---

## 6. Node memory budget

The point of this section is that the budget exists *before* the code does. Numbers are **design allocations set by this document**, not measurements.

**The 64 KB cap is a Potluck design limit, not a silicon one**, and that distinction now matters because the two targets have different memory architectures. The cap applies to both; the headroom around it does not.

| | ESP32 classic | ESP32-S3 |
|---|---|---|
| Internal SRAM | 328 KB DRAM + 168 KB IRAM, **separate banks** | ~512 KB, **one unified pool** |
| Static DRAM ceiling | 160 KB, fixed | none fixed — IRAM and DRAM trade against each other |
| Bluetooth | BLE and Classic; disabling reclaims 64 KB | **BLE only**, so the 64 KB figure does not transfer |
| Reported by `idf.py size` as | `DRAM` | `DIRAM` |

Sources: [ESP-IDF memory types (ESP32)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/memory-types.html), [(ESP32-S3)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/memory-types.html), and the SoC headers in ESP-IDF v6.0.2. The S3 documentation is explicit that "any internal SRAM which is not used for Instruction RAM will be made available as DRAM" and that "the maximum statically allocated DRAM size is reduced by the IRAM size of the compiled application."

The table below is written against **ESP32 classic, Wi-Fi enabled, Bluetooth disabled** — the tighter of the two, so a design that fits it fits the S3 with room to spare. Where the two diverge in a way that changes a decision, the S3 figure is given alongside. The **[MEASURE]** item at the end of this section is per-target: the Wi-Fi stack's DRAM cost must be measured on whichever part is actually being shipped.

| Allocation | Budget | Basis |
|---|---|---|
| RX ring, 8 × 1470 B | 11.5 KB | one ESP-NOW v2 MTU per slot |
| TX ring, 4 × 1470 B | 5.7 KB | |
| Reassembly buffers, 4 × 1470 B | 5.7 KB | hard cap from §5.4 |
| Namespace table, 128 entries × 64 B | 8.0 KB | path hash, type, unit, class, staleness bound, cached value+ts |
| Peer table, 20 × 128 B | 2.5 KB | 20 is the [ESP-NOW peer ceiling](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html) |
| Actor control blocks + mailboxes, 16 × 512 B | 8.0 KB | |
| Crypto contexts + scratch | 8.0 KB | HMAC/Ed25519 verify state |
| Router + transport task stacks, 2 × 4 KB | 8.0 KB | |
| Counters, link stats, event ring | 2.0 KB | |
| **Potluck core total** | **59.4 KB** | |
| **Budget cap (hard)** | **64 KB** | build fails above this |

On **ESP32 classic**, against the 160 KB static DRAM ceiling, this leaves ~96 KB static for application code plus the ~160 KB runtime heap. Note what that makes obvious: a wasm3 instance measured at ~156 KB ([arXiv 2512.00035](https://arxiv.org/html/2512.00035v1)) does not coexist with the core and an application on a classic ESP32 in any comfortable way. Hence ADR-003.

On **ESP32-S3** the same 59.4 KB sits against a ~512 KB unified pool with no fixed static ceiling, so the headroom argument is much weaker — and on a module with PSRAM it is weaker still. **ADR-003's revisit trigger is therefore live but not fired**: WASM was rejected as a per-node default on a memory argument that the S3 partly dissolves. It is not reopened here, because M7 is gated on M0–M6 shipping (§13) and ADR-001 names scope creep as the standing fatal risk. It is reopened when someone has both a named workload requiring untrusted or hot-swappable code *and* a measured headroom figure from the part they are shipping.

**[MEASURE]** The DRAM actually consumed by the Wi-Fi stack with ESP-NOW active is not stated on the pages cited above and must be measured on your bench at M0, **on the part you are shipping** — the two targets do not share a memory architecture, so one figure does not stand in for the other. If it exceeds ~40 KB, the RX ring shrinks first. The M0 firmware measures this itself at every bring-up step and warns on the boot line when the threshold is crossed, so the trigger does not depend on anyone remembering to check.

---

## 7. Structure

### 7.0 Node layers

```
  L4  Application actors ..................... user code, declared class, no pin access
  L3  Potluck Core .............................. namespace, router, actor supervision,
                                             lifecycle, membership
  L2  Virtual HAL .......................... path → local driver call | remote Potluck Frame
  L1  Transport ............................ ESP-NOW | TWAI | UART, one Potluck Frame codec
  L0  ESP-IDF / FreeRTOS ................... drivers, ISRs, timers, NVS, LittleFS
```

Only L2 knows whether a resource is local. Only L1 knows which wire it is on. An actor knows neither — but its *class* (§4) knows what it is allowed to cost.

### 7.1 Host side: four daemons

The four-daemon split from Vision Document 2 is retained; it is sound Unix thinking and the bridge surviving an application crash is the right call. Three changes.

| Daemon | Role | Change from Doc 2 |
|---|---|---|
| **potluck-bridge** | Physical link ⇄ local frame socket. C++ or Rust. Never blocks on user code. Keeps the cluster connected while everything above it restarts. | **Forwards Potluck Frames unchanged.** No Protobuf translation at this layer — same bytes on both sides. Also **tees every frame to a rotating capture file** (§7.6). |
| **potluck-agent** | Host as a mega-node: advertises capability, hosts L4 services. | Advertises **named services**, not raw `cores:16, ram:32000`. See §7.5. What it donates is capped by a **local donation config owned by the machine's user** — how many cores, a RAM ceiling, GPU yes/no, which peripherals join the namespace, pause-on-battery — so the cluster takes what it is given and nothing more. |
| **potctl / pot-dashboard** | `kubectl` for the mesh: topology, live namespace, logs, deploy. | Speaks gRPC to `potluck-agent`, which is the *only* place a second schema exists. |
| **pot-app** | User logic in Python/Rust/C++. Consumes the namespace. | Unchanged in role. Gets the same typed/timestamped read contract as on-node actors. |

The chain is `pot-app | potluck-agent ⇄ potluck-bridge ⇄ wire`, with `potctl` hanging off `potluck-agent`. On Windows the local socket is a named pipe; on Linux/macOS a Unix domain socket. Nothing above `potluck-bridge` may open a serial port.

**On a PC, Potluck is deliberately not the operating system.** All four daemons are ordinary background services — systemd units on Linux, services on Windows — the machine's own OS stays in charge, and uninstalling them leaves the machine untouched. Even on the microcontrollers, Potluck rides on FreeRTOS (§7.0): the name is ambition; the implementation is a well-behaved guest everywhere. A cluster may contain several such machines — each is simply a node with more to donate, subject to its own donation config.

### 7.2 The namespace

```
potluck://<cluster>/<node-or-role>/<class>/<instance>[/<attribute>]

potluck://lab/node-07/adc/1                     physical, canonical
potluck://lab/knee-left/imu/0/accel             by role
potluck://lab/house/vents/kitchen/position      logical, via bind table
```

Canonical paths are physical. **Logical paths are bindings**, in the Plan 9 sense — a per-cluster bind table maps `/house/vents/kitchen` onto `node-07/pwm/2`, and rebinding it to a different node is a manifest change, not a code change. That is the property worth building the whole system for: the sensor moves, the application does not change.

Every entry carries: `kind` (`sampled | event`), `type`, `unit`, `access` (r/w/rw), `latency_class`, `staleness_bound_ms`, `owner_node`, plus the cached `value` and `timestamp`. Reads return the tuple from §4 rule 2.

The `kind` field exists because not every peripheral is a measurement — the supercomputer test (§1.2) forced this distinction. **Sampled** entries have last-value semantics: a cache, a staleness bound, `STALE` marking — the moisture sensor, the joint angle. **Event** entries have queue semantics: every publication is delivered to subscribers in order, staleness does not apply — two clicks are two clicks, not a newer value replacing an older one — and the per-subscriber queue is bounded with *loud* overflow (an `EVT_DROPPED` counter, never silent loss). The keyboard, the mouse, the wake-word hit. For `event` entries, `SUBSCRIBE`'s interval and staleness parameters are ignored. Conflating the two kinds is how a click gets eaten by a cache; the type system keeps them apart.

Displays are peripherals too, at state level rather than pixel level: a screen's entry exposes typed scene/widget state, its render actor is pinned to the owning node, and raw framebuffers stay off the wire (§1.2). And mega-nodes own entries exactly like MCU nodes — a Pi's screen, keyboard and mouse are ordinary resources (`potluck://fun/pi/kbd/events`, kind `event`); its "pins" are `/dev/input` and a framebuffer. Heterogeneity (§1.1) means this direction too: hosts donate peripherals, not just compute.

### 7.3 Membership and the capability registry

Gossip, no election, no consensus. Each node owns exactly its own record and stamps it `(node_id, epoch, seq)`; `epoch` increments on every boot, `seq` on every change. Merge is last-writer-wins on `(epoch, seq)` — a well-behaved CRDT register, arrived at without pulling in a CRDT library. `HELLO` at join and on epoch change; `HEARTBEAT` carries a digest so divergence is detected without shipping the full map.

Ownership of an actuator is **static, assigned in the deploy manifest**, not contended at runtime. This is why no consensus is needed (ADR-006).

### 7.4 Deploy and detach

The lifecycle from Vision Document 3 is retained. The storage design changes, because NVS is the wrong home for module images: blobs are capped at 508,000 B or 97.6% of the partition minus 4000 B, and the documentation is explicit that "NVS works best for storing many small values, rather than a few large values" and points at FAT with wear levelling for large blobs ([ESP-IDF NVS](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/nvs_flash.html)).

**Split the storage:**

- **NVS** — node identity, cluster public key, role, bind table, rollback counter, active slot pointer. Small values, exactly what NVS is for.
- **LittleFS partition** — module images and manifests, in **A/B slots**.

**The deploy unit is the cluster application package — one artifact per system, not per node.** "You make a singular software package for the whole car" is the vision sentence this section implements. The package holds every actor, binding, policy and asset for the system; developers declare *constraints* (this actor needs `/car/lights/rear/left`, which pins it; this one needs ≥40 KB headroom and class L4), and the build tool resolves placement — pinned actors to the nodes physics dictates, portable actors via the same deterministic assignment function the reconciler uses (§7.7) — then **freezes the resolution into the signed manifest**, so what-runs-where is inspectable and reproducible, never improvised at runtime. Hand-written node pins remain available as overrides, not as the norm. No application code ever names a node.

**Placement obeys data gravity: computation moves to the data, never the reverse, unless told otherwise.** Among nodes that satisfy an actor's hard constraints, the solver scores by (1) *co-location* — the node that owns the resources the actor binds most, when it has headroom; then (2) minimal path cost to those resources; and only then (3) rendezvous hash, as the deterministic tie-break. So the actor triggered by the soil sensor on node 8 lands *on node 8* by default — even when declared loose — and touches the radio never, not rarely. Kubernetes expresses the same two-tier idea as node affinity: a `required` rule "can't schedule the Pod unless the rule is met", a `preferred` rule is tried but yields ([k8s: assigning Pods to nodes](https://kubernetes.io/docs/concepts/scheduling-eviction/assign-pod-node/)). Potluck's split is the same, with one upgrade: the *required* tier isn't a label, it's the Locality Contract — declare the binding L1 and co-location is enforced by the type system (§4), not preferred by a scheduler. Whether the millisecond matters is therefore never the developer's problem: if it matters, declare it tight and it is *forced* local; if it doesn't, declare it loose and gravity still keeps it local while allowing the reconciler to move it when node 8 dies.

**Sequence:**

1. **Build.** Toolchain compiles the modules, runs the Locality Contract check (§4), resolves constraint-based placement, and emits the package manifest: module hashes, declared classes, resource bindings, frozen placement, min core version.
2. **Sign.** Manifest signed with the cluster deploy key. The signature covers the module hash, so the image cannot be swapped.
3. **Dispatch.** `DEPLOY_BEGIN` / `DEPLOY_CHUNK` / `DEPLOY_COMMIT` to the inactive slot. Each node verifies the signature **before** commit.
4. **Commit with a trial period.** New slot marked `PENDING`. The node reboots into it. It must send `HEARTBEAT` with `slot_ok` **N times** (default 10) before the slot is marked `CONFIRMED`. Otherwise the bootloader-level counter reverts it to the previous slot. This is the single feature that makes detaching a 30-node mesh survivable.
5. **Detach.** Host removed. Nodes cold-boot from the confirmed slot, autonomously, indefinitely.
6. **Rollback counter** in NVS is monotonic and refuses manifests with a lower counter — anti-downgrade (§9.3).

### 7.5 Host offload as named services

Not work-stealing. A host advertises capability by publishing an **actor path** into the namespace:

```
potluck://lab/svc/slam              L4, provided by potluck-agent on 'workstation'
potluck://lab/svc/pose-estimate     L4
potluck://home/svc/speech-to-text   L4 — the voice box's wake-word actor stays pinned
                                to its mic; only utterances travel (§1.2)
```

An MCU actor calls it exactly as it calls any other remote actor — `CALL` to a path — and must handle `ERR_UNAVAILABLE` because the service is on a machine that might be off. The class is L4 by definition: if it needed a deadline, it would not be on the other end of a USB cable. Simple, testable, and it degrades to "the feature is unavailable" rather than "the robot stops."

### 7.6 Record and replay

`potluck-bridge` tees every frame — in both directions, with a host timestamp — to a rotating capture file. `potctl replay <capture>` injects it into a bridge running against a simulator or a bench cluster.

This costs almost nothing and is the highest-leverage testing feature in the whole system, because the defining property of distributed embedded bugs is that they are not reproducible. Given the field data in §3 — a PDR that oscillates between 100% and 0 across a 14 m band — you *will* hit behaviour you cannot recreate by hand. Build this at M2, not "later."

**The line format.** One JSON object per line. The record type lives under the key **`rec`** — `frame`, `log`, or one of the statistics kinds (`boot`, `link`, `node`, `event`, `ns`) — alongside a `host_ts`. Everything else on the line is the record's own payload, written flat.

`rec`, and not `kind`, for a reason found the hard way. The envelope key was `kind` and the payload was merged in after it, so any record carrying its own field of that name silently overwrote the envelope and became unidentifiable. Two record types do exactly that: `ns` carries the resource kind (`sampled` / `event`) and `event` carries the event kind (`peer_dead` …). Every such line in every capture was corrupt, with nothing to indicate it — the loss only surfaced when M2's acceptance test was actually built and a capture full of `ns` records replayed to an empty namespace. Note that the node's console stream never had the problem, because there the envelope key is `t`; the collision was created purely by renaming `t` to something a payload might also use. The envelope is now written *after* the payload as well, so a future collision cannot win either.

**Acceptance is checkable, not eyeballed.** "Byte-identical namespace state" (§13-M2) means a canonical serialisation of every namespace entry's last known state — sorted, fixed field order, and containing nothing host-side, since a digest that moved when the host was busier would test nothing. `potluck-capture` and `potctl` both print the SHA-256 of what they observed, along with the command that replays the capture and requires the same digest:

```
python -m potluck --replay session.jsonl --expect-digest <sha256>
```

A mismatch exits non-zero. A capture is only evidence once something has confirmed it is sufficient to rebuild the state it claims to record.

### 7.7 The reconciler: failure-driven re-placement

The Kubernetes half of the vision, in one loop: the signed manifest is **desired state** (this set of actors, running, under these constraints); membership (§7.3, §8.2) is **observed state**; the reconciler closes the gap. No scheduler heuristics, no election, no consensus — ADR-006 stands.

- **Portability is derived, not declared.** An actor whose tightest binding is class L3 or looser (§4) is *portable* — nothing ties it to local silicon. An actor binding L0–L2 resources is pinned by physics. Actuator-*owning* actors are never portable regardless of class (ADR-006).
- **Code moves at deploy time, not at failure time.** M3 deploys ship every portable actor's module and configuration to *all* eligible nodes' flash. At failure time only activation moves — one decision, zero code transfer. This is Doc 1's Option B insight made load-bearing, and it is why §2's WASM arithmetic never applies here.
- **Assignment is a pure function.** Every node computes `assign(actor, eligible_live_nodes)` — constraint-filtered (capability, headroom, class compatibility), scored by data gravity (§7.4: co-locate with bound resources first, then minimal path cost), with rendezvous hashing as the deterministic tie-break — from its own membership view. On any membership change, each node re-evaluates: it activates portable actors newly assigned to it and quiesces ones no longer assigned. Identical inputs, identical answers, no coordinator.
- **Re-placement waits for declared death** (§8.2 timers), so a portable actor's availability gap is death-declaration plus activation — under a second on wireless. Portable actors are L3/L4 *by construction*, so the gap lives inside the class model: reads served by the actor go `STALE` loudly (§4) and recover.
- **Duplication is bounded, not impossible.** A partition can leave two nodes briefly believing they own the same portable actor; gossip convergence ends it, and consumers fence by accepting the highest `(epoch, assignment)` per actor. This is the price of refusing a consensus protocol, and it is why actuator ownership stays static: **at-least-once activation, exactly-once actuation.** "Never run the same job twice" holds in steady state and is violated only for the seconds a partition lies about who is alive — stated here so nobody discovers it in the field.
- **State:** v1 re-activation restarts from the actor's declared initial state, or its last checkpoint if one exists in the namespace. Exactly-once *execution* is not promised, and this document will not pretend otherwise.

### 7.8 Background compute: using the stranded cycles

The supercomputer test (§1.2) is not a toy case; it is the general pattern for the compute half of the vision. Much real-world computation is latency-indifferent — nightly aggregation, compression, indexing, batch analytics, Monte Carlo anything — and per §0.1 the fleet's idle cycles are capacity, not garnish.

- **Actors carry a `priority: critical | normal | background` manifest field.** ESP-IDF's FreeRTOS is a fixed-priority preemptive scheduler — "the scheduler executes the highest priority ready-state task", with the idle task at priority 0 ([ESP-IDF FreeRTOS](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/freertos_idf.html)) — so `background` maps just above idle, below every pinned duty on the node: it consumes exactly the cycles nothing else wants and preempts nothing.
- **The second core is real capacity.** ESP-IDF "provides a unique implementation of FreeRTOS with dual-core symmetric multiprocessing" for "dual-core ESP targets, such as ESP32", with per-core task pinning via `xTaskCreatePinnedToCore()` ([same source](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/freertos_idf.html)). A background worker can occupy the core the node's duty isn't using — the flowerpot node's second core was doing nothing at all.
- **Background frames ride low wire priority** (§5.1's 0..31 field, capped for background actors), so batch traffic loses every contention — on CAN by arbitration, by construction. The compute is opportunistic; the network never is.
- **The pattern is coordinator + workers**, and it changes nothing settled: worker placement is frozen at build like everything else (ADR-005's determinism intact); work units travel as ordinary `CALL`/`REPLY` messages at runtime, which is application messaging, not OS scheduling; code is already on every eligible node per §7.7's pre-provisioning.
- **Power is the one honest constraint.** Harvesting idle cycles denies sleep. Battery and solar nodes are excluded from background pools by default and opt in via manifest — the §1.2 home's garden node stays sleepy unless told otherwise.
- **v1 scoping:** batch workers ship inside the system's application package, and adding a job is a package update through the A/B slots (§7.4). A `kubectl run`-style ad-hoc job submission flow is v2 — it changes tooling, not architecture.

---

## 8. Operational modes, liveness, and safe state

### 8.1 Modes A / B / C

The three-mode spectrum from Vision Document 3 is retained and is one of the strongest ideas in the trilogy: you develop tethered and ship detached on the same fabric, so the test environment and the product are the same system.

| | Mode A — Host-centric | Mode B — Hybrid | Mode C — Autonomous |
|---|---|---|---|
| Host | runs application logic, issues RPCs | runs L4 work (SLAM, inference, planning) | absent |
| Mesh | virtual I/O expansion | runs all L0–L2 loops locally | runs everything from flash |
| Use | HIL, bench bring-up, diagnostics | mobile robots, exosuits, development | buildings, vehicles, remote grids |

**The reframe:** these are not configurations you switch between. **Mode C is the base state; A and B are what a node does when a host happens to be present.** Every node is always deploy-and-detach-ready. A mode transition is a non-event — no reconfiguration, no reload, no gap in the control loops. If your mesh behaves differently when the laptop is unplugged, you have two systems and you have only tested one.

And "host" is a **role, not a place**: a Pi riding inside the suit, the car, or the combine cab is a Mode B host even though the system never meets a lab or a laptop. Mode C means no host-class node is present *at all* — not merely that the desk PC is unplugged. Loss of an onboard host triggers the same per-actor `on_host_loss` menu (§8.3) as loss of a tethered one.

### 8.2 Heartbeat — corrected

Vision Document 3 specifies 50 ms heartbeats with `HOST_DISCONNECT` after 3 misses (150 ms). **This design produces false disconnects on ESP-NOW**, and the measurements say so directly: single-frame delays reach 25.6 ms on a 99.85% PDR link and 59.2 ms on an 83% link, and the protocol's own retry machinery admits up to 31 retransmissions at ~3350 µs — about 104 ms in which a frame is still legitimately in flight ([WONS 2025](https://dl.ifip.org/db/conf/wons/wons2025/1571077625.pdf)). **Any wireless liveness timeout below ~150 ms is provably wrong**, and 150 ms is the point at which you start getting them, not a safe margin.

**How the beacon reaches its peers is not a detail, and this section originally left it open.** A
node that unicasts a heartbeat to every peer costs `2·N·(N−1)` frames per period, which at §3's
1 Mbit/s default and a 108-byte on-air frame is 840 frames/s for seven nodes and 7,600 for twenty —
73% and 657% of the channel respectively, *at a floor* that excludes PHY preamble, ACK and backoff.
That contradicts §4's "one radio cell of ~20 nodes" outright, and the contradiction was invisible
while the only test fleet was two boards.

So liveness rides a **broadcast beacon**: one frame per node per period reaches every peer, making
the cost O(N) rather than O(N²) — 240 frames/s at twenty nodes, 21% of the channel. The timers below
are unchanged; only the transport of the beacon differs. RTT and ACK-based PDR ride a separate
**round-robin unicast probe**, one peer per node per probe interval, because measurement is a
diagnostic rather than a liveness signal and does not need the heartbeat's cadence. One consequence
is stated rather than hidden: a broadcast draws no MAC-layer ACK, so a beacon's delivery is measured
by its *receivers* from `hb_seq` gaps, and the ACK-based figure comes from the unicast probes.

Per-link-class timers, unchanged:

| Link | Period | Misses | Declared dead after | Rationale |
|---|---|---|---|---|
| Wired (UART / CAN) | 20 ms | 3 | 60 ms | deterministic media, tight bound is honest |
| Wireless (ESP-NOW) | 100 ms | 6 | 600 ms | ≈ 6× the ~104 ms worst-case in-protocol retry window, and strictly above L3's 500 ms deadline so liveness and class cannot flap at the same boundary |
| Host link | 250 ms | 4 | 1000 ms | host loss is not urgent — see 8.3 |

**A node that is asleep is not a node that has failed, and this section originally could not tell them apart.** §1.2's home has a garden watering node and §7.8 says it "stays sleepy unless told otherwise" — but a sleeping node's radio is off, so it misses every beacon, and the table above declares it dead after 600 ms. Simulated with one duty-cycled node in a five-node cell (awake 2 s, asleep 8 s — *conservative*; a real solar node wakes for seconds every few minutes): **240 death declarations and 232 revivals in five minutes, and the cell never settles.** Every consumer of that node's resources sees it flapping, and the death-declaration count — the signal that something is wrong — becomes noise.

So sleep is **announced, not inferred**. `HELLO` carries the sender's wake schedule (wake interval and wake window, zero for always-on). A peer with a declared schedule is measured against *that* rather than against the beacon period, and when it is inside its sleep window its state is **`DOZING`**: expected to be silent, not a failure, and not counted as one. A dozing peer that misses its *own* announced wake still goes to `DEAD` — the point is to stop confusing a duty cycle with a fault, not to stop noticing faults.

This costs nothing above: §4 already delivers a read from a silent node as `STALE` with its exact age, and a dozing peer's staleness is bounded by its own advertised interval, which is better information than a live node's usually is. `DOZING` is the third state after `DEAD` and `LEFT` in the same argument — a peer that is quiet on purpose must be distinguishable from one that is quiet because it broke.

### 8.3 The safety inversion

**Loss of the host is not an emergency.** Vision Document 3 couples `HOST_DISCONNECT` to safety loops cutting motor power. A node that brakes because a PC's heartbeat was late is a node that will brake randomly — and on the measured link statistics, "randomly" means "regularly." Worse, it trains the operator to distrust the safety system, which is the actual hazard.

Corrected policy:

- **Host loss → transition to Mode C, through a declared response.** The transition is normal, tested, and exercised in CI — but "autonomous" is not one behaviour, and the OS does not guess. Every actor's manifest declares `on_host_loss: continue | hold | controlled_stop | safe_state` — defaults: `continue` for sensors and self-contained loops, `hold` for actuators fed by host-computed setpoints, because a node that keeps integrating a dead host's last velocity command is not autonomous, it is unattended. Mechanism menu in the OS, policy in the deployment: the same fabric serves the vent (`continue`) and the arm (`controlled_stop`) without the architecture ever choosing a domain.
- **Safe state is triggered locally, always:** a local watchdog expiring, a local sensor out of bounds, a hardware e-stop input (L0), or a received `SAFE_STATE` broadcast.
- **Every node must be able to reach its safe state with the network entirely absent.** No node's safe state may depend on receiving a message. A safe state that requires a packet is not a safe state. Note the quiet demand this makes: each node-local safe state must be *independently* safe — coordinated multi-axis stop sequences cannot be the safety layer — which is a constraint on the machine's mechanical design (§12), not only its software.
- **`SAFE_STATE` is one frame, priority 31, on every transport, unfragmented.** A *genuine* safe-state frame is never throttled — each enrolled source holds a reserved verification slot — but verification *attempts* are rate-limited per source, because every spoofed frame costs a signature check before it can be rejected, and the safety lane must not double as a CPU-exhaustion lane (§9.4). Replay is fenced: the signed payload carries the sender's boot epoch, receivers reject epochs older than the last heard from that source, and high-water `(epoch, seq)` for safety sources persists in NVS — so a mesh power cycle does not reopen the window. On CAN, a max-priority flood still starves the bus by arbitration design, which is one more reason §12 puts the true e-stop in hardware.

### 8.4 Edge intelligence

Two strategies, both retained from Vision Document 3, both L4 by construction:

- **On-chip summarisation** — decision trees, thresholding, template reports. Runs on the node, no network needed, keeps Mode C useful rather than merely alive.
- **Direct-to-cloud reporting** — a node with Wi-Fi posts structured payloads or sends a daily report without a PC in the loop.

Constraint: **an outbound cloud path is an inbound attack surface.** Reporting nodes are outbound-only, hold no inbound listener, and never accept a deploy from anything but the cluster deploy key (§9). A node that can be told what to do by an HTTP response is not a node in your cluster any more.

---

## 9. Security

Absent from all three vision documents. This section exists because a mesh that accepts over-the-air executable modules with no signing and no trust story is not an architecture, it is a botnet with a build system.

### 9.1 What the platform gives you, and where it stops

Secure Boot v2 uses RSA-PSS with RSA-3072, stores the SHA-256 digest of the public key in eFuse, and "the application image is not only verified on every boot but also on each over the air (OTA) update" ([ESP-IDF Secure Boot v2](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/security/secure-boot-v2.html)). Note that on ESP32, "only one public key can be generated and stored in the chip during manufacturing."

**Where it stops:** a Potluck module written into a LittleFS slot is *data*, not an application image. Secure Boot does not see it, does not verify it, and will happily boot a firmware that then executes an unsigned module. **Module signature verification is Potluck's own responsibility.** This gap is the single most important thing in this section.

### 9.2 What ESP-NOW's link crypto gives you, and where it stops

CCMP protects the action frame, PMK and LMK are both 16 bytes, encrypted peers are capped at 17 with a default of 7, and — decisively — **"Encrypting multicast vendor-specific action frame is not supported"** ([ESP-IDF ESP-NOW](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html)).

Three consequences:

1. **Discovery is in the clear.** `HELLO` and any broadcast is unencrypted and unauthenticated at link layer. Discovery frames must therefore be *signed* at application layer. Signed, not encrypted — you cannot hide the existence of a mesh you are broadcasting.
2. **The 17-peer ceiling is a hard scaling limit** on link-layer crypto. A cluster above 17 nodes needs application-layer keying regardless, so Potluck does it from node one and treats CCMP as defence in depth.
3. **Link crypto is not the security boundary.** The Potluck Frame `auth_tag` is.

### 9.3 Trust model

- **Cluster CA.** One offline root key. Not on the workstation that runs `potctl`. Not in the repo.
- **Node identity.** Per-node keypair generated on-device at provisioning; the private key never leaves. Node certificate signed by the cluster CA binds `node_id` ↔ public key.
- **Session keys.** Pairwise, derived at `HELLO_ACK` via ECDH over the node certificates, rotated on epoch change. `auth_tag` on data frames is HMAC-SHA256 truncated to 64 bits under the session key.
- **Ed25519 signatures — not HMAC — on:** `HELLO`, `DEPLOY_*`, `MIGRATE_*`, `SAFE_STATE`, and any `WRITE` to a resource flagged `safety_relevant`. These are the frames whose forgery is worth an attacker's time, and they are rare enough that signature cost does not matter.
- **Deploy key.** Separate from the CA. Signs manifests. Compromise means bad code, not a forged cluster identity — a deliberate separation so the two failures are recoverable independently.
- **Anti-rollback.** Monotonic counter in NVS; manifests below it are refused.
- **Provisioning is physical.** A node joins by being connected over USB and enrolled. There is no over-the-air join in v1. This is a real limitation and it is the correct one at this scale — see ADR-007.

Hardware support: ESP-IDF's mbedTLS offers hardware AES, SHA, MPI (bignum/RSA) and ECC acceleration options ([ESP-IDF mbedTLS](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/protocols/mbedtls.html)). **[MEASURE]** Ed25519/Curve25519 is not mentioned on that page; confirm availability and per-verify cost on your target part at M5 before committing to Ed25519 over P-256.

### 9.4 Threat table

| Threat | Vector | Mitigation |
|---|---|---|
| Rogue node joins | forged `HELLO` on an open channel | CA-signed node certificate; unsigned/unknown `HELLO` refused and logged |
| Malicious module | pushed module executes on nodes | manifest + image signature verified **before commit**; anti-rollback counter |
| Command replay | actuator `WRITE` captured and resent | `seq` monotonic per `(src,dst)`, replay window enforced; safety-relevant writes signed |
| Downgrade | attacker forces the 226-byte v1 profile or an older manifest | version pinned in the authenticated `HELLO`; rollback counter refuses old manifests |
| Firmware extraction | physical access, flash readout | flash encryption + Secure Boot v2; note NVS is "not directly compatible with the ESP32 flash encryption system" and needs [NVS encryption](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/nvs_flash.html) alongside it |
| Broadcast flood / DoS | shared channel, [must be on the local device's channel](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html) | per-peer token bucket; unauthenticated frames dropped before parse; **safe state must not depend on the network** (§8.3) |
| Forged / replayed safe-state | attacker halts the system at will — including replaying a captured *genuine* frame after a power cycle | `SAFE_STATE` is signed **and** epoch-fenced, with high-water `(epoch, seq)` persisted in NVS (§8.3); a spurious halt remains fail-safe, but a *repeatable* remote halt is treated as denial-of-service, not shrugged off |
| Host compromise | workstation with `potctl` is owned | deploy key ≠ CA key; anti-rollback; nodes accept deploys only from the deploy key |

### 9.5 Stated limitations

- **64-bit truncated tags** buy integrity against casual injection on a 226-byte MTU. They do not stand against a determined attacker with sustained access. If a cluster ever guards something valuable, move to 128-bit tags and accept the payload cost.
- **No forward secrecy** across epoch boundaries in v1.
- **No key revocation** beyond re-enrolment in v1.
- **Physical access is game over** without flash encryption enabled, and flash encryption has irreversible eFuse consequences — read the Espressif documentation fully before you burn anything.

---

## 10. Architecture Decision Records

Each ADR is a closed decision with the observation that would reopen it.

**ADR-001 — v1 targets the ESP32 family only.**
*Context:* Vision Document 1 lists ESP32, STM32, RP2040 and RISC-V class parts. Four MCU families × six transports × three runtimes × four daemons is a research-lab surface area for one person.
*Decision:* ESP32 family via ESP-IDF only. Xtensa and RISC-V variants are both covered by one SDK, which gives portability evidence without a second toolchain. Two transports (ESP-NOW, UART) at M0–M3; CAN at M4. No Zephyr, no Thread in v1 — though [ESP32-H2, C6 and C5 carry 802.15.4 radios](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-guides/openthread.html) and are the natural v2 path.
*Rationale:* A portable abstraction validated on zero platforms is worth less than a working one validated on one.
*Revisit when:* M0–M5 are shipped and a second family is required by an actual deployment.

**ADR-002 — The Locality Contract is mandatory and checked at build time.**
*Context:* §4. Transparent naming without explicit cost is a latent field failure.
*Decision:* Latency class is part of every resource, actor and binding. Cross-class binding is a build error. Reads always return `(value, unit, timestamp, age, class, quality)`; past the bound, `quality = STALE` with the value still delivered under the default `informative` policy, or an error under the per-resource opt-in `strict` policy. *(Amended 2026-08-01: staleness handling is a per-resource toggle, not a global behaviour — agnosticism means the OS ships the mechanism and the manifest picks the policy.)*
*Rationale:* Converts an untestable runtime heuristic into a checkable static property.
*Revisit when:* Never for the classes themselves; the *deadline values* in the §4 table should be re-derived once M0 produces bench measurements.

**ADR-003 — Native actors are the default runtime. WASM is a gated tier.**
*Context:* Measured wasm3 at ~156 KB and ~11× native slowdown on ESP32-C6 ([arXiv 2512.00035](https://arxiv.org/html/2512.00035v1)) against a 160 KB static DRAM ceiling ([ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/memory-types.html)).
*Decision:* **Tier 0** — native C++ actors, configuration-driven, all nodes, default. **Tier 1** — wasm3, only on nodes with measured headroom, only for L3/L4 actors, only after M7 is justified. **Tier 2** — WAMR AOT on the host, where 480 KB is free.
*Rationale:* Deploy-and-detach needs *changeable behaviour*, which a signed configuration and a state machine deliver at a fraction of the cost. Dynamic bytecode is one way to get that, not the only way, and it is the expensive way here.
*Revisit when:* A concrete use case needs untrusted third-party code, or the baseline part gains enough measured RAM headroom to host an instance alongside the core and the application (§6).

**ADR-004 — One frame format everywhere; no schema translation in the bridge.**
*Context:* Vision Document 2 has `potluck-bridge` translating wire frames into gRPC.
*Decision:* Potluck Frame v1 (§5) is byte-identical on radio, wire, serial and host IPC. gRPC exists only between `potluck-agent` and `potctl`.
*Rationale:* One parser, one fuzz target, one capture format, byte-identical replay end to end. A translation layer at the bridge is a second place for bugs to live and a place where captures stop matching reality.
*Revisit when:* A third-party host integration genuinely requires a typed IDL below the agent.

**ADR-005 — Placement, migration and failure-reconciliation replace work-stealing.**
*Context:* §2, §7.7.
*Decision:* Actor placement is declared in the manifest. Portable actors (derived per §7.7) are re-placed on node failure by the deterministic reconciler, from code pre-provisioned at deploy time. Migration is explicit, checkpointed and acknowledged. Host offload is by named service. No load-driven stealing between live nodes.
*Rationale:* The arithmetic in §2 kills code-shipping steals; the owner's directive — every declared job runs somewhere, never twice — is Kubernetes' actual semantics, which is reconciliation, not stealing. And load-driven stealing makes system behaviour non-reproducible, the dominant cost in distributed embedded debugging. *(Amended 2026-08-01.)*
*Revisit when:* A profiled workload where moving work between two *live* MCUs beats both local execution and host offload.

**ADR-006 — No consensus protocol. Gossip + static ownership.**
*Context:* Vision Document 1, Decision Area 3, offered gossip/CRDT versus Raft.
*Decision:* Each node owns its own record, stamped `(node_id, epoch, seq)`; merge is last-writer-wins. Actuator ownership is assigned statically in the manifest. No election.
*Rationale:* Consensus is only needed when two parties must agree on something neither owns. Here nothing meets that description. Raft on a mesh whose PDR oscillates between 100% and 0 across a 14 m band ([WONS 2025](https://dl.ifip.org/db/conf/wons/wons2025/1571077625.pdf)) will spend its life re-electing.
*Revisit when:* Two nodes must contend at runtime for exclusive control of one actuator.

**ADR-007 — Provisioning is physical in v1.**
*Context:* §9.3. Over-the-air join needs a trust-on-first-use story, and TOFU on an open broadcast channel with [no encrypted multicast](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html) is weak.
*Decision:* Nodes enrol over USB. No OTA join.
*Rationale:* At the target scale, physical enrolment is not a burden, and it removes the largest attack surface in the system.
*Revisit when:* A deployment needs to add nodes without physical access — at which point design the enrolment ceremony *first*, not the mesh.

**ADR-008 — Host loss triggers Mode C through a per-actor declared response, never a global panic.**
*Context:* §8.3.
*Decision:* Safe state is triggered only by local conditions or a signed `SAFE_STATE` broadcast. Host loss is a normal mode transition whose per-actor behaviour is declared in the manifest: `on_host_loss: continue | hold | controlled_stop | safe_state` (defaults: sensors `continue`, host-fed actuators `hold`).
*Rationale:* False safety trips make a safety system distrusted, and the measured link statistics guarantee them under Doc 3's design. But single-response designs assume a domain — Doc 3's panic assumed a machine, the first draft's shrug assumed a building. A domain-agnostic OS ships the menu; the deployment picks. *(Amended 2026-08-01.)*
*Revisit when:* The four-option menu proves insufficient for a real deployment.

---

## 11. Prior art, and what you should not build

Honest landscape. Two projects already occupy a large part of the ground the vision documents claim.

**AtomVM** — "a ground-up implementation of the Bogdan Erlang Abstract Machine (a.k.a the BEAM) […] designed specifically to run on small systems", targeting Espressif ESP32 and ST STM32, offering "a concurrency-oriented platform, allowing users to spawn, monitor, and communicate with lightweight processes", with Distributed Erlang as a documented topic, and running in "around 128k of addressable RAM" at minimum ([AtomVM docs](https://doc.atomvm.org/main/welcome-to-atomvm.html)). That is the actor model, supervision, and inter-node messaging on ESP32 — already built, already running.

**Toit** — containers on ESP32 with a patch-based over-the-air update mechanism via the Artemis CLI, and containers that "can be started periodically, or when certain conditions are met" ([Toit fleet management](https://toit.io/product/fleet-management/)). That is deploy-and-detach with fleet management — already built.

**WAMR / wasm3** — if you do reach Tier 1, do not write a runtime. [WAMR](https://github.com/bytecodealliance/wasm-micro-runtime) supports ESP-IDF among its embedded targets with interpreter, fast interpreter, AOT and JIT modes; [wasm3](https://github.com/wasm3/wasm3) lists ESP32 and needs "~64Kb for code and ~10Kb RAM".

**The strategic reading — and this is my judgement, not a sourced claim:** Potluck's genuine differentiators are the **typed, bounded, location-transparent namespace** and the **Locality Contract**. Neither AtomVM nor Toit offers those; both offer the runtime and lifecycle machinery underneath. Writing another actor VM is the most expensive and least differentiated thing you could do with this design.

Licensing, verified 2026-08-01, removes friction from every path: [Kubernetes](https://api.github.com/repos/kubernetes/kubernetes), [ESP-IDF](https://api.github.com/repos/espressif/esp-idf), [WAMR](https://api.github.com/repos/bytecodealliance/wasm-micro-runtime) and [AtomVM](https://api.github.com/repos/atomvm/AtomVM) are Apache-2.0; [wasm3](https://api.github.com/repos/wasm3/wasm3) and the [FreeRTOS kernel](https://api.github.com/repos/FreeRTOS/FreeRTOS-Kernel) are MIT. Permissive across the board: designs are free to take outright, and code is reusable — even in a closed product — provided notices and attributions are kept.

Two credible paths:

- **Path A — build the layer, not the base.** Implement Potluck as native C++ on ESP-IDF (ADR-003, Tier 0). Full control of memory and timing, no dependency risk, most work.
- **Path B — build the layer on AtomVM.** Take BEAM processes, supervision and distribution as given; spend your effort entirely on the namespace, the contract, and deploy-and-detach. Far less code; you inherit a dependency and a memory floor.

**My recommendation:** Path A for M0–M2, because you need to own the wire format and the timing behaviour to trust your own numbers, and M0–M2 is exactly where the wire format lives. Then evaluate Path B at M3 with real measurements in hand. Committing to either before M0 is a decision made without data.

---

## 12. Where this stops being an engineering question

Two things in this design are not mine to decide, and one is not yours alone either.

**Anything worn by, attached to, or capable of striking a human being.** The vision documents name exosuits and cybernetic suits. The moment Potluck is anywhere near actuated force applied to a person, the governing question stops being architectural and becomes functional safety — hazard analysis, a rated safety function, an independent hardware interlock, and the relevant standards for that class of machine. **Potluck must never be the only thing between a motor and a person.** The safe state must be reachable by hardware with the software absent, and the design of that interlock belongs to a qualified functional-safety engineer, not to this document and not to me. I have designed for the software side to fail safe; that is not the same as a safe system.

**Radio and EMC.** ESP-NOW operates in a shared band, and CAN in an industrial setting has EMC obligations. Whether a given deployment is compliant is a question for someone with the test equipment and the local regulations.

**The compute-pooling cut (§2).** I have argued it from measurements and I think it is right. It is still your product decision, and it changes the pitch: without pooling, Potluck is "a location-transparent peripheral namespace with deploy-and-detach", which — in my view — is the stronger and more honest story anyway.

---

## 13. Roadmap: falsifiable milestones

The failure mode this roadmap is designed against is a beautiful specification with no heartbeat ever exchanged between two physical boards. Each milestone has an acceptance test you can fail and a kill criterion.

**M0 — Two boards, one heartbeat.** *(The only milestone that matters until it is done.)*
Two ESP32s. Potluck Frame codec. `HELLO` / `HEARTBEAT` over ESP-NOW. Link statistics: PDR, **round-trip** delay histogram, retry counts. (This line previously said "one-way delay histogram". Two boards have unsynchronised clocks, so a one-way figure would require either clock synchronisation M0 does not have or an assumption of path symmetry that ESP-NOW's retry machinery does not provide — a frame may sit in up to 31 retransmissions in one direction and none in the other, §3. §3's cited 2782.85 µs one-way figure comes from an instrumented experiment and is not something this firmware can reproduce. M0 therefore measures and reports RTT, decomposed into the local transmit-queue delay and the peer's own measured turnaround, and never divides anything by two.)
*Accept:* 24-hour soak at 100 ms heartbeat with a published delay histogram and PDR figure **measured on your bench, not cited from this document**. Wi-Fi-stack DRAM measured (§6, **[MEASURE]**).
*Kill:* if a stable link at your intended geometry cannot be achieved, the transport decision reopens **before** anything else is built.

**M1 — One remote read.**
Namespace, bind table, `READ`/`WRITE`, typed and timestamped values with staleness bounds.
*Accept:* `potctl read potluck://lab/n2/adc/0` returns value + unit + age + class. Unplug node 2 — the read returns `UNAVAILABLE`, never a cached number presented as fresh. (This line previously said `STALE`, which contradicts §4's own definitions and would make a correct implementation look like a failure at acceptance time. §4 assigns `STALE` to a value past its staleness bound that is *still delivered with its exact age*, and `UNAVAILABLE` to a resource whose owning node is dead or unreachable, with no value at all. An unplugged node is the second case: its last reading is not merely old, it is unattributable — nothing can say whether the resource still exists. Both satisfy the sentence that matters, "never a cached number presented as fresh", but only one is what the code returns, and §4 is the normative text. To see `STALE` here instead, stop *publishing* to a resource while leaving its owner alive.)

**M2 — Host in the loop, and replay.**
`potluck-bridge` over USB serial, frame tee-ing, `potctl`, `potctl replay`.
*Accept:* a captured 10-minute session replays and produces byte-identical namespace state.

**M3 — Deploy and detach.**
A/B LittleFS slots, signed manifests, trial-commit with automatic revert, cold boot into Mode C.
*Accept:* deploy a behaviour change to 3 nodes, unplug the host, power-cycle the whole mesh, and it comes up doing the new thing. Then deploy a deliberately broken module and watch all 3 nodes revert themselves.

**M4 — Locality Contract enforced, and CAN.**
Build-time checker; TWAI transport with segmentation.
*Accept:* a manifest binding an L1 actor to a remote resource fails the build with a readable error naming both ends. A `SAFE_STATE` frame wins CAN arbitration against saturating telemetry, demonstrated on a scope.

**M5 — Signed everything.**
Cluster CA, node enrolment, session keys, frame auth, anti-rollback.
*Accept:* an unenrolled board on the same channel is refused and logged. A replayed actuator `WRITE` is rejected. A downgraded manifest is refused. A genuine `SAFE_STATE` captured before a full-mesh power cycle is rejected when replayed after it. Ed25519 vs P-256 decided on measured verify cost (§9.3, **[MEASURE]**).

**M6 — Reconciler.** *Gated on M5 — autonomous re-placement without signed manifests is a self-rearranging botnet.*
Portability derivation from the manifest, rendezvous assignment, pre-provisioned activation, epoch fencing (§7.7).
*Accept:* kill the node running a portable actor — the actor serves reads from another node within 2 s; when the killed node returns, it does **not** double-run. Partition and heal the mesh — duplication ends within gossip convergence, and fenced consumers never accept the losing instance.

**M7 — WASM tier.** *Gated: only if M0–M6 shipped and a named workload requires untrusted or hot-swappable code.*
*Accept:* wasm3 on a node with measured headroom, with per-instance memory measured against the §6 budget.

**M8 — Host services and Mode B.** *Gated on M5.*
`potluck-agent` advertising `potluck://.../svc/*`; an MCU actor calling one and degrading correctly when the host is off.

---

## 14. Risk register

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Scope expansion back toward the full matrix | **High** — it is the natural pull of the original documents | Fatal | ADR-001; M7/M8 explicitly gated; every ADR has a *revisit trigger* rather than an open question |
| Partition-induced duplicate portable actors | Medium | wasted compute; conflicting L3/L4 outputs | epoch fencing (§7.7); actuator ownership never portable (ADR-006); duplication window bounded by gossip convergence |
| Wireless does not meet even L3 at your geometry | Medium | Reopens transport | M0 is the gate and has an explicit kill criterion |
| Namespace lookup cost dominates on-node | Medium | Reduces node count | 128-entry cap and perfect-hash the paths at build time from the manifest |
| Solo-maintainer bus factor | High | Fatal to continuity | keep the wire format specified and versioned independently of the implementation — the spec must outlive any one codebase |
| Reinventing AtomVM or Toit | Medium | Large wasted effort | §11; Path A/B re-evaluated with data at M3 |
| Security bolted on late | Medium | Redesign | `auth_tag` is in the frame header from M0 even though it is not enforced until M5 — the *bytes* are reserved from day one |
| False safe-state trips destroy trust | Medium | Product failure | ADR-008; per-class heartbeat timers derived from measurements |

---

## 15. Genuinely open questions

Four, each with the experiment that settles it. Everything else in this document is decided.

1. **Does the Locality Contract survive contact with real application code, or does everything end up L4?** *Experiment:* write three real applications at M4 — a vent controller, a joint controller, a building monitor — and count the class distribution. If they are all L4, the classes are not carrying weight and the taxonomy needs collapsing.
2. **Is the 128-entry namespace cap right?** *Experiment:* build the largest realistic bind table you actually want at M1 and measure lookup cost and memory against §6.
3. **Path A or Path B (§11)?** *Experiment:* at M3, with M0–M2 measurements in hand, prototype the M1 namespace on AtomVM for one week and compare lines of code and measured RAM against the native implementation.
4. **Does deploy-and-detach need dynamic code at all, or is signed configuration enough?** *Experiment:* implement M3 with configuration-only deploys and log every occasion over the following month where you genuinely needed new code rather than new parameters. If the count is zero, ADR-003's Tier 1 never opens and the project is simpler forever.

---

## Sources

All facts in this document were verified on 2026-08-01 and are recorded with retrieval dates in `.claude/zero-assumption/memory.md`.

- [ESP-IDF — ESP-NOW API reference](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html)
- [ESP-IDF — Memory Types (ESP32)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/memory-types.html)
- [ESP-IDF — TWAI (CAN) peripheral](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/twai.html)
- [ESP-IDF — NVS flash storage](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/nvs_flash.html)
- [ESP-IDF — Secure Boot V2](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/security/secure-boot-v2.html)
- [ESP-IDF — mbedTLS hardware acceleration](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/protocols/mbedtls.html)
- [ESP-IDF — OpenThread (ESP32-C6)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-guides/openthread.html)
- [esp-idf — components/freertos/Kconfig](https://raw.githubusercontent.com/espressif/esp-idf/master/components/freertos/Kconfig)
- Becker, Oberli, Zobel, Steinmetz & Meuser — [*ESP-NOW Performance in Outdoor Environments: Field Experiments and Analysis*, IEEE/IFIP WONS 2025](https://dl.ifip.org/db/conf/wons/wons2025/1571077625.pdf)
- [*WebAssembly on Resource-Constrained IoT Devices: Performance, Efficiency, and Portability*, arXiv:2512.00035](https://arxiv.org/html/2512.00035v1)
- [wasm3 — GitHub](https://github.com/wasm3/wasm3)
- [WebAssembly Micro Runtime (WAMR) — GitHub](https://github.com/bytecodealliance/wasm-micro-runtime)
- [AtomVM documentation](https://doc.atomvm.org/main/welcome-to-atomvm.html)
- [Toit — fleet management](https://toit.io/product/fleet-management/)
- Pike, Presotto, Dorward, Flandrena, Thompson, Trickey & Winterbottom — [*Plan 9 from Bell Labs*](https://9p.io/sys/doc/9.html)
- [Copperhill — CAN bus message frame format](https://copperhilltech.com/blog/controller-area-network-can-bus-tutorial-message-frame-format/) *(vendor blog; single source, treated as indicative)*

---

*Merged and extended from Potluck Architectural Vision Documents 1, 2 and 3 (Tue Wincentz Boas, July 2026). Decision spaces closed; quantitative claims re-verified against primary sources; Locality Contract, wire specification, memory budget, threat model, prior-art assessment and milestone gates added. Amended 2026-08-01: twelve adversarial-review findings applied (second-analysis.md), reconciler added (§7.7), vision invariant and known deviations recorded (§0.1).*
