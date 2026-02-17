/*
 * libmarmot - TLS Presentation Language serialization
 *
 * Implements the wire format used by MLS (RFC 9420) for encoding
 * structs, vectors, and opaque data.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mls-internal.h"
#include <stdlib.h>
#include <string.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Write buffer
 * ──────────────────────────────────────────────────────────────────────── */

int
mls_tls_buf_init(MlsTlsBuf *buf, size_t initial_cap)
{
    if (!buf) return -1;
    buf->data = malloc(initial_cap > 0 ? initial_cap : 256);
    if (!buf->data) return -1;
    buf->len = 0;
    buf->cap = initial_cap > 0 ? initial_cap : 256;
    return 0;
}

void
mls_tls_buf_free(MlsTlsBuf *buf)
{
    if (!buf) return;
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

static int
buf_ensure(MlsTlsBuf *buf, size_t additional)
{
    size_t needed = buf->len + additional;
    if (needed <= buf->cap) return 0;

    size_t new_cap = buf->cap * 2;
    /* Check for overflow in doubling loop */
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) return -1;
        new_cap *= 2;
    }

    uint8_t *new_data = realloc(buf->data, new_cap);
    if (!new_data) return -1;
    buf->data = new_data;
    buf->cap = new_cap;
    return 0;
}

int
mls_tls_buf_append(MlsTlsBuf *buf, const uint8_t *data, size_t len)
{
    if (buf_ensure(buf, len) != 0) return -1;
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    return 0;
}

int
mls_tls_write_u8(MlsTlsBuf *buf, uint8_t val)
{
    return mls_tls_buf_append(buf, &val, 1);
}

int
mls_tls_write_u16(MlsTlsBuf *buf, uint16_t val)
{
    uint8_t bytes[2] = {
        (uint8_t)(val >> 8),
        (uint8_t)(val & 0xff)
    };
    return mls_tls_buf_append(buf, bytes, 2);
}

int
mls_tls_write_u32(MlsTlsBuf *buf, uint32_t val)
{
    uint8_t bytes[4] = {
        (uint8_t)(val >> 24),
        (uint8_t)((val >> 16) & 0xff),
        (uint8_t)((val >> 8) & 0xff),
        (uint8_t)(val & 0xff)
    };
    return mls_tls_buf_append(buf, bytes, 4);
}

int
mls_tls_write_u64(MlsTlsBuf *buf, uint64_t val)
{
    uint8_t bytes[8];
    for (int i = 7; i >= 0; i--) {
        bytes[i] = (uint8_t)(val & 0xff);
        val >>= 8;
    }
    return mls_tls_buf_append(buf, bytes, 8);
}

int
mls_tls_write_opaque8(MlsTlsBuf *buf, const uint8_t *data, size_t len)
{
    if (len > 255) return -1;
    if (mls_tls_write_u8(buf, (uint8_t)len) != 0) return -1;
    if (len > 0)
        return mls_tls_buf_append(buf, data, len);
    return 0;
}

int
mls_tls_write_opaque16(MlsTlsBuf *buf, const uint8_t *data, size_t len)
{
    if (len > 65535) return -1;
    if (mls_tls_write_u16(buf, (uint16_t)len) != 0) return -1;
    if (len > 0)
        return mls_tls_buf_append(buf, data, len);
    return 0;
}

int
mls_tls_write_opaque32(MlsTlsBuf *buf, const uint8_t *data, size_t len)
{
    if (mls_tls_write_u32(buf, (uint32_t)len) != 0) return -1;
    if (len > 0)
        return mls_tls_buf_append(buf, data, len);
    return 0;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Read cursor
 * ──────────────────────────────────────────────────────────────────────── */

void
mls_tls_reader_init(MlsTlsReader *r, const uint8_t *data, size_t len)
{
    r->data = data;
    r->len = len;
    r->pos = 0;
}

bool
mls_tls_reader_done(const MlsTlsReader *r)
{
    return r->pos >= r->len;
}

size_t
mls_tls_reader_remaining(const MlsTlsReader *r)
{
    return r->pos < r->len ? r->len - r->pos : 0;
}

int
mls_tls_read_u8(MlsTlsReader *r, uint8_t *out)
{
    if (mls_tls_reader_remaining(r) < 1) return -1;
    *out = r->data[r->pos++];
    return 0;
}

int
mls_tls_read_u16(MlsTlsReader *r, uint16_t *out)
{
    if (mls_tls_reader_remaining(r) < 2) return -1;
    *out = (uint16_t)((r->data[r->pos] << 8) | r->data[r->pos + 1]);
    r->pos += 2;
    return 0;
}

int
mls_tls_read_u32(MlsTlsReader *r, uint32_t *out)
{
    if (mls_tls_reader_remaining(r) < 4) return -1;
    *out = ((uint32_t)r->data[r->pos] << 24) |
           ((uint32_t)r->data[r->pos + 1] << 16) |
           ((uint32_t)r->data[r->pos + 2] << 8) |
           (uint32_t)r->data[r->pos + 3];
    r->pos += 4;
    return 0;
}

int
mls_tls_read_u64(MlsTlsReader *r, uint64_t *out)
{
    if (mls_tls_reader_remaining(r) < 8) return -1;
    *out = 0;
    for (int i = 0; i < 8; i++) {
        *out = (*out << 8) | r->data[r->pos + i];
    }
    r->pos += 8;
    return 0;
}

int
mls_tls_read_fixed(MlsTlsReader *r, uint8_t *out, size_t n)
{
    if (mls_tls_reader_remaining(r) < n) return -1;
    memcpy(out, r->data + r->pos, n);
    r->pos += n;
    return 0;
}

int
mls_tls_read_opaque8(MlsTlsReader *r, uint8_t **out, size_t *out_len)
{
    uint8_t len;
    if (mls_tls_read_u8(r, &len) != 0) return -1;
    if (mls_tls_reader_remaining(r) < len) return -1;

    if (len == 0) {
        *out = NULL;
        *out_len = 0;
        return 0;
    }

    *out = malloc(len);
    if (!*out) return -1;
    memcpy(*out, r->data + r->pos, len);
    r->pos += len;
    *out_len = len;
    return 0;
}

int
mls_tls_read_opaque16(MlsTlsReader *r, uint8_t **out, size_t *out_len)
{
    uint16_t len;
    if (mls_tls_read_u16(r, &len) != 0) return -1;
    if (mls_tls_reader_remaining(r) < len) return -1;

    if (len == 0) {
        *out = NULL;
        *out_len = 0;
        return 0;
    }

    *out = malloc(len);
    if (!*out) return -1;
    memcpy(*out, r->data + r->pos, len);
    r->pos += len;
    *out_len = len;
    return 0;
}

int
mls_tls_read_opaque32(MlsTlsReader *r, uint8_t **out, size_t *out_len)
{
    uint32_t len;
    if (mls_tls_read_u32(r, &len) != 0) return -1;
    if (mls_tls_reader_remaining(r) < (size_t)len) return -1;

    if (len == 0) {
        *out = NULL;
        *out_len = 0;
        return 0;
    }

    *out = malloc(len);
    if (!*out) return -1;
    memcpy(*out, r->data + r->pos, len);
    r->pos += len;
    *out_len = (size_t)len;
    return 0;
}
