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

## Publication — 2026-08-01

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| License text is the canonical Apache-2.0 | Fetched from apache.org (11,358 bytes); section 4(d) is the NOTICE-preservation clause that makes it the "acknowledge me" license — redistributions of the work or derivatives must carry the NOTICE file's attribution. 4(b) additionally requires stating significant changes; section 3 grants patents, which MIT does not | https://www.apache.org/licenses/LICENSE-2.0.txt | 2026-08-01 | stable | verified |
| Public repository | https://github.com/Tubifix77/potluck — public, license auto-detected as Apache-2.0, 13 topics, initial commit = the S3 Edition release (127 files). Build trees, generated sdkconfig, local Claude settings and the two regenerable multi-MB corpora are untracked; the zero-assumption ledger and the two small M2 acceptance captures are tracked deliberately | gh repo view, 2026-08-01 | volatile | verified |

## Bench hardware — checked before buying, 2026-08-01

Registered during the hardware pause, while planning the purchase. These are facts about the
*physical boards*, not about the firmware, and each one changes either what gets bought or what the
first bench session does.

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **The onboard RGB LED's GPIO differs by board revision** | **GPIO48 on DevKitC-1 v1.0, GPIO38 on v1.1.** Espressif moved it because GPIO47/48 are supplied by the 1.8 V SPI rail used by PSRAM rather than the normal GPIO supply. Both revisions are still sold, and a listing rarely says which. **Any LED code must take the pin as a Kconfig value, never a literal** — a hardcoded 48 on a v1.1 board is a dead LED that looks like a broken driver | [v1.1 user guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html), [v1.0 user guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.0.html) | 2026-08-01 | stable | verified |
| Octal PSRAM forbids three more pins than the usual list | GPIO35, 36 and 37 **must not be used when the module has Octal PSRAM** — which is exactly the `R8` in the fleet's N16R8 part. On top of the familiar exclusions: GPIO26–32 (SPI flash / PSRAM), GPIO19–20 (native USB), GPIO0/3/45/46 (strapping) | [ESP-IDF GPIO reference](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/gpio.html), [hardware design guidelines](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/schematic-checklist.html) | 2026-08-01 | stable | verified |
| Potluck's own pin choices survive that list | `CONFIG_POT_SERIAL_TX_GPIO=17` / `RX=18` are both in the clean range, and `CONFIG_POT_BYE_BUTTON_GPIO=0` is the BOOT button — the deliberate and correct use of a strapping pin. **No pin work is needed before first bring-up**; checked because a wrong default would waste the first hardware session | derived from the two rows above against `firmware/main/Kconfig.projbuild` | 2026-08-01 | stable | verified |
| A board without a separate USB-to-UART socket cannot run this firmware | `CONFIG_ESP_PHY_ENABLE_USB=n` (set for RF accuracy) means the native USB PHY is unusable while Wi-Fi is on, so the UART socket is the only console during any radio run. Many cheap S3 boards expose only the native USB port. **Purchasing criterion, not a preference** | this repo's own config + the `CONFIG_ESP_PHY_ENABLE_USB` row above | 2026-08-01 | stable | verified |
| SN65HVD230 breakout modules carry their own 120 Ω termination | The common module includes the bus termination resistor, so a two-node CAN bus built from two of them is correctly terminated at both ends with **no separate resistors to buy**. Relevant at M4. (Three or more nodes would need termination *removed* from the middle ones) | [Waveshare SN65HVD230 CAN Board](https://www.waveshare.com/sn65hvd230-can-board.htm) | 2026-08-01 | volatile | verified — module-dependent, confirm against the actual listing |
| **…and the module actually ordered, checked against its listing** | The row above asked to be confirmed against the real listing, and now there is one. The 2026-09-21 order's photos show an SMD part marked `121` (= 120 Ω), consistent with onboard termination; a **4-pin male header in the order 3.3V / GND / RX / TX**; **no RS pin exposed**, so slope control is onboard and there is nothing to jumper; and **CANH/CANL brought out twice in parallel** — a 2-pin header *and* a screw terminal, so F-F jumpers do the bus while the terminal stays free for a probe. The pin order and the doubled bus pins are **verified** from the photos. The resistance is **inferred from a marking, not measured** — no multimeter was ordered — and at two nodes over ~20 cm it barely matters: propagation is ~1.5 ns against a 1 µs bit time | seller listing photos for the ordered part, recorded in [WHEN-THE-BOARDS-ARRIVE.md](../../WHEN-THE-BOARDS-ARRIVE.md) | 2026-09-21 | volatile | pinout verified; **termination [MEASURE]** — a multimeter across CANH/CANL settles it in ten seconds at unboxing |

## ESP-IDF configuration traps — from Powersuit, then measured here, 2026-08-09

Cross-pollination from Powersuit (sibling ESP32-S3 project, same owner), which found these by
building and running. Each was re-tested against *this* repository rather than accepted, because a
finding from ESP-IDF 5.5 is not automatically true of the v6.0.2 this project pins.

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **A Kconfig `choice` member cannot be set from an `sdkconfig.defaults` overlay** | Measured here: an overlay carrying `CONFIG_POT_BEACON_UNICAST=y` produced a build whose generated `sdkconfig` said `CONFIG_POT_BEACON_BROADCAST=y`. A plain `int` in the *same fragment* applied correctly, so the fragment mechanism works and choice members specifically are dropped — **in silence, with the build reporting success**. Fixed by flattening the choice to a plain `bool`; verified both directions end to end (`beacon mode: unicast_full_mesh` vs `broadcast_beacon` on the boot record) | Powersuit's finding, reproduced on this repo with ESP-IDF v6.0.2 | 2026-08-09 | volatile | verified |
| **An existing `sdkconfig` outranks the defaults overlay entirely** | Worse than the row above and found while fixing it: ESP-IDF treats an existing `sdkconfig` as authoritative and consults `sdkconfig.defaults` only for symbols it does not already contain. So on the *second and later* runs a changed overlay is ignored — `CONFIG_POT_NODE_ID=4097` was observed silently reverting to `0`. This makes any "vary one knob per run" tool unreliable in exactly the situation it exists for. Fixed by deleting `sdkconfig` before every configure in `run_qemu.ps1` | measured directly, this repo | 2026-08-09 | volatile | verified |
| An unselected Kconfig symbol is **undefined**, not `0` | Confirmed the hard way: flattening the choice left `CONFIG_POT_PROBE_INTERVAL_MS` with a `depends on` a symbol that no longer existed, and the build failed with *"was not declared in this scope"*. In `#if` an undefined symbol reads as 0 and compiles silently; in ordinary C it is an undeclared identifier. A knob meaningless in one mode is better left defined and ignored than made to vanish | Powersuit's finding, reproduced here | 2026-08-09 | stable | verified |
| A bare `set(SDKCONFIG_DEFAULTS ...)` before `project()` clobbers `-D SDKCONFIG_DEFAULTS` | Powersuit hit this; **this repo does not have it** — `firmware/CMakeLists.txt` contains no such `set()`, so the `-D` passed by `run_qemu.ps1` is honoured. Checked rather than assumed | Powersuit's finding, checked against `firmware/CMakeLists.txt` | 2026-08-09 | stable | verified — not applicable here |
| `uxTaskGetStackHighWaterMark()` returns **bytes** on ESP-IDF, not words | Powersuit's finding; reading it as words overstates headroom fourfold. **This repo does not call it** — the only matches are linker artefacts. Recorded now because §6's stack budgets are exactly where it would be used, and getting it wrong would mean reporting 4× the real headroom | Powersuit's finding, checked against `firmware/` | 2026-08-09 | stable | verified — not yet applicable |

### TWAI/CAN facts that scope M4

Registered before M4 is written, because they change what the milestone can even mean.

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **A lone CAN node cannot transmit successfully** | TWAI requires another node to ACK each frame; a single node retries forever and reports errors. **Any CAN work is inherently a ≥2-board activity** — no single-board CAN task can be scheduled. M4's acceptance already implies this (arbitration between competing transmitters), but the constraint should be explicit | Powersuit, observed on hardware | 2026-08-09 | stable | verified |
| One transceiver per node, never shared | N nodes on a bus needs N transceiver modules | Powersuit | 2026-08-09 | stable | verified |
| Termination is wrong by default above two nodes | Each SN65HVD230 breakout carries its own 120 Ω, so two modules terminate a two-node bus correctly — but three modules give three terminators where CAN wants exactly two, one at each physical end. Check whether the module puts its resistor on a removable jumper before buying a third. **Powersuit flags this as inference from the two-module case, not measured** | Powersuit (unverified inference) + CAN termination practice | 2026-08-09 | volatile | **unverified — confirm against the actual module before a 3-node bus** |
| ESP-IDF's `esp_driver_twai` does not copy the TX payload | The frame buffer must stay valid until the tx-done callback fires; a stack local silently corrupts frames under load. Powersuit's fix is a TX slot pool recycled by the tx-done ISR. **Observed on IDF 5.5; not re-checked against the v6.0.2 pinned here** — do that before writing M4's transmit path. Note ESP-NOW does *not* share this hazard (`esp_now_send` copies), which is why M0's TX path is safe | Powersuit, ESP-IDF 5.5 | 2026-08-09 | volatile | **carried over, unverified on v6.0.2** |

### Bench power — added 2026-08-09

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| ESP32-S3-WROOM-1 peak transmit current | **330 mA at 3.3 V**, 802.11b @ 1 Mbps, 20.5 dBm, 25 °C, measured at the RF port on a 100 % duty cycle. (The MINI-1 module is 355 mA.) An LDO passes this through from the 5 V rail nearly one-for-one, so budget ~350 mA of USB current per board at peak | [ESP32-S3 datasheet](https://documentation.espressif.com/esp32-s3_datasheet_en.html) | 2026-08-09 | stable | verified |
| A bus-powered USB hub cannot run three boards | USB 2.0 gives 500 mA per port and a bus-powered hub shares one such budget across everything downstream — against ~1050 mA of peak demand from three boards. Three *separate* PC ports (500 mA each, 900 mA on USB 3.0) or a mains-powered hub are both fine. **This is a measurement-integrity issue, not a convenience one:** the 330 mA figure is 100 % duty cycle while heartbeat traffic is short bursts, so an undersized supply sags intermittently rather than failing outright; each sag resets a board, bumps its boot epoch and stops its heartbeat, and the peer declares it dead — manufacturing exactly the false death declarations §13-M0's acceptance forbids | USB port current limits + the row above | 2026-08-09 | stable | verified |
| One USB cable per board covers power, flashing, console and capture | The USB-to-UART port supplies 5 V, carries esptool flashing, the serial console and the JSON statistics stream. No separate power supply is required for any bench configuration in this project | this repo's runbook + board design | 2026-08-09 | stable | verified |
| A USB-serial adapter on the frame link must be 3.3 V logic | An ESP32 GPIO's absolute maximum input is VDD+0.3 V (~3.6 V); 5 V drives overcurrent through the internal clamping diodes and damages the pin over time. Most CH340 modules carry a 3.3 V/5 V jumper — it must be on 3.3 V. Wiring is three lines only (GPIO17→RX, GPIO18→TX, GND→GND) with **no** power line, since the board is already supplied by its own USB cable | ESP32 datasheet absolute-maximum ratings, corroborated across [ESP32 forum discussion of 5 V tolerance](https://www.esp32.com/viewtopic.php?t=18327) | 2026-08-09 | stable | verified |

## Body-worn 2.4 GHz — registered 2026-08-09, prompted by the Powersuit topology

The owner's Powersuit is 7 ESP32-S3 nodes distributed over armour, a Pi-class node for the HUD, and a
5G link to a cloud LLM. He observed that node separation is "just the size of a person" and asked
whether 7 counts as a large network. Two findings, and the second inverts the intuition.

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| 7 nodes is the designed cell size, not a large network | §3's ESP-NOW peer ceiling is 20 *including* broadcast, and `sim/` validates 7 nodes directly: 109 frames/s, ~8.7 % airtime, zero false death declarations under the broadcast beacon. A 7-node suit is inside the envelope with margin | this repo's §3 and `sim/` sweep output | 2026-08-09 | stable | verified |
| **A human body between two nodes costs roughly 20 dB at 2.4 GHz** | Free-space path loss of ~44 dB rises to **~65 dB** when the body shadows the link at close range. The on-body path-loss *exponent* is 3–4 line-of-sight and **5–6 non-line-of-sight**, against 2 in free space; 2.4 GHz decays about twice as fast on-body as 915 MHz; limbs propagate markedly better than front-to-back across the torso. **Short range does not mean good link** — a body-worn mesh is the shortest-range and worst RF environment in this project, and ~20 dB is a factor of 100 in power | [Human body shadowing at 2.4 GHz (PMC6211019)](https://www.ncbi.nlm.nih.gov/pmc/articles/PMC6211019/), [On-body RF links 420 MHz–2.4 GHz (PMC6308834)](https://pmc.ncbi.nlm.nih.gov/articles/PMC6308834/) | 2026-08-09 | stable | verified |

**Consequence, and a decision that was already right for a different reason.** ARCHITECTURE §1.2's
Ironman-suit entry routes the suit over a **wired CAN spine**, with tight loops pinned to the node
wired to their own actuator, because sub-millisecond loops cannot span a radio (§3.1). That was a
*latency* argument. The body-shadowing figures make it independently correct on *RF* grounds too — an
on-body ESP-NOW mesh would be fighting 20 dB of torso on a large fraction of its links. No
architecture change follows; this is a second, unrelated justification for a decision already closed,
and it should be cited as such rather than re-derived.

**[MEASURE]** The bench can settle this cheaply: place two boards on opposite sides of a torso and
capture PDR, alongside the free-space distance sweep. That is the actual Powersuit link condition and
neither the farmland paper nor a desk test produces it.

### ESP-NOW alongside station mode — added 2026-08-09

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **ESP-NOW and Wi-Fi station mode coexist on one chip** | "You can send ESP-NOW data via both the Station and the SoftAP interface." A single node can be a full cluster peer *and* joined to a router — no dedicated bridge node and no cable between boards is needed | [ESP-IDF ESP-NOW](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/network/esp_now.html) | 2026-08-09 | stable | verified |
| **…but the router then dictates the whole cell channel** | "The channel must be set as the channel that station or softap is on"; a mismatch returns `ESP_ERR_ESPNOW_CHAN` ("current Wi-Fi channel does not match that of peer"). So one node joining an AP forces every other node onto that AP channel. Two consequences: a router that auto-selects channels can silently split the cell, and the cluster then shares airtime with all household Wi-Fi traffic — which is exactly the contention §13-M0 measures *without*. This is why the firmware pins `CONFIG_POT_CHANNEL` and joins no router | same | 2026-08-09 | stable | verified |
| Power save must be configured explicitly for ESP-NOW | Sleep with ESP-NOW requires `esp_wifi_connectionless_module_set_wake_interval()`; sleep is supported only in station mode. A dozing node otherwise misses frames | same | 2026-08-09 | stable | verified |

**Design note.** For a cluster that needs internet, the least-compromised bridge is usually **the host over
the serial frame link** — it is already an ordinary peer (§8.1) and already has connectivity, so no node
touches a router and no channel is forced. A node in station mode is only necessary when the cluster
must reach the internet with the host switched off.

### Ed25519, for the host-side manifest signer — added 2026-08-23

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **RFC 8032 publishes Ed25519 test vectors, and all three of section 7.1 now pass against this repo's implementation** | Test 1 (empty message), Test 2 (1 byte, `72`), Test 3 (2 bytes, `af82`), each with a secret key, public key and 64-byte signature. Reproduced verbatim in `host/potluck/tests/test_signing.py` and checked on every gate run | [RFC 8032 §7.1](https://www.rfc-editor.org/rfc/rfc8032.txt) | 2026-08-23 | stable | verified |
| The field prime, group order and cofactor used by `ed25519_ref.py` | p = 2^255 − 19; L = 2^252 + 27742317777372353535851937790883648493; the secret scalar has its low three bits cleared, bit 255 cleared and bit 254 set (§5.1.5) | same | 2026-08-23 | stable | verified |

**Why the vectors matter more than the code.** `host/potluck/potluck/ed25519_ref.py` is a
dependency-free implementation written from the specification, and the only thing that makes it
trustworthy is that the specification's own vectors pass. They are also what would let a vetted
library replace it later without anyone having to trust the swap.

**Still open, and not answered by any of this.** §13-M5's **[MEASURE]** — *"Ed25519 vs P-256 decided
on measured verify cost"* — is a bench question about the *node*, and choosing Ed25519 for the host
tool says nothing about it. Which is why `alg` is a field in every signing structure and the verifier
dispatches on it: adding P-256 when the bench answers needs no format change.

### First contact with real silicon — 2026-10-01

Facts that stopped being lookups and became observations. Three boards arrived; one was brought up.

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **The ordered boards are genuinely N16R8** | `esptool chip-id` on board A reports ESP32-S3 (QFN56) rev v0.2, dual core + LP core, 240 MHz, **embedded PSRAM 8 MB (AP_3v3)**, and `flash-id` reports **16 MB** flash with the eFuse set to quad at 3.3 V. Flash quad and PSRAM octal is the correct combination for this part. Worth confirming rather than assuming, because the listing is a clone with no Espressif seal and recycled ESP32-C3 marketing copy | the board itself, over COM3 | 2026-10-01 | stable | **measured** |
| **The boards' USB-to-UART bridge is a CH343, not a CP2102** | `USB-Enhanced-SERIAL CH343 (COM3)`, `VID_1A86 PID_55D3` — WCH, not Silicon Labs. Windows 11 enumerated it on first plug with no driver install. This **falsifies** the reasoning recorded on 2026-08-01 for preferring a CP2102 adapter, which argued that CP210x "is already required for the boards themselves". It is not, and neither part needed a driver. The preference itself still stands on the 3.3 V-native argument, which was always the load-bearing half | the board itself | 2026-10-01 | stable | **measured — supersedes an inference** |
| **Board A's identity, and therefore its node id** | MAC `b8:1f:3f:da:63:00`. The firmware derives `node_id = (mac[4] << 8) \| mac[5]`, so this board is **0x6300** — not 0x0000 or 0xFFFF, so no nudge applies. Which board answers to which id is decided by silicon, and the register lives in [WHEN-THE-BOARDS-ARRIVE.md](../../WHEN-THE-BOARDS-ARRIVE.md) | the board itself | 2026-10-01 | stable | **measured** |
| **DTR/RTS auto-reset is wired** | esptool finishes with *"Hard resetting via RTS pin"* and needed no BOOT button held to enter the bootloader. Flashing can be scripted without a human pressing anything | the board itself | 2026-10-01 | stable | **measured** |

**Still [MEASURE], and now cheap.** The SN65HVD230 termination row above is inferred from a `121`
marking. No multimeter was ordered, so it stays open — but it is ten seconds of work if one is
borrowed before M4.

### The section 6 Wi-Fi DRAM [MEASURE] is CLOSED — 2026-10-01

Open since 2026-08-01, named in section 13-M0's acceptance table, and the reason nothing past M2 was
built: 11.8 KB of the 64 KB budget was committed on paper against a number nobody had. Board A was
flashed with the radio enabled and printed it on the boot line, exactly as section 6 said it would.

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **The Wi-Fi stack costs 32,264 B of internal DRAM with ESP-NOW active on an ESP32-S3 N16R8** | 31.5 KiB, measured as `esp_wifi_init` plus `esp_wifi_start` (`DramProfile::wifi_stack_bytes()`). **Under section 6's ~40 KB trigger, so the RX ring does not shrink** and the headroom committed on paper survives | board A (MAC `b8:1f:3f:da:63:00`), fw `095f21e`, IDF v6.0.2, ESP-NOW v2, channel 1 | 2026-10-01 | stable per IDF version | **measured — this is the [MEASURE] answer** |
| ESP-NOW costs almost nothing on top of a started Wi-Fi stack | **152 B**. The expensive thing is the radio underneath it, not the protocol | same | 2026-10-01 | stable per IDF version | measured |
| Boot-to-radio total is 41,684 B, and that is **not** the figure to quote | It folds in NVS (1,424 B) and netif (7,844 B), which are Potluck's own choices rather than the radio's cost. Quoting it against the ~40 KB trigger would read as a breach that is not one — which is precisely why `dram_probe.hpp` attributes the steps separately | same | 2026-10-01 | stable per IDF version | measured |
| Steady-state free internal DRAM with the radio up | **249,936 B free, largest block 204,800 B**, 20 s after boot with one node and no peers | same | 2026-10-01 | volatile — grows with M1+ allocations | measured |

**A correction worth keeping, because the mistake is reusable.** This session first reported the
threshold as *crossed* and the warning as *unimplemented*, on the strength of a `grep` whose patterns
happened not to match `exceeds_section6_expectation`. An empty grep was read as absence. The check
exists, it is correct, and it stayed quiet because 31.5 KB is under 40 KB. Same family as the
stale-ELF and the one-stack-sample lessons: a negative result from an instrument you did not verify
is not evidence.

### The first measured link — 2026-10-01

Two boards, 20 cm apart on a desk, 26 seconds. The first PDR and RTT figures in this repository that
were **measured rather than cited**. Easiest possible conditions, so these are a ceiling and not a
characterisation.

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **ESP-NOW round-trip time between two ESP32-S3 at 20 cm** | **p50 in [4000, 6000] µs on both sides**, min 4031 / 4036 µs, remote turnaround 114–119 µs. A's p99 landed in [11000, 16000] µs; B saw one 44.6 ms outlier that put its p99 in [42000, 60000] | boards 0x6300 and 0x7368, fw `095f21e`, `captures/pair-*-first-link.log` | 2026-10-01 | volatile — desk conditions, one 26 s window | **measured** |
| **`sim/link_model.hpp` is calibrated against reality, not merely self-consistent** | The model puts the one-way delay at 2782.85 µs from §3's cited figures — a **5.57 ms round trip** — and the measured p50 bucket is [4, 6] ms. The modelled figure falls inside the measured median. Every H2 conclusion rests on this: 19 workers at 18.99x, the livelock at 20 ms units, and "a unit must run far longer than a round trip" | same, against `sim/link_model.hpp` | 2026-10-01 | volatile | **measured — model validated** |
| Packet delivery at 20 cm is 100 % in both directions | 1000000 ppm each way, 0 `lost_seqgap`, 0 `hb_lost`, 0 `cb_fail`, 0 `enqueue_err`, RSSI −21 / −22 dBm. **This is the trivial case**: a link that failed here would mean something was badly wrong. §13-M0's figure needs the 24 h soak and §3's distance sweep across the range cliff | same | 2026-10-01 | volatile | measured, and not the M0 number |
| ESP-NOW v2 negotiates the 1446-byte profile between two real boards | Both link records report `espnow_ver: 2, mtu: 1446`, so §5.3's v2 profile is reachable on this hardware rather than only on paper | same | 2026-10-01 | stable per IDF version | measured |
| **The CH343 exposes a unique serial per device** | Board A is `5CBC414102`, board B `5C93086589`, in the Windows instance id. Three identical boards are therefore individually identifiable **without unplugging anything** — this repo assumed the unplug-and-see-what-vanishes trick would be needed | Device Manager / `Get-PnpDevice` | 2026-10-01 | stable | measured |

**Two anomalies, open rather than explained.** One 26-second window diagnoses nothing, which is the
same discipline that stopped a single stack sample from diagnosing a hang here. (1) `reorder_dup` is
1 on A and **149** on B against ~785 received frames each, while every other figure is symmetric to
within a dB — first suspect is the broadcast-versus-unicast sequence accounting §5.1 separates.
(2) B emits `peer_admitted` for an already-admitted peer **every 2 s**, exactly `hello_interval_ms`,
which would make a genuine admission indistinguishable from a repeat in the event ring.

### A standing measurement condition, and a three-node cell — 2026-10-01

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **The bench sits in an RF-saturated house** | The owner's home is full of Wi-Fi and Bluetooth devices. Every PDR, RTT and loss figure measured here carries household contention in it, and `CONFIG_POT_CHANNEL=1` is among the most crowded Wi-Fi channels. §13-M0's figures **must state this condition** or they mislead. §3's range citations came from open farmland, so expect the distance sweep to be worse here without that being a contradiction. Measuring PDR across channels is legitimate M0 work | the owner, stated 2026-10-01 | 2026-10-01 | stable | stated by the owner |
| **A three-node cell converges and holds** | 0x6300, 0x7368, 0x8160 reset together: `peers_alive: 2` on every node, **0 deaths and 0 reboots_seen over 41 s**, all six links at 100 % tx PDR with 0 `lost_seqgap` and 0 `hb_lost`, RTT p50 in [4000, 6000] µs on every link, maxima 7.2–11.6 ms. RSSI −13 to −32 dBm by physical position | `captures/trio-*.log` | 2026-10-01 | volatile — desk range, 41 s | **measured** |
| **`sim/`'s airtime model validates against hardware** | Each node transmits ~13.6 frames/s; `print_sweep()` predicts ~12 for broadcast-beacon mode at n=3 (ten beacons plus a probe round per second). Together with the round-trip figure, **both load-bearing numbers in the simulator now have measurements behind them** — it was built from citations with no hardware to check against | same, against `sim/sim.cpp` | 2026-10-01 | volatile | **measured — model validated** |

**`reorder_dup` settled by experiment, and a withdrawn claim.** This session asserted the asymmetry
was *provably not* RF because PDR was 100 % and `lost_seqgap` 0. **Withdrawn** — a lost MAC-layer ACK
followed by a retry delivers a genuine duplicate with delivery still at 100 % and no sequence gap, so
RF was a live hypothesis. Three boards reset together settled it: **~20 duplicates for every peer that
booted *before* the reporting node and 1 for every peer that booted after, across all six links with
no exceptions.** RF knows nothing of boot order and does not land on exactly 20 three times. It is a
sequence-expectation artefact at admission, corroborated by `unknown_peer` counts of 2 / 24 / 44 in
boot order. Transient and harmless in effect, but `reorder_dup` is a link-health signal and for the
first seconds of every link it reports a flaky link that is not flaky. **Open, mechanism named.**

**Still open and not RF:** `peer_admitted` fires for an already-admitted peer at exactly
`hello_interval_ms` with no jitter. A congested band cannot produce a metronome.

### The CP2102 adapter, and the safe order to wire it — 2026-10-01

M1's acceptance needs `potctl` to reach a board's frame-link pins, which is what the adapter is for.
One of these facts is safety-relevant: the ESP32-S3's GPIOs are not 5 V tolerant.

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **A CP2102's UART logic level is set by its `VIO` pin, not by the chip** | `VIO` powers the serial I/O and **accepts 1.8–5 V**; output high is **V_OH ≈ VIO − 0.1 V**. So a module's signalling voltage is a property of *how that module is wired*, not a guarantee of the part. A board that ties `VIO` to USB VBUS would idle TX near 5 V | [CP2102/9 datasheet](https://www.silabs.com/documents/public/data-sheets/CP2102-9.pdf) (via search; silabs returns 403 to direct fetch) | 2026-10-01 | stable | verified |
| The ordered module claims 3.3 V signalling | Seller's description: pins are **3.3 V (<40 mA), 5 V, GND, TX, RX**; "signal pin level is 3.3 V, positive logic"; 300 bps–1 Mbps; onboard state LED plus transceiver LEDs that blink on traffic; needs the CP210x driver. The presence of a 3.3 V output pin implies an onboard regulator with `VIO` tied to it, which is the usual design — **but this is a seller's listing, not a datasheet, and the row above means it cannot be assumed** | AliExpress product description for the ordered part | 2026-10-01 | volatile | **claimed, not verified** |
| **Its 3.3 V rail cannot power a board** | **<40 mA.** An ESP32-S3 with the radio up draws several hundred. This replaces the earlier generic "do not tie two powered rails together" caution with a specific number: the pin physically cannot do it | same | 2026-10-01 | volatile | claimed |
| 921600 baud fits, with about 8 % margin | `CONFIG_POT_SERIAL_BAUD` defaults to 921600 against the module's stated 1 Mbps ceiling. If the frame link proves flaky, 460800 is a drop-in and nothing in §5.3's framing cares | same, against `firmware/main/Kconfig.projbuild` | 2026-10-01 | volatile | derived |

**Bring-up order, because one direction is safe and the other is not.** Board TX into the adapter is
3.3 V into a 1.8–5 V tolerant input: safe regardless. Adapter TX into the board is the direction that
could put 5 V on a GPIO rated to ~3.6 V. So:

1. **Loopback the adapter first** — one jumper, its own TX to its own RX, no board involved. Proves
   the adapter, its driver and the COM port before anything else can be blamed. The seller's own
   description recommends this and it is right.
2. **Then GND and board-TX → adapter-RX only.** Receive-only. Read the frame link, confirm the baud
   and the byte stream, with the adapter's output still unconnected and unable to drive anything.
3. **Only then add adapter-TX → board-RX**, which is what `potctl` needs to send READs.

No multimeter was ordered, so step 2 is how the 3.3 V claim gets tested without one: if the stream
decodes, the convention is right, and the remaining risk is confined to one wire added last.

> **~~Step 2 tests the 3.3 V claim~~ — withdrawn 2026-10-04.** It does not. Step 2 sends the
> *board's* 3.3 V into the adapter's 1.8–5 V-tolerant input, and that decodes whatever the adapter's
> own `VIO` is. It proves the wiring, the baud and the byte stream; it says nothing about the level
> the adapter will *drive*, which is the only dangerous direction. Before step 3, the adapter's TX
> level needs one of: a multimeter reading of its TX pin against GND while idle (should be ~3.3 V,
> not ~5 V); or a series resistor of 1–10 kΩ in the adapter-TX → board-RX wire, which limits the
> current into the GPIO's protection diodes if the level turns out to be 5 V; or an explicit decision
> to accept the seller's "3.3 V signal level" claim at the cost of one board if it is wrong.

**Not a datasheet.** The PDF supplied with the order (`S47e3eaf6a9ed4e11a850141ba34a132f7.pdf`) is
two pages of EU RoHS/REACH/CE compliance boilerplate for an "Integrated circuit-module" with the
model field blank. It contains no pinout, no ratings and no CAN timing, and says so itself: *"refer to
the product datasheet"*. Recorded so nobody reads it again hoping for specifications.

### The CAN transceivers are capped at 250 kbit/s by a fixed resistor — 2026-10-01

The seller's listing included the module's **schematic**, which settles two things and raises one that
affects M4's bench directly.

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **The onboard 120 Ω termination is confirmed, not inferred** | The schematic draws **R2 = 120** directly across CANH/CANL. This closes the open item that had been inferred from a `121` SMD marking and left `[MEASURE]` for want of a multimeter — a drawing is better evidence than a meter reading anyway | seller's schematic for the ordered module | 2026-10-01 | stable | **verified** |
| Header pinout confirmed a second way | P1 is a 4-pin header, order **3.3V / GND / RX / TX**, matching the photos. The schematic shows `CAN_TX` to pin 1 (`D`), GND to 2, 3.3 V to 3 (`VCC`), `CAN_RX` to 4 (`R`). CANH/CANL go to a 2-pin screw terminal | same | 2026-10-01 | stable | verified |
| ~~`Rs` = 10 kΩ caps the module at 250 kbit/s~~ | ~~"A 10 kΩ resistor gives a slew rate of approximately 1.0 V/µs", recommended maximum 250 kbit/s~~ | ~~TI datasheet~~ | 2026-10-01 | — | **WITHDRAWN the same day — both numbers were wrong. See the correction below** |
| **`Rs` is tied to GND through R1 = 10 kΩ, which selects slope-control mode** | Verified from the module's schematic. `Rs` selects high-speed (GND or open), **slope control (10 kΩ–100 kΩ to GND)**, or low-power (pull up to VCC). This half was right | the module schematic + TI SLOS346O | 2026-10-01 | stable | verified |
| **The mode cannot be changed without soldering** | `Rs` is not brought out to the header — P1 carries only 3.3V/GND/RX/TX — so changing it means removing or shorting an SMD resistor, and no iron was bought. True, but it matters far less than the withdrawn row claimed | same | 2026-10-01 | stable | verified |
| Module supply and thresholds | 3.0–3.6 V; input low ≤0.8 V, input high ≥2 V (so the S3's 3.3 V logic drives it comfortably); differential input −6 V to +6 V; −40 °C to 80 °C; 370 µA standby. The listing's graphic reads "370MA", which is wrong by three orders of magnitude — the body text says µA and that is the believable figure | seller's listing | 2026-10-01 | volatile | claimed, with one obvious typo |

**What this does and does not cost us.**

§5.3.1's capacity analysis is written at **500 kbit/s and 1 Mbit/s**, and already found that eleven
nodes under *segmentation-only* need 122 % of a 500 kbit/s bus — which is precisely why the `SINGLE`
flag exists, making a liveness beacon exactly one CAN frame instead of ten. With `SINGLE`, eleven
nodes at §8.2's 20 ms wired period need ~550 frames/s at ≤111 bits — **about 12 % of a 500 kbit/s bus
and 24 % of a 250 kbit/s one.** The car fits either way.

~~What the cap does remove is the ability to validate §5.3.1's 500 kbit/s and 1 Mbit/s rows on this
bench at all.~~ **Withdrawn — there is no cap.** Those rows *can* be validated here; see the
correction at the end of this file. The rate is chosen from the bus budget, and on a 20 cm bench the
budget is generous.

**§13-M4's acceptance survives intact.** *"A `SAFE_STATE` frame wins CAN arbitration against
saturating telemetry"* is a statement about identifier priority, which is bitrate-independent. It
demonstrates just as well at 250 kbit/s.

**Cross-project, for the sibling Powersuit bench:** its `node_bench` TWAI configuration must be set to
**250 kbit/s or below**, or the dead-man-switch test will fight the hardware rather than exercise the
firmware. Above the recommended rate the slew-limited edges cause bit errors; on a 20 cm bench it may
appear to work anyway, which is the worst outcome — out of spec and seemingly fine.

### Correction: there is no 250 kbit/s cap. Read the document, not a summary of it — 2026-10-01

Hours after writing the row above, the sibling Powersuit project challenged it from the datasheet.
It was right on every point, and the primary source settles it. Quoting **TI SLOS346O, revised April
2018**, read directly from the PDF:

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **10 kΩ on `Rs` gives ~15 V/µs, not ~1.0 V/µs** | *"a resistor value of 10 kΩ the device will have a slew rate of ~15 V/μs, and with a resistor value of 100 kΩ the device will have a slew rate of ~2 V/μs"*. The withdrawn figure was wrong by a factor of fifteen | TI SLOS346O §10.4.2 | 2026-10-01 | stable | **verified from the PDF** |
| **No data-rate limit is stated for slope-control mode** | The part is *"Designed for Data Rates up to 1 Mbps"* with no slope-control exception. **"250 kbps" occurs exactly once in a rate context**, as the caption *"Figure 41. Typical SN65HVD230 250-kbps Output Pulse Waveforms With Slope Control"* — a test condition on a waveform plot, not a recommendation | TI SLOS346O | 2026-10-01 | stable | **verified from the PDF** |
| **The trade-off slope control actually names is bus length, not bit rate** | *"a tradeoff between the total bus length able to be used and the driver's output slope used via the slope control"*. Loop delay rises with 10 kΩ (datasheet timing rows: 70–180 ns, against *"Figure 40. 70.7-ns Loop Delay ... With RS = 0"*), and delay eats the sample-point budget — which is a *length* constraint at a given rate, not a rate ceiling | TI SLOS346O | 2026-10-01 | stable | **verified from the PDF** |
| **Consequence: 1 Mbit/s is fine on a 20 cm bench, and §5.3.1's rows can be validated after all** | The withdrawn row claimed this bench could never test the 500 kbit/s and 1 Mbit/s capacity analysis. False. Pick the rate from the bus budget — length, node count, loop delay against the sample point — never from a figure caption | derived | 2026-10-01 | stable | corrected |

**The method failure, which matters more than the numbers.** The wrong claim did not come from the
AliExpress listing. It came from fetching the TI datasheet through a tool that **summarises a PDF with
a small model** and treating that prose as the datasheet. The model promoted a figure caption to a
recommendation and invented a slew rate. The real PDF was saved to disk by the same call and went
unread.

**Rule, stated so it is reusable:** a fetched *summary* is a model reading a document; it is not the
document. For any load-bearing spec claim — a rating, a limit, a threshold — extract and read the
primary text. This is the fifth time in one day that a shortcut produced a confident false claim, and
the other four were all variants of the same thing: trusting an instrument without checking it could
see what was being asked of it.

**Also worth recording: the challenge came from the other project, and it was right.** Powersuit
flagged one thing it could not verify — *"that Rs is actually 10 kΩ on these modules — reported, not
read off the board"* — and **our schematic verifies it.** Each project held the half the other was
missing.
### The 24-hour soak, the status LED, and a recorder that rebooted what it recorded — 2026-10-03

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **M0's 24-hour soak, three ESP32-S3 at ~10 cm** | 24.52 h; unicast PDR 100.0000 % outbound (791,324, MAC ACK) and inbound (6,337,757, seq gaps) on all six links; 264,850 RTTs, p50 [4, 6] ms, p99 [16, 22] ms, p99.9 [42, 60] ms, none above 85 ms; heartbeat delivery 99.69–99.84 % (reconstructed); `free_dram` identical at start and end on all three | `captures/soak-2026-10-02-report.txt`, fw `095f21e`, layout C B A | 2026-10-03 | volatile | **measured** |
| **The onboard RGB pixel takes GRB byte order and lights from the firmware's GPIO48/GPIO38 pair** | Power-up self-test sent red, green, blue in GRB order on both pins; the owner saw red, green, blue in that order, then blue, then green on joining the cell. Which of the two pins carries it on these boards is **not established** -- both are driven | owner's eyes on board B, fw `a652cab` | 2026-10-03 | stable | **measured** |
| **Opening the console port with pyserial's defaults reboots these boards** | `serial.serial_for_url("COM3", 115200)` produced a `rst:0x1 (POWERON)` banner, a new boot epoch and ~12 s of uptime in the first record; opening with DTR and RTS deasserted before `open()` left the board running (239 s uptime, epoch unchanged). Both orders tried on the same board within minutes. Why run 2 of the soak, opened by the same code, did not reboot its boards is **not established** | bisected on board A, CH343 on COM3 | 2026-10-03 | stable | **measured** |
| **RTT is unaffected by the `on_tx_done` race; only `txq_*` was** | RTT is `recv_us - probe_submit_us` (node.cpp) and never reads `probe_sendcb_us`; the race wrapped `txq_max_us` to 2^32 - 86 on both B–C links in the soak | code read, soak report | 2026-10-03 | stable | verified |

### The distance sweep — M0's kill criterion, answered indoors — 2026-10-04

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **ESP-NOW (ESP32-S3 DevKitC-1 PCB antenna, channel 1) holds across a ~10 × 20 m two-storey house** | Eight spots up to ~16 m, through up to four walls, a floor, and a tumble-dryer/fridge shadow. Unicast PDR 100 % at six spots; worst 99.17 % outbound / 99.80 % inbound. Worst p99 RTT bucket 30–42 ms. No open-field-style cliff | `captures/sweep-2026-10-04-report.txt`, fw `a652cab`, owner's house | 2026-10-04 | volatile (one house, one afternoon) | **measured** |
| **The indoor edge is brief membership dropouts, not a delivery cliff** | Worst spot: 9 deaths in 13.9 min on the wall-facing observer, each revived within seconds, unicast still 99 %. Deaths only where median RSSI ≤ −73 dBm; none at −63 dBm or better; −77 dBm at the farthest spot had none, so −73 is where they become possible, not certain | same | 2026-10-04 | volatile | **measured** |
| **Orientation and obstacles outweigh distance indoors** | Farthest spot (~16 m) clean; a spot 3 m closer behind the appliances was the worst. The wall-facing fixed board was the weaker observer at every marginal spot | same | 2026-10-04 | volatile | **measured** |
| **Sampled link state hides short dropouts** | Every one of the 9 deaths at the worst spot recovered between two 10 s samples, so `state` read `alive` throughout. Count deaths from the event stream | same, compared against the live checks | 2026-10-04 | stable | **measured** |

### ESP32-S3-DevKitC-1 headers, for wiring the frame link — 2026-10-04

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **J1 pin 10 is labelled `17` (GPIO17, U1TXD); J1 pin 11 is `18` (GPIO18, U1RXD); J1 pin 22 is `G`** | The frame link's TX and RX (`CONFIG_POT_SERIAL_TX_GPIO=17`, `_RX_GPIO=18`) and a ground on the same header | [DevKitC-1 v1.1 user guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html), Header Block tables, read from the page's HTML rather than a summary | 2026-10-04 | stable | verified |
| **J3 pins 2 and 3 are labelled `TX` / `RX`, but they are UART0 (GPIO43/44), the console** | Wiring the adapter there would put it on the console, which the CH343 already drives — not the frame link | same | 2026-10-04 | stable | verified |
| **Full header order, J1 1→22:** 3V3, 3V3, RST, 4, 5, 6, 7, 15, 16, 17, 18, 8, 3, 46, 9, 10, 11, 12, 13, 14, 5V, G. **J3 1→22:** G, TX, RX, 1, 2, 42, 41, 40, 39, 38, 37, 36, 35, 0, 45, 48, 47, 21, 20, 19, G, G | Clone boards usually copy this, but a pin is identified by its **printed label**, never by counting positions | same | 2026-10-04 | stable | verified |
| **The owner's CP2102 adapter: pin order 3V3, TXD, RXD, GND, +5V (top to bottom)** | Read off the adapter's silkscreen by the owner. Differs from the order in the seller's text (3.3V, 5V, GND, TX, RX), which listed the pins, not their positions | owner, from the board | 2026-10-04 | stable | **measured** |
| **Board A's printed header labels match Espressif's DevKitC-1 v1.1 table pin for pin** | The owner checked both headers against `bench/m1-wiring.html`: J1 pins 10/11/22 are `17`/`18`/`G`; J3 starts `G`, `TX`, `RX` | owner, from board A | 2026-10-04 | stable | **measured** |
| **The CP2102 adapter works, on COM6, at the frame link's 921600 baud** | Loopback (TXD to RXD): 4,096 random bytes returned byte-identical at 921600 and at 115200. Needed Silicon Labs' *CP210x Universal Windows Driver* v11.6.0 installed through Device Manager; the package's .bat reported success but left no driver in the store (device code 28) | loopback on the owner's PC | 2026-10-04 | stable | **measured** |
| **~~Step 2 (board TX into the adapter, receive-only) can be verified by listening~~ — withdrawn 2026-10-04** | It cannot. The node sends to the host only after the host's HELLO has made it a peer (`hal_send` routes to the UART only for `kHostMac`, and the node adds a peer when it receives a HELLO); its beacons go to ESP-NOW broadcast. With only board-17 → adapter-RXD wired, 12 s at 921600 received 0 bytes, which is the correct behaviour. The receive path can only be confirmed once the host can send, i.e. in step 3 | `firmware/main/m0_main.cpp` `hal_send`, measured on COM6 | 2026-10-04 | stable | verified |

### The adapter cannot drive 5 V: the CP2102's UART runs from its own 3.3 V — 2026-10-04

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **~~A CP2102's UART logic level is set by its `VIO` pin (1.8–5 V)~~ — corrected for this chip** | The original **CP2102 has no VIO pin**: the word does not occur in its datasheet. The UART I/O is powered from **VDD, 3.0–3.6 V** (absolute max 4.2 V), which is the chip's own 3.3 V regulator output. The VIO behaviour belongs to other members of the family, not this part | Silicon Labs *CP2102* datasheet Rev. 1.2, Tables 2, 3 and 5, read from the PDF text ([SparkFun-hosted copy](https://cdn.sparkfun.com/datasheets/BreakoutBoards/CP2102_v1.2.pdf); silabs.com refuses scripted downloads) | 2026-10-04 | stable | verified |
| **CP2102 TXD output high is at most VDD, so at most 3.6 V** | V_OH = VDD − 0.1 V at 10 µA, VDD − 0.7 V at 3 mA. Inputs tolerate up to 5.8 V | same, Table 3 and absolute maximums | 2026-10-04 | stable | verified |
| **The owner's HW-598A adapter reports itself as an original CP2102** | Windows device name *"CP2102 USB to UART Bridge Controller"*, `USB\VID_10C4&PID_EA60&REV_0100`. A clone that copies the name could still differ; this is the device's own claim, not a die photo | Device Manager / PnP query on the owner's PC | 2026-10-04 | stable | measured |
| **Consequence: adapter TXD → board GPIO18 can be wired directly** | 3.6 V maximum against the ESP32-S3's 3.3 V I/O; no series resistor required. Residual risk is a counterfeit chip that both copies the name and breaks the datasheet | derived from the rows above | 2026-10-04 | stable | derived |
| **M1 accepted on hardware** | potctl via CP2102 on board A read board B's `sys/uptime`: value + unit + age + class while B lived; `UNAVAILABLE` with no value while B was unplugged; recovered on replug. Same read works for C | M0-LOG session 16, fw `12ab64d` on all three | 2026-10-04 | volatile | **measured** |
| **M2 accepted on hardware** | A 10.05-min session over the CP2102 frame link, 3 boards / 18 entries (12 via replicas), 14,628 raw frames, 4,428 reads, 0 timeouts; replay → sha256 `bd735ccb…` matches (exit 0), wrong digest exit 6 | `captures/m2-hw-10min.jsonl`, M0-LOG session 17 | 2026-10-04 | volatile | **measured** |
| **M3 accepted on hardware** | Purple (counter 2) deployed to 3 nodes through A, survived host unplug + power cycle on a mains charger; broken module (fault at 3 s, counter 3) -> each node 3 trial boots, then reverted to counter 2 by itself | `captures/m3-broken-revert-*.log`, M0-LOG session 18, fw `0dd9645` | 2026-10-04 | volatile | **measured** |
| **The owner's SN65HVD230 modules: 4-pin header top→bottom TX, RX, GND, 3.3V; 2-pin CAN header top→bottom CANL, CANH** | Read off the modules by the owner, 2026-10-04. Agrees with the photo/schematic order (3.3V/GND/RX/TX) read from the other end. Module TX = CAN_TX = the transceiver's driver input D, RX = its receiver output R (schematic, row above): **ESP32 CAN-TX → module TX, CAN-RX → module RX, straight, not crossed** | owner, from the modules + the module schematic | 2026-10-04 | stable | **measured** |

### M4 on two boards: CAN transmit order and SAFE_STATE latency — 2026-10-05

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **The ESP-IDF v6 TWAI driver transmits in FIFO order** | `twai_node_transmit()` pushes a frame pointer on `tx_mount_queue` (a FreeRTOS queue); `on_tx_done` pops the next in order | `esp_driver_twai/esp_twai_onchip.c`, local ESP-IDF v6 tree | 2026-10-04 | per IDF version | verified |
| **The arbitration-lost interrupt is enabled by that driver on the S3** | `TWAI_LL_DRIVER_INTERRUPTS` includes `TWAI_LL_INTR_ALI`; the HAL maps it to `err_flags.arb_lost` | `esp_hal_twai/esp32s3/include/hal/twai_ll.h`, `twai_hal_v1.c` | 2026-10-04 | per IDF version | verified |
| **Measured: FIFO transmit, SAFE_STATE 2,522–5,531 µs under flood** | B ~3,050 frames/s; C's SAFE_STATE queued behind its own 10-frame probe | `captures/m4-can-two-board-fifo-0x7368-0x8160.log` | 2026-10-04 | bench | measured |
| **Measured: priority transmit, SAFE_STATE 335–683 µs under flood, worst 1,056 µs since boot** | B 3,072 frames/s for 5 min; 301 consecutive SAFE_STATEs received; B lost 44 arbitrations, C 0; zero bit/form/stuff errors | `captures/m4-can-two-board-priority-5min.log`, `-report.txt` | 2026-10-05 | bench | measured |
| **[MEASURE] SAFE_STATE winning arbitration, on a scope or logic analyzer** | Open: no instrument on the bench | — | — | — | open |

### M5: Ed25519 against P-256 on the board — 2026-10-05

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **mbedTLS 4.1.0 in ESP-IDF v6.0.2 implements no EdDSA** | Only the PSA constants (`PSA_ALG_PURE_EDDSA`, twisted-Edwards family) exist; no implementation file under `tf-psa-crypto/drivers` or `core` handles them. X25519 (Montgomery) and P-256 ECDSA/ECDH are implemented | local ESP-IDF v6.0.2 tree: `components/mbedtls/mbedtls/tf-psa-crypto` | 2026-10-05 | per IDF version | verified |
| **The ESP32-S3 has an MPI (bignum) accelerator and no ECC/ECDSA peripheral** | `SOC_MPI_SUPPORTED 1`, `SOC_SHA_SUPPORTED 1`; no `SOC_ECDSA_SUPPORTED` / `SOC_ECC_SUPPORTED` | `components/soc/esp32s3/include/soc/soc_caps.h` | 2026-10-05 | per IDF version | verified |
| **Monocypher 4.0.3: dual 2-clause BSD / CC-0; Ed25519 (RFC 8032, SHA-512) is the optional module** | Vendored byte-identical, blob ids recorded | `firmware/components/monocypher/PROVENANCE.md`, upstream tag 4.0.3 | 2026-10-05 | pinned | verified |
| **Measured: Ed25519 verify 27.0 ms, P-256 ECDSA verify 441.5 ms (160 MHz, -Os); 39.0 / 325.9 ms with -O2** | Also sign 9.3 / 222.8 ms; X25519 19.0 ms (Monocypher) vs 209.3 ms (mbedTLS) vs P-256 ECDH 205.4 ms; HMAC-SHA256 64 B 0.17 ms; known-answer checks passed first | `captures/m5-crypto-bench-size-2e69038.log`, `-perf-` | 2026-10-05 | bench | measured |

### M5 step 3 on the boards — 2026-10-07

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **An Ed25519 operation does not fit in the link task's 4 KB stack alongside its own work** | fw `5e44950`, first enrolled boot: `A stack overflow in task pot_link has been detected` on A and C, boot loop; B (unenrolled, no verification) unaffected | `captures/m5-step3-overflow-5e44950.log` | 2026-10-07 | stable per build | measured |
| **With crypto on the console task: link task keeps 748 B free, crypto worker 1,252-1,300 B** | fw `84a4763`, 70 s, two enrolled peers verified, one unenrolled refused | `captures/m5-step3-auth-84a4763.log` | 2026-10-07 | volatile | measured |
| **A full HELLO check (certificate + signature + X25519 session key) costs 81-87 ms on the board** | includes up to 10 ms of waiting for the worker's console poll; once per peer per boot, repeats recognised by digest | same | 2026-10-07 | volatile | measured |

### M5 step 4 design sources: frame tags and the replay window — 2026-10-07

Read from the primary texts (RFCs downloaded as plain text from rfc-editor.org and grepped; libsodium's
doc fetched as raw markdown from its repository), not from a summary.

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **ESN: only the low-order bits are transmitted; the high-order bits are kept by both ends and included in the integrity computation** | "Only the low-order 32 bits of the sequence number are transmitted ... The high-order 32 bits are maintained as part of the sequence number counter by both transmitter and receiver and are included in the computation of the ICV" | [RFC 4303 §2.2.1](https://www.rfc-editor.org/rfc/rfc4303.txt) | 2026-10-07 | stable | verified |
| **Receiver's choice of the high-order bits (Seqh) relative to window top T and size W** | Case A (Tl >= W-1): Seql >= Tl-W+1 gives Th, else Th+1. Case B: Seql >= Tl-W+1 gives Th-1, else Th. The window is updated only if integrity verification succeeds | RFC 4303 Appendix A2.2, A2.3 and §3.4.3 | 2026-10-07 | stable | verified |
| **Anti-replay window size** | "A minimum window size of 32 packets MUST be supported ... a window size of 64 is preferred and SHOULD be employed as the default" | RFC 4303 §3.4.3 | 2026-10-07 | stable | verified |
| **HMAC truncation guidance** | "We recommend that the output length t be not less than half the length of the hash output ... and not less than 80 bits". **Potluck's 64-bit tag is below this**: the 8 bytes were reserved in the v1 frame from M0 (`kAuthTagSize`), and ARCHITECTURE §9.5 already states 64-bit tags as a limitation. Recorded as a known deviation, not an oversight | [RFC 2104 §5](https://www.rfc-editor.org/rfc/rfc2104.txt) | 2026-10-07 | stable | verified |
| **libsodium on one key pair for Ed25519 and X25519** | Conversion is supported "so that the same key pair can be used both for authenticated encryption ... and for signatures", but "If you can afford it, using distinct keys for signing and for encryption is still highly recommended". Step 3's shared identity key is a HELLO-size trade against this advice | [libsodium-doc, ed25519-curve25519.md](https://raw.githubusercontent.com/jedisct1/libsodium-doc/master/advanced/ed25519-curve25519.md) | 2026-10-07 | stable | verified |
| **Monocypher 4.0.3 provides HMAC-SHA-512** | `crypto_sha512_hmac(hmac[64], key, key_size, message, message_size)` in the optional Ed25519 module; there is no SHA-256 in Monocypher. Hence Potluck's tag is HMAC-SHA-512 truncated to 64 bits, not §9.3's HMAC-SHA256 | `firmware/components/monocypher/monocypher-ed25519.h` (vendored, unmodified) | 2026-10-07 | pinned | verified |

### M5 steps 4-6 on the boards — 2026-10-07

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **A recorded WRITE re-sent over the air is rejected; the setpoint does not move** | C: `frame_rejected` reason 1 (replay) at seq 147, setpoint 7, writes served 2, after A re-sent its recorded frame unchanged and bit-flipped | `captures/m5-step4-replay-0429f5f.log`, fw `0429f5f` | 2026-10-07 | volatile | measured |
| **Frame tag check costs at most 0.83 ms per frame on the board** | `tag_us_max` 816-832 µs (HMAC-SHA-512, software, 160 MHz) | same | 2026-10-07 | volatile | measured |
| **A genuine SAFE_STATE recorded before a reset of all three boards is refused after it** | C restored its high-water (A, epoch 138, counter 1) from NVS; the replay was refused (reason 4); a new epoch-139 frame was accepted. Reset by the EN line, not by removing power | `captures/m5-step5-safe-state-power-cycle-23fab51.log`, fw `23fab51` | 2026-10-07 | volatile | measured |
| **A signed SAFE_STATE costs ~31 ms to verify on the board** | `verify_us_max` 30,453-32,012 µs (Ed25519 plus the hop to the crypto worker) | same | 2026-10-07 | volatile | measured |
| **The node refuses unsigned, rogue-CA and downgraded deploys itself** | `UNSIGNED` at COMMIT; `BAD_SIGNATURE` at COMMIT for a deploy certificate from another CA; `DOWNGRADE` at BEGIN for counter 2; counter 4 signed was committed on A and C, and C booted slot B at counter 4 with its trial confirmed | `captures/m5-step6-deploy-94da774.log`, fw `94da774` | 2026-10-07 | volatile | measured |
| **The node caps the host's payload at the 226-byte v1 profile** | a 236-byte DEPLOY_CHUNK from the host went unanswered; 220-byte chunks were accepted. `PeerLink::max_payload()` pins v1 unless the peer is v2, and the host on the serial link is not pinned v2 | the bench, and `firmware/components/pot_link/src/link_stats.cpp` | 2026-10-07 | stable | measured |

### M5.1 CR-1 design sources: following a channel change — 2026-10-07

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **A station sees its home channel change as an event** | `WIFI_EVENT_HOME_CHANNEL_CHANGE`, argument `wifi_event_home_channel_change_t {old_chan, old_snd, new_chan, new_snd}`, "doesn't occur when scanning" | `components/esp_wifi/include/esp_wifi_types_generic.h`, ESP-IDF v6.0.2 | 2026-10-07 | per IDF version | verified |
| **`esp_wifi_set_channel` constraints** | call after `esp_wifi_start()`; in STA mode not while scanning or connecting to an AP; on a SoftAP with connected stations it starts a CSA; the channel is not stored in NVS | `components/esp_wifi/include/esp_wifi.h`, attention notes 1-5 | 2026-10-07 | per IDF version | verified |
| **ESP-NOW peer channel 0 means the current channel** | "If the value is 0, use the current channel which station or softap is on. Otherwise, it must be set as the channel that station or softap is on" | `components/esp_wifi/include/esp_now.h`, `esp_now_peer_info.channel` | 2026-10-07 | per IDF version | verified |
| **Default country is world-safe mode, channels 1-11** | `{.cc="01", .schan=1, .nchan=11, .policy=WIFI_COUNTRY_POLICY_AUTO}`; `ieee80211d_enabled` TRUE by default | `components/esp_wifi/include/esp_wifi.h` (notes on country APIs) | 2026-10-07 | per IDF version | verified |

### M5.1 on the boards — 2026-10-07

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **PSRAM on these boards: octal, 8 MB, AP gen 3, 40 MHz by default** | `octal_psram: vendor id 0x0d (AP)`, `density 0x03 (64 Mbit)`, `Found 8MB PSRAM device`, `Speed: 40MHz`, memory test OK, 8,115 KB added to the heap | `captures/m51-cr5-psram-membench.log`, board B | 2026-10-07 | stable | measured |
| **Copy throughput, 160 MHz CPU, PSRAM at 40 MHz** | internal→internal 239 MiB/s; PSRAM→internal 31; internal→PSRAM 17.5; PSRAM→PSRAM 11.2 (64 KB internal / 256 KB PSRAM blocks). 16 KB blocks measured the data cache instead (~230-247 MiB/s for all) | same | 2026-10-07 | volatile | measured |
| **`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` moves lwIP's BSS (12,828 B) to PSRAM** | External RAM `.bss` 12,828 B with no Potluck buffer there; `liblwip.a` objects listed under `.ext_ram.bss` in the map | `firmware/build-psram/potluck_m0.map`, size report | 2026-10-07 | per IDF version | measured |
| **Adjacent 2.4 GHz channels leak into each other at close range** | at ~10 cm, board C scanning locked onto channel 10 hearing board A on 11; with the channel declared in HELLO it corrected to 11 in 1.1 s | `captures/m51-cr1-channel-e6541ae-adjacent-lock.log`, `-b04c987` run | 2026-10-07 | volatile | measured |
| **Re-convergence after an unannounced channel change: ~5 s** | lost-to-found 3.9 s (3 s alone + 4 hops), then 1.1 s to adopt the station's declared channel | `captures/m51-cr1-channel-*` | 2026-10-07 | volatile | measured |

### ADR-009, the relay, on the boards — 2026-10-07

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **Through one relay, an authenticated WRITE lands two hops away** | A wrote 99 to B's `act/setpoint` while A and B could not hear each other (bench instrument) and C relayed; B applied it; `bad_tag` 0 and `untagged` 0 on both ends | `captures/m51-cr2-relay-8032bc2.log`, fw `8032bc2` | 2026-10-07 | volatile | measured |
| **A capture taken through the relay replays byte-identical** | 3-min soak via A's cable, B behind the relay: 1,350 reads, 0 timeouts; replay digest `4d716fe205447b5d0a982013eeedeaa03c6a01f238b001434b1d8bb5813cb761` matches (exit 0), wrong digest exit 6 | `captures/m51-cr2-relay-session-8032bc2.jsonl` | 2026-10-07 | stable for that capture | measured |

### M5.1 step 0 — 2026-10-07

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **esp32_nat_router S3 install** | prebuilt `firmware_esp32s3/` (bootloader 17,664 B, partition table 3,072 B, ota data 8,192 B, app 1,374,176 B; build `699e5c4`, 2026-10-06); offsets 0x0 / 0x8000 / 0xf000 / 0x20000; first boot offers open AP `ESP32_NAT_Router`, config at http://192.168.4.1; "Expect something in the range from 5 - 15 mbps"; settings in NVS survive reflashing unless erased | github.com/martin-ger/esp32_nat_router README and wiki Installation.md, read as raw markdown | 2026-10-07 | volatile | verified |
| **Google Home app's documented Wi-Fi detail is the band, not the channel** | help page describes seeing which band a device uses; no channel display described | [Google Nest help 6293481](https://support.google.com/googlenest/answer/6293481) | 2026-10-07 | volatile | verified (absence in that page only) |
| **Beside a phone streaming video through the repeater, the cell declared no false death** | 16 min, A↔C: deaths 0; heartbeat delivery 99.26 % / 99.06 % (control 99.45 % / 99.45 %); RTT p50 unchanged; p99 22-30 / 85-110 ms (control 30-42 / 16-22); max 157 ms | `captures/soak-step0-*`, owner's house, channel 1 | 2026-10-07 | volatile | measured |

### M6, the reconciler — 2026-10-07/08

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **Rendezvous (HRW) hashing** | Thaler & Ravishankar, "Using Name-Based Mappings to Increase Hit Rates", IEEE/ACM ToN 6(1):1-14, 1998: HRW "always maps a given object name to the same server within a given cluster" and "adapts well to changes in the set of servers" | [Microsoft Research publication page](https://www.microsoft.com/en-us/research/?p=362984) (abstract only; the paper itself not read) | 2026-10-07 | stable | verified (abstract) |
| **Fencing token** | "a fencing token is simply a number that increases (e.g. incremented by the lock service) every time a client acquires the lock"; the server "rejects the request with token 33" after processing 34 | [Kleppmann, How to do distributed locking (2016)](https://martin.kleppmann.com/2016/02/08/how-to-do-distributed-locking.html) | 2026-10-07 | stable | verified |
| **MurmurHash3 fmix32** | `h ^= h >> 16; h *= 0x85ebca6b; h ^= h >> 13; h *= 0xc2b2ae35; h ^= h >> 16`; "placed in the public domain" | [smhasher MurmurHash3.cpp](https://raw.githubusercontent.com/aappleby/smhasher/master/src/MurmurHash3.cpp) | 2026-10-07 | stable | verified |
| **`EXT_RAM_BSS_ATTR` is empty without `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`** | `#else #define EXT_RAM_BSS_ATTR` | local ESP-IDF v6.0.2 `components/esp_common/include/esp_attr.h:142-147` | 2026-10-07 | version-pinned | verified |
| **PSRAM is inaccessible while the flash cache is off; BSS in PSRAM forbids booting without it** | "When flash cache is disabled (for example, if the flash is being written to), the external RAM also becomes inaccessible"; with BSS external "the option to ignore failure is not available" | local ESP-IDF v6.0.2 `docs/en/api-guides/external-ram.rst:212,236` | 2026-10-07 | version-pinned | verified |
| **A flash write from a PSRAM buffer is copied through internal RAM** | `direct_write |= esp_ptr_in_dram(buffer)`; otherwise `memcpy(temp_buf, buffer, write_len)` in 32-byte chunks | local ESP-IDF v6.0.2 `components/spi_flash/esp_flash_api.c:1114-1141` | 2026-10-07 | version-pinned | verified |
| **Internal core static RAM, PSRAM build** | 49.4 KB (50,563 B) of 64 KB; no-PSRAM build 63.6 KB with the reconciler compiled out | `firmware/size/esp32s3/`, `firmware/size/esp32s3-nopsram/` | 2026-10-08 | volatile | measured |
| **A rebooted, enrolled board admits its peers 1.9-3.5 s after the reconciler starts** | `peer_admitted` at_ms 1,884-3,534 after boot in three returns | `captures/m6-failover-207751e.jsonl` | 2026-10-08 | volatile | measured |
| **Portable actor failover, reads through A** | served by another node 0.46-0.69 s after the runner is held in reset (15 kills); 0 good reads of the dead node's value; 8 returns by handover, 0 starts under another's live claim; partition healed in 0.38 s; 0 term regressions at consumers | `captures/m6-failover-*`, `captures/m6-partition-fd1478d*` | 2026-10-08 | volatile | measured |

### M6.1 (CR-6) — 2026-10-08

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **ESP-NOW over STA or SoftAP; channel 0 = current channel** | "You can send ESP-NOW data via both the Station and the SoftAP interface"; "If the channel is set to 0, data will be sent on the current channel" | local ESP-IDF v6.0.2 `docs/en/api-reference/network/esp_now.rst:93,97` | 2026-10-08 | version-pinned | verified |
| **`esp_wifi_set_channel` in STA+softAP mode** | "should not be called when STA is scanning or connecting to an external AP or softAP has connected to external STAs" | local ESP-IDF v6.0.2 `components/esp_wifi/include/esp_wifi.h`, attention 4 | 2026-10-08 | version-pinned | verified |
| **`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`** | "Try to allocate memories of WiFi and LWIP in SPIRAM firstly. If failed, allocate internal memory"; default n | local ESP-IDF v6.0.2 `components/esp_psram/Kconfig.spiram.common:109-112` | 2026-10-08 | version-pinned | verified |
| **Wi-Fi reason 15 / 201** | 15 = `WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT` (in practice a wrong password); 201 = no AP found | local ESP-IDF v6.0.2 `esp_wifi_types_generic.h` | 2026-10-08 | version-pinned | verified (15 read; 201 from the same enum) |
| **ESP-IDF's console (linenoise) asks the terminal for the cursor position (`ESC[6n`) and swallows input until answered; started with no terminal answering, it falls back to a plain mode** | observed on board B: commands unanswered until `ESC[1;1R` / `ESC[1;250R` were returned; after a reset with nothing answering, "does not support escape sequences" and every command understood | bench, M0-LOG session 27 | 2026-10-08 | volatile | measured |
| **Extender hotspot baseline (no Potluck)** | 18-19 Mbps down, 14-15 up, latency 8-12 / 43-47 ms (fast.com, 3 runs); 10-min stream, no reboot, 0 reconnects, channel 1, RSSI -47..-52 dBm | `captures/m61-step0-*` | 2026-10-08 | volatile | measured |
| **An erased board restarts its boot epoch; peers that remember the old one refused its re-enrolled HELLO** | A and C: `stale_epoch` 539-540, `bad_tag` ~1,055 until the fix | bench, M0-LOG session 27 | 2026-10-08 | volatile | measured |
| **An extender station hunting for a missing router takes the radio off the cell's channel** | reason 201 every ~4.8 s; peers declared it dead and revived it 10 times in 32 s | bench, M0-LOG session 27 | 2026-10-08 | volatile | measured |

### M8 — 2026-10-08

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **`ESP_ERR_ESPNOW_NO_MEM` = 0x3067** | `ESP_ERR_WIFI_BASE` 0x3000; `ESP_ERR_ESPNOW_BASE` = base + 100; `NO_MEM` = ESPNOW base + 3, "Out of memory" | local ESP-IDF v6.0.2 `esp_now.h:18-21`, `esp_err.h:39` | 2026-10-08 | version-pinned | verified |
| **With Wi-Fi/lwIP in PSRAM, TX buffers are static** | `ESP_WIFI_DYNAMIC_TX_BUFFER` "depends on !(SPIRAM_TRY_ALLOCATE_WIFI_LWIP && !SPIRAM_IGNORE_NOTFOUND)"; the cache TX queue (default 32) holds what a full static pool cannot take | local ESP-IDF v6.0.2 `components/esp_wifi/Kconfig:77-116` | 2026-10-08 | version-pinned | verified |
| **A board's service client degrades honestly when the host goes** | host pulled: degraded, output STALE 5 -> 37 s, never refreshed; host back: serving, GOOD; 130 calls served, 0 lost, 0 timed out | `captures/m8-svc-db91dc6*` | 2026-10-08 | volatile | measured |
| **Static TX buffers + AMPDU wedge ESP-NOW on the S3 (bench)** | extender config on board B: every send `ESP_ERR_ESPNOW_NO_MEM` from 157-275 s into a boot, for good; AMPDU off -> 0 failures in 10 min; dynamic TX with AMPDU -> 0 failures in 10 min; no ESP-IDF documentation found linking them | `captures/m8-b-*`, `captures/m8-bisect-*`, M0-LOG session 28 | 2026-10-08 | volatile | measured |
| **RTT host <-> board A over the CP2102 frame link** | 52-72 ms round trip at 921,600 baud; host turnaround 12-26 us | bench, M0-LOG session 28 | 2026-10-08 | volatile | measured |

### M6.1 step 2 — 2026-10-08

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **Combined node vs hotspot alone** | 18-19 / 14-17 Mbps, latency 10-12 / 20-27 ms vs 18-19 / 14-15, 8-12 / 43-47; 10-min stream clean; 0 false deaths in 14 min; ESP-NOW NO_MEM 1.4 % of sends in self-clearing bursts; heartbeat delivery 98.4-99.4 % | `captures/m61-step2-*` | 2026-10-08 | volatile | measured |
| **House mesh direct (Google Wifi)** | 640 / 240 Mbps, latency 10 / 44 ms (owner's phone, fast.com) | owner, 2026-10-08 | 2026-10-08 | volatile | measured (owner) |
| **Google Wifi restart can change the 2.4 GHz channel** | twice from 1 to 11 | bench | 2026-10-08 | volatile | measured |
| **Cell follows a router channel change (after the sweep fix)** | ~2.7 s from losing the station to finding it on the new channel | `captures/m61-retest-*` | 2026-10-08 | volatile | measured |

### Session 30 — 2026-10-08 (router gone, step 0 rerun)

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **Router gone, with pme_hotspot's back-off (f1624de)** | 15 / 19 deaths of B (A / C) in 10.9 min, reconnects 2 -> 15; was 10 deaths in 32 s | `captures/m61-norouter-*` | 2026-10-08 | volatile | measured |
| **Off-channel time of one never-associated reconnect attempt** | 3.7-4.0 s of silence (death at 0.6 s, revived 2.2-3.3 s later) | `captures/m61-norouter-COM3.jsonl` | 2026-10-08 | volatile | measured |
| **Hotspot alone on the aligned config (pme f1624de)** | 16-18 / 16-17 Mbps, latency 7-11 / 26-33 ms; 10-min stream clean, 0 reconnects | `captures/m61-step0b-*` | 2026-10-08 | volatile | measured |
| **Combined node, 1.96 h soak** | 0 deaths / revivals / reboots; 0 reconnects; heartbeat delivery 99.74-99.91 %; RTT p50 4-6, p99 8-11 ms | `captures/soak-2026-10-08-combined-*` | 2026-10-08 | volatile | measured |
| **Phantom unicast loss of a late-joining board** | ~12,500 per peer before `c70756f`; 0 after (B reset into a running cell) | `captures/soak-2026-10-08-combined-COM4.jsonl`, `captures/s30-verify-COM4.jsonl` | 2026-10-08 | stable | measured |

### Roadmap review — 2026-10-08 (M8.1, M9, M10)

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **ESP32-S3 CoreMark (current)** | "Two cores at 240 MHz: 1329.92 CoreMark; 5.54 CoreMark/MHz" -- no single-core figure given | Espressif, *ESP32-S3 Series Datasheet* v2.2 (2026-03-05), section CPU and Memory; https://documentation.espressif.com/esp32-s3_datasheet_en.pdf | 2026-10-08 | stable | verified |
| **ESP32-S3 CoreMark (superseded)** | 613.86 single core, 1181.60 dual core at 240 MHz -- the figures before the datasheet's v2.0 (2025-04-24: "Updated the CoreMark score"); still on distributor pages | datasheet v2.2 revision history; Mouser ESP32-S3 product page (secondary) | 2026-10-08 | stable | verified (superseded) |
| **ESP32-S3 internal temperature sensor** | driver `temperature_sensor_install/_enable/_get_celsius/_disable/_uninstall`; `_get_celsius` "Should not be called from interrupt"; S3 ranges (min, max, error): (50,125,3) (20,100,2) (-10,80,1) (-30,50,2) (-40,20,3) | ESP-IDF v6.0.2 source: `components/esp_driver_tsens/include/driver/temperature_sensor.h`, `components/esp_hal_ana_conv/esp32s3/temperature_sensor_periph.c` | 2026-10-08 | stable | verified |
| **...measures the die, not the room** | "designed primarily to measure the temperature inside the silicon"; "not recommended to use it for ambient temperature measurement"; driver not thread-safe | https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/temp_sensor.html (page is v6.1) | 2026-10-08 | stable | verified |

### M8.1 bench — 2026-10-08 (session 31)

| claim | value | source | retrieved | freshness | status |
|-------|-------|--------|-----------|-----------|--------|
| **Die temperatures, idle cell** | A 46-47, B 44.5, C 41 degC (ESP-IDF temperature_sensor, -10..80 range) | `captures/m81-die-temp-via-A.txt`, stats lines | 2026-10-08 | volatile | measured |
| **Remote read of a pinned actor's output, through A** | GOOD with age ~1 s; FAULTY within 1 s of the fault, no number; UNAVAILABLE within 1 s of B leaving; GOOD 8 s after B returns | `captures/m81-die-temp-via-A*` | 2026-10-08 | volatile | measured |
| **The house mesh offers the SSID on channels 1 and 11** | B's station joined 11 on 3 of 6 boots in one run, 0 of 16 an hour later | `captures/m81-rejoin.txt`, session 31 | 2026-10-08 | volatile | measured |
| **CR-7 boot picks at board B (bench)** | 15/15 "preferred" (channel 11, -50..-55 dBm) against the strongest (channel 1, -45..-49 dBm): 1-8 dB below, inside the 10 dB default | `captures/m81-cr7-reboots.txt` | 2026-10-08 | volatile | measured |
| **Rejoin after a reboot of the extender, cell on its channel** | 6-18 s over 6 reboots with the re-key fix (was never, 3 of 3, when the reboot split the cell) | `captures/m81-cr7-reboots.txt` | 2026-10-08 | volatile | measured |
