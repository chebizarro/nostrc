/* SPDX-License-Identifier: MIT */
#include "nostrc-test-vault.h"
#include <vault/nip59.h>
#include <nostr/nip59/nip59.h>
#include <nostr/nip44/nip44.h>
#include <nostr-tag.h>
#include <openssl/crypto.h>
#include <time.h>

typedef struct {
    char *json;
    unsigned sends, receives;
    int send_error, receive_error;
} wire;
static int publish(void *ctx, const NostrEvent *e)
{
    wire *w = ctx;
    ++w->sends;
    assert(e->kind == 1059 && nostr_nip59_validate_gift_wrap((NostrEvent *)e));
    assert(!w->json);
    w->json = nostr_event_serialize_compact(e);
    assert(w->json && !strstr(w->json, "node_key") && !strstr(w->json, "root_manifest"));
    return w->send_error; /* A nonzero uncertain ACK is never retried. */
}
static int receive(void *ctx, NostrEvent **out)
{
    wire *w = ctx;
    ++w->receives;
    *out = NULL;
    if (!w->json)
        return VAULT_ERR_AGAIN;
    *out = nostr_event_new();
    assert(*out &&
           nostr_event_deserialize_signed(*out, w->json, NULL) == NOSTR_EVENT_VALIDATION_OK);
    free(w->json);
    w->json = NULL;
    return w->receive_error;
}
static void queue(wire *w, NostrEvent *event)
{
    assert(!w->json && event);
    w->json = nostr_event_serialize_compact(event);
    assert(w->json);
    nostr_event_free(event);
}
static NostrEvent *seal_plain(world *w, size_t sender, const uint8_t *plain, size_t len)
{
    char *content = NULL;
    assert(nostr_nip44_encrypt_v2(w->sk[sender], w->pub[1], plain, len, &content) == 0);
    NostrEvent *seal = nostr_event_new();
    char pub[65];
    porthome_hex64(w->pub[sender], pub);
    nostr_event_set_kind(seal, 13);
    nostr_event_set_pubkey(seal, pub);
    nostr_event_set_content(seal, content);
    nostr_event_set_created_at(seal,
                               1); /* Crafted adversarial fixture, not production timestamp. */
    assert(nostr_event_sign(seal, w->secret[sender]) == 0);
    free(content);
    return seal;
}
static NostrEvent *seal_rumor(world *w, size_t sender, NostrEvent *rumor)
{
    char *json = nostr_event_serialize_compact(rumor);
    assert(json);
    NostrEvent *seal = seal_plain(w, sender, (uint8_t *)json, strlen(json));
    OPENSSL_cleanse(json, strlen(json));
    free(json);
    return seal;
}
static NostrEvent *wrap(world *w, NostrEvent *inner)
{
    char recipient[65];
    porthome_hex64(w->pub[1], recipient);
    /* A fixed ephemeral key is only used to construct negative fixtures. */
    NostrEvent *outer = nostr_nip59_wrap(inner, recipient, w->secret[3]);
    assert(outer);
    nostr_event_free(inner);
    return outer;
}
static void refused(wire *w, const vault_grant_carrier *carrier, NostrEvent *event)
{
    queue(w, event);
    unsigned before = w->receives;
    vault_grant *g = NULL;
    assert(vault_grant_receive(carrier, &g) != 0 && !g && !w->json);
    assert(w->receives == before + 1);
}
int main(void)
{
    world world_;
    world *w = &world_;
    world_init(w);
    uint8_t second[32] = {0x91};
    uint64_t epoch;
    assert(vault_ring_append(&w->ring, second, &epoch) == 0 && epoch == 2);
    char *relays[] = {"wss://nos.lol"}, *blossom[] = {"https://vault.example"};
    vault_grant grant = {.ring = w->ring,
                         .cap = VAULT_CAP_WRITE,
                         .issued_at = 123,
                         .relays = relays,
                         .relays_len = 1,
                         .blossom = blossom,
                         .blossom_len = 1};
    memcpy(grant.owner, w->pub[0], 32);
    memcpy(grant.node_id, w->node.node_id, 32);
    memcpy(grant.vault_id, w->vault.vault_id, 32);
    memset(grant.root_manifest, 0x41, 32);
    wire channel = {0};
    vault_nip59_transport io = {&channel, publish, receive};
    vault_nip59_carrier *alice = NULL, *bob = NULL, *eve = NULL;
    assert(vault_nip59_carrier_new(w->sk[0], &io, &alice) == 0);
    assert(vault_nip59_carrier_new(w->sk[1], &io, &bob) == 0);
    assert(vault_nip59_carrier_new(w->sk[2], &io, &eve) == 0);
    const vault_grant_carrier *a = vault_nip59_carrier_interface(alice);
    const vault_grant_carrier *b = vault_nip59_carrier_interface(bob);
    vault_grant *decoded = NULL;
    assert(vault_grant_receive(b, &decoded) == VAULT_ERR_AGAIN && !decoded);
    char *sample = NULL, *previous_author = NULL;
    for (int cap = VAULT_CAP_READ; cap <= VAULT_CAP_WRITE; ++cap) {
        grant.cap = cap;
        char *before = NULL, *after = NULL;
        assert(vault_grant_encode(&grant, &before) == 0);
        unsigned sends = channel.sends;
        assert(vault_grant_send(a, w->pub[1], &grant) == 0 && channel.sends == sends + 1);
        NostrEvent *outer = nostr_event_new();
        assert(nostr_event_deserialize_signed(outer, channel.json, NULL) ==
               NOSTR_EVENT_VALIDATION_OK);
        char author[65];
        porthome_hex64(w->pub[0], author);
        assert(outer->kind == 1059 && strcmp(outer->pubkey, author));
        if (previous_author)
            assert(strcmp(previous_author, outer->pubkey));
        free(previous_author);
        previous_author = strdup(outer->pubkey);
        int64_t now = (int64_t)time(NULL);
        assert(outer->created_at < now && outer->created_at >= now - 172801);
        NostrEvent *seal = nostr_nip59_unwrap_with_key(outer, w->sk[1]);
        assert(seal && seal->kind == 13 && !nostr_tags_size(seal->tags) &&
               !strcmp(seal->pubkey, author) &&
               nostr_event_validate(seal, NULL) == NOSTR_EVENT_VALIDATION_OK);
        assert(seal->created_at < now && seal->created_at >= now - 172801);
        nostr_event_free(seal);
        nostr_event_free(outer);
        free(sample);
        sample = strdup(channel.json);
        assert(vault_grant_receive(b, &decoded) == 0 && decoded);
        assert(vault_grant_encode(decoded, &after) == 0 && !strcmp(before, after));
        assert(decoded->ring.count == 2 && decoded->ring.entries[0].epoch == 1 &&
               decoded->ring.entries[1].epoch == 2 &&
               !memcmp(decoded->ring.entries, grant.ring.entries, 2 * sizeof(*grant.ring.entries)));
        OPENSSL_cleanse(before, strlen(before));
        free(before);
        OPENSSL_cleanse(after, strlen(after));
        free(after);
        vault_grant_free(decoded);
        decoded = NULL;
    }
    free(previous_author);
    /* The raw callback seam preserves the unsigned rumor body and canonical ID
     * byte-for-byte, and authenticates the identity rather than the wrap key. */
    NostrEvent *original = NULL, *roundtrip = NULL;
    assert(vault_grant_rumor_new(&grant, &original) == 0);
    assert(a->send(a->ctx, w->pub[1], original) == 0);
    uint8_t authenticated_sender[32] = {0};
    assert(b->receive(b->ctx, &roundtrip, authenticated_sender) == 0 && roundtrip);
    assert(!memcmp(authenticated_sender, w->pub[0], 32) && !roundtrip->sig &&
           !strcmp(original->id, roundtrip->id) && !strcmp(original->content, roundtrip->content));
    vault_grant_rumor_free(original);
    vault_grant_rumor_free(roundtrip);
    /* Wrong recipient; outer signature/id tampering; transport failure: no retry. */
    channel.json = strdup(sample);
    assert(vault_grant_receive(vault_nip59_carrier_interface(eve), &decoded) == VAULT_ERR_AUTH &&
           !decoded);
    NostrEvent *outer = nostr_event_new();
    assert(nostr_event_deserialize_signed(outer, sample, NULL) == NOSTR_EVENT_VALIDATION_OK);
    outer->content[10] ^= 1;
    refused(&channel, b, outer);
    outer = nostr_event_new();
    assert(nostr_event_deserialize_signed(outer, sample, NULL) == NOSTR_EVENT_VALIDATION_OK);
    outer->id[0] = outer->id[0] == '0' ? '1' : '0';
    refused(&channel, b, outer);
    channel.receive_error = VAULT_ERR_IO;
    channel.json = strdup(sample);
    assert(vault_grant_receive(b, &decoded) == VAULT_ERR_IO && !decoded && !channel.json);
    channel.receive_error = 0;
    unsigned sends = channel.sends;
    channel.send_error = VAULT_ERR_IO;
    assert(vault_grant_send(a, w->pub[1], &grant) == VAULT_ERR_IO && channel.sends == sends + 1);
    free(channel.json);
    channel.json = NULL;
    channel.send_error = 0;
    /* Validly wrapped hostile seals cannot authenticate an echoed rumor author. */
    NostrEvent *rumor = NULL;
    assert(vault_grant_rumor_new(&grant, &rumor) == 0);
    refused(&channel, b, wrap(w, seal_rumor(w, 2, rumor)));
    NostrEvent *seal = seal_rumor(w, 0, rumor);
    seal->sig[0] = seal->sig[0] == '0' ? '1' : '0';
    refused(&channel, b, wrap(w, seal));
    seal = seal_rumor(w, 0, rumor);
    nostr_event_set_tags(seal, nostr_tags_new(1, nostr_tag_new("p", "not-empty", NULL)));
    assert(nostr_event_sign(seal, w->secret[0]) == 0);
    refused(&channel, b, wrap(w, seal));
    char recipient[65];
    porthome_hex64(w->pub[1], recipient);
    refused(&channel, b, nostr_nip59_wrap(rumor, recipient, NULL)); /* No seal. */
    outer = wrap(w, seal_rumor(w, 0, rumor));
    char wrong[65];
    porthome_hex64(w->pub[2], wrong);
    nostr_event_set_tags(outer, nostr_tags_new(1, nostr_tag_new("p", wrong, NULL)));
    assert(nostr_event_sign(outer, w->secret[3]) == 0);
    refused(&channel, b, outer);
    outer = wrap(w, seal_rumor(w, 0, rumor));
    nostr_event_set_tags(outer, nostr_tags_new(2, nostr_tag_new("p", recipient, NULL),
                                               nostr_tag_new("p", recipient, NULL)));
    assert(nostr_event_sign(outer, w->secret[3]) == 0);
    refused(&channel, b, outer);
    char *json = nostr_event_serialize_compact(rumor);
    refused(&channel, b, wrap(w, seal_plain(w, 0, (uint8_t *)json, strlen(json) + 1)));
    OPENSSL_cleanse(json, strlen(json));
    free(json);
    assert(nostr_event_sign(rumor, w->secret[0]) == 0);
    refused(&channel, b, wrap(w, seal_rumor(w, 0, rumor))); /* Signed rumor is forbidden. */
    sends = channel.sends;
    assert(a->send(a->ctx, w->pub[1], rumor) == VAULT_ERR_AUTH && channel.sends == sends);
    vault_grant_rumor_free(rumor);
    memcpy(grant.owner, w->pub[2], 32);
    assert(vault_grant_send(a, w->pub[1], &grant) == VAULT_ERR_AUTH && channel.sends == sends);
    memcpy(grant.owner, w->pub[0], 32);
    char large_url[2001];
    memset(large_url, 'a', 2000);
    memcpy(large_url, "https://vault.example/", 22);
    large_url[2000] = 0;
    char *large[24];
    for (size_t i = 0; i < 24; ++i)
        large[i] = large_url;
    grant.blossom = large;
    grant.blossom_len = 24;
    assert(vault_grant_send(a, w->pub[1], &grant) == PORTHOME_ERR_TOO_LARGE &&
           channel.sends == sends);
    uint8_t zero[32] = {0};
    vault_nip59_carrier *bad = NULL;
    assert(vault_nip59_carrier_new(zero, &io, &bad) == VAULT_ERR_ARG && !bad);
    vault_nip59_transport absent = {0};
    assert(vault_nip59_carrier_new(w->sk[0], &absent, &bad) == VAULT_ERR_ARG && !bad);
    vault_nip59_carrier_free(alice);
    vault_nip59_carrier_free(bob);
    vault_nip59_carrier_free(eve);
    free(sample);
    world_dispose(w);
    puts("OK NIP-59 grant seal/wrap/unwrap, exact body/ring, authenticated sender (section13 e "
         "nip59)");
    return 0;
}
