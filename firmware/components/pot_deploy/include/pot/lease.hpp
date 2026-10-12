// M14: the cell's leases -- one resident hub per cell, one `potluck apply` at a time (ARCHITECTURE section 13).
//
// Two PCs, each on a cable to a different board, must not both deploy (they would race for one rollback counter)
// and must not both run a resident hub (another system would see the cell twice). The arbiter is in the cell: the
// lowest-id live board holds a small table of named leases. A PC asks the board on its cable (LEASE, 0x39, inside
// the deploy range, answered by the deploy server like NODE_STATUS); target 0 lets that board pick the arbiter
// itself -- the lowest node id among itself and the live boards it hears -- and pass the request on over the radio,
// so two PCs never have to agree on who is alive. A lease is held by a token (random, the holder's) for a time to
// live the holder renews at a third of it; a request for a lease someone else holds is refused with the holder's
// name and how long it has left.
//
// What a lease is not: it is advisory -- a deploy without one is not refused by the boards (old tools keep working);
// `potluck apply` and the hub take theirs before changing anything. It lives in RAM: an arbiter that reboots, or a
// cell whose lowest board changes, forgets its leases, and the next holder to renew takes it there (renewals at a
// third of the time to live make that window short; two claimants inside it are the stated limit).
//
// Wire, little-endian.
//   request:  u16 target (0 = the arbiter), u8 kind (kLease*), u8 op (kLeaseTake/Release/Query), u16 ttl_s
//             (1..kLeaseMaxTtlS, take only), u32 token (non-zero), u8 name_len (1..kLeaseNameMax, take only), name
//   reply:    u8 status (LeaseStatus), u8 kind, u16 arbiter (the board that answered), u32 remaining_ms,
//             u32 holder token (0 = free), u8 name_len, name (the holder's)

#pragma once

#include <cstddef>
#include <cstdint>

namespace pot {

constexpr uint8_t kLeaseDeploy = 1;  // `potluck apply` -- a deploy or firmware rollout under way
constexpr uint8_t kLeaseHub = 2;     // a resident potluck-hub
constexpr uint8_t kLeaseKinds = 2;

constexpr uint8_t kLeaseTake = 1;  // take, or renew one this token holds
constexpr uint8_t kLeaseRelease = 2;
constexpr uint8_t kLeaseQuery = 3;

constexpr size_t kLeaseNameMax = 31;
constexpr uint16_t kLeaseMaxTtlS = 600;
// Host ids sit below this (the bridge's and every enrolled host's 0x00fe): a host never arbitrates.
constexpr uint16_t kLeaseMinBoardId = 0x0100;
constexpr size_t kLeaseRequestMaxLen = 11 + kLeaseNameMax;
constexpr size_t kLeaseReplyMaxLen = 13 + kLeaseNameMax;

enum class LeaseStatus : uint8_t {
    Ok = 0,           // taken, renewed, released, or (query) here is what holds it
    Held = 1,         // someone else holds it: the reply names them
    Malformed = 2,    // a field out of range
    Unreachable = 3,  // the arbiter the cabled board chose did not answer
};

const char* lease_status_str(LeaseStatus s);
const char* lease_kind_str(uint8_t kind);

struct LeaseRequest {
    uint16_t target = 0;
    uint8_t kind = 0;
    uint8_t op = 0;
    uint16_t ttl_s = 0;
    uint32_t token = 0;
    uint8_t name_len = 0;
    char name[kLeaseNameMax + 1] = {};
};

struct LeaseReply {
    LeaseStatus status = LeaseStatus::Malformed;
    uint8_t kind = 0;
    uint16_t arbiter = 0;
    uint32_t remaining_ms = 0;
    uint32_t token = 0;
    uint8_t name_len = 0;
    char name[kLeaseNameMax + 1] = {};
};

// Parses the fixed fields and the name; ranges are judged by LeaseTable::handle (a malformed request still gets a
// reply). False only when the bytes are too short to be a request.
bool load_lease_request(const uint8_t* p, size_t len, LeaseRequest& out);
size_t store_lease_request(const LeaseRequest& r, uint8_t* out, size_t cap);
bool load_lease_reply(const uint8_t* p, size_t len, LeaseReply& out);
size_t store_lease_reply(const LeaseReply& r, uint8_t* out, size_t cap);

// The arbiter's table: one entry per kind. Not thread-safe; the deploy server calls it with the node's lock held.
class LeaseTable {
  public:
    LeaseReply handle(const LeaseRequest& req, uint32_t now_ms, uint16_t self);
    // The holder's name, "" when free or expired (for the console's stats line).
    const char* holder(uint8_t kind, uint32_t now_ms) const;

  private:
    struct Entry {
        uint32_t token = 0;  // 0 = free
        uint32_t expires_ms = 0;
        uint8_t name_len = 0;
        char name[kLeaseNameMax + 1] = {};
    };
    bool live(const Entry& e, uint32_t now_ms) const {
        return e.token != 0 && static_cast<int32_t>(e.expires_ms - now_ms) > 0;
    }
    Entry e_[kLeaseKinds];
};

// The arbiter a board picks for target 0: the lowest id among `self` and `alive` (the live peers' ids), hosts
// (ids below kLeaseMinBoardId) left out.
uint16_t lease_arbiter(uint16_t self, const uint16_t* alive, size_t n);

}  // namespace pot
