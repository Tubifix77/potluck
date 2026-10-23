// M19: stub cells on the PC -- simulated boards that join the real cell's tree through the cable.
//
// The bench has three boards and the owner buys no more (ARCHITECTURE section 13, M19's accept): the three are one
// real base cell, and this process hosts the rest of the tree -- sibling cells with their own foremen, and a level
// above -- as real pot::Node objects with the real reconciler and foreman actor, the same sources as the boards. The
// stubs in the upper cell's radio space are on the cable: the cabled board sees each as a peer of its own (02:00:00:00:
// <id>, m0_main's cable_mac) and relays them to the cell (ADR-009), so the real foreman holds its two memberships
// against them. Stub boards are the image's boards with ids kStubFirst..kStubLast; each keeps what a version-5 image
// gives it (parse_image's scope), and publishes a stand-in node-<id>/hw/die_temp for its cell's summary.
//
// Transport as pot_hostnode's: potluck.stubs owns the serial port (or the hub's relay) and hands this process raw
// Potluck Frames on stdin, each a 2-byte little-endian length then the frame; frames for the cable go to stdout the
// same way; statistics to stderr, one JSON object per line. Control records (a length with bit 15 set, then text):
// "sleep <hex id> 1|0" -- a stub's radio off or on, to kill a stub foreman.
//
//   pot_stubs --image <file> --ids <dir> [--cable-cell <n>] [--system <name>]
//
// <dir> holds stub-<id>.id per stub (hex id, lower case): "seed <64 hex>", "ca <64 hex>", "cert <hex>" -- made and
// enrolled by potluck.stubs. --cable-cell: the cell whose radio space the cable is in (the real cell's parent); by
// default the parent of the cell whose foreman role lists a board that is not a stub.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#include "pot/actor.hpp"
#include "pot/deploy.hpp"
#include "pot/foreman.hpp"
#include "pot/frame.hpp"
#include "pot/node.hpp"
#include "pot/position.hpp"
#include "pot/reconcile.hpp"
#include "pot/ticker.hpp"
#include "pot/trust.hpp"

using namespace pot;

namespace {

constexpr uint16_t kStubFirst = 0xF000;
constexpr uint16_t kStubLast = 0xFDFF;
// The cabled board, as every stub on the cable addresses it (pot_hostnode's kCableMac).
constexpr uint8_t kCableMac[kMacLen] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};

using Clock = std::chrono::steady_clock;
const Clock::time_point g_t0 = Clock::now();
uint32_t now_us() {
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - g_t0).count());
}
// From the clock, not from now_us(): divided down from a 32-bit microsecond count it fell back to 0 every 71.6 min
// (the same fault as pot_hostnode's, 2026-10-23).
uint32_t now_ms() {
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_t0).count());
}

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return true;
}
bool hex_to(const std::string& h, uint8_t* out, size_t n) {
    if (h.size() != 2 * n) return false;
    for (size_t i = 0; i < n; ++i) {
        char* end = nullptr;
        const std::string b = h.substr(2 * i, 2);
        out[i] = static_cast<uint8_t>(std::strtol(b.c_str(), &end, 16));
        if (end != b.c_str() + 2) return false;
    }
    return true;
}
std::string field(const std::string& text, const char* key) {
    const std::string k = std::string(key) + " ";
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.compare(0, k.size(), k) == 0) return line.substr(k.size());
        at = end + 1;
    }
    return "";
}

std::mutex g_out_mu;
void to_cable(const uint8_t* data, size_t len) {
    const uint8_t hdr[2] = {static_cast<uint8_t>(len & 0xFF), static_cast<uint8_t>(len >> 8)};
    std::lock_guard<std::mutex> lk(g_out_mu);
    std::fwrite(hdr, 1, 2, stdout);
    std::fwrite(data, 1, len, stdout);
    std::fflush(stdout);
}

std::mutex g_in_mu;
std::deque<std::vector<uint8_t>> g_in;
std::deque<std::string> g_ctl;
std::atomic<bool> g_eof{false};
void reader() {
    for (;;) {
        uint8_t hdr[2];
        if (std::fread(hdr, 1, 2, stdin) != 2) break;
        const size_t raw = static_cast<size_t>(hdr[0]) | (static_cast<size_t>(hdr[1]) << 8);
        const size_t len = raw & 0x7FFF;
        std::vector<uint8_t> f(len);
        if (len > 0 && std::fread(f.data(), 1, len, stdin) != len) break;
        std::lock_guard<std::mutex> lk(g_in_mu);
        if (raw & 0x8000) g_ctl.emplace_back(f.begin(), f.end());
        else g_in.push_back(std::move(f));
    }
    g_eof = true;
}

struct Stub;
struct Flight {
    uint32_t due_us;
    Stub* to;
    uint8_t from_mac[kMacLen];
    std::vector<uint8_t> data;
};
std::vector<Flight> g_air;

struct Stub {
    uint16_t id = 0;
    uint64_t spaces = 0;
    bool asleep = false;
    uint8_t mac[kMacLen] = {};
    Identity identity;
    NodeHal hal{};
    std::unique_ptr<Node> node;
    std::unique_ptr<Reconciler> rec;
    std::vector<PortableSpec> specs;
    std::vector<uint8_t> image;  // its own copy: decls point into it
    uint32_t temp_hash = 0;
};
std::vector<std::unique_ptr<Stub>> g_stubs;
uint64_t g_cable_space = 0;
struct Watch {
    uint16_t stub;
    uint32_t hash;
};
std::vector<Watch> g_watch;  // control "watch"/"refer": a path read at a stub every stats round

int32_t stub_send(void* ctx, const uint8_t mac[kMacLen], const uint8_t* data, size_t len) {
    Stub* s = static_cast<Stub*>(ctx);
    if (s->asleep) return 0;
    const bool bcast = std::memcmp(mac, kBroadcastMacAddr, kMacLen) == 0;
    bool to_stub = false;
    for (auto& o : g_stubs) {
        if (o.get() == s || o->asleep || (o->spaces & s->spaces) == 0) continue;
        if (bcast || std::memcmp(o->mac, mac, kMacLen) == 0) {
            g_air.push_back(Flight{now_us() + 3000, o.get(), {}, std::vector<uint8_t>(data, data + len)});
            std::memcpy(g_air.back().from_mac, s->mac, kMacLen);
            to_stub = true;
        }
    }
    // The cable: a broadcast from a stub in its space, or a unicast to a member reached through the cabled board.
    if ((s->spaces & g_cable_space) != 0 && (bcast || !to_stub)) to_cable(data, len);
    return 0;
}

const char* state_str(PeerState st) {
    return st == PeerState::Alive ? "alive" : st == PeerState::Dead ? "dead" : "other";
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::string image_path, ids_dir;
    int cable_cell = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--image" && i + 1 < argc) image_path = argv[++i];
        else if (a == "--ids" && i + 1 < argc) ids_dir = argv[++i];
        else if (a == "--cable-cell" && i + 1 < argc) cable_cell = std::atoi(argv[++i]);
        else if (a == "--system" && i + 1 < argc) ++i;  // the image carries it
    }
    if (image_path.empty() || ids_dir.empty()) {
        std::fprintf(stderr, "usage: pot_stubs --image <file> --ids <dir> [--cable-cell <n>]\n");
        return 2;
    }
    std::vector<uint8_t> img;
    if (!read_file(image_path, img)) {
        std::fprintf(stderr, "cannot read %s\n", image_path.c_str());
        return 1;
    }
    // The whole image once, to find the stubs, their cells, and the cable's cell.
    const char* why = "";
    if (!parse_image(img.data(), img.size(), validation_image(), &why, kKeepNone)) {
        std::fprintf(stderr, "image refused: %s\n", why);
        return 1;
    }
    std::map<uint16_t, uint64_t> cells_of;  // stub -> its spaces
    std::vector<uint16_t> stub_ids;
    {
        // Walk the declarations by hand: the image may list more than one node keeps.
        const uint8_t count = img[5];
        size_t at = kImageHeaderLen;
        std::vector<ActorDecl> decls;
        for (uint8_t i = 0; i < count; ++i) {
            ActorDecl d{static_cast<uint16_t>(img[at] | img[at + 1] << 8), static_cast<ActorType>(img[at + 2]),
                        img[at + 3], img.data() + at + 4};
            decls.push_back(d);
            at += 4 + d.cfg_len;
        }
        for (const ActorDecl& d : decls) {
            PositionConfig pc{};
            if (d.type == ActorType::Position && position_config(d, pc) && d.node_id >= kStubFirst &&
                d.node_id <= kStubLast) {
                stub_ids.push_back(d.node_id);
                for (uint8_t c : pc.cells)
                    if (c != 0 && c < 64) cells_of[d.node_id] |= 1ull << c;
            }
        }
        for (const ActorDecl& d : decls) {
            ForemanConfig fc{};
            if (d.type != ActorType::Foreman || !foreman_config(d, fc)) continue;
            bool real = false;
            for (uint8_t k = 0; k < fc.n_members; ++k)
                real = real || fc.members[k] < kStubFirst || fc.members[k] > kStubLast;
            if (real && cable_cell == 0) cable_cell = fc.parent;
            for (uint8_t k = 0; k < fc.place.count; ++k) {  // a board that may hold the role hears the parent too
                auto it = cells_of.find(fc.place.node[k]);
                if (it != cells_of.end() && fc.parent < 64) it->second |= 1ull << fc.parent;
            }
        }
    }
    if (cable_cell <= 0 || cable_cell >= 64) {
        std::fprintf(stderr, "no cable cell: give --cable-cell, or a package whose real cell has a foreman role\n");
        return 1;
    }
    g_cable_space = 1ull << cable_cell;
    static const ActorKind kKinds[] = {kForemanKind, kTickerKind};
    const uint32_t epoch = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    for (uint16_t id : stub_ids) {
        auto s = std::make_unique<Stub>();
        s->id = id;
        s->spaces = cells_of[id];
        s->mac[0] = 0x02;
        s->mac[4] = static_cast<uint8_t>(id >> 8);
        s->mac[5] = static_cast<uint8_t>(id & 0xFF);
        char name[32];
        std::snprintf(name, sizeof name, "stub-%04x.id", static_cast<unsigned>(id));
        std::vector<uint8_t> idf;
        if (!read_file(ids_dir + "/" + name, idf)) {
            std::fprintf(stderr, "cannot read %s/%s\n", ids_dir.c_str(), name);
            return 1;
        }
        const std::string text(idf.begin(), idf.end());
        uint8_t seed[kEdSeedLen], ca[kEdPubLen];
        const std::string cert_hex = field(text, "cert");
        std::vector<uint8_t> cert(cert_hex.size() / 2);
        if (!hex_to(field(text, "seed"), seed, sizeof seed) || !hex_to(field(text, "ca"), ca, sizeof ca) ||
            cert.empty() || !hex_to(cert_hex, cert.data(), cert.size())) {
            std::fprintf(stderr, "%s: not an enrolled identity\n", name);
            return 1;
        }
        identity_from_seed(s->identity, seed);
        if (identity_install_cert(s->identity, id, ca, cert.data(), cert.size()) != CertError::Ok) {
            std::fprintf(stderr, "%s: certificate refused\n", name);
            return 1;
        }
        s->image = img;
        static DeployImage mine;
        mine = DeployImage{};
        if (!parse_image(s->image.data(), s->image.size(), mine, &why, id)) {
            std::fprintf(stderr, "image refused for %04x: %s\n", id, why);
            return 1;
        }
        s->specs.resize(kMaxPortable);
        size_t n_specs = 0;
        if (!collect_portable(mine, kKinds, 2, s->specs.data(), kMaxPortable, n_specs, &why)) {
            std::fprintf(stderr, "image refused for %04x: %s\n", id, why);
            return 1;
        }
        s->specs.resize(n_specs);
        NodeConfig cfg;
        cfg.node_id = id;
        cfg.boot_epoch = epoch;
        std::memcpy(cfg.mac, s->mac, kMacLen);
        cfg.has_trusted_mac = true;  // the cable, for the stubs on it
        std::memcpy(cfg.trusted_mac, kCableMac, kMacLen);
        cfg.channel_known = false;   // no radio here: nobody follows a stub's channel
        cfg.msg_id_max = 0x7FFF;     // behind potluck-hub, whose own requests are numbered from 0x8000
        s->hal.ctx = s.get();
        s->hal.send = &stub_send;
        s->hal.now_ms = [](void*) { return now_ms(); };
        s->hal.now_us = [](void*) { return now_us(); };
        s->node = std::make_unique<Node>(cfg, s->hal);
        s->node->set_trust(&s->identity, true);
        for (uint8_t i = 0; i < mine.actor_count; ++i) {  // its cells, from its own position (as the actor would)
            PositionConfig pc{};
            if (mine.actors[i].type == ActorType::Position && mine.actors[i].node_id == id &&
                position_config(mine.actors[i], pc)) {
                s->node->set_groups(pc.cells[0], pc.cells[1], pc.cell_window[0] * 100u, pc.cell_window[1] * 100u);
            }
        }
        s->rec = std::make_unique<Reconciler>(*s->node);
        if (!s->rec->load(s->specs.data(), s->specs.size())) {
            std::fprintf(stderr, "reconciler for %04x: too many portable actors\n", id);
            return 1;
        }
        s->node->set_call_handler(
            [](void* c, uint16_t from, uint16_t, uint32_t path, const uint8_t* args, uint16_t len) {
                return static_cast<Stub*>(c)->rec->on_cast(from, path, args, len);
            },
            s.get());
        g_stubs.push_back(std::move(s));
    }
    // A stand-in die temperature on every stub, and replicas of its cell-mates' (as pinned outputs are declared).
    char sys[40] = "lab";
    {
        // the cluster's name, from the image's Cluster declaration, for the paths
        const uint8_t count = img[5];
        size_t at = kImageHeaderLen;
        for (uint8_t i = 0; i < count; ++i) {
            const uint8_t len = img[at + 3];
            if (img[at + 2] == static_cast<uint8_t>(ActorType::Cluster) && len < sizeof sys) {
                std::memcpy(sys, img.data() + at + 4, len);
                sys[len] = 0;
            }
            at += 4 + len;
        }
    }
    for (auto& s : g_stubs) {
        char p[96];
        std::snprintf(p, sizeof p, "potluck://%s/node-%04x/hw/die_temp", sys, static_cast<unsigned>(s->id));
        s->temp_hash = path_hash(p);
    }
    for (auto& s : g_stubs) {
        for (auto& o : g_stubs) {
            if ((cells_of[o->id] & cells_of[s->id] & ~g_cable_space) == 0 && o.get() != s.get()) continue;
            NsDecl d;
            d.path_hash = o->temp_hash;
            d.owner_node = o->id;
            d.type = ValueType::F32;
            d.unit = Unit::Celsius;
            d.staleness_bound_ms = 6000;
            s->node->ns().declare(d);
        }
        s->node->start();
        s->rec->start(now_ms());
        std::fprintf(stderr, "{\"t\":\"stub_boot\",\"node\":%u,\"spaces\":%llu,\"groups\":%u,\"portable\":%u}\n",
                     static_cast<unsigned>(s->id), static_cast<unsigned long long>(s->spaces),
                     static_cast<unsigned>(s->node->groups()), static_cast<unsigned>(s->specs.size()));
    }
    std::fflush(stderr);

    std::thread rd(reader);
    rd.detach();
    uint32_t next_stats = now_ms() + 2000, next_temp = 0;
    while (!g_eof) {
        for (;;) {
            std::vector<uint8_t> f;
            {
                std::lock_guard<std::mutex> lk(g_in_mu);
                if (g_in.empty()) break;
                f = std::move(g_in.front());
                g_in.pop_front();
            }
            Frame fr{};
            if (parse(f.data(), f.size(), fr) != FrameError::Ok) continue;
            for (auto& s : g_stubs) {  // from the cabled board: to the stubs on the cable, a unicast to its stub only
                if (s->asleep || (s->spaces & g_cable_space) == 0) continue;
                if (fr.hdr.dst != kNodeBroadcast && fr.hdr.dst != s->id) continue;
                s->node->on_rx(kCableMac, f.data(), f.size(), now_us(), 0);
            }
        }
        for (;;) {
            std::string c;
            {
                std::lock_guard<std::mutex> lk(g_in_mu);
                if (g_ctl.empty()) break;
                c = std::move(g_ctl.front());
                g_ctl.pop_front();
            }
            unsigned id = 0, on = 0, via = 0;
            unsigned long hash = 0;
            if (std::sscanf(c.c_str(), "refer %x %x %lu", &id, &via, &hash) == 3) {
                // a referral (Node::request_referral) from stub <id> through <via>, then watched
                for (auto& s : g_stubs)
                    if (s->id == id) s->node->request_referral(static_cast<uint16_t>(via), static_cast<uint32_t>(hash));
                g_watch.push_back({static_cast<uint16_t>(id), static_cast<uint32_t>(hash)});
            } else if (std::sscanf(c.c_str(), "watch %x %lu", &id, &hash) == 2) {
                g_watch.push_back({static_cast<uint16_t>(id), static_cast<uint32_t>(hash)});
            } else if (std::sscanf(c.c_str(), "sleep %x %u", &id, &on) == 2) {
                for (auto& s : g_stubs)
                    if (s->id == id) s->asleep = on != 0;
                std::fprintf(stderr, "{\"t\":\"stub_sleep\",\"node\":%u,\"asleep\":%u}\n", id, on);
            }
        }
        std::vector<Flight> due;
        for (size_t k = 0; k < g_air.size();) {
            if (static_cast<int32_t>(now_us() - g_air[k].due_us) >= 0) {
                due.push_back(std::move(g_air[k]));
                g_air.erase(g_air.begin() + static_cast<std::ptrdiff_t>(k));
            } else {
                ++k;
            }
        }
        for (Flight& f : due) {
            if (!f.to->asleep) f.to->node->on_rx(f.from_mac, f.data.data(), f.data.size(), now_us(), -50);
        }
        const uint32_t t = now_ms();  // after this pass's frames, as a board's link task reads it
        if (static_cast<int32_t>(t - next_temp) >= 0) {
            next_temp = t + 1000;
            for (auto& s : g_stubs) {
                if (!s->asleep) s->node->publish(s->temp_hash, Value::of_f32(30.0f + static_cast<float>(s->id % 7)));
            }
        }
        for (auto& s : g_stubs) {
            if (s->asleep) continue;
            s->node->tick(t);
            s->rec->tick(t);
            Event e{};
            while (s->node->events().pop(e)) {
                if (e.kind != EventKind::ActorStarted && e.kind != EventKind::ActorStopped &&
                    e.kind != EventKind::ActorOwner && e.kind != EventKind::PeerDead) {
                    continue;
                }
                std::fprintf(stderr, "{\"t\":\"event\",\"node\":%u,\"at_ms\":%u,\"kind\":\"%s\",\"peer\":%u,\"a\":%u,\"b\":%u}\n",
                             static_cast<unsigned>(s->id), static_cast<unsigned>(t), event_kind_str(e.kind),
                             static_cast<unsigned>(e.node_id), static_cast<unsigned>(e.detail_a),
                             static_cast<unsigned>(e.detail_b));
            }
        }
        if (static_cast<int32_t>(t - next_stats) >= 0) {
            next_stats = t + 2000;
            for (const Watch& w : g_watch) {
                for (auto& s : g_stubs) {
                    if (s->id != w.stub) continue;
                    const NsEntry* e = s->node->ns().find(w.hash);
                    Reading r;
                    const NsError st = s->node->read(w.hash, r);
                    uint32_t u = 0;
                    float fv = 0;
                    const bool hu = r.value.as_u32(u), hf = r.value.as_f32(fv);
                    std::fprintf(stderr, "{\"t\":\"watch\",\"node\":%u,\"up_ms\":%u,\"hash\":%u,\"status\":%u,\"owner\":%u,"
                                 "\"quality\":%u,\"u32\":%lld,\"f32\":%.3f,\"unit\":%u,\"age_ms\":%u}\n",
                                 static_cast<unsigned>(s->id), static_cast<unsigned>(t), static_cast<unsigned>(w.hash),
                                 static_cast<unsigned>(st), static_cast<unsigned>(e != nullptr ? e->owner_node : 0),
                                 static_cast<unsigned>(r.quality), hu ? static_cast<long long>(u) : -1LL,
                                 hf ? static_cast<double>(fv) : 0.0, static_cast<unsigned>(r.unit),
                                 static_cast<unsigned>(r.age_ms));
                    if (e != nullptr && e->owner_node != 0 && e->owner_node != s->id) {
                        s->node->request_read(e->owner_node, w.hash);
                    }
                }
            }
            for (auto& s : g_stubs) {
                std::string peers, roles;
                for (size_t i = 0; i < PeerTable::capacity(); ++i) {
                    const PeerLink& p = s->node->peers().slot(i);
                    if (p.state == PeerState::Free) continue;
                    char b[96];
                    std::snprintf(b, sizeof b,
                                  "%s{\"id\":%u,\"state\":\"%s\",\"relay\":%d,\"groups\":%u,\"silent\":%u,"
                                  "\"hb\":%u,\"limit\":%u}",
                                  peers.empty() ? "" : ",", static_cast<unsigned>(p.node_id), state_str(p.state),
                                  s->node->peer_relayed(&p) ? 1 : 0, static_cast<unsigned>(s->node->peer_groups(i)),
                                  static_cast<unsigned>(t - p.last_rx_ms), static_cast<unsigned>(p.hb_period_ms),
                                  static_cast<unsigned>(p.miss_limit));
                    peers += b;
                }
                for (size_t i = 0; i < s->rec->actor_count(); ++i) {
                    const Reconciler::ActorView v = s->rec->view(i);
                    Reading r;
                    uint32_t alive = 0;
                    const NsError rst = s->node->read(v.key, r);
                    const bool good = rst == NsError::Ok && r.quality == Quality::Good && r.value.as_u32(alive);
                    const NsEntry* e = s->node->ns().find(v.key);
                    if (e != nullptr && e->owner_node != 0 && e->owner_node != s->id) s->node->request_read(e->owner_node, v.key);
                    char b[160];
                    std::snprintf(b, sizeof b,
                                  "%s{\"key\":%u,\"running\":%d,\"owner\":%u,\"standby\":%u,\"alive\":%d,\"q\":%u,"
                                  "\"st\":%u,\"age\":%u}",
                                  roles.empty() ? "" : ",", static_cast<unsigned>(v.key), v.running ? 1 : 0,
                                  static_cast<unsigned>(v.owner), static_cast<unsigned>(v.standby),
                                  good ? static_cast<int>(alive) : -1, static_cast<unsigned>(r.quality),
                                  static_cast<unsigned>(rst), static_cast<unsigned>(r.age_ms));
                    roles += b;
                }
                const Node::AuthCounters& ac = s->node->auth_counters();
                const Node::NsCounters& nc = s->node->ns_counters();
                std::fprintf(stderr, "{\"t\":\"stub\",\"node\":%u,\"up_ms\":%u,\"asleep\":%d,\"groups\":%u,\"settled\":%d,"
                             "\"peers\":[%s],\"roles\":[%s],\"auth\":{\"verified\":%u,\"refused\":%u,\"limited\":%u,"
                             "\"tags_ok\":%u,\"bad_tag\":%u,\"untagged\":%u},\"ns\":{\"served\":%u,\"asked\":%u,"
                             "\"matched\":%u,\"unmatched\":%u,\"fenced\":%u,\"timeouts\":%u}}\n",
                             static_cast<unsigned>(s->id), static_cast<unsigned>(t), s->asleep ? 1 : 0,
                             static_cast<unsigned>(s->node->groups()), s->rec->settled() ? 1 : 0, peers.c_str(),
                             roles.c_str(), static_cast<unsigned>(ac.verified), static_cast<unsigned>(ac.refused),
                             static_cast<unsigned>(ac.rate_limited), static_cast<unsigned>(ac.tags_ok),
                             static_cast<unsigned>(ac.bad_tag), static_cast<unsigned>(ac.untagged),
                             static_cast<unsigned>(nc.reads_served), static_cast<unsigned>(nc.reads_requested),
                             static_cast<unsigned>(nc.replies_matched), static_cast<unsigned>(nc.replies_unmatched),
                             static_cast<unsigned>(nc.replies_fenced), static_cast<unsigned>(nc.read_timeouts));
            }
            std::fflush(stderr);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return 0;
}
