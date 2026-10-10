#pragma once

// M7's experiment on a board (CONFIG_POT_WASM_BENCH): the sandbox's cost against native, and its limits
// tested. Prints one {"t":"wasm_bench"} line per measurement. Returns the number of failures.
int pot_wasm_bench_run();
