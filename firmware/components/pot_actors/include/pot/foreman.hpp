// M19: a cell's foreman -- the board that presents a summary of its cell to the cell above, as a player-manager:
// it stays a full member of its own cell, keeps its own sensors and cores, and is one more member of the upper cell.
//
// A role, not a board (ARCHITECTURE section 13, M19; Review of M19 #4): one portable actor per federating cell,
// eligible on the boards the cluster file marks "foreman": true. The reconciler (M6) places it by its ranking, fences
// it by term -- so never two foremen of a cell for longer than its claims take to meet -- and hands it over. Whoever
// runs it is a member of the parent cell: the reconciler calls the kind's membership hook, which sets the node's role
// group (Node::set_role_group). The board ranked second is the standby: warm in its own cell (it holds the role's
// checkpoint and starts it the moment the foreman is gone) and a member of the parent from then on -- one seat in the
// upper cell per child cell, so a cell of ten under an upper cell of ten fits a board's nineteen peers. Everyone else in the cell, and
// every member of the parent, keeps the declaration too (the image's scope section lists it under both cells), so the
// summary's entries are declared there, owned by whoever holds the role.
//
// What goes up is a summary the owner chose, never a mirror (Review of M19 #8): each export is one entry,
// potluck://<system>/cell-<name>/<export>, published every period_ms --
//
//   alive      how many of the cell's boards this foreman sees alive, itself included (u32)
//   copy       one chosen resource, re-published under the cell's name, with its own type and unit
//   max, min, sum, avg   over every board of the cell's node-<id>/<suffix> that has a value (f32)
//   count      how many boards of the cell have a usable node-<id>/<suffix> (u32)
//
// Its age is the oldest input's (published with that input's timestamp), so a summary is never fresher than what it
// summarises; a faulty input makes the summary faulty; a board that is dead is not an input at all. A read while the
// foreman is down is Unavailable at the reader, since the reader holds the entry owned by the foreman.
//
//   config: the portable header (deploy.hpp), then
//           cell u8, parent u8, parent_window u8 (the parent's death window, 100 ms steps; 0: none),
//           n_members u8, prefix u32 (the path hash state of "potluck://<system>/node-", path_hash_from's seed),
//           members u16[n_members], n_exports u8, then each export:
//             out_hash u32, kind u8, unit u16, then for copy: type u8, src_hash u32;
//                                              for max/min/sum/avg/count: len u8, suffix (len ASCII bytes)

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/actor.hpp"
#include "pot/deploy.hpp"
#include "pot/node.hpp"

namespace pot {

constexpr size_t kForemanMaxMembers = 20;
constexpr size_t kForemanMaxExports = 8;
constexpr size_t kForemanMaxSuffix = 32;

enum class ExportKind : uint8_t { Alive = 0, Copy = 1, Max = 2, Min = 3, Sum = 4, Count = 5, Avg = 6 };

struct ForemanExport {
    uint32_t out_hash;
    ExportKind kind;
    Unit unit;
    ValueType type;       // copy only
    uint32_t src_hash;    // copy only
    uint8_t suffix_len;   // the aggregates
    const uint8_t* suffix;  // into the declaration's config, which outlives the actor
};

struct ForemanConfig {
    TickerConfig place;
    uint8_t cell, parent, parent_window;
    uint32_t prefix;
    uint8_t n_members;
    uint16_t members[kForemanMaxMembers];
    uint8_t n_exports;
    const uint8_t* exports;  // n_exports records, into the declaration's config
    // Export i, decoded (foreman_config has checked every one).
    ForemanExport export_at(size_t i) const;
    // The input path of export `e` on board `member` (max/min/sum/avg/count).
    uint32_t input_hash(const ForemanExport& e, uint16_t member) const;
    bool lists(uint16_t node) const {
        for (uint8_t i = 0; i < n_members; ++i)
            if (members[i] == node) return true;
        return false;
    }
};
bool foreman_config(const ActorDecl& a, ForemanConfig& out);

class ForemanActor : public Actor {
  public:
    ForemanActor(Node& node, const ForemanConfig& cfg) : node_(node), cfg_(cfg) {}
    bool start(uint32_t now_ms) override;
    void tick(uint32_t now_ms) override;
    size_t stats_json(char* buf, size_t cap, uint32_t now_ms) override;
    uint32_t rounds() const { return rounds_; }

  private:
    void publish_export(const ForemanExport& e, uint32_t now);
    Node& node_;
    ForemanConfig cfg_;
    uint32_t next_ms_ = 0;
    uint32_t rounds_ = 0;
    uint8_t alive_ = 0;
};

extern const ActorKind kForemanKind;

}  // namespace pot
