#include "esp_attr.h"
/*
 * Stateless PSRAM Shim — Xtensa port (ESP32-S3) for MicroPython Payload
 *
 * The Phase 15/16 loader verifies the Specter-signed bundle and jumps to the
 * app's custom entry point without ever running the ESP-IDF 2nd-stage
 * bootloader's app-loading flow. On Xtensa, `my_entry_point` in entry.S
 * rebuilds a clean Xtensa windowed context, then hands off to
 * `__wrap_call_start_cpu0`, which re-implements the early-boot init that
 * `call_start_cpu0()` would do.
 */

#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include "soc/timer_group_struct.h"
#include "soc/timer_periph.h"
#include "esp_rom_sys.h"
#include "hal/wdt_hal.h"
#include "esp_cpu.h"
#include "esp_intr_alloc.h"
#include "soc/uart_periph.h"
#include "hal/uart_hal.h"
#include "hal/uart_ll.h"
#include "esp_heap_caps.h"

static void drain_uart_fifo(void) {
    for (volatile int i = 0; i < 5000000; i++);
}

uint8_t custom_boot_stack[16384] __attribute__((used, section(".dram0.data"), aligned(16))) = { 0 };

extern void __real_call_start_cpu0(void);

void IRAM_ATTR __wrap_cache_hal_init(void) {
    esp_rom_printf("[Intercepted] cache_hal_init\r\n");
    drain_uart_fifo();
}

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
    esp_rom_printf("\r\n=== STATELESS SHIM S3 MICROPYTHON PAYLOAD BOOT OK ===\r\n");
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

    // Install the app's vector table. Writes VECBASE.
    esp_cpu_intr_set_ivt_addr(&_vector_table);

    // Invalidate the caches so the freshly-remapped IROM/DROM windows (which
    // the loader pointed at PSRAM-backed fake_flash) are fetched fresh.
    Cache_Invalidate_ICache_All();
    Cache_Invalidate_DCache_All();
    asm volatile ("isync" ::: "memory");

    esp_rom_printf("Calling clocks init...\r\n");
    esp_clk_init();
    esp_perip_clk_init();
    uint32_t core_id = esp_cpu_get_core_id();
    for (int i = 0; i < ETS_MAX_INTR_SOURCE; i++) {
        esp_rom_route_intr_matrix(core_id, i, ETS_INVALID_INUM);
    }
    esp_rom_printf("Clocks initialized and interrupt matrix cleared.\r\n");

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

    // Run global constructors
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

    esp_rom_printf("Manually adding PSRAM to heap...\r\n");
    extern esp_err_t heap_caps_add_region_with_caps(const uint32_t caps[], intptr_t start, intptr_t end);
    uint32_t byte_aligned_caps[] = {MALLOC_CAP_SPIRAM | MALLOC_CAP_DEFAULT, 0, MALLOC_CAP_8BIT | MALLOC_CAP_32BIT};
    heap_caps_add_region_with_caps(byte_aligned_caps, 0x3C300000, 0x3C800000);

    esp_rom_printf("Jumping to esp_startup_start_app...\r\n");
    drain_uart_fifo();
    esp_startup_start_app();

    while(1);
}

extern void uart_irq_handler(void *arg);

void IRAM_ATTR __wrap_uart_stdout_init(void) {
    // MicroPython's stock uart_stdout_init() installs the UART0 RX interrupt so REPL
    // keystrokes reach stdin_ringbuf. The stateless loader has already configured
    // UART0 (pin mux, 115200 baud) and the ROM console driver owns the peripheral, so
    // we deliberately do NOT reset the UART here (that would call uart_hal_init()).
    // Only wire up the RX interrupt path, preserving the bootloader's UART config.
    uart_hal_context_t repl_hal = { .dev = UART_LL_GET_HW(0) };
    esp_err_t err = esp_intr_alloc(uart_periph_signal[0].irq,
        ESP_INTR_FLAG_LOWMED | ESP_INTR_FLAG_IRAM,
        uart_irq_handler, NULL, NULL);
    if (err != ESP_OK) {
        esp_rom_printf("[P16] uart_stdout_init: esp_intr_alloc failed %d\r\n", err);
        return;
    }
    uart_hal_set_rxfifo_full_thr(&repl_hal, SOC_UART_FIFO_LEN - 8);
    uart_hal_set_rx_timeout(&repl_hal, 10);
    uart_hal_ena_intr_mask(&repl_hal, UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT);
}
