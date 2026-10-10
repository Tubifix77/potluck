// M7: the functions a sandbox lets a guest import -- for pot_wasm's own sources, which include wasm3.
// Everything a guest may call is in the table it is loaded with; a module importing anything else, from
// any module name, is refused at load.

#pragma once

#include <cstddef>

extern "C" {
#include "wasm3.h"
}

namespace pot {

struct WasmImport {
    const char* name;       // imported as potluck.<name>
    const char* signature;  // wasm3's: "v(i)", "f(i)", "i(*i)", ...
    M3RawCall fn;           // reads its user data with m3_GetUserData(runtime)
};

}  // namespace pot
