/*
 * libmarmot tests: KeyPackage transport lifecycle (nostrc-0bdg;
 * foundation/key-packages.md "Selection and lifecycle", "Failure behavior";
 * transports/nostr.md "KeyPackage publication").
 *
 * Real KeyPackages, groups and Welcomes between in-memory instances:
 * rotation keeps the `d` slot; old private material goes only when a newer
 * KeyPackage is confirmed published; a delayed Welcome to the old
 * last-resort KeyPackage opens until then and not after; a consumed
 * single-use KeyPackage goes on a successful join; a failed Welcome keeps
 * everything; the Lifetime ends it; accounts from before the record are
 * seeded; the record is bounded; the adopted producer works with an
 * enrolled proof only (signer-only callers); mls_ciphersuite is a singleton.
 *
 * SPDX-License-Identifier: MIT
 */

#include <marmot/marmot.h>
#include "marmot-internal.h"
#include "kp_profile.h"
#include "test_enroll.h"
#include <nostr-event.h>
#include <nostr-tag.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "\n  FAIL %s:%d: ", __FILE__, __LINE__);        \
            fprintf(stderr, __VA_ARGS__);                                   \
            fprintf(stderr, "\n");                                          \
            abort();                                                        \
        }                                                                   \
    } while (0)

#define OK(expr)                                                            \
    do {                                                                    \
        MarmotError e_ = (expr);                                            \
        CHECK(e_ == MARMOT_OK, "%s -> %d (%s)", #expr, e_,                  \
              marmot_error_string(e_));                                     \
    } while (0)

#define EXPECT_ERR(expr, want)                                              \
    do {                                                                    \
        int e_ = (expr);                                                    \
        CHECK(e_ == (want), "%s -> %d (%s), want %d (%s)", #expr, e_,       \
              marmot_error_string((MarmotError)e_), (int)(want),            \
              marmot_error_string((MarmotError)(want)));                    \
    } while (0)

#define RUN(fn)                                                             \
    do {                                                                    \
        printf("  %-52s", #fn); fflush(stdout); fn(); printf("PASS\n");    \
    } while (0)

/* ── Members ──────────────────────────────────────────────────────────── */

typedef struct {
    Marmot  *m;
    uint8_t  sk[32], pk[32];
    char     sk_hex[65];
} Member;

static void
member_init(Member *x)
{
    memset(x, 0, sizeof(*x));
    x->m = marmot_new(marmot_storage_memory_new());
    CHECK(x->m, "marmot_new");
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    do randombytes_buf(x->sk, 32); while (!secp256k1_ec_seckey_verify(ctx, x->sk));
    secp256k1_keypair kp;
    secp256k1_xonly_pubkey xonly;
    CHECK(secp256k1_keypair_create(ctx, &kp, x->sk) &&
          secp256k1_keypair_xonly_pub(ctx, &xonly, NULL, &kp) &&
          secp256k1_xonly_pubkey_serialize(ctx, x->pk, &xonly), "keypair");
    secp256k1_context_destroy(ctx);
    for (int i = 0; i < 32; i++) snprintf(x->sk_hex + 2 * i, 3, "%02x", x->sk[i]);
    OK(test_enroll(x->m, x->pk, x->sk));
}

static void
member_free(Member *x)
{
    marmot_free(x->m);
    sodium_memzero(x->sk, sizeof(x->sk));
}

/* A KeyPackage of @x: the signed kind:30443 event and its ref. */
typedef struct {
    char    *json;
    uint8_t  ref[32];
} Kp;

static Kp
legacy_kp(Member *x)
{
    static const char *relays[] = {"wss://write.example.com"};
    MarmotKeyPackageResult r;
    memset(&r, 0, sizeof(r));
    OK(marmot_create_key_package(x->m, x->pk, x->sk, relays, 1, &r));
    Kp kp = {strdup(r.event_json), {0}};
    memcpy(kp.ref, r.key_package_ref, 32);
    marmot_key_package_result_free(&r);
    return kp;
}

static Kp
adopted_kp(Member *x, bool last_resort)
{
    MarmotKeyPackageResult r;
    memset(&r, 0, sizeof(r));
    OK(marmot_create_key_package_adopted_internal_ex(x->m, x->pk, x->sk, NULL, NULL,
                                                     last_resort, &r));
    Kp kp = {strdup(r.event_json), {0}};
    memcpy(kp.ref, r.key_package_ref, 32);
    marmot_key_package_result_free(&r);
    return kp;
}

static bool
has_key(Member *x, const Kp *kp)
{
    bool present = false;
    OK(marmot_key_package_has_private_key(x->m, kp->ref, &present));
    return present;
}

static char *
tag_value(const char *json, const char *key)
{
    NostrEvent *ev = nostr_event_new();
    CHECK(nostr_event_deserialize_compact(ev, json, NULL), "parse");
    char *out = NULL;
    for (size_t i = 0; i < nostr_tags_size(ev->tags) && !out; i++) {
        NostrTag *t = nostr_tags_get(ev->tags, i);
        if (nostr_tag_size(t) >= 2 && strcmp(nostr_tag_get_key(t), key) == 0)
            out = strdup(nostr_tag_get(t, 1));
    }
    nostr_event_free(ev);
    return out;
}

static int64_t
created_at_of(const char *json)
{
    NostrEvent *ev = nostr_event_new();
    CHECK(nostr_event_deserialize_compact(ev, json, NULL), "parse");
    int64_t t = nostr_event_get_created_at(ev);
    nostr_event_free(ev);
    return t;
}

/* @inviter makes a group with @invitee's KeyPackage; the Welcome rumor. */
static char *
invite(Member *inviter, const Kp *kp, bool adopted)
{
    const char *relays[] = {"wss://group.example.com"};
    MarmotGroupConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "Lifecycle";
    cfg.relay_urls = (char **)relays;
    cfg.relay_count = 1;
    MarmotCreateGroupResult r;
    memset(&r, 0, sizeof(r));
    const char *kps[] = {kp->json};
    if (adopted)
        OK(marmot_create_group_for_profile(inviter->m, MARMOT_GROUP_PROFILE_ADOPTED, inviter->pk,
                                           inviter->sk, NULL, NULL, kps, 1, &cfg, &r));
    else
        OK(marmot_create_group(inviter->m, inviter->pk, kps, 1, &cfg, &r));
    CHECK(r.welcome_count == 1, "one Welcome");
    char *rumor = strdup(r.welcome_rumor_jsons[0]);
    marmot_create_group_result_free(&r);
    return rumor;
}

/* The invitee receives @rumor (a fresh gift wrap) and accepts it. */
static MarmotError
join(Member *x, const char *rumor)
{
    uint8_t wrapper[32];
    randombytes_buf(wrapper, sizeof(wrapper));
    MarmotWelcome *w = NULL;
    MarmotError err = marmot_process_welcome(x->m, wrapper, rumor, &w);
    if (err == MARMOT_OK) err = marmot_accept_welcome(x->m, w);
    marmot_welcome_free(w);
    return err;
}

/* @rumor with one byte of its MLS Welcome flipped (the encrypted
 * GroupInfo's last byte: the AEAD fails). */
static char *
corrupt_rumor(const char *rumor)
{
    NostrEvent *ev = nostr_event_new();
    CHECK(nostr_event_deserialize_compact(ev, rumor, NULL), "parse rumor");
    size_t clen = strlen(ev->content), len = 0;
    uint8_t *raw = malloc(clen);
    CHECK(raw && sodium_base642bin(raw, clen, ev->content, clen, NULL, &len, NULL,
                                   sodium_base64_VARIANT_ORIGINAL) == 0 && len > 8,
          "base64 Welcome");
    raw[len - 1] ^= 0x5a;
    size_t b64_len = sodium_base64_ENCODED_LEN(len, sodium_base64_VARIANT_ORIGINAL);
    char *b64 = malloc(b64_len);
    sodium_bin2base64(b64, b64_len, raw, len, sodium_base64_VARIANT_ORIGINAL);
    nostr_event_set_content(ev, b64);
    char *out = nostr_event_serialize_compact(ev);
    free(b64);
    free(raw);
    nostr_event_free(ev);
    return out;
}

/* ── Tests ────────────────────────────────────────────────────────────── */

/* Rotation reuses the slot; the old private material stays until a relay
 * accepted the replacement, then goes; the replacement stays. */
static void
test_rotation_ack_tied(void)
{
    Member bob;
    member_init(&bob);
    Kp k1 = legacy_kp(&bob);
    OK(marmot_key_package_confirm_published(bob.m, bob.pk, k1.ref));
    Kp k2 = legacy_kp(&bob);
    char *d1 = tag_value(k1.json, "d"), *d2 = tag_value(k2.json, "d");
    CHECK(d1 && d2 && strcmp(d1, d2) == 0, "the replacement reuses the d slot");
    /* Made within the same second: still strictly newer in the slot (an
     * equal created_at would fall back to the lower event id). */
    CHECK(created_at_of(k2.json) > created_at_of(k1.json), "the replacement is newer");
    CHECK(has_key(&bob, &k1) && has_key(&bob, &k2), "both until a relay accepts k2");
    OK(marmot_key_package_confirm_published(bob.m, bob.pk, k2.ref));
    CHECK(!has_key(&bob, &k1), "k1 deleted on k2's confirmation");
    MarmotKeyPackageInfo *info = NULL;
    OK(bob.m->storage->find_key_package_by_ref(bob.m->storage->ctx, k1.ref, &info));
    CHECK(info == NULL, "k1's info row deleted with it (no rotation history)");
    OK(bob.m->storage->find_key_package_by_ref(bob.m->storage->ctx, k2.ref, &info));
    CHECK(info != NULL, "k2's info row kept");
    marmot_key_package_info_free(info);
    CHECK(has_key(&bob, &k2), "k2 kept");
    /* Idempotent; an unknown or retired ref is not confirmable. */
    OK(marmot_key_package_confirm_published(bob.m, bob.pk, k2.ref));
    CHECK(has_key(&bob, &k2), "k2 still kept");
    EXPECT_ERR(marmot_key_package_confirm_published(bob.m, bob.pk, k1.ref),
               MARMOT_ERR_KEY_NOT_FOUND);
    free(d1); free(d2); free(k1.json); free(k2.json);
    member_free(&bob);
}

/* A replacement whose publish failed (never confirmed) keeps the current
 * one; a later confirmed one retires both. A late OK of an older one
 * leaves a newer pending one alone. */
static void
test_unconfirmed_replacement(void)
{
    Member bob;
    member_init(&bob);
    Kp k1 = legacy_kp(&bob);
    OK(marmot_key_package_confirm_published(bob.m, bob.pk, k1.ref));
    Kp k2 = legacy_kp(&bob);    /* its publish fails: no confirmation */
    Kp k3 = legacy_kp(&bob);
    CHECK(has_key(&bob, &k1) && has_key(&bob, &k2) && has_key(&bob, &k3), "all kept");
    OK(marmot_key_package_confirm_published(bob.m, bob.pk, k3.ref));
    CHECK(!has_key(&bob, &k1) && !has_key(&bob, &k2) && has_key(&bob, &k3),
          "k3's confirmation retires k1 and the abandoned k2");

    Kp k4 = legacy_kp(&bob);
    Kp k5 = legacy_kp(&bob);
    OK(marmot_key_package_confirm_published(bob.m, bob.pk, k4.ref)); /* a late OK */
    CHECK(!has_key(&bob, &k3) && has_key(&bob, &k4) && has_key(&bob, &k5),
          "a newer pending KeyPackage survives an older one's confirmation");
    free(k1.json); free(k2.json); free(k3.json); free(k4.json); free(k5.json);
    member_free(&bob);
}

/* Last resort: a Welcome joins and the key survives; a delayed Welcome to
 * it still opens while the replacement is unconfirmed; after the confirmed
 * replacement one no longer does (the spec's trade-off). */
static void
test_last_resort_delayed_welcome(void)
{
    Member bob, alice, carol, dave;
    member_init(&bob);
    member_init(&alice);
    member_init(&carol);
    member_init(&dave);
    Kp k1 = legacy_kp(&bob);
    OK(marmot_key_package_confirm_published(bob.m, bob.pk, k1.ref));
    /* Three inviters fetched k1. */
    char *from_alice = invite(&alice, &k1, false);
    char *from_carol = invite(&carol, &k1, false);
    char *from_dave = invite(&dave, &k1, false);

    OK(join(&bob, from_alice));
    CHECK(has_key(&bob, &k1), "a consumed last-resort KeyPackage survives the join");

    /* Bob rotates; the replacement is not confirmed yet. */
    Kp k2 = legacy_kp(&bob);
    OK(join(&bob, from_carol));
    CHECK(has_key(&bob, &k1), "the old key is available within its window");

    OK(marmot_key_package_confirm_published(bob.m, bob.pk, k2.ref));
    CHECK(!has_key(&bob, &k1), "deleted at the confirmed replacement");
    EXPECT_ERR(join(&bob, from_dave), MARMOT_ERR_KEY_NOT_FOUND);
    CHECK(has_key(&bob, &k2), "the failed Welcome leaves the current KeyPackage");

    free(from_alice); free(from_carol); free(from_dave);
    free(k1.json); free(k2.json);
    member_free(&bob); member_free(&alice); member_free(&carol); member_free(&dave);
}

/* A consumed single-use (non-last-resort) KeyPackage: its private
 * material goes with the successful join; a second Welcome to it fails. */
static void
test_single_use_consumed(void)
{
    Member bob, alice, carol;
    member_init(&bob);
    member_init(&alice);
    member_init(&carol);
    Kp once = adopted_kp(&bob, false);
    OK(marmot_key_package_confirm_published(bob.m, bob.pk, once.ref));
    char *first = invite(&alice, &once, true);
    char *second = invite(&carol, &once, true);
    CHECK(has_key(&bob, &once), "kept before any Welcome");
    OK(join(&bob, first));
    CHECK(!has_key(&bob, &once), "a consumed single-use init key is deleted after the join");
    /* An adopted Welcome is opened on arrival: refused there. */
    EXPECT_ERR(join(&bob, second), MARMOT_ERR_KEY_NOT_FOUND);

    /* The same with a last-resort adopted KeyPackage: it survives. */
    Member dave;
    member_init(&dave);
    Kp lr = adopted_kp(&bob, true);
    char *third = invite(&dave, &lr, true);
    OK(join(&bob, third));
    CHECK(has_key(&bob, &lr), "an adopted last-resort KeyPackage survives the join");
    free(first); free(second); free(third); free(once.json); free(lr.json);
    member_free(&bob); member_free(&alice); member_free(&carol); member_free(&dave);
}

/* Failed Welcome processing changes nothing: the key stays and the
 * inviter's retry joins. */
static void
test_failed_welcome_preserves(void)
{
    Member bob, alice;
    member_init(&bob);
    member_init(&alice);
    Kp once = adopted_kp(&bob, false);
    Kp lr = legacy_kp(&bob);
    /* lr is newer and confirmed only later: confirm nothing yet. */
    char *good = invite(&alice, &lr, false);
    char *bad = corrupt_rumor(good);
    CHECK(join(&bob, bad) != MARMOT_OK, "the corrupted Welcome fails");
    CHECK(has_key(&bob, &lr) && has_key(&bob, &once), "every key kept after the failure");
    OK(join(&bob, good));
    CHECK(has_key(&bob, &lr), "the retry joins with the same KeyPackage");

    /* The same for a single-use KeyPackage. */
    Member carol;
    member_init(&carol);
    char *good2 = invite(&carol, &once, true);
    char *bad2 = corrupt_rumor(good2);
    CHECK(join(&bob, bad2) != MARMOT_OK, "the corrupted adopted Welcome fails");
    CHECK(has_key(&bob, &once), "a single-use key survives a failed Welcome");
    OK(join(&bob, good2));
    CHECK(!has_key(&bob, &once), "and goes with the successful one");
    free(good); free(bad); free(good2); free(bad2); free(once.json); free(lr.json);
    member_free(&bob); member_free(&alice); member_free(&carol);
}

/* The Lifetime bound: at not_after the private material goes in any
 * state, current and confirmed included. */
static void
test_lifetime_sweep(void)
{
    Member bob;
    member_init(&bob);
    Kp k1 = legacy_kp(&bob);
    OK(marmot_key_package_confirm_published(bob.m, bob.pk, k1.ref));
    MlsKeyPackage kp;
    uint8_t owner[32];
    OK(marmot_parse_key_package_event_for_profile(k1.json, MARMOT_KEY_PACKAGE_PROFILE_MDK_0_8,
                                                  0, &kp, owner));
    int64_t not_after = (int64_t)kp.leaf_node.lifetime_not_after;
    mls_key_package_clear(&kp);
    int64_t next = 0;
    OK(marmot_key_package_next_expiry(bob.m, bob.pk, &next));
    CHECK(next == not_after, "next expiry is the KeyPackage's not_after");
    size_t n = 99;
    OK(marmot_key_package_sweep_expired(bob.m, bob.pk, not_after - 1, &n));
    CHECK(n == 0 && has_key(&bob, &k1), "kept before not_after");
    OK(marmot_key_package_sweep_expired(bob.m, bob.pk, not_after, &n));
    CHECK(n == 1 && !has_key(&bob, &k1), "deleted at not_after");
    OK(marmot_key_package_next_expiry(bob.m, bob.pk, &next));
    CHECK(next == 0, "nothing left to expire");
    free(k1.json);
    member_free(&bob);
}

/* KeyPackages made before the lifecycle record existed (libmarmot < 0.12)
 * are seeded from the store: a confirmed replacement retires them. */
static void
test_seeded_from_store(void)
{
    Member bob;
    member_init(&bob);
    Kp k1 = legacy_kp(&bob);
    Kp k2 = legacy_kp(&bob);
    OK(bob.m->storage->mls_delete(bob.m->storage->ctx, "kp_life", bob.pk, 32));
    OK(marmot_key_package_confirm_published(bob.m, bob.pk, k2.ref));
    CHECK(!has_key(&bob, &k1) && has_key(&bob, &k2), "the older one retired");
    /* Seeded again for the next one too. */
    OK(bob.m->storage->mls_delete(bob.m->storage->ctx, "kp_life", bob.pk, 32));
    Kp k3 = legacy_kp(&bob);
    OK(marmot_key_package_confirm_published(bob.m, bob.pk, k3.ref));
    CHECK(!has_key(&bob, &k2) && has_key(&bob, &k3), "seeded at creation");
    free(k1.json); free(k2.json); free(k3.json);
    member_free(&bob);
}

/* The record is bounded: a long run of unconfirmed publishes drops the
 * oldest, never the confirmed current one. */
static void
test_bounded(void)
{
    Member bob;
    member_init(&bob);
    Kp current = legacy_kp(&bob);
    OK(marmot_key_package_confirm_published(bob.m, bob.pk, current.ref));
    Kp first = legacy_kp(&bob);
    for (int i = 0; i < 40; i++) {
        Kp k = legacy_kp(&bob);
        free(k.json);
    }
    CHECK(has_key(&bob, &current), "the confirmed current KeyPackage kept");
    CHECK(!has_key(&bob, &first), "the oldest unconfirmed one dropped");
    free(current.json); free(first.json);
    member_free(&bob);
}

/* A signer-only caller (no account secret, no synchronous signer) makes
 * an adopted KeyPackage with its enrolled proof, as Groundhog does. */
static void
test_adopted_enrolled_producer(void)
{
    Member bob;
    member_init(&bob);
    MarmotKeyPackageResult r;
    memset(&r, 0, sizeof(r));
    OK(marmot_create_key_package_adopted_internal(bob.m, bob.pk, NULL, NULL, NULL, &r));
    NostrEvent *ev = nostr_event_new();
    CHECK(nostr_event_deserialize_compact(ev, r.event_json, NULL), "parse");
    CHECK(nostr_event_sign(ev, bob.sk_hex) == 0, "sign as the signer would");
    char *signed_json = nostr_event_serialize_compact(ev);
    nostr_event_free(ev);
    uint8_t ref[32];
    OK(marmot_validate_key_package_event_json(signed_json, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED, 0,
                                              NULL, ref));
    CHECK(memcmp(ref, r.key_package_ref, 32) == 0, "ref");
    free(signed_json);
    marmot_key_package_result_free(&r);

    Marmot *bare = marmot_new(marmot_storage_memory_new());
    EXPECT_ERR(marmot_create_key_package_adopted_internal(bare, bob.pk, NULL, NULL, NULL, &r),
               MARMOT_ERR_INVALID_ARG);
    marmot_free(bare);
    member_free(&bob);
}

/* transports/nostr.md: mls_ciphersuite is exactly one id-list tag naming
 * the KeyPackage's one suite. */
static void
test_ciphersuite_singleton(void)
{
    Member bob;
    member_init(&bob);
    Kp k = adopted_kp(&bob, true);
    OK(marmot_validate_key_package_event_json(k.json, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED, 0, NULL,
                                              NULL));
    NostrEvent *ev = nostr_event_new();
    CHECK(nostr_event_deserialize_compact(ev, k.json, NULL), "parse");
    for (size_t i = 0; i < nostr_tags_size(ev->tags); i++) {
        NostrTag *t = nostr_tags_get(ev->tags, i);
        if (strcmp(nostr_tag_get_key(t), "mls_ciphersuite") == 0) nostr_tag_append(t, "0x0002");
    }
    free(ev->id);
    ev->id = NULL;
    CHECK(nostr_event_sign(ev, bob.sk_hex) == 0, "re-sign");
    char *two = nostr_event_serialize_compact(ev);
    nostr_event_free(ev);
    EXPECT_ERR(marmot_validate_key_package_event_json(two, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED, 0,
                                                      NULL, NULL),
               MARMOT_ERR_VALIDATION);
    free(two); free(k.json);
    member_free(&bob);
}

/* @json with @value appended to (or, NULL, the last value dropped from) its
 * one @key tag, re-signed by @x. */
static char *
retag(Member *x, const char *json, const char *key, const char *value)
{
    NostrEvent *ev = nostr_event_new();
    CHECK(nostr_event_deserialize_compact(ev, json, NULL), "parse");
    NostrTags *tags = nostr_tags_new(0);
    for (size_t i = 0; i < nostr_tags_size(ev->tags); i++) {
        NostrTag *t = nostr_tags_get(ev->tags, i);
        bool hit = strcmp(nostr_tag_get_key(t), key) == 0;
        size_t n = nostr_tag_size(t) - (hit && !value ? 1 : 0);
        NostrTag *c = nostr_tag_new(nostr_tag_get_key(t), NULL);
        for (size_t j = 1; j < n; j++) nostr_tag_append(c, nostr_tag_get(t, j));
        if (hit && value) nostr_tag_append(c, value);
        nostr_tags_append(tags, c);
    }
    nostr_event_set_tags(ev, tags);
    free(ev->id);
    ev->id = NULL;
    CHECK(nostr_event_sign(ev, x->sk_hex) == 0, "re-sign");
    char *out = nostr_event_serialize_compact(ev);
    nostr_event_free(ev);
    return out;
}

/* Review L4 (MDK 0.11 key_package_records.rs): mls_extensions and
 * mls_proposals name exactly the decoded leaf's capabilities. */
static void
test_capability_tags_match_leaf(void)
{
    Member bob;
    member_init(&bob);
    Kp k = adopted_kp(&bob, true);
    char *extra_ext = retag(&bob, k.json, "mls_extensions", "0x0007");
    char *extra_prop = retag(&bob, k.json, "mls_proposals", "0x0007");
    char *less_prop = retag(&bob, k.json, "mls_proposals", NULL);
    EXPECT_ERR(marmot_validate_key_package_event_json(extra_ext, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED,
                                                      0, NULL, NULL), MARMOT_ERR_VALIDATION);
    EXPECT_ERR(marmot_validate_key_package_event_json(extra_prop,
                                                      MARMOT_KEY_PACKAGE_PROFILE_ADOPTED, 0,
                                                      NULL, NULL), MARMOT_ERR_VALIDATION);
    EXPECT_ERR(marmot_validate_key_package_event_json(less_prop, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED,
                                                      0, NULL, NULL), MARMOT_ERR_VALIDATION);
    /* Same set in another order: still valid (compared as sets). */
    NostrEvent *ev = nostr_event_new();
    CHECK(nostr_event_deserialize_compact(ev, k.json, NULL), "parse");
    for (size_t i = 0; i < nostr_tags_size(ev->tags); i++) {
        NostrTag *t = nostr_tags_get(ev->tags, i);
        if (strcmp(nostr_tag_get_key(t), "mls_proposals") != 0 || nostr_tag_size(t) < 3) continue;
        NostrTag *c = nostr_tag_new("mls_proposals", NULL);
        for (size_t j = nostr_tag_size(t) - 1; j >= 1; j--) nostr_tag_append(c, nostr_tag_get(t, j));
        nostr_tags_set(ev->tags, i, c);   /* does not free the old tag */
        nostr_tag_free(t);
    }
    free(ev->id);
    ev->id = NULL;
    CHECK(nostr_event_sign(ev, bob.sk_hex) == 0, "re-sign");
    char *reordered = nostr_event_serialize_compact(ev);
    nostr_event_free(ev);
    OK(marmot_validate_key_package_event_json(reordered, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED, 0,
                                              NULL, NULL));
    free(extra_ext); free(extra_prop); free(less_prop); free(reordered); free(k.json);
    member_free(&bob);
}

/* Review A2: GREASE extension ids of the leaf are not expected in the
 * mls_extensions tag (as MDK filters them); proposals are compared whole. */
static void
test_grease_skipped(void)
{
    CHECK(marmot_mls_is_grease(0x0A0A) && marmot_mls_is_grease(0x1A1A) &&
          marmot_mls_is_grease(0xEAEA), "GREASE values");
    CHECK(!marmot_mls_is_grease(0xFAFA) && !marmot_mls_is_grease(0x0A1A) &&
          !marmot_mls_is_grease(0x0006) && !marmot_mls_is_grease(0x000A), "not GREASE");
    NostrTags *tags = nostr_tags_new(0);
    nostr_tags_append(tags, nostr_tag_new("mls_extensions", "0x0006", NULL));
    const uint16_t leaf[] = {0x0006, 0x2A2A};
    CHECK(marmot_kp_id_list_tag_is_set(tags, "mls_extensions", leaf, 2, true),
          "a GREASE leaf extension is not expected in the tag");
    CHECK(!marmot_kp_id_list_tag_is_set(tags, "mls_extensions", leaf, 2, false),
          "unless GREASE is compared (proposals)");
    const uint16_t other[] = {0x0006, 0x0007};
    CHECK(!marmot_kp_id_list_tag_is_set(tags, "mls_extensions", other, 2, true),
          "a real extra extension still mismatches");
    nostr_tags_free(tags);
}

/* Two wire profiles must not replace one another's relay slot or retire
 * one another's init keys when their own replacement is acknowledged. */
/* N1 (W25 review): a join records the profile of the KeyPackage it spent. */
static void
test_last_used_profile(void)
{
    Member alice, bob;
    member_init(&alice);
    member_init(&bob);
    MarmotKeyPackageProfile used = MARMOT_KEY_PACKAGE_PROFILE_ADOPTED;
    CHECK(marmot_key_package_last_used_profile(bob.m, bob.pk, &used) ==
          MARMOT_ERR_STORAGE_NOT_FOUND, "no join yet: unknown");
    Kp legacy = legacy_kp(&bob);
    Kp adopted = adopted_kp(&bob, true);
    char *rumor = invite(&alice, &legacy, false);
    OK(join(&bob, rumor));
    free(rumor);
    OK(marmot_key_package_last_used_profile(bob.m, bob.pk, &used));
    CHECK(used == MARMOT_KEY_PACKAGE_PROFILE_MDK_0_8, "the legacy KeyPackage was spent");
    rumor = invite(&alice, &adopted, true);
    OK(join(&bob, rumor));
    free(rumor);
    OK(marmot_key_package_last_used_profile(bob.m, bob.pk, &used));
    CHECK(used == MARMOT_KEY_PACKAGE_PROFILE_ADOPTED, "the adopted KeyPackage was spent");
    free(legacy.json);
    free(adopted.json);
    member_free(&alice);
    member_free(&bob);
}

static void
test_profiles_have_independent_slots(void)
{
    Member bob;
    member_init(&bob);
    Kp legacy1 = legacy_kp(&bob);
    Kp adopted1 = adopted_kp(&bob, true);
    char *legacy_slot = tag_value(legacy1.json, "d");
    char *adopted_slot = tag_value(adopted1.json, "d");
    CHECK(legacy_slot && adopted_slot && strcmp(legacy_slot, adopted_slot) != 0,
          "profiles need distinct relay replacement slots");
    OK(marmot_key_package_confirm_published(bob.m, bob.pk, legacy1.ref));
    OK(marmot_key_package_confirm_published(bob.m, bob.pk, adopted1.ref));
    CHECK(has_key(&bob, &legacy1) && has_key(&bob, &adopted1),
          "confirming either profile preserves the other");
    /* Each slot's own expiry: what a caller holds back per profile. */
    int64_t legacy_exp = 0, adopted_exp = 0, any_exp = 0;
    OK(marmot_key_package_next_expiry_for_profile(bob.m, bob.pk,
                                                  MARMOT_KEY_PACKAGE_PROFILE_MDK_0_8,
                                                  &legacy_exp));
    OK(marmot_key_package_next_expiry_for_profile(bob.m, bob.pk,
                                                  MARMOT_KEY_PACKAGE_PROFILE_ADOPTED,
                                                  &adopted_exp));
    OK(marmot_key_package_next_expiry(bob.m, bob.pk, &any_exp));
    CHECK(legacy_exp > 0 && adopted_exp > 0 &&
          any_exp == (legacy_exp < adopted_exp ? legacy_exp : adopted_exp),
          "per-profile expiries, the earliest overall");
    int64_t none = -1;
    CHECK(marmot_key_package_next_expiry_for_profile(bob.m, bob.pk, (MarmotKeyPackageProfile)7,
                                                     &none) == MARMOT_ERR_INVALID_ARG &&
          none == 0, "unknown profile refused");
    Kp legacy2 = legacy_kp(&bob);
    Kp adopted2 = adopted_kp(&bob, true);
    char *legacy_slot2 = tag_value(legacy2.json, "d");
    char *adopted_slot2 = tag_value(adopted2.json, "d");
    CHECK(strcmp(legacy_slot, legacy_slot2) == 0 &&
          strcmp(adopted_slot, adopted_slot2) == 0, "both slots stay stable");
    OK(marmot_key_package_confirm_published(bob.m, bob.pk, legacy2.ref));
    CHECK(!has_key(&bob, &legacy1) && has_key(&bob, &adopted1),
          "legacy ACK retires only legacy");
    OK(marmot_key_package_confirm_published(bob.m, bob.pk, adopted2.ref));
    CHECK(!has_key(&bob, &adopted1) && has_key(&bob, &legacy2),
          "adopted ACK retires only adopted");
    /* The slot ids, as a deletion request would address them. */
    char d_hex[65];
    OK(marmot_key_package_slot(bob.m, bob.pk, MARMOT_KEY_PACKAGE_PROFILE_MDK_0_8, d_hex));
    CHECK(strcmp(d_hex, legacy_slot) == 0, "legacy slot id");
    OK(marmot_key_package_slot(bob.m, bob.pk, MARMOT_KEY_PACKAGE_PROFILE_ADOPTED, d_hex));
    CHECK(strcmp(d_hex, adopted_slot) == 0, "adopted slot id");
    /* N3: one active KeyPackage per profile, each the newest of its slot. */
    {
        MarmotKeyPackageInfo **infos = NULL;
        size_t n = 0;
        OK(bob.m->storage->find_key_packages_by_pubkey(bob.m->storage->ctx, bob.pk, &infos, &n));
        size_t active = 0;
        bool legacy_active = false, adopted_active = false;
        for (size_t i = 0; i < n; i++) {
            if (!infos[i]->active) continue;
            active++;
            legacy_active |= memcmp(infos[i]->ref, legacy2.ref, 32) == 0;
            adopted_active |= memcmp(infos[i]->ref, adopted2.ref, 32) == 0;
            marmot_key_package_info_free(infos[i]);
            infos[i] = NULL;
        }
        for (size_t i = 0; i < n; i++)
            if (infos[i]) marmot_key_package_info_free(infos[i]);
        free(infos);
        CHECK(active == 2 && legacy_active && adopted_active, "the newest of each profile active");
    }
    /* M4: a deletion request dated after every KeyPackage made, the next
     * KeyPackage after it. */
    int64_t reserved = 0;
    OK(marmot_key_package_reserve_created_at(bob.m, bob.pk, 1, &reserved));
    CHECK(reserved > created_at_of(legacy2.json) && reserved > created_at_of(adopted2.json),
          "the reservation is later than every KeyPackage, even dated ahead");
    Kp legacy3 = legacy_kp(&bob);
    CHECK(created_at_of(legacy3.json) > reserved, "the next KeyPackage is later still");
    free(legacy3.json);
    /* Stopping a profile retires all of its keys, never the other's. */
    size_t retired = 0;
    OK(marmot_key_package_retire_profile(bob.m, bob.pk, MARMOT_KEY_PACKAGE_PROFILE_MDK_0_8,
                                         &retired));
    CHECK(retired == 2 && !has_key(&bob, &legacy2) && has_key(&bob, &adopted2),
          "legacy retired, adopted kept");
    OK(marmot_key_package_next_expiry_for_profile(bob.m, bob.pk,
                                                  MARMOT_KEY_PACKAGE_PROFILE_MDK_0_8,
                                                  &legacy_exp));
    CHECK(legacy_exp == 0, "nothing of the legacy profile left");
    Member carol;
    member_init(&carol);
    Kp legacy_only = legacy_kp(&carol);
    int64_t carol_adopted = -1;
    OK(marmot_key_package_next_expiry_for_profile(carol.m, carol.pk,
                                                  MARMOT_KEY_PACKAGE_PROFILE_ADOPTED,
                                                  &carol_adopted));
    CHECK(carol_adopted == 0, "no adopted KeyPackage: nothing of that profile to protect");
    free(legacy_only.json);
    member_free(&carol);
    free(legacy_slot); free(adopted_slot); free(legacy_slot2); free(adopted_slot2);
    free(legacy1.json); free(legacy2.json); free(adopted1.json); free(adopted2.json);
    member_free(&bob);
}

int
main(void)
{
    if (sodium_init() < 0) return 1;
    printf("KeyPackage lifecycle (nostrc-0bdg)\n");
    RUN(test_rotation_ack_tied);
    RUN(test_profiles_have_independent_slots);
    RUN(test_last_used_profile);
    RUN(test_unconfirmed_replacement);
    RUN(test_last_resort_delayed_welcome);
    RUN(test_single_use_consumed);
    RUN(test_failed_welcome_preserves);
    RUN(test_lifetime_sweep);
    RUN(test_seeded_from_store);
    RUN(test_bounded);
    RUN(test_adopted_enrolled_producer);
    RUN(test_ciphersuite_singleton);
    RUN(test_capability_tags_match_leaf);
    RUN(test_grease_skipped);
    return 0;
}
