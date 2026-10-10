// M7: guest actors -- a program from a party the cluster's owner does not trust, run as a workload of the
// cluster (ARCHITECTURE section 13, M7).
//
// A guest is a WebAssembly module carried in the deploy image's guest section (pot/deploy.hpp, version 2)
// with what the owner decided about it: its inputs and outputs (namespace paths, wired by the owner's
// manifest), how much fuel one tick may burn, how much memory it may have. Its author signs the module
// (a role-3 certificate from the cluster CA, pot_trust's guest_check); the owner signs the image. Each
// node verifies the author's signature before the module runs, and runs it only in the sandbox
// (pot/wasm_sandbox.hpp): it reaches the cluster through the guest API below and nothing else.
//
// A guest actor is portable (section 7.7): the reconciler places it, moves it when its node dies, and
// hands it its checkpoint (up to 128 bytes the guest saved) on the new node. Its code runs on a task of
// its own (GuestRuntime::service), never on the link task: the actor's tick only snapshots the inputs,
// hands the work over, and publishes what the last run produced.
//
// Faults are the guest's, never the node's. A trap -- out of fuel, out of bounds, unreachable, a
// publish to an output it does not have -- fails that tick: nothing it published or saved in that tick
// is kept, its outputs read FAULTY until a tick succeeds, and the instance is discarded (the next tick
// starts a fresh one from the last checkpoint). Three failed ticks in a row quarantine it: it runs no
// more in this activation and its outputs stay FAULTY. A module that cannot be loaded at all (a bad
// author signature, a refused import, no `tick` export) is quarantined at once. A failover or a reboot
// is a new activation, with three strikes of its own.
//
// What a guest can never do, by construction: write anything (every output is a read-only resource
// under act/<actor>/, and the API has no write); bind a resource that is not readable (the host refuses
// it, as for any portable actor -- ADR-006); name a pin, the radio, another node or an address outside
// its own memory. So a guest is never the thing between an actuator and anything (section 12).
//
// THE GUEST BLOB (one per guest actor; little-endian):
//
//   0   4        magic "PGST"
//   4   1        version (1)
//   5   1        n_inputs   0..kMaxGuestInputs     inputs, by index, in the manifest's `needs` order
//   6   1        n_outputs  1..kMaxGuestOutputs
//   7   1        memory_pages 0..kGuestMaxPages    the linear memory allowed, in 64 KiB pages
//   8   4        fuel_per_tick                     for init() and for each tick() alike
//   12  4 x n_in   input path hashes
//   ..  5 x n_out  output path hash u32, ValueType u8 (I32 or F32)
//   ..  112      the author's certificate (role 3)
//   ..  64       the author's signature over "potluck-guest-v1\0" || SHA-512(module)
//   ..  rest     the module (a WebAssembly binary)
//
// Portable: the same sources run in the host tests, where the test drives service() itself.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "pot/actor.hpp"
#include "pot/deploy.hpp"
#include "pot/node.hpp"
#include "pot/trust.hpp"
#include "pot/wasm_sandbox.hpp"

namespace pot {

constexpr uint32_t kGuestMagic = 0x54534750u;  // "PGST"
constexpr uint8_t kGuestVersion = 1;
constexpr size_t kMaxGuestInputs = 8;
constexpr size_t kMaxGuestOutputs = kMaxOutputs;
constexpr uint8_t kGuestMaxPages = 1;  // 64 KiB: what a Rust guest built by tools/build_guests.py asks for
constexpr uint32_t kGuestFuelMin = 1000;
constexpr uint32_t kGuestFuelMax = 50000000;  // ~16 s at the S3's measured 327 ns a unit: a bound, not a budget
constexpr size_t kGuestFixedLen = 12;
constexpr uint8_t kGuestStrikes = 3;  // failed ticks in a row before quarantine
// The guest's calls may drive the C stack this far below call(): the guest task has 16 KB (each wasm3
// op nests a frame on Xtensa, M0-LOG session 36), and the imports and the trap path need the rest.
constexpr uint32_t kGuestNativeStack = 12 * 1024;
constexpr uint32_t kGuestValueStack = 8 * 1024;

struct GuestSpec {
    uint8_t n_in = 0;
    uint8_t n_out = 0;
    uint8_t mem_pages = 0;
    uint32_t fuel = 0;
    uint32_t in_hash[kMaxGuestInputs] = {};
    uint32_t out_hash[kMaxGuestOutputs] = {};
    ValueType out_type[kMaxGuestOutputs] = {};
    const uint8_t* cert = nullptr;  // kNodeCertLen bytes
    const uint8_t* sig = nullptr;   // kEdSigLen bytes
    const uint8_t* module = nullptr;
    uint32_t module_len = 0;
};

// Structure only: the signature is guest_check's. False sets `why`.
bool parse_guest_blob(const uint8_t* blob, size_t len, GuestSpec& out, const char** why);

// The author signatures of every guest in an image, under the cluster CA. Run where the owner's image
// signature is checked, so a guest whose author signature fails is refused with the image, before it is
// committed. `which` is set to the failing guest's index.
CertError guest_image_check(const DeployImage& img, const uint8_t ca_pub[kEdPubLen], uint8_t* which);

// The guests of the running image, copied out of the deploy buffer (which the receiver reuses) into a
// pool the board provides -- in PSRAM on the boards.
class GuestLibrary {
  public:
    // Parse every blob, check that each guest is run by exactly one guest actor, and copy them all into
    // `pool`. All or nothing: on false the library is empty and `why` names the first problem.
    bool load(const DeployImage& img, uint8_t* pool, size_t pool_len, const char** why);
    void clear() { n_ = 0; }
    uint8_t count() const { return n_; }
    const GuestSpec* spec(uint8_t i) const { return i < n_ ? &specs_[i] : nullptr; }

  private:
    GuestSpec specs_[kMaxGuests];
    uint8_t n_ = 0;
};
GuestLibrary& guest_library();

// ---- the runtime: one context per guest, the link task on one side and the guest task on the other ----

struct GuestInput {
    Quality q = Quality::NoData;
    Value v{};
};

// What the actor hands over for one run.
struct GuestRequest {
    uint32_t gen = 0;  // the activation; a different one discards the instance and starts afresh
    uint32_t now_ms = 0;
    GuestInput in[kMaxGuestInputs];
    uint8_t ck[kMaxCheckpoint] = {};  // what restore() returns
    uint8_t ck_len = 0;
    bool has_ck = false;
};

// What one run produced. Outputs and the checkpoint count only when `error` is null.
struct GuestResult {
    uint32_t gen = 0;
    const char* error = nullptr;
    bool fatal = false;  // a refusal no retry can change: quarantine now
    bool fresh = false;  // this run loaded a new instance (and ran init() if the guest has one)
    uint64_t fuel_init = 0;
    uint64_t fuel_tick = 0;
    Value out[kMaxGuestOutputs];
    bool out_set[kMaxGuestOutputs] = {};
    bool save_set = false;
    uint8_t save[kMaxCheckpoint] = {};
    uint8_t save_len = 0;
    uint32_t logs = 0;
    int32_t log_last = 0;
    uint32_t run_us = 0;       // the whole run on the guest task (load and init included), when metered
    uint32_t load_heap_b = 0;  // heap a fresh instance took (module, runtime, memory, init), when metered
};

class GuestRuntime {
  public:
    // The cluster CA every author certificate must chain to; null on a node that is not enrolled, which
    // then runs no guest (it cannot tell who wrote one).
    void set_ca(const uint8_t* ca_pub);
    // Called when a request is handed over (the board wakes its guest task). May be null.
    void set_waker(void (*fn)(void* ctx), void* ctx) {
        wake_ = fn;
        wake_ctx_ = ctx;
    }
    // A clock and the free heap, for the stats (M7's [MEASURE]). Either may be null.
    void set_meter(uint32_t (*now_us)(void* ctx), size_t (*heap_free)(void* ctx), void* ctx) {
        now_us_ = now_us;
        heap_free_ = heap_free;
        meter_ctx_ = ctx;
    }

    // ---- the link task's side (in place: the request and result are too big for an actor's slot) ----
    // A new activation of guest `i`: returns its generation. Any instance of an older one is discarded.
    uint32_t activate(uint8_t i);
    // The activation ended (the actor was destroyed): the guest task unloads the instance.
    void release(uint8_t i, uint32_t gen);
    // The request to fill, or null while the previous run is unfinished or uncollected. send() hands it
    // over.
    GuestRequest* request(uint8_t i);
    void send(uint8_t i);
    // The finished run for `gen`, or null. A finished run of another generation is dropped here. Read
    // it, then consumed().
    const GuestResult* result(uint8_t i, uint32_t gen);
    void consumed(uint8_t i);

    // ---- the guest task's side ----
    // Run every handed-over request once, and unload what no activation wants. True if it ran one.
    bool service();

    // Tests: forget everything (unload every instance, forget the CA and every verdict).
    void reset();
    bool loaded(uint8_t i) const { return i < kMaxGuests && ctx_[i].sb.loaded(); }
    uint8_t verified(uint8_t i) const { return i < kMaxGuests ? ctx_[i].verified : 0; }

    // The guest API's implementation reads this.
    struct Ctx {
        // 0 idle, 1 requested, 2 running, 3 done. The request is written in 0 by the link task and
        // read in 2 by the guest task; the result the other way round, in 2 and 3.
        std::atomic<uint8_t> state{0};
        std::atomic<uint32_t> active_gen{0};  // 0: no activation wants this guest
        uint32_t next_gen = 0;                // link task only
        GuestRequest req;
        GuestResult res;
        // guest task only
        WasmSandbox sb;
        uint32_t loaded_gen = 0;
        uint8_t verified = 0;  // 0 not yet, 1 the author signature verified, 2 it failed
        const GuestSpec* spec = nullptr;  // while a run is under way: the imports read the wiring here
    };

  private:
    void run(uint8_t i);
    Ctx ctx_[kMaxGuests];
    const uint8_t* ca_ = nullptr;
    void (*wake_)(void*) = nullptr;
    void* wake_ctx_ = nullptr;
    uint32_t (*now_us_)(void*) = nullptr;
    size_t (*heap_free_)(void*) = nullptr;
    void* meter_ctx_ = nullptr;
};
GuestRuntime& guest_runtime();

// ---- the actor ----

class GuestActor : public Actor {
  public:
    GuestActor(Node& node, const TickerConfig& place, uint8_t guest, const CheckpointStore* ck);
    ~GuestActor() override;
    bool start(uint32_t now_ms) override;
    void tick(uint32_t now_ms) override;
    size_t stats_json(char* buf, size_t cap, uint32_t now_ms) override;

    struct Stats {
        uint32_t runs;         // runs finished (init and tick together count once)
        uint32_t ok;           // ticks that succeeded
        uint32_t failed;       // ticks that trapped or could not run
        uint32_t overruns;     // periods skipped because the previous run had not finished
        uint32_t fresh;        // instances loaded
        uint32_t reads;        // remote input refreshes asked for
        uint32_t ck_saved;     // checkpoints the reconciler kept
        uint64_t fuel_last;    // the last tick's fuel
        uint32_t logs;
        int32_t log_last;
        uint32_t run_us_last;  // metered boards only
        uint32_t run_us_max;
        uint32_t load_heap_b;
    };
    const Stats& stats() const { return stats_; }
    bool quarantined() const { return quarantined_; }
    const char* last_error() const { return last_error_; }
    uint8_t strikes() const { return strikes_; }

  private:
    void refresh_inputs(uint32_t now_ms);
    void fail(const char* why, bool fatal, uint32_t now_ms);
    void mark_faulty(uint32_t now_ms);

    Node& node_;
    TickerConfig place_;
    uint8_t guest_;
    const GuestSpec* spec_;
    const CheckpointStore* ck_store_;
    uint32_t gen_ = 0;
    uint32_t next_ms_ = 0;
    bool started_ = false;
    bool quarantined_ = false;
    uint8_t strikes_ = 0;
    const char* last_error_ = nullptr;
    // The newest checkpoint: loaded at start, then the guest's last successful save.
    uint8_t ck_[kMaxCheckpoint] = {};
    uint8_t ck_len_ = 0;
    bool has_ck_ = false;
    bool ck_dirty_ = false;  // saved by the guest, not yet kept by the reconciler (it allows one a second)
    uint32_t asked_ms_[kMaxGuestInputs] = {};
    Stats stats_{};
};

// The registration table's row: portable only, with the placement and outputs hooks.
bool guest_check_decl(const ActorDecl& d, const ActorEnv& env, const char** why);
size_t guest_outputs(const ActorDecl& d, uint16_t node, uint16_t peer, NsDecl* out, size_t cap);
extern const ActorKind kGuestKind;

}  // namespace pot
