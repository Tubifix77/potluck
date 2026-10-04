# Monocypher, vendored

| | |
|---|---|
| upstream | https://github.com/LoupVaillant/Monocypher |
| version | tag `4.0.3`, commit `ab2b16dd619ad5f6979a4fbe69cfa324a6fcc35f` |
| fetched | 2026-10-05, `git clone --depth 1 --branch 4.0.3` |
| licence | dual: 2-clause BSD or CC-0, recipient's choice (`LICENCE.md`, verbatim). Potluck takes it under the BSD licence, which `NOTICE` records |
| modified | **no** — the five files below are byte-identical to the tag |

| file | upstream path | sha256 | git blob (equals upstream's) |
|---|---|---|---|
| `monocypher.c` | `src/monocypher.c` | `f1f838cdd483bdebe0df0ff5c5ed60535e496f769c6a2f933ac4c0b114207123` | `c7c5450e84e4dffda44514d9f918162f1830de56` |
| `monocypher.h` | `src/monocypher.h` | `fcaf6ed771358bb4f40fba016f6518ae86ec02b1b877d2cc35ad92d3a26fd7b3` | `cf635e88e4f5851e1f58a9e2b3e85b8029a73861` |
| `monocypher-ed25519.c` | `src/optional/monocypher-ed25519.c` | `ce0d2f8e32ca8f66398ba5b3456cc74327c3eff14e7b950ce7d57be9025cc453` | `4fdd07641e1c4d54a348e95d0ab05c28f84654a6` |
| `monocypher-ed25519.h` | `src/optional/monocypher-ed25519.h` | `3a3035181f991a158d0e1c7567258f0bae8ba0f1f23c5512b4a1db1b3c9730ce` | `d7aa004123e1d38b891fdabd4171fbf9957242ae` |
| `LICENCE.md` | `LICENCE.md` | `a5781770269d2516e52ba4863f790c10a16da4089a1e81823aee19ff1e9026b0` | `9d69ad72ec113a743264d34dd86a5c04d0c42d35` |

**Check it:** `git hash-object <file>` must print the blob id above, which is the id of the same path
in upstream's tag (`git rev-parse 4.0.3:<upstream path>`). The first copy committed here (`fd30b31`)
was NOT byte-identical: a Windows clone had converted it to CRLF line endings, and its hashes were of
the converted bytes. Copy files out of a clone with `git show 4.0.3:<path>`, never from the checkout.

**Why it is here.** M5 must decide Ed25519 against P-256 on measured verify cost on the node. The
ESP-IDF v6.0.2 tree ships mbedTLS 4.1.0, which implements P-256 ECDSA and X25519 but not EdDSA (only
the PSA constants exist). Monocypher's core signs with EdDSA over BLAKE2b; the optional module is RFC
8032 Ed25519 over SHA-512, the variant everything else in Potluck uses.

**To update:** fetch the new tag, replace the five files, update this table, and re-run the host
known-answer test and the board benchmark (`CONFIG_POT_CRYPTO_BENCH`).
