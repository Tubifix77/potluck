// The simulated cell: N real pot::Node objects over a modelled ESP-NOW channel.
//
// Pulled out of sim.cpp so more than one driver can use it. sim.cpp runs the membership and airtime
// scenarios; work.cpp runs section 7.8's coordinator and workers. Both drive the *same* pot::Node
// the firmware runs, which is the only reason either result means anything.
//
// The clock is virtual: a simulated hour costs a second or two, which is what makes it possible to
// ask "does a 20-node cell work" before owning 20 boards. See link_model.hpp for where every
// channel parameter comes from.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>
#include <queue>
#include <vector>

#include "pot/node.hpp"
#include "link_model.hpp"

namespace potsim {

using pot::kBroadcastMacAddr;
using pot::kEspNowV2LinkMtu;
using pot::kMacLen;
using pot::kMaxPeers;
using pot::BeaconMode;
using pot::Node;
using pot::NodeConfig;
using pot::NodeCounters;
using pot::NodeHal;
using pot::kHbMissLimitWireless;

// -------------------------------------------------------------------------------------------
// The simulated world
// -------------------------------------------------------------------------------------------

struct InFlight {
    uint64_t due_us;      // when it lands
    size_t to;            // receiving node index, or kAll for a broadcast fan-out already expanded
    size_t from;
    uint16_t len;
    int8_t rssi;
    uint8_t data[kEspNowV2LinkMtu];

    bool operator>(const InFlight& o) const { return due_us > o.due_us; }
};

struct TxCompletion {
    uint64_t due_us;
    size_t node;
    uint8_t mac[kMacLen];
    bool ok;

    bool operator>(const TxCompletion& o) const { return due_us > o.due_us; }
};

class Cell;

// Per-node glue between pot::Node and the simulated channel.
struct SimNode {
    Cell* cell = nullptr;
    size_t index = 0;
    uint8_t mac[kMacLen] = {};
    Node* node = nullptr;
    NodeHal hal{};
    uint32_t simulated_free_dram = 200 * 1024;
};

class Cell {
  public:
    Cell(size_t n, LinkPoint link, BeaconMode mode, uint64_t seed, uint32_t hb_period_ms,
         uint32_t probe_interval_ms)
        : link_(link), rng_(seed), mode_(mode) {
        nodes_.resize(n);
        for (size_t i = 0; i < n; ++i) {
            SimNode& s = nodes_[i];
            s.cell = this;
            s.index = i;
            // Locally-administered MACs, distinct and stable across runs.
            s.mac[0] = 0x02;
            s.mac[1] = 0x00;
            s.mac[2] = 0x00;
            s.mac[3] = 0x00;
            s.mac[4] = static_cast<uint8_t>((i >> 8) & 0xFF);
            s.mac[5] = static_cast<uint8_t>(i & 0xFF);

            NodeConfig cfg;
            cfg.node_id = static_cast<uint16_t>(0x1000 + i);
            cfg.boot_epoch = 1;
            std::memcpy(cfg.mac, s.mac, kMacLen);
            cfg.espnow_version = 2;
            cfg.hb_period_ms = hb_period_ms;
            cfg.hb_miss_limit = kHbMissLimitWireless;
            cfg.beacon_mode = mode;
            cfg.probe_interval_ms = probe_interval_ms;
            cfg.hello_interval_ms = 2000;

            s.hal.ctx = &s;
            s.hal.send = &Cell::hal_send;
            s.hal.add_peer = &Cell::hal_add_peer;
            s.hal.now_ms = &Cell::hal_now_ms;
            s.hal.now_us = &Cell::hal_now_us;
            s.hal.free_dram = &Cell::hal_free_dram;

            s.node = new Node(cfg, s.hal);
        }
    }

    ~Cell() {
        for (SimNode& s : nodes_) {
            delete s.node;
        }
    }

    Cell(const Cell&) = delete;
    Cell& operator=(const Cell&) = delete;

    // Start the virtual clock somewhere other than zero, so a run can straddle a counter wrap
    // without simulating the days before it. §1.2's environmental monitor "runs from flash
    // indefinitely", and uptime_ms / last_rx_ms are uint32 milliseconds — 49.7 days.
    void set_clock_us(uint64_t t) { now_us_ = t; window_start_us_ = t; }
    uint64_t now_us() const { return now_us_; }
    size_t size() const { return nodes_.size(); }
    SimNode& node(size_t i) { return nodes_[i]; }

    // --- statistics the simulation itself keeps ---
    uint64_t frames_sent = 0;
    uint64_t frames_delivered = 0;
    uint64_t frames_lost = 0;
    uint64_t frames_lost_to_collision = 0;
    double busy_us = 0.0;  // total channel airtime consumed, at the floor

    // --- adversarial controls -----------------------------------------------------------------
    // A bench cannot produce fifty reboots or a clean partition on demand, and those are exactly
    // where the subtle logic lives: boot-epoch handling, sequence-expectation resets, and the
    // difference between "revived" and "rebooted" that §7.7's fencing will later depend on.

    // Power-cycle a node: a fresh Node with the next boot epoch, and an empty peer table. Its peers
    // keep their view of it, which is the whole point — they must notice the epoch changed.
    void reboot(size_t i) {
        SimNode& s = nodes_[i];
        NodeConfig cfg = s.node->config();
        cfg.boot_epoch += 1;

        // Harvest the counters before the node is destroyed. Without this the scenario measures
        // itself into a lie: rebooting a node zeroes the very counters being summed at the end, so
        // a cell where every node reboots reports almost no observations no matter how correctly
        // the epoch change was detected. Found exactly that way — 24 reboots injected, 35 observed,
        // and the shortfall was the instrumentation rather than the code under test.
        const NodeCounters& c = s.node->counters();
        retired_.deaths += c.deaths_declared;
        retired_.revivals += c.revivals;
        retired_.reboots_seen += c.reboots_seen;

        delete s.node;
        s.node = new Node(cfg, s.hal);
        s.node->start();
        ++reboots_injected;
    }

    // Cell-wide totals that survive node restarts.
    struct Totals {
        uint32_t deaths = 0;
        uint32_t revivals = 0;
        uint32_t reboots_seen = 0;
    };
    Totals totals() const {
        Totals t = retired_;
        for (const SimNode& s : nodes_) {
            t.deaths += s.node->counters().deaths_declared;
            t.revivals += s.node->counters().revivals;
            t.reboots_seen += s.node->counters().reboots_seen;
        }
        return t;
    }

    // Put a node to sleep: radio off, so it neither transmits nor receives. §1.2's home has a
    // garden node and §7.8 says it "stays sleepy unless told otherwise".
    // §3's range cliff: between 56 m and 70 m the measured PDR "fluctuates between 100% and
    // zero" rather than degrading smoothly. Override the link's PDR while a run is in flight so
    // that behaviour can be reproduced -- this is the condition §13-M0's kill criterion is about.
    void set_pdr(double p) { link_.pdr = p; }
    double pdr() const { return link_.pdr; }

    void set_asleep(size_t i, bool v) { asleep_[i] = v; }
    bool asleep(size_t i) const { return asleep_[i]; }

    // Split the cell into two groups that cannot hear each other.
    void partition(size_t split_at) { partition_at_ = split_at; }
    void heal() { partition_at_ = 0; }

    uint32_t reboots_injected = 0;

    void run(uint64_t duration_us) {
        for (SimNode& s : nodes_) {
            s.node->start();
        }

        const uint64_t end = now_us_ + duration_us;
        // A 1 ms simulation step: fine enough that a 100 ms period is exact, coarse enough that an
        // hour of simulated time is a million steps rather than a billion.
        constexpr uint64_t kStepUs = 1000;

        while (now_us_ < end) {
            const uint64_t next = now_us_ + kStepUs;

            while (!rx_q_.empty() && rx_q_.top().due_us <= next) {
                InFlight f = rx_q_.top();
                rx_q_.pop();
                now_us_ = std::max(now_us_, f.due_us);
                nodes_[f.to].node->on_rx(nodes_[f.from].mac, f.data, f.len,
                                         static_cast<uint32_t>(now_us_), f.rssi);
            }
            while (!tx_q_.empty() && tx_q_.top().due_us <= next) {
                TxCompletion c = tx_q_.top();
                tx_q_.pop();
                now_us_ = std::max(now_us_, c.due_us);
                nodes_[c.node].node->on_tx_done(c.mac, c.ok, static_cast<uint32_t>(now_us_));
            }

            now_us_ = next;
            const uint32_t ms = static_cast<uint32_t>(now_us_ / 1000);
            for (SimNode& s : nodes_) {
                s.node->tick(ms);
            }

            if (on_step_ != nullptr) {
                on_step_(*this, step_ctx_);
            }
            if (on_second_ != nullptr && (now_us_ % 1000000ull) == 0) {
                on_second_(*this, static_cast<uint32_t>(now_us_ / 1000000ull));
            }
            if (stop_) {
                return;
            }
        }
    }

    void set_on_second(void (*fn)(Cell&, uint32_t)) { on_second_ = fn; }

    // A hook on every simulation step, with a context pointer. The per-second hook above exists to
    // perturb the cell; this one exists to drive something built *on* the cell -- section 7.8's
    // coordinator hands out work and collects results here, once per simulated millisecond.
    void set_on_step(void (*fn)(Cell&, void*), void* ctx) { on_step_ = fn; step_ctx_ = ctx; }

    // Stop the run early: the work scenario finishes when the last unit lands, not when the clock
    // runs out, and a run that kept going would report a throughput averaged over idle time.
    void stop() { stop_ = true; }

  private:
    // --- HAL callbacks -----------------------------------------------------------------------
    static uint32_t hal_now_ms(void* ctx) {
        return static_cast<uint32_t>(static_cast<SimNode*>(ctx)->cell->now_us_ / 1000);
    }
    static uint32_t hal_now_us(void* ctx) {
        return static_cast<uint32_t>(static_cast<SimNode*>(ctx)->cell->now_us_);
    }
    static uint32_t hal_free_dram(void* ctx) {
        return static_cast<SimNode*>(ctx)->simulated_free_dram;
    }
    static bool hal_add_peer(void* ctx, const uint8_t mac[kMacLen]) {
        (void)ctx;
        (void)mac;
        return true;  // no transport-level peer list in the model
    }

    static int32_t hal_send(void* ctx, const uint8_t mac[kMacLen], const uint8_t* data,
                            size_t len) {
        SimNode* s = static_cast<SimNode*>(ctx);
        return s->cell->transmit(s->index, mac, data, len);
    }

    // --- the channel -------------------------------------------------------------------------
    int32_t transmit(size_t from, const uint8_t mac[kMacLen], const uint8_t* data, size_t len) {
        if (asleep_[from]) {
            return 0;  // radio off: the transport accepts it, the air never sees it
        }
        const bool broadcast = std::memcmp(mac, kBroadcastMacAddr, kMacLen) == 0;
        const double air = airtime_floor_us(static_cast<uint32_t>(len));
        busy_us += air;
        ++frames_sent;

        // Channel occupancy. This is the whole point of the exercise: a shared medium can only
        // carry so much, and a policy that asks for more does not degrade gracefully — it collapses.
        // Frames offered while the channel is already saturated are dropped, which is a crude stand-
        // in for the collisions and retry exhaustion that would really happen.
        const double window_us = 1e6;
        if (now_us_ >= window_start_us_ + window_us) {
            window_start_us_ = now_us_;
            window_busy_us_ = 0.0;
        }
        window_busy_us_ += air;
        const bool saturated = window_busy_us_ > window_us;

        if (broadcast) {
            for (size_t to = 0; to < nodes_.size(); ++to) {
                if (to == from) {
                    continue;
                }
                deliver(from, to, data, len, /*unicast_retries=*/false, saturated);
            }
            // A broadcast has no MAC-layer ACK, so no completion is reported. The node already
            // knows this and does not wait for one.
            return 0;
        }

        size_t to = nodes_.size();
        for (size_t i = 0; i < nodes_.size(); ++i) {
            if (std::memcmp(nodes_[i].mac, mac, kMacLen) == 0) {
                to = i;
                break;
            }
        }
        if (to == nodes_.size()) {
            return -1;  // no such peer
        }

        const bool delivered = deliver(from, to, data, len, /*unicast_retries=*/true, saturated);

        // The send completion reports the MAC-layer ACK, which is what makes it a delivery signal.
        TxCompletion c{};
        c.due_us = now_us_ + static_cast<uint64_t>(kInitTxDelayUs);
        c.node = from;
        std::memcpy(c.mac, mac, kMacLen);
        c.ok = delivered;
        tx_q_.push(c);
        return 0;
    }

    bool deliver(size_t from, size_t to, const uint8_t* data, size_t len, bool unicast_retries,
                 bool saturated) {
        // A partition is not loss: the frame is simply never heard. Modelling it as loss would let
        // ESP-NOW's retries "recover" from it, which is not what a partition does.
        if (asleep_[to]) {
            ++frames_lost;
            return false;  // radio off
        }
        if (partition_at_ != 0 && ((from < partition_at_) != (to < partition_at_))) {
            ++frames_lost;
            return false;
        }
        if (saturated) {
            ++frames_lost;
            ++frames_lost_to_collision;
            return false;
        }

        // Per-attempt delivery, from §3's measured PDR at the configured distance. A unicast gets
        // ESP-NOW's retries; a broadcast does not, which is a real asymmetry and not a modelling
        // shortcut.
        bool ok = false;
        uint32_t attempts = 0;
        const uint32_t limit = unicast_retries ? kRetryLimit : 1;
        for (; attempts < limit; ++attempts) {
            if (rng_.uniform() <= link_.pdr) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            ++frames_lost;
            return false;
        }

        double delay = link_.mean_us + rng_.normal() * link_.sigma_us;
        delay += static_cast<double>(attempts) * kRetxDelayUs;
        // The measured maximum is a real observation; a Gaussian tail is not, so it is clamped
        // rather than allowed to invent delays §3 never saw.
        delay = std::min(std::max(delay, 500.0), link_.max_us + attempts * kRetxDelayUs);

        InFlight f{};
        f.due_us = now_us_ + static_cast<uint64_t>(delay);
        f.to = to;
        f.from = from;
        f.len = static_cast<uint16_t>(len);
        f.rssi = static_cast<int8_t>(-40 - static_cast<int>(link_.distance_m / 2));
        std::memcpy(f.data, data, len);
        rx_q_.push(f);
        ++frames_delivered;
        return true;
    }

    std::vector<SimNode> nodes_;
    LinkPoint link_;
    Rng rng_;
    BeaconMode mode_;
    uint64_t now_us_ = 0;
    Totals retired_{};  // counters harvested from nodes destroyed by reboot()
    bool asleep_[kMaxPeers] = {};
    size_t partition_at_ = 0;  // 0 = whole cell hears itself
    void (*on_second_)(Cell&, uint32_t) = nullptr;
    void (*on_step_)(Cell&, void*) = nullptr;
    void* step_ctx_ = nullptr;
    bool stop_ = false;
    uint64_t window_start_us_ = 0;
    double window_busy_us_ = 0.0;

    std::priority_queue<InFlight, std::vector<InFlight>, std::greater<InFlight>> rx_q_;
    std::priority_queue<TxCompletion, std::vector<TxCompletion>, std::greater<TxCompletion>> tx_q_;
};

}  // namespace potsim
