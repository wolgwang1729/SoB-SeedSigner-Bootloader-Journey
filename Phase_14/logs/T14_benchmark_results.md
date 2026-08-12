# Phase 14 — T14 brute-force benchmark (host, specter_crypto sha2.c)

Source: `Phase_14/tools/bench_sha2.c`, compiled with `gcc -O2` against
`Phase_13/.../specter_crypto/crypto/sha2.c` + `memzero.c` (the EXACT SHA-256 the
loader uses for the anti-phishing digest). The ESP32-P4 HP CPU is a 400 MHz
in-order RISC-V core — the host x86-64 is ~3-4 GHz OoO, so a rough device
penalty of 3-8x applies.

Measured (region = 7,077,888 bytes ≈ 6.8 MB, the random_fill partition):

| Search strategy | per try | 2^22 (2 words) | 2^44 (4 words) |
|---|---|---|---|
| Naive full-region rehash | 149 ms | 7.2 days | 83M years |
| Incremental trailing-block (compression only) | 1.29 us | 5.4 s | 262 days |
| Incremental + Init/Final overhead | 2.64 us | 11 s | 537 days |
| Flash-write bound (1 sector erase per candidate) | 40 ms | 46.6 h | 22,314 years |

On-device (x5 host): incremental 2^44 ≈ 3.6-5.9 years.

## Corrections to existing docs

- Phase 7 claimed ~84 s for 2 words. Not reproducible for a 512 KB region by
  full rehash (measured ~45,138 s). Only the incremental trailing-block attack
  reaches seconds — which Phase 7 did not describe.
- Phase 13 claimed "~7 h at PSRAM software rate" for 4 words. Not reproducible:
  full-region is 83M years; incremental is ~262 days host / ~3.6 yr device.
- Conclusion: the dev-mode SHA-256-only 44-bit proof is FAR stronger than the
  docs claimed. Still, HMAC eFuse binding (Production_Hardening_Plan) remains
  the correct endgame because the incremental attack is compute-bound, not
  flash-bound, and a determined attacker with PSRAM execution could in
  principle grind it out in months-to-years on a single device.
