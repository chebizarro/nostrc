/* SPDX-License-Identifier: MIT */
#include <porthome/crypto.h>

static uint64_t rol1(uint64_t x) { return (x << 1) | (x >> 63); }

/* Fixed SplitMix64 table expansion: part of this chunker's deterministic
 * contract, not a random source or a cryptographic primitive. */
static void byte_table(uint64_t table[256]) {
    uint64_t state = UINT64_C(0x706f7274686f6d65);
    for (size_t i = 0; i < 256; ++i) {
        state += UINT64_C(0x9e3779b97f4a7c15);
        uint64_t z = state;
        z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
        table[i] = z ^ (z >> 31);
    }
}

size_t porthome_chunk_next(const uint8_t *data, size_t len) {
    if (!data || !len) return 0;
    if (len <= PORTHOME_CHUNK_MIN) return len;
    size_t limit = len < PORTHOME_CHUNK_MAX ? len : PORTHOME_CHUNK_MAX;
    uint64_t table[256];
    byte_table(table);
    uint64_t hash = 0;
    for (size_t i = 0; i < limit; ++i) {
        hash = rol1(hash) ^ table[data[i]];
        /* rol(table[outgoing], 64) == table[outgoing]. */
        if (i >= 64) hash ^= table[data[i - 64]];
        if (i + 1 >= PORTHOME_CHUNK_MIN &&
            (hash & (PORTHOME_CHUNK_TARGET - 1)) == 0) return i + 1;
    }
    return limit;
}
