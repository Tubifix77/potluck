// M26: the pocket page accepts writes -- the portable half of its login, sessions and write checks.
//
// Off by default: a board takes no write until the owner sets the page's login password on its console (`POT! pocket
// login <password>`), and only a hash of it is kept (BLAKE2b keyed with a random salt, Monocypher). A phone
// that logs in gets a session: a random id in a cookie (HttpOnly, SameSite=Strict) and a separate random token in the
// page's forms; a write must carry both (the token is what a cross-site form cannot know). Five wrong passwords lock the
// login for a minute, and each lockout after that twice as long (up to 16 minutes) until a right password; a session
// ends after 15 minutes without a request. Writes share the hub's limit: 2 a second, bursts
// of 4. Which resources may be written is the package's outside section (writable, never safety-relevant), checked again
// by the board that owns the resource, as every outside write is.
//
// All state is small and static: kMaxSessions sessions of 40 bytes, a few counters -- M26's kill line is 4 KB of a board's
// internal heap. Portable: no ESP-IDF, no heap; the board half (pot/pocket.hpp) supplies randomness and the clock.

#pragma once

#include <cstddef>
#include <cstdint>

#include "pot/value.hpp"

namespace pot::pocket {

constexpr size_t kTokenBytes = 16;
constexpr size_t kTokenHex = kTokenBytes * 2;
constexpr size_t kSaltBytes = 16;
constexpr size_t kHashBytes = 32;
constexpr size_t kMaxSessions = 4;
constexpr uint32_t kSessionIdleMs = 15u * 60u * 1000u;
constexpr uint32_t kLockAfter = 5;
constexpr uint32_t kLockMs = 60000;      // the first lockout; each one after it doubles, to kLockMaxMs
constexpr uint32_t kLockMaxMs = 16u * 60u * 1000u;
constexpr size_t kLoginMin = 12;  // security review 2026-10-26: guessable at the lockout's pace below this
constexpr size_t kLoginMax = 63;

using RandomFn = void (*)(uint8_t* out, size_t n);

struct Session {
    bool used = false;
    uint8_t id[kTokenBytes] = {};
    uint8_t csrf[kTokenBytes] = {};
    uint32_t last_ms = 0;
};

struct AuthCounters {
    uint32_t logins = 0, bad_passwords = 0, locked = 0, bad_session = 0, bad_csrf = 0, bad_origin = 0, rate_limited = 0,
             writes = 0;
};

enum class LoginResult : uint8_t { Ok, Wrong, Locked, Off };

class Auth {
  public:
    // The saved login: salt and hash, or none (writes off).
    void set_login(const uint8_t salt[kSaltBytes], const uint8_t hash[kHashBytes]);
    void clear_login();
    bool enabled() const { return enabled_; }

    // A password tried: on Ok, `*out` is the new session (the oldest replaced when all are taken).
    LoginResult login(const char* password, size_t len, uint32_t now_ms, RandomFn rnd, const Session** out);
    // The session a cookie value names (32 hex digits), if it is live; touches it.
    const Session* session(const char* cookie_hex, size_t len, uint32_t now_ms);
    void logout(const Session* s);
    // A form's token against its session's, in constant time.
    bool csrf_ok(const Session& s, const char* hex, size_t len);
    // One write, at the hub's rate (2 a second, bursts of 4). False: over it.
    bool take_write(uint32_t now_ms);

    AuthCounters& counters() { return counters_; }
    static constexpr size_t state_bytes() { return sizeof(Session) * kMaxSessions + kSaltBytes + kHashBytes + 32; }

  private:
    bool enabled_ = false;
    uint8_t salt_[kSaltBytes] = {};
    uint8_t hash_[kHashBytes] = {};
    Session sessions_[kMaxSessions];
    uint32_t fails_ = 0, locked_until_ = 0, lock_ms_ = kLockMs;
    float tokens_ = 4.0f;
    uint32_t bucket_ms_ = 0;
    bool bucket_started_ = false;
    AuthCounters counters_{};
};

// BLAKE2b-256 of the password, keyed with the salt: what is kept instead of the password.
void hash_login(const uint8_t salt[kSaltBytes], const char* password, size_t len, uint8_t out[kHashBytes]);
// Bytes to lowercase hex (out holds 2n + 1) and back (false on a bad digit or length).
void to_hex(const uint8_t* in, size_t n, char* out);
bool from_hex(const char* hex, size_t len, uint8_t* out, size_t n);
// A form's value for a resource of `type`: bool as on/off/true/false/1/0, whole numbers, decimals. False if it is not
// one of those for that type.
bool parse_value(ValueType type, const char* text, Value& out);
// One field of an application/x-www-form-urlencoded body, decoded (+ and %XX), into out (cap includes the NUL).
bool form_field(const char* body, size_t len, const char* name, char* out, size_t cap);

}  // namespace pot::pocket
