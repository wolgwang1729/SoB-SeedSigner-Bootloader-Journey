/*
 * Phase 15 — stateless loader JMP-zone relocation for the ESP32-S3.
 *
 * The stock esp32s3 memory.ld.in places the loader's IRAM (iram0_0_seg) at
 * 0x40374000 with ICACHE=16KB, which is exactly the region the payload's IRAM
 * segments are copied onto at JMP time. The loader must therefore move its
 * "point of no return" trampoline (do_mmu_mapping_and_jump and friends) out of
 * that region and into a dedicated high-IRAM zone:
 *
 *   iram0_0_seg  org 0x40374000, len 0xD400    (loader text, ends 0x40381400)
 *   jmp_zone     org 0x403A0000, len 0x18000   (ends 0x403B8000)
 *
 * ROOT CAUSE of the "dead IRAM0 window" (found 2026-08-15): it is NOT a silicon
 * limit. ESP-IDF enables the S3's PMS memory protection
 * (CONFIG_ESP_SYSTEM_MEMPROT_FEATURE=y, `esp_mprot_set_prot` in cpu_start.c).
 * With the default config the IRAM0 split line is pinned to `_iram_text_end`
 * (end of the loader's .iram0.text, 0x403812F8) and the PMS areas are set to
 *
 *   IRAM0_0/1/2 (below the split): READ|EXEC   -> D-writes silently dropped
 *   IRAM0_3     (above the split): NONE        -> I-fetch + D-reads return 0
 *
 * That is exactly why:
 *   - a naked `ret.n` probe at 0x403A0000/0x40386400 panicked IllegalInstruction
 *     (I-fetch of zeros),
 *   - D-reads of the relocated zone returned 0 while the DRAM alias showed the
 *     bootloader copy had landed,
 *   - every D-write to IRAM0 (0x40381000, 0x40386C00, ...) was dropped.
 *
 * The fix is in main.c: right before the JMP-zone entry the loader writes all
 * four IRAM0 WORLD_0 PMS areas to READ|WRITE|EXEC
 * (SENSITIVE_CORE_X_IRAM0_PMS_CONSTRAIN_2_REG = 0xFFF). This requires
 * CONFIG_ESP_SYSTEM_MEMPROT_FEATURE_LOCK=n so the SENSITIVE registers are not
 * hardware-locked by cpu_start. The zone can then be anywhere in the IRAM0
 * window; 0x403A0000-0x403B8000 is used (matches the original design, clear of
 * the loader text, payload copies and the 2nd-stage bootloader at 0x403CB700).
 *
 * JMP-zone statics live in DRAM (`.jmp_zone.bss` -> jmp_bss, 0x3FC98000):
 * - IRAM0 D-writes drop while the loader's memprot baseline is armed, so the
 *   zone's state (safe_entry_addr, jump_stack, ...) is kept on the D-side where
 *   reads AND writes work regardless of PMS.
 * - Physical memory model: I/D are aliases (I = D + 0x6F0000). The zone text at
 *   0x403A0000 aliases 0x3FCB0000-0x3FCB8000, so the zone's .bss must stay below
 *   D 0x3FCB0000 (asserted below) or its writes would clobber the zone code.
 * - The internal heap is pinned away by CONFIG_ESP32S3_USE_FIXED_STATIC_RAM_SIZE
 *   =0x40000 (_heap_start = 0x3FCC8000 > 0x3FCB8000), so the heap can never
 *   reclaim the zone's physical alias.
 * - The loader's own .data/.bss ends at 0x3FC96010 (< 0x3FCB0000) — clean.
 *
 * GNU ld processes MEMORY redefinitions in script order: a later declaration of
 * the same region name overrides the earlier one (only a "redeclaration of
 * memory region" warning is emitted). This script must therefore be linked
 * after esp_system's memory.ld/sections.ld — it is, because this component is
 * linked after esp_system on the final link line (target_linker_script).
 *
 * The `.jmp_zone.*` section names deliberately start with a prefix that no
 * wildcard in esp_system's sections.ld.in matches, so they land only here and
 * are not also captured into iram0_0_seg.
 */
MEMORY
{
  iram0_0_seg (RX) : org = 0x40374000, len = 0xD400
  jmp_zone    (RX) : org = 0x403A0000, len = 0x18000
  /* JMP-zone .bss DRAM home: placed at 0x3FCB8000 (directly above the
   * JMP zone text's D-alias 0x3FCB0000-0x3FCB8000 and well below _heap_start
   * 0x3FCC8000). This guarantees that payload DRAM copies (0x3FC90000-0x3FCA0050)
   * and loader text/data never collide with jump_stack or JMP-zone state. */
  jmp_bss     (RW) : org = 0x3FCB8000, len = 0x8000
}

SECTIONS
{
  .jmp_zone.text :
  {
    /* Xtensa l32r: the literal pool must sit BEFORE the code that uses it
     * (within 256KB below the PC). The compiler emits .jmp_zone.literal for
     * .jmp_zone.text functions — capture it first. */
    *(.jmp_zone.literal .jmp_zone.literal.*)
    KEEP(*(.jmp_zone.text .jmp_zone.text.*))
    . = ALIGN(4);
  } > jmp_zone

  .jmp_zone.bss (NOLOAD) :
  {
    . = ALIGN(16);
    _jmp_zone_bss_start = ABSOLUTE(.);
    KEEP(*(.jmp_zone.stack .jmp_zone.stack.*))
    . = ALIGN(16);
    . += 0x400;   /* window-overflow spill margin above the stack top */
    KEEP(*(.jmp_zone.bss .jmp_zone.bss.*))
    . = ALIGN(8);
    _jmp_zone_bss_end = ABSOLUTE(.);
  } > jmp_bss
}

ASSERT(_jmp_zone_bss_end <= 0x3FCC8000, "JMP-zone .bss reaches heap start. Shrink jump_stack or reduce loader DRAM usage.")
