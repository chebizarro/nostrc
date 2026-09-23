/*
 * nh_porthome_cbor.c — Canonical CBOR encoder/decoder for porthome.
 * SPDX-License-Identifier: MIT
 */

#include "nh_porthome_cbor.h"

#include <stdlib.h>
#include <string.h>
/* endian.h intentionally not included — the encoder writes big-endian bytes by hand for portability. */

/* CBOR major types (RFC 8949). */
#define MT_UINT  0u
#define MT_BSTR  2u
#define MT_TSTR  3u
#define MT_ARRAY 4u
#define MT_MAP   5u

/* ────────────── encoder ────────────── */

int nhpc_enc_init(nhpc_enc *e, size_t initial_cap) {
    if (!e) return -1;
    e->buf = NULL; e->len = 0; e->cap = 0; e->err = 0;
    if (initial_cap == 0) initial_cap = 128;
    e->buf = (uint8_t *)malloc(initial_cap);
    if (!e->buf) return -1;
    e->cap = initial_cap;
    return 0;
}

void nhpc_enc_dispose(nhpc_enc *e) {
    if (!e) return;
    free(e->buf); e->buf = NULL; e->len = 0; e->cap = 0;
}

int nhpc_enc_take(nhpc_enc *e, uint8_t **out, size_t *out_len) {
    if (!e || !out || !out_len) return -1;
    if (e->err) { nhpc_enc_dispose(e); *out = NULL; *out_len = 0; return e->err; }
    *out = e->buf;
    *out_len = e->len;
    e->buf = NULL; e->len = 0; e->cap = 0;
    return 0;
}

static int enc_reserve(nhpc_enc *e, size_t need) {
    if (e->err) return e->err;
    if (e->len + need <= e->cap) return 0;
    size_t new_cap = e->cap ? e->cap : 128;
    while (new_cap < e->len + need) {
        if (new_cap > (SIZE_MAX / 2)) { e->err = -1; return -1; }
        new_cap *= 2;
    }
    uint8_t *nb = (uint8_t *)realloc(e->buf, new_cap);
    if (!nb) { e->err = -1; return -1; }
    e->buf = nb; e->cap = new_cap;
    return 0;
}

/* Canonical CBOR head: major type + argument in the shortest form
 * (0-23 inline, else 1/2/4/8-byte big-endian). */
static int enc_head(nhpc_enc *e, uint8_t mt, uint64_t arg) {
    uint8_t ib = (uint8_t)(mt << 5);
    if (arg < 24u) {
        if (enc_reserve(e, 1)) return -1;
        e->buf[e->len++] = (uint8_t)(ib | (uint8_t)arg);
    } else if (arg <= 0xFFu) {
        if (enc_reserve(e, 2)) return -1;
        e->buf[e->len++] = (uint8_t)(ib | 24u);
        e->buf[e->len++] = (uint8_t)arg;
    } else if (arg <= 0xFFFFu) {
        if (enc_reserve(e, 3)) return -1;
        e->buf[e->len++] = (uint8_t)(ib | 25u);
        e->buf[e->len++] = (uint8_t)((arg >> 8) & 0xFF);
        e->buf[e->len++] = (uint8_t)(arg & 0xFF);
    } else if (arg <= 0xFFFFFFFFu) {
        if (enc_reserve(e, 5)) return -1;
        e->buf[e->len++] = (uint8_t)(ib | 26u);
        e->buf[e->len++] = (uint8_t)((arg >> 24) & 0xFF);
        e->buf[e->len++] = (uint8_t)((arg >> 16) & 0xFF);
        e->buf[e->len++] = (uint8_t)((arg >>  8) & 0xFF);
        e->buf[e->len++] = (uint8_t)(arg & 0xFF);
    } else {
        if (enc_reserve(e, 9)) return -1;
        e->buf[e->len++] = (uint8_t)(ib | 27u);
        for (int i = 7; i >= 0; --i)
            e->buf[e->len++] = (uint8_t)((arg >> (i * 8)) & 0xFF);
    }
    return 0;
}

int nhpc_write_uint(nhpc_enc *e, uint64_t v) { return enc_head(e, MT_UINT, v); }
int nhpc_write_array_hdr(nhpc_enc *e, uint64_t n) { return enc_head(e, MT_ARRAY, n); }
int nhpc_write_map_hdr(nhpc_enc *e, uint64_t n) { return enc_head(e, MT_MAP, n); }

int nhpc_write_bstr(nhpc_enc *e, const uint8_t *b, size_t n) {
    if (enc_head(e, MT_BSTR, (uint64_t)n)) return -1;
    if (n) {
        if (enc_reserve(e, n)) return -1;
        memcpy(e->buf + e->len, b, n);
        e->len += n;
    }
    return 0;
}

int nhpc_write_tstr_n(nhpc_enc *e, const char *s, size_t n) {
    if (enc_head(e, MT_TSTR, (uint64_t)n)) return -1;
    if (n) {
        if (enc_reserve(e, n)) return -1;
        memcpy(e->buf + e->len, s, n);
        e->len += n;
    }
    return 0;
}

int nhpc_write_tstr(nhpc_enc *e, const char *s) {
    return nhpc_write_tstr_n(e, s, s ? strlen(s) : 0);
}

/* ────────────── decoder ────────────── */

void nhpc_dec_init(nhpc_dec *d, const uint8_t *buf, size_t len) {
    d->buf = buf; d->len = len; d->pos = 0; d->err = 0;
}

int nhpc_dec_at_end(const nhpc_dec *d) {
    return d->err == 0 && d->pos == d->len;
}

int nhpc_peek_major(const nhpc_dec *d) {
    if (d->err) return -1;
    if (d->pos >= d->len) return -1;
    return (int)((d->buf[d->pos] >> 5) & 0x7);
}

/* Read the header at the cursor and return the (major, arg) pair.
 * Advances the cursor past the header. Enforces canonical shortest
 * form: an argument that fits in the previous size class MUST use it. */
static int dec_head(nhpc_dec *d, uint8_t *out_mt, uint64_t *out_arg) {
    if (d->err) return -1;
    if (d->pos >= d->len) { d->err = -1; return -1; }
    uint8_t ib = d->buf[d->pos++];
    uint8_t mt = (uint8_t)(ib >> 5);
    uint8_t ai = (uint8_t)(ib & 0x1Fu);
    uint64_t arg = 0;
    if (ai < 24u) {
        arg = ai;
    } else if (ai == 24u) {
        if (d->pos + 1 > d->len) { d->err = -1; return -1; }
        arg = d->buf[d->pos]; d->pos += 1;
        if (arg < 24u) { d->err = -1; return -1; } /* not shortest form */
    } else if (ai == 25u) {
        if (d->pos + 2 > d->len) { d->err = -1; return -1; }
        arg = ((uint64_t)d->buf[d->pos] << 8) | (uint64_t)d->buf[d->pos + 1];
        d->pos += 2;
        if (arg <= 0xFFu) { d->err = -1; return -1; }
    } else if (ai == 26u) {
        if (d->pos + 4 > d->len) { d->err = -1; return -1; }
        arg = ((uint64_t)d->buf[d->pos]   << 24)
            | ((uint64_t)d->buf[d->pos+1] << 16)
            | ((uint64_t)d->buf[d->pos+2] <<  8)
            | ((uint64_t)d->buf[d->pos+3]);
        d->pos += 4;
        if (arg <= 0xFFFFu) { d->err = -1; return -1; }
    } else if (ai == 27u) {
        if (d->pos + 8 > d->len) { d->err = -1; return -1; }
        arg = 0;
        for (int i = 0; i < 8; i++) arg = (arg << 8) | (uint64_t)d->buf[d->pos + i];
        d->pos += 8;
        if (arg <= 0xFFFFFFFFu) { d->err = -1; return -1; }
    } else {
        /* 28..30 reserved; 31 = indefinite-length — refused in canonical form. */
        d->err = -1;
        return -1;
    }
    *out_mt = mt;
    *out_arg = arg;
    return 0;
}

int nhpc_read_uint(nhpc_dec *d, uint64_t *out) {
    uint8_t mt; uint64_t arg;
    if (dec_head(d, &mt, &arg)) return -1;
    if (mt != MT_UINT) { d->err = -1; return -1; }
    *out = arg;
    return 0;
}

int nhpc_read_bstr(nhpc_dec *d, const uint8_t **out, size_t *out_len) {
    uint8_t mt; uint64_t arg;
    if (dec_head(d, &mt, &arg)) return -1;
    if (mt != MT_BSTR) { d->err = -1; return -1; }
    if (arg > (uint64_t)(d->len - d->pos)) { d->err = -1; return -1; }
    *out = d->buf + d->pos;
    *out_len = (size_t)arg;
    d->pos += (size_t)arg;
    return 0;
}

int nhpc_read_tstr(nhpc_dec *d, const uint8_t **out, size_t *out_len) {
    uint8_t mt; uint64_t arg;
    if (dec_head(d, &mt, &arg)) return -1;
    if (mt != MT_TSTR) { d->err = -1; return -1; }
    if (arg > (uint64_t)(d->len - d->pos)) { d->err = -1; return -1; }
    *out = d->buf + d->pos;
    *out_len = (size_t)arg;
    d->pos += (size_t)arg;
    return 0;
}

int nhpc_read_array_hdr(nhpc_dec *d, uint64_t *n_items) {
    uint8_t mt; uint64_t arg;
    if (dec_head(d, &mt, &arg)) return -1;
    if (mt != MT_ARRAY) { d->err = -1; return -1; }
    *n_items = arg;
    return 0;
}

int nhpc_read_map_hdr(nhpc_dec *d, uint64_t *n_pairs) {
    uint8_t mt; uint64_t arg;
    if (dec_head(d, &mt, &arg)) return -1;
    if (mt != MT_MAP) { d->err = -1; return -1; }
    *n_pairs = arg;
    return 0;
}

int nhpc_skip_item(nhpc_dec *d) {
    /* Recursive skip. Only supports the types we emit — a schema type we
     * don't recognise is a parse error. */
    if (d->err) return -1;
    uint8_t mt; uint64_t arg;
    size_t save = d->pos;
    if (dec_head(d, &mt, &arg)) { d->pos = save; return -1; }
    switch (mt) {
        case MT_UINT:
            return 0;
        case MT_BSTR:
        case MT_TSTR:
            if (arg > (uint64_t)(d->len - d->pos)) { d->err = -1; return -1; }
            d->pos += (size_t)arg;
            return 0;
        case MT_ARRAY:
            for (uint64_t i = 0; i < arg; i++) if (nhpc_skip_item(d)) return -1;
            return 0;
        case MT_MAP:
            for (uint64_t i = 0; i < arg; i++) {
                if (nhpc_skip_item(d)) return -1; /* key */
                if (nhpc_skip_item(d)) return -1; /* val */
            }
            return 0;
        default:
            d->err = -1;
            return -1;
    }
}
