/*
 * porthome_cbor.h — Canonical-CBOR encoder/decoder for
 * porthome manifests. Not a general-purpose CBOR library — only the
 * subset the manifest schema needs (uints ≤ UINT64_MAX, byte strings,
 * text strings, definite-length arrays and maps, integer-keyed maps
 * with strictly-ascending keys). RFC 8949 §4.2.1 canonical form.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef PORTHOME_CBOR_H
#define PORTHOME_CBOR_H

#include <porthome/export.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Encoder — grows an owned buffer. */
typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
    int      err;  /* 0 == OK; non-zero -> subsequent ops are no-ops */
} porthome_cbor_enc;

PORTHOME_API int  porthome_cbor_enc_init(porthome_cbor_enc *e, size_t initial_cap);
PORTHOME_API void porthome_cbor_enc_dispose(porthome_cbor_enc *e);
PORTHOME_API int  porthome_cbor_enc_take(porthome_cbor_enc *e, uint8_t **out, size_t *out_len);

PORTHOME_API int porthome_cbor_write_uint(porthome_cbor_enc *e, uint64_t v);
PORTHOME_API int porthome_cbor_write_bstr(porthome_cbor_enc *e, const uint8_t *b, size_t n);
PORTHOME_API int porthome_cbor_write_tstr(porthome_cbor_enc *e, const char *s);
PORTHOME_API int porthome_cbor_write_tstr_n(porthome_cbor_enc *e, const char *s, size_t n);
PORTHOME_API int porthome_cbor_write_array_hdr(porthome_cbor_enc *e, uint64_t n_items);
PORTHOME_API int porthome_cbor_write_map_hdr(porthome_cbor_enc *e,   uint64_t n_pairs);

/* Decoder — cursor over caller-owned bytes. */
typedef struct {
    const uint8_t *buf;
    size_t         len;
    size_t         pos;
    int            err;
} porthome_cbor_dec;

PORTHOME_API void porthome_cbor_dec_init(porthome_cbor_dec *d, const uint8_t *buf, size_t len);

PORTHOME_API int porthome_cbor_read_uint(porthome_cbor_dec *d, uint64_t *out);
PORTHOME_API int porthome_cbor_read_bstr(porthome_cbor_dec *d, const uint8_t **out, size_t *out_len);
PORTHOME_API int porthome_cbor_read_tstr(porthome_cbor_dec *d, const uint8_t **out, size_t *out_len);
PORTHOME_API int porthome_cbor_read_array_hdr(porthome_cbor_dec *d, uint64_t *n_items);
PORTHOME_API int porthome_cbor_read_map_hdr(porthome_cbor_dec *d,   uint64_t *n_pairs);

/* Peek the major type of the next item without consuming. Returns -1 on EOF. */
PORTHOME_API int porthome_cbor_peek_major(const porthome_cbor_dec *d);

/* Advance past whatever item is at the cursor (used only by unknown-key
 * skips, but the strict decoder refuses unknown keys — so unused in the
 * manifest decoder). Included for completeness. */
PORTHOME_API int porthome_cbor_skip_item(porthome_cbor_dec *d);

/* Assert the decoder is exactly at end-of-buffer. */
PORTHOME_API int porthome_cbor_dec_at_end(const porthome_cbor_dec *d);

#ifdef __cplusplus
}
#endif
#endif /* PORTHOME_CBOR_H */
