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
#include <time.h>

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

/* The observer joins a forgery group from its KeyPackage and Welcome. */
static void
forgery_observer(Member *x, MarmotGroupId *gid, const char *kp_hex, const char *init,
                 const char *enc, const char *seed, const char *pub, const char *welcome,
                 const char *inviter_seed, const char *inviter_hex)
{
    member_init(x, "observer");
    size_t kp_len = 0;
    uint8_t *kp = unhex(kp_hex, &kp_len);
    install_key_package(x, kp, kp_len, init, enc, seed, pub);
    free(kp);
    uint8_t inviter[32];
    mdk_test_identity(inviter_seed, inviter);
    CHECK(same_key_hex(inviter, inviter_hex), "inviter identity");
    char *rumor = rumor_for(welcome, inviter);
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

static void
omls_observer(Member *x, MarmotGroupId *gid)
{
    forgery_observer(x, gid, H_OMLS_KP, H_OMLS_INIT_SK, H_OMLS_ENC_SK, H_OMLS_SIG_SEED,
                     H_OMLS_SIG_PUB, H_OMLS_WELCOME, "w24h-neg-x", H_OMLS_X);
}

/* The group that has not enabled lifecycle-v1 (slice H review M1). */
static void
lc_observer(Member *x, MarmotGroupId *gid)
{
    forgery_observer(x, gid, H_OMLS_LC_KP, H_OMLS_LC_INIT_SK, H_OMLS_LC_ENC_SK,
                     H_OMLS_LC_SIG_SEED, H_OMLS_LC_SIG_PUB, H_OMLS_LC_WELCOME, "w24h-lc-x",
                     H_OMLS_LC_XL);
}

static const char *
forgery_in(const AdoptedForgery *list, size_t n, const char *name)
{
    for (size_t i = 0; i < n; i++)
        if (strcmp(list[i].name, name) == 0) return list[i].message;
    CHECK(0, "no forgery %s", name);
    return NULL;
}

static const char *
forgery(const char *name)
{
    return forgery_in(H_OMLS_COMMITS, H_OMLS_COMMIT_COUNT, name);
}

/* How libmarmot judges a Commit (MLSMessage hex) of the stored epoch: the
 * specific refusal (marmot_commit_judge()), which the public API reports as
 * MARMOT_ERR_COMMIT_REFUSED when an admin made it. */
static MarmotError
judge(Member *x, const MarmotGroupId *gid, const char *mls_hex)
{
    size_t len = 0;
    uint8_t *msg = unhex(mls_hex, &len);
    MarmotError err = marmot_commit_judge(x->m, gid, msg, len);
    free(msg);
    return err;
}

typedef struct {
    const char *name;
    MarmotError want;     /* marmot_process_message() */
    MarmotError reason;   /* marmot_commit_judge() */
} Refusal;

/* Each of `cases` (forgeries of `list`) refused, with its reason, the group
 * untouched; then `ok` still applies. */
static void
expect_refusals(void (*observer)(Member *, MarmotGroupId *), const AdoptedForgery *list,
                size_t n_list, const Refusal *cases, size_t n, const char *ok)
{
    Member o;
    MarmotGroupId gid;
    observer(&o, &gid);
    for (size_t i = 0; i < n; i++) {
        const char *mls = forgery_in(list, n_list, cases[i].name);
        MarmotError reason = judge(&o, &gid, mls);
        CHECK(reason == cases[i].reason, "%s: reason %d (%s), want %d (%s)", cases[i].name,
              reason, marmot_error_string(reason), cases[i].reason,
              marmot_error_string(cases[i].reason));
        char *event = seal(&o, &gid, mls);
        expect_refused(&o, &gid, event, cases[i].want, cases[i].name);
        free(event);
    }
    /* After every refusal the group still follows a valid Commit. */
    char *event = seal(&o, &gid, forgery_in(list, n_list, ok));
    MarmotMessageResult r;
    OK(deliver(&o, event, &r));
    CHECK(r.type == MARMOT_RESULT_COMMIT, "%s applies after the refusals", ok);
    marmot_message_result_free(&r);
    free(event);
    marmot_group_id_free(&gid);
    member_free(&o);
}

static void
test_openmls_negatives(void)
{
#define ADMIN(name_, reason_) {name_, MARMOT_ERR_COMMIT_REFUSED, reason_}
    static const Refusal cases[] = {
        /* Authorization against the candidate parent (admins X and W): a
         * non-admin's Commit keeps its specific error (every member refuses
         * it; slice H review L2). */
        {"nonadmin_rename", MARMOT_ERR_COMMIT_FROM_NON_ADMIN, MARMOT_ERR_COMMIT_FROM_NON_ADMIN},
        {"nonadmin_add", MARMOT_ERR_COMMIT_FROM_NON_ADMIN, MARMOT_ERR_COMMIT_FROM_NON_ADMIN},
        /* Y's Remove of W, an admin, without dropping W's key: the
         * resulting-epoch check (an admin without a leaf) refuses it before
         * authorization does (a non-admin's Remove of a non-admin:
         * test_nonadmin_removal_refused). */
        {"nonadmin_remove", MARMOT_ERR_MLS_PROCESS_MESSAGE, MARMOT_ERR_MLS_PROCESS_MESSAGE},
        /* An admin's Commit refused for good: MARMOT_ERR_COMMIT_REFUSED (L2),
         * for its reason.  Resulting-epoch invariants (the MLS layer's
         * entered-epoch check, slice I's component validator). */
        ADMIN("drop_required_routing", MARMOT_ERR_MLS_PROCESS_MESSAGE),
        ADMIN("remove_admin_policy", MARMOT_ERR_MLS_PROCESS_MESSAGE),
        ADMIN("remove_lifecycle", MARMOT_ERR_MLS_PROCESS_MESSAGE),
        ADMIN("empty_admins", MARMOT_ERR_MLS_PROCESS_MESSAGE),
        ADMIN("admin_not_member", MARMOT_ERR_MLS_PROCESS_MESSAGE),
        ADMIN("remove_admin_uncoupled", MARMOT_ERR_MLS_PROCESS_MESSAGE),
        ADMIN("malformed_profile", MARMOT_ERR_MLS_PROCESS_MESSAGE),
        ADMIN("malformed_routing", MARMOT_ERR_MLS_PROCESS_MESSAGE),
        ADMIN("proof_in_group_context", MARMOT_ERR_MLS_PROCESS_MESSAGE),
        ADMIN("disband", MARMOT_ERR_MLS_PROCESS_MESSAGE),
        ADMIN("add_without_proof", MARMOT_ERR_MLS_PROCESS_MESSAGE),
        /* Malformed 0x800b, 0x8002, 0x8007 and 0x8005 states, and an agent
         * stream requiring the send role (slice I's validator; L4). */
        ADMIN("malformed_media_v2", MARMOT_ERR_MLS_PROCESS_MESSAGE),
        ADMIN("malformed_image", MARMOT_ERR_MLS_PROCESS_MESSAGE),
        ADMIN("avatar_not_url", MARMOT_ERR_MLS_PROCESS_MESSAGE),
        ADMIN("retention_7_bytes", MARMOT_ERR_MLS_PROCESS_MESSAGE),
        ADMIN("agent_stream_send_required", MARMOT_ERR_MLS_PROCESS_MESSAGE),
        /* Lifecycle transitions MDK refuses (M1; the reviewer's P1-P3). */
        ADMIN("lifecycle_unrequire", MARMOT_ERR_VALIDATION),
        ADMIN("lifecycle_redundant", MARMOT_ERR_VALIDATION),
        ADMIN("lifecycle_unrequire_rename", MARMOT_ERR_VALIDATION),
        /* A removal of our leaf judged whole (L1): its public result has a
         * malformed 0x8002, which every member refuses. */
        ADMIN("remove_observer_malformed", MARMOT_ERR_MLS_PROCESS_MESSAGE),
        /* What libmarmot cannot judge as MDK does: fail closed. */
        ADMIN("group_context_extensions", MARMOT_ERR_UNSUPPORTED),
        /* A proof that does not verify: its own refusal state (nostrc-prrl). */
        {"add_bad_proof", MARMOT_ERR_KEY_PACKAGE_IDENTITY, MARMOT_ERR_KEY_PACKAGE_IDENTITY},
    };
#undef ADMIN
    expect_refusals(omls_observer, H_OMLS_COMMITS, H_OMLS_COMMIT_COUNT, cases,
                    sizeof(cases) / sizeof(cases[0]), "ok_rename");
}

/* MDK's lifecycle enablement rules (validate_group_lifecycle_transition;
 * slice H review M1) in a group that has not enabled lifecycle-v1. */
static void
test_lifecycle_enablement(void)
{
    static const Refusal cases[] = {
        /* Enabling with an unrelated proposal riding along. */
        {"enable_lifecycle_with_rename", MARMOT_ERR_COMMIT_REFUSED, MARMOT_ERR_VALIDATION},
        /* A lifecycle state without the requirement. */
        {"lifecycle_state_unrequired", MARMOT_ERR_COMMIT_REFUSED, MARMOT_ERR_VALIDATION},
    };
    expect_refusals(lc_observer, H_OMLS_LC_COMMITS, H_OMLS_LC_COMMIT_COUNT, cases,
                    sizeof(cases) / sizeof(cases[0]), "ok_enable_lifecycle");

    /* The enablement applies (MDK's EnableDisbanding shape) and its rules
     * judge the shape, not only the states: the same states with a
     * by-reference proposal, or with no shape known, are refused. */
    Member o;
    MarmotGroupId gid;
    lc_observer(&o, &gid);
    MlsGroup pre, post;
    uint8_t *blob = NULL;
    size_t len = 0;
    OK(o.m->storage->mls_load(o.m->storage->ctx, "mls_group", gid.data, gid.len, &blob, &len));
    CHECK(mls_group_deserialize(blob, len, &pre) == 0, "pre");
    sodium_memzero(blob, len);
    free(blob);
    char *event = seal(&o, &gid, forgery_in(H_OMLS_LC_COMMITS, H_OMLS_LC_COMMIT_COUNT,
                                             "ok_enable_lifecycle"));
    MarmotMessageResult r;
    OK(deliver(&o, event, &r));
    CHECK(r.type == MARMOT_RESULT_COMMIT, "enablement applied");
    marmot_message_result_free(&r);
    free(event);
    MarmotGroupComponents parts;
    OK(marmot_get_group_components(o.m, &gid, &parts));
    bool required = false;
    for (size_t i = 0; i < parts.required_component_count; i++)
        required |= parts.required_components[i] == 0x800c;
    CHECK(required && parts.epoch == 2, "lifecycle-v1 now required");
    marmot_group_components_clear(&parts);
    OK(o.m->storage->mls_load(o.m->storage->ctx, "mls_group", gid.data, gid.len, &blob, &len));
    CHECK(mls_group_deserialize(blob, len, &post) == 0, "post");
    sodium_memzero(blob, len);
    free(blob);
    MarmotCommitKey key;
    MarmotGroupDataExtension *gde = NULL;
    MlsCommitSummary s;
    memset(&s, 0, sizeof(s));
    s.shape_known = true;
    s.has_path = true;
    s.proposal_count = 2;
    s.adu_count = 2;
    s.adu_lifecycle_count = 1;
    s.adu_inline_enablement_count = 2;
    OK(marmot_commit_authorize_ex(&pre, &post, 0, false, &s, &key, &gde));
    s.adu_inline_enablement_count = 1;   /* one of the two by reference */
    s.ref_count = 1;
    s.ref_type[0] = MLS_PROPOSAL_APP_DATA_UPDATE;
    s.ref_component[0] = 0x0001;
    s.ref_sender[0] = 0;
    EXPECT_ERR(marmot_commit_authorize_ex(&pre, &post, 0, false, &s, &key, &gde),
               MARMOT_ERR_VALIDATION);
    EXPECT_ERR(marmot_commit_authorize_ex(&pre, &post, 0, false, NULL, &key, &gde),
               MARMOT_ERR_VALIDATION);
    /* Each rule on its own, with states made by applying AppDataUpdates and
     * a summary that names no lifecycle update: (a) un-requiring 0x800c,
     * its state kept; (b) a lifecycle state appearing while 0x800c is not
     * required. */
    memset(&s, 0, sizeof(s));
    s.shape_known = true;
    s.has_path = true;
    s.proposal_count = 1;
    s.adu_count = 1;
    {
        static const uint8_t unrequired[] = {0x08, 0x80, 0x01, 0x80, 0x03, 0x80, 0x04, 0x80, 0x09};
        MlsAppDataUpdate op = {0x0001, MLS_APP_DATA_UPDATE_OP_UPDATE, (uint8_t *)unrequired,
                               sizeof(unrequired)};
        const MlsAppDataUpdate *ops[] = {&op};
        MlsGroup next;
        uint8_t *b2 = NULL;
        size_t l2 = 0;
        CHECK(mls_group_serialize(&post, &b2, &l2) == 0 && mls_group_deserialize(b2, l2, &next) == 0,
              "clone");
        sodium_memzero(b2, l2);
        free(b2);
        uint8_t *ext = NULL;
        size_t ext_len = 0;
        CHECK(mls_app_data_update_apply(post.extensions_data, post.extensions_len, ops, 1, &ext,
                                        &ext_len) == 0, "apply");
        free(next.extensions_data);
        next.extensions_data = ext;
        next.extensions_len = ext_len;
        EXPECT_ERR(marmot_commit_authorize_ex(&post, &next, 0, false, &s, &key, &gde),
                   MARMOT_ERR_VALIDATION);   /* (a) */
        mls_group_free(&next);
    }
    {
        static const uint8_t active[] = {0x00};
        MlsAppDataUpdate op = {0x800c, MLS_APP_DATA_UPDATE_OP_UPDATE, (uint8_t *)active, 1};
        const MlsAppDataUpdate *ops[] = {&op};
        MlsGroup next;
        uint8_t *b2 = NULL;
        size_t l2 = 0;
        CHECK(mls_group_serialize(&pre, &b2, &l2) == 0 && mls_group_deserialize(b2, l2, &next) == 0,
              "clone");
        sodium_memzero(b2, l2);
        free(b2);
        uint8_t *ext = NULL;
        size_t ext_len = 0;
        CHECK(mls_app_data_update_apply(pre.extensions_data, pre.extensions_len, ops, 1, &ext,
                                        &ext_len) == 0, "apply");
        free(next.extensions_data);
        next.extensions_data = ext;
        next.extensions_len = ext_len;
        EXPECT_ERR(marmot_commit_authorize_ex(&pre, &next, 0, false, &s, &key, &gde),
                   MARMOT_ERR_VALIDATION);   /* (b) */
        mls_group_free(&next);
    }
    CHECK(!gde, "no GroupData");
    mls_group_free(&pre);
    mls_group_free(&post);
    marmot_group_id_free(&gid);
    member_free(&o);
}

/* A Commit removing our leaf is judged whole (slice H review L1): the
 * valid one ends the group for us; the one with a malformed component
 * every member refuses (test_openmls_negatives) does not. */
static void
test_removal_judged_whole(void)
{
    Member o;
    MarmotGroupId gid;
    omls_observer(&o, &gid);
    char *bad = seal(&o, &gid, forgery("remove_observer_malformed"));
    expect_refused(&o, &gid, bad, MARMOT_ERR_COMMIT_REFUSED, "removal with a malformed 0x8002");
    free(bad);
    bool removed = true;
    OK(marmot_get_group_removal(o.m, &gid, &removed, NULL, NULL, NULL));
    CHECK(!removed, "not removed by a refused Commit");
    char *ok = seal(&o, &gid, forgery("ok_remove_observer"));
    MarmotMessageResult r;
    MarmotError err = deliver(&o, ok, &r);
    marmot_message_result_free(&r);
    free(ok);
    CHECK(err == MARMOT_OK || err == MARMOT_ERR_USE_AFTER_EVICTION, "valid removal: %d (%s)",
          err, marmot_error_string(err));
    uint8_t remover[32];
    OK(marmot_get_group_removal(o.m, &gid, &removed, remover, NULL, NULL));
    CHECK(removed && same_key_hex(remover, H_OMLS_X), "removed by X");
    marmot_group_id_free(&gid);
    member_free(&o);
}

static void
test_openmls_positives(void)
{
    static const char *const names[] = {
        "ok_rename", "ok_unknown_component", "ok_nonadmin_self_update",
        "ok_remove_admin_coupled", "ok_add",
        /* Valid White Noise component updates (slice I's validator): encrypted
         * media v2 and the agent stream's receive role (MC). */
        "ok_media_v2", "ok_agent_stream",
        /* Valid 0x8002, 0x8007 and 0x8005 states (L4). */
        "ok_image", "ok_avatar", "ok_retention",
        /* What MDK accepts (L3): a removal of a component with no state, 20
         * AppDataUpdates in one Commit; and un-requiring 0x8001 (M1's
         * control, P4). */
        "ok_remove_absent", "ok_twenty_updates", "ok_unrequire_profile"};
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
        MarmotGroupComponents parts;
        OK(marmot_get_group_components(o.m, &gid, &parts));
        if (strcmp(names[i], "ok_media_v2") == 0)
            CHECK(parts.has_media_policy && parts.media_policy.default_blob_endpoint_count == 1 &&
                      strcmp(parts.media_policy.default_blob_endpoints[0].base_url,
                             "https://blossom.example.com/") == 0,
                  "media policy followed");
        if (strcmp(names[i], "ok_agent_stream") == 0)
            CHECK(parts.has_agent_text_stream &&
                      parts.agent_text_stream.required_member_roles ==
                          MARMOT_AGENT_STREAM_ROLE_RECEIVE,
                  "agent stream followed");
        if (strcmp(names[i], "ok_image") == 0)
            CHECK(parts.image.present, "image followed");
        if (strcmp(names[i], "ok_avatar") == 0)
            CHECK(parts.avatar_url.url &&
                      strcmp(parts.avatar_url.url, "https://example.com/avatar.png") == 0,
                  "avatar followed");
        if (strcmp(names[i], "ok_unrequire_profile") == 0) {
            bool profile_required = false;
            for (size_t k = 0; k < parts.required_component_count; k++)
                profile_required |= parts.required_components[k] == 0x8001;
            CHECK(!profile_required && strcmp(parts.name, "W24-H forgeries") == 0,
                  "0x8001 no longer required, its state kept");
        }
        marmot_group_components_clear(&parts);
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
    /* The processor reports a by-reference AppDataUpdate's sender and
     * component to the authorization (slice H review L4: the summary's
     * capture, which the ingest's refusal of a non-admin's proposal
     * otherwise hides): X's Commit of Y's rename, Y's proposal opened
     * directly (libmarmot refused to keep it). */
    {
        uint8_t *blob = NULL;
        size_t len = 0;
        OK(o.m->storage->mls_load(o.m->storage->ctx, "mls_group", gid.data, gid.len, &blob,
                                  &len));
        MlsGroup g1;
        CHECK(mls_group_deserialize(blob, len, &g1) == 0, "state");
        sodium_memzero(blob, len);
        free(blob);
        size_t pl = 0, cl = 0;
        uint8_t *pm = unhex(H_OMLS_REF_PROPOSAL_Y, &pl);
        uint8_t *cm = unhex(H_OMLS_REF_COMMIT_X_OF_Y, &cl);
        MlsOpenedProposal opened;
        CHECK(mls_group_open_proposal(&g1, pm, pl, &opened) == 0 &&
                  opened.sender_leaf == H_OMLS_Y_LEAF,
              "Y's proposal opens");
        bool cited = false;
        for (size_t i = 0; i + MLS_HASH_LEN <= cl && !cited; i++)
            cited = memcmp(cm + i, opened.ref, MLS_HASH_LEN) == 0;
        CHECK(cited, "X's Commit cites Y's proposal by reference");
        const uint8_t *acs[1] = {opened.ac};
        const size_t ac_lens[1] = {opened.ac_len};
        MlsCommitSummary sum;
        memset(&sum, 0, sizeof(sum));
        CHECK(mls_group_process_commit_by_ref(&g1, cm, cl, 0, acs, ac_lens, 1, &sum) == 0,
              "processed");
        CHECK(sum.shape_known && sum.proposal_count == 1 && sum.adu_count == 1 &&
                  sum.ref_count == 1 && sum.ref_sender[0] == H_OMLS_Y_LEAF &&
                  sum.ref_type[0] == MLS_PROPOSAL_APP_DATA_UPDATE &&
                  sum.ref_component[0] == 0x8001 && sum.adu_inline_enablement_count == 0,
              "by-reference sender in the summary: proposals %zu adu %zu refs %zu sender %u "
              "type %u component 0x%04x",
              sum.proposal_count, sum.adu_count, sum.ref_count, sum.ref_sender[0],
              sum.ref_type[0], sum.ref_component[0]);
        mls_opened_proposal_clear(&opened);
        free(pm);
        free(cm);
        mls_group_free(&g1);
    }
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
    /* A SelfRemove's sender must be no admin of its source epoch
     * (admin-policy-v1.md; MDK reject_admin_self_remove_proposals): here
     * the authorization's own check, which the ingest's refusal of an
     * admin's SelfRemove proposal otherwise hides (slice H review L4).
     * The states are the parent's: what is judged is the shape. */
    memset(&s, 0, sizeof(s));
    s.shape_known = true;
    s.has_path = true;
    s.proposal_count = 1;
    s.self_remove_count = 1;
    s.self_removed[0] = H_OMLS_W_LEAF;   /* an admin */
    EXPECT_ERR(marmot_commit_authorize_ex(&pre, &post, H_OMLS_Y_LEAF, false, &s, &key, &gde),
               MARMOT_ERR_ADMIN_CANNOT_LEAVE);
    s.self_removed[0] = H_OMLS_Y_LEAF;   /* no admin; committed by X */
    OK(marmot_commit_authorize_ex(&pre, &post, x_leaf, false, &s, &key, &gde));
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
    /* Judged on its own the Commit is valid; it is the routing record that
     * refuses it, for good: Alice is an admin, so the application hears
     * MARMOT_ERR_COMMIT_REFUSED (slice H review L2). */
    OK(marmot_commit_judge(t.bob.m, &t.gid, res.commit_data, res.commit_len));
    expect_refused(&t.bob, &t.gid, ev, MARMOT_ERR_COMMIT_REFUSED,
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

/* Alice's rotation of the trio's routing to `to`, from her MLS state `g`
 * (advanced), sealed at address `at`, followed by Bob. */
static void
rotate(Trio *t, MlsGroup *g, const uint8_t at[32], const uint8_t to[32])
{
    uint8_t routing[32 + 1 + 1 + 25];
    memcpy(routing, to, 32);
    routing[32] = 26;                       /* relays<V>: one entry */
    routing[33] = 25;                       /* url<V> */
    memcpy(routing + 34, "wss://relay-a.example.com", 25);
    MlsAppDataUpdate op = {0x8004, MLS_APP_DATA_UPDATE_OP_UPDATE, routing, sizeof(routing)};
    uint8_t exporter[32];
    memcpy(exporter, g->epoch_secrets.exporter_secret, 32);
    MlsAddResult res;
    memset(&res, 0, sizeof(res));
    CHECK(mls_group_commit_adopted(g, NULL, 0, NULL, 0, &op, 1, &res) == 0, "MLS rotation");
    char *ev = marmot_commit_build_event(res.commit_data, res.commit_len, exporter, at,
                                         marmot_now());
    MarmotMessageResult r;
    OK(deliver(&t->bob, ev, &r));
    CHECK(r.type == MARMOT_RESULT_COMMIT && r.commit.routing_changed &&
              memcmp(r.commit.previous_nostr_group_id, at, 32) == 0,
          "rotation followed");
    marmot_message_result_free(&r);
    free(ev);
    mls_add_result_clear(&res);
    sodium_memzero(exporter, sizeof(exporter));
}

/* A group that rotates back to an earlier address (slice H review N5): the
 * address is current again, and no longer listed among the previous ones
 * (nor kept as an alias of itself). */
static void
test_rotation_back_to_old_address(void)
{
    Trio t;
    trio_create(&t);
    uint8_t *blob = NULL;
    size_t len = 0;
    OK(t.alice.m->storage->mls_load(t.alice.m->storage->ctx, "mls_group", t.gid.data, t.gid.len,
                                    &blob, &len));
    MlsGroup g;
    CHECK(mls_group_deserialize(blob, len, &g) == 0, "state");
    sodium_memzero(blob, len);
    free(blob);
    uint8_t away[32];
    randombytes_buf(away, sizeof(away));
    rotate(&t, &g, t.nostr_gid, away);
    rotate(&t, &g, away, t.nostr_gid);
    uint8_t current[32];
    char **relays = NULL;
    size_t n_relays = 0;
    uint8_t (*previous)[32] = NULL;
    size_t n_previous = 0;
    OK(marmot_get_group_routing(t.bob.m, &t.gid, current, &relays, &n_relays, &previous,
                                &n_previous));
    CHECK(memcmp(current, t.nostr_gid, 32) == 0, "back at the first address");
    CHECK(n_previous == 1 && memcmp(previous[0], away, 32) == 0,
          "previous: only the address left (%zu)", n_previous);
    for (size_t i = 0; i < n_relays; i++) free(relays[i]);
    free(relays);
    free(previous);
    mls_group_free(&g);
    trio_free(&t);
}

/* ── Re-review (R2, R3, R5) ─────────────────────────────────────────── */

static double
now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void
load_group(Member *x, const MarmotGroupId *gid, MlsGroup *out)
{
    uint8_t *blob = NULL;
    size_t len = 0;
    OK(x->m->storage->mls_load(x->m->storage->ctx, "mls_group", gid->data, gid->len, &blob, &len));
    CHECK(mls_group_deserialize(blob, len, out) == 0, "state");
    sodium_memzero(blob, len);
    free(blob);
}

static char *
to_hex(const uint8_t *bin, size_t len)
{
    char *hex = malloc(2 * len + 1);
    CHECK(hex, "alloc");
    sodium_bin2hex(hex, 2 * len + 1, bin, len);
    return hex;
}

/* `commit` as a PublicMessage of `g`'s epoch, framed, signed and
 * membership-tagged by `g`'s own member -- what a modified client of that
 * member can send -- with confirmation tag `tag` (which only a member of
 * the next epoch can check). */
static uint8_t *
frame_commit(const MlsGroup *g, const MlsCommit *commit, const uint8_t tag[MLS_HASH_LEN],
             size_t *out_len)
{
    uint8_t *gc = NULL;
    size_t gc_len = 0;
    CHECK(mls_group_context_build(g, &gc, &gc_len) == 0, "group context");
    MlsTlsBuf body = {0};
    CHECK(mls_tls_buf_init(&body, 4096) == 0 && mls_commit_serialize(commit, &body) == 0,
          "Commit");
    MlsMLSMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.wire_format = MLS_WIRE_FORMAT_PUBLIC_MESSAGE;
    msg.cipher_suite = MARMOT_CIPHERSUITE;
    MlsPublicMessage *pm = &msg.public_message;
    pm->content.group_id = malloc(g->group_id_len);
    CHECK(pm->content.group_id, "alloc");
    memcpy(pm->content.group_id, g->group_id, g->group_id_len);
    pm->content.group_id_len = g->group_id_len;
    pm->content.epoch = g->epoch;
    pm->content.sender.sender_type = MLS_SENDER_TYPE_MEMBER;
    pm->content.sender.leaf_index = g->own_leaf_index;
    pm->content.content_type = MLS_CONTENT_TYPE_COMMIT;
    pm->content.content = body.data;
    pm->content.content_len = body.len;
    body.data = NULL;
    size_t body_len = body.len;
    mls_tls_buf_free(&body);
    CHECK(mls_framed_content_sign(&pm->content, MLS_WIRE_FORMAT_PUBLIC_MESSAGE, gc, gc_len,
                                  g->own_signature_key, &pm->auth) == 0, "signature");
    pm->auth.has_confirmation_tag = true;
    memcpy(pm->auth.confirmation_tag, tag, MLS_HASH_LEN);
    pm->auth.confirmation_tag_len = MLS_HASH_LEN;
    CHECK(mls_public_message_compute_membership_tag(pm, g->epoch_secrets.membership_key, gc,
                                                    gc_len) == 0, "membership tag");
    MlsTlsBuf out = {0};
    CHECK(mls_tls_buf_init(&out, body_len + 1024) == 0 && mls_message_serialize(&msg, &out) == 0,
          "MLSMessage");
    mls_message_clear(&msg);
    free(gc);
    *out_len = out.len;
    return out.data;
}

/* Re-review R2: a member (Bob, no admin) sends a Commit of 40,000
 * AppDataUpdates followed by 4,000 Adds -- the order the application sort
 * reverses, before any authorization -- and the same proposals already in
 * application order (Adds first).  Both are refused (their Adds do not
 * verify), and the reversed one costs about what the ordered one does: the
 * sort is one linear pass (the insertion sort it replaced moved 512-byte
 * proposals O(n^2) times -- 60x the ordered cost here, seconds).  Measured
 * as a ratio, which sanitizers and slow machines leave alone.  A Commit
 * with more proposals than any may have (MLS_COMMIT_MAX_PROPOSALS) is
 * refused while it is parsed, and that bound is exact. */
static void
test_large_commit_refused_quickly(void)
{
    Trio t;
    trio_create(&t);
    MlsGroup bob;
    load_group(&t.bob, &t.gid, &bob);
    uint8_t who[32];
    randombytes_buf(who, sizeof(who));
    MlsKeyPackage kp;
    MlsKeyPackagePrivate priv;
    CHECK(mls_key_package_create(&kp, &priv, who, 32, NULL, 0) == 0, "KeyPackage");
    kp.signature[0] ^= 0x01;   /* validated only when its Add is applied */
    enum { K = 40000, A = 4000 };
    const size_t n = MLS_COMMIT_MAX_PROPOSALS + 1;
    MlsProposal *props = calloc(n, sizeof(*props));
    MlsProposal *ordered = calloc(K + A, sizeof(*ordered));
    CHECK(props && ordered, "alloc");
    static uint8_t one = 1;
    for (size_t i = 0; i < n; i++) {
        props[i].sender_leaf = UINT32_MAX;
        props[i].update_leaf_index = UINT32_MAX;
        if (i >= K && i < K + A) {
            props[i].type = MLS_PROPOSAL_ADD;
            props[i].add.key_package = kp;   /* shallow: serialized, never freed here */
        } else {
            props[i].type = MLS_PROPOSAL_APP_DATA_UPDATE;
            props[i].app_data_update.component_id = (uint16_t)(0x1000 + i);
            props[i].app_data_update.operation = MLS_APP_DATA_UPDATE_OP_UPDATE;
            props[i].app_data_update.update = &one;
            props[i].app_data_update.update_len = 1;
        }
    }
    memcpy(ordered, props + K, A * sizeof(*ordered));       /* the Adds first */
    memcpy(ordered + A, props, K * sizeof(*ordered));       /* then the updates */
    uint8_t tag[MLS_HASH_LEN];
    memset(tag, 0x5a, sizeof(tag));
    MlsCommit c;
    memset(&c, 0, sizeof(c));

    double cost[2];
    for (int k = 0; k < 2; k++) {
        c.proposals = k == 0 ? ordered : props;   /* application order, then reversed */
        c.proposal_count = K + A;
        size_t len = 0;
        uint8_t *msg = frame_commit(&bob, &c, tag, &len);
        double t0 = now_s();
        MarmotError err = marmot_commit_judge(t.alice.m, &t.gid, msg, len);
        cost[k] = now_s() - t0;
        CHECK(err == MARMOT_ERR_MLS_PROCESS_MESSAGE, "refused: %d (%s)", err,
              marmot_error_string(err));
        free(msg);
    }
    printf("[ordered %.3f s, reversed %.3f s] ", cost[0], cost[1]);
    CHECK(cost[1] < 3.0 * cost[0] + 0.05,
          "40,000 AppDataUpdates then 4,000 Adds: %.3f s, in application order %.3f s",
          cost[1], cost[0]);

    /* Over the bound: refused at parse, no dearer than the ones above. */
    c.proposals = props;
    c.proposal_count = n;
    size_t len = 0;
    uint8_t *msg = frame_commit(&bob, &c, tag, &len);
    double t0 = now_s();
    MarmotError err = marmot_commit_judge(t.alice.m, &t.gid, msg, len);
    double dt = now_s() - t0;
    CHECK(err == MARMOT_ERR_MLS_PROCESS_MESSAGE, "refused: %d (%s)", err, marmot_error_string(err));
    CHECK(dt < 3.0 * cost[0] + 0.05, "%zu proposals refused in %.3f s", n, dt);
    free(msg);

    /* The bound, exactly, on the Commit body. */
    for (size_t extra = 0; extra < 2; extra++) {
        c.proposal_count = MLS_COMMIT_MAX_PROPOSALS + extra;
        MlsTlsBuf body = {0};
        CHECK(mls_tls_buf_init(&body, 4096) == 0 && mls_commit_serialize(&c, &body) == 0, "body");
        MlsTlsReader rd;
        mls_tls_reader_init(&rd, body.data, body.len);
        MlsCommit parsed;
        int rc = mls_commit_deserialize(&rd, &parsed);
        if (extra == 0) {
            CHECK(rc == 0 && parsed.proposal_count == MLS_COMMIT_MAX_PROPOSALS,
                  "MLS_COMMIT_MAX_PROPOSALS proposals parse");
            mls_commit_clear(&parsed);
        } else {
            CHECK(rc != 0, "one more is refused");
        }
        mls_tls_buf_free(&body);
    }
    free(ordered);
    free(props);
    mls_key_package_clear(&kp);
    mls_key_package_private_clear(&priv);
    mls_group_free(&bob);
    trio_free(&t);
}

/* Re-review R3: an admin's Commit that does not authenticate (its
 * membership tag tampered) is never reported as the group's own
 * (MARMOT_ERR_COMMIT_REFUSED): anyone holding the exporter secret could
 * otherwise raise every member's "change refused" with a forged admin
 * sender.  The same Commit intact is (test_openmls_negatives). */
static void
test_tampered_admin_commit_not_reported(void)
{
    Member o;
    MarmotGroupId gid;
    omls_observer(&o, &gid);
    size_t len = 0;
    uint8_t *msg = unhex(forgery("lifecycle_redundant"), &len);
    msg[len - 1] ^= 0x01;   /* the membership tag, the PublicMessage's last field */
    char *hex = to_hex(msg, len);
    EXPECT_ERR(judge(&o, &gid, hex), MARMOT_ERR_MLS_PROCESS_MESSAGE);
    char *event = seal(&o, &gid, hex);
    expect_refused(&o, &gid, event, MARMOT_ERR_MLS_PROCESS_MESSAGE, "a tampered admin Commit");
    free(event);
    free(hex);
    free(msg);
    marmot_group_id_free(&gid);
    member_free(&o);
}

/* Re-review R3: the lifecycle enablement with its 0x0001 update by
 * reference (WL's standalone proposal, an admin's, kept) and its 0x800c
 * state inline: MDK requires every enablement proposal inline, so the
 * processor must not count the referenced update as inline. */
static void
test_enablement_by_reference_refused(void)
{
    Member o;
    MarmotGroupId gid;
    lc_observer(&o, &gid);
    char *proposal = seal(&o, &gid, H_OMLS_LC_REF_PROPOSAL_WL_REQUIREMENTS);
    MarmotMessageResult r;
    OK(deliver(&o, proposal, &r));
    CHECK(r.type == MARMOT_RESULT_PROPOSAL, "an admin's 0x0001 proposal is kept");
    marmot_message_result_free(&r);
    EXPECT_ERR(judge(&o, &gid, H_OMLS_LC_REF_COMMIT_XL_ENABLE_BY_REF), MARMOT_ERR_VALIDATION);
    char *commit = seal(&o, &gid, H_OMLS_LC_REF_COMMIT_XL_ENABLE_BY_REF);
    expect_refused(&o, &gid, commit, MARMOT_ERR_COMMIT_REFUSED, "the enablement by reference");
    free(proposal);
    free(commit);
    marmot_group_id_free(&gid);
    member_free(&o);
}

/* Re-review R5: Alice's removal of Carol with one UpdatePath node's public
 * key altered (the Commit re-signed and re-tagged by Alice: the parent-hash
 * chain of her new leaf no longer matches).  Carol, whom it removes, checks
 * the chain on its public result as Bob does on the full Commit: neither
 * follows it, and Carol is not removed.  The Commit intact removes her. */
static void
test_removal_parent_hash_checked(void)
{
    Trio t;
    trio_create(&t);
    MlsGroup pre, g, carol;
    load_group(&t.alice, &t.gid, &pre);
    load_group(&t.alice, &t.gid, &g);
    load_group(&t.carol, &t.gid, &carol);
    uint32_t rm = carol.own_leaf_index;
    MlsAddResult res;
    memset(&res, 0, sizeof(res));
    CHECK(mls_group_commit_adopted(&g, &rm, 1, NULL, 0, NULL, 0, &res) == 0, "Alice removes Carol");

    MlsTlsReader rd;
    mls_tls_reader_init(&rd, res.commit_data, res.commit_len);
    MlsMLSMessage wire;
    CHECK(mls_message_deserialize(&rd, &wire) == 0 &&
              wire.wire_format == MLS_WIRE_FORMAT_PUBLIC_MESSAGE, "PublicMessage");
    MlsTlsReader cr;
    mls_tls_reader_init(&cr, wire.public_message.content.content,
                        wire.public_message.content.content_len);
    MlsCommit commit;
    CHECK(mls_commit_deserialize(&cr, &commit) == 0 && commit.has_path &&
              commit.path.node_count >= 1, "Commit with a path");
    commit.path.nodes[0].encryption_key[0] ^= 0x01;
    size_t len = 0;
    uint8_t *tampered = frame_commit(&pre, &commit, wire.public_message.auth.confirmation_tag, &len);
    mls_commit_clear(&commit);
    mls_message_clear(&wire);
    char *hex = to_hex(tampered, len);
    EXPECT_ERR(marmot_commit_judge(t.carol.m, &t.gid, tampered, len), MARMOT_ERR_MLS_PROCESS_MESSAGE);
    char *bad = marmot_commit_build_event(tampered, len, pre.epoch_secrets.exporter_secret,
                                          t.nostr_gid, marmot_now());
    expect_refused(&t.carol, &t.gid, bad, MARMOT_ERR_COMMIT_REFUSED, "Carol: a bad parent hash");
    expect_refused(&t.bob, &t.gid, bad, MARMOT_ERR_COMMIT_REFUSED, "Bob: a bad parent hash");
    bool removed = true;
    OK(marmot_get_group_removal(t.carol.m, &t.gid, &removed, NULL, NULL, NULL));
    CHECK(!removed, "Carol not removed by it");

    char *ok = marmot_commit_build_event(res.commit_data, res.commit_len,
                                         pre.epoch_secrets.exporter_secret, t.nostr_gid,
                                         marmot_now());
    MarmotMessageResult r;
    MarmotError err = deliver(&t.carol, ok, &r);
    marmot_message_result_free(&r);
    CHECK(err == MARMOT_OK || err == MARMOT_ERR_USE_AFTER_EVICTION, "intact: %d (%s)", err,
          marmot_error_string(err));
    OK(marmot_get_group_removal(t.carol.m, &t.gid, &removed, NULL, NULL, NULL));
    CHECK(removed, "the intact Commit removes Carol");
    free(ok);
    free(bad);
    free(hex);
    free(tampered);
    mls_add_result_clear(&res);
    mls_group_free(&pre);
    mls_group_free(&g);
    mls_group_free(&carol);
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

static void
test_group_image_component_updates(void)
{
    Trio t;
    trio_create(&t);
    Member *receivers[] = { &t.bob, &t.carol };
    char *event = NULL;
    const uint8_t pixels[] = { 0x89, 'P', 'N', 'G', 1, 2, 3 };
    MarmotGroupBlossomImage image = {0};
    uint8_t *ciphertext = NULL;
    size_t ciphertext_len = 0;
    OK(marmot_group_image_encrypt(pixels, sizeof pixels, "image/png", &image,
                                  &ciphertext, &ciphertext_len));
    CHECK(ciphertext_len > sizeof pixels, "image encrypted before component update");
    EXPECT_ERR(marmot_update_group_blossom_image(t.bob.m, &t.gid, &image, &event),
               MARMOT_ERR_ADMIN_ONLY);
    CHECK(event == NULL, "non-admin produced no image Commit");
    /* An invalid state is refused before any Commit is staged. */
    MarmotGroupBlossomImage bad = image;
    bad.media_type = "Image/PNG";   /* not canonical */
    CHECK(marmot_update_group_blossom_image(t.alice.m, &t.gid, &bad, &event) != MARMOT_OK &&
          event == NULL, "non-canonical image state refused");
    MarmotGroupAvatarUrl http = { .url = "http://example.com/a.png" };
    CHECK(marmot_update_group_avatar_url(t.alice.m, &t.gid, &http, &event) != MARMOT_OK &&
          event == NULL, "an avatar URL libmarmot cannot produce is refused");
    /* The Commit is pending until published: one at a time, and clearing it
     * leaves the group as it was. */
    Snapshot before;
    snapshot(&t.alice, &t.gid, &before);
    OK(marmot_update_group_blossom_image(t.alice.m, &t.gid, &image, &event));
    CHECK(event != NULL, "a Commit to publish");
    free(event);
    event = NULL;
    EXPECT_ERR(marmot_update_group_blossom_image(t.alice.m, &t.gid, NULL, &event),
               MARMOT_ERR_OWN_COMMIT_PENDING);
    OK(marmot_clear_pending_commit(t.alice.m, &t.gid));
    MarmotGroupComponents unchanged;
    OK(marmot_get_group_components(t.alice.m, &t.gid, &unchanged));
    CHECK(!unchanged.image.present, "an unpublished image Commit changed nothing");
    marmot_group_components_clear(&unchanged);
    {
        /* The dated floor may move (a built Commit reserves its time, as
         * for metadata); the epoch and MLS state must not. */
        Snapshot after;
        snapshot(&t.alice, &t.gid, &after);
        CHECK(after.epoch == before.epoch && after.state_len == before.state_len &&
              memcmp(after.state, before.state, after.state_len) == 0,
              "cleared image Commit left the MLS state as it was");
        snapshot_clear(&after);
    }
    snapshot_clear(&before);
    OK(marmot_update_group_blossom_image(t.alice.m, &t.gid, &image, &event));
    publish(&t.alice, &t.gid, event, receivers, 2);
    free(event);
    event = NULL;
    MarmotGroupComponents parts;
    OK(marmot_get_group_components(t.bob.m, &t.gid, &parts));
    CHECK(parts.image.present &&
          memcmp(parts.image.image_hash, image.image_hash, 32) == 0 &&
          parts.avatar_source == MARMOT_GROUP_AVATAR_BLOSSOM,
          "image component applied by receiver");
    marmot_group_components_clear(&parts);
    EXPECT_ERR(marmot_update_group_blossom_image(t.alice.m, &t.gid, &image, &event),
               MARMOT_ERR_INVALID_ARG);

    MarmotGroupAvatarUrl avatar = { .url = "https://example.com/avatar.png" };
    EXPECT_ERR(marmot_update_group_avatar_url(t.bob.m, &t.gid, &avatar, &event),
               MARMOT_ERR_ADMIN_ONLY);
    OK(marmot_update_group_avatar_url(t.alice.m, &t.gid, &avatar, &event));
    publish(&t.alice, &t.gid, event, receivers, 2);
    free(event);
    event = NULL;
    OK(marmot_get_group_components(t.carol.m, &t.gid, &parts));
    CHECK(parts.avatar_source == MARMOT_GROUP_AVATAR_URL &&
          strcmp(parts.avatar_url.url, avatar.url) == 0,
          "URL avatar takes precedence after Commit");
    marmot_group_components_clear(&parts);

    OK(marmot_update_group_avatar_url(t.alice.m, &t.gid, NULL, &event));
    publish(&t.alice, &t.gid, event, receivers, 2);
    free(event);
    event = NULL;
    OK(marmot_get_group_components(t.bob.m, &t.gid, &parts));
    CHECK(parts.avatar_source == MARMOT_GROUP_AVATAR_BLOSSOM && parts.avatar_url.url == NULL,
          "clearing URL restores Blossom image");
    marmot_group_components_clear(&parts);

    OK(marmot_update_group_blossom_image(t.alice.m, &t.gid, NULL, &event));
    publish(&t.alice, &t.gid, event, receivers, 2);
    free(event);
    OK(marmot_get_group_components(t.bob.m, &t.gid, &parts));
    CHECK(!parts.image.present && parts.avatar_source == MARMOT_GROUP_AVATAR_NONE,
          "clearing image is replicated");
    marmot_group_components_clear(&parts);
    /* 0x800b: the group's media servers, admin-only, followed by receivers. */
    char *kinds[] = { "blossom-v1", NULL };
    MarmotMediaBlobEndpoint endpoints[] = { { "blossom-v1", "https://media.example.com", false } };
    MarmotGroupMediaPolicy policy = { kinds, 1, endpoints, 1 };
    event = NULL;
    EXPECT_ERR(marmot_update_group_media_policy(t.bob.m, &t.gid, &policy, &event),
               MARMOT_ERR_ADMIN_ONLY);
    CHECK(event == NULL, "non-admin produced no media policy Commit");
    EXPECT_ERR(marmot_update_group_media_policy(t.alice.m, &t.gid, NULL, &event),
               MARMOT_ERR_INVALID_ARG);
    OK(marmot_update_group_media_policy(t.alice.m, &t.gid, &policy, &event));
    publish(&t.alice, &t.gid, event, receivers, 2);
    free(event);
    event = NULL;
    OK(marmot_get_group_components(t.carol.m, &t.gid, &parts));
    CHECK(parts.has_media_policy && parts.media_policy.default_blob_endpoint_count == 1 &&
          strcmp(parts.media_policy.default_blob_endpoints[0].base_url,
                 "https://media.example.com/") == 0 &&
          !parts.media_policy.default_blob_endpoints[0].base_url_unverified,
          "media policy applied by receiver");
    marmot_group_components_clear(&parts);
    EXPECT_ERR(marmot_update_group_media_policy(t.alice.m, &t.gid, &policy, &event),
               MARMOT_ERR_INVALID_ARG);   /* no change, no Commit */
    marmot_group_blossom_image_clear(&image);
    sodium_memzero(ciphertext, ciphertext_len);
    free(ciphertext);
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
    RUN(test_lifecycle_enablement);
    RUN(test_removal_judged_whole);
    RUN(test_openmls_positives);
    RUN(test_openmls_by_reference);
    RUN(test_authorize_by_reference_rules);
    RUN(test_partial_write_rollback);
    RUN(test_own_commits);
    RUN(test_group_image_component_updates);
    RUN(test_self_update_with_new_proof);
    RUN(test_rotation_onto_another_group_refused);
    RUN(test_rotation_back_to_old_address);
    RUN(test_large_commit_refused_quickly);
    RUN(test_tampered_admin_commit_not_reported);
    RUN(test_enablement_by_reference_refused);
    RUN(test_removal_parent_hash_checked);
    RUN(test_nonadmin_removal_refused);
    RUN(test_refused_commit_draws_no_time);
    printf("all passed\n");
    return 0;
}
