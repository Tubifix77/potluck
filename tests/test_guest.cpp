// M7: guest actors -- the blob, the author signature, image format version 2, the guest library and the
// actor itself, running the compiled example guests (guests/*.rs) and hand-assembled ones in the same
// sandbox the boards run. The guest task is this test: it calls GuestRuntime::service() between ticks.

#include <cmath>
#include <cstring>
#include <new>
#include <vector>

#include "guest_fixture.hpp"
#include "pot/deploy.hpp"
#include "pot/guest.hpp"
#include "pot/guest_examples.h"
#include "pot/node.hpp"
#include "pot/wasm_modules.h"
#include "test_harness.hpp"

using namespace pot;
using namespace guestfx;
namespace ge = pot::guest_examples;

namespace {

uint32_t g_t = 0;

const uint32_t kTemp = path_hash("potluck://lab/node-0100/hw/temp");
const uint32_t kOut = path_hash("potluck://lab/act/alarm/out");
const uint32_t kAlarm = path_hash("potluck://lab/act/alarm/alarm");
const uint32_t kCount = path_hash("potluck://lab/act/alarm/count");

Bytes overheat_blob(const Keys& k, uint32_t fuel = 100000) {
    return make_blob(k, ge::k_overheat_alarm, sizeof(ge::k_overheat_alarm), {kTemp},
                     {{kOut, ValueType::F32}, {kAlarm, ValueType::I32}, {kCount, ValueType::I32}}, fuel);
}

// A checkpoint store like the reconciler's, minus the replication and the once-a-second limit.
struct Store {
    uint8_t data[kMaxCheckpoint] = {};
    size_t len = 0;
    bool have = false;
    int saves = 0;
    CheckpointStore cs;
    Store() {
        cs.ctx = this;
        cs.save = [](void* c, const uint8_t* d, size_t n) {
            Store* s = static_cast<Store*>(c);
            std::memcpy(s->data, d, n);
            s->len = n;
            s->have = true;
            ++s->saves;
            return true;
        };
        cs.load = [](void* c, uint8_t* out, size_t cap, size_t* n, uint32_t* age) {
            Store* s = static_cast<Store*>(c);
            if (!s->have || cap < s->len) return false;
            std::memcpy(out, s->data, s->len);
            *n = s->len;
            *age = 0;
            return true;
        };
    }
};

// One node, one guest actor, the guest library loaded from a version-2 image.
struct Rig {
    Keys keys;
    NodeHal hal{};
    Node* node = nullptr;
    Bytes image;
    uint8_t pool[kMaxImageLen] = {};
    Store store;
    alignas(GuestActor) unsigned char mem[sizeof(GuestActor)];
    GuestActor* actor = nullptr;
    TickerConfig place{};

    explicit Rig(const Bytes& blob, bool enrolled = true) {
        guest_runtime().reset();
        NodeConfig cfg;
        cfg.node_id = 0x100;
        cfg.boot_epoch = 1;
        cfg.mac[0] = 0x02;
        hal.now_ms = [](void*) { return g_t; };
        hal.send = [](void*, const uint8_t*, const uint8_t*, size_t) -> int32_t { return 0; };
        node = new Node(cfg, hal);
        NsDecl t;
        t.path_hash = kTemp;
        t.owner_node = 0x100;
        t.type = ValueType::None;  // any type: the flaky guest reads it as an integer
        t.latency_class = kClassL3;
        CHECK(node->ns().declare(t) == NsError::Ok);
        image = make_image({guest_decl(kOut, 100, {0x100}, 0)}, {blob});
        DeployImage img{};
        const char* why = nullptr;
        CHECK(parse_image(image.data(), image.size(), img, &why));
        CHECK(guest_library().load(img, pool, sizeof(pool), &why));
        NsDecl outs[kMaxOutputs];
        const size_t n = guest_outputs(img.actors[0], 0, 0, outs, kMaxOutputs);
        for (size_t i = 0; i < n; ++i) {
            outs[i].owner_node = 0x100;  // as the reconciler's claim would make it
            CHECK(node->ns().declare(outs[i]) == NsError::Ok);
        }
        place.out_hash = kOut;
        place.period_ms = 100;
        guest_runtime().set_ca(enrolled ? keys.ca.pub : nullptr);
    }
    ~Rig() {
        stop();
        guest_runtime().service();  // unloads the instance
        delete node;
        guest_library().clear();
    }
    void start() {
        actor = new (mem) GuestActor(*node, place, 0, &store.cs);
        CHECK(actor->start(g_t));
    }
    void stop() {
        if (actor != nullptr) actor->~GuestActor();
        actor = nullptr;
    }
    void temp(float c) { node->publish(kTemp, Value::of_f32(c)); }
    void run_ms(uint32_t ms) {
        for (uint32_t k = 0; k < ms; ++k) {
            ++g_t;
            actor->tick(g_t);
            guest_runtime().service();
        }
    }
    Reading read(uint32_t h) {
        Reading r;
        node->read(h, r);
        return r;
    }
    int32_t i32(uint32_t h) {
        int32_t v = -999;
        Quality q;
        read(h).get_i32(v, q);
        return v;
    }
    float f32(uint32_t h) {
        float v = -999;
        Quality q;
        read(h).get_f32(v, q);
        return v;
    }
};

}  // namespace

// ---- the blob ---------------------------------------------------------------------------------------

TEST(guest, a_blob_parses_into_its_wiring_and_module) {
    Keys k;
    const Bytes b = overheat_blob(k, 5000);
    GuestSpec s;
    const char* why = nullptr;
    CHECK(parse_guest_blob(b.data(), b.size(), s, &why));
    CHECK_EQ(s.n_in, 1u);
    CHECK_EQ(s.n_out, 3u);
    CHECK_EQ(s.mem_pages, 1u);
    CHECK_EQ(s.fuel, 5000u);
    CHECK_EQ(s.in_hash[0], kTemp);
    CHECK_EQ(s.out_hash[2], kCount);
    CHECK(s.out_type[0] == ValueType::F32 && s.out_type[1] == ValueType::I32);
    CHECK_EQ(s.module_len, static_cast<uint32_t>(sizeof(ge::k_overheat_alarm)));
    CHECK(std::memcmp(s.module, ge::k_overheat_alarm, s.module_len) == 0);
    CHECK(std::memcmp(s.cert, k.cert.data(), kNodeCertLen) == 0);
}

TEST(guest, every_malformed_blob_is_refused_with_a_reason) {
    Keys k;
    const Bytes good = overheat_blob(k);
    GuestSpec s;
    auto refused = [&](Bytes b) {
        const char* why = nullptr;
        const bool ok = parse_guest_blob(b.data(), b.size(), s, &why);
        return !ok && why != nullptr;
    };
    CHECK(refused(Bytes(good.begin(), good.begin() + 11)));                // shorter than the fixed part
    CHECK(refused(Bytes(good.begin(), good.end() - static_cast<long>(sizeof(ge::k_overheat_alarm)))));  // no module
    Bytes b = good;
    b[0] ^= 1;
    CHECK(refused(b));  // magic
    b = good;
    b[4] = 2;
    CHECK(refused(b));  // version
    b = good;
    b[5] = static_cast<uint8_t>(kMaxGuestInputs + 1);
    CHECK(refused(b));
    b = good;
    b[6] = 0;
    CHECK(refused(b));  // no output
    b = good;
    b[7] = 2;
    CHECK(refused(b));  // 128 KiB of memory
    b = good;
    b[8] = 0x10, b[9] = 0x00, b[10] = 0x00, b[11] = 0x00;
    CHECK(refused(b));  // 16 units of fuel
    b = good;
    b[11] = 0xFF;
    CHECK(refused(b));  // billions
    b = good;
    b[12 + 4 + 4] = static_cast<uint8_t>(ValueType::Bool);
    CHECK(refused(b));  // an output that is neither i32 nor f32
    b = make_blob(k, ge::k_overheat_alarm, sizeof(ge::k_overheat_alarm), {kTemp},
                  {{kOut, ValueType::F32}, {kOut, ValueType::I32}});
    CHECK(refused(b));  // two outputs at one path
    b = make_blob(k, ge::k_overheat_alarm, sizeof(ge::k_overheat_alarm), {0}, {{kOut, ValueType::F32}});
    CHECK(refused(b));  // an input hash of 0
    b = good;
    b[b.size() - sizeof(ge::k_overheat_alarm)] = 'X';
    CHECK(refused(b));  // not a WebAssembly binary
}

// ---- the author ------------------------------------------------------------------------------------------

TEST(guest, the_authors_signature_verifies_and_names_the_author) {
    Keys k;
    uint16_t author = 0;
    const Bytes sig = author_sig(k.author, ge::k_endless_tick, sizeof(ge::k_endless_tick));
    CHECK(guest_check(k.ca.pub, k.cert.data(), sig.data(), ge::k_endless_tick, sizeof(ge::k_endless_tick), &author) ==
          CertError::Ok);
    CHECK_EQ(author, 7u);
}

TEST(guest, a_changed_module_another_ca_or_another_role_is_refused) {
    Keys k;
    Bytes mod(ge::k_endless_tick, ge::k_endless_tick + sizeof(ge::k_endless_tick));
    const Bytes sig = author_sig(k.author, mod.data(), mod.size());
    mod[mod.size() - 3] ^= 0x01;
    CHECK(guest_check(k.ca.pub, k.cert.data(), sig.data(), mod.data(), mod.size(), nullptr) == CertError::BadSignature);
    mod[mod.size() - 3] ^= 0x01;
    Key other_ca(200);
    const Bytes foreign = make_cert(other_ca, kCertRoleGuest, 7, k.author.pub);
    CHECK(guest_check(k.ca.pub, foreign.data(), sig.data(), mod.data(), mod.size(), nullptr) == CertError::OtherCa);
    // The CA's genuine certificate for the same key, as a deploy key: a deploy key is not a guest author.
    const Bytes deploy = make_cert(k.ca, kCertRoleDeploy, 0, k.author.pub);
    CHECK(guest_check(k.ca.pub, deploy.data(), sig.data(), mod.data(), mod.size(), nullptr) == CertError::Role);
    // Signed by someone else under the author's certificate.
    Key mallory(150);
    const Bytes forged = author_sig(mallory, mod.data(), mod.size());
    CHECK(guest_check(k.ca.pub, k.cert.data(), forged.data(), mod.data(), mod.size(), nullptr) == CertError::BadSignature);
}

TEST(guest, an_image_check_names_the_guest_whose_author_signature_fails) {
    Keys k;
    Bytes bad = overheat_blob(k);
    bad[bad.size() - 1] ^= 0x40;  // the module changed after the author signed it
    const Bytes img = make_image({guest_decl(kOut, 100, {0x100}, 0), guest_decl(kAlarm, 100, {0x100}, 1)},
                                 {overheat_blob(k), bad});
    DeployImage di{};
    const char* why = nullptr;
    CHECK(parse_image(img.data(), img.size(), di, &why));
    uint8_t which = 9;
    CHECK(guest_image_check(di, k.ca.pub, &which) == CertError::BadSignature);
    CHECK_EQ(which, 1u);
    Key other(200);
    CHECK(guest_image_check(di, other.pub, &which) == CertError::OtherCa);
    CHECK_EQ(which, 0u);
}

// ---- image format version 2 -----------------------------------------------------------------------------

TEST(guest, a_version_2_image_carries_its_guests) {
    Keys k;
    const Bytes blob = overheat_blob(k);
    const Bytes img = make_image({guest_decl(kOut, 100, {0x100, 0x101}, 0)}, {blob});
    DeployImage di{};
    const char* why = nullptr;
    CHECK(parse_image(img.data(), img.size(), di, &why));
    CHECK_EQ(di.actor_count, 1u);
    CHECK(di.actors[0].type == ActorType::Guest);
    CHECK_EQ(di.guest_count, 1u);
    CHECK_EQ(di.guests[0].len, static_cast<uint32_t>(blob.size()));
    CHECK(std::memcmp(di.guests[0].data, blob.data(), blob.size()) == 0);
}

TEST(guest, a_version_2_image_that_lies_about_its_guests_is_refused) {
    Keys k;
    const Bytes blob = overheat_blob(k);
    DeployImage di{};
    const char* why = nullptr;
    Bytes img = make_image({guest_decl(kOut, 100, {0x100}, 1)}, {blob});
    CHECK(!parse_image(img.data(), img.size(), di, &why));  // names guest 1 of 1
    img = make_image({guest_decl(kOut, 100, {0x100}, 0)}, {blob});
    img.push_back(0);
    CHECK(!parse_image(img.data(), img.size(), di, &why));  // a byte after the last guest
    img = make_image({guest_decl(kOut, 100, {0x100}, 0)}, {blob});
    img.pop_back();
    CHECK(!parse_image(img.data(), img.size(), di, &why));  // the guest is one byte short
    img = make_image({guest_decl(kOut, 100, {0x100}, 0)}, {blob, blob, blob, blob, blob});
    CHECK(!parse_image(img.data(), img.size(), di, &why));  // five guests
    // A guest that is not portable: the same declaration pinned to a node.
    Bytes pinned = guest_decl(kOut, 100, {0x100}, 0);
    pinned[0] = 0x00, pinned[1] = 0x01;
    img = make_image({pinned}, {blob});
    CHECK(!parse_image(img.data(), img.size(), di, &why));
    // body_len past the declarations would read the guest section as an actor.
    img = make_image({guest_decl(kOut, 100, {0x100}, 0)}, {blob});
    img[20] = static_cast<uint8_t>(img[20] + 1);
    CHECK(!parse_image(img.data(), img.size(), di, &why));
}

// ---- the library -----------------------------------------------------------------------------------------

TEST(guest, the_library_copies_the_guests_out_of_the_deploy_buffer) {
    Keys k;
    Bytes img = make_image({guest_decl(kOut, 100, {0x100}, 0)}, {overheat_blob(k)});
    DeployImage di{};
    const char* why = nullptr;
    CHECK(parse_image(img.data(), img.size(), di, &why));
    static uint8_t pool[kMaxImageLen];
    CHECK(guest_library().load(di, pool, sizeof(pool), &why));
    std::fill(img.begin(), img.end(), 0xEE);  // the receiver reuses the buffer
    const GuestSpec* s = guest_library().spec(0);
    CHECK(s != nullptr);
    CHECK_EQ(s->in_hash[0], kTemp);
    CHECK(std::memcmp(s->module, ge::k_overheat_alarm, sizeof(ge::k_overheat_alarm)) == 0);
    CHECK(guest_library().spec(1) == nullptr);
    guest_library().clear();
}

TEST(guest, the_library_wants_each_guest_run_by_exactly_one_actor_and_room_for_all) {
    Keys k;
    const Bytes blob = overheat_blob(k);
    static uint8_t pool[kMaxImageLen];
    DeployImage di{};
    const char* why = nullptr;
    Bytes img = make_image({guest_decl(kOut, 100, {0x100}, 0)}, {blob, blob});
    CHECK(parse_image(img.data(), img.size(), di, &why));
    CHECK(!guest_library().load(di, pool, sizeof(pool), &why));  // guest 1 runs nowhere
    CHECK_EQ(guest_library().count(), 0u);
    img = make_image({guest_decl(kOut, 100, {0x100}, 0), guest_decl(kAlarm, 100, {0x100}, 0)}, {blob});
    CHECK(parse_image(img.data(), img.size(), di, &why));
    CHECK(!guest_library().load(di, pool, sizeof(pool), &why));  // guest 0 twice
    img = make_image({guest_decl(kOut, 100, {0x100}, 0)}, {blob});
    CHECK(parse_image(img.data(), img.size(), di, &why));
    CHECK(!guest_library().load(di, pool, blob.size() - 1, &why));  // the pool is a byte short
    CHECK(guest_library().load(di, pool, blob.size(), &why));
    guest_library().clear();
}

// ---- the actor --------------------------------------------------------------------------------------------

TEST(guest, a_vendors_alarm_runs_and_publishes_what_it_computes) {
    Rig r(overheat_blob(Keys{}));
    r.temp(30.0f);
    r.start();
    r.run_ms(5);
    CHECK_EQ(r.actor->stats().ok, 1u);
    CHECK_EQ(r.actor->stats().fresh, 1u);
    CHECK(r.read(kOut).quality == Quality::Good);
    CHECK_EQ(r.f32(kOut), 30.0f);
    CHECK_EQ(r.i32(kAlarm), 0);
    CHECK_EQ(r.i32(kCount), 1);
    CHECK(r.actor->stats().fuel_last > 0u);
    // Hot: the smoothed value climbs past 38 and the alarm rises; cool: it falls below 37 and clears.
    r.temp(60.0f);
    r.run_ms(150);
    CHECK_EQ(r.i32(kAlarm), 0);  // one hot tick: 30 -> 36, not over 38 yet
    r.run_ms(1000);
    CHECK_EQ(r.i32(kAlarm), 1);
    CHECK(r.f32(kOut) > 38.0f);
    r.temp(37.5f);
    r.run_ms(3000);
    CHECK_EQ(r.i32(kAlarm), 1);  // between the thresholds: hysteresis holds it
    r.temp(20.0f);
    r.run_ms(3000);
    CHECK_EQ(r.i32(kAlarm), 0);
    CHECK_EQ(r.i32(kCount), static_cast<int32_t>(r.actor->stats().ok));
    CHECK_EQ(r.actor->stats().failed, 0u);
    CHECK_EQ(r.actor->stats().fresh, 1u);  // one instance for the whole activation
    CHECK(r.store.saves > 0);
}

TEST(guest, a_new_activation_starts_a_fresh_instance_from_the_checkpoint) {
    Rig r(overheat_blob(Keys{}));
    r.temp(45.0f);
    r.start();
    r.run_ms(1000);
    const int32_t count = r.i32(kCount);
    CHECK(count >= 9);
    CHECK_EQ(r.i32(kAlarm), 1);
    r.stop();  // the reconciler stops it here and starts it again (here, or on another node)
    r.run_ms(0);
    r.start();
    r.run_ms(5);
    CHECK_EQ(r.actor->stats().fresh, 1u);
    CHECK_EQ(r.actor->stats().logs, 1u);  // the guest logs 1 when it restores
    CHECK_EQ(r.actor->stats().log_last, 1);
    CHECK_EQ(r.i32(kCount), count + 1);  // continued, not restarted
    CHECK_EQ(r.i32(kAlarm), 1);          // and still alarmed, without warming up again
}

TEST(guest, an_input_without_a_value_is_seen_as_such) {
    Rig r(overheat_blob(Keys{}));  // the temperature is never published: NoData
    r.start();
    r.run_ms(250);
    CHECK_EQ(r.actor->stats().ok, 3u);
    CHECK(r.read(kOut).quality == Quality::NoData);  // nothing to smooth: it publishes no number
    CHECK_EQ(r.i32(kAlarm), 0);
    CHECK_EQ(r.i32(kCount), 3);
}

TEST(guest, a_tick_that_never_ends_runs_out_of_fuel_three_times_and_is_quarantined) {
    Keys k;
    Rig r(make_blob(k, ge::k_endless_tick, sizeof(ge::k_endless_tick), {}, {{kOut, ValueType::I32}}, 20000));
    r.node->ns().publish(kOut, Value::of_i32(5), g_t);  // an old value: a reader must not get it
    r.start();
    r.run_ms(5);
    CHECK_EQ(r.actor->strikes(), 1u);
    CHECK(r.actor->last_error() == pot_wasm_out_of_fuel);
    CHECK(r.read(kOut).quality == Quality::Faulty);
    CHECK(!r.read(kOut).usable());
    r.run_ms(200);
    CHECK(r.actor->quarantined());
    CHECK_EQ(r.actor->stats().failed, 3u);
    CHECK_EQ(r.actor->stats().fresh, 3u);  // each failure discarded its instance
    CHECK_EQ(r.actor->stats().fuel_last, 0u);
    r.run_ms(1000);
    CHECK_EQ(r.actor->stats().runs, 3u);  // and nothing ran since
    CHECK(!guest_runtime().loaded(0));
    CHECK(r.read(kOut).quality == Quality::Faulty);
}

TEST(guest, a_trapped_tick_publishes_nothing_and_a_good_one_clears_the_strikes) {
    Keys k;
    const Bytes mod = flaky_module();
    Rig r(make_blob(k, mod.data(), mod.size(), {kTemp}, {{kOut, ValueType::I32}}, 5000, 0));
    r.node->publish(kTemp, Value::of_i32(0));  // the input: 0 runs, anything else traps (kTemp holds any type here)
    const uint32_t t0 = g_t;
    r.start();
    r.run_ms(5);
    CHECK_EQ(r.i32(kOut), static_cast<int32_t>(t0 + 1));  // published `now` at its first tick
    r.node->publish(kTemp, Value::of_i32(1));
    r.run_ms(200);
    CHECK_EQ(r.actor->strikes(), 2u);
    CHECK(r.read(kOut).quality == Quality::Faulty);  // its `now` from the trapped ticks was never published
    r.node->publish(kTemp, Value::of_i32(0));
    r.run_ms(100);
    CHECK_EQ(r.actor->strikes(), 0u);
    CHECK(r.read(kOut).quality == Quality::Good);
    r.node->publish(kTemp, Value::of_i32(1));
    r.run_ms(300);
    CHECK(r.actor->quarantined());
    CHECK(std::strstr(r.actor->last_error(), "unreachable") != nullptr);
}

TEST(guest, a_module_whose_author_signature_fails_never_runs) {
    Keys k;
    Bytes b = overheat_blob(k);
    b[b.size() - 1] ^= 0x40;
    Rig r(b);
    r.start();
    r.run_ms(5);
    CHECK(r.actor->quarantined());  // at once: no retry can change a signature
    CHECK_EQ(r.actor->stats().failed, 1u);
    CHECK(std::strstr(r.actor->last_error(), "signature") != nullptr);
    CHECK_EQ(guest_runtime().verified(0), 2u);
    CHECK(!guest_runtime().loaded(0));
    CHECK(r.read(kAlarm).quality == Quality::Faulty);
}

TEST(guest, a_node_that_is_not_enrolled_runs_no_guest) {
    Rig r(overheat_blob(Keys{}), false);
    r.temp(30.0f);
    r.start();
    r.run_ms(5);
    CHECK(r.actor->quarantined());
    CHECK(std::strstr(r.actor->last_error(), "not enrolled") != nullptr);
    CHECK(!guest_runtime().loaded(0));
}

TEST(guest, a_module_asking_for_more_than_the_api_is_refused_at_load) {
    Keys k;
    {
        // An import from outside the guest API: env.pin_write, from M7's experiment.
        Rig r(make_blob(k, wasm_modules::k_pin, sizeof(wasm_modules::k_pin), {}, {{kOut, ValueType::I32}}));
        r.start();
        r.run_ms(5);
        CHECK(r.actor->quarantined());
        CHECK(std::strstr(r.actor->last_error(), "refused") != nullptr);
    }
    {
        // A name from the API with another type: refused when linked, never called with the wrong stack.
        const Bytes mod = liar_module();
        Rig r(make_blob(k, mod.data(), mod.size(), {kTemp}, {{kOut, ValueType::I32}}, 5000, 0));
        r.start();
        r.run_ms(5);
        CHECK(r.actor->quarantined());
        CHECK(r.actor->last_error() != nullptr);
    }
    {
        // More memory than the owner allowed it: the module wants one page, the blob allows none.
        Rig r(make_blob(k, ge::k_overheat_alarm, sizeof(ge::k_overheat_alarm), {kTemp},
                        {{kOut, ValueType::F32}, {kAlarm, ValueType::I32}, {kCount, ValueType::I32}}, 5000, 0));
        r.start();
        r.run_ms(5);
        CHECK(r.actor->quarantined());
        CHECK(std::strstr(r.actor->last_error(), "memory") != nullptr);
    }
}

TEST(guest, a_publish_to_an_output_the_owner_did_not_wire_traps) {
    // overheat_alarm publishes to outputs 0, 1 and 2; wired with one output, its first publish beyond it traps.
    Keys k;
    Rig r(make_blob(k, ge::k_overheat_alarm, sizeof(ge::k_overheat_alarm), {kTemp}, {{kOut, ValueType::F32}}));
    r.temp(30.0f);
    r.start();
    r.run_ms(300);
    CHECK(r.actor->quarantined());
    CHECK(std::strstr(r.actor->last_error(), "no such output") != nullptr);
    CHECK(r.read(kOut).quality == Quality::Faulty);  // its value from the same tick was discarded
}

TEST(guest, a_run_still_going_when_the_period_comes_round_is_skipped_not_queued) {
    Rig r(overheat_blob(Keys{}));
    r.temp(30.0f);
    r.start();
    // Tick without a guest task: nothing runs, and each later period finds the request still waiting.
    for (int i = 0; i < 350; ++i) r.actor->tick(++g_t);
    CHECK_EQ(r.actor->stats().overruns, 3u);
    guest_runtime().service();
    r.run_ms(2);
    CHECK_EQ(r.actor->stats().ok, 1u);
}

TEST(guest, the_outputs_are_read_only_l3_resources_with_the_staleness_of_five_periods) {
    Keys k;
    const Bytes img = make_image({guest_decl(kOut, 200, {0x100}, 0)}, {overheat_blob(k)});
    DeployImage di{};
    const char* why = nullptr;
    CHECK(parse_image(img.data(), img.size(), di, &why));
    static uint8_t pool[kMaxImageLen];
    CHECK(guest_library().load(di, pool, sizeof(pool), &why));
    NsDecl outs[kMaxOutputs];
    CHECK_EQ(guest_outputs(di.actors[0], 0, 0, outs, kMaxOutputs), 3u);
    for (size_t i = 0; i < 3; ++i) {
        CHECK(outs[i].access == Access::Read);
        CHECK_EQ(outs[i].latency_class, kClassL3);
        CHECK_EQ(outs[i].staleness_bound_ms, 1000u);
    }
    CHECK(outs[0].type == ValueType::F32 && outs[1].type == ValueType::I32);
    CHECK_EQ(guest_outputs(di.actors[0], 0, 0x101, outs, kMaxOutputs), 0u);  // no per-link outputs
    TickerConfig pc{};
    CHECK(kGuestKind.placement(di.actors[0], pc));
    CHECK_EQ(pc.period_ms, 200u);
    CHECK(kGuestKind.check(di.actors[0], ActorEnv{}, &why));
    guest_library().clear();
    CHECK(!kGuestKind.check(di.actors[0], ActorEnv{}, &why));  // the library does not hold it
}

// ---- the host tools' golden blob (host/potluck/tests/test_guest.py makes the same bytes) ------------------

namespace {
const char* kGoldenCa = "03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8";
const char* kGoldenBlob =
    "5047535401000101204e0000883bc3c202504e433101030700ed4242ea803bb16a174553b456dddfc6908ecab1c101fe"
    "6ab21e2baa0617795b7d43a63482993fd57c3846ebc2e3f677a9ef73f256d038602016954fc1aade1209bfa1909537cc"
    "e97c87cbc023d2ea71f42cdf0958c8a9085cd267e7c22907ae6313161c2895b205656568d1caa27e4d824c9b372e95d2"
    "578a33f051727a70f8fd2ae15c4b9621c0aeb5f6e9393cf48553e001e8123242b378fee912bad02e164fe85eee63a74a"
    "0f0061736d0100000001050160017f00030201000504010101010616037f014180c0000b7f004180c0000b7f004180c0"
    "000b072c04066d656d6f72790200047469636b00000a5f5f646174615f656e6403010b5f5f686561705f626173650302"
    "0a37013501017f23808080800041106b210103402001200041ed9c998e046c41b9e0006a36020c2001410c6a21002001"
    "28020c21000c000b0b";
Bytes unhex(const char* s) {
    Bytes b;
    auto v = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (; s[0] != '\0' && s[1] != '\0'; s += 2) b.push_back(static_cast<uint8_t>((v(s[0]) << 4) | v(s[1])));
    return b;
}
}  // namespace

TEST(guest, the_host_tools_golden_blob_parses_and_its_author_verifies_here) {
    const Bytes blob = unhex(kGoldenBlob);
    const Bytes ca = unhex(kGoldenCa);
    GuestSpec s;
    const char* why = nullptr;
    CHECK(parse_guest_blob(blob.data(), blob.size(), s, &why));
    CHECK_EQ(s.n_in, 0u);
    CHECK_EQ(s.n_out, 1u);
    CHECK_EQ(s.fuel, 20000u);
    CHECK_EQ(s.out_hash[0], kOut);
    CHECK(s.out_type[0] == ValueType::I32);
    CHECK_EQ(s.module_len, static_cast<uint32_t>(sizeof(ge::k_endless_tick)));
    CHECK(std::memcmp(s.module, ge::k_endless_tick, s.module_len) == 0);
    uint16_t author = 0;
    CHECK(guest_check(ca.data(), s.cert, s.sig, s.module, s.module_len, &author) == CertError::Ok);
    CHECK_EQ(author, 7u);
    // And the C++ fixture's keys are the Python tests' keys: the same seeds give the same certificate.
    Keys k;
    CHECK(std::memcmp(k.ca.pub, ca.data(), 32) == 0);
    CHECK(std::memcmp(k.cert.data(), s.cert, kNodeCertLen) == 0);
}
