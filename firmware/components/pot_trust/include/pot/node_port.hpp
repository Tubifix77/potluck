// M29: the node port's login, on a board -- the same door the hub's node port has (host/potluck/potluck/hub_bridge.py),
// so a node from the house network (a tablet) logs in to a board's own station exactly as it does to the hub.
//
//     board -> node   potluck-node 1 <nonce: 64 hex>\n
//     node -> board   <certificate: 224 hex> <signature: 128 hex>\n          (one line, within kLoginTimeoutMs)
//     board -> node   ok <node id: 4 hex> <this board: 4 hex> <its MAC: 12 hex>\n   then frames both ways
//                     | refused <why>\n and the board closes
//
// The certificate is the node's own, from the cell's CA; the signature is the node's key over kLoginDomain || nonce ||
// certificate. Who may log in: a node certificate with an id the hub bridges (kBridgedFirst..kBridgedLast) -- the
// certificate is the cell's word, so a board needs no list. Pure logic: the caller holds the socket, draws the nonce
// and runs the check where the crypto may run (NodeHal::run_heavy).

#pragma once

#include <cstddef>
#include <cstdint>

namespace pot {

constexpr uint16_t kNodePortDefault = 8472;  // the hub's node port, beside its API's 8470
constexpr uint16_t kBridgedFirst = 0x00F0;   // the ids a node from the house network may have (hub_bridge.py)
constexpr uint16_t kBridgedLast = 0x00FD;
constexpr size_t kNodePortNonceLen = 32;
constexpr uint32_t kLoginTimeoutMs = 10000;
constexpr size_t kLoginLineMax = 600;
constexpr char kLoginDomain[] = "potluck node port v1";  // with its NUL, as the hub's b"potluck node port v1\x00"

// "potluck-node 1 <hex>\n" into out; its length, or 0 if cap is too small.
size_t node_port_challenge(const uint8_t nonce[kNodePortNonceLen], char* out, size_t cap);

struct NodePortLogin {
    uint16_t node_id = 0;    // 0: refused
    const char* why = "";    // a short reason, for "refused <why>"
};

// The node's answer line (without its newline), checked against the nonce and the cell's CA.
NodePortLogin node_port_check(const char* line, size_t len, const uint8_t nonce[kNodePortNonceLen],
                              const uint8_t ca_pub[32]);

// "ok <node> <board> <mac>\n" into out; its length, or 0 if cap is too small.
size_t node_port_ok(uint16_t node_id, uint16_t board_id, const uint8_t board_mac[6], char* out, size_t cap);

}  // namespace pot
