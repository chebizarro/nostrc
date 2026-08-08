#ifndef NOSTR_NIP_CONCORD_INTERNAL_H
#define NOSTR_NIP_CONCORD_INTERNAL_H

#include "nip_concord.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Shared internal helpers (not API). */

/* HKDF-SHA256 with zero-length salt and arbitrary-length info (CORD-02 A.1). */
int concord_hkdf_sha256(const uint8_t *ikm, size_t ikm_len,
                        const uint8_t *info, size_t info_len,
                        uint8_t out[32]);

int concord_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

/* secp256k1: seed validity + x-only pubkey derivation. */
bool concord_seckey_verify(const uint8_t sk[32]);
int concord_xonly_pubkey(const uint8_t sk[32], uint8_t out_pk[32]);

/* Growable string builder for deterministic JSON output. */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool oom;
} concord_sb_t;

void concord_sb_init(concord_sb_t *sb);
void concord_sb_free(concord_sb_t *sb);
void concord_sb_append(concord_sb_t *sb, const char *s);
void concord_sb_append_len(concord_sb_t *sb, const char *s, size_t len);
void concord_sb_append_json_string(concord_sb_t *sb, const char *s);
void concord_sb_append_u64(concord_sb_t *sb, uint64_t v);
void concord_sb_append_i64(concord_sb_t *sb, int64_t v);
/* Returns the built string (caller frees) or NULL on OOM; resets sb. */
char *concord_sb_take(concord_sb_t *sb);

char *concord_strdup(const char *s);

/* Strict decimal u64 parse: no sign, no leading zeros (except "0"). */
bool concord_parse_u64(const char *s, uint64_t *out);

/* Strict ms tag parse: 0..999, no leading zeros. */
bool concord_parse_ms(const char *s, int *out);

/* Copies a 64-char lowercase hex string into a 65-byte field; fails closed. */
bool concord_copy_hex32(char dst[65], const char *src);

/* base64url without padding. */
char *concord_b64url_encode(const uint8_t *data, size_t len);
/* Returns malloc'd buffer, sets *out_len; NULL on any non-canonical input. */
uint8_t *concord_b64url_decode(const char *s, size_t *out_len);

#endif /* NOSTR_NIP_CONCORD_INTERNAL_H */
