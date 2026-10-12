// M14: the cell's leases -- one resident hub, one deploy at a time (pot/lease.hpp).

#include <cstring>
#include <string>
#include <vector>

#include "pot/lease.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

LeaseRequest take(uint8_t kind, uint32_t token, const char* name, uint16_t ttl_s = 60) {
    LeaseRequest r;
    r.kind = kind;
    r.op = kLeaseTake;
    r.ttl_s = ttl_s;
    r.token = token;
    r.name_len = static_cast<uint8_t>(std::strlen(name));
    std::memcpy(r.name, name, r.name_len);
    return r;
}

LeaseRequest op(uint8_t kind, uint8_t o, uint32_t token) {
    LeaseRequest r;
    r.kind = kind;
    r.op = o;
    r.token = token;
    return r;
}

}  // namespace

TEST(lease, a_second_claimant_is_refused_by_the_first_ones_name_until_it_expires) {
    LeaseTable t;
    LeaseReply r = t.handle(take(kLeaseHub, 0x11, "hub on desk-pc"), 1000, 0x6300);
    CHECK(r.status == LeaseStatus::Ok);
    CHECK_EQ(r.arbiter, static_cast<uint16_t>(0x6300));
    CHECK_EQ(r.remaining_ms, 60000u);
    r = t.handle(take(kLeaseHub, 0x22, "hub on laptop"), 21000, 0x6300);
    CHECK(r.status == LeaseStatus::Held);
    CHECK(std::string(r.name) == "hub on desk-pc");
    CHECK_EQ(r.token, 0x11u);
    CHECK_EQ(r.remaining_ms, 40000u);
    // The other kind is separate: the hub's lease does not stop a deploy.
    CHECK(t.handle(take(kLeaseDeploy, 0x22, "apply on laptop"), 21000, 0x6300).status == LeaseStatus::Ok);
    // The holder renews; the other still cannot take it. Then the holder stops renewing, and it expires.
    CHECK(t.handle(take(kLeaseHub, 0x11, "hub on desk-pc"), 40000, 0x6300).status == LeaseStatus::Ok);
    CHECK(t.handle(take(kLeaseHub, 0x22, "hub on laptop"), 99000, 0x6300).status == LeaseStatus::Held);
    CHECK(std::string(t.holder(kLeaseHub, 99000)) == "hub on desk-pc");
    r = t.handle(take(kLeaseHub, 0x22, "hub on laptop"), 100001, 0x6300);
    CHECK(r.status == LeaseStatus::Ok);
    CHECK(std::string(r.name) == "hub on laptop");
}

TEST(lease, only_the_holder_releases_and_a_query_names_the_holder) {
    LeaseTable t;
    CHECK(t.handle(take(kLeaseDeploy, 7, "apply on desk-pc"), 0, 0x6300).status == LeaseStatus::Ok);
    LeaseReply r = t.handle(op(kLeaseDeploy, kLeaseRelease, 8), 10, 0x6300);
    CHECK(r.status == LeaseStatus::Held);
    r = t.handle(op(kLeaseDeploy, kLeaseQuery, 8), 10, 0x6300);
    CHECK(r.status == LeaseStatus::Ok);
    CHECK_EQ(r.token, 7u);
    CHECK(std::string(r.name) == "apply on desk-pc");
    CHECK(t.handle(op(kLeaseDeploy, kLeaseRelease, 7), 20, 0x6300).status == LeaseStatus::Ok);
    r = t.handle(op(kLeaseDeploy, kLeaseQuery, 8), 30, 0x6300);
    CHECK(r.status == LeaseStatus::Ok);
    CHECK_EQ(r.token, 0u);
    CHECK_EQ(static_cast<int>(r.name_len), 0);
    CHECK(std::string(t.holder(kLeaseDeploy, 30)).empty());
}

TEST(lease, a_request_out_of_range_is_malformed_and_changes_nothing) {
    LeaseTable t;
    CHECK(t.handle(take(3, 1, "x"), 0, 1).status == LeaseStatus::Malformed);           // no such kind
    CHECK(t.handle(take(kLeaseHub, 0, "x"), 0, 1).status == LeaseStatus::Malformed);   // token 0 is "free"
    CHECK(t.handle(take(kLeaseHub, 1, "x", 0), 0, 1).status == LeaseStatus::Malformed);
    CHECK(t.handle(take(kLeaseHub, 1, "x", kLeaseMaxTtlS + 1), 0, 1).status == LeaseStatus::Malformed);
    CHECK(t.handle(take(kLeaseHub, 1, ""), 0, 1).status == LeaseStatus::Malformed);
    CHECK(t.handle(take(kLeaseHub, 1, "a \"quote\""), 0, 1).status == LeaseStatus::Malformed);
    CHECK(t.handle(op(kLeaseHub, 9, 1), 0, 1).status == LeaseStatus::Malformed);
    CHECK_EQ(t.handle(op(kLeaseHub, kLeaseQuery, 1), 0, 1).token, 0u);
}

TEST(lease, the_clock_wrapping_does_not_end_a_lease_early) {
    LeaseTable t;
    const uint32_t near_wrap = 0xFFFFF000u;
    CHECK(t.handle(take(kLeaseHub, 5, "hub"), near_wrap, 1).status == LeaseStatus::Ok);
    LeaseReply r = t.handle(take(kLeaseHub, 6, "other"), near_wrap + 30000u, 1);  // past the wrap, 30 s in
    CHECK(r.status == LeaseStatus::Held);
    CHECK_EQ(r.remaining_ms, 30000u);
}

TEST(lease, request_and_reply_round_trip_and_a_short_one_is_refused) {
    LeaseRequest a = take(kLeaseDeploy, 0xA1B2C3D4u, "apply on desk-pc", 90);
    a.target = 0x7368;
    uint8_t wire[kLeaseRequestMaxLen];
    const size_t n = store_lease_request(a, wire, sizeof(wire));
    CHECK_EQ(n, static_cast<size_t>(11 + 16));
    LeaseRequest b;
    CHECK(load_lease_request(wire, n, b));
    CHECK_EQ(b.target, static_cast<uint16_t>(0x7368));
    CHECK_EQ(b.token, 0xA1B2C3D4u);
    CHECK_EQ(b.ttl_s, static_cast<uint16_t>(90));
    CHECK(std::string(b.name) == "apply on desk-pc");
    CHECK(!load_lease_request(wire, 10, b));
    // A name longer than the bytes that follow: loaded, and judged malformed.
    wire[10] = 30;
    CHECK(load_lease_request(wire, n, b));
    LeaseTable t;
    CHECK(t.handle(b, 0, 1).status == LeaseStatus::Malformed);

    LeaseReply r;
    r.status = LeaseStatus::Held;
    r.kind = kLeaseHub;
    r.arbiter = 0x6300;
    r.remaining_ms = 12345;
    r.token = 9;
    r.name_len = 3;
    std::memcpy(r.name, "hub", 4);
    uint8_t rw[kLeaseReplyMaxLen];
    const size_t rn = store_lease_reply(r, rw, sizeof(rw));
    CHECK_EQ(rn, static_cast<size_t>(16));
    LeaseReply s;
    CHECK(load_lease_reply(rw, rn, s));
    CHECK(s.status == LeaseStatus::Held);
    CHECK_EQ(s.arbiter, static_cast<uint16_t>(0x6300));
    CHECK_EQ(s.remaining_ms, 12345u);
    CHECK(std::string(s.name) == "hub");
    CHECK(!load_lease_reply(rw, rn - 1, s));
}

TEST(lease, the_arbiter_is_the_lowest_board_and_never_a_host) {
    const uint16_t alive[] = {0x8160, 0x00FE, 0x7368};
    CHECK_EQ(lease_arbiter(0x6300, alive, 3), static_cast<uint16_t>(0x6300));
    CHECK_EQ(lease_arbiter(0x8160, alive, 3), static_cast<uint16_t>(0x7368));
    const uint16_t none[] = {0x00FE};
    CHECK_EQ(lease_arbiter(0x00FE, none, 1), static_cast<uint16_t>(0xFFFF));  // a host alone has no arbiter
}

namespace {
std::vector<uint8_t> unhex(const char* hex) {
    std::vector<uint8_t> v;
    auto nib = [](char ch) { return ch <= '9' ? ch - '0' : ch - 'a' + 10; };
    for (size_t i = 0; hex[i] != 0 && hex[i + 1] != 0; i += 2) v.push_back(static_cast<uint8_t>(nib(hex[i]) * 16 + nib(hex[i + 1])));
    return v;
}
}  // namespace

TEST(lease, the_host_tools_bytes_are_read_here_and_ours_are_read_there) {
    // host/potluck/tests/test_lease.py's REQUEST_GOLDEN and REPLY_GOLDEN.
    const auto req = unhex("687301015a00d4c3b2a1106170706c79206f6e206465736b2d7063");
    LeaseRequest r;
    CHECK(load_lease_request(req.data(), req.size(), r));
    CHECK_EQ(r.target, static_cast<uint16_t>(0x7368));
    CHECK_EQ(static_cast<int>(r.kind), static_cast<int>(kLeaseDeploy));
    CHECK_EQ(static_cast<int>(r.op), static_cast<int>(kLeaseTake));
    CHECK_EQ(r.ttl_s, static_cast<uint16_t>(90));
    CHECK_EQ(r.token, 0xA1B2C3D4u);
    CHECK(std::string(r.name) == "apply on desk-pc");
    LeaseReply rep;
    rep.status = LeaseStatus::Held;
    rep.kind = kLeaseHub;
    rep.arbiter = 0x6300;
    rep.remaining_ms = 12345;
    rep.token = 9;
    rep.name_len = 3;
    std::memcpy(rep.name, "hub", 4);
    uint8_t w[kLeaseReplyMaxLen];
    const size_t n = store_lease_reply(rep, w, sizeof(w));
    CHECK(std::vector<uint8_t>(w, w + n) == unhex("01020063393000000900000003687562"));
}
