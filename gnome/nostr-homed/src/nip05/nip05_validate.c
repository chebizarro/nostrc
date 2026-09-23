/* nip05_validate.c — syntactic validation, parsing, URL assembly,
 * and .well-known/nostr.json response parsing. Zero external deps
 * beyond libc + jansson (JSON), so the unit-test binary can pull
 * this compilation unit in directly. */
#define _GNU_SOURCE
#include "nostr_nip05.h"

#include <ctype.h>
#include <jansson.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ── Grammar ─────────────────────────────────────────────────── */

/* Local-part character set per NIP-05 §"…the local part may include
 * a-z0-9-_.". We treat upper-case letters as accepted (matched
 * case-insensitively) but leave the original bytes in place — case
 * folding happens at query time. */
static int is_local_char(unsigned char c) {
    if (c >= 'a' && c <= 'z') return 1;
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    if (c == '-' || c == '_' || c == '.') return 1;
    return 0;
}

/* RFC 1035 preferred-name grammar. Each label: [A-Za-z0-9] with
 * optional interior [-] then [A-Za-z0-9]; label length <= 63; total
 * length <= 253. Underscore is allowed by NIP-05 but not RFC 1035;
 * we permit '_' in a label because some real-world .well-known
 * domains use it (e.g. `_nostr` sub-domains for dev). */
static int is_domain_char(unsigned char c) {
    if (c >= 'a' && c <= 'z') return 1;
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    if (c == '-' || c == '_') return 1;
    return 0;
}

static int domain_is_valid(const char *domain) {
    size_t n = strlen(domain);
    if (n == 0 || n > NH_NIP05_DOMAIN_MAX) return 0;
    if (domain[0] == '.' || domain[n - 1] == '.') return 0;
    size_t label_len = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)domain[i];
        if (c == '.') {
            if (label_len == 0) return 0;                   /* empty label */
            label_len = 0;
            continue;
        }
        if (!is_domain_char(c)) return 0;
        /* First byte of a label must be alnum (or '_' per relaxation
         * above); '-' cannot start a label per RFC 1035. */
        if (label_len == 0 && c == '-') return 0;
        label_len++;
        if (label_len > 63) return 0;
    }
    if (label_len == 0) return 0; /* trailing '.' */
    /* Require at least one dot so bare hostnames ("localhost") are
     * refused — the SSRF guard would still catch loopback, but we
     * want to fail closed at parse time. */
    if (!strchr(domain, '.')) return 0;
    return 1;
}

static int local_is_valid(const char *local) {
    size_t n = strlen(local);
    if (n == 0 || n > NH_NIP05_LOCAL_MAX) return 0;
    /* Root identifier "_" is explicitly allowed. */
    if (n == 1 && local[0] == '_') return 1;
    /* No leading/trailing '.' (would produce empty implicit labels
     * on the wire); interior dots are fine. */
    if (local[0] == '.' || local[n - 1] == '.') return 0;
    for (size_t i = 0; i < n; i++) {
        if (!is_local_char((unsigned char)local[i])) return 0;
    }
    return 1;
}

int nh_nip05_is_valid(const char *s) {
    if (!s) return 0;
    /* Must contain exactly one '@'. */
    const char *at = strchr(s, '@');
    if (!at || strchr(at + 1, '@')) return 0;
    size_t local_len = (size_t)(at - s);
    if (local_len == 0 || local_len > NH_NIP05_LOCAL_MAX) return 0;
    size_t domain_len = strlen(at + 1);
    if (domain_len == 0 || domain_len > NH_NIP05_DOMAIN_MAX) return 0;
    /* Reject any whitespace / control byte anywhere. */
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p <= 0x20 || *p == 0x7f) return 0;
    }
    char local[NH_NIP05_LOCAL_MAX + 1];
    char domain[NH_NIP05_DOMAIN_MAX + 1];
    memcpy(local, s, local_len);
    local[local_len] = '\0';
    memcpy(domain, at + 1, domain_len);
    domain[domain_len] = '\0';
    return local_is_valid(local) && domain_is_valid(domain);
}

int nh_nip05_parse(const char *s, nh_nip05_address *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!nh_nip05_is_valid(s)) return -1;
    const char *at = strchr(s, '@');
    size_t local_len = (size_t)(at - s);
    memcpy(out->local, s, local_len);
    out->local[local_len] = '\0';
    size_t domain_len = strlen(at + 1);
    /* Lower-case the domain (case-insensitive per DNS + NIP-05). */
    for (size_t i = 0; i < domain_len; i++) {
        unsigned char c = (unsigned char)at[1 + i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        out->domain[i] = (char)c;
    }
    out->domain[domain_len] = '\0';
    int n = snprintf(out->address, sizeof out->address, "%s@%s",
                     out->local, out->domain);
    if (n <= 0 || (size_t)n >= sizeof out->address) {
        memset(out, 0, sizeof *out);
        return -1;
    }
    return 0;
}

/* ── URL assembly (RFC 3986 pct-encode) ──────────────────────── */

static int is_unreserved(unsigned char c) {
    if (c >= 'a' && c <= 'z') return 1;
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    if (c == '-' || c == '.' || c == '_' || c == '~') return 1;
    return 0;
}

int nh_nip05_wellknown_url(const nh_nip05_address *addr, char *out,
                           size_t cap) {
    if (!addr || !out || cap < 32) return -1;
    /* "https://" + domain + "/.well-known/nostr.json?name=" + encoded */
    static const char hex[] = "0123456789abcdef";
    /* Encode local into a scratch buffer first so we can measure the
     * final URL length in one shot. Worst case = 3x local length. */
    char enc[NH_NIP05_LOCAL_MAX * 3 + 1];
    size_t elen = 0;
    for (size_t i = 0; addr->local[i]; i++) {
        unsigned char c = (unsigned char)addr->local[i];
        if (is_unreserved(c)) {
            if (elen + 1 >= sizeof enc) return -1;
            enc[elen++] = (char)c;
        } else {
            if (elen + 3 >= sizeof enc) return -1;
            enc[elen++] = '%';
            enc[elen++] = hex[c >> 4];
            enc[elen++] = hex[c & 0xf];
        }
    }
    enc[elen] = '\0';
    int n = snprintf(out, cap, "https://%s/.well-known/nostr.json?name=%s",
                     addr->domain, enc);
    if (n <= 0 || (size_t)n >= cap) return -1;
    return 0;
}

/* ── .well-known/nostr.json parse ────────────────────────────── */

static int is_lc_hex64(const char *s) {
    if (!s) return 0;
    if (strlen(s) != 64) return 0;
    for (int i = 0; i < 64; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return 1;
}

/* Copy @s lower-cased into @dst[cap]. Returns -1 on overflow. */
static int copy_lower(char *dst, size_t cap, const char *s) {
    size_t n = strlen(s);
    if (n + 1 > cap) return -1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        dst[i] = (char)c;
    }
    dst[n] = '\0';
    return 0;
}

nh_nip05_rc nh_nip05_parse_wellknown(const char *json, size_t len,
                                     const char *local,
                                     nh_nip05_result *result_out) {
    if (!json || !local || !result_out) return NH_NIP05_ERR_JSON;
    memset(result_out, 0, sizeof *result_out);

    json_error_t je;
    json_t *root = json_loadb(json, len, JSON_REJECT_DUPLICATES, &je);
    if (!root) return NH_NIP05_ERR_JSON;
    if (!json_is_object(root)) { json_decref(root); return NH_NIP05_ERR_JSON; }

    json_t *names = json_object_get(root, "names");
    if (!names || !json_is_object(names)) {
        json_decref(root);
        return NH_NIP05_ERR_NAME;
    }

    /* Case-insensitive lookup: try the exact-case key first (the
     * common path), then a lowercased scan for issuers that stored
     * the key in a different case. */
    json_t *pk = json_object_get(names, local);
    char local_lc[NH_NIP05_LOCAL_MAX + 1];
    if (copy_lower(local_lc, sizeof local_lc, local) != 0) {
        json_decref(root);
        return NH_NIP05_ERR_ARG;
    }
    if (!pk) {
        const char *k;
        json_t *v;
        json_object_foreach(names, k, v) {
            char k_lc[NH_NIP05_LOCAL_MAX + 1];
            if (copy_lower(k_lc, sizeof k_lc, k) != 0) continue;
            if (!strcmp(k_lc, local_lc)) { pk = v; break; }
        }
    }
    if (!pk || !json_is_string(pk)) {
        json_decref(root);
        return NH_NIP05_ERR_NAME;
    }
    const char *pks = json_string_value(pk);
    /* Down-case a hex string that happens to be upper-case (some
     * issuers store 0xAA…), but refuse anything non-hex. */
    char pk_lc[65];
    if (!pks || strlen(pks) != 64) {
        json_decref(root);
        return NH_NIP05_ERR_NAME;
    }
    for (int i = 0; i < 64; i++) {
        unsigned char c = (unsigned char)pks[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        pk_lc[i] = (char)c;
    }
    pk_lc[64] = '\0';
    if (!is_lc_hex64(pk_lc)) {
        json_decref(root);
        return NH_NIP05_ERR_NAME;
    }
    memcpy(result_out->pubkey_hex, pk_lc, 65);

    /* relays[<pubkey>] is optional. NIP-05 §"nostr.json" spells out
     * that the key is the *pubkey*, not the local name. Match
     * case-insensitively; ignore anything that isn't a string
     * ws(s):// URL of reasonable length. */
    json_t *relays = json_object_get(root, "relays");
    if (relays && json_is_object(relays)) {
        json_t *arr = json_object_get(relays, result_out->pubkey_hex);
        if (!arr) {
            /* Fall back to a case-insensitive scan. */
            const char *k;
            json_t *v;
            json_object_foreach(relays, k, v) {
                if (strlen(k) != 64) continue;
                char k_lc[65];
                for (int i = 0; i < 64; i++) {
                    unsigned char c = (unsigned char)k[i];
                    if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
                    k_lc[i] = (char)c;
                }
                k_lc[64] = '\0';
                if (!strcmp(k_lc, result_out->pubkey_hex)) { arr = v; break; }
            }
        }
        if (arr && json_is_array(arr)) {
            size_t idx;
            json_t *v;
            json_array_foreach(arr, idx, v) {
                if (result_out->relays_count >= NH_NIP05_RELAY_HINTS_MAX) break;
                if (!json_is_string(v)) continue;
                const char *rs = json_string_value(v);
                if (!rs) continue;
                size_t rlen = strlen(rs);
                if (rlen == 0 || rlen > NH_NIP05_RELAY_URL_MAX) continue;
                if (strncmp(rs, "ws://", 5) != 0 &&
                    strncmp(rs, "wss://", 6) != 0) continue;
                memcpy(result_out->relays[result_out->relays_count], rs,
                       rlen + 1);
                result_out->relays_count++;
            }
        }
    }

    json_decref(root);
    return NH_NIP05_OK;
}
