/* SPDX-License-Identifier: MIT */
#include "internal.h"
#include <vault/nip59.h>
#include <nostr/nip59/nip59.h>
#include <nostr/nip44/nip44.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-kinds.h>
#include <nostr-tag.h>

#define MAX_CIPHERTEXT 90000u
#define MAX_PLAINTEXT 65535u
struct vault_nip59_carrier {
    vault_grant_carrier interface;
    vault_nip59_transport transport;
    uint8_t sk[32], pub[32];
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
    char *json = nostr_event_serialize_compact(rumor), *encrypted = NULL;
    char sk_hex[65], pub_hex[65], recipient_hex[65];
    NostrEvent *seal = NULL, *wrap = NULL;
    if (!json)
        return VAULT_ERR_OOM;
    if (strlen(json) > MAX_PLAINTEXT) {
        rc = PORTHOME_ERR_TOO_LARGE;
        goto done;
    }
    if (nostr_nip44_encrypt_v2(c->sk, recipient, (uint8_t *)json, strlen(json), &encrypted)) {
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
    porthome_hex64(recipient, recipient_hex);
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
static int receive_grant(void *ctx, NostrEvent **out, uint8_t sender[32])
{
    if (out)
        *out = NULL;
    if (sender)
        memset(sender, 0, 32);
    vault_nip59_carrier *c = ctx;
    if (!c || !out || !sender || !c->transport.receive)
        return VAULT_ERR_ARG;
    NostrEvent *wrap = NULL, *seal = NULL, *rumor = NULL;
    vault_grant *grant = NULL;
    uint8_t *plain = NULL, author[32];
    char *json = NULL;
    size_t len = 0;
    int rc = c->transport.receive(c->transport.ctx, &wrap);
    if (rc)
        goto done;
    rc = VAULT_ERR_AUTH;
    if (!ciphertext_valid(wrap) || wrap->kind != NOSTR_KIND_GIFT_WRAP ||
        !recipient_matches(wrap, c->pub))
        goto done;
    /* nip59 verifies the outer id/signature before any decrypt, and verifies
     * the signed inner event too. A directly wrapped rumor is not a seal. */
    seal = nostr_nip59_unwrap_with_key(wrap, c->sk);
    if (!ciphertext_valid(seal) || seal->kind != NOSTR_KIND_SEAL || nostr_tags_size(seal->tags) ||
        nostr_event_validate(seal, NULL) != NOSTR_EVENT_VALIDATION_OK ||
        porthome_from_hex64(seal->pubkey, author))
        goto done;
    if (nostr_nip44_decrypt_v2(c->sk, author, seal->content, &plain, &len))
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
    nostr_event_free(wrap);
    return rc;
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
