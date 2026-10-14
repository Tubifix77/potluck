// The worked example of DEVELOPING-APPS.md, tested the way an app's actors can be: on a real pot::Node,
// on the host, before any board. examples/apps/pot_hello is compiled into this suite from its own
// directory, as an application's component would be. The declaration below is what the host tools
// compile from examples/apps/hello.json (host/potluck/tests/test_examples.py checks the same bytes).

#include <cstring>
#include <new>

#include "hello/hello.hpp"
#include "pot/node.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

uint32_t g_t = 0;

// node 0xFFFF (every node), type 0xF0, 6 config bytes: system_prefix u32, period_ms u16 = 5000.
const uint8_t kGoldenDecl[] = {0xff, 0xff, 0xf0, 0x06, 0x27, 0x11, 0x39, 0x61, 0x88, 0x13};

struct Led {
    int calls = 0;
    bool on = false;
};

struct Rig {
    NodeHal hal{};
    Node* node = nullptr;
    Led led;
    BoardServices board{};
    ActorDecl decl{};
    alignas(hello::PeersActor) unsigned char mem[sizeof(hello::PeersActor)];
    Actor* actor = nullptr;

    Rig() {
        NodeConfig cfg;
        cfg.node_id = 0x100;
        cfg.boot_epoch = 1;
        cfg.mac[0] = 0x02;
        hal.now_ms = [](void*) { return g_t; };
        hal.send = [](void*, const uint8_t*, const uint8_t*, size_t) -> int32_t { return 0; };
        node = new Node(cfg, hal);
        board.ctx = &led;
        board.led_app = [](void* c, bool on, uint8_t, uint8_t, uint8_t, uint16_t) {
            Led* l = static_cast<Led*>(c);
            ++l->calls;
            l->on = on;
        };
        decl = ActorDecl{kEveryNode, static_cast<ActorType>(kGoldenDecl[2]), kGoldenDecl[3], kGoldenDecl + 4};
    }
    ~Rig() {
        if (actor != nullptr) actor->~Actor();
        delete node;
    }
    bool create() {
        const ActorKind k = hello::peers_row();
        const char* why = nullptr;
        ActorEnv env;
        env.node = node;
        env.board = &board;
        if (!k.check(decl, env, &why)) return false;
        actor = k.create(mem, decl, env);
        return actor != nullptr && actor->start(g_t);
    }
};

}  // namespace

TEST(hello_app, the_host_tools_bytes_parse_into_its_config) {
    Rig r;
    hello::Config c;
    CHECK(hello::parse(r.decl, c));
    CHECK_EQ(c.system_prefix, path_hash("potluck://lab/"));  // what the descriptor's system_prefix sends
    CHECK_EQ(c.period_ms, static_cast<uint16_t>(5000));
    CHECK_EQ(hello::output_hash(c.system_prefix, 0x7368), path_hash("potluck://lab/node-7368/hello/peers"));
}

TEST(hello_app, it_publishes_its_peer_count_and_asks_for_cyan_only_in_company) {
    Rig r;
    CHECK(r.create());
    for (int i = 0; i < 10; ++i) r.actor->tick(++g_t);
    Reading rd;
    CHECK(r.node->read(path_hash("potluck://lab/node-0100/hello/peers"), rd) == NsError::Ok);
    uint32_t n = 99;
    CHECK(rd.value.as_u32(n) && n == 0u);  // no radio peer in this rig
    CHECK(rd.quality == Quality::Good);
    CHECK_EQ(r.led.calls, 1);  // once per period, not once per tick
    CHECK(!r.led.on);  // alone: no cyan (the board itself shows yellow, searching)
    for (int i = 0; i < 5000; ++i) r.actor->tick(++g_t);
    CHECK_EQ(r.led.calls, 2);
    char line[128];
    CHECK(r.actor->stats_json(line, sizeof(line), g_t) > 0);
    CHECK(std::strstr(line, "\"t\":\"hello\"") != nullptr);
}

TEST(hello_app, other_nodes_learn_its_output_from_the_image) {
    Rig r;
    NsDecl out[kMaxOutputs];
    const ActorKind k = hello::peers_row();
    CHECK_EQ(k.outputs(r.decl, 0x7368, 0, out, kMaxOutputs), 1u);
    CHECK_EQ(out[0].path_hash, path_hash("potluck://lab/node-7368/hello/peers"));
    CHECK_EQ(out[0].owner_node, static_cast<uint16_t>(0x7368));
    CHECK(out[0].access == Access::Read);
    CHECK_EQ(k.outputs(r.decl, 0x7368, 0x8160, out, kMaxOutputs), 0u);  // no per-link outputs
}

TEST(hello_app, a_declaration_it_cannot_run_is_refused_with_a_reason) {
    Rig r;
    uint8_t bad[6];
    std::memcpy(bad, kGoldenDecl + 4, 6);
    bad[4] = 0x10, bad[5] = 0x00;  // 16 ms
    r.decl.cfg = bad;
    const char* why = nullptr;
    CHECK(!hello::peers_row().check(r.decl, ActorEnv{}, &why) && why != nullptr);
    r.decl.cfg = kGoldenDecl + 4;
    r.decl.node_id = kPortableNode;  // it is an every-node actor; the reconciler has no placement for it
    CHECK(!hello::peers_row().check(r.decl, ActorEnv{}, &why));
}
