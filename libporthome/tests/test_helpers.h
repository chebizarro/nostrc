/* SPDX-License-Identifier: MIT */
#ifndef PORTHOME_TEST_HELPERS_H
#define PORTHOME_TEST_HELPERS_H
#include <porthome/crypto.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static inline void expect_hex(const uint8_t *bytes, size_t len, const char *hex) {
    static const char digits[] = "0123456789abcdef";
    assert(strlen(hex) == 2 * len);
    for (size_t i = 0; i < len; ++i) {
        assert(hex[2*i] == digits[bytes[i] >> 4]);
        assert(hex[2*i+1] == digits[bytes[i] & 15]);
    }
}
static inline void fixture(uint8_t root[32], uint8_t child[32]) {
    uint8_t seed[32], id[32];
    for (size_t i = 0; i < 32; ++i) { seed[i] = (uint8_t)i; id[i] = (uint8_t)(i + 32); }
    assert(porthome_root_node_key(seed, root) == 0);
    assert(porthome_child_node_key(root, id, NULL, child) == 0);
}
#endif
