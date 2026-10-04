// §5.3.1: the CAN (TWAI, classic) transport profile -- M4. Portable and host-tested; the TWAI driver
// glue is can_port.cpp.
//
// A Potluck Frame travels over classic CAN in one of two encodings, and the decoder reconstructs the
// same bytes either way, so everything above L1 sees an ordinary frame:
//
// SINGLE FRAME -- for the messages §5.4 forbids from fragmenting. The routing header rides in the
// 29-bit extended ID and the 8 data bytes are the whole payload:
//
//    [28:24] priority, INVERTED (31 - p): CAN arbitration is won by the lowest ID, so Potluck's
//            highest priority must encode lowest. SAFE_STATE (31) encodes 0 and wins the bus.
//    [23:17] opcode     §5.2 opcodes are all <= 0x7F
//    [16:11] src alias  6 bits, see can_alias_of()
//    [10:5]  dst alias  0x3F = broadcast
//    [4]     SINGLE = 1
//    [3:0]   0
//
// Only a frame with nothing the ID cannot carry qualifies: payload <= 8, seq 0, msg_id 0, no
// fragment, no ACKREQ, no AUTH, and the class this opcode implies (can_single_frame_class). The
// receiver rebuilds the header with pot_frame's own encoder, so it is byte-identical by construction.
// The src node id comes from the alias table, which beacons fill: an 8-byte beacon names its sender
// in its first two bytes.
//
// SEGMENTED -- everything else. The whole encoded Potluck Frame, header included, is cut into 7-byte
// pieces behind one segmentation byte, [7] FIRST, [6] LAST, [5:0] index; the ID has SINGLE = 0 and
// the index's low 4 bits. A 16-byte control frame is 3 CAN frames and a 64-byte one is 10, as §3.1
// counts. 64 indexes x 7 bytes caps a message at 448 bytes, so CAN peers use the 226-byte v1
// payload profile (242 bytes encoded, 35 CAN frames).

#pragma once

#include <cstddef>
#include <cstdint>

namespace pot {

constexpr uint8_t kCanBroadcastAlias = 0x3F;
constexpr size_t kCanSegData = 7;
constexpr size_t kCanMaxSegments = 64;
constexpr size_t kCanMaxMessage = kCanSegData * kCanMaxSegments;  // 448
constexpr size_t kCanReassemblySlots = 4;                          // §5.4's per-peer cap, used cell-wide
constexpr uint32_t kCanReassemblyTimeoutMs = 1500;                 // §5.4: 3x L3's 500 ms

// v1 aliases are derived, not assigned: node_id mod 63, so 0..62 and never the broadcast 0x3F. Two
// nodes can collide; the alias table reports it. Enrolment (M5) assigns aliases properly.
uint8_t can_alias_of(uint16_t node_id);

// The node core keys peers by a 6-byte address. A CAN peer has none, so it gets a reserved one,
// 02:CA:00:00:<node id>, the same way the serial host is kHostMac: hal_send routes by it.
void can_mac_of(uint16_t node_id, uint8_t mac[6]);
bool can_mac_node(const uint8_t mac[6], uint16_t& node_id);

struct CanId {
    uint8_t priority;  // Potluck priority 0..31; packed inverted
    uint8_t opcode;
    uint8_t src;
    uint8_t dst;
    bool single;
    uint8_t index;  // low 4 bits of the segment index when !single
};
uint32_t can_id_pack(const CanId& id);
CanId can_id_unpack(uint32_t raw);

struct CanFrame {
    uint32_t id;  // 29-bit extended
    uint8_t dlc;
    uint8_t data[8];
};

// The class a single-frame message of this opcode must carry. False if the opcode never travels as
// a single frame.
bool can_single_frame_class(uint8_t opcode, uint8_t& lclass);

// Encode one already-encoded Potluck Frame into CAN frames. Returns the number written, or 0 if it
// does not fit `max_out` or the 448-byte cap.
size_t can_encode(const uint8_t* raw, size_t len, uint8_t src_alias, uint8_t dst_alias, CanFrame* out,
                  size_t max_out);

class CanAliasTable {
  public:
    // Learn alias -> node from a beacon. Returns false (and counts it) when the alias already names a
    // different node: two nodes on one bus with the same derived alias.
    bool learn(uint8_t alias, uint16_t node_id);
    bool lookup(uint8_t alias, uint16_t& node_id) const;
    uint32_t conflicts() const { return conflicts_; }

  private:
    uint16_t node_[64] = {};
    bool known_[64] = {};
    uint32_t conflicts_ = 0;
};

// Transmit order (M4 bench, 2026-10-04). The TWAI driver sends frames in the order they are queued,
// so on the two-board bench a SAFE_STATE queued behind its own node's 10-frame probe took 2.5-5.5 ms
// to leave under flood: the bus was never the bottleneck, the node's own queue was. A node therefore
// hands the controller one frame at a time and picks it here:
//  - lowest packed priority first (the ID's top five bits, as arbitration would), FIFO among equals;
//  - a single frame may go between another message's segments (the receiver handles it apart);
//  - a segmented message, once started, finishes before another segmented one starts, because a
//    receiver reassembles one message per sender (CanReassembler).
// So a SAFE_STATE waits for the frame already in the controller and whatever other nodes win, never
// for the rest of its own node's queue.
struct CanTxMeta {
    uint32_t submit_us;
    uint8_t dst_mac[6];
};

struct CanTxItem {
    CanFrame f;
    CanTxMeta meta;
    bool last;  // the final CAN frame of its Potluck frame
};

class CanTxQueue {
  public:
    static constexpr size_t kCapacity = kCanMaxSegments + 8;  // one whole message plus a few singles

    // Queue all of one Potluck frame's CAN frames, or none of them. False when they do not fit.
    bool push(const CanFrame* f, size_t n, const CanTxMeta& meta);
    // The frame to send next, removed from the queue. False when there is none.
    bool pop(CanTxItem& out);
    size_t free() const { return kCapacity - used_; }
    size_t size() const { return used_; }

  private:
    struct Entry {
        CanTxItem item;
        uint32_t seq;  // message order
        uint8_t part;  // index within its message
        bool single;
        bool used;
    };
    static bool before(const Entry& a, const Entry& b);

    Entry e_[kCapacity] = {};
    size_t used_ = 0;
    uint32_t next_seq_ = 0;
    bool in_progress_ = false;  // a segmented message has frames out and frames still queued
    uint32_t in_progress_seq_ = 0;
};

struct CanRxCounters {
    uint32_t frames;
    uint32_t singles;
    uint32_t messages;          // complete Potluck frames delivered
    uint32_t not_for_us;        // dst alias neither ours nor broadcast
    uint32_t unknown_alias;     // a single frame from a sender no beacon has named yet
    uint32_t out_of_order;      // a segment that did not continue its message
    uint32_t no_slot;           // more concurrent reassemblies than kCanReassemblySlots
    uint32_t timeouts;          // a partial message abandoned
    uint32_t malformed;
};

class CanReassembler {
  public:
    explicit CanReassembler(uint8_t my_alias) : my_alias_(my_alias) {}

    // Feed one received CAN frame. When it completes a Potluck Frame, writes the frame's bytes to
    // `out` and returns their length; otherwise 0.
    size_t feed(const CanFrame& f, uint32_t now_ms, uint8_t* out, size_t cap);
    void expire(uint32_t now_ms);

    CanAliasTable& aliases() { return aliases_; }
    const CanRxCounters& counters() const { return c_; }

  private:
    struct Slot {
        bool used;
        uint8_t src;
        uint8_t next;
        uint32_t started_ms;
        size_t len;
        uint8_t buf[kCanMaxMessage];
    };
    size_t single(const CanFrame& f, const CanId& id, uint8_t* out, size_t cap);

    uint8_t my_alias_;
    CanAliasTable aliases_;
    Slot slots_[kCanReassemblySlots] = {};
    CanRxCounters c_{};
};

}  // namespace pot
