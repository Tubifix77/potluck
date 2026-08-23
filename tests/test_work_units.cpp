// Work units across the link -- section 7.8's CALL / REPLY / CAST, between real Nodes.
//
// Section 7.8 is the compute half of the project: a coordinator hands units of work to peers that
// would otherwise be idle. The wire format for it is tested in test_ns_wire.cpp; what is tested
// here is the behaviour a coordinator depends on, and one property above all others:
//
//   a unit is either answered or written off, exactly once.
//
// A coordinator told twice hands the same work out twice. A coordinator told nothing waits forever
// for a worker that is already gone. Both are silent failures, so both get a test.

#include <cstring>
#include <vector>

#include "pot/node.hpp"
#include "pot/ns_payloads.hpp"
#include "pot/opcodes.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

struct WorkCell;

// What a worker was asked to do, kept until the test decides it finishes. Real work units run for
// seconds -- section 7.8's whole premise is that the round trip is cheap next to the job -- so the
// handler accepting and the result arriving are separate events here too.
struct WorkUnit {
    uint16_t from = 0;
    uint16_t msg_id = 0;
    uint32_t path_hash = 0;
    std::vector<uint8_t> args;
};

// What a coordinator was told, in order.
struct Outcome {
    uint16_t worker = 0;
    uint16_t msg_id = 0;
    uint32_t path_hash = 0;
    Node::CallOutcome outcome = Node::CallOutcome::Ok;
    Value value{};
};

struct Slot {
    WorkCell* cell = nullptr;
    size_t index = 0;
    uint8_t mac[kMacLen] = {};
    Node* node = nullptr;
    NodeHal hal{};
    bool accept = true;              // whether this node's handler takes the work
    bool muted = false;              // its transport accepts frames; the air never sees them
    std::vector<WorkUnit> accepted;  // units it is holding
    std::vector<Outcome> results;    // what it was told about units it dispatched
};

// N nodes, a perfect link, a clock the test advances by hand, and a way to make one node stop
// existing. Loss is not modelled: what is under test is bookkeeping, and a flaky link would make
// every failure ambiguous.
struct WorkCell {
    std::vector<Slot> slots;
    uint32_t now_us = 0;

    void build(size_t n) {
        slots.resize(n);
        for (size_t i = 0; i < n; ++i) {
            Slot& s = slots[i];
            s.cell = this;
            s.index = i;
            s.mac[0] = 0x02;
            s.mac[5] = static_cast<uint8_t>(i);

            NodeConfig cfg;
            cfg.node_id = static_cast<uint16_t>(0x300 + i);
            cfg.boot_epoch = 1;
            std::memcpy(cfg.mac, s.mac, kMacLen);
            cfg.hello_interval_ms = 500;

            s.hal.ctx = &s;
            s.hal.send = &WorkCell::send;
            s.hal.now_ms = &WorkCell::now_ms;
            s.hal.now_us = &WorkCell::now_us_cb;
            s.node = new Node(cfg, s.hal);
            s.node->set_call_handler(&WorkCell::on_call, &s);
            s.node->set_call_result(&WorkCell::on_result, &s);
        }
    }
    ~WorkCell() {
        for (Slot& s : slots) {
            delete s.node;
        }
    }

    void start() {
        for (Slot& s : slots) {
            s.node->start();
        }
    }
    void advance_ms(uint32_t ms) {
        for (uint32_t k = 0; k < ms; ++k) {
            now_us += 1000;
            for (Slot& s : slots) {
                if (s.node != nullptr) {
                    s.node->tick(now_us / 1000);
                }
            }
        }
    }

    // A node that stops existing: no ticks, no frames in, no frames out. Its peers must reach
    // "dead" on their own, from the heartbeat miss limit, which is the only definition of gone.
    void kill(size_t i) {
        delete slots[i].node;
        slots[i].node = nullptr;
    }

    // A node that comes back as a new incarnation: same id and MAC, next boot epoch, and no memory
    // of any unit it had accepted.
    void reboot(size_t i) {
        Slot& s = slots[i];
        NodeConfig cfg = s.node->config();
        cfg.boot_epoch += 1;
        delete s.node;
        s.accepted.clear();
        s.node = new Node(cfg, s.hal);
        s.node->set_call_handler(&WorkCell::on_call, &s);
        s.node->set_call_result(&WorkCell::on_result, &s);
        s.node->start();
    }

    uint16_t id(size_t i) const { return static_cast<uint16_t>(0x300 + i); }

    static uint32_t now_ms(void* c) { return static_cast<Slot*>(c)->cell->now_us / 1000; }
    static uint32_t now_us_cb(void* c) { return static_cast<Slot*>(c)->cell->now_us; }

    static int32_t send(void* ctx, const uint8_t mac[kMacLen], const uint8_t* data, size_t len) {
        Slot* from = static_cast<Slot*>(ctx);
        WorkCell* c = from->cell;
        if (from->muted) {
            return 0;  // the transport took it; nobody hears it. A one-way outage, not a crash.
        }
        const bool bcast = std::memcmp(mac, kBroadcastMacAddr, kMacLen) == 0;
        for (size_t i = 0; i < c->slots.size(); ++i) {
            if (i == from->index || c->slots[i].node == nullptr) {
                continue;
            }
            if (bcast || std::memcmp(c->slots[i].mac, mac, kMacLen) == 0) {
                c->slots[i].node->on_rx(from->mac, data, len, c->now_us, -50);
            }
        }
        if (!bcast) {
            from->node->on_tx_done(mac, true, c->now_us);
        }
        return 0;
    }

    static bool on_call(void* ctx, uint16_t from_node, uint16_t msg_id, uint32_t path_hash,
                        const uint8_t* args, uint16_t arg_len) {
        Slot* s = static_cast<Slot*>(ctx);
        if (!s->accept) {
            return false;
        }
        WorkUnit u;
        u.from = from_node;
        u.msg_id = msg_id;
        u.path_hash = path_hash;
        u.args.assign(args, args + arg_len);
        s->accepted.push_back(u);
        return true;
    }

    static void on_result(void* ctx, uint16_t from_node, uint16_t msg_id, uint32_t path_hash,
                          Node::CallOutcome outcome, const Value& v) {
        Slot* s = static_cast<Slot*>(ctx);
        Outcome o;
        o.worker = from_node;
        o.msg_id = msg_id;
        o.path_hash = path_hash;
        o.outcome = outcome;
        o.value = v;
        s->results.push_back(o);
    }
};

const uint32_t kJob = path_hash("potluck://lab/n0/compute/montecarlo");

// Bring a cell up and let HELLO admit everybody.
void join(WorkCell& c, size_t n) {
    c.build(n);
    c.start();
    c.advance_ms(1200);
}

}  // namespace

TEST(work, a_unit_arrives_with_its_arguments_intact) {
    WorkCell c;
    join(c, 2);

    const uint8_t args[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01};
    const uint16_t msg_id = c.slots[0].node->request_call(c.id(1), kJob, args, sizeof(args));
    CHECK(msg_id != 0);
    c.advance_ms(10);

    CHECK_EQ(c.slots[1].accepted.size(), static_cast<size_t>(1));
    if (c.slots[1].accepted.empty()) {
        return;
    }
    const WorkUnit& u = c.slots[1].accepted[0];
    CHECK_EQ(u.from, c.id(0));
    CHECK_EQ(u.msg_id, msg_id);
    CHECK_EQ(u.path_hash, kJob);
    CHECK_EQ(u.args.size(), sizeof(args));
    CHECK(std::memcmp(u.args.data(), args, sizeof(args)) == 0);

    // Accepted, not answered: the coordinator has been told nothing yet, and the slot is still
    // held. A handler that answered inline would have blocked the receive path for the whole job.
    CHECK_EQ(c.slots[0].results.size(), static_cast<size_t>(0));
    CHECK_EQ(c.slots[0].node->calls_outstanding(), static_cast<size_t>(1));
}

TEST(work, a_result_comes_back_as_a_value_and_frees_the_slot) {
    WorkCell c;
    join(c, 2);

    const uint32_t seed = 4242;
    const uint16_t msg_id = c.slots[0].node->request_call(
        c.id(1), kJob, reinterpret_cast<const uint8_t*>(&seed), sizeof(seed));
    CHECK(msg_id != 0);
    c.advance_ms(10);

    CHECK_EQ(c.slots[1].accepted.size(), static_cast<size_t>(1));
    if (c.slots[1].accepted.empty()) {
        return;  // nothing was dispatched; indexing it would abort the run rather than fail a case
    }

    // The worker finishes, seconds later, and answers with the one thing a reply can carry.
    c.advance_ms(3000);
    const WorkUnit u = c.slots[1].accepted[0];
    CHECK(c.slots[1].node->reply_call(u.from, u.msg_id, u.path_hash, Value::of_u32(785398)));
    c.advance_ms(10);

    CHECK_EQ(c.slots[0].results.size(), static_cast<size_t>(1));
    if (c.slots[0].results.empty()) {
        return;
    }
    const Outcome& o = c.slots[0].results[0];
    CHECK(o.outcome == Node::CallOutcome::Ok);
    CHECK_EQ(o.worker, c.id(1));
    CHECK_EQ(o.msg_id, msg_id);
    CHECK_EQ(o.path_hash, kJob);
    CHECK(o.value.type == ValueType::U32);
    uint32_t got = 0;
    CHECK(o.value.as_u32(got));
    CHECK_EQ(got, 785398u);
    CHECK_EQ(c.slots[0].node->calls_outstanding(), static_cast<size_t>(0));

    // A unit that ran for three seconds was answered on a link whose namespace request timeout is
    // two. What decides a worker is gone is the death window, never a deadline on the work.
    CHECK_EQ(c.slots[0].node->ns_counters().read_timeouts, 0u);
}

TEST(work, a_worker_with_no_handler_refuses_instead_of_going_quiet) {
    WorkCell c;
    join(c, 2);
    c.slots[1].accept = false;

    const uint16_t msg_id = c.slots[0].node->request_call(c.id(1), kJob, nullptr, 0);
    CHECK(msg_id != 0);
    c.advance_ms(10);  // nowhere near a death window, let alone a timeout

    CHECK_EQ(c.slots[0].results.size(), static_cast<size_t>(1));
    if (c.slots[0].results.empty()) {
        return;
    }
    CHECK(c.slots[0].results[0].outcome == Node::CallOutcome::Refused);
    CHECK_EQ(c.slots[0].node->calls_outstanding(), static_cast<size_t>(0));
    CHECK_EQ(c.slots[1].node->ns_counters().calls_refused, 1u);
}

TEST(work, a_worker_that_dies_holding_a_unit_gives_it_back_exactly_once) {
    // The acceptance property. Killing a worker mid-unit must lose no work: the coordinator has to
    // hear about the unit once, so it can hand it to somebody else once.
    WorkCell c;
    join(c, 3);

    const uint16_t msg_id = c.slots[0].node->request_call(c.id(1), kJob, nullptr, 0);
    CHECK(msg_id != 0);
    c.advance_ms(10);
    CHECK_EQ(c.slots[1].accepted.size(), static_cast<size_t>(1));

    c.kill(1);
    c.advance_ms(2000);  // past the death window, and past the read timeout too

    CHECK_EQ(c.slots[0].results.size(), static_cast<size_t>(1));
    if (c.slots[0].results.empty()) {
        return;
    }
    const Outcome& o = c.slots[0].results[0];
    CHECK(o.outcome == Node::CallOutcome::Unavailable);
    CHECK_EQ(o.msg_id, msg_id);
    CHECK_EQ(o.path_hash, kJob);
    CHECK(o.value.type == ValueType::None);
    CHECK_EQ(c.slots[0].node->ns_counters().calls_lost, 1u);

    // Told once, and the slot is free, so the same unit can go to the surviving worker.
    c.advance_ms(2000);
    CHECK_EQ(c.slots[0].results.size(), static_cast<size_t>(1));
    CHECK_EQ(c.slots[0].node->calls_outstanding(), static_cast<size_t>(0));
    CHECK(c.slots[0].node->request_call(c.id(2), kJob, nullptr, 0) != 0);
    c.advance_ms(10);
    CHECK_EQ(c.slots[2].accepted.size(), static_cast<size_t>(1));
}

TEST(work, a_rebooted_worker_has_forgotten_the_unit_and_the_unit_comes_back) {
    // A worker that reboots is answering again within milliseconds, so nothing times out and
    // nothing dies -- but the unit it accepted went with the old incarnation.
    WorkCell c;
    join(c, 2);

    const uint16_t msg_id = c.slots[0].node->request_call(c.id(1), kJob, nullptr, 0);
    CHECK(msg_id != 0);
    c.advance_ms(10);
    CHECK_EQ(c.slots[1].accepted.size(), static_cast<size_t>(1));

    c.reboot(1);
    c.advance_ms(1200);

    CHECK_EQ(c.slots[0].results.size(), static_cast<size_t>(1));
    if (c.slots[0].results.empty()) {
        return;
    }
    CHECK(c.slots[0].results[0].outcome == Node::CallOutcome::Unavailable);
    CHECK_EQ(c.slots[0].results[0].msg_id, msg_id);
    CHECK_EQ(c.slots[0].node->counters().reboots_seen, 1u);
}

TEST(work, a_late_result_for_a_written_off_unit_is_counted_not_delivered) {
    // The other half of "exactly once": a worker that answers a unit the coordinator has already
    // reassigned must not produce a second outcome for it.
    WorkCell c;
    join(c, 2);

    CHECK(c.slots[0].node->request_call(c.id(1), kJob, nullptr, 0) != 0);
    c.advance_ms(10);
    CHECK_EQ(c.slots[1].accepted.size(), static_cast<size_t>(1));
    PeerLink* worker = c.slots[0].node->peers().find_by_node_id(c.id(1));
    CHECK(worker != nullptr);
    if (c.slots[1].accepted.empty() || worker == nullptr) {
        return;
    }
    const WorkUnit u = c.slots[1].accepted[0];

    // Nothing the worker sends is heard any more, so the coordinator reaches the miss limit and
    // declares it dead -- while the worker itself is fine and still holding the unit. Setting
    // `misses` by hand would not do it: the next heartbeat resets the counter, which is the whole
    // point of it.
    (void)worker;
    c.slots[1].muted = true;
    c.advance_ms(1200);
    CHECK_EQ(c.slots[0].results.size(), static_cast<size_t>(1));
    if (c.slots[0].results.empty()) {
        return;
    }
    CHECK(c.slots[0].results[0].outcome == Node::CallOutcome::Unavailable);

    // And now the answer arrives anyway.
    c.slots[1].muted = false;
    CHECK(c.slots[1].node->reply_call(u.from, u.msg_id, u.path_hash, Value::of_u32(7)));
    c.advance_ms(10);
    CHECK_EQ(c.slots[0].results.size(), static_cast<size_t>(1));
    CHECK(c.slots[0].node->ns_counters().replies_unmatched >= 1u);
}

TEST(work, a_cast_runs_the_work_and_takes_no_slot) {
    WorkCell c;
    join(c, 2);

    const uint8_t args[] = {1, 2, 3};
    CHECK(c.slots[0].node->cast(c.id(1), kJob, args, sizeof(args)));
    c.advance_ms(10);

    CHECK_EQ(c.slots[1].accepted.size(), static_cast<size_t>(1));
    if (c.slots[1].accepted.empty()) {
        return;
    }
    CHECK_EQ(c.slots[1].accepted[0].args.size(), sizeof(args));
    // Nobody is waiting: no pending slot, and no outcome will ever be reported for it.
    CHECK_EQ(c.slots[0].node->calls_outstanding(), static_cast<size_t>(0));
    CHECK_EQ(c.slots[0].results.size(), static_cast<size_t>(0));
}

TEST(work, a_coordinator_can_hold_one_unit_per_peer_and_says_so) {
    // The pending table used to be four slots, which would have capped a coordinator at four busy
    // workers in a cell of twenty, for no reason anybody would have found in the code.
    CHECK(Node::max_calls_outstanding() >= kMaxUnicastPeers);

    WorkCell c;
    join(c, 4);
    for (size_t w = 1; w < 4; ++w) {
        CHECK(c.slots[0].node->request_call(c.id(w), kJob, nullptr, 0) != 0);
    }
    c.advance_ms(10);
    CHECK_EQ(c.slots[0].node->calls_outstanding(), static_cast<size_t>(3));
    for (size_t w = 1; w < 4; ++w) {
        CHECK_EQ(c.slots[w].accepted.size(), static_cast<size_t>(1));
    }
}

TEST(work, an_argument_list_too_big_for_a_frame_is_refused_before_it_is_sent) {
    WorkCell c;
    join(c, 2);

    std::vector<uint8_t> huge(kMaxCallArgsV1 + 1, 0xAA);
    CHECK_EQ(c.slots[0].node->request_call(c.id(1), kJob, huge.data(),
                                          static_cast<uint16_t>(huge.size())),
             static_cast<uint16_t>(0));
    CHECK_EQ(c.slots[0].node->calls_outstanding(), static_cast<size_t>(0));
    CHECK_EQ(c.slots[1].accepted.size(), static_cast<size_t>(0));

    // The largest list that does fit goes out whole.
    std::vector<uint8_t> big(kMaxCallArgsV1, 0x5A);
    CHECK(c.slots[0].node->request_call(c.id(1), kJob, big.data(),
                                        static_cast<uint16_t>(big.size())) != 0);
    c.advance_ms(10);
    CHECK_EQ(c.slots[1].accepted.size(), static_cast<size_t>(1));
    if (c.slots[1].accepted.empty()) {
        return;
    }
    CHECK_EQ(c.slots[1].accepted[0].args.size(), static_cast<size_t>(kMaxCallArgsV1));
}

TEST(work, a_unit_outlives_the_read_timeout_and_a_coordinator_ends_it_on_its_own_terms) {
    // Two facts that belong together. A work unit gets no deadline from the node: section 7.8's
    // premise is that a unit runs far longer than a round trip, so the two-second namespace request
    // timeout must not touch it. And a coordinator that does want to give up says so explicitly,
    // because it is the only party that knows how long its own work should take.
    WorkCell c;
    join(c, 2);

    const uint16_t msg_id = c.slots[0].node->request_call(c.id(1), kJob, nullptr, 0);
    CHECK(msg_id != 0);
    c.advance_ms(30000);  // fifteen times the read timeout

    CHECK_EQ(c.slots[0].node->calls_outstanding(), static_cast<size_t>(1));
    CHECK_EQ(c.slots[0].results.size(), static_cast<size_t>(0));
    CHECK_EQ(c.slots[0].node->ns_counters().read_timeouts, 0u);

    CHECK(c.slots[0].node->cancel_call(msg_id));
    CHECK_EQ(c.slots[0].results.size(), static_cast<size_t>(1));
    if (c.slots[0].results.empty()) {
        return;
    }
    CHECK(c.slots[0].results[0].outcome == Node::CallOutcome::Unavailable);
    CHECK_EQ(c.slots[0].results[0].msg_id, msg_id);
    CHECK_EQ(c.slots[0].node->calls_outstanding(), static_cast<size_t>(0));

    // Cancelling twice reports nothing the second time. Exactly once, in this direction too.
    CHECK(!c.slots[0].node->cancel_call(msg_id));
    CHECK_EQ(c.slots[0].results.size(), static_cast<size_t>(1));
}
