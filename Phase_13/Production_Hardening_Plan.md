# Production Hardening Plan — HMAC eFuse Binding, Anti-Rollback, NVS Encryption, Flash Encryption, Key Rotation, Lockdown

> **Status:** **Planned** — none of this is implemented yet. This document captures the full production hardening roadmap designed in Phase 13 research. Each section is a candidate for a future phase.
>
> **Companion to:** [Phase_13_Secure_Bootloader_Hardening.md](./Phase_13_Secure_Bootloader_Hardening.md) (Phase 13 implements flash-fill anti-phishing only).

- **Date:** August 5, 2026
- **Author:** Mayank (wolgwang)
- **Project:** SeedSigner Secure Boot — Summer of Bitcoin

---

## eFuse Key Block Budget

The ESP32-P4 has **6 eFuse key blocks** (KEY0–KEY5). The full plan allocates all 6:

| Block | Use | Purpose | Irreversible? |
|---|---|---|---|
| KEY0 | Secure Boot Digest 0 | RSA-3072 primary signing key | Yes **[IRREVERSIBLE]** |
| KEY1 | Secure Boot Digest 1 | RSA-3072 rotation #1 | Yes **[IRREVERSIBLE]** |
| KEY2 | Secure Boot Digest 2 | RSA-3072 rotation #2 | Yes **[IRREVERSIBLE]** |
| KEY3 | Flash Encryption | XTS-AES-128 | Yes **[IRREVERSIBLE]** **[BLOCKED: IDF ≥ 6.1]** |
| KEY4 | NVS Encryption HMAC | `HMAC_UP` (UPSTREAM) | Yes **[IRREVERSIBLE]** |
| KEY5 | Anti-phishing HMAC | `HMAC_UP` (UPSTREAM) | Yes **[IRREVERSIBLE]** |

> **WARNING: Zero spare blocks.** If any future feature needs a key block (DS module for hardware-backed TLS, JTAG soft-re-enable for field debug), there are none remaining. Consider starting with **2 Secure Boot rotation slots** (KEY0 + KEY1) instead of 3, keeping KEY2 as a spare. This reduces key-compromise tolerance from 2 rotations to 1, but preserves flexibility.

---

## 1. HMAC eFuse Binding for Anti-Phishing (KEY5)

**Upgrades Phase 13's SHA-256-only proof to device-bound HMAC.**

- **[IRREVERSIBLE: burning KEY5 is permanent]**
- KEY5 burned with **`HMAC_UP` (`HMAC_KEY_UPSTREAM`) purpose** — firmware (including the payload in PSRAM) *can* call `esp_hmac_calculate()`, but the **raw 256-bit key is read-protected** and never leaves the eFuse block.
- Security comes from **hardware rate-limiting + entropy**: at ~1 ms/call, brute-forcing 2^44 combinations (4 words) takes **~557 years**.
- ~~(Previous draft said `HMAC_KEY_DOWNSTREAM` — that is wrong: `DOWNSTREAM` routes the HMAC output internally to the Digital Signature peripheral or JTAG re-enablement hardware, not to software. The bootloader itself could not compute anti-phishing words with a `DOWNSTREAM` key.)~~

### Virtual eFuse Limitation

The HMAC **hardware peripheral** does NOT work with `CONFIG_EFUSE_VIRTUAL=y` — it reads key material from the **physical eFuse registers**, not the RAM emulation. `esp_hmac_calculate()` fails with `ESP_ERR_HW_CRYPTO_DS_HMAC_FAIL` when keys are virtual.

**Dev-mode fallback:** compile-time software HMAC via mbedTLS (`mbedtls_md_hmac()`) with a hardcoded test key, gated by `#ifdef CONFIG_EFUSE_VIRTUAL`.

### Upgrade Path (from Phase 13)

```diff
 // In verify_anti_phishing_proof():
-    derive_bip39_words(current_hash, words);
+    uint8_t hmac_out[32];
+#ifdef CONFIG_EFUSE_VIRTUAL
+    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
+                    test_key, 32, current_hash, 32, hmac_out);
+#else
+    esp_hmac_calculate(HMAC_KEY5, current_hash, 32, hmac_out);
+#endif
+    derive_bip39_words(hmac_out, words);
```

---

## 2. Anti-Rollback via eFuse `SECURE_VERSION`

**[IRREVERSIBLE: each burn consumes one of 16 lifetime slots]**

Mirrors Kern Phase 6c: 16-bit unary-encoded counter in the `SECURE_VERSION` eFuse field.

```c
// Called after Specter signature verification, before JMP
bool check_secure_version(uint32_t payload_version) {
    uint16_t burned_version = 0;
    esp_efuse_read_field_blob(ESP_EFUSE_SECURE_VERSION, &burned_version, 16);
    // SECURE_VERSION is 16-bit unary-encoded (0x0000 → 0xFFFF)
    // Count leading 1-bits = version number
    int version = __builtin_popcount(burned_version);
    if (payload_version <= version) {
        ESP_LOGE(TAG, "Anti-rollback: payload v%lu <= burned v%d", payload_version, version);
        return false;
    }
    // Burn next version (set next 0-bit to 1)
    // Note: unary encoding means the version can only INCREASE
    return true;
}
```

### Two Separate Anti-Rollback Mechanisms

1. **IDF-native (Layer 1, loader self-rollback):** `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=y` in `sdkconfig.secure` — the 2nd-stage bootloader checks the loader binary's own `esp_app_desc_t.secure_version` against the eFuse counter. This prevents flashing an older loader.
2. **Custom (Layer 2, payload rollback):** `check_secure_version()` in the loader checks the Specter bundle's `bl_section_t.pl_ver` against the same eFuse counter. This prevents booting an older SD-card payload.

Both mechanisms share the same 16-bit `SECURE_VERSION` eFuse field. The loader's own `secure_version` and the payload's `pl_ver` must be coordinated (loader version ≤ payload version at all times).

### Versioning Strategy (16-Shot Budget)

- **Do NOT bump `SECURE_VERSION` on every release** — only on security-critical updates that must invalidate all prior versions
- Feature releases and bug fixes should reuse the current `pl_ver` (they are still signature-verified by Specter multisig)
- **Recommended cadence:** ~1 bump per major security patch = ~16 major security updates over device lifetime
- Consider splitting `bl_section_t` into `min_secure_version` (checked against eFuse) and `build_version` (informational, not eFuse-gated) for finer-grained version tracking without consuming eFuse bits

### Dev Mode

Virtual eFuses (`CONFIG_EFUSE_VIRTUAL=y` + `KEEP_IN_FLASH`) handle `esp_efuse_read_field_blob()` correctly — the counter can be "reset" by erasing the `efuse` partition.

---

## 3. NVS Encryption via HMAC KEY4

**[IRREVERSIBLE: burning KEY4 is permanent]**

Mirrors Kern Phase 3 (`core/nvs_secure.c`):

```c
// Called early in loader init (before SD mount)
esp_err_t nvs_secure_init(void) {
    // Check if KEY4 is provisioned (burned + read-protected)
    bool key4_provisioned = false;
    esp_efuse_read_field_blob(ESP_EFUSE_KEY_PURPOSE_4, &key4_provisioned, 1);
    
    if (key4_provisioned) {
        nvs_sec_cfg_t cfg = { .key_id = NVS_SEC_HMAC_EFUSE_KEY_ID_4 };
        return nvs_flash_secure_init(&cfg);
    } else {
        // First boot / dev mode: plaintext NVS, but NEVER call nvs_flash_init()
        // (its keygen path would burn KEY4 without consent)
        return nvs_flash_init_partition("nvs");
    }
}
```

- `CONFIG_NVS_ENCRYPTION=y`, `CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC=y`, `CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID=4`
- NVS partition placed after the app loader region (e.g. `0x140000`). **Not** at `0x11000` — that address overlaps the bootloader region on some layouts.
- Key derivation happens at runtime inside the HMAC peripheral — the XTS-AES-256 NVS key is **never in software memory**
- **WARNING: Virtual eFuse limitation (same as KEY5):** HMAC peripheral does NOT work with virtual eFuses. Dev mode falls back to plaintext NVS.

---

## 4. Flash Encryption (XTS-AES-128 in KEY3)

**[BLOCKED: requires ESP-IDF ≥ 6.1; project currently on v5.5]**
**[IRREVERSIBLE: burns KEY3 + `SPI_BOOT_CRYPT_CNT`; even dev-mode FE has a 3-reflash limit]**

- **Only the immutable loader artifacts are flash-encrypted**: bootloader (0x2000), partition table (0x20000), app loader (0x30000)
- **Payload is on SD card** → never touches flash → stateless model preserved
- **Key**: XTS-AES-128 generated by FE bootloader from TRNG on first boot, **[IRREVERSIBLE]** burned to KEY3, read-protected at burn time. Even in development mode (`CONFIG_FLASH_ENCRYPTION_MODE_DEVELOPMENT`), flash encryption burns `SPI_BOOT_CRYPT_CNT` with a **3-reflash limit** before the counter saturates. **Do NOT enable on dev boards.**
- **Requires ESP-IDF ≥ 6.1** for `CONFIG_SPIRAM_ENC_EXEMPT` (PSRAM DMA masters — MIPI DSI, camera — must access PSRAM outside the MMU "sensitive" bit path). The Waveshare P4 board uses MIPI-DSI for display — the DSI DMA must allocate buffers from the exempt region.
- `sdkconfig.secure` sets:
  ```
  # UNCOMMENT AFTER IDF UPGRADE TO >= 6.1:
  # CONFIG_FLASH_ENCRYPTION_ENABLED=y
  # CONFIG_FLASH_ENCRYPTION_MODE_RELEASE=y
  # CONFIG_SPIRAM_ENC_EXEMPT=y
  ```

---

## 5. Three-Slot RSA-3072 Key Rotation (KEY0/KEY1/KEY2)

**[IRREVERSIBLE: eFuse digest slots are one-time-write; revocation is permanent]**

| Slot | eFuse Block | Purpose | Rotation |
|---|---|---|---|
| 0 | KEY0 | Primary signing key digest | Active |
| 1 | KEY1 | Rotation #1 | Standby |
| 2 | KEY2 | Rotation #2 | Standby |

- Bootloader (`seedsigner_secure_loader.bin`) is signed with **all three keys** at initial flash (frozen forever — never OTA updated)
- Payloads are signed with the **current primary** (slot 0)
- On key compromise: burn `SECURE_BOOT_KEY_REVOKE0` **[IRREVERSIBLE]** → device accepts slots 1/2
- Two independent compromises absorbable before device is bricked. **WARNING: Revoking 2 of 3 slots leaves a single point of failure — losing the last key permanently bricks the device.**
- Keys generated offline, stored in HSM / air-gapped signing PC

### Payload Verification (Layer 2) — secp256k1, 3 Vendor Keys

| Key | Purpose |
|---|---|
| `vendor_keys[0]` | Primary (offline) |
| `vendor_keys[1]` | Rotation #1 |
| `vendor_keys[2]` | Rotation #2 |

- `blsig_verify_multisig()` accepts any valid signature from the active set
- Rotation: add new key to `vendor_keys[]` in a new loader build, sign payloads with it
- No eFuse involvement — pure software key rotation

---

## 6. On-Device Lockdown Menu

**[IRREVERSIBLE: permanently bricks the chip for unsigned firmware]**

> **WARNING: COMPILE-TIME GATE:** The lockdown component **must be disabled by default** in `sdkconfig.defaults` (`CONFIG_SEEDSIGNER_LOCKDOWN_ENABLE=n`). It is only enabled in `sdkconfig.secure` on the production signing machine. `run.sh` must verify that lockdown is disabled when `CONFIG_EFUSE_VIRTUAL=y`. All code in `lockdown_menu.c` and `efuse_provision.c` must be wrapped in `#if CONFIG_SEEDSIGNER_LOCKDOWN_ENABLE`.

Trigger: hold **BOOT button (GPIO 0)** + **RESET** for 3 seconds → enters lockdown menu on UART.

```
=== SeedSigner Secure Lockdown ===
This will PERMANENTLY burn eFuses and lock the device.
Current state:
  SECURE_BOOT_EN:       [NOT BURNED]
  DIS_DIRECT_BOOT:      [NOT BURNED]
  DIS_DOWNLOAD_MODE:    [NOT BURNED]
  JTAG_DISABLE:         [NOT BURNED]
  FLASH_ENCRYPTION:     [NOT BURNED]
  KEY0/1/2 digests:     [NOT BURNED]
  KEY3 (XTS-AES):       [NOT BURNED]
  KEY4 (NVS HMAC):      [NOT BURNED]
  KEY5 (Anti-phish):    [NOT BURNED]
  RD_DIS (write-protect): [NOT BURNED]

WARNING: This action is IRREVERSIBLE.
Enter your 6-digit PIN to confirm: ______
```

- PIN verified via `esp_hmac_calculate(HMAC_KEY4, PIN)` → decrypts NVS to confirm (requires physical eFuses — lockdown menu is production-only)
- **[IRREVERSIBLE]** On confirm: burns all hardening eFuses in sequence, then `SECURE_BOOT_EN`, then write-protects `RD_DIS`
- After lockdown: device **only** boots signed loader + signed payloads; JTAG/UART download disabled; flash encrypted

---

## 7. Host-Side Provisioning (Signing Machine)

`tools/provision_efuses.py` runs on the air-gapped signing PC (connected via USB):

```python
# [ALL IRREVERSIBLE] Burns all keys/digests in one session:
# 1. Generate RSA-3072 keypair → save private key offline, burn 3 digests to KEY0/1/2 [IRREVERSIBLE]
# 2. Generate XTS-AES-128 key → burn to KEY3 (read-protected) [IRREVERSIBLE] [BLOCKED: IDF ≥ 6.1]
# 3. Generate HMAC keys for KEY4 (HMAC_UP) / KEY5 (HMAC_UP) → burn (read/write-protected) [IRREVERSIBLE]
# 4. Set SECURE_BOOT_EN, DIS_DIRECT_BOOT, DIS_DOWNLOAD_MODE, JTAG_DISABLE [IRREVERSIBLE]
# 5. Write-protect RD_DIS [IRREVERSIBLE]
# 6. Verify by reading back digests and comparing
```

- Uses `espefuse.py` / `espsecure.py` under the hood
- Logs every burn step with SHA-256 of the keys for audit trail
- **Never run on a development board** — only on production devices at the factory/signing station
- **WARNING: KEY5 purpose is `HMAC_UP` (not `DOWNSTREAM`)** — see Section 1

---

## 8. Project-Hardened Release Signing

- Replace the Phase 9/12 dummy `vendor_keys[]` and `secure_boot_signing_key.pem` with offline-held keys
- Air-gapped signing PC
- Signed sparse Intel HEX for release artifacts
- `tools/generate_signed_payload.py` updated to accept `--secure-version N` and embed it in the `bl_section_t` header

---

## `sdkconfig.secure` Overlay (Production Only)

```
# SECURE BOOT V2 (RSA-3072)
CONFIG_SECURE_BOOT=y
CONFIG_SECURE_BOOT_V2_ENABLED=y
CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME=y
CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=y
CONFIG_SECURE_BOOT_VERIFICATION_KEY_EFUSE=y

# ANTI-ROLLBACK
CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=y
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y

# NVS ENCRYPTION (HMAC KEY4)
CONFIG_NVS_ENCRYPTION=y
CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC=y
CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID=4

# FLASH ENCRYPTION (XTS-AES-128 KEY3) [BLOCKED: IDF >= 6.1 required]
# CONFIG_FLASH_ENCRYPTION_ENABLED=y     # UNCOMMENT AFTER IDF UPGRADE
# CONFIG_FLASH_ENCRYPTION_MODE_RELEASE=y # UNCOMMENT AFTER IDF UPGRADE
# CONFIG_SPIRAM_ENC_EXEMPT=y             # UNCOMMENT AFTER IDF UPGRADE

# HARDENING EFUSES (burned by lockdown / provision_efuses.py)
CONFIG_SECURE_DISABLE_ROM_DL_MODE=y
CONFIG_SECURE_BOOT_LOCK_KEYS=y
CONFIG_SECURE_BOOT_DISABLE_DIRECT_BOOT=y
CONFIG_SECURE_BOOT_ENABLE_AGGRESSIVE_KEY_REVOKE=y
CONFIG_SECURE_UART_ROM_DL_MODE=y
CONFIG_SECURE_FLASH_ENC_ENABLED=y

# VIRTUAL EFUSES — ONLY FOR DEV (run.sh verifies this is DISABLED in production)
CONFIG_EFUSE_VIRTUAL=n

# LOCKDOWN MENU (production only)
CONFIG_SEEDSIGNER_LOCKDOWN_ENABLE=y
```

**Policy (from Kern):** `sdkconfig.secure` is **never committed** to the repo. It lives only on the signing machine and is applied during the release build. Development builds use `sdkconfig.defaults` with `CONFIG_EFUSE_VIRTUAL=y`, `CONFIG_SEEDSIGNER_LOCKDOWN_ENABLE=n`, and no hardening eFuses.

---

## Production Boot Chain (Target End State)

```
ESP32-P4 ROM (hardware root)
    → 2nd-stage bootloader (0x2000)
        - RSA-3072 signature verified against KEY0/KEY1/KEY2 digests in eFuse
        - SECURE_BOOT_EN burned, DIS_DIRECT_BOOT, DIS_DOWNLOAD_MODE, JTAG disabled
        - Flash encryption: XTS-AES-128 key in KEY3 (read-protected at burn)
    → App loader `seedsigner_secure_loader` (0x30000)
        - Runs from flash (XIP), verified by Layer 1 above
        - Mounts SD card (on-chip LDO4, 4-bit, 20 MHz), reads `/sdcard/seedsigner_esp32p4.bin` → PSRAM
        - **Unmounts SD card immediately** (TOCTOU-safe)
        - Verifies Specter bundle:
            1. `blsect_validate_header()` — magic, revision, CRC
            2. Platform attr == `"seedsigner_esp32p4"`
            3. `pl_ver >= 1` AND `pl_ver > burned_secure_version` (anti-rollback)
            4. SHA-256 over main section (header + payload) via `blsys_flash_read` → PSRAM `memcpy`
            5. Locate `sign` section, build Bech32 message, `blsig_verify_multisig()` against `vendor_keys[]`
        - **Flash-fill anti-phishing proof:**
            - On first boot (provisioning): fill all unallocated flash with TRNG (`esp_fill_random`)
            - On every boot: SHA-256(random_flash) → `esp_hmac_calculate(HMAC_KEY5, hash)` → 4 BIP-39 words
            - Display words; user verifies against their recorded card
        - **Anti-rollback:** read `SECURE_VERSION` eFuse (16-bit unary); if `pl_ver <= burned_version` → halt
        - MMU PSRAM remap + WDT/int/PMP teardown (`JMP[1..8]`)
        → Payload executes from PSRAM (stateless)
```

---

## Verification Status (All Features — Target)

| Check | Method | Expected | Requires Physical eFuse? |
|---|---|---|---|
| Unsigned loader rejected | Flash tampered `seedsigner_secure_loader.bin` | ROM: `Error verifying app image` | No (virtual eFuses work for SB V2 signature verify) |
| Anti-rollback | Flash payload with `pl_ver=1` after `SECURE_VERSION=2` burned | Loader: `Anti-rollback: payload v1 <= burned v2` → halt | No (virtual eFuses work for `esp_efuse_read_field_blob`) |
| Flash tamper detection | Erase one sector of filled flash region | Next boot: anti-phishing words **change** → user alerted | **Yes** for HMAC path; dev mode uses software HMAC fallback |
| NVS encryption | Write/read setting via NVS API | Data encrypted on flash; `nvs_secure_init()` decrypts | **Yes** for HMAC key derivation; dev mode uses plaintext NVS |
| HMAC-KEY5 anti-phishing | Verify words are device-bound and rate-limited | `esp_hmac_calculate(KEY5, hash)` returns HMAC; words derived from it | **Yes** (`esp_hmac_calculate` fails with virtual eFuses); dev mode uses software fallback |
| Key rotation | Revoke KEY0 (`SECURE_BOOT_KEY_REVOKE0`), sign loader with KEY1 | Device boots with KEY1 digest | **Yes** (revocation is eFuse burn; virtual mode has inconsistent revocation behavior) |
| MicroPython payload | Full chain: lockdown → signed MP payload on SD → boot | `>>>` REPL with working UART input | Yes (full chain requires physical eFuses) |

---

## Notes / Caveats

- **`SECURE_VERSION` is 16-shot** — plan version numbers carefully. Kern uses 16-bit unary; once all 16 bits are 1, no further updates possible. See "Versioning Strategy" in Section 2.
- **Flash encryption + PSRAM DMA** requires IDF ≥ 6.1 and `CONFIG_SPIRAM_ENC_EXEMPT`. **[BLOCKED on IDF v5.5]** The Waveshare P4 board uses MIPI-DSI for display — the DSI DMA must allocate buffers from the exempt region.
- **`HMAC_UP` (UPSTREAM) for KEY4 and KEY5**: Both keys use the `HMAC_KEY_UPSTREAM` purpose. This means software *can* call `esp_hmac_calculate()` — but the raw 256-bit key is read-protected and never leaves the eFuse block. The security of the 4-word anti-phishing proof relies on **hardware rate-limiting** (~1 ms/call) combined with the **44-bit search space** (2^44 × 1 ms ≈ 557 years). ~~(Previous draft incorrectly specified `HMAC_KEY_DOWNSTREAM` for KEY5 — `DOWNSTREAM` routes HMAC output to the Digital Signature peripheral or JTAG re-enablement, not to software. The bootloader could not compute anti-phishing words with a `DOWNSTREAM` key.)~~
- **Virtual eFuses do NOT work with the HMAC hardware peripheral** — `CONFIG_EFUSE_VIRTUAL=y` only emulates eFuse reads/writes in RAM. The HMAC peripheral reads key material from the **physical eFuse registers**, so `esp_hmac_calculate()` fails with `ESP_ERR_HW_CRYPTO_DS_HMAC_FAIL` when keys are virtual. **Dev-mode mitigation:** compile-time software HMAC fallback via mbedTLS (`mbedtls_md_hmac()`) with a hardcoded test key, gated by `#ifdef CONFIG_EFUSE_VIRTUAL`. Similarly, NVS encryption falls back to plaintext NVS in dev mode. Secure Boot V2 signature verification **does** work with virtual eFuses (the bootloader handles this in software, not via the HMAC peripheral).
- **Virtual eFuses are dev-only** — `run.sh` has a pre-flight check that **aborts** if `CONFIG_EFUSE_VIRTUAL=y` is detected in a production build (i.e., if `sdkconfig.secure` was not applied).
- **No `sdkconfig.secure` in git** — per Kern policy and ESP-IDF best practice. The overlay is applied only on the signing machine.
- **Provisioning is one-way** — `tools/provision_efuses.py` burns physical eFuses. **[ALL IRREVERSIBLE]** **Only run on production devices at the factory.** Development boards stay on virtual eFuses.
- **eFuse key block budget is tight** — all 6 key blocks (KEY0–KEY5) are allocated. Zero spare blocks remain for future features (e.g., DS module for hardware-backed TLS, JTAG soft-re-enable for field debug). Consider starting with 2 Secure Boot rotation slots (KEY0 + KEY1) instead of 3, keeping KEY2 as a spare.

---

## Future Steps (Post-Hardening)

- **FROST/multi-sig key ceremony** — threshold signing for `vendor_keys[]` and RSA keys
- **Dev key mode** — a separate `sdkconfig.dev` with `CONFIG_SECURE_BOOT_V2_ENABLED=n`, `CONFIG_EFUSE_VIRTUAL=y`, test keys for CI
- **Settings storage design** — what user settings live in encrypted NVS vs. SD card (Kern uses NVS for PIN, display, theme; SeedSigner may need more)
- **Signed language packs** — per Keith's requirement: verify signed language pack bundles before loading (reuses Specter section format)
- **Generalized `bl_platform_t`** — factor out P4-specific code for STM32 (Specter) / S3 (future SeedSigner) portability
- **CI/CD pipeline** — automated build + sign + hardware-in-the-loop test on real boards
