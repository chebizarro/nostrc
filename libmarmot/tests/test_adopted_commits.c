/*
 * libmarmot tests: Commits in adopted-profile groups (nostrc-qp24.5.1.3).
 *
 * Fixtures (tests/vectors/mdk-0.11/adopted-commits.json, provenance in its
 * README): a real MDK v0.11.0 Commit sequence a libmarmot member follows,
 * and Commits built directly with MDK's pinned OpenMLS that MDK itself
 * refuses to send (the negatives).  libmarmot's own adopted Commits are
 * checked against a libmarmot receiver here and against MDK by the
 * Groundhog MDK 0.11 harness.  Every refusal is checked for its error and
 * for leaving the stored group exactly as it was.
 *
 * SPDX-License-Identifier: MIT
 */

#include <marmot/marmot.h>
#include "marmot-internal.h"
#include "adopted.h"
#include "commits.h"
#include "proposals.h"
#include "kp_profile.h"
#include "mls/mls_group.h"
#include "mls/mls_app_components.h"
#include "mls/mls_app_data_update.h"
#include "mls/mls-internal.h"
#include "vectors/mdk-0.11/adopted_commits_fixture.h"
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

static const char *g_only;
#define RUN(fn)                                                             \
    do {                                                                    \
        if (g_only && strcmp(g_only, #fn) != 0) break;                      \
        printf("  %-58s", #fn); fflush(stdout); fn(); printf("PASS\n");    \
    } while (0)

/* ── Bytes ────────────────────────────────────────────────────────────── */

static uint8_t *
unhex(const char *hex, size_t *len)
{
    size_t n = strlen(hex) / 2;
    uint8_t *out = malloc(n ? n : 1);
    CHECK(out && sodium_hex2bin(out, n, hex, 2 * n, NULL, len, NULL) == 0 && *len == n, "hex");
    return out;
}

static void
unhex_into(const char *hex, uint8_t *out, size_t n)
{
    size_t len = 0;
    CHECK(strlen(hex) == 2 * n && sodium_hex2bin(out, n, hex, 2 * n, NULL, &len, NULL) == 0 &&
          len == n, "hex %zu", n);
}

static bool
same_key_hex(const uint8_t k[32], const char *hex)
{
    uint8_t b[32];
    unhex_into(hex, b, 32);
    return memcmp(k, b, 32) == 0;
}

/* ── Members ──────────────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    Marmot     *m;
    uint8_t     sk[32], pk[32];
} Member;

static void
member_init(Member *x, const char *name)
{
    memset(x, 0, sizeof(*x));
    x->name = name;
    MarmotConfig cfg = marmot_config_default();
    cfg.max_event_age_secs = 0;   /* fixtures are dated at capture */
    x->m = marmot_new_with_config(marmot_storage_memory_new(), &cfg);
    CHECK(x->m, "marmot_new");
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    do randombytes_buf(x->sk, 32); while (!secp256k1_ec_seckey_verify(ctx, x->sk));
    secp256k1_keypair kp;
    secp256k1_xonly_pubkey xonly;
    CHECK(secp256k1_keypair_create(ctx, &kp, x->sk) &&
          secp256k1_keypair_xonly_pub(ctx, &xonly, NULL, &kp) &&
          secp256k1_xonly_pubkey_serialize(ctx, x->pk, &xonly), "keypair");
    secp256k1_context_destroy(ctx);
}

static void
member_free(Member *x)
{
    marmot_free(x->m);
    sodium_memzero(x->sk, sizeof(x->sk));
}

static char *
adopted_key_package(Member *x)
{
    MarmotKeyPackageResult r;
    memset(&r, 0, sizeof(r));
    OK(marmot_create_key_package_adopted_internal(x->m, x->pk, x->sk, NULL, NULL, &r));
    char *json = strdup(r.event_json);
    marmot_key_package_result_free(&r);
    return json;
}

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

/* A KeyPackage's private material, stored as marmot_create_key_package()
 * stores it ("kp_priv" = init || encryption || Ed25519 secret; "kp_full"). */
static void
install_key_package(Member *x, const uint8_t *kp_bytes, size_t kp_len, const char *init_hex,
                    const char *enc_hex, const char *seed_hex, const char *pub_hex)
{
    MlsKeyPackage kp;
    memset(&kp, 0, sizeof(kp));
    MlsTlsReader r;
    mls_tls_reader_init(&r, kp_bytes, kp_len);
    CHECK(mls_key_package_deserialize(&r, &kp) == 0 && mls_tls_reader_done(&r), "kp");
    uint8_t ref[32];
    CHECK(mls_key_package_ref(&kp, ref) == 0, "ref");
    uint8_t priv[32 + 32 + 64], seed[32], pk[32], derived[32];
    unhex_into(init_hex, priv, 32);
    unhex_into(enc_hex, priv + 32, 32);
    unhex_into(seed_hex, seed, 32);
    unhex_into(pub_hex, pk, 32);
    CHECK(crypto_sign_seed_keypair(derived, priv + 64, seed) == 0 && memcmp(derived, pk, 32) == 0,
          "signature key");
    OK(x->m->storage->mls_store(x->m->storage->ctx, "kp_priv", ref, 32, priv, sizeof(priv)));
    OK(x->m->storage->mls_store(x->m->storage->ctx, "kp_full", ref, 32, kp_bytes, kp_len));
    sodium_memzero(priv, sizeof(priv));
    mls_key_package_clear(&kp);
}

/* The account key MDK's test support derives from a seed. */
static void
mdk_test_identity(const char *seed, uint8_t out[32])
{
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    for (uint64_t counter = 0;; counter++) {
        uint8_t sk[32], be[8];
        for (int i = 0; i < 8; i++) be[i] = (uint8_t)(counter >> (56 - 8 * i));
        crypto_hash_sha256_state st;
        crypto_hash_sha256_init(&st);
        crypto_hash_sha256_update(&st, (const uint8_t *)"cgka-engine-test-identity-v1", 28);
        crypto_hash_sha256_update(&st, (const uint8_t *)seed, strlen(seed));
        crypto_hash_sha256_update(&st, be, 8);
        crypto_hash_sha256_final(&st, sk);
        if (!secp256k1_ec_seckey_verify(ctx, sk)) continue;
        secp256k1_keypair kp;
        secp256k1_xonly_pubkey x;
        CHECK(secp256k1_keypair_create(ctx, &kp, sk) &&
              secp256k1_keypair_xonly_pub(ctx, &x, NULL, &kp) &&
              secp256k1_xonly_pubkey_serialize(ctx, out, &x), "identity");
        break;
    }
    secp256k1_context_destroy(ctx);
}

/* A kind:444 rumor carrying an MLSMessage Welcome (as an adopted inviter's). */
static char *
rumor_for(const char *welcome_hex, const uint8_t sender[32])
{
    size_t len = 0;
    uint8_t *w = unhex(welcome_hex, &len);
    size_t b64_len = sodium_base64_ENCODED_LEN(len, sodium_base64_VARIANT_ORIGINAL);
    char *b64 = malloc(b64_len);
    sodium_bin2base64(b64, b64_len, w, len, sodium_base64_VARIANT_ORIGINAL);
    char sender_hex[65];
    for (int i = 0; i < 32; i++) snprintf(sender_hex + 2 * i, 3, "%02x", sender[i]);
    NostrEvent *ev = nostr_event_new();
    NostrTags *tags = nostr_tags_new(0);
    nostr_tags_append(tags, nostr_tag_new("e", "1111111111111111111111111111111111111111111111111111111111111111", NULL));
    nostr_tags_append(tags, nostr_tag_new("relays", "wss://relay-a.example.com", NULL));
    nostr_event_set_kind(ev, 444);
    nostr_event_set_pubkey(ev, sender_hex);
    nostr_event_set_created_at(ev, 1790000000);
    nostr_event_set_content(ev, b64);
    nostr_event_set_tags(ev, tags);
    char *json = nostr_event_serialize_compact(ev);
    nostr_event_free(ev);
    free(b64);
    free(w);
    return json;
}

/* ── Stored state ─────────────────────────────────────────────────────── */

typedef struct {
    uint8_t *state, *parent, *floor;
    size_t   state_len, parent_len, floor_len;
    uint64_t epoch;
    char    *name;
    uint8_t  nostr_gid[32];
    bool     has_exporter_next;
    bool     active;
} Snapshot;

static void
snapshot(Member *x, const MarmotGroupId *gid, Snapshot *s)
{
    memset(s, 0, sizeof(*s));
    MarmotStorage *st = x->m->storage;
    OK(st->mls_load(st->ctx, "mls_group", gid->data, gid->len, &s->state, &s->state_len));
    if (st->mls_load(st->ctx, "mls_group_parent", gid->data, gid->len, &s->parent,
                     &s->parent_len) != MARMOT_OK)
        s->parent = NULL;
    MarmotGroup *g = NULL;
    OK(marmot_get_group(x->m, gid, &g));
    CHECK(g, "group");
    s->epoch = g->epoch;
    s->name = g->name ? strdup(g->name) : NULL;
    s->active = g->state == MARMOT_GROUP_STATE_ACTIVE;
    memcpy(s->nostr_gid, g->nostr_group_id, 32);
    uint8_t next[32];
    s->has_exporter_next = st->get_exporter_secret(st->ctx, gid, g->epoch + 1, next) == MARMOT_OK;
    if (st->mls_load(st->ctx, "group_event_created_at", g->nostr_group_id, 32, &s->floor,
                     &s->floor_len) != MARMOT_OK)
        s->floor = NULL;
    marmot_group_free(g);
}

static void
snapshot_clear(Snapshot *s)
{
    if (s->state) sodium_memzero(s->state, s->state_len);
    if (s->parent) sodium_memzero(s->parent, s->parent_len);
    free(s->state);
    free(s->parent);
    free(s->floor);
    free(s->name);
    memset(s, 0, sizeof(*s));
}

static void
expect_unchanged(Member *x, const MarmotGroupId *gid, const Snapshot *b, const char *what)
{
    Snapshot n;
    snapshot(x, gid, &n);
    CHECK(n.epoch == b->epoch, "%s: epoch moved", what);
    CHECK(n.active == b->active, "%s: activity changed", what);
    CHECK((!n.name && !b->name) || (n.name && b->name && strcmp(n.name, b->name) == 0),
          "%s: name changed", what);
    CHECK(memcmp(n.nostr_gid, b->nostr_gid, 32) == 0, "%s: routing changed", what);
    CHECK(n.state_len == b->state_len && memcmp(n.state, b->state, n.state_len) == 0,
          "%s: MLS state changed", what);
    CHECK((!n.parent) == (!b->parent) &&
          (!n.parent || (n.parent_len == b->parent_len &&
                         memcmp(n.parent, b->parent, n.parent_len) == 0)),
          "%s: retained parent changed", what);
    CHECK(n.has_exporter_next == b->has_exporter_next, "%s: next exporter stored", what);
    CHECK((!n.floor) == (!b->floor) &&
          (!n.floor || (n.floor_len == b->floor_len &&
                        memcmp(n.floor, b->floor, n.floor_len) == 0)),
          "%s: created_at floor moved", what);
    snapshot_clear(&n);
}

static MarmotError
deliver(Member *x, const char *event_json, MarmotMessageResult *out)
{
    MarmotMessageResult res;
    memset(&res, 0, sizeof(res));
    MarmotError err = marmot_process_message(x->m, event_json, &res);
    if (out) *out = res;
    else marmot_message_result_free(&res);
    return err;
}

/* `event` refused with `want`, the group untouched. */
static void
expect_refused(Member *x, const MarmotGroupId *gid, const char *event, MarmotError want,
               const char *what)
{
    Snapshot before;
    snapshot(x, gid, &before);
    MarmotMessageResult res;
    MarmotError err = deliver(x, event, &res);
    CHECK(err == want, "%s: got %d (%s), want %d (%s)", what, err, marmot_error_string(err), want,
          marmot_error_string(want));
    CHECK(res.type != MARMOT_RESULT_COMMIT, "%s: no Commit result", what);
    marmot_message_result_free(&res);
    expect_unchanged(x, gid, &before, what);
    snapshot_clear(&before);
}

static MarmotGroup *
group_of(Member *x, const MarmotGroupId *gid)
{
    MarmotGroup *g = NULL;
    OK(marmot_get_group(x->m, gid, &g));
    CHECK(g, "group stored");
    return g;
}

static size_t
member_count(Member *x, const MarmotGroupId *gid)
{
    uint8_t (*members)[32] = NULL;
    size_t n = 0;
    OK(marmot_get_group_members(x->m, gid, &members, &n));
    free(members);
    return n;
}

static bool
has_admin(const MarmotGroup *g, const char *hex)
{
    for (size_t i = 0; i < g->admin_count; i++)
        if (same_key_hex(g->admin_pubkeys[i], hex)) return true;
    return false;
}

/* MLS bytes of the stored group's epoch sealed as a kind:445 (what any
 * member of that epoch could publish). */
static char *
seal(Member *x, const MarmotGroupId *gid, const char *mls_hex)
{
    MarmotGroup *g = group_of(x, gid);
    uint8_t exporter[32];
    OK(x->m->storage->get_exporter_secret(x->m->storage->ctx, gid, g->epoch, exporter));
    size_t len = 0;
    uint8_t *msg = unhex(mls_hex, &len);
    char *event = marmot_commit_build_event(msg, len, exporter, g->nostr_group_id, marmot_now());
    CHECK(event, "seal");
    sodium_memzero(exporter, sizeof(exporter));
    free(msg);
    marmot_group_free(g);
    return event;
}

/* ══════════════════════════════════════════════════════════════════════════
 * MDK v0.11.0: a libmarmot member follows real adopted Commits
 * ══════════════════════════════════════════════════════════════════════════ */

static MarmotGroupId
gid_of(const char *hex)
{
    size_t len = 0;
    uint8_t *id = unhex(hex, &len);
    MarmotGroupId gid = marmot_group_id_new(id, len);
    free(id);
    return gid;
}

static void
mdk_observer(Member *x, MarmotGroupId *gid)
{
    member_init(x, "observer");
    size_t kp_len = 0;
    uint8_t *framed = unhex(H_MDK_KP_MLS_MESSAGE, &kp_len);
    CHECK(kp_len > 4 && framed[3] == 5, "MLSMessage(mls_key_package)");
    install_key_package(x, framed + 4, kp_len - 4, H_MDK_INIT_SK, H_MDK_ENC_SK, H_MDK_SIG_SEED,
                        H_MDK_SIG_PUB);
    free(framed);
    OK(join(x, H_MDK_RUMOR_JSON));
    *gid = gid_of(H_MDK_GROUP_ID);
}

static const AdoptedMdkStep *
mdk_step(const char *name)
{
    for (size_t i = 0; i < H_MDK_STEP_COUNT; i++)
        if (strcmp(H_MDK_STEPS[i].name, name) == 0) return &H_MDK_STEPS[i];
    CHECK(0, "no step %s", name);
    return NULL;
}

/* Apply `name`'s Commit: a Commit result naming its committer, at MDK's
 * epoch. */
static MarmotMessageResult
mdk_apply(Member *x, const MarmotGroupId *gid, const char *name)
{
    const AdoptedMdkStep *s = mdk_step(name);
    MarmotMessageResult res;
    MarmotError err = deliver(x, s->event_json, &res);
    CHECK(err == MARMOT_OK && res.type == MARMOT_RESULT_COMMIT, "%s: err %d (%s) type %d", name,
          err, marmot_error_string(err), res.type);
    CHECK(res.commit.committer_pubkey_hex &&
              strcmp(res.commit.committer_pubkey_hex, s->by_account) == 0,
          "%s: committed by %s", name, s->by);
    MarmotGroup *g = group_of(x, gid);
    CHECK(g->epoch == s->epoch_after, "%s: epoch %llu, MDK %llu", name,
          (unsigned long long)g->epoch, s->epoch_after);
    marmot_group_free(g);
    return res;
}

/* An MDK application message `name` reads as `text`. */
static void
mdk_message(Member *x, const char *name, const char *text)
{
    MarmotMessageResult m;
    MarmotError err = deliver(x, mdk_step(name)->event_json, &m);
    CHECK(err == MARMOT_OK && m.type == MARMOT_RESULT_APPLICATION_MESSAGE &&
              m.app_msg.inner_event_json && strstr(m.app_msg.inner_event_json, text),
          "%s: err %d (%s) type %d", name, err, marmot_error_string(err), m.type);
    marmot_message_result_free(&m);
}

static void
test_mdk_commit_sequence(void)
{
    Member o;
    MarmotGroupId gid;
    mdk_observer(&o, &gid);
    CHECK(member_count(&o, &gid) == 4, "alice, bob, carol, observer");

    /* 0. A message of the founding epoch (MDK -> libmarmot, adopted). */
    mdk_message(&o, "message_founding_epoch", "before any commit");

    /* 1. Rename: an admin's inline AppDataUpdate (0x8001). */
    MarmotMessageResult r = mdk_apply(&o, &gid, "rename");
    marmot_message_result_free(&r);
    mdk_message(&o, "message_after_rename", "after rename");
    MarmotGroup *g = group_of(&o, &gid);
    CHECK(g->name && strcmp(g->name, "W24-H renamed") == 0 && g->description &&
          strcmp(g->description, "renamed by MDK") == 0, "renamed: %s", g->name);
    marmot_group_free(g);

    /* 2. A non-admin's self-update: ordinary, any member's. */
    r = mdk_apply(&o, &gid, "self_update");
    CHECK(r.commit.committer_leaf != UINT32_MAX, "renewed leaf named");
    marmot_message_result_free(&r);
    mdk_message(&o, "message_after_self_update", "after self_update");

    /* 3. Add (admin). */
    r = mdk_apply(&o, &gid, "add");
    marmot_message_result_free(&r);
    mdk_message(&o, "message_after_add", "after add");
    CHECK(member_count(&o, &gid) == 5, "dave added");

    /* 4. Admin change (0x8003): Bob is a co-admin. */
    r = mdk_apply(&o, &gid, "admin_change");
    marmot_message_result_free(&r);
    mdk_message(&o, "message_after_admin_change", "after admin_change");
    g = group_of(&o, &gid);
    CHECK(g->admin_count == 2 && has_admin(g, H_MDK_ALICE) && has_admin(g, H_MDK_BOB), "admins");
    marmot_group_free(g);

    /* 5. Dave's SelfRemove (kept), and the Commit of it by reference. */
    MarmotMessageResult p;
    OK(deliver(&o, mdk_step("self_remove_proposal")->event_json, &p));
    CHECK(p.type == MARMOT_RESULT_PROPOSAL && p.proposal.leave &&
              p.proposal.proposal_type == MARMOT_PROPOSAL_TYPE_SELF_REMOVE &&
              strcmp(p.proposal.sender_pubkey_hex, H_MDK_DAVE) == 0,
          "dave's SelfRemove kept");
    marmot_message_result_free(&p);
    r = mdk_apply(&o, &gid, "self_remove_commit");
    CHECK(r.commit.departed_count == 1 && strcmp(r.commit.departed_pubkey_hexes[0], H_MDK_DAVE) == 0,
          "dave left");
    marmot_message_result_free(&r);
    mdk_message(&o, "message_after_self_remove_commit", "after self_remove_commit");
    CHECK(member_count(&o, &gid) == 4, "dave gone");

    /* 6. Remove of Carol (no admin). */
    r = mdk_apply(&o, &gid, "remove");
    marmot_message_result_free(&r);
    mdk_message(&o, "message_after_remove", "after remove");
    CHECK(member_count(&o, &gid) == 3, "carol gone");

    /* 7. Remove of an admin: MDK drops Bob's key from 0x8003 in the same
     *    Commit (admin-policy-v1.md coupling). */
    r = mdk_apply(&o, &gid, "remove_admin");
    marmot_message_result_free(&r);
    mdk_message(&o, "message_after_remove_admin", "after remove_admin");
    g = group_of(&o, &gid);
    CHECK(member_count(&o, &gid) == 2 && g->admin_count == 1 && has_admin(g, H_MDK_ALICE),
          "bob removed with his admin key");
    marmot_group_free(g);

    /* 8. Routing rotation, published at the old address: reported with the
     *    old id; the record and marmot_get_group_routing() move on; the old
     *    address keeps routing to the group. */
    uint8_t old_ngid[32], new_ngid[32];
    unhex_into(H_MDK_NOSTR_GROUP_ID, old_ngid, 32);
    unhex_into(H_MDK_ROTATED_NOSTR_GROUP_ID, new_ngid, 32);
    r = mdk_apply(&o, &gid, "routing_rotation");
    CHECK(r.commit.routing_changed && memcmp(r.commit.previous_nostr_group_id, old_ngid, 32) == 0,
          "rotation reported");
    CHECK(r.commit.updated_group && memcmp(r.commit.updated_group->nostr_group_id, new_ngid, 32) == 0,
          "updated group carries the new id");
    marmot_message_result_free(&r);
    uint8_t cur[32];
    char **relays = NULL;
    size_t n_relays = 0;
    uint8_t (*previous)[32] = NULL;
    size_t n_previous = 0;
    OK(marmot_get_group_routing(o.m, &gid, cur, &relays, &n_relays, &previous, &n_previous));
    CHECK(memcmp(cur, new_ngid, 32) == 0 && n_relays == 2 &&
              strcmp(relays[0], H_MDK_RELAY_A) == 0 && strcmp(relays[1], H_MDK_RELAY_C) == 0,
          "new routing");
    CHECK(n_previous == 1 && memcmp(previous[0], old_ngid, 32) == 0, "old address listed");
    for (size_t i = 0; i < n_relays; i++) free(relays[i]);
    free(relays);
    free(previous);
    /* The rotating Commit again, at the old address: routed (via the
     * alias), recognised as already applied -- not "group not found". */
    MarmotMessageResult again;
    OK(deliver(&o, mdk_step("routing_rotation")->event_json, &again));
    CHECK(again.type == MARMOT_RESULT_OWN_MESSAGE, "old address still routes: %d", again.type);
    marmot_message_result_free(&again);

    /* 9. Traffic at the new address. */
    mdk_message(&o, "message_after_rotation", "after the rotation");

    marmot_group_id_free(&gid);
    member_free(&o);
}

/* A Commit citing a proposal not received yet is held, not refused for good
 * (nostrc-2um6 H1: MARMOT_ERR_PROPOSAL_UNKNOWN), and applies once the
 * proposal arrives. */
static void
test_mdk_commit_before_its_proposal(void)
{
    Member o;
    MarmotGroupId gid;
    mdk_observer(&o, &gid);
    static const char *const before[] = {"rename", "self_update", "add", "admin_change"};
    for (size_t i = 0; i < 4; i++) {
        MarmotMessageResult r = mdk_apply(&o, &gid, before[i]);
        marmot_message_result_free(&r);
    }
    expect_refused(&o, &gid, mdk_step("self_remove_commit")->event_json,
                   MARMOT_ERR_PROPOSAL_UNKNOWN, "SelfRemove Commit before its proposal");
    OK(deliver(&o, mdk_step("self_remove_proposal")->event_json, NULL));
    MarmotMessageResult r = mdk_apply(&o, &gid, "self_remove_commit");
    marmot_message_result_free(&r);
    marmot_group_id_free(&gid);
    member_free(&o);
}

/* Competing Commits of one epoch (convergence.md, the bounded legacy
 * subset): libmarmot commits Dave's SelfRemove itself, then Alice's MDK
 * Commit of the same proposal arrives.  Both are ordinary (SelfRemove
 * only); the lower committer key wins, transport order never decides. */
static void
test_mdk_competing_self_remove_commits(void)
{
    Member o;
    MarmotGroupId gid;
    mdk_observer(&o, &gid);
    static const char *const before[] = {"rename", "self_update", "add", "admin_change"};
    for (size_t i = 0; i < 4; i++) {
        MarmotMessageResult r = mdk_apply(&o, &gid, before[i]);
        marmot_message_result_free(&r);
    }
    OK(deliver(&o, mdk_step("self_remove_proposal")->event_json, NULL));
    char *ours = NULL;
    OK(marmot_commit_pending_proposals(o.m, &gid, &ours));
    CHECK(ours, "the observer (no admin) commits a SelfRemove");
    OK(marmot_merge_pending_commit(o.m, &gid));
    free(ours);
    CHECK(member_count(&o, &gid) == 4, "dave gone on our branch");
    Snapshot ours_state;
    snapshot(&o, &gid, &ours_state);
    uint8_t alice[32], observer[32];
    unhex_into(H_MDK_ALICE, alice, 32);
    unhex_into(H_MDK_OBSERVER, observer, 32);
    const char *theirs = mdk_step("self_remove_commit")->event_json;
    if (memcmp(observer, alice, 32) < 0) {
        /* Ours sorts first: Alice's loses and is stale. */
        expect_refused(&o, &gid, theirs, MARMOT_ERR_WRONG_EPOCH, "losing competitor");
    } else {
        /* Alice's sorts first: it replaces ours, and MDK's next Commit
         * applies on top of it. */
        MarmotMessageResult r;
        OK(deliver(&o, theirs, &r));
        CHECK(r.type == MARMOT_RESULT_COMMIT, "winning competitor applied");
        marmot_message_result_free(&r);
        r = mdk_apply(&o, &gid, "remove");
        marmot_message_result_free(&r);
    }
    snapshot_clear(&ours_state);
    marmot_group_id_free(&gid);
    member_free(&o);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Pinned OpenMLS: Commits MDK refuses to send (negatives), by reference
 * ══════════════════════════════════════════════════════════════════════════ */

static void
omls_observer(Member *x, MarmotGroupId *gid)
{
    member_init(x, "observer");
    size_t kp_len = 0;
    uint8_t *kp = unhex(H_OMLS_KP, &kp_len);
    install_key_package(x, kp, kp_len, H_OMLS_INIT_SK, H_OMLS_ENC_SK, H_OMLS_SIG_SEED,
                        H_OMLS_SIG_PUB);
    free(kp);
    uint8_t inviter[32];
    mdk_test_identity("w24h-neg-x", inviter);
    CHECK(same_key_hex(inviter, H_OMLS_X), "inviter identity");
    char *rumor = rumor_for(H_OMLS_WELCOME, inviter);
    OK(join(x, rumor));
    free(rumor);
    MarmotGroup **groups = NULL;
    size_t n = 0;
    OK(marmot_get_all_groups(x->m, &groups, &n));
    CHECK(n == 1, "joined");
    *gid = marmot_group_id_new(groups[0]->mls_group_id.data, groups[0]->mls_group_id.len);
    for (size_t i = 0; i < n; i++) marmot_group_free(groups[i]);
    free(groups);
}

static const char *
forgery(const char *name)
{
    for (size_t i = 0; i < H_OMLS_COMMIT_COUNT; i++)
        if (strcmp(H_OMLS_COMMITS[i].name, name) == 0) return H_OMLS_COMMITS[i].message;
    CHECK(0, "no forgery %s", name);
    return NULL;
}

static void
test_openmls_negatives(void)
{
    static const struct {
        const char *name;
        MarmotError want;
    } cases[] = {
        /* Authorization against the candidate parent (admins X and W). */
        {"nonadmin_rename", MARMOT_ERR_COMMIT_FROM_NON_ADMIN},
        {"nonadmin_add", MARMOT_ERR_COMMIT_FROM_NON_ADMIN},
        /* Y's Remove of W, an admin, without dropping W's key: the
         * resulting-epoch check (an admin without a leaf) refuses it before
         * authorization does (a non-admin's Remove of a non-admin:
         * test_nonadmin_removal_refused). */
        {"nonadmin_remove", MARMOT_ERR_MLS_PROCESS_MESSAGE},
        /* Resulting-epoch invariants (the MLS layer's entered-epoch check). */
        {"drop_required_routing", MARMOT_ERR_MLS_PROCESS_MESSAGE},
        {"remove_admin_policy", MARMOT_ERR_MLS_PROCESS_MESSAGE},
        {"remove_lifecycle", MARMOT_ERR_MLS_PROCESS_MESSAGE},
        {"empty_admins", MARMOT_ERR_MLS_PROCESS_MESSAGE},
        {"admin_not_member", MARMOT_ERR_MLS_PROCESS_MESSAGE},
        {"remove_admin_uncoupled", MARMOT_ERR_MLS_PROCESS_MESSAGE},
        {"malformed_profile", MARMOT_ERR_MLS_PROCESS_MESSAGE},
        {"malformed_routing", MARMOT_ERR_MLS_PROCESS_MESSAGE},
        {"proof_in_group_context", MARMOT_ERR_MLS_PROCESS_MESSAGE},
        {"disband", MARMOT_ERR_MLS_PROCESS_MESSAGE},
        {"add_without_proof", MARMOT_ERR_MLS_PROCESS_MESSAGE},
        /* A proof that does not verify: the Marmot layer. */
        {"add_bad_proof", MARMOT_ERR_KEY_PACKAGE_IDENTITY},
        /* What libmarmot cannot judge as MDK does: fail closed. */
        {"unsupported_component", MARMOT_ERR_UNSUPPORTED},
        {"group_context_extensions", MARMOT_ERR_UNSUPPORTED},
    };
    Member o;
    MarmotGroupId gid;
    omls_observer(&o, &gid);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *event = seal(&o, &gid, forgery(cases[i].name));
        expect_refused(&o, &gid, event, cases[i].want, cases[i].name);
        free(event);
    }
    /* After every refusal the group still follows a valid Commit. */
    char *event = seal(&o, &gid, forgery("ok_rename"));
    MarmotMessageResult r;
    OK(deliver(&o, event, &r));
    CHECK(r.type == MARMOT_RESULT_COMMIT, "valid rename applies");
    marmot_message_result_free(&r);
    free(event);
    MarmotGroup *g = group_of(&o, &gid);
    CHECK(g->name && strcmp(g->name, "renamed by X") == 0, "renamed");
    marmot_group_free(g);
    marmot_group_id_free(&gid);
    member_free(&o);
}

static void
test_openmls_positives(void)
{
    static const char *const names[] = {"ok_rename", "ok_unknown_component",
                                        "ok_nonadmin_self_update", "ok_remove_admin_coupled",
                                        "ok_add"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        Member o;
        MarmotGroupId gid;
        omls_observer(&o, &gid);
        char *event = seal(&o, &gid, forgery(names[i]));
        MarmotMessageResult r;
        MarmotError err = deliver(&o, event, &r);
        CHECK(err == MARMOT_OK && r.type == MARMOT_RESULT_COMMIT, "%s: %d (%s)", names[i], err,
              marmot_error_string(err));
        marmot_message_result_free(&r);
        free(event);
        MarmotGroup *g = group_of(&o, &gid);
        if (strcmp(names[i], "ok_remove_admin_coupled") == 0)
            CHECK(g->admin_count == 1 && has_admin(g, H_OMLS_X) && member_count(&o, &gid) == 3,
                  "W removed with its admin key");
        if (strcmp(names[i], "ok_add") == 0)
            CHECK(member_count(&o, &gid) == 5, "added");
        CHECK(g->epoch == 2, "%s: epoch 2", names[i]);
        marmot_group_free(g);
        marmot_group_id_free(&gid);
        member_free(&o);
    }
}

/* By reference (app-components/README.md "Authorization Evaluation"): an
 * admin's standalone AppDataUpdate is kept and its Commit applies; a
 * non-admin's is refused on arrival, and so is one from an account that
 * was an admin in an earlier epoch but not in its source epoch. */
static void
test_openmls_by_reference(void)
{
    Member o;
    MarmotGroupId gid;
    omls_observer(&o, &gid);

    /* Y (no admin): its proposal is refused, so X's Commit of it cites a
     * proposal libmarmot never kept. */
    char *py = seal(&o, &gid, H_OMLS_REF_PROPOSAL_Y);
    expect_refused(&o, &gid, py, MARMOT_ERR_ADMIN_ONLY, "a non-admin's AppDataUpdate proposal");
    MarmotPendingProposal *pending = NULL;
    size_t n_pending = 0;
    OK(marmot_get_pending_proposals(o.m, &gid, &pending, &n_pending));
    CHECK(n_pending == 0, "not kept");
    marmot_pending_proposals_free(pending);
    char *cy = seal(&o, &gid, H_OMLS_REF_COMMIT_X_OF_Y);
    expect_refused(&o, &gid, cy, MARMOT_ERR_PROPOSAL_UNKNOWN, "Commit of the refused proposal");

    /* W, an admin of epoch 1, proposes at epoch 1 (kept) ... */
    char *pw1 = seal(&o, &gid, H_OMLS_REF_PROPOSAL_W_EPOCH1);
    /* X's own proposal and its Commit by reference. */
    char *px = seal(&o, &gid, H_OMLS_REF_PROPOSAL_X);
    MarmotMessageResult r;
    OK(deliver(&o, px, &r));
    CHECK(r.type == MARMOT_RESULT_PROPOSAL && !r.proposal.leave &&
              r.proposal.proposal_type == 0x0008,
          "an admin's AppDataUpdate proposal is kept");
    marmot_message_result_free(&r);
    char *cx = seal(&o, &gid, H_OMLS_REF_COMMIT_X);
    OK(deliver(&o, cx, &r));
    CHECK(r.type == MARMOT_RESULT_COMMIT, "by-reference rename applied");
    marmot_message_result_free(&r);
    MarmotGroup *g = group_of(&o, &gid);
    CHECK(g->name && strcmp(g->name, "renamed by reference") == 0 && g->epoch == 2, "renamed");
    marmot_group_free(g);
    /* ... which is now of a past epoch. */
    expect_refused(&o, &gid, pw1, MARMOT_ERR_WRONG_EPOCH, "a proposal of the previous epoch");
    marmot_group_id_free(&gid);
    member_free(&o);

    /* The demotion branch: X demotes W (epoch 1 -> 2); W's next proposal
     * is judged in its source epoch, where W is no admin. */
    omls_observer(&o, &gid);
    char *demote = seal(&o, &gid, H_OMLS_REF_DEMOTE_W);
    OK(deliver(&o, demote, &r));
    CHECK(r.type == MARMOT_RESULT_COMMIT, "demotion applied");
    marmot_message_result_free(&r);
    g = group_of(&o, &gid);
    CHECK(g->admin_count == 1 && has_admin(g, H_OMLS_X), "W demoted");
    marmot_group_free(g);
    char *pw2 = seal(&o, &gid, H_OMLS_REF_PROPOSAL_W_EPOCH2);
    expect_refused(&o, &gid, pw2, MARMOT_ERR_ADMIN_ONLY, "a former admin's proposal");
    char *cw2 = seal(&o, &gid, H_OMLS_REF_COMMIT_X_OF_W_EPOCH2);
    expect_refused(&o, &gid, cw2, MARMOT_ERR_PROPOSAL_UNKNOWN, "Commit of a former admin's proposal");
    free(py);
    free(cy);
    free(pw1);
    free(px);
    free(cx);
    free(demote);
    free(pw2);
    free(cw2);
    marmot_group_id_free(&gid);
    member_free(&o);
}

/* The authorization rules on their own, where the processor's guards would
 * hide them: a by-reference proposal's sender, a lifecycle update by
 * reference, a non-admin's privileged shape, a type libmarmot does not
 * apply. */
static void
test_authorize_by_reference_rules(void)
{
    Member o;
    MarmotGroupId gid;
    omls_observer(&o, &gid);
    uint8_t *blob = NULL;
    size_t len = 0;
    OK(o.m->storage->mls_load(o.m->storage->ctx, "mls_group", gid.data, gid.len, &blob, &len));
    MlsGroup pre, post;
    CHECK(mls_group_deserialize(blob, len, &pre) == 0 && mls_group_deserialize(blob, len, &post) == 0,
          "state");
    sodium_memzero(blob, len);
    free(blob);
    /* X's leaf: the inviter, leaf 0. */
    uint32_t x_leaf = 0;
    MarmotCommitKey key;
    MarmotGroupDataExtension *gde = NULL;
    MlsCommitSummary s;
    memset(&s, 0, sizeof(s));
    s.shape_known = true;
    s.has_path = true;
    s.proposal_count = 1;
    s.adu_count = 1;
    s.ref_count = 1;
    s.ref_type[0] = MLS_PROPOSAL_APP_DATA_UPDATE;
    s.ref_component[0] = 0x8001;
    s.ref_sender[0] = H_OMLS_Y_LEAF;   /* no admin */
    EXPECT_ERR(marmot_commit_authorize_ex(&pre, &post, x_leaf, false, &s, &key, &gde),
               MARMOT_ERR_COMMIT_FROM_NON_ADMIN);
    s.ref_sender[0] = H_OMLS_W_LEAF;   /* an admin */
    OK(marmot_commit_authorize_ex(&pre, &post, x_leaf, false, &s, &key, &gde));
    CHECK(key.privileged, "an AppDataUpdate Commit is privileged");
    s.ref_component[0] = 0x800c;       /* lifecycle: inline only */
    EXPECT_ERR(marmot_commit_authorize_ex(&pre, &post, x_leaf, false, &s, &key, &gde),
               MARMOT_ERR_VALIDATION);
    /* Y commits anything but a self-update or SelfRemove-only shape. */
    memset(&s, 0, sizeof(s));
    s.shape_known = true;
    s.has_path = true;
    s.proposal_count = 1;
    s.adu_count = 1;
    EXPECT_ERR(marmot_commit_authorize_ex(&pre, &post, H_OMLS_Y_LEAF, false, &s, &key, &gde),
               MARMOT_ERR_COMMIT_FROM_NON_ADMIN);
    s.proposal_count = 0;
    s.adu_count = 0;
    OK(marmot_commit_authorize_ex(&pre, &post, H_OMLS_Y_LEAF, false, &s, &key, &gde));
    CHECK(!key.privileged, "a self-update is ordinary");
    s.has_path = false;   /* an empty Commit without a path is no self-update */
    EXPECT_ERR(marmot_commit_authorize_ex(&pre, &post, H_OMLS_Y_LEAF, false, &s, &key, &gde),
               MARMOT_ERR_COMMIT_FROM_NON_ADMIN);
    /* Update proposals are not applied in an adopted group. */
    s.has_path = true;
    s.proposal_count = 1;
    s.update_count = 1;
    EXPECT_ERR(marmot_commit_authorize_ex(&pre, &post, x_leaf, false, &s, &key, &gde),
               MARMOT_ERR_UNSUPPORTED);
    CHECK(!gde, "no GroupData for an adopted group");
    mls_group_free(&pre);
    mls_group_free(&post);
    marmot_group_id_free(&gid);
    member_free(&o);
}

/* A failed write while applying an adopted Commit leaves the stored group
 * exactly as it was (the memory backend has no transactions: the
 * compensation path), and the Commit applies once storage is healthy. */
typedef struct {
    MarmotStorage orig;
    int writes, fail_at;
} Faults;
static Faults g_faults;

static MarmotError
fault(void)
{
    return ++g_faults.writes == g_faults.fail_at ? MARMOT_ERR_STORAGE_CONSTRAINT : MARMOT_OK;
}
static MarmotError
f_mls_store(void *c, const char *l, const uint8_t *k, size_t kl, const uint8_t *v, size_t vl)
{
    MarmotError e = fault();
    return e != MARMOT_OK ? e : g_faults.orig.mls_store(c, l, k, kl, v, vl);
}
static MarmotError
f_save_exporter(void *c, const MarmotGroupId *g, uint64_t e, const uint8_t s[32])
{
    MarmotError err = fault();
    return err != MARMOT_OK ? err : g_faults.orig.save_exporter_secret(c, g, e, s);
}
static MarmotError
f_save_group(void *c, const MarmotGroup *g)
{
    MarmotError e = fault();
    return e != MARMOT_OK ? e : g_faults.orig.save_group(c, g);
}

static void
test_partial_write_rollback(void)
{
    for (int k = 1; k <= 4; k++) {
        Member o;
        MarmotGroupId gid;
        omls_observer(&o, &gid);
        char *event = seal(&o, &gid, forgery("ok_rename"));
        Snapshot before;
        snapshot(&o, &gid, &before);
        MarmotStorage *s = o.m->storage;
        memset(&g_faults, 0, sizeof(g_faults));
        g_faults.orig = *s;
        g_faults.fail_at = k;
        s->mls_store = f_mls_store;
        s->save_exporter_secret = f_save_exporter;
        s->save_group = f_save_group;
        MarmotError err = deliver(&o, event, NULL);
        s->mls_store = g_faults.orig.mls_store;
        s->save_exporter_secret = g_faults.orig.save_exporter_secret;
        s->save_group = g_faults.orig.save_group;
        CHECK(err == MARMOT_ERR_STORAGE_CONSTRAINT, "write %d: err %d", k, err);
        expect_unchanged(&o, &gid, &before, "rolled back");
        snapshot_clear(&before);
        MarmotMessageResult r;
        OK(deliver(&o, event, &r));
        CHECK(r.type == MARMOT_RESULT_COMMIT, "applies once healthy");
        marmot_message_result_free(&r);
        MarmotGroup *g = group_of(&o, &gid);
        CHECK(g->name && strcmp(g->name, "renamed by X") == 0, "renamed");
        marmot_group_free(g);
        free(event);
        marmot_group_id_free(&gid);
        member_free(&o);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * libmarmot's own adopted Commits, followed by libmarmot members
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    Member alice, bob, carol;
    MarmotGroupId gid;
    uint8_t nostr_gid[32];
} Trio;

/* Alice (the only admin) creates an adopted group with Bob and Carol. */
static void
trio_create(Trio *t)
{
    member_init(&t->alice, "alice");
    member_init(&t->bob, "bob");
    member_init(&t->carol, "carol");
    char *kb = adopted_key_package(&t->bob), *kc = adopted_key_package(&t->carol);
    const char *kps[] = {kb, kc};
    const char *relays[] = {"wss://relay-a.example.com"};
    MarmotGroupConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "Trio";
    cfg.description = "adopted";
    cfg.relay_urls = (char **)relays;
    cfg.relay_count = 1;
    MarmotCreateGroupResult r;
    memset(&r, 0, sizeof(r));
    OK(marmot_create_group_for_profile(t->alice.m, MARMOT_GROUP_PROFILE_ADOPTED, t->alice.pk,
                                       t->alice.sk, NULL, NULL, kps, 2, &cfg, &r));
    t->gid = marmot_group_id_new(r.group->mls_group_id.data, r.group->mls_group_id.len);
    memcpy(t->nostr_gid, r.group->nostr_group_id, 32);
    OK(marmot_merge_pending_commit(t->alice.m, &t->gid));
    OK(join(&t->bob, r.welcome_rumor_jsons[0]));
    OK(join(&t->carol, r.welcome_rumor_jsons[1]));
    marmot_create_group_result_free(&r);
    free(kb);
    free(kc);
}

static void
trio_free(Trio *t)
{
    marmot_group_id_free(&t->gid);
    member_free(&t->alice);
    member_free(&t->bob);
    member_free(&t->carol);
}

/* `x` publishes its pending Commit (`event`), merges it, and every
 * receiver applies it. */
static void
publish(Member *x, const MarmotGroupId *gid, const char *event, Member **rx, size_t n)
{
    OK(marmot_merge_pending_commit(x->m, gid));
    for (size_t i = 0; i < n; i++) {
        MarmotMessageResult r;
        MarmotError err = deliver(rx[i], event, &r);
        CHECK(err == MARMOT_OK && r.type == MARMOT_RESULT_COMMIT, "%s applies %s's Commit: %d (%s)",
              rx[i]->name, x->name, err, marmot_error_string(err));
        marmot_message_result_free(&r);
    }
}

static char *
hex32(const uint8_t k[32])
{
    char *h = malloc(65);
    sodium_bin2hex(h, 65, k, 32);
    return h;
}

static void
test_own_commits(void)
{
    Trio t;
    trio_create(&t);
    Member *bc[] = {&t.bob, &t.carol};
    char *ev = NULL;

    /* Rename (0x8001), by the admin. */
    MarmotGroupConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "Trio renamed";
    OK(marmot_update_group_metadata(t.alice.m, &t.gid, &cfg, &ev));
    publish(&t.alice, &t.gid, ev, bc, 2);
    free(ev);
    for (int i = 0; i < 2; i++) {
        MarmotGroup *g = group_of(bc[i], &t.gid);
        CHECK(g->name && strcmp(g->name, "Trio renamed") == 0 && g->description &&
              strcmp(g->description, "adopted") == 0, "%s: renamed, description kept", bc[i]->name);
        marmot_group_free(g);
    }
    /* Nothing to change: no Commit. */
    EXPECT_ERR(marmot_update_group_metadata(t.alice.m, &t.gid, &cfg, &ev), MARMOT_ERR_INVALID_ARG);

    /* A non-admin's self-update (ordinary). */
    OK(marmot_self_update(t.bob.m, &t.gid, NULL, &ev));
    Member *ac[] = {&t.alice, &t.carol};
    publish(&t.bob, &t.gid, ev, ac, 2);
    free(ev);
    /* A non-admin's rename is refused before anything is made. */
    EXPECT_ERR(marmot_update_group_metadata(t.bob.m, &t.gid, &cfg, &ev), MARMOT_ERR_ADMIN_ONLY);

    /* Admin change (0x8003): Bob becomes a co-admin. */
    uint8_t admins[2][32];
    memcpy(admins[0], t.alice.pk, 32);
    memcpy(admins[1], t.bob.pk, 32);
    memset(&cfg, 0, sizeof(cfg));
    cfg.admin_pubkeys = admins;
    cfg.admin_count = 2;
    OK(marmot_update_group_metadata(t.alice.m, &t.gid, &cfg, &ev));
    publish(&t.alice, &t.gid, ev, bc, 2);
    free(ev);
    MarmotGroup *g = group_of(&t.carol, &t.gid);
    CHECK(g->admin_count == 2, "two admins");
    marmot_group_free(g);
    /* An admin who is not a member is refused. */
    Member stranger;
    member_init(&stranger, "stranger");
    memcpy(admins[1], stranger.pk, 32);
    EXPECT_ERR(marmot_update_group_metadata(t.alice.m, &t.gid, &cfg, &ev),
               MARMOT_ERR_MEMBER_NOT_FOUND);

    /* Add Dave (a proof-bearing adopted KeyPackage). */
    Member dave;
    member_init(&dave, "dave");
    char *kd = adopted_key_package(&dave);
    const char *kps[] = {kd};
    char **welcomes = NULL;
    size_t n_welcomes = 0;
    OK(marmot_add_members(t.bob.m, &t.gid, kps, 1, &welcomes, &n_welcomes, &ev));
    Member *acarol[] = {&t.alice, &t.carol};
    publish(&t.bob, &t.gid, ev, acarol, 2);
    free(ev);
    CHECK(n_welcomes == 1, "one Welcome");
    OK(join(&dave, welcomes[0]));
    free(welcomes[0]);
    free(welcomes);
    free(kd);
    CHECK(member_count(&t.alice, &t.gid) == 4 && member_count(&dave, &t.gid) == 4, "four");
    /* A legacy KeyPackage cannot be added to an adopted group. */
    MarmotKeyPackageResult legacy;
    memset(&legacy, 0, sizeof(legacy));
    OK(marmot_create_key_package(stranger.m, stranger.pk, stranger.sk, NULL, 0, &legacy));
    const char *lkps[] = {legacy.event_json};
    CHECK(marmot_add_members(t.alice.m, &t.gid, lkps, 1, &welcomes, &n_welcomes, &ev) != MARMOT_OK,
          "legacy KeyPackage refused");
    marmot_key_package_result_free(&legacy);

    /* Remove Carol (no admin); she learns it. */
    uint8_t victims[1][32];
    memcpy(victims[0], t.carol.pk, 32);
    OK(marmot_remove_members(t.alice.m, &t.gid, (const uint8_t (*)[32])victims, 1, &ev));
    Member *bd[] = {&t.bob, &dave};
    publish(&t.alice, &t.gid, ev, bd, 2);
    MarmotMessageResult r;
    OK(deliver(&t.carol, ev, &r));
    CHECK(r.type == MARMOT_RESULT_COMMIT, "carol: her removal");
    marmot_message_result_free(&r);
    free(ev);
    bool removed = false;
    uint8_t remover[32];
    OK(marmot_get_group_removal(t.carol.m, &t.gid, &removed, remover, NULL, NULL));
    CHECK(removed && memcmp(remover, t.alice.pk, 32) == 0, "carol removed by alice");
    g = group_of(&t.carol, &t.gid);
    CHECK(g->state != MARMOT_GROUP_STATE_ACTIVE, "carol's copy inactive");
    marmot_group_free(g);

    /* Remove Bob, an admin: his key leaves 0x8003 in the same Commit. */
    memcpy(victims[0], t.bob.pk, 32);
    OK(marmot_remove_members(t.alice.m, &t.gid, (const uint8_t (*)[32])victims, 1, &ev));
    Member *d[] = {&dave};
    publish(&t.alice, &t.gid, ev, d, 1);
    free(ev);
    g = group_of(&dave, &t.gid);
    CHECK(g->admin_count == 1 && memcmp(g->admin_pubkeys[0], t.alice.pk, 32) == 0, "alice only");
    marmot_group_free(g);

    /* Relays (0x8004, same nostr_group_id): reported as a routing change. */
    const char *relays[] = {"wss://relay-z.example.com", "wss://relay-a.example.com"};
    memset(&cfg, 0, sizeof(cfg));
    cfg.relay_urls = (char **)relays;
    cfg.relay_count = 2;
    OK(marmot_update_group_metadata(t.alice.m, &t.gid, &cfg, &ev));
    OK(marmot_merge_pending_commit(t.alice.m, &t.gid));
    OK(deliver(&dave, ev, &r));
    CHECK(r.type == MARMOT_RESULT_COMMIT && r.commit.routing_changed &&
              memcmp(r.commit.previous_nostr_group_id, t.nostr_gid, 32) == 0,
          "relay change reported, same id");
    marmot_message_result_free(&r);
    free(ev);
    uint8_t cur[32];
    char **rl = NULL;
    size_t nr = 0;
    OK(marmot_get_group_routing(dave.m, &t.gid, cur, &rl, &nr, NULL, NULL));
    CHECK(memcmp(cur, t.nostr_gid, 32) == 0 && nr == 2 &&
              strcmp(rl[0], "wss://relay-a.example.com") == 0 &&
              strcmp(rl[1], "wss://relay-z.example.com") == 0,
          "sorted relays");
    for (size_t i = 0; i < nr; i++) free(rl[i]);
    free(rl);

    /* A message from the admin still reads. */
    MarmotOutgoingMessage out;
    memset(&out, 0, sizeof(out));
    OK(marmot_create_message(t.alice.m, &t.gid,
                             "{\"kind\":9,\"content\":\"still here\",\"created_at\":1700000000,\"tags\":[]}",
                             &out));
    OK(deliver(&dave, out.event_json, &r));
    CHECK(r.type == MARMOT_RESULT_APPLICATION_MESSAGE, "message");
    marmot_message_result_free(&r);
    marmot_outgoing_message_free(&out);
    (void)hex32;
    member_free(&stranger);
    member_free(&dave);
    trio_free(&t);
}

/* A self-update renewing the account proof (marmot_group_account_proof_
 * template(), signed by the account): in an adopted group only the leaf's
 * 0x8009 entry is replaced, every advertised component kept, and members
 * follow it. */
static void
test_self_update_with_new_proof(void)
{
    Trio t;
    trio_create(&t);
    char *template = NULL;
    OK(marmot_group_account_proof_template(t.bob.m, &t.gid, &template));
    NostrEvent *ev = nostr_event_new();
    char sk_hex[65];
    sodium_bin2hex(sk_hex, sizeof(sk_hex), t.bob.sk, 32);
    CHECK(nostr_event_deserialize_compact(ev, template, NULL) && nostr_event_sign(ev, sk_hex) == 0,
          "sign the proof template");
    char *signed_json = nostr_event_serialize_compact(ev);
    nostr_event_free(ev);
    free(template);
    sodium_memzero(sk_hex, sizeof(sk_hex));
    char *commit = NULL;
    OK(marmot_self_update(t.bob.m, &t.gid, signed_json, &commit));
    Member *ac[] = {&t.alice, &t.carol};
    publish(&t.bob, &t.gid, commit, ac, 2);
    free(commit);
    free(signed_json);
    /* Bob's renewed leaf: still a proven adopted leaf. */
    uint8_t *blob = NULL;
    size_t len = 0;
    OK(t.alice.m->storage->mls_load(t.alice.m->storage->ctx, "mls_group", t.gid.data, t.gid.len,
                                    &blob, &len));
    MlsGroup g;
    CHECK(mls_group_deserialize(blob, len, &g) == 0, "state");
    sodium_memzero(blob, len);
    free(blob);
    OK(marmot_adopted_members_proven(&g));
    CHECK(g.epoch == 2, "epoch 2");
    mls_group_free(&g);
    trio_free(&t);
}

/* A routing rotation onto an address another group of ours already uses
 * (h tags are public) would misroute that group's traffic: refused, the
 * group untouched, the other group still delivered. */
static void
test_rotation_onto_another_group_refused(void)
{
    Trio t;
    trio_create(&t);
    /* A second adopted group of Alice's with Bob. */
    char *kb = adopted_key_package(&t.bob);
    const char *kps[] = {kb};
    const char *relays[] = {"wss://relay-a.example.com"};
    MarmotGroupConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "Second";
    cfg.relay_urls = (char **)relays;
    cfg.relay_count = 1;
    MarmotCreateGroupResult r;
    memset(&r, 0, sizeof(r));
    OK(marmot_create_group_for_profile(t.alice.m, MARMOT_GROUP_PROFILE_ADOPTED, t.alice.pk,
                                       t.alice.sk, NULL, NULL, kps, 1, &cfg, &r));
    MarmotGroupId gid2 = marmot_group_id_new(r.group->mls_group_id.data, r.group->mls_group_id.len);
    uint8_t ngid2[32];
    memcpy(ngid2, r.group->nostr_group_id, 32);
    OK(marmot_merge_pending_commit(t.alice.m, &gid2));
    OK(join(&t.bob, r.welcome_rumor_jsons[0]));
    marmot_create_group_result_free(&r);
    free(kb);

    /* Alice (an admin of the first group) rotates it onto the second's id. */
    uint8_t *blob = NULL;
    size_t len = 0;
    OK(t.alice.m->storage->mls_load(t.alice.m->storage->ctx, "mls_group", t.gid.data, t.gid.len,
                                    &blob, &len));
    MlsGroup g;
    CHECK(mls_group_deserialize(blob, len, &g) == 0, "state");
    sodium_memzero(blob, len);
    free(blob);
    uint8_t routing[32 + 1 + 1 + 25];
    memcpy(routing, ngid2, 32);
    routing[32] = 26;                       /* relays<V>: one entry */
    routing[33] = 25;                       /* url<V> */
    memcpy(routing + 34, "wss://relay-a.example.com", 25);
    MlsAppDataUpdate op = {0x8004, MLS_APP_DATA_UPDATE_OP_UPDATE, routing, sizeof(routing)};
    uint8_t exporter[32];
    memcpy(exporter, g.epoch_secrets.exporter_secret, 32);
    MlsAddResult res;
    memset(&res, 0, sizeof(res));
    CHECK(mls_group_commit_adopted(&g, NULL, 0, NULL, 0, &op, 1, &res) == 0, "MLS rotation");
    char *ev = marmot_commit_build_event(res.commit_data, res.commit_len, exporter, t.nostr_gid,
                                         marmot_now());
    expect_refused(&t.bob, &t.gid, ev, MARMOT_ERR_PROTOCOL_GROUP_MISMATCH,
                   "rotation onto another group's address");
    free(ev);
    mls_add_result_clear(&res);
    mls_group_free(&g);
    sodium_memzero(exporter, sizeof(exporter));

    /* The second group still reaches Bob. */
    MarmotOutgoingMessage out;
    memset(&out, 0, sizeof(out));
    OK(marmot_create_message(t.alice.m, &gid2,
                             "{\"kind\":9,\"content\":\"second group\",\"created_at\":1700000000,\"tags\":[]}",
                             &out));
    MarmotMessageResult m;
    OK(deliver(&t.bob, out.event_json, &m));
    CHECK(m.type == MARMOT_RESULT_APPLICATION_MESSAGE && strstr(m.app_msg.inner_event_json, "second group"),
          "second group delivered");
    marmot_message_result_free(&m);
    marmot_outgoing_message_free(&out);
    marmot_group_id_free(&gid2);
    trio_free(&t);
}

/* W24 review H1, now judged: a non-admin's Commit removing a member is
 * refused (never "everyone is an admin"), the victim untouched. */
static void
test_nonadmin_removal_refused(void)
{
    Trio t;
    trio_create(&t);
    uint8_t *blob = NULL;
    size_t len = 0;
    OK(t.bob.m->storage->mls_load(t.bob.m->storage->ctx, "mls_group", t.gid.data, t.gid.len, &blob,
                                  &len));
    MlsGroup g;
    CHECK(mls_group_deserialize(blob, len, &g) == 0, "state");
    sodium_memzero(blob, len);
    free(blob);
    uint32_t carol_leaf = UINT32_MAX;
    for (uint32_t i = 0; i < g.tree.n_leaves; i++) {
        const MlsNode *n = &g.tree.nodes[mls_tree_leaf_to_node(i)];
        if (n->type == MLS_NODE_LEAF && memcmp(n->leaf.credential_identity, t.carol.pk, 32) == 0)
            carol_leaf = i;
    }
    uint8_t exporter[32];
    memcpy(exporter, g.epoch_secrets.exporter_secret, 32);
    MlsAddResult res;
    memset(&res, 0, sizeof(res));
    CHECK(mls_group_commit_adopted(&g, &carol_leaf, 1, NULL, 0, NULL, 0, &res) == 0, "MLS Remove");
    char *ev = marmot_commit_build_event(res.commit_data, res.commit_len, exporter, t.nostr_gid,
                                         marmot_now());
    expect_refused(&t.carol, &t.gid, ev, MARMOT_ERR_COMMIT_FROM_NON_ADMIN, "carol, a non-admin's removal");
    expect_refused(&t.alice, &t.gid, ev, MARMOT_ERR_COMMIT_FROM_NON_ADMIN, "alice, a non-admin's removal");
    bool removed = true;
    OK(marmot_get_group_removal(t.carol.m, &t.gid, &removed, NULL, NULL, NULL));
    CHECK(!removed, "no removal recorded");
    free(ev);
    mls_add_result_clear(&res);
    mls_group_free(&g);
    sodium_memzero(exporter, sizeof(exporter));
    trio_free(&t);
}

/* nostrc-u9kv: a Commit of ours the group would refuse draws no created_at
 * from the group's floor (backends without transactions keep it): the
 * policy check runs first.  Bob's record still says "admin" while his MLS
 * state's 0x8003 no longer lists him. */
static void
test_refused_commit_draws_no_time(void)
{
    Trio t;
    trio_create(&t);
    /* Make Bob's stored MLS state list Alice alone (his record keeps the
     * creator's view: Alice; give him admin standing in the record). */
    MarmotGroup *rec = group_of(&t.bob, &t.gid);
    uint8_t (*admins)[32] = realloc(rec->admin_pubkeys, 2 * 32);
    CHECK(admins, "realloc");
    memcpy(admins[1], t.bob.pk, 32);
    rec->admin_pubkeys = admins;
    rec->admin_count = 2;
    OK(t.bob.m->storage->save_group(t.bob.m->storage->ctx, rec));
    marmot_group_free(rec);
    Snapshot before;
    snapshot(&t.bob, &t.gid, &before);
    MarmotGroupConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "not bob's to rename";
    char *ev = NULL;
    for (int i = 0; i < 100; i++)
        EXPECT_ERR(marmot_update_group_metadata(t.bob.m, &t.gid, &cfg, &ev),
                   MARMOT_ERR_COMMIT_FROM_NON_ADMIN);
    CHECK(!ev, "no event");
    expect_unchanged(&t.bob, &t.gid, &before, "refused own Commit");
    snapshot_clear(&before);
    trio_free(&t);
}

int
main(int argc, char **argv)
{
    if (argc > 1) g_only = argv[1];
    if (sodium_init() < 0) return 1;
    printf("libmarmot adopted Commit tests (nostrc-qp24.5.1.3)\n");
    RUN(test_mdk_commit_sequence);
    RUN(test_mdk_commit_before_its_proposal);
    RUN(test_mdk_competing_self_remove_commits);
    RUN(test_openmls_negatives);
    RUN(test_openmls_positives);
    RUN(test_openmls_by_reference);
    RUN(test_authorize_by_reference_rules);
    RUN(test_partial_write_rollback);
    RUN(test_own_commits);
    RUN(test_self_update_with_new_proof);
    RUN(test_rotation_onto_another_group_refused);
    RUN(test_nonadmin_removal_refused);
    RUN(test_refused_commit_draws_no_time);
    printf("all passed\n");
    return 0;
}
