// The test kit's main(): runs every TEST the executable holds, or those whose suite or name contains argv[1].
// The same loop as Potluck's own suite (tests/test_main.cpp), without its fixture generators.

#include <cstdio>
#include <cstring>

#include "test_harness.hpp"

namespace tst {

int run_all(int argc, char** argv) {
    const char* filter = (argc > 1) ? argv[1] : nullptr;
    const char* last_suite = "";
    int ran = 0;
    for (const Case& c : registry()) {
        if (filter != nullptr && std::strstr(c.suite, filter) == nullptr && std::strstr(c.name, filter) == nullptr) {
            continue;
        }
        if (std::strcmp(last_suite, c.suite) != 0) {
            std::printf("\n[%s]\n", c.suite);
            last_suite = c.suite;
        }
        current_case() = c.name;
        const int before = failures();
        c.fn();
        ++ran;
        std::printf("  %-4s %s\n", failures() == before ? "ok" : "FAIL", c.name);
        std::fflush(stdout);
    }
    std::printf("\n%d case(s), %d check(s), %d failure(s)\n%s\n", ran, checks(), failures(),
                failures() == 0 ? "PASS" : "FAILED");
    return failures() == 0 ? 0 : 1;
}

}  // namespace tst

int main(int argc, char** argv) { return tst::run_all(argc, argv); }
