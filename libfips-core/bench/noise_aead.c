/* AEAD-only baseline for nostrc-f66sr.4: upstream benches/noise_aead.rs
 * uses 64/1400-byte payloads, 16-byte AD and a 1024-packet open ring.
 * Session/replay/worker key hand-out costs belong to later Noise tasks.
 * Host benchmark reads a monotonic clock; the core does not. Not a ctest. */
#include <fips/noise/aead.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RING 1024u
#define ITERATIONS 200000u

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) abort();
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static void require(fips_aead_status rc)
{
    if (rc != FIPS_AEAD_OK) abort();
}

static void run(size_t size)
{
    uint8_t kb[32] = {0}, ad[16], payload[1400], wire[1416], plain[1400];
    kb[0] = (uint8_t)size;
    memset(ad, 0x5a, sizeof(ad));
    memset(payload, 0xa5, sizeof(payload));
    fips_aead_key key;
    require(fips_aead_key_init(&key, kb));
    size_t clen = size + FIPS_NOISE_TAG_SIZE, got;
    uint8_t *ring = malloc(RING * clen);
    if (!ring) abort();
    for (uint64_t i = 0; i < RING; ++i)
        require(fips_aead_seal(&key, i, ad, sizeof(ad), payload, size,
                               ring + i * clen, clen, &got));
    uint64_t start = now_ns();
    for (uint64_t i = 0; i < ITERATIONS; ++i)
        require(fips_aead_seal(&key, RING + i, ad, sizeof(ad), payload, size,
                               wire, sizeof(wire), &got));
    double seal = (double)(now_ns() - start) / ITERATIONS;
    start = now_ns();
    for (size_t i = 0; i < ITERATIONS; ++i) {
        size_t k = i % RING;
        require(fips_aead_open(&key, k, ad, sizeof(ad), ring + k * clen, clen,
                               plain, sizeof(plain), &got));
    }
    double open = (double)(now_ns() - start) / ITERATIONS;
    if (got != size || memcmp(plain, payload, size) != 0) abort();
    printf("noise_aead/seal/%zu %.1f ns/op %.1f MiB/s\n", size, seal,
           (double)size * 1e9 / seal / 1048576.0);
    printf("noise_aead/open/%zu %.1f ns/op %.1f MiB/s\n", size, open,
           (double)size * 1e9 / open / 1048576.0);
    fips_aead_key_destroy(&key);
    free(ring);
}

int main(void)
{
    run(64);
    run(1400);
    return 0;
}
