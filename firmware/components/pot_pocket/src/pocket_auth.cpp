// M26: the pocket page's login, sessions and write checks. The behaviour is specified in pocket_auth.hpp.

#include "pot/pocket_auth.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "monocypher.h"

namespace pot::pocket {

void hash_login(const uint8_t salt[kSaltBytes], const char* password, size_t len, uint8_t out[kHashBytes]) {
    crypto_blake2b_keyed(out, kHashBytes, salt, kSaltBytes, reinterpret_cast<const uint8_t*>(password), len);
}

void to_hex(const uint8_t* in, size_t n, char* out) {
    static const char kDigits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out[2 * i] = kDigits[in[i] >> 4];
        out[2 * i + 1] = kDigits[in[i] & 0xF];
    }
    out[2 * n] = '\0';
}

namespace {
int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}  // namespace

bool from_hex(const char* hex, size_t len, uint8_t* out, size_t n) {
    if (hex == nullptr || len != 2 * n) return false;
    for (size_t i = 0; i < n; ++i) {
        const int hi = hex_digit(hex[2 * i]), lo = hex_digit(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

void Auth::set_login(const uint8_t salt[kSaltBytes], const uint8_t hash[kHashBytes]) {
    std::memcpy(salt_, salt, kSaltBytes);
    std::memcpy(hash_, hash, kHashBytes);
    enabled_ = true;
    fails_ = 0;  // the owner setting it on the console is the way back from a lockout a stranger keeps going
    locked_until_ = 0;
    lock_ms_ = kLockMs;
}

void Auth::clear_login() {
    enabled_ = false;
    std::memset(salt_, 0, sizeof(salt_));
    std::memset(hash_, 0, sizeof(hash_));
    for (Session& s : sessions_) s = Session{};
}

LoginResult Auth::login(const char* password, size_t len, uint32_t now_ms, RandomFn rnd, const Session** out) {
    *out = nullptr;
    if (!enabled_) return LoginResult::Off;
    if (fails_ >= kLockAfter && static_cast<int32_t>(locked_until_ - now_ms) > 0) {
        ++counters_.locked;
        return LoginResult::Locked;
    }
    if (fails_ >= kLockAfter) fails_ = 0;  // the lockout is over -- the next one will be longer
    uint8_t h[kHashBytes];
    hash_login(salt_, password, len > kLoginMax ? kLoginMax : len, h);
    const bool ok = len >= 1 && len <= kLoginMax && crypto_verify32(h, hash_) == 0;
    crypto_wipe(h, sizeof(h));
    if (!ok) {
        ++counters_.bad_passwords;
        if (++fails_ >= kLockAfter) {
            locked_until_ = now_ms + lock_ms_;
            lock_ms_ = lock_ms_ * 2 > kLockMaxMs ? kLockMaxMs : lock_ms_ * 2;  // a stranger guessing gets slower and slower
        }
        return LoginResult::Wrong;
    }
    fails_ = 0;
    lock_ms_ = kLockMs;
    Session* slot = &sessions_[0];
    for (Session& s : sessions_) {
        if (!s.used) {
            slot = &s;
            break;
        }
        if (static_cast<int32_t>(s.last_ms - slot->last_ms) < 0) slot = &s;  // the oldest goes
    }
    slot->used = true;
    rnd(slot->id, kTokenBytes);
    rnd(slot->csrf, kTokenBytes);
    slot->last_ms = now_ms;
    ++counters_.logins;
    *out = slot;
    return LoginResult::Ok;
}

const Session* Auth::session(const char* cookie_hex, size_t len, uint32_t now_ms) {
    uint8_t id[kTokenBytes];
    if (!enabled_ || !from_hex(cookie_hex, len, id, kTokenBytes)) return nullptr;
    for (Session& s : sessions_) {
        if (!s.used) continue;
        if (now_ms - s.last_ms >= kSessionIdleMs) {
            s = Session{};  // ended: idle too long
            continue;
        }
        if (crypto_verify16(id, s.id) == 0) {
            s.last_ms = now_ms;
            return &s;
        }
    }
    return nullptr;
}

void Auth::logout(const Session* s) {
    for (Session& x : sessions_) {
        if (&x == s) x = Session{};
    }
}

bool Auth::csrf_ok(const Session& s, const char* hex, size_t len) {
    uint8_t t[kTokenBytes];
    const bool ok = from_hex(hex, len, t, kTokenBytes) && crypto_verify16(t, s.csrf) == 0;
    if (!ok) ++counters_.bad_csrf;
    return ok;
}

bool Auth::take_write(uint32_t now_ms) {
    if (!bucket_started_) {
        bucket_started_ = true;
        bucket_ms_ = now_ms;
    }
    tokens_ += static_cast<float>(now_ms - bucket_ms_) * 0.002f;  // 2 a second
    if (tokens_ > 4.0f) tokens_ = 4.0f;                           // bursts of 4
    bucket_ms_ = now_ms;
    if (tokens_ < 1.0f) {
        ++counters_.rate_limited;
        return false;
    }
    tokens_ -= 1.0f;
    ++counters_.writes;
    return true;
}

bool parse_value(ValueType type, const char* text, Value& out) {
    if (text == nullptr || text[0] == '\0') return false;
    char* end = nullptr;
    switch (type) {
        case ValueType::Bool:
            if (!std::strcmp(text, "on") || !std::strcmp(text, "true") || !std::strcmp(text, "1")) {
                out = Value::of_bool(true);
                return true;
            }
            if (!std::strcmp(text, "off") || !std::strcmp(text, "false") || !std::strcmp(text, "0")) {
                out = Value::of_bool(false);
                return true;
            }
            return false;
        case ValueType::I32: {
            const long v = std::strtol(text, &end, 10);
            if (*end != '\0' || v < INT32_MIN || v > INT32_MAX) return false;
            out = Value::of_i32(static_cast<int32_t>(v));
            return true;
        }
        case ValueType::U32: {
            if (text[0] == '-') return false;
            const unsigned long long v = std::strtoull(text, &end, 10);
            if (*end != '\0' || v > UINT32_MAX) return false;
            out = Value::of_u32(static_cast<uint32_t>(v));
            return true;
        }
        case ValueType::F32: {
            const float v = std::strtof(text, &end);
            if (*end != '\0' || v != v) return false;  // not a number
            out = Value::of_f32(v);
            return true;
        }
        default:
            return false;  // 64-bit and bytes: not offered on the page
    }
}

bool form_field(const char* body, size_t len, const char* name, char* out, size_t cap) {
    const size_t nl = std::strlen(name);
    size_t i = 0;
    while (i < len) {
        size_t end = i;
        while (end < len && body[end] != '&') ++end;
        if (end - i > nl && std::memcmp(body + i, name, nl) == 0 && body[i + nl] == '=') {
            size_t o = 0;
            for (size_t k = i + nl + 1; k < end; ++k) {
                if (o + 1 >= cap) return false;  // longer than the field may be: refused, never cut
                char c = body[k];
                if (c == '+') {
                    c = ' ';
                } else if (c == '%') {
                    if (k + 2 >= end) return false;  // a % needs two digits after it, inside the field
                    const int hi = hex_digit(body[k + 1]), lo = hex_digit(body[k + 2]);
                    if (hi < 0 || lo < 0) return false;
                    c = static_cast<char>((hi << 4) | lo);
                    k += 2;
                }
                if (static_cast<unsigned char>(c) < 0x20) return false;  // no control characters
                out[o++] = c;
            }
            out[o] = '\0';
            return true;
        }
        i = end + 1;
    }
    return false;
}

}  // namespace pot::pocket
