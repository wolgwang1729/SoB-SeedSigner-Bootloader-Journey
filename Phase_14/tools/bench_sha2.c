/*
 * bench_sha2.c — Phase 14, T14: measure the exact SHA-256 implementation the
 * loader uses (specter_crypto sha2.c) so we can extrapolate the anti-phishing
 * brute-force collision time at 22 bits (Phase 7) and 44 bits (Phase 13).
 *
 * Build:  gcc -O2 bench_sha2.c ../Phase_13/.../crypto/sha2.c ../Phase_13/.../crypto/memzero.c -o bench_sha2
 * Run:    ./bench_sha2
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sha2.h"

#define REGION_BYTES (6 * 1024 * 1024 + 768 * 1024)  /* ~6.8 MB random_fill */

int main(void) {
    uint8_t *region = malloc(REGION_BYTES);
    if (!region) { fprintf(stderr, "alloc failed\n"); return 1; }
    memset(region, 0xA5, REGION_BYTES);

    /* warm-up + correctness sanity vs a known vector */
    SHA256_CTX c; uint8_t out[32];
    sha256_Init(&c); sha256_Update(&c, (const uint8_t*)"abc", 3); sha256_Final(&c, out);
    printf("SHA-256(\"abc\") = ");
    for (int i = 0; i < 32; i++) printf("%02x", out[i]);
    printf("  (expect ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad)\n");

    /* benchmark full-region rehash rate */
    int rounds = 20;
    clock_t t0 = clock();
    for (int r = 0; r < rounds; r++) {
        sha256_Init(&c);
        sha256_Update(&c, region, REGION_BYTES);
        sha256_Final(&c, out);
    }
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    double bytes_per_sec = (double)REGION_BYTES * rounds / secs;
    printf("\nregion            : %u bytes (~6.8 MB)\n", REGION_BYTES);
    printf("rounds            : %d\n", rounds);
    printf("elapsed           : %.3f s\n", secs);
    printf("throughput        : %.1f MB/s (host software sha2.c)\n", bytes_per_sec / 1e6);
    printf("ms per full hash  : %.3f\n", 1000.0 * secs / rounds);

    double secs22 = (double)(1ULL << 22) * secs / rounds;
    double secs44 = (double)(1ULL << 44) * secs / rounds;
    printf("\n-- extrapolated naive collision search (full-region rehash each try) --\n");
    printf("2^22 tries (2 words) : %.1f s  (%.2f min)\n", secs22, secs22 / 60.0);
    printf("2^44 tries (4 words) : %.1f s  (%.2f h)\n", secs44, secs44 / 3600.0);

    /* a real attacker only needs to match 44 bits of the DERIVED words, but
     * each try still costs one full-region hash; the above is the naive bound.
     * Phase-7's ~84 s at 2 words is only reachable with the incremental trick
     * below (vary only the trailing block, so each try costs ~1 SHA block). */
    int small = 512 * 1024;
    t0 = clock();
    for (int r = 0; r < rounds; r++) {
        sha256_Init(&c); sha256_Update(&c, region, small); sha256_Final(&c, out);
    }
    secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    secs22 = (double)(1ULL << 22) * secs / rounds;
    printf("\n512 KB-region model (Phase 7, 22 bits): ~%.1f s per 2^22 tries\n", secs22);

    /* incremental attack: attacker precomputes the SHA-256 state over the
     * region prefix, then brute-forces ONLY the final 64-byte block. Each try
     * is one block compression (~tens of ns). */
    t0 = clock();
    int iters = 2000000;
    for (int r = 0; r < iters; r++) {
        sha256_Init(&c);
        sha256_Update(&c, region, 64);   /* stand-in for one candidate block */
        sha256_Final(&c, out);
    }
    secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    double per_try = secs / iters;

    /* tight Update-only loop: reuse ctx, no Init/Final overhead per try */
    sha256_Init(&c);
    t0 = clock();
    for (int r = 0; r < iters; r++) {
        sha256_Update(&c, region, 64);   /* recompress one candidate block */
    }
    secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    double per_block = secs / iters;

    printf("\n-- incremental trailing-block attack --\n");
    printf("per-try incl Init/Final : %.1f ns\n", per_try * 1e9);
    printf("per-try (compression only): %.1f ns\n", per_block * 1e9);
    printf("2^22 tries               : %.1f s (%.2f min) / %.1f s\n",
           (double)(1ULL << 22) * per_try, (double)(1ULL << 22) * per_try / 60,
           (double)(1ULL << 22) * per_block);
    printf("2^44 tries               : %.1f h  / %.1f h\n",
           (double)(1ULL << 44) * per_try / 3600,
           (double)(1ULL << 44) * per_block / 3600);

    /* physical bound: the region is TRNG-filled, so ANY write requires an
     * erase cycle. A brute-force search that writes each candidate to flash
     * (to actually change the region the words cover) is bounded by erase. */
    double flash_erase_ms = 40.0;  /* typical SPI NOR sector-erase */
    printf("\n-- flash-write physical bound (each candidate = 1 sector erase) --\n");
    printf("per candidate      : ~%.0f ms (4 KB sector erase)\n", flash_erase_ms);
    printf("2^22 candidates    : %.1f h\n", (double)(1ULL << 22) * flash_erase_ms / 1000 / 3600);
    printf("2^44 candidates    : %.0f years\n", (double)(1ULL << 44) * flash_erase_ms / 1000 / 3600 / 24 / 365);

    free(region);
    return 0;
}
