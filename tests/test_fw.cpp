// M11: firmware over the cell, the portable half (pot/fw.hpp) and its signature (pot_trust). The golden
// trailer is built by host/potluck/tests/test_fw.py; if either side changes the format, one suite fails.

#include <cstring>
#include <vector>

#include "pot/fw.hpp"
#include "pot/trust.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

void hex(const char* s, uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        auto v = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        out[i] = static_cast<uint8_t>((v(s[2 * i]) << 4) | v(s[2 * i + 1]));
    }
}

const char* kCaPub = "03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8";
// test_deploy.py's deploy certificate, then the deploy key's signature over counter 7, length 1500.
const char* kGoldenFwTrailer =
    "504e433101020000ed4242ea803bb16a2543b92ff1095511476adc8369db6ddc933665a11978dda1404ee1066ca9559d"
    "b1d3a333735e10e016f05892d472bb8410ea752f25e182ce8d79a8654d385ac61e0f65630acebf8463daea22f39da958"
    "b7e4df70610768f218429d943da07d0d"
    "7f884e0776c11171cd0ae58cf0f13367c2bf2a4bfa871a13afdf231673e67b9a"
    "d73ae11a0bbbbc6bc414f89a7ea11e5e2e56395e52b4365144961bce97751a0a";
constexpr uint32_t kGoldenCounter = 7;
constexpr uint32_t kGoldenLen = 1500;

std::vector<uint8_t> golden_image() {
    std::vector<uint8_t> img(kGoldenLen);
    for (size_t i = 0; i < img.size(); ++i) img[i] = static_cast<uint8_t>(i * 7);
    return img;
}

struct MemSink : FwSink {
    uint32_t cap = 4096;
    std::vector<uint8_t> bytes;
    bool open = false, finished = false, activated = false, aborted = false;
    bool fail_finish = false;
    uint32_t capacity() const override { return cap; }
    bool begin(uint32_t) override {
        bytes.clear();
        open = true;
        finished = activated = aborted = false;
        return true;
    }
    bool write(const uint8_t* d, size_t n) override {
        bytes.insert(bytes.end(), d, d + n);
        return true;
    }
    bool finish() override { return finished = !fail_finish; }
    bool activate() override { return activated = true; }
    void abort() override {
        aborted = true;
        open = false;
    }
};

struct Verify {
    uint8_t ca[32];
    Verify() { hex(kCaPub, ca, 32); }
    static bool fn(void* ctx, uint32_t counter, uint32_t len, const uint8_t sha[64], const uint8_t* tr) {
        return fw_trailer_check(static_cast<Verify*>(ctx)->ca, counter, len, sha, tr, kFwTrailerLen) ==
               CertError::Ok;
    }
};

struct Rig {
    MemSink sink;
    Verify v;
    FwReceiver rx{sink, &Verify::fn, &v};
    uint8_t trailer[kFwTrailerLen];
    std::vector<uint8_t> img = golden_image();
    Rig() { hex(kGoldenFwTrailer, trailer, sizeof(trailer)); }
    FwStatus begin(uint32_t counter = kGoldenCounter) {
        return rx.begin(FwBegin{0x7368, static_cast<uint32_t>(img.size()), counter, trailer});
    }
    FwStatus send_from(size_t start, size_t step = 200) {
        for (size_t off = start; off < img.size(); off += step) {
            const size_t n = off + step <= img.size() ? step : img.size() - off;
            const FwStatus s = rx.chunk(
                FwChunk{0x7368, static_cast<uint32_t>(off), static_cast<uint16_t>(n), img.data() + off});
            if (s != FwStatus::Ok) return s;
        }
        return FwStatus::Ok;
    }
    FwStatus send_all() { return send_from(0); }
};

}  // namespace

TEST(fw, the_host_tools_signed_firmware_verifies_here) {
    Rig r;
    uint8_t sha[64];
    crypto_sha512(sha, r.img.data(), r.img.size());
    CHECK(fw_trailer_check(r.v.ca, kGoldenCounter, kGoldenLen, sha, r.trailer, kFwTrailerLen) == CertError::Ok);
    // The counter and the length are signed: a replay cannot claim another number.
    CHECK(fw_trailer_check(r.v.ca, kGoldenCounter + 1, kGoldenLen, sha, r.trailer, kFwTrailerLen) ==
          CertError::BadSignature);
    CHECK(fw_trailer_check(r.v.ca, kGoldenCounter, kGoldenLen - 1, sha, r.trailer, kFwTrailerLen) ==
          CertError::BadSignature);
    uint8_t other_ca[32] = {};
    CHECK(fw_trailer_check(other_ca, kGoldenCounter, kGoldenLen, sha, r.trailer, kFwTrailerLen) ==
          CertError::OtherCa);
    CHECK(fw_trailer_check(r.v.ca, kGoldenCounter, kGoldenLen, sha, r.trailer, kFwTrailerLen - 1) ==
          CertError::Length);
}

TEST(fw, a_package_signature_check_refuses_a_firmware_signature) {
    // Same key, same trailer layout, a different domain: neither can be passed off as the other.
    Rig r;
    CHECK(image_trailer_check(r.v.ca, r.img.data(), r.img.size(), r.trailer, kFwTrailerLen) ==
          CertError::BadSignature);
}

TEST(fw, a_signed_image_streams_in_and_is_activated) {
    Rig r;
    CHECK(r.begin() == FwStatus::Ok);
    CHECK(r.rx.in_progress());
    CHECK(r.send_all() == FwStatus::Ok);
    CHECK_EQ(r.rx.received(), kGoldenLen);
    CHECK(r.rx.commit() == FwStatus::Ok);
    CHECK(r.sink.finished);
    CHECK(r.sink.activated);
    CHECK(r.sink.bytes == r.img);
    CHECK(!r.rx.in_progress());
}

TEST(fw, a_flipped_byte_is_refused_and_nothing_activated) {
    Rig r;
    r.img[700] ^= 0x40;
    CHECK(r.begin() == FwStatus::Ok);
    CHECK(r.send_all() == FwStatus::Ok);
    CHECK(r.rx.commit() == FwStatus::BadSignature);
    CHECK(!r.sink.activated);
    CHECK(r.sink.aborted);
}

TEST(fw, a_counter_at_or_below_the_floor_is_a_downgrade) {
    Rig r;
    r.rx.set_floor(kGoldenCounter);
    CHECK(r.begin() == FwStatus::Downgrade);
    CHECK(!r.sink.open);
    r.rx.set_floor(kGoldenCounter - 1);
    CHECK(r.begin() == FwStatus::Ok);
}

TEST(fw, a_counter_the_signer_did_not_sign_fails_at_commit) {
    // BEGIN claims counter 9 to clear a floor of 8; the signature is over 7, so the commit refuses it.
    Rig r;
    r.rx.set_floor(8);
    CHECK(r.begin(9) == FwStatus::Ok);
    CHECK(r.send_all() == FwStatus::Ok);
    CHECK(r.rx.commit() == FwStatus::BadSignature);
    CHECK(!r.sink.activated);
}

TEST(fw, a_busy_node_and_an_oversized_image_are_refused) {
    Rig r;
    r.rx.set_busy(true);
    CHECK(r.begin() == FwStatus::Busy);
    r.rx.set_busy(false);
    r.sink.cap = kGoldenLen - 1;
    CHECK(r.begin() == FwStatus::TooLarge);
}

TEST(fw, chunks_arrive_in_order_and_a_retransmission_is_harmless) {
    Rig r;
    CHECK(r.rx.chunk(FwChunk{0x7368, 0, 10, r.img.data()}) == FwStatus::NotStarted);
    CHECK(r.begin() == FwStatus::Ok);
    CHECK(r.rx.chunk(FwChunk{0x7368, 200, 200, r.img.data() + 200}) == FwStatus::BadOffset);
    CHECK(r.rx.chunk(FwChunk{0x7368, 0, 200, r.img.data()}) == FwStatus::Ok);
    CHECK(r.rx.chunk(FwChunk{0x7368, 0, 200, r.img.data()}) == FwStatus::Ok);  // its REPLY was lost
    CHECK_EQ(r.rx.received(), 200u);
    CHECK(r.rx.commit() == FwStatus::Incomplete);
    CHECK(r.rx.in_progress());  // an early COMMIT does not throw the transfer away
    CHECK(r.send_from(200) == FwStatus::Ok);
    CHECK(r.rx.commit() == FwStatus::Ok);
    CHECK(r.sink.bytes == r.img);
}

TEST(fw, an_image_the_sink_rejects_is_not_activated) {
    Rig r;
    r.sink.fail_finish = true;
    CHECK(r.begin() == FwStatus::Ok);
    CHECK(r.send_all() == FwStatus::Ok);
    CHECK(r.rx.commit() == FwStatus::BadImage);
    CHECK(!r.sink.activated);
}

TEST(fw, a_new_begin_replaces_an_unfinished_transfer) {
    Rig r;
    CHECK(r.begin() == FwStatus::Ok);
    CHECK(r.rx.chunk(FwChunk{0x7368, 0, 200, r.img.data()}) == FwStatus::Ok);
    CHECK(r.begin() == FwStatus::Ok);
    CHECK_EQ(r.rx.received(), 0u);
    CHECK(r.send_all() == FwStatus::Ok);
    CHECK(r.rx.commit() == FwStatus::Ok);
    CHECK(r.sink.bytes == r.img);
}

TEST(fw, payloads_round_trip_in_the_host_tools_layout) {
    Rig r;
    uint8_t buf[kFwBeginLen];
    CHECK_EQ(store_fw_begin(FwBegin{0x7368, 1500, 7, r.trailer}, buf, sizeof(buf)), kFwBeginLen);
    FwBegin b{};
    CHECK(load_fw_begin(buf, kFwBeginLen, b));
    CHECK_EQ(b.target, static_cast<uint16_t>(0x7368));
    CHECK_EQ(b.image_len, 1500u);
    CHECK_EQ(b.counter, 7u);
    CHECK(std::memcmp(b.trailer, r.trailer, kFwTrailerLen) == 0);
    CHECK(!load_fw_begin(buf, kFwBeginLen - 1, b));

    uint8_t cb[kFwChunkHeaderLen + 3];
    const uint8_t d[3] = {1, 2, 3};
    CHECK_EQ(store_fw_chunk(FwChunk{0x8160, 400, 3, d}, cb, sizeof(cb)), sizeof(cb));
    const uint8_t want[] = {0x60, 0x81, 0x90, 0x01, 0, 0, 3, 0, 1, 2, 3};  // test_fw.py's layout
    CHECK(std::memcmp(cb, want, sizeof(want)) == 0);
    FwChunk c{};
    CHECK(load_fw_chunk(cb, sizeof(cb), c));
    CHECK(!load_fw_chunk(cb, sizeof(cb) - 1, c));

    FwReply rep{};
    rep.status = FwStatus::Downgrade;
    rep.node = 0x7368;
    rep.received = 10;
    rep.running = 5;
    rep.floor = 6;
    rep.state = FwState::OnTrial;
    std::strcpy(rep.version, "m11-test");
    uint8_t rb[kFwReplyLen];
    CHECK_EQ(store_fw_reply(rep, rb, sizeof(rb)), kFwReplyLen);
    FwReply back{};
    CHECK(load_fw_reply(rb, sizeof(rb), back));
    CHECK(back.status == FwStatus::Downgrade);
    CHECK_EQ(back.floor, 6u);
    CHECK(back.state == FwState::OnTrial);
    CHECK(std::strcmp(back.version, "m11-test") == 0);
}
