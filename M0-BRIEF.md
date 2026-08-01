# M0 Brief — Two boards, one heartbeat (overnight work order)

**Read first:** `CLAUDE.md`, then `ARCHITECTURE.md` §3 (constraints), §5 (Potluck Frame), §6 (memory
budget), §8.2 (heartbeat timers), §13-M0 (acceptance). All session rules in CLAUDE.md apply,
including the zero-assumption ledger and scope discipline.

**Scope: M0 only.** No namespace, no deploy, no CAN, no WASM, no reconciler — those are M1+.
If a task seems to need them, log it in `M0-LOG.md` and move on. Do not start M1.

**Hardware status: none attached tonight.** Target is *code-complete*: everything compiles,
host-side tests green, so the morning session is flash-and-measure only.

## Deliverables

1. **`firmware/`** — ESP-IDF project (C++17, latest stable IDF ≥ 5.5 — ESP-NOW v2's 1470 B
   payload requires it, see ledger):
   - Potluck Frame codec exactly per §5.1: 16-byte little-endian header, struct-cast parse,
     `auth_tag` bytes reserved and zeroed but **not** enforced (M5 enforces).
   - Opcodes: `HELLO`, `HELLO_ACK`, `HEARTBEAT`, `BYE`, `ERR` only.
   - ESP-NOW transport: v2 profile with per-peer v1 fallback pinned via `HELLO` (§5.3).
   - Heartbeat task: 100 ms period, peer declared dead at 6 misses (§8.2).
   - Link stats per peer: delivered/lost counts (PDR), RTT histogram, ESP-NOW retry/error
     counts. Dumped over serial as one JSON line per interval.
   - **Delay methodology must be honest:** clocks are unsynced, so measure RTT and report
     RTT — never fabricate a one-way figure. Document the method in the runbook.
   - Static allocation per the §6 budget table; save the `idf.py size` report to the repo.
     Core above the 64 KB cap = build treated as failing.
   - Measure and log Wi-Fi-stack DRAM at init (the §6 **[MEASURE]** item) — code the probe
     now, numbers land when hardware does.

2. **`host/potluck/`** — Python is fine for M0 tooling (the real bridge is M2):
   serial reader, frame decoder, capture-to-file (timestamp + direction + raw frame, the §7.6
   format), live PDR/RTT printout.

3. **`tests/`** — host-compiled, all green before stopping:
   codec round-trip, truncated/corrupt/bad-magic frames, a small fuzz corpus, heartbeat
   state machine (miss counting, death declaration, revival), stats accumulation.

4. **`M0-RUNBOOK.md`** — morning procedure: flash two boards (any ESP-IDF Wi-Fi board; classic
   ESP32 per ADR-001), start the soak, read the stats, and the accept/kill checklist copied
   from §13-M0.

5. **`M0-LOG.md`** — append-only progress log as you work: decisions, blockers, anything
   deferred. Assume the morning session starts by reading it.

## Hard rules

- Match §5.1 byte-for-byte. The wire format is the product; the firmware is scaffolding.
- Every factual claim (IDF API behaviour, sizes, versions) → verify live, register in
  `.claude/zero-assumption/memory.md`.
- Prefer boring code. No cleverness the §6 budget doesn't pay for.
- Stop at: tests green + runbook written + log current. Do not begin M1 with leftover hours;
  spend them on more codec fuzz cases instead.
