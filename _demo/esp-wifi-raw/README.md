# esp-wifi-raw: the S31 WiFi blob on the llgo fiber runtime, no ESP-IDF

`llgo build -target esp32s31-flash` links the closed-source ESP32-S31 WiFi
libraries (libpp / libnet80211 / libphy / libmesh / ... from the pinned
`espressif/idf:latest` image) directly into a bare-metal firmware. There is
no ESP-IDF project, no FreeRTOS, no heap_caps: the OS layer the blob expects
is the `wifi_osi_funcs_t` table (OS adapter v0x9) implemented in
`osal/`, mapped onto the llgo cooperative fiber runtime.

This is the P1b (wifi-osal) runnable artifact. The P0 research that scoped
it (symbol density, ISR analysis, prior art: TinyGo espradio / NuttX /
Zephyr / esp-radio) lives in the session artifacts under `wifi-osal/`.

## Layout

    idfparts/            pinned inputs (see idfparts/MANIFEST.md)
      lib/*.a            closed-source archives - gitignored, refetch via
                         the docker command in MANIFEST.md
      lib-soft/*.a       build artifact: same archives with the ilp32f
                         float-ABI metadata cleared (normalize-blobs.sh;
                         integer ABI identical, see the script header)
      rom/*.ld           the 13 ROM symbol scripts (committed)
      include*/          pinned esp_wifi/esp_phy headers + shims (committed)
      sdkconfig.h        the matching IDF default config (committed)
      bootloader.bin / partition-table.bin  XIP boot assets, gitignored
    osal/                the OSAL: Go primitives + exported entry points +
                         _wrap/*.c (the table itself, ISR framework, glue)
    build.sh             resolves paths, generates osal/link_args.go, builds
    run-emu.sh           merges the XIP flash image and runs esp-emu
    main.go              the gate sequence (see below)

## Build & run

    ./build.sh          # wifi.bin.elf + wifi.bin.bin (app image)
    ./run-emu.sh        # esp-emu, /tmp/esp-emu.lock serialized

`LLGO_BIN` overrides the llgo binary (default /tmp/llgo-wifiosal-bin).

### Why esp32s31-flash (XIP), not esp32s31

The blob's text+rodata are ~846 KiB (per-library sizes in
idfparts/MANIFEST.md); the SRAM-only target's DIRAM window is 512 KiB and
already holds the runtime. The flash target executes text/rodata from the
XIP window and keeps only data/bss (≈40 KiB blob + runtime) in SRAM - the
same choice the repo's s31-xip tests make, booting through the IDF
second-stage bootloader.

## OSAL mapping notes

- Blocking primitives (semaphores, recursive mutexes, queues, event groups,
  task delay, ets timers) are Go exports over channels/`sync`/runtime
  timers; they park the calling goroutine and preserve the goroutine
  identity variable across the park (osal/osal.go package comment).
- Everything the MAC/BB ISRs can reach stays in C: CLIC trap entry
  (_wrap/osal_intr.c), interrupt masking via mintthresh, and the per-queue
  ISR staging rings whose items the task side drains on each bounded park
  (the ISR cannot touch the Go scheduler).
- Task -> goroutine: `_task_create*` spawns a goroutine that enters the
  library's raw function pointer through `llgo_osal_run_task`, with
  synchronized start (the creator waits until the identity is installed -
  espradio's sync-start rule).
- The blob is an ilp32f build; the link normalizes its ELF metadata to the
  soft-float ABI llgo compiles for (normalize-blobs.sh - integer calling
  convention identical, integer-only signatures at the boundary, blob saves
  its own callee-saved fs registers, ISR bodies verified FP-free).

## Acceptance gates (emu)

1. `esp_wifi_init` returns ESP_OK with the OSAL table installed
2. set_mode(STATION) + start, WIFI_EVENT_STA_START reaches Go
3. scan -> SCAN_DONE event -> non-empty AP records
4. (best effort) connect + DHCP against the emu gateway AP

Per-gate serial logs land in the session artifacts under
`wifi-osal/logs/`. Honest boundary notes are in those logs, not here.
