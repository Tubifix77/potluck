# Monocypher, vendored

| | |
|---|---|
| upstream | https://github.com/LoupVaillant/Monocypher |
| version | tag `4.0.3`, commit `ab2b16dd619ad5f6979a4fbe69cfa324a6fcc35f` |
| fetched | 2026-10-05, `git clone --depth 1 --branch 4.0.3` |
| licence | dual: 2-clause BSD or CC-0, recipient's choice (`LICENCE.md`, verbatim). Potluck takes it under the BSD licence, which `NOTICE` records |
| modified | **no** — the five files below are byte-identical to the tag |

| file | upstream path | sha256 |
|---|---|---|
| `monocypher.c` | `src/monocypher.c` | `91594377d2044c7212e88aeeab7fb5a03078f4face45e50a5b0635b52d8f89b7` |
| `monocypher.h` | `src/monocypher.h` | `7911a870e0b26d301ec552015b89abc70091ae81e8830385791e936a8f828c65` |
| `monocypher-ed25519.c` | `src/optional/monocypher-ed25519.c` | `bdece3b1c55790527fe054fd7230aee0dcfa76cb564ffa6f3129eec0dac9af2a` |
| `monocypher-ed25519.h` | `src/optional/monocypher-ed25519.h` | `40d90a7e146929e961ef6900f8c471b6fe1a8c4ce41d0bf063d371e5ffd5baf2` |
| `LICENCE.md` | `LICENCE.md` | `035c7355e948ce49e49f7c4e500ebabe79135f1b46080149649d144a02c23fe5` |

**Why it is here.** M5 must decide Ed25519 against P-256 on measured verify cost on the node. The
ESP-IDF v6.0.2 tree ships mbedTLS 4.1.0, which implements P-256 ECDSA and X25519 but not EdDSA (only
the PSA constants exist). Monocypher's core signs with EdDSA over BLAKE2b; the optional module is RFC
8032 Ed25519 over SHA-512, the variant everything else in Potluck uses.

**To update:** fetch the new tag, replace the five files, update this table, and re-run the host
known-answer test and the board benchmark (`CONFIG_POT_CRYPTO_BENCH`).
