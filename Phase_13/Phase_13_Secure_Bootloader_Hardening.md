# Phase 13: Flash-Fill Anti-Phishing Proof on the Stateless Loader

- **Date:** August 5, 2026
- **Author:** Mayank (wolgwang)
- **Project:** SeedSigner Secure Boot — Summer of Bitcoin

## 1. Overview

Phase 12 delivered a complete stateless secure boot chain on ESP32-P4: the 2nd-stage bootloader is RSA-signed and verified by the ROM/eFuse root of trust (Layer 1, Secure Boot V2), and the app loader (`seedsigner_secure_loader`) cryptographically verifies every SD-card payload via secp256k1 multisig against embedded `vendor_keys[]` (Layer 2, Specter crypto). The payload executes entirely from PSRAM with no flash writes.

Phase 13 adds the **flash-fill anti-phishing proof** designed in Phase 7 — the one hardening feature that is tamper-evident yet fully reversible (**no eFuse burns**, testable on real silicon with virtual eFuses):

```
ESP-IDF 2nd-stage bootloader (0x2000)
        →  stateless secure loader (0x30000)
            - mounts the SD card, reads /sdcard/seedsigner_esp32p4.bin into PSRAM
            - unmounts the SD card immediately (TOCTOU-safe)
            - verifies the Specter bundle (secp256k1 multisig)   [Phase 12]
            - PROVISION (first boot): fills the ~6.8 MB random_fill
              partition with TRNG data, SHA-256 hashes it, stores the
              digest in the nvs partition                        [Phase 13]
            - VERIFY (every boot): re-hashes random_fill, compares
              against the stored digest, derives 4 BIP-39 words   [Phase 13]
            - tamper detected → words change / boot halts
        →  JMP[1..8] stateless handoff (WDT/int/PMP teardown, cache
           eviction, MMU PSRAM remap)  [unchanged from Phase 11/12]
        →  payload executes entirely from PSRAM
```

The 4 BIP-39 words are printed to UART right before the handoff (headless dev board — UART is the only output). Any modification of the filled region changes the hash → different words → the user is alerted to tampering. In dev mode the proof is **software SHA-256 only**; the production upgrade to an eFuse-bound HMAC (KEY5) is designed in [Production_Hardening_Plan.md](./Production_Hardening_Plan.md) for a future phase.

**Why flash-fill only?** Every other hardening feature requires an **irreversible eFuse burn** or an IDF version I don't have yet:

| Feature | Irreversible? | Blocked? | Phase 13? |
|---|---|---|---|
| Flash-fill anti-phishing (SHA-256 only) | No — flash is re-erasable | No | **This phase** |
| HMAC eFuse binding (KEY5) | Permanent eFuse burn | HMAC peripheral doesn't work with virtual eFuses | Future |
| Anti-rollback (`SECURE_VERSION`) | Each burn consumes 1 of 16 lifetime slots | — | Future |
| NVS encryption (HMAC KEY4) | Permanent eFuse burn | HMAC peripheral doesn't work with virtual eFuses | Future |
| Flash encryption (KEY3) | Burns KEY3 + `SPI_BOOT_CRYPT_CNT` | Requires IDF ≥ 6.1 (I'm on v5.5) | Future |
| Key rotation (KEY0/KEY1/KEY2) | Permanent eFuse burn per slot | — | Future |
| Lockdown menu | Permanently bricks chip for unsigned FW | — | Future |

## 2. Target

1. Take `Phase_12/seedsigner_bootloader_p4_stateless_os` as the base loader.
2. Add the flash-fill anti-phishing logic as a new module (`anti_phish.c`/`anti_phish.h`) plus the embedded `bip39_wordlist.c` (2048 BIP-39 words).
3. Extend `partitions.csv`: a plaintext `nvs` partition (48 KB, stores the provisioned digest) and the `random_fill` data partition (~6.8 MB, subtype `0x06`) covering all remaining flash.
4. Run the proof **between** the Specter signature verification and the JMP handoff in `main.c` — provision on first boot, verify + print words every boot.
5. Fix the RAM spill caused by the new component set (`sram_high` elimination in `main/ld/loader_high.ld`) so the hardened loader fits and boots.
6. Verify on hardware: words stable across reboots, change on flash tamper, and the full Specter chain still passes.

## 3. What was copied / changed

| Artifact | From | To |
|---|---|---|
| Loader (build system, `main/main.c`, `partitions.csv`, `sdkconfig.defaults`) | `Phase_12/seedsigner_bootloader_p4_stateless_os` | `Phase_13/seedsigner_bootloader_p4_stateless_os` |
| `specter_crypto` component | Phase 12 (verbatim) | Phase 13 — **unchanged** |
| Anti-phishing module `main/anti_phish.c` + `main/anti_phish.h` | — | **New** — TRNG flash-fill, SHA-256 digest, BIP-39 word derivation, tamper check |
| BIP-39 wordlist `main/bip39_wordlist.c` | — | **New** — full 2048-word English list embedded in flash |
| `main/ld/loader_high.ld` | Phase 12 (declared `sram_high` at `0x4FFA0000`) | **Updated** — `sram_high` eliminated (len 0), `sram_low` kept in the safe `0x4FF40000`–`0x4FFA0000` (384 KB) window |
| `partitions.csv` | Phase 12 (no `nvs`, no `random_fill`) | **Updated** — adds `nvs` (data `0x99`, 48 KB) + `random_fill` (data `0x06`, ~6.8 MB), leaving **0 bytes unallocated** |
| Run script `run.sh` | Phase 12 | Phase 13 (header label; still payload-agnostic — payload scripts stay in Phase 12) |
| Signing key `secure_boot_signing_key.pem` | Phase 12 | Phase 13 (same RSA key — eFuse digest matches) |

## 4. Partition layout (Phase 13)

```
# Name,     Type,  Subtype,  Offset,    Size,    Flags
nvs,        data,  0x99,     0x21000,   0xC000,           # 48 KB — anti-phish digest + provisioned flag (plaintext)
otadata,    data,  ota,      0x2D000,   0x2000,           # 8 KB (required by IDF)
phy_init,   data,  phy,      0x2F000,   0x1000,           # 4 KB
factory,    app,   factory,  0x30000,   0x100000,         # 1 MB — seedsigner_secure_loader
efuse,      data,  0x05,     0x130000,  0x2000,           # 8 KB — virtual eFuse persistence
random_fill,data,  0x06,     0x132000,  0x6CE000,         # ~6.8 MB — TRNG anti-phishing fill region
```

The `random_fill` region spans `0x132000`–`0x800000` — every byte of flash the boot path doesn't use is filled with TRNG data and hashed, so the digest is a fingerprint of all *unallocated* storage. The bootloader occupies `0x2000`–`0x20000`, the partition table `0x20000`, and the loader app `0x30000` (all unchanged from Phase 12).

## 5. Integration details

### `anti_phish.c` — provisioning + every-boot verification

The module talks to the `nvs` and `random_fill` partitions directly via the `esp_partition` API (no NVS flash driver, no crypto beyond `sha2.h`):

- **`provision_flash_fill()`** (first boot only): reads the first bytes of the `nvs` partition; if the `APOK` magic is present the region is already provisioned and it returns immediately. Otherwise it finds `random_fill`, erases it, fills it in 4 KB chunks with `esp_fill_random()` (TRNG), computes SHA-256 over the whole region, erases the first 4 KB sector of `nvs`, and writes `anti_phish_state_t { magic = "APOK", hash[32] }` at offset 0. Provisioning ~6.8 MB takes about 2 minutes (TRNG + SPI flash); a `vTaskDelay(1)` every 64 sectors (256 KB) yields to the IDLE task so the FreeRTOS watchdog never trips.
- **`verify_anti_phishing_proof(char words[4][12])`** (every boot): checks the `APOK` magic is present, re-hashes the entire `random_fill` region, and `memcmp`s against the stored digest. A mismatch logs `FLASH TAMPERED! Hash mismatch detected.` and returns `ESP_ERR_INVALID_STATE` (the caller halts boot). On match it derives and logs the 4 BIP-39 words:
  ```
  ANTI-PHISHING PROOF: <w0> <w1> <w2> <w3>
  ```
- **`derive_bip39_words()`** — extracts 11 bits (MSB-first) per word from the 32-byte digest and indexes into `bip39_wordlist[2048]`; 4 words = 44 bits of entropy.

### `bip39_wordlist.c`

The full standard English BIP-39 list (2048 words, ~25 KB flash: pointer table + string literals) so users recognize the words and can cross-check against a printed card. `bip39_wordlist[]` is declared `extern const char *` in `anti_phish.c` and linked into the loader via `main/CMakeLists.txt`.

### `main.c` — integration point

The proof runs **after** the Specter multisig verification and **before** image-header validation, the PSRAM copies, and the JMP handoff:

```c
// Phase 13: Anti-phishing proof
provision_flash_fill();       // no-op if already provisioned
char words[4][12];
esp_err_t ap_err = verify_anti_phishing_proof(words);
if (ap_err == ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "FLASH TAMPERED — halting boot");
    memset(psram_buf, 0, fw_size);
    while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}
```

The words are logged to UART; in the headless dev setup (no LCD) that is the only output the user can check against their recorded card. The loader never writes to the payload's SD bundle — the fill region is separate flash, so statelessness of the payload path is preserved.

### `loader_high.ld` — RAM spill fix (the reason the build needed hardening)

In Phase 12, `loader_high.ld` declared `sram_high` at `0x4FFA0000`. That region **statically overlaps the ESP32-P4's hardware L2 cache**. Phase 12's loader was small enough that `sram_high` stayed empty — but adding `anti_phish.c` pulled in extra ESP-IDF components (partitions, TRNG, SPI flash), spilling `.bss` into `sram_high`. When the hardware L2 cache was enabled during early boot it corrupted the spilled `s_mmu_ctx` variables → `abort()` during partition loading.

Fix: eliminate `sram_high` entirely (`len = 0x0`) and keep `sram_low` within the safe `0x4FF40000`–`0x4FFA0000` (384 KB) window, plus `CONFIG_COMPILER_OPTIMIZATION_SIZE=y` to fit the hardened loader into RAM. The loader's footprint stays clear of the payload copy region `0x4FF00000`–`0x4FF20000` and the ROM data/stack.

## 6. Security analysis (Phase 13 — SHA-256 only, no HMAC)

The proof is flash-fill + SHA-256 → 4 BIP-39 words, without HMAC eFuse binding:

| Defense | Effective Against | NOT Effective Against |
|---|---|---|
| Tamper detection (hash mismatch) | Naive secret stashing, supply chain reflashing | — |
| 4 BIP-39 words (44-bit space) | Casual attacker (no code execution) | Sophisticated attacker with PSRAM execution (~557 years for 44 bits at hardware SHA rate, but only ~7 hours at PSRAM software rate) |
| Evil Maid device swap | — | Requires HMAC eFuse binding (device-bound secret) — future phase |

Why this is still useful without HMAC:
1. **Tamper detection works** — any flash modification changes the hash → words change → user alerted.
2. **44-bit brute-force from PSRAM** is far harder than the old 22-bit (2 words): ~7 hours vs ~84 seconds. Not infeasible, but a meaningful improvement.
3. **The code path is the same as the HMAC upgrade** — when KEY5 is burned, only the word-derivation input changes (HMAC output instead of raw SHA-256); the fill, hash, and verification logic stay. See the drop-in diff in [Production_Hardening_Plan.md](./Production_Hardening_Plan.md).

## 7. Verification status

- **Builds:** `Phase_13/seedsigner_bootloader_p4_stateless_os` compiles clean under ESP-IDF v5.5 — `seedsigner_secure_loader.bin` (397 KB) + bootloader + partition table all present; `run.sh` builds and flashes with the `CONFIG_EFUSE_VIRTUAL=y` pre-flight guard.
- **On-hardware checklist** (board attached):

| Check | Method | Expected |
|---|---|---|
| First-boot provisioning | Boot with empty `random_fill` partition | `Filling with TRNG random data...` → `Flash fill provisioned` → 4 words displayed (~2 min) |
| Stable words across reboots | Reboot 3× | Same 4 words every time (fast re-hash path) |
| Tamper detection | `esptool.py write_flash 0x132000 ...` (overwrite a sector) | Next boot: `FLASH TAMPERED — halting boot` |
| Words differ per device | Flash same loader on two boards | Different words (different TRNG fill) |
| Specter chain intact | Boot with signed payload on SD card | `Signature verification PASSED!` → words → `JMP[1..8]` → payload runs |
| MicroPython payload | Signed MP bundle on SD → boot | `>>>` REPL with working UART input, words displayed before handoff |

## 8. Next steps

The production hardening roadmap — HMAC eFuse binding (KEY5, upgrades this proof from 44-bit SHA-256 to device-bound HMAC with ~557-year brute-force), anti-rollback via `SECURE_VERSION`, NVS encryption via HMAC KEY4, XTS-AES-128 flash encryption (needs IDF ≥ 6.1), 3-slot RSA-3072 key rotation, and the on-device lockdown menu — is fully designed, with an eFuse key-block budget and a dev-mode software-HMAC fallback, in **[Production_Hardening_Plan.md](./Production_Hardening_Plan.md)**.

## 9. Notes / caveats

- **No eFuse burns in this phase** — everything is reversible. The `random_fill` region can be erased and re-provisioned anytime.
- **The `nvs` partition is plaintext** — it stores the digest as raw bytes (not via the NVS flash driver, no encryption). The digest is only a cache: it is re-computed and compared every boot, so tampering with the stored digest itself causes a mismatch and a halt.
- **SHA-256 only (no HMAC)** — the 44-bit brute-force exposure (Phase 7 Section 3) is partially mitigated by 4 words vs 2, but not fully closed. HMAC binding is a future phase.
- **Zero unallocated flash** — `random_fill` (0x6CE000) + `nvs` (0xC000) leave exactly 0 bytes free on the 8 MB chip, so the digest fingerprints *all* unused storage.
- **First boot is slow** (~2 min for TRNG fill + hash); subsequent boots re-hash ~6.8 MB in a fraction of a second with the Specter `sha2` software implementation.
- **Virtual eFuses unaffected** — this phase never touches the HMAC peripheral; all crypto is the Specter `sha2` software SHA-256 implementation from `components/specter_crypto/crypto/sha2.c`.

## 10. Artifacts

- `Phase_13/seedsigner_bootloader_p4_stateless_os/` — loader with `main/anti_phish.c`/`.h`, `main/bip39_wordlist.c`, updated `partitions.csv`/`main.c`/`loader_high.ld`/`run.sh`
- `Phase_13/Phase_13_Secure_Bootloader_Hardening.md` — this document
- `Phase_13/Production_Hardening_Plan.md` — future hardening roadmap (HMAC, anti-rollback, NVS, flash encryption, lockdown, keys)
