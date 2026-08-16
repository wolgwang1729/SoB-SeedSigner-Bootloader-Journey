#include "esp_attr.h"
/*
 * Stateless PSRAM Shim — Xtensa port (ESP32-S3) of the Phase 11/12 shim.
 *
 * The Phase 15 loader verifies the Specter-signed bundle and jumps to the
 * app's custom entry point without ever running the ESP-IDF 2nd-stage
 * bootloader's app-loading flow. On Xtensa this has one extra duty compared to
 * the RISC-V (ESP32-P4) version: `my_entry_point` must rebuild a clean Xtensa
 * windowed context (the loader left WINDOWBASE / WINDOWSTART / PS in whatever
 * state its FreeRTOS task was in), then hand off to `__wrap_call_start_cpu0`,
 * which re-implements the early-boot init that `call_start_cpu0()` would do.
 *
 * ESP32-S3 specific notes (vs. the ESP32-P4 shim):
 *  - Xtensa: no CLIC, so no `_mtvt_table` / `esp_cpu_intr_set_xtvt_addr`;
 *    only `_vector_table` via `esp_cpu_intr_set_ivt_addr` (writes VECBASE).
 *  - S3 BSS symbols are `_bss_start`/`_bss_end` (the P4's `_bss_start_low` /
 *    `_bss_start_high` don't exist here).
 *  - ROM cache API is `Cache_Invalidate_ICache_All()` / `Cache_Invalidate_DCache_All()`
 *    (no `map` argument, unlike P4).
 *  - The payload does NOT enable CONFIG_SPIRAM, so `esp_psram_is_initialized()`
 *    reads a freshly-zeroed .bss and `add_psram_to_heap` (an ESP_SYSTEM_INIT_FN)
 *    never hands the PSRAM pages (which alias the loader's fake_flash .text/
 *    .rodata) to the heap — no manual `heap_caps_add_region_with_caps` here.
 */

#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include "soc/timer_group_struct.h"
#include "soc/timer_periph.h"
#include "esp_rom_sys.h"
#include "hal/wdt_hal.h"
#include "esp_cpu.h"

static void drain_uart_fifo(void) {
    for (volatile int i = 0; i < 5000000; i++);
}

uint8_t custom_boot_stack[8192] __attribute__((used, section(".dram0.data"), aligned(16))) = { 0 };

extern void __real_call_start_cpu0(void);

void IRAM_ATTR __wrap_cache_hal_init(void) { esp_rom_printf("[Intercepted] cache_hal_init\r\n"); drain_uart_fifo(); }

typedef struct {
    int (*fn)(void);
    uint16_t cores;
    uint16_t stage;
} esp_system_init_fn_t;

extern esp_system_init_fn_t _esp_system_init_fn_array_start;
extern esp_system_init_fn_t _esp_system_init_fn_array_end;

extern void esp_startup_start_app(void);
extern void esp_clk_init(void);
extern void esp_perip_clk_init(void);
extern int Cache_Invalidate_ICache_All(void);
extern int Cache_Invalidate_DCache_All(void);

extern int _bss_start, _bss_end;
extern int _rtc_bss_start, _rtc_bss_end;
extern int _vector_table;

void IRAM_ATTR __attribute__((noinline)) __wrap_call_start_cpu0(void) {
    esp_rom_printf("\r\n=== STATELESS SHIM PSRAM PAYLOAD BOOT OK ===\r\n");
    esp_rom_printf("=== __wrap_call_start_cpu0 ENTERED ===\r\n");

    // Disable all hardware watchdogs (RWDT, MWDT0, MWDT1) to prevent hardware
    // resets during slow boot operations.
    wdt_hal_context_t rwdt_ctx = RWDT_HAL_CONTEXT_DEFAULT();
    wdt_hal_write_protect_disable(&rwdt_ctx);
    wdt_hal_disable(&rwdt_ctx);
    wdt_hal_write_protect_enable(&rwdt_ctx);

    wdt_hal_context_t mwdt0_ctx = {.inst = WDT_MWDT0, .mwdt_dev = &TIMERG0};
    wdt_hal_write_protect_disable(&mwdt0_ctx);
    wdt_hal_disable(&mwdt0_ctx);
    wdt_hal_write_protect_enable(&mwdt0_ctx);

    wdt_hal_context_t mwdt1_ctx = {.inst = WDT_MWDT1, .mwdt_dev = &TIMERG1};
    wdt_hal_write_protect_disable(&mwdt1_ctx);
    wdt_hal_disable(&mwdt1_ctx);
    wdt_hal_write_protect_enable(&mwdt1_ctx);

    // Clear BSS. The loader already copied the .data segment into internal
    // DRAM; .bss (which the loader did not copy) must be zeroed here.
    memset(&_bss_start, 0, ((uint8_t *)&_bss_end - (uint8_t *)&_bss_start));
    memset(&_rtc_bss_start, 0, ((uint8_t *)&_rtc_bss_end - (uint8_t *)&_rtc_bss_start));

    esp_rom_printf("BSS cleared.\r\n");

    // Install the app's vector table. A normal IDF app gets this from
    // call_start_cpu0() -> init_cpu(); the shim replaces call_start_cpu0(), so
    // we do it here (writes VECBASE).
    esp_cpu_intr_set_ivt_addr(&_vector_table);

    // Invalidate the caches so the freshly-remapped IROM/DROM windows (which
    // the loader pointed at PSRAM-backed fake_flash) are fetched fresh.
    Cache_Invalidate_ICache_All();
    Cache_Invalidate_DCache_All();
    asm volatile ("isync" ::: "memory");

    esp_rom_printf("Calling clocks init...\r\n");
    esp_clk_init();
    esp_perip_clk_init();
    esp_rom_printf("Clocks initialized.\r\n");

    esp_rom_printf("Initializing MMU software contexts...\r\n");
    extern void esp_mmu_map_init(void);
    esp_mmu_map_init();

    esp_rom_printf("Calling init functions...\r\n");
    esp_system_init_fn_t *p = &_esp_system_init_fn_array_start;

    // First pass: Stage 0 (CORE)
    while (p < &_esp_system_init_fn_array_end) {
        if ((p->cores & 1) && (p->stage == 0)) {
            esp_rom_printf("--- calling fn %p, stage 0 ---\r\n", p->fn);
            drain_uart_fifo();
            if (p->fn()) {
                esp_rom_printf("init failed\n");
                while(1);
            }
        }
        p++;
    }

    // Run global constructors (descending order, matching do_global_ctors() in
    // esp_system/startup.c; Xtensa has no __init_priority_array_*).
    esp_rom_printf("Running global constructors...\r\n");
    extern void (*__init_array_start)(void);
    extern void (*__init_array_end)(void);
    void (**ctor)(void);
    for (ctor = &__init_array_end - 1; ctor >= &__init_array_start; --ctor) {
        (*ctor)();
    }

    // Second pass: Stage 1 (SECONDARY)
    p = &_esp_system_init_fn_array_start;
    while (p < &_esp_system_init_fn_array_end) {
        if ((p->cores & 1) && (p->stage == 1)) {
            esp_rom_printf("--- calling fn %p, stage 1 ---\r\n", p->fn);
            drain_uart_fifo();
            if (p->fn()) {
                esp_rom_printf("init failed\n");
                while(1);
            }
        }
        p++;
    }

    esp_rom_printf("Jumping to esp_startup_start_app...\r\n");
    drain_uart_fifo();
    esp_startup_start_app();

    while(1);
}
