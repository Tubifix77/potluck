#pragma once

// M5's signature-cost [MEASURE]. Runs the known-answer checks, then times every operation and prints
// one {"t":"bench"} line each. Returns the number of failures.
int pot_crypto_bench_run();

// M5.1 CR-5: internal RAM against PSRAM. Prints one {"t":"mem_bench"} line per measurement.
void pot_mem_bench_run();
