// M5's [MEASURE]: "Ed25519 vs P-256 decided on measured verify cost" (section 9.3, 13-M5).
//
// Built only with CONFIG_POT_CRYPTO_BENCH. Like the self-test, it runs first and then nothing else:
// no radio, no serial link, no tasks but its own, so the timings are the cryptography's and not the
// node's. It times every operation M5's trust model needs, on the firmware's real settings (CPU
// frequency and optimisation level are printed with the results), and checks each implementation
// against a known answer before timing it -- a fast wrong answer is not a result.
//
//   signatures    Ed25519 (Monocypher 4.0.3, RFC 8032)  vs  P-256 ECDSA (mbedTLS 4.1 PSA, hardware MPI)
//   key exchange  X25519 (Monocypher, and mbedTLS)      vs  P-256 ECDH (mbedTLS)
//   frame tag     HMAC-SHA256 over 64 bytes (mbedTLS, hardware SHA)
//
// Each operation runs in a fresh task with a 16 KB stack, so the stack figure is that operation's own
// high-water mark and not the worst of everything before it.

#include "crypto_bench.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "monocypher-ed25519.h"
#include "monocypher.h"
#include "psa/crypto.h"
#include "sdkconfig.h"

namespace {

constexpr int kRuns = 16;
constexpr uint32_t kStack = 16384;

int g_failures = 0;

void hex_in(const char* s, uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        unsigned v = 0;
        std::sscanf(s + 2 * i, "%2x", &v);
        out[i] = static_cast<uint8_t>(v);
    }
}

void check(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        std::printf("{\"t\":\"bench\",\"fail\":\"%s\"}\n", what);
    }
}

// ---- one operation, timed kRuns times inside its own task ----

struct Job {
    const char* op;
    const char* lib;
    void (*setup)();
    void (*run)();
    uint32_t us[kRuns];
    uint32_t stack_used;
    SemaphoreHandle_t done;
};

void job_task(void* arg) {
    Job* j = static_cast<Job*>(arg);
    if (j->setup) j->setup();
    for (int i = 0; i < kRuns; ++i) {
        const int64_t t0 = esp_timer_get_time();
        j->run();
        j->us[i] = static_cast<uint32_t>(esp_timer_get_time() - t0);
        vTaskDelay(1);  // outside the timed region: let core 1's idle task feed the task watchdog
    }
    j->stack_used = kStack - uxTaskGetStackHighWaterMark(nullptr);  // ESP-IDF counts stack in bytes
    xSemaphoreGive(j->done);
    vTaskDelete(nullptr);
}

void time_op(const char* op, const char* lib, void (*setup)(), void (*run)()) {
    static Job j;
    std::memset(&j, 0, sizeof(j));
    j.op = op;
    j.lib = lib;
    j.setup = setup;
    j.run = run;
    j.done = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(job_task, "bench", kStack, &j, 5, nullptr, 1);
    xSemaphoreTake(j.done, portMAX_DELAY);
    vSemaphoreDelete(j.done);
    uint32_t s[kRuns];
    std::memcpy(s, j.us, sizeof(s));
    std::sort(s, s + kRuns);
    std::printf("{\"t\":\"bench\",\"op\":\"%s\",\"lib\":\"%s\",\"n\":%d,\"min_us\":%u,\"med_us\":%u,"
                "\"max_us\":%u,\"stack_b\":%u}\n",
                op, lib, kRuns, static_cast<unsigned>(s[0]), static_cast<unsigned>(s[kRuns / 2]),
                static_cast<unsigned>(s[kRuns - 1]), static_cast<unsigned>(j.stack_used));
}

// ---- Ed25519, Monocypher ----

// RFC 8032 section 7.1, test 2 (one-byte message 0x72), the same vector host/potluck/tests uses.
const char* kRfcSeed = "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb";
const char* kRfcPub = "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c";
const char* kRfcSig = "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
                      "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00";

uint8_t g_msg[64];
uint8_t g_ed_sk[64], g_ed_pk[32], g_ed_sig[64];
volatile int g_sink;

void ed_kat() {
    uint8_t seed[32], pk_want[32], sig_want[64], sk[64], pk[32], sig[64];
    hex_in(kRfcSeed, seed, 32);
    hex_in(kRfcPub, pk_want, 32);
    hex_in(kRfcSig, sig_want, 64);
    const uint8_t m = 0x72;
    crypto_ed25519_key_pair(sk, pk, seed);  // wipes seed
    check(std::memcmp(pk, pk_want, 32) == 0, "ed25519 RFC 8032 public key");
    crypto_ed25519_sign(sig, sk, &m, 1);
    check(std::memcmp(sig, sig_want, 64) == 0, "ed25519 RFC 8032 signature");
    check(crypto_ed25519_check(sig_want, pk_want, &m, 1) == 0, "ed25519 RFC 8032 verify");
    sig_want[0] ^= 1;
    check(crypto_ed25519_check(sig_want, pk_want, &m, 1) != 0, "ed25519 rejects a flipped bit");
}

void ed_setup() {
    uint8_t seed[32];
    for (int i = 0; i < 32; ++i) seed[i] = static_cast<uint8_t>(i * 7 + 1);
    crypto_ed25519_key_pair(g_ed_sk, g_ed_pk, seed);
    crypto_ed25519_sign(g_ed_sig, g_ed_sk, g_msg, sizeof(g_msg));
}
void ed_sign() { crypto_ed25519_sign(g_ed_sig, g_ed_sk, g_msg, sizeof(g_msg)); }
void ed_verify() { g_sink = crypto_ed25519_check(g_ed_sig, g_ed_pk, g_msg, sizeof(g_msg)); }
void ed_keygen() {
    uint8_t seed[32] = {9};
    crypto_ed25519_key_pair(g_ed_sk, g_ed_pk, seed);
}

// ---- X25519, Monocypher ----

uint8_t g_x_sk[32], g_x_peer[32], g_x_out[32];
void mx_setup() {
    for (int i = 0; i < 32; ++i) g_x_sk[i] = static_cast<uint8_t>(0x40 + i);
    uint8_t other[32];
    for (int i = 0; i < 32; ++i) other[i] = static_cast<uint8_t>(0x90 - i);
    crypto_x25519_public_key(g_x_peer, other);
}
void mx_shared() { crypto_x25519(g_x_out, g_x_sk, g_x_peer); }

// ---- PSA: P-256 ECDSA, P-256 ECDH, X25519, HMAC-SHA256 ----

psa_key_id_t g_p256 = 0, g_p256_pub = 0, g_ecdh = 0, g_x_psa = 0, g_hmac = 0;
uint8_t g_hash[32], g_p_sig[64], g_p_pub[65], g_ecdh_peer[65], g_x_psa_peer[32], g_mac[32];
size_t g_p_sig_len = 0, g_p_pub_len = 0, g_ecdh_peer_len = 0;

psa_key_id_t gen(psa_key_type_t type, size_t bits, psa_key_usage_t usage, psa_algorithm_t alg) {
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, type);
    psa_set_key_bits(&a, bits);
    psa_set_key_usage_flags(&a, usage);
    psa_set_key_algorithm(&a, alg);
    psa_key_id_t id = 0;
    check(psa_generate_key(&a, &id) == PSA_SUCCESS, "psa_generate_key");
    return id;
}

constexpr psa_algorithm_t kEcdsa = PSA_ALG_ECDSA(PSA_ALG_SHA_256);

void p256_setup() {
    size_t n = 0;
    check(psa_hash_compute(PSA_ALG_SHA_256, g_msg, sizeof(g_msg), g_hash, sizeof(g_hash), &n) == PSA_SUCCESS,
          "sha256");
    if (g_p256 == 0) {
        g_p256 = gen(PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1), 256,
                     PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_VERIFY_HASH, kEcdsa);
        check(psa_export_public_key(g_p256, g_p_pub, sizeof(g_p_pub), &g_p_pub_len) == PSA_SUCCESS, "export");
        psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&a, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
        psa_set_key_bits(&a, 256);
        psa_set_key_usage_flags(&a, PSA_KEY_USAGE_VERIFY_HASH);
        psa_set_key_algorithm(&a, kEcdsa);
        check(psa_import_key(&a, g_p_pub, g_p_pub_len, &g_p256_pub) == PSA_SUCCESS, "import pub");
    }
    check(psa_sign_hash(g_p256, kEcdsa, g_hash, 32, g_p_sig, sizeof(g_p_sig), &g_p_sig_len) == PSA_SUCCESS,
          "p256 sign");
}
void p256_sign() {
    g_sink = psa_sign_hash(g_p256, kEcdsa, g_hash, 32, g_p_sig, sizeof(g_p_sig), &g_p_sig_len);
}
void p256_verify() { g_sink = psa_verify_hash(g_p256_pub, kEcdsa, g_hash, 32, g_p_sig, g_p_sig_len); }

void p256_kat() {
    // No published P-256 vector is wired in here; instead the property that matters: a signature
    // from this key verifies, and the same signature over a different hash does not.
    p256_setup();
    check(psa_verify_hash(g_p256_pub, kEcdsa, g_hash, 32, g_p_sig, g_p_sig_len) == PSA_SUCCESS,
          "p256 verify own signature");
    uint8_t other[32];
    std::memcpy(other, g_hash, 32);
    other[0] ^= 1;
    check(psa_verify_hash(g_p256_pub, kEcdsa, other, 32, g_p_sig, g_p_sig_len) != PSA_SUCCESS,
          "p256 rejects a different hash");
}

void ecdh_setup() {
    if (g_ecdh != 0) return;
    g_ecdh = gen(PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1), 256, PSA_KEY_USAGE_DERIVE, PSA_ALG_ECDH);
    psa_key_id_t other = gen(PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1), 256, PSA_KEY_USAGE_DERIVE,
                             PSA_ALG_ECDH);
    check(psa_export_public_key(other, g_ecdh_peer, sizeof(g_ecdh_peer), &g_ecdh_peer_len) == PSA_SUCCESS,
          "export ecdh peer");
    psa_destroy_key(other);
}
void ecdh_shared() {
    uint8_t out[32];
    size_t n = 0;
    g_sink = psa_raw_key_agreement(PSA_ALG_ECDH, g_ecdh, g_ecdh_peer, g_ecdh_peer_len, out, sizeof(out), &n);
}

void xpsa_setup() {
    if (g_x_psa != 0) return;
    g_x_psa = gen(PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY), 255, PSA_KEY_USAGE_DERIVE,
                  PSA_ALG_ECDH);
    // Same peer public key as the Monocypher run, so both libraries do the same work.
    mx_setup();
    std::memcpy(g_x_psa_peer, g_x_peer, 32);
}
void xpsa_shared() {
    uint8_t out[32];
    size_t n = 0;
    g_sink = psa_raw_key_agreement(PSA_ALG_ECDH, g_x_psa, g_x_psa_peer, 32, out, sizeof(out), &n);
}

void x25519_kat() {
    // Both libraries must agree: Monocypher's shared secret from (a, B) equals PSA's from the same a.
    uint8_t a[32], b[32], A[32], B[32], s1[32], s2[32];
    for (int i = 0; i < 32; ++i) {
        a[i] = static_cast<uint8_t>(3 * i + 5);
        b[i] = static_cast<uint8_t>(0xF0 - 2 * i);
    }
    crypto_x25519_public_key(A, a);
    crypto_x25519_public_key(B, b);
    crypto_x25519(s1, a, B);
    crypto_x25519(s2, b, A);
    check(std::memcmp(s1, s2, 32) == 0, "x25519 monocypher agrees with itself");
    psa_key_attributes_t at = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&at, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(&at, 255);
    psa_set_key_usage_flags(&at, PSA_KEY_USAGE_DERIVE);
    psa_set_key_algorithm(&at, PSA_ALG_ECDH);
    psa_key_id_t id = 0;
    check(psa_import_key(&at, a, 32, &id) == PSA_SUCCESS, "x25519 import");
    uint8_t s3[32];
    size_t n = 0;
    check(psa_raw_key_agreement(PSA_ALG_ECDH, id, B, 32, s3, sizeof(s3), &n) == PSA_SUCCESS && n == 32,
          "x25519 psa agreement");
    check(std::memcmp(s1, s3, 32) == 0, "x25519 psa agrees with monocypher");
    psa_destroy_key(id);
}

void hmac_setup() {
    if (g_hmac != 0) return;
    g_hmac = gen(PSA_KEY_TYPE_HMAC, 256, PSA_KEY_USAGE_SIGN_MESSAGE, PSA_ALG_HMAC(PSA_ALG_SHA_256));
}
void hmac_tag() {
    size_t n = 0;
    g_sink = psa_mac_compute(g_hmac, PSA_ALG_HMAC(PSA_ALG_SHA_256), g_msg, sizeof(g_msg), g_mac, sizeof(g_mac),
                             &n);
}

void kat_task(void* arg) {
    ed_kat();
    p256_kat();
    x25519_kat();
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(arg));
    vTaskDelete(nullptr);
}

}  // namespace

int pot_crypto_bench_run() {
    for (size_t i = 0; i < sizeof(g_msg); ++i) g_msg[i] = static_cast<uint8_t>(i);
    std::printf("{\"t\":\"bench_cfg\",\"cpu_mhz\":%d,\"opt\":\"%s\",\"runs\":%d}\n", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
#if CONFIG_COMPILER_OPTIMIZATION_PERF
                "perf",
#elif CONFIG_COMPILER_OPTIMIZATION_SIZE
                "size",
#else
                "other",
#endif
                kRuns);
    check(psa_crypto_init() == PSA_SUCCESS, "psa_crypto_init");

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(kat_task, "kat", kStack, done, 5, nullptr, 1);
    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
    std::printf("{\"t\":\"bench_kat\",\"failures\":%d}\n", g_failures);
    if (g_failures != 0) {
        return g_failures;  // timing a wrong implementation would be worse than no number
    }

    time_op("verify", "ed25519/monocypher", ed_setup, ed_verify);
    time_op("verify", "p256-ecdsa/mbedtls", p256_setup, p256_verify);
    time_op("sign", "ed25519/monocypher", ed_setup, ed_sign);
    time_op("sign", "p256-ecdsa/mbedtls", p256_setup, p256_sign);
    time_op("keygen", "ed25519/monocypher", nullptr, ed_keygen);
    time_op("shared_secret", "x25519/monocypher", mx_setup, mx_shared);
    time_op("shared_secret", "x25519/mbedtls", xpsa_setup, xpsa_shared);
    time_op("shared_secret", "p256-ecdh/mbedtls", ecdh_setup, ecdh_shared);
    time_op("tag_64B", "hmac-sha256/mbedtls", hmac_setup, hmac_tag);
    std::printf("{\"t\":\"bench_done\",\"failures\":%d}\n", g_failures);
    return g_failures;
}
