#include "test_harness.hpp"

#include <cstring>

namespace pot_test_fixtures {
// Defined in test_stats_json.cpp: writes one of each serial record to <dir>/stats_sample.jsonl so
// the Python tests in host/potluck parse exactly the bytes a board emits.
int emit(const char* dir);
}  // namespace pot_test_fixtures

namespace pot_fuzz_corpus {
// Defined in test_frame_fuzz.cpp: writes every fuzz input plus this parser's verdict to
// <dir>/fuzz_corpus.jsonl, for the differential test against the Python decoder.
int emit(const char* dir);
}  // namespace pot_fuzz_corpus

namespace pot_serial_corpus {
// Defined in test_serial_framing.cpp: writes the exact bytes this encoder produces, for the
// Python differential test.
int emit(const char* dir);
}  // namespace pot_serial_corpus

namespace pot_ns_corpus {
// Defined in test_ns_wire.cpp: writes READ/WRITE/REPLY bytes across every value type, quality and
// error, so the host's second implementation of §5.2's namespace payloads has to agree with this
// one rather than only with itself.
int emit(const char* dir);
}  // namespace pot_ns_corpus

namespace tst {

int run_all(int argc, char** argv) {
    if (argc > 2 && std::strcmp(argv[1], "--emit-fixtures") == 0) {
        return pot_test_fixtures::emit(argv[2]);
    }
    if (argc > 2 && std::strcmp(argv[1], "--emit-fuzz-corpus") == 0) {
        return pot_fuzz_corpus::emit(argv[2]);
    }
    if (argc > 2 && std::strcmp(argv[1], "--emit-serial-corpus") == 0) {
        return pot_serial_corpus::emit(argv[2]);
    }
    if (argc > 2 && std::strcmp(argv[1], "--emit-ns-corpus") == 0) {
        return pot_ns_corpus::emit(argv[2]);
    }
    const char* filter = (argc > 1) ? argv[1] : nullptr;

    const char* last_suite = "";
    int ran = 0;
    for (const Case& c : registry()) {
        if (filter != nullptr && std::strstr(c.suite, filter) == nullptr &&
            std::strstr(c.name, filter) == nullptr) {
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
    }

    std::printf("\n%d case(s), %d check(s), %d failure(s)\n", ran, checks(), failures());
    if (failures() == 0) {
        std::printf("PASS\n");
        return 0;
    }
    std::printf("FAILED\n");
    return 1;
}

}  // namespace tst

int main(int argc, char** argv) { return tst::run_all(argc, argv); }
