// A model of an ESP-NOW cell, parameterised by ARCHITECTURE.md §3's measured numbers.
//
// Every constant here is either cited in §3 / the zero-assumption ledger, or derived by stated
// arithmetic from something that is. Nothing is invented, and where a figure is a floor rather than
// a prediction it says so — a simulator whose parameters came from intuition would produce
// confident numbers about nothing, which is worse than having no simulator.
//
// What it models:
//   * per-frame delivery, from §3's measured PDR-versus-distance points
//   * per-frame delay, from §3's measured mean/σ/max at those distances
//   * the retry machinery, from §3's modelled init/retx/slot/limit figures
//   * channel occupancy, so a cell can be shown to saturate
//
// What it deliberately does not model: 802.11 backoff in detail, capture effect, interference from
// other networks, and the PHY preamble/ACK/IFS overhead. The first three are out of reach without
// measurements this project does not have; the last is omitted so that the airtime figure is a
// **hard floor** — real occupancy is higher, and a conclusion that holds at the floor holds.

#pragma once

#include <string>

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace potsim {

// -------------------------------------------------------------------------------------------
// §3's measured ESP-NOW behaviour. Source: Becker et al., "ESP-NOW Performance in Outdoor
// Environments", IEEE/IFIP WONS 2025, via ARCHITECTURE.md §3 and the ledger.
// -------------------------------------------------------------------------------------------

struct LinkPoint {
    const char* label;
    double distance_m;
    double pdr;         // 1.0 == no loss
    double mean_us;     // one-way delay
    double sigma_us;
    double max_us;
};

// The four measured points §3 records. A simulated link is configured by picking one, so the
// scenario is always traceable to a real measurement rather than to a made-up loss rate.
constexpr LinkPoint kBench{"bench", 1.0, 1.0, 2782.85, 108.72, 3200.0};
constexpr LinkPoint kClear54m{"54m_clear", 54.0, 1.0, 2782.85, 108.72, 3200.0};
constexpr LinkPoint kFringe52m{"52m_fringe", 52.0, 0.9985, 3461.65, 2079.06, 25628.0};
constexpr LinkPoint kCliff58m{"58m_cliff", 58.0, 0.832, 7851.15, 8033.23, 59192.0};

// §3's retransmission model: "initial tx delay 2800 µs, retransmission delay 3350 µs, slot 481 µs,
// retransmission limit 31; slotted p-persistent channel access".
constexpr uint32_t kInitTxDelayUs = 2800;
constexpr uint32_t kRetxDelayUs = 3350;
constexpr uint32_t kSlotUs = 481;
constexpr uint32_t kRetryLimit = 31;

// §3.1: "a single frame may legitimately remain in the protocol's retry machinery for
// 31 × 3350 µs ≈ 104 ms before it is abandoned."
constexpr uint32_t kMaxRetryWindowUs = kRetryLimit * kRetxDelayUs;
static_assert(kMaxRetryWindowUs > 100000 && kMaxRetryWindowUs < 110000,
              "§3.1's ~104 ms retry window");

// -------------------------------------------------------------------------------------------
// Airtime. The one number that decides whether a cell fits its channel.
// -------------------------------------------------------------------------------------------

// The ESP-NOW vendor-specific action frame, from the documented layout:
//   MAC header 24 + category 1 + organisation identifier 3 + random value 4 + FCS 4 = 36
//   vendor-specific element header: element id 1 + length 1 + OUI 3 + type 1 + reserved 1
//                                   + version 1 = 8
constexpr uint32_t kEspNowOverheadBytes = 44;

// "The default ESP-NOW bit rate is 1 Mbps."
constexpr uint32_t kBitRateBps = 1000000;

// Airtime **floor** for a Potluck frame of `payload_bytes` on the wire, in microseconds. Data bits only:
// no PHY preamble, no SIFS, no MAC ACK, no DIFS, no contention backoff. Real occupancy is
// materially higher — typically more than double for a unicast exchange — so a cell that exceeds
// 100% here is definitely oversubscribed, while one below it is merely not yet proven so.
constexpr double airtime_floor_us(uint32_t frame_bytes) {
    return static_cast<double>((kEspNowOverheadBytes + frame_bytes) * 8u) * 1e6 / kBitRateBps;
}

static_assert(airtime_floor_us(64) > 863.0 && airtime_floor_us(64) < 865.0,
              "a 64-byte pot frame is 108 B on air = 864 us at 1 Mbit/s");

// -------------------------------------------------------------------------------------------
// Deterministic RNG. xorshift64*, so a run is reproducible on any machine — a simulator that
// cannot be replayed exactly is a simulator whose failures cannot be investigated.
// -------------------------------------------------------------------------------------------

class Rng {
  public:
    explicit Rng(uint64_t seed) : s_(seed ? seed : 0x9E3779B97F4A7C15ull) {}

    uint64_t next() {
        s_ ^= s_ >> 12;
        s_ ^= s_ << 25;
        s_ ^= s_ >> 27;
        return s_ * 0x2545F4914F6CDD1Dull;
    }

    // Uniform in [0,1).
    double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }

    uint32_t below(uint32_t n) { return n ? static_cast<uint32_t>(next() % n) : 0; }

    // Normal via Box-Muller, cached. Used for the delay distribution, whose mean and σ are both
    // measured; the *shape* being Gaussian is an assumption, and a poor one in the tail — see
    // sample_delay_us(), which clamps to the measured maximum rather than letting a tail run away.
    double normal() {
        if (have_spare_) {
            have_spare_ = false;
            return spare_;
        }
        double u, v, s;
        do {
            u = uniform() * 2.0 - 1.0;
            v = uniform() * 2.0 - 1.0;
            s = u * u + v * v;
        } while (s >= 1.0 || s == 0.0);
        const double f = std::sqrt(-2.0 * std::log(s) / s);
        spare_ = v * f;
        have_spare_ = true;
        return u * f;
    }

  private:
    uint64_t s_;
    double spare_ = 0.0;
    bool have_spare_ = false;
};

// Look up a link point by the name the command line uses. Here rather than in a driver, because
// both drivers offer the same --link flag and two copies would drift.
inline const LinkPoint* link_by_name(const std::string& s) {
    if (s == "bench") return &kBench;
    if (s == "54m_clear") return &kClear54m;
    if (s == "52m_fringe") return &kFringe52m;
    if (s == "58m_cliff") return &kCliff58m;
    return nullptr;
}

}  // namespace potsim
