#!/usr/bin/env bash
# Compile the portable core with a strict GCC warning set.
#
#   tools/check_portability.sh              # core only, warnings are errors
#   tools/check_portability.sh --all        # also test/ and sim/, informational
#
# Why this exists: everything in this repository has only ever been compiled by MSVC on the
# maintainer's machine, and by the IDF's own (looser) flag set on the Xtensa side. The strict flags
# in tests/CMakeLists.txt -- -Wconversion, -Wsign-conversion, -Wshadow, -Wcast-qual -- had never
# actually run against a GCC frontend. They do now.
#
# It prefers a native g++, and falls back to the Xtensa cross-compiler that ESP-IDF installs, which
# is a real GCC and finds the same warnings. One deliberate difference when using the cross
# compiler: its target is ILP32, where uint32_t is `unsigned long`, so printf("%u", a_uint32_t)
# warns -- while on x86-64 Linux uint32_t is `unsigned int` and the same call is correct. Those
# diagnostics are an artefact of the cross target rather than a portability defect, which is one
# reason test/ and sim/ are informational and only the core is gated.
#
# The core is gated because it is the code that ships: it runs on Xtensa today and is meant to
# survive a port to a RISC-V ESP32 variant or to a host bridge without a rewrite.

set -uo pipefail
cd "$(dirname "$0")/.."

STRICT="-std=c++17 -fsyntax-only -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        -Wshadow -Wcast-qual -Wold-style-cast -Wdouble-promotion -Wformat=2"
INC="-Ifirmware/components/pot_frame/include -Ifirmware/components/pot_link/include
     -Ifirmware/components/pot_ns/include -Itests -Isim"

CORE=$(ls firmware/components/pot_frame/src/*.cpp firmware/components/pot_link/src/*.cpp \
          firmware/components/pot_ns/src/*.cpp)
EXTRA=$(ls tests/*.cpp sim/*.cpp 2>/dev/null)

# --- find a compiler ---------------------------------------------------------------------------
CXX=""
for c in g++ clang++ c++; do
    if command -v "$c" >/dev/null 2>&1; then CXX="$c"; NATIVE=1; break; fi
done
if [ -z "$CXX" ]; then
    # ESP-IDF's Xtensa GCC. Present on any machine set up to build the firmware. $HOME is not
    # dependable here: Git Bash launched from PowerShell can report a different home from the one
    # ESP-IDF installed under, so try USERPROFILE and the conventional Windows path too.
    # Note both /c/... (Git Bash) and /mnt/c/... (WSL): on this machine `bash` invoked from
    # PowerShell resolves to WSL, whose $HOME is a Linux path with no ESP-IDF under it, while the
    # toolchain lives on the Windows side. Globbing the user directory avoids depending on $USERNAME,
    # which WSL does not set.
    for home in "$HOME" "${USERPROFILE:-}" /c/Users/* /mnt/c/Users/*; do
        [ -n "$home" ] && [ -d "$home" ] || continue
        # Normalise a Windows-style path (C:\Users\x) into the MSYS form Git Bash globs.
        case "$home" in
            [A-Za-z]:\\*|[A-Za-z]:/*)
                drive=$(printf '%s' "$home" | cut -c1 | tr 'A-Z' 'a-z')
                rest=$(printf '%s' "$home" | cut -c3- | tr '\\' '/')
                home="/$drive$rest"
                ;;
        esac
        for g in "$home"/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32-elf-g++*; do
            [ -x "$g" ] && CXX="$g" && NATIVE=0 && break 2
        done
    done
fi
if [ -z "$CXX" ]; then
    echo "no C++ compiler found (looked for g++, clang++, c++, and ESP-IDF's Xtensa GCC)" >&2
    exit 2
fi

echo "compiler: $CXX"
"$CXX" --version | head -1
[ "${NATIVE:-0}" = "0" ] && echo "note: cross compiler (ILP32). %u vs uint32_t diagnostics are a target artefact."
echo

run_set() {
    local label="$1"; shift
    local files="$1"; shift
    local total=0
    echo "### $label"
    for f in $files; do
        out=$("$CXX" $STRICT $INC "$f" 2>&1) || true
        n=$(printf '%s' "$out" | grep -c 'warning:\|error:') || true
        if [ "${n:-0}" -gt 0 ]; then
            echo "  $f: $n"
            printf '%s\n' "$out" | grep 'warning:\|error:' | sed 's|^.*/||' | head -8 | sed 's/^/      /'
            total=$((total + n))
        fi
    done
    [ "$total" -eq 0 ] && echo "  clean"
    echo
    return "$total"
}

run_set "portable core (gated)" "$CORE"
core_diags=$?

extra_diags=0
if [ "${1:-}" = "--all" ]; then
    run_set "tests and simulator (informational)" "$EXTRA"
    extra_diags=$?
fi

echo "================================================"
echo "core diagnostics: $core_diags"
[ "${1:-}" = "--all" ] && echo "test/sim diagnostics: $extra_diags (not gated)"
if [ "$core_diags" -ne 0 ]; then
    echo "FAIL - the portable core must be clean under the strict set"
    exit 1
fi
echo "PASS"
