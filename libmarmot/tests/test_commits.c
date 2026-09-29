/*
 * libmarmot - Commit publication and ingestion through the Marmot API
 * (nostrc-9ata).
 *
 * Three Marmot instances (Alice = admin, Bob, Charlie), each with its own
 * storage, exchange the kind:445 events the API returns, exactly as relays
 * would deliver them: every member must follow every Commit and end with the
 * same metadata, epoch, epoch secrets, tree hash and transcript hashes.
 *
 * SPDX-License-Identifier: MIT
 */

#include <marmot/marmot.h>
#include "marmot-internal.h"
#include "commits.h"
#include "mls/mls_group.h"
#include "mls/mls-internal.h"
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

#define RUN(fn) do { printf("  %-58s", #fn); fflush(stdout); fn(); printf("PASS\n"); } while (0)

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
}

static char *
key_package(Member *x)
{
    MarmotKeyPackageResult r;
    memset(&r, 0, sizeof(r));
    OK(marmot_create_key_package(x->m, x->pk, x->sk, NULL, 0, &r));
    char *json = strdup(r.event_json);
    marmot_key_package_result_free(&r);
    return json;
}

static void
join(Member *x, const char *welcome_rumor)
{
    uint8_t wrapper[32];
    randombytes_buf(wrapper, sizeof(wrapper));
    MarmotWelcome *w = NULL;
    OK(marmot_process_welcome(x->m, wrapper, welcome_rumor, &w));
    OK(marmot_accept_welcome(x->m, w));
    marmot_welcome_free(w);
}

/* Deliver a kind:445 event; returns the result type, *err the error. */
static MarmotMessageResultType
deliver(Member *x, const char *event_json, MarmotError *err, MarmotGroup **updated)
{
    MarmotMessageResult r;
    memset(&r, 0, sizeof(r));
    *err = marmot_process_message(x->m, event_json, &r);
    MarmotMessageResultType type = r.type;
    if (updated && *err == MARMOT_OK && type == MARMOT_RESULT_COMMIT) {
        *updated = r.commit.updated_group;
        r.commit.updated_group = NULL;
    }
    marmot_message_result_free(&r);
    return type;
}

static void
expect_commit(Member *x, const char *event_json, const char *what)
{
    MarmotError err;
    MarmotGroup *g = NULL;
    MarmotMessageResultType t = deliver(x, event_json, &err, &g);
    CHECK(err == MARMOT_OK && t == MARMOT_RESULT_COMMIT,
          "%s: %s got err=%d (%s) type=%d", what, x->name, err,
          marmot_error_string(err), t);
    CHECK(g != NULL, "%s: %s: COMMIT result without updated_group", what, x->name);
    marmot_group_free(g);
}

/* ── Stored state ─────────────────────────────────────────────────────── */

static void
load_mls(Member *x, const MarmotGroupId *gid, MlsGroup *out)
{
    uint8_t *blob = NULL;
    size_t len = 0;
    OK(x->m->storage->mls_load(x->m->storage->ctx, "mls_group", gid->data, gid->len,
                               &blob, &len));
    CHECK(mls_group_deserialize(blob, len, out) == 0, "deserialize %s", x->name);
    sodium_memzero(blob, len);
    free(blob);
}

/* Everything a Commit writes, for "nothing changed" checks. */
typedef struct {
    uint8_t *state, *parent;
    size_t   state_len, parent_len;
    uint64_t epoch;
    char    *name;
    uint8_t  exporter_next[32];
    bool     has_exporter_next;
} Snapshot;

static void
snapshot(Member *x, const MarmotGroupId *gid, Snapshot *s)
{
    memset(s, 0, sizeof(*s));
    MarmotStorage *st = x->m->storage;
    OK(st->mls_load(st->ctx, "mls_group", gid->data, gid->len, &s->state, &s->state_len));
    if (st->mls_load(st->ctx, "mls_group_parent", gid->data, gid->len,
                     &s->parent, &s->parent_len) != MARMOT_OK)
        s->parent = NULL;
    MarmotGroup *g = NULL;
    OK(marmot_get_group(x->m, gid, &g));
    s->epoch = g->epoch;
    s->name = g->name ? strdup(g->name) : NULL;
    s->has_exporter_next = st->get_exporter_secret(st->ctx, gid, g->epoch + 1,
                                                   s->exporter_next) == MARMOT_OK;
    marmot_group_free(g);
}

static void
snapshot_clear(Snapshot *s)
{
    if (s->state) sodium_memzero(s->state, s->state_len);
    if (s->parent) sodium_memzero(s->parent, s->parent_len);
    free(s->state);
    free(s->parent);
    free(s->name);
    sodium_memzero(s, sizeof(*s));
}

static void
expect_unchanged(Member *x, const MarmotGroupId *gid, const Snapshot *before,
                 const char *what)
{
    Snapshot now;
    snapshot(x, gid, &now);
    CHECK(now.epoch == before->epoch, "%s: %s epoch moved", what, x->name);
    CHECK((now.name == NULL) == (before->name == NULL) &&
          (!now.name || strcmp(now.name, before->name) == 0),
          "%s: %s name changed", what, x->name);
    CHECK(now.state_len == before->state_len &&
          memcmp(now.state, before->state, now.state_len) == 0,
          "%s: %s MLS state changed", what, x->name);
    CHECK((now.parent == NULL) == (before->parent == NULL) &&
          (!now.parent || (now.parent_len == before->parent_len &&
                           memcmp(now.parent, before->parent, now.parent_len) == 0)),
          "%s: %s retained parent changed", what, x->name);
    CHECK(now.has_exporter_next == before->has_exporter_next,
          "%s: %s stored a next-epoch exporter secret", what, x->name);
    snapshot_clear(&now);
}

static void
expect_rejected(Member *x, const MarmotGroupId *gid, const char *event_json,
                MarmotError want, const char *what)
{
    Snapshot before;
    snapshot(x, gid, &before);
    MarmotError err;
    deliver(x, event_json, &err, NULL);
    CHECK(err == want, "%s: %s got %d (%s), want %d (%s)", what, x->name, err,
          marmot_error_string(err), want, marmot_error_string(want));
    expect_unchanged(x, gid, &before, what);
    snapshot_clear(&before);
}

/* Same metadata, epoch, epoch secrets, tree hash, GroupContext (confirmed
 * transcript hash, extensions) and interim transcript hash everywhere. */
static void
expect_converged(Member *const *ms, size_t n, const MarmotGroupId *gid,
                 const char *name, uint64_t epoch)
{
    MlsGroup g0;
    load_mls(ms[0], gid, &g0);
    uint8_t th0[MLS_HASH_LEN];
    uint8_t *gc0 = NULL;
    size_t gc0_len = 0;
    CHECK(mls_group_tree_hash(&g0, th0) == 0 &&
          mls_group_context_build(&g0, &gc0, &gc0_len) == 0, "context");
    for (size_t i = 0; i < n; i++) {
        MarmotGroup *rec = NULL;
        OK(marmot_get_group(ms[i]->m, gid, &rec));
        CHECK(rec->epoch == epoch, "%s record epoch %llu, want %llu", ms[i]->name,
              (unsigned long long)rec->epoch, (unsigned long long)epoch);
        CHECK(rec->name && strcmp(rec->name, name) == 0, "%s name '%s', want '%s'",
              ms[i]->name, rec->name ? rec->name : "(null)", name);
        CHECK(rec->state == MARMOT_GROUP_STATE_ACTIVE, "%s not active", ms[i]->name);
        marmot_group_free(rec);

        MlsGroup g;
        load_mls(ms[i], gid, &g);
        uint8_t th[MLS_HASH_LEN];
        uint8_t *gc = NULL;
        size_t gc_len = 0;
        CHECK(g.epoch == epoch, "%s MLS epoch", ms[i]->name);
        CHECK(mls_group_tree_hash(&g, th) == 0 && memcmp(th, th0, MLS_HASH_LEN) == 0,
              "%s tree hash differs", ms[i]->name);
        CHECK(mls_group_context_build(&g, &gc, &gc_len) == 0 && gc_len == gc0_len &&
              memcmp(gc, gc0, gc_len) == 0, "%s GroupContext differs", ms[i]->name);
        CHECK(memcmp(g.interim_transcript_hash, g0.interim_transcript_hash,
                     MLS_HASH_LEN) == 0, "%s interim transcript differs", ms[i]->name);
        CHECK(sodium_memcmp(&g.epoch_secrets, &g0.epoch_secrets,
                            sizeof(g.epoch_secrets)) == 0,
              "%s epoch secrets differ", ms[i]->name);
        free(gc);
        mls_group_free(&g);
    }
    free(gc0);
    mls_group_free(&g0);
}

/* Every member sends one message; every other member reads it. */
static void
expect_messages_flow(Member *const *ms, size_t n, const MarmotGroupId *gid)
{
    for (size_t s = 0; s < n; s++) {
        char inner[160];
        snprintf(inner, sizeof(inner),
                 "{\"kind\":9,\"content\":\"hello from %s\",\"created_at\":1700000000,\"tags\":[]}",
                 ms[s]->name);
        MarmotOutgoingMessage out;
        memset(&out, 0, sizeof(out));
        OK(marmot_create_message(ms[s]->m, gid, inner, &out));
        for (size_t r = 0; r < n; r++) {
            if (r == s) continue;
            MarmotMessageResult in;
            memset(&in, 0, sizeof(in));
            MarmotError err = marmot_process_message(ms[r]->m, out.event_json, &in);
            CHECK(err == MARMOT_OK && in.type == MARMOT_RESULT_APPLICATION_MESSAGE &&
                  in.app_msg.inner_event_json &&
                  strstr(in.app_msg.inner_event_json, ms[s]->name),
                  "%s cannot read %s's message: %d", ms[r]->name, ms[s]->name, err);
            marmot_message_result_free(&in);
        }
        marmot_outgoing_message_free(&out);
    }
}

/* ── Fixture: Alice (admin) + Bob + Charlie ────────────────────────────── */

typedef struct {
    Member        alice, bob, charlie;
    Member       *all[3];
    MarmotGroupId gid;
    uint8_t       nostr_gid[32];
    uint64_t      epoch;
} Trio;

static void
trio_init(Trio *t)
{
    member_init(&t->alice, "Alice");
    member_init(&t->bob, "Bob");
    member_init(&t->charlie, "Charlie");
    t->all[0] = &t->alice;
    t->all[1] = &t->bob;
    t->all[2] = &t->charlie;

    char *bob_kp = key_package(&t->bob);
    const char *kps[] = { bob_kp };
    MarmotGroupConfig cfg = {0};
    cfg.name = "Before";
    cfg.description = "Desc";
    cfg.admin_pubkeys = (uint8_t (*)[32])t->alice.pk;
    cfg.admin_count = 1;
    MarmotCreateGroupResult cg;
    memset(&cg, 0, sizeof(cg));
    OK(marmot_create_group(t->alice.m, t->alice.pk, kps, 1, &cfg, &cg));
    free(bob_kp);
    join(&t->bob, cg.welcome_rumor_jsons[0]);
    t->gid = marmot_group_id_new(cg.group->mls_group_id.data, cg.group->mls_group_id.len);
    memcpy(t->nostr_gid, cg.group->nostr_group_id, 32);
    marmot_create_group_result_free(&cg);

    /* Charlie joins through an Add Commit that Bob applies from the relay. */
    char *charlie_kp = key_package(&t->charlie);
    const char *kps2[] = { charlie_kp };
    char **welcomes = NULL;
    size_t welcome_count = 0;
    char *commit = NULL;
    OK(marmot_add_members(t->alice.m, &t->gid, kps2, 1, &welcomes, &welcome_count,
                          &commit));
    free(charlie_kp);
    expect_commit(&t->bob, commit, "Alice adds Charlie");
    join(&t->charlie, welcomes[0]);
    free(welcomes[0]);
    free(welcomes);
    free(commit);

    MarmotGroup *g = NULL;
    OK(marmot_get_group(t->alice.m, &t->gid, &g));
    t->epoch = g->epoch;
    marmot_group_free(g);
    expect_converged(t->all, 3, &t->gid, "Before", t->epoch);
}

static void
trio_clear(Trio *t)
{
    for (size_t i = 0; i < 3; i++) marmot_free(t->all[i]->m);
    marmot_group_id_free(&t->gid);
}

static char *
rename_group(Member *x, const MarmotGroupId *gid, const char *name)
{
    MarmotGroupConfig cfg = {0};
    cfg.name = (char *)name;
    char *commit = NULL;
    OK(marmot_update_group_metadata(x->m, gid, &cfg, &commit));
    CHECK(commit != NULL, "no Commit returned");
    return commit;
}

/* An ordinary Commit (empty Commit with an UpdatePath) made and applied by
 * `x` through the same authorize/persist path as the API producers; there
 * is no public self-update API yet. */
static char *
self_update(Member *x, const MarmotGroupId *gid)
{
    MlsGroup pre, post;
    load_mls(x, gid, &pre);
    load_mls(x, gid, &post);
    MlsCommitResult r;
    memset(&r, 0, sizeof(r));
    CHECK(mls_group_self_update(&post, &r) == 0, "self_update");
    MarmotCommitKey key;
    MarmotGroupDataExtension *gde = NULL;
    OK(marmot_commit_authorize(&pre, &post, pre.own_leaf_index, &key, &gde));
    CHECK(!key.privileged, "self-update must be ordinary");
    CHECK(mls_crypto_hash(key.digest, r.commit_data, r.commit_len) == 0, "hash");
    MarmotGroup *g = NULL;
    OK(marmot_get_group(x->m, gid, &g));
    char *json = marmot_commit_build_event(r.commit_data, r.commit_len,
                                           pre.epoch_secrets.exporter_secret,
                                           g->nostr_group_id);
    CHECK(json, "build event");
    OK(marmot_commit_persist(x->m, &pre, &post, &key, gde, g));
    marmot_group_free(g);
    marmot_group_data_extension_free(gde);
    mls_commit_result_clear(&r);
    mls_group_free(&pre);
    mls_group_free(&post);
    return json;
}

/* A GroupContextExtensions Commit carrying `gde` that `x` makes but never
 * applies (a forgery from the receivers' point of view). */
static char *
forge_group_data_commit(Member *x, const MarmotGroupId *gid,
                        const MarmotGroupDataExtension *gde)
{
    uint8_t *bytes = NULL;
    size_t len = 0;
    CHECK(marmot_group_data_extension_serialize(gde, &bytes, &len) == 0, "serialize");
    MlsTlsBuf ext;
    CHECK(mls_tls_buf_init(&ext, len + 8) == 0 &&
          mls_tls_write_u16(&ext, MARMOT_EXTENSION_TYPE) == 0 &&
          mls_tls_write_opaque16(&ext, bytes, len) == 0, "extension list");
    free(bytes);
    MlsGroup g;
    load_mls(x, gid, &g);
    uint8_t exporter[32];
    memcpy(exporter, g.epoch_secrets.exporter_secret, 32);
    MlsCommitResult r;
    memset(&r, 0, sizeof(r));
    CHECK(mls_group_commit_extensions(&g, ext.data, ext.len, &r) == 0, "commit_extensions");
    MarmotGroup *rec = NULL;
    OK(marmot_get_group(x->m, gid, &rec));
    char *json = marmot_commit_build_event(r.commit_data, r.commit_len, exporter,
                                           rec->nostr_group_id);
    CHECK(json, "build event");
    marmot_group_free(rec);
    mls_commit_result_clear(&r);
    mls_tls_buf_free(&ext);
    mls_group_free(&g);
    sodium_memzero(exporter, sizeof(exporter));
    return json;
}

/* The group's current GroupData with `name` (and optionally a new
 * nostr_group_id). */
static MarmotGroupDataExtension *
group_data_with(Member *x, const MarmotGroupId *gid, const char *name,
                const uint8_t *new_nostr_gid)
{
    MlsGroup g;
    load_mls(x, gid, &g);
    const uint8_t *data = NULL;
    size_t len = 0;
    MlsTlsReader r;
    mls_tls_reader_init(&r, g.extensions_data, g.extensions_len);
    uint16_t type = 0;
    uint8_t *ext = NULL;
    size_t ext_len = 0;
    CHECK(mls_tls_read_u16(&r, &type) == 0 && type == MARMOT_EXTENSION_TYPE &&
          mls_tls_read_opaque16(&r, &ext, &ext_len) == 0, "GroupData extension");
    data = ext;
    len = ext_len;
    MarmotGroupDataExtension *gde = marmot_group_data_extension_deserialize(data, len);
    CHECK(gde, "GroupData parses");
    free(ext);
    free(gde->name);
    gde->name = strdup(name);
    if (new_nostr_gid) memcpy(gde->nostr_group_id, new_nostr_gid, 32);
    mls_group_free(&g);
    return gde;
}

/* ── Tests ────────────────────────────────────────────────────────────── */

/* Alice renames the group; Bob and Charlie process the published kind:445
 * and everyone agrees on the metadata and the epoch (was: the Commit was
 * discarded and receivers treated every kind:445 as an application
 * message, so only Alice moved on). */
static void
test_rename_reaches_every_member(void)
{
    Trio t;
    trio_init(&t);

    char *commit = rename_group(&t.alice, &t.gid, "Renamed");
    MarmotError err;
    MarmotGroup *g = NULL;
    for (size_t i = 1; i < 3; i++) {
        MarmotMessageResultType type = deliver(t.all[i], commit, &err, &g);
        CHECK(err == MARMOT_OK && type == MARMOT_RESULT_COMMIT,
              "%s: rename not applied: %d (%s)", t.all[i]->name, err,
              marmot_error_string(err));
        CHECK(g && g->epoch == t.epoch + 1 && strcmp(g->name, "Renamed") == 0 &&
              g->description && strcmp(g->description, "Desc") == 0 &&
              g->admin_count == 1 && memcmp(g->admin_pubkeys[0], t.alice.pk, 32) == 0,
              "%s: updated_group does not carry the new GroupData", t.all[i]->name);
        marmot_group_free(g);
        g = NULL;
    }
    /* The relay echoes Alice's own Commit back to her: nothing to do. */
    Snapshot before;
    snapshot(&t.alice, &t.gid, &before);
    CHECK(deliver(&t.alice, commit, &err, NULL) == MARMOT_RESULT_OWN_MESSAGE &&
          err == MARMOT_OK, "own echo: %d", err);
    expect_unchanged(&t.alice, &t.gid, &before, "own echo");
    snapshot_clear(&before);

    expect_converged(t.all, 3, &t.gid, "Renamed", t.epoch + 1);
    expect_messages_flow(t.all, 3, &t.gid);

    /* Bob is not an admin: he cannot rename. */
    MarmotGroupConfig cfg = {0};
    cfg.name = "Bob's";
    char *none = NULL;
    CHECK(marmot_update_group_metadata(t.bob.m, &t.gid, &cfg, &none) ==
          MARMOT_ERR_ADMIN_ONLY && none == NULL, "non-admin rename");
    CHECK(marmot_update_group_metadata(t.alice.m, &t.gid, &cfg, NULL) ==
          MARMOT_ERR_INVALID_ARG, "out_commit_json is required");
    expect_converged(t.all, 3, &t.gid, "Renamed", t.epoch + 1);

    free(commit);
    trio_clear(&t);
}

/* Duplicates, stale Commits and Commits from an epoch we have not reached
 * leave the group untouched; delivered in order afterwards, they apply. */
static void
test_stale_duplicate_and_future_commits(void)
{
    Trio t;
    trio_init(&t);

    char *r1 = rename_group(&t.alice, &t.gid, "One");
    expect_commit(&t.bob, r1, "R1");
    Snapshot before;
    snapshot(&t.bob, &t.gid, &before);
    MarmotError err;
    CHECK(deliver(&t.bob, r1, &err, NULL) == MARMOT_RESULT_OWN_MESSAGE &&
          err == MARMOT_OK, "duplicate R1: %d", err);
    expect_unchanged(&t.bob, &t.gid, &before, "duplicate R1");
    snapshot_clear(&before);

    char *r2 = rename_group(&t.alice, &t.gid, "Two");
    expect_commit(&t.bob, r2, "R2");
    /* R1 is now older than the Commit Bob's state was built on. */
    expect_rejected(&t.bob, &t.gid, r1, MARMOT_ERR_WRONG_EPOCH, "stale R1");

    /* Charlie missed R1: R2 comes from an epoch he has no key for yet. */
    expect_rejected(&t.charlie, &t.gid, r2, MARMOT_ERR_NIP44, "future R2");
    /* Retried in order, both apply. */
    expect_commit(&t.charlie, r1, "R1 late");
    expect_commit(&t.charlie, r2, "R2 retried");
    expect_converged(t.all, 3, &t.gid, "Two", t.epoch + 2);
    expect_messages_flow(t.all, 3, &t.gid);

    /* Old Commits stay stale for everyone, the committer included. */
    for (size_t i = 0; i < 3; i++)
        expect_rejected(t.all[i], &t.gid, r1, MARMOT_ERR_WRONG_EPOCH, "stale R1 again");
    free(r1);
    free(r2);
    trio_clear(&t);
}

/* MIP-01 authorization and MLS validation of received Commits, each
 * failing closed; an ordinary Commit from a non-admin is accepted. */
static void
test_unauthorized_and_invalid_commits_rejected(void)
{
    Trio t;
    trio_init(&t);

    /* Bob (not an admin) renames the group behind the API's back. */
    MarmotGroupDataExtension *gde = group_data_with(&t.bob, &t.gid, "Bob's", NULL);
    char *forged = forge_group_data_commit(&t.bob, &t.gid, gde);
    marmot_group_data_extension_free(gde);
    expect_rejected(&t.alice, &t.gid, forged, MARMOT_ERR_COMMIT_FROM_NON_ADMIN,
                    "non-admin GroupData change");
    expect_rejected(&t.charlie, &t.gid, forged, MARMOT_ERR_COMMIT_FROM_NON_ADMIN,
                    "non-admin GroupData change");
    free(forged);

    /* Removing a member is privileged too. */
    {
        MlsGroup g;
        load_mls(&t.bob, &t.gid, &g);
        uint8_t exporter[32];
        memcpy(exporter, g.epoch_secrets.exporter_secret, 32);
        MlsCommitResult r;
        memset(&r, 0, sizeof(r));
        CHECK(mls_group_remove_member(&g, 2, &r) == 0, "remove");
        char *removal = marmot_commit_build_event(r.commit_data, r.commit_len,
                                                  exporter, t.nostr_gid);
        expect_rejected(&t.alice, &t.gid, removal, MARMOT_ERR_COMMIT_FROM_NON_ADMIN,
                        "non-admin Remove");
        free(removal);
        mls_commit_result_clear(&r);
        mls_group_free(&g);
        sodium_memzero(exporter, sizeof(exporter));
    }

    /* An admin cannot move the group to another nostr_group_id. */
    uint8_t other[32];
    randombytes_buf(other, sizeof(other));
    gde = group_data_with(&t.alice, &t.gid, "Moved", other);
    forged = forge_group_data_commit(&t.alice, &t.gid, gde);
    marmot_group_data_extension_free(gde);
    expect_rejected(&t.bob, &t.gid, forged, MARMOT_ERR_PROTOCOL_GROUP_MISMATCH,
                    "nostr_group_id change");
    expect_rejected(&t.charlie, &t.gid, forged, MARMOT_ERR_PROTOCOL_GROUP_MISMATCH,
                    "nostr_group_id change");
    free(forged);

    /* A Commit whose bytes were altered in transit fails MLS validation. */
    {
        MlsGroup g;
        load_mls(&t.charlie, &t.gid, &g);
        uint8_t exporter[32];
        memcpy(exporter, g.epoch_secrets.exporter_secret, 32);
        MlsCommitResult r;
        memset(&r, 0, sizeof(r));
        CHECK(mls_group_self_update(&g, &r) == 0, "self_update");
        r.commit_data[r.commit_len - 5] ^= 0x01;   /* inside the membership tag */
        char *tampered = marmot_commit_build_event(r.commit_data, r.commit_len,
                                                   exporter, t.nostr_gid);
        expect_rejected(&t.alice, &t.gid, tampered, MARMOT_ERR_MLS_PROCESS_MESSAGE,
                        "tampered Commit");
        expect_rejected(&t.bob, &t.gid, tampered, MARMOT_ERR_MLS_PROCESS_MESSAGE,
                        "tampered Commit");
        free(tampered);
        mls_commit_result_clear(&r);
        mls_group_free(&g);
        sodium_memzero(exporter, sizeof(exporter));
    }

    /* Nothing above moved anyone. */
    expect_converged(t.all, 3, &t.gid, "Before", t.epoch);

    /* Any member may self-update. */
    char *upd = self_update(&t.bob, &t.gid);
    expect_commit(&t.alice, upd, "Bob self-update");
    expect_commit(&t.charlie, upd, "Bob self-update");
    expect_converged(t.all, 3, &t.gid, "Before", t.epoch + 1);
    expect_messages_flow(t.all, 3, &t.gid);
    free(upd);
    trio_clear(&t);
}

/* Two members commit from the same epoch.  Every member ends on the same
 * branch whatever order the Commits arrive in (Marmot convergence,
 * "Same-epoch races"): privileged before ordinary, then the lower committer
 * key -- including a committer whose own Commit loses. */
static void
test_same_epoch_race_converges(void)
{
    Trio t;
    trio_init(&t);

    /* Admin rename (privileged) vs Bob's self-update (ordinary). */
    char *c_a = rename_group(&t.alice, &t.gid, "Admin wins");
    char *c_b = self_update(&t.bob, &t.gid);
    expect_commit(&t.charlie, c_b, "Charlie: B first");
    expect_commit(&t.charlie, c_a, "Charlie: A replaces B");
    expect_commit(&t.bob, c_a, "Bob: A replaces his own B");
    expect_rejected(&t.alice, &t.gid, c_b, MARMOT_ERR_WRONG_EPOCH, "Alice: B loses");
    /* Re-deliveries change nothing. */
    expect_rejected(&t.charlie, &t.gid, c_b, MARMOT_ERR_WRONG_EPOCH, "B again");
    MarmotError err;
    CHECK(deliver(&t.bob, c_a, &err, NULL) == MARMOT_RESULT_OWN_MESSAGE &&
          err == MARMOT_OK, "A again: %d", err);
    expect_converged(t.all, 3, &t.gid, "Admin wins", t.epoch + 1);
    expect_messages_flow(t.all, 3, &t.gid);
    free(c_a);
    free(c_b);

    /* Two ordinary Commits: the lower committer key wins, in either arrival
     * order at the bystander. */
    for (int order = 0; order < 2; order++) {
        uint64_t e = t.epoch + 1 + (uint64_t)order;
        char *cb = self_update(&t.bob, &t.gid);
        char *cc = self_update(&t.charlie, &t.gid);
        expect_commit(&t.alice, order == 0 ? cb : cc, "Alice: first");
        MarmotError e2;
        MarmotMessageResultType ty = deliver(&t.alice, order == 0 ? cc : cb, &e2, NULL);
        bool bob_wins = memcmp(t.bob.pk, t.charlie.pk, 32) < 0;
        bool second_is_winner = (order == 0) ? !bob_wins : bob_wins;
        if (second_is_winner)
            CHECK(e2 == MARMOT_OK && ty == MARMOT_RESULT_COMMIT,
                  "Alice: the winning second Commit must replace the first: %d", e2);
        else
            CHECK(e2 == MARMOT_ERR_WRONG_EPOCH,
                  "Alice: the losing second Commit must be rejected: %d", e2);
        /* Each committer sees the other's Commit. */
        MarmotError eb, ec;
        MarmotMessageResultType tb = deliver(&t.bob, cc, &eb, NULL);
        MarmotMessageResultType tc = deliver(&t.charlie, cb, &ec, NULL);
        if (bob_wins) {
            CHECK(eb == MARMOT_ERR_WRONG_EPOCH, "Bob keeps his winning Commit: %d", eb);
            CHECK(ec == MARMOT_OK && tc == MARMOT_RESULT_COMMIT,
                  "Charlie switches to Bob's Commit: %d", ec);
        } else {
            CHECK(ec == MARMOT_ERR_WRONG_EPOCH, "Charlie keeps his winning Commit: %d", ec);
            CHECK(eb == MARMOT_OK && tb == MARMOT_RESULT_COMMIT,
                  "Bob switches to Charlie's Commit: %d", eb);
        }
        expect_converged(t.all, 3, &t.gid, "Admin wins", e + 1);
        expect_messages_flow(t.all, 3, &t.gid);
        free(cb);
        free(cc);
    }
    trio_clear(&t);
}

int
main(void)
{
    if (sodium_init() < 0) return 1;
    printf("libmarmot: Commit publication and ingestion (nostrc-9ata)\n");
    RUN(test_rename_reaches_every_member);
    RUN(test_stale_duplicate_and_future_commits);
    RUN(test_unauthorized_and_invalid_commits_rejected);
    RUN(test_same_epoch_race_converges);
    printf("All commit tests passed\n");
    return 0;
}
