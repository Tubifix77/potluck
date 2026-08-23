// Potluck cell simulator — N real pot::Node objects over a modelled ESP-NOW channel.
//
//   pot_sim --nodes 7 --mode broadcast --minutes 60
//   pot_sim --nodes 20 --mode unicast --minutes 5 --link 58m_cliff
//   pot_sim --sweep                       # the airtime table, both modes, 2..20 nodes
//
// The nodes are the *same* pot::Node the firmware runs. Only the transport is simulated, so a
// policy bug found here is a policy bug in the firmware, and a fix verified here is verified in the
// thing that ships. See sim/link_model.hpp for where every channel parameter comes from.
//
// The clock is virtual: a simulated hour costs a second or two, which is what makes it possible to
// ask "does a 20-node cell work" before owning 20 boards.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <queue>
#include <string>
#include <vector>

#include "pot/node.hpp"
#include "pot/stats_json.hpp"
#include "cell.hpp"
#include "link_model.hpp"

using namespace pot;
using namespace potsim;

namespace {


// -------------------------------------------------------------------------------------------
// The airtime sweep — the answer to "does a cell of N fit its channel", with no simulation at all.
// Pure arithmetic from §3's frame structure and bit rate, so it is checkable by hand.
// -------------------------------------------------------------------------------------------

void print_sweep(uint32_t hb_period_ms, uint32_t probe_interval_ms) {
    const uint32_t hb_frame = kHeaderSize + sizeof(HeartbeatPayload);  // 16 + 48 = 64
    const double floor_us = airtime_floor_us(hb_frame);
    const double meas_us = static_cast<double>(kInitTxDelayUs);

    std::printf("\nESP-NOW cell channel load\n");
    std::printf("  frame: %u B pot + %u B ESP-NOW/802.11 overhead = %u B on air\n", hb_frame,
                kEspNowOverheadBytes, hb_frame + kEspNowOverheadBytes);
    std::printf("  heartbeat period %u ms; round-robin probe interval %u ms\n\n", hb_period_ms,
                probe_interval_ms);

    std::printf("  Bracketed by two figures, because the true cost per frame lies between them\n");
    std::printf("  and neither alone would be honest:\n");
    std::printf("    FLOOR  %6.0f us  data bits only at the documented %u bit/s default. Excludes\n",
                floor_us, static_cast<unsigned>(kBitRateBps));
    std::printf("                    PHY preamble, SIFS, MAC ACK, DIFS and backoff, so real cost\n");
    std::printf("                    is strictly higher. Anything over 100%% here is impossible.\n");
    std::printf("    MEAS   %6.0f us  §3's modelled initial-transmission delay (WONS 2025). Includes\n",
                meas_us);
    std::printf("                    channel access, so across contending nodes it double-counts\n");
    std::printf("                    somewhat -- an upper bound rather than an exact occupancy.\n\n");

    std::printf("  %5s | %-30s | %-30s\n", "nodes", "unicast full mesh (as shipped)",
                "broadcast beacon + rr probe");
    std::printf("  %5s | %9s %8s %8s | %9s %8s %8s\n", "", "frames/s", "floor", "meas",
                "frames/s", "floor", "meas");
    std::printf("  ------+--------------------------------+--------------------------------\n");

    auto verdict = [](double util) { return util > 1.0 ? "!!" : (util > 0.5 ? "~" : ""); };

    for (uint32_t n : {2u, 3u, 5u, 7u, 10u, 15u, 20u}) {
        // Unicast full mesh: every node probes every peer each period, each answered.
        const double uni_fps = 2.0 * n * (n - 1) * (1000.0 / hb_period_ms);
        // Broadcast beacon: one frame per node per period, plus a round-robin probe and its reply,
        // two frames per node per probe interval, independent of N.
        const double bc_fps = n * (1000.0 / hb_period_ms) + 2.0 * n * (1000.0 / probe_interval_ms);

        const double uf = uni_fps * floor_us / 1e6, um = uni_fps * meas_us / 1e6;
        const double bf = bc_fps * floor_us / 1e6, bm = bc_fps * meas_us / 1e6;

        std::printf("  %5u | %9.0f %6.0f%%%2s %6.0f%%%2s | %9.0f %6.0f%%%2s %6.0f%%%2s\n", n,
                    uni_fps, uf * 100.0, verdict(uf), um * 100.0, verdict(um),
                    bc_fps, bf * 100.0, verdict(bf), bm * 100.0, verdict(bm));
    }

    std::printf("\n  !! over 100%%   ~ over 50%%\n");
    std::printf("\n  §4 says a v1 wireless cluster is \"one radio cell of ~20 nodes\"; §8.2 mandates\n");
    std::printf("  a %u ms heartbeat. Under unicast full mesh those cannot both hold: N=20 is\n",
                hb_period_ms);
    std::printf("  impossible on either bound, and N=7 -- the fleet actually on order -- is\n");
    std::printf("  impossible on the measured bound and marginal on the floor.\n");
    std::printf("  Broadcast beacon plus round-robin probe stays inside both bounds to N=20.\n");
}

// -------------------------------------------------------------------------------------------
// Adversarial scenarios: per-second hooks that perturb the cell while it runs.
//
// These are the cases a bench will not reproduce on demand, and they are where the subtle logic
// lives — boot-epoch change detection, the sequence-expectation reset, and telling "revived" from
// "rebooted", which §7.7's fencing will later depend on.
// -------------------------------------------------------------------------------------------

// A node power-cycles every 5 s, round robin. Get the epoch handling wrong and its peers charge the
// returning node with tens of thousands of phantom losses, quietly ruining the PDR figure.
void scenario_reboot(Cell& c, uint32_t sec) {
    if (sec > 0 && sec % 5 == 0) {
        c.reboot((sec / 5) % c.size());
    }
}

// Split the cell for 3 s, heal for 7 s, repeat. Each half should declare the other dead (§8.2) and
// then revive it — *not* report it as rebooted, because nothing rebooted.
void scenario_partition(Cell& c, uint32_t sec) {
    const uint32_t phase = sec % 10;
    if (phase == 0) {
        c.partition(c.size() / 2);
    } else if (phase == 3) {
        c.heal();
    }
}

// Both at once: a node reboots while the cell is partitioned, so its peers must tell a reboot from
// a heal with both happening together.
void scenario_chaos(Cell& c, uint32_t sec) {
    scenario_partition(c, sec);
    if (sec > 0 && sec % 4 == 0) {
        c.reboot((sec / 4) % c.size());
    }
}

// The last node duty-cycles: awake 2 s, asleep 8 s. That is a *conservative* sleepy node -- a
// real solar garden node wakes for seconds every few minutes. §8.2 declares a peer dead after
// 600 ms of silence and has no notion of a peer that is intentionally asleep.
// The §3 range cliff, which is what §13-M0's kill criterion is written about: PDR does not
// decay, it oscillates between 1.0 and 0.0 in seconds. Four seconds usable, four seconds dead.
void scenario_cliff(Cell& c, uint32_t sec) {
    c.set_pdr((sec % 8) < 4 ? 1.0 : 0.0);
}

void scenario_sleepy(Cell& c, uint32_t sec) {
    c.set_asleep(c.size() - 1, (sec % 10) >= 2);
}

void usage() {
    std::printf(
        "pot_sim - run N real pot::Node objects over a modelled ESP-NOW cell\n"
        "\n"
        "  --nodes N          number of nodes (default 7)\n"
        "  --mode M           broadcast | unicast   (default broadcast)\n"
        "  --minutes M        simulated minutes (default 5)\n"
        "  --link L           bench | 54m_clear | 52m_fringe | 58m_cliff (default bench)\n"
        "  --hb-ms N          heartbeat period, default 100 (section 8.2)\n"
        "  --probe-ms N       round-robin probe interval, default 1000\n"
        "  --seed N           RNG seed, default 1\n"
        "  --scenario S       none|reboot|partition|chaos|sleepy|cliff (default none)\n"
        "  --start-days D     start the virtual clock D days in, to straddle a counter wrap\n"
        "  --json             emit one link/node record per node, in the firmware's format\n"
        "  --sweep            print the airtime table and exit\n"
        "\n"
        "Every channel parameter comes from ARCHITECTURE.md section 3; see sim/link_model.hpp.\n");
}

}  // namespace

int main(int argc, char** argv) {
    size_t nodes = 7;
    BeaconMode mode = BeaconMode::BroadcastBeacon;
    double minutes = 5.0;
    const LinkPoint* link = &kBench;
    uint32_t hb_ms = kHbPeriodWirelessMs;
    uint32_t probe_ms = 1000;
    uint64_t seed = 1;
    bool sweep = false;
    bool json = false;
    std::string scenario = "none";
    double start_days = 0.0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--nodes") nodes = static_cast<size_t>(std::stoul(next()));
        else if (a == "--mode") {
            const std::string m = next();
            mode = (m == "unicast") ? BeaconMode::UnicastFullMesh : BeaconMode::BroadcastBeacon;
        } else if (a == "--minutes") minutes = std::stod(next());
        else if (a == "--link") {
            const std::string l = next();
            const LinkPoint* p = link_by_name(l);
            if (p == nullptr) { std::printf("unknown link '%s'\n", l.c_str()); return 2; }
            link = p;
        }
        else if (a == "--hb-ms") hb_ms = static_cast<uint32_t>(std::stoul(next()));
        else if (a == "--probe-ms") probe_ms = static_cast<uint32_t>(std::stoul(next()));
        else if (a == "--seed") seed = std::stoull(next());
        else if (a == "--sweep") sweep = true;
        else if (a == "--json") json = true;
        else if (a == "--scenario") scenario = next();
        else if (a == "--start-days") start_days = std::stod(next());
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { std::printf("unknown option '%s'\n\n", a.c_str()); usage(); return 2; }
    }

    if (sweep) {
        print_sweep(hb_ms, probe_ms);
        return 0;
    }

    if (nodes < 2 || nodes > kMaxPeers) {
        std::printf("--nodes must be between 2 and %zu (section 3's ESP-NOW peer ceiling)\n",
                    kMaxPeers);
        return 2;
    }

    std::printf("pot_sim: %zu nodes, %s, link %s (PDR %.4f, mean %.0f us), "
                "hb %u ms, probe %u ms, %.1f simulated minutes, seed %llu\n",
                nodes, beacon_mode_str(mode), link->label, link->pdr, link->mean_us, hb_ms,
                probe_ms, minutes, static_cast<unsigned long long>(seed));

    Cell cell(nodes, *link, mode, seed, hb_ms, probe_ms);
    if (start_days > 0.0) {
        cell.set_clock_us(static_cast<uint64_t>(start_days * 86400.0 * 1e6));
        std::printf("clock starts at %.4f days (uint32 ms wraps at 49.7103)\n", start_days);
    }
    if (scenario == "reboot") {
        cell.set_on_second(&scenario_reboot);
    } else if (scenario == "partition") {
        cell.set_on_second(&scenario_partition);
    } else if (scenario == "cliff") {
        cell.set_on_second(&scenario_cliff);
    } else if (scenario == "sleepy") {
        cell.set_on_second(&scenario_sleepy);
    } else if (scenario == "chaos") {
        cell.set_on_second(&scenario_chaos);
    } else if (scenario != "none") {
        std::printf("unknown scenario '%s'\n", scenario.c_str());
        return 2;
    }
    if (scenario != "none") {
        std::printf("scenario: %s\n", scenario.c_str());
    }
    cell.run(static_cast<uint64_t>(minutes * 60.0 * 1e6));

    const double elapsed_s = minutes * 60.0;
    const double util = cell.busy_us / (elapsed_s * 1e6);

    std::printf("\nchannel\n");
    std::printf("  frames offered      %llu  (%.0f/s)\n",
                static_cast<unsigned long long>(cell.frames_sent), static_cast<double>(cell.frames_sent) / elapsed_s);
    std::printf("  delivered           %llu\n",
                static_cast<unsigned long long>(cell.frames_delivered));
    std::printf("  lost                %llu  (of which %llu to saturation)\n",
                static_cast<unsigned long long>(cell.frames_lost),
                static_cast<unsigned long long>(cell.frames_lost_to_collision));
    std::printf("  airtime (floor)     %.1f%%  %s\n", util * 100.0,
                util > 1.0 ? "<-- OVERSUBSCRIBED" : "");

    std::printf("\nper node\n");
    std::printf("  %-8s %6s %7s %7s %8s %8s %9s %9s\n", "node", "peers", "beacons", "probes",
                "replies", "rtt n", "rtt min", "rtt max");
    for (size_t i = 0; i < cell.size(); ++i) {
        Node& n = *cell.node(i).node;
        const Node::TxTally& t = n.tx_tally();
        uint32_t rtt_n = 0, rtt_min = 0xFFFFFFFFu, rtt_max = 0;
        for (size_t j = 0; j < kMaxPeers; ++j) {
            const PeerLink& p = n.peers().slot(j);
            if (p.state == PeerState::Free) continue;
            rtt_n += p.rtt_samples;
            if (p.rtt_samples) {
                rtt_min = std::min(rtt_min, p.rtt_min_us);
                rtt_max = std::max(rtt_max, p.rtt_max_us);
            }
        }
        std::printf("  0x%04x   %6zu %7u %7u %8u %8u %9s %9s\n", n.config().node_id,
                    n.peers().count_in_state(PeerState::Alive), t.beacons, t.probes, t.replies,
                    rtt_n,
                    rtt_n ? std::to_string(rtt_min).c_str() : "-",
                    rtt_n ? std::to_string(rtt_max).c_str() : "-");
    }

    // Deaths are the headline correctness signal: on a healthy cell there should be none, and any
    // at all mean the heartbeat did not fit the channel.
    const Cell::Totals tot = cell.totals();
    const uint32_t deaths = tot.deaths;
    const uint32_t revivals = tot.revivals;
    // Reboot detection is the point of the reboot scenario: a peer that power-cycles must be seen
    // as rebooted, not merely revived, or its stale sequence expectations survive and every
    // subsequent frame is counted as a loss.
    const uint32_t reboots_seen = tot.reboots_seen;
    if (cell.reboots_injected > 0) {
        std::printf("\nreboots: %u injected, %u observed by peers%s\n",
                    static_cast<unsigned>(cell.reboots_injected),
                    static_cast<unsigned>(reboots_seen),
                    reboots_seen == 0 ? "  <-- peers did not notice the epoch change" : "");
    }

    // What "good" means depends on the scenario. With no perturbation, any death at all means the
    // heartbeat did not fit the channel. Under partition or chaos, deaths are the *expected*
    // response to a genuinely unreachable peer, and the thing to check instead is that the cell
    // converged: nobody left dead once the link came back.
    size_t still_dead = 0;
    for (size_t i = 0; i < cell.size(); ++i) {
        still_dead += cell.node(i).node->peers().count_in_state(PeerState::Dead);
    }
    std::printf("\nmembership: %u death declarations, %u revivals across the cell\n", deaths,
                revivals);
    if (scenario == "none") {
        std::printf("  %s\n", deaths ? "<-- unperturbed cell lost peers; the heartbeat did not hold"
                                     : "no deaths on an unperturbed cell, as required");
    } else {
        std::printf("  deaths are expected under '%s'; what matters is convergence\n",
                    scenario.c_str());
    }
    std::printf("  peers still marked dead at the end: %zu%s\n", still_dead,
                still_dead ? "   <-- DID NOT CONVERGE" : "   (converged)");

    if (json) {
        std::printf("\n");
        char buf[kJsonLineMax];
        for (size_t i = 0; i < cell.size(); ++i) {
            Node& n = *cell.node(i).node;
            for (size_t j = 0; j < kMaxPeers; ++j) {
                const PeerLink& p = n.peers().slot(j);
                if (p.state == PeerState::Free) continue;
                LinkRecord lr{};
                lr.uptime_ms = static_cast<uint32_t>(cell.now_us() / 1000);
                lr.node_id = n.config().node_id;
                lr.peer_node_id = p.node_id;
                lr.peer = &p;
                lr.histogram = &n.peers().histogram(j);
                lr.last_rssi = p.last_rssi;
                if (write_link_json(buf, sizeof(buf), lr) > 0) std::fputs(buf, stdout);
            }
            NodeRecord nr{};
            nr.uptime_ms = static_cast<uint32_t>(cell.now_us() / 1000);
            nr.node_id = n.config().node_id;
            nr.boot_epoch = n.config().boot_epoch;
            nr.counters = &n.counters();
            nr.free_dram_now = cell.node(i).simulated_free_dram;
            nr.peers_alive = static_cast<uint32_t>(n.peers().count_in_state(PeerState::Alive));
            nr.peers_dead = static_cast<uint32_t>(n.peers().count_in_state(PeerState::Dead));
            if (write_node_json(buf, sizeof(buf), nr) > 0) std::fputs(buf, stdout);
        }
    }

    return 0;
}
