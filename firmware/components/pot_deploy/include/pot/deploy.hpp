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
// ---------------------------------------------------------------------------------------------

constexpr uint32_t kImageMagic = 0x44544F50u;  // "POTD"
constexpr uint8_t kImageVersion = 1;
constexpr size_t kImageHeaderLen = 24;
constexpr size_t kMaxActors = 16;
// The largest image the format can hold today is 24 + 16 x (4 + 6) = 184 bytes. 512 leaves room
// for larger actor configs without spending §6 core DRAM on buffers nothing can fill: the first
// cut was 2048 here, which cost 4 KB of committed headroom for two buffers 90 % empty.
constexpr size_t kMaxImageLen = 512;
constexpr uint16_t kEveryNode = 0xFFFF;

enum class ActorType : uint8_t {
    Led = 1,    // the status LED's healthy-state appearance
    Fault = 2,  // test actor: aborts the node after a delay, to exercise trial-and-revert
};

struct ActorDecl {
    uint16_t node_id;
    ActorType type;
    uint8_t cfg_len;
    const uint8_t* cfg;  // points into the image buffer
};

struct DeployImage {
    uint32_t rollback_counter;
    uint8_t package_digest[8];
    uint8_t actor_count;
    ActorDecl actors[kMaxActors];
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
};

}  // namespace pot
