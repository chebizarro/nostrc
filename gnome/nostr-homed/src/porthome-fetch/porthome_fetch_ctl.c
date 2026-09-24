/*
 * porthome_fetch_ctl.c — bounded JSON parser for the nostr-home-fetch
 * control payload, plus a bounded progress-line codec. See
 * porthome_fetch_ctl.h.
 *
 * SPDX-License-Identifier: MIT
 * EXPERIMENTAL.
 *
 * Deliberately tiny and self-contained: no libc dep beyond <string.h> /
 * <ctype.h> / <stdlib.h>, no jansson. Rationale: this code runs before
 * we know anything about the network payloads, so its attack surface
 * MUST be small. It handles exactly the flat object shape documented
 * in the header and refuses anything else.
 */

#include "porthome_fetch_ctl.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ───────────────────────────────── tiny JSON scanner ─────────── */

typedef struct {
    const char *s;
    size_t      len;
    size_t      pos;
    int         depth;
} scan;

static void skip_ws(scan *sc) {
    while (sc->pos < sc->len) {
        unsigned char c = (unsigned char)sc->s[sc->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') sc->pos++;
        else break;
    }
}

static int peek(scan *sc) {
    return sc->pos < sc->len ? (unsigned char)sc->s[sc->pos] : -1;
}

static int expect_char(scan *sc, char c) {
    skip_ws(sc);
    if (sc->pos >= sc->len || sc->s[sc->pos] != c) return -1;
    sc->pos++;
    return 0;
}

/* Parse a JSON string into `out` (a fixed-size buffer of size cap,
 * NUL-terminated on success). Accepts ONLY: ASCII printable, no
 * backslash escapes (we don't need any — hex/URL fields never contain
 * them; the d-tag is an ASCII opaque tag). Any escape or non-ASCII
 * byte → -1. This is a defensive restriction. */
static int parse_string(scan *sc, char *out, size_t cap) {
    skip_ws(sc);
    if (peek(sc) != '"') return -1;
    sc->pos++;
    size_t n = 0;
    while (sc->pos < sc->len) {
        unsigned char c = (unsigned char)sc->s[sc->pos++];
        if (c == '"') {
            if (n >= cap) return -1;
            out[n] = '\0';
            return 0;
        }
        if (c == '\\') return -1;              /* no escapes */
        if (c < 0x20 || c > 0x7E) return -1;   /* ASCII printable only */
        if (n + 1 >= cap) return -1;
        out[n++] = (char)c;
    }
    return -1; /* unterminated */
}

/* Parse an unsigned decimal integer, up to UINT64_MAX. Refuses leading
 * '+', leading '0' followed by more digits (canonical), and empty. */
static int parse_u64(scan *sc, uint64_t *out) {
    skip_ws(sc);
    if (sc->pos >= sc->len) return -1;
    if (!isdigit((unsigned char)sc->s[sc->pos])) return -1;

    uint64_t v = 0;
    int leading_zero = (sc->s[sc->pos] == '0');
    int digits = 0;
    while (sc->pos < sc->len && isdigit((unsigned char)sc->s[sc->pos])) {
        unsigned d = (unsigned)(sc->s[sc->pos] - '0');
        /* Overflow check: v*10 + d */
        if (v > (UINT64_MAX - d) / 10ull) return -1;
        v = v * 10ull + d;
        sc->pos++;
        digits++;
    }
    if (digits == 0) return -1;
    if (leading_zero && digits > 1) return -1;
    *out = v;
    return 0;
}

/* Parse a JSON bool. `*out` set to 0/1. */
static int parse_bool(scan *sc, int *out) {
    skip_ws(sc);
    if (sc->pos + 4 <= sc->len && memcmp(sc->s + sc->pos, "true", 4) == 0) {
        sc->pos += 4; *out = 1; return 0;
    }
    if (sc->pos + 5 <= sc->len && memcmp(sc->s + sc->pos, "false", 5) == 0) {
        sc->pos += 5; *out = 0; return 0;
    }
    return -1;
}

/* Parse a JSON array of strings into a caller-supplied 2D char buffer.
 * Enforces max_count and per-string max_len (excluding NUL). Returns
 * count on success or -1 on failure. Empty array is allowed and
 * yields 0 (caller may reject that). */
static int parse_string_array(scan *sc,
                              char (*out)[NH_PORTHOME_FETCH_MAX_URL_LEN + 1],
                              size_t max_count,
                              size_t max_len_excl_nul) {
    skip_ws(sc);
    if (peek(sc) != '[') return -1;
    sc->pos++;
    skip_ws(sc);
    size_t n = 0;
    if (peek(sc) == ']') { sc->pos++; return 0; }
    for (;;) {
        if (n >= max_count) return -1;
        char tmp[NH_PORTHOME_FETCH_MAX_URL_LEN + 1];
        if (parse_string(sc, tmp, sizeof tmp) != 0) return -1;
        size_t l = strlen(tmp);
        if (l == 0 || l > max_len_excl_nul) return -1;
        memcpy(out[n], tmp, l + 1);
        n++;
        skip_ws(sc);
        int c = peek(sc);
        if (c == ',') { sc->pos++; continue; }
        if (c == ']') { sc->pos++; return (int)n; }
        return -1;
    }
}

/* ────────────────────────── field validators ─────────────────── */

static int hex64_ok(const char *s) {
    if (!s) return -1;
    for (size_t i = 0; i < 64; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return -1;
    }
    return s[64] == '\0' ? 0 : -1;
}

static int url_wss_ok(const char *u) {
    if (!u) return -1;
    if (strncmp(u, "wss://", 6) != 0 && strncmp(u, "ws://", 5) != 0)
        return -1;
    for (const char *p = u; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= 0x20 || c == 0x7F || c == '#' || c == '"' || c == '\\')
            return -1;
    }
    return 0;
}

static int url_https_ok(const char *u, int allow_insecure) {
    if (!u) return -1;
    int https = strncmp(u, "https://", 8) == 0;
    int http  = strncmp(u, "http://", 7) == 0;
    if (!https && !(http && allow_insecure)) return -1;
    for (const char *p = u; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= 0x20 || c == 0x7F || c == '#' || c == '"' || c == '\\')
            return -1;
    }
    return 0;
}

static int dtag_ok(const char *s) {
    if (!s || !s[0]) return -1;
    size_t l = strlen(s);
    if (l > NH_PORTHOME_FETCH_MAX_DTAG_LEN) return -1;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c > 0x7E) return -1;
    }
    return 0;
}

/* ─────────────────────────── main parse ──────────────────────── */

nh_porthome_fetch_ctl_status
nh_porthome_fetch_ctl_parse(const char *data, size_t len,
                            nh_porthome_fetch_ctl *out)
{
    if (!out) return NH_PORTHOME_FETCH_CTL_ERR_ARG;
    memset(out, 0, sizeof *out);
    if (!data) return NH_PORTHOME_FETCH_CTL_ERR_ARG;
    if (len > NH_PORTHOME_FETCH_MAX_CTL_BYTES)
        return NH_PORTHOME_FETCH_CTL_ERR_TOO_LARGE;

    scan sc = { .s = data, .len = len, .pos = 0, .depth = 0 };
    if (expect_char(&sc, '{') != 0) return NH_PORTHOME_FETCH_CTL_ERR_JSON;

    /* Bitset of which fields we have seen. */
    enum {
        F_ACCOUNT=0, F_ROOT, F_KEY, F_DTAG, F_RELAYS, F_SERVERS,
        F_BANDWIDTH, F_TIMEOUT_SEC, F_MAX_BYTES, F_RELAY_TIMEOUT_MS,
        F_ALLOW_INSECURE, F_COUNT
    };
    unsigned seen = 0;
    #define MARK(f) do { seen |= (1u << (f)); } while (0)
    #define HAS(f)  ((seen >> (f)) & 1u)

    skip_ws(&sc);
    if (peek(&sc) == '}') { sc.pos++; goto validate; }

    for (;;) {
        char key[64];
        if (parse_string(&sc, key, sizeof key) != 0)
            return NH_PORTHOME_FETCH_CTL_ERR_JSON;
        if (expect_char(&sc, ':') != 0)
            return NH_PORTHOME_FETCH_CTL_ERR_JSON;

        if (strcmp(key, "account_pubkey_hex") == 0) {
            if (parse_string(&sc, out->account_pubkey_hex,
                             sizeof out->account_pubkey_hex) != 0)
                return NH_PORTHOME_FETCH_CTL_ERR_JSON;
            if (hex64_ok(out->account_pubkey_hex) != 0)
                return NH_PORTHOME_FETCH_CTL_ERR_HEX;
            MARK(F_ACCOUNT);
        } else if (strcmp(key, "home_root_id_hex") == 0) {
            if (parse_string(&sc, out->home_root_id_hex,
                             sizeof out->home_root_id_hex) != 0)
                return NH_PORTHOME_FETCH_CTL_ERR_JSON;
            if (hex64_ok(out->home_root_id_hex) != 0)
                return NH_PORTHOME_FETCH_CTL_ERR_HEX;
            MARK(F_ROOT);
        } else if (strcmp(key, "home_key_hex") == 0) {
            if (parse_string(&sc, out->home_key_hex,
                             sizeof out->home_key_hex) != 0)
                return NH_PORTHOME_FETCH_CTL_ERR_JSON;
            if (hex64_ok(out->home_key_hex) != 0)
                return NH_PORTHOME_FETCH_CTL_ERR_HEX;
            MARK(F_KEY);
        } else if (strcmp(key, "d_tag") == 0) {
            if (parse_string(&sc, out->d_tag, sizeof out->d_tag) != 0)
                return NH_PORTHOME_FETCH_CTL_ERR_JSON;
            if (dtag_ok(out->d_tag) != 0)
                return NH_PORTHOME_FETCH_CTL_ERR_ARG;
            MARK(F_DTAG);
        } else if (strcmp(key, "relays") == 0) {
            int n = parse_string_array(&sc, out->relays,
                                       NH_PORTHOME_FETCH_MAX_RELAYS,
                                       NH_PORTHOME_FETCH_MAX_URL_LEN);
            if (n < 0) return NH_PORTHOME_FETCH_CTL_ERR_JSON;
            out->relays_count = (size_t)n;
            MARK(F_RELAYS);
        } else if (strcmp(key, "blossom_servers") == 0) {
            int n = parse_string_array(&sc, out->blossom_servers,
                                       NH_PORTHOME_FETCH_MAX_SERVERS,
                                       NH_PORTHOME_FETCH_MAX_URL_LEN);
            if (n < 0) return NH_PORTHOME_FETCH_CTL_ERR_JSON;
            out->blossom_servers_count = (size_t)n;
            MARK(F_SERVERS);
        } else if (strcmp(key, "bandwidth_cap_bytes") == 0) {
            if (parse_u64(&sc, &out->bandwidth_cap_bytes) != 0)
                return NH_PORTHOME_FETCH_CTL_ERR_JSON;
            MARK(F_BANDWIDTH);
        } else if (strcmp(key, "per_file_timeout_sec") == 0) {
            uint64_t v;
            if (parse_u64(&sc, &v) != 0 || v > UINT32_MAX)
                return NH_PORTHOME_FETCH_CTL_ERR_JSON;
            out->per_file_timeout_sec = (uint32_t)v;
            MARK(F_TIMEOUT_SEC);
        } else if (strcmp(key, "max_total_bytes") == 0) {
            if (parse_u64(&sc, &out->max_total_bytes) != 0)
                return NH_PORTHOME_FETCH_CTL_ERR_JSON;
            MARK(F_MAX_BYTES);
        } else if (strcmp(key, "relay_timeout_ms") == 0) {
            uint64_t v;
            if (parse_u64(&sc, &v) != 0 || v > UINT32_MAX)
                return NH_PORTHOME_FETCH_CTL_ERR_JSON;
            out->relay_timeout_ms = (uint32_t)v;
            MARK(F_RELAY_TIMEOUT_MS);
        } else if (strcmp(key, "allow_insecure") == 0) {
            int b;
            if (parse_bool(&sc, &b) != 0) return NH_PORTHOME_FETCH_CTL_ERR_JSON;
            out->allow_insecure = b ? 1 : 0;
            MARK(F_ALLOW_INSECURE);
        } else {
            return NH_PORTHOME_FETCH_CTL_ERR_UNKNOWN_KEY;
        }

        skip_ws(&sc);
        int c = peek(&sc);
        if (c == ',') { sc.pos++; continue; }
        if (c == '}') { sc.pos++; break; }
        return NH_PORTHOME_FETCH_CTL_ERR_JSON;
    }

    /* Trailing whitespace is fine, trailing content is not. */
    skip_ws(&sc);
    if (sc.pos != sc.len) return NH_PORTHOME_FETCH_CTL_ERR_JSON;

validate:;
    /* Required fields. allow_insecure is optional. */
    const unsigned required =
        (1u<<F_ACCOUNT) | (1u<<F_ROOT) | (1u<<F_KEY) | (1u<<F_DTAG) |
        (1u<<F_RELAYS)  | (1u<<F_SERVERS) |
        (1u<<F_BANDWIDTH) | (1u<<F_TIMEOUT_SEC) | (1u<<F_MAX_BYTES) |
        (1u<<F_RELAY_TIMEOUT_MS);
    if ((seen & required) != required)
        return NH_PORTHOME_FETCH_CTL_ERR_MISSING;
    if (out->relays_count == 0 || out->blossom_servers_count == 0)
        return NH_PORTHOME_FETCH_CTL_ERR_MISSING;

    /* URL validation. wss:// (or ws:// if allow_insecure) for relays,
     * https:// (or http:// if allow_insecure) for Blossom. */
    for (size_t i = 0; i < out->relays_count; i++) {
        if (url_wss_ok(out->relays[i]) != 0) {
            if (!(out->allow_insecure &&
                  strncmp(out->relays[i], "ws://", 5) == 0))
                return NH_PORTHOME_FETCH_CTL_ERR_URL;
        }
        /* If we are strict, refuse ws:// even when parseable. */
        if (!out->allow_insecure &&
            strncmp(out->relays[i], "ws://", 5) == 0)
            return NH_PORTHOME_FETCH_CTL_ERR_URL;
    }
    for (size_t i = 0; i < out->blossom_servers_count; i++) {
        if (url_https_ok(out->blossom_servers[i], out->allow_insecure) != 0)
            return NH_PORTHOME_FETCH_CTL_ERR_URL;
    }

    return NH_PORTHOME_FETCH_CTL_OK;
    #undef MARK
    #undef HAS
}

const char *nh_porthome_fetch_ctl_strerror(nh_porthome_fetch_ctl_status s) {
    switch (s) {
    case NH_PORTHOME_FETCH_CTL_OK:              return "ok";
    case NH_PORTHOME_FETCH_CTL_ERR_ARG:         return "arg";
    case NH_PORTHOME_FETCH_CTL_ERR_TOO_LARGE:   return "too_large";
    case NH_PORTHOME_FETCH_CTL_ERR_JSON:        return "json";
    case NH_PORTHOME_FETCH_CTL_ERR_MISSING:     return "missing_field";
    case NH_PORTHOME_FETCH_CTL_ERR_URL:         return "url";
    case NH_PORTHOME_FETCH_CTL_ERR_HEX:         return "hex";
    case NH_PORTHOME_FETCH_CTL_ERR_UNKNOWN_KEY: return "unknown_key";
    }
    return "unknown";
}

/* ─────────────────────── progress line codec ─────────────────── */

static const struct { const char *name; nh_porthome_fetch_phase phase; } PHASES[] = {
    { "manifest", NH_PORTHOME_FETCH_PHASE_MANIFEST },
    { "chunk",    NH_PORTHOME_FETCH_PHASE_CHUNK    },
    { "decode",   NH_PORTHOME_FETCH_PHASE_DECODE   },
    { "done",     NH_PORTHOME_FETCH_PHASE_DONE     },
};

nh_porthome_fetch_progress_status
nh_porthome_fetch_progress_parse(const char *line, size_t line_len,
                                 nh_porthome_fetch_progress *out) {
    if (!line || !out) return NH_PORTHOME_FETCH_PROG_ERR;
    if (line_len >= NH_PORTHOME_FETCH_MAX_PROGRESS_LINE)
        return NH_PORTHOME_FETCH_PROG_TOO_LONG;

    /* Copy to a bounded local so we can NUL-terminate. */
    char buf[NH_PORTHOME_FETCH_MAX_PROGRESS_LINE];
    memcpy(buf, line, line_len);
    buf[line_len] = '\0';

    scan sc = { .s = buf, .len = line_len, .pos = 0, .depth = 0 };
    if (expect_char(&sc, '{') != 0) return NH_PORTHOME_FETCH_PROG_ERR;

    nh_porthome_fetch_progress p = {0};
    int have_bytes = 0, have_files = 0, have_phase = 0;

    for (;;) {
        char key[16];
        if (parse_string(&sc, key, sizeof key) != 0)
            return NH_PORTHOME_FETCH_PROG_ERR;
        if (expect_char(&sc, ':') != 0) return NH_PORTHOME_FETCH_PROG_ERR;

        if (strcmp(key, "bytes") == 0) {
            if (parse_u64(&sc, &p.bytes) != 0) return NH_PORTHOME_FETCH_PROG_ERR;
            have_bytes = 1;
        } else if (strcmp(key, "files") == 0) {
            if (parse_u64(&sc, &p.files) != 0) return NH_PORTHOME_FETCH_PROG_ERR;
            have_files = 1;
        } else if (strcmp(key, "phase") == 0) {
            char pstr[16];
            if (parse_string(&sc, pstr, sizeof pstr) != 0)
                return NH_PORTHOME_FETCH_PROG_ERR;
            int matched = 0;
            for (size_t i = 0; i < sizeof(PHASES)/sizeof(PHASES[0]); i++) {
                if (strcmp(pstr, PHASES[i].name) == 0) {
                    p.phase = PHASES[i].phase;
                    matched = 1;
                    break;
                }
            }
            if (!matched) return NH_PORTHOME_FETCH_PROG_ERR;
            have_phase = 1;
        } else {
            return NH_PORTHOME_FETCH_PROG_ERR;
        }

        skip_ws(&sc);
        int c = peek(&sc);
        if (c == ',') { sc.pos++; continue; }
        if (c == '}') { sc.pos++; break; }
        return NH_PORTHOME_FETCH_PROG_ERR;
    }
    skip_ws(&sc);
    if (sc.pos != sc.len) return NH_PORTHOME_FETCH_PROG_ERR;
    if (!have_bytes || !have_files || !have_phase)
        return NH_PORTHOME_FETCH_PROG_ERR;
    *out = p;
    return NH_PORTHOME_FETCH_PROG_OK;
}

int nh_porthome_fetch_progress_format(char *buf, size_t buf_cap,
                                      const nh_porthome_fetch_progress *p) {
    if (!buf || !p) return -1;
    const char *phase = NULL;
    for (size_t i = 0; i < sizeof(PHASES)/sizeof(PHASES[0]); i++) {
        if (PHASES[i].phase == p->phase) { phase = PHASES[i].name; break; }
    }
    if (!phase) return -1;
    int n = snprintf(buf, buf_cap,
                     "{\"bytes\":%llu,\"files\":%llu,\"phase\":\"%s\"}",
                     (unsigned long long)p->bytes,
                     (unsigned long long)p->files, phase);
    if (n < 0 || (size_t)n >= buf_cap) return -1;
    return n;
}
