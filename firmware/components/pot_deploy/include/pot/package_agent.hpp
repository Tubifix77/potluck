// A board's half of moving packages through the cell (sections 7.4 and M12): the deploy server for DEPLOY_*, the
// push that passes a committed package on to the radio peers, and catch-up -- a package reaching a board that was
// off, or across a partition, when it was deployed. Until 2026-10-14 this lived in firmware/main/m0_main.cpp, where
// no host test could reach it; it moved here so a partitioned cell could be tested with the code the boards run
// (tests/test_deploy.cpp, [deploy_partition]). The firmware keeps what is the chip's: applying a package to the
// running actors, the trial's heartbeat count, and the reboot.
//
// CATCH-UP. Every kPkgAnnounceMs, and kPkgSoonMs after a peer appears or reboots, a board running a confirmed package
// tells its radio peers which (CAST "potluck:sys/package/v1", kind 1). A board whose highest committed counter is
// lower asks that peer for it (kind 2), at most once per kPkgAskCooldownMs; the peer, when idle, sends the signed
// stream from its slot exactly as a deploy would (BEGIN, CHUNKs, COMMIT), and the receiver checks it and takes it on
// trial as if the host had sent it.
//
//   announce: u8 kind (1), u32 counter, u8[8] digest   (the digest since 2026-10-14; before, 5 bytes)
//   ask:      u8 kind (2), u32 counter
//
// ONE COUNTER, TWO PACKAGES (found 2026-10-14 by the partition tests). Two deploys signed at the same counter --
// two PCs on two sides of a partition, each holding its side's deploy lease -- used to leave the cell mixed for
// good: each side's counter equalled the other's, so nobody asked. Now the higher digest (bytewise, the image
// header's) wins at one counter, everywhere the same way: a board running its floor's counter asks a peer announcing
// that counter with a higher digest, and refuses, at COMMIT, a package at its floor with a lower one. A board whose
// trial reverted runs a counter below its floor and asks for nothing at the floor, so a package that crashed it is not
// fetched again. A board before this change ignores the digest (it reads the first 5 bytes) and serves an ask as
// before, so a mixed-firmware cell still converges through the new boards.
//
// AN ASK WHILE BUSY (found the same day). Every board behind hears the same announces at the same moment after a heal
// and asks the same peer; a peer that is busy passing the package to one of them used to drop the rest, which then
// waited out their cooldown and the next announce -- a 4+4 split took 195 s to converge in the simulation. A busy peer
// now queues an ask (at most one per asker, dropped after kPkgAskCooldownMs, when the asker asks again anyway) and serves
// it when it is free.
//
// A RECEIVE NOBODY FINISHES (found the same day). A push cut off mid-transfer -- the sender lost behind a partition,
// or reset -- left the receiver "in progress" until the next BEGIN, and a board in that state neither asks for a
// package nor serves one, so it could miss the newer package for good. A receive with no step for kRxStaleMs is
// abandoned.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/deploy.hpp"
#include "pot/link_stats.hpp"
#include "pot/namespace.hpp"

namespace pot {

class Node;

constexpr uint32_t kPkgAnnounceMs = 60000;
constexpr uint32_t kPkgFirstAnnounceMs = 30000;  // after boot
constexpr uint32_t kPkgSoonMs = 5000;            // after a peer appears: long enough for its trust to settle
constexpr uint32_t kPkgAskCooldownMs = 45000;
constexpr uint32_t kRxStaleMs = 15000;  // a step every few ms on a live transfer; the host's steps time out at 3 s
constexpr uint8_t kPkgAnnounce = 1, kPkgAsk = 2;
constexpr size_t kPkgAnnounceLen = 13;  // with the digest
constexpr uint32_t kPkgPath = path_hash("potluck:sys/package/v1");

struct PackageAgentHooks {
    void* ctx = nullptr;
    // A COMMIT stored a package and put it on trial; the agent's state was reloaded from the store. Take it: return
    // true if it runs now (live), false to reboot into it. Set *refused (not Ok) when it cannot run at all -- the hook
    // has reverted the state -- and the commit is answered with that status.
    bool (*committed)(void* ctx, DeployStatus* refused) = nullptr;
    // Something else needs the deploy path to itself (M11's firmware transfer, on trial or committed).
    bool (*busy)(void* ctx) = nullptr;
    // The flash behind the store is up.
    bool (*ready)(void* ctx) = nullptr;
    // One line for the log.
    void (*log)(void* ctx, const char* line) = nullptr;
};

class PackageAgent {
  public:
    // `state` is the board's A/B state (the firmware's, persisted by it); `rx` and its buffer receive packages and
    // hold the one being passed on; `host_mac` is the cable's, never announced to.
    PackageAgent(Node& node, SlotStore& store, DeployReceiver& rx, uint8_t* buf, size_t cap, DeployState& state,
                 const PackageAgentHooks& hooks, const uint8_t* host_mac);

    // DEPLOY_BEGIN/CHUNK/COMMIT/ABORT, from the node's deploy server. Returns the REPLY's length, 0 when the answer
    // comes later (a COMMIT being passed on).
    size_t serve(uint16_t from, uint16_t msg_id, uint8_t op, const uint8_t* p, uint16_t len, uint8_t* reply,
                 size_t cap, uint32_t now);
    // The answer to one of this board's own DEPLOY_* steps; false if it was not the push's.
    bool on_result(uint16_t peer, uint16_t msg, uint8_t op, bool timed_out, const uint8_t* p, uint16_t len,
                   uint32_t now);
    // A CAST; false if it was not the package path's.
    bool on_cast(uint16_t from, uint32_t path, const uint8_t* args, uint16_t len, uint32_t now);
    // Every pass: announce when due, abandon a stale receive.
    void tick(uint32_t now);
    // A peer appeared or rebooted: announce soon.
    void soon(uint32_t now);

    // What runs here (0: the built-in behaviour). The firmware sets it whenever a package is applied.
    void set_running(uint32_t counter, const uint8_t digest[8]);
    uint32_t running_counter() const { return running_counter_; }
    const uint8_t* running_digest() const { return running_digest_; }

    bool pushing() const { return push_.active; }
    void schedule_reboot(uint32_t now);
    bool reboot_scheduled() const { return reboot_at_ms_ != 0; }
    bool reboot_due(uint32_t now) const { return reboot_at_ms_ != 0 && static_cast<int32_t>(now - reboot_at_ms_) >= 0; }
    bool idle() const;

    struct Stats {
        uint32_t announces = 0;
        uint32_t asked = 0;          // asks sent
        uint32_t asked_tie = 0;      // ... of them for a higher digest at this board's counter
        uint32_t served = 0;         // catch-ups started
        uint32_t queued = 0;         // asks that arrived while busy, kept for later
        uint32_t refused_lower = 0;  // a COMMIT at this board's counter with a lower digest
        uint32_t rx_abandoned = 0;   // receives nobody finished
    };
    const Stats& stats() const { return stats_; }

  private:
    struct Push {
        bool active = false;
        uint16_t peers[kMaxPeers];
        size_t count = 0;
        size_t idx = 0;
        uint32_t offset = 0;
        uint16_t awaiting = 0;
        uint8_t ok = 0;
        uint8_t failed = 0;
        uint16_t requester = 0;
        uint16_t requester_msg = 0;
        bool catchup = false;  // this node's own package, sent to a peer that was off; nobody to answer
        DeployBegin begin{};
    };

    void logf(const char* fmt, ...);
    void push_send(uint8_t op, const uint8_t* payload, size_t len, uint32_t now);
    void push_begin(uint32_t now);
    void push_chunk(uint32_t now);
    void push_finish(uint32_t now);
    void push_next_peer(uint32_t now);
    void announce(uint32_t now);
    void catchup_start(uint16_t peer, uint32_t now);
    bool lower_at_floor(const uint8_t digest[8]) const;

    Node& node_;
    SlotStore& store_;
    DeployReceiver& rx_;
    uint8_t* buf_;
    size_t cap_;
    DeployState& state_;
    PackageAgentHooks hooks_;
    uint8_t host_mac_[6];
    Push push_;
    bool live_ = false;  // the commit being answered was applied live: no reboot after the push
    uint32_t reboot_at_ms_ = 0;
    uint32_t running_counter_ = 0;
    uint8_t running_digest_[8] = {};
    uint32_t next_announce_ms_ = kPkgFirstAnnounceMs;
    uint32_t last_ask_ms_ = 0;
    bool asked_ever_ = false;
    uint32_t rx_last_ms_ = 0;
    struct Ask {
        uint16_t peer;
        uint32_t counter;
        uint32_t at_ms;
    };
    Ask asks_[kMaxPeers] = {};  // waiting while this board was busy, oldest first
    size_t asks_n_ = 0;
    Stats stats_;
};

}  // namespace pot
