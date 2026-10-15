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
#include "pot/opcodes.hpp"
#include "pot/stats_json.hpp"
#include "pot/telemetry.hpp"
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

// M13: the presenting role's read load. Node 0 presents: it polls kMaxOutside (32) exposed resources, spread over
// its peers, one READ each every 2 s (16 a second) -- the package maximum, by polling since SUBSCRIBE is not
// built. Each owner publishes its resource every second. Compare the cell's airtime, deaths and PDR with
// --scenario none on the same seed.
void scenario_present(Cell& c, uint32_t sec) {
    constexpr uint32_t kExposed = 32;
    const size_t peers = c.size() - 1;
    if (peers == 0) return;
    auto hash_of = [](uint32_t k) { return 0x50000000u + k; };
    auto owner_of = [&](uint32_t k) { return static_cast<size_t>(1 + k % peers); };
    if (sec == 1) {
        for (uint32_t k = 0; k < kExposed; ++k) {
            NsDecl d;
            d.path_hash = hash_of(k);
            d.owner_node = c.node(owner_of(k)).node->config().node_id;
            d.type = ValueType::I32;
            d.staleness_bound_ms = 5000;
            for (size_t i = 0; i < c.size(); ++i) c.node(i).node->ns().declare(d);
        }
    }
    if (sec < 2) return;
    for (uint32_t k = 0; k < kExposed; ++k) {
        c.node(owner_of(k)).node->publish(hash_of(k), Value::of_i32(static_cast<int32_t>(sec)));
    }
    // Half of them each second: every resource once per 2 s.
    for (uint32_t k = sec % 2; k < kExposed; k += 2) {
        c.node(0).node->request_read(c.node(owner_of(k)).node->config().node_id, hash_of(k));
    }
}

// passive-sensor's D20 (its DECISIONS, 2026-10-15): the read load of presence and position in rooms. Rooms of five
// boards in a row (0-4, 5-9, ...). Every board publishes its evidence and its peer list (Bytes(8) each) and sixteen
// per-link fields every 2 s, all local; the first board of each room holds that room's fusion and reads one evidence
// value from each other board in the room every 2 s, and their peer lists once a minute. Per-link fields are read
// only while someone records, so not here. --scenario perlink is the design before D20 for comparison: one fusion
// reads two values per directed link of the whole cell every 2 s.
constexpr size_t kRoom = 5;
uint32_t d20_hash(size_t owner, uint32_t k) { return 0x60000000u + static_cast<uint32_t>(owner) * 64u + k; }
uint32_t g_d20_reads = 0;   // reads that went out (request_read refuses past kMaxPendingNs, 20, outstanding a node)
uint32_t g_d20_wanted = 0;  // reads the design asks for

void d20_declare(Cell& c, uint32_t per_owner) {
    for (size_t o = 0; o < c.size(); ++o) {
        for (uint32_t k = 0; k < per_owner; ++k) {
            NsDecl d;
            d.path_hash = d20_hash(o, k);
            d.owner_node = c.node(o).node->config().node_id;
            d.type = k < 2 ? ValueType::Bytes : ValueType::F32;
            d.staleness_bound_ms = 6000;
            for (size_t i = 0; i < c.size(); ++i) c.node(i).node->ns().declare(d);
        }
    }
}

void scenario_d20(Cell& c, uint32_t sec) {
    if (sec == 1) d20_declare(c, 2);  // evidence (0) and peers (1): what the radio carries
    if (sec < 2) return;
    const uint8_t pack[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    if (sec % 2 == 0) {
        for (size_t o = 0; o < c.size(); ++o) c.node(o).node->publish(d20_hash(o, 0), Value::of_bytes(pack, 8));
    }
    for (size_t holder = 0; holder < c.size(); holder += kRoom) {
        for (size_t o = holder + 1; o < holder + kRoom && o < c.size(); ++o) {
            const uint16_t id = c.node(o).node->config().node_id;
            if (sec % 2 == 0) {  // evidence every window
                ++g_d20_wanted;
                g_d20_reads += c.node(holder).node->request_read(id, d20_hash(o, 0)) != 0;
            }
            if (sec % 60 == 1) {  // peers once a minute, in the odd second between windows
                ++g_d20_wanted;
                g_d20_reads += c.node(holder).node->request_read(id, d20_hash(o, 1)) != 0;
            }
        }
    }
}

void scenario_perlink(Cell& c, uint32_t sec) {
    const uint32_t per_owner = static_cast<uint32_t>(2 * (c.size() - 1));  // mean and variance of each outgoing link
    if (sec == 1) d20_declare(c, per_owner);
    if (sec < 2 || sec % 2) return;
    for (size_t o = 0; o < c.size(); ++o) {
        for (uint32_t k = 0; k < per_owner; ++k) c.node(o).node->publish(d20_hash(o, k), Value::of_f32(1.0f));
    }
    g_d20_wanted += static_cast<uint32_t>((c.size() - 1) * per_owner);
}

// The fusion issues up to four reads every step (until request_read refuses one: all 20 slots taken), walking every
// link's two values in turn -- about the fastest it could go; whether that reaches the reads a window wants is the
// question.
void perlink_step(Cell& c, void*) {
    static size_t o = 1;
    static uint32_t k = 0;
    if (c.now_us() < 2000000) return;
    const uint32_t per_owner = static_cast<uint32_t>(2 * (c.size() - 1));
    for (int tries = 0; tries < 4; ++tries) {
        if (c.node(0).node->request_read(c.node(o).node->config().node_id, d20_hash(o, k)) == 0) return;
        ++g_d20_reads;
        if (++k == per_owner) { k = 0; o = o + 1 < c.size() ? o + 1 : 1; }
    }
}

// M15: the hub's telemetry pull. Node 0 is the board on the hub's cable: every period it asks each other board, one
// request at a time as the real proxy does, for its health, its links (paged, five peers a reply), its actors and its
// events. Each board answers with a reply of the size pot/telemetry.hpp gives for a cell this size -- the bytes on
// the air are what matters here, not their content. Compare airtime, deaths and RTT with --scenario none.
struct TelemetryDriver {
    uint32_t period_ms = 10000;
    uint8_t actors = 4;       // portable actors in the package
    size_t board = 1;         // whom we are asking
    size_t step = 0;          // which reply of that board's pull
    uint16_t awaiting = 0;
    uint64_t sent_us = 0;
    uint64_t cycle_start_us = 0;
    bool idle = true;
    uint32_t sent = 0, answered = 0, timeouts = 0, refused = 0, cycles = 0;
    uint64_t reply_bytes = 0;
    uint64_t cycle_us_sum = 0, cycle_us_max = 0;
};
TelemetryDriver g_tel;

size_t tel_link_pages(Cell& c) {
    const size_t peers = c.size() - 1;
    return (peers + kTelemetryLinksPerReply - 1) / kTelemetryLinksPerReply;
}

// Step i of a board's pull: 0 health, 1..pages links, then actors, then events.
TelemetryRequest tel_request(Cell& c, size_t board, size_t step) {
    TelemetryRequest r;
    r.target = c.node(board).node->config().node_id;
    const size_t pages = tel_link_pages(c);
    if (step == 0) {
        r.section = kTelemetryHealth;
    } else if (step <= pages) {
        r.section = kTelemetryLinks;
        r.start = static_cast<uint8_t>((step - 1) * kTelemetryLinksPerReply);
    } else if (step == pages + 1) {
        r.section = kTelemetryActors;
    } else {
        r.section = kTelemetryEvents;
    }
    return r;
}

size_t tel_server(void* ctx, uint16_t from, uint16_t msg_id, uint8_t op, const uint8_t* p, uint16_t len, uint8_t*,
                  size_t) {
    auto* sn = static_cast<SimNode*>(ctx);
    TelemetryRequest r;
    if (op != kOpTelemetry || !load_telemetry_request(p, len, r)) return 0;
    const size_t peers = sn->cell->size() - 1;
    size_t n = kTelemetryHeaderLen;
    if (r.section == kTelemetryHealth) {
        n += kTelemetryHealthLen;
    } else if (r.section == kTelemetryLinks) {
        const size_t left = peers > r.start ? peers - r.start : 0;
        n += 3 + (left < kTelemetryLinksPerReply ? left : kTelemetryLinksPerReply) * kTelemetryLinkLen;
    } else if (r.section == kTelemetryActors) {
        n += 3 + g_tel.actors * kTelemetryActorLen;
    } else {
        n += 5;  // a quiet cell: no new events
    }
    uint8_t buf[kTelemetryReplyMax] = {};
    sn->node->send_reply_raw(from, msg_id, buf, static_cast<uint16_t>(n));
    return 0;
}

void tel_result(void*, uint16_t, uint16_t msg_id, uint8_t op, bool timed_out, const uint8_t*, uint16_t len) {
    if (op != kOpTelemetry || msg_id != g_tel.awaiting) return;
    g_tel.awaiting = 0;
    if (timed_out) {
        ++g_tel.timeouts;
    } else {
        ++g_tel.answered;
        g_tel.reply_bytes += len;
    }
}

void tel_step(Cell& c, void*) {
    const uint64_t now = c.now_us();
    if (g_tel.awaiting != 0) {
        if (now - g_tel.sent_us < 500000) return;  // the hub's own timeout; the node's pending entry times out too
        g_tel.awaiting = 0;
        ++g_tel.timeouts;
    }
    if (g_tel.idle) {
        if (now < g_tel.cycle_start_us + static_cast<uint64_t>(g_tel.period_ms) * 1000) return;
        g_tel.idle = false;
        g_tel.cycle_start_us = now;
        g_tel.board = 1;
        g_tel.step = 0;
    }
    const size_t steps = tel_link_pages(c) + 3;
    if (g_tel.step >= steps) {
        g_tel.step = 0;
        ++g_tel.board;
    }
    if (g_tel.board >= c.size()) {
        g_tel.idle = true;
        ++g_tel.cycles;
        const uint64_t took = now - g_tel.cycle_start_us;
        g_tel.cycle_us_sum += took;
        if (took > g_tel.cycle_us_max) g_tel.cycle_us_max = took;
        return;
    }
    const TelemetryRequest r = tel_request(c, g_tel.board, g_tel.step++);
    uint8_t w[kTelemetryRequestLen];
    store_telemetry_request(r, w, sizeof(w));
    const uint16_t m = c.node(0).node->send_deploy(r.target, kOpTelemetry, w, sizeof(w));
    if (m == 0) {
        ++g_tel.refused;  // the board is not alive in node 0's view, or every pending slot is taken
        return;
    }
    g_tel.awaiting = m;
    g_tel.sent_us = now;
    ++g_tel.sent;
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
        "  --scenario S       none|reboot|partition|chaos|sleepy|cliff|present|telemetry|d20|perlink\n"
        "  --tel-ms N         telemetry: the hub's pull period per board, default 10000 (M15)\n"
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
        else if (a == "--tel-ms") g_tel.period_ms = static_cast<uint32_t>(std::stoul(next()));
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
    if (scenario == "present") {
        cell.set_on_second(&scenario_present);
    } else if (scenario == "d20") {
        cell.set_on_second(&scenario_d20);
        std::printf("d20: rooms of %zu, each room's fusion reads one evidence value per room-mate every 2 s, peers "
                    "once a minute\n", kRoom);
    } else if (scenario == "perlink") {
        cell.set_on_second(&scenario_perlink);
        cell.set_on_step(&perlink_step, nullptr);
        std::printf("perlink: one fusion reads mean and variance of every directed link every 2 s\n");
    } else if (scenario == "telemetry") {
        for (size_t i = 1; i < cell.size(); ++i) cell.node(i).node->set_deploy_server(&tel_server, &cell.node(i));
        cell.node(0).node->set_deploy_result(&tel_result, nullptr);
        g_tel.cycle_start_us = 3000000;  // the first pull 3 s in plus a period, once the cell has formed
        cell.set_on_step(&tel_step, nullptr);
        std::printf("telemetry: node 0 pulls every board every %u ms, %zu replies a board\n",
                    static_cast<unsigned>(g_tel.period_ms), tel_link_pages(cell) + 3);
    } else if (scenario == "reboot") {
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
    if (scenario == "telemetry") {
        std::printf("\ntelemetry: %u pulls, %u requests, %u answered, %u timed out, %u refused, %.1f KB of replies; "
                    "a pull of the whole cell took %.0f ms on average, %.0f ms at most\n",
                    static_cast<unsigned>(g_tel.cycles), static_cast<unsigned>(g_tel.sent),
                    static_cast<unsigned>(g_tel.answered), static_cast<unsigned>(g_tel.timeouts),
                    static_cast<unsigned>(g_tel.refused), g_tel.reply_bytes / 1024.0,
                    g_tel.cycles ? g_tel.cycle_us_sum / 1000.0 / g_tel.cycles : 0.0, g_tel.cycle_us_max / 1000.0);
    }
    if (scenario == "d20" || scenario == "perlink") {
        std::printf("\n%s: the design wants %.1f reads a second; %.1f went out (%u of %u)\n", scenario.c_str(),
                    g_d20_wanted / (minutes * 60.0), g_d20_reads / (minutes * 60.0),
                    static_cast<unsigned>(g_d20_reads), static_cast<unsigned>(g_d20_wanted));
    }
    if (scenario == "none" || scenario == "present" || scenario == "telemetry" || scenario == "d20" ||
        scenario == "perlink") {  // polling must not cost a death
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
