// pot_hello: the worked example of DEVELOPING-APPS.md -- one actor, small enough to read in a sitting.
//
// ext:hello/peers (type 0xF0, every node): every period, each board publishes how many radio peers it
// sees alive, to potluck://<system>/node-<id>/hello/peers (u32), and lights its status LED cyan while it
// sees none. A reader anywhere in the cell gets the number with its age and quality, like any resource.
//
// What it shows: a config parsed from the bytes the host tools encode from the descriptor; an output
// path built on the board from the descriptor's system_prefix; the output declared by the actor on its
// own node and described to every other node through the outputs hook (so they can hold a replica);
// one publish per period from tick(); the LED through BoardServices; a stats line.
//
// Config (potluck-modules.json order): system_prefix u32 (the FNV-1a state after "potluck://<system>/"),
// period_ms u16.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/actor.hpp"
#include "pot/deploy.hpp"
#include "pot/node.hpp"

namespace pot {
namespace hello {

constexpr ActorType kPeersType = static_cast<ActorType>(0xF0);
constexpr uint8_t kCfgLen = 6;

struct Config {
    uint32_t system_prefix = 0;
    uint16_t period_ms = 0;
};

// False if the declaration is not this actor's or its bytes are out of range.
bool parse(const ActorDecl& d, Config& out);

// potluck://<system>/node-<id>/hello/peers, for a node id.
uint32_t output_hash(uint32_t system_prefix, uint16_t node_id);

class PeersActor : public Actor {
  public:
    PeersActor(Node& node, const BoardServices* board, const Config& cfg) : node_(node), board_(board), cfg_(cfg) {}
    bool start(uint32_t now_ms) override;
    void tick(uint32_t now_ms) override;
    size_t stats_json(char* buf, size_t cap, uint32_t now_ms) override;

    uint32_t published() const { return published_; }

  private:
    uint32_t alive_peers() const;

    Node& node_;
    const BoardServices* board_;
    Config cfg_;
    uint32_t out_ = 0;
    uint32_t next_ms_ = 0;
    uint32_t published_ = 0;
    uint32_t last_ = 0;
};

// The registration row: check, create, no placement hook (it is an every-node actor), outputs hook.
ActorKind peers_row();

}  // namespace hello

// The hook Potluck's firmware build calls for this component (generated from the directory's name).
const ActorKind* app_rows_pot_hello(size_t* n);

}  // namespace pot
