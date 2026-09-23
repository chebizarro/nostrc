/*
 * nh_porthome_cbor.h — Private canonical-CBOR encoder/decoder for
 * porthome manifests. Not a general-purpose CBOR library — only the
 * subset the manifest schema needs (uints ≤ UINT64_MAX, byte strings,
 * text strings, definite-length arrays and maps, integer-keyed maps
 * with strictly-ascending keys). RFC 8949 §4.2.1 canonical form.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NH_PORTHOME_CBOR_H
#define NH_PORTHOME_CBOR_H

#include <stddef.h>
#include <stdint.h>

/* Encoder — grows an owned buffer. */
typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
    int      err;  /* 0 == OK; non-zero -> subsequent ops are no-ops */
} nhpc_enc;

int  nhpc_enc_init(nhpc_enc *e, size_t initial_cap);
void nhpc_enc_dispose(nhpc_enc *e);
int  nhpc_enc_take(nhpc_enc *e, uint8_t **out, size_t *out_len);

int nhpc_write_uint(nhpc_enc *e, uint64_t v);
int nhpc_write_bstr(nhpc_enc *e, const uint8_t *b, size_t n);
int nhpc_write_tstr(nhpc_enc *e, const char *s);
int nhpc_write_tstr_n(nhpc_enc *e, const char *s, size_t n);
int nhpc_write_array_hdr(nhpc_enc *e, uint64_t n_items);
int nhpc_write_map_hdr(nhpc_enc *e,   uint64_t n_pairs);

/* Decoder — cursor over caller-owned bytes. */
typedef struct {
    const uint8_t *buf;
    size_t         len;
    size_t         pos;
    int            err;
} nhpc_dec;

void nhpc_dec_init(nhpc_dec *d, const uint8_t *buf, size_t len);

int nhpc_read_uint(nhpc_dec *d, uint64_t *out);
int nhpc_read_bstr(nhpc_dec *d, const uint8_t **out, size_t *out_len);
int nhpc_read_tstr(nhpc_dec *d, const uint8_t **out, size_t *out_len);
int nhpc_read_array_hdr(nhpc_dec *d, uint64_t *n_items);
int nhpc_read_map_hdr(nhpc_dec *d,   uint64_t *n_pairs);

/* Peek the major type of the next item without consuming. Returns -1 on EOF. */
int nhpc_peek_major(const nhpc_dec *d);

/* Advance past whatever item is at the cursor (used only by unknown-key
 * skips, but the strict decoder refuses unknown keys — so unused in the
 * manifest decoder). Included for completeness. */
int nhpc_skip_item(nhpc_dec *d);

/* Assert the decoder is exactly at end-of-buffer. */
int nhpc_dec_at_end(const nhpc_dec *d);

#endif
