#pragma once

// M5's signature-cost [MEASURE]. Runs the known-answer checks, then times every operation and prints
// one {"t":"bench"} line each. Returns the number of failures.
int pot_crypto_bench_run();
