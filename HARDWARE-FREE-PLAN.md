# Hardware-free work plan

**This does not change M0–M8.** Those milestones are decision-closed, with acceptance tests and kill
criteria, in [ARCHITECTURE.md](ARCHITECTURE.md) §13. This is a work *order* underneath them, which
knows about a constraint they do not: no ESP32-S3 is attached to this machine.

Steps are numbered **H0–H6** so they can never be confused with milestones. Each says which milestone
it advances, why it is unblocked, and what it explicitly does not deliver.

## Progress

| step | state | notes |
|---|---|---|
| **H0** | **DONE** -- M2 is accepted | 13.7-minute session, 11,444 frames, all six resources, replayed to a byte-identical digest |
| **H1** | **DONE** | CALL/CAST opcodes real, 8-byte header, both-side bounds check |
| **H2** | **DONE** | coordinator and workers measured in simulation; 19 workers give 18.99x; a killed worker loses no work |
| **H3** | **DONE** | manifest schema, parser and validator; 41 tests |
| **H4** | **DONE -- half of M4 accepted** | the locality-contract checker; section 13-M4's first sentence passes, error names both ends |
| **H5** | **DONE** | 29 on-target checks under QEMU, run by `tools/run_selftest.ps1` |
| **H6** | **DONE** | cluster CA, deploy keys, signed packages, anti-rollback; RFC 8032 vectors pass |

**All seven steps are done.** What remains is hardware: M0's acceptance soak, M1's unplug test, M3's
A/B slots, M4's CAN half, M5's firmware verification path, M6's reconciler.

21 gates green: 172 C++ cases and 36,658 checks on the firmware core, 29 on-target checks under QEMU,
and 154 Python cases including the manifest, the locality contract and the signer. Zero failures, with
AddressSanitizer and the firmware build. Firmware core 53,406 B, 11.8 KB of the 64 KB cap left.

### What H0 produced

`potctl soak` (new) sweeps every built-in resource in one connection for a duration, then prints the
digest of the namespace state it observed and the command that checks a capture reproduces it:

```
potctl --tcp 127.0.0.1:5555 --node 1001 --capture captures/m2-10min-soak.jsonl soak --seconds 630
python -m potluck --replay captures/m2-10min-soak.jsonl --expect-digest <printed digest>
```

The first attempt met the letter of section 13-M2 with a namespace of **one** entry, because `watch`
holds a single path. One entry is a weak thing to call byte-identical, hence `soak`. One connection
matters: QEMU's chardev accepts exactly one per VM lifetime.

Two bugs surfaced on the way, both of which had been making the emulator look flaky:

- `TcpTransport` connected lazily from the reader thread *and* the writer, so a race opened two
  sockets; QEMU bound the guest's UART to the first and left the second in the backlog. One thread
  talked to the guest, the other read silence, and the symptom was a node that answers nothing.
- `run_qemu.ps1` killed every `qemu-system-xtensa` on the way out, so a two-minute smoke run's
  teardown shot down a ten-minute soak three minutes in. It now kills only the VM it started.

### What H2 produced, and what it settles

`pot_work` (new, `sim/work.cpp`) runs section 7.8's pattern on the same modelled cell `pot_sim`
uses, driving the same real `pot::Node` the firmware runs. `sim/cell.hpp` was extracted so both
drivers share it.

**Scaling, 380 units of 200 ms, bench link:**

| workers | elapsed | speedup | efficiency | airtime |
|---|---|---|---|---|
| 1 | 78.30 s | 1.00x | 97% | 2.9% |
| 6 | 13.19 s | 5.94x | 96% | 12.9% |
| 12 | 6.59 s | 11.87x | 96% | 28.5% |
| 19 | 4.12 s | 18.99x | 97% | 55.8% |

Linear to the ESP-NOW peer ceiling, at 93-97% of a perfect scheduler with no dispatch cost at all.
Efficiency is against that ideal, so the missing few percent *is* the price of distribution.

**Where it stops working.** With units of 20 ms -- only three round trips long -- efficiency falls
to 76% and the channel saturates at 13 workers; at 18 the cell collapses and the job never finishes,
because the dispatch traffic starves the heartbeats and peers start being declared dead. The
guidance follows from the measurement rather than from taste: **a unit must be much longer than a
round trip**, and the round trip here is 5.6 ms.

**Killing a worker mid-job loses no work.** 60 units, six workers, worker 3 killed while holding a
unit: 60 of 60 completed, one unit written off and handed out again. The lossy links behave too --
`58m_cliff` (PDR 0.832) finishes at 96% efficiency, because ESP-NOW's unicast retries absorb the
loss before the pattern ever sees it.

**Three bugs it found, none of which any unit test had a chance of finding:**

1. The correlation table keyed on `msg_id` alone. A `msg_id` is unique per *peer*, so six workers
   all have a msg 0x0001 outstanding; a reply was credited to whichever slot matched first, marking
   one unit done twice and stranding another. It stalled a job at 59 of 60 units. Now keyed on
   (peer, msg_id).
2. A unit dispatched to a peer already believed dead vanished silently: the frame went nowhere, the
   slot stayed held, and the write-off for that death had already happened. `request_call` now
   refuses.
3. A coordinator that reads a worker's state instead of its own put a second unit on the wire during
   the 2.8 ms the first was still in flight -- 78 wasted frames in a 60-unit job.

**And one design consequence worth stating.** The node puts *no* deadline on a work unit, on purpose:
it cannot know how long somebody else's job takes. That means a lost CALL or REPLY on a worker that
stays alive strands its unit for good unless the coordinator has a deadline of its own. That is the
coordinator's half of the contract, `Node::cancel_call()` is how it keeps it, and `pot_work`
demonstrates both it and the livelock that follows from setting it shorter than a unit.

## The actual constraint, stated narrowly

An earlier session said "everything is blocked on hardware". That was too broad and it was wrong. The
real constraint is only two things:

1. **Do not allocate significant static firmware RAM.** §6's core cap is 64 KB, 53,118 B is used, and
   the remaining 12.1 KB is committed on paper to later milestones — while the Wi-Fi stack's real DRAM
   cost is still an unresolved `[MEASURE]`. This bars large buffers, actor blocks, crypto contexts and
   WASM instances. It does **not** bar small structures: a four-entry correlation table is tens of
   bytes and is noise against 12.1 KB.
2. **Do not tune timing policy against cited rather than measured PDR.** §8.2's period and miss limit
   stand until the bench says otherwise.

Everything else is fair game. That leaves host tooling, simulator work, wire-format work and
on-target testing under emulation — which is most of what M3, M4 and M5 actually are.

---

## H0 — Accept M2 · advances **M2** · ~20 minutes

§13-M2 reads, in full: *"a captured 10-minute session replays and produces byte-identical namespace
state."* It says nothing about hardware, and it does not need any: the firmware under QEMU is the real
firmware, and the property under test is a software one.

The mechanism is already proven and digest-gated (Session 6). The only thing missing was **duration** —
and that was only missing because QEMU's socket serial accepts one connection per VM lifetime. A
single `potctl` invocation holds a single connection, so:

```
potctl … --capture captures/m2-10min.jsonl watch sys/heap-free --interval 1 --count 600
python -m potluck --replay captures/m2-10min.jsonl --expect-digest <printed digest>
```

**Deliverable:** the capture, the digest, and M2 moved from *built* to **accepted** in the README's
milestone table.
**Does not deliver:** anything about the radio. M2 never asked.

## H1 — `CALL` / `REPLY` / `CAST` on the wire · advances **M7/§7.8 groundwork**

Opcodes `0x20`, `0x21`, `0x22` are reserved in `opcodes.hpp` and commented out. `REPLY` is already
implemented — it answers READ and WRITE — so the correlation and timeout machinery exists and is
tested. What is missing is the request side for arbitrary work units.

Built the same way as every other payload here: fixed layout, `static_assert` on each offset, golden
bytes typed from the spec, a generated corpus, and a second independent implementation in Python that
must agree byte-for-byte.

**Why unblocked:** a pending-call table is four entries, tens of bytes. Constraint 1 does not bite.
**Deliverable:** `pot::Node::call()`, host-side `Call`/`Cast` codecs, corpus in the gate suite,
`potctl call <path> <args>` working against `FakeNode`.
**Acceptance:** the differential corpus agrees byte-for-byte; a call round-trips over the loopback and
a lost reply surfaces as a timeout rather than a hang.

## H2 — Coordinator and workers in the simulator · advances **§7.8** · *the point of this plan*

§7.8 is the compute half of the vision and the answer to the project's founding grievance — *"an ESP32
is far too powerful to just watch a flowerpot"*. It is designed, its pattern is named
(coordinator + workers, work units as `CALL`/`REPLY`), and **nobody has built any of it.**

Build it in `sim/`, where seven virtual nodes already run the *real* `pot::Node` over a
§3-parameterised link. A latency-indifferent, embarrassingly parallel job — Monte Carlo is the
canonical one, and the shape that matters is **tiny in, tiny out, seconds in the middle**, since a
round trip costs ~5.6 ms during which a worker could have run ~1.3 M cycles.

**Why unblocked:** zero firmware bytes. It is host code exercising the portable core.
**Deliverable:** a coordinator actor, a worker actor, a scatter/gather job, and measured numbers —
throughput against one node, overhead fraction, behaviour under induced packet loss.
**Acceptance:** N workers give approximately N× throughput on a latency-indifferent job; and **killing
a worker mid-job loses no work** — the coordinator reassigns. That second half is a preview of M6's
reconciler for free.
**What it settles:** whether §7.8's promise holds, before any firmware commits RAM to it — and
therefore whether §7.8 deserves promoting up the roadmap.

### What H3 and H4 produced

`potluck/manifest.py` is the schema, parser and validator; `potluck/locality.py` is the Locality
Contract check. Two manifests ship with them: `manifests/home.json`, which passes, and
`manifests/broken-locality.json`, which is broken on purpose in six different ways.

```
python -m potluck.manifest  check manifests/home.json --resolved
python -m potluck.locality  check manifests/home.json
```

**Section 13-M4's first sentence now passes**, and it is worth quoting because the wording is the
test: *"a manifest binding an L1 actor to a remote resource fails the build with a readable error
naming both ends."* What the checker actually prints for that case:

```
vent_loop -> potluck://home/vents/hallway: actor is L1 (<10 ms, same node) and runs on 'utility'
(0x1003), but this resource is owned by 'hallway' (0x1001). L1 may not leave its own node, so this
binding cannot be made -- pin the actor to 'hallway', or declare it a looser class and accept the
deadline that comes with it
```

It finds six kinds of violation: an L0/L1 actor bound off-node; an actor bound to a resource looser
than itself; a class the transport between those two nodes cannot carry (an L2 actor reaching across
ESP-NOW, which is L3 at best -- *"class is transport-derived, not aspirational"*); two nodes with no
transport between them at all; a route needing two hops, which v1 does not have, because two <500 ms
hops compose to <1 s and that is not L3; and actors whose declared headroom cannot fit on the node
they were placed on.

**Two validator checks exist because of properties of the firmware rather than of the document,** and
neither could be caught anywhere else:

- **Path hash collisions.** The node stores a 32-bit FNV-1a hash and never the string, so two
  colliding paths are one resource to it -- which is what `NsError::HashCollision` means by "a
  manifest error". It is a property of the whole set of paths, so the build is the only place it can
  be seen. The test uses a *real* collision, found by search rather than asserted:
  `potluck://home/node-1001/x/aguzx` and `.../x/a52ad` both hash to `0x627da2e7`.
- **The 128-entry namespace cap per node.** Otherwise the manifest deploys and fails at the 129th
  `declare`, at runtime, on one node.

Three policy checks are there because the architecture says the runtime must not guess:

- **Unknown keys are errors, not warnings.** A misspelled `staleness_policy` is a resource that
  silently reverts to `informative`, which is the exact failure §4 rule 2 exists to prevent.
- **An actor that binds a writable resource must declare `on_host_loss`.** §8.3: a node that keeps
  integrating a dead host's last velocity command "is not autonomous, it is unattended". There is no
  safe default for something that can move the world, so the manifest has to say.
- **Background work on a battery or solar node needs an explicit opt-in.** §7.8: harvesting idle
  cycles denies sleep, and the garden node stays sleepy unless told otherwise.

The manifest's canonical byte form (sorted keys, no incidental whitespace, one trailing newline) is
in place from the start, because H6 signs it and a signature over "whatever the editor saved" is a
signature over formatting.

## H3 — The manifest · advances **M3**

M3's acceptance needs three boards and a power cycle. Its *format* needs neither. Define what an
application package declares: actors, their bindings, their constraints (`needs
potluck://…/adc/0`, `≥40 KB headroom`, `class L4`), and `priority: critical | normal | background`
per §7.8.

**Deliverable:** schema, parser, validator, tests.
**Does not deliver:** A/B slots, trial-commit, revert — all firmware, all after the DRAM measurement.

## H4 — The locality-contract checker · advances **M4, half of it genuinely accepted**

§13-M4's acceptance is two sentences and **the first needs no hardware at all**: *"a manifest binding
an L1 actor to a remote resource fails the build with a readable error naming both ends."*

Given a manifest (H3) and a fleet description, verify every binding's latency class is satisfiable by
its placement. This is the piece that turns §4's central safety claim from a convention into something
the build *refuses* to violate.

**Deliverable:** the checker, wired into the gate suite, with a deliberately-broken manifest as a
negative test.
**Acceptance:** exactly §13-M4's first sentence, including that the error names both ends.
**Does not deliver:** the CAN half — that needs two boards, two transceivers and a scope.

### What H5 produced

`firmware/main/selftest.cpp`, behind `CONFIG_POT_SELFTEST`, run by `tools/run_selftest.ps1`. 29
checks, all passing, of things that are properties of the running machine rather than of the source:

- **A struct cast onto an unaligned buffer, at all four offsets in a word.** §5's parser casts the
  header straight onto received bytes; on x86-64 an unaligned load is merely slow and on Xtensa it is
  a different instruction path. If it were wrong, every frame arriving at an odd offset would be
  wrong, and no host test could ever see it. It is not wrong.
- **Bytes from `.rodata` and from RAM parse identically** — different address regions on this part.
- **Doubles keep all 53 bits, and a denormal is not flushed.** The S3's FPU is single-precision only,
  so `double` is software, and this is where a silent truncation would live.
- **A task actually lands on core 1.** `portNUM_PROCESSORS` is a constant; this is an observation.
- **The codec path uses about 1 KB of stack** after 32 parses (3060 B unused of 4096). A property of
  the compiler and the target's register windows, so the number is worth keeping.
- **The namespace holds exactly its capacity and refuses the entry past it** — the runtime half of
  H3's manifest check, since a manifest is only one of the ways a resource gets declared.
- **`FREERTOS_HZ` is 1000**, which §4 mandates because L1's 10 ms budget sits exactly on the 100 Hz
  default tick. A build that lost that setting would pass every host test and give L1 zero margin.

**And one finding about the emulator, which is why this step was worth doing at all.** The first
version asserted that `vTaskDelay(50 ms)` takes at least 50 ms. It failed at 48,703 us -- short by
more than one tick, so not the familiar "N ticks means N-1 full ticks" off-by-one. A second run
measured 49,677 us. Under QEMU the FreeRTOS tick and the microsecond timer run at slightly different
rates, and not even consistently between runs. **Timing cannot be checked under emulation, not even
to one tick.** The check now asserts what it honestly can -- that the two clocks agree within a tenth,
which a gross tick misconfiguration would break -- and prints the delta as an emulator observation
rather than a measurement.

## H5 — On-target self-test under QEMU · advances **test coverage, all milestones**

All 158 C++ cases run on host x86-64. The layout `static_assert`s *do* compile for Xtensa, but nothing
**executes** there. A dedicated image touching no unemulated peripheral can close that gap: packed
struct access on Xtensa, single-precision float behaviour, dual-core scheduling, real stack
consumption, timing against a real clock — none reachable from a laptop test.

**Deliverable:** a self-test firmware target plus a `tools\` runner that reports pass/fail from the
console.
**Note:** the pattern is borrowed from the sibling Powersuit project, which runs 27 such checks.

### What H6 produced

```
python -m potluck.signing keygen  --role ca     --label lab-ca  --out keys/lab-ca
python -m potluck.signing keygen  --role deploy --label lab-dep --out keys/lab-deploy
python -m potluck.signing certify --ca keys/lab-ca.key --key keys/lab-deploy.pub --out keys/lab-deploy.cert
python -m potluck.signing sign    --key keys/lab-deploy.key --cert keys/lab-deploy.cert                                   --counter 7 manifests/home.json --out build/home.potpkg.json
python -m potluck.signing verify  --ca keys/lab-ca.pub --min-counter 7 build/home.potpkg.json
```

§9.3 asks for three things and two of them are here:

- **A cluster CA separate from the deploy key**, because *"compromise means bad code, not a forged
  cluster identity"*. The CA signs a short certificate authorising a deploy key; the deploy key signs
  manifests; a verifier needs only the CA's public key, since the certificate travels in the package.
- **Anti-rollback.** The counter is *inside* the signed preimage, so a downgrade produces an invalid
  signature rather than a valid one with a suspicious number beside it. A correctly signed older
  package is refused too, before the signature is even checked — that is the attack the counter is
  for.
- The firmware's verification path, enrolment and the NVS counter itself are M5 proper and wait for
  boards.

**Ed25519 in one dependency-free file**, `ed25519_ref.py`, checked against all three of RFC 8032
§7.1's published test vectors on every gate run. The project's test harness is a single header
specifically to avoid a first dependency, and the same reasoning applies here; the vectors are what
make it trustworthy, and what would let a vetted library replace it later without anyone having to
trust the swap. It is not constant-time and says so at the top.

**What this deliberately does not decide.** §13-M5's **[MEASURE]** — *"Ed25519 vs P-256 decided on
measured verify cost"* — is a bench question about the node. `alg` is therefore a field in every
structure and the verifier dispatches on it, so adding P-256 when the bench answers needs no format
change.

Three properties the tests insist on, each because the opposite is a plausible mistake:

- **A signature says who wrote it, never that it is well-formed**, so a signed manifest is re-parsed
  and re-validated in full before the signature is checked.
- **A certificate cannot be replayed as a package signature**, because the two preimages are
  domain-separated by their type strings.
- **Nothing secret reaches a package or a `.pub` file**, and `.gitignore` refuses `*.key` by pattern
  rather than by path — a key that reaches a public repository has to be replaced along with
  everything it ever signed.

## H6 — Manifest signing · advances **M5** · only if the wait is long

Cluster CA, key generation, manifest signing and verification — all host-side tooling. The firmware
verification path, anti-rollback and enrolment are M5 proper and wait for hardware.

---

## Order, and why

**H0 → H1 → H2 → H3 → H4 → H5 → H6.**

H0 first because it is twenty minutes and moves the scoreboard from *0 of 9 accepted* to *1 of 9*.
H1 before H2 because H2 needs it — it is a prerequisite, not a detour. H3 before H4 because a checker
needs something to check. H5 is independent and can be pulled forward whenever a break is wanted.

Two things this plan deliberately refuses to do: touch the closed ADRs, and build any firmware feature
whose static footprint the §6 budget cannot yet be shown to absorb.

**Nothing here substitutes for M0.** Two boards exchanging measured heartbeats remains the only thing
that matters until it is done; this is what to do *while the post is slow*, not instead.
