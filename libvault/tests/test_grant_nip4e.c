/* SPDX-License-Identifier: MIT */
#include "nostrc-test-vault.h"
#include <vault/nip59.h>
#include <nostr/nip59/nip59.h>
#include <nostr-filter.h>
#include <nostr-tag.h>
#include <openssl/crypto.h>

typedef struct {
    NostrEvent *events[2][4];
    size_t count[2];
    int eose[2], error[2];
    unsigned queries, sends;
    NostrEvent *wrap;
    const char *identity;
} fixture;
static const char *const relays[] = {"ws://127.0.0.1:10001", "ws://127.0.0.1:10002"};
static int query(void *ctx, const char *relay, const NostrFilter *filter,
                 vault_nip4e_emit_fn emit, void *emit_ctx, int *eose)
{
    fixture *f = ctx;
    size_t at = !strcmp(relay, relays[0]) ? 0 : 1;
    assert(!strcmp(relay, relays[at]));
    assert(filter->kinds.size == 1 && filter->kinds.data[0] == 10044 &&
           filter->authors.size == 1 && !strcmp(filter->authors.data[0], f->identity) &&
           filter->limit == 1 && !filter->since && !filter->until &&
           !nostr_tags_size(filter->tags));
    ++f->queries;
    /* emit is backfill, never completion. A failing/missing EOSE must prevent
     * send even when a usable key has already arrived on this/another relay. */
    for (size_t i = 0; i < f->count[at]; ++i) {
        int rc = emit(emit_ctx, f->events[at][i]);
        if (rc)
            return rc;
    }
    *eose = f->eose[at];
    return f->error[at];
}
static int publish(void *ctx, const NostrEvent *wrap)
{
    fixture *f = ctx;
    assert(!f->wrap && wrap->kind == 1059 &&
           nostr_event_validate(wrap, NULL) == NOSTR_EVENT_VALIDATION_OK);
    assert(f->queries == 0 || (f->eose[0] && f->eose[1] && !f->error[0] && !f->error[1]));
    char *json = nostr_event_serialize_compact(wrap);
    f->wrap = nostr_event_new();
    assert(json && f->wrap && nostr_event_deserialize_signed(f->wrap, json, NULL) ==
                                NOSTR_EVENT_VALIDATION_OK);
    free(json);
    ++f->sends;
    return 0; /* Recording transport's explicit OK. */
}
static int receive(void *ctx, NostrEvent **out)
{
    fixture *f = ctx;
    *out = f->wrap;
    f->wrap = NULL;
    return *out ? 0 : VAULT_ERR_AGAIN;
}
static NostrEvent *announcement(world *w, size_t author, const char *key, int64_t when)
{
    NostrEvent *e = nostr_event_new();
    assert(e);
    nostr_event_set_kind(e, 10044);
    nostr_event_set_created_at(e, when);
    nostr_event_set_content(e, "");
    nostr_event_set_tags(e, nostr_tags_new(1, nostr_tag_new("n", key, NULL)));
    assert(!nostr_event_sign(e, w->secret[author]));
    return e;
}
static void selected(fixture *f, const uint8_t pub[32])
{
    assert(f->wrap && nostr_tags_size(f->wrap->tags) == 1);
    NostrTag *p = nostr_tags_get(f->wrap->tags, 0);
    char hex[65];
    porthome_hex64(pub, hex);
    assert(nostr_tag_size(p) == 2 && !strcmp(nostr_tag_get(p, 0), "p") &&
           !strcmp(nostr_tag_get(p, 1), hex));
}
static void opened(fixture *f, const vault_grant_carrier *receiver, const vault_grant *original)
{
    vault_grant *g = NULL;
    assert(!vault_grant_receive(receiver, &g) && g && !f->wrap);
    char *before = NULL, *after = NULL;
    assert(!vault_grant_encode(original, &before) && !vault_grant_encode(g, &after));
    assert(!strcmp(before, after));
    OPENSSL_cleanse(before, strlen(before));
    OPENSSL_cleanse(after, strlen(after));
    free(before);
    free(after);
    vault_grant_free(g);
}
static void denied(fixture *f, const vault_grant_carrier *sender, world *w,
                   const vault_grant *g, int error)
{
    unsigned sends = f->sends;
    f->queries = 0;
    assert(vault_grant_send(sender, w->pub[1], g) == error && !f->wrap && f->sends == sends);
}
int main(void)
{
    world w;
    world_init(&w);
    char identity[65], device[65], old_key[65];
    porthome_hex64(w.pub[1], identity);
    porthome_hex64(w.pub[2], device);
    porthome_hex64(w.pub[3], old_key);
    fixture f = {.identity = identity, .eose = {1, 1}};
    vault_nip59_transport transport = {&f, publish, receive};
    vault_nip4e_resolver resolver = {&f, relays, 2, query};
    vault_nip59_carrier *alice = NULL, *bob = NULL;
    assert(!vault_nip59_carrier_new(w.sk[0], &transport, &alice));
    assert(!vault_nip59_carrier_new(w.sk[1], &transport, &bob));
    const vault_grant_carrier *a = vault_nip59_carrier_interface(alice);
    const vault_grant_carrier *b = vault_nip59_carrier_interface(bob);
    vault_grant g = {.ring = w.ring, .cap = VAULT_CAP_READ, .issued_at = 123};
    memcpy(g.owner, w.pub[0], 32);
    memcpy(g.node_id, w.node.node_id, 32);
    memcpy(g.vault_id, w.vault.vault_id, 32);
    memset(g.root_manifest, 0x41, 32);
    NostrEvent *valid = announcement(&w, 1, device, 20);
    NostrEvent *older = announcement(&w, 1, old_key, 10);
    f.events[0][0] = valid;
    f.count[0] = 1;
    /* Off by default, even with a device announcement available. */
    assert(!vault_grant_send(a, w.pub[1], &g) && !f.queries);
    selected(&f, w.pub[1]);
    opened(&f, b, &g);
    assert(!vault_nip59_carrier_set_nip4e(alice, &resolver));
    assert(!vault_grant_send(a, w.pub[1], &g) && f.queries == 2);
    selected(&f, w.pub[2]);
    /* With no configured device key the same envelope fails generically. */
    char *saved = nostr_event_serialize_compact(f.wrap);
    vault_grant *out = NULL;
    assert(vault_grant_receive(b, &out) == VAULT_ERR_AUTH && !out);
    f.wrap = nostr_event_new();
    assert(nostr_event_deserialize_signed(f.wrap, saved, NULL) == NOSTR_EVENT_VALIDATION_OK);
    free(saved);
    uint8_t local[32];
    memcpy(local, w.sk[2], 32);
    assert(!vault_nip59_carrier_set_device_key(bob, local));
    memset(local, 0, 32); /* The adapter owns a copy. */
    opened(&f, b, &g);
    /* Older and duplicate events on a later relay cannot override latest. */
    f.events[1][0] = older;
    f.events[1][1] = valid;
    f.count[1] = 2;
    f.queries = 0;
    assert(!vault_grant_send(a, w.pub[1], &g) && f.queries == 2);
    selected(&f, w.pub[2]);
    opened(&f, b, &g);
    /* NIP-01 same-timestamp tie is lowest event id, not last arrival. */
    NostrEvent *tie = announcement(&w, 1, old_key, 20);
    f.events[1][0] = tie;
    f.count[1] = 1;
    int device_wins = strcmp(valid->id, tie->id) < 0;
    f.queries = 0;
    assert(!vault_grant_send(a, w.pub[1], &g));
    selected(&f, w.pub[device_wins ? 2 : 3]);
    assert(!vault_nip59_carrier_set_device_key(bob, w.sk[device_wins ? 2 : 3]));
    opened(&f, b, &g);
    assert(!vault_nip59_carrier_set_device_key(bob, w.sk[2]));
    f.count[0] = f.count[1] = 0;
    f.queries = 0;
    assert(!vault_grant_send(a, w.pub[1], &g) && f.queries == 2);
    selected(&f, w.pub[1]);
    opened(&f, b, &g); /* Device-key attempt falls back to identity. */
    /* Empty and nonempty incomplete reads both fail without publication. */
    f.error[1] = VAULT_ERR_IO; /* Unreachable/disconnected relay. */
    denied(&f, a, &w, &g, VAULT_ERR_IO);
    assert(f.queries == 2);
    f.error[1] = VAULT_ERR_AUTH; /* CLOSED/refused or unhandled AUTH. */
    denied(&f, a, &w, &g, VAULT_ERR_AUTH);
    f.error[1] = 0;
    f.eose[1] = 0;
    denied(&f, a, &w, &g, VAULT_ERR_INCOMPLETE);
    f.count[0] = 1;
    denied(&f, a, &w, &g, VAULT_ERR_INCOMPLETE);
    f.error[1] = VAULT_ERR_IO;
    denied(&f, a, &w, &g, VAULT_ERR_IO);
    f.error[1] = 0;
    f.eose[1] = 1;
    NostrEvent *forged = announcement(&w, 3, device, 30);
    f.events[0][0] = forged; /* Valid signature, different author. */
    denied(&f, a, &w, &g, VAULT_ERR_AUTH);
    nostr_event_free(forged);
    forged = announcement(&w, 1, device, 30);
    forged->sig[0] = forged->sig[0] == '0' ? '1' : '0';
    f.events[0][0] = forged;
    denied(&f, a, &w, &g, VAULT_ERR_AUTH);
    nostr_event_free(forged);
    /* A malformed latest announcement must not resurrect an older key. */
    const char *bad_keys[] = {"not-hex", "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
    f.events[0][0] = valid;
    f.count[1] = 1;
    for (size_t i = 0; i < sizeof(bad_keys) / sizeof(*bad_keys); ++i) {
        NostrEvent *bad = announcement(&w, 1, bad_keys[i], 40);
        f.events[1][0] = bad;
        denied(&f, a, &w, &g, VAULT_ERR_AUTH);
        nostr_event_free(bad);
    }
    NostrEvent *bad = announcement(&w, 1, device, 40);
    nostr_event_set_tags(bad, nostr_tags_new(2, nostr_tag_new("n", device, NULL),
                                              nostr_tag_new("n", device, NULL)));
    assert(!nostr_event_sign(bad, w.secret[1]));
    f.events[1][0] = bad;
    denied(&f, a, &w, &g, VAULT_ERR_AUTH);
    nostr_event_free(bad);
    bad = announcement(&w, 1, device, 40);
    nostr_event_set_tags(bad, nostr_tags_new(0));
    assert(!nostr_event_sign(bad, w.secret[1]));
    f.events[1][0] = bad;
    denied(&f, a, &w, &g, VAULT_ERR_AUTH);
    nostr_event_free(bad);
    /* Explicitly disabling resolution restores the unchanged identity path. */
    assert(!vault_nip59_carrier_set_nip4e(alice, NULL));
    f.queries = 0;
    assert(!vault_grant_send(a, w.pub[1], &g) && !f.queries);
    selected(&f, w.pub[1]);
    opened(&f, b, &g);
    uint8_t zero[32] = {0};
    assert(vault_nip59_carrier_set_device_key(bob, zero) == VAULT_ERR_ARG);
    assert(!vault_nip59_carrier_set_device_key(bob, NULL));
    vault_nip4e_resolver absent = {0};
    assert(vault_nip59_carrier_set_nip4e(alice, &absent) == VAULT_ERR_ARG);
    assert(!vault_grant_send(a, w.pub[1], &g));
    opened(&f, b, &g);
    nostr_event_free(tie);
    nostr_event_free(valid);
    nostr_event_free(older);
    vault_nip59_carrier_free(alice);
    vault_nip59_carrier_free(bob);
    world_dispose(&w);
    puts("OK opt-in NIP-4E grant recipients, verified latest/EOSE, no silent downgrade, device/identity receive");
    return 0;
}
