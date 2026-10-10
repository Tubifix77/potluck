// M11: firmware over the cell -- deploy-and-detach for code (ARCHITECTURE section 13).
//
// A signed native firmware image travels the way a package does (section 7.4), into the spare of two
// firmware slots, and boots on trial; the board keeps it only if it proves itself, and ESP-IDF's
// bootloader falls back to the previous image otherwise. This file is the portable half: the wire
// payloads and the receiving state machine. Where the bytes go (ESP-IDF's OTA API on a board, memory in
// the tests) is an FwSink; how the signature is checked (pot_trust's fw_trailer_check, with the
// cluster's CA) is a callback, so this component needs no knowledge of either.
//
// The wire: FW_BEGIN / FW_CHUNK / FW_COMMIT / FW_STATUS (pot/opcodes.hpp), each answered with a REPLY
// carrying FwReply. Every request names its target node: the board the host is cabled to answers for
// itself, and passes a request for any other node on to it, returning that node's reply -- so the image
// is never stored on the way, and the host drives the rollout one node at a time.
//
//   FW_BEGIN   target u16, image_len u32, counter u32, trailer (176: deploy certificate + signature)
//   FW_CHUNK   target u16, offset u32, len u16, data
//   FW_COMMIT  target u16
//   FW_STATUS  target u16

#pragma once

#include <cstddef>
#include <cstdint>

#include "monocypher-ed25519.h"

namespace pot {

constexpr size_t kFwTrailerLen = 176;  // pot_trust's kImageTrailerLen
constexpr size_t kFwBeginLen = 2 + 4 + 4 + kFwTrailerLen;
constexpr size_t kFwChunkHeaderLen = 2 + 4 + 2;
constexpr size_t kFwChunkMax = 1024;  // per frame; the host sends what its link carries
constexpr size_t kFwTargetLen = 2;    // COMMIT and STATUS: the target alone
constexpr size_t kFwVersionLen = 32;  // esp_app_desc_t's version field

enum class FwStatus : uint16_t {
    Ok = 0,
    TooLarge = 1,      // larger than the spare slot
    Downgrade = 2,     // counter not above the highest this node has confirmed
    NotStarted = 3,    // CHUNK or COMMIT without a BEGIN
    BadOffset = 4,     // chunks arrive in order, without gaps
    Malformed = 5,     // payload too short for its opcode
    Busy = 6,          // on trial with a new image, or about to reboot into one
    BadSignature = 7,  // the deploy certificate or the signature did not verify
    StoreFailed = 8,   // the slot would not open or take the bytes
    BadImage = 9,      // complete, signed, and still not a valid image for this chip (the sink said so)
    Unreachable = 10,  // the board passing it on could not reach the target
    Incomplete = 11,   // COMMIT before every byte arrived
};
const char* fw_status_str(FwStatus s);

enum class FwState : uint8_t {
    Confirmed = 0,  // running an image that passed its trial (or the one flashed by cable)
    OnTrial = 1,    // running a new image that has not yet proven itself
    Factory = 2,    // running the image flashed by cable; never updated over the air
};

struct FwBegin {
    uint16_t target;
    uint32_t image_len;
    uint32_t counter;
    const uint8_t* trailer;  // kFwTrailerLen
};
struct FwChunk {
    uint16_t target;
    uint32_t offset;
    uint16_t len;
    const uint8_t* data;
};

struct FwReply {
    FwStatus status = FwStatus::Ok;
    uint16_t node = 0;          // who answered
    uint32_t received = 0;      // bytes of the image in progress
    uint32_t running = 0;       // counter of the running image (0: the cable-flashed one)
    uint32_t floor = 0;         // highest counter this node has confirmed: anything at or below it is refused
    FwState state = FwState::Factory;
    char version[kFwVersionLen] = {};  // the running image's version string
};
constexpr size_t kFwReplyLen = 2 + 2 + 4 + 4 + 4 + 1 + kFwVersionLen;  // 49

size_t store_fw_begin(const FwBegin& b, uint8_t* out, size_t cap);
size_t store_fw_chunk(const FwChunk& c, uint8_t* out, size_t cap);
size_t store_fw_target(uint16_t target, uint8_t* out, size_t cap);
size_t store_fw_reply(const FwReply& r, uint8_t* out, size_t cap);
bool load_fw_begin(const uint8_t* p, size_t len, FwBegin& out);
bool load_fw_chunk(const uint8_t* p, size_t len, FwChunk& out);
bool load_fw_target(const uint8_t* p, size_t len, uint16_t& target);
bool load_fw_reply(const uint8_t* p, size_t len, FwReply& out);

// Where the image goes. On a board: ESP-IDF's OTA API on the slot not booted from.
class FwSink {
  public:
    virtual ~FwSink() = default;
    virtual uint32_t capacity() const = 0;               // the spare slot's size
    virtual bool begin(uint32_t image_len) = 0;
    virtual bool write(const uint8_t* data, size_t len) = 0;  // in order: the receiver checks offsets
    virtual bool finish() = 0;    // the image is complete: validate it (on a board, esp_ota_end)
    virtual bool activate() = 0;  // boot it next (esp_ota_set_boot_partition); the caller then reboots
    virtual void abort() = 0;
};

// Checks the trailer against the signed message for (counter, image_len, sha512). True if it verifies.
using FwVerifyFn = bool (*)(void* ctx, uint32_t counter, uint32_t image_len, const uint8_t sha512[64],
                            const uint8_t* trailer);

// One image at a time. begin() refuses a downgrade and a busy node; chunk() takes bytes in order and
// hashes them; commit() checks the length, the signature over (counter, length, SHA-512), then the
// image itself, and activates it. Nothing is activated unless every check passed.
class FwReceiver {
  public:
    FwReceiver(FwSink& sink, FwVerifyFn verify, void* verify_ctx) : sink_(sink), verify_(verify), vctx_(verify_ctx) {}

    void set_floor(uint32_t floor) { floor_ = floor; }
    void set_busy(bool busy) { busy_ = busy; }  // on trial: a second image would overwrite the fallback

    FwStatus begin(const FwBegin& b);
    FwStatus chunk(const FwChunk& c);
    FwStatus commit();
    void abort();

    bool in_progress() const { return active_; }
    uint32_t received() const { return received_; }
    uint32_t counter() const { return counter_; }  // of the image in progress, or the one committed last

  private:
    FwSink& sink_;
    FwVerifyFn verify_;
    void* vctx_;
    uint32_t floor_ = 0;
    bool busy_ = false;
    bool active_ = false;
    uint32_t image_len_ = 0;
    uint32_t counter_ = 0;
    uint32_t received_ = 0;
    uint8_t trailer_[kFwTrailerLen] = {};
    crypto_sha512_ctx sha_{};
};

}  // namespace pot
