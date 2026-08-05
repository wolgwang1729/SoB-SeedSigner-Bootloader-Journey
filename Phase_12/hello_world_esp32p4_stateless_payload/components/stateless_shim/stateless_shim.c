#include "esp_attr.h"
/*
 * Stateless PSRAM Shim — ported from Phase 11's MicroPython shim
 * (Phase_11/seedsigner_micropython_builder_changes/.../stateless_shim.c).
 *
 * The Phase 12 loader verifies the Specter-signed bundle and jumps to the
 * app's custom entry point without ever running the ESP-IDF 2nd-stage
 * bootloader. This shim re-implements the early-boot hand-off that the normal
 * bootloader would otherwise do: entry point, .bss clearing, clock init, PSRAM
 * heap injection, and the system init-fn array, then hands off to the native
 * FreeRTOS startup (esp_startup_start_app).
 *
 * Differences from the MicroPython version:
 *  - MicroPython's `uart_stdout_init`/`uart_irq_handler` wraps are removed:
 *    those symbols are MicroPython-specific (ports/esp32/uart.c), not part of
 *    a plain ESP-IDF app, and this hello-world payload needs no REPL input.
 */

#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include "soc/timer_group_struct.h"
#include "soc/timer_periph.h"
#include "esp_rom_sys.h"
#include "esp_heap_caps.h"
#include "hal/wdt_hal.h"
#include "esp_cpu.h"

static void drain_uart_fifo(void) {
    for (volatile int i = 0; i < 5000000; i++);
}

__attribute__((used, section(".data"), aligned(16))) uint8_t custom_boot_stack[8192] = { 0 };

void IRAM_ATTR __attribute__((naked)) my_entry_point(void) {
    asm volatile (
        "li   t0, 0x6000          \n"
        "csrs mstatus, t0         \n"
        ".option push              \n"
        ".option norelax           \n"
        "la   gp, __global_pointer$ \n"
        ".option pop               \n"
        "la   sp, custom_boot_stack \n"
        "li   t0, 8192             \n"
        "add  sp, sp, t0           \n"
        "tail __wrap_call_start_cpu0 \n"
    );
}

extern void __real_call_start_cpu0(void);
extern int Cache_Invalidate_All(uint32_t map);

void IRAM_ATTR my_trap_handler(void) {
    uint32_t mcause, mepc;
    asm volatile ("csrr %0, mcause" : "=r"(mcause));
    asm volatile ("csrr %0, mepc" : "=r"(mepc));
    esp_rom_printf("\r\nTRAP! mcause=0x%08x mepc=0x%08x\r\n", mcause, mepc);
    drain_uart_fifo();
    while(1);
}

typedef struct {
    int (*fn)(void);
    uint16_t cores;
    uint16_t stage;
} esp_system_init_fn_t;

extern esp_system_init_fn_t _esp_system_init_fn_array_start;
extern esp_system_init_fn_t _esp_system_init_fn_array_end;

#include "hal/interrupt_clic_ll.h"
#include "soc/interrupts.h"

extern void esp_startup_start_app(void);
extern void esp_clk_init(void);
extern void esp_perip_clk_init(void);

static void core_intr_matrix_clear(void)
{
    for (int i = 0; i < ETS_MAX_INTR_SOURCE; i++) {
        interrupt_clic_ll_route(0, i, 0); // 0 is invalid/disabled for CLIC route usually
    }
}

#include <string.h>

extern int _bss_start_low, _bss_start_high;
extern int _bss_end_low, _bss_end_high;
extern int _rtc_bss_start, _rtc_bss_end;
extern int _iram_bss_start, _iram_bss_end;

void IRAM_ATTR __attribute__((noinline)) __wrap_call_start_cpu0(void) {
    esp_rom_printf("\r\n=== __wrap_call_start_cpu0 ENTERED ===\r\n");

    // Disable all hardware watchdogs (SWD, LP_WDT, MWDT0, MWDT1, RWDT) 
    // to prevent hardware resets during slow boot operations.
    wdt_hal_context_t rwdt_ctx = RWDT_HAL_CONTEXT_DEFAULT();
    wdt_hal_write_protect_disable(&rwdt_ctx);
    wdt_hal_disable(&rwdt_ctx);
    wdt_hal_write_protect_enable(&rwdt_ctx);

    #include "soc/soc.h"
    // Disable Super Watchdog (SWD)
    REG_WRITE(0x50116020, 0x50D83AA1);
    REG_WRITE(0x5011601C, (1U << 31) | (1U << 30) | (1U << 19) | (1U << 18));
    REG_WRITE(0x50116020, 0);

    // Disable Low Power Watchdog (LP_WDT)
    REG_WRITE(0x50116018, 0x50D83AA1);
    REG_WRITE(0x50116014, (1U << 31));
    REG_CLR_BIT(0x50116000, (1U << 31) | (0xFFFU << 19) | (1U << 12));
    REG_WRITE(0x5011602C, 0);
    REG_WRITE(0x50116030, 0xFFFFFFFF);
    REG_WRITE(0x50116018, 0);

    wdt_hal_context_t mwdt0_ctx = {.inst = WDT_MWDT0, .mwdt_dev = &TIMERG0};
    wdt_hal_write_protect_disable(&mwdt0_ctx);
    wdt_hal_disable(&mwdt0_ctx);
    wdt_hal_write_protect_enable(&mwdt0_ctx);

    wdt_hal_context_t mwdt1_ctx = {.inst = WDT_MWDT1, .mwdt_dev = &TIMERG1};
    wdt_hal_write_protect_disable(&mwdt1_ctx);
    wdt_hal_disable(&mwdt1_ctx);
    wdt_hal_write_protect_enable(&mwdt1_ctx);

    // Clear BSS!
    memset(&_bss_start_low, 0, ((uint8_t *)&_bss_end_low - (uint8_t *)&_bss_start_low));
    memset(&_bss_start_high, 0, ((uint8_t *)&_bss_end_high - (uint8_t *)&_bss_start_high));
    memset(&_rtc_bss_start, 0, ((uint8_t *)&_rtc_bss_end - (uint8_t *)&_rtc_bss_start));
    memset(&_iram_bss_start, 0, ((uint8_t *)&_iram_bss_end - (uint8_t *)&_iram_bss_start));

    esp_rom_printf("BSS cleared.\r\n");

    /* Install diagnostic trap handler */
    asm volatile ("csrw mtvec, %0" : : "r"(my_trap_handler));

    Cache_Invalidate_All(0x03);
    asm volatile ("fence.i\n");

    esp_rom_printf("Calling clocks init...\r\n");
    esp_clk_init();
    esp_perip_clk_init();
    core_intr_matrix_clear();
    esp_rom_printf("Clocks initialized.\r\n");

    esp_rom_printf("Initializing MMU software contexts...\r\n");
    extern void esp_mmu_map_init(void);
    esp_mmu_map_init();

    esp_rom_printf("Calling init functions...\r\n");
    esp_system_init_fn_t *p = &_esp_system_init_fn_array_start;
    
    // First pass: Stage 0
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
    extern void (*__init_priority_array_start)(void);
    extern void (*__init_priority_array_end)(void);
    extern void (*__init_array_start)(void);
    extern void (*__init_array_end)(void);
    void (**ctor)(void);
    for (ctor = &__init_priority_array_start; ctor < &__init_priority_array_end; ++ctor) {
        (*ctor)();
    }
    for (ctor = &__init_array_end - 1; ctor >= &__init_array_start; --ctor) {
        (*ctor)();
    }

    // Second pass: Stage 1
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
    uint32_t byte_aligned_caps[] = {MALLOC_CAP_SPIRAM | MALLOC_CAP_DEFAULT, 0, MALLOC_CAP_8BIT | MALLOC_CAP_32BIT | MALLOC_CAP_SIMD};
    // Skip the first 8MB to protect .text, .rodata, and .bss.
    heap_caps_add_region_with_caps(byte_aligned_caps, 0x48800000, 0x4A000000);

    /* Install the app's CLIC vector tables.
     *
     * A normal IDF app gets these from call_start_cpu0() -> init_cpu(); the shim
     * replaces call_start_cpu0(), so we do it here. The FreeRTOS tick and every
     * other interrupt vectors through xtvt/mtvt into the app's _mtvt_table. If the
     * tables are not installed the app's tick hooks never fire properly and the
     * interrupt watchdog (MWDT, re-armed by esp_int_wdt_init in
     * esp_startup_start_app) is never fed -> HP_SYS_HP_WDT_RESET.
     */
    extern char _vector_table[];
    extern char _mtvt_table[];
    esp_cpu_intr_set_ivt_addr(_vector_table);
    esp_cpu_intr_set_xtvt_addr(_mtvt_table);

    esp_rom_printf("Jumping to esp_startup_start_app...\r\n");
    drain_uart_fifo();
    esp_startup_start_app();

    while(1);
}

void IRAM_ATTR __wrap_cache_hal_init(void) { esp_rom_printf("[Intercepted] cache_hal_init\r\n"); drain_uart_fifo(); }
void IRAM_ATTR __wrap_mspi_timing_flash_tuning(void) { esp_rom_printf("[Intercepted] mspi_timing_flash_tuning\r\n"); drain_uart_fifo(); }
int IRAM_ATTR __wrap_esp_psram_chip_init(void) { esp_rom_printf("[Intercepted] esp_psram_chip_init\r\n"); drain_uart_fifo(); return 0; }
void IRAM_ATTR __wrap_bootloader_flash_update_id(void) { esp_rom_printf("[Intercepted] bootloader_flash_update_id\r\n"); drain_uart_fifo(); }
int IRAM_ATTR __wrap_image_process(void) { esp_rom_printf("[Intercepted] image_process\r\n"); drain_uart_fifo(); return 0; }
extern esp_err_t __real_spi_flash_init_chip_state(void);
extern void __real_esp_mspi_pin_init(void);
extern void __real_esp_mspi_pin_reserve(void);

esp_err_t IRAM_ATTR __wrap_spi_flash_init_chip_state(void) {
    esp_rom_printf("[Intercepted] spi_flash_init_chip_state -> calling real\r\n"); drain_uart_fifo();
    return __real_spi_flash_init_chip_state();
}
void IRAM_ATTR __wrap_esp_mspi_pin_init(void) {
    esp_rom_printf("[Intercepted] esp_mspi_pin_init -> calling real\r\n"); drain_uart_fifo();
    __real_esp_mspi_pin_init();
}
void IRAM_ATTR __wrap_esp_mspi_pin_reserve(void) {
    esp_rom_printf("[Intercepted] esp_mspi_pin_reserve -> calling real\r\n"); drain_uart_fifo();
    __real_esp_mspi_pin_reserve();
}
void IRAM_ATTR __wrap_esp_rom_output_tx_wait_idle(uint8_t uart_no) { }
void IRAM_ATTR __wrap_esp_rom_uart_set_clock_baudrate(uint8_t uart_no, uint32_t clock_hz, uint32_t baud_rate) { }

void IRAM_ATTR __wrap_esp_rtc_init(void) { esp_rom_printf("[Intercepted] esp_rtc_init\r\n"); drain_uart_fifo(); }

extern void *__real_heap_caps_calloc(size_t n, size_t size, uint32_t caps);

void * IRAM_ATTR __wrap_heap_caps_calloc(size_t n, size_t size, uint32_t caps) {
    if ((caps & (1 << 10)) && (caps & (1 << 3))) { 
        caps &= ~(1 << 3);
    }
    return __real_heap_caps_calloc(n, size, caps);
}
