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

/* Regenerate `x`'s account key until it sorts above (`above` true) or
 * below `other`'s: keeps key-order-dependent tests deterministic. */
static void
member_rekey_order(Member *x, const Member *other, bool above)
{
    for (;;) {
        int c = memcmp(x->pk, other->pk, 32);
        if ((above && c > 0) || (!above && c < 0)) return;
        marmot_free(x->m);
        member_init(x, x->name);
    }
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

/* The relay accepted our Commit (MIP-03: merge only then). */
static void
merge(Member *x, const MarmotGroupId *gid)
{
    OK(marmot_merge_pending_commit(x->m, gid));
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

/* The same kind:445 content re-published in a new envelope (another
 * ephemeral key, so another event id): what a relay replay or a second
 * publisher looks like.  Event-id idempotency does not apply to it. */
static char *
republish(const char *event_json)
{
    NostrEvent *ev = nostr_event_new();
    CHECK(ev && nostr_event_deserialize_compact(ev, event_json, NULL), "parse");
    CHECK(marmot_sign_ephemeral(ev) == 0, "re-sign");
    char *json = nostr_event_serialize_compact(ev);
    nostr_event_free(ev);
    CHECK(json, "serialize");
    return json;
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

/* ── Fixture: Alice + Bob (admins) + Charlie ──────────────────────────── */

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
    /* Alice's account key sorts above Bob's: wherever they compete with
     * equal priority Bob wins, so a test in which Alice's privileged Commit
     * beats Bob's ordinary one depends on the privileged step alone. */
    member_rekey_order(&t->alice, &t->bob, true);
    t->all[0] = &t->alice;
    t->all[1] = &t->bob;
    t->all[2] = &t->charlie;

    char *bob_kp = key_package(&t->bob);
    const char *kps[] = { bob_kp };
    uint8_t admins[2][32];
    memcpy(admins[0], t->alice.pk, 32);
    memcpy(admins[1], t->bob.pk, 32);
    MarmotGroupConfig cfg = {0};
    cfg.name = "Before";
    cfg.description = "Desc";
    cfg.admin_pubkeys = admins;
    cfg.admin_count = 2;
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
    merge(&t->alice, &t->gid);
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

/* A rename, left pending (not merged). */
static char *
rename_pending(Member *x, const MarmotGroupId *gid, const char *name)
{
    MarmotGroupConfig cfg = {0};
    cfg.name = (char *)name;
    char *commit = NULL;
    OK(marmot_update_group_metadata(x->m, gid, &cfg, &commit));
    CHECK(commit != NULL, "no Commit returned");
    return commit;
}

/* A rename a relay accepted: pending, then merged. */
static char *
rename_group(Member *x, const MarmotGroupId *gid, const char *name)
{
    char *commit = rename_pending(x, gid, name);
    merge(x, gid);
    return commit;
}

/* An ordinary Commit (empty Commit with an UpdatePath) made and applied by
 * `x` through the same authorize/persist path as a merged producer; there is
 * no public self-update API yet (nostrc-yd0q). */
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

/* A Commit event `x` makes from `state` but never applies. */
static char *
event_for_commit(const MlsCommitResult *r, const uint8_t exporter[32],
                 const uint8_t nostr_gid[32])
{
    char *json = marmot_commit_build_event(r->commit_data, r->commit_len, exporter,
                                           nostr_gid);
    CHECK(json, "build event");
    return json;
}

/* A GroupContextExtensions Commit carrying `ext` that `x` makes but never
 * applies (a forgery from the receivers' point of view). */
static char *
forge_extensions_commit(Member *x, const MarmotGroupId *gid,
                        const uint8_t *ext, size_t ext_len,
                        const uint8_t nostr_gid[32])
{
    MlsGroup g;
    load_mls(x, gid, &g);
    uint8_t exporter[32];
    memcpy(exporter, g.epoch_secrets.exporter_secret, 32);
    MlsCommitResult r;
    memset(&r, 0, sizeof(r));
    CHECK(mls_group_commit_extensions(&g, ext, ext_len, &r) == 0, "commit_extensions");
    char *json = event_for_commit(&r, exporter, nostr_gid);
    mls_commit_result_clear(&r);
    mls_group_free(&g);
    sodium_memzero(exporter, sizeof(exporter));
    return json;
}

static char *
forge_group_data_commit(Member *x, const MarmotGroupId *gid,
                        const MarmotGroupDataExtension *gde, const uint8_t nostr_gid[32])
{
    uint8_t *bytes = NULL;
    size_t len = 0;
    CHECK(marmot_group_data_extension_serialize(gde, &bytes, &len) == 0, "serialize");
    MlsTlsBuf ext;
    CHECK(mls_tls_buf_init(&ext, len + 8) == 0 &&
          mls_tls_write_u16(&ext, MARMOT_EXTENSION_TYPE) == 0 &&
          mls_tls_write_opaque16(&ext, bytes, len) == 0, "extension list");
    free(bytes);
    char *json = forge_extensions_commit(x, gid, ext.data, ext.len, nostr_gid);
    mls_tls_buf_free(&ext);
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
    MlsTlsReader r;
    mls_tls_reader_init(&r, g.extensions_data, g.extensions_len);
    uint16_t type = 0;
    uint8_t *ext = NULL;
    size_t ext_len = 0;
    CHECK(mls_tls_read_u16(&r, &type) == 0 && type == MARMOT_EXTENSION_TYPE &&
          mls_tls_read_opaque16(&r, &ext, &ext_len) == 0, "GroupData extension");
    MarmotGroupDataExtension *gde = marmot_group_data_extension_deserialize(ext, ext_len);
    CHECK(gde, "GroupData parses");
    free(ext);
    free(gde->name);
    gde->name = strdup(name);
    if (new_nostr_gid) memcpy(gde->nostr_group_id, new_nostr_gid, 32);
    mls_group_free(&g);
    return gde;
}

/* ── Fault-injecting storage (review N3/N5) ───────────────────────────── */

typedef struct {
    MarmotStorage orig;           /* the wrapped backend's operations */
    int   writes;                 /* writes seen while armed */
    int   fail_at;                /* fail the n-th write (1-based); 0 = never */
    bool  fail_later;             /* ...and every write after it (undo fails) */
    bool  drop_group_record;      /* save_group "succeeds" without writing (crash) */
    const char *fail_load_label;  /* mls_load of this label fails */
    const char *fail_delete_label;/* mls_delete of this label fails (a crash) */
    bool  fail_exporter_load;     /* get_exporter_secret(exporter_epoch) fails */
    uint64_t exporter_epoch;
} Faults;

static Faults g_faults;

/* MARMOT_OK, or the injected failure: the n-th write fails with
 * STORAGE_CONSTRAINT (so its propagation is observable), later ones -- the
 * undo -- with STORAGE when fail_later is set. */
static MarmotError
fault_write(void)
{
    g_faults.writes++;
    if (g_faults.fail_at == 0) return MARMOT_OK;
    if (g_faults.writes == g_faults.fail_at) return MARMOT_ERR_STORAGE_CONSTRAINT;
    if (g_faults.fail_later && g_faults.writes > g_faults.fail_at) return MARMOT_ERR_STORAGE;
    return MARMOT_OK;
}

static MarmotError
f_mls_store(void *ctx, const char *label, const uint8_t *key, size_t key_len,
            const uint8_t *value, size_t value_len)
{
    MarmotError ferr = fault_write();
    if (ferr != MARMOT_OK) return ferr;
    return g_faults.orig.mls_store(ctx, label, key, key_len, value, value_len);
}

static MarmotError
f_mls_delete(void *ctx, const char *label, const uint8_t *key, size_t key_len)
{
    if (g_faults.fail_delete_label && strcmp(label, g_faults.fail_delete_label) == 0)
        return MARMOT_ERR_STORAGE;
    MarmotError ferr = fault_write();
    if (ferr != MARMOT_OK) return ferr;
    return g_faults.orig.mls_delete(ctx, label, key, key_len);
}

static MarmotError
f_mls_load(void *ctx, const char *label, const uint8_t *key, size_t key_len,
           uint8_t **out, size_t *out_len)
{
    if (g_faults.fail_load_label && strcmp(label, g_faults.fail_load_label) == 0)
        return MARMOT_ERR_STORAGE;
    return g_faults.orig.mls_load(ctx, label, key, key_len, out, out_len);
}

static MarmotError
f_save_exporter(void *ctx, const MarmotGroupId *gid, uint64_t epoch,
                const uint8_t secret[32])
{
    MarmotError ferr = fault_write();
    if (ferr != MARMOT_OK) return ferr;
    return g_faults.orig.save_exporter_secret(ctx, gid, epoch, secret);
}

static MarmotError
f_delete_exporter(void *ctx, const MarmotGroupId *gid, uint64_t epoch)
{
    MarmotError ferr = fault_write();
    if (ferr != MARMOT_OK) return ferr;
    return g_faults.orig.delete_exporter_secret(ctx, gid, epoch);
}

static MarmotError
f_get_exporter(void *ctx, const MarmotGroupId *gid, uint64_t epoch, uint8_t out[32])
{
    if (g_faults.fail_exporter_load && epoch == g_faults.exporter_epoch)
        return MARMOT_ERR_STORAGE;
    return g_faults.orig.get_exporter_secret(ctx, gid, epoch, out);
}

static MarmotError
f_save_group(void *ctx, const MarmotGroup *group)
{
    if (g_faults.drop_group_record) return MARMOT_OK;
    MarmotError ferr = fault_write();
    if (ferr != MARMOT_OK) return ferr;
    return g_faults.orig.save_group(ctx, group);
}

static void
faults_arm(Member *x)
{
    MarmotStorage *s = x->m->storage;
    memset(&g_faults, 0, sizeof(g_faults));
    g_faults.orig = *s;
    s->mls_store = f_mls_store;
    s->mls_delete = f_mls_delete;
    s->mls_load = f_mls_load;
    s->save_exporter_secret = f_save_exporter;
    s->delete_exporter_secret = f_delete_exporter;
    s->get_exporter_secret = f_get_exporter;
    s->save_group = f_save_group;
}

static void
faults_disarm(Member *x)
{
    MarmotStorage *s = x->m->storage;
    s->mls_store = g_faults.orig.mls_store;
    s->mls_delete = g_faults.orig.mls_delete;
    s->mls_load = g_faults.orig.mls_load;
    s->save_exporter_secret = g_faults.orig.save_exporter_secret;
    s->delete_exporter_secret = g_faults.orig.delete_exporter_secret;
    s->get_exporter_secret = g_faults.orig.get_exporter_secret;
    s->save_group = g_faults.orig.save_group;
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
              g->admin_count == 2 && memcmp(g->admin_pubkeys[0], t.alice.pk, 32) == 0,
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

    /* Charlie is not an admin: he cannot rename. */
    MarmotGroupConfig cfg = {0};
    cfg.name = "Charlie's";
    char *none = NULL;
    CHECK(marmot_update_group_metadata(t.charlie.m, &t.gid, &cfg, &none) ==
          MARMOT_ERR_ADMIN_ONLY && none == NULL, "non-admin rename");
    CHECK(marmot_update_group_metadata(t.alice.m, &t.gid, &cfg, NULL) ==
          MARMOT_ERR_INVALID_ARG, "out_commit_json is required");
    expect_converged(t.all, 3, &t.gid, "Renamed", t.epoch + 1);

    free(commit);
    trio_clear(&t);
}

/* Every kind:445 is signed by its own fresh ephemeral key (MIP-03, review
 * B1): never an account key, never reused. */
static void
test_events_signed_by_fresh_ephemeral_keys(void)
{
    Trio t;
    trio_init(&t);
    char *events[4];
    MarmotOutgoingMessage out[2];
    memset(out, 0, sizeof(out));
    for (int i = 0; i < 2; i++) {
        OK(marmot_create_message(t.alice.m, &t.gid,
                                 "{\"kind\":9,\"content\":\"x\",\"created_at\":1,\"tags\":[]}",
                                 &out[i]));
        events[i] = out[i].event_json;
    }
    events[2] = rename_group(&t.alice, &t.gid, "Signed");
    events[3] = self_update(&t.bob, &t.gid);
    char *pubkeys[4];
    for (int i = 0; i < 4; i++) {
        NostrEvent ev;
        memset(&ev, 0, sizeof(ev));
        CHECK(nostr_event_deserialize_compact(&ev, events[i], NULL), "parse %d", i);
        CHECK(ev.kind == MARMOT_KIND_GROUP_MESSAGE, "kind");
        CHECK(ev.id && ev.sig && ev.pubkey && nostr_event_check_signature(&ev),
              "event %d is not signed", i);
        pubkeys[i] = strdup(ev.pubkey);
        for (size_t k = 0; k < 3; k++) {
            char *acct = marmot_hex_encode(t.all[k]->pk, 32);
            CHECK(strcmp(acct, ev.pubkey) != 0, "event %d signed by an account key", i);
            free(acct);
        }
        for (int j = 0; j < i; j++)
            CHECK(strcmp(pubkeys[j], pubkeys[i]) != 0, "ephemeral key reused");
        free(ev.id); free(ev.pubkey); free(ev.content); free(ev.sig);
        nostr_tags_free(ev.tags);
    }
    for (int i = 0; i < 4; i++) free(pubkeys[i]);
    marmot_outgoing_message_free(&out[0]);
    marmot_outgoing_message_free(&out[1]);
    free(events[2]);
    free(events[3]);
    trio_clear(&t);
}

/* Publish-before-merge (MIP-03, review B1): a Commit changes nothing until
 * merged; a failed publish is cleared and changes nothing either. */
static void
test_commit_pending_until_merged(void)
{
    Trio t;
    trio_init(&t);

    Snapshot before;
    snapshot(&t.alice, &t.gid, &before);
    char *failed = rename_pending(&t.alice, &t.gid, "Never published");
    expect_unchanged(&t.alice, &t.gid, &before, "pending rename");
    /* One Commit at a time. */
    MarmotGroupConfig cfg = {0};
    cfg.name = "Second";
    char *none = NULL;
    CHECK(marmot_update_group_metadata(t.alice.m, &t.gid, &cfg, &none) ==
          MARMOT_ERR_OWN_COMMIT_PENDING && !none, "second Commit while pending");
    /* The restart path sees it, with the event to republish. */
    char *ev = NULL;
    bool superseded = true;
    OK(marmot_get_pending_commit(t.alice.m, &t.gid, &ev, &superseded));
    CHECK(ev && strcmp(ev, failed) == 0 && !superseded, "pending event");
    free(ev);
    /* No relay accepted it. */
    OK(marmot_clear_pending_commit(t.alice.m, &t.gid));
    expect_unchanged(&t.alice, &t.gid, &before, "cleared");
    CHECK(marmot_merge_pending_commit(t.alice.m, &t.gid) == MARMOT_OK, "nothing to merge");
    OK(marmot_get_pending_commit(t.alice.m, &t.gid, &ev, NULL));
    CHECK(ev == NULL, "nothing pending after the clear");
    expect_unchanged(&t.alice, &t.gid, &before, "merge without pending");
    snapshot_clear(&before);
    expect_converged(t.all, 3, &t.gid, "Before", t.epoch);
    expect_messages_flow(t.all, 3, &t.gid);

    /* The next rename is published and merged. */
    char *ok = rename_group(&t.alice, &t.gid, "Published");
    expect_commit(&t.bob, ok, "published rename");
    expect_commit(&t.charlie, ok, "published rename");
    expect_converged(t.all, 3, &t.gid, "Published", t.epoch + 1);
    free(failed);
    free(ok);
    trio_clear(&t);
}

/* A member's Commit reaches us while ours is pending: the two compete now. */
static void
test_pending_commit_races(void)
{
    /* 1. Bob's ordinary Commit loses to Alice's pending rename: it is kept
     *    and, when Alice's publish fails, applied by the clear. */
    {
        Trio t;
        trio_init(&t);
        char *mine = rename_pending(&t.alice, &t.gid, "Lost");
        char *theirs = self_update(&t.bob, &t.gid);
        expect_rejected(&t.alice, &t.gid, theirs, MARMOT_ERR_OWN_COMMIT_PENDING,
                        "deferred behind the pending rename");
        expect_commit(&t.charlie, theirs, "Charlie follows Bob");
        OK(marmot_clear_pending_commit(t.alice.m, &t.gid));
        expect_converged(t.all, 3, &t.gid, "Before", t.epoch + 1);
        expect_messages_flow(t.all, 3, &t.gid);
        free(mine);
        free(theirs);
        trio_clear(&t);
    }
    /* 2. Same, but Alice's rename reaches a relay: merging applies it and
     *    the deferred Commit stays lost; Bob and Charlie switch to it. */
    {
        Trio t;
        trio_init(&t);
        char *mine = rename_pending(&t.alice, &t.gid, "Merged");
        char *theirs = self_update(&t.bob, &t.gid);
        expect_rejected(&t.alice, &t.gid, theirs, MARMOT_ERR_OWN_COMMIT_PENDING,
                        "deferred behind the pending rename");
        merge(&t.alice, &t.gid);
        expect_commit(&t.charlie, theirs, "Charlie: Bob first");
        expect_commit(&t.charlie, mine, "Charlie: Alice's wins");
        expect_commit(&t.bob, mine, "Bob: Alice's wins over his own");
        expect_converged(t.all, 3, &t.gid, "Merged", t.epoch + 1);
        expect_messages_flow(t.all, 3, &t.gid);
        free(mine);
        free(theirs);
        trio_clear(&t);
    }
    /* 3. Bob's privileged rename beats Alice's pending one (equal priority,
     *    Bob's key sorts lower): Alice applies Bob's, and her merge reports
     *    that her Commit lost. */
    {
        Trio t;
        trio_init(&t);
        char *mine = rename_pending(&t.alice, &t.gid, "Alice's");
        char *theirs = rename_group(&t.bob, &t.gid, "Bob's");
        expect_commit(&t.alice, theirs, "Bob's rename supersedes Alice's pending one");
        CHECK(marmot_merge_pending_commit(t.alice.m, &t.gid) == MARMOT_ERR_WRONG_EPOCH,
              "merge of a superseded Commit must fail");
        CHECK(marmot_merge_pending_commit(t.alice.m, &t.gid) == MARMOT_OK,
              "the superseded Commit is gone");
        expect_commit(&t.charlie, theirs, "Charlie follows Bob");
        expect_converged(t.all, 3, &t.gid, "Bob's", t.epoch + 1);
        free(mine);
        free(theirs);
        trio_clear(&t);
    }
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
    /* The same event again is already processed (event-id idempotency)... */
    snapshot(&t.bob, &t.gid, &before);
    CHECK(deliver(&t.bob, r1, &err, NULL) == MARMOT_RESULT_OWN_MESSAGE &&
          err == MARMOT_OK, "processed R1 event: %d", err);
    expect_unchanged(&t.bob, &t.gid, &before, "processed R1 event");
    snapshot_clear(&before);
    /* ...and R1 in a new envelope is older than the Commit Bob's state was
     * built on. */
    char *r1_again = republish(r1);
    expect_rejected(&t.bob, &t.gid, r1_again, MARMOT_ERR_WRONG_EPOCH, "stale R1");

    /* Charlie missed R1: R2 comes from an epoch he has no key for yet. */
    expect_rejected(&t.charlie, &t.gid, r2, MARMOT_ERR_NIP44, "future R2");
    /* Retried in order, both apply. */
    expect_commit(&t.charlie, r1, "R1 late");
    expect_commit(&t.charlie, r2, "R2 retried");
    expect_converged(t.all, 3, &t.gid, "Two", t.epoch + 2);
    expect_messages_flow(t.all, 3, &t.gid);

    /* Old Commits stay stale for everyone, the committer included. */
    for (size_t i = 0; i < 3; i++)
        expect_rejected(t.all[i], &t.gid, r1_again, MARMOT_ERR_WRONG_EPOCH,
                        "stale R1 again");
    free(r1_again);
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

    /* Charlie (not an admin) renames the group behind the API's back. */
    MarmotGroupDataExtension *gde = group_data_with(&t.charlie, &t.gid, "Charlie's", NULL);
    char *forged = forge_group_data_commit(&t.charlie, &t.gid, gde, t.nostr_gid);
    marmot_group_data_extension_free(gde);
    expect_rejected(&t.alice, &t.gid, forged, MARMOT_ERR_COMMIT_FROM_NON_ADMIN,
                    "non-admin GroupData change");
    expect_rejected(&t.bob, &t.gid, forged, MARMOT_ERR_COMMIT_FROM_NON_ADMIN,
                    "non-admin GroupData change");
    free(forged);

    /* Removing a member is privileged too. */
    {
        MlsGroup g;
        load_mls(&t.charlie, &t.gid, &g);
        uint8_t exporter[32];
        memcpy(exporter, g.epoch_secrets.exporter_secret, 32);
        MlsCommitResult r;
        memset(&r, 0, sizeof(r));
        CHECK(mls_group_remove_member(&g, 1, &r) == 0, "remove");
        char *removal = event_for_commit(&r, exporter, t.nostr_gid);
        expect_rejected(&t.alice, &t.gid, removal, MARMOT_ERR_COMMIT_FROM_NON_ADMIN,
                        "non-admin Remove");
        free(removal);
        mls_commit_result_clear(&r);
        mls_group_free(&g);
        sodium_memzero(exporter, sizeof(exporter));
    }

    /* An admin cannot move the group to another nostr_group_id... */
    uint8_t other[32];
    randombytes_buf(other, sizeof(other));
    gde = group_data_with(&t.alice, &t.gid, "Moved", other);
    forged = forge_group_data_commit(&t.alice, &t.gid, gde, t.nostr_gid);
    marmot_group_data_extension_free(gde);
    expect_rejected(&t.bob, &t.gid, forged, MARMOT_ERR_PROTOCOL_GROUP_MISMATCH,
                    "nostr_group_id change");
    expect_rejected(&t.charlie, &t.gid, forged, MARMOT_ERR_PROTOCOL_GROUP_MISMATCH,
                    "nostr_group_id change");
    free(forged);

    /* ...nor drop the GroupData altogether (review N5). */
    forged = forge_extensions_commit(&t.alice, &t.gid, NULL, 0, t.nostr_gid);
    expect_rejected(&t.bob, &t.gid, forged, MARMOT_ERR_EXTENSION_FORMAT,
                    "GroupData removed");
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
        char *tampered = event_for_commit(&r, exporter, t.nostr_gid);
        expect_rejected(&t.alice, &t.gid, tampered, MARMOT_ERR_MLS_PROCESS_MESSAGE,
                        "tampered Commit");
        expect_rejected(&t.bob, &t.gid, tampered, MARMOT_ERR_MLS_PROCESS_MESSAGE,
                        "tampered Commit");
        free(tampered);
        mls_commit_result_clear(&r);
        mls_group_free(&g);
        sodium_memzero(exporter, sizeof(exporter));
    }

    /* Sealed under another epoch's exporter secret than its own (review N5):
     * a valid current-epoch Commit wrapped with the previous epoch's key. */
    {
        MlsGroup g;
        load_mls(&t.alice, &t.gid, &g);
        uint8_t previous[32];
        OK(t.alice.m->storage->get_exporter_secret(t.alice.m->storage->ctx,
                                                   &t.gid, g.epoch - 1, previous));
        MlsCommitResult r;
        memset(&r, 0, sizeof(r));
        CHECK(mls_group_self_update(&g, &r) == 0, "self_update");
        char *wrapped = event_for_commit(&r, previous, t.nostr_gid);
        /* Bob holds both exporter secrets: the envelope opens with the older
         * one, the Commit inside is for the current epoch. */
        expect_rejected(&t.bob, &t.gid, wrapped, MARMOT_ERR_WRONG_EPOCH,
                        "outer/inner epoch mismatch");
        free(wrapped);
        mls_commit_result_clear(&r);
        mls_group_free(&g);
        sodium_memzero(previous, sizeof(previous));
    }

    /* Nothing above moved anyone. */
    expect_converged(t.all, 3, &t.gid, "Before", t.epoch);

    /* Any member may self-update. */
    char *upd = self_update(&t.charlie, &t.gid);
    expect_commit(&t.alice, upd, "Charlie self-update");
    expect_commit(&t.bob, upd, "Charlie self-update");
    expect_converged(t.all, 3, &t.gid, "Before", t.epoch + 1);
    expect_messages_flow(t.all, 3, &t.gid);
    free(upd);
    trio_clear(&t);
}

/* The committer must stay in the group under its own account (review N5:
 * the MLS layer already pins UpdatePath identities, so this is checked on
 * the authorization function directly). */
static void
test_authorize_pins_committer_identity(void)
{
    Trio t;
    trio_init(&t);
    MlsGroup pre, post;
    load_mls(&t.alice, &t.gid, &pre);
    load_mls(&t.alice, &t.gid, &post);
    MarmotCommitKey key;
    MarmotGroupDataExtension *gde = NULL;
    OK(marmot_commit_authorize(&pre, &post, 0, &key, &gde));
    marmot_group_data_extension_free(gde);
    gde = NULL;
    CHECK(!key.privileged && memcmp(key.committer, t.alice.pk, 32) == 0, "key");

    MlsLeafNode *leaf = &post.tree.nodes[mls_tree_leaf_to_node(0)].leaf;
    leaf->credential_identity[0] ^= 0x01;
    CHECK(marmot_commit_authorize(&pre, &post, 0, &key, &gde) ==
          MARMOT_ERR_IDENTITY_CHANGE && !gde, "committer account changed");
    leaf->credential_identity[0] ^= 0x01;
    /* Another member's slot taken by a different account is a membership
     * change (Remove + Add), i.e. privileged -- not an identity change. */
    MlsLeafNode *bob_leaf = &post.tree.nodes[mls_tree_leaf_to_node(1)].leaf;
    bob_leaf->credential_identity[0] ^= 0x01;
    OK(marmot_commit_authorize(&pre, &post, 0, &key, &gde));
    CHECK(key.privileged, "reused slot must be privileged");
    marmot_group_data_extension_free(gde);
    gde = NULL;
    CHECK(marmot_commit_authorize(&pre, &post, 2, &key, &gde) ==
          MARMOT_ERR_COMMIT_FROM_NON_ADMIN && !gde, "non-admin membership change");
    mls_group_free(&pre);
    mls_group_free(&post);
    trio_clear(&t);
}

/* Our own leaf cannot author a Commit we did not make (review N5). */
static void
test_own_leaf_commit_not_ours_rejected(void)
{
    Trio t;
    trio_init(&t);
    MlsGroup parent;
    load_mls(&t.bob, &t.gid, &parent);
    uint8_t exporter[32];
    memcpy(exporter, parent.epoch_secrets.exporter_secret, 32);
    char *applied = self_update(&t.bob, &t.gid);
    /* A second Commit from the same parent state and Bob's own leaf. */
    MlsCommitResult r;
    memset(&r, 0, sizeof(r));
    CHECK(mls_group_self_update(&parent, &r) == 0, "self_update");
    char *other = event_for_commit(&r, exporter, t.nostr_gid);
    expect_rejected(&t.bob, &t.gid, other, MARMOT_ERR_WRONG_EPOCH,
                    "a different Commit from our own leaf");
    free(other);
    free(applied);
    mls_commit_result_clear(&r);
    mls_group_free(&parent);
    sodium_memzero(exporter, sizeof(exporter));
    trio_clear(&t);
}

/* A failed write rolls back every earlier write; a failed rollback is
 * reported as a storage error; unreadable records abort before any write
 * (review N3/N5). */
static void
test_persist_rolls_back_failed_writes(void)
{
    Trio t;
    trio_init(&t);
    char *c = rename_group(&t.alice, &t.gid, "Faults");
    /* Bob applying `c` writes: exporter secret, retained parent, MLS state,
     * group record. */
    for (int k = 1; k <= 4; k++) {
        Snapshot before;
        snapshot(&t.bob, &t.gid, &before);
        faults_arm(&t.bob);
        g_faults.fail_at = k;
        MarmotError err;
        deliver(&t.bob, c, &err, NULL);
        faults_disarm(&t.bob);
        /* A clean rollback reports the failed write's own error. */
        CHECK(err == MARMOT_ERR_STORAGE_CONSTRAINT, "write %d: err %d", k, err);
        expect_unchanged(&t.bob, &t.gid, &before, "rolled back");
        snapshot_clear(&before);
    }
    /* The undo itself fails: the caller must learn storage is suspect. */
    faults_arm(&t.bob);
    g_faults.fail_at = 3;
    g_faults.fail_later = true;
    MarmotError err;
    deliver(&t.bob, c, &err, NULL);
    faults_disarm(&t.bob);
    CHECK(err == MARMOT_ERR_STORAGE, "a failed undo must be reported as such: %d", err);
    /* Unreadable earlier records: nothing is written at all. */
    const char *labels[] = { "mls_group_parent", NULL };
    for (int i = 0; i < 2; i++) {
        Snapshot before;
        snapshot(&t.charlie, &t.gid, &before);
        faults_arm(&t.charlie);
        g_faults.fail_load_label = labels[i];
        /* The next epoch's secret is read only by the persist step. */
        g_faults.fail_exporter_load = labels[i] == NULL;
        g_faults.exporter_epoch = t.epoch + 1;
        deliver(&t.charlie, c, &err, NULL);
        int writes = g_faults.writes;
        faults_disarm(&t.charlie);
        CHECK(err == MARMOT_ERR_STORAGE && writes == 0,
              "read error %d: err %d after %d writes", i, err, writes);
        expect_unchanged(&t.charlie, &t.gid, &before, "read error");
        snapshot_clear(&before);
    }
    expect_commit(&t.charlie, c, "Charlie, storage healthy");
    free(c);
    trio_clear(&t);
}

/* A crash between the MLS state write and the group record write leaves the
 * record an epoch behind; the next operation repairs it (review N2). */
static void
test_interrupted_transition_is_repaired(void)
{
    Trio t;
    trio_init(&t);
    char *c = rename_group(&t.alice, &t.gid, "After crash");
    faults_arm(&t.bob);
    g_faults.drop_group_record = true;
    expect_commit(&t.bob, c, "record write lost");
    faults_disarm(&t.bob);
    MarmotGroup *rec = NULL;
    MarmotStorage *s = t.bob.m->storage;
    OK(s->find_group_by_mls_id(s->ctx, &t.gid, &rec));
    CHECK(rec->epoch == t.epoch, "the record lags the MLS state");
    marmot_group_free(rec);
    expect_commit(&t.charlie, c, "Charlie");
    /* Bob reads Alice's next-epoch message, which trial decryption from the
     * stale record epoch alone would miss. */
    expect_messages_flow(t.all, 3, &t.gid);
    expect_converged(t.all, 3, &t.gid, "After crash", t.epoch + 1);
    free(c);
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

    /* Admin rename (privileged) vs Bob's self-update (ordinary).  Alice's key
     * sorts above Bob's, so only the privileged step makes Alice's win. */
    char *c_a = rename_group(&t.alice, &t.gid, "Admin wins");
    char *c_b = self_update(&t.bob, &t.gid);
    expect_commit(&t.charlie, c_b, "Charlie: B first");
    expect_commit(&t.charlie, c_a, "Charlie: A replaces B");
    expect_commit(&t.bob, c_a, "Bob: A replaces his own B");
    expect_rejected(&t.alice, &t.gid, c_b, MARMOT_ERR_WRONG_EPOCH, "Alice: B loses");
    /* Re-deliveries in new envelopes change nothing. */
    char *c_b2 = republish(c_b), *c_a2 = republish(c_a);
    expect_rejected(&t.charlie, &t.gid, c_b2, MARMOT_ERR_WRONG_EPOCH, "B again");
    MarmotError err;
    CHECK(deliver(&t.bob, c_a2, &err, NULL) == MARMOT_RESULT_OWN_MESSAGE &&
          err == MARMOT_OK, "A again: %d", err);
    free(c_b2);
    free(c_a2);
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

/* Several invitees or members per call are one Commit (review B2,
 * nostrc-wc6v): a three-invitee group, a two-member add and a two-member
 * remove leave every member on the same epoch. */
static void
test_multi_member_commits_converge(void)
{
    Member alice, bob, charlie, dave, eve, frank;
    member_init(&alice, "Alice");
    member_init(&bob, "Bob");
    member_init(&charlie, "Charlie");
    member_init(&dave, "Dave");
    member_init(&eve, "Eve");
    member_init(&frank, "Frank");

    char *kp[3] = { key_package(&bob), key_package(&charlie), key_package(&dave) };
    MarmotGroupConfig cfg = {0};
    cfg.name = "Many";
    cfg.admin_pubkeys = (uint8_t (*)[32])alice.pk;
    cfg.admin_count = 1;
    MarmotCreateGroupResult cg;
    memset(&cg, 0, sizeof(cg));
    OK(marmot_create_group(alice.m, alice.pk, (const char **)kp, 3, &cfg, &cg));
    CHECK(cg.welcome_count == 3 && cg.group->epoch == 1,
          "three invitees, one Commit: epoch %llu", (unsigned long long)cg.group->epoch);
    MarmotGroupId gid = marmot_group_id_new(cg.group->mls_group_id.data,
                                            cg.group->mls_group_id.len);
    join(&bob, cg.welcome_rumor_jsons[0]);
    join(&charlie, cg.welcome_rumor_jsons[1]);
    join(&dave, cg.welcome_rumor_jsons[2]);
    marmot_create_group_result_free(&cg);
    for (int i = 0; i < 3; i++) free(kp[i]);
    Member *four[] = { &alice, &bob, &charlie, &dave };
    expect_converged(four, 4, &gid, "Many", 1);
    expect_messages_flow(four, 4, &gid);

    /* Two more in one Commit. */
    char *kp2[2] = { key_package(&eve), key_package(&frank) };
    char **welcomes = NULL;
    size_t n_welcomes = 0;
    char *commit = NULL;
    OK(marmot_add_members(alice.m, &gid, (const char **)kp2, 2, &welcomes, &n_welcomes,
                          &commit));
    merge(&alice, &gid);
    for (int i = 1; i < 4; i++) expect_commit(four[i], commit, "two Adds in one Commit");
    join(&eve, welcomes[0]);
    join(&frank, welcomes[1]);
    for (size_t i = 0; i < n_welcomes; i++) free(welcomes[i]);
    free(welcomes);
    free(commit);
    for (int i = 0; i < 2; i++) free(kp2[i]);
    Member *six[] = { &alice, &bob, &charlie, &dave, &eve, &frank };
    expect_converged(six, 6, &gid, "Many", 2);
    expect_messages_flow(six, 6, &gid);

    /* Two out in one Commit. */
    uint8_t out[2][32];
    memcpy(out[0], dave.pk, 32);
    memcpy(out[1], eve.pk, 32);
    OK(marmot_remove_members(alice.m, &gid, (const uint8_t (*)[32])out, 2, &commit));
    merge(&alice, &gid);
    Member *rest[] = { &alice, &bob, &charlie, &frank };
    for (int i = 1; i < 4; i++) expect_commit(rest[i], commit, "two Removes in one Commit");
    free(commit);
    expect_converged(rest, 4, &gid, "Many", 3);
    expect_messages_flow(rest, 4, &gid);
    /* The same member twice is refused, not half-applied. */
    uint8_t twice[2][32];
    memcpy(twice[0], bob.pk, 32);
    memcpy(twice[1], bob.pk, 32);
    CHECK(marmot_remove_members(alice.m, &gid, (const uint8_t (*)[32])twice, 2, &commit) ==
          MARMOT_ERR_INVALID_ARG && commit == NULL, "duplicate Remove");
    expect_converged(rest, 4, &gid, "Many", 3);

    for (int i = 0; i < 6; i++) marmot_free(six[i]->m);
    marmot_group_id_free(&gid);
}


/* Review R1: a winning competitor replaces the state our pending Commit was
 * built on -- with another of the same epoch.  The pending Commit must not
 * defer the group's next Commit, and must not merge. */
static void
test_stale_pending_commit_cannot_merge(void)
{
    Trio t;
    trio_init(&t);
    bool bob_wins = memcmp(t.bob.pk, t.charlie.pk, 32) < 0;
    Member *w = bob_wins ? &t.bob : &t.charlie;      /* lower key */
    Member *l = bob_wins ? &t.charlie : &t.bob;      /* higher key */

    char *c_l = self_update(l, &t.gid);
    char *c_w = self_update(w, &t.gid);
    expect_commit(&t.alice, c_l, "Alice applies L first");
    char *rename = rename_pending(&t.alice, &t.gid, "Built on L");
    expect_commit(&t.alice, c_w, "W wins and replaces L at Alice");
    expect_commit(l, c_w, "L switches to W");
    expect_rejected(w, &t.gid, c_l, MARMOT_ERR_WRONG_EPOCH, "W keeps W");
    expect_converged(t.all, 3, &t.gid, "Before", t.epoch + 1);

    /* The pending rename now reports itself superseded... */
    char *ev = NULL;
    bool superseded = false;
    OK(marmot_get_pending_commit(t.alice.m, &t.gid, &ev, &superseded));
    CHECK(ev && superseded, "pending rename must be superseded");
    free(ev);
    /* ...does not hold back the group's next Commit... */
    char *c_x = self_update(w, &t.gid);
    expect_commit(&t.alice, c_x, "W's next Commit applies at Alice");
    expect_commit(l, c_x, "and at L");
    expect_converged(t.all, 3, &t.gid, "Before", t.epoch + 2);
    /* ...and cannot merge when the relay's OK finally arrives. */
    CHECK(marmot_merge_pending_commit(t.alice.m, &t.gid) == MARMOT_ERR_WRONG_EPOCH,
          "a stale pending Commit must not merge");
    expect_converged(t.all, 3, &t.gid, "Before", t.epoch + 2);
    expect_messages_flow(t.all, 3, &t.gid);
    /* The record is gone: Alice can commit again. */
    char *next = rename_group(&t.alice, &t.gid, "After");
    expect_commit(&t.bob, next, "next rename");
    expect_commit(&t.charlie, next, "next rename");
    expect_converged(t.all, 3, &t.gid, "After", t.epoch + 3);
    free(c_l);
    free(c_w);
    free(c_x);
    free(rename);
    free(next);
    trio_clear(&t);
}

/* Review R2: the committer crashes (or loses the relay's OK) with its Commit
 * pending while a relay stored it and the others applied it.  Its own echo
 * from the relay merges it; the restart path offers the event to republish
 * and merging stays idempotent. */
static void
test_pending_commit_recovered_by_echo(void)
{
    Trio t;
    trio_init(&t);
    char *c = rename_pending(&t.alice, &t.gid, "Stored");
    expect_commit(&t.bob, c, "Bob applies the stored Commit");
    expect_commit(&t.charlie, c, "Charlie applies the stored Commit");
    /* "Restart": everything is in storage; the pending event is there. */
    char *ev = NULL;
    OK(marmot_get_pending_commit(t.alice.m, &t.gid, &ev, NULL));
    CHECK(ev && strcmp(ev, c) == 0, "restart path returns the signed event");
    free(ev);
    /* Relay backfill delivers Alice's own Commit: merged. */
    MarmotError err;
    MarmotGroup *g = NULL;
    CHECK(deliver(&t.alice, c, &err, &g) == MARMOT_RESULT_COMMIT && err == MARMOT_OK,
          "own echo must merge the pending Commit: %d", err);
    CHECK(g && strcmp(g->name, "Stored") == 0, "echo result");
    marmot_group_free(g);
    expect_converged(t.all, 3, &t.gid, "Stored", t.epoch + 1);
    /* A late relay OK: the merge is already done. */
    OK(marmot_merge_pending_commit(t.alice.m, &t.gid));
    /* Alice is not wedged: others' Commits decrypt, hers are allowed. */
    char *upd = self_update(&t.bob, &t.gid);
    expect_commit(&t.alice, upd, "Bob's next Commit");
    expect_commit(&t.charlie, upd, "Bob's next Commit");
    char *next = rename_group(&t.alice, &t.gid, "Unwedged");
    expect_commit(&t.bob, next, "Alice's next rename");
    expect_commit(&t.charlie, next, "Alice's next rename");
    expect_converged(t.all, 3, &t.gid, "Unwedged", t.epoch + 3);
    free(c);
    free(upd);
    free(next);
    trio_clear(&t);
}

/* Review R2/N-a: a crash between persisting the merge and deleting the
 * pending record.  The leftover is recognised as merged: merging again
 * succeeds, its Welcomes reach the outbox once, and new Commits proceed. */
static void
test_merge_idempotent_after_crash(void)
{
    Trio t;
    trio_init(&t);
    Member dave;
    member_init(&dave, "Dave");
    char *kp = key_package(&dave);
    const char *kps[] = { kp };
    char **welcomes = NULL;
    size_t n = 0;
    char *commit = NULL;
    OK(marmot_add_members(t.alice.m, &t.gid, kps, 1, &welcomes, &n, &commit));
    /* The pending record's delete is lost in the crash. */
    faults_arm(&t.alice);
    g_faults.fail_delete_label = "mls_group_pending";
    OK(marmot_merge_pending_commit(t.alice.m, &t.gid));
    faults_disarm(&t.alice);
    uint8_t *left = NULL;
    size_t left_len = 0;
    MarmotStorage *st = t.alice.m->storage;
    OK(st->mls_load(st->ctx, "mls_group_pending", t.gid.data, t.gid.len, &left, &left_len));
    CHECK(left != NULL, "the pending record survived the crash");
    free(left);
    /* Merging again finishes it -- not "superseded". */
    OK(marmot_merge_pending_commit(t.alice.m, &t.gid));
    CHECK(st->mls_load(st->ctx, "mls_group_pending", t.gid.data, t.gid.len, &left,
                       &left_len) == MARMOT_ERR_STORAGE_NOT_FOUND, "record dropped");
    MarmotUnsentWelcome *out = NULL;
    size_t out_n = 0;
    OK(marmot_get_unsent_welcomes(t.alice.m, &t.gid, &out, &out_n));
    CHECK(out_n == 1 && memcmp(out[0].recipient, dave.pk, 32) == 0 &&
          strcmp(out[0].rumor_json, welcomes[0]) == 0, "the Add's Welcome is in the outbox");
    marmot_unsent_welcomes_free(out, out_n);
    OK(marmot_mark_welcomes_sent(t.alice.m, &t.gid));
    OK(marmot_get_unsent_welcomes(t.alice.m, &t.gid, &out, &out_n));
    CHECK(out == NULL && out_n == 0, "outbox emptied");
    expect_commit(&t.bob, commit, "Bob");
    expect_commit(&t.charlie, commit, "Charlie");
    join(&dave, welcomes[0]);
    Member *four[] = { &t.alice, &t.bob, &t.charlie, &dave };
    expect_converged(four, 4, &t.gid, "Before", t.epoch + 1);
    expect_messages_flow(four, 4, &t.gid);

    /* A crash-leftover is also finished by the next producer. */
    char *r = rename_pending(&t.alice, &t.gid, "Next");
    faults_arm(&t.alice);
    g_faults.fail_delete_label = "mls_group_pending";
    OK(marmot_merge_pending_commit(t.alice.m, &t.gid));
    faults_disarm(&t.alice);
    char *r2 = rename_group(&t.alice, &t.gid, "After leftover");
    for (int i = 1; i < 4; i++) {
        expect_commit(four[i], r, "rename");
        expect_commit(four[i], r2, "rename after leftover");
    }
    expect_converged(four, 4, &t.gid, "After leftover", t.epoch + 3);

    for (size_t i = 0; i < n; i++) free(welcomes[i]);
    free(welcomes);
    free(commit);
    free(kp);
    free(r);
    free(r2);
    marmot_free(dave.m);
    trio_clear(&t);
}

/* Review R2/N-c: a merge that fails on storage leaves the Commit pending and
 * clearable; nothing changed. */
static void
test_failed_merge_stays_clearable(void)
{
    Trio t;
    trio_init(&t);
    Snapshot before;
    snapshot(&t.alice, &t.gid, &before);
    char *c = rename_pending(&t.alice, &t.gid, "Not yet");
    faults_arm(&t.alice);
    g_faults.fail_at = 3;   /* the MLS state write */
    MarmotError err = marmot_merge_pending_commit(t.alice.m, &t.gid);
    faults_disarm(&t.alice);
    CHECK(err == MARMOT_ERR_STORAGE_CONSTRAINT, "merge storage failure: %d", err);
    expect_unchanged(&t.alice, &t.gid, &before, "failed merge");
    char *ev = NULL;
    bool superseded = true;
    OK(marmot_get_pending_commit(t.alice.m, &t.gid, &ev, &superseded));
    CHECK(ev && !superseded, "still pending and live");
    free(ev);
    OK(marmot_clear_pending_commit(t.alice.m, &t.gid));
    expect_unchanged(&t.alice, &t.gid, &before, "cleared");
    snapshot_clear(&before);
    char *next = rename_group(&t.alice, &t.gid, "Recovered");
    expect_commit(&t.bob, next, "next rename");
    expect_commit(&t.charlie, next, "next rename");
    expect_converged(t.all, 3, &t.gid, "Recovered", t.epoch + 1);
    free(c);
    free(next);
    trio_clear(&t);
}

int
main(void)
{
    if (sodium_init() < 0) return 1;
    printf("libmarmot: Commit publication and ingestion (nostrc-9ata)\n");
    RUN(test_rename_reaches_every_member);
    RUN(test_events_signed_by_fresh_ephemeral_keys);
    RUN(test_commit_pending_until_merged);
    RUN(test_pending_commit_races);
    RUN(test_stale_duplicate_and_future_commits);
    RUN(test_unauthorized_and_invalid_commits_rejected);
    RUN(test_authorize_pins_committer_identity);
    RUN(test_own_leaf_commit_not_ours_rejected);
    RUN(test_persist_rolls_back_failed_writes);
    RUN(test_interrupted_transition_is_repaired);
    RUN(test_same_epoch_race_converges);
    RUN(test_multi_member_commits_converge);
    RUN(test_stale_pending_commit_cannot_merge);
    RUN(test_pending_commit_recovered_by_echo);
    RUN(test_merge_idempotent_after_crash);
    RUN(test_failed_merge_stays_clearable);
    printf("All commit tests passed\n");
    return 0;
}
