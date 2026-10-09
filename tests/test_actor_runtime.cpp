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
    uint32_t samples = 0;
    int last_rssi = 0;
    void on_rx_sample(const RxSample& rs) override {
        ++samples;
        last_rssi = rs.rssi;
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

#include "pot/die_temp.hpp"

TEST(actor_runtime, the_output_of_an_actor_pinned_elsewhere_is_declared_here_as_the_owners) {
    // So a host cabled to this node, or an actor here, can read another board's value with its age
    // and quality (single-hop: we hold a replica, refreshed by our own READ to the owner).
    OneNode n;
    const uint32_t out = path_hash("potluck://lab/node-0200/hw/die_temp");
    const uint8_t cfg[6] = {static_cast<uint8_t>(out), static_cast<uint8_t>(out >> 8),
                            static_cast<uint8_t>(out >> 16), static_cast<uint8_t>(out >> 24), 0xE8, 0x03};
    const DeployImage img = image({{0x200, ActorType::DieTemp, 6, cfg}});
    const ActorKind table[] = {{ActorType::DieTemp, "die_temp", &die_temp_check, nullptr, nullptr, &die_temp_outputs}};
    ActorRuntime rt(table, 1);
    ActorEnv env;
    env.node = n.node;
    CHECK(rt.load(img, 0x100, env, nullptr));
    CHECK_EQ(rt.loaded(), static_cast<size_t>(0));  // not ours to run
    rt.start(env, 0, nullptr);
    CHECK_EQ(rt.remote_outputs(), static_cast<size_t>(1));  // declared at boot: a pinned actor's per-node output
    const NsEntry* e = n.node->ns().find(out);
    CHECK(e != nullptr);
    if (e == nullptr) return;
    CHECK_EQ(static_cast<unsigned>(e->owner_node), 0x200u);
    CHECK_EQ(static_cast<int>(e->unit), static_cast<int>(Unit::Celsius));
    Reading r;
    n.node->read(out, r);
    CHECK_EQ(static_cast<int>(r.quality), static_cast<int>(Quality::Unavailable));  // its owner is no peer of ours
}

// ---- M8.2 (PS-1): external components join the table ----------------------------------------------

namespace {
const ActorKind kExtA[] = {{static_cast<ActorType>(0x80), "rf_link", &probe_check, &probe_create}};
const ActorKind kExtClash[] = {{static_cast<ActorType>(0x80), "other", &probe_check, &probe_create}};
const ActorKind kExtLow[] = {{static_cast<ActorType>(0x10), "squatter", &probe_check, &probe_create}};
const ActorKind* rows_a(size_t* n) { *n = 1; return kExtA; }
const ActorKind* rows_clash(size_t* n) { *n = 1; return kExtClash; }
const ActorKind* rows_low(size_t* n) { *n = 1; return kExtLow; }
}  // namespace

TEST(actor_runtime, external_components_rows_follow_the_builtins) {
    const AppRows apps[] = {{"pot_passive_sensor", &rows_a}};
    ActorKind out[kMaxActorKinds];
    size_t n = 0;
    const char* why = nullptr;
    CHECK(merge_actor_tables(kTable, 2, apps, 1, out, kMaxActorKinds, n, &why, nullptr, nullptr));
    CHECK_EQ(n, static_cast<size_t>(3));
    CHECK(find_actor_kind(out, n, static_cast<ActorType>(0x80)) != nullptr);
}

TEST(actor_runtime, two_rows_for_one_type_are_refused_naming_both_components) {
    const AppRows apps[] = {{"pot_passive_sensor", &rows_a}, {"pme_extra", &rows_clash}};
    ActorKind out[kMaxActorKinds];
    size_t n = 0;
    const char *why = nullptr, *a = nullptr, *b = nullptr;
    CHECK(!merge_actor_tables(kTable, 2, apps, 2, out, kMaxActorKinds, n, &why, &a, &b));
    CHECK(a != nullptr && std::strcmp(a, "pot_passive_sensor") == 0);
    CHECK(b != nullptr && std::strcmp(b, "pme_extra") == 0);
    CHECK_EQ(n, static_cast<size_t>(2));  // the built-ins only: the node still runs
}

TEST(actor_runtime, an_external_row_in_potlucks_range_is_refused) {
    const AppRows apps[] = {{"squatter", &rows_low}};
    ActorKind out[kMaxActorKinds];
    size_t n = 0;
    const char *why = nullptr, *a = nullptr;
    CHECK(!merge_actor_tables(kTable, 2, apps, 1, out, kMaxActorKinds, n, &why, &a, nullptr));
    CHECK(a != nullptr && std::strcmp(a, "squatter") == 0);
}

TEST(actor_runtime, the_image_parser_carries_an_external_type_and_its_portable_header) {
    // node 0xFFFE, type 0xF1, cfg = header (out_hash, period 200, 1 node 0x0100 gravity 1) + step 3
    const uint8_t cfg[11] = {0x78, 0x56, 0x34, 0x12, 0xC8, 0x00, 1, 0x00, 0x01, 1, 3};
    uint8_t img[kImageHeaderLen + 4 + sizeof(cfg)] = {};
    const uint32_t magic = kImageMagic;
    std::memcpy(img, &magic, 4);
    img[4] = kImageVersion;
    img[5] = 1;  // one actor
    const uint32_t body = 4 + sizeof(cfg);
    std::memcpy(img + 20, &body, 4);
    img[24] = 0xFE;
    img[25] = 0xFF;
    img[26] = 0xF1;
    img[27] = sizeof(cfg);
    std::memcpy(img + 28, cfg, sizeof(cfg));
    DeployImage di{};
    const char* why = nullptr;
    CHECK(parse_image(img, sizeof(img), di, &why));
    TickerConfig h{};
    size_t len = 0;
    CHECK(portable_header(di.actors[0], h, len));
    CHECK_EQ(len, static_cast<size_t>(10));
    CHECK_EQ(h.out_hash, 0x12345678u);
    CHECK_EQ(static_cast<unsigned>(di.actors[0].cfg[len]), 3u);  // the actor's own byte after the header
    img[26] = 0x7E;  // the same bytes under an unknown BUILT-IN number: refused, as before
    CHECK(!parse_image(img, sizeof(img), di, &why));
}

TEST(actor_runtime, every_rx_sample_reaches_every_running_actor) {
    // M8.2 (PS-2): delivered synchronously, so nothing is queued in between to overflow.
    OneNode n;
    const uint8_t a[3] = {1, 0, 0}, b[3] = {2, 0, 0};
    const DeployImage img = image({{0x100, ActorType::Led, 3, a}, {0x100, ActorType::Fault, 3, b}});
    ActorRuntime rt(kTable, 2);
    ActorEnv env;
    env.node = n.node;
    CHECK(rt.load(img, 0x100, env, nullptr));
    CHECK_EQ(rt.start(env, 0, nullptr), static_cast<size_t>(2));
    for (int k = 0; k < 1000; ++k) {
        RxSample rs{};
        rs.node_id = 0x101;
        rs.rssi = static_cast<int8_t>(-40 - (k % 20));
        rs.kind = kRxKindBeacon;
        rs.hb_seq = static_cast<uint16_t>(k);
        rt.on_rx_sample(rs);
    }
    for (size_t i = 0; i < 2; ++i) {
        CHECK_EQ(static_cast<Probe*>(rt.actor(i))->samples, 1000u);
        CHECK_EQ(static_cast<Probe*>(rt.actor(i))->last_rssi, -40 - 19);
    }
}

// ---- M8.2 (PS-3): several outputs, per link, adopted on demand; and a portable actor's set ---------

#include "pot/reconcile.hpp"

namespace {
// An every-node "link meter": per link, two outputs under potluck://lab/node-<on>/test/link-<peer>/.
constexpr uint32_t kLabPrefix = path_hash("potluck://lab/");
size_t meter_outputs(const ActorDecl&, uint16_t node, uint16_t peer, NsDecl* out, size_t cap) {
    if (peer == 0 || cap < 2) return 0;
    char buf[64];
    const char* fields[2] = {"mean", "count"};
    for (int i = 0; i < 2; ++i) {
        std::snprintf(buf, sizeof(buf), "node-%04x/test/link-%04x/%s", static_cast<unsigned>(node),
                      static_cast<unsigned>(peer), fields[i]);
        out[i] = NsDecl{};
        out[i].path_hash = path_hash_from(kLabPrefix, buf);
        out[i].owner_node = node;
        out[i].type = i == 0 ? ValueType::F32 : ValueType::U32;
        out[i].kind = ResourceKind::Sampled;
        out[i].access = Access::Read;
        out[i].latency_class = kClassL4;
        out[i].staleness_bound_ms = 3000;
    }
    return 2;
}
// A portable "fusion" with three outputs under act/fusion/.
size_t fusion_outputs(const ActorDecl&, uint16_t, uint16_t peer, NsDecl* out, size_t cap) {
    if (peer != 0 || cap < 3) return 0;
    const char* paths[3] = {"potluck://lab/act/fusion/presence", "potluck://lab/act/fusion/confidence",
                            "potluck://lab/act/fusion/motion"};
    for (int i = 0; i < 3; ++i) {
        out[i] = NsDecl{};
        out[i].path_hash = path_hash(paths[i]);
        out[i].type = ValueType::None;
        out[i].kind = ResourceKind::Sampled;
        out[i].access = Access::Read;
        out[i].latency_class = kClassL4;
        out[i].staleness_bound_ms = 1000;
    }
    return 3;
}
bool fusion_place(const ActorDecl&, TickerConfig& out) {
    out = TickerConfig{};
    out.out_hash = path_hash("potluck://lab/act/fusion/presence");
    out.period_ms = 200;
    out.count = 1;
    out.node[0] = 0x100;
    return true;
}
const ActorKind kPs3Table[] = {
    {static_cast<ActorType>(0x80), "meter", &probe_check, &probe_create, nullptr, &meter_outputs},
    {static_cast<ActorType>(0x81), "fusion", &probe_check, &probe_create, &fusion_place, &fusion_outputs},
};
}  // namespace

TEST(actor_runtime, path_hash_from_continues_a_prefix) {
    CHECK_EQ(path_hash_from(path_hash("potluck://lab/"), "node-7368/rf/link-8160/mean"),
             path_hash("potluck://lab/node-7368/rf/link-8160/mean"));
}

TEST(actor_runtime, an_every_node_actors_per_link_output_on_another_node_is_adopted_on_demand) {
    OneNode n;  // node 0x100, whose peers are 0x200 and 0x300
    const uint8_t cfg[3] = {1, 0, 0};
    const DeployImage img = image({{kEveryNode, static_cast<ActorType>(0x80), 3, cfg}});
    ActorRuntime rt(kPs3Table, 2);
    ActorEnv env;
    env.node = n.node;
    CHECK(rt.load(img, 0x100, env, nullptr));
    CHECK_EQ(rt.start(env, 0, nullptr), static_cast<size_t>(1));  // runs here too
    const uint16_t cell[3] = {0x100, 0x200, 0x300};
    const uint32_t want = path_hash("potluck://lab/node-0200/test/link-0300/count");
    CHECK(n.node->ns().find(want) == nullptr);           // nothing declared up front
    CHECK(rt.adopt(n.node->ns(), want, cell, 3));
    const NsEntry* e = n.node->ns().find(want);
    CHECK(e != nullptr && e->owner_node == 0x200);       // a replica of node 0x200's output
    CHECK(!rt.adopt(n.node->ns(), path_hash("potluck://lab/node-0200/test/link-0200/count"), cell, 3));  // no self-link
    CHECK(!rt.adopt(n.node->ns(), path_hash("potluck://lab/nothing"), cell, 3));
}

TEST(actor_runtime, a_read_that_finds_no_entry_asks_the_adopt_handler) {
    OneNode n;
    static int asked = 0;
    asked = 0;
    const uint32_t h = path_hash("potluck://lab/node-0200/test/link-0300/mean");
    n.node->set_adopt_handler(
        [](void* ctx, uint32_t ph) {
            ++asked;
            NsDecl d;
            d.path_hash = ph;
            d.owner_node = 0x200;
            d.type = ValueType::F32;
            return static_cast<Node*>(ctx)->ns().declare(d) == NsError::Ok;
        },
        n.node);
    Reading r;
    CHECK_EQ(static_cast<int>(n.node->read(h, r)), static_cast<int>(NsError::Ok));  // not NotFound
    CHECK_EQ(asked, 1);
    CHECK_EQ(static_cast<int>(r.quality), static_cast<int>(Quality::Unavailable));  // 0x200 is no peer of ours
    n.node->read(h, r);
    CHECK_EQ(asked, 1);  // declared now: not asked again
}

TEST(actor_runtime, a_portable_actors_outputs_are_all_declared_and_move_together) {
    OneNode n;
    const uint8_t cfg[3] = {9, 0, 0};
    const DeployImage img = image({{kPortableNode, static_cast<ActorType>(0x81), 3, cfg}});
    PortableSpec specs[kMaxPortable];
    size_t count = 0;
    CHECK(collect_portable(img, kPs3Table, 2, specs, kMaxPortable, count, nullptr));
    Reconciler rec(*n.node);
    CHECK(rec.load(specs, count));
    const uint32_t outs[3] = {path_hash("potluck://lab/act/fusion/presence"),
                              path_hash("potluck://lab/act/fusion/confidence"),
                              path_hash("potluck://lab/act/fusion/motion")};
    for (uint32_t h : outs) {
        const NsEntry* e = n.node->ns().find(h);
        CHECK(e != nullptr && e->owner_node == 0);  // declared, nobody's yet
    }
    g_now = 1000;
    rec.start(g_now);
    for (int k = 0; k < 2000 && !rec.view(0).running; ++k) rec.tick(++g_now);
    for (int k = 0; k < 10; ++k) rec.tick(++g_now);
    CHECK(rec.view(0).running);
    for (uint32_t h : outs) {
        const NsEntry* e = n.node->ns().find(h);
        CHECK(e != nullptr && e->owner_node == 0x100);  // every output moved to the holder
    }
}

// ---- M8.2 (PS-5): the board's services reach portable actors through the reconciler ---------------

namespace {
const BoardServices* g_seen_board = nullptr;
Actor* board_probe_create(void* mem, const ActorDecl& d, const ActorEnv& env) {
    g_seen_board = env.board;
    return probe_create(mem, d, env);
}
const ActorKind kBoardTable[] = {
    {ActorType::Led, "probe", &probe_check, &board_probe_create, &probe_place},
};
}  // namespace

TEST(actor_runtime, a_portable_actor_gets_the_boards_services) {
    OneNode n;
    const uint8_t cfg[3] = {5, 0, 0};
    const DeployImage img = image({{kPortableNode, ActorType::Led, 3, cfg}});
    PortableSpec specs[kMaxPortable];
    size_t count = 0;
    CHECK(collect_portable(img, kBoardTable, 1, specs, kMaxPortable, count, nullptr));
    static const BoardServices board{};
    g_seen_board = nullptr;
    {
        Reconciler rec(*n.node);
        rec.set_board(&board);
        CHECK(rec.load(specs, count));
        g_now = 1000;
        rec.start(g_now);
        for (int k = 0; k < 2000 && g_seen_board == nullptr; ++k) rec.tick(++g_now);
    }
    CHECK(g_seen_board == &board);
}
