/* SPDX-License-Identifier: MIT */
#include "internal.h"
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr/nip44/nip44.h>
#include <inttypes.h>
static int more(vj_reader *r, char end)
{
    vj_ws(r);
    if (r->p < r->end && *r->p == end) {
        ++r->p;
        return 0;
    }
    return vj_take(r, ',') ? -1 : 1;
}
static int hints(vj_reader *r, char ***relays, size_t *rn, char ***blossom, size_t *bn)
{
    static const char *const names[] = {"relays", "blossom"};
    if (vj_take(r, '{'))
        return VAULT_ERR_ARG;
    unsigned seen = 0;
    for (;;) {
        int f = vj_field(r, names, 2);
        if (f < 0 || (seen & (1u << f)))
            return VAULT_ERR_ARG;
        seen |= 1u << f;
        int rc = f ? vj_urls(r, blossom, bn, 0) : vj_urls(r, relays, rn, 1);
        if (rc)
            return rc;
        int next = more(r, '}');
        if (next < 0)
            return VAULT_ERR_ARG;
        if (!next)
            return seen == 3 ? 0 : VAULT_ERR_ARG;
    }
}
static int read_ring(vj_reader *r, vault_ring *ring)
{
    if (vj_take(r, '['))
        return VAULT_ERR_ARG;
    ring->entries = calloc(VAULT_MAX_EPOCHS, sizeof(*ring->entries));
    if (!ring->entries)
        return VAULT_ERR_OOM;
    for (;;) {
        if (ring->count == VAULT_MAX_EPOCHS || vj_take(r, '{'))
            return VAULT_ERR_ARG;
        porthome_ring_entry_t *e = &ring->entries[ring->count++];
        unsigned seen = 0;
        static const char *const names[] = {"epoch", "node_key"};
        for (;;) {
            int f = vj_field(r, names, 2);
            if (f < 0 || (seen & (1u << f)))
                return VAULT_ERR_ARG;
            seen |= 1u << f;
            int rc = f ? vj_hex(r, e->node_key) : vj_uint(r, &e->epoch);
            if (rc)
                return rc;
            int next = more(r, '}');
            if (next < 0)
                return VAULT_ERR_ARG;
            if (!next)
                break;
        }
        if (seen != 3)
            return VAULT_ERR_ARG;
        int next = more(r, ']');
        if (next < 0)
            return VAULT_ERR_ARG;
        if (!next)
            break;
    }
    porthome_ring_t view = vault_ring_view(ring);
    return vault_ring_valid(&view);
}
void vault_grant_free(vault_grant *g)
{
    if (!g)
        return;
    vault_ring_dispose(&g->ring);
    vj_urls_free(g->relays, g->relays_len);
    vj_urls_free(g->blossom, g->blossom_len);
    OPENSSL_cleanse(g, sizeof(*g));
    free(g);
}
int vault_grant_encode(const vault_grant *g, char **out)
{
    if (out)
        *out = NULL;
    if (!out || !g || g->issued_at > INT64_MAX ||
        (g->cap != VAULT_CAP_READ && g->cap != VAULT_CAP_WRITE))
        return VAULT_ERR_ARG;
    porthome_ring_t view = vault_ring_view(&g->ring);
    int rc = vault_ring_valid(&view);
    if (rc)
        return rc;
    char *s = malloc(65536);
    if (!s)
        return VAULT_ERR_OOM;
    size_t used = 0;
    char a[65], b[65], c[65], d[65];
    porthome_hex64(g->vault_id, a);
    porthome_hex64(g->node_id, b);
    porthome_hex64(g->owner, c);
    porthome_hex64(g->root_manifest, d);
    rc = vj_append(s, 65536, &used, "{\"vault_id\":\"%s\",\"node_id\":\"%s\",\"ring\":[", a, b);
    if (rc)
        goto done;
    for (size_t i = 0; i < g->ring.count; ++i) {
        char key[65];
        porthome_hex64(g->ring.entries[i].node_key, key);
        rc = vj_append(s, 65536, &used, "%s{\"epoch\":%" PRIu64 ",\"node_key\":\"%s\"}",
                       i ? "," : "", g->ring.entries[i].epoch, key);
        OPENSSL_cleanse(key, sizeof(key));
        if (rc)
            goto done;
    }
    rc = vj_append(
        s, 65536, &used,
        "],\"cap\":\"%s\",\"owner\":\"%s\",\"root_manifest\":\"%s\",\"hints\":{\"relays\":",
        g->cap == VAULT_CAP_READ ? "read" : "write", c, d);
    if (rc)
        goto done;
    rc = vj_write_urls(s, 65536, &used, g->relays, g->relays_len, 1);
    if (rc)
        goto done;
    rc = vj_append(s, 65536, &used, ",\"blossom\":");
    if (rc)
        goto done;
    rc = vj_write_urls(s, 65536, &used, g->blossom, g->blossom_len, 0);
    if (rc)
        goto done;
    rc = vj_append(s, 65536, &used, "},\"issued_at\":%" PRIu64 "}", g->issued_at);
done:
    if (rc) {
        OPENSSL_cleanse(s, 65536);
        free(s);
    } else
        *out = s;
    return rc;
}
int vault_grant_decode(const char *json, vault_grant **out)
{
    if (out)
        *out = NULL;
    if (!out)
        return VAULT_ERR_ARG;
    vj_reader r;
    int rc = vj_init(json, &r);
    if (rc)
        return rc;
    if (vj_take(&r, '{'))
        return VAULT_ERR_ARG;
    vault_grant *g = calloc(1, sizeof(*g));
    if (!g)
        return VAULT_ERR_OOM;
    unsigned seen = 0;
    static const char *const names[] = {"vault_id", "node_id",       "ring",  "cap",
                                        "owner",    "root_manifest", "hints", "issued_at"};
    for (;;) {
        int f = vj_field(&r, names, 8);
        rc = VAULT_ERR_ARG;
        if (f < 0 || (seen & (1u << f)))
            goto fail;
        seen |= 1u << f;
        switch (f) {
        case 0:
            rc = vj_hex(&r, g->vault_id);
            break;
        case 1:
            rc = vj_hex(&r, g->node_id);
            break;
        case 2:
            rc = read_ring(&r, &g->ring);
            break;
        case 3: {
            char cap[8];
            rc = vj_string(&r, cap, sizeof(cap));
            if (!rc) {
                if (!strcmp(cap, "read"))
                    g->cap = VAULT_CAP_READ;
                else if (!strcmp(cap, "write"))
                    g->cap = VAULT_CAP_WRITE;
                else
                    rc = VAULT_ERR_ARG;
            }
            break;
        }
        case 4:
            rc = vj_hex(&r, g->owner);
            break;
        case 5:
            rc = vj_hex(&r, g->root_manifest);
            break;
        case 6:
            rc = hints(&r, &g->relays, &g->relays_len, &g->blossom, &g->blossom_len);
            break;
        case 7:
            rc = vj_uint(&r, &g->issued_at);
            break;
        }
        if (rc)
            goto fail;
        int next = more(&r, '}');
        if (next < 0) {
            rc = VAULT_ERR_ARG;
            goto fail;
        }
        if (!next)
            break;
    }
    if (seen != 255 || g->issued_at > INT64_MAX || vj_end(&r)) {
        rc = VAULT_ERR_ARG;
        goto fail;
    }
    *out = g;
    return 0;
fail:
    vault_grant_free(g);
    return rc;
}
static int event_new(int kind, uint64_t time, const uint8_t pub[32], const char *content,
                     NostrEvent **out)
{
    NostrEvent *e = nostr_event_new();
    if (!e)
        return VAULT_ERR_OOM;
    char hex[65];
    porthome_hex64(pub, hex);
    nostr_event_set_kind(e, kind);
    nostr_event_set_created_at(e, (int64_t)time);
    nostr_event_set_pubkey(e, hex);
    nostr_event_set_content(e, content);
    if (!e->pubkey || !e->content) {
        vault_grant_rumor_free(e);
        return VAULT_ERR_OOM;
    }
    *out = e;
    return 0;
}
void vault_grant_rumor_free(NostrEvent *e)
{
    if (!e)
        return;
    if (e->content)
        OPENSSL_cleanse(e->content, strlen(e->content));
    nostr_event_free(e);
}
int vault_grant_rumor_new(const vault_grant *g, NostrEvent **out)
{
    if (out)
        *out = NULL;
    if (!out)
        return VAULT_ERR_ARG;
    char *body = NULL;
    int rc = vault_grant_encode(g, &body);
    if (rc)
        return rc;
    NostrEvent *e = NULL;
    rc = event_new(30804, g->issued_at, g->owner, body, &e);
    OPENSSL_cleanse(body, strlen(body));
    free(body);
    if (rc)
        return rc;
    char id[65];
    if (nostr_event_compute_id(e, id) != NOSTR_EVENT_VALIDATION_OK) {
        vault_grant_rumor_free(e);
        return PORTHOME_ERR_CRYPTO;
    }
    e->id = strdup(id);
    if (!e->id) {
        vault_grant_rumor_free(e);
        return VAULT_ERR_OOM;
    }
    *out = e;
    return 0;
}
int vault_grant_rumor_open(const NostrEvent *e, vault_grant **out)
{
    if (out)
        *out = NULL;
    if (!out || !e)
        return VAULT_ERR_ARG;
    if (e->kind != 30804 || (e->sig && *e->sig) || nostr_tags_size(e->tags) || !e->content ||
        e->created_at < 0 || strnlen(e->content, 65536) > 65535 ||
        nostr_event_validate_id(e, NULL) != NOSTR_EVENT_VALIDATION_OK)
        return VAULT_ERR_AUTH;
    int rc = vault_grant_decode(e->content, out);
    if (rc)
        return rc;
    uint8_t author[32];
    if (porthome_from_hex64(e->pubkey, author) || memcmp(author, (*out)->owner, 32) ||
        (uint64_t)e->created_at != (*out)->issued_at) {
        vault_grant_free(*out);
        *out = NULL;
        return VAULT_ERR_AUTH;
    }
    return 0;
}
int vault_grant_send(const vault_grant_carrier *c, const uint8_t recipient[32],
                     const vault_grant *g)
{
    if (!c || !c->send || !recipient)
        return VAULT_ERR_ARG;
    NostrEvent *rumor = NULL;
    int rc = vault_grant_rumor_new(g, &rumor);
    if (!rc)
        rc = c->send(c->ctx, recipient, rumor);
    vault_grant_rumor_free(rumor);
    return rc;
}
int vault_grant_receive(const vault_grant_carrier *c, vault_grant **out)
{
    if (out)
        *out = NULL;
    if (!out || !c || !c->receive)
        return VAULT_ERR_ARG;
    NostrEvent *rumor = NULL;
    uint8_t sender[32] = {0};
    int rc = c->receive(c->ctx, &rumor, sender);
    if (!rc)
        rc = vault_grant_rumor_open(rumor, out);
    if (!rc &&
        (!CRYPTO_memcmp(sender, (uint8_t[32]){0}, 32) || memcmp(sender, (*out)->owner, 32))) {
        vault_grant_free(*out);
        *out = NULL;
        rc = VAULT_ERR_AUTH;
    }
    vault_grant_rumor_free(rumor);
    return rc;
}
void vault_service_list_free(vault_service_list *s)
{
    if (!s)
        return;
    vj_urls_free(s->relays, s->relays_len);
    vj_urls_free(s->blossom, s->blossom_len);
    free(s);
}
int vault_service_list_encode(const vault_service_list *v, char **out)
{
    if (out)
        *out = NULL;
    if (!out || !v)
        return VAULT_ERR_ARG;
    char *s = malloc(65536);
    if (!s)
        return VAULT_ERR_OOM;
    size_t n = 0;
    int rc = vj_append(s, 65536, &n, "{\"vault-relay\":");
    if (!rc)
        rc = vj_write_urls(s, 65536, &n, v->relays, v->relays_len, 1);
    if (!rc)
        rc = vj_append(s, 65536, &n, ",\"vault-blossom\":");
    if (!rc)
        rc = vj_write_urls(s, 65536, &n, v->blossom, v->blossom_len, 0);
    if (!rc)
        rc = vj_append(s, 65536, &n, "}");
    if (rc)
        free(s);
    else
        *out = s;
    return rc;
}
int vault_service_list_decode(const char *json, vault_service_list **out)
{
    if (out)
        *out = NULL;
    if (!out)
        return VAULT_ERR_ARG;
    vj_reader r;
    int rc = vj_init(json, &r);
    if (rc)
        return rc;
    if (vj_take(&r, '{'))
        return VAULT_ERR_ARG;
    vault_service_list *s = calloc(1, sizeof(*s));
    if (!s)
        return VAULT_ERR_OOM;
    unsigned seen = 0;
    static const char *const names[] = {"vault-relay", "vault-blossom"};
    for (;;) {
        int f = vj_field(&r, names, 2);
        if (f < 0 || (seen & (1u << f))) {
            rc = VAULT_ERR_ARG;
            goto fail;
        }
        seen |= 1u << f;
        rc = f ? vj_urls(&r, &s->blossom, &s->blossom_len, 0)
               : vj_urls(&r, &s->relays, &s->relays_len, 1);
        if (rc)
            goto fail;
        int next = more(&r, '}');
        if (next < 0) {
            rc = VAULT_ERR_ARG;
            goto fail;
        }
        if (!next)
            break;
    }
    if (seen != 3 || vj_end(&r)) {
        rc = VAULT_ERR_ARG;
        goto fail;
    }
    *out = s;
    return 0;
fail:
    vault_service_list_free(s);
    return rc;
}
static int self_pub(const uint8_t sk[32], uint8_t pub[32])
{
    char secret[65];
    porthome_hex64(sk, secret);
    char *hex = nostr_key_get_public(secret);
    OPENSSL_cleanse(secret, sizeof(secret));
    if (!hex)
        return PORTHOME_ERR_CRYPTO;
    int rc = porthome_from_hex64(hex, pub);
    free(hex);
    return rc;
}
int vault_service_list_event_new(const vault_service_list *s, const uint8_t sk[32], uint64_t time,
                                 NostrEvent **out)
{
    if (out)
        *out = NULL;
    if (!out || !sk || time > INT64_MAX)
        return VAULT_ERR_ARG;
    char *body = NULL, *payload = NULL;
    uint8_t pub[32];
    int rc = vault_service_list_encode(s, &body);
    if (!rc)
        rc = self_pub(sk, pub);
    if (!rc && nostr_nip44_encrypt_v2(sk, pub, (uint8_t *)body, strlen(body), &payload))
        rc = PORTHOME_ERR_CRYPTO;
    if (!rc)
        rc = event_new(10319, time, pub, payload, out);
    if (body)
        OPENSSL_cleanse(body, strlen(body));
    free(body);
    free(payload);
    return rc;
}
int vault_service_list_event_open(const NostrEvent *e, const uint8_t sk[32],
                                  vault_service_list **out)
{
    if (out)
        *out = NULL;
    if (!out || !e || !sk)
        return VAULT_ERR_ARG;
    uint8_t pub[32], author[32];
    int rc = self_pub(sk, pub);
    if (rc)
        return rc;
    if (e->kind != 10319 || e->created_at < 0 || nostr_tags_size(e->tags) || !e->content ||
        strnlen(e->content, 90001) > 90000 || porthome_from_hex64(e->pubkey, author) ||
        memcmp(author, pub, 32) || nostr_event_validate(e, NULL) != NOSTR_EVENT_VALIDATION_OK)
        return VAULT_ERR_AUTH;
    uint8_t *body = NULL;
    size_t n = 0;
    if (nostr_nip44_decrypt_v2(sk, pub, e->content, &body, &n))
        return PORTHOME_ERR_CRYPTO;
    if (memchr(body, 0, n))
        rc = VAULT_ERR_ARG;
    else {
        char *s = malloc(n + 1);
        if (!s)
            rc = VAULT_ERR_OOM;
        else {
            memcpy(s, body, n);
            s[n] = 0;
            rc = vault_service_list_decode(s, out);
            OPENSSL_cleanse(s, n);
            free(s);
        }
    }
    OPENSSL_cleanse(body, n);
    free(body);
    return rc;
}
