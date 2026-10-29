/* SPDX-License-Identifier: MIT */
#include "internal.h"
#include <vault/nip59.h>
#include <nostr/nip59/nip59.h>
#include <nostr/nip44/nip44.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-kinds.h>
#include <nostr-tag.h>
#include <nostr-filter.h>

/* Experimental NIPs PR #1647, b22711d050df662bfba5f813a35d801daf067dc8. */
#define NIP4E_ENCRYPTION_KEY_KIND 10044

#define MAX_CIPHERTEXT 90000u
#define MAX_PLAINTEXT 65535u
struct vault_nip59_carrier {
    vault_grant_carrier interface;
    vault_nip59_transport transport;
    uint8_t sk[32], pub[32], device_sk[32], device_pub[32];
    int has_device;
    vault_nip4e_resolver resolver;
};
static void wipe_string(char *s)
{
    if (s)
        OPENSSL_cleanse(s, strlen(s));
    free(s);
}
static int ciphertext_valid(const NostrEvent *e)
{
    return e && e->created_at >= 0 && e->content && e->content[0] &&
           strnlen(e->content, MAX_CIPHERTEXT + 1) <= MAX_CIPHERTEXT;
}
static int recipient_matches(const NostrEvent *e, const uint8_t pub[32])
{
    size_t count = nostr_tags_size(e->tags), found = 0;
    if (count > 128)
        return 0;
    for (size_t i = 0; i < count; ++i) {
        NostrTag *tag = nostr_tags_get(e->tags, i);
        const char *name = nostr_tag_get(tag, 0);
        if (!name || strcmp(name, "p"))
            continue;
        uint8_t key[32];
        size_t n = nostr_tag_size(tag);
        if (++found != 1 || n < 2 || n > 3 || porthome_from_hex64(nostr_tag_get(tag, 1), key) ||
            CRYPTO_memcmp(key, pub, 32))
            return 0;
    }
    return found == 1;
}
typedef struct {
    const uint8_t *identity;
    uint8_t id[32], key[32];
    int64_t when;
    int found, key_error, error;
} recipient_lookup;
static int announcement(void *ctx, const NostrEvent *e)
{
    recipient_lookup *s = ctx;
    uint8_t author[32], id[32];
    if (s->error)
        return s->error;
    if (!e || e->kind != NIP4E_ENCRYPTION_KEY_KIND || e->created_at < 0 ||
        porthome_from_hex64(e->pubkey, author) || CRYPTO_memcmp(author, s->identity, 32) ||
        nostr_event_validate(e, NULL) != NOSTR_EVENT_VALIDATION_OK ||
        porthome_from_hex64(e->id, id))
        return s->error = VAULT_ERR_AUTH;
    /* NIP-01 replaceable ordering, independent of relay/arrival order. Duplicate
     * ids have no side effects. Never resurrect an older well-formed n tag. */
    if (s->found && (e->created_at < s->when ||
        (e->created_at == s->when && memcmp(id, s->id, 32) >= 0)))
        return 0;
    s->found = 1;
    s->when = e->created_at;
    memcpy(s->id, id, 32);
    s->key_error = VAULT_ERR_AUTH;
    size_t count = nostr_tags_size(e->tags), found = 0;
    if (count > 128)
        return 0;
    for (size_t i = 0; i < count; ++i) {
        NostrTag *tag = nostr_tags_get(e->tags, i);
        const char *name = nostr_tag_get(tag, 0);
        if (!name || strcmp(name, "n"))
            continue;
        const char *key = nostr_tag_get(tag, 1);
        if (++found != 1 || nostr_tag_size(tag) != 2 || !nostr_key_is_valid_public(key) ||
            porthome_from_hex64(key, s->key)) {
            s->key_error = VAULT_ERR_AUTH;
            return 0;
        }
        s->key_error = 0;
    }
    return 0;
}
static int resolve_recipient(vault_nip59_carrier *c, const uint8_t identity[32], uint8_t key[32])
{
    memcpy(key, identity, 32);
    if (!c->resolver.query)
        return 0;
    NostrFilter *filter = nostr_filter_new();
    if (!filter)
        return VAULT_ERR_OOM;
    char hex[65];
    porthome_hex64(identity, hex);
    const char *authors[] = {hex};
    int kind = NIP4E_ENCRYPTION_KEY_KIND;
    nostr_filter_set_authors(filter, authors, 1);
    nostr_filter_set_kinds(filter, &kind, 1);
    nostr_filter_set_limit(filter, 1);
    if (filter->authors.size != 1 || filter->kinds.size != 1) {
        nostr_filter_free(filter);
        return VAULT_ERR_OOM;
    }
    recipient_lookup s = {.identity = identity};
    int rc = 0;
    for (size_t i = 0; i < c->resolver.relays_len; ++i) {
        int eose = 0;
        rc = c->resolver.query(c->resolver.ctx, c->resolver.relays[i], filter,
                               announcement, &s, &eose);
        if (!rc)
            rc = s.error;
        if (!rc && eose != 1)
            rc = VAULT_ERR_INCOMPLETE;
        if (rc)
            break;
    }
    nostr_filter_free(filter);
    if (!rc && s.found) {
        rc = s.key_error;
        if (!rc)
            memcpy(key, s.key, 32);
    }
    return rc;
}
static int send_grant(void *ctx, const uint8_t recipient[32], const NostrEvent *rumor)
{
    vault_nip59_carrier *c = ctx;
    if (!c || !recipient || !c->transport.publish)
        return VAULT_ERR_ARG;
    vault_grant *grant = NULL;
    int rc = vault_grant_rumor_open(rumor, &grant);
    if (rc)
        return rc;
    if (CRYPTO_memcmp(grant->owner, c->pub, 32))
        rc = VAULT_ERR_AUTH;
    vault_grant_free(grant);
    if (rc)
        return rc;
    uint8_t delivery_key[32];
    rc = resolve_recipient(c, recipient, delivery_key);
    if (rc)
        return rc;
    char *json = nostr_event_serialize_compact(rumor), *encrypted = NULL;
    char sk_hex[65], pub_hex[65], recipient_hex[65];
    NostrEvent *seal = NULL, *wrap = NULL;
    if (!json)
        return VAULT_ERR_OOM;
    if (strlen(json) > MAX_PLAINTEXT) {
        rc = PORTHOME_ERR_TOO_LARGE;
        goto done;
    }
    if (nostr_nip44_encrypt_v2(c->sk, delivery_key, (uint8_t *)json, strlen(json), &encrypted)) {
        rc = PORTHOME_ERR_CRYPTO;
        goto done;
    }
    int64_t when = 0;
    if (nostr_nip59_randomize_timestamp(0, 0, &when)) {
        rc = PORTHOME_ERR_CRYPTO;
        goto done;
    }
    seal = nostr_event_new();
    if (!seal) {
        rc = VAULT_ERR_OOM;
        goto done;
    }
    porthome_hex64(c->sk, sk_hex);
    porthome_hex64(c->pub, pub_hex);
    porthome_hex64(delivery_key, recipient_hex);
    nostr_event_set_kind(seal, NOSTR_KIND_SEAL);
    nostr_event_set_pubkey(seal, pub_hex);
    nostr_event_set_created_at(seal, when);
    nostr_event_set_content(seal, encrypted);
    if (nostr_event_sign(seal, sk_hex)) {
        rc = PORTHOME_ERR_CRYPTO;
        goto done;
    }
    /* Check the second envelope's bound before asking nip59 to encrypt it. */
    char *seal_json = nostr_event_serialize_compact(seal);
    if (!seal_json) {
        rc = VAULT_ERR_OOM;
        goto done;
    }
    size_t seal_len = strlen(seal_json);
    free(seal_json); /* Contains only the encrypted rumor. */
    if (seal_len > MAX_PLAINTEXT) {
        rc = PORTHOME_ERR_TOO_LARGE;
        goto done;
    }
    wrap = nostr_nip59_wrap(seal, recipient_hex, NULL);
    if (!wrap) {
        rc = PORTHOME_ERR_CRYPTO;
        goto done;
    }
    rc = c->transport.publish(c->transport.ctx, wrap);
done:
    OPENSSL_cleanse(sk_hex, sizeof(sk_hex));
    wipe_string(json);
    free(encrypted);
    nostr_event_free(seal);
    nostr_event_free(wrap);
    return rc;
}
static int open_grant(NostrEvent *wrap, const uint8_t sk[32], const uint8_t pub[32],
                      NostrEvent **out, uint8_t sender[32])
{
    NostrEvent *seal = NULL, *rumor = NULL;
    vault_grant *grant = NULL;
    uint8_t *plain = NULL, author[32];
    char *json = NULL;
    size_t len = 0;
    int rc = VAULT_ERR_AUTH;
    if (!ciphertext_valid(wrap) || wrap->kind != NOSTR_KIND_GIFT_WRAP ||
        !recipient_matches(wrap, pub))
        goto done;
    /* nip59 verifies the outer id/signature before any decrypt, and verifies
     * the signed inner event too. A directly wrapped rumor is not a seal. */
    seal = nostr_nip59_unwrap_with_key(wrap, sk);
    if (!ciphertext_valid(seal) || seal->kind != NOSTR_KIND_SEAL || nostr_tags_size(seal->tags) ||
        nostr_event_validate(seal, NULL) != NOSTR_EVENT_VALIDATION_OK ||
        porthome_from_hex64(seal->pubkey, author))
        goto done;
    if (nostr_nip44_decrypt_v2(sk, author, seal->content, &plain, &len))
        goto done;
    if (!plain || !len || len > MAX_PLAINTEXT || memchr(plain, 0, len))
        goto done;
    json = malloc(len + 1);
    rumor = nostr_event_new();
    if (!json || !rumor) {
        rc = VAULT_ERR_OOM;
        goto done;
    }
    memcpy(json, plain, len);
    json[len] = 0;
    if (nostr_event_deserialize_unsigned(rumor, json, NULL) != NOSTR_EVENT_VALIDATION_OK)
        goto done;
    rc = vault_grant_rumor_open(rumor, &grant);
    if (rc)
        goto done;
    if (CRYPTO_memcmp(grant->owner, author, 32)) {
        rc = VAULT_ERR_AUTH;
        goto done;
    }
    memcpy(sender, author, 32);
    *out = rumor;
    rumor = NULL;
    rc = 0;
done:
    if (plain)
        OPENSSL_cleanse(plain, len);
    free(plain);
    wipe_string(json);
    vault_grant_free(grant);
    vault_grant_rumor_free(rumor);
    /* The rejected inner event may be a bare grant, not an encrypted seal. */
    vault_grant_rumor_free(seal);
    return rc == 0 || rc == VAULT_ERR_OOM ? rc : VAULT_ERR_AUTH;
}
static int receive_grant(void *ctx, NostrEvent **out, uint8_t sender[32])
{
    if (out)
        *out = NULL;
    if (sender)
        memset(sender, 0, 32);
    vault_nip59_carrier *c = ctx;
    if (!c || !out || !sender || !c->transport.receive)
        return VAULT_ERR_ARG;
    NostrEvent *wrap = NULL;
    int rc = c->transport.receive(c->transport.ctx, &wrap);
    if (!rc) {
        rc = VAULT_ERR_AUTH;
        if (c->has_device)
            rc = open_grant(wrap, c->device_sk, c->device_pub, out, sender);
        if (rc == VAULT_ERR_AUTH)
            rc = open_grant(wrap, c->sk, c->pub, out, sender);
    }
    nostr_event_free(wrap);
    return rc;
}
int vault_nip59_carrier_set_nip4e(vault_nip59_carrier *c, const vault_nip4e_resolver *resolver)
{
    if (!c)
        return VAULT_ERR_ARG;
    if (resolver) {
        if (!resolver->query || !resolver->relays || !resolver->relays_len)
            return VAULT_ERR_ARG;
        for (size_t i = 0; i < resolver->relays_len; ++i)
            if (!resolver->relays[i] || !resolver->relays[i][0])
                return VAULT_ERR_ARG;
    }
    c->resolver = resolver ? *resolver : (vault_nip4e_resolver){0};
    return 0;
}
int vault_nip59_carrier_set_device_key(vault_nip59_carrier *c, const uint8_t sk[32])
{
    if (!c)
        return VAULT_ERR_ARG;
    uint8_t pub[32];
    if (sk) {
        char hex[65];
        porthome_hex64(sk, hex);
        char *key = nostr_key_get_public(hex);
        OPENSSL_cleanse(hex, sizeof(hex));
        if (!key)
            return VAULT_ERR_ARG;
        int rc = porthome_from_hex64(key, pub);
        free(key);
        if (rc)
            return VAULT_ERR_ARG;
    }
    OPENSSL_cleanse(c->device_sk, 32);
    memset(c->device_pub, 0, 32);
    c->has_device = sk != NULL;
    if (sk) {
        memcpy(c->device_sk, sk, 32);
        memcpy(c->device_pub, pub, 32);
    }
    return 0;
}
int vault_nip59_carrier_new(const uint8_t sk[32], const vault_nip59_transport *transport,
                            vault_nip59_carrier **out)
{
    if (out)
        *out = NULL;
    if (!out || !sk || !transport || (!transport->publish && !transport->receive))
        return VAULT_ERR_ARG;
    char hex[65];
    porthome_hex64(sk, hex);
    char *pub = nostr_key_get_public(hex);
    OPENSSL_cleanse(hex, sizeof(hex));
    if (!pub)
        return VAULT_ERR_ARG;
    vault_nip59_carrier *c = calloc(1, sizeof(*c));
    if (!c) {
        free(pub);
        return VAULT_ERR_OOM;
    }
    int rc = porthome_from_hex64(pub, c->pub);
    free(pub);
    if (rc) {
        vault_nip59_carrier_free(c);
        return VAULT_ERR_ARG;
    }
    memcpy(c->sk, sk, 32);
    c->transport = *transport;
    c->interface = (vault_grant_carrier){c, send_grant, receive_grant};
    *out = c;
    return 0;
}
const vault_grant_carrier *vault_nip59_carrier_interface(vault_nip59_carrier *c)
{
    return c ? &c->interface : NULL;
}
void vault_nip59_carrier_free(vault_nip59_carrier *c)
{
    if (!c)
        return;
    OPENSSL_cleanse(c, sizeof(*c));
    free(c);
}
