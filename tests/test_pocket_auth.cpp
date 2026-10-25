// M26: the pocket page accepts writes -- its login, sessions, form tokens, write rate and forms (pot/pocket_auth.hpp,
// pot/pocket_core.hpp). The board half (pot/pocket.hpp) only carries these over ESP-IDF's HTTP server.

#include <cstring>
#include <string>

#include "pot/pocket_auth.hpp"
#include "pot/pocket_core.hpp"
#include "test_harness.hpp"

using namespace pot;
using namespace pot::pocket;

namespace {

uint8_t g_counter = 0;
void counting_random(uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; ++i) out[i] = ++g_counter;  // distinct, repeatable
}

Auth with_login(const char* pw) {
    Auth a;
    uint8_t salt[kSaltBytes] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    uint8_t h[kHashBytes];
    hash_login(salt, pw, std::strlen(pw), h);
    a.set_login(salt, h);
    return a;
}

std::string hex(const uint8_t* b) {
    char out[kTokenHex + 1];
    to_hex(b, kTokenBytes, out);
    return out;
}

}  // namespace

TEST(pocket_auth, writes_are_off_until_a_login_is_set) {
    Auth a;
    const Session* s = nullptr;
    CHECK(a.login("anything", 8, 0, &counting_random, &s) == LoginResult::Off);
    CHECK(s == nullptr);
    CHECK(!a.enabled());
}

TEST(pocket_auth, the_right_password_opens_a_session_the_cookie_finds_it_and_the_token_guards_forms) {
    Auth a = with_login("ship-ahoy");
    const Session* s = nullptr;
    CHECK(a.login("ship-ahoy", 9, 1000, &counting_random, &s) == LoginResult::Ok);
    CHECK(s != nullptr);
    const std::string cookie = hex(s->id), token = hex(s->csrf);
    CHECK(cookie != token);  // two separate secrets: the cookie the browser sends, the token only the page knows
    CHECK(a.session(cookie.c_str(), cookie.size(), 2000) == s);
    CHECK(a.csrf_ok(*s, token.c_str(), token.size()));
    std::string forged = token;
    forged[0] = forged[0] == 'a' ? 'b' : 'a';
    CHECK(!a.csrf_ok(*s, forged.c_str(), forged.size()));  // a cross-site form cannot know it
    CHECK(!a.csrf_ok(*s, "", 0));
    CHECK_EQ(a.counters().bad_csrf, 2u);
    CHECK(a.session("00112233445566778899aabbccddeeff", 32, 2000) == nullptr);  // not a session of this board
    a.logout(s);
    CHECK(a.session(cookie.c_str(), cookie.size(), 2100) == nullptr);
}

TEST(pocket_auth, five_wrong_passwords_lock_the_login_for_a_minute) {
    Auth a = with_login("ship-ahoy");
    const Session* s = nullptr;
    for (int i = 0; i < 5; ++i) CHECK(a.login("wrong", 5, 1000, &counting_random, &s) == LoginResult::Wrong);
    CHECK(a.login("ship-ahoy", 9, 2000, &counting_random, &s) == LoginResult::Locked);  // even the right one, now
    CHECK(a.login("ship-ahoy", 9, 1000 + kLockMs + 1, &counting_random, &s) == LoginResult::Ok);
    CHECK_EQ(a.counters().bad_passwords, 5u);
    CHECK_EQ(a.counters().locked, 1u);
    CHECK(a.login("", 0, 70000 + kLockMs, &counting_random, &s) == LoginResult::Wrong);  // empty is never right
}

TEST(pocket_auth, a_session_ends_after_fifteen_idle_minutes_and_the_oldest_gives_way) {
    Auth a = with_login("pw");
    const Session* s = nullptr;
    CHECK(a.login("pw", 2, 0, &counting_random, &s) == LoginResult::Ok);
    const std::string first = hex(s->id);
    CHECK(a.session(first.c_str(), first.size(), kSessionIdleMs - 1) != nullptr);  // touched: idle from here
    CHECK(a.session(first.c_str(), first.size(), 2 * kSessionIdleMs) == nullptr);
    for (int i = 0; i < 5; ++i) CHECK(a.login("pw", 2, 3 * kSessionIdleMs + i, &counting_random, &s) == LoginResult::Ok);
    CHECK(a.state_bytes() <= 4096);  // M26's kill line: login and token state within 4 KB
}

TEST(pocket_auth, writes_go_at_the_hubs_rate_two_a_second_bursts_of_four) {
    Auth a = with_login("pw");
    int ok = 0;
    for (int i = 0; i < 10; ++i) ok += a.take_write(1000) ? 1 : 0;
    CHECK_EQ(ok, 4);
    CHECK(!a.take_write(1100));
    CHECK(a.take_write(1500));  // a second's half: one more
    CHECK_EQ(a.counters().rate_limited, 7u);
}

TEST(pocket_auth, values_are_parsed_by_their_type_and_form_fields_decoded) {
    Value v;
    CHECK(parse_value(ValueType::Bool, "on", v));
    bool b = false;
    CHECK(v.as_bool(b) && b);
    CHECK(parse_value(ValueType::Bool, "0", v) && v.as_bool(b) && !b);
    CHECK(!parse_value(ValueType::Bool, "maybe", v));
    uint32_t u = 0;
    CHECK(parse_value(ValueType::U32, "42", v) && v.as_u32(u) && u == 42);
    CHECK(!parse_value(ValueType::U32, "-1", v));
    CHECK(!parse_value(ValueType::U32, "4x", v));
    float f = 0;
    CHECK(parse_value(ValueType::F32, "21.5", v) && v.as_f32(f) && f > 21.4f && f < 21.6f);
    CHECK(!parse_value(ValueType::U64, "1", v));  // not offered on the page

    const char body[] = "name=node-7368%2Fled%2Fon&value=on&csrf=ab+cd";
    char out[48];
    CHECK(form_field(body, sizeof(body) - 1, "name", out, sizeof(out)));
    CHECK_EQ(std::string(out), std::string("node-7368/led/on"));
    CHECK(form_field(body, sizeof(body) - 1, "csrf", out, sizeof(out)));
    CHECK_EQ(std::string(out), std::string("ab cd"));
    CHECK(!form_field(body, sizeof(body) - 1, "nope", out, sizeof(out)));
    CHECK(!form_field(body, sizeof(body) - 1, "name", out, 8));  // longer than allowed: refused, never cut
    const char evil[] = "value=a%0Ab";
    CHECK(!form_field(evil, sizeof(evil) - 1, "value", out, sizeof(out)));  // no control characters
    const char cut[] = "value=a%4";
    CHECK(!form_field(cut, sizeof(cut) - 1, "value", out, sizeof(out)));
}

TEST(pocket_auth, the_page_offers_a_login_then_forms_only_for_writable_values) {
    Snapshot s;
    std::strcpy(s.system, "lab");
    s.n_items = 2;
    std::strcpy(s.items[0].name, "node-7368/led/on");
    s.items[0].writable = true;
    std::strcpy(s.items[1].name, "node-7368/hw/die_temp");
    static char page[kPageMax];
    PageView v;
    CHECK(render_html(s, v, page, sizeof(page)) > 0);
    CHECK(std::strstr(page, "read-only") != nullptr);  // writes off: no login, no forms
    CHECK(std::strstr(page, "/login") == nullptr);
    v.writes_on = true;
    CHECK(render_html(s, v, page, sizeof(page)) > 0);
    CHECK(std::strstr(page, "action=\"/login\"") != nullptr);
    CHECK(std::strstr(page, "action=\"/write\"") == nullptr);  // not logged in: no write forms
    v.logged_in = true;
    std::strcpy(v.csrf, "0123456789abcdef0123456789abcdef");
    CHECK(render_html(s, v, page, sizeof(page)) > 0);
    const char* first = std::strstr(page, "action=\"/write\"");
    CHECK(first != nullptr);
    CHECK(std::strstr(first + 1, "action=\"/write\"") == nullptr);  // one form: the LED, not the temperature
    CHECK(std::strstr(page, "0123456789abcdef0123456789abcdef") != nullptr);
    CHECK(std::strstr(page, "http-equiv=\"refresh\"") == nullptr);  // a form being typed into is not reloaded
}
