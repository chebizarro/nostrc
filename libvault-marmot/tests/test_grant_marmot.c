/* SPDX-License-Identifier: MIT */
#include <vault-marmot/vault-marmot.h>
#include <vault/nip59.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-tag.h>
#include <json.h>
#include <openssl/crypto.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    char *json;
    int kind, send_error, receive_error;
    unsigned sends, receives;
} wire;

static int publish(void *ctx, const NostrEvent *event)
{
    wire *w = ctx;
    assert(event && event->kind == w->kind && !w->json);
    assert(nostr_event_validate(event, NULL) == NOSTR_EVENT_VALIDATION_OK);
    w->json = nostr_event_serialize_compact(event);
    assert(w->json && !strstr(w->json, "node_key") && !strstr(w->json, "root_manifest"));
    ++w->sends;
    return w->send_error;
}

static int receive(void *ctx, NostrEvent **event)
{
    wire *w = ctx;
    ++w->receives;
    *event = NULL;
    if (!w->json)
        return VAULT_ERR_AGAIN;
    *event = nostr_event_new();
    assert(*event && nostr_event_deserialize(*event, w->json) == 0);
    free(w->json);
    w->json = NULL;
    return w->receive_error;
}

static void queue(wire *w, const char *json)
{
    assert(!w->json);
    w->json = strdup(json);
    assert(w->json);
}

typedef struct {
    Marmot *m;
    uint8_t sk[32], pk[32];
    MarmotKeyPackageResult kp;
} person;

static void person_init(person *p, uint8_t scalar)
{
    memset(p, 0, sizeof *p);
    p->sk[31] = scalar;
    char sk[65];
    porthome_hex64(p->sk, sk);
    char *pk = nostr_key_get_public(sk);
    assert(pk && porthome_from_hex64(pk, p->pk) == 0);
    free(pk);
    OPENSSL_cleanse(sk, sizeof sk);
    p->m = marmot_new(marmot_storage_memory_new());
    assert(p->m);
    /* Also enrolls the account proof: no unproven or legacy raw mode. */
    assert(marmot_create_key_package(p->m, p->pk, p->sk, NULL, 0, &p->kp) == MARMOT_OK);
}

static void person_free(person *p)
{
    marmot_key_package_result_free(&p->kp);
    marmot_free(p->m);
    OPENSSL_cleanse(p, sizeof *p);
}

static void join(person *p, const char *welcome_json, uint8_t id)
{
    uint8_t wrapper_id[32] = {0};
    wrapper_id[0] = id;
    MarmotWelcome *welcome = NULL;
    assert(marmot_process_welcome(p->m, wrapper_id, welcome_json, &welcome) == MARMOT_OK);
    assert(welcome && marmot_accept_welcome(p->m, welcome) == MARMOT_OK);
    marmot_welcome_free(welcome);
}

static void same_ring(const vault_grant *a, const vault_grant *b)
{
    assert(a && b && a->ring.count == b->ring.count);
    for (size_t i = 0; i < a->ring.count; ++i) {
        assert(a->ring.entries[i].epoch == b->ring.entries[i].epoch);
        assert(!memcmp(a->ring.entries[i].node_key, b->ring.entries[i].node_key, 32));
    }
    char *ja = NULL, *jb = NULL;
    assert(vault_grant_encode(a, &ja) == 0 && vault_grant_encode(b, &jb) == 0);
    assert(!strcmp(ja, jb));
    OPENSSL_cleanse(ja, strlen(ja));
    OPENSSL_cleanse(jb, strlen(jb));
    free(ja);
    free(jb);
}

static void seed_equal(person *a, person *b, const MarmotGroupId *gid, const uint8_t id[32], uint8_t out[32])
{
    uint8_t other[32];
    assert(vault_marmot_seed(a->m, gid, id, out) == MARMOT_OK);
    assert(vault_marmot_seed(b->m, gid, id, other) == MARMOT_OK);
    assert(!memcmp(out, other, 32));
    OPENSSL_cleanse(other, sizeof other);
}

int main(void)
{
    nostr_json_init();
    person alice, bob, eve;
    person_init(&alice, 1);
    person_init(&bob, 2);
    person_init(&eve, 3);
    char *relays[] = {"wss://nos.lol"};
    MarmotGroupConfig config = {.name = "Vault", .admin_pubkeys = &alice.pk,
                                .admin_count = 1, .relay_urls = relays, .relay_count = 1};
    const char *kps[] = {bob.kp.event_json};
    MarmotCreateGroupResult gr = {0};
    assert(marmot_create_group(alice.m, alice.pk, kps, 1, &config, &gr) == MARMOT_OK);
    assert(gr.group && gr.welcome_count == 1);
    const MarmotGroupId *gid = &gr.group->mls_group_id;
    join(&bob, gr.welcome_rumor_jsons[0], 1);

    uint8_t vault_id[32] = {0x45}, seed0[32], seed1[32], seed2[32], key[32];
    seed_equal(&alice, &bob, gid, vault_id, seed0);
    uint8_t other_id[32] = {0x46}, separated[32];
    assert(!strcmp(VAULT_MARMOT_EXPORTER_LABEL, "porthome/v2/vault"));
    assert(vault_marmot_seed(alice.m, gid, other_id, separated) == MARMOT_OK);
    assert(memcmp(seed0, separated, 32));
    memset(separated, 0xff, 32);
    MarmotGroupId missing = {(uint8_t *)"missing", 7};
    assert(vault_marmot_seed(alice.m, &missing, vault_id, separated) != MARMOT_OK);
    assert(!memcmp(separated, (uint8_t[32]){0}, 32));
    assert(vault_marmot_seed(alice.m, gid, NULL, separated) == MARMOT_ERR_INVALID_ARG);
    assert(!memcmp(separated, (uint8_t[32]){0}, 32));

    assert(porthome_root_node_key(seed0, key) == 0);
    vault_grant grant = {.cap = VAULT_CAP_WRITE, .issued_at = (uint64_t)time(NULL),
                         .relays = relays, .relays_len = 1};
    memcpy(grant.owner, alice.pk, 32);
    memcpy(grant.vault_id, vault_id, 32);
    memset(grant.node_id, 0x21, 32);
    memset(grant.root_manifest, 0x31, 32);
    assert(vault_ring_init(&grant.ring, 1, key) == 0);
    uint8_t historical[32] = {0x88};
    uint64_t epoch;
    assert(vault_ring_append(&grant.ring, historical, &epoch) == 0 && epoch == 2);

    wire mw = {.kind = 445}, nw = {.kind = 1059};
    vault_marmot_transport mio = {&mw, publish, receive};
    vault_nip59_transport nio = {&nw, publish, receive};
    vault_marmot_carrier *ma = NULL, *mb = NULL, *wrong = NULL;
    vault_nip59_carrier *na = NULL, *nb = NULL;
    assert(vault_marmot_carrier_new(alice.m, gid, vault_id, &mio, &ma) == 0);
    assert(vault_marmot_carrier_new(bob.m, gid, vault_id, &mio, &mb) == 0);
    assert(vault_marmot_carrier_new(bob.m, gid, other_id, &mio, &wrong) == 0);
    assert(vault_nip59_carrier_new(alice.sk, &nio, &na) == 0);
    assert(vault_nip59_carrier_new(bob.sk, &nio, &nb) == 0);
    const vault_grant_carrier *a = vault_marmot_carrier_interface(ma);
    const vault_grant_carrier *b = vault_marmot_carrier_interface(mb);
    const vault_grant_carrier *fallback = vault_nip59_carrier_interface(na), *chosen = NULL;
    assert(vault_marmot_prefer(VAULT_MARMOT_KP_PRESENT, a, fallback, &chosen) == 0 && chosen == a);
    assert(vault_marmot_prefer(VAULT_MARMOT_KP_ABSENT, a, fallback, &chosen) == 0 && chosen == fallback);
    assert(vault_marmot_prefer(VAULT_MARMOT_KP_UNKNOWN, a, fallback, &chosen) == VAULT_ERR_AGAIN && !chosen);
    assert(vault_marmot_prefer(VAULT_MARMOT_KP_PRESENT, NULL, fallback, &chosen) == VAULT_ERR_ARG && !chosen);
    assert(vault_marmot_prefer(99, a, fallback, &chosen) == VAULT_ERR_ARG && !chosen);
    assert(!mw.sends && !nw.sends);

    vault_grant *gm = NULL, *gn = NULL;
    assert(vault_grant_receive(b, &gm) == VAULT_ERR_AGAIN && !gm);
    assert(vault_grant_send(a, eve.pk, &grant) == VAULT_ERR_AUTH && !mw.sends);
    grant.vault_id[0] ^= 1;
    assert(vault_grant_send(a, bob.pk, &grant) == VAULT_ERR_AUTH && !mw.sends);
    grant.vault_id[0] ^= 1;
    assert(vault_grant_send(b, alice.pk, &grant) != 0 && !mw.sends); /* Not our account. */

    for (int cap = VAULT_CAP_READ; cap <= VAULT_CAP_WRITE; ++cap) {
        grant.cap = cap;
        assert(vault_grant_send(a, bob.pk, &grant) == 0);
        char *replay = strdup(mw.json);
        assert(replay);
        assert(vault_grant_receive(b, &gm) == 0);
        assert(vault_grant_send(fallback, bob.pk, &grant) == 0);
        assert(vault_grant_receive(vault_nip59_carrier_interface(nb), &gn) == 0);
        same_ring(&grant, gm);
        same_ring(gm, gn);
        vault_grant_free(gm);
        vault_grant_free(gn);
        gm = gn = NULL;
        queue(&mw, replay);
        assert(vault_grant_receive(b, &gm) != 0 && !gm); /* No grant on duplicate. */
        free(replay);
    }

    /* Authenticated MLS delivery still cannot grant to a different vault. */
    ++grant.issued_at;
    assert(vault_grant_send(a, bob.pk, &grant) == 0);
    assert(vault_grant_receive(vault_marmot_carrier_interface(wrong), &gm) == VAULT_ERR_AUTH && !gm);

    /* Signed envelope tampering must fail, without retrying or producing a grant. */
    ++grant.issued_at;
    assert(vault_grant_send(a, bob.pk, &grant) == 0);
    NostrEvent *tampered = nostr_event_new();
    assert(tampered && nostr_event_deserialize(tampered, mw.json) == 0);
    free(mw.json);
    mw.json = NULL;
    tampered->content[0] = tampered->content[0] == 'A' ? 'B' : 'A';
    mw.json = nostr_event_serialize_compact(tampered);
    nostr_event_free(tampered);
    unsigned receives = mw.receives;
    assert(vault_grant_receive(b, &gm) == VAULT_ERR_AUTH && !gm && mw.receives == receives + 1);

    /* Uncertain publish/receive failures propagate exactly once; no NIP-59 retry. */
    ++grant.issued_at;
    mw.send_error = VAULT_ERR_IO;
    unsigned sends = mw.sends, fallback_sends = nw.sends;
    assert(vault_grant_send(a, bob.pk, &grant) == VAULT_ERR_IO);
    assert(mw.sends == sends + 1 && nw.sends == fallback_sends);
    mw.send_error = 0;
    mw.receive_error = VAULT_ERR_IO;
    assert(vault_grant_receive(b, &gm) == VAULT_ERR_IO && !gm && !mw.json);
    mw.receive_error = 0;

    ++grant.issued_at;
    assert(vault_grant_send(a, bob.pk, &grant) == 0);
    char *delayed = mw.json;
    mw.json = NULL;

    /* A real Add + merge advances the seed, and Bob applies that signed Commit
     * through the same incoming adapter. No fabricated exporter storage writes. */
    const char *add_kps[] = {eve.kp.event_json};
    char **welcomes = NULL, *commit = NULL;
    size_t welcome_count = 0;
    assert(marmot_add_members(alice.m, gid, add_kps, 1, &welcomes, &welcome_count, &commit) == MARMOT_OK);
    assert(commit && welcome_count == 1);
    assert(vault_marmot_seed(alice.m, gid, vault_id, separated) == MARMOT_OK);
    assert(!memcmp(seed0, separated, 32)); /* A pending Commit is not the current epoch. */
    assert(marmot_merge_pending_commit(alice.m, gid) == MARMOT_OK);
    queue(&mw, commit);
    assert(vault_grant_receive(b, &gm) == VAULT_ERR_AGAIN && !gm);
    join(&eve, welcomes[0], 2);
    free(welcomes[0]);
    free(welcomes);
    free(commit);
    seed_equal(&alice, &bob, gid, vault_id, seed1);
    assert(memcmp(seed0, seed1, 32));
    queue(&mw, delayed);
    free(delayed);
    assert(vault_grant_receive(b, &gm) == VAULT_ERR_AUTH && !gm);
    assert(vault_marmot_seed(eve.m, gid, vault_id, separated) == MARMOT_OK);
    assert(!memcmp(seed1, separated, 32));
    ++grant.issued_at;
    assert(vault_grant_send(a, bob.pk, &grant) == 0 && vault_grant_receive(b, &gm) == 0);
    same_ring(&grant, gm);
    vault_grant_free(gm);
    gm = NULL;

    commit = NULL;
    assert(marmot_remove_members(alice.m, gid, (const uint8_t (*)[32])&eve.pk, 1, &commit) == MARMOT_OK);
    assert(commit && marmot_merge_pending_commit(alice.m, gid) == MARMOT_OK);
    queue(&mw, commit);
    assert(vault_grant_receive(b, &gm) == VAULT_ERR_AGAIN && !gm);
    MarmotMessageResult removed = {0};
    /* The removed member may report eviction rather than a successful result. */
    MarmotError removal_rc = marmot_process_message(eve.m, commit, &removed);
    assert(removal_rc == MARMOT_OK || removal_rc == MARMOT_ERR_USE_AFTER_EVICTION);
    marmot_message_result_free(&removed);
    free(commit);
    seed_equal(&alice, &bob, gid, vault_id, seed2);
    assert(memcmp(seed1, seed2, 32) && memcmp(seed0, seed2, 32));
    assert(vault_grant_send(a, eve.pk, &grant) == VAULT_ERR_AUTH);
    assert(vault_marmot_seed(eve.m, gid, vault_id, separated) != MARMOT_OK);
    assert(!memcmp(separated, (uint8_t[32]){0}, 32));
    ++grant.issued_at;
    assert(vault_grant_send(a, bob.pk, &grant) == 0 && vault_grant_receive(b, &gm) == 0);
    same_ring(&grant, gm);
    vault_grant_free(gm);

    /* The MLS sender's account may have multiple device leaves. A proven
     * leaf must not lend its proof to another, unproven leaf of that account. */
    MarmotConfig legacy = marmot_config_default();
    legacy.allow_unproven_self = true;
    Marmot *unproven = marmot_new_with_config(marmot_storage_memory_new(), &legacy);
    assert(unproven);
    MarmotKeyPackageResult legacy_kp = {0};
    assert(marmot_create_key_package_unsigned(unproven, alice.pk, NULL, 0, &legacy_kp) == MARMOT_OK);
    NostrEvent *legacy_event = nostr_event_new();
    assert(legacy_event && nostr_event_deserialize_compact(legacy_event, legacy_kp.event_json, NULL));
    char alice_sk[65];
    porthome_hex64(alice.sk, alice_sk);
    /* Explicit legacy admission fixture: the account signed the KeyPackage
     * event but this device leaf has no account-identity proof of its own. */
    assert(nostr_event_sign(legacy_event, alice_sk) == 0);
    OPENSSL_cleanse(alice_sk, sizeof alice_sk);
    free(legacy_kp.event_json);
    legacy_kp.event_json = nostr_event_serialize_compact(legacy_event);
    nostr_event_free(legacy_event);
    const char *legacy_kps[] = {legacy_kp.event_json};
    welcomes = NULL;
    welcome_count = 0;
    commit = NULL;
    assert(marmot_add_members(alice.m, gid, legacy_kps, 1, &welcomes, &welcome_count, &commit) == MARMOT_OK);
    assert(commit && welcome_count == 1 && marmot_merge_pending_commit(alice.m, gid) == MARMOT_OK);
    queue(&mw, commit);
    gm = NULL;
    assert(vault_grant_receive(b, &gm) == VAULT_ERR_AGAIN && !gm);
    person legacy_person = {.m = unproven};
    join(&legacy_person, welcomes[0], 3);
    free(welcomes[0]);
    free(welcomes);
    free(commit);
    MarmotMemberIdentity *identities = NULL;
    size_t identity_count = 0, proven_count = 0, unproven_count = 0;
    assert(marmot_get_group_member_identities(bob.m, gid, &identities, &identity_count) == MARMOT_OK);
    for (size_t i = 0; i < identity_count; ++i)
        if (!memcmp(identities[i].account_pubkey, alice.pk, 32)) {
            proven_count += identities[i].status == MARMOT_MEMBER_IDENTITY_PROVEN;
            unproven_count += identities[i].status == MARMOT_MEMBER_IDENTITY_UNPROVEN;
        }
    assert(proven_count == 1 && unproven_count == 1);
    free(identities);
    ++grant.issued_at;
    NostrEvent *rumor = NULL;
    assert(vault_grant_rumor_new(&grant, &rumor) == 0);
    char *inner = nostr_event_serialize_compact(rumor);
    assert(inner);
    vault_grant_rumor_free(rumor);
    MarmotOutgoingMessage legacy_msg = {0};
    assert(marmot_create_message(unproven, gid, inner, &legacy_msg) == MARMOT_OK);
    OPENSSL_cleanse(inner, strlen(inner));
    free(inner);
    queue(&mw, legacy_msg.event_json);
    if (legacy_msg.message) {
        if (legacy_msg.message->content)
            OPENSSL_cleanse(legacy_msg.message->content, strlen(legacy_msg.message->content));
        if (legacy_msg.message->event_json)
            OPENSSL_cleanse(legacy_msg.message->event_json, strlen(legacy_msg.message->event_json));
    }
    marmot_outgoing_message_free(&legacy_msg);
    assert(vault_grant_receive(b, &gm) == VAULT_ERR_AUTH && !gm);
    sends = mw.sends;
    assert(vault_grant_send(a, bob.pk, &grant) == VAULT_ERR_AUTH && mw.sends == sends);
    marmot_key_package_result_free(&legacy_kp);
    marmot_free(unproven);

    vault_marmot_carrier_free(ma);
    vault_marmot_carrier_free(mb);
    vault_marmot_carrier_free(wrong);
    vault_nip59_carrier_free(na);
    vault_nip59_carrier_free(nb);
    vault_ring_dispose(&grant.ring);
    marmot_create_group_result_free(&gr);
    person_free(&alice);
    person_free(&bob);
    person_free(&eve);
    OPENSSL_cleanse(key, sizeof key);
    OPENSSL_cleanse(seed0, sizeof seed0);
    OPENSSL_cleanse(seed1, sizeof seed1);
    OPENSSL_cleanse(seed2, sizeof seed2);
    OPENSSL_cleanse(separated, sizeof separated);
    assert(!mw.json && !nw.json);
    puts("test_vault_grant_marmot: OK (real MLS Add/Remove, Marmot and NIP-59)");
    return 0;
}
