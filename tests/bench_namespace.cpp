// Namespace lookup microbenchmark -- ARCHITECTURE.md section 15, question 2 (2026-10-15).
//
// Not part of pot_tests: a standalone program that links the real pot_ns sources (namespace.cpp,
// value.cpp) and measures Namespace::find() at several fill levels, with path hashes made from
// realistic Potluck paths (FNV-1a, exactly as the boards and the host compute them).
//
// It reports two kinds of figure, and they transfer differently:
//   - probes (slots compared) per lookup: a property of the table and the hashes, identical on any
//     machine, the ESP32-S3 included;
//   - nanoseconds per lookup: a HOST measurement, for scale only. The on-board time per probe is a
//     [MEASURE] (an S3 at 240 MHz runs from cache/SRAM with no out-of-order core).
//
// Build and run: tools\run_ns_bench.ps1   (MSVC /O2, the same compiler the host tests use)

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "pot/namespace.hpp"

using namespace pot;

namespace {

// Realistic paths: what a 20-board cell's tables actually hold -- built-ins, per-link radio statistics,
// actor outputs, pinned sensor readings (tools/ns_census.py names the bench's entries from the same set).
std::vector<std::string> realistic_paths() {
    static const char* const sys[] = {"sys/heap-free", "sys/heap-largest", "sys/uptime",
                                      "sys/boot-epoch", "sys/peers-alive", "sys/rssi"};
    static const char* const link[] = {"seq", "count", "mean", "var", "min", "max"};
    std::vector<std::string> out;
    char buf[96];
    uint16_t nodes[20];
    for (int i = 0; i < 20; ++i) nodes[i] = static_cast<uint16_t>(0x6300 + i * 0x0461);
    for (uint16_t n : nodes) {
        for (const char* s : sys) {
            std::snprintf(buf, sizeof(buf), "potluck://lab/node-%04x/%s", n, s);
            out.emplace_back(buf);
        }
        std::snprintf(buf, sizeof(buf), "potluck://lab/node-%04x/hw/die_temp", n);
        out.emplace_back(buf);
        std::snprintf(buf, sizeof(buf), "potluck://lab/node-%04x/led/on", n);
        out.emplace_back(buf);
        for (int r = 0; r < 4; ++r) {
            std::snprintf(buf, sizeof(buf), "potluck://building/node-%04x/room%d/co2", n, r);
            out.emplace_back(buf);
        }
        for (uint16_t q : nodes) {
            if (q == n) continue;
            for (const char* f : link) {
                std::snprintf(buf, sizeof(buf), "potluck://lab/node-%04x/rf/link-%04x/%s", n, q, f);
                out.emplace_back(buf);
            }
        }
    }
    return out;
}

NsDecl decl(uint32_t h) {
    NsDecl d;
    d.path_hash = h;
    d.owner_node = 0x6300;
    d.type = ValueType::F32;
    d.staleness_bound_ms = 5000;
    return d;
}

// Probes find() makes for `h`: slots compared until a match or a free slot (or the whole table).
size_t probes_for(const Namespace& ns, uint32_t h) {
    const size_t start = static_cast<size_t>(h) & (kNamespaceEntries - 1);
    for (size_t p = 0; p < kNamespaceEntries; ++p) {
        const NsEntry& e = ns.slot((start + p) & (kNamespaceEntries - 1));
        if (e.free() || e.path_hash == h) return p + 1;
    }
    return kNamespaceEntries;
}

volatile uintptr_t g_sink;

double ns_per_lookup(const Namespace& ns, const std::vector<uint32_t>& keys, int reps) {
    uintptr_t acc = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < reps; ++r) {
        for (uint32_t k : keys) acc += reinterpret_cast<uintptr_t>(ns.find(k));
    }
    const auto t1 = std::chrono::steady_clock::now();
    g_sink = acc;
    const double total = std::chrono::duration<double, std::nano>(t1 - t0).count();
    return total / (static_cast<double>(reps) * static_cast<double>(keys.size()));
}

}  // namespace

int main() {
    std::printf("sizeof(NsEntry) = %zu B, sizeof(Namespace) = %zu B, capacity %zu\n", sizeof(NsEntry),
                sizeof(Namespace), Namespace::capacity());
    std::printf("per entry incl. arrival time: %zu B\n\n",
                sizeof(NsEntry) + sizeof(uint32_t));

    const std::vector<std::string> paths = realistic_paths();
    std::vector<uint32_t> all;
    for (const auto& p : paths) all.push_back(path_hash(p.c_str()));

    const size_t fills[] = {16, 32, 64, 96, 112, 120, 127, 128};
    const int kTables = 2000;  // random draws of which paths fill the table
    std::mt19937 rng(20261015u);

    std::printf("probes per find(), over %d random tables of real paths (machine-independent):\n", kTables);
    std::printf("%6s %10s %10s %10s %10s %10s\n", "fill", "hit_mean", "hit_worst", "miss_mean", "miss_worst",
                "worst_seen");
    for (size_t fill : fills) {
        double hit_sum = 0, miss_sum = 0;
        size_t hit_n = 0, miss_n = 0, hit_worst = 0, miss_worst = 0, worst_probe = 0;
        for (int t = 0; t < kTables; ++t) {
            std::vector<uint32_t> pool = all;
            std::shuffle(pool.begin(), pool.end(), rng);
            Namespace ns;
            size_t placed = 0, i = 0;
            for (; i < pool.size() && placed < fill; ++i) {
                if (ns.declare(decl(pool[i])) == NsError::Ok) ++placed;
            }
            for (size_t k = 0; k < i; ++k) {
                const size_t p = probes_for(ns, pool[k]);
                hit_sum += p;
                ++hit_n;
                if (p > hit_worst) hit_worst = p;
            }
            for (size_t k = i; k < i + 64 && k < pool.size(); ++k) {  // paths not in the table
                const size_t p = probes_for(ns, pool[k]);
                miss_sum += p;
                ++miss_n;
                if (p > miss_worst) miss_worst = p;
            }
            if (ns.worst_probe() > worst_probe) worst_probe = ns.worst_probe();
        }
        std::printf("%6zu %10.2f %10zu %10.2f %10zu %10zu\n", fill, hit_sum / hit_n, hit_worst, miss_sum / miss_n,
                    miss_worst, worst_probe);
    }

    std::printf("\nhost time per find() (MSVC /O2, x64; HOST ONLY -- an S3 figure is a [MEASURE]):\n");
    std::printf("%6s %12s %12s\n", "fill", "hit_ns", "miss_ns");
    for (size_t fill : fills) {
        std::vector<uint32_t> pool = all;
        std::shuffle(pool.begin(), pool.end(), rng);
        Namespace ns;
        std::vector<uint32_t> hits, misses;
        size_t i = 0;
        for (; i < pool.size() && hits.size() < fill; ++i) {
            if (ns.declare(decl(pool[i])) == NsError::Ok) hits.push_back(pool[i]);
        }
        for (size_t k = i; k < i + 128 && k < pool.size(); ++k) misses.push_back(pool[k]);
        const int reps = 200000 / static_cast<int>(fill) + 1000;
        // best of five, to keep scheduler noise out of a sub-microsecond figure
        double best_hit = 1e30, best_miss = 1e30;
        for (int r = 0; r < 5; ++r) {
            const double h = ns_per_lookup(ns, hits, reps);
            const double m = ns_per_lookup(ns, misses, reps);
            if (h < best_hit) best_hit = h;
            if (m < best_miss) best_miss = m;
        }
        std::printf("%6zu %12.1f %12.1f\n", fill, best_hit, best_miss);
    }

    // A full table: declare refuses the 129th, and a miss walks every slot.
    Namespace full;
    size_t placed = 0;
    for (uint32_t h : all) {
        if (placed == kNamespaceEntries) break;
        if (full.declare(decl(h)) == NsError::Ok) ++placed;
    }
    const NsError e = full.declare(decl(path_hash("potluck://lab/one/too/many")));
    std::printf("\nfull table: count %zu, declare #129 -> %s, a miss probes %zu slots\n", full.count(),
                ns_error_str(e), probes_for(full, path_hash("potluck://lab/one/too/many")));
    return 0;
}
