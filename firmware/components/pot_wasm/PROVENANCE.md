# wasm3, vendored for M7's experiment

| | |
|---|---|
| upstream | https://github.com/wasm3/wasm3 |
| version | tag `v0.9.0`, commit `0cd38327f0c721e75172f4f1eeb55854dc0517af` (2026-08-24) |
| fetched | 2026-10-10, `https://github.com/wasm3/wasm3/archive/refs/tags/v0.9.0.tar.gz`, 2,656,936 B, sha256 `cab79ce74bcac25bbf80b5ebe14af9795b9bac30b05ee8f620a3bc8002f3b8e6` -- the download the owner allowed (M0-LOG session 36) |
| licence | MIT (`LICENSE`, verbatim); `NOTICE` records it |
| modified | **yes, three files**, the two patches below; every other file is byte-identical to the tag |
| taken | the interpreter core only: `source/` minus the WASI, libc and tracer APIs (`m3_api_*`), which a sandbox that grants nothing does not need |

**Patch 1** (`m3_core.c`, default in `m3_config.h`): `d_m3ExternalYield` -- the embedder defines
`m3_Yield` itself. Upstream's default is `M3_WEAK`, which is empty on MSVC, so the host build's
definition would collide with it.

**Patch 2** (`m3_exec.h`, the `Loop` op, default in `m3_config.h`): `d_m3YieldInLoops` -- call
`m3_Yield` on every loop back-edge. Upstream calls it only from the call ops (`Call`, `ReturnCall`,
`CallIndirect`), so a guest's `loop br 0` never reaches it and cannot be stopped; with it, the
sandbox's fuel counter (`src/wasm_sandbox.cpp`) traps such a guest. Both default to 0, which is
upstream's behaviour.

| file | upstream path | upstream sha256 | here |
|---|---|---|---|
| `LICENSE` | `LICENSE` | `c67aa5c0d77ea831c9a9a025fcbdb69d8241da5d7c673243ae34b1a660afcda2` | identical |
| `m3_bind.c` | `source/m3_bind.c` | `00435c456c9a0bb956b4068036ec02b3bcbbf5061a412e518216725e28becc58` | identical |
| `m3_bind.h` | `source/m3_bind.h` | `4c66b0261c33fa79e7c8b63fe6183ee3eaa93f63d2f48f052739ded97cc19ec9` | identical |
| `m3_code.c` | `source/m3_code.c` | `01ae23645d7e045f1f7e0d8d6e08eef0d3d6e572225e80b69cd426ce491fd604` | identical |
| `m3_code.h` | `source/m3_code.h` | `2370482bf254cd276547c478528050a3bcee6fb0f9524623f5c171aae4d5e8f0` | identical |
| `m3_compile.c` | `source/m3_compile.c` | `a607c82507edf738e2621970a077896291247afabbe6e5e82a96693b9cb9056a` | identical |
| `m3_compile.h` | `source/m3_compile.h` | `e2371686bf3f377c2d8016786b368954bdf5bd51c3b37b6267fd2f2fee0de774` | identical |
| `m3_config.h` | `source/m3_config.h` | `b84841ce6182c70332f53e9a096b4ca7f731a3b2d4daaba0fd523865d54054e1` | PATCHED, now 222f3555d71c9e84... |
| `m3_config_platforms.h` | `source/m3_config_platforms.h` | `105cafc36aae0a545844c1db9affa359d414abc7bcf61c8e9b190747c30d3118` | identical |
| `m3_core.c` | `source/m3_core.c` | `980f94170fb8a748fb298509fbef4388640c9a66f54b54d3beaa22fb3daba996` | PATCHED, now 0619b8392175e17a... |
| `m3_core.h` | `source/m3_core.h` | `3e7818242316214bef86c06a28215bc3f2c6edadfad6340ad4b66594c6ed3f05` | identical |
| `m3_env.c` | `source/m3_env.c` | `694f32446738d87a3d9b45dba30fdf0e6ce45485f0ec266c81887156592daafa` | identical |
| `m3_env.h` | `source/m3_env.h` | `93ffae0fa497360485c1f3581c30fa99dcde5ef169fae84227b19d7998ea58e5` | identical |
| `m3_exception.h` | `source/m3_exception.h` | `e6beeab90bed0f30d1d14e8a31434b9878d82356579ddda747b05fb2d6d6fda4` | identical |
| `m3_exec.c` | `source/m3_exec.c` | `9e5d6adb9c757c0e8fa06c8c18c335b1f8ccc16922b65693071f1878124f09ec` | identical |
| `m3_exec.h` | `source/m3_exec.h` | `8d427cb0fdd128be763245691fd8c86846d6906448643568c8849895b23a67ca` | PATCHED, now a59efed36f5848ef... |
| `m3_exec_defs.h` | `source/m3_exec_defs.h` | `d1af4d9bcee94f9416e250a262a4e892b094d0cb02833b846ec7f70e58efa66b` | identical |
| `m3_function.c` | `source/m3_function.c` | `35290133a7fdf94175c8694b40de843be66daa8b831a40dbaab0c98c69c388c0` | identical |
| `m3_function.h` | `source/m3_function.h` | `6acdb0d65f67e3cfff5c8968f19555d92f9a05347701f72c8a751d5ef330b21f` | identical |
| `m3_info.c` | `source/m3_info.c` | `4d4cb31d68b7df8584ac25fdace33cb3cda5dff6fc91a0ca6ad6faf907d848c5` | identical |
| `m3_info.h` | `source/m3_info.h` | `ebb3d06fc4b2213aa0b5d8fb70d133f17ab6a01b2ae3fa6a78e0b8ba0f96d485` | identical |
| `m3_math_utils.h` | `source/m3_math_utils.h` | `8a123a5c79bccb3e96d46df451c0afdccfb67eb44c887efbfb38bf2f54e8d83a` | identical |
| `m3_module.c` | `source/m3_module.c` | `a23907d3d34d02d3b91204957df619abfe353686656c609eef21f10b777d36c1` | identical |
| `m3_parse.c` | `source/m3_parse.c` | `337c7d0cbba341a545caf5c803066eac1d014914cdf2dd5359f0b75935648729` | identical |
| `m3_validate.c` | `source/m3_validate.c` | `3f0049f1e97a793298733d0eb1c93f3e27bf62cc00c50936e9045ae7ce0c3e03` | identical |
| `m3_validate.h` | `source/m3_validate.h` | `bc5259773e07bdeb3654116b2587f5fbe4d806218858acf9d96a126603a27e97` | identical |
| `wasm3.h` | `source/wasm3.h` | `6452dfb74b54eb77a5483597a7d6abe0a7779344facfb581f89ab6813e8767e9` | identical |
| `wasm3_defs.h` | `source/wasm3_defs.h` | `98f33c6bf16ab94a75082fe13f12f66abce3d3f4e7b8384e5d23f905ed44f12f` | identical |

**Check it:** `sha256sum` each file against the table; the three patched ones differ from upstream
exactly by the blocks marked `POTLUCK PATCH` / `POTLUCK:`.

**To update:** fetch the new tag into an empty directory, copy `source/` (minus `m3_api_*`) and
`LICENSE`, re-apply the two patches, update this table, regenerate `include/pot/wasm_modules.h`
(`python tools/wasm_modules.py`) and re-run `tests/test_wasm.cpp` and the board bench
(`CONFIG_POT_WASM_BENCH`).
