/* SPDX-License-Identifier: MIT */
#include "internal.h"
#include <stdarg.h>
#include <stdio.h>
void vj_ws(vj_reader *r)
{
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\n' || *r->p == '\r' || *r->p == '\t'))
        ++r->p;
}
int vj_take(vj_reader *r, char c)
{
    vj_ws(r);
    if (r->p == r->end || *r->p != c)
        return VAULT_ERR_ARG;
    ++r->p;
    return 0;
}
int vj_init(const char *s, vj_reader *r)
{
    if (!s || !r)
        return VAULT_ERR_ARG;
    size_t n = strnlen(s, 65536);
    if (n > 65535)
        return PORTHOME_ERR_TOO_LARGE;
    r->p = s;
    r->end = s + n;
    return 0;
}
int vj_end(vj_reader *r)
{
    vj_ws(r);
    return r->p == r->end ? 0 : VAULT_ERR_ARG;
}
int vj_string(vj_reader *r, char *out, size_t cap)
{
    if (vj_take(r, '"'))
        return VAULT_ERR_ARG;
    size_t n = 0;
    while (r->p < r->end && *r->p != '"') {
        unsigned char c = (unsigned char)*r->p++;
        if (c < 0x20 || c > 0x7e || c == '\\' || n + 1 >= cap)
            return VAULT_ERR_ARG;
        out[n++] = (char)c;
    }
    if (r->p == r->end)
        return VAULT_ERR_ARG;
    ++r->p;
    out[n] = 0;
    return 0;
}
int vj_field(vj_reader *r, const char *const *names, size_t n)
{
    char field[64];
    if (vj_string(r, field, sizeof(field)) || vj_take(r, ':'))
        return -1;
    for (size_t i = 0; i < n; ++i)
        if (!strcmp(field, names[i]))
            return (int)i;
    return -1;
}
int vj_uint(vj_reader *r, uint64_t *out)
{
    vj_ws(r);
    if (r->p == r->end || *r->p < '0' || *r->p > '9')
        return VAULT_ERR_ARG;
    int zero = *r->p == '0';
    size_t digits = 0;
    uint64_t v = 0;
    while (r->p < r->end && *r->p >= '0' && *r->p <= '9') {
        unsigned d = (unsigned)(*r->p++ - '0');
        if ((zero && digits) || v > (UINT64_MAX - d) / 10)
            return VAULT_ERR_ARG;
        v = v * 10 + d;
        ++digits;
    }
    *out = v;
    return 0;
}
static int nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}
int vj_hex(vj_reader *r, uint8_t out[32])
{
    if (vj_take(r, '"') || r->end - r->p < 65)
        return VAULT_ERR_ARG;
    uint8_t b[32];
    int rc = VAULT_ERR_ARG;
    for (size_t i = 0; i < 32; ++i) {
        int hi = nibble(r->p[2 * i]), lo = nibble(r->p[2 * i + 1]);
        if (hi < 0 || lo < 0)
            goto done;
        b[i] = (uint8_t)(hi * 16 + lo);
    }
    r->p += 64;
    if (*r->p++ != '"')
        goto done;
    memcpy(out, b, 32);
    rc = 0;
done:
    OPENSSL_cleanse(b, 32);
    return rc;
}
int vj_url_valid(const char *s, int relay)
{
    if (!s)
        return 0;
    size_t n = strnlen(s, VAULT_MAX_URL + 1);
    if (!n || n > VAULT_MAX_URL)
        return 0;
    size_t prefix = 0;
    if (relay) {
        if (!strncmp(s, "wss://", 6))
            prefix = 6;
        else if (!strncmp(s, "ws://", 5))
            prefix = 5;
    } else {
        if (!strncmp(s, "https://", 8))
            prefix = 8;
        else if (!strncmp(s, "http://", 7))
            prefix = 7;
    }
    if (!prefix || n == prefix || s[prefix] == '/')
        return 0;
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char)s[i] <= 0x20 || (unsigned char)s[i] >= 0x7f || s[i] == '"' ||
            s[i] == '\\')
            return 0;
    return 1;
}
void vj_urls_free(char **urls, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        free(urls[i]);
    free(urls);
}
int vj_urls(vj_reader *r, char ***out, size_t *len, int relay)
{
    if (vj_take(r, '['))
        return VAULT_ERR_ARG;
    vj_ws(r);
    if (r->p < r->end && *r->p == ']') {
        ++r->p;
        return 0;
    }
    *out = calloc(VAULT_MAX_HINTS, sizeof(**out));
    if (!*out)
        return VAULT_ERR_OOM;
    for (;;) {
        char url[VAULT_MAX_URL + 1];
        if (*len == VAULT_MAX_HINTS || vj_string(r, url, sizeof(url)) || !vj_url_valid(url, relay))
            return VAULT_ERR_ARG;
        (*out)[*len] = strdup(url);
        if (!(*out)[*len])
            return VAULT_ERR_OOM;
        ++*len;
        vj_ws(r);
        if (r->p < r->end && *r->p == ']') {
            ++r->p;
            return 0;
        }
        if (vj_take(r, ','))
            return VAULT_ERR_ARG;
    }
}
int vj_append(char *s, size_t cap, size_t *used, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(s + *used, cap - *used, fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= cap - *used)
        return PORTHOME_ERR_TOO_LARGE;
    *used += (size_t)n;
    return 0;
}
int vj_write_urls(char *s, size_t cap, size_t *used, char *const *urls, size_t n, int relay)
{
    if (n > VAULT_MAX_HINTS || (n && !urls))
        return VAULT_ERR_ARG;
    int rc = vj_append(s, cap, used, "[");
    if (rc)
        return rc;
    for (size_t i = 0; i < n; ++i) {
        if (!vj_url_valid(urls[i], relay))
            return VAULT_ERR_ARG;
        rc = vj_append(s, cap, used, "%s\"%s\"", i ? "," : "", urls[i]);
        if (rc)
            return rc;
    }
    return vj_append(s, cap, used, "]");
}
