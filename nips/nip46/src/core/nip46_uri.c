/** Strict, bounded URI parsing for NIP-46 tokens. */
#include "nostr/nip46/nip46_uri.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define URI_MAX 16384u
#define RELAY_MAX 16u
#define RELAY_LEN_MAX 2048u
#define VALUE_LEN_MAX 4096u

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static int decode(const char *src, size_t max, char **out) {
    if (out) *out = NULL;
    if (!src || !out) return -1;
    size_t n = strlen(src), j = 0;
    if (!n || n > max * 3u) return -1;
    char *dst = malloc(n + 1u);
    if (!dst) return -1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '%') {
            if (i + 2u >= n) goto fail;
            int hi = hexval((unsigned char)src[i + 1u]);
            int lo = hexval((unsigned char)src[i + 2u]);
            if (hi < 0 || lo < 0) goto fail;
            c = (unsigned char)((hi << 4) | lo);
            i += 2u;
        }
        if (c < 0x20u || c == 0x7fu || j >= max) goto fail;
        dst[j++] = (char)c;
    }
    if (!j) goto fail;
    dst[j] = '\0';
    *out = dst;
    return 0;
fail:
    memset(dst, 0, j);
    free(dst);
    return -1;
}

static int valid_pubkey(const char *s) {
    if (!s) return 0;
    size_t n = strlen(s);
    if (n != 64 && n != 66 && n != 130) return 0;
    for (size_t i = 0; i < n; i++)
        if (!isxdigit((unsigned char)s[i])) return 0;
    return 1;
}

static void free_array(char **items, size_t n) {
    if (!items) return;
    for (size_t i = 0; i < n; i++) free(items[i]);
    free(items);
}

static void wipe_free(char **value) {
    if (!value || !*value) return;
    size_t n = strlen(*value);
    memset(*value, 0, n);
    free(*value);
    *value = NULL;
}

void nostr_nip46_uri_bunker_free(NostrNip46BunkerURI *u) {
    if (!u) return;
    free(u->remote_signer_pubkey_hex); u->remote_signer_pubkey_hex = NULL;
    free_array(u->relays, u->n_relays); u->relays = NULL; u->n_relays = 0;
    wipe_free(&u->secret);
}

void nostr_nip46_uri_connect_free(NostrNip46ConnectURI *u) {
    if (!u) return;
    free(u->client_pubkey_hex); u->client_pubkey_hex = NULL;
    free_array(u->relays, u->n_relays); u->relays = NULL; u->n_relays = 0;
    wipe_free(&u->secret);
    free(u->perms_csv); u->perms_csv = NULL;
    free(u->name); u->name = NULL;
    free(u->url); u->url = NULL;
    free(u->image); u->image = NULL;
}

static int parse_head(const char *uri, const char *scheme, char **pubkey,
                      const char **query) {
    if (pubkey) *pubkey = NULL;
    if (query) *query = NULL;
    if (!uri || !scheme || !pubkey || !query) return -1;
    size_t un = strlen(uri), sn = strlen(scheme);
    if (un <= sn || un > URI_MAX || strncmp(uri, scheme, sn)) return -1;
    const char *start = uri + sn;
    const char *q = strchr(start, '?');
    size_t pn = q ? (size_t)(q - start) : strlen(start);
    if (!pn || (q && !q[1])) return -1;
    char *pk = malloc(pn + 1u);
    if (!pk) return -1;
    memcpy(pk, start, pn); pk[pn] = '\0';
    if (!valid_pubkey(pk)) { free(pk); return -1; }
    *pubkey = pk;
    *query = q ? q + 1 : NULL;
    return 0;
}

static int segment(const char *s, size_t n, char **key, char **value) {
    *key = NULL; *value = NULL;
    const char *eq = memchr(s, '=', n);
    if (!eq || eq == s || eq == s + n - 1) return -1;
    size_t kn = (size_t)(eq - s), vn = n - kn - 1u;
    char *k = malloc(kn + 1u), *v = malloc(vn + 1u);
    if (!k || !v) { free(k); free(v); return -1; }
    memcpy(k, s, kn); k[kn] = '\0';
    memcpy(v, eq + 1, vn); v[vn] = '\0';
    *key = k; *value = v;
    return 0;
}

static int add_relay(char ***items, size_t *n, const char *encoded) {
    if (*n >= RELAY_MAX) return -1;
    char *relay = NULL;
    if (decode(encoded, RELAY_LEN_MAX, &relay)) return -1;
    for (size_t i = 0; i < *n; i++) {
        if (!strcmp((*items)[i], relay)) { free(relay); return -1; }
    }
    char **next = realloc(*items, (*n + 1u) * sizeof(*next));
    if (!next) { free(relay); return -1; }
    *items = next;
    (*items)[(*n)++] = relay;
    return 0;
}

static int singleton(char **field, int *seen, const char *encoded) {
    if (*seen) return -1;
    *seen = 1;
    return decode(encoded, VALUE_LEN_MAX, field);
}

int nostr_nip46_uri_parse_bunker(const char *uri, NostrNip46BunkerURI *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    const char *q = NULL;
    if (parse_head(uri, "bunker://", &out->remote_signer_pubkey_hex, &q)) return -1;
    if (!q) return 0;
    int secret_seen = 0;
    while (*q) {
        const char *amp = strchr(q, '&');
        size_t n = amp ? (size_t)(amp - q) : strlen(q);
        char *key = NULL, *value = NULL;
        if (segment(q, n, &key, &value)) goto fail;
        int rc = 0;
        if (!strcmp(key, "relay")) rc = add_relay(&out->relays, &out->n_relays, value);
        else if (!strcmp(key, "secret")) rc = singleton(&out->secret, &secret_seen, value);
        free(key); free(value);
        if (rc) goto fail;
        if (!amp) break;
        q = amp + 1;
        if (!*q) goto fail;
    }
    return 0;
fail:
    nostr_nip46_uri_bunker_free(out);
    return -1;
}

int nostr_nip46_uri_parse_connect(const char *uri, NostrNip46ConnectURI *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    const char *q = NULL;
    if (parse_head(uri, "nostrconnect://", &out->client_pubkey_hex, &q)) return -1;
    if (!q) return 0;
    int secret = 0, perms = 0, name = 0, url = 0, image = 0;
    while (*q) {
        const char *amp = strchr(q, '&');
        size_t n = amp ? (size_t)(amp - q) : strlen(q);
        char *key = NULL, *value = NULL;
        if (segment(q, n, &key, &value)) goto fail;
        int rc = 0;
        if (!strcmp(key, "relay")) rc = add_relay(&out->relays, &out->n_relays, value);
        else if (!strcmp(key, "secret")) rc = singleton(&out->secret, &secret, value);
        else if (!strcmp(key, "perms")) rc = singleton(&out->perms_csv, &perms, value);
        else if (!strcmp(key, "name")) rc = singleton(&out->name, &name, value);
        else if (!strcmp(key, "url")) rc = singleton(&out->url, &url, value);
        else if (!strcmp(key, "image")) rc = singleton(&out->image, &image, value);
        free(key); free(value);
        if (rc) goto fail;
        if (!amp) break;
        q = amp + 1;
        if (!*q) goto fail;
    }
    return 0;
fail:
    nostr_nip46_uri_connect_free(out);
    return -1;
}

/* nostrc-z1fb Phase 1: URI builder. RFC-3986 unreserved chars pass through
 * literally; everything else (including ':', '/', '?', '&', '=' and the
 * high bit) is percent-encoded so an embedded relay wss://host or a
 * comma-separated perms list never terminates the query prematurely. */
static int is_unreserved_uc(unsigned char c) {
    return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||
           c=='-'||c=='.'||c=='_'||c=='~';
}

static char *pct_encode(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    /* Worst case: every byte encodes to 3 chars. */
    char *out = (char *)malloc(n * 3u + 1u);
    if (!out) return NULL;
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (is_unreserved_uc(c)) {
            out[j++] = (char)c;
        } else {
            out[j++] = '%';
            out[j++] = hex[(c >> 4) & 0xF];
            out[j++] = hex[c & 0xF];
        }
    }
    out[j] = '\0';
    return out;
}

/* Append `piece` to `*buf` growing as needed. Returns 0/-1. */
static int append_str(char **buf, size_t *len, size_t *cap, const char *piece) {
    if (!piece) return 0;
    size_t pn = strlen(piece);
    if (*len + pn + 1u > *cap) {
        size_t ncap = (*cap ? *cap : 128u);
        while (ncap < *len + pn + 1u) ncap *= 2u;
        char *nb = (char *)realloc(*buf, ncap);
        if (!nb) return -1;
        *buf = nb;
        *cap = ncap;
    }
    memcpy(*buf + *len, piece, pn);
    *len += pn;
    (*buf)[*len] = '\0';
    return 0;
}

static int append_kv(char **buf, size_t *len, size_t *cap, int *first,
                     const char *key, const char *raw_value) {
    if (!raw_value) return 0;
    char *enc = pct_encode(raw_value);
    if (!enc) return -1;
    if (append_str(buf, len, cap, *first ? "?" : "&")) { free(enc); return -1; }
    *first = 0;
    if (append_str(buf, len, cap, key)) { free(enc); return -1; }
    if (append_str(buf, len, cap, "=")) { free(enc); return -1; }
    int rc = append_str(buf, len, cap, enc);
    /* enc may contain a percent-encoded secret; wipe before free. */
    memset(enc, 0, strlen(enc));
    free(enc);
    return rc;
}

int nostr_nip46_uri_build_connect(const NostrNip46ConnectURI *in, char **out_uri) {
    if (!in || !out_uri) return -1;
    *out_uri = NULL;
    if (!in->client_pubkey_hex) return -1;
    if (!valid_pubkey(in->client_pubkey_hex)) return -1;

    char *buf = NULL;
    size_t len = 0, cap = 0;
    if (append_str(&buf, &len, &cap, "nostrconnect://")) goto fail;
    if (append_str(&buf, &len, &cap, in->client_pubkey_hex)) goto fail;

    int first = 1;
    if (in->relays && in->n_relays) {
        for (size_t i = 0; i < in->n_relays; ++i) {
            if (!in->relays[i]) continue;
            if (append_kv(&buf, &len, &cap, &first, "relay", in->relays[i])) goto fail;
        }
    }
    if (in->secret && *in->secret) {
        if (append_kv(&buf, &len, &cap, &first, "secret", in->secret)) goto fail;
    }
    if (in->perms_csv && *in->perms_csv) {
        if (append_kv(&buf, &len, &cap, &first, "perms", in->perms_csv)) goto fail;
    }
    if (in->name && *in->name) {
        if (append_kv(&buf, &len, &cap, &first, "name", in->name)) goto fail;
    }
    if (in->url && *in->url) {
        if (append_kv(&buf, &len, &cap, &first, "url", in->url)) goto fail;
    }
    if (in->image && *in->image) {
        if (append_kv(&buf, &len, &cap, &first, "image", in->image)) goto fail;
    }

    *out_uri = buf;
    return 0;
fail:
    if (buf) { memset(buf, 0, len); free(buf); }
    return -1;
}
