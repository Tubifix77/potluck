// M25: the bulk channel -- a file the PC offers, read by a board's app in chunks over the existing frames (ADR-001: no
// new transport).
//
// The app CALLs the PC's file service (`potluck://<cluster>/svc/file`, the cluster file's pc_services and bulk.folder)
// with a request, and the answer is the file's size (or kBulkNoFile). The hub then pushes the file to the app's input
// (act/<actor>/in) as CALLs, through the board on the PC's cable (ns_payloads.hpp kCallForward), a few in flight at a
// time. The app takes a chunk only at the offset it expects next and answers every chunk with that offset -- a
// cumulative acknowledgement: the hub sends again from it when the answers stop moving (go-back-N).
//
//   request (CALL args to svc/file):  op u8 (kBulkOpen), in_hash u32 (the app's input), name (1..64 bytes)
//   answer (REPLY value, u32):        the file's size in bytes, or kBulkNoFile
//   chunk (CALL args to act/<actor>/in): version u8 (kBulkVersion), xfer u16, offset u32, data (1..kBulkChunkMax)
//   chunk answer (REPLY value, u32):   the offset the app expects next
//
// `xfer` numbers the transfer (the hub's, per request): a chunk of another transfer than the one the app is reading is
// answered and ignored. Portable: no ESP-IDF, no heap.

#pragma once

#include <cstddef>
#include <cstdint>

namespace pot {

constexpr uint8_t kBulkOpen = 1;
constexpr uint8_t kBulkVersion = 1;
constexpr size_t kBulkChunkHeader = 7;
constexpr size_t kBulkChunkMax = 192;  // 7 + 192 = 199 B, +2 passed on: inside the radio's 218 (kMaxCallArgsV1)
constexpr uint32_t kBulkNoFile = 0xFFFFFFFFu;
constexpr uint32_t kBulkSizeMax = 1u << 20;  // 1 MiB: what the hub offers at most

struct BulkChunk {
    uint16_t xfer;
    uint32_t offset;
    const uint8_t* data;
    uint16_t len;
};

// A chunk's fields (data points into args), or false if the arguments are not a chunk of this version.
inline bool parse_bulk_chunk(const uint8_t* args, uint16_t len, BulkChunk& out) {
    if (args == nullptr || len < kBulkChunkHeader + 1 || len > kBulkChunkHeader + kBulkChunkMax || args[0] != kBulkVersion) {
        return false;
    }
    out.xfer = static_cast<uint16_t>(args[1] | (args[2] << 8));
    out.offset = static_cast<uint32_t>(args[3]) | (static_cast<uint32_t>(args[4]) << 8) |
                 (static_cast<uint32_t>(args[5]) << 16) | (static_cast<uint32_t>(args[6]) << 24);
    out.data = args + kBulkChunkHeader;
    out.len = static_cast<uint16_t>(len - kBulkChunkHeader);
    return true;
}

}  // namespace pot
