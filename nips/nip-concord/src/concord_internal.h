#ifndef NOSTR_NIP_CONCORD_INTERNAL_H
#define NOSTR_NIP_CONCORD_INTERNAL_H

#include "nip_concord.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Shared internal helpers (not API). */

int concord_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

/* HKDF-SHA256, zero-length salt, arbitrary-length info (CORD-02 A.1). */
int concord_hkdf_sha256(const uint8_t *ikm, size_t ikm_len,
                        const uint8_t *info, size_t info_len,
                        uint8_t out[32]);

/* secp256k1: seed validity + x-only pubkey derivation. */
bool concord_seckey_verify(const uint8_t sk[32]);
int concord_xonly_pubkey(const uint8_t sk[32], uint8_t out_pk[32]);

/* Builds the A.1 info buffer. Returns the length written, or 0 on overflow.
 * `counter` is the A.3 scalar_normalize retry byte: pass counter < 0 for the
 * first attempt, which appends no counter byte at all. */
size_t concord_build_info(const char *label, const uint8_t id[32],
                          bool has_epoch, uint64_t epoch, int counter,
                          uint8_t *out, size_t out_cap);

/* Constant-time 32-byte compare. */
bool concord_memeq_32(const uint8_t a[32], const uint8_t b[32]);

char *concord_strndup(const char *s, size_t n);

#endif /* NOSTR_NIP_CONCORD_INTERNAL_H */
