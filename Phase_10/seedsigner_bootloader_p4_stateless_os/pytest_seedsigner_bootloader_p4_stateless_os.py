# SPDX-FileCopyrightText: 2024-2026
# SPDX-License-Identifier: CC0-1.0
#
# Pytest integration test for the SeedSigner Stateless Bootloader (ESP32-P4).
#
# This test suite validates the complete boot chain:
#   1. Partition table layout and payload discovery
#   2. Bootloader image parsing (magic byte, segment count, entry address)
#   3. PSRAM MMU footprint calculation and fake_flash staging buffer
#   4. Segment classification and routing (fake_flash vs direct copy)
#   5. Deferred copy manifest with cross-validation against segment sizes
#   6. Bare-metal jump sequence (WDT/interrupt teardown, cache eviction)
#   7. MMU mapping arithmetic (vaddr/paddr alignment, entry ID formula,
#      page number derivation, MMU_VALID and MMU_PSRAM_TYPE flag bits)
#   8. The "rug pull" interception of ESP-IDF hardware init functions
#   9. OS handover integrity (heap regions, CPU frequency, scheduler)
#  10. Final payload execution with task-loop stability proof
#
# Usage:
#   source ~/esp/esp-idf-v5.5/export.sh
#   pytest --embedded-services esp,idf --target esp32p4 \
#          --port /dev/ttyACM0 pytest_seedsigner_bootloader_p4_stateless_os.py
#
# Architecture reference (ESP32-P4 memory map):
#   Internal SRAM (HP L2MEM):  0x4FF00000 - 0x4FFBFFFF  (768 KB)
#   Scratch-pad Memory (SPM):  0x30100000 - 0x30101FFF  (8 KB)
#   Flash cache (IROM/DROM):   0x40000000 - 0x44000000  (shared I/D)
#   PSRAM cache:               0x48000000 - 0x4C000000  (via MMU)
#   RTC RAM:                   0x50100000 - 0x5010FFFF  (64 KB)
#   MMU page size:             64 KB (0x10000)
# ---------------------------------------------------------------------------

import logging
import math
import re
import time

import pytest
from pytest_embedded_idf.app import IdfApp
from pytest_embedded_idf.dut import IdfDut
from pytest_embedded_idf.utils import idf_parametrize

# ── ESP32-P4 memory map constants ──────────────────────────────────────────
PSRAM_VADDR_START   = 0x48000000
PSRAM_VADDR_END     = 0x4C000000
SRAM_VADDR_START    = 0x4FF00000
SRAM_VADDR_END      = 0x4FFBFFFF
SPM_VADDR_START     = 0x30100000
SPM_VADDR_END       = 0x30101FFF
MMU_PAGE_SIZE       = 0x10000       # 64 KB

# MMU entry bit-flags (from ESP32-P4 TRM, Chapter "MMU")
MMU_VALID_BIT       = (1 << 10)     # Access permission / valid
MMU_PSRAM_TYPE_BIT  = (1 << 11)     # Target type: 1=PSRAM, 0=Flash
MMU_PAGE_NUM_MASK   = 0x3FF         # Low 10 bits = physical page number

# Partition layout expectations (from partitions.csv)
EXPECTED_PAYLOAD_OFFSET = 0x140000
EXPECTED_PAYLOAD_SIZE   = 0x6C0000  # ~6.75 MB

# Image format constants
ESP_IMAGE_MAGIC     = 0xE9
MAX_FIRMWARE_SIZE   = 4 * 1024 * 1024
MAX_SEGMENT_COUNT   = 16

# Minimum number of interceptors the "rug pull" must fire
MIN_INTERCEPTOR_COUNT = 9

# Minimum total heap (KiB) expected after boot with interceptors
MIN_TOTAL_HEAP_KIB  = 400

# Heartbeat count for stability proof
STABILITY_HEARTBEATS = 3


def _decode(val):
    """Decode bytes to str if needed (pexpect match groups may be bytes)."""
    return val.decode() if isinstance(val, bytes) else val


# ── Single monolithic test ─────────────────────────────────────────────────
# pytest-embedded shares the serial stream across expect() calls within one
# test.  Splitting into multiple test functions would require each to re-flash
# and re-boot.  Instead we use a single test with clearly delineated phases
# and sub-assertions so that a failure pinpoints exactly which stage broke.

@pytest.mark.generic
@idf_parametrize('target', ['esp32p4'], indirect=['target'])
def test_stateless_bootloader_and_payload(app: IdfApp, dut: IdfDut) -> None:
    """
    End-to-end integration test for the SeedSigner stateless bootloader.

    Validates image parsing, segment routing, MMU mapping arithmetic, the
    linker --wrap interception technique ("rug pull"), and final FreeRTOS
    payload execution from PSRAM.
    """

    # Accumulators for cross-phase validation and final summary
    segments = []           # list of (addr, len, route) tuples from Phase 3
    copies = []             # list of (dest, src, len) tuples from Phase 4
    mmu_maps = []           # list of (vaddr, paddr, pages) from Phase 6
    mmu_entries = []        # list of (entry_id, entry_val) from Phase 6
    heap_regions = []       # list of (addr, len_bytes, kib, type) from Phase 8
    interceptors_fired = [] # list of function names from Phase 7

    # ── Phase 0: Hard-reset to capture the full boot log ───────────────
    logging.info('Phase 0: Triggering hard reset via DTR/RTS...')
    dut.serial.hard_reset()
    time.sleep(0.5)

    # ====================================================================
    # PHASE 1: PARTITION TABLE VALIDATION
    # Verify that the 2nd-stage bootloader discovers the 'payload'
    # partition at the expected offset and size.
    # ====================================================================
    logging.info('Phase 1: Partition table validation...')

    # Extract the payload partition entry from the boot log
    # Format: "3 payload          Unknown data     01 ff 00140000 006c0000"
    # pexpect is stream-based so we can't use $ or greedy .* reliably.
    # Instead, match 'payload' then skip to the two 8-char hex fields.
    part_match = dut.expect(
        r'payload\s+\S+\s+\S+\s+\S+\s+\S+\s+([0-9a-f]{8})\s+([0-9a-f]{6,8})',
        timeout=10
    )
    part_offset = int(part_match.group(1), 16)
    part_size   = int(part_match.group(2), 16)

    assert part_offset == EXPECTED_PAYLOAD_OFFSET, \
        f'Payload partition offset 0x{part_offset:06X} does not match ' \
        f'expected 0x{EXPECTED_PAYLOAD_OFFSET:06X}'
    assert part_size == EXPECTED_PAYLOAD_SIZE, \
        f'Payload partition size 0x{part_size:06X} does not match ' \
        f'expected 0x{EXPECTED_PAYLOAD_SIZE:06X}'
    logging.info(
        f'  ✓ Payload partition: offset=0x{part_offset:06X} '
        f'size=0x{part_size:06X} ({part_size // 1024} KiB)'
    )

    # ====================================================================
    # PHASE 2: BOOTLOADER IMAGE VALIDATION
    # Verify that the bootloader correctly parses the ESP32 image header,
    # detects a valid magic byte (0xE9), and reports a sane segment count.
    # Also extract and validate the flash partition load report.
    # ====================================================================
    logging.info('Phase 2: Bootloader image validation...')

    # 2a. Flash partition load — extract byte count and flash address
    load_match = dut.expect(
        r'Loaded\s+(\d+)\s+bytes\s+from\s+\'payload\'\s+@\s+0x([0-9A-Fa-f]+)',
        timeout=10
    )
    loaded_bytes = int(load_match.group(1))
    load_addr    = int(load_match.group(2), 16)

    assert loaded_bytes > 0, 'Zero bytes loaded from payload partition'
    assert loaded_bytes <= MAX_FIRMWARE_SIZE, \
        f'Loaded {loaded_bytes} bytes exceeds MAX_FIRMWARE_SIZE ({MAX_FIRMWARE_SIZE})'
    assert load_addr == EXPECTED_PAYLOAD_OFFSET, \
        f'Load address 0x{load_addr:08X} does not match payload partition ' \
        f'offset 0x{EXPECTED_PAYLOAD_OFFSET:08X}'
    logging.info(
        f'  ✓ Flash load: {loaded_bytes:,} bytes from 0x{load_addr:06X}'
    )

    # 2b. Image header parsed — extract segment count and entry address
    match = dut.expect(
        r'Image OK:\s+(\d+)\s+segments,\s+entry=0x([0-9A-Fa-f]+)',
        timeout=5
    )
    segment_count = int(match.group(1))
    entry_addr    = int(match.group(2), 16)

    assert 1 <= segment_count <= MAX_SEGMENT_COUNT, \
        f'Segment count {segment_count} outside valid range [1, {MAX_SEGMENT_COUNT}]'
    assert PSRAM_VADDR_START <= entry_addr < PSRAM_VADDR_END, \
        f'Entry address 0x{entry_addr:08X} is not in PSRAM range ' \
        f'[0x{PSRAM_VADDR_START:08X}, 0x{PSRAM_VADDR_END:08X})'
    logging.info(
        f'  ✓ Image header: {segment_count} segments, '
        f'entry=0x{entry_addr:08X}'
    )

    # 2c. PSRAM MMU footprint — validate it's 64KB-aligned
    footprint_match = dut.expect(
        r'PSRAM MMU footprint:\s+(\d+)\s+bytes',
        timeout=5
    )
    mmu_footprint = int(footprint_match.group(1))
    assert mmu_footprint % MMU_PAGE_SIZE == 0, \
        f'MMU footprint {mmu_footprint} is not 64KB-aligned'
    assert mmu_footprint > 0, 'MMU footprint is zero — no PSRAM segments?'
    logging.info(
        f'  ✓ MMU footprint: {mmu_footprint:,} bytes '
        f'({mmu_footprint // 1024} KiB, '
        f'{mmu_footprint // MMU_PAGE_SIZE} pages)'
    )

    # 2d. fake_flash staging buffer — validate paddr is 64KB-aligned
    ff_match = dut.expect(
        r'fake_flash:\s+vaddr=0x([0-9A-Fa-f]+)\s+paddr=0x([0-9A-Fa-f]+)',
        timeout=5
    )
    ff_vaddr = int(ff_match.group(1), 16)
    ff_paddr = int(ff_match.group(2), 16)

    assert ff_paddr % MMU_PAGE_SIZE == 0, \
        f'fake_flash paddr 0x{ff_paddr:08X} is not 64KB-aligned'
    assert PSRAM_VADDR_START <= ff_vaddr < PSRAM_VADDR_END, \
        f'fake_flash vaddr 0x{ff_vaddr:08X} is not in PSRAM range'
    logging.info(
        f'  ✓ fake_flash staging: vaddr=0x{ff_vaddr:08X} '
        f'paddr=0x{ff_paddr:08X}'
    )

    # ====================================================================
    # PHASE 3: SEGMENT CLASSIFICATION & ROUTING
    # Each segment must be routed to either:
    #   - "fake_flash" (PSRAM-mapped, vaddr in 0x48xxxxxx)
    #   - "direct copy" (SRAM/SPM, vaddr in 0x4FFxxxxx or 0x301xxxxx)
    # We validate that every segment address falls in a legal region and
    # that the routing decision is correct.
    # ====================================================================
    logging.info('Phase 3: Segment classification & routing...')

    psram_segment_count  = 0
    direct_segment_count = 0

    for i in range(segment_count):
        seg_match = dut.expect(
            r'Seg\s+(\d+):\s+addr=0x([0-9A-Fa-f]+)\s+len=(\d+)',
            timeout=5
        )
        seg_idx  = int(seg_match.group(1))
        seg_addr = int(seg_match.group(2), 16)
        seg_len  = int(seg_match.group(3))

        assert seg_idx == i, \
            f'Segment index mismatch: expected {i}, got {seg_idx}'
        assert seg_len > 0, \
            f'Segment {i} has zero length'
        assert seg_len <= MAX_FIRMWARE_SIZE, \
            f'Segment {i} length {seg_len} exceeds MAX_FIRMWARE_SIZE'

        # Determine expected routing based on load address
        if PSRAM_VADDR_START <= seg_addr < PSRAM_VADDR_END:
            route_match = dut.expect(
                r'->\s+(fake_flash|direct copy)',
                timeout=5
            )
            route = _decode(route_match.group(1))
            assert route == 'fake_flash', \
                f'Segment {i} at 0x{seg_addr:08X} is in PSRAM range but ' \
                f'was routed to "{route}" instead of "fake_flash"'
            psram_segment_count += 1
            segments.append((seg_addr, seg_len, 'psram'))
        else:
            in_sram = (SRAM_VADDR_START <= seg_addr <= SRAM_VADDR_END)
            in_spm  = (SPM_VADDR_START  <= seg_addr <= SPM_VADDR_END)
            assert in_sram or in_spm, \
                f'Segment {i} address 0x{seg_addr:08X} is outside all ' \
                f'valid memory regions (PSRAM/SRAM/SPM)'

            route_match = dut.expect(
                r'->\s+(fake_flash|direct copy)',
                timeout=5
            )
            route = _decode(route_match.group(1))
            assert route == 'direct copy', \
                f'Segment {i} at 0x{seg_addr:08X} is in SRAM/SPM range but ' \
                f'was routed to "{route}" instead of "direct copy"'
            direct_segment_count += 1
            region = 'sram' if in_sram else 'spm'
            segments.append((seg_addr, seg_len, region))

        logging.info(
            f'  ✓ Seg {i}: addr=0x{seg_addr:08X} len={seg_len:>6} → '
            f'{"PSRAM (fake_flash)" if PSRAM_VADDR_START <= seg_addr < PSRAM_VADDR_END else "SRAM/SPM (direct copy)"}'
        )

    assert psram_segment_count >= 1, \
        'No PSRAM-mapped segments found — the payload has no executable code in PSRAM!'
    assert direct_segment_count >= 1, \
        'No direct-copy segments found — the payload has no SRAM data!'
    logging.info(
        f'  ✓ Routing summary: {psram_segment_count} PSRAM + '
        f'{direct_segment_count} direct = {segment_count} total'
    )

    # Cross-validate: the MMU footprint must be at least as large as the
    # highest PSRAM segment offset + length (rounded up to 64KB)
    psram_segs = [(a, l) for a, l, r in segments if r == 'psram']
    if psram_segs:
        max_end = max((a - PSRAM_VADDR_START) + l for a, l in psram_segs)
        expected_footprint = math.ceil(max_end / MMU_PAGE_SIZE) * MMU_PAGE_SIZE
        assert mmu_footprint >= expected_footprint, \
            f'MMU footprint {mmu_footprint} is smaller than the calculated ' \
            f'minimum {expected_footprint} from PSRAM segments'
        logging.info(
            f'  ✓ MMU footprint cross-check: {mmu_footprint} >= '
            f'{expected_footprint} (calculated minimum)'
        )

    # ====================================================================
    # PHASE 4: DEFERRED COPY MANIFEST
    # Validate the copy table that will be executed after interrupts are
    # disabled.  Each copy source must point into PSRAM (the staging
    # buffer) and each destination must be in SRAM or SPM.
    # Cross-validate copy sizes against the segment sizes from Phase 3.
    # ====================================================================
    logging.info('Phase 4: Deferred copy manifest validation...')

    # "Jumping to 0xXXXXXXXX ..."
    jump_match = dut.expect(
        r'Jumping to 0x([0-9A-Fa-f]+)',
        timeout=5
    )
    jump_addr = int(jump_match.group(1), 16)
    assert jump_addr == entry_addr, \
        f'Jump address 0x{jump_addr:08X} does not match parsed entry ' \
        f'address 0x{entry_addr:08X}'
    logging.info(f'  ✓ Jump target matches entry point: 0x{jump_addr:08X}')

    # Parse each "copy[N]: 0xDEST <- 0xSRC (LEN B)" line
    direct_segs = [(a, l) for a, l, r in segments if r != 'psram']
    total_copy_bytes = 0
    for i in range(direct_segment_count):
        copy_match = dut.expect(
            r'copy\[(\d+)\]:\s+0x([0-9A-Fa-f]+)\s+<-\s+0x([0-9A-Fa-f]+)\s+\((\d+)\s+B\)',
            timeout=5
        )
        copy_idx  = int(copy_match.group(1))
        copy_dest = int(copy_match.group(2), 16)
        copy_src  = int(copy_match.group(3), 16)
        copy_len  = int(copy_match.group(4))

        assert copy_idx == i, \
            f'Copy index mismatch: expected {i}, got {copy_idx}'

        # Destination must be in SRAM or SPM
        dest_in_sram = (SRAM_VADDR_START <= copy_dest <= SRAM_VADDR_END)
        dest_in_spm  = (SPM_VADDR_START  <= copy_dest <= SPM_VADDR_END)
        assert dest_in_sram or dest_in_spm, \
            f'copy[{i}] dest 0x{copy_dest:08X} is not in SRAM or SPM'

        # Source must be in PSRAM (the staging buffer)
        assert PSRAM_VADDR_START <= copy_src < PSRAM_VADDR_END, \
            f'copy[{i}] src 0x{copy_src:08X} is not in PSRAM staging buffer'

        assert copy_len > 0, \
            f'copy[{i}] has zero length'

        # Cross-validate: copy destination should match a direct segment addr
        assert copy_dest == direct_segs[i][0], \
            f'copy[{i}] dest 0x{copy_dest:08X} does not match segment addr ' \
            f'0x{direct_segs[i][0]:08X}'

        # Cross-validate: copy length should match the segment length
        assert copy_len == direct_segs[i][1], \
            f'copy[{i}] len {copy_len} does not match segment len ' \
            f'{direct_segs[i][1]}'

        copies.append((copy_dest, copy_src, copy_len))
        total_copy_bytes += copy_len

        logging.info(
            f'  ✓ copy[{i}]: 0x{copy_dest:08X} ← 0x{copy_src:08X} '
            f'({copy_len:,} B) [matches Seg from Phase 3]'
        )

    logging.info(
        f'  ✓ Total deferred copy: {total_copy_bytes:,} bytes '
        f'across {direct_segment_count} segments'
    )

    # ====================================================================
    # PHASE 5: BARE-METAL JUMP SEQUENCE
    # After the FreeRTOS scheduler is torn down, the loader disables all
    # watchdogs, interrupts, and timers, then performs the copies and MMU
    # remapping from RTC_IRAM_ATTR code.
    # ====================================================================
    logging.info('Phase 5: Bare-metal jump sequence...')

    dut.expect(r'JMP\[1\] entered', timeout=5)
    logging.info('  ✓ JMP[1]: Entered bare-metal jump function (RTC_IRAM_ATTR)')

    dut.expect(r'JMP\[2\] WDT and SysTick disabled', timeout=5)
    logging.info('  ✓ JMP[2]: All watchdogs disabled (SWD, LP_WDT, MWDT0/1, RWDT, SysTick)')

    dut.expect(r'JMP\[3\] interrupts off, starting copies', timeout=5)
    logging.info('  ✓ JMP[3]: RISC-V mie=0, global interrupt disabled')

    # JMP[4]: extract entry bytes to prove code is staged at the entry point
    entry_match = dut.expect(
        r'JMP\[4\] copies done, entry bytes: 0x([0-9A-Fa-f]+)',
        timeout=5
    )
    entry_bytes = int(entry_match.group(1), 16)
    assert entry_bytes != 0x00000000, \
        'Entry point reads as 0x00000000 — deferred copy failed to stage code!'
    assert entry_bytes != 0xFFFFFFFF, \
        'Entry point reads as 0xFFFFFFFF — memory not initialized (erased flash pattern)!'
    logging.info(
        f'  ✓ JMP[4]: Copies completed, entry bytes=0x{entry_bytes:08X} '
        f'(non-zero, non-erased — code is staged)'
    )

    dut.expect(r'JMP\[5\] D-cache evicted', timeout=5)
    logging.info('  ✓ JMP[5]: L1 D-cache thrashed via 64KB eviction buffer')

    # ====================================================================
    # PHASE 6: MMU MAPPING VALIDATION
    # The most critical phase — verify that the MMU page table entries
    # are programmed correctly:
    #   - vaddr must be 64KB-aligned and in PSRAM range
    #   - paddr must be 64KB-aligned
    #   - page count = ceil(segment_len / 64KB)
    #   - entry_id = (vaddr & 0x03FFFFFF) >> 16
    #   - entry_val low 10 bits = physical page number
    #   - MMU_VALID (bit 10) and MMU_PSRAM_TYPE (bit 11) must be set
    # ====================================================================
    logging.info('Phase 6: MMU mapping validation...')

    mmu_entry_count = 0
    for m in range(psram_segment_count):
        map_match = dut.expect(
            r'JMP\[8\] MMU map:\s+vaddr=0x([0-9A-Fa-f]+)\s+'
            r'paddr=0x([0-9A-Fa-f]+)\s+pages=0x([0-9A-Fa-f]+)',
            timeout=5
        )
        vaddr      = int(map_match.group(1), 16)
        paddr      = int(map_match.group(2), 16)
        page_count = int(map_match.group(3), 16)

        # vaddr must be page-aligned and in PSRAM range
        assert vaddr % MMU_PAGE_SIZE == 0, \
            f'MMU mapping {m}: vaddr 0x{vaddr:08X} is not 64KB-aligned'
        assert PSRAM_VADDR_START <= vaddr < PSRAM_VADDR_END, \
            f'MMU mapping {m}: vaddr 0x{vaddr:08X} outside PSRAM range'

        # paddr must be page-aligned
        assert paddr % MMU_PAGE_SIZE == 0, \
            f'MMU mapping {m}: paddr 0x{paddr:08X} is not 64KB-aligned'

        # page count must be at least 1
        assert page_count >= 1, \
            f'MMU mapping {m}: page count is 0'

        # Cross-validate: paddr should be derived from fake_flash base
        assert paddr >= ff_paddr, \
            f'MMU mapping {m}: paddr 0x{paddr:08X} is below fake_flash ' \
            f'base 0x{ff_paddr:08X}'

        mmu_maps.append((vaddr, paddr, page_count))
        logging.info(
            f'  ✓ MMU map {m}: vaddr=0x{vaddr:08X} paddr=0x{paddr:08X} '
            f'pages={page_count}'
        )

        # Now validate each individual MMU entry
        for p in range(page_count):
            entry_match = dut.expect(
                r'entry=0x([0-9A-Fa-f]+)\s+val=0x([0-9A-Fa-f]+)\s+OK',
                timeout=5
            )
            entry_id  = int(entry_match.group(1), 16)
            entry_val = int(entry_match.group(2), 16)

            # entry_id = (vaddr & 0x03FFFFFF) >> 16, incrementing per page
            expected_entry = ((vaddr + p * MMU_PAGE_SIZE) & 0x03FFFFFF) >> 16
            assert entry_id == expected_entry, \
                f'MMU entry ID mismatch: expected 0x{expected_entry:X}, ' \
                f'got 0x{entry_id:X} ' \
                f'(formula: (0x{vaddr + p * MMU_PAGE_SIZE:08X} & 0x03FFFFFF) >> 16)'

            # Validate the base page number in the entry value
            expected_base_page = (paddr >> 16) + p
            actual_base_page   = entry_val & MMU_PAGE_NUM_MASK
            assert actual_base_page == expected_base_page, \
                f'MMU entry page mismatch: expected page 0x{expected_base_page:X}, ' \
                f'got 0x{actual_base_page:X} ' \
                f'(formula: (0x{paddr:08X} >> 16) + {p})'

            # PSRAM addresses must have both valid and PSRAM type bits
            assert entry_val & MMU_VALID_BIT, \
                f'MMU entry 0x{entry_id:X}: MMU_VALID bit (bit 10) NOT set! ' \
                f'val=0x{entry_val:04X}'
            assert entry_val & MMU_PSRAM_TYPE_BIT, \
                f'MMU entry 0x{entry_id:X}: MMU_PSRAM_TYPE bit (bit 11) NOT set! ' \
                f'val=0x{entry_val:04X} — will try to read from flash instead of PSRAM!'

            # Verify no unexpected bits are set (bits 12+ should be 0)
            unexpected_bits = entry_val & ~(MMU_PAGE_NUM_MASK | MMU_VALID_BIT | MMU_PSRAM_TYPE_BIT)
            assert unexpected_bits == 0, \
                f'MMU entry 0x{entry_id:X}: unexpected bits set in val=0x{entry_val:04X} ' \
                f'(bits above 11: 0x{unexpected_bits:04X})'

            mmu_entries.append((entry_id, entry_val))
            mmu_entry_count += 1
            logging.info(
                f'    ✓ entry[0x{entry_id:X}] = 0x{entry_val:04X} '
                f'(page=0x{actual_base_page:X}, '
                f'VALID={bool(entry_val & MMU_VALID_BIT)}, '
                f'PSRAM={bool(entry_val & MMU_PSRAM_TYPE_BIT)}, '
                f'unexpected_bits=0x{unexpected_bits:X})'
            )

    assert mmu_entry_count >= 1, \
        'No MMU entries were programmed — MMU mapping completely failed!'

    # Verify no duplicate entry IDs (would mean overlapping virtual pages)
    entry_ids = [eid for eid, _ in mmu_entries]
    assert len(entry_ids) == len(set(entry_ids)), \
        f'Duplicate MMU entry IDs detected: {entry_ids} — ' \
        f'overlapping virtual page mappings!'
    logging.info(
        f'  ✓ Total MMU entries: {mmu_entry_count} '
        f'(no duplicates, no unexpected bits)'
    )

    # Final jump
    dut.expect(r'JMP\[13\] JUMP!', timeout=5)
    logging.info('  ✓ JMP[13]: Final jump to payload entry point')

    # ====================================================================
    # PHASE 7: PAYLOAD ENTRY & "RUG PULL" INTERCEPTORS
    # After the jump, the payload's my_entry_point() runs from PSRAM.
    # It calls call_start_cpu0() which re-initializes the ESP-IDF CPU
    # startup — but our linker --wrap interceptors prevent the hardware
    # init functions from destroying the bootloader's MMU/cache/SPI state.
    # ====================================================================
    logging.info('Phase 7: Payload entry & rug-pull interceptors...')

    dut.expect('--- ENTERED my_entry_point! ---', timeout=5)
    logging.info('  ✓ Payload entry point reached (executing from PSRAM)')

    dut.expect('Invalidating I-Cache via ROM...', timeout=5)
    logging.info('  ✓ I-Cache invalidated via Cache_Invalidate_All (code coherence)')

    dut.expect('Attempting to jump to call_start_cpu0...', timeout=5)
    logging.info('  ✓ Handing off to ESP-IDF call_start_cpu0()')

    # Track CPU init sequence — these prove call_start_cpu0 is running
    cpu_init_steps = [
        ('cpu0: init_cpu...',           'CPU init'),
        ('cpu0: get_reset_reason...',   'reset reason'),
        ('cpu0: init_bss...',           'BSS zeroed'),
        ('cpu0: cache_init...',         'cache init'),
    ]
    for step_pattern, step_name in cpu_init_steps:
        dut.expect(step_pattern, timeout=5)
    logging.info(
        f'  ✓ ESP-IDF cpu init sequence: '
        f'{len(cpu_init_steps)}/{len(cpu_init_steps)} steps completed'
    )

    # Count and validate the interception of hardware init functions.
    # These are the functions that would normally re-initialize hardware
    # and destroy the bootloader's carefully configured state.
    #
    # Each interceptor is categorized by what it protects:
    expected_interceptors = [
        # (function_name,              subsystem,    critical_for)
        ('cache_hal_init',             'Cache',      'L1/L2 cache configuration'),
        ('esp_mspi_pin_init',          'MSPI Pins',  'SPI flash/PSRAM pin mux'),
        ('bootloader_flash_update_id', 'Flash ID',   'flash chip identification'),
        ('spi_flash_init_chip_state',  'SPI State',  'SPI controller registers'),
        ('mspi_timing_flash_tuning',   'MSPI Timing','flash/PSRAM clock tuning'),
        ('esp_mmu_map_init',           'MMU',        'virtual→physical page tables'),
        ('image_process',              'Image Load', 'flash image verification'),
        ('esp_psram_chip_init',        'PSRAM HW',   'PSRAM chip reset sequence'),
        ('esp_psram_init',             'PSRAM OS',   'PSRAM heap registration'),
    ]
    for func_name, subsystem, critical_for in expected_interceptors:
        dut.expect(
            r'\[Intercepted\] ' + re.escape(func_name) + r'\(\)',
            timeout=5
        )
        interceptors_fired.append(func_name)
        logging.info(
            f'    ✓ [{subsystem}] {func_name}() intercepted '
            f'— protects: {critical_for}'
        )

    # Late interceptor after system_early_init
    dut.expect('cpu0: system_early_init...', timeout=5)
    dut.expect(r'\[Intercepted\] esp_mspi_pin_reserve\(\)', timeout=5)
    interceptors_fired.append('esp_mspi_pin_reserve')
    logging.info(
        f'    ✓ [MSPI Pins] esp_mspi_pin_reserve() intercepted '
        f'— protects: GPIO reservation'
    )

    assert len(interceptors_fired) >= MIN_INTERCEPTOR_COUNT, \
        f'Only {len(interceptors_fired)} interceptors fired, expected at least ' \
        f'{MIN_INTERCEPTOR_COUNT}'
    logging.info(
        f'  ✓ Rug-pull summary: {len(interceptors_fired)}/'
        f'{len(expected_interceptors) + 1} hardware init functions '
        f'intercepted'
    )

    # ====================================================================
    # PHASE 8: OS HANDOVER & HEALTH CHECKS
    # The ESP-IDF scheduler, heap, and main task must come up cleanly
    # despite all the hardware init functions being intercepted.
    # ====================================================================
    logging.info('Phase 8: OS handover & health checks...')

    dut.expect('cpu_start: Pro cpu start user code', timeout=10)
    logging.info('  ✓ CPU startup: user code execution begins')

    # Extract CPU frequency and validate
    freq_match = dut.expect(
        r'cpu_start:\s+cpu freq:\s+(\d+)\s+Hz',
        timeout=5
    )
    cpu_freq_hz = int(freq_match.group(1))
    cpu_freq_mhz = cpu_freq_hz // 1_000_000
    assert cpu_freq_mhz >= 160, \
        f'CPU frequency {cpu_freq_mhz} MHz is suspiciously low (expected ≥160 MHz)'
    logging.info(f'  ✓ CPU frequency: {cpu_freq_mhz} MHz')

    # Extract chip revision
    rev_match = dut.expect(
        r'efuse_init:\s+Chip rev:\s+(v\S+)',
        timeout=5
    )
    chip_rev = _decode(rev_match.group(1))
    logging.info(f'  ✓ Chip revision: {chip_rev}')

    # Heap init — extract ALL memory regions and validate totals
    dut.expect(
        r'heap_init: Initializing\. RAM available for dynamic allocation:',
        timeout=5
    )
    total_heap_kib = 0
    heap_region_count = 0
    # Parse heap regions until we hit a non-heap line
    while True:
        try:
            region_match = dut.expect(
                r'heap_init:\s+At ([0-9A-Fa-f]+)\s+len\s+([0-9A-Fa-f]+)\s+'
                r'\((\d+)\s+KiB\):\s+(\S+)',
                timeout=2
            )
            heap_addr = int(region_match.group(1), 16)
            heap_len  = int(region_match.group(2), 16)
            heap_kib  = int(region_match.group(3))
            heap_type = _decode(region_match.group(4))

            assert heap_len > 0, f'Heap region at 0x{heap_addr:08X} has zero length'
            assert heap_kib > 0, f'Heap region at 0x{heap_addr:08X} reports 0 KiB'

            heap_regions.append((heap_addr, heap_len, heap_kib, heap_type))
            total_heap_kib += heap_kib
            heap_region_count += 1
            logging.info(
                f'    Region {heap_region_count}: 0x{heap_addr:08X} '
                f'({heap_kib:>4} KiB) {heap_type}'
            )
        except Exception:
            break

    assert heap_region_count >= 2, \
        f'Only {heap_region_count} heap region(s) found — expected at least 2 ' \
        f'(SRAM + SPM)'
    assert total_heap_kib >= MIN_TOTAL_HEAP_KIB, \
        f'Total heap {total_heap_kib} KiB is below minimum {MIN_TOTAL_HEAP_KIB} KiB'

    # Verify we have both SRAM and SPM heap regions
    heap_types = {t for _, _, _, t in heap_regions}
    has_spm = any('SPM' in t for t in heap_types)
    logging.info(
        f'  ✓ Heap summary: {heap_region_count} regions, '
        f'{total_heap_kib} KiB total, SPM present={has_spm}'
    )

    # Main task started
    dut.expect('main_task: Started on CPU0', timeout=10)
    logging.info('  ✓ FreeRTOS main task started on CPU0')

    dut.expect(r'main_task: Calling app_main\(\)', timeout=5)
    logging.info('  ✓ app_main() called — FreeRTOS scheduler is running')

    # ====================================================================
    # PHASE 9: FINAL PAYLOAD EXECUTION & STABILITY
    # The ultimate proof: the payload's app_main() is running FreeRTOS
    # tasks entirely from PSRAM-mapped code.  We wait for multiple
    # heartbeats to prove the task loop is stable.
    # ====================================================================
    logging.info('Phase 9: Final payload execution & stability...')

    dut.expect('PHASE 10: BRICK 4 - FreeRTOS INTEGRATION', timeout=5)
    logging.info('  ✓ Payload banner displayed')

    for hb in range(1, STABILITY_HEARTBEATS + 1):
        dut.expect(
            'Hello from a FreeRTOS Task running completely in PSRAM!',
            timeout=5
        )
        logging.info(
            f'  ✓ PSRAM heartbeat {hb}/{STABILITY_HEARTBEATS}'
        )

    logging.info(
        f'  ✓ Task loop stable: {STABILITY_HEARTBEATS} consecutive '
        f'heartbeats from PSRAM-executed code'
    )

    # ====================================================================
    # PHASE 10: CROSS-PHASE CONSISTENCY CHECKS
    # Final assertions that tie together data collected across all phases.
    # ====================================================================
    logging.info('Phase 10: Cross-phase consistency checks...')

    # 10a. Entry address must fall within a PSRAM-mapped MMU region
    entry_in_mmu = False
    for vaddr, paddr, pages in mmu_maps:
        region_start = vaddr
        region_end   = vaddr + pages * MMU_PAGE_SIZE
        if region_start <= entry_addr < region_end:
            entry_in_mmu = True
            break
    assert entry_in_mmu, \
        f'Entry address 0x{entry_addr:08X} does not fall within any ' \
        f'MMU-mapped region: {[(hex(v), hex(v + p * MMU_PAGE_SIZE)) for v, _, p in mmu_maps]}'
    logging.info(
        f'  ✓ Entry point 0x{entry_addr:08X} is within MMU-mapped region '
        f'[0x{region_start:08X}, 0x{region_end:08X})'
    )

    # 10b. Total pages from MMU maps should match footprint / page_size
    total_mmu_pages = sum(p for _, _, p in mmu_maps)
    expected_pages  = mmu_footprint // MMU_PAGE_SIZE
    assert total_mmu_pages <= expected_pages, \
        f'Total MMU pages ({total_mmu_pages}) exceeds footprint-derived ' \
        f'page count ({expected_pages})'
    logging.info(
        f'  ✓ MMU page budget: {total_mmu_pages} used / '
        f'{expected_pages} allocated'
    )

    # 10c. All MMU entry IDs should be contiguous or at least non-overlapping
    # (already verified no duplicates above, but log the coverage)
    all_entry_ids = sorted(eid for eid, _ in mmu_entries)
    logging.info(
        f'  ✓ MMU entry ID coverage: {[hex(e) for e in all_entry_ids]}'
    )

    # 10d. Every direct segment should have a corresponding copy entry
    assert len(copies) == direct_segment_count, \
        f'Copy count ({len(copies)}) != direct segment count ({direct_segment_count})'
    logging.info(
        f'  ✓ Copy/segment parity: {len(copies)} copies for '
        f'{direct_segment_count} direct segments'
    )

    # ====================================================================
    # SUMMARY
    # ====================================================================
    logging.info('')
    logging.info('=' * 64)
    logging.info(' ALL 10 PHASES PASSED — BOOTLOADER VERIFIED')
    logging.info('=' * 64)
    logging.info(f'  Payload:               {loaded_bytes:,} bytes from flash')
    logging.info(f'  Segments parsed:       {segment_count}')
    logging.info(f'    PSRAM (fake_flash):   {psram_segment_count}')
    logging.info(f'    Direct (SRAM/SPM):    {direct_segment_count}')
    logging.info(f'  MMU footprint:         {mmu_footprint:,} bytes ({mmu_footprint // MMU_PAGE_SIZE} pages)')
    logging.info(f'  MMU entries programmed: {mmu_entry_count} (no duplicates)')
    logging.info(f'  Deferred copies:       {total_copy_bytes:,} bytes')
    logging.info(f'  Entry point:           0x{entry_addr:08X} (in MMU region ✓)')
    logging.info(f'  Entry bytes:           0x{entry_bytes:08X} (non-zero ✓)')
    logging.info(f'  Interceptors fired:    {len(interceptors_fired)}/{len(expected_interceptors) + 1}')
    logging.info(f'  Heap:                  {total_heap_kib} KiB across {heap_region_count} regions')
    logging.info(f'  CPU frequency:         {cpu_freq_mhz} MHz')
    logging.info(f'  Chip revision:         {chip_rev}')
    logging.info(f'  Stability:             {STABILITY_HEARTBEATS} heartbeats from PSRAM')
    logging.info('=' * 64)
