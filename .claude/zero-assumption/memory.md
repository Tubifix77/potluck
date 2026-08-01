# Zero-Assumption Memory Ledger

Every row is a fact earned from a live lookup and trusted *only* because it carries a source.
Never add a fact from memory. See the `zero-assumption` contract §5 for read/validate/write,
staleness, and the contradiction protocol.

- **claim** — short, stable identifier for the fact
- **value** — the verified answer
- **source** — URL or identifier that substantiates it
- **retrieved** — ISO date (YYYY-MM-DD) fetched
- **freshness** — `volatile` (re-verify every use) or `stable` (reuse while verified)
- **status** — `verified` (trusted) or `superseded` (history only, not reused)

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| ESP-NOW v1.0 max payload | 250 bytes (`ESP_NOW_MAX_DATA_LEN`) | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html | 2026-08-01 | stable | verified |
| ESP-NOW v2.0 max payload | 1470 bytes (`ESP_NOW_MAX_DATA_LEN_V2`) | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html | 2026-08-01 | stable | verified |
| ESP-NOW v1/v2 interop | v2.0 devices can receive from v2.0 and v1.0; v1.0 devices can only receive from v1.0 | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html | 2026-08-01 | stable | superseded — incomplete; see "ESP-NOW v1/v2 interop, length-qualified" |
| ESP-NOW max peers | 20 paired devices total; encrypted peers no more than 17, default 7 | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html | 2026-08-01 | stable | verified |
| ESP-NOW encryption | CCMP protects the action frame; PMK and LMK both 16 bytes; encrypting multicast vendor-specific action frames not supported | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html | 2026-08-01 | stable | verified |
| ESP-NOW channel constraint | The channel must be set as the channel the local device is on | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html | 2026-08-01 | stable | verified |
| ESP32 SRAM total | 520 KB available SRAM = 320 KB DRAM + 200 KB IRAM | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/memory-types.html | 2026-08-01 | stable | verified |
| ESP32 static DRAM cap | Max statically allocated DRAM 160 KB; remaining 160 KB only allocatable at runtime as heap | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/memory-types.html | 2026-08-01 | stable | verified |
| ESP32 DRAM reduced by BT/trace | −64 KB if Bluetooth stack used; −16–32 KB if trace memory used | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/memory-types.html | 2026-08-01 | stable | verified |
| ESP32 TWAI standard | Compatible with ISO 11898-1 frame structure; 11-bit standard and 29-bit extended IDs | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/twai.html | 2026-08-01 | stable | verified |
| ESP32 TWAI no CAN FD | "not compatible with FD format frames and will interpret such frames as errors" | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/twai.html | 2026-08-01 | stable | verified |
| ESP32 TWAI needs transceiver | No internal transceiver; external transceiver required (e.g. TJA105x for ISO 11898-2) | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/twai.html | 2026-08-01 | stable | verified |
| ESP32 TWAI max bitrate | NOT STATED in the ESP-IDF TWAI page; only examples of 200 kbit/s and 500 kbit/s given | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/twai.html | 2026-08-01 | stable | verified |
| WAMR binary sizes | ~58.9 K fast interpreter, ~56.3 K classic interpreter, ~29.4 K AOT runtime, ~21.4 K libc-wasi, ~3.7 K libc-builtin | https://github.com/bytecodealliance/wasm-micro-runtime | 2026-08-01 | stable | verified |
| WAMR execution modes / platforms | interpreter, fast interpreter, AOT, JIT (tier-up Fast JIT → LLVM JIT); supports Zephyr, ESP-IDF (FreeRTOS), NuttX, RT-Thread, VxWorks, AliOS-Things; XTENSA + RISCV32 among archs | https://github.com/bytecodealliance/wasm-micro-runtime | 2026-08-01 | stable | verified |
| Wasm3 minimum requirements | "Minimum useful system requirements: ~64Kb for code and ~10Kb RAM"; ESP32 listed as supported MCU | https://github.com/wasm3/wasm3 | 2026-08-01 | stable | verified |
| Measured Wasm RAM on ESP32-C6 | Bubble-sort-100 benchmark: wasm3 ~156 KB, WAMR ~480 KB total memory (Fig. 4) | https://arxiv.org/html/2512.00035v1 | 2026-08-01 | stable | verified |
| Measured Wasm slowdown ESP32-C6 | Bubble-sort-100: native C 577.5 µs vs wasm3 6,358 µs (~11×); WAMR higher (Fig. 5) | https://arxiv.org/html/2512.00035v1 | 2026-08-01 | stable | verified |
| Measured Wasm energy ESP32-C6 | Bubble-sort-100: native C 0.1 mJ, wasm3 1.12 mJ, WAMR 2.96 mJ (Fig. 3) | https://arxiv.org/html/2512.00035v1 | 2026-08-01 | stable | verified |
| ESP32 Secure Boot v2 scheme | RSA-PSS (RSA-3072) signature verification of bootloader and app; SHA-256 digest of public key in eFuse BLK2; on ESP32 only one public key can be stored | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/security/secure-boot-v2.html | 2026-08-01 | stable | verified |
| ESP32 Secure Boot v2 OTA | "The application image is not only verified on every boot but also on each over the air (OTA) update." | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/security/secure-boot-v2.html | 2026-08-01 | stable | verified |
| NVS limits | Max key length 15 chars; strings ≤4000 bytes; blobs ≤508,000 bytes or 97.6% of partition − 4000 bytes, whichever lower | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/nvs_flash.html | 2026-08-01 | stable | verified |
| NVS not for large blobs | "NVS works best for storing many small values, rather than a few large values"; recommends FAT + wear levelling for large blobs | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/nvs_flash.html | 2026-08-01 | stable | verified |
| NVS encryption | NVS is not directly compatible with ESP32 flash encryption; data can be stored encrypted if NVS encryption is used with flash encryption | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/nvs_flash.html | 2026-08-01 | stable | verified |
| AtomVM what it is | Ground-up implementation of the BEAM designed to run on small systems; ESP32 and STM32 targets; spawn/monitor/message lightweight processes; "Distributed Erlang" is a documented topic | https://doc.atomvm.org/main/welcome-to-atomvm.html | 2026-08-01 | stable | verified |
| AtomVM minimum RAM | "The smallest environment in which AtomVM runs has around 128k of addressable RAM." | https://doc.atomvm.org/main/welcome-to-atomvm.html | 2026-08-01 | stable | verified |
| Toit fleet OTA | Artemis CLI features a patch-based over-the-air update mechanism; containers can be started periodically or on conditions | https://toit.io/product/fleet-management/ | 2026-08-01 | volatile | verified |
| ESP-NOW best-case one-way delay | 54 m LOS farmland: mean 2782.85 µs, σ 108.72 µs, PDR 1.0 (250-byte payload, 802.11b/g 1 Mbit/s) | https://dl.ifip.org/db/conf/wons/wons2025/1571077625.pdf | 2026-08-01 | stable | verified |
| ESP-NOW delay tail | 52 m: PDR 99.85%, mean 3461.65 µs, σ 2079.06 µs, max 25628 µs. 58 m: PDR 83.2%, mean 7851.15 µs, σ 8033.23 µs, max 59192 µs | https://dl.ifip.org/db/conf/wons/wons2025/1571077625.pdf | 2026-08-01 | stable | verified |
| ESP-NOW retransmission model | initial tx delay 2800 µs, retransmission delay 3350 µs, slot 481 µs, retransmission limit 31; slotted p-persistent channel access | https://dl.ifip.org/db/conf/wons/wons2025/1571077625.pdf | 2026-08-01 | stable | verified |
| ESP-NOW range cliff | Open farmland: PDR constantly >99% below 56 m, zero above 70 m; between 56–70 m PDR fluctuates between 100% and zero | https://dl.ifip.org/db/conf/wons/wons2025/1571077625.pdf | 2026-08-01 | stable | verified |
| ESP-NOW paper provenance | Becker, Oberli, Zobel, Steinmetz, Meuser, "ESP-NOW Performance in Outdoor Environments: Field Experiments and Analysis", IEEE/IFIP WONS 2025 | https://dl.ifip.org/db/conf/wons/wons2025/1571077625.pdf | 2026-08-01 | stable | verified |
| ESP32 chips with 802.15.4 | "chips with 15.4 radio such as ESP32-H2, ESP32-C6 and ESP32-C5"; roles: standalone node, RCP, OpenThread host, border router | https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-guides/openthread.html | 2026-08-01 | stable | verified |
| Plan 9 / 9P | Pike, Presotto, Dorward, Flandrena, Thompson, Trickey, Winterbottom, "Plan 9 from Bell Labs": resources named and accessed like files; "There is a standard protocol, called 9P, for accessing these resources"; each window created in a separate name space | https://9p.io/sys/doc/9.html | 2026-08-01 | stable | verified |
| ESP-IDF mbedTLS HW accel | Hardware AES, SHA, MPI (bignum/RSA) and ECC acceleration options exist; Ed25519/Curve25519 not mentioned on that page | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/protocols/mbedtls.html | 2026-08-01 | stable | verified |
| ESP-IDF FreeRTOS tick default | FREERTOS_HZ default 100, range 1–1000 | https://raw.githubusercontent.com/espressif/esp-idf/master/components/freertos/Kconfig | 2026-08-01 | stable | verified |
| CAN 2.0A frame bit length | Standard data frame is 47–111 bits depending on 0–8 data bytes; 47 bits overhead; excludes stuffing bits. At 1 Mbit/s → 47–111 µs. NOTE: single vendor-blog source, not primary | https://copperhilltech.com/blog/controller-area-network-can-bus-tutorial-message-frame-format/ | 2026-08-01 | stable | verified |
| ESP32 is dual-core (IDF SMP) | "to support dual-core ESP targets, such as ESP32, ESP32-S3, and ESP32-P4, ESP-IDF provides a unique implementation of FreeRTOS with dual-core symmetric multiprocessing (SMP)" | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/freertos_idf.html | 2026-08-01 | stable | verified |
| ESP-IDF task core affinity | `xTaskCreatePinnedToCore()` pins a task to core 0 or 1; `tskNO_AFFINITY` lets it run on both cores | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/freertos_idf.html | 2026-08-01 | stable | verified |
| FreeRTOS preemption + idle priority | Fixed-priority preemptive scheduler: "executes the highest priority ready-state task"; idle task has priority 0 and runs when no other task is ready | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/freertos_idf.html | 2026-08-01 | stable | verified |
| k8s placement preferences exist | Node affinity: `required…` = "The scheduler can't schedule the Pod unless the rule is met"; `preferred…` = "The scheduler tries to find a node that meets the rule. If a matching node is not available, the scheduler still schedules the Pod." | https://kubernetes.io/docs/concepts/scheduling-eviction/assign-pod-node/ | 2026-08-01 | stable | verified |
| Kubernetes license | Apache-2.0 | https://api.github.com/repos/kubernetes/kubernetes | 2026-08-01 | stable | verified |
| ESP-IDF license | Apache-2.0 | https://api.github.com/repos/espressif/esp-idf | 2026-08-01 | stable | verified |
| FreeRTOS-Kernel license | MIT | https://api.github.com/repos/FreeRTOS/FreeRTOS-Kernel | 2026-08-01 | stable | verified |
| wasm3 license | MIT | https://api.github.com/repos/wasm3/wasm3 | 2026-08-01 | stable | verified |
| WAMR license | Apache-2.0 | https://api.github.com/repos/bytecodealliance/wasm-micro-runtime | 2026-08-01 | stable | verified |
| AtomVM license | Apache-2.0 | https://api.github.com/repos/atomvm/AtomVM | 2026-08-01 | stable | verified |

### M0 additions — 2026-08-01

Registered while building M0. Two rows above are superseded by rows here; see the Contradictions
section below for how each was resolved.

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| ESP-IDF latest stable version | **v6.0.2** — `docs.espressif.com/.../en/stable/` renders as "ESP-IDF Programming Guide v6.0.2 documentation"; GitHub releases list v6.0.2 (2026-06-29) as the newest non-prerelease minor, v6.1-beta1 (2026-06-29) prerelease, v5.5.5 (2026-07-17) newest 5.5.x patch | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html + https://api.github.com/repos/espressif/esp-idf/releases | 2026-08-01 | volatile | verified |
| ESP-NOW v1/v2 interop, length-qualified | "The v2.0 devices are capable of receiving packets from both v2.0 and v1.0 devices. In contrast, v1.0 devices can only receive packets from other v1.0 devices. However, v1.0 devices **can** receive v2.0 packets if the packet length is ≤ 250 (`ESP_NOW_MAX_IE_DATA_LEN`). For packets exceeding this length, the v1.0 devices will either truncate the data to the first 250 bytes or discard the packet entirely." | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html | 2026-08-01 | stable | verified |
| ESP-NOW encrypted-peer ceiling, header vs Kconfig | `esp_now.h` defines `ESP_NOW_MAX_ENCRYPT_PEER_NUM 6` in **both** v5.5.5 and v6.0.2, but the real ceiling is `CONFIG_ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM`: `range 0 17 if (!IDF_TARGET_ESP32C2)`, `default 7`. The docs' "≤17, default 7" is authoritative; the header constant is not the ceiling | https://raw.githubusercontent.com/espressif/esp-idf/v6.0.2/components/esp_wifi/include/esp_now.h + https://raw.githubusercontent.com/espressif/esp-idf/v6.0.2/components/esp_wifi/Kconfig | 2026-08-01 | stable | verified |
| ESP-NOW total-peer constant | `#define ESP_NOW_MAX_TOTAL_PEER_NUM 20`, identical in v5.5.5 and v6.0.2 headers | https://raw.githubusercontent.com/espressif/esp-idf/v6.0.2/components/esp_wifi/include/esp_now.h | 2026-08-01 | stable | verified |
| ESP-NOW v1 payload constant is an alias | `#define ESP_NOW_MAX_IE_DATA_LEN 250`; `#define ESP_NOW_MAX_DATA_LEN ESP_NOW_MAX_IE_DATA_LEN`; `#define ESP_NOW_MAX_DATA_LEN_V2 1470` | https://raw.githubusercontent.com/espressif/esp-idf/v6.0.2/components/esp_wifi/include/esp_now.h | 2026-08-01 | stable | verified |
| ESP-NOW send-callback signature | `typedef void (*esp_now_send_cb_t)(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)`; `typedef wifi_tx_info_t esp_now_send_info_t`. Identical in v5.5.5 and v6.0.2 — no per-version shim needed across those two | https://raw.githubusercontent.com/espressif/esp-idf/v6.0.2/components/esp_wifi/include/esp_now.h | 2026-08-01 | stable | verified |
| ESP-NOW recv-callback signature | `typedef void (*esp_now_recv_cb_t)(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int data_len)`; `struct esp_now_recv_info { uint8_t *src_addr; uint8_t *des_addr; wifi_pkt_rx_ctrl_t *rx_ctrl; }` | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html | 2026-08-01 | stable | verified |
| ESP_NOW_SEND_SUCCESS meaning | "It will return `ESP_NOW_SEND_SUCCESS` in sending callback function if the data is received successfully **on the MAC layer**. Otherwise … `ESP_NOW_SEND_FAIL`." — i.e. the callback reports the 802.11 ACK, which is what makes it usable as an outbound-PDR signal | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html | 2026-08-01 | stable | verified |
| `esp_now_send()` signature and errors | `esp_err_t esp_now_send(const uint8_t *peer_addr, const uint8_t *data, size_t len)`; `peer_addr == NULL` sends to all peers; errors `ESP_ERR_ESPNOW_{NOT_INIT,ARG,INTERNAL,NO_MEM,NOT_FOUND,IF,CHAN}`; on `NO_MEM` "you can delay a while before sending the next data" | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html | 2026-08-01 | stable | verified |
| `esp_now_send()` documented length cap | **Doc inconsistency.** The Frame Format section states v2.0 supports 1470 B, but `esp_now_send()`'s "Attention 3" still reads "The maximum length of data must be less than `ESP_NOW_MAX_DATA_LEN`" (= 250). The official `examples/wifi/espnow` Kconfig sets `ESPNOW_SEND_LEN` `range 10 1470`, so 1470 is accepted in practice on v6.0.2 | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html + https://raw.githubusercontent.com/espressif/esp-idf/v6.0.2/examples/wifi/espnow/main/Kconfig.projbuild | 2026-08-01 | stable | verified |
| `esp_now_get_version()` | `esp_err_t esp_now_get_version(uint32_t *version)` — "Get the version of ESPNOW. Currently, ESPNOW supports two versions: v1.0 and v2.0." No Kconfig option gates ESP-NOW v2 (`grep ESPNOW components/esp_wifi/Kconfig` yields only `ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM`) | https://raw.githubusercontent.com/espressif/esp-idf/v6.0.2/components/esp_wifi/include/esp_now.h + https://raw.githubusercontent.com/espressif/esp-idf/v6.0.2/components/esp_wifi/Kconfig | 2026-08-01 | stable | verified |
| ESP-NOW peer channel field | `esp_now_peer_info.channel`: "If the value is 0, use the current channel which station or softap is on. Otherwise, it must be set as the channel that station or softap is on." Range of paired-device channel is 0–14 | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html | 2026-08-01 | stable | verified |
| ESP-NOW receive without pairing | "For the receiving device, calling `esp_now_add_peer()` is not required. If no paired device is added, it can only receive broadcast packets and unencrypted unicast packets." A broadcast MAC peer must be added before *sending* broadcast | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html | 2026-08-01 | stable | verified |
| ESP-NOW default bit rate | "The default ESP-NOW bit rate is 1 Mbps." Per-peer rate via `esp_now_set_peer_rate_config()`, called after `esp_wifi_start()` and `esp_now_add_peer()` | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html | 2026-08-01 | stable | verified |
| ESP-NOW init ordering | "ESP-NOW data must be transmitted after Wi-Fi is started, so it is recommended to start Wi-Fi before initializing ESP-NOW and stop Wi-Fi after de-initializing ESP-NOW." `esp_now_deinit()` deletes all paired-device information | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html | 2026-08-01 | stable | verified |
| Free-DRAM probe API | `size_t heap_caps_get_free_size(uint32_t caps)`; "all DRAM heaps possess the `MALLOC_CAP_8BIT` capability. Users can call `heap_caps_get_free_size(MALLOC_CAP_8BIT)` to get the free size of all DRAM heaps"; `MALLOC_CAP_INTERNAL` = "Memory must be internal". `void heap_caps_get_info(multi_heap_info_t *info, uint32_t caps)` also available | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/mem_alloc.html | 2026-08-01 | stable | verified |
| Microsecond clock API | `int64_t esp_timer_get_time(void)` — "Get time in microseconds since boot… since the initialization of ESP Timer" | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/esp_timer.html | 2026-08-01 | stable | verified |
| `idf.py size` reporting | Prints a "Memory Type Usage Summary" with Used/Used%/Remain/Total per memory type; DRAM row breaks down into `.data` and `.bss`. Sub-commands `idf.py size-components` (per-archive) and `idf.py size-files` exist; report format is selectable via `--format`, and the current `esp-idf-size` emits `json` (the legacy `--ng` flag and `ESP_IDF_SIZE_NG` env var are no longer needed) | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/tools/idf-size.html + https://raw.githubusercontent.com/espressif/esp-idf-size/master/README.md | 2026-08-01 | volatile | verified |
| MSVC toolchain on this bench | `cl.exe` 14.51.36231 at `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools`, with CMake and Ninja bundled under `Common7\IDE\CommonExtensions\Microsoft\CMake`. VS 2022 Community also present with MSVC 14.44.35207 but no bundled CMake | local `vswhere.exe` + filesystem inspection, 2026-08-01 | 2026-08-01 | volatile | verified |
| System ANSI codepage on this bench | ACP = **1252** (windows-1252), from `HKLM:\SYSTEM\CurrentControlSet\Control\Nls\CodePage\ACP` | local registry read, 2026-08-01 | 2026-08-01 | volatile | verified |
| CP1252 best-fit for both mu characters | `[Text.Encoding]::GetEncoding(1252)` maps **both** U+03BC GREEK SMALL LETTER MU and U+00B5 MICRO SIGN to the single byte **0xB5**. So "CP1252 cannot represent U+03BC" is false; the encoding is lossy by *collision*, not by omission | local measurement, 2026-08-01 | 2026-08-01 | stable | verified |
| ESP-IDF v6.0.2 fails on a U+03BC project path | Xtensa GCC reports `cc1.exe: fatal error: D:/Projects/d?OS/...: No such file or directory` for files that exist. Mechanism is a round-trip mismatch: disk path is U+03BC, argv arrives as 0xB5, the CRT widens it back to U+00B5, which does not match | local measurement, 2026-08-01 | 2026-08-01 | volatile | verified |
| ESP-IDF v6.0.2 also fails on a U+00B5 project path | Tested by copying `firmware/` to `D:\esp\dµOS-probe` (U+00B5) and running `idf.py set-target esp32`: `esp-idf-kconfig` fails with `FileNotFoundError: 'D:/esp/dÂµOS-probe/build/kconfigs.in'` — the UTF-8 bytes of µ (0xC2 0xB5) read back as CP1252. A second, independent encoding bug, so **renaming the project directory is not a fix**; any non-ASCII path breaks the toolchain | local measurement, 2026-08-01 | 2026-08-01 | volatile | verified |
| ESP-IDF v6.0.2 builds fine from an ASCII path | Same sources at `D:\esp\potluck-fw` build to completion, repeatedly | local measurement, 2026-08-01 | 2026-08-01 | volatile | verified |

### ESP32-S3 additions — 2026-08-01

Registered when the target hardware changed from classic ESP32 to 7 × ESP32-S3-DevKitC-1 N16R8.
Sources are the locally installed ESP-IDF v6.0.2 tree, which is the same source the build uses.

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| ESP32-S3 has no Bluetooth Classic | `soc_caps.h` defines `SOC_BLE_SUPPORTED` and `SOC_BLE_MESH_SUPPORTED` but **not** `SOC_BT_CLASSIC_SUPPORTED`; classic ESP32 defines both. So the S3 is BLE-only, and §6's "−64 KB if the BT stack is used" is a classic-ESP32 figure that does not transfer | `components/soc/esp32s3/include/soc/soc_caps.h` vs `components/soc/esp32/include/soc/soc_caps.h`, ESP-IDF v6.0.2 | 2026-08-01 | stable | verified |
| ESP32-S3 is 2.4 GHz only | No dual-band or 5 GHz capability appears anywhere in the S3 `soc_caps.h` Wi-Fi block. **The merchant listing's "dual-band WiFi" claim is false** — no ESP32 variant has a 5 GHz radio | `components/soc/esp32s3/include/soc/soc_caps.h`, ESP-IDF v6.0.2 | 2026-08-01 | stable | verified |
| ESP32-S3 memory windows | DRAM `0x3FC88000–0x3FD00000` = 480 KB; IRAM `0x40370000–0x403E0000` = 448 KB. These overlap one unified internal SRAM: "Any internal SRAM which is not used for Instruction RAM will be made available as DRAM", and "the maximum statically allocated DRAM size is reduced by the IRAM size of the compiled application" | `components/soc/esp32s3/include/soc/soc.h` + https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/memory-types.html | 2026-08-01 | stable | verified |
| Classic ESP32 memory windows, for contrast | DRAM `0x3FFAE000–0x40000000` = 328 KB; IRAM `0x40080000–0x400AA000` = 168 KB, as **separate** physical banks with a fixed 160 KB static DRAM cap. §6's budget is written against this model, not the S3's | `components/soc/esp32/include/soc/soc.h`, ESP-IDF v6.0.2 | 2026-08-01 | stable | verified |
| ESP32-S3 USB PHY degrades Wi-Fi | `SOC_WIFI_PHY_NEEDS_USB_WORKAROUND` is set for the S3. `CONFIG_ESP_PHY_ENABLE_USB` **defaults to `y` on S3**, and its help states: "the USB PHY can interfere with WiFi thus lowering WiFi performance… This option can be disabled to increase WiFi performance. However, disabling this option will also mean that the USB PHY cannot be used while WiFi is enabled." Directly affects any RF measurement taken with the stock configuration | `components/esp_phy/Kconfig` + `components/soc/esp32s3/include/soc/soc_caps.h`, ESP-IDF v6.0.2 | 2026-08-01 | stable | verified |

### Contradictions resolved — 2026-08-01

1. **ESP-NOW v1/v2 interop.** The earlier row read "v1.0 devices can only receive from v1.0", which
   is the docs' own summary sentence but not its whole statement: the next sentence adds that a v1.0
   device *does* receive v2.0 packets at or below 250 B, and truncates or discards above it.
   Resolution: superseded the old row, added the length-qualified one. This **strengthens**
   ARCHITECTURE §5.3 — pinning a mixed link to the 226 B profile is not a courtesy, it is the
   condition under which the link works at all, and exceeding it fails *silently by truncation*,
   which is worse than a drop. §5.3's parenthetical was corrected to match.

2. **Encrypted-peer ceiling.** `esp_now.h`'s `ESP_NOW_MAX_ENCRYPT_PEER_NUM` is 6; the docs and the
   Kconfig say ≤17 with a default of 7. The Kconfig is the mechanism that sets the limit, so the
   docs' figure stands and the header constant is a trap for anyone who sizes a table from it.
   ARCHITECTURE §3's row was already correct and is unchanged; the trap is now recorded.
   Not on M0's path (M0 runs unencrypted — link crypto is M5) but it sizes the §6 peer table.

3. **`esp_now_send()` length cap.** "Attention 3" (250 B) contradicts the same page's Frame Format
   section (1470 B for v2.0). The official example ships a 1470 B upper bound, so the attention note
   is stale. Potluck never relies on this at M0 — every M0 frame is ≤ 64 B — but the v2 profile's
   1446 B payload does, so it is tagged **[MEASURE]** in M0-LOG.md rather than assumed.

## QEMU emulation — measured on this machine, 2026-08-01

These are **observations of the emulator**, not of the ESP32-S3. They are registered because each one
cost real debugging time and each would otherwise be re-derived. Emulator behaviour is a moving
target, so freshness is "volatile" throughout: re-check against the installed QEMU version.

QEMU build in use: `esp_develop_9.2.2_20250817`, installed via `idf_tools.py install qemu-xtensa`.

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| Espressif's QEMU fork emulates both S3 cores | The feature table marks "Dual-Core CPU" supported for ESP32 and ESP32-S3, N/A for ESP32-C3. So a hang under emulation is **not** explained by a missing second core | https://github.com/espressif/esp-toolchain-docs/blob/main/qemu/README.md | 2026-08-01 | volatile | verified |
| Wi-Fi, Bluetooth and USB are not emulated | All three are marked unsupported for every target. `esp_wifi_start()` therefore enters PHY calibration and never returns — the build hangs at boot rather than failing, which is why `CONFIG_POT_RADIO_DISABLE` exists | same | 2026-08-01 | volatile | verified |
| GPIO matrix / IOMUX is not emulated | Marked unsupported for every target. `gpio_get_level()` returns 0 on any pin, so an active-low button reads as permanently pressed. Potluck's BYE button on GPIO0 made every emulated node announce a departure ~6 ms after boot until `CONFIG_POT_BYE_BUTTON_GPIO=-1` was set | same, and observed in `build/qemu-console.log` | 2026-08-01 | volatile | verified |
| SysTimer, Timer Groups, UART, eFuse, NOR flash and PSRAM **are** emulated | Listed as supported. Confirmed here: NVS persists across runs and the boot epoch increments, and UART0/UART1 both carry traffic | same | 2026-08-01 | volatile | verified |
| The eFuse MAC block is blank under QEMU | `esp_read_mac(ESP_MAC_WIFI_STA)` yields `00:00:00:00:00:00`, so a node id derived from the MAC lands on the reserved 0x0000 and gets nudged to 0x0001 — **every** emulated node claims the same id. Pin `CONFIG_POT_NODE_ID` (`tools\run_qemu.ps1 -NodeId`) to run more than one | observed; `Potluck M0 node 0x0001` in `build/qemu-console.log` | 2026-08-01 | volatile | verified |
| ~~A dual-core build deadlocks a few hundred ms after `app_main`~~ | **WITHDRAWN, same day. This was our bug, not the emulator's.** `link_task` busy-spun at priority 6 on any radio-less build, because its only sleep was inside `espnow_rx_pop()` and that returns immediately when the radio is down. `stats_task` at priority 3 was starved for ever. The "frozen clock" was an artefact of `ESP_LOG` not being called again after `app_main` on that build — the statistics went out through `fputs`, from the starved task. The single QEMU-monitor sample that found CPU0 in `xPortEnterCriticalTimeout` with `SPINLOCK_FREE` in `SCOMPARE1` was **not** evidence of a deadlock: a loop taking and releasing a mutex every iteration is inside `portENTER_CRITICAL` a good fraction of the time, so one sample landing there is what a spin looks like. Fixed in `m0_main.cpp` with an explicit `vTaskDelay`; a 30 s emulated run then produces 144 statistics records | bisected with `run_qemu.ps1 -Extra "CONFIG_POT_SERIAL_LINK=n"`, then by reading `espnow_rx_pop`'s guard | 2026-08-01 | — | **withdrawn — kept as a record of the wrong conclusion, not as a fact** |
| GDB cannot be trusted to unwind a live QEMU Xtensa target | `xtensa-esp32s3-elf-gdb` against QEMU's gdbstub produced frames that cannot be real: `app_main` called from `serial_port_send`, `xTickCount` of 1,852,141,683, and locals contradicting the console output of the same run. Xtensa's windowed ABI needs the register windows spilled to unwind, which cannot be done on a running target. **A GDB backtrace from this setup is not evidence.** The QEMU monitor's `info registers` (a single frame, no unwinding) is trustworthy; anything built on top of it is not | observed directly, ELF SHA verified to match | 2026-08-01 | volatile | verified |
| QEMU's socket serial port accepts one connection per VM lifetime | `-serial tcp:127.0.0.1:5555,server,nowait` serves the first client and then stops accepting: a second `potctl` against the same run fails at *connect* with a timeout, while the console log keeps filling with statistics — so the node is fine and the socket is not. Do everything in one invocation, or restart the run. A real serial port has no such limit | observed directly, twice, on QEMU esp_develop_9.2.2_20250817 | 2026-08-01 | volatile | verified |
| One stack sample is not a diagnosis | Corollary of the two rows above, and the methodology lesson of the session: a sampled PC says where execution *was*, not that it is stuck. Establishing "stuck" needs a second sample showing no progress, or a bisection that removes the suspect. The cheapest experiment that actually resolved a day of speculation was turning one Kconfig option off | — | 2026-08-01 | stable | method |

## Name-collision scan for the rename — 2026-08-01

Candidate names for dropping the μ, each checked by live search. "Crowded" means multiple active
projects, at least one in our semantic space; a crowded name is disqualified, not merely risky.

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| `muster` is crowded, in-domain | LLNL ships "Muster: Massively Scalable Clustering" (MPI, HPC), and Giant Swarm ships an active `muster` control plane for MCP/Kubernetes tooling. Both are cluster-orchestration adjacent | github.com/llnl/muster, github.com/giantswarm/muster | 2026-08-01 | volatile | verified — disqualified |
| `murmur` is being vacated | Mumble's server was named `murmur` for years and has been **officially renamed** `mumble-server` (fixed in 1.5.517-1; Arch tracked it as FS#69334). Remaining uses are MurmurHash (an algorithm, not a product) | github.com/mumble-voip/mumble/issues/4046, bugs.archlinux.org/task/69334 | 2026-08-01 | volatile | verified — viable |
| `potluck` collides only with a dead prototype | Ink & Switch's Potluck (2022, dynamic-documents research) states the team is "not planning on developing this particular prototype any further, or turning it into a product" | inkandswitch.com/potluck | 2026-08-01 | volatile | verified — viable |
| `planktos` is moderately crowded | xuset/planktos (websites over BitTorrent, 541★), mountaindust/Planktos (academic dispersal modelling), PlanktoScope (plankton-imaging hardware). No major product, but three named projects | github.com/xuset/planktos et al. | 2026-08-01 | volatile | verified — viable with reservations |
| `pluribus` is taken twice, in-domain | Pluribus Networks: SDN company doing "distributed network fabric" across datacenter sites, 240+ customers. Plus Facebook/CMU's Pluribus poker AI | lightreading.com, packtpub.com | 2026-08-01 | volatile | verified — disqualified |
| `krill` is crowded, in-domain | NLnetLabs krill is a production RPKI certificate authority (Rust); Zero-Robotics krill is a DAG process orchestrator with health monitoring and automatic restarts — uncomfortably close to what Potluck does | github.com/NLnetLabs/krill, github.com/Zero-Robotics/krill | 2026-08-01 | volatile | verified — disqualified |
| `smelt` is a generic-name graveyard | Nine unrelated GitHub projects share it, including an ML framework, a test runner and a game engine | github.com search | 2026-08-01 | volatile | verified — disqualified |

**Outcome: the owner chose `potluck`.** Rejected candidates stay in the table above so the checks are
not repeated. `murmur` was the runner-up; if `potluck` ever has to be abandoned, start there.

One claim that was **not** verifiable and was therefore not relied on: whether `potluck` is free on PyPI
or as a GitHub org. Nothing here is published, so it did not gate the decision — but it is unregistered
work, not a checked fact, and must not be reported as one.

### The ESP32-S3 wordplay the owner asked about, and why it was declined

Not a lookup so much as a consequence of §0.1, recorded because the reasoning will be asked for again:
a name built on `esp` brands a deliberately vendor-agnostic architecture as an Espressif accessory, and
bakes one vendor's silicon into the name permanently. The search also found that the best pun in that
space is occupied by a company doing almost exactly this — Pluribus Networks ships a "distributed
network fabric" across datacenter sites. The wink lives in the tagline instead:
**e(SP)luribus unum — out of many, one machine.**

### A methodology trap that cost more than any of the above

`idf.py build` produces `potluck_m0.elf`; `qemu_flash.bin` is a **separate merge** of bootloader,
partition table and application. `tools\run_qemu.ps1` originally generated the flash image only when
it was missing, so after a rebuild QEMU booted the *previous* application while the freshly built ELF
sat beside it. Every backtrace decoded against that ELF named a plausible but entirely wrong function
— on one occasion `mbedtls_psa_crypto_init`, which this firmware never calls — and **nothing in
addr2line's output indicated the mismatch.** A whole debugging session concluded that a fix had not
worked when it had.

ESP-IDF prints `ELF file SHA256: <9 hex>` immediately before `Rebooting...` for exactly this reason.
`tools\decode_backtrace.ps1` now compares it against the ELF and **refuses to decode on a mismatch**;
`run_qemu.ps1` always regenerates the flash image. The general rule this earns:
**an address is only as good as the binary it is decoded against, and that pairing must be checked
by a machine rather than assumed by a person.**
