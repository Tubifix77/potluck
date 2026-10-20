// 2026-10-20: an inbox -- named values a program outside the cell writes, held by one board for the whole cell.
//
// Asked for by the Potluck Desk for the owner's Android app (Synth Senses): a program on the hub's API owned no
// resources, it only read and wrote the cell's. An inbox actor, pinned to a board, declares its outputs
// act/<actor>/<name> as writable entries; the program writes them through the hub's POST /api/write (the cluster
// file's `outside` must say read_write for each), and every other board holds replicas of them from the image, as
// of any pinned actor's outputs. Until written an output is NO_DATA; a writer that stops lets it age into STALE
// past its staleness bound -- the bound is the contract the program keeps by writing more often than that.
//
// It moves nothing and reads nothing: values are at most eight bytes, as every resource's (section 4).
//
//   config: stale_ms u32 (0: never stale -- a setting), n u8 (1..kMaxOutputs), then n x {hash u32, type u8, unit u8}

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/actor.hpp"
#include "pot/deploy.hpp"
#include "pot/node.hpp"

namespace pot {

struct InboxConfig {
    uint32_t stale_ms = 0;
    uint8_t n = 0;
    uint32_t hash[kMaxOutputs] = {};
    ValueType type[kMaxOutputs] = {};
    Unit unit[kMaxOutputs] = {};
};
bool inbox_config(const ActorDecl& a, InboxConfig& out);
NsDecl inbox_decl(uint16_t node, const InboxConfig& c, size_t i);

class InboxActor : public Actor {
  public:
    InboxActor(Node& node, const InboxConfig& cfg) : node_(node), cfg_(cfg) {}
    bool start(uint32_t now_ms) override;
    void tick(uint32_t now_ms) override;
    size_t stats_json(char* buf, size_t cap, uint32_t now_ms) override;

  private:
    Node& node_;
    InboxConfig cfg_;
    uint32_t writes_ = 0;
    uint32_t last_count_[kMaxOutputs] = {};
};

extern const ActorKind kInboxKind;

}  // namespace pot
