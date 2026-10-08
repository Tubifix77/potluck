// M8.1: the actor API's runtime. One registration table, all-or-nothing validation, the image's
// configuration copied out, and every instance driven the same way.

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <new>

#include "pot/actor.hpp"
#include "pot/deploy.hpp"
#include "pot/node.hpp"
#include "pot/svc_client.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

uint32_t g_now = 0;

struct OneNode {
    NodeHal hal{};
    Node* node = nullptr;
    OneNode() {
        NodeConfig cfg;
        cfg.node_id = 0x100;
        cfg.boot_epoch = 1;
        cfg.mac[0] = 0x02;
        hal.now_ms = [](void*) { return g_now; };
        hal.send = [](void*, const uint8_t*, const uint8_t*, size_t) -> int32_t { return 0; };
        node = new Node(cfg, hal);
    }
    ~OneNode() { delete node; }
};

// A test actor: counts what the runtime does to it. cfg byte 0 is its id; byte 1 non-zero makes
// start() fail; byte 2 non-zero makes it claim CALL results.
struct Probe : Actor {
    uint8_t id, fail, claims;
    uint32_t ticks = 0;
    uint32_t results = 0;
    static int alive;
    explicit Probe(const uint8_t* c) : id(c[0]), fail(c[1]), claims(c[2]) { ++alive; }
    ~Probe() override { --alive; }
    bool start(uint32_t) override { return fail == 0; }
    void tick(uint32_t) override { ++ticks; }
    bool on_call_result(uint16_t, uint16_t, uint32_t, Node::CallOutcome, const Value&) override {
        ++results;
        return claims != 0;
    }
    size_t stats_json(char* buf, size_t cap, uint32_t) override {
        return static_cast<size_t>(std::snprintf(buf, cap, "{\"t\":\"probe\",\"id\":%u}", id));
    }
};
int Probe::alive = 0;

bool probe_check(const ActorDecl& d, const ActorEnv&, const char** why) {
    if (d.cfg_len != 3) {
        *why = "probe config must be 3 bytes";
        return false;
    }
    return true;
}
Actor* probe_create(void* mem, const ActorDecl& d, const ActorEnv&) {
    static_assert(sizeof(Probe) <= kActorSlotBytes, "Probe fits");
    return new (mem) Probe(d.cfg);
}

// Two kinds under the Led and Fault type codes: the table, not the type code, decides behaviour.
const ActorKind kTable[] = {
    {ActorType::Led, "probe", &probe_check, &probe_create},
    {ActorType::Fault, "probe2", &probe_check, &probe_create},
};

DeployImage image(std::initializer_list<ActorDecl> decls) {
    DeployImage img{};
    for (const ActorDecl& d : decls) img.actors[img.actor_count++] = d;
    return img;
}

}  // namespace

TEST(actor_runtime, loads_only_this_nodes_pinned_actors_and_every_node_ones) {
    OneNode n;
    const uint8_t a[3] = {1, 0, 0}, b[3] = {2, 0, 0}, c[3] = {3, 0, 0}, p[3] = {4, 0, 0};
    const DeployImage img = image({{0x100, ActorType::Led, 3, a},
                                   {0x200, ActorType::Led, 3, b},          // another node's
                                   {kEveryNode, ActorType::Fault, 3, c},  // everyone's, so ours too
                                   {kPortableNode, ActorType::Led, 3, p}});  // the reconciler's
    ActorRuntime rt(kTable, 2);
    ActorEnv env;
    env.node = n.node;
    const char* why = nullptr;
    CHECK(rt.load(img, 0x100, env, &why));
    CHECK_EQ(rt.loaded(), static_cast<size_t>(2));
}

TEST(actor_runtime, one_invalid_actor_refuses_the_whole_image) {
    OneNode n;
    const uint8_t good[3] = {1, 0, 0}, bad[2] = {2, 0};
    const DeployImage img = image({{0x100, ActorType::Led, 3, good}, {0x100, ActorType::Fault, 2, bad}});
    ActorRuntime rt(kTable, 2);
    ActorEnv env;
    env.node = n.node;
    const char* why = nullptr;
    CHECK(!rt.load(img, 0x100, env, &why));
    CHECK(why != nullptr && std::strcmp(why, "probe config must be 3 bytes") == 0);
    CHECK_EQ(rt.loaded(), static_cast<size_t>(0));  // nothing half-kept
}

TEST(actor_runtime, an_actor_type_without_a_row_is_refused_not_skipped) {
    OneNode n;
    const uint8_t c[13] = {};
    const DeployImage img = image({{0x100, ActorType::SvcClient, 13, c}});
    ActorRuntime rt(kTable, 2);  // no svc_client row
    ActorEnv env;
    env.node = n.node;
    const char* why = nullptr;
    CHECK(!rt.load(img, 0x100, env, &why));
    CHECK(why != nullptr);
}

TEST(actor_runtime, the_configuration_is_copied_out_of_the_image_buffer) {
    OneNode n;
    uint8_t buf[3] = {7, 0, 0};
    const DeployImage img = image({{0x100, ActorType::Led, 3, buf}});
    ActorRuntime rt(kTable, 2);
    ActorEnv env;
    env.node = n.node;
    CHECK(rt.load(img, 0x100, env, nullptr));
    buf[0] = 99;  // the receiver reuses the image buffer once boot is over
    CHECK_EQ(rt.start(env, 0, nullptr), static_cast<size_t>(1));
    CHECK_EQ(static_cast<unsigned>(static_cast<Probe*>(rt.actor(0))->id), 7u);
}

TEST(actor_runtime, drives_every_instance_the_same_way_and_drops_one_that_will_not_start) {
    OneNode n;
    {
        const uint8_t a[3] = {1, 0, 0}, b[3] = {2, 1, 0}, c[3] = {3, 0, 1};
        const DeployImage img = image({{0x100, ActorType::Led, 3, a},
                                       {0x100, ActorType::Fault, 3, b},
                                       {0x100, ActorType::Led, 3, c}});
        ActorRuntime rt(kTable, 2);
        ActorEnv env;
        env.node = n.node;
        CHECK(rt.load(img, 0x100, env, nullptr));
        const char* failed = nullptr;
        CHECK_EQ(rt.start(env, 0, &failed), static_cast<size_t>(2));
        CHECK(failed != nullptr && std::strcmp(failed, "probe2") == 0);
        CHECK_EQ(Probe::alive, 2);  // the one that failed to start was destroyed

        rt.tick(1);
        rt.tick(2);
        Probe* first = static_cast<Probe*>(rt.actor(0));
        Probe* third = static_cast<Probe*>(rt.actor(1));
        CHECK_EQ(first->ticks, 2u);
        CHECK_EQ(third->ticks, 2u);

        // A CALL result is offered in order until one actor claims it.
        CHECK(rt.on_call_result(1, 1, 0, Node::CallOutcome::Ok, Value::of_u32(1)));
        CHECK_EQ(first->results, 1u);
        CHECK_EQ(third->results, 1u);

        char line[64];
        CHECK(first->stats_json(line, sizeof(line), 0) > 0);
        CHECK(std::strcmp(line, "{\"t\":\"probe\",\"id\":1}") == 0);
        CHECK(std::strcmp(rt.name(1), "probe") == 0);
    }
    CHECK_EQ(Probe::alive, 0);  // the runtime destroys what it built
}

TEST(actor_runtime, the_svc_client_row_builds_a_service_client) {
    OneNode n;
    // svc_hash u32, provider u16, out_hash u32, period_ms u16, on_host_loss u8 -- little-endian
    const uint8_t cfg[13] = {0x11, 0x22, 0x33, 0x44, 0xFE, 0x00, 0x55, 0x66, 0x77, 0x88, 0xE8, 0x03, 0};
    const DeployImage img = image({{0x100, ActorType::SvcClient, 13, cfg}});
    const ActorKind table[] = {kSvcClientKind};
    ActorRuntime rt(table, 1);
    ActorEnv env;
    env.node = n.node;
    const char* why = nullptr;
    CHECK(rt.load(img, 0x100, env, &why));
    CHECK_EQ(rt.start(env, 0, nullptr), static_cast<size_t>(1));
    char line[512];
    CHECK(rt.actor(0)->stats_json(line, sizeof(line), 5) > 0);
    CHECK(std::strncmp(line, "{\"t\":\"svc\",\"node\":256,\"up_ms\":5,", 32) == 0);
    CHECK(std::strstr(line, "\"state\":\"waiting\"") != nullptr);
}

// ---- portable actors: the reconciler builds them from the same table (M8.1 step 1b) ----------------

#include "pot/reconcile.hpp"

namespace {
bool probe_place(const ActorDecl&, TickerConfig& out) {
    out = TickerConfig{};
    out.out_hash = path_hash("potluck://lab/test/probe");
    out.period_ms = 100;
    out.count = 1;
    out.node[0] = 0x100;  // only here
    out.gravity[0] = 0;
    return true;
}
const ActorKind kPortableTable[] = {
    {ActorType::Led, "probe", &probe_check, &probe_create, &probe_place},
    {ActorType::Fault, "pinned_only", &probe_check, &probe_create},
};
}  // namespace

TEST(actor_runtime, a_portable_actor_of_any_type_is_placed_and_run_by_the_reconciler) {
    OneNode n;
    const uint8_t cfg[3] = {5, 0, 0};
    const DeployImage img = image({{kPortableNode, ActorType::Led, 3, cfg}});
    PortableSpec specs[kMaxPortable];
    size_t count = 0;
    const char* why = nullptr;
    CHECK(collect_portable(img, kPortableTable, 2, specs, kMaxPortable, count, &why));
    CHECK_EQ(count, static_cast<size_t>(1));
    {
        Reconciler rec(*n.node);
        CHECK(rec.load(specs, count));
        g_now = 1000;
        rec.start(g_now);
        CHECK_EQ(Probe::alive, 0);
        for (int k = 0; k < 2000 && Probe::alive == 0; ++k) rec.tick(++g_now);
        CHECK_EQ(Probe::alive, 1);  // settled alone, the only eligible node: activated
        for (int k = 0; k < 10; ++k) rec.tick(++g_now);
        CHECK(rec.view(0).running);
    }
    CHECK_EQ(Probe::alive, 0);  // the reconciler destroys what it built
}

TEST(actor_runtime, a_type_without_a_placement_hook_cannot_be_portable) {
    const uint8_t cfg[3] = {5, 0, 0};
    const DeployImage img = image({{kPortableNode, ActorType::Fault, 3, cfg}});
    PortableSpec specs[kMaxPortable];
    size_t count = 7;
    const char* why = nullptr;
    CHECK(!collect_portable(img, kPortableTable, 2, specs, kMaxPortable, count, &why));
    CHECK_EQ(count, static_cast<size_t>(0));
    CHECK(why != nullptr);
}
