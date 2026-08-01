// A test harness in one header.
//
// No GoogleTest, no Catch2, no fetch at configure time. The portable half of Potluck has zero
// dependencies and its tests should be runnable on any machine with a C++17 compiler and nothing
// else — including one with no network, which is the machine this was written on. Adding a test
// framework would be the first dependency in the project and it would buy a nicer failure message.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace tst {

struct Case {
    const char* suite;
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

inline int& failures() {
    static int f = 0;
    return f;
}

inline int& checks() {
    static int c = 0;
    return c;
}

inline const char*& current_case() {
    static const char* c = "";
    return c;
}

struct Registrar {
    Registrar(const char* suite, const char* name, void (*fn)()) {
        registry().push_back(Case{suite, name, fn});
    }
};

inline void fail(const char* file, int line, const std::string& msg) {
    ++failures();
    std::printf("  FAIL %s\n    %s:%d\n    %s\n", current_case(), file, line, msg.c_str());
}

inline std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 3);
    for (size_t i = 0; i < n; ++i) {
        if (i) s.push_back(' ');
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 0xF]);
    }
    return s;
}

template <typename T>
inline std::string to_str(const T& v) {
    return std::to_string(v);
}
inline std::string to_str(const char* v) { return v ? std::string(v) : std::string("(null)"); }
inline std::string to_str(const std::string& v) { return v; }
inline std::string to_str(bool v) { return v ? "true" : "false"; }

int run_all(int argc, char** argv);

}  // namespace tst

#define TST_CONCAT_(a, b) a##b
#define TST_CONCAT(a, b) TST_CONCAT_(a, b)

#define TEST(suite, name)                                                       \
    static void TST_CONCAT(tst_fn_, __LINE__)();                                \
    static ::tst::Registrar TST_CONCAT(tst_reg_, __LINE__)(#suite, #name,       \
                                                           &TST_CONCAT(tst_fn_, __LINE__)); \
    static void TST_CONCAT(tst_fn_, __LINE__)()

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++::tst::checks();                                                     \
        if (!(cond)) {                                                         \
            ::tst::fail(__FILE__, __LINE__, "CHECK(" #cond ") is false");      \
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                                       \
    do {                                                                                     \
        ++::tst::checks();                                                                   \
        const auto tst_a_ = (a);                                                             \
        const auto tst_b_ = (b);                                                             \
        if (!(tst_a_ == tst_b_)) {                                                           \
            ::tst::fail(__FILE__, __LINE__,                                                  \
                        std::string(#a " == " #b "\n      lhs = ") + ::tst::to_str(tst_a_) + \
                            "\n      rhs = " + ::tst::to_str(tst_b_));                       \
        }                                                                                    \
    } while (0)

#define CHECK_STR_EQ(a, b)                                                                 \
    do {                                                                                   \
        ++::tst::checks();                                                                 \
        const std::string tst_a_ = ::tst::to_str(a);                                       \
        const std::string tst_b_ = ::tst::to_str(b);                                       \
        if (tst_a_ != tst_b_) {                                                            \
            ::tst::fail(__FILE__, __LINE__, std::string(#a " == " #b "\n      lhs = \"") + \
                                                tst_a_ + "\"\n      rhs = \"" + tst_b_ + "\""); \
        }                                                                                  \
    } while (0)

// Compare raw bytes and print both sides in hex on failure — for a project whose product is a
// wire format, this is the assertion that matters most.
#define CHECK_BYTES_EQ(got, got_len, want, want_len)                                          \
    do {                                                                                      \
        ++::tst::checks();                                                                    \
        const size_t tst_gl_ = (got_len);                                                     \
        const size_t tst_wl_ = (want_len);                                                    \
        const uint8_t* tst_g_ = (got);                                                        \
        const uint8_t* tst_w_ = (want);                                                       \
        if (tst_gl_ != tst_wl_ || std::memcmp(tst_g_, tst_w_, tst_wl_) != 0) {                \
            ::tst::fail(__FILE__, __LINE__,                                                   \
                        std::string("bytes differ\n      got  (") + std::to_string(tst_gl_) + \
                            ") = " + ::tst::hex(tst_g_, tst_gl_) + "\n      want (" +         \
                            std::to_string(tst_wl_) + ") = " + ::tst::hex(tst_w_, tst_wl_));  \
        }                                                                                     \
    } while (0)
