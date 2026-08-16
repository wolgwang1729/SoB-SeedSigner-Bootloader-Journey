/*
 * Stateless PSRAM shim — Xtensa entry point (ESP32-S3 MicroPython Payload).
 */

    .section .iram0.text, "ax"
    .equ CUSTOM_BOOT_STACK_SIZE, 16384
    .equ EXTRA_SAVE_AREA_SIZE, 32
    .equ BASE_SAVE_AREA_SIZE, 16
    .equ SAVE_AREA_OFFSET, (EXTRA_SAVE_AREA_SIZE + BASE_SAVE_AREA_SIZE)
    .equ BASE_AREA_SP_OFFSET, 12

    .equ PS_EXCM, 0x10
    .equ PS_WOE, 0x40000
    .equ PS_INTLEVEL_MASK, 0xF
    .global my_entry_point
    .type   my_entry_point, @function

    .align  4
.Lstack_top:
    .word   custom_boot_stack + CUSTOM_BOOT_STACK_SIZE
.Lps_clear_mask:
    .word   ~(PS_EXCM | PS_WOE | PS_INTLEVEL_MASK | 0x1F00000)
.Lps_init:
    .word   PS_WOE | PS_INTLEVEL_MASK

    .align  4
my_entry_point:
    /* 1. Mask all interrupts and set EXCM during window state reset */
    rsr     a5, PS
    l32r    a6, .Lps_clear_mask
    and     a5, a5, a6
    movi    a6, PS_EXCM | PS_INTLEVEL_MASK
    or      a5, a5, a6
    wsr     a5, PS
    rsync

    /* 2. Fresh top-level window: WINDOWBASE = 0, WINDOWSTART = 1 */
    movi    a6, 0
    wsr     a6, WINDOWBASE
    movi    a6, 1
    wsr     a6, WINDOWSTART
    rsync

    /* 3. Setup stack pointer on custom_boot_stack (.dram0.data) with save area */
    l32r    a1, .Lstack_top
    addi    a5, a1, -SAVE_AREA_OFFSET
    addi    a6, a5, -BASE_AREA_SP_OFFSET
    s32i    a1, a6, 0
    mov     a1, a5

    /* 4. a0 = 0 marks the outermost window frame */
    movi    a0, 0

    /* 5. Set PS with WOE enabled and INTLEVEL=15 (interrupts masked), EXCM cleared */
    l32r    a5, .Lps_init
    wsr     a5, PS
    rsync

    /* 6. Call into C implementation */
    call4   __wrap_call_start_cpu0

    /* Should not return; loop if it does */
.Ldead:
    j       .Ldead
    .size   my_entry_point, . - my_entry_point
