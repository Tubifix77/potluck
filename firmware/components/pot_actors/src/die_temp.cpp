// M8.1: the die-temperature actor. See die_temp.hpp.

#include "pot/die_temp.hpp"

#include <cstdio>
#include <cstring>

namespace pot {

bool DieTempActor::start(uint32_t now_ms) {
    NsDecl d;
    d.path_hash = cfg_.out_hash;
    d.owner_node = node_.config().node_id;
    d.type = ValueType::F32;
    d.unit = Unit::Celsius;
    d.kind = ResourceKind::Sampled;
    d.access = Access::Read;
    d.latency_class = kClassL4;  // it moves over seconds; nothing binds to it tightly
    // Three periods: one late sample is not staleness, a stopped actor soon is.
    d.staleness_bound_ms = 3u * cfg_.period_ms;
    d.staleness_policy = StalenessPolicy::Informative;
    if (node_.ns().declare(d) != NsError::Ok) return false;
    open_ = drv_.open != nullptr && drv_.open(drv_.ctx);
    next_ms_ = now_ms;
    return true;
}

void DieTempActor::tick(uint32_t now) {
    if (static_cast<int32_t>(now - next_ms_) < 0) return;
    next_ms_ = now + cfg_.period_ms;
    if (!open_) open_ = drv_.open != nullptr && drv_.open(drv_.ctx);
    float c = 0.0f;
    if (open_ && !forced_fail_ && drv_.read != nullptr && drv_.read(drv_.ctx, &c)) {
        if (node_.publish(cfg_.out_hash, Value::of_f32(c)) == NsError::Ok) {
            ++stats_.reads;
            stats_.last_c = c;
        }
        return;
    }
    // The owner knows its sensor failed: say so, rather than let the last number age as if it
    // still meant something.
    ++stats_.errors;
    node_.publish_faulty(cfg_.out_hash);
}

bool DieTempActor::on_console(const char* line, size_t len) {
    static const char kCmd[] = "POT! die_temp fail ";
    constexpr size_t kLen = sizeof(kCmd) - 1;
    if (len <= kLen || std::strncmp(line, kCmd, kLen) != 0) return false;
    forced_fail_ = line[kLen] == '1';
    std::printf("{\"t\":\"test\",\"cmd\":\"die_temp_fail\",\"fail\":%d}\n", forced_fail_ ? 1 : 0);
    return true;
}

size_t DieTempActor::stats_json(char* buf, size_t cap, uint32_t now_ms) {
    Reading r;
    node_.read(cfg_.out_hash, r);
    const int n = std::snprintf(buf, cap,
                                "{\"t\":\"die_temp\",\"node\":%u,\"up_ms\":%u,\"celsius\":%.2f,\"reads\":%u,"
                                "\"errors\":%u,\"open\":%d,\"forced_fail\":%d,\"quality\":\"%s\",\"age_ms\":%u}",
                                static_cast<unsigned>(node_.config().node_id), static_cast<unsigned>(now_ms),
                                static_cast<double>(stats_.last_c), static_cast<unsigned>(stats_.reads),
                                static_cast<unsigned>(stats_.errors), open_ ? 1 : 0, forced_fail_ ? 1 : 0,
                                quality_str(r.quality), static_cast<unsigned>(r.age_ms));
    return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
}

bool die_temp_check(const ActorDecl& d, const ActorEnv&, const char** why) {
    DieTempConfig c{};
    if (!die_temp_config(d, c)) {
        *why = "die_temp config invalid";
        return false;
    }
    return true;
}

}  // namespace pot
