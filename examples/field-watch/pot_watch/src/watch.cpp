// pot_watch: the field watch. The behaviour is specified in watch/watch.hpp.

#include "watch/watch.hpp"

#include <cstdio>
#include <cstring>
#include <new>

namespace pot {
namespace watch {

namespace {
template <typename T>
T get(const uint8_t*& p) {
    T v;
    std::memcpy(&v, p, sizeof(T));  // little-endian, as the host packs it and as the S3 is
    p += sizeof(T);
    return v;
}

// Any number an input may publish -- a presence bool, a motion or level f32, a count -- as a float.
bool as_float(const Value& v, float& out) {
    float f = 0;
    int32_t i = 0;
    uint32_t u = 0;
    bool b = false;
    if (v.as_f32(f)) {
        out = f;
    } else if (v.as_bool(b)) {
        out = b ? 1.0f : 0.0f;
    } else if (v.as_i32(i)) {
        out = static_cast<float>(i);
    } else if (v.as_u32(u)) {
        out = static_cast<float>(u);
    } else {
        return false;
    }
    return true;
}
}  // namespace

bool parse(const ActorDecl& d, Config& out) {
    if (d.type != kWatchType || d.node_id != kPortableNode) return false;
    size_t len = 0;
    if (!portable_header(d, out.place, len) || d.cfg_len != len + kOwnCfgLen) return false;
    const uint8_t* p = d.cfg + len;
    const uint32_t outputs = get<uint32_t>(p);
    out.presence = get<uint32_t>(p);
    out.presence_min = get<float>(p);
    out.motion = get<uint32_t>(p);
    out.motion_b = get<uint32_t>(p);
    out.motion_min = get<float>(p);
    out.sound = get<uint32_t>(p);
    out.sound_min = get<float>(p);
    out.hold_ms = get<uint16_t>(p);
    out.led = get<uint8_t>(p) != 0;
    out.agree_hash = path_hash_from(outputs, "agree");
    out.missing_hash = path_hash_from(outputs, "missing");
    // Three distinct inputs, none of them the watch's own outputs; a hold of at least a period.
    const uint32_t own[] = {out.place.out_hash, out.agree_hash, out.missing_hash};
    const uint32_t inputs[] = {out.presence, out.motion, out.motion_b, out.sound};
    for (uint32_t in : inputs) {
        if (in == 0) return false;
        for (uint32_t o : own) {
            if (in == o) return false;
        }
    }
    if (out.presence == out.motion || out.presence == out.sound || out.motion == out.sound) return false;
    return out.hold_ms >= out.place.period_ms;
}

WatchActor::~WatchActor() { led(false); }  // moved away or stopped: this board's LED is not the watch's any more

bool WatchActor::start(uint32_t now_ms) {
    next_ms_ = now_ms;
    return true;  // the reconciler declares the outputs and ticks this only while it owns them
}

bool WatchActor::sample(uint32_t hash, float& out) {
    Reading r;
    bool local = true;
    if (node_.read(hash, r, &local) != NsError::Ok) return false;  // not declared yet: treated as NO_DATA
    if (!local) {
        const NsEntry* e = node_.ns().find(hash);
        if (e != nullptr && e->owner_node != 0) node_.request_read(e->owner_node, hash);  // fresh by the next period
    }
    return r.quality == Quality::Good && as_float(r.value, out);
}

void WatchActor::led(bool on) {
    if (board_ == nullptr || (!on && !led_on_)) return;
    led_on_ = on;
    const uint8_t g = on ? 0xFF : 0, b = on ? 0xFF : 0;  // cyan
    const uint16_t blink = on ? kLedBlinkMs : 0;
    if (board_->led_claim != nullptr) {
        board_->led_claim(board_->ctx, kLedSlot, on, 0, g, b, blink);
    } else if (board_->led_app != nullptr) {
        board_->led_app(board_->ctx, on, 0, g, b, blink);
    }
}

void WatchActor::tick(uint32_t now) {
    if (static_cast<int32_t>(now - next_ms_) < 0) return;
    next_ms_ = now + cfg_.place.period_ms;

    float v[3] = {};
    bool good[3] = {};
    good[0] = sample(cfg_.presence, v[0]);
    float a = 0, b = 0;
    const bool ga = sample(cfg_.motion, a);
    const bool gb = cfg_.motion_b != cfg_.motion && sample(cfg_.motion_b, b);
    good[1] = ga || gb;
    v[1] = ga && gb ? (a > b ? a : b) : (ga ? a : b);
    good[2] = sample(cfg_.sound, v[2]);
    const float min[3] = {cfg_.presence_min, cfg_.motion_min, cfg_.sound_min};

    uint32_t agree = 0, missing = 0;
    for (int i = 0; i < 3; ++i) {
        const uint32_t bit = 1u << i;
        if (!good[i]) {
            missing |= bit;
            seen_[i] = false;  // an input that went away does not hold the alarm up
            continue;
        }
        if (v[i] >= min[i]) {
            last_on_[i] = now;
            seen_[i] = true;
        }
        if (seen_[i] && now - last_on_[i] < cfg_.hold_ms) agree |= bit;
    }
    const bool alarm = (agree & kPresence ? 1 : 0) + (agree & kMotion ? 1 : 0) + (agree & kSound ? 1 : 0) >= 2;
    if (alarm && !alarm_) ++alarms_;
    const bool changed = !published_ || alarm != alarm_ || agree != agree_ || missing != missing_;
    alarm_ = alarm, agree_ = agree, missing_ = missing;
    if (changed || now - published_at_ >= kRepublishMs) {
        // One set, one timestamp (actor.hpp's note on publishing a set).
        node_.ns().publish(cfg_.place.out_hash, Value::of_bool(alarm), now);
        node_.ns().publish(cfg_.agree_hash, Value::of_u32(agree), now);
        node_.ns().publish(cfg_.missing_hash, Value::of_u32(missing), now);
        published_ = true;
        published_at_ = now;
    }
    if (cfg_.led) led(alarm);  // called every period while on: an app's LED lapses 5 s after the last call
}

size_t WatchActor::stats_json(char* buf, size_t cap, uint32_t) {
    const int n = std::snprintf(buf, cap,
                                "{\"t\":\"watch\",\"node\":%u,\"alarm\":%d,\"agree\":%u,\"missing\":%u,\"alarms\":%u}",
                                static_cast<unsigned>(node_.config().node_id), alarm_ ? 1 : 0,
                                static_cast<unsigned>(agree_), static_cast<unsigned>(missing_),
                                static_cast<unsigned>(alarms_));
    return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
}

namespace {
bool watch_check(const ActorDecl& d, const ActorEnv&, const char** why) {
    Config c;
    if (!parse(d, c)) {
        *why = "field watch: portable, three distinct inputs, hold_ms at least the period";
        return false;
    }
    return true;
}

Actor* watch_create(void* mem, const ActorDecl& d, const ActorEnv& env) {
    static_assert(sizeof(WatchActor) <= kActorSlotBytes, "WatchActor outgrew its actor slot");
    Config c;
    if (env.node == nullptr || !parse(d, c)) return nullptr;
    return new (mem) WatchActor(*env.node, env.board, c);
}

bool watch_placement(const ActorDecl& d, TickerConfig& out) {
    Config c;
    if (!parse(d, c)) return false;
    out = c.place;
    return true;
}

size_t watch_outputs(const ActorDecl& d, uint16_t node, uint16_t peer, NsDecl* out, size_t cap) {
    Config c;
    if (node != 0 || peer != 0 || cap < 3 || !parse(d, c)) return 0;
    const uint32_t hashes[3] = {c.place.out_hash, c.agree_hash, c.missing_hash};
    const ValueType types[3] = {ValueType::Bool, ValueType::U32, ValueType::U32};
    for (size_t i = 0; i < 3; ++i) {
        out[i] = NsDecl{};
        out[i].path_hash = hashes[i];
        out[i].owner_node = 0;  // node-free: the reconciler moves ownership with the claims
        out[i].type = types[i];
        out[i].unit = Unit::None;
        out[i].kind = ResourceKind::Sampled;
        out[i].access = Access::Read;
        out[i].latency_class = kClassL4;
        out[i].staleness_bound_ms = 5u * c.place.period_ms;  // Potluck's convention for a portable output
        out[i].staleness_policy = StalenessPolicy::Informative;
    }
    return 3;
}
}  // namespace

ActorKind watch_row() {
    return {kWatchType, "watch", &watch_check, &watch_create, &watch_placement, &watch_outputs};
}

}  // namespace watch

const ActorKind* app_rows_pot_watch(size_t* n) {
    static const ActorKind kRows[] = {watch::watch_row()};
    *n = sizeof(kRows) / sizeof(kRows[0]);
    return kRows;
}

}  // namespace pot
