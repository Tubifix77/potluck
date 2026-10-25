// M24: the test app for events from the PC (pot/input.hpp). Pinned to one board, it takes the hub's batches on
// act/<name>/in and declares three resources of its own:
//
//   act/<name>/key     a u32: the last key event taken, code | down << 16 (0 before any)
//   act/<name>/count   a u32: events taken, key and pointer
//   act/<name>/gaps    a u32: events missed (a number skipped) -- the hub never skips one, so anything but 0 is a fault
//
// It answers every batch with the last number taken, at once: echoing is no work to hand elsewhere.
//
// Portable: no ESP-IDF, no heap, called under the node's lock.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/actor.hpp"
#include "pot/deploy.hpp"
#include "pot/input.hpp"
#include "pot/node.hpp"

namespace pot {

class KeyEchoActor : public Actor {
  public:
    KeyEchoActor(Node& node, const KeyEchoConfig& cfg) : node_(node), cfg_(cfg) {}
    bool start(uint32_t now_ms) override;
    void tick(uint32_t now_ms) override;
    int on_call(uint16_t from_node, uint16_t msg_id, uint32_t path_hash, const uint8_t* args, uint16_t len) override;
    size_t stats_json(char* buf, size_t cap, uint32_t now_ms) override;

    const InputOrder& order() const { return order_; }
    uint32_t batches() const { return batches_; }
    uint32_t last_key() const { return key_; }

  private:
    void publish(uint32_t now_ms);
    Node& node_;
    KeyEchoConfig cfg_;
    InputOrder order_{};
    uint32_t key_ = 0;
    uint32_t batches_ = 0, bad_ = 0;
    uint32_t now_ = 0;
    uint32_t published_at_ = 0;
};

bool key_echo_check(const ActorDecl& d, const ActorEnv& env, const char** why);
size_t key_echo_outputs(const ActorDecl& d, uint16_t node, uint16_t peer, NsDecl* out, size_t cap);
extern const ActorKind kKeyEchoKind;

}  // namespace pot
