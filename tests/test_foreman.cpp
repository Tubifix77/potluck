// M19: a cell's foreman -- the role placed by the reconciler, the standby beside it, both in the parent cell; the
// summary the parent's members read; a referral; and the handover when the foreman dies.
//
// Two base cells of three boards (cell 1: 0x100, 0x101, 0x102; cell 2: 0x200, 0x201, 0x202), the first two of each
// foreman-eligible, under an upper cell 3 with a 5 s window. Every board keeps what the image's scope section would
// give it (pot/deploy.hpp): its cell's foreman role, and -- when it may hold that role -- the sibling cell's too.

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "pot/foreman.hpp"
#include "pot/node.hpp"
#include "pot/opcodes.hpp"
#include "pot/reconcile.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

constexpr uint32_t kPrefix = path_hash("potluck://lab/node-");
uint32_t temp_hash(uint16_t node) {
    char p[64];
    std::snprintf(p, sizeof p, "potluck://lab/node-%04x/hw/t", static_cast<unsigned>(node));
    return path_hash(p);
}
uint32_t cell_hash(int cell, const char* what) {
    char p[64];
    std::snprintf(p, sizeof p, "potluck://lab/cell-c%d/%s", cell, what);
    return path_hash(p);
}

// The foreman declaration potluck.deploy compiles: alive (its identity), and the max of each board's hw/t.
std::vector<uint8_t> foreman_cfg(int cell, std::vector<uint16_t> eligible, std::vector<uint16_t> members) {
    std::vector<uint8_t> c;
    put32(c, cell_hash(cell, "alive"));
    put16(c, 1000);
    c.push_back(static_cast<uint8_t>(eligible.size()));
    for (uint16_t n : eligible) {
        put16(c, n);
        c.push_back(1);
    }
    c.push_back(static_cast<uint8_t>(cell));
    c.push_back(3);   // the parent
    c.push_back(50);  // its window, 5 s
    c.push_back(static_cast<uint8_t>(members.size()));
    put32(c, kPrefix);
    for (uint16_t n : members) put16(c, n);
    c.push_back(2);
    put32(c, cell_hash(cell, "alive"));
    c.push_back(static_cast<uint8_t>(ExportKind::Alive));
    put16(c, 0);
    put32(c, cell_hash(cell, "t_max"));
    c.push_back(static_cast<uint8_t>(ExportKind::Max));
    put16(c, 0);
    c.push_back(4);
    for (char ch : std::string("hw/t")) c.push_back(static_cast<uint8_t>(ch));
    return c;
}

PortableSpec spec_of(const std::vector<uint8_t>& cfg) {
    PortableSpec s{};
    std::memcpy(s.cfg, cfg.data(), cfg.size());
    s.kind = &kForemanKind;
    s.decl = ActorDecl{kPortableNode, ActorType::Foreman, static_cast<uint8_t>(cfg.size()), s.cfg};
    CHECK(kForemanKind.placement(s.decl, s.place));
    return s;
}

struct Tree;

struct FNode {
    Tree* owner = nullptr;
    Tree* tree() const { return owner; }
    uint16_t id = 0;
    uint8_t cell = 0;
    size_t index = 0;
    bool down = false;
    uint32_t epoch = 1;
    uint8_t mac[kMacLen] = {};
    Node* node = nullptr;
    Reconciler* rec = nullptr;
    NodeHal hal{};
    std::vector<PortableSpec> specs;
    float temp = 0;
    uint8_t ch = 1;  // the radio's channel, when the tree models channels
};

struct Tree {
    std::vector<FNode> nodes;
    uint32_t now_us = 0;
    bool channels = false;  // model the radio's channel: a frame is heard only on the channel it was sent on
    struct Flight {
        uint32_t due_us;
        size_t from, to;
        bool bcast;
        std::vector<uint8_t> data;
        uint8_t ch;
    };
    std::vector<Flight> air;

    static bool on_call(void* ctx, uint16_t from, uint16_t, uint32_t path, const uint8_t* args, uint16_t len) {
        return static_cast<FNode*>(ctx)->rec->on_cast(from, path, args, len);
    }
    static int32_t send(void* ctx, const uint8_t mac[kMacLen], const uint8_t* data, size_t len);

    void boot(FNode& t) {
        delete t.rec;
        delete t.node;
        NodeConfig cfg;
        cfg.node_id = t.id;
        cfg.boot_epoch = t.epoch;
        std::memcpy(cfg.mac, t.mac, kMacLen);
        cfg.hello_interval_ms = 500;
        t.hal.ctx = &t;
        t.hal.send = &Tree::send;
        t.hal.now_ms = [](void* c) { return static_cast<FNode*>(c)->tree()->now_us / 1000; };
        t.hal.now_us = [](void* c) { return static_cast<FNode*>(c)->tree()->now_us; };
        t.ch = 1;
        if (channels) t.hal.set_channel = [](void* c, uint8_t ch) { static_cast<FNode*>(c)->ch = ch; };
        t.node = new Node(cfg, t.hal);
        t.node->set_groups(t.cell, 0);
        // its own temperature, and a replica of each cell-mate's (what the image's pinned outputs would declare)
        for (const FNode& o : nodes) {
            if (o.cell != t.cell) continue;
            NsDecl d;
            d.path_hash = temp_hash(o.id);
            d.owner_node = o.id;
            d.type = ValueType::F32;
            d.staleness_bound_ms = 3000;
            CHECK(t.node->ns().declare(d) == NsError::Ok);
        }
        t.rec = new Reconciler(*t.node);
        CHECK(t.rec->load(t.specs.data(), t.specs.size()));
        t.node->set_call_handler(&Tree::on_call, &t);
        t.node->start();
        t.rec->start(now_us / 1000);
        t.down = false;
    }

    void build() {
        const std::vector<uint8_t> f1 = foreman_cfg(1, {0x100, 0x101}, {0x100, 0x101, 0x102});
        const std::vector<uint8_t> f2 = foreman_cfg(2, {0x200, 0x201}, {0x200, 0x201, 0x202});
        const uint16_t ids[6] = {0x100, 0x101, 0x102, 0x200, 0x201, 0x202};
        nodes.resize(6);
        for (size_t i = 0; i < 6; ++i) {
            FNode& t = nodes[i];
            t.id = ids[i];
            t.cell = static_cast<uint8_t>(i < 3 ? 1 : 2);
            t.index = i;
            t.owner = this;
            t.mac[0] = 0x02;
            t.mac[5] = static_cast<uint8_t>(i);
            t.temp = 20.0f + static_cast<float>(i) * 2.5f;
            const bool eligible = i % 3 != 2;
            const auto& own = t.cell == 1 ? f1 : f2;
            const auto& sib = t.cell == 1 ? f2 : f1;
            t.specs.push_back(spec_of(own));
            if (eligible) t.specs.push_back(spec_of(sib));  // the scope rule: its role's parent is cell 3
        }
        for (FNode& t : nodes) boot(t);
    }
    ~Tree() {
        for (FNode& t : nodes) {
            delete t.rec;
            delete t.node;
        }
    }

    void deliver() {
        std::vector<Flight> due;
        for (size_t k = 0; k < air.size();) {
            if (air[k].due_us <= now_us) {
                due.push_back(std::move(air[k]));
                air.erase(air.begin() + static_cast<std::ptrdiff_t>(k));
            } else {
                ++k;
            }
        }
        for (Flight& f : due) {
            FNode& from = nodes[f.from];
            FNode& to = nodes[f.to];
            // one radio space: admission does the separating (and the channel, when modelled)
            const bool heard = !to.down && to.node != nullptr && (!channels || to.ch == f.ch);
            if (heard) to.node->on_rx(from.mac, f.data.data(), f.data.size(), now_us, -50);
            if (!f.bcast && !from.down && from.node != nullptr) from.node->on_tx_done(to.mac, heard, now_us);
        }
    }

    void run(uint32_t ms) {
        for (uint32_t k = 0; k < ms; ++k) {
            now_us += 1000;
            deliver();
            for (FNode& t : nodes) {
                if (t.down) continue;
                const uint32_t now = now_us / 1000;
                if (now % 500 == 0) t.node->publish(temp_hash(t.id), Value::of_f32(t.temp));
                t.node->tick(now);
                t.rec->tick(now);
            }
        }
    }

    FNode& by_id(uint16_t id) {
        for (FNode& t : nodes)
            if (t.id == id) return t;
        return nodes[0];
    }
    // Who runs cell `cell`'s foreman role: the first board of that cell with its slot 0 running.
    int foreman_of(int cell) {
        int who = -1, count = 0;
        for (FNode& t : nodes) {
            if (t.down || t.cell != cell) continue;
            if (t.rec->view(0).running) {
                who = t.id;
                ++count;
            }
        }
        return count == 1 ? who : -count;
    }
    bool alive_at(const FNode& t, uint16_t peer) const {
        const PeerLink* p = t.node->peers().find_by_node_id(peer);
        return p != nullptr && p->state == PeerState::Alive;
    }
    // A fresh value of `h` at `t`, asked of its owner.
    Reading read_fresh(FNode& t, uint32_t h) {
        const NsEntry* e = t.node->ns().find(h);
        if (e != nullptr && e->owner_node != 0 && e->owner_node != t.id) t.node->request_read(e->owner_node, h);
        run(100);
        Reading r;
        t.node->read(h, r);
        return r;
    }
};

int32_t Tree::send(void* ctx, const uint8_t mac[kMacLen], const uint8_t* data, size_t len) {
    FNode* from = static_cast<FNode*>(ctx);
    Tree* c = from->tree();
    if (from->down) return 0;
    const bool bcast = std::memcmp(mac, kBroadcastMacAddr, kMacLen) == 0;
    for (size_t i = 0; i < c->nodes.size(); ++i) {
        if (i == from->index) continue;
        if (bcast || std::memcmp(c->nodes[i].mac, mac, kMacLen) == 0) {
            c->air.push_back(Flight{c->now_us + 3000, from->index, i, bcast, std::vector<uint8_t>(data, data + len),
                                    from->ch});
        }
    }
    return 0;
}

}  // namespace

TEST(foreman, the_role_and_its_standby_join_the_upper_cell_and_the_members_stay_home) {
    Tree t;
    t.build();
    t.run(10000);
    const int f1 = t.foreman_of(1), f2 = t.foreman_of(2);
    CHECK(f1 == 0x100 || f1 == 0x101);
    CHECK(f2 == 0x200 || f2 == 0x201);
    // the foreman and the standby are in cell 3 as well; the third board only in its own
    CHECK_EQ(t.by_id(0x100).node->groups(), static_cast<uint16_t>(1 | 3 << 8));
    CHECK_EQ(t.by_id(0x101).node->groups(), static_cast<uint16_t>(1 | 3 << 8));
    CHECK_EQ(t.by_id(0x102).node->groups(), static_cast<uint16_t>(1));
    CHECK_EQ(t.by_id(0x202).node->groups(), static_cast<uint16_t>(2));
    CHECK(t.by_id(0x100).rec->view(0).standby != 0);
    // the upper cell: the four role holders hear each other; a member hears only its own cell
    CHECK(t.alive_at(t.by_id(0x100), 0x200) && t.alive_at(t.by_id(0x101), 0x201));
    CHECK(!t.alive_at(t.by_id(0x102), 0x200) && !t.alive_at(t.by_id(0x202), 0x101));
    CHECK(t.alive_at(t.by_id(0x102), 0x100));
}

TEST(foreman, the_upper_cell_reads_the_summary_and_a_referral_with_the_tuple) {
    Tree t;
    t.build();
    t.run(10000);
    const uint16_t f1 = static_cast<uint16_t>(t.foreman_of(1));
    FNode& reader = t.by_id(0x200);  // cell 2's, in the upper cell
    t.run(3000);
    Reading r = t.read_fresh(reader, cell_hash(1, "alive"));
    uint32_t alive = 0;
    CHECK(r.quality == Quality::Good && r.value.as_u32(alive) && alive == 3u);
    r = t.read_fresh(reader, cell_hash(1, "t_max"));
    float v = 0;
    Quality q;
    CHECK(r.get_f32(v, q) && v == 25.0f);  // 0x102's 25.0, the warmest of cell 1
    CHECK_EQ(reader.node->ns().find(cell_hash(1, "t_max"))->owner_node, f1);
    // a referral: 0x102 is no peer of 0x200; cell 1's foreman answers for it, under its own identity
    CHECK(reader.node->ns().find(temp_hash(0x102)) == nullptr);
    CHECK(reader.node->request_referral(f1, temp_hash(0x102)) != 0);
    t.run(100);
    CHECK_EQ(reader.node->ns().find(temp_hash(0x102))->owner_node, f1);
    t.read_fresh(reader, temp_hash(0x102));  // the first answer primes the foreman's own replica
    r = t.read_fresh(reader, temp_hash(0x102));
    CHECK(r.get_f32(v, q) && v == 25.0f && r.quality == Quality::Good);
    CHECK(r.age_ms < 1000);
}

TEST(foreman, a_dead_foreman_hands_the_role_to_the_standby_within_the_upper_window_plus_ten_seconds) {
    Tree t;
    t.build();
    t.run(10000);
    const uint16_t f1 = static_cast<uint16_t>(t.foreman_of(1));
    const uint16_t standby = f1 == 0x100 ? 0x101 : 0x100;
    FNode& reader = t.by_id(0x200);
    t.read_fresh(reader, cell_hash(1, "alive"));
    t.by_id(f1).down = true;
    uint32_t back_ms = 0;
    for (uint32_t ms = 0; ms < 15000 && back_ms == 0; ms += 250) {
        Reading r = t.read_fresh(reader, cell_hash(1, "alive"));
        uint32_t alive = 0;
        if (r.quality == Quality::Good && r.value.as_u32(alive) && alive == 2u &&
            reader.node->ns().find(cell_hash(1, "alive"))->owner_node == standby) {
            back_ms = ms + 100;
        }
        t.run(150);
    }
    std::printf("    the standby took the role and the summary was GOOD again at the reader after %u ms\n",
                static_cast<unsigned>(back_ms));
    CHECK(back_ms > 0 && back_ms <= 15000);  // the upper window (5 s) plus 10 s
    CHECK_EQ(t.foreman_of(1), static_cast<int>(standby));
    // and with cell 1's last eligible board gone too, a referral to it reads Unavailable -- never an old value
    reader.node->request_referral(standby, temp_hash(0x102));
    t.run(200);
    t.read_fresh(reader, temp_hash(0x102));
    t.by_id(standby).down = true;
    t.run(6000);
    Reading r;
    reader.node->read(temp_hash(0x102), r);
    CHECK(r.quality == Quality::Unavailable);
}

TEST(foreman, the_host_compilers_foreman_declaration_parses_here) {
    // potluck.deploy for cell "house" (1) of boards 0100, 0101 (both may be foreman) and 0102 under "tree" (2, 5 s):
    // alive, a_temp (a copy of node-0100/hw/die_temp) and warmest (the max of every board's hw/die_temp), 1 s
    const char* hex =
        "d2bde385e8030200010101010101023203f885aaca00010101020103d2bde385006400ef7e500d01320004e37c33fe3c9a659e023200"
        "0b68772f6469655f74656d70";
    std::vector<uint8_t> cfg;
    for (size_t i = 0; hex[i] != 0; i += 2) cfg.push_back(static_cast<uint8_t>(std::stoi(std::string(hex + i, 2), nullptr, 16)));
    const ActorDecl d{kPortableNode, ActorType::Foreman, static_cast<uint8_t>(cfg.size()), cfg.data()};
    ForemanConfig c{};
    CHECK(foreman_config(d, c));
    CHECK(c.place.out_hash == path_hash("potluck://home/cell-house/alive") && c.place.period_ms == 1000);
    CHECK(c.place.count == 2 && c.place.node[0] == 0x0100 && c.place.node[1] == 0x0101);
    CHECK(c.cell == 1 && c.parent == 2 && c.parent_window == 50 && c.n_members == 3 && c.members[2] == 0x0102);
    CHECK_EQ(c.n_exports, static_cast<uint8_t>(3));
    const ForemanExport alive = c.export_at(0), copy = c.export_at(1), warm = c.export_at(2);
    CHECK(alive.kind == ExportKind::Alive && alive.unit == Unit::Count);
    CHECK(copy.kind == ExportKind::Copy && copy.type == ValueType::F32 && copy.unit == Unit::Celsius &&
          copy.src_hash == path_hash("potluck://home/node-0100/hw/die_temp") &&
          copy.out_hash == path_hash("potluck://home/cell-house/a_temp"));
    CHECK(warm.kind == ExportKind::Max && warm.out_hash == path_hash("potluck://home/cell-house/warmest"));
    CHECK_EQ(c.input_hash(warm, 0x0101), path_hash("potluck://home/node-0101/hw/die_temp"));
    NsDecl outs[kMaxOutputs];
    CHECK_EQ(kForemanKind.outputs(d, 0, 0, outs, kMaxOutputs), size_t{3});
    CHECK(outs[0].type == ValueType::U32 && outs[2].type == ValueType::F32 && outs[2].latency_class == kClassL4);
    // a board that may hold the role and is not one of the cell's: refused
    auto bad = cfg;
    bad[7] = 0x07;  // the first eligible node becomes 0x0107
    const ActorDecl bd{kPortableNode, ActorType::Foreman, static_cast<uint8_t>(bad.size()), bad.data()};
    CHECK(!foreman_config(bd, c));
}

TEST(foreman, a_channel_move_in_one_cell_is_carried_to_the_whole_tree) {
    // Review of M19 #7: cell 1 moves to channel 6. Its foreman and standby carry the move to the upper cell, whose
    // other two members carry it to cell 2 -- whose third board never heard cell 1 at all.
    Tree t;
    t.channels = true;
    t.build();
    t.run(10000);
    CHECK(t.by_id(0x102).node->move_cell(6, 300));
    t.run(8000);
    for (FNode& n : t.nodes) CHECK_EQ(static_cast<int>(n.ch), 6);
    CHECK(t.by_id(0x100).node->channel_counters().moves_carried + t.by_id(0x101).node->channel_counters().moves_carried >= 1);
    CHECK(t.alive_at(t.by_id(0x202), 0x200) && t.alive_at(t.by_id(0x200), 0x100));
    CHECK_EQ(t.by_id(0x102).node->channel_counters().moves_carried, 0u);  // in one cell: nothing to carry
    // and it does not keep going: a second look later finds no new carries
    uint32_t before = 0, after = 0;
    for (FNode& n : t.nodes) before += n.node->channel_counters().moves_carried;
    t.run(5000);
    for (FNode& n : t.nodes) after += n.node->channel_counters().moves_carried;
    CHECK_EQ(after, before);
}
