#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#include "nostr/nip19/nip19.h"
#include <nostr-utils.h> /* nostr_hex2bin */

/* forward decls */
static int append_tlv(uint8_t **buf, size_t *len, size_t *cap, uint8_t t, const uint8_t *v, size_t l);

/* Helpers */
static int append_bytes(uint8_t **buf, size_t *len, size_t *cap, const uint8_t *data, size_t n) {
    if (*len + n > *cap) {
        size_t ncap = (*cap ? *cap : 64);
        while (*len + n > ncap) ncap *= 2;
        uint8_t *tmp = (uint8_t *)realloc(*buf, ncap);
        if (!tmp) return -1;
        *buf = tmp; *cap = ncap;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    return 0;
}

/* nevent: T=0 (32-byte event id), T=1 (relay), T=2 (author 32-byte), T=3 (kind uint32 BE) */
int nostr_nip19_encode_nevent(const NostrEventPointer *e, char **out_bech) {
    if (!e || !e->id || !out_bech) return -1;
    *out_bech = NULL;
    if (strlen(e->id) != 64) return -1;
    uint8_t id[32]; if (!nostr_hex2bin(id, e->id, sizeof id)) return -1;
    uint8_t *tlv = NULL; size_t len = 0, cap = 0;
    if (append_tlv(&tlv, &len, &cap, 0, id, sizeof id) != 0) { free(tlv); return -1; }
    if (e->author && strlen(e->author) == 64) {
        uint8_t au[32]; if (!nostr_hex2bin(au, e->author, sizeof au)) { free(tlv); return -1; }
        if (append_tlv(&tlv, &len, &cap, 2, au, sizeof au) != 0) { free(tlv); return -1; }
    }
    if (e->kind > 0) {
        uint8_t k[4] = { (uint8_t)((e->kind >> 24) & 0xff), (uint8_t)((e->kind >> 16) & 0xff), (uint8_t)((e->kind >> 8) & 0xff), (uint8_t)(e->kind & 0xff) };
        if (append_tlv(&tlv, &len, &cap, 3, k, 4) != 0) { free(tlv); return -1; }
    }
    for (size_t i = 0; i < e->relays_count; ++i) {
        const char *url = e->relays ? e->relays[i] : NULL; if (!url) continue; size_t l = strlen(url); if (l > 255) { free(tlv); return -1; }
        if (append_tlv(&tlv, &len, &cap, 1, (const uint8_t *)url, l) != 0) { free(tlv); return -1; }
    }
    int rc = nostr_nip19_encode_tlv("nevent", tlv, len, out_bech);
    free(tlv); return rc;
}

int nostr_nip19_decode_nevent(const char *bech, NostrEventPointer **out_e) {
    if (!bech || !out_e) return -1; *out_e = NULL;
    char *hrp = NULL; uint8_t *tlv = NULL; size_t tlv_len = 0;
    if (nostr_nip19_decode_tlv(bech, &hrp, &tlv, &tlv_len) != 0) return -1;
    if (strcmp(hrp, "nevent") != 0) { free(hrp); free(tlv); return -1; }
    free(hrp);
    NostrEventPointer *e = nostr_event_pointer_new(); if (!e) { free(tlv); return -1; }
    int rc = -1; size_t i = 0;
    while (i + 2 <= tlv_len) {
        uint8_t t = tlv[i++], l = tlv[i++]; if (i + l > tlv_len) { goto done; }
        const uint8_t *v = tlv + i; i += l;
        if (t == 0) {
            if (l != 32) goto done;
            static const char hexd[17] = "0123456789abcdef"; char *hex = (char *)malloc(65); if (!hex) goto done;
            for (size_t k = 0; k < 32; ++k) { hex[2*k] = hexd[(v[k] >> 4) & 0xF]; hex[2*k+1] = hexd[v[k] & 0xF]; }
            hex[64] = '\0'; free(e->id); e->id = hex;
        } else if (t == 1) {
            char *s = (char *)malloc(l + 1); if (!s) goto done; memcpy(s, v, l); s[l] = '\0';
            char **nr = (char **)realloc(e->relays, (e->relays_count + 1) * sizeof(char *)); if (!nr) { free(s); goto done; }
            e->relays = nr; e->relays[e->relays_count++] = s;
        } else if (t == 2) {
            if (l != 32) goto done;
            static const char hexd[17] = "0123456789abcdef"; char *hex = (char *)malloc(65); if (!hex) goto done;
            for (size_t k = 0; k < 32; ++k) { hex[2*k] = hexd[(v[k] >> 4) & 0xF]; hex[2*k+1] = hexd[v[k] & 0xF]; }
            hex[64] = '\0'; free(e->author); e->author = hex;
        } else if (t == 3) {
            if (l != 4) goto done; e->kind = (int)((v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3]);
        } else {
            /* ignore unknown */
        }
    }
    *out_e = e; e = NULL; rc = 0;
done:
    free(tlv); if (e) nostr_event_pointer_free(e); return rc;
}

/* naddr: T=0 (identifier string), T=1 (relay), T=2 (author 32-byte), T=3 (kind uint32 BE) */
int nostr_nip19_encode_naddr(const NostrEntityPointer *a, char **out_bech) {
    if (!a || !a->identifier || !a->public_key || a->kind <= 0 || !out_bech) return -1;
    *out_bech = NULL;
    size_t idlen = strlen(a->identifier); if (idlen == 0 || idlen > 255) return -1;
    if (strlen(a->public_key) != 64) return -1;
    uint8_t au[32]; if (!nostr_hex2bin(au, a->public_key, sizeof au)) return -1;
    uint8_t k[4] = { (uint8_t)((a->kind >> 24) & 0xff), (uint8_t)((a->kind >> 16) & 0xff), (uint8_t)((a->kind >> 8) & 0xff), (uint8_t)(a->kind & 0xff) };
    uint8_t *tlv = NULL; size_t len = 0, cap = 0;
    if (append_tlv(&tlv, &len, &cap, 0, (const uint8_t *)a->identifier, idlen) != 0) { free(tlv); return -1; }
    if (append_tlv(&tlv, &len, &cap, 2, au, sizeof au) != 0) { free(tlv); return -1; }
    if (append_tlv(&tlv, &len, &cap, 3, k, 4) != 0) { free(tlv); return -1; }
    for (size_t i = 0; i < a->relays_count; ++i) {
        const char *url = a->relays ? a->relays[i] : NULL; if (!url) continue; size_t l = strlen(url); if (l > 255) { free(tlv); return -1; }
        if (append_tlv(&tlv, &len, &cap, 1, (const uint8_t *)url, l) != 0) { free(tlv); return -1; }
    }
    int rc = nostr_nip19_encode_tlv("naddr", tlv, len, out_bech);
    free(tlv); return rc;
}

int nostr_nip19_decode_naddr(const char *bech, NostrEntityPointer **out_a) {
    if (!bech || !out_a) return -1; *out_a = NULL;
    char *hrp = NULL; uint8_t *tlv = NULL; size_t tlv_len = 0;
    if (nostr_nip19_decode_tlv(bech, &hrp, &tlv, &tlv_len) != 0) return -1;
    if (strcmp(hrp, "naddr") != 0) { free(hrp); free(tlv); return -1; }
    free(hrp);
    NostrEntityPointer *a = nostr_entity_pointer_new(); if (!a) { free(tlv); return -1; }
    int rc = -1; size_t i = 0; int have_id = 0, have_author = 0, have_kind = 0;
    while (i + 2 <= tlv_len) {
        uint8_t t = tlv[i++], l = tlv[i++]; if (i + l > tlv_len) { goto done; }
        const uint8_t *v = tlv + i; i += l;
        if (t == 0) {
            char *s = (char *)malloc(l + 1); if (!s) goto done; memcpy(s, v, l); s[l] = '\0';
            free(a->identifier); a->identifier = s; have_id = 1;
        } else if (t == 1) {
            char *s = (char *)malloc(l + 1); if (!s) goto done; memcpy(s, v, l); s[l] = '\0';
            char **nr = (char **)realloc(a->relays, (a->relays_count + 1) * sizeof(char *)); if (!nr) { free(s); goto done; }
            a->relays = nr; a->relays[a->relays_count++] = s;
        } else if (t == 2) {
            if (l != 32) goto done; static const char hexd[17] = "0123456789abcdef"; char *hex = (char *)malloc(65); if (!hex) goto done;
            for (size_t k = 0; k < 32; ++k) { hex[2*k] = hexd[(v[k] >> 4) & 0xF]; hex[2*k+1] = hexd[v[k] & 0xF]; }
            hex[64] = '\0'; free(a->public_key); a->public_key = hex; have_author = 1;
        } else if (t == 3) {
            if (l != 4) goto done; a->kind = (int)((v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3]); have_kind = 1;
        } else {
            /* ignore */
        }
    }
    if (!(have_id && have_author && have_kind)) goto done;
    *out_a = a; a = NULL; rc = 0;
done:
    free(tlv); if (a) nostr_entity_pointer_free(a); return rc;
}

/* nrelay: one or more T=1 relay strings */
int nostr_nip19_encode_nrelay_multi(const char *const *relays, size_t relay_count, char **out_bech) {
    if (!relays || relay_count == 0 || !out_bech) return -1; *out_bech = NULL;
    uint8_t *buf = NULL; size_t len = 0, cap = 0;
    for (size_t i = 0; i < relay_count; ++i) {
        const char *r = relays[i]; if (!r) continue; size_t l = strlen(r);
        if (l == 0 || l > 255) { free(buf); return -1; }
        if (append_tlv(&buf, &len, &cap, 1, (const uint8_t *)r, l) != 0) { free(buf); return -1; }
    }
    if (len == 0) { free(buf); return -1; }
    int rc = nostr_nip19_encode_tlv("nrelay", buf, len, out_bech);
    free(buf);
    return rc;
}

int nostr_nip19_encode_nrelay(const char *relay_url, char **out_bech) {
    if (!relay_url || !out_bech) return -1; *out_bech = NULL;
    const char *arr[1] = { relay_url };
    return nostr_nip19_encode_nrelay_multi(arr, 1, out_bech);
}

int nostr_nip19_decode_nrelay(const char *bech, char ***out_relays, size_t *out_count) {
    if (!bech || !out_relays) return -1; *out_relays = NULL; if (out_count) *out_count = 0;
    char *hrp = NULL; uint8_t *tlv = NULL; size_t tlv_len = 0;
    if (nostr_nip19_decode_tlv(bech, &hrp, &tlv, &tlv_len) != 0) return -1;
    if (strcmp(hrp, "nrelay") != 0) { free(hrp); free(tlv); return -1; }
    free(hrp);
    size_t i = 0, cnt = 0; char **arr = NULL;
    while (i + 2 <= tlv_len) {
        uint8_t t = tlv[i++], l = tlv[i++]; if (i + l > tlv_len) { goto fail; }
        const uint8_t *v = tlv + i; i += l;
        if (t == 1) {
            char *s = (char *)malloc(l + 1); if (!s) goto fail; memcpy(s, v, l); s[l] = '\0';
            char **nr = (char **)realloc(arr, (cnt + 1) * sizeof(char *)); if (!nr) { free(s); goto fail; }
            arr = nr; arr[cnt++] = s;
        }
    }
    *out_relays = arr; if (out_count) *out_count = cnt; free(tlv); return 0;
fail:
    if (arr) { for (size_t j = 0; j < cnt; ++j) free(arr[j]); free(arr); }
    free(tlv); return -1;
}

static int append_tlv(uint8_t **buf, size_t *len, size_t *cap, uint8_t t, const uint8_t *v, size_t l) {
    uint8_t hdr[2] = { t, (uint8_t)l };
    if (l > 255) return -1;
    if (append_bytes(buf, len, cap, hdr, 2) != 0) return -1;
    if (append_bytes(buf, len, cap, v, l) != 0) return -1;
    return 0;
}

/* Generic TLV */
int nostr_nip19_encode_tlv(const char *hrp, const uint8_t *tlv, size_t tlv_len, char **out_bech) {
    if (!hrp || !tlv || !out_bech) return -1;
    *out_bech = NULL;
    uint8_t *data5 = NULL; size_t data5_len = 0;
    if (nostr_b32_to_5bit(tlv, tlv_len, &data5, &data5_len) != 0) return -1;
    int rc = nostr_b32_encode(hrp, data5, data5_len, out_bech);
    free(data5);
    return rc;
}

int nostr_nip19_decode_tlv(const char *bech, char **out_hrp, uint8_t **out_tlv, size_t *out_tlv_len) {
    if (!bech || !out_hrp || !out_tlv) return -1;
    *out_hrp = NULL; *out_tlv = NULL; if (out_tlv_len) *out_tlv_len = 0;
    char *hrp = NULL; uint8_t *data5 = NULL; size_t data5_len = 0;
    if (nostr_b32_decode(bech, &hrp, &data5, &data5_len) != 0) return -1;
    uint8_t *data8 = NULL; size_t data8_len = 0;
    int rc = nostr_b32_to_8bit(data5, data5_len, &data8, &data8_len);
    free(data5);
    if (rc != 0) { free(hrp); return -1; }
    *out_hrp = hrp; *out_tlv = data8; if (out_tlv_len) *out_tlv_len = data8_len;
    return 0;
}

/* nprofile: T=0 (32-byte pubkey), T=1 (relay, repeatable) */
int nostr_nip19_encode_nprofile(const NostrProfilePointer *p, char **out_bech) {
    if (!p || !p->public_key || !out_bech) return -1;
    *out_bech = NULL;
    uint8_t pk[32];
    if (strlen(p->public_key) != 64) return -1;
    if (!nostr_hex2bin(pk, p->public_key, sizeof pk)) return -1;
    uint8_t *tlv = NULL; size_t len = 0, cap = 0;
    if (append_tlv(&tlv, &len, &cap, 0, pk, sizeof pk) != 0) { free(tlv); return -1; }
    for (size_t i = 0; i < p->relays_count; ++i) {
        const char *url = p->relays ? p->relays[i] : NULL;
        if (!url) continue;
        size_t l = strlen(url);
        if (l > 255) { free(tlv); return -1; }
        if (append_tlv(&tlv, &len, &cap, 1, (const uint8_t *)url, l) != 0) { free(tlv); return -1; }
    }
    int rc = nostr_nip19_encode_tlv("nprofile", tlv, len, out_bech);
    free(tlv);
    return rc;
}

int nostr_nip19_decode_nprofile(const char *bech, NostrProfilePointer **out_p) {
    if (!bech || !out_p) return -1;
    *out_p = NULL;
    char *hrp = NULL; uint8_t *tlv = NULL; size_t tlv_len = 0;
    if (nostr_nip19_decode_tlv(bech, &hrp, &tlv, &tlv_len) != 0) return -1;
    int rc = -1;
    if (strcmp(hrp, "nprofile") != 0) { free(hrp); free(tlv); return -1; }
    free(hrp);
    NostrProfilePointer *p = nostr_profile_pointer_new();
    if (!p) { free(tlv); return -1; }
    size_t i = 0;
    while (i + 2 <= tlv_len) {
        uint8_t t = tlv[i++]; uint8_t l = tlv[i++];
        if (i + l > tlv_len) { rc = -1; goto done; }
        const uint8_t *v = tlv + i; i += l;
        if (t == 0) {
            if (l != 32) { rc = -1; goto done; }
            /* hex-encode 32 bytes */
            static const char hexd[17] = "0123456789abcdef";
            char *hex = (char *)malloc(65);
            if (!hex) { rc = -1; goto done; }
            for (size_t k = 0; k < 32; ++k) {
                hex[2*k] = hexd[(v[k] >> 4) & 0xF];
                hex[2*k+1] = hexd[v[k] & 0xF];
            }
            hex[64] = '\0';
            free(p->public_key); p->public_key = hex;
        } else if (t == 1) {
            char *s = (char *)malloc(l + 1);
            if (!s) { rc = -1; goto done; }
            memcpy(s, v, l); s[l] = '\0';
            char **nr = (char **)realloc(p->relays, (p->relays_count + 1) * sizeof(char *));
            if (!nr) { free(s); rc = -1; goto done; }
            p->relays = nr; p->relays[p->relays_count++] = s;
        } else {
            /* ignore unknown tlvs per spec */
        }
    }
    *out_p = p; p = NULL; rc = 0;
 done:
    free(tlv);
    if (p) nostr_profile_pointer_free(p);
    return rc;
}
