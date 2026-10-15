// M19's acceptance in simulation: a tree of cells in separate radio spaces, the foreman role placed at run time.
//
// --scenario tree2 --cells K: K base cells of ten boards (cell k: boards 10k..10k+9, in radio space k), each with a
// foreman role eligible on its first two boards, under one upper cell (number K+1, a 5 s window, radio space K). The
// two eligible boards of a cell stand where they hear the upper cell too (spaces k and K): whoever the reconciler gives
// the role joins the upper cell, the other stays the warm standby. Every board runs a real Reconciler and the real
// foreman actor (pot/foreman.hpp), with the scope a version-5 image gives it -- its cell's role, and an eligible board
// every sibling's too. Each foreman reads every sibling cell's summary once a minute.
//
// Reported: deaths (an unperturbed tree must have none), foreman handovers per cell (none without a failure), seconds
// in which a cell had no foreman or two, the airtime of each radio space against the single-cell figures, and whether
// every foreman read every sibling's summary GOOD at the end. --kill-at S puts cell 0's foreman to sleep at second S
// for two minutes, to time a handover as the upper cell's readers see it.

#pragma once

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "cell.hpp"
#include "pot/foreman.hpp"
#include "pot/reconcile.hpp"

namespace potsim {

using namespace pot;

constexpr size_t kT2Cell = 10;

struct Tree2 {
    size_t cells = 10;
    uint8_t upper = 11;
    std::vector<Reconciler*> recs;
    std::vector<std::vector<PortableSpec>> specs;
    std::vector<uint16_t> runner;        // per cell, last second's
    std::vector<uint32_t> handovers;     // per cell, after the first assignment
    std::vector<uint32_t> no_foreman_s, two_foremen_s;
    uint32_t kill_at_s = 0;
    int killed = -1;
    uint32_t killed_at_s = 0, back_at_s = 0;  // the reader in cell 1 saw cell 0's summary GOOD again from the new one
};
Tree2 g_t2;

uint32_t t2_cell_hash(size_t cell, const char* what) {
    char p[64];
    std::snprintf(p, sizeof p, "potluck://lab/cell-c%zu/%s", cell, what);
    return path_hash(p);
}
uint16_t t2_id(size_t i) { return static_cast<uint16_t>(0x1000 + i); }

std::vector<uint8_t> t2_foreman_cfg(size_t cell, uint8_t upper) {
    std::vector<uint8_t> c;
    auto put16 = [&](uint16_t x) { c.push_back(static_cast<uint8_t>(x)); c.push_back(static_cast<uint8_t>(x >> 8)); };
    auto put32 = [&](uint32_t x) { for (int i = 0; i < 4; ++i) c.push_back(static_cast<uint8_t>(x >> (8 * i))); };
    const uint32_t alive = t2_cell_hash(cell, "alive");
    put32(alive);
    put16(2000);  // the summary every 2 s
    c.push_back(2);
    for (size_t k = 0; k < 2; ++k) {
        put16(t2_id(cell * kT2Cell + k));
        c.push_back(1);
    }
    c.push_back(static_cast<uint8_t>(cell + 1));
    c.push_back(upper);
    c.push_back(50);  // the upper cell's window, 5 s
    c.push_back(static_cast<uint8_t>(kT2Cell));
    put32(path_hash("potluck://lab/node-"));
    for (size_t k = 0; k < kT2Cell; ++k) put16(t2_id(cell * kT2Cell + k));
    c.push_back(1);
    put32(alive);
    c.push_back(static_cast<uint8_t>(ExportKind::Alive));
    put16(static_cast<uint16_t>(Unit::Count));
    return c;
}

PortableSpec t2_spec(const std::vector<uint8_t>& cfg) {
    PortableSpec s{};
    std::memcpy(s.cfg, cfg.data(), cfg.size());
    s.kind = &kForemanKind;
    s.decl = ActorDecl{kPortableNode, ActorType::Foreman, static_cast<uint8_t>(cfg.size()), s.cfg};
    kForemanKind.placement(s.decl, s.place);
    return s;
}

bool t2_on_call(void* ctx, uint16_t from, uint16_t, uint32_t path, const uint8_t* args, uint16_t len) {
    return static_cast<Reconciler*>(ctx)->on_cast(from, path, args, len);
}

void t2_step(Cell& cell, void*) {
    // Every 5 ms, not every 1: a board's link task runs the reconciler about as often, and a hundred reconcilers a
    // millisecond made a simulated day take most of a real one.
    const uint32_t ms = static_cast<uint32_t>(cell.now_us() / 1000);
    if (ms % 5 != 0) return;
    for (Reconciler* r : g_t2.recs) r->tick(ms);
}

// The runner of cell k's role (its slot 0 on the cell's boards), 0 for none, 0xFFFF for two.
uint16_t t2_runner(Cell& cell, size_t k) {
    uint16_t who = 0;
    for (size_t b = 0; b < 2; ++b) {
        const size_t i = k * kT2Cell + b;
        if (cell.asleep(i)) continue;
        if (g_t2.recs[i]->view(0).running) who = who == 0 ? t2_id(i) : 0xFFFF;
    }
    return who;
}

void t2_second(Cell& cell, uint32_t s) {
    for (size_t k = 0; k < g_t2.cells; ++k) {
        const uint16_t r = t2_runner(cell, k);
        if (s > 20) {  // past the start-up, while the reconcilers settle
            if (r == 0) ++g_t2.no_foreman_s[k];
            if (r == 0xFFFF) ++g_t2.two_foremen_s[k];
        }
        if (r != 0 && r != 0xFFFF) {
            if (g_t2.runner[k] != 0 && g_t2.runner[k] != r) ++g_t2.handovers[k];
            g_t2.runner[k] = r;
        }
    }
    // Every minute, each foreman asks for every sibling's summary (its owner is whoever holds that role).
    if (s % 60 == 30) {
        for (size_t k = 0; k < g_t2.cells; ++k) {
            const uint16_t f = g_t2.runner[k];
            if (f == 0 || f == 0xFFFF) continue;
            Node& n = *cell.node(f - 0x1000).node;
            for (size_t j = 0; j < g_t2.cells; ++j) {
                if (j == k) continue;
                const NsEntry* e = n.ns().find(t2_cell_hash(j, "alive"));
                if (e != nullptr && e->owner_node != 0) n.request_read(e->owner_node, t2_cell_hash(j, "alive"));
            }
        }
    }
    // A foreman killed: cell 1's foreman reads cell 0's summary every second until it is GOOD from the new one.
    if (g_t2.kill_at_s != 0 && s == g_t2.kill_at_s) {
        g_t2.killed = g_t2.runner[0] - 0x1000;
        cell.set_asleep(static_cast<size_t>(g_t2.killed), true);
        g_t2.killed_at_s = s;
        std::printf("  t=%us: cell 0's foreman 0x%04x put to sleep\n", s, static_cast<unsigned>(g_t2.runner[0]));
    }
    if (g_t2.killed >= 0 && g_t2.back_at_s == 0 && g_t2.runner[1] != 0 && g_t2.runner[1] != 0xFFFF) {
        Node& rd = *cell.node(g_t2.runner[1] - 0x1000).node;
        const uint32_t h = t2_cell_hash(0, "alive");
        const NsEntry* e = rd.ns().find(h);
        if (e != nullptr && e->owner_node != 0) {
            Reading r;
            rd.read(h, r);
            uint32_t v = 0;
            if (r.quality == Quality::Good && e->owner_node != t2_id(static_cast<size_t>(g_t2.killed)) &&
                r.value.as_u32(v) && v == kT2Cell - 1 && r.age_ms < 3000) {
                g_t2.back_at_s = s;
                std::printf("  t=%us: cell 1's foreman reads cell 0's summary GOOD (alive %u) from 0x%04x\n", s,
                            static_cast<unsigned>(v), static_cast<unsigned>(e->owner_node));
            }
            rd.request_read(e->owner_node, h);
        }
    }
    if (g_t2.killed >= 0 && s == g_t2.killed_at_s + 120) cell.set_asleep(static_cast<size_t>(g_t2.killed), false);
}

void tree2_setup(Cell& cell, size_t cells, uint32_t kill_at_s) {
    g_t2.cells = cells;
    g_t2.upper = static_cast<uint8_t>(cells + 1);
    g_t2.kill_at_s = kill_at_s;
    g_t2.runner.assign(cells, 0);
    g_t2.handovers.assign(cells, 0);
    g_t2.no_foreman_s.assign(cells, 0);
    g_t2.two_foremen_s.assign(cells, 0);
    std::vector<PortableSpec> roles;
    for (size_t k = 0; k < cells; ++k) roles.push_back(t2_spec(t2_foreman_cfg(k, g_t2.upper)));
    g_t2.specs.resize(cell.size());
    for (size_t i = 0; i < cell.size(); ++i) {
        const size_t k = i / kT2Cell;
        const bool eligible = i % kT2Cell < 2;
        cell.set_spaces(i, (1ull << k) | (eligible ? (1ull << cells) : 0));
        Node& n = *cell.node(i).node;
        n.set_groups(static_cast<uint8_t>(k + 1), 0);
        // the scope a version-5 image gives it: its cell's role, and when it may hold it, every sibling's
        std::vector<PortableSpec>& sp = g_t2.specs[i];
        sp.push_back(roles[k]);
        if (eligible) {
            for (size_t j = 0; j < cells; ++j)
                if (j != k) sp.push_back(roles[j]);
        }
        for (PortableSpec& p : sp) p.decl.cfg = p.cfg;  // each its own copy's
        Reconciler* r = new Reconciler(n);
        r->load(sp.data(), sp.size());
        n.set_call_handler(&t2_on_call, r);
        g_t2.recs.push_back(r);
    }
    cell.set_on_step(&t2_step, nullptr);
    cell.set_on_second(&t2_second);
}

void tree2_start(Cell& cell) {
    for (Reconciler* r : g_t2.recs) r->start(static_cast<uint32_t>(cell.now_us() / 1000));
}

void tree2_report(Cell& cell, double elapsed_s) {
    std::printf("\ntree2: %zu cells of %zu, each in its own radio space, under one upper cell (space %zu)\n",
                g_t2.cells, kT2Cell, g_t2.cells);
    uint32_t handovers = 0, none_s = 0, two_s = 0;
    for (size_t k = 0; k < g_t2.cells; ++k) {
        handovers += g_t2.handovers[k];
        none_s += g_t2.no_foreman_s[k];
        two_s += g_t2.two_foremen_s[k];
    }
    std::printf("  foreman handovers   %u  (%.2f per cell per day)\n", handovers,
                handovers / static_cast<double>(g_t2.cells) / (elapsed_s / 86400.0));
    std::printf("  cell-seconds with no foreman %u, with two %u\n", none_s, two_s);
    double worst_base = 0;
    size_t worst_k = 0;
    for (size_t k = 0; k < g_t2.cells; ++k) {
        if (cell.space_busy_us(k) > worst_base) {
            worst_base = cell.space_busy_us(k);
            worst_k = k;
        }
    }
    const double us = elapsed_s * 1e6;
    std::printf("  airtime, worst base space  %.1f%% (%.1f%% of it CASTs)   upper space (%zu radios)  %.1f%% (%.1f%% CASTs)\n"
                "    against one radio space of 10 boards, or 20: pot_sim --nodes 10|20 (which sends no CASTs)\n",
                worst_base / us * 100.0, cell.space_cast_us(worst_k) / us * 100.0, 2 * g_t2.cells,
                cell.space_busy_us(g_t2.cells) / us * 100.0, cell.space_cast_us(g_t2.cells) / us * 100.0);
    // every foreman, every sibling's summary GOOD from that sibling's foreman
    size_t good = 0, asked = 0;
    for (size_t k = 0; k < g_t2.cells; ++k) {
        const uint16_t f = t2_runner(cell, k);
        if (f == 0 || f == 0xFFFF) continue;
        Node& n = *cell.node(f - 0x1000).node;
        for (size_t j = 0; j < g_t2.cells; ++j) {
            if (j == k) continue;
            ++asked;
            Reading r;
            uint32_t v = 0;
            const NsEntry* e = n.ns().find(t2_cell_hash(j, "alive"));
            if (e != nullptr && e->owner_node == t2_runner(cell, j) && n.read(t2_cell_hash(j, "alive"), r) == NsError::Ok &&
                r.quality != Quality::Unavailable && r.value.as_u32(v) && v == kT2Cell) {
                ++good;
            }
        }
    }
    std::printf("  sibling summaries read by the foremen: %zu of %zu, from the current holder, alive = %zu\n", good,
                asked, kT2Cell);
    if (g_t2.killed >= 0) {
        std::printf("  handover: cell 0's summary GOOD again at cell 1's foreman %s\n",
                    g_t2.back_at_s ? (std::to_string(g_t2.back_at_s - g_t2.killed_at_s) + " s after the kill").c_str()
                                   : "never");
    }
}

}  // namespace potsim
