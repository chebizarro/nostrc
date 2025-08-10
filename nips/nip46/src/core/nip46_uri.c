/**
 * URI parsing for NIP-46 tokens.
 * bunker://<rs-pubkey>?relay=...&secret=...
 * nostrconnect://<client-pubkey>?relay=...&secret=...&perms=...&name=...&url=...&image=...
 */

#include "nostr/nip46/nip46_uri.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static char *percent_decode(const char *s) {
    size_t n = strlen(s);
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '%' && i + 2 < n) {
            int h1 = hexval(s[i+1]);
            int h2 = hexval(s[i+2]);
            if (h1 >= 0 && h2 >= 0) {
                out[j++] = (char)((h1 << 4) | h2);
                i += 2;
                continue;
            }
        }
        out[j++] = s[i];
    }
    out[j] = '\0';
    return out;
}

static int is_hex_str(const char *s) {
    if (!s) return 0; size_t n = strlen(s);
    /* Accept 64 (32-byte), 66 (compressed pubkey), or 130 (uncompressed pubkey) hex */
    if (!(n == 64 || n == 66 || n == 130)) return 0;
    for (size_t i = 0; i < n; ++i) {
        if (!isxdigit((unsigned char)s[i])) return 0;
    }
    return 1;
}

static int add_relay(char ***arr, size_t *cnt, const char *val) {
    char *dec = percent_decode(val);
    if (!dec) return -1;
    char **tmp = (char **)realloc(*arr, (*cnt + 1) * sizeof(char *));
    if (!tmp) { free(dec); return -1; }
    *arr = tmp; (*arr)[*cnt] = dec; (*cnt)++;
    return 0;
}

static void free_str_array(char **arr, size_t cnt) {
    if (!arr) return;
    for (size_t i = 0; i < cnt; ++i) free(arr[i]);
    free(arr);
}

static int parse_query_kv(const char *q, char **key_out, char **val_out) {
    const char *eq = strchr(q, '=');
    if (!eq) return -1;
    size_t kl = (size_t)(eq - q);
    size_t vl = strlen(eq + 1);
    char *k = (char *)malloc(kl + 1);
    char *v = (char *)malloc(vl + 1);
    if (!k || !v) { free(k); free(v); return -1; }
    memcpy(k, q, kl); k[kl] = '\0';
    memcpy(v, eq + 1, vl + 1);
    *key_out = k; *val_out = v;
    return 0;
}

static void zero_and_free(char **p) {
    if (!p || !*p) return; size_t n = strlen(*p); memset(*p, 0, n); free(*p); *p = NULL;
}

int nostr_nip46_uri_parse_bunker(const char *uri, NostrNip46BunkerURI *out) {
    if (!uri || !out) return -1; memset(out, 0, sizeof(*out));
    const char *scheme = "bunker://";
    size_t sl = strlen(scheme);
    if (strncmp(uri, scheme, sl) != 0) return -1;
    const char *p = uri + sl;
    const char *q = strchr(p, '?');
    char *pub = NULL;
    if (q) {
        size_t pl = (size_t)(q - p); pub = (char *)malloc(pl + 1); if (!pub) return -1;
        memcpy(pub, p, pl); pub[pl] = '\0';
    } else {
        pub = strdup(p);
    }
    if (!pub || !is_hex_str(pub)) { free(pub); return -1; }
    out->remote_signer_pubkey_hex = pub;
    if (!q) return 0;
    // parse query
    const char *cur = q + 1;
    while (*cur) {
        const char *amp = strchr(cur, '&');
        char *kv = NULL; size_t seglen = 0;
        if (amp) { seglen = (size_t)(amp - cur); }
        else { seglen = strlen(cur); }
        kv = (char *)malloc(seglen + 1); if (!kv) return -1;
        memcpy(kv, cur, seglen); kv[seglen] = '\0';
        char *key = NULL, *val = NULL;
        if (parse_query_kv(kv, &key, &val) == 0) {
            if (strcmp(key, "relay") == 0) {
                (void)add_relay(&out->relays, &out->n_relays, val);
            } else if (strcmp(key, "secret") == 0) {
                char *dec = percent_decode(val);
                if (dec) { out->secret = dec; }
            } else {
                /* ignore unknown */
            }
        }
        free(key); free(val); free(kv);
        if (!amp) break; cur = amp + 1;
    }
    return 0;
}

int nostr_nip46_uri_parse_connect(const char *uri, NostrNip46ConnectURI *out) {
    if (!uri || !out) return -1; memset(out, 0, sizeof(*out));
    const char *scheme = "nostrconnect://";
    size_t sl = strlen(scheme);
    if (strncmp(uri, scheme, sl) != 0) return -1;
    const char *p = uri + sl;
    const char *q = strchr(p, '?');
    char *pub = NULL;
    if (q) { size_t pl = (size_t)(q - p); pub = (char *)malloc(pl + 1); if (!pub) return -1; memcpy(pub, p, pl); pub[pl] = '\0'; }
    else { pub = strdup(p); }
    if (!pub || !is_hex_str(pub)) { free(pub); return -1; }
    out->client_pubkey_hex = pub;
    if (!q) return 0;
    const char *cur = q + 1;
    while (*cur) {
        const char *amp = strchr(cur, '&'); size_t seglen = amp ? (size_t)(amp - cur) : strlen(cur);
        char *kv = (char *)malloc(seglen + 1); if (!kv) return -1;
        memcpy(kv, cur, seglen); kv[seglen] = '\0';
        char *key = NULL, *val = NULL;
        if (parse_query_kv(kv, &key, &val) == 0) {
            if (strcmp(key, "relay") == 0) {
                (void)add_relay(&out->relays, &out->n_relays, val);
            } else if (strcmp(key, "secret") == 0) {
                char *dec = percent_decode(val); if (dec) out->secret = dec;
            } else if (strcmp(key, "perms") == 0) {
                char *dec = percent_decode(val); if (dec) out->perms_csv = dec;
            } else if (strcmp(key, "name") == 0) {
                char *dec = percent_decode(val); if (dec) out->name = dec;
            } else if (strcmp(key, "url") == 0) {
                char *dec = percent_decode(val); if (dec) out->url = dec;
            } else if (strcmp(key, "image") == 0) {
                char *dec = percent_decode(val); if (dec) out->image = dec;
            }
        }
        free(key); free(val); free(kv);
        if (!amp) break; cur = amp + 1;
    }
    return 0;
}

void nostr_nip46_uri_bunker_free(NostrNip46BunkerURI *u) {
    if (!u) return; free(u->remote_signer_pubkey_hex); u->remote_signer_pubkey_hex = NULL;
    free_str_array(u->relays, u->n_relays); u->relays = NULL; u->n_relays = 0;
    zero_and_free(&u->secret);
}

void nostr_nip46_uri_connect_free(NostrNip46ConnectURI *u) {
    if (!u) return; free(u->client_pubkey_hex); u->client_pubkey_hex = NULL;
    free_str_array(u->relays, u->n_relays); u->relays = NULL; u->n_relays = 0;
    zero_and_free(&u->secret);
    if (u->perms_csv) { free(u->perms_csv); u->perms_csv = NULL; }
    if (u->name) { free(u->name); u->name = NULL; }
    if (u->url) { free(u->url); u->url = NULL; }
    if (u->image) { free(u->image); u->image = NULL; }
}
