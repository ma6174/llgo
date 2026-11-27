/* Extra placement rules for the closed-source blob's non-standard sections.
 *
 * The llgo esp32s31-flash script handles .text/.rodata/.data/.bss; the blob
 * ships ESP32-heritage sections that need explicit homes:
 *
 *   .wifi0iram / .wifirxiram / .wifislpiram / .wifiorslpiram /
 *   .wifislprxiram / .wifiextrairam / .iram1
 *       Code sections. IDF's esp_wifi linker fragment routes them to RAM
 *       (iram0_text) only under ESP_WIFI_IRAM_OPT/SLP_IRAM_OPT; its default
 *       scheme maps the same sections to flash text. We follow the default:
 *       P1 has no cache-disabled or sleep path, so cache-mapped execution is
 *       safe, and the flash target's SRAM is the scarce resource.
 *
 *   .rodata_wlog_*
 *       The library's log-format strings (one section per level). Flash
 *       rodata; the OSAL's log_write consumes them via printf.
 *
 *   .dram1
 *       Blob writable data outside .data/.bss. SRAM, appended after the
 *       script's .bss; the heap bounds symbols are reassigned afterwards so
 *       the collector's heap window starts above it (and the globals scan
 *       range covers it - tinygogc scans [_globals_start, _globals_end]).
 *
 * This file is passed as an additional -T script AFTER
 * targets/esp32s31-flash.memory.ld's script chain; lld merges SECTIONS rules
 * in command order, so these rules run last and the final symbol values of
 * _end/_heapStart/_globals_end are the reassigned ones.
 */

SECTIONS
{
  .wifi_iram :
  {
    *(.wifi0iram .wifi0iram.*)
    *(.wifirxiram .wifirxiram.*)
    *(.wifislpiram .wifislpiram.*)
    *(.wifiorslpiram .wifiorslpiram.*)
    *(.wifislprxiram .wifislprxiram.*)
    *(.wifiextrairam .wifiextrairam.*)
    *(.iram1 .iram1.*)
  } > flash_seg

  .wlog_rodata :
  {
    *(.rodata_wlog_error .rodata_wlog_error.*)
    *(.rodata_wlog_warning .rodata_wlog_warning.*)
    *(.rodata_wlog_info .rodata_wlog_info.*)
    *(.rodata_wlog_debug .rodata_wlog_debug.*)
    *(.rodata_wlog_verbose .rodata_wlog_verbose.*)
  } > flash_seg

  /* Explicitly addressed at the main script's _end: this script runs after
   * .stack was placed at the top of sram_seg, so a region-relative section
   * here would start past the region end and overflow. _end is the byte
   * after .bss - free, below the stack, and above every scanned global. */
  .wifi_data _end :
  {
    *(.dram1 .dram1.*)
    . = ALIGN(4);
  } > sram_seg

  /* The blob data is static, GC-unscanned C memory like .bss's tail: move
   * the heap floor and the globals-scan ceiling above it. */
  _end = .;
  PROVIDE(end = .);
  _heapStart = .;
  _globals_end = ALIGN(., 4);
}
