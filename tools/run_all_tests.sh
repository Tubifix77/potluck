#!/usr/bin/env bash
# Linux/macOS equivalent of tools\run_all_tests.ps1.
#
#   tools/run_all_tests.sh            # C++ suite, fixtures, Python suites, portability gate
#   tools/run_all_tests.sh --asan     # also run the C++ suite under AddressSanitizer
#   tools/run_all_tests.sh --clean    # start from a fresh build directory
#
# Needs: a C++17 compiler, CMake >= 3.20, Python 3.10+. Nothing else — the portable half of Potluck
# has no dependencies, and the Python tooling is standard library only.
#
#   Debian/Ubuntu:  sudo apt install -y g++ cmake ninja-build python3
#   Fedora:         sudo dnf install -y gcc-c++ cmake ninja-build python3
#   macOS:          xcode-select --install && brew install cmake ninja
#
# NOTE FOR THE MAINTAINER: as of 2026-08-01 this script has never been executed. The development
# machine is Windows with MSVC, and the WSL Ubuntu on it has no C++ compiler and no passwordless
# sudo. What *has* been verified is that the portable core compiles clean under a GCC frontend with
# a stricter warning set than this build uses (tools/check_portability.sh), and that CMakeLists
# carries a non-MSVC branch. Treat a first run here as unproven and expect to fix something.

set -uo pipefail
cd "$(dirname "$0")/.."

ASAN=0
CLEAN=0
BUILD_DIR="build/tests"
for arg in "$@"; do
    case "$arg" in
        --asan)  ASAN=1; BUILD_DIR="build/tests-asan" ;;
        --clean) CLEAN=1 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

results=()
record() {
    results+=("$2|$1")
    [ "$2" -eq 0 ] && echo "==> PASS  $1" || echo "==> FAIL  $1"
    echo
}

need() {
    command -v "$1" >/dev/null 2>&1 || { echo "missing: $1" >&2; return 1; }
}
need cmake || exit 2
PYTHON=$(command -v python3 || command -v python) || { echo "missing: python3" >&2; exit 2; }

GENERATOR=()
command -v ninja >/dev/null 2>&1 && GENERATOR=(-G Ninja)

[ "$CLEAN" -eq 1 ] && rm -rf "$BUILD_DIR"

echo "### C++ host tests${ASAN:+ (asan=$ASAN)}"
cmake -S tests -B "$BUILD_DIR" "${GENERATOR[@]}" \
      -DCMAKE_BUILD_TYPE=Debug \
      -DPOT_ASAN=$([ "$ASAN" -eq 1 ] && echo ON || echo OFF) >/dev/null
cmake --build "$BUILD_DIR" || { record "C++ build" 1; exit 1; }
"$BUILD_DIR/pot_tests"
record "C++ host tests" $?

echo "### regenerating fixtures"
mkdir -p host/potluck/tests/fixtures
"$BUILD_DIR/pot_tests" --emit-fixtures host/potluck/tests/fixtures
record "serial format fixture" $?
# ~4 MB, deterministic, gitignored. Regenerated so test_differential.py has something to replay.
"$BUILD_DIR/pot_tests" --emit-fuzz-corpus host/potluck/tests/fixtures
record "differential fuzz corpus" $?
"$BUILD_DIR/pot_tests" --emit-serial-corpus host/potluck/tests/fixtures
record "serial framing corpus" $?

for t in test_frame.py test_records.py test_differential.py test_paths.py test_sys_paths.py test_serial.py test_serial_diff.py test_value.py test_ns_diff.py test_bridge.py test_replay.py test_transport.py test_manifest.py test_locality.py; do
    echo "### Python: $t"
    ( cd host/potluck && "$PYTHON" "tests/$t" )
    record "Python $t" $?
done

if [ -x tools/check_portability.sh ] || [ -f tools/check_portability.sh ]; then
    echo "### portability gate"
    bash tools/check_portability.sh >/dev/null 2>&1
    record "portable core strict warnings" $?
fi

echo "================================================"
failed=0
for r in "${results[@]}"; do
    code="${r%%|*}"; name="${r#*|}"
    if [ "$code" -eq 0 ]; then printf '%-34s PASS\n' "$name"
    else printf '%-34s FAIL (%s)\n' "$name" "$code"; failed=$((failed+1)); fi
done
echo "================================================"
if [ "$failed" -eq 0 ]; then echo "ALL PASS"; exit 0; fi
echo "$failed FAILED"
exit 1
