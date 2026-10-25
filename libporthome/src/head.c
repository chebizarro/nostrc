/* SPDX-License-Identifier: MIT */
#include <porthome/head.h>
#include "crypto-internal.h"
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr/nip44/nip44.h>
#include <openssl/crypto.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int head_valid(const porthome_head *h) {
    if (!h || !h->epoch || h->written_at > INT64_MAX ||
        h->has_ref_state < 0 || h->has_ref_state > 1 ||
        h->has_key_override < 0 || h->has_key_override > 1 ||
        (h->parents_len && !h->parents)) return PORTHOME_ERR_ARG;
    if (h->parents_len > PORTHOME_HEAD_MAX_PARENTS) return PORTHOME_ERR_TOO_LARGE;
    return PORTHOME_OK;
}
static int append(char *s, size_t cap, size_t *used, const char *fmt, ...) {
    va_list args; va_start(args,fmt);
    int n = vsnprintf(s+*used,cap-*used,fmt,args);
    va_end(args);
    if (n < 0 || (size_t)n >= cap-*used) return -1;
    *used += (size_t)n; return 0;
}
int porthome_head_encode(const porthome_head *h, uint8_t **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!out || !out_len) return PORTHOME_ERR_ARG;
    int rc = head_valid(h);
    if (rc) return rc;
    size_t cap = 640 + h->parents_len * 224, used = 0;
    char *s = malloc(cap);
    if (!s) return PORTHOME_ERR_OOM;
    char vault[65], root[65], ref[65];
    porthome_hex64(h->vault_id,vault); porthome_hex64(h->root_manifest,root);
    if (append(s,cap,&used,"{\"vault_id\":\"%s\",\"epoch\":%" PRIu64 ",\"root_manifest\":\"%s\"",vault,h->epoch,root)) goto fail;
    if (h->has_ref_state) {
        porthome_hex64(h->ref_state,ref);
        if (append(s,cap,&used,",\"ref_state\":\"%s\"",ref)) goto fail;
    }
    if (h->has_key_override) {
        char override[65]; porthome_hex64(h->key_override,override);
        int failed = append(s,cap,&used,",\"key_override\":\"%s\"",override);
        OPENSSL_cleanse(override,sizeof(override));
        if (failed) goto fail;
    }
    if (append(s,cap,&used,",\"seq\":%" PRIu64 ",\"parents\":[",h->seq)) goto fail;
    for (size_t i = 0; i < h->parents_len; ++i) {
        const porthome_head_parent *p = &h->parents[i];
        char writer[65]; porthome_hex64(p->writer,writer); porthome_hex64(p->root_manifest,root);
        if (append(s,cap,&used,"%s{\"writer\":\"%s\",\"seq\":%" PRIu64 ",\"root_manifest\":\"%s\"}",i ? "," : "",writer,p->seq,root)) goto fail;
    }
    if (append(s,cap,&used,"],\"grants_rev\":%" PRIu64 ",\"written_at\":%" PRIu64 "}",h->grants_rev,h->written_at)) goto fail;
    if (used > PORTHOME_HEAD_MAX_JSON) goto fail;
    *out = (uint8_t *)s; *out_len = used; return PORTHOME_OK;
fail:
    OPENSSL_cleanse(s,cap); free(s); return PORTHOME_ERR_TOO_LARGE;
}

/* Bounded, schema-specific JSON reader. The only strings in this schema are
 * ASCII field names and lowercase hex; escaped representations are refused.
 * No generic JSON AST or independent JSON dependency is needed. */
typedef struct { const uint8_t *p, *end; } reader;
static void ws(reader *r) {
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\r' || *r->p == '\n')) ++r->p;
}
static int take(reader *r, uint8_t c) {
    ws(r);
    if (r->p == r->end || *r->p != c) return -1;
    ++r->p; return 0;
}
static int field(reader *r, const char *const *names, size_t count) {
    if (take(r,'"')) return -1;
    const uint8_t *start = r->p;
    while (r->p < r->end && *r->p != '"') {
        if (*r->p < 0x20 || *r->p == '\\') return -1;
        ++r->p;
    }
    if (r->p == r->end) return -1;
    size_t len = (size_t)(r->p++ - start);
    if (take(r,':')) return -1;
    for (size_t i = 0; i < count; ++i)
        if (strlen(names[i]) == len && !memcmp(start,names[i],len)) return (int)i;
    return -1;
}
static int uint_read(reader *r, uint64_t *out) {
    ws(r);
    if (r->p == r->end || *r->p < '0' || *r->p > '9') return -1;
    int zero = *r->p == '0'; size_t digits = 0; uint64_t value = 0;
    while (r->p < r->end && *r->p >= '0' && *r->p <= '9') {
        unsigned digit = *r->p++ - '0';
        if ((zero && digits) || value > (UINT64_MAX-digit)/10) return -1;
        value = value*10+digit; ++digits;
    }
    *out = value; return 0;
}
static int hex_read(reader *r, uint8_t out[32]) {
    if (take(r,'"') || r->end-r->p < 65) return -1;
    char hex[65];
    for (size_t i = 0; i < 64; ++i) {
        uint8_t c = r->p[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { OPENSSL_cleanse(hex,sizeof(hex)); return -1; }
        hex[i] = (char)c;
    }
    hex[64] = 0;
    r->p += 64;
    int rc = *r->p++ == '"' ? porthome_from_hex64(hex,out) : -1;
    OPENSSL_cleanse(hex,sizeof(hex));
    return rc;
}
static int parent_read(reader *r, porthome_head_parent *p) {
    static const char *const names[] = {"writer","seq","root_manifest"};
    if (take(r,'{')) return -1;
    unsigned seen = 0;
    for (;;) {
        int f = field(r,names,3);
        if (f < 0 || (seen & (1u << f))) return -1;
        seen |= 1u << f;
        if ((f == 0 && hex_read(r,p->writer)) || (f == 1 && uint_read(r,&p->seq)) ||
            (f == 2 && hex_read(r,p->root_manifest))) return -1;
        ws(r);
        if (r->p < r->end && *r->p == '}') { ++r->p; return seen == 7 ? 0 : -1; }
        if (take(r,',')) return -1;
    }
}
static int parents_read(reader *r, porthome_head *h) {
    if (take(r,'[')) return -1;
    ws(r);
    if (r->p < r->end && *r->p == ']') { ++r->p; return 0; }
    h->parents = calloc(PORTHOME_HEAD_MAX_PARENTS,sizeof(*h->parents));
    if (!h->parents) return PORTHOME_ERR_OOM;
    for (;;) {
        if (h->parents_len == PORTHOME_HEAD_MAX_PARENTS || parent_read(r,&h->parents[h->parents_len])) return -1;
        ++h->parents_len;
        ws(r);
        if (r->p < r->end && *r->p == ']') { ++r->p; return 0; }
        if (take(r,',')) return -1;
    }
}
void porthome_head_dispose(porthome_head *h) {
    if (!h) return;
    free(h->parents); OPENSSL_cleanse(h,sizeof(*h));
}
int porthome_head_decode(const uint8_t *json, size_t len, porthome_head **out) {
    if (out) *out = NULL;
    if (!out || !json) return PORTHOME_ERR_ARG;
    if (len > PORTHOME_HEAD_MAX_JSON) return PORTHOME_ERR_TOO_LARGE;
    if (memchr(json,0,len)) return PORTHOME_ERR_ARG;
    reader r = {json,json+len};
    if (take(&r,'{')) return PORTHOME_ERR_ARG;
    porthome_head *h = calloc(1,sizeof(*h));
    if (!h) return PORTHOME_ERR_OOM;
    static const char *const names[] = {"vault_id","epoch","root_manifest","ref_state","seq","parents","grants_rev","written_at","key_override"};
    unsigned seen = 0;
    int rc = PORTHOME_ERR_ARG;
    for (;;) {
        int f = field(&r,names,9);
        if (f < 0 || (seen & (1u << f))) goto fail;
        seen |= 1u << f;
        int bad = 0;
        switch (f) {
        case 0: bad = hex_read(&r,h->vault_id); break;
        case 1: bad = uint_read(&r,&h->epoch); break;
        case 2: bad = hex_read(&r,h->root_manifest); break;
        case 3: h->has_ref_state = 1; bad = hex_read(&r,h->ref_state); break;
        case 4: bad = uint_read(&r,&h->seq); break;
        case 5: bad = parents_read(&r,h); break;
        case 6: bad = uint_read(&r,&h->grants_rev); break;
        case 7: bad = uint_read(&r,&h->written_at); break;
        case 8: h->has_key_override = 1; bad = hex_read(&r,h->key_override); break;
        }
        if (bad) { if (bad == PORTHOME_ERR_OOM) rc = PORTHOME_ERR_OOM; goto fail; }
        ws(&r);
        if (r.p < r.end && *r.p == '}') { ++r.p; break; }
        if (take(&r,',')) goto fail;
    }
    ws(&r);
    if (r.p != r.end || (seen & 0xf7) != 0xf7 || head_valid(h)) goto fail;
    *out = h; return PORTHOME_OK;
fail:
    porthome_head_dispose(h); free(h); return rc;
}
int porthome_head_d_tag(const uint8_t vault_id[32], char out[PORTHOME_HEAD_D_LEN]) {
    if (!vault_id || !out) return PORTHOME_ERR_ARG;
    memcpy(out,"porthome/v2/",12); porthome_hex64(vault_id,out+12); return PORTHOME_OK;
}
static int convkey(const uint8_t node_key[32], uint8_t out[32]) {
    uint8_t head_key[32], pub[32]; char secret_hex[65];
    int rc = porthome_head_key_v2(node_key,head_key);
    char *pub_hex = NULL;
    if (rc) goto done;
    porthome_hex64(head_key,secret_hex);
    /* libnostr's public key function derives and serializes BIP340 x-only. */
    pub_hex = nostr_key_get_public(secret_hex);
    if (!pub_hex || porthome_from_hex64(pub_hex,pub) || nostr_nip44_convkey(head_key,pub,out)) rc = PORTHOME_ERR_CRYPTO;
done:
    OPENSSL_cleanse(secret_hex,sizeof(secret_hex)); OPENSSL_cleanse(head_key,sizeof(head_key));
    free(pub_hex);
    if (rc) OPENSSL_cleanse(out,32);
    return rc;
}
int porthome_head_seal(const porthome_head *h, const uint8_t node_key[32], char **out) {
    if (out) *out = NULL;
    if (!node_key || !out) return PORTHOME_ERR_ARG;
    uint8_t *json = NULL, key[32]; size_t len = 0;
    int rc = porthome_head_encode(h,&json,&len);
    if (!rc && h->has_key_override && CRYPTO_memcmp(h->key_override,node_key,32)) rc = PORTHOME_ERR_ARG;
    if (!rc) rc = convkey(node_key,key);
    if (!rc && nostr_nip44_encrypt_v2_with_convkey(key,json,len,out)) rc = PORTHOME_ERR_CRYPTO;
    OPENSSL_cleanse(key,sizeof(key));
    if (json) OPENSSL_cleanse(json,len);
    free(json); return rc;
}
int porthome_head_open(const porthome_ring_t *ring, uint64_t epoch, const char *payload, porthome_head **out) {
    if (out) *out = NULL;
    if (!out || !payload) return PORTHOME_ERR_ARG;
    const uint8_t *node_key;
    int rc = porthome_ring_key(ring,epoch,&node_key);
    if (rc) return rc;
    if (strnlen(payload,PORTHOME_HEAD_MAX_PAYLOAD+1) > PORTHOME_HEAD_MAX_PAYLOAD) return PORTHOME_ERR_TOO_LARGE;
    uint8_t key[32], *json = NULL; size_t len = 0;
    rc = convkey(node_key,key);
    if (!rc && nostr_nip44_decrypt_v2_with_convkey(key,payload,&json,&len)) rc = PORTHOME_ERR_CRYPTO;
    if (!rc) rc = porthome_head_decode(json,len,out);
    if (!rc && ((*out)->epoch != epoch || ((*out)->has_key_override && CRYPTO_memcmp((*out)->key_override,node_key,32)))) { porthome_head_dispose(*out); free(*out); *out = NULL; rc = PORTHOME_ERR_EPOCH; }
    OPENSSL_cleanse(key,sizeof(key));
    if (json) OPENSSL_cleanse(json,len);
    free(json); return rc;
}
int porthome_head_event_new(const porthome_head *h, const uint8_t node_key[32],
    const uint8_t writer[32], NostrEvent **out) {
    if (out) *out = NULL;
    if (!writer || !out) return PORTHOME_ERR_ARG;
    char *payload = NULL;
    int rc = porthome_head_seal(h,node_key,&payload);
    if (rc) return rc;
    NostrEvent *event = nostr_event_new();
    if (!event) { free(payload); return PORTHOME_ERR_OOM; }
    char pub[65], d[PORTHOME_HEAD_D_LEN];
    porthome_hex64(writer,pub); porthome_head_d_tag(h->vault_id,d);
    nostr_event_set_pubkey(event,pub); nostr_event_set_kind(event,PORTHOME_HEAD_KIND);
    nostr_event_set_created_at(event,(int64_t)h->written_at); nostr_event_set_content(event,payload);
    free(payload);
    NostrTag *tag = nostr_tag_new("d",d,NULL);
    if (!tag) { nostr_event_free(event); return PORTHOME_ERR_OOM; }
    nostr_tags_append(event->tags,tag);
    if (!event->pubkey || !event->content || nostr_tags_size(event->tags) != 1) {
        if (!nostr_tags_size(event->tags)) nostr_tag_free(tag);
        nostr_event_free(event); return PORTHOME_ERR_OOM;
    }
    *out = event; return PORTHOME_OK;
}
int porthome_head_event_open(const NostrEvent *event, const uint8_t vault_id[32],
    const porthome_ring_t *ring, uint64_t epoch, porthome_head **out) {
    if (out) *out = NULL;
    if (!event || !vault_id || !out) return PORTHOME_ERR_ARG;
    if (event->kind != PORTHOME_HEAD_KIND || !event->content ||
        strnlen(event->content,PORTHOME_HEAD_MAX_PAYLOAD+1) > PORTHOME_HEAD_MAX_PAYLOAD) return PORTHOME_ERR_ARG;
    char expected[PORTHOME_HEAD_D_LEN]; porthome_head_d_tag(vault_id,expected);
    unsigned found = 0;
    for (size_t i = 0; i < nostr_tags_size(event->tags); ++i) {
        NostrTag *tag = nostr_tags_get(event->tags,i);
        const char *name = nostr_tag_get_key(tag);
        if (!name || strcmp(name,"d")) continue;
        const char *value = nostr_tag_get_value(tag);
        if (++found != 1 || nostr_tag_size(tag) != 2 || !value || strcmp(value,expected)) return PORTHOME_ERR_ARG;
    }
    if (found != 1 || nostr_event_validate(event,NULL) != NOSTR_EVENT_VALIDATION_OK) return PORTHOME_ERR_CRYPTO;
    int rc = porthome_head_open(ring,epoch,event->content,out);
    if (!rc && memcmp((*out)->vault_id,vault_id,32)) {
        porthome_head_dispose(*out); free(*out); *out = NULL; rc = PORTHOME_ERR_ARG;
    }
    return rc;
}
