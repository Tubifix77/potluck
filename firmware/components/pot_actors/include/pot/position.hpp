// M20: where a board sits on the cell's floor plan, held by the board itself. Pinned to one board, it declares
// node-<id>/sys/pos/x and node-<id>/sys/pos/y -- f32 metres from the plan's top-left corner, x to the right, y
// down -- and publishes the two numbers its config carries. The owner places the boards on the dashboard's floor
// plan; `potluck apply` writes the positions into the signed package, so a cell with no PC still knows them and an
// app (passive-sensor's localisation) reads them through the cell like any other resource.
//
// The values never change while the package runs, but they are re-published every kPositionPeriodMs so a reader
// that adopts them late sees them GOOD, and so a stopped actor shows as STALE rather than as a position forever.
//
//   config: x_hash u32, y_hash u32, x f32, y f32 (little-endian; 0..1000 m)

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/actor.hpp"
#include "pot/deploy.hpp"
#include "pot/node.hpp"

namespace pot {

constexpr uint8_t kPositionCfgLen = 16;
constexpr uint32_t kPositionPeriodMs = 10000;
constexpr float kPositionMaxM = 1000.0f;

struct PositionConfig {
    uint32_t x_hash, y_hash;
    float x, y;
};
bool position_config(const ActorDecl& a, PositionConfig& out);
NsDecl position_decl(uint16_t node, uint32_t hash);

class PositionActor : public Actor {
  public:
    PositionActor(Node& node, const PositionConfig& cfg) : node_(node), cfg_(cfg) {}
    bool start(uint32_t now_ms) override;
    void tick(uint32_t now_ms) override;
    size_t stats_json(char* buf, size_t cap, uint32_t now_ms) override;

  private:
    Node& node_;
    PositionConfig cfg_;
    uint32_t next_ms_ = 0;
};

extern const ActorKind kPositionKind;

}  // namespace pot
