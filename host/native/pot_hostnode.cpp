// M10: the PC as a placement node. A Potluck node built from the boards' own sources -- pot::Node, the
// reconciler, the actor runtime and the built-in actors, the same files the unit tests compile -- so a
// portable actor runs on the PC from the same source it runs from on a board (ARCHITECTURE section 13,
// M10 (3)). Not a reimplementation: there is no host-side copy of any of it.
//
// It has no transport of its own. potluck.hostnode (Python) owns the serial port and its COBS/CRC
// framing, and hands this process raw Potluck Frames on stdin; it writes the frames it sends to stdout.
// Both directions: a 2-byte little-endian length, then the frame. Statistics go to stderr, one JSON
// object per line.
//
//   pot_hostnode --keygen <identity file>              make an identity (prints its public key)
//   pot_hostnode --identity <file> --image <file> [--node 0x00fe]
//
// The identity file is text: "seed <64 hex>", then, once enrolled, "ca <64 hex>" and "cert <hex>". The
// image is a compiled deploy image (potluck.deploy.compile_image), the same bytes a board stores.
//
// On the wire this node is the cable's far end: the board it is cabled to reaches it at kHostMac and
// trusts that link (section 9.3); here, every frame arrives from that one board, which relays the rest
// of the cell (ADR-009). So this node trusts the cable too, and reaches every other member through it.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#include "pot/actor.hpp"
#include "pot/deploy.hpp"
#include "pot/node.hpp"
#include "pot/reconcile.hpp"
#include "pot/ticker.hpp"
#include "pot/trust.hpp"

using namespace pot;

namespace {

// The cable's far end -- the board this node is attached to -- as this node addresses it. Frames from
// every member arrive through it; the header's src says which member sent each one.
constexpr uint8_t kCableMac[kMacLen] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
// This node's own address, as the board knows it (the firmware's kHostMac, potluck.bridge.HOST_MAC).
constexpr uint8_t kSelfMac[kMacLen] = {0x02, 0x00, 0x00, 0x00, 0x00, 0xFE};

using Clock = std::chrono::steady_clock;
const Clock::time_point g_t0 = Clock::now();
uint32_t now_us() {
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - g_t0).count());
}
uint32_t now_ms() { return now_us() / 1000u; }

std::mutex g_out_mu;
std::atomic<uint32_t> g_tx_frames{0};
std::atomic<uint32_t> g_rx_frames{0};

int32_t hal_send(void*, const uint8_t*, const uint8_t* data, size_t len) {
    if (len > 0xFFFF) return -1;
    const uint8_t hdr[2] = {static_cast<uint8_t>(len & 0xFF), static_cast<uint8_t>(len >> 8)};
    std::lock_guard<std::mutex> lk(g_out_mu);
    if (std::fwrite(hdr, 1, 2, stdout) != 2 || std::fwrite(data, 1, len, stdout) != len) return -1;
    std::fflush(stdout);
    ++g_tx_frames;
    return 0;
}

// Frames from stdin, queued by a reader thread and handed to the node on the main thread only.
std::mutex g_in_mu;
std::deque<std::vector<uint8_t>> g_in;
std::atomic<bool> g_eof{false};

void reader() {
    for (;;) {
        uint8_t hdr[2];
        if (std::fread(hdr, 1, 2, stdin) != 2) break;
        const size_t len = static_cast<size_t>(hdr[0]) | (static_cast<size_t>(hdr[1]) << 8);
        std::vector<uint8_t> f(len);
        if (len > 0 && std::fread(f.data(), 1, len, stdin) != len) break;
        std::lock_guard<std::mutex> lk(g_in_mu);
        g_in.push_back(std::move(f));
    }
    g_eof = true;
}

bool hex_to(const std::string& h, uint8_t* out, size_t n) {
    if (h.size() != 2 * n) return false;
    for (size_t i = 0; i < n; ++i) {
        const std::string b = h.substr(2 * i, 2);
        char* end = nullptr;
        const long v = std::strtol(b.c_str(), &end, 16);
        if (end != b.c_str() + 2) return false;
        out[i] = static_cast<uint8_t>(v);
    }
    return true;
}
std::string to_hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

bool read_file(const char* path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return false;
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return true;
}

// "key value" lines.
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

const char* state_str(PeerState s) {
    switch (s) {
        case PeerState::Alive: return "alive";
        case PeerState::Dead: return "dead";
        case PeerState::Free: return "free";
        default: return "other";
    }
}

int keygen(const char* path) {
    std::random_device rd;
    uint8_t seed[kEdSeedLen];
    for (uint8_t& b : seed) b = static_cast<uint8_t>(rd());
    Identity id;
    identity_from_seed(id, seed);
    FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "cannot write %s\n", path);
        return 1;
    }
    std::fprintf(f, "seed %s\n", to_hex(seed, sizeof(seed)).c_str());
    std::fclose(f);
    std::printf("%s\n", to_hex(id.pub, sizeof(id.pub)).c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    const char* identity_path = nullptr;
    const char* image_path = nullptr;
    uint16_t node_id = 0x00FE;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--keygen" && i + 1 < argc) return keygen(argv[i + 1]);
        if (a == "--identity" && i + 1 < argc) identity_path = argv[++i];
        else if (a == "--image" && i + 1 < argc) image_path = argv[++i];
        else if (a == "--node" && i + 1 < argc) node_id = static_cast<uint16_t>(std::strtoul(argv[++i], nullptr, 0));
    }
    if (identity_path == nullptr || image_path == nullptr) {
        std::fprintf(stderr, "usage: pot_hostnode --identity <file> --image <file> [--node 0x00fe] | --keygen <file>\n");
        return 2;
    }

    // ---- identity: the same enrolment a board has ----
    std::vector<uint8_t> idfile;
    if (!read_file(identity_path, idfile)) {
        std::fprintf(stderr, "cannot read %s\n", identity_path);
        return 1;
    }
    const std::string idtext(idfile.begin(), idfile.end());
    static Identity id;
    uint8_t seed[kEdSeedLen];
    if (!hex_to(field(idtext, "seed"), seed, sizeof(seed))) {
        std::fprintf(stderr, "identity: no seed\n");
        return 1;
    }
    identity_from_seed(id, seed);
    uint8_t ca[kEdPubLen];
    const std::string cert_hex = field(idtext, "cert");
    std::vector<uint8_t> cert(cert_hex.size() / 2);
    if (!hex_to(field(idtext, "ca"), ca, sizeof(ca)) || cert.empty() ||
        !hex_to(cert_hex, cert.data(), cert.size())) {
        std::fprintf(stderr, "identity: not enrolled (no ca/cert)\n");
        return 1;
    }
    const CertError ce = identity_install_cert(id, node_id, ca, cert.data(), cert.size());
    if (ce != CertError::Ok) {
        std::fprintf(stderr, "identity: certificate refused (%u)\n", static_cast<unsigned>(ce));
        return 1;
    }

    // ---- the image: every portable actor, as on every board (section 7.7) ----
    std::vector<uint8_t> imgbytes;
    if (!read_file(image_path, imgbytes)) {
        std::fprintf(stderr, "cannot read %s\n", image_path);
        return 1;
    }
    DeployImage img{};
    const char* why = "";
    if (!parse_image(imgbytes.data(), imgbytes.size(), img, &why)) {
        std::fprintf(stderr, "image refused: %s\n", why);
        return 1;
    }
    static const ActorKind kKinds[] = {kTickerKind};
    static PortableSpec specs[kMaxPortable];
    size_t n_specs = 0;
    if (!collect_portable(img, kKinds, 1, specs, kMaxPortable, n_specs, &why)) {
        std::fprintf(stderr, "image refused: %s\n", why);
        return 1;
    }

    // ---- the node ----
    NodeConfig cfg;
    cfg.node_id = node_id;
    std::memcpy(cfg.mac, kSelfMac, kMacLen);
    // A restart must be a new incarnation to the cell, and the trust fence is (certificate issue
    // time, epoch): seconds since the Unix epoch only ever grow.
    cfg.boot_epoch = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                               std::chrono::system_clock::now().time_since_epoch())
                                               .count());
    cfg.has_trusted_mac = true;  // the cable, as the board trusts it (section 9.3)
    std::memcpy(cfg.trusted_mac, kCableMac, kMacLen);
    cfg.channel_known = false;   // no radio here: declare no channel, so nobody follows a PC anywhere
    NodeHal hal{};
    hal.send = &hal_send;
    hal.now_ms = [](void*) { return now_ms(); };
    hal.now_us = [](void*) { return now_us(); };
    static Node node(cfg, hal);
    node.set_trust(&id, true);
    static Reconciler rec(node);
    if (!rec.load(specs, n_specs)) {
        std::fprintf(stderr, "reconciler: could not load %u portable actor(s)\n", static_cast<unsigned>(n_specs));
        return 1;
    }
    node.set_call_handler(
        [](void*, uint16_t from, uint16_t, uint32_t path, const uint8_t* args, uint16_t len) {
            return rec.on_cast(from, path, args, len);
        },
        nullptr);
    node.start();
    rec.start(now_ms());
    std::fprintf(stderr, "{\"t\":\"host_boot\",\"node\":%u,\"epoch\":%u,\"portable\":%u,\"pub\":\"%s\"}\n",
                 static_cast<unsigned>(node_id), static_cast<unsigned>(cfg.boot_epoch),
                 static_cast<unsigned>(n_specs), to_hex(id.pub, 8).c_str());
    std::fflush(stderr);

    std::thread rd(reader);
    rd.detach();
    uint32_t next_stats = now_ms() + 2000;
    while (!g_eof) {
        for (;;) {
            std::vector<uint8_t> f;
            {
                std::lock_guard<std::mutex> lk(g_in_mu);
                if (g_in.empty()) break;
                f = std::move(g_in.front());
                g_in.pop_front();
            }
            ++g_rx_frames;
            node.on_rx(kCableMac, f.data(), f.size(), now_us(), 0);
        }
        const uint32_t t = now_ms();
        node.tick(t);
        rec.tick(t);
        Event e{};
        while (node.events().pop(e)) {
            if (e.kind == EventKind::PeerAdmitted) continue;
            std::fprintf(stderr, "{\"t\":\"event\",\"node\":%u,\"at_ms\":%u,\"kind\":\"%s\",\"peer\":%u,\"a\":%u,\"b\":%u}\n",
                         static_cast<unsigned>(node_id), static_cast<unsigned>(t), event_kind_str(e.kind),
                         static_cast<unsigned>(e.node_id), static_cast<unsigned>(e.detail_a),
                         static_cast<unsigned>(e.detail_b));
        }
        if (static_cast<int32_t>(t - next_stats) >= 0) {
            next_stats = t + 1000;
            std::string peers;
            for (size_t i = 0; i < PeerTable::capacity(); ++i) {
                const PeerLink& p = node.peers().slot(i);
                if (p.state == PeerState::Free) continue;
                char b[128];
                std::snprintf(b, sizeof(b), "%s{\"id\":%u,\"state\":\"%s\",\"via_relay\":%d,\"epoch\":%u}",
                              peers.empty() ? "" : ",", static_cast<unsigned>(p.node_id), state_str(p.state),
                              node.peer_relayed(&p) ? 1 : 0, static_cast<unsigned>(p.boot_epoch));
                peers += b;
            }
            std::fprintf(stderr, "{\"t\":\"host\",\"node\":%u,\"up_ms\":%u,\"rx\":%u,\"tx\":%u,\"settled\":%d,\"peers\":[%s]}\n",
                         static_cast<unsigned>(node_id), static_cast<unsigned>(t), static_cast<unsigned>(g_rx_frames.load()),
                         static_cast<unsigned>(g_tx_frames.load()), rec.settled() ? 1 : 0, peers.c_str());
            {
                const Reconciler::Counters& rc = rec.counters();
                std::fprintf(stderr, "{\"t\":\"rec_counters\",\"node\":%u,\"up_ms\":%u,\"sent\":%u,\"received\":%u,"
                             "\"malformed\":%u,\"unknown_peer\":%u,\"views\":%u,\"send_failures\":%u}\n",
                             static_cast<unsigned>(node_id), static_cast<unsigned>(t), static_cast<unsigned>(rc.sets_sent),
                             static_cast<unsigned>(rc.sets_received), static_cast<unsigned>(rc.sets_malformed),
                             static_cast<unsigned>(rc.sets_unknown_peer), static_cast<unsigned>(rc.view_changes),
                             static_cast<unsigned>(rc.send_failures));
            }
            for (size_t i = 0; i < rec.actor_count(); ++i) {
                const Reconciler::ActorView v = rec.view(i);
                std::fprintf(stderr, "{\"t\":\"rec\",\"node\":%u,\"up_ms\":%u,\"key\":%u,\"running\":%d,\"term\":%u,"
                             "\"owner\":%u,\"owner_term\":%u,\"assigned\":%u,\"activations\":%u,\"fenced\":%u,\"released\":%u}\n",
                             static_cast<unsigned>(node_id), static_cast<unsigned>(t), static_cast<unsigned>(v.key),
                             v.running ? 1 : 0, static_cast<unsigned>(v.term), static_cast<unsigned>(v.owner),
                             static_cast<unsigned>(v.owner_term), static_cast<unsigned>(v.assigned),
                             static_cast<unsigned>(v.activations), static_cast<unsigned>(v.fenced),
                             static_cast<unsigned>(v.released));
            }
            std::fflush(stderr);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return 0;
}
