// Potluck's test kit (M12): a rig for testing one actor on a laptop, against the real portable core.
//
//   #include "pot/testkit.hpp"
//   #include "test_harness.hpp"
//   #include "my/my.hpp"                       // your component's header: its row, its config
//
//   TEST(my_app, publishes_after_one_period) {
//       pot::testkit::Rig rig;                 // one node, 0x0100, in cluster "lab"; the clock at 0
//       rig.declare(pot::kEveryNode, 0x80, cfg, sizeof(cfg));    // or rig.declare_bytes(the host's bytes)
//       CHECK(rig.start(my::row()));           // your row's check(), then create() and start()
//       rig.advance(1000);                     // 1000 ms, ticking the actor every millisecond
//       pot::Reading r = rig.read("potluck://lab/node-0100/my/out");
//       CHECK(r.quality == pot::Quality::Good);
//   }
//
// What the rig gives the actor is what a board does: the Node (namespace, peers, calls), the LED
// (captured in rig.led), a checkpoint store for a portable actor (kept in memory, survives
// rig.restart()), and background work run at once on the caller. What it does not have is a radio: no
// peer ever appears unless a test adds one (rig.node().peers()), and nothing is sent anywhere.

#pragma once

#include <cstdint>
#include <cstring>
#include <new>

#include "pot/actor.hpp"
#include "pot/node.hpp"
#include "pot/sys_resources.hpp"

namespace pot {
namespace testkit {

class Rig {
  public:
    struct Led {
        int calls = 0;
        bool on = false;
        uint8_t r = 0, g = 0, b = 0;
        uint16_t blink_period_ms = 0;
    };
    Led led;
    // The checkpoint the actor saved last (portable actors), and how many times it saved.
    uint8_t checkpoint[kMaxCheckpoint] = {};
    size_t checkpoint_len = 0;
    int checkpoint_saves = 0;

    explicit Rig(uint16_t node_id = 0x0100, const char* system = "lab") {
        set_sys_system(system);
        NodeConfig cfg;
        cfg.node_id = node_id;
        cfg.boot_epoch = 1;
        cfg.mac[0] = 0x02;
        cfg.mac[4] = static_cast<uint8_t>(node_id >> 8);
        cfg.mac[5] = static_cast<uint8_t>(node_id);
        hal_.ctx = this;
        hal_.now_ms = [](void* c) { return static_cast<Rig*>(c)->now_; };
        hal_.send = [](void*, const uint8_t*, const uint8_t*, size_t) -> int32_t { return 0; };
        node_ = new Node(cfg, hal_);
        declare_sys_resources(node_->ns(), node_id);
        board_.ctx = this;
        board_.led_app = [](void* c, bool on, uint8_t r, uint8_t g, uint8_t b, uint16_t p) {
            Led& l = static_cast<Rig*>(c)->led;
            ++l.calls;
            l.on = on;
            l.r = r, l.g = g, l.b = b;
            l.blink_period_ms = p;
        };
        board_.bg_submit = [](void*, void (*work)(void*), void* arg) {
            work(arg);  // at once, on the caller: a test sees the result on return
            return true;
        };
        board_.bg_workers = 1;
        store_.ctx = this;
        store_.save = [](void* c, const uint8_t* d, size_t n) {
            Rig* r = static_cast<Rig*>(c);
            if (n > kMaxCheckpoint) return false;
            std::memcpy(r->checkpoint, d, n);
            r->checkpoint_len = n;
            ++r->checkpoint_saves;
            return true;
        };
        store_.load = [](void* c, uint8_t* out, size_t cap, size_t* len, uint32_t* age) {
            Rig* r = static_cast<Rig*>(c);
            if (r->checkpoint_len == 0 || r->checkpoint_len > cap) return false;
            std::memcpy(out, r->checkpoint, r->checkpoint_len);
            *len = r->checkpoint_len;
            *age = 0;
            return true;
        };
    }
    ~Rig() {
        stop();
        delete node_;
        set_sys_system("lab");
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    Node& node() { return *node_; }
    uint32_t now() const { return now_; }
    Actor* actor() { return actor_; }
    const ActorDecl& decl() const { return decl_; }

    // The declaration the actor is built from: what `potluck.deploy` compiles for it.
    void declare(uint16_t node_id, uint8_t type, const uint8_t* cfg, size_t len) {
        cfg_len_ = len > kMaxActorCfg ? kMaxActorCfg : len;
        std::memcpy(cfg_, cfg, cfg_len_);
        decl_ = ActorDecl{node_id, static_cast<ActorType>(type), static_cast<uint8_t>(cfg_len_), cfg_};
    }
    // The same from the host's bytes for one actor: node u16 (little-endian), type u8, length u8, config.
    bool declare_bytes(const uint8_t* bytes, size_t len) {
        if (len < 4 || static_cast<size_t>(bytes[3]) + 4 != len) return false;
        declare(static_cast<uint16_t>(bytes[0] | (bytes[1] << 8)), bytes[2], bytes + 4, bytes[3]);
        return true;
    }

    // The row's check(), then create() and start(), as the runtime does. `why` gets check()'s reason.
    // `portable`: give the actor the checkpoint store (the reconciler does for a portable actor).
    bool start(const ActorKind& row, const char** why = nullptr, bool portable = false) {
        stop();
        row_ = row;
        portable_ = portable;
        ActorEnv env = env_for();
        const char* w = nullptr;
        if (row.check != nullptr && !row.check(decl_, env, &w)) {
            if (why != nullptr) *why = w;
            return false;
        }
        actor_ = row.create(mem_, decl_, env);
        return actor_ != nullptr && actor_->start(now_);
    }
    // Stop the actor and start a new instance from the same declaration: a reboot, or a move to another
    // node, as the actor sees it. The checkpoint stays.
    bool restart() { return start(row_, nullptr, portable_); }
    void stop() {
        if (actor_ != nullptr) actor_->~Actor();
        actor_ = nullptr;
    }

    // Move the clock by `ms`, ticking the node and the actor every `step_ms`.
    void advance(uint32_t ms, uint32_t step_ms = 1) {
        for (uint32_t t = 0; t < ms; t += step_ms) {
            now_ += step_ms;
            node_->tick(now_);
            if (actor_ != nullptr) actor_->tick(now_);
        }
    }

    // The section 4 tuple for a resource, as a reader on this node gets it.
    Reading read(uint32_t hash) {
        Reading r;
        node_->read(hash, r);
        return r;
    }
    Reading read(const char* path) { return read(path_hash(path)); }

    // An input: declare a resource owned by this node and publish to it, as another actor here would.
    NsError feed(const char* path, const Value& v, ValueType type, Unit unit = Unit::None, uint32_t bound_ms = 0) {
        NsDecl d;
        d.path_hash = path_hash(path);
        d.owner_node = node_->config().node_id;
        d.type = type;
        d.unit = unit;
        d.staleness_bound_ms = bound_ms;
        if (node_->ns().find(d.path_hash) == nullptr) node_->ns().declare(d);
        return node_->publish(d.path_hash, v);
    }

    // What the actor's row says it publishes (the outputs other nodes learn from the image).
    size_t outputs(NsDecl* out, size_t cap, uint16_t on_node, uint16_t peer = 0) const {
        return row_.outputs != nullptr ? row_.outputs(decl_, on_node, peer, out, cap) : 0;
    }

  private:
    ActorEnv env_for() {
        ActorEnv env;
        env.node = node_;
        env.trial_window_ms = 10000;
        env.board = &board_;
        env.checkpoint = portable_ ? &store_ : nullptr;
        return env;
    }

    NodeHal hal_{};
    Node* node_ = nullptr;
    uint32_t now_ = 0;
    BoardServices board_{};
    CheckpointStore store_{};
    ActorKind row_{};
    bool portable_ = false;
    ActorDecl decl_{};
    uint8_t cfg_[kMaxActorCfg] = {};
    size_t cfg_len_ = 0;
    alignas(std::max_align_t) unsigned char mem_[kActorSlotBytes];
    Actor* actor_ = nullptr;
};

}  // namespace testkit
}  // namespace pot
