/**
 * @file nostr_json_parse.c
 * @brief Shared JSON parsing primitives for compact deserializers.
 *
 * Extracted from duplicated static helpers in event.c, envelope.c, and
 * filter.c to provide a single implementation of hex conversion, whitespace
 * skipping, UTF-8 encoding, JSON string parsing, and integer parsing.
 */
#include "nostr-json-parse.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* Max realloc cap for JSON string buffers (16 MB) - nostrc-g4t hardening */
#define JSON_STRING_MAX_CAP (16 * 1024 * 1024)

int nostr_json_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

const char *nostr_json_skip_ws(const char *p) {
    if (!p) return p;
    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') ++p;
    return p;
}

int nostr_json_utf8_encode(uint32_t cp, char *out) {
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

char *nostr_json_parse_string(const char **pp) {
    const char *p = nostr_json_skip_ws(*pp);
    if (*p != '"') return NULL;
    ++p;
    const char *start = p;

    /* Fast path: scan for escape sequences. If none, direct copy. */
    const char *q = p;
    int has_escape = 0;
    while (*q && *q != '"') {
        if (*q == '\\') { has_escape = 1; break; }
        ++q;
    }
    if (!*q) return NULL; /* missing closing quote */

    if (!has_escape) {
        size_t len = (size_t)(q - start);
        char *s = (char *)malloc(len + 1);
        if (!s) return NULL;
        memcpy(s, start, len);
        s[len] = '\0';
        *pp = q + 1;
        return s;
    }

    /* Slow path: decode escape sequences */
    size_t cap = 64;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;

    /* Copy any literal characters before the first escape */
    size_t prefix = (size_t)(q - start);
    if (prefix > 0) {
        if (prefix >= cap) {
            cap = prefix + 64;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        memcpy(buf, start, prefix);
        len = prefix;
    }
    p = q; /* resume from the backslash */

    while (*p && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\') {
            if (!*p) { free(buf); return NULL; } /* null after backslash */
            char e = *p++;
            switch (e) {
                case '"':  buf[len++] = '"'; break;
                case '\\': buf[len++] = '\\'; break;
                case '/':  buf[len++] = '/'; break;
                case 'b':  buf[len++] = '\b'; break;
                case 'f':  buf[len++] = '\f'; break;
                case 'n':  buf[len++] = '\n'; break;
                case 'r':  buf[len++] = '\r'; break;
                case 't':  buf[len++] = '\t'; break;
                case 'u': {
                    /* Validate 4 hex digits remain before dereferencing */
                    if (!p[0] || !p[1] || !p[2] || !p[3]) { free(buf); return NULL; }
                    int h0 = nostr_json_hexval(p[0]);
                    int h1 = nostr_json_hexval(p[1]);
                    int h2 = nostr_json_hexval(p[2]);
                    int h3 = nostr_json_hexval(p[3]);
                    if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) { free(buf); return NULL; }
                    uint32_t cp = (uint32_t)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                    p += 4;

                    /* Handle UTF-16 surrogate pairs */
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (!p[0] || !p[1] || p[0] != '\\' || p[1] != 'u') { free(buf); return NULL; }
                        p += 2;
                        if (!p[0] || !p[1] || !p[2] || !p[3]) { free(buf); return NULL; }
                        int g0 = nostr_json_hexval(p[0]);
                        int g1 = nostr_json_hexval(p[1]);
                        int g2 = nostr_json_hexval(p[2]);
                        int g3 = nostr_json_hexval(p[3]);
                        if (g0 < 0 || g1 < 0 || g2 < 0 || g3 < 0) { free(buf); return NULL; }
                        uint32_t low = (uint32_t)((g0 << 12) | (g1 << 8) | (g2 << 4) | g3);
                        if (low < 0xDC00 || low > 0xDFFF) { free(buf); return NULL; }
                        cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                        p += 4;
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        /* Lone low surrogate is invalid */
                        free(buf); return NULL;
                    }

                    /* Ensure room for up to 4 UTF-8 bytes + null */
                    if (len + 4 >= cap) {
                        size_t ncap = cap * 2;
                        while (len + 5 > ncap) ncap *= 2;
                        if (ncap > JSON_STRING_MAX_CAP) { free(buf); return NULL; }
                        char *nb = (char *)realloc(buf, ncap);
                        if (!nb) { free(buf); return NULL; }
                        buf = nb;
                        cap = ncap;
                    }
                    char tmp[4];
                    int n = nostr_json_utf8_encode(cp, tmp);
                    for (int i = 0; i < n; i++) buf[len++] = tmp[i];
                    continue; /* skip the buf grow check below */
                }
                default:
                    free(buf);
                    return NULL;
            }
        } else {
            buf[len++] = (char)c;
        }
        /* Ensure room for next character + null */
        if (len + 1 >= cap) {
            cap *= 2;
            if (cap > JSON_STRING_MAX_CAP) { free(buf); return NULL; }
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
    }

    if (*p != '"') { free(buf); return NULL; }
    buf[len] = '\0';
    *pp = p + 1;
    return buf;
}

int nostr_json_parse_int64(const char **pp, long long *out) {
    const char *p = nostr_json_skip_ws(*pp);
    int neg = 0;
    long long v = 0;
    int any = 0;
    if (*p == '-') { neg = 1; ++p; }
    while (*p >= '0' && *p <= '9') {
        if (v > (LLONG_MAX - 9) / 10) return 0; /* overflow check */
        v = v * 10 + (*p - '0'); ++p; any = 1;
    }
    if (!any) return 0;
    *out = neg ? -v : v;
    *pp = p;
    return 1;
}
