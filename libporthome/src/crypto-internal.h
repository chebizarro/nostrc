/* SPDX-License-Identifier: MIT — internal, not installed or exported. */
#ifndef PORTHOME_CRYPTO_INTERNAL_H
#define PORTHOME_CRYPTO_INTERNAL_H
#include <porthome/crypto.h>
int porthome_ring_key(const porthome_ring_t *ring, uint64_t epoch, const uint8_t **key);
#endif
