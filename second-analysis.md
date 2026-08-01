# Second analysis — adversarial re-read of ARCHITECTURE.md §2, §4, §8.3

Reviewer: Fable 5 Max, 2026-08-01, same session that produced the merge. Brief: attack the three
sections where the merged document departs furthest from the vision trilogy. These are findings,
not applied edits. Three of them (F1, F4, F9) amend decisions recorded as closed — those are the
author's call, which is exactly what the ADR revisit discipline is for.

Overall verdict up front: all three decisions **survive**. The pooling cut, the Locality Contract,
and the safety inversion are right. What does not survive intact is some of the *reasoning* — two
arguments prove less than they claim, one rule destroys information it doesn't need to, one revisit
trigger is written so it can never fire, and one replay hole allows a repeatable remote halt of a
detached mesh.

---

## §2 — The compute-pooling cut

**F1 (high) — The arithmetic attacks the weakest implementation, and the ADR trigger inherits the
strawman.** Every number in §2 — 156 KB, 11×, 480 KB — prices the *ship-WASM-bytecode-at-steal-time*
variant. Doc 1's Option B was native actors pre-compiled into the firmware image. On a homogeneous
mesh where every node carries the same image, a "steal" is a **state transfer, not a code
transfer**: no interpreter, no 11× penalty, transfer cost ≈ one message. The cut still survives —
via bar (d) and ADR-005's reproducibility argument, which is the stronger ground anyway — but the
stated rationale doesn't cover the native variant. Worse, ADR-005's revisit trigger reads "beats
both local execution and host offload **plus 11× interpretation**": the only plausible future
counterexample is the native variant, which has no 11× term, so the trigger as written can never
fire. A revisit trigger that can't fire is a decision pretending to be falsifiable.
*Fix:* reword §2's conclusion — dynamic code-shipping steals are dead on the numbers;
pre-provisioned activation is cut on determinism/reproducibility grounds (ADR-005) — and strike
"plus 11× interpretation" from the trigger.

**F2 (high) — Bar (d) is mode-dependent, and the flagship mode deletes it.** "Not better served by
the host" assumes a host. In Mode C there is none — and Mode C is the product's celebrated state.
Doc 1's own structural-monitoring use case furnishes the counterexample: FFT over accelerometer
windows, shipped as *data* (not code) to a mains-powered neighbour, is latency-tolerant,
frame-friendly, too heavy to co-locate with sampling — and has no host to defer to. The honest
defence is static placement: put the fusion actor on the mains node at deploy time. That defence
works, but §2 never states it, so the four-bar proof silently assumes Modes A/B.
*Fix:* add the mode-C paragraph — in detached operation, (d) is replaced by deploy-time placement,
and the residual claim is that compute-bound MCU workloads with *time-varying* placement needs are
rare enough to not justify a scheduler. That is a judgment, and it should be visible as one.

**F3 (medium) — Migration has no initiator in Mode C.** §7.4 migration is "planned, acknowledged,
checkpointed" — planned *by whom* when there is no host? Doc 1's smart-building strategy (battery
nodes shed compute to mains nodes) needs load-shedding at 3 a.m. with the PC gone. Either
(a) permit node-initiated `MIGRATE_PREPARE` under a manifest-declared policy (small spec addition),
or (b) state that battery-aware placement is static-only in v1 and Doc 1's dynamic version is cut.
Both are defensible; the document currently chooses neither.

*(Minor, no action: the four bars overlap — (b) transfer cost only bites when (c) latency
sensitivity makes it bite, and deploy uses the same lossy link and accepts it. Presenting them as
four independent gates overstates the machinery; it's really (c)+(d) doing the work.)*

---

## §4 — The Locality Contract

**F4 (high, amends ADR-002) — Hard STALE refusal destroys information the application legitimately
owns.** Rule 2: past the staleness bound "the read returns `STALE` — it does not return the old
number." That bans last-known-good-with-age — the standard pattern for estimators that predict
through dropouts (a Kalman filter *wants* the stale measurement and its exact age; that is my
domain judgment, not a sourced claim). The contract's true target is *silent* staleness, and the
tuple already carries `age` and `quality` — the machinery to make staleness loud without hiding
the value exists in the same sentence. As written, rule 2 and the tuple are redundant with each
other, and the rule wins by deleting data.
*Fix:* return the stale value with `quality = STALE`; the banned act is returning it *unmarked*.
Hard refusal becomes an opt-in per-resource policy (right for safety-relevant actuator feedback,
wrong as the universal default).

**F5 (high) — "A linter can check it on your laptop" oversells — it's the Plan 9 sin, one level
up.** Build-time checking verifies **topology**: which hop types a binding crosses. Whether an L3
binding actually meets 500 ms is **geometry and RF environment** — the WONS data has PDR flipping
between 100% and 0 across a 14 m band, so the same manifest passes on a bench at 3 m and fails in
a building at 60 m. The section that exists to replace transparent-but-lying naming with
explicit cost ends its pitch by implying the cost is statically knowable. It isn't; only the
*ceiling* is static — *satisfaction* is runtime-monitored (rule 4 already says so).
*Fix:* one wording change in the payoff paragraph: "class ceilings are checked at build time;
class satisfaction is monitored at runtime." The contract must not commit the lie it was built
to kill.

**F6 (medium) — Rule 3's path arithmetic is wrong for multi-hop, and v1's hop scope is nowhere
stated.** The class table defines classes as "one wired hop" / "one wireless hop", yet rule 3
speaks of "the worst class of any transport on its path" — worst-hop is a *lower bound*, not a
budget: two L3 hops at <500 ms each compose to <1 s, which is not L3. Min-rule is only correct
for single-hop paths. Meanwhile §5 has dst-addressing and a router, and Doc 1 promised a mesh —
so is v1 single-hop-only? With the 20-peer ESP-NOW ceiling, single-hop means a cluster is ~20
nodes inside one radio cell. That is a major scope statement currently hiding in a table.
*Fix:* either declare "v1 paths are single-hop; forwarding is v2" (and say the ~20-node
consequence out loud), or define budget-summing: a path's ceiling is the tightest class whose
deadline exceeds the sum of per-hop budgets.

**F7 (medium) — Rule 4's p99 demotion isn't implementable inside §6's budget as written.**
Per-binding p99 needs a histogram or quantile sketch: 128 bindings × 16 buckets × 2 B ≈ 4 KB,
against a 2 KB line item for *all* counters, link stats, and the event ring. And at realistic
publish rates a p99 estimate needs hundreds of samples — minutes of detection latency — while the
measured failure mode (PDR cliff) kills links in seconds. Precise-sounding, wrong tool.
*Fix:* demote on fast signals the node already has — consecutive timeouts, ESP-NOW retry counts,
heartbeat misses — and make p99 a host-side audit in Modes A/B (`potctl` has the capture stream;
that's what §7.6 is for). Node-side rule 4 becomes "N consecutive deadline misses → DEGRADED."

**F8 (low, concrete) — L1's budget sits exactly on the default tick.** `FREERTOS_HZ` defaults to
100 → 10 ms per tick (Kconfig, already cited in §3). L1's deadline is <10 ms: an L1 actor under
default config has zero scheduling margin — one tick of jitter is a deadline miss. §3.1 noticed
the tick for sub-millisecond work but never connected it to L1.
*Fix:* the reference config mandates `FREERTOS_HZ=1000` (the Kconfig range allows it), or L1's
budget moves off the tick boundary. State it in §4's table notes.

---

## §8.3 — The safety inversion

**F9 (high, amends ADR-008) — The inversion is binary where motion systems need a third state.**
Host-loss → "Mode C, a normal transition" is right for the vent and wrong for the robot — and
Mode B is *defined* as the host running SLAM/pathing. When the host dies mid-motion, the mesh
holds either a stale setpoint or no plan; "carry on autonomously" can mean "keep driving at last
commanded velocity." The missing vocabulary between *emergency stop* and *carry on* is the
**controlled stop / hold**: decelerate, hold position, await a new plan. Doc 3 had one response
(panic); the merge replaced it with one response (shrug); both are single-response designs.
*Fix:* host-loss behaviour becomes a per-actor manifest declaration —
`on_host_loss: continue | hold | controlled_stop | safe_state` — defaulting to `continue` for
sensors and `hold` for host-fed actuators. ADR-008 is amended, not reversed: the false-trip
argument stands; the response menu grows.

**F10 (high) — A genuinely signed `SAFE_STATE` is replayable after a power cycle.** Anti-replay
is "seq monotonic per (src,dst)" (§9.4) — and nothing says the last-seen seq survives reboot.
Capture one legitimate signed `SAFE_STATE` today; cold-boot the mesh (routine in Mode C); replay
it → the whole cluster halts, repeatably, forever, with no key compromise. Fail-safe direction,
yes — but a repeatable remote halt of a detached building mesh is a real denial-of-service, and
§9.4's threat table waves the forged-safe-state row away on exactly the asymmetry this breaks.
*Fix candidates:* bind `SAFE_STATE` signatures to the sender's boot epoch and reject epochs older
than last-heard (epoch already exists in `HELLO`; tolerate unknown-epoch with a challenge), and/or
persist high-water seqs for safety-relevant sources in NVS — many small values is precisely what
NVS is for. Choose at M5.

**F11 (medium) — "Never rate-limited" + verify-before-act = a priority-31 DoS lane.** Every
spoofed `SAFE_STATE` must be signature-verified before it can be rejected; per-verify cost on
target is unmeasured (§9.3's own [MEASURE] flag), and the frame is exempted from rate limiting by
design. On CAN, a flood of max-priority frames additionally starves the bus by arbitration. The
fix is standard but must be stated: rate-limit *verification attempts* per source with one
reserved guaranteed slot (a genuine safe-state always gets through), cache the last verified
(src, seq), and lean on §12's position that the true e-stop is the hardware interlock anyway.

**F12 (low) — Liveness deadline equals class deadline, and the composition assumption is
unstated.** Wireless peer declared dead at 500 ms; L3 budget is 500 ms p99 — a peer can be
declared dead while its binding is still nominally in-class. Boundary flap by construction; make
death strictly exceed the tightest class deadline it carries (6 misses, or 120 ms period).
Separately, "no node's safe state may depend on receiving a message" quietly *requires that every
node-local safe state be independently safe* — it forbids coordinated multi-axis stops as the
safety layer. That is a real constraint on mechanical design (per §12, where it belongs), but it
is load-bearing and currently invisible. One sentence makes it visible.

---

## Disposition

| Finding | Severity | Nature | Blocked on |
|---|---|---|---|
| F1 | high | rationale gap + rigged ADR-005 trigger | author accept (amends ADR-005 text) |
| F2 | high | §2 proof assumes a host | mechanical fix |
| F3 | medium | migration initiator undefined in Mode C | author picks (a) or (b) |
| F4 | high | STALE refusal deletes data | **author accept (amends ADR-002)** |
| F5 | high | static-check oversell | mechanical wording fix |
| F6 | medium | multi-hop composition / scope unstated | author picks single-hop v1 or budget-summing |
| F7 | medium | p99 not implementable in budget | mechanical fix (fast signals + host audit) |
| F8 | low | L1 = default tick, zero margin | mechanical fix (mandate 1000 Hz) |
| F9 | high | missing controlled-stop state | **author accept (amends ADR-008)** |
| F10 | high | SAFE_STATE replay after reboot | mechanical fix, mechanism chosen at M5 |
| F11 | medium | verify-DoS on the safety lane | mechanical fix |
| F12 | low | liveness = deadline; hidden safety constraint | mechanical fix |

Nothing here reopens the three decisions. Everything here is why a second read exists.

---

## Resolution — 2026-08-01, same day

The owner declined the verdict framing and restated the working contract: they supply the vision —
*"hardware agnostic but mostly 'Kubernetes for CPU/GPU/RAM AND attached hardware on ESP32 and
other machines'"* — sessions make the engineering decisions inside it, and only *deviations from
the invariant* are escalated. Their three responses, decoded and applied:

- **F1** — *"any job needs to be done in the network, and doing the same twice is misuse of
  resources."* That is a reconciliation requirement, not work-stealing — and it is Kubernetes'
  actual semantics (declarative placement + failure-driven re-placement; k8s never steals between
  live nodes). Applied: §7.7 reconciler added (derived portability, rendezvous assignment,
  pre-provisioned activation, epoch fencing); ADR-005 amended; §2's trigger unrigged. Absorbs F3.
- **F4** — *"not 'how should it react' but a feature that can be toggled."* Applied: per-resource
  `staleness_policy: informative | strict`; ADR-002 amended. Mechanism in the OS, policy in the
  manifest.
- **F9** — the domain question itself was flagged as the red flag, correctly: Potluck is
  domain-agnostic by premise, so the OS must never need that answer. Applied: per-actor
  `on_host_loss: continue | hold | controlled_stop | safe_state` in the manifest; ADR-008 amended.
  The reviewer's question was mis-framed; the proposed mechanism was already the agnostic answer.

All nine mechanical findings applied in the same pass (F2, F3 → §7.7, F5, F6, F7, F8, F10, F11,
F12). Milestones renumbered: reconciler is M6; WASM tier → M7; host services → M8. The vision
invariant and the two knowing narrowings (ESP32-first sequencing, stealing→reconciliation) are
recorded in ARCHITECTURE.md §0.1 as standing deviation disclosures.
