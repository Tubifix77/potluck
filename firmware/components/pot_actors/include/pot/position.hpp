// M20: where a board sits on the cell's floor plan, held by the board itself. Pinned to one board, it declares
// node-<id>/sys/pos/x and node-<id>/sys/pos/y -- f32 metres from the plan's top-left corner, x to the right, y
// down -- and publishes the two numbers its config carries. The owner places the boards on the dashboard's floor
// plan; `potluck apply` writes the positions into the signed package, so a cell with no PC still knows them and an
// app (passive-sensor's localisation) reads them through the cell like any other resource.
//
// It also holds the board's room as a number, node-<id>/sys/room (u32): the rooms the cluster file names, sorted by
// name and numbered from 1, so an app can tell which boards share a room. A board with no position declares no
// pos entries, one with no room no room entry -- a reader treats NotFound as "not placed" or "no room".
//
// The values never change while the package runs, but they are re-published every kPositionPeriodMs so a reader
// that adopts them late sees them GOOD, and so a stopped actor shows as STALE rather than as a position forever.
//
// And the boards nearest it in its room, node-<id>/sys/near (Bytes, up to four u16 node ids, nearest first): `potluck
// apply` computes them from every board's position and room when an app's descriptor asks ("peers": {"nearest":
// K}), so an app on the board reads its own set locally rather than every board's position over the radio.
//
// Other boards adopt these entries when they first read them (ActorKind::outputs_on_demand), never at boot.
//
// M19: and the cells (groups) the board belongs to -- up to two, numbered by `apply` from the cluster file's names --
// which it sets on the node (Node::set_groups) when it starts and clears when it stops, so admission follows the package.
//
//   config: x_hash u32, y_hash u32, room_hash u32, x f32, y f32 (-1: not placed; else 0..1000 m), room u32 (0: none),
//           near_hash u32, near u16[4] (0: none; no ids at all: no sys/near), cell u8[2] (0: none)

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/actor.hpp"
#include "pot/deploy.hpp"
#include "pot/node.hpp"

namespace pot {

constexpr uint8_t kPositionCfgLen = 38;
constexpr size_t kPositionNearMax = 4;
constexpr uint32_t kPositionPeriodMs = 10000;
constexpr float kPositionMaxM = 1000.0f;

struct PositionConfig {
    uint32_t x_hash, y_hash, room_hash;
    float x, y;
    uint32_t room;
    uint32_t near_hash;
    uint16_t near[kPositionNearMax];
    uint8_t cells[2];  // M19
    bool placed() const { return x >= 0.0f; }
    uint8_t near_count() const {
        uint8_t n = 0;
        while (n < kPositionNearMax && near[n] != 0) ++n;
        return n;
    }
};
bool position_config(const ActorDecl& a, PositionConfig& out);
NsDecl position_decl(uint16_t node, uint32_t hash, Unit unit = Unit::Metre, ValueType type = ValueType::F32);

class PositionActor : public Actor {
  public:
    PositionActor(Node& node, const PositionConfig& cfg) : node_(node), cfg_(cfg) {}
    bool start(uint32_t now_ms) override;
    void tick(uint32_t now_ms) override;
    size_t stats_json(char* buf, size_t cap, uint32_t now_ms) override;
    ~PositionActor() override;

  private:
    Node& node_;
    PositionConfig cfg_;
    uint32_t next_ms_ = 0;
};

extern const ActorKind kPositionKind;

}  // namespace pot
