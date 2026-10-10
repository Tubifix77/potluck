// §7.4 deploy and detach, the node's half -- M3.
//
// WHAT A MODULE IS HERE. ADR-003: native actors are the default runtime and WASM is a gated tier, so
// "changeable behaviour" is "a signed configuration and a state machine". A deployed image is
// therefore not code. It is a list of actor instances -- which built-in native actor, on which
// node, configured how -- and the firmware already contains every actor it can name. Swapping the
// image changes what a node does without changing what it runs.
//
// WHAT IS NOT HERE YET. The image records which signed package it came from (an 8-byte digest) and
// its anti-downgrade counter, but the node does not verify the package signature: that is M5's
// "node's side", already scoped there in the README. The CRC below is integrity against a torn or
// corrupted write, not authenticity.
//
// STORAGE. §7.4 says "LittleFS partition, A/B slots". A slot holds exactly one small blob, which
// LittleFS would wrap in a filesystem for nothing, and LittleFS is not part of ESP-IDF. So the slots
// are two raw data partitions with a header; the A/B semantics, the trial and the revert are as
// written. When a package grows multiple files this is the line to revisit.
//
// Everything in this header is portable and host-tested. The flash/NVS backend is deploy_esp.cpp.

#pragma once

#include <cstddef>
#include <cstdint>

namespace pot {

uint32_t crc32(const uint8_t* data, size_t len, uint32_t seed = 0);

// ---------------------------------------------------------------------------------------------
// The image: a header, then actor declarations. Little-endian throughout, like every Potluck
// payload.
//
//   offset size  field
//    0      4    magic 'POTD'
//    4      1    version (1)
//    5      1    actor_count
//    6      2    reserved (0)
//    8      4    rollback_counter    §7.4 step 6: monotonic, refuses anything lower
//   12      8    package_digest      first 8 bytes of SHA-256 over the signed package (traceability)
//   20      4    body_len            bytes of actor declarations that follow
//   24      ...  actor declarations: node_id u16 (0xFFFF = every node), type u8, cfg_len u8, cfg
//
// Version 2 (M7) adds, after the declarations, the guest section -- the WebAssembly programs that
// guest actors (ActorType::Guest) run, each opaque here and parsed by pot_wasm:
//          1    guest_count
//          ...  guest_count x (len u32, blob)
// A version-1 image is a version-2 image with no guest section; both are accepted.
// ---------------------------------------------------------------------------------------------

constexpr uint32_t kImageMagic = 0x44544F50u;  // "POTD"
constexpr uint8_t kImageVersion = 1;
constexpr uint8_t kImageVersionGuests = 2;  // M7
constexpr size_t kImageHeaderLen = 24;
constexpr size_t kMaxActors = 16;
// Without guests an image is at most 24 + 16 x (4 + 255) bytes, and usually under 200. M7's guests
// carry their code, so the limit is 32 KB: the receive buffer lives in PSRAM on the boards (the first
// cut, 2048 B in internal RAM, had cost 4 KB of committed headroom for buffers 90 % empty), and a slot
// partition holds 64 KB.
constexpr size_t kMaxImageLen = 32 * 1024;
constexpr size_t kMaxGuests = 4;
constexpr uint16_t kEveryNode = 0xFFFF;
// M6 (section 7.7): placed at run time by the reconciler, among the nodes the actor's config lists.
constexpr uint16_t kPortableNode = 0xFFFE;

enum class ActorType : uint8_t {
    Led = 1,    // the status LED's healthy-state appearance
    Fault = 2,  // test actor: aborts the node after a delay, to exercise trial-and-revert
    Ticker = 3, // M6: publishes a counter to one resource; portable, so the reconciler places it
    SvcClient = 4,  // M8: calls a host's named service (section 7.5) and publishes what it answers
    DieTemp = 5,    // M8.1: the chip's own temperature sensor, published to the namespace
    McLender = 6,   // M9: lends this node's idle cores to a pure function other nodes call
    McJob = 7,      // M9: a Monte Carlo job that runs its units here and borrows idle cores elsewhere
    Guest = 8,      // M7: a sandboxed WebAssembly program from the image's guest section (pot_wasm)
};

// M8.2 (PS-1): 0x00-0x7F are Potluck's built-ins; 0x80-0xFF belong to external components (an
// application's actors in its own repository), allocated in blocks of 16 by ARCHITECTURE section 7.
// The image format carries an external type's config opaquely: only the build's registration row
// for that type can judge it.
constexpr uint8_t kFirstExternalType = 0x80;
inline bool is_external_type(ActorType t) { return static_cast<uint8_t>(t) >= kFirstExternalType; }

struct ActorDecl {
    uint16_t node_id;
    ActorType type;
    uint8_t cfg_len;
    const uint8_t* cfg;  // points into the image buffer
};

struct GuestBlob {
    const uint8_t* data;  // points into the image buffer
    uint32_t len;
};

struct DeployImage {
    uint32_t rollback_counter;
    uint8_t package_digest[8];
    uint8_t actor_count;
    ActorDecl actors[kMaxActors];
    uint8_t guest_count = 0;  // M7
    GuestBlob guests[kMaxGuests] = {};
};

// Parses and validates. `why` names the first problem, for the log. Unknown actor types are
// rejected: a node that silently skipped an actor it does not have would report a deployment it
// is not running.
bool parse_image(const uint8_t* data, size_t len, DeployImage& out, const char** why);

// LED: what "healthy" looks like. Blue/yellow/red keep their diagnostic meaning; only the healthy
// state is configurable, so a deployed behaviour can never hide a dead link.
struct LedConfig {
    uint8_t r, g, b;
    uint8_t blink;        // 0 = steady, 1 = blink
    uint16_t period_ms;   // full blink period, 200..10000
};
constexpr uint8_t kLedCfgLen = 6;
bool led_config(const ActorDecl& a, LedConfig& out);

// Fault: abort after this many ms of uptime. It must fire inside the trial window, or a node could
// confirm a module that then crashes forever with no slot left to fall back to; the caller checks
// that against its own window.
struct FaultConfig {
    uint32_t panic_after_ms;
};
constexpr uint8_t kFaultCfgLen = 4;
bool fault_config(const ActorDecl& a, FaultConfig& out);

// Ticker (M6): the smallest actor that serves reads. While running it publishes, every period_ms,
// a u32 to the resource `out_hash`: this node's id in the high 16 bits and a count that restarts at
// every activation in the low 16 -- section 7.7: "re-activation restarts from the actor's declared
// initial state". So a reader sees which instance answered, and that it is a fresh one.
//
// A ticker is always portable: its declaration's node_id is kPortableNode and its config lists the
// eligible nodes with a data-gravity score each (section 7.4), frozen by the build. A pinned ticker
// is one with a single eligible node. `out_hash` doubles as the actor's identity for rendezvous
// hashing and for claims, so two tickers cannot share an output.
//
//   out_hash u32, period_ms u16, count u8, then count x (node_id u16, gravity u8)
constexpr size_t kMaxEligible = 8;
struct TickerConfig {
    uint32_t out_hash;
    uint16_t period_ms;  // 50..60000
    uint8_t count;       // 1..kMaxEligible
    uint16_t node[kMaxEligible];
    uint8_t gravity[kMaxEligible];  // higher wins; equal scores fall to the rendezvous hash
};
constexpr uint8_t kTickerCfgFixed = 7;
bool ticker_config(const ActorDecl& a, TickerConfig& out);

// M8.2 (PS-1): the standard portable-actor header that starts a portable actor's config -- the
// ticker's layout above (out_hash u32, period_ms u16, count u8, count x (node_id u16, gravity u8)),
// which any portable type, built-in or external, now shares. Fills `out` and `len` (the header's
// size: the actor's own config begins there). False if malformed. No type check: the caller's.
bool portable_header(const ActorDecl& a, TickerConfig& out, size_t& len);
// The inverse, for code that holds a TickerConfig and needs its declaration bytes (tests, and the
// reconciler's TickerConfig overload). Returns the length written, 0 if `cap` is too small.
size_t encode_ticker_config(const TickerConfig& c, uint8_t* out, size_t cap);

// SvcClient (M8, section 7.5): every period_ms, CALL the service `svc_hash` on node `provider` -- a
// host's potluck-agent -- and publish the answer to `out_hash`, a resource this node owns. Pinned:
// node_id names the node it runs on.
//
// on_host_loss (section 8.3) is the manifest's policy for when the provider is gone. A service client
// owns no actuator, so the menu's stop and safe-state entries do not apply to it, and its two meanings
// are the same mechanism: it stops publishing, its output keeps its last value with its true age, and
// past the staleness bound every read of it says STALE -- never a host's answer presented as fresh.
// It keeps trying, and the next answer after the provider returns is published at once.
//
//   svc_hash u32, provider u16, out_hash u32, period_ms u16, on_host_loss u8 (0 continue, 1 hold)
constexpr uint8_t kSvcClientCfgLen = 13;
struct SvcClientConfig {
    uint32_t svc_hash;
    uint16_t provider;
    uint32_t out_hash;
    uint16_t period_ms;  // 100..60000
    uint8_t on_host_loss;
};
bool svc_client_config(const ActorDecl& a, SvcClientConfig& out);

// DieTemp (M8.1): every period_ms, read the chip's internal temperature sensor and publish it, in
// degrees Celsius as an f32, to `out_hash` -- a resource this node owns. ESP-IDF says the sensor
// measures "the temperature inside the silicon" and is "not recommended" for ambient use, so the
// resource is the die's temperature, not the room's. A failed read publishes nothing new and marks
// the resource FAULTY: a reader gets no number rather than an old one. Pinned.
//
//   out_hash u32, period_ms u16
constexpr uint8_t kDieTempCfgLen = 6;
struct DieTempConfig {
    uint32_t out_hash;
    uint16_t period_ms;  // 200..60000
};
bool die_temp_config(const ActorDecl& a, DieTempConfig& out);

// McLender (M9): accept units of the pure function mc_hits (pot/mc.hpp) from other nodes and run them
// on this node's background workers -- at most `slots` at once; a unit arriving with every slot busy
// is refused at once (BUSY), so the caller runs it elsewhere instead of waiting. Every node or pinned.
//
//   slots u8 (1..4)
constexpr uint8_t kMcLenderCfgLen = 1;
struct McLenderConfig {
    uint8_t slots;
};
bool mc_lender_config(const ActorDecl& a, McLenderConfig& out);

// McJob (M9): estimate pi by Monte Carlo in `units` units of `samples` samples each, unit i seeded
// from seed + i, so the result is the same wherever each unit ran. Runs units on this node's own
// background workers and, when `lend` is 1, sends a unit to a peer's McLender when waiting for a
// local worker would take longer than the peer's round trip plus its compute time. Publishes the
// estimate (f32) to `out_hash` when the job ends. Starts `start_after_ms` after boot, or on the
// console ("POT! mc run 0|1") when that is 0. Pinned.
//
//   out_hash u32, units u16 (1..4096), samples u32 (1000..100000000), seed u32, lend u8 (0..1),
//   start_after_ms u32
constexpr uint8_t kMcJobCfgLen = 19;
struct McJobConfig {
    uint32_t out_hash;
    uint16_t units;
    uint32_t samples;
    uint32_t seed;
    uint8_t lend;
    uint32_t start_after_ms;
};
bool mc_job_config(const ActorDecl& a, McJobConfig& out);

// ---------------------------------------------------------------------------------------------
// The A/B state machine, persisted in NVS.
// ---------------------------------------------------------------------------------------------

constexpr uint8_t kSlotNone = 0;
constexpr uint8_t kSlotA = 1;
constexpr uint8_t kSlotB = 2;

struct DeployState {
    uint8_t active = kSlotNone;    // the slot this node runs
    uint8_t previous = kSlotNone;  // the last confirmed slot, the revert target while active is pending
    uint8_t pending = 0;           // 1 while active is on trial
    uint8_t trial_boots = 0;       // boots spent on the current trial
    uint32_t counter = 0;          // highest rollback counter ever committed
};

enum class BootOutcome : uint8_t {
    NoDeployment,  // nothing deployed: the firmware's built-in behaviour
    Confirmed,     // run the active slot
    Trial,         // run the active slot, still on trial; state.trial_boots was incremented
    Reverted,      // the trial used its boots up: state now points at the previous slot
};

const char* boot_outcome_str(BootOutcome o);

// Decide what to boot, mutating `s` for the caller to persist BEFORE running anything -- a trial
// boot that crashes must already have been counted, or a crash loop would never revert.
BootOutcome decide_boot(DeployState& s, uint8_t max_trial_boots);

// The active slot failed to load or to start: revert now rather than spend the remaining boots.
void revert_now(DeployState& s);

// The trial passed.
void confirm(DeployState& s);

// ---------------------------------------------------------------------------------------------
// Storage seam. deploy_esp.cpp implements it on partitions + NVS; tests use RAM.
// ---------------------------------------------------------------------------------------------

class SlotStore {
  public:
    virtual ~SlotStore() = default;
    virtual bool load_state(DeployState& out) = 0;
    virtual bool save_state(const DeployState& s) = 0;
    // Writes `len` bytes as the slot's content, with a header carrying length and CRC.
    virtual bool write_slot(uint8_t slot, const uint8_t* data, size_t len) = 0;
    // Reads the slot's content back, verifying the CRC. False if empty, torn or corrupt.
    virtual bool read_slot(uint8_t slot, uint8_t* buf, size_t cap, size_t& len) = 0;
};

// ---------------------------------------------------------------------------------------------
// The wire: DEPLOY_BEGIN / CHUNK / COMMIT / ABORT, each answered with a REPLY carrying DeployReply.
// ---------------------------------------------------------------------------------------------

constexpr uint8_t kDeployFlagDistribute = 0x01;  // the receiver passes the image on to its peers

struct DeployBegin {
    uint32_t image_len;
    uint32_t image_crc;
    uint32_t rollback_counter;
    uint8_t package_digest[8];
    uint8_t flags;
};
constexpr size_t kDeployBeginLen = 24;

struct DeployChunk {
    uint32_t offset;
    uint16_t len;
    const uint8_t* data;
};
constexpr size_t kDeployChunkHeaderLen = 6;
constexpr size_t kDeployChunkMax = 512;

constexpr size_t kDeployCommitLen = 4;  // image_crc again: the receiver commits only what it checked

enum class DeployStatus : uint16_t {
    Ok = 0,
    TooLarge = 1,
    Downgrade = 2,      // rollback_counter below the highest ever committed
    NotStarted = 3,     // CHUNK or COMMIT without a BEGIN
    BadOffset = 4,      // chunks must arrive in order, without gaps
    CrcMismatch = 5,
    BadImage = 6,       // parsed, and is not a valid image
    TrialInProgress = 7,// the active slot is still on trial; deploying over its fallback is refused
    StoreFailed = 8,
    Malformed = 9,      // payload too short for its opcode
    Busy = 10,          // already passing an image on to peers, or about to reboot into one
    Unsigned = 11,      // M5: this node checks signatures and the image carried none
    BadSignature = 12,  // M5: the deploy certificate or the image signature did not verify
};
const char* deploy_status_str(DeployStatus s);

struct DeployReply {
    DeployStatus status;
    uint8_t slot;          // the slot written (COMMIT) or targeted
    uint8_t pending;       // 1 if that slot is now on trial
    uint32_t received;     // bytes received so far
    uint32_t counter;      // rollback counter of the image
    uint8_t peers_ok;      // COMMIT with distribution: peers that committed
    uint8_t peers_failed;  // ... and peers that did not
};
constexpr size_t kDeployReplyLen = 16;

bool load_deploy_begin(const uint8_t* p, size_t len, DeployBegin& out);
bool load_deploy_chunk(const uint8_t* p, size_t len, DeployChunk& out);
bool load_deploy_commit(const uint8_t* p, size_t len, uint32_t& crc);
size_t store_deploy_begin(const DeployBegin& b, uint8_t* out, size_t cap);
size_t store_deploy_chunk(uint32_t offset, const uint8_t* data, uint16_t len, uint8_t* out, size_t cap);
size_t store_deploy_commit(uint32_t crc, uint8_t* out, size_t cap);
size_t store_deploy_reply(const DeployReply& r, uint8_t* out, size_t cap);
bool load_deploy_reply(const uint8_t* p, size_t len, DeployReply& out);

// ---------------------------------------------------------------------------------------------
// The receiving side of one deployment.
// ---------------------------------------------------------------------------------------------

class DeployReceiver {
  public:
    DeployReceiver(SlotStore& store, uint8_t* buf, size_t cap) : store_(store), buf_(buf), cap_(cap) {}

    DeployReply begin(const DeployBegin& b);
    DeployReply chunk(const DeployChunk& c);
    // Verifies, writes the inactive slot, and marks it active-and-pending in the persisted state.
    // The caller reboots afterwards; the trial runs on the next boot.
    DeployReply commit(uint32_t crc);
    void abort();

    // M5 step 6: check the received stream before anything is written. Called at COMMIT with the
    // image (its length from its own header) and whatever followed it -- the signature trailer, or
    // nothing. Return Ok to proceed, or the status to refuse with. Null: no check (an unenrolled
    // node, or a host test); any trailer is then ignored.
    using Verifier = DeployStatus (*)(void* ctx, const uint8_t* image, size_t image_len, const uint8_t* trailer,
                                      size_t trailer_len);
    void set_verifier(Verifier fn, void* ctx) {
        verifier_ = fn;
        verifier_ctx_ = ctx;
    }

    bool in_progress() const { return started_; }
    bool distribute() const { return (begin_.flags & kDeployFlagDistribute) != 0; }
    const DeployBegin& header() const { return begin_; }
    const uint8_t* image() const { return buf_; }
    size_t image_len() const { return received_; }

  private:
    DeployReply reply(DeployStatus s) const;

    SlotStore& store_;
    uint8_t* buf_;
    size_t cap_;
    DeployBegin begin_{};
    size_t received_ = 0;
    bool started_ = false;
    uint8_t target_ = kSlotNone;
    bool committed_ = false;
    Verifier verifier_ = nullptr;
    void* verifier_ctx_ = nullptr;
};

}  // namespace pot
