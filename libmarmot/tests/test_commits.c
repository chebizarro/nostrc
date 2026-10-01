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
#include "kp_profile.h"
#include "test_enroll.h"
#include "mls/mls_group.h"
#include "mls/mls_framing.h"
#include "mls/mls-internal.h"
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <sodium.h>
#include <inttypes.h>
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

/* argv[1], when given, runs only the test of that name. */
static const char *g_only;
#define RUN(fn)                                                             \
    do {                                                                    \
        if (g_only && strcmp(g_only, #fn) != 0) break;                      \
        printf("  %-58s", #fn); fflush(stdout); fn(); printf("PASS\n");    \
    } while (0)

/* ── Members ──────────────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    Marmot     *m;
    uint8_t     sk[32], pk[32];
} Member;

/* A member whose instance holds no account proof yet (nostrc-7vyi). */
static void
member_init_unenrolled(Member *x, const char *name)
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

/* A member enrolled as a signer-only client would be: it can create groups. */
static void
member_init(Member *x, const char *name)
{
    member_init_unenrolled(x, name);
    OK(test_enroll(x->m, x->pk, x->sk));
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

/* The application confirmed every Welcome in the outbox as sent. */
static void
mark_all_welcomes_sent(Member *x, const MarmotGroupId *gid)
{
    MarmotUnsentWelcome *w = NULL;
    size_t n = 0;
    OK(marmot_get_unsent_welcomes(x->m, gid, &w, &n));
    for (size_t i = 0; i < n; i++)
        OK(marmot_mark_welcomes_sent(x->m, gid, (const uint8_t (*)[32]) w[i].id, 1));
    marmot_unsent_welcomes_free(w, n);
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

/* The sender data of `x`'s kind:445 application message (RFC 9420 §6.3.2):
 * which leaf sent it, at which generation, with which reuse guard.  Read
 * with `x`'s stored state, which must be at the message's epoch. */
static MlsSenderData
sent_sender_data(Member *x, const MarmotGroupId *gid, const char *event_json)
{
    NostrEvent *ev = nostr_event_new();
    CHECK(ev && nostr_event_deserialize_compact(ev, event_json, NULL), "parse");
    MlsGroup g;
    load_mls(x, gid, &g);
    uint8_t *mls = NULL;
    size_t mls_len = 0;
    CHECK(marmot_group_event_decrypt(g.epoch_secrets.exporter_secret, ev->content,
                                     &mls, &mls_len) == 0, "NIP-44 layer");
    MlsTlsReader r;
    mls_tls_reader_init(&r, mls, mls_len);
    MlsMLSMessage wire;
    CHECK(mls_message_deserialize(&r, &wire) == 0 &&
          wire.wire_format == MLS_WIRE_FORMAT_PRIVATE_MESSAGE, "a PrivateMessage");
    const MlsPrivateMessage *pm = &wire.private_message;
    CHECK(pm->epoch == g.epoch, "the sender's state is at the message's epoch");
    size_t sample = pm->ciphertext_len < MLS_HASH_LEN ? pm->ciphertext_len : MLS_HASH_LEN;
    MlsSenderData sd;
    const MlsSenderDataAAD sd_aad = { pm->group_id, pm->group_id_len, pm->epoch, pm->content_type };
    CHECK(mls_sender_data_decrypt(g.epoch_secrets.sender_data_secret, &sd_aad, pm->ciphertext,
                                  sample, pm->encrypted_sender_data,
                                  pm->encrypted_sender_data_len, &sd) == 0,
          "sender data");
    mls_message_clear(&wire);
    free(mls);
    mls_group_free(&g);
    nostr_event_free(ev);
    return sd;
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
    /* A new inner event each time: the same one again is a duplicate. */
    static unsigned round;
    round++;
    for (size_t s = 0; s < n; s++) {
        char inner[160];
        snprintf(inner, sizeof(inner),
                 "{\"kind\":9,\"content\":\"hello from %s (%u)\",\"created_at\":1700000000,\"tags\":[]}",
                 ms[s]->name, round);
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

/* trio_init(): Charlie's key sorts below Bob's too (Charlie < Bob < Alice). */
static bool g_charlie_lowest;

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
    if (g_charlie_lowest) member_rekey_order(&t->charlie, &t->bob, false);
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
    mark_all_welcomes_sent(&t->alice, &t->gid);
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
    OK(marmot_commit_authorize(&pre, &post, pre.own_leaf_index, false, &key, &gde));
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
    bool  fail_save_message;      /* save_message fails (not counted as a write) */
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
f_save_message(void *ctx, const MarmotMessage *msg)
{
    if (g_faults.fail_save_message) return MARMOT_ERR_STORAGE_CONSTRAINT;
    return g_faults.orig.save_message(ctx, msg);
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
    s->save_message = f_save_message;
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
    s->save_message = g_faults.orig.save_message;
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
    OK(marmot_commit_authorize(&pre, &post, 0, false, &key, &gde));
    marmot_group_data_extension_free(gde);
    gde = NULL;
    CHECK(!key.privileged && memcmp(key.committer, t.alice.pk, 32) == 0, "key");

    MlsLeafNode *leaf = &post.tree.nodes[mls_tree_leaf_to_node(0)].leaf;
    leaf->credential_identity[0] ^= 0x01;
    CHECK(marmot_commit_authorize(&pre, &post, 0, false, &key, &gde) ==
          MARMOT_ERR_IDENTITY_CHANGE && !gde, "committer account changed");
    leaf->credential_identity[0] ^= 0x01;
    /* Another member's slot taken by a different account is a membership
     * change (Remove + Add), i.e. privileged -- not an identity change.
     * Bob's proof does not bind the new account (nostrc-7vyi) ... */
    MlsLeafNode *bob_leaf = &post.tree.nodes[mls_tree_leaf_to_node(1)].leaf;
    bob_leaf->credential_identity[0] ^= 0x01;
    CHECK(marmot_commit_authorize(&pre, &post, 0, true, &key, &gde) ==
          MARMOT_ERR_KEY_PACKAGE_IDENTITY && !gde, "Bob's proof on another account's leaf");
    /* ... and without any, only legacy mode takes it. */
    free(bob_leaf->extensions_data);
    bob_leaf->extensions_data = NULL;
    bob_leaf->extensions_len = 0;
    CHECK(marmot_commit_authorize(&pre, &post, 0, false, &key, &gde) ==
          MARMOT_ERR_KEY_PACKAGE_IDENTITY && !gde, "an unproven account in Bob's slot");
    OK(marmot_commit_authorize(&pre, &post, 0, true, &key, &gde));
    CHECK(key.privileged, "reused slot must be privileged");
    marmot_group_data_extension_free(gde);
    gde = NULL;
    CHECK(marmot_commit_authorize(&pre, &post, 2, true, &key, &gde) ==
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

/* MIP-01 "Required MLS Extensions": the GroupContext carries
 * required_capabilities requiring 0xF2EE (no proposal type: exactly what
 * MDK 0.8 computes for a group with a member without SelfRemove), and a
 * metadata Commit keeps exactly one. Without it OpenMLS refused every
 * GroupContextExtensions proposal of ours (nostrc-7gx7). */
static void
expect_required_capabilities(Member *x, const MarmotGroupId *gid, const char *what)
{
    static const uint8_t want[] = { 0x02, 0xf2, 0xee, 0x00, 0x00 };
    MlsGroup mls;
    load_mls(x, gid, &mls);
    const uint8_t *data = NULL;
    size_t len = 0, count = 0;
    int found = marmot_extensions_find(mls.extensions_data, mls.extensions_len, 0x0003, &data,
                                       &len, &count);
    CHECK(found == 0 && count == 1, "%s: %s has %zu required_capabilities", what, x->name,
          count);
    CHECK(len == sizeof want && memcmp(data, want, len) == 0,
          "%s: required_capabilities of %s", what, x->name);
    mls_group_free(&mls);
}

static void
test_group_context_required_capabilities(void)
{
    Trio t;
    trio_init(&t);
    for (int i = 0; i < 3; i++) expect_required_capabilities(t.all[i], &t.gid, "joined");
    char *commit = rename_group(&t.alice, &t.gid, "Renamed");
    expect_commit(&t.bob, commit, "rename");
    expect_commit(&t.charlie, commit, "rename");
    free(commit);
    for (int i = 0; i < 3; i++) expect_required_capabilities(t.all[i], &t.gid, "renamed");
    trio_clear(&t);
}

/* Whether a Welcome rumor's relays tag (MIP-02) lists exactly `relays`. */
static bool
rumor_relays_are(const char *rumor_json, const char *const *relays, size_t n)
{
    NostrEvent *ev = nostr_event_new();
    CHECK(ev && nostr_event_deserialize_compact(ev, rumor_json, NULL), "parse rumor");
    const NostrTags *tags = nostr_event_get_tags(ev);
    bool ok = false;
    for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
        NostrTag *tag = nostr_tags_get(tags, i);
        if (strcmp(nostr_tag_get_key(tag), "relays") != 0) continue;
        ok = nostr_tag_size(tag) == n + 1;
        for (size_t j = 0; ok && j < n; j++) ok = strcmp(nostr_tag_get(tag, j + 1), relays[j]) == 0;
        break;
    }
    nostr_event_free(ev);
    return ok;
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
    char *relays[] = { "wss://relay.example", "wss://nos.lol" };
    cfg.relay_urls = relays;
    cfg.relay_count = 2;
    MarmotCreateGroupResult cg;
    memset(&cg, 0, sizeof(cg));
    OK(marmot_create_group(alice.m, alice.pk, (const char **)kp, 3, &cfg, &cg));
    CHECK(cg.welcome_count == 3 && cg.group->epoch == 1,
          "three invitees, one Commit: epoch %llu", (unsigned long long)cg.group->epoch);
    CHECK(rumor_relays_are(cg.welcome_rumor_jsons[0], (const char *const *)relays, 2),
          "a creation's Welcome names the group relays");
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
    /* MIP-02: an Add's Welcome names them too (none before 0.11.0: MDK 0.8
     * refused it, nostrc-7gx7). */
    for (size_t i = 0; i < n_welcomes; i++)
        CHECK(rumor_relays_are(welcomes[i], (const char *const *)relays, 2),
              "an Add's Welcome names the group relays");
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
    OK(marmot_mark_welcomes_sent(t.alice.m, &t.gid, (const uint8_t (*)[32]) out[0].id, 1));
    marmot_unsent_welcomes_free(out, out_n);
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


/* W17b addendum C2: the outbox is append-only; an entry leaves only when
 * its own send is confirmed.  Two merged Adds before the first Welcome is
 * sent keep both; a mark covers only the ids it names. */
static void
test_outbox_keeps_every_welcome_until_marked(void)
{
    Trio t;
    trio_init(&t);
    Member dave, eve, frank;
    member_init(&dave, "Dave");
    member_init(&eve, "Eve");
    member_init(&frank, "Frank");
    Member *joiner[3] = { &dave, &eve, &frank };
    char *commits[3];
    char *rumors[3];
    for (int i = 0; i < 2; i++) {
        char *kp = key_package(joiner[i]);
        const char *kps[] = { kp };
        char **w = NULL;
        size_t n = 0;
        OK(marmot_add_members(t.alice.m, &t.gid, kps, 1, &w, &n, &commits[i]));
        merge(&t.alice, &t.gid);
        rumors[i] = w[0];
        free(w);
        free(kp);
    }
    MarmotUnsentWelcome *out = NULL;
    size_t n = 0;
    OK(marmot_get_unsent_welcomes(t.alice.m, &t.gid, &out, &n));
    CHECK(n == 2 && memcmp(out[0].recipient, dave.pk, 32) == 0 &&
          memcmp(out[1].recipient, eve.pk, 32) == 0, "both Adds' Welcomes kept: %zu", n);
    /* Dave's send is confirmed; Eve's is not. */
    uint8_t dave_id[32], eve_id[32];
    memcpy(dave_id, out[0].id, 32);
    memcpy(eve_id, out[1].id, 32);
    marmot_unsent_welcomes_free(out, n);
    OK(marmot_mark_welcomes_sent(t.alice.m, &t.gid, (const uint8_t (*)[32]) dave_id, 1));
    /* Frank's Add merges between a read and a mark: the mark (Eve's id)
     * must not take Frank's Welcome with it. */
    {
        char *kp = key_package(&frank);
        const char *kps[] = { kp };
        char **w = NULL;
        size_t wn = 0;
        OK(marmot_add_members(t.alice.m, &t.gid, kps, 1, &w, &wn, &commits[2]));
        merge(&t.alice, &t.gid);
        rumors[2] = w[0];
        free(w);
        free(kp);
    }
    OK(marmot_mark_welcomes_sent(t.alice.m, &t.gid, (const uint8_t (*)[32]) eve_id, 1));
    OK(marmot_get_unsent_welcomes(t.alice.m, &t.gid, &out, &n));
    CHECK(n == 1 && memcmp(out[0].recipient, frank.pk, 32) == 0 &&
          strcmp(out[0].rumor_json, rumors[2]) == 0, "only Frank's Welcome remains");
    /* An unknown id changes nothing; Frank's own id empties the outbox. */
    OK(marmot_mark_welcomes_sent(t.alice.m, &t.gid, (const uint8_t (*)[32]) dave_id, 1));
    size_t n2 = 0;
    MarmotUnsentWelcome *again = NULL;
    OK(marmot_get_unsent_welcomes(t.alice.m, &t.gid, &again, &n2));
    CHECK(n2 == 1, "unknown id ignored");
    marmot_unsent_welcomes_free(again, n2);
    OK(marmot_mark_welcomes_sent(t.alice.m, &t.gid, (const uint8_t (*)[32]) out[0].id, 1));
    marmot_unsent_welcomes_free(out, n);
    OK(marmot_get_unsent_welcomes(t.alice.m, &t.gid, &out, &n));
    CHECK(out == NULL && n == 0, "outbox empty");

    /* Every Welcome still joins, and everyone converges. */
    for (int i = 0; i < 3; i++) {
        expect_commit(&t.bob, commits[i], "Bob follows the Adds");
        expect_commit(&t.charlie, commits[i], "Charlie follows the Adds");
    }
    join(&dave, rumors[0]);
    expect_commit(&dave, commits[1], "Dave follows Eve's Add");
    expect_commit(&dave, commits[2], "Dave follows Frank's Add");
    join(&eve, rumors[1]);
    expect_commit(&eve, commits[2], "Eve follows Frank's Add");
    join(&frank, rumors[2]);
    Member *six[] = { &t.alice, &t.bob, &t.charlie, &dave, &eve, &frank };
    expect_converged(six, 6, &t.gid, "Before", t.epoch + 3);
    expect_messages_flow(six, 6, &t.gid);
    for (int i = 0; i < 3; i++) {
        free(commits[i]);
        free(rumors[i]);
        marmot_free(joiner[i]->m);
    }
    trio_clear(&t);
}

/* W17b addendum N1: a second copy of a Welcome (another gift wrap, or a
 * resend) must not roll a joined member back; a re-invite after removal
 * still joins. */
static void
test_duplicate_welcome_does_not_roll_back(void)
{
    Trio t;
    trio_init(&t);
    Member dave;
    member_init(&dave, "Dave");
    char *kp = key_package(&dave);
    const char *kps[] = { kp };
    char **w = NULL;
    size_t n = 0;
    char *add = NULL;
    OK(marmot_add_members(t.alice.m, &t.gid, kps, 1, &w, &n, &add));
    merge(&t.alice, &t.gid);
    expect_commit(&t.bob, add, "Bob");
    expect_commit(&t.charlie, add, "Charlie");
    join(&dave, w[0]);
    char *r = rename_group(&t.alice, &t.gid, "Moved on");
    Member *four[] = { &t.alice, &t.bob, &t.charlie, &dave };
    for (int i = 1; i < 4; i++) expect_commit(four[i], r, "rename");
    expect_converged(four, 4, &t.gid, "Moved on", t.epoch + 2);

    Snapshot before;
    snapshot(&dave, &t.gid, &before);
    uint8_t wrapper[32];
    randombytes_buf(wrapper, sizeof(wrapper));
    MarmotWelcome *copy = NULL;
    MarmotError err = marmot_process_welcome(dave.m, wrapper, w[0], &copy);
    if (err == MARMOT_OK) {
        err = marmot_accept_welcome(dave.m, copy);
        marmot_welcome_free(copy);
    }
    CHECK(err == MARMOT_ERR_WELCOME_ALREADY_ACCEPTED,
          "a duplicate Welcome must be refused: %d", err);
    expect_unchanged(&dave, &t.gid, &before, "duplicate Welcome");
    snapshot_clear(&before);
    expect_converged(four, 4, &t.gid, "Moved on", t.epoch + 2);
    expect_messages_flow(four, 4, &t.gid);

    /* Removed (Dave cannot follow his own removal yet, nostrc-yo95) and
     * invited again -- with the same KeyPackage, so the new Welcome carries
     * his old leaf signature key: being for a later epoch, it joins. */
    char *rm = NULL;
    OK(marmot_remove_members(t.alice.m, &t.gid, (const uint8_t (*)[32]) dave.pk, 1, &rm));
    merge(&t.alice, &t.gid);
    expect_commit(&t.bob, rm, "Bob");
    expect_commit(&t.charlie, rm, "Charlie");
    char *kp2 = strdup(kp);
    const char *kps2[] = { kp2 };
    char **w2 = NULL;
    size_t n2 = 0;
    char *readd = NULL;
    OK(marmot_add_members(t.alice.m, &t.gid, kps2, 1, &w2, &n2, &readd));
    merge(&t.alice, &t.gid);
    expect_commit(&t.bob, readd, "Bob");
    expect_commit(&t.charlie, readd, "Charlie");
    join(&dave, w2[0]);
    expect_converged(four, 4, &t.gid, "Moved on", t.epoch + 4);
    expect_messages_flow(four, 4, &t.gid);

    free(w[0]); free(w); free(w2[0]); free(w2);
    free(kp); free(kp2); free(add); free(r); free(rm); free(readd);
    marmot_free(dave.m);
    trio_clear(&t);
}

/* ── Envelope authentication (nostrc-6r6s) ─────────────────────────────── */

typedef enum {
    TAMPER_FORGED_ID,     /* another well-formed id */
    TAMPER_CONTENT,       /* content changed, id and signature kept */
    TAMPER_BAD_SIG,       /* one signature byte changed */
    TAMPER_FOREIGN_SIG,   /* a valid signature by another key over this id */
    TAMPER_UNSIGNED,      /* no signature (a rumor) */
    TAMPER_NO_ID_NO_SIG,  /* neither id nor signature */
} Tamper;

static void
flip_hex(char *hex)
{
    hex[0] = hex[0] == '0' ? '1' : '0';
}

/* `event_json` changed as `how` says. */
static char *
tampered(const char *event_json, Tamper how)
{
    NostrEvent *ev = nostr_event_new();
    CHECK(ev && nostr_event_deserialize_compact(ev, event_json, NULL), "parse");
    CHECK(ev->pubkey, "the source event has a pubkey");
    CHECK(ev->id || how == TAMPER_NO_ID_NO_SIG, "the source event has an id");
    CHECK(ev->sig || how == TAMPER_FORGED_ID || how == TAMPER_CONTENT ||
          how == TAMPER_UNSIGNED || how == TAMPER_NO_ID_NO_SIG, "the source event is signed");
    switch (how) {
    case TAMPER_FORGED_ID:
        flip_hex(ev->id);
        break;
    case TAMPER_CONTENT: {
        size_t n = strlen(ev->content);
        char *c = malloc(n + 1);
        CHECK(c, "alloc");
        memcpy(c, ev->content, n + 1);
        c[n / 2] = c[n / 2] == 'A' ? 'B' : 'A';
        free(ev->content);
        ev->content = c;
        break;
    }
    case TAMPER_BAD_SIG:
        flip_hex(ev->sig + 70);
        break;
    case TAMPER_FOREIGN_SIG: {
        /* Signed by another ephemeral key, then the original pubkey put
         * back: the id no longer matches, and a recomputed one would not
         * match the signature. */
        char *pk = strdup(ev->pubkey);
        CHECK(pk && marmot_sign_ephemeral(ev) == 0, "re-sign");
        free(ev->pubkey);
        ev->pubkey = pk;
        break;
    }
    case TAMPER_UNSIGNED:
        free(ev->sig);
        ev->sig = NULL;
        break;
    case TAMPER_NO_ID_NO_SIG:
        free(ev->sig);
        free(ev->id);
        ev->sig = NULL;
        ev->id = NULL;
        break;
    }
    char *json = nostr_event_serialize_compact(ev);
    nostr_event_free(ev);
    CHECK(json, "serialize");
    return json;
}

static MarmotMessageResultType
deliver_rumor(Member *x, const char *rumor_json, MarmotError *err)
{
    MarmotMessageResult r;
    memset(&r, 0, sizeof(r));
    *err = marmot_process_rumor_message(x->m, rumor_json, &r);
    MarmotMessageResultType type = r.type;
    marmot_message_result_free(&r);
    return type;
}

static char *
app_message(Member *x, const MarmotGroupId *gid, const char *text)
{
    char inner[160];
    snprintf(inner, sizeof(inner),
             "{\"kind\":9,\"content\":\"%s\",\"created_at\":1700000000,\"tags\":[]}", text);
    MarmotOutgoingMessage out;
    memset(&out, 0, sizeof(out));
    OK(marmot_create_message(x->m, gid, inner, &out));
    char *json = strdup(out.event_json);
    marmot_outgoing_message_free(&out);
    CHECK(json, "strdup");
    return json;
}

/* transports/nostr.md: a relay-delivered kind:445 whose id or signature does
 * not verify is rejected before any decryption, and changes nothing: the
 * genuine event is still processed afterwards (its MLS generation was not
 * consumed, the Commit was not applied or deferred). */
static void
test_unauthenticated_events_rejected(void)
{
    Trio t;
    trio_init(&t);
    static const struct { Tamper how; MarmotError want; const char *what; } cases[] = {
        { TAMPER_FORGED_ID,    MARMOT_ERR_EVENT,     "forged id" },
        { TAMPER_CONTENT,      MARMOT_ERR_EVENT,     "content changed under the id" },
        { TAMPER_BAD_SIG,      MARMOT_ERR_SIGNATURE, "bad signature" },
        { TAMPER_FOREIGN_SIG,  MARMOT_ERR_EVENT,     "signature by another key" },
        { TAMPER_UNSIGNED,     MARMOT_ERR_SIGNATURE, "unsigned" },
        { TAMPER_NO_ID_NO_SIG, MARMOT_ERR_SIGNATURE, "no id, no signature" },
    };

    /* An application message. */
    char *msg = app_message(&t.alice, &t.gid, "authentic");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *bad = tampered(msg, cases[i].how);
        expect_rejected(&t.bob, &t.gid, bad, cases[i].want, cases[i].what);
        free(bad);
    }
    MarmotError err;
    CHECK(deliver(&t.bob, msg, &err, NULL) == MARMOT_RESULT_APPLICATION_MESSAGE &&
          err == MARMOT_OK, "the genuine message after the forgeries: %d", err);
    free(msg);

    /* A Commit: rejected copies neither apply nor defer anything. */
    char *commit = rename_group(&t.alice, &t.gid, "After");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *bad = tampered(commit, cases[i].how);
        expect_rejected(&t.charlie, &t.gid, bad, cases[i].want, cases[i].what);
        free(bad);
    }
    expect_commit(&t.charlie, commit, "the genuine Commit after the forgeries");
    expect_commit(&t.bob, commit, "Bob follows");
    free(commit);
    expect_converged(t.all, 3, &t.gid, "After", t.epoch + 1);
    trio_clear(&t);
}

/* The gift-wrap route: an unsigned kind:445 rumor (its seal authenticated
 * it) is processed by marmot_process_rumor_message(), with or without an
 * id; a declared id must still be canonical; the relay path refuses it. */
static void
test_rumor_path_accepts_unsigned(void)
{
    Trio t;
    trio_init(&t);

    char *msg = app_message(&t.charlie, &t.gid, "wrapped one");
    char *rumor = tampered(msg, TAMPER_UNSIGNED);
    expect_rejected(&t.bob, &t.gid, rumor, MARMOT_ERR_SIGNATURE, "rumor on the relay path");
    char *forged = tampered(rumor, TAMPER_FORGED_ID);
    Snapshot before;
    snapshot(&t.bob, &t.gid, &before);
    MarmotError err;
    deliver_rumor(&t.bob, forged, &err);
    CHECK(err == MARMOT_ERR_EVENT, "rumor with a forged id: %d", err);
    expect_unchanged(&t.bob, &t.gid, &before, "rumor with a forged id");
    snapshot_clear(&before);
    CHECK(deliver_rumor(&t.bob, rumor, &err) == MARMOT_RESULT_APPLICATION_MESSAGE &&
          err == MARMOT_OK, "rumor with id: %d", err);
    /* Processed under its canonical id: the same rumor again is a duplicate. */
    CHECK(deliver_rumor(&t.bob, rumor, &err) == MARMOT_RESULT_OWN_MESSAGE &&
          err == MARMOT_OK, "rumor again: %d", err);
    free(forged);
    free(rumor);
    free(msg);

    msg = app_message(&t.charlie, &t.gid, "wrapped two");
    rumor = tampered(msg, TAMPER_NO_ID_NO_SIG);
    CHECK(deliver_rumor(&t.bob, rumor, &err) == MARMOT_RESULT_APPLICATION_MESSAGE &&
          err == MARMOT_OK, "rumor without id: %d", err);
    free(rumor);
    free(msg);

    /* A Commit through the rumor path is applied too. */
    char *commit = rename_group(&t.bob, &t.gid, "Wrapped");
    rumor = tampered(commit, TAMPER_NO_ID_NO_SIG);
    CHECK(deliver_rumor(&t.charlie, rumor, &err) == MARMOT_RESULT_COMMIT && err == MARMOT_OK,
          "Commit rumor: %d", err);
    expect_commit(&t.alice, commit, "Alice from a relay");
    free(rumor);
    free(commit);
    expect_converged(t.all, 3, &t.gid, "Wrapped", t.epoch + 1);
    trio_clear(&t);
}

/* ── Storage transactions (nostrc-qp24.7) ────────────────────────────────
 *
 * A shim over the memory backend: it offers begin/commit/rollback, aborts if
 * libmarmot writes outside a transaction or nests one, counts them, and can
 * fail the Nth write.  (The memory backend cannot undo anything; real
 * atomicity is tested in Groundhog's GhStoreMarmot crash tests.) */

typedef struct {
    MarmotStorage *inner;      /* the memory backend's struct (vtable + ctx) */
    int  depth;
    int  begins, commits, rollbacks, writes;
    int  fail_write;           /* 1-based index of the write that fails; 0 = none */
    bool fail_commit;          /* the commit fails (the backend undid it all) */
} TxnShim;

static TxnShim g_shim;

static MarmotError
shim_write_gate(void)
{
    CHECK(g_shim.depth == 1, "a storage write outside a transaction (depth %d)", g_shim.depth);
    g_shim.writes++;
    if (g_shim.fail_write && g_shim.writes == g_shim.fail_write) return MARMOT_ERR_STORAGE;
    return MARMOT_OK;
}

#define SHIM_WRITE(name, params, args)                                         \
    static MarmotError shim_##name params                                      \
    {                                                                          \
        MarmotError gate = shim_write_gate();                                  \
        if (gate != MARMOT_OK) return gate;                                    \
        return g_shim.inner->name args;                                        \
    }

SHIM_WRITE(save_group, (void *ctx, const MarmotGroup *g), (ctx, g))
SHIM_WRITE(delete_group, (void *ctx, const MarmotGroupId *gid), (ctx, gid))
SHIM_WRITE(save_message, (void *ctx, const MarmotMessage *msg), (ctx, msg))
SHIM_WRITE(save_processed_message,
           (void *ctx, const uint8_t w[32], const uint8_t *id, int64_t at, uint64_t ep,
            const MarmotGroupId *gid, int st, const char *why),
           (ctx, w, id, at, ep, gid, st, why))
SHIM_WRITE(save_welcome, (void *ctx, const MarmotWelcome *w), (ctx, w))
SHIM_WRITE(save_processed_welcome,
           (void *ctx, const uint8_t w[32], const uint8_t *id, int64_t at, int st,
            const char *why),
           (ctx, w, id, at, st, why))
SHIM_WRITE(save_key_package_info, (void *ctx, const MarmotKeyPackageInfo *i), (ctx, i))
SHIM_WRITE(deactivate_key_packages, (void *ctx, const uint8_t pk[32]), (ctx, pk))
SHIM_WRITE(replace_group_relays,
           (void *ctx, const MarmotGroupId *gid, const char **urls, size_t n),
           (ctx, gid, urls, n))
SHIM_WRITE(save_exporter_secret,
           (void *ctx, const MarmotGroupId *gid, uint64_t ep, const uint8_t sec[32]),
           (ctx, gid, ep, sec))
SHIM_WRITE(delete_exporter_secret, (void *ctx, const MarmotGroupId *gid, uint64_t ep),
           (ctx, gid, ep))
SHIM_WRITE(mls_store,
           (void *ctx, const char *l, const uint8_t *k, size_t kl, const uint8_t *v, size_t vl),
           (ctx, l, k, kl, v, vl))
SHIM_WRITE(mls_delete, (void *ctx, const char *l, const uint8_t *k, size_t kl), (ctx, l, k, kl))

static MarmotError
shim_begin(void *ctx)
{
    (void)ctx;
    CHECK(g_shim.depth == 0, "libmarmot nested a transaction");
    g_shim.depth = 1;
    g_shim.begins++;
    return MARMOT_OK;
}

static MarmotError
shim_commit(void *ctx)
{
    (void)ctx;
    CHECK(g_shim.depth == 1, "commit without begin");
    g_shim.depth = 0;
    if (g_shim.fail_commit) {
        g_shim.rollbacks++;
        return MARMOT_ERR_STORAGE;
    }
    g_shim.commits++;
    return MARMOT_OK;
}

static void
shim_rollback(void *ctx)
{
    (void)ctx;
    CHECK(g_shim.depth == 1, "rollback without begin");
    g_shim.depth = 0;
    g_shim.rollbacks++;
}

/* A Marmot on the shim (one at a time); free with shim_free(). */
static Marmot *
shim_marmot_new(void)
{
    memset(&g_shim, 0, sizeof(g_shim));
    g_shim.inner = marmot_storage_memory_new();
    CHECK(g_shim.inner, "memory storage");
    MarmotStorage *s = malloc(sizeof(*s));
    CHECK(s, "alloc");
    *s = *g_shim.inner;   /* reads go straight to the memory backend */
#define HOOK(name) s->name = shim_##name
    HOOK(save_group); HOOK(delete_group); HOOK(save_message);
    HOOK(save_processed_message); HOOK(save_welcome); HOOK(save_processed_welcome);
    HOOK(save_key_package_info); HOOK(deactivate_key_packages);
    HOOK(replace_group_relays); HOOK(save_exporter_secret);
    HOOK(delete_exporter_secret); HOOK(mls_store); HOOK(mls_delete);
    HOOK(begin); HOOK(commit); HOOK(rollback);
#undef HOOK
    Marmot *m = marmot_new(s);
    CHECK(m, "marmot_new on the shim");
    return m;
}

static void
shim_free(Marmot *m)
{
    marmot_free(m);            /* destroys the memory ctx and frees the shim struct */
    free(g_shim.inner);        /* the memory backend's own struct */
    memset(&g_shim, 0, sizeof(g_shim));
}

static void
shim_reset_counts(void)
{
    g_shim.begins = g_shim.commits = g_shim.rollbacks = g_shim.writes = 0;
}

static void
expect_txns(int begins, int commits, int rollbacks, const char *what)
{
    CHECK(g_shim.depth == 0, "%s: transaction left open", what);
    CHECK(g_shim.begins == begins && g_shim.commits == commits &&
          g_shim.rollbacks == rollbacks,
          "%s: begin/commit/rollback %d/%d/%d, want %d/%d/%d", what, g_shim.begins,
          g_shim.commits, g_shim.rollbacks, begins, commits, rollbacks);
    shim_reset_counts();
}

/* Every write of every operation runs inside exactly one transaction; an
 * error rolls it back unless its writes are the outcome (a superseded
 * pending Commit dropped); a failed write rolls back; a storage offering
 * only some hooks is refused. */
static void
transaction_case(bool alice_wins)
{
    /* Alice's instance runs on the shim; her keys come from member_init. */
    Member alice, bob;
    member_init(&alice, "Alice");
    marmot_free(alice.m);
    alice.m = shim_marmot_new();
    OK(test_enroll(alice.m, alice.pk, alice.sk));
    member_init(&bob, "Bob");
    member_rekey_order(&bob, &alice, alice_wins);   /* Bob above: Alice wins */

    /* create_group: one transaction, committed. */
    char *bob_kp = key_package(&bob);
    const char *kps[] = { bob_kp };
    uint8_t admins[2][32];
    memcpy(admins[0], alice.pk, 32);
    memcpy(admins[1], bob.pk, 32);
    MarmotGroupConfig cfg = {0};
    cfg.name = "Txn";
    cfg.admin_pubkeys = admins;
    cfg.admin_count = 2;
    char *relays[] = { "wss://relay.example" };
    cfg.relay_urls = relays;
    cfg.relay_count = 1;
    MarmotCreateGroupResult cg;
    memset(&cg, 0, sizeof(cg));
    OK(marmot_create_group(alice.m, alice.pk, kps, 1, &cfg, &cg));
    free(bob_kp);
    CHECK(g_shim.writes >= 4, "create_group wrote %d records", g_shim.writes);
    expect_txns(1, 1, 0, "create_group");
    join(&bob, cg.welcome_rumor_jsons[0]);
    MarmotGroupId gid = marmot_group_id_new(cg.group->mls_group_id.data,
                                            cg.group->mls_group_id.len);
    marmot_create_group_result_free(&cg);

    /* A pending Commit, merged: two operations, two transactions. */
    char *c1 = rename_pending(&alice, &gid, "One");
    expect_txns(1, 1, 0, "update_group_metadata");
    merge(&alice, &gid);
    expect_txns(1, 1, 0, "merge_pending_commit");
    expect_commit(&bob, c1, "Bob");

    /* Messages both ways (the receiving side's ratchet, marker and message). */
    char *to_alice = app_message(&bob, &gid, "to Alice");
    MarmotError err;
    CHECK(deliver(&alice, to_alice, &err, NULL) == MARMOT_RESULT_APPLICATION_MESSAGE &&
          err == MARMOT_OK, "Alice reads Bob: %d", err);
    expect_txns(1, 1, 0, "process_message (application)");
    free(app_message(&alice, &gid, "to Bob"));
    expect_txns(1, 1, 0, "create_message");

    /* A rejected event rolls back (nothing of it may stay). */
    char *forged = tampered(to_alice, TAMPER_FORGED_ID);
    deliver(&alice, forged, &err, NULL);
    CHECK(err == MARMOT_ERR_EVENT, "forged: %d", err);
    expect_txns(1, 0, 1, "rejected event");
    free(forged);
    free(to_alice);

    /* A failed write rolls the whole operation back: the Nth write of a
     * merge (exporter secret, parent, state, group record, ...). */
    char *c2 = rename_pending(&alice, &gid, "Two");
    shim_reset_counts();
    for (int n = 1; n <= 4; n++) {
        g_shim.fail_write = n;
        g_shim.writes = 0;
        CHECK(marmot_merge_pending_commit(alice.m, &gid) != MARMOT_OK,
              "merge with write %d failing", n);
        CHECK(g_shim.rollbacks == 1 && g_shim.commits == 0,
              "write %d failed: rolled back (%d/%d)", n, g_shim.commits, g_shim.rollbacks);
        shim_reset_counts();
    }
    g_shim.fail_write = 0;

    /* Keep: Bob (also an admin) renames while Alice's "Two" is pending.
     * Both are privileged, so the lower account key wins.  Either Bob's
     * Commit loses and is deferred (MARMOT_ERR_OWN_COMMIT_PENDING, the
     * deferral kept), or it wins and Alice's pending Commit is superseded:
     * her merge drops it (MARMOT_ERR_WRONG_EPOCH, the drop kept). */
    char *cb = rename_pending(&bob, &gid, "Bob's");
    MarmotMessageResultType t = deliver(&alice, cb, &err, NULL);
    if (alice_wins) {
        CHECK(err == MARMOT_ERR_OWN_COMMIT_PENDING, "Bob's loser deferred: %d", err);
        expect_txns(1, 1, 0, "deferred inbound Commit (kept)");
        OK(marmot_clear_pending_commit(alice.m, &gid));   /* re-processes Bob's */
        expect_txns(1, 1, 0, "clear_pending_commit");
    } else {
        CHECK(err == MARMOT_OK && t == MARMOT_RESULT_COMMIT, "Bob's winner applied: %d", err);
        expect_txns(1, 1, 0, "inbound Commit");
        CHECK(marmot_merge_pending_commit(alice.m, &gid) == MARMOT_ERR_WRONG_EPOCH,
              "superseded merge");
        expect_txns(1, 1, 0, "superseded pending Commit dropped (kept)");
    }
    char *ev = NULL;
    OK(marmot_get_pending_commit(alice.m, &gid, &ev, NULL));
    CHECK(ev == NULL, "nothing pending any more");
    expect_txns(1, 1, 0, "get_pending_commit");
    merge(&bob, &gid);
    free(cb);
    free(c2);

    /* A partial set of hooks is refused (fail closed). */
    MarmotStorage *half = marmot_storage_memory_new();
    half->begin = shim_begin;
    CHECK(marmot_new(half) == NULL, "a storage with begin but no commit is refused");
    marmot_storage_free(half);

    free(c1);
    marmot_group_id_free(&gid);
    marmot_free(bob.m);
    shim_free(alice.m);
}

static void
test_operations_run_in_one_transaction(void)
{
    transaction_case(true);    /* the losing inbound Commit is deferred (kept) */
    transaction_case(false);   /* the superseded pending Commit is dropped (kept) */
}

/* A crash between advancing the sender ratchet and sending (nostrc-ai04):
 * marmot_create_message() stores the step in the operation's transaction
 * and returns the event only once that committed.  A failed ratchet write
 * or commit returns no event -- nothing can be published under a step that
 * may not be stored -- and every event it does return used a generation no
 * other event used.  (Durability across a real crash: Groundhog's
 * GhStoreMarmot crash suite.) */
static void
test_send_stores_step_before_event(void)
{
    Member alice, bob;
    member_init(&alice, "Alice");
    marmot_free(alice.m);
    alice.m = shim_marmot_new();
    OK(test_enroll(alice.m, alice.pk, alice.sk));
    member_init(&bob, "Bob");
    char *bob_kp = key_package(&bob);
    const char *kps[] = { bob_kp };
    MarmotGroupConfig cfg = {0};
    cfg.name = "Crash";
    cfg.admin_pubkeys = (uint8_t (*)[32])alice.pk;
    cfg.admin_count = 1;
    MarmotCreateGroupResult cg;
    memset(&cg, 0, sizeof(cg));
    OK(marmot_create_group(alice.m, alice.pk, kps, 1, &cfg, &cg));
    free(bob_kp);
    join(&bob, cg.welcome_rumor_jsons[0]);
    MarmotGroupId gid = marmot_group_id_new(cg.group->mls_group_id.data,
                                            cg.group->mls_group_id.len);
    marmot_create_group_result_free(&cg);

    char *sent[2];
    sent[0] = app_message(&alice, &gid, "before the failures");
    MlsSenderData first = sent_sender_data(&alice, &gid, sent[0]);

    /* The ratchet write fails, then the commit: no event either time. */
    for (int round = 0; round < 2; round++) {
        shim_reset_counts();
        g_shim.fail_write = round == 0 ? 1 : 0;
        g_shim.fail_commit = round == 1;
        MarmotOutgoingMessage out;
        memset(&out, 0, sizeof(out));
        MarmotError err = marmot_create_message(
            alice.m, &gid,
            "{\"kind\":9,\"content\":\"lost\",\"created_at\":1700000000,\"tags\":[]}",
            &out);
        CHECK(err != MARMOT_OK, "round %d: the send reported success", round);
        CHECK(out.event_json == NULL && out.message == NULL,
              "round %d: an event was handed out without its stored step", round);
        CHECK(g_shim.rollbacks == 1 && g_shim.commits == 0, "round %d: rolled back", round);
    }
    g_shim.fail_write = 0;
    g_shim.fail_commit = false;

    sent[1] = app_message(&alice, &gid, "after the failures");
    MlsSenderData next = sent_sender_data(&alice, &gid, sent[1]);
    CHECK(next.generation > first.generation,
          "generation %u after %u", next.generation, first.generation);
    MarmotError err;
    for (int i = 0; i < 2; i++) {
        CHECK(deliver(&bob, sent[i], &err, NULL) == MARMOT_RESULT_APPLICATION_MESSAGE &&
              err == MARMOT_OK, "Bob reads send %d: %d", i, err);
        char *again = republish(sent[i]);
        deliver(&bob, again, &err, NULL);
        CHECK(err == MARMOT_ERR_MLS, "replay of send %d: %d", i, err);
        free(again);
        free(sent[i]);
    }
    marmot_group_id_free(&gid);
    marmot_free(bob.m);
    shim_free(alice.m);
}

/* The epoch check runs in one transaction, and EPOCH_CHANGED is an answer,
 * not a failure: a repair it made is committed, not rolled back. */
static void
test_media_check_epoch_transaction(void)
{
    Marmot *m = shim_marmot_new();
    MarmotGroupId gid = marmot_group_id_new((const uint8_t *)"epoch-txn", 9);
    MarmotGroup *g = marmot_group_new();
    g->mls_group_id = marmot_group_id_new(gid.data, gid.len);
    g->epoch = 3;
    OK(g_shim.inner->save_group(g_shim.inner->ctx, g));   /* outside the shim's gate */
    marmot_group_free(g);
    shim_reset_counts();
    OK(marmot_media_check_epoch(m, &gid, 3));
    expect_txns(1, 1, 0, "check, same epoch");
    shim_reset_counts();
    CHECK(marmot_media_check_epoch(m, &gid, 2) == MARMOT_ERR_MEDIA_EPOCH_CHANGED, "older epoch");
    expect_txns(1, 1, 0, "check, epoch changed: committed");
    marmot_group_id_free(&gid);
    shim_free(m);
}

/* ── Late messages (nostrc-qp24.7) ────────────────────────────────────── */

/* Messages Bob sent in epoch E reach Charlie after Charlie applied the Commit
 * to E+1: they are read with the retained parent state, out of order, and
 * the same event again is a duplicate.  A message two epochs back is past
 * the one-epoch rewind horizon; its rejection changes nothing. */
static void
test_late_messages_use_retained_parent(void)
{
    Trio t;
    trio_init(&t);
    char *late1 = app_message(&t.bob, &t.gid, "late one");
    char *late2 = app_message(&t.bob, &t.gid, "late two");
    char *late3 = app_message(&t.bob, &t.gid, "late three");
    char *commit = rename_group(&t.alice, &t.gid, "Moved");
    expect_commit(&t.charlie, commit, "Charlie moves to E+1");

    MarmotError err;
    CHECK(deliver(&t.charlie, late2, &err, NULL) == MARMOT_RESULT_APPLICATION_MESSAGE &&
          err == MARMOT_OK, "late message (out of order) at E+1: %d", err);
    CHECK(deliver(&t.charlie, late1, &err, NULL) == MARMOT_RESULT_APPLICATION_MESSAGE &&
          err == MARMOT_OK, "earlier late message at E+1: %d", err);
    CHECK(deliver(&t.charlie, late2, &err, NULL) == MARMOT_RESULT_OWN_MESSAGE &&
          err == MARMOT_OK, "the same late message again is a duplicate: %d", err);
    /* A replay in a new envelope (new event id, so no processed marker) is
     * refused by the retained parent's persisted ratchet (nostrc-ai04): its
     * generation was consumed and its key deleted. */
    char *late2_again = republish(late2), *late1_again = republish(late1);
    expect_rejected(&t.charlie, &t.gid, late2_again, MARMOT_ERR_MLS,
                    "late message replayed in a new envelope");
    expect_rejected(&t.charlie, &t.gid, late1_again, MARMOT_ERR_MLS,
                    "earlier late message replayed in a new envelope");
    free(late2_again);
    free(late1_again);

    /* The live epoch is untouched: everyone still talks at E+1. */
    expect_commit(&t.bob, commit, "Bob follows");
    expect_converged(t.all, 3, &t.gid, "Moved", t.epoch + 1);
    expect_messages_flow(t.all, 3, &t.gid);

    /* E+2: the E message is past the horizon. */
    char *commit2 = rename_group(&t.alice, &t.gid, "Moved again");
    expect_commit(&t.charlie, commit2, "Charlie moves to E+2");
    expect_rejected(&t.charlie, &t.gid, late3, MARMOT_ERR_MLS, "two epochs late");
    expect_commit(&t.bob, commit2, "Bob follows again");
    free(commit2);
    free(commit);
    free(late1);
    free(late2);
    free(late3);
    trio_clear(&t);
}

/* Encrypted media v2 (nostrc-u7cb): the source epoch is the epoch of the
 * carrying message, which process_message reports.  Bob attaches media in E;
 * Charlie, already at E+1, reads the late message with the retained parent
 * and decrypts the attachment with the retained E media secret -- not with
 * the epoch it is in now. */
static void
test_media_source_epoch_of_late_message(void)
{
    Trio t;
    trio_init(&t);
    static const uint8_t file[] = "a picture sent in epoch E";
    MarmotMediaUpload up;
    OK(marmot_media_encrypt(t.bob.m, &t.gid, file, sizeof file - 1, "image/png", "e.png", &up));
    CHECK(up.source_epoch == t.epoch, "media epoch %" PRIu64 " != %" PRIu64,
          up.source_epoch, t.epoch);
    OK(marmot_media_reference_add_locator(&up.reference, MARMOT_MEDIA_LOCATOR_BLOSSOM_V1,
                                          "https://blossom.example/blob"));
    char *late = app_message(&t.bob, &t.gid, "see attachment");
    char *commit = rename_group(&t.alice, &t.gid, "Moved");
    expect_commit(&t.charlie, commit, "Charlie moves to E+1");

    MarmotMessageResult r;
    memset(&r, 0, sizeof r);
    OK(marmot_process_message(t.charlie.m, late, &r));
    CHECK(r.type == MARMOT_RESULT_APPLICATION_MESSAGE, "late message type %d", r.type);
    CHECK(r.app_msg.epoch == t.epoch, "late message epoch %" PRIu64 ", sent in %" PRIu64,
          r.app_msg.epoch, t.epoch);
    uint8_t *pt = NULL;
    size_t pt_len = 0;
    OK(marmot_media_decrypt(t.charlie.m, &t.gid, r.app_msg.epoch, &up.reference,
                            up.ciphertext, up.ciphertext_len, &pt, &pt_len));
    CHECK(pt_len == sizeof file - 1 && memcmp(pt, file, pt_len) == 0, "media plaintext");
    free(pt);
    CHECK(marmot_media_decrypt(t.charlie.m, &t.gid, t.epoch + 1, &up.reference,
                               up.ciphertext, up.ciphertext_len, &pt, &pt_len) ==
              MARMOT_ERR_MEDIA_DECRYPT,
          "the current epoch's media secret must not open E media");
    marmot_message_result_free(&r);

    /* A message of the current epoch reports it. */
    expect_commit(&t.bob, commit, "Bob follows");
    char *now = app_message(&t.bob, &t.gid, "now");
    memset(&r, 0, sizeof r);
    OK(marmot_process_message(t.charlie.m, now, &r));
    CHECK(r.type == MARMOT_RESULT_APPLICATION_MESSAGE && r.app_msg.epoch == t.epoch + 1,
          "current message epoch %" PRIu64, r.app_msg.epoch);
    marmot_message_result_free(&r);
    free(now);
    free(late);
    free(commit);
    marmot_media_upload_clear(&up);
    trio_clear(&t);
}

/* Review L3: the epoch check reconciles first.  A crash between storing
 * Charlie's MLS state at E+1 and his group record leaves the record at E;
 * media sealed for E must still be refused, because marmot_create_message()
 * would reconcile and send in E+1. */
static void
test_media_check_epoch_reconciles(void)
{
    Trio t;
    trio_init(&t);
    char *commit = rename_group(&t.alice, &t.gid, "Moved");
    expect_commit(&t.charlie, commit, "Charlie moves to E+1");
    OK(marmot_media_check_epoch(t.charlie.m, &t.gid, t.epoch + 1));

    MarmotGroup *g = NULL;
    OK(marmot_get_group(t.charlie.m, &t.gid, &g));
    g->epoch = t.epoch;   /* the record an interrupted transition leaves */
    OK(t.charlie.m->storage->save_group(t.charlie.m->storage->ctx, g));
    marmot_group_free(g);

    CHECK(marmot_media_check_epoch(t.charlie.m, &t.gid, t.epoch) ==
              MARMOT_ERR_MEDIA_EPOCH_CHANGED,
          "a stale record must not pass media sealed for the old epoch");
    g = NULL;
    OK(marmot_get_group(t.charlie.m, &t.gid, &g));
    CHECK(g->epoch == t.epoch + 1, "the check kept its repair: %" PRIu64, g->epoch);
    marmot_group_free(g);
    OK(marmot_media_check_epoch(t.charlie.m, &t.gid, t.epoch + 1));
    expect_commit(&t.bob, commit, "Bob follows");
    free(commit);
    trio_clear(&t);
}

/* ── Secret-tree ratchet persistence (nostrc-ai04) ────────────────────── */

/* ── Sender authentication (nostrc-we6g) ──────────────────────────────────────── */

/* A kind:445 carrying `inner`, built from the MLS state `g` the way a
 * malicious member's own client would (libmarmot's API refuses to): MLS
 * PrivateMessage, NIP-44 under the epoch's exporter secret, `h` tag, fresh
 * ephemeral signature. */
static char *
forge_app_event(MlsGroup *g, const uint8_t nostr_gid[32], const char *inner)
{
    uint8_t *ct = NULL;
    size_t len = 0;
    CHECK(mls_group_encrypt(g, (const uint8_t *)inner, strlen(inner), &ct, &len) == 0,
          "MLS layer");
    char *content = NULL;
    CHECK(marmot_group_event_encrypt(g->epoch_secrets.exporter_secret, ct, len,
                                     &content) == 0, "NIP-44 layer");
    free(ct);
    NostrEvent *ev = nostr_event_new();
    CHECK(ev, "event");
    nostr_event_set_kind(ev, MARMOT_KIND_GROUP_MESSAGE);
    nostr_event_set_content(ev, content);
    nostr_event_set_created_at(ev, 1700000000);
    free(content);
    char *gid_hex = marmot_hex_encode(nostr_gid, 32);
    NostrTags *tags = nostr_tags_new(0);
    CHECK(gid_hex && tags, "tags");
    nostr_tags_append(tags, nostr_tag_new("h", gid_hex, NULL));
    free(gid_hex);
    nostr_event_set_tags(ev, tags);
    CHECK(marmot_sign_ephemeral(ev) == 0, "sign");
    char *json = nostr_event_serialize_compact(ev);
    nostr_event_free(ev);
    CHECK(json, "serialize");
    return json;
}

static void
expect_rumor_rejected(Member *x, const MarmotGroupId *gid, const char *event_json,
                      MarmotError want, const char *what)
{
    char *rumor = tampered(event_json, TAMPER_UNSIGNED);
    Snapshot before;
    snapshot(x, gid, &before);
    MarmotError err;
    deliver_rumor(x, rumor, &err);
    CHECK(err == want, "%s (rumor path): %s got %d (%s), want %d", what, x->name, err,
          marmot_error_string(err), want);
    expect_unchanged(x, gid, &before, what);
    snapshot_clear(&before);
    free(rumor);
}

/* The reviewer's repro (libmarmot-w19-review C1): Bob posts an inner event
 * with Alice's pubkey.  The API refuses to build it; a message a modified
 * client builds anyway is dropped by every receiver, on the relay and the
 * rumor path, live and through the retained parent, and changes nothing.
 * Bob using Alice's leaf (he can derive its keys: the secret tree is shared
 * by the group) fails on the PrivateMessageContent signature. */
static void
test_member_cannot_post_as_another(void)
{
    Trio t;
    trio_init(&t);
    char *alice_hex = marmot_hex_encode(t.alice.pk, 32);
    char *bob_hex = marmot_hex_encode(t.bob.pk, 32);
    CHECK(alice_hex && bob_hex, "hex");
    char as_alice[256], no_author[160];
    snprintf(as_alice, sizeof(as_alice),
             "{\"pubkey\":\"%s\",\"kind\":9,\"content\":\"I am Alice\","
             "\"created_at\":1700000000,\"tags\":[]}", alice_hex);
    snprintf(no_author, sizeof(no_author),
             "{\"kind\":9,\"content\":\"nobody\",\"created_at\":1700000000,\"tags\":[]}");

    /* 1. The API does not sign for another account. */
    MarmotOutgoingMessage out;
    memset(&out, 0, sizeof(out));
    MarmotError err = marmot_create_message(t.bob.m, &t.gid, as_alice, &out);
    CHECK(err == MARMOT_ERR_AUTHOR_MISMATCH && out.event_json == NULL,
          "create_message as Alice: %d", err);

    /* 2. A modified client: Bob's leaf, Alice's pubkey. */
    MlsGroup bob;
    load_mls(&t.bob, &t.gid, &bob);
    char *forged = forge_app_event(&bob, t.nostr_gid, as_alice);
    expect_rejected(&t.charlie, &t.gid, forged, MARMOT_ERR_AUTHOR_MISMATCH, "Alice's pubkey");
    expect_rumor_rejected(&t.charlie, &t.gid, forged, MARMOT_ERR_AUTHOR_MISMATCH,
                          "Alice's pubkey");
    expect_rejected(&t.alice, &t.gid, forged, MARMOT_ERR_AUTHOR_MISMATCH,
                    "Alice's pubkey, to Alice");
    mls_group_free(&bob);

    /* 3. No author at all. */
    load_mls(&t.bob, &t.gid, &bob);
    char *anonymous = forge_app_event(&bob, t.nostr_gid, no_author);
    expect_rejected(&t.charlie, &t.gid, anonymous, MARMOT_ERR_AUTHOR_MISMATCH, "no pubkey");
    expect_rumor_rejected(&t.charlie, &t.gid, anonymous, MARMOT_ERR_AUTHOR_MISMATCH,
                          "no pubkey");
    mls_group_free(&bob);

    /* 4. Alice's leaf and pubkey, Bob's signature key. */
    MlsGroup as_leaf;
    load_mls(&t.bob, &t.gid, &as_leaf);
    MlsGroup alice_state;
    load_mls(&t.alice, &t.gid, &alice_state);
    as_leaf.own_leaf_index = alice_state.own_leaf_index;
    mls_group_free(&alice_state);
    char *leaf_forged = forge_app_event(&as_leaf, t.nostr_gid, as_alice);
    expect_rejected(&t.charlie, &t.gid, leaf_forged, MARMOT_ERR_MLS, "Alice's leaf");
    expect_rumor_rejected(&t.charlie, &t.gid, leaf_forged, MARMOT_ERR_MLS, "Alice's leaf");
    mls_group_free(&as_leaf);

    /* The genuine messages still flow, each under its own author. */
    char *from_bob = app_message(&t.bob, &t.gid, "really Bob");
    MarmotMessageResult r;
    memset(&r, 0, sizeof(r));
    OK(marmot_process_message(t.charlie.m, from_bob, &r));
    CHECK(r.type == MARMOT_RESULT_APPLICATION_MESSAGE && r.app_msg.sender_pubkey_hex &&
          strcmp(r.app_msg.sender_pubkey_hex, bob_hex) == 0, "Bob's message is Bob's");
    marmot_message_result_free(&r);
    char *from_alice = app_message(&t.alice, &t.gid, "really Alice");
    memset(&r, 0, sizeof(r));
    OK(marmot_process_message(t.charlie.m, from_alice, &r));
    CHECK(r.type == MARMOT_RESULT_APPLICATION_MESSAGE && r.app_msg.sender_pubkey_hex &&
          strcmp(r.app_msg.sender_pubkey_hex, alice_hex) == 0, "Alice's message is Alice's");
    marmot_message_result_free(&r);

    /* 5. Through the retained parent: a forgery of epoch E read at E+1. */
    load_mls(&t.bob, &t.gid, &bob);
    char *late_forged = forge_app_event(&bob, t.nostr_gid, as_alice);
    mls_group_free(&bob);
    char *commit = rename_group(&t.alice, &t.gid, "Moved");
    expect_commit(&t.charlie, commit, "Charlie moves on");
    expect_rejected(&t.charlie, &t.gid, late_forged, MARMOT_ERR_AUTHOR_MISMATCH,
                    "late, Alice's pubkey");
    expect_rumor_rejected(&t.charlie, &t.gid, late_forged, MARMOT_ERR_AUTHOR_MISMATCH,
                          "late, Alice's pubkey");
    expect_commit(&t.bob, commit, "Bob follows");

    free(commit);
    free(late_forged);
    free(from_alice);
    free(from_bob);
    free(leaf_forged);
    free(anonymous);
    free(forged);
    free(alice_hex);
    free(bob_hex);
    trio_clear(&t);
}

/* An inner event delivered once is not delivered again in another
 * envelope, even when the MLS layer would read it (a sender's re-send, or
 * a replay a state migration let through): it is a duplicate by its NIP-01
 * id (review N3). */
static void
test_inner_event_delivered_once(void)
{
    Trio t;
    trio_init(&t);
    const char *inner =
        "{\"kind\":9,\"content\":\"only once\",\"created_at\":1700000000,\"tags\":[]}";
    MarmotOutgoingMessage a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    OK(marmot_create_message(t.bob.m, &t.gid, inner, &a));
    OK(marmot_create_message(t.bob.m, &t.gid, inner, &b));
    CHECK(sent_sender_data(&t.bob, &t.gid, a.event_json).generation !=
          sent_sender_data(&t.bob, &t.gid, b.event_json).generation, "two generations");
    MarmotError err;
    CHECK(deliver(&t.charlie, a.event_json, &err, NULL) == MARMOT_RESULT_APPLICATION_MESSAGE &&
          err == MARMOT_OK, "first: %d", err);
    CHECK(deliver(&t.charlie, b.event_json, &err, NULL) == MARMOT_RESULT_OWN_MESSAGE &&
          err == MARMOT_OK, "the same inner event again: %d", err);
    /* Its generation is used up: a replay of that envelope is refused. */
    char *again = republish(b.event_json);
    expect_rejected(&t.charlie, &t.gid, again, MARMOT_ERR_MLS, "replay of the duplicate");
    free(again);
    marmot_outgoing_message_free(&a);
    marmot_outgoing_message_free(&b);
    trio_clear(&t);
}

/* On a storage without transaction hooks (the memory backend), a received
 * message that cannot be stored does not keep its ratchet step: the step is
 * written back, so the same event is read again later instead of being lost
 * with a consumed key -- live and through the retained parent.  Once it is
 * stored, a replay fails (nostrc-ai04). */
static void
test_failed_receive_keeps_the_message(void)
{
    Trio t;
    trio_init(&t);
    char *live = app_message(&t.bob, &t.gid, "live");
    char *late = app_message(&t.bob, &t.gid, "late");
    MarmotError err;

    for (int round = 0; round < 2; round++) {
        const char *ev = round == 0 ? live : late;
        if (round == 1) {
            char *commit = rename_group(&t.alice, &t.gid, "Moved");
            expect_commit(&t.charlie, commit, "Charlie moves on");
            expect_commit(&t.bob, commit, "Bob follows");
            free(commit);
        }
        Snapshot before;
        snapshot(&t.charlie, &t.gid, &before);
        faults_arm(&t.charlie);
        g_faults.fail_save_message = true;
        deliver(&t.charlie, ev, &err, NULL);
        faults_disarm(&t.charlie);
        CHECK(err == MARMOT_ERR_STORAGE_CONSTRAINT, "round %d: %d", round, err);
        expect_unchanged(&t.charlie, &t.gid, &before, round ? "late, not stored" : "not stored");
        snapshot_clear(&before);
        CHECK(deliver(&t.charlie, ev, &err, NULL) == MARMOT_RESULT_APPLICATION_MESSAGE &&
              err == MARMOT_OK, "round %d: read again: %d", round, err);
        char *again = republish(ev);
        expect_rejected(&t.charlie, &t.gid, again, MARMOT_ERR_MLS, "then a replay fails");
        free(again);
    }
    free(live);
    free(late);
    trio_clear(&t);
}

/* A message re-published in a new envelope (new id, so the processed marker
 * does not catch it) must not decrypt again in the live epoch: its
 * generation was consumed and its key deleted (RFC 9420 §9.2). */
static void
test_live_replay_in_new_envelope_rejected(void)
{
    Trio t;
    trio_init(&t);
    char *m1 = app_message(&t.bob, &t.gid, "once");
    MarmotError err;
    CHECK(deliver(&t.charlie, m1, &err, NULL) == MARMOT_RESULT_APPLICATION_MESSAGE &&
          err == MARMOT_OK, "Charlie reads it: %d", err);
    char *again = republish(m1);
    expect_rejected(&t.charlie, &t.gid, again, MARMOT_ERR_MLS,
                    "live-epoch replay in a new envelope");
    /* A fresh process (a restart) is no different. */
    MarmotStorage *s = t.charlie.m->storage;
    t.charlie.m->storage = NULL;
    marmot_free(t.charlie.m);
    t.charlie.m = marmot_new(s);
    CHECK(t.charlie.m, "restart");
    expect_rejected(&t.charlie, &t.gid, again, MARMOT_ERR_MLS,
                    "live-epoch replay after a restart");
    /* The genuine flow goes on. */
    char *m2 = app_message(&t.bob, &t.gid, "twice");
    CHECK(deliver(&t.charlie, m2, &err, NULL) == MARMOT_RESULT_APPLICATION_MESSAGE &&
          err == MARMOT_OK, "Charlie reads the next one: %d", err);
    free(m2);
    free(again);
    free(m1);
    trio_clear(&t);
}

/* marmot_create_message() loads the stored state on every call: each send
 * in an epoch must still use a new generation (a new AES-GCM key and nonce),
 * never generation 0 again (nostrc-ai04: the secret tree was re-derived from
 * encryption_secret on every load). */
static void
test_sends_use_distinct_generations(void)
{
    Trio t;
    trio_init(&t);
    enum { N = 5 };
    char *sent[N];
    MlsSenderData sd[N];
    for (int i = 0; i < N; i++) {
        char text[32];
        snprintf(text, sizeof(text), "send %d", i);
        sent[i] = app_message(&t.bob, &t.gid, text);
        sd[i] = sent_sender_data(&t.bob, &t.gid, sent[i]);
    }
    printf("\n    Bob's sends in epoch %llu: leaf/generation",
           (unsigned long long)t.epoch);
    for (int i = 0; i < N; i++) printf(" %u/%u", sd[i].leaf_index, sd[i].generation);
    printf("\n  %-58s", "");
    for (int i = 0; i < N; i++) {
        CHECK(sd[i].leaf_index == sd[0].leaf_index, "one sender");
        for (int j = 0; j < i; j++)
            CHECK(sd[i].generation != sd[j].generation,
                  "sends %d and %d in epoch %llu both used generation %u: the same "
                  "AES-GCM key, nonces differing only in the reuse guard",
                  j, i, (unsigned long long)t.epoch, sd[i].generation);
    }
    /* Everyone still reads them, in order. */
    for (int i = 0; i < N; i++) {
        MarmotError err;
        CHECK(deliver(&t.charlie, sent[i], &err, NULL) == MARMOT_RESULT_APPLICATION_MESSAGE &&
              err == MARMOT_OK, "Charlie reads send %d: %d", i, err);
        free(sent[i]);
    }
    trio_clear(&t);
}

/* ── Member identity binding (nostrc-7vyi) ─────────────────────────────── */

/* credentials.c (internal) */
extern MarmotError marmot_parse_key_package_event(const char *event_json, MlsKeyPackage *kp_out,
                                                  uint8_t nostr_pubkey_out[32]);
extern MarmotError marmot_validate_key_package_event(NostrEvent *event, MlsKeyPackage *kp_out,
                                                     uint8_t nostr_pubkey_out[32]);

static void
groups_free(MarmotGroup **groups, size_t n)
{
    for (size_t i = 0; i < n; i++) marmot_group_free(groups[i]);
    free(groups);
}

typedef enum {
    LEAF_NO_PROOF,        /* the shape of every leaf before 0.10.0 */
    LEAF_PROOF_BY_OTHER,  /* a proof the forger signed with its own account */
    LEAF_REPLAYED_PROOF,  /* the victim's genuine proof, over another key */
    LEAF_GENUINE,         /* the account's own proof over this leaf's key */
} LeafProof;

/* A KeyPackage whose leaf claims `identity`, with a fresh MLS signature key
 * (the forger's), carrying the proof `mode` says.  `owner_sk` is the
 * identity's secret key (LEAF_GENUINE, LEAF_REPLAYED_PROOF); `forger` signs
 * LEAF_PROOF_BY_OTHER. */
static void
leaf_key_package(const uint8_t identity[32], const uint8_t *owner_sk, const Member *forger,
                 LeafProof mode, MlsKeyPackage *kp, MlsKeyPackagePrivate *priv)
{
    CHECK(mls_key_package_create_unsigned(kp, priv, identity, 32, NULL, 0) == 0, "KeyPackage");
    uint8_t proof[MARMOT_ACCOUNT_PROOF_LEN];
    uint64_t now = (uint64_t)time(NULL);
    switch (mode) {
    case LEAF_NO_PROOF:
        break;
    case LEAF_PROOF_BY_OTHER:
        CHECK(marmot_account_proof_create(forger->pk, forger->sk, NULL, NULL, MARMOT_CIPHERSUITE,
                                          MARMOT_SIGNATURE_SCHEME_ED25519,
                                          kp->leaf_node.signature_key, MLS_SIG_PK_LEN, now,
                                          proof) == MARMOT_OK, "forger's proof");
        CHECK(marmot_leaf_set_proof(&kp->leaf_node, proof) == MARMOT_OK, "set proof");
        break;
    case LEAF_REPLAYED_PROOF: {
        /* The victim's real proof, as anyone can read it from a KeyPackage
         * the victim published: it signs the victim's own leaf key. */
        uint8_t victim_key[MLS_SIG_PK_LEN];
        randombytes_buf(victim_key, sizeof(victim_key));
        CHECK(marmot_account_proof_create(identity, owner_sk, NULL, NULL, MARMOT_CIPHERSUITE,
                                          MARMOT_SIGNATURE_SCHEME_ED25519, victim_key,
                                          MLS_SIG_PK_LEN, now, proof) == MARMOT_OK,
              "victim's proof");
        CHECK(marmot_leaf_set_proof(&kp->leaf_node, proof) == MARMOT_OK, "set proof");
        break;
    }
    case LEAF_GENUINE:
        CHECK(marmot_account_proof_create(identity, owner_sk, NULL, NULL, MARMOT_CIPHERSUITE,
                                          MARMOT_SIGNATURE_SCHEME_ED25519,
                                          kp->leaf_node.signature_key, MLS_SIG_PK_LEN, now,
                                          proof) == MARMOT_OK, "genuine proof");
        CHECK(marmot_leaf_set_proof(&kp->leaf_node, proof) == MARMOT_OK, "set proof");
        break;
    }
    CHECK(mls_key_package_sign(kp, priv) == 0, "sign KeyPackage");
    MarmotLeafProofStatus want = mode == LEAF_GENUINE    ? MARMOT_LEAF_PROOF_VALID
                                 : mode == LEAF_NO_PROOF ? MARMOT_LEAF_PROOF_ABSENT
                                                         : MARMOT_LEAF_PROOF_INVALID;
    CHECK(marmot_leaf_proof_status(&kp->leaf_node, MARMOT_CIPHERSUITE) == want,
          "leaf proof status %d", (int)mode);
}

/* The Add Commit `x`'s own (modified) client makes for `kp`, never applied. */
static char *
forge_add_commit(Member *x, const MarmotGroupId *gid, const MlsKeyPackage *kp,
                 const uint8_t nostr_gid[32])
{
    MlsGroup g;
    load_mls(x, gid, &g);
    uint8_t exporter[32];
    memcpy(exporter, g.epoch_secrets.exporter_secret, 32);
    MlsAddResult add;
    memset(&add, 0, sizeof(add));
    CHECK(mls_group_add_member(&g, kp, &add) == 0, "MLS Add");
    char *json = marmot_commit_build_event(add.commit_data, add.commit_len, exporter,
                                           nostr_gid);
    CHECK(json, "Commit event");
    mls_add_result_clear(&add);
    mls_group_free(&g);
    sodium_memzero(exporter, sizeof(exporter));
    return json;
}

/* The reviewer's F2: a malicious admin adds a leaf that claims another
 * account, to post as it.  Every other member rejects the Commit, and
 * nothing changes; honest Adds carry the account's proof and pass. */
static void
test_forged_member_identity_rejected(void)
{
    Trio t;
    trio_init(&t);
    Member victor;
    member_init(&victor, "Victor");   /* the impersonated account, not a member */

    /* 1. The API refuses a KeyPackage without the proof (an unproven event
     *    that is otherwise valid: signed by its own account). */
    {
        MarmotConfig legacy = marmot_config_default();
        legacy.allow_unproven_members = true;
        Marmot *old = marmot_new_with_config(marmot_storage_memory_new(), &legacy);
        CHECK(old, "legacy instance");
        MarmotKeyPackageResult r;
        memset(&r, 0, sizeof(r));
        OK(marmot_create_key_package_unsigned(old, victor.pk, NULL, 0, &r));
        NostrEvent *ev = nostr_event_new();
        char *sk_hex = marmot_hex_encode(victor.sk, 32);
        CHECK(ev && sk_hex && nostr_event_deserialize_compact(ev, r.event_json, NULL) &&
              nostr_event_sign(ev, sk_hex) == 0, "sign the unproven KeyPackage");
        char *unproven = nostr_event_serialize_compact(ev);
        const char *kps[] = { unproven };
        char **welcomes = NULL;
        size_t n = 0;
        char *commit = NULL;
        MarmotError err = marmot_add_members(t.alice.m, &t.gid, kps, 1, &welcomes, &n, &commit);
        CHECK(err == MARMOT_ERR_KEY_PACKAGE_IDENTITY && !commit && !welcomes,
              "add_members with an unproven KeyPackage: %d", err);
        bool pending = true;
        char *pending_json = NULL;
        OK(marmot_get_pending_commit(t.alice.m, &t.gid, &pending_json, &pending));
        CHECK(pending_json == NULL, "nothing pending");
        /* Group creation applies its Commit at once: the inviter's own
         * check is all there is before the Welcome goes out. */
        MarmotGroupConfig cfg = {0};
        cfg.name = "With Victor";
        MarmotCreateGroupResult cg;
        memset(&cg, 0, sizeof(cg));
        err = marmot_create_group(t.alice.m, t.alice.pk, kps, 1, &cfg, &cg);
        CHECK(err == MARMOT_ERR_KEY_PACKAGE_IDENTITY && !cg.group && !cg.welcome_rumor_jsons,
              "create_group with an unproven KeyPackage: %d", err);
        free(unproven);
        free(sk_hex);
        nostr_event_free(ev);
        marmot_key_package_result_free(&r);
        marmot_free(old);
    }

    /* 2. A modified client builds the Adds anyway: every other member drops
     *    them, whatever the leaf carries. */
    struct {
        const char   *what;
        const uint8_t *claimed;
        LeafProof     mode;
    } cases[] = {
        { "Victor, no proof", victor.pk, LEAF_NO_PROOF },
        { "Victor, a proof Alice signed", victor.pk, LEAF_PROOF_BY_OTHER },
        { "Victor, his published proof over Alice's key", victor.pk, LEAF_REPLAYED_PROOF },
        { "a second Charlie, no proof", t.charlie.pk, LEAF_NO_PROOF },
        { "a second Charlie, his proof over Alice's key", t.charlie.pk, LEAF_REPLAYED_PROOF },
    };
    char *forged[5] = { NULL };
    for (size_t i = 0; i < 5; i++) {
        const uint8_t *owner_sk = cases[i].claimed == victor.pk ? victor.sk : t.charlie.sk;
        MlsKeyPackage kp;
        MlsKeyPackagePrivate priv;
        leaf_key_package(cases[i].claimed, owner_sk, &t.alice, cases[i].mode, &kp, &priv);
        forged[i] = forge_add_commit(&t.alice, &t.gid, &kp, t.nostr_gid);
        mls_key_package_clear(&kp);
        mls_key_package_private_clear(&priv);
        expect_rejected(&t.bob, &t.gid, forged[i], MARMOT_ERR_KEY_PACKAGE_IDENTITY, cases[i].what);
        expect_rejected(&t.charlie, &t.gid, forged[i], MARMOT_ERR_KEY_PACKAGE_IDENTITY,
                        cases[i].what);
    }

    /* 3. A member's Commit may not drop the proof from its own leaf: the
     *    transition as authorization sees it, Bob's leaf replaced by a
     *    (re-signed) one without the proof.  Not even in legacy mode. */
    {
        MlsGroup pre, post;
        load_mls(&t.alice, &t.gid, &pre);
        load_mls(&t.alice, &t.gid, &post);
        uint32_t bob_leaf = UINT32_MAX;
        uint8_t id[32];
        for (uint32_t i = 0; i < post.tree.n_leaves; i++)
            if (marmot_mls_sender_identity(&post, i, id) == 0 && memcmp(id, t.bob.pk, 32) == 0)
                bob_leaf = i;
        CHECK(bob_leaf != UINT32_MAX, "Bob's leaf");
        MlsLeafNode *leaf = &post.tree.nodes[mls_tree_leaf_to_node(bob_leaf)].leaf;
        CHECK(marmot_leaf_proof_status(leaf, MARMOT_CIPHERSUITE) == MARMOT_LEAF_PROOF_VALID,
              "Bob's leaf carries his proof");
        free(leaf->extensions_data);
        leaf->extensions_data = NULL;
        leaf->extensions_len = 0;
        leaf->signature[0] ^= 1;
        MarmotCommitKey key;
        MarmotGroupDataExtension *gde = NULL;
        CHECK(marmot_commit_authorize(&pre, &post, bob_leaf, false, &key, &gde) ==
                  MARMOT_ERR_KEY_PACKAGE_IDENTITY && !gde, "Bob drops his proof");
        CHECK(marmot_commit_authorize(&pre, &post, bob_leaf, true, &key, &gde) ==
                  MARMOT_ERR_KEY_PACKAGE_IDENTITY && !gde, "Bob drops his proof (legacy mode)");
        mls_group_free(&post);
        mls_group_free(&pre);
    }

    /* 4. Honest Commits pass: a member's UpdatePath keeps its proof, and
     *    Victor's own KeyPackage admits him; he then posts as himself. */
    char *renamed = rename_group(&t.bob, &t.gid, "Still bound");
    expect_commit(&t.alice, renamed, "Bob's rename (UpdatePath keeps his proof)");
    expect_commit(&t.charlie, renamed, "Bob's rename (UpdatePath keeps his proof)");
    char *victor_kp = key_package(&victor);
    const char *kps[] = { victor_kp };
    char **welcomes = NULL;
    size_t n = 0;
    char *add = NULL;
    OK(marmot_add_members(t.alice.m, &t.gid, kps, 1, &welcomes, &n, &add));
    merge(&t.alice, &t.gid);
    expect_commit(&t.bob, add, "Victor's genuine Add");
    expect_commit(&t.charlie, add, "Victor's genuine Add");
    join(&victor, welcomes[0]);
    Member *four[] = { &t.alice, &t.bob, &t.charlie, &victor };
    expect_messages_flow(four, 4, &t.gid);

    /* 5. Legacy mode (MarmotConfig.allow_unproven_members) takes a leaf
     *    without any proof -- the pre-0.10.0 behaviour this fixes -- but
     *    never one whose proof does not verify. */
    t.charlie.m->config.allow_unproven_members = true;
    MlsKeyPackage kp;
    MlsKeyPackagePrivate priv;
    leaf_key_package(t.bob.pk, t.bob.sk, &t.alice, LEAF_REPLAYED_PROOF, &kp, &priv);
    char *bad = forge_add_commit(&t.alice, &t.gid, &kp, t.nostr_gid);
    expect_rejected(&t.charlie, &t.gid, bad, MARMOT_ERR_KEY_PACKAGE_IDENTITY,
                    "legacy mode, a replayed proof");
    mls_key_package_clear(&kp);
    mls_key_package_private_clear(&priv);
    leaf_key_package(t.bob.pk, NULL, &t.alice, LEAF_NO_PROOF, &kp, &priv);
    char *bare = forge_add_commit(&t.alice, &t.gid, &kp, t.nostr_gid);
    expect_commit(&t.charlie, bare, "legacy mode, no proof");
    mls_key_package_clear(&kp);
    mls_key_package_private_clear(&priv);

    free(bare);
    free(bad);
    free(add);
    free(welcomes[0]);
    free(welcomes);
    free(victor_kp);
    free(renamed);
    for (size_t i = 0; i < 5; i++) free(forged[i]);
    marmot_free(victor.m);
    trio_clear(&t);
}

/* A kind:444 rumor for Mallory's Welcome: she built the group herself with
 * her own (modified) client, her leaf credential naming `creator`, and adds
 * `other` and the joiner's genuine KeyPackage `joiner_kp` in one Commit.
 * `author` is the rumor's pubkey (the NIP-59 seal's, in real delivery). */
static char *
mallory_welcome(const uint8_t creator[32], const MlsKeyPackage *other,
                const MlsKeyPackage *joiner_kp, const uint8_t author[32])
{
    MarmotGroupDataExtension *gde = marmot_group_data_extension_new();
    CHECK(gde, "GroupData");
    gde->version = MARMOT_EXTENSION_VERSION;
    randombytes_buf(gde->nostr_group_id, 32);
    gde->name = strdup("Mallory's");
    uint8_t *bytes = NULL;
    size_t len = 0;
    CHECK(marmot_group_data_extension_serialize(gde, &bytes, &len) == 0, "serialize");
    marmot_group_data_extension_free(gde);
    MlsTlsBuf ext;
    CHECK(mls_tls_buf_init(&ext, len + 8) == 0 &&
          mls_tls_write_u16(&ext, MARMOT_EXTENSION_TYPE) == 0 &&
          mls_tls_write_opaque16(&ext, bytes, len) == 0, "extension list");
    free(bytes);

    uint8_t gid[32], sig_sk[MLS_SIG_SK_LEN], sig_pk[MLS_SIG_PK_LEN];
    randombytes_buf(gid, sizeof(gid));
    CHECK(mls_crypto_sign_keygen(sig_sk, sig_pk) == 0, "Mallory's leaf key");
    MlsGroup g;
    CHECK(mls_group_create(&g, gid, 32, creator, 32, sig_sk, ext.data, ext.len) == 0,
          "Mallory's group");
    mls_tls_buf_free(&ext);
    sodium_memzero(sig_sk, sizeof(sig_sk));
    const MlsKeyPackage *kps[] = { other, joiner_kp };
    MlsAddResult add;
    memset(&add, 0, sizeof(add));
    CHECK(mls_group_add_members(&g, kps, 2, &add) == 0, "Mallory's Add");

    size_t b64_len = sodium_base64_ENCODED_LEN(add.welcome_len, sodium_base64_VARIANT_ORIGINAL);
    char *b64 = malloc(b64_len);
    CHECK(b64, "base64");
    sodium_bin2base64(b64, b64_len, add.welcome_data, add.welcome_len,
                      sodium_base64_VARIANT_ORIGINAL);
    char *author_hex = marmot_hex_encode(author, 32);
    size_t cap = strlen(b64) + 256;
    char *rumor = malloc(cap);
    CHECK(author_hex && rumor, "rumor");
    snprintf(rumor, cap,
             "{\"pubkey\":\"%s\",\"created_at\":%lld,\"kind\":444,"
             "\"tags\":[[\"encoding\",\"base64\"]],\"content\":\"%s\"}",
             author_hex, (long long)time(NULL), b64);
    free(author_hex);
    free(b64);
    mls_add_result_clear(&add);
    mls_group_free(&g);
    return rumor;
}

/* The joiner's own KeyPackage, as mls_group_add_members() takes it. */
static void
own_key_package(Member *x, MlsKeyPackage *kp)
{
    char *json = key_package(x);
    uint8_t owner[32];
    OK(marmot_parse_key_package_event(json, kp, owner));
    free(json);
}

/* Bob tries to join through `rumor`: `want` is the outcome; on failure
 * nothing of the group may be stored.  `forget_sender` models a storage
 * backend that keeps only the rumor (nostrdb): the record loses welcomer. */
static void
expect_join_ex(Member *x, const char *rumor, MarmotError want, const char *what,
               bool forget_sender)
{
    uint8_t wrapper[32];
    randombytes_buf(wrapper, sizeof(wrapper));
    MarmotWelcome *w = NULL;
    OK(marmot_process_welcome(x->m, wrapper, rumor, &w));
    if (forget_sender) memset(w->welcomer, 0, sizeof(w->welcomer));
    MarmotGroup **before = NULL;
    size_t n_before = 0;
    OK(marmot_get_all_groups(x->m, &before, &n_before));
    MarmotError err = marmot_accept_welcome(x->m, w);
    CHECK(err == want, "%s: %s got %d (%s), want %d", what, x->name, err,
          marmot_error_string(err), want);
    MarmotGroup **after = NULL;
    size_t n_after = 0;
    OK(marmot_get_all_groups(x->m, &after, &n_after));
    CHECK(n_after == n_before + (want == MARMOT_OK ? 1 : 0),
          "%s: %s has %zu groups, had %zu", what, x->name, n_after, n_before);
    groups_free(before, n_before);
    groups_free(after, n_after);
    /* Accepted or refused, it is no longer an invitation (a refusal is final:
     * since 0.11.0 it leaves the pending list, nostrc-7gx7). */
    MarmotPagination page = marmot_pagination_default();
    MarmotWelcome **pending = NULL;
    size_t n_pending = 0;
    OK(marmot_get_pending_welcomes(x->m, &page, &pending, &n_pending));
    for (size_t i = 0; i < n_pending; i++) {
        CHECK(memcmp(pending[i]->wrapper_event_id, wrapper, 32) != 0 ||
                  pending[i]->state != MARMOT_WELCOME_STATE_PENDING,
              "%s: %s still lists the Welcome as pending", what, x->name);
        marmot_welcome_free(pending[i]);
    }
    free(pending);
    marmot_welcome_free(w);
}

static void
expect_join(Member *x, const char *rumor, MarmotError want, const char *what)
{
    expect_join_ex(x, rumor, want, what, false);
}

/* A Welcome whose tree holds a leaf claiming another account is rejected by
 * the joiner, which stores nothing of the group (joining.md steps 5-6).  The
 * Welcome's own author may lack a proof: the seal binds it. */
static void
test_welcome_with_forged_member_rejected(void)
{
    Member mallory, alice, bob;
    member_init(&mallory, "Mallory");
    member_init(&alice, "Alice");
    member_init(&bob, "Bob");
    MlsKeyPackage bob_kp, other;
    MlsKeyPackagePrivate other_priv;
    own_key_package(&bob, &bob_kp);

    struct {
        const char *what;
        LeafProof   mode;
    } forged[] = {
        { "Alice's leaf without a proof", LEAF_NO_PROOF },
        { "Alice's leaf with a proof Mallory signed", LEAF_PROOF_BY_OTHER },
        { "Alice's leaf with her published proof over Mallory's key", LEAF_REPLAYED_PROOF },
    };
    for (size_t i = 0; i < 3; i++) {
        leaf_key_package(alice.pk, alice.sk, &mallory, forged[i].mode, &other, &other_priv);
        char *rumor = mallory_welcome(mallory.pk, &other, &bob_kp, mallory.pk);
        expect_join(&bob, rumor, MARMOT_ERR_KEY_PACKAGE_IDENTITY, forged[i].what);
        free(rumor);
        mls_key_package_clear(&other);
        mls_key_package_private_clear(&other_priv);
    }

    /* Only the GroupInfo signer's leaf is exempt (review W20 N7): a second,
     * unproven leaf naming the sender's own account is not. */
    leaf_key_package(mallory.pk, NULL, &mallory, LEAF_NO_PROOF, &other, &other_priv);
    char *second_device = mallory_welcome(mallory.pk, &other, &bob_kp, mallory.pk);
    expect_join(&bob, second_device, MARMOT_ERR_KEY_PACKAGE_IDENTITY,
                "a second unproven leaf of the sender");
    free(second_device);
    mls_key_package_clear(&other);
    mls_key_package_private_clear(&other_priv);

    /* The GroupInfo signer is unproven, so it must be the Welcome's sender. */
    leaf_key_package(alice.pk, alice.sk, &mallory, LEAF_GENUINE, &other, &other_priv);
    char *claims_alice = mallory_welcome(alice.pk, &other, &bob_kp, mallory.pk);
    expect_join(&bob, claims_alice, MARMOT_ERR_KEY_PACKAGE_IDENTITY,
                "Mallory's leaf claims Alice");
    char *sent_as_alice = mallory_welcome(mallory.pk, &other, &bob_kp, alice.pk);
    expect_join(&bob, sent_as_alice, MARMOT_ERR_KEY_PACKAGE_IDENTITY,
                "Welcome says Alice sent it; Mallory signed the GroupInfo");

    /* Genuine: Alice's own proof, Mallory the sender -- also when the
     * stored record lost the sender (it is read from the rumor). */
    char *genuine = mallory_welcome(mallory.pk, &other, &bob_kp, mallory.pk);
    expect_join(&bob, genuine, MARMOT_OK, "every leaf bound");
    char *genuine2 = mallory_welcome(mallory.pk, &other, &bob_kp, mallory.pk);
    expect_join_ex(&bob, genuine2, MARMOT_OK, "sender read from the stored rumor", true);
    free(genuine2);
    mls_key_package_clear(&other);
    mls_key_package_private_clear(&other_priv);

    /* Legacy mode takes a leaf without a proof, never a bad proof. */
    bob.m->config.allow_unproven_members = true;
    leaf_key_package(alice.pk, alice.sk, &mallory, LEAF_REPLAYED_PROOF, &other, &other_priv);
    char *replayed = mallory_welcome(mallory.pk, &other, &bob_kp, mallory.pk);
    expect_join(&bob, replayed, MARMOT_ERR_KEY_PACKAGE_IDENTITY, "legacy mode, replayed proof");
    mls_key_package_clear(&other);
    mls_key_package_private_clear(&other_priv);
    leaf_key_package(alice.pk, NULL, &mallory, LEAF_NO_PROOF, &other, &other_priv);
    char *bare = mallory_welcome(mallory.pk, &other, &bob_kp, mallory.pk);
    expect_join(&bob, bare, MARMOT_OK, "legacy mode, no proof");
    mls_key_package_clear(&other);
    mls_key_package_private_clear(&other_priv);

    free(bare);
    free(replayed);
    free(genuine);
    free(sent_as_alice);
    free(claims_alice);
    mls_key_package_clear(&bob_kp);
    marmot_free(mallory.m);
    marmot_free(alice.m);
    marmot_free(bob.m);
}

/* Review W20 N1: marmot_process_welcome_from() takes the NIP-59 seal's
 * author explicitly.  The join's sender exemption then rests on it, not on
 * the rumor: a rumor naming another author is refused, and a rumor without
 * a pubkey still gets its sender. */
static void
test_welcome_sender_from_seal(void)
{
    Member mallory, alice, bob;
    member_init(&mallory, "Mallory");
    member_init(&alice, "Alice");
    member_init(&bob, "Bob");
    MlsKeyPackage bob_kp, other;
    MlsKeyPackagePrivate other_priv;
    own_key_package(&bob, &bob_kp);
    leaf_key_package(alice.pk, alice.sk, &mallory, LEAF_GENUINE, &other, &other_priv);

    /* Mallory's unproven leaf signs; her seal says Mallory. */
    char *rumor = mallory_welcome(mallory.pk, &other, &bob_kp, mallory.pk);
    uint8_t wrapper[32];
    MarmotWelcome *w = NULL;
    randombytes_buf(wrapper, sizeof(wrapper));
    CHECK(marmot_process_welcome_from(bob.m, wrapper, rumor, alice.pk, &w) ==
              MARMOT_ERR_AUTHOR_MISMATCH && !w, "rumor says Mallory, seal says Alice");
    randombytes_buf(wrapper, sizeof(wrapper));
    OK(marmot_process_welcome_from(bob.m, wrapper, rumor, mallory.pk, &w));
    CHECK(memcmp(w->welcomer, mallory.pk, 32) == 0, "sender recorded");
    OK(marmot_accept_welcome(bob.m, w));
    marmot_welcome_free(w);

    /* A rumor without a pubkey: the seal's author is the sender. */
    char *bare = mallory_welcome(mallory.pk, &other, &bob_kp, mallory.pk);
    char *at = strstr(bare, "\"pubkey\":\"");
    CHECK(at, "pubkey field");
    memmove(at, at + 76, strlen(at + 76) + 1);   /* drop "pubkey":"<64 hex>", */
    CHECK(!strstr(bare, "pubkey") && strncmp(bare, "{\"created_at\":", 14) == 0,
          "no pubkey left, JSON intact");
    randombytes_buf(wrapper, sizeof(wrapper));
    OK(marmot_process_welcome_from(bob.m, wrapper, bare, mallory.pk, &w));
    OK(marmot_accept_welcome(bob.m, w));
    marmot_welcome_free(w);
    /* ... while the rumor-trusting call has no sender for it: refused. */
    char *bare2 = mallory_welcome(mallory.pk, &other, &bob_kp, mallory.pk);
    at = strstr(bare2, "\"pubkey\":\"");
    memmove(at, at + 76, strlen(at + 76) + 1);
    expect_join(&bob, bare2, MARMOT_ERR_KEY_PACKAGE_IDENTITY, "no sender at all");

    free(bare2);
    free(bare);
    free(rumor);
    mls_key_package_clear(&other);
    mls_key_package_private_clear(&other_priv);
    mls_key_package_clear(&bob_kp);
    marmot_free(mallory.m);
    marmot_free(alice.m);
    marmot_free(bob.m);
}

/* marmot_account_proof_template() + marmot_set_account_proof(): what a
 * signer-only client (Gnostr) does to prove its leaves; the created group's
 * creator leaf then admits members added by another admin. */
static void
test_account_proof_enrollment(void)
{
    Member alice, bob, charlie;
    member_init_unenrolled(&alice, "Alice");
    member_init(&bob, "Bob");
    member_init(&charlie, "Charlie");

    /* Without enrollment, a signer-only KeyPackage would carry no proof. */
    MarmotKeyPackageResult r;
    memset(&r, 0, sizeof(r));
    CHECK(marmot_create_key_package_unsigned(alice.m, alice.pk, NULL, 0, &r) ==
              MARMOT_ERR_KEY_PACKAGE_IDENTITY && !r.event_json,
          "unsigned KeyPackage before enrollment");
    CHECK(!marmot_has_account_proof(alice.m, alice.pk), "not enrolled");

    /* Enroll: sign the template with the account key, as a signer would. */
    char *tmpl = NULL;
    OK(marmot_account_proof_template(alice.m, alice.pk, &tmpl));
    char *sk_hex = marmot_hex_encode(alice.sk, 32);
    NostrEvent *ev = nostr_event_new();
    CHECK(ev && sk_hex && nostr_event_deserialize_compact(ev, tmpl, NULL) &&
          ev->kind == 450 && nostr_event_sign(ev, sk_hex) == 0, "sign template");
    char *signed_json = nostr_event_serialize_compact(ev);
    /* Signed by another account, or a changed field: refused. */
    char *bob_hex = marmot_hex_encode(bob.sk, 32);
    NostrEvent *wrong = nostr_event_new();
    CHECK(wrong && nostr_event_deserialize_compact(wrong, tmpl, NULL) &&
          nostr_event_sign(wrong, bob_hex) == 0, "sign as Bob");
    char *wrong_json = nostr_event_serialize_compact(wrong);
    CHECK(marmot_set_account_proof(alice.m, alice.pk, wrong_json) == MARMOT_ERR_VALIDATION,
          "Bob's signature");
    char *edited = strdup(signed_json);
    char *c = strstr(edited, "Authorize");
    CHECK(c, "content");
    c[0] = 'a';
    CHECK(marmot_set_account_proof(alice.m, alice.pk, edited) == MARMOT_ERR_VALIDATION,
          "edited content");
    CHECK(!marmot_has_account_proof(alice.m, alice.pk), "still not enrolled");
    OK(marmot_set_account_proof(alice.m, alice.pk, signed_json));
    CHECK(marmot_has_account_proof(alice.m, alice.pk), "enrolled");

    /* Now the signer-only KeyPackage carries it... */
    OK(marmot_create_key_package_unsigned(alice.m, alice.pk, NULL, 0, &r));
    NostrEvent *kpev = nostr_event_new();
    MlsKeyPackage kp;
    CHECK(kpev && nostr_event_deserialize_compact(kpev, r.event_json, NULL) &&
          marmot_validate_key_package_event(kpev, &kp, NULL) == MARMOT_OK, "validate");
    CHECK(marmot_leaf_proof_status(&kp.leaf_node, MARMOT_CIPHERSUITE) ==
              MARMOT_LEAF_PROOF_VALID, "the unsigned KeyPackage's leaf is proven");
    mls_key_package_clear(&kp);
    nostr_event_free(kpev);
    marmot_key_package_result_free(&r);

    /* ...and so does the leaf of a group Alice creates: Bob (an admin) can
     * admit Charlie, who accepts Alice's leaf although Bob sent him the
     * Welcome. */
    char *bob_kp = key_package(&bob);
    const char *kps[] = { bob_kp };
    uint8_t admins[2][32];
    memcpy(admins[0], alice.pk, 32);
    memcpy(admins[1], bob.pk, 32);
    MarmotGroupConfig cfg = {0};
    cfg.name = "Enrolled";
    cfg.admin_pubkeys = admins;
    cfg.admin_count = 2;
    MarmotCreateGroupResult cg;
    memset(&cg, 0, sizeof(cg));
    OK(marmot_create_group(alice.m, alice.pk, kps, 1, &cfg, &cg));
    join(&bob, cg.welcome_rumor_jsons[0]);
    MarmotGroupId gid = marmot_group_id_new(cg.group->mls_group_id.data,
                                            cg.group->mls_group_id.len);
    marmot_create_group_result_free(&cg);
    char *charlie_kp = key_package(&charlie);
    const char *kps2[] = { charlie_kp };
    char **welcomes = NULL;
    size_t n = 0;
    char *add = NULL;
    OK(marmot_add_members(bob.m, &gid, kps2, 1, &welcomes, &n, &add));
    merge(&bob, &gid);
    expect_commit(&alice, add, "Bob adds Charlie");
    join(&charlie, welcomes[0]);
    Member *all[] = { &alice, &bob, &charlie };
    expect_messages_flow(all, 3, &gid);

    free(add);
    free(welcomes[0]);
    free(welcomes);
    free(charlie_kp);
    free(bob_kp);
    marmot_group_id_free(&gid);
    free(edited);
    free(wrong_json);
    nostr_event_free(wrong);
    free(bob_hex);
    free(signed_json);
    nostr_event_free(ev);
    free(sk_hex);
    free(tmpl);
    marmot_free(alice.m);
    marmot_free(bob.m);
    marmot_free(charlie.m);
}

/* A KeyPackage without the account proof, as libmarmot 0.9.0 made them
 * (signed by its account): what `x`'s unenrolled legacy-mode instance
 * still produces. */
static char *
legacy_key_package(Member *x)
{
    CHECK(x->m->config.allow_unproven_members && !marmot_has_account_proof(x->m, x->pk),
          "an unenrolled legacy instance");
    MarmotKeyPackageResult r;
    memset(&r, 0, sizeof(r));
    OK(marmot_create_key_package_unsigned(x->m, x->pk, NULL, 0, &r));
    NostrEvent *ev = nostr_event_new();
    char *sk_hex = marmot_hex_encode(x->sk, 32);
    CHECK(ev && sk_hex && nostr_event_deserialize_compact(ev, r.event_json, NULL) &&
          nostr_event_sign(ev, sk_hex) == 0, "sign");
    char *json = nostr_event_serialize_compact(ev);
    free(sk_hex);
    nostr_event_free(ev);
    marmot_key_package_result_free(&r);
    return json;
}

static size_t
mls_leaf_count(Member *x, const MarmotGroupId *gid)
{
    MlsGroup g;
    load_mls(x, gid, &g);
    size_t n = 0;
    for (uint32_t i = 0; i < g.tree.n_leaves; i++)
        if (g.tree.nodes[mls_tree_leaf_to_node(i)].type == MLS_NODE_LEAF) n++;
    mls_group_free(&g);
    return n;
}

/* Review W20 B1: a group of unproven members (created by 0.9.0, or before
 * enrollment) upgraded to the default mode.  Adding Dave would produce a
 * Welcome Dave must reject (Bob's and Charlie's leaves are unproven and not
 * the sender's), and Dave's leaf would stay as a ghost: the inviter refuses
 * the Add and nothing changes.  In legacy mode the group still grows. */
static void
test_add_refused_when_joiners_would_reject(void)
{
    Member alice, bob, charlie, dave;
    member_init_unenrolled(&alice, "Alice");
    member_init_unenrolled(&bob, "Bob");
    member_init_unenrolled(&charlie, "Charlie");
    member_init(&dave, "Dave");
    Member *three[] = { &alice, &bob, &charlie };
    for (size_t i = 0; i < 3; i++) three[i]->m->config.allow_unproven_members = true;

    /* The 0.9.0 group: unproven creator, unproven members. */
    char *bob_kp = legacy_key_package(&bob), *charlie_kp = legacy_key_package(&charlie);
    const char *kps[] = { bob_kp, charlie_kp };
    MarmotGroupConfig cfg = {0};
    cfg.name = "Upgraded";
    cfg.admin_pubkeys = (uint8_t (*)[32])alice.pk;
    cfg.admin_count = 1;
    MarmotCreateGroupResult cg;
    memset(&cg, 0, sizeof(cg));
    OK(marmot_create_group(alice.m, alice.pk, kps, 2, &cfg, &cg));
    join(&bob, cg.welcome_rumor_jsons[0]);
    join(&charlie, cg.welcome_rumor_jsons[1]);
    MarmotGroupId gid = marmot_group_id_new(cg.group->mls_group_id.data,
                                            cg.group->mls_group_id.len);
    marmot_create_group_result_free(&cg);
    expect_messages_flow(three, 3, &gid);

    /* Everyone upgrades to the default mode. */
    for (size_t i = 0; i < 3; i++) three[i]->m->config.allow_unproven_members = false;
    char *dave_kp = key_package(&dave);
    const char *kps2[] = { dave_kp };
    Snapshot before;
    snapshot(&alice, &gid, &before);
    char **welcomes = NULL;
    size_t n = 0;
    char *add = NULL;
    MarmotError err = marmot_add_members(alice.m, &gid, kps2, 1, &welcomes, &n, &add);
    CHECK(err == MARMOT_ERR_KEY_PACKAGE_IDENTITY && !add && !welcomes,
          "Alice's Add of Dave: %d", err);
    expect_unchanged(&alice, &gid, &before, "refused Add");
    snapshot_clear(&before);
    char *pending = NULL;
    bool live = false;
    OK(marmot_get_pending_commit(alice.m, &gid, &pending, &live));
    CHECK(!pending, "nothing pending");
    CHECK(mls_leaf_count(&alice, &gid) == 3, "no ghost leaf");
    expect_messages_flow(three, 3, &gid);

    /* Legacy mode (the transition) still admits Dave, and he can join. */
    for (size_t i = 0; i < 3; i++) three[i]->m->config.allow_unproven_members = true;
    dave.m->config.allow_unproven_members = true;
    OK(marmot_add_members(alice.m, &gid, kps2, 1, &welcomes, &n, &add));
    merge(&alice, &gid);
    expect_commit(&bob, add, "Bob, legacy");
    expect_commit(&charlie, add, "Charlie, legacy");
    join(&dave, welcomes[0]);
    CHECK(mls_leaf_count(&alice, &gid) == 4, "Dave joined");

    free(add);
    free(welcomes[0]);
    free(welcomes);
    free(dave_kp);
    free(charlie_kp);
    free(bob_kp);
    marmot_group_id_free(&gid);
    marmot_free(alice.m);
    marmot_free(bob.m);
    marmot_free(charlie.m);
    marmot_free(dave.m);
}

/* `tmpl` (a kind:450 proof template) signed with `signer`'s account key. */
static char *
sign_template(const Member *signer, const char *tmpl)
{
    char *sk_hex = marmot_hex_encode(signer->sk, 32);
    NostrEvent *ev = nostr_event_new();
    CHECK(sk_hex && ev && nostr_event_deserialize_compact(ev, tmpl, NULL) &&
          nostr_event_sign(ev, sk_hex) == 0, "sign template");
    char *json = nostr_event_serialize_compact(ev);
    nostr_event_free(ev);
    free(sk_hex);
    return json;
}

/* `x` proves its existing leaf in `gid` by a self-update (nostrc-rgb5); the
 * others apply the Commit. */
static void
prove_leaf(Member *x, const MarmotGroupId *gid, Member *const *others, size_t n)
{
    char *tmpl = NULL;
    OK(marmot_group_account_proof_template(x->m, gid, &tmpl));
    char *signed_json = sign_template(x, tmpl);
    char *commit = NULL;
    OK(marmot_self_update(x->m, gid, signed_json, &commit));
    merge(x, gid);
    for (size_t i = 0; i < n; i++) expect_commit(others[i], commit, "a member proves its leaf");
    free(commit);
    free(signed_json);
    free(tmpl);
}

/* The migration path (review W20 B1, nostrc-rgb5): an upgraded group whose
 * members' leaves are unproven cannot admit anyone until they are proven.
 * Each member signs the template for its own group leaf and self-updates;
 * receivers accept the new leaf with the proof, reject one signed by another
 * account, and once every leaf is proven an admin can admit Dave, who joins
 * in the default mode. */
static void
test_members_prove_existing_leaves_by_self_update(void)
{
    Member alice, bob, charlie, dave;
    member_init_unenrolled(&alice, "Alice");
    member_init_unenrolled(&bob, "Bob");
    member_init_unenrolled(&charlie, "Charlie");
    member_init(&dave, "Dave");
    Member *three[] = { &alice, &bob, &charlie };
    for (size_t i = 0; i < 3; i++) three[i]->m->config.allow_unproven_members = true;
    char *bob_kp = legacy_key_package(&bob), *charlie_kp = legacy_key_package(&charlie);
    const char *kps[] = { bob_kp, charlie_kp };
    MarmotGroupConfig cfg = {0};
    cfg.name = "Migrating";
    cfg.admin_pubkeys = (uint8_t (*)[32])alice.pk;
    cfg.admin_count = 1;
    MarmotCreateGroupResult cg;
    memset(&cg, 0, sizeof(cg));
    OK(marmot_create_group(alice.m, alice.pk, kps, 2, &cfg, &cg));
    join(&bob, cg.welcome_rumor_jsons[0]);
    join(&charlie, cg.welcome_rumor_jsons[1]);
    MarmotGroupId gid = marmot_group_id_new(cg.group->mls_group_id.data,
                                            cg.group->mls_group_id.len);
    marmot_create_group_result_free(&cg);
    for (size_t i = 0; i < 3; i++) three[i]->m->config.allow_unproven_members = false;

    /* A proof signed by another account is refused; nothing is pending. */
    char *tmpl = NULL;
    OK(marmot_group_account_proof_template(bob.m, &gid, &tmpl));
    char *by_charlie = sign_template(&charlie, tmpl);
    char *commit = NULL;
    CHECK(marmot_self_update(bob.m, &gid, by_charlie, &commit) == MARMOT_ERR_VALIDATION &&
              !commit, "Bob's leaf with Charlie's signature");
    free(by_charlie);
    free(tmpl);

    /* A self-update without a proof rotates keys and is accepted. */
    OK(marmot_self_update(charlie.m, &gid, NULL, &commit));
    merge(&charlie, &gid);
    expect_commit(&alice, commit, "Charlie rotates");
    expect_commit(&bob, commit, "Charlie rotates");
    free(commit);

    char *dave_kp = key_package(&dave);
    const char *kps2[] = { dave_kp };
    char **welcomes = NULL;
    size_t n = 0;
    char *add = NULL;
    Member *not_alice[] = { &bob, &charlie }, *not_bob[] = { &alice, &charlie },
           *not_charlie[] = { &alice, &bob };
    prove_leaf(&alice, &gid, not_alice, 2);
    prove_leaf(&bob, &gid, not_bob, 2);
    CHECK(marmot_add_members(alice.m, &gid, kps2, 1, &welcomes, &n, &add) ==
              MARMOT_ERR_KEY_PACKAGE_IDENTITY, "Charlie's leaf is still unproven");
    prove_leaf(&charlie, &gid, not_charlie, 2);

    OK(marmot_add_members(alice.m, &gid, kps2, 1, &welcomes, &n, &add));
    merge(&alice, &gid);
    expect_commit(&bob, add, "Bob applies the Add");
    expect_commit(&charlie, add, "Charlie applies the Add");
    join(&dave, welcomes[0]);
    Member *four[] = { &alice, &bob, &charlie, &dave };
    expect_messages_flow(four, 4, &gid);

    free(add);
    free(welcomes[0]);
    free(welcomes);
    free(dave_kp);
    free(charlie_kp);
    free(bob_kp);
    marmot_group_id_free(&gid);
    for (size_t i = 0; i < 4; i++) marmot_free(four[i]->m);
}

/* Review W20 B1: without an account proof for the creator, the group would
 * start with a leaf no joiner accepts in another admin's Welcome.  The
 * default mode refuses to create it; enrolling (or legacy mode) creates it. */
static void
test_create_group_needs_enrollment(void)
{
    Member alice, bob;
    member_init_unenrolled(&alice, "Alice");
    member_init(&bob, "Bob");
    char *bob_kp = key_package(&bob);
    const char *kps[] = { bob_kp };
    MarmotGroupConfig cfg = {0};
    cfg.name = "Needs enrollment";
    MarmotCreateGroupResult cg;
    memset(&cg, 0, sizeof(cg));
    MarmotGroupId *none = NULL;
    MarmotGroup **groups = NULL;
    size_t n = 0;
    (void)none;
    CHECK(marmot_create_group(alice.m, alice.pk, kps, 1, &cfg, &cg) ==
              MARMOT_ERR_KEY_PACKAGE_IDENTITY && !cg.group && !cg.welcome_rumor_jsons,
          "create_group without enrollment");
    CHECK(marmot_create_group(alice.m, alice.pk, NULL, 0, &cfg, &cg) ==
              MARMOT_ERR_KEY_PACKAGE_IDENTITY && !cg.group, "alone, without enrollment");
    OK(marmot_get_all_groups(alice.m, &groups, &n));
    CHECK(n == 0, "nothing stored");
    groups_free(groups, n);

    OK(test_enroll(alice.m, alice.pk, alice.sk));
    OK(marmot_create_group(alice.m, alice.pk, kps, 1, &cfg, &cg));
    join(&bob, cg.welcome_rumor_jsons[0]);
    marmot_create_group_result_free(&cg);

    /* A different account on the same instance is not enrolled. */
    Member carol;
    member_init_unenrolled(&carol, "Carol");
    CHECK(marmot_create_group(alice.m, carol.pk, NULL, 0, &cfg, &cg) ==
              MARMOT_ERR_KEY_PACKAGE_IDENTITY, "another account");

    free(bob_kp);
    marmot_free(carol.m);
    marmot_free(alice.m);
    marmot_free(bob.m);
}

/* ── Retiring the retained parent (nostrc-yuj2) ────────────────────────── */

/* The retained parent record as stored: its parent state, and the tier from
 * the 0.10.0 trailer (-1 without one). */
static int
stored_parent(Member *x, const MarmotGroupId *gid, MlsGroup *parent_out)
{
    MarmotStorage *st = x->m->storage;
    uint8_t *data = NULL;
    size_t len = 0;
    OK(st->mls_load(st->ctx, "mls_group_parent", gid->data, gid->len, &data, &len));
    MlsTlsReader r;
    mls_tls_reader_init(&r, data, len);
    uint8_t version = 0, privileged = 0, key[64];
    uint64_t epoch = 0;
    uint8_t *blob = NULL;
    size_t blob_len = 0;
    CHECK(mls_tls_read_u8(&r, &version) == 0 && version == 1 &&
          mls_tls_read_u64(&r, &epoch) == 0 && mls_tls_read_u8(&r, &privileged) == 0 &&
          mls_tls_read_fixed(&r, key, sizeof(key)) == 0 &&
          mls_tls_read_opaque32(&r, &blob, &blob_len) == 0, "parent record prefix");
    int tier = -1;
    if (!mls_tls_reader_done(&r)) {
        uint8_t t8 = 0;
        CHECK(mls_tls_read_u8(&r, &t8) == 0, "tier");
        tier = t8;
    }
    if (parent_out)
        CHECK(mls_group_deserialize(blob, blob_len, parent_out) == 0 &&
              parent_out->epoch == epoch, "parent state");
    sodium_memzero(blob, blob_len);
    free(blob);
    sodium_memzero(data, len);
    free(data);
    return tier;
}

#define TIER_CONVERGENCE 0
#define TIER_READER      1

/* Review B1's attack, with everything the stored state holds: the retained
 * parent re-processes the relay-visible `commit` from `committer_leaf`, and
 * the re-derived epoch decrypts `consumed` (an application message this
 * member already read).  True when the plaintext comes back. */
static bool
store_attack(Member *x, const MarmotGroupId *gid, const char *commit, uint32_t committer_leaf,
             const char *consumed)
{
    MlsGroup parent;
    stored_parent(x, gid, &parent);
    MarmotStorage *st = x->m->storage;
    uint8_t exporter[32];
    OK(st->get_exporter_secret(st->ctx, gid, parent.epoch, exporter));
    NostrEvent *ev = nostr_event_new();
    CHECK(ev && nostr_event_deserialize_compact(ev, commit, NULL), "Commit event");
    uint8_t *msg = NULL;
    size_t msg_len = 0;
    CHECK(marmot_group_event_decrypt(exporter, ev->content, &msg, &msg_len) == 0,
          "the Commit, from the relay and the stored exporter secret");
    nostr_event_free(ev);
    bool recovered = false;
    if (mls_group_process_commit(&parent, msg, msg_len, committer_leaf) == 0) {
        NostrEvent *m = nostr_event_new();
        CHECK(m && nostr_event_deserialize_compact(m, consumed, NULL), "message event");
        uint8_t *ct = NULL, *pt = NULL;
        size_t ct_len = 0, pt_len = 0;
        uint32_t sender = 0;
        if (marmot_group_event_decrypt(parent.epoch_secrets.exporter_secret, m->content, &ct,
                                       &ct_len) == 0 &&
            mls_group_decrypt(&parent, ct, ct_len, &pt, &pt_len, &sender) == 0)
            recovered = true;
        free(ct);
        if (pt) sodium_memzero(pt, pt_len);
        free(pt);
        nostr_event_free(m);
    }
    free(msg);
    sodium_memzero(exporter, sizeof(exporter));
    mls_group_free(&parent);
    return recovered;
}

static void
expect_app(Member *x, const char *event_json, const char *what)
{
    MarmotError err;
    CHECK(deliver(x, event_json, &err, NULL) == MARMOT_RESULT_APPLICATION_MESSAGE &&
          err == MARMOT_OK, "%s: %s got %d", what, x->name, err);
}

/* A privileged rename `x` makes from `state` (its own view of an epoch). */
static char *
rename_from(MlsGroup *state, const uint8_t nostr_gid[32], const char *name)
{
    MlsTlsReader r;
    mls_tls_reader_init(&r, state->extensions_data, state->extensions_len);
    uint16_t type = 0;
    uint8_t *ext = NULL;
    size_t ext_len = 0;
    CHECK(mls_tls_read_u16(&r, &type) == 0 && type == MARMOT_EXTENSION_TYPE &&
          mls_tls_read_opaque16(&r, &ext, &ext_len) == 0, "GroupData");
    MarmotGroupDataExtension *gde = marmot_group_data_extension_deserialize(ext, ext_len);
    free(ext);
    CHECK(gde, "GroupData parse");
    free(gde->name);
    gde->name = strdup(name);
    uint8_t *bytes = NULL;
    size_t len = 0;
    CHECK(marmot_group_data_extension_serialize(gde, &bytes, &len) == 0, "serialize");
    marmot_group_data_extension_free(gde);
    MlsTlsBuf list;
    CHECK(mls_tls_buf_init(&list, len + 8) == 0 &&
          mls_tls_write_u16(&list, MARMOT_EXTENSION_TYPE) == 0 &&
          mls_tls_write_opaque16(&list, bytes, len) == 0, "extension list");
    free(bytes);
    uint8_t exporter[32];
    memcpy(exporter, state->epoch_secrets.exporter_secret, 32);
    MlsCommitResult res;
    memset(&res, 0, sizeof(res));
    CHECK(mls_group_commit_extensions(state, list.data, list.len, &res) == 0, "rename Commit");
    char *json = event_for_commit(&res, exporter, nostr_gid);
    mls_commit_result_clear(&res);
    mls_tls_buf_free(&list);
    sodium_memzero(exporter, sizeof(exporter));
    return json;
}

/* The exposure of review B1 ends once no competitor can matter.  Alice
 * (admin) renames; Charlie keeps the full parent while Bob (an admin whose
 * key sorts below Alice's) could still publish a winning rename from it, and
 * retires it when Bob speaks at the new epoch.  After that the stored state
 * and the public Commit no longer re-derive the epoch; late messages still
 * read; a competitor is WRONG_EPOCH and changes nothing. */
static void
test_retained_parent_retires_once_settled(void)
{
    Trio t;
    trio_init(&t);
    uint32_t alice_leaf = 0;
    char *late = app_message(&t.bob, &t.gid, "sent at E, read at E+1");
    MlsGroup bob_at_e;
    load_mls(&t.bob, &t.gid, &bob_at_e);
    char *commit = rename_group(&t.alice, &t.gid, "Settled");
    expect_commit(&t.charlie, commit, "Charlie applies Alice's rename");
    CHECK(stored_parent(&t.charlie, &t.gid, NULL) == TIER_CONVERGENCE,
          "Bob could still win: the full parent is kept");

    /* Alice (the committer) speaks: that proves nothing about Bob. */
    char *m1 = app_message(&t.alice, &t.gid, "Alice at E+1");
    expect_app(&t.charlie, m1, "Alice's first E+1 message");
    CHECK(stored_parent(&t.charlie, &t.gid, NULL) == TIER_CONVERGENCE, "still waiting for Bob");
    /* The window: stored state + Commit re-derive E+1, m1 included. */
    CHECK(store_attack(&t.charlie, &t.gid, commit, alice_leaf, m1),
          "inside the window the attack still works (review B1)");

    /* Bob applies the rename and speaks at E+1: nobody can win any more. */
    expect_commit(&t.bob, commit, "Bob follows");
    char *m2 = app_message(&t.bob, &t.gid, "Bob at E+1");
    expect_app(&t.charlie, m2, "Bob's first E+1 message");
    MlsGroup parent;
    CHECK(stored_parent(&t.charlie, &t.gid, &parent) == TIER_READER, "retired");
    static const uint8_t zero[MLS_HASH_LEN];
    CHECK(memcmp(parent.epoch_secrets.init_secret, zero, MLS_HASH_LEN) == 0 &&
          memcmp(parent.epoch_secrets.membership_key, zero, MLS_HASH_LEN) == 0 &&
          sodium_is_zero(parent.own_encryption_key, sizeof(parent.own_encryption_key)) &&
          sodium_is_zero(parent.own_signature_key, sizeof(parent.own_signature_key)),
          "no init secret, membership key or private key left");
    mls_group_free(&parent);
    CHECK(!store_attack(&t.charlie, &t.gid, commit, alice_leaf, m1) &&
          !store_attack(&t.charlie, &t.gid, commit, alice_leaf, m2),
          "retired: the stored state and the Commit no longer decrypt consumed messages");

    /* Late messages of E still read, once. */
    expect_app(&t.charlie, late, "late message after retirement");
    char *late_again = republish(late);
    expect_rejected(&t.charlie, &t.gid, late_again, MARMOT_ERR_MLS, "late replay");
    free(late_again);
    /* The applied Commit again is ours; a competitor is refused, unchanged. */
    MarmotError err;
    CHECK(deliver(&t.charlie, commit, &err, NULL) == MARMOT_RESULT_OWN_MESSAGE && err == MARMOT_OK,
          "the applied Commit again");
    char *rival = rename_from(&bob_at_e, t.nostr_gid, "Bob's rival");
    char *rival_env = republish(rival);
    expect_rejected(&t.charlie, &t.gid, rival_env, MARMOT_ERR_WRONG_EPOCH,
                    "a competitor after retirement");
    expect_converged(t.all, 3, &t.gid, "Settled", t.epoch + 1);
    expect_messages_flow(t.all, 3, &t.gid);

    /* The next Commit replaces the record (with a full parent again: a
     * rename by Bob leaves nobody who could beat it). */
    char *next = rename_group(&t.bob, &t.gid, "Next");
    expect_commit(&t.charlie, next, "Charlie applies Bob's rename");
    expect_commit(&t.alice, next, "Alice applies Bob's rename");
    CHECK(stored_parent(&t.charlie, &t.gid, NULL) == TIER_READER,
          "no admin sorts below Bob: retired at once");

    free(next);
    free(rival_env);
    free(rival);
    mls_group_free(&bob_at_e);
    free(m2);
    free(m1);
    free(commit);
    free(late);
    trio_clear(&t);
}

/* Inside the window a winning competitor still replaces the applied Commit,
 * as before (Marmot convergence, "Same-epoch races"). */
static void
test_competitor_within_window_still_wins(void)
{
    Trio t;
    trio_init(&t);
    MlsGroup bob_at_e;
    load_mls(&t.bob, &t.gid, &bob_at_e);
    char *commit = rename_group(&t.alice, &t.gid, "Alice's");
    expect_commit(&t.charlie, commit, "Charlie applies Alice's rename");
    char *m1 = app_message(&t.alice, &t.gid, "Alice at E+1");
    expect_app(&t.charlie, m1, "Alice's E+1 message");
    CHECK(stored_parent(&t.charlie, &t.gid, NULL) == TIER_CONVERGENCE, "window open");
    char *rival = rename_from(&bob_at_e, t.nostr_gid, "Bob's");
    expect_commit(&t.charlie, rival, "Bob's lower-key rename wins inside the window");
    MarmotGroup *g = NULL;
    OK(marmot_get_group(t.charlie.m, &t.gid, &g));
    CHECK(g->name && strcmp(g->name, "Bob's") == 0 && g->epoch == t.epoch + 1, "switched");
    marmot_group_free(g);
    free(rival);
    free(m1);
    free(commit);
    mls_group_free(&bob_at_e);
    trio_clear(&t);
}

/* Nobody could publish a winning competitor: the parent is reduced when the
 * Commit is applied.  Bob (admin) applies Alice's privileged rename: Alice
 * committed, Charlie is no admin.  Late messages still read. */
static void
test_retained_parent_retired_at_once(void)
{
    Trio t;
    trio_init(&t);
    char *late = app_message(&t.charlie, &t.gid, "Charlie at E");
    char *commit = rename_group(&t.alice, &t.gid, "At once");
    char *m1 = app_message(&t.alice, &t.gid, "Alice at E+1");
    expect_commit(&t.bob, commit, "Bob applies");
    CHECK(stored_parent(&t.bob, &t.gid, NULL) == TIER_READER, "nobody can win: retired at once");
    expect_app(&t.bob, m1, "Alice's E+1 message");
    CHECK(!store_attack(&t.bob, &t.gid, commit, 0, m1), "no re-derivation");
    expect_app(&t.bob, late, "Charlie's late E message");
    free(m1);
    free(commit);
    free(late);
    trio_clear(&t);
}

/* A record written by 0.9.0 (no trailer) loads as the full parent, with
 * every member that could win pending -- the committer too, whose leaf the
 * old record does not name -- and retires the same way. */
static void
test_retained_parent_without_trailer_migrates(void)
{
    Trio t;
    trio_init(&t);
    char *late = app_message(&t.bob, &t.gid, "late");
    char *commit = rename_group(&t.alice, &t.gid, "Old record");
    expect_commit(&t.charlie, commit, "Charlie applies");
    MarmotStorage *st = t.charlie.m->storage;
    uint8_t *data = NULL;
    size_t len = 0;
    OK(st->mls_load(st->ctx, "mls_group_parent", t.gid.data, t.gid.len, &data, &len));
    /* Cut the trailer: tier, count, one pending leaf (Bob). */
    CHECK(len > 9 && data[len - 9] == TIER_CONVERGENCE, "trailer with one pending leaf");
    OK(st->mls_store(st->ctx, "mls_group_parent", t.gid.data, t.gid.len, data, len - 9));
    sodium_memzero(data, len);
    free(data);
    CHECK(stored_parent(&t.charlie, &t.gid, NULL) == -1, "a 0.9.0 record");
    expect_app(&t.charlie, late, "late message through the old record");
    CHECK(stored_parent(&t.charlie, &t.gid, NULL) == TIER_CONVERGENCE, "rewritten, still full");
    expect_commit(&t.bob, commit, "Bob follows");
    char *m2 = app_message(&t.bob, &t.gid, "Bob at E+1");
    expect_app(&t.charlie, m2, "Bob speaks");
    CHECK(stored_parent(&t.charlie, &t.gid, NULL) == TIER_CONVERGENCE,
          "the committer's leaf is unknown: Alice still pending");
    char *m3 = app_message(&t.alice, &t.gid, "Alice at E+1");
    expect_app(&t.charlie, m3, "Alice speaks");
    CHECK(stored_parent(&t.charlie, &t.gid, NULL) == TIER_READER, "retired");
    free(m3);
    free(m2);
    free(commit);
    free(late);
    trio_clear(&t);
}

/* A witness must be the very member the parent listed (review W20 N7): the
 * same account and the same leaf signature key at that index.  A slot
 * re-filled by another account, or by another key of the same account, says
 * nothing about whether the listed member applied the Commit. */
static void
test_witness_must_be_the_listed_member(void)
{
    Trio t;
    trio_init(&t);
    char *commit = rename_group(&t.alice, &t.gid, "Witnessed");
    expect_commit(&t.charlie, commit, "Charlie applies");
    CHECK(stored_parent(&t.charlie, &t.gid, NULL) == TIER_CONVERGENCE, "Bob pending");
    MlsGroup cur;
    load_mls(&t.charlie, &t.gid, &cur);
    uint32_t bob_leaf = UINT32_MAX;
    uint8_t id[32];
    for (uint32_t i = 0; i < cur.tree.n_leaves; i++)
        if (marmot_mls_sender_identity(&cur, i, id) == 0 && memcmp(id, t.bob.pk, 32) == 0)
            bob_leaf = i;
    CHECK(bob_leaf != UINT32_MAX, "Bob's leaf");
    MlsLeafNode *leaf = &cur.tree.nodes[mls_tree_leaf_to_node(bob_leaf)].leaf;
    uint8_t *undo = NULL;
    size_t undo_len = 0;

    leaf->signature_key[0] ^= 1;   /* another key at Bob's index */
    OK(marmot_commit_note_witness(t.charlie.m, &cur, bob_leaf, &undo, &undo_len));
    CHECK(!undo && stored_parent(&t.charlie, &t.gid, NULL) == TIER_CONVERGENCE,
          "another leaf key does not witness for Bob");
    leaf->signature_key[0] ^= 1;
    leaf->credential_identity[0] ^= 1;   /* another account at Bob's index */
    OK(marmot_commit_note_witness(t.charlie.m, &cur, bob_leaf, &undo, &undo_len));
    CHECK(!undo && stored_parent(&t.charlie, &t.gid, NULL) == TIER_CONVERGENCE,
          "another account does not witness for Bob");
    leaf->credential_identity[0] ^= 1;
    OK(marmot_commit_note_witness(t.charlie.m, &cur, bob_leaf, &undo, &undo_len));
    CHECK(undo && stored_parent(&t.charlie, &t.gid, NULL) == TIER_READER, "Bob himself does");
    sodium_memzero(undo, undo_len);
    free(undo);
    mls_group_free(&cur);
    free(commit);
    trio_clear(&t);
}

/* Review W20 N3: KeyPackages made without the account key use the enrolled
 * instance key, so every such KeyPackage of one instance run has the same
 * leaf signature key.  RFC 9420 section 7.3 wants it unique in a group:
 * a second one in the same group is refused and nothing changes. */
static void
test_signer_only_key_packages_share_one_leaf_key(void)
{
    Member alice, bob;
    member_init_unenrolled(&alice, "Alice");
    member_init(&bob, "Bob");
    OK(test_enroll(alice.m, alice.pk, alice.sk));
    char *jsons[2];
    MlsKeyPackage kps[2];
    for (int i = 0; i < 2; i++) {
        MarmotKeyPackageResult r;
        memset(&r, 0, sizeof(r));
        OK(marmot_create_key_package_unsigned(alice.m, alice.pk, NULL, 0, &r));
        NostrEvent *ev = nostr_event_new();
        char *sk_hex = marmot_hex_encode(alice.sk, 32);
        CHECK(ev && sk_hex && nostr_event_deserialize_compact(ev, r.event_json, NULL) &&
              nostr_event_sign(ev, sk_hex) == 0, "sign");
        jsons[i] = nostr_event_serialize_compact(ev);
        OK(marmot_parse_key_package_event(jsons[i], &kps[i], NULL));
        free(sk_hex);
        nostr_event_free(ev);
        marmot_key_package_result_free(&r);
    }
    CHECK(memcmp(kps[0].leaf_node.signature_key, kps[1].leaf_node.signature_key,
                 MLS_SIG_PK_LEN) == 0, "one leaf key per instance run");

    MarmotGroupConfig cfg = {0};
    cfg.name = "One key";
    MarmotCreateGroupResult cg;
    memset(&cg, 0, sizeof(cg));
    const char *first[] = { jsons[0] };
    OK(marmot_create_group(bob.m, bob.pk, first, 1, &cfg, &cg));
    join(&alice, cg.welcome_rumor_jsons[0]);
    MarmotGroupId gid = marmot_group_id_new(cg.group->mls_group_id.data,
                                            cg.group->mls_group_id.len);
    marmot_create_group_result_free(&cg);

    Snapshot before;
    snapshot(&bob, &gid, &before);
    const char *second[] = { jsons[1] };
    char **welcomes = NULL;
    size_t n = 0;
    char *add = NULL;
    CHECK(marmot_add_members(bob.m, &gid, second, 1, &welcomes, &n, &add) != MARMOT_OK &&
              !add, "Alice's second signer-only KeyPackage in the same group");
    expect_unchanged(&bob, &gid, &before, "refused duplicate leaf key");
    snapshot_clear(&before);

    for (int i = 0; i < 2; i++) {
        free(jsons[i]);
        mls_key_package_clear(&kps[i]);
    }
    marmot_group_id_free(&gid);
    marmot_free(alice.m);
    marmot_free(bob.m);
}

/* Settling the parent is part of the message's writes: when it, or a later
 * write, fails, the message is not delivered and nothing changes (storage
 * without transactions: the records are written back). */
static void
test_settling_write_failure_keeps_everything(void)
{
    Trio t;
    trio_init(&t);
    char *commit = rename_group(&t.alice, &t.gid, "Moved");
    expect_commit(&t.charlie, commit, "Charlie applies");
    expect_commit(&t.bob, commit, "Bob follows");
    char *m2 = app_message(&t.bob, &t.gid, "Bob at E+1");
    MarmotError err;
    for (int round = 0; round < 2; round++) {
        Snapshot before;
        snapshot(&t.charlie, &t.gid, &before);
        faults_arm(&t.charlie);
        if (round == 0) g_faults.fail_at = 2;              /* the parent write */
        else g_faults.fail_save_message = true;            /* after it */
        deliver(&t.charlie, m2, &err, NULL);
        faults_disarm(&t.charlie);
        CHECK(err != MARMOT_OK, "round %d: delivered despite the failure", round);
        expect_unchanged(&t.charlie, &t.gid, &before, "settling failed");
        snapshot_clear(&before);
        CHECK(stored_parent(&t.charlie, &t.gid, NULL) == TIER_CONVERGENCE,
              "round %d: still the full parent", round);
    }
    expect_app(&t.charlie, m2, "delivered once storage works");
    CHECK(stored_parent(&t.charlie, &t.gid, NULL) == TIER_READER, "retired");
    free(m2);
    free(commit);
    trio_clear(&t);
}

/* marmot_get_group_members(): the leaf identities of our stored epoch, each
 * once; a pending Remove changes nothing until it is merged or applied. */
static bool
has_member(const uint8_t (*members)[32], size_t n, const uint8_t pk[32])
{
    for (size_t i = 0; i < n; i++)
        if (memcmp(members[i], pk, 32) == 0) return true;
    return false;
}

static void
test_group_members_follow_the_epoch(void)
{
    Trio t;
    trio_init(&t);
    for (size_t i = 0; i < 3; i++) {
        uint8_t (*members)[32] = NULL;
        size_t n = 0;
        OK(marmot_get_group_members(t.all[i]->m, &t.gid, &members, &n));
        CHECK(n == 3 && has_member((const uint8_t (*)[32])members, n, t.alice.pk) &&
              has_member((const uint8_t (*)[32])members, n, t.bob.pk) &&
              has_member((const uint8_t (*)[32])members, n, t.charlie.pk),
              "%s sees %zu members", t.all[i]->name, n);
        free(members);
    }
    char *rm = NULL;
    OK(marmot_remove_members(t.alice.m, &t.gid, (const uint8_t (*)[32]) t.charlie.pk, 1, &rm));
    uint8_t (*members)[32] = NULL;
    size_t n = 0;
    OK(marmot_get_group_members(t.alice.m, &t.gid, &members, &n));
    CHECK(n == 3, "a pending Remove is not applied: %zu", n);
    free(members);
    merge(&t.alice, &t.gid);
    expect_commit(&t.bob, rm, "Bob");
    for (size_t i = 0; i < 2; i++) {
        members = NULL;
        OK(marmot_get_group_members(t.all[i]->m, &t.gid, &members, &n));
        CHECK(n == 2 && !has_member((const uint8_t (*)[32])members, n, t.charlie.pk),
              "%s still counts Charlie (%zu members)", t.all[i]->name, n);
        free(members);
    }
    MarmotGroupId unknown = marmot_group_id_new((const uint8_t *)"no such group", 13);
    members = (uint8_t (*)[32])1;
    CHECK(marmot_get_group_members(t.alice.m, &unknown, &members, &n) ==
          MARMOT_ERR_GROUP_NOT_FOUND && members == NULL && n == 0, "unknown group");
    CHECK(marmot_get_group_members(t.alice.m, &t.gid, NULL, &n) == MARMOT_ERR_INVALID_ARG,
          "out_members is required");
    marmot_group_id_free(&unknown);
    free(rm);
    trio_clear(&t);
}

/* nostrc-xrya: a member an admin removes cannot enter the next epoch, but
 * learns it was removed and by whom; its group turns inactive.  A removal
 * forged by a non-admin changes nothing, and a Welcome back clears it. */
static bool
removal_of(Member *x, const MarmotGroupId *gid, uint8_t by[32], uint64_t *epoch)
{
    bool removed = true;
    OK(marmot_get_group_removal(x->m, gid, &removed, by, epoch, NULL));
    return removed;
}

static bool
removal_final(Member *x, const MarmotGroupId *gid)
{
    bool removed = false, final = false;
    OK(marmot_get_group_removal(x->m, gid, &removed, NULL, NULL, &final));
    CHECK(removed, "%s: no removal", x->name);
    return final;
}

static MarmotGroupState
state_of(Member *x, const MarmotGroupId *gid)
{
    MarmotGroup *g = NULL;
    OK(marmot_get_group(x->m, gid, &g));
    CHECK(g != NULL, "%s: group", x->name);
    MarmotGroupState state = g->state;
    marmot_group_free(g);
    return state;
}

static void
test_removed_member_learns_it(void)
{
    Trio t;
    trio_init(&t);
    uint8_t by[32];
    uint64_t epoch = 0;
    for (size_t i = 0; i < 3; i++)
        CHECK(!removal_of(t.all[i], &t.gid, by, &epoch), "%s not removed", t.all[i]->name);

    /* Charlie (no admin) forges a Commit removing Bob: Bob keeps his group. */
    {
        MlsGroup bob_state, g;
        load_mls(&t.bob, &t.gid, &bob_state);
        load_mls(&t.charlie, &t.gid, &g);
        uint8_t exporter[32];
        memcpy(exporter, g.epoch_secrets.exporter_secret, 32);
        MlsCommitResult r;
        memset(&r, 0, sizeof(r));
        CHECK(mls_group_remove_member(&g, bob_state.own_leaf_index, &r) == 0, "remove");
        char *forged = event_for_commit(&r, exporter, t.nostr_gid);
        expect_rejected(&t.bob, &t.gid, forged, MARMOT_ERR_COMMIT_FROM_NON_ADMIN,
                        "a non-admin's removal of Bob");
        CHECK(!removal_of(&t.bob, &t.gid, by, NULL), "a forged removal is not kept");
        CHECK(state_of(&t.bob, &t.gid) == MARMOT_GROUP_STATE_ACTIVE, "Bob stays active");
        free(forged);
        mls_commit_result_clear(&r);
        mls_group_free(&g);
        mls_group_free(&bob_state);
        sodium_memzero(exporter, sizeof(exporter));
    }

    /* Alice (the admin) removes Charlie. */
    char *rm = NULL;
    OK(marmot_remove_members(t.alice.m, &t.gid, (const uint8_t (*)[32]) t.charlie.pk, 1, &rm));
    merge(&t.alice, &t.gid);
    expect_commit(&t.bob, rm, "Bob applies the removal");
    CHECK(!removal_of(&t.bob, &t.gid, by, NULL), "Bob was not removed");
    MarmotError err;
    MarmotGroup *updated = NULL;
    MarmotMessageResultType type = deliver(&t.charlie, rm, &err, &updated);
    CHECK(err == MARMOT_OK && type == MARMOT_RESULT_COMMIT,
          "Charlie gets the removal as a Commit: err=%d (%s) type=%d", err,
          marmot_error_string(err), type);
    CHECK(updated && updated->state == MARMOT_GROUP_STATE_INACTIVE, "the result says inactive");
    marmot_group_free(updated);
    CHECK(removal_of(&t.charlie, &t.gid, by, &epoch), "Charlie learns he was removed");
    CHECK(memcmp(by, t.alice.pk, 32) == 0, "by Alice");
    CHECK(epoch == t.epoch, "from epoch %" PRIu64 " (got %" PRIu64 ")", t.epoch, epoch);
    CHECK(state_of(&t.charlie, &t.gid) == MARMOT_GROUP_STATE_INACTIVE, "inactive");
    /* The same Commit again (another relay) changes nothing: a duplicate. */
    type = deliver(&t.charlie, rm, &err, NULL);
    CHECK((err == MARMOT_OK && type == MARMOT_RESULT_OWN_MESSAGE) ||
          err == MARMOT_ERR_USE_AFTER_EVICTION, "a copy: %d/%d", err, type);
    CHECK(removal_of(&t.charlie, &t.gid, NULL, NULL), "still removed");

    /* Nothing of the next epoch, and no sending. */
    char *next = app_message(&t.bob, &t.gid, "after Charlie");
    expect_app(&t.alice, next, "Alice reads Bob");
    deliver(&t.charlie, next, &err, NULL);
    CHECK(err == MARMOT_ERR_USE_AFTER_EVICTION, "Charlie reads nothing more: %d", err);
    MarmotOutgoingMessage out;
    memset(&out, 0, sizeof(out));
    CHECK(marmot_create_message(t.charlie.m, &t.gid, "{}", &out) != MARMOT_OK,
          "Charlie cannot send");
    free(next);
    free(rm);
    trio_clear(&t);
}

/* ── W22 review B1: a removal is judged by the Commit ordering, not by
 * arrival.  In the trio Alice and Bob are admins and Bob's key sorts below
 * Alice's, so Bob's privileged Commit beats Alice's removal of Charlie. ── */

static MarmotGroupState
deliver_state(Member *x, const char *event_json, MarmotError *err, uint64_t *epoch)
{
    MarmotGroup *g = NULL;
    MarmotMessageResultType type = deliver(x, event_json, err, &g);
    MarmotGroupState state = g ? g->state : (MarmotGroupState)-1;
    if (epoch) *epoch = g ? g->epoch : 0;
    CHECK(*err != MARMOT_OK || type == MARMOT_RESULT_COMMIT, "%s: type %d", x->name, type);
    marmot_group_free(g);
    return state;
}

static bool
has_state(Member *x, const MarmotGroupId *gid, const char *label)
{
    uint8_t *blob = NULL;
    size_t len = 0;
    MarmotError err = x->m->storage->mls_load(x->m->storage->ctx, label, gid->data, gid->len,
                                              &blob, &len);
    free(blob);
    CHECK(err == MARMOT_OK || err == MARMOT_ERR_STORAGE_NOT_FOUND, "%s: %s load %d", x->name,
          label, err);
    return err == MARMOT_OK;
}

static void
test_losing_removal_first_is_undone(void)
{
    Trio t;
    trio_init(&t);
    char *rm = NULL;
    OK(marmot_remove_members(t.alice.m, &t.gid, (const uint8_t (*)[32]) t.charlie.pk, 1, &rm));
    char *ren = rename_pending(&t.bob, &t.gid, "Bob's");

    /* The losing removal first: Charlie is removed, but not for good. */
    MarmotError err;
    uint64_t epoch = 0;
    CHECK(deliver_state(&t.charlie, rm, &err, &epoch) == MARMOT_GROUP_STATE_INACTIVE &&
          err == MARMOT_OK, "the removal: err=%d", err);
    CHECK(removal_of(&t.charlie, &t.gid, NULL, NULL), "removed");
    CHECK(!removal_final(&t.charlie, &t.gid), "Bob could still win the epoch");
    CHECK(has_state(&t.charlie, &t.gid, "mls_group"), "the epoch's state is kept");
    char *early = app_message(&t.alice, &t.gid, "not for Charlie");
    deliver(&t.charlie, early, &err, NULL);
    CHECK(err == MARMOT_ERR_USE_AFTER_EVICTION, "a message is not read: %d", err);
    free(early);

    /* Then Bob's winning rename: Charlie is a member of that epoch again. */
    CHECK(deliver_state(&t.charlie, ren, &err, &epoch) == MARMOT_GROUP_STATE_ACTIVE &&
          err == MARMOT_OK && epoch == t.epoch + 1, "the winner re-activates: err=%d", err);
    CHECK(!removal_of(&t.charlie, &t.gid, NULL, NULL), "the removal is forgotten");
    merge(&t.bob, &t.gid);
    expect_commit(&t.alice, ren, "Alice applies Bob's winning rename");
    expect_converged(t.all, 3, &t.gid, "Bob's", t.epoch + 1);
    char *msg = app_message(&t.bob, &t.gid, "Charlie still here");
    expect_app(&t.charlie, msg, "Charlie reads the winning epoch");
    /* The losing removal again, in a new envelope: it loses to the Commit
     * applied (the same envelope is a processed duplicate). */
    char *again = republish(rm);
    deliver(&t.charlie, again, &err, NULL);
    CHECK(err == MARMOT_ERR_WRONG_EPOCH, "the loser again: %d", err);
    free(again);
    CHECK(!removal_of(&t.charlie, &t.gid, NULL, NULL), "still a member");
    free(msg);
    free(ren);
    free(rm);
    trio_clear(&t);
}

/* The winner first: the losing removal of the previous epoch is WRONG_EPOCH
 * (review L4). */
static void
test_losing_removal_after_winner(void)
{
    Trio t;
    trio_init(&t);
    char *rm = NULL;
    OK(marmot_remove_members(t.alice.m, &t.gid, (const uint8_t (*)[32]) t.charlie.pk, 1, &rm));
    char *ren = rename_pending(&t.bob, &t.gid, "Bob's");
    MarmotError err;
    uint64_t epoch = 0;
    CHECK(deliver_state(&t.charlie, ren, &err, &epoch) == MARMOT_GROUP_STATE_ACTIVE &&
          err == MARMOT_OK, "the winner: err=%d", err);
    deliver(&t.charlie, rm, &err, NULL);
    CHECK(err == MARMOT_ERR_WRONG_EPOCH, "the losing removal: %d", err);
    CHECK(!removal_of(&t.charlie, &t.gid, NULL, NULL), "not removed");
    merge(&t.bob, &t.gid);
    char *msg = app_message(&t.bob, &t.gid, "hello Charlie");
    expect_app(&t.charlie, msg, "Charlie reads Bob");
    free(msg);
    free(ren);
    free(rm);
    trio_clear(&t);
}

/* A removal of the previous epoch that beats the Commit applied (Bob's
 * ordinary self-update loses to any privileged Commit) ends the group, judged
 * on the retained parent; Alice's key sorts above Bob's, so it stays open to
 * a winner from Bob, and the parent state is kept. */
static void
test_previous_epoch_removal_wins(void)
{
    Trio t;
    trio_init(&t);
    char *rm = NULL;
    OK(marmot_remove_members(t.alice.m, &t.gid, (const uint8_t (*)[32]) t.charlie.pk, 1, &rm));
    char *upd = self_update(&t.bob, &t.gid);
    expect_commit(&t.charlie, upd, "Charlie applies Bob's self-update");
    MarmotError err;
    uint64_t epoch = 0;
    CHECK(deliver_state(&t.charlie, rm, &err, &epoch) == MARMOT_GROUP_STATE_INACTIVE &&
          err == MARMOT_OK, "the removal beats it: err=%d", err);
    uint8_t by[32];
    uint64_t removed_epoch = 0;
    CHECK(removal_of(&t.charlie, &t.gid, by, &removed_epoch) &&
          memcmp(by, t.alice.pk, 32) == 0 && removed_epoch == t.epoch, "by Alice, epoch");
    CHECK(!removal_final(&t.charlie, &t.gid), "Bob could still win");
    CHECK(has_state(&t.charlie, &t.gid, MARMOT_MLS_PARENT_LABEL), "the parent is kept");
    free(upd);
    free(rm);
    trio_clear(&t);
}

/* Alice's rename and Alice's removal of Charlie, both of the current epoch
 * and made from her state (neither applied there), the removal's digest
 * sorting above the rename's: one committer, so the digest decides. */
static void
forge_rename_and_removal(Trio *t, char **rename_json, char **removal_json)
{
    MlsGroup c;
    load_mls(&t->charlie, &t->gid, &c);
    uint32_t charlie_leaf = c.own_leaf_index;
    mls_group_free(&c);
    for (int attempt = 0; attempt < 64; attempt++) {
        MlsGroup a, b;
        load_mls(&t->alice, &t->gid, &a);
        load_mls(&t->alice, &t->gid, &b);
        uint8_t exporter[32];
        memcpy(exporter, a.epoch_secrets.exporter_secret, 32);
        MarmotGroupDataExtension *gde = group_data_with(&t->alice, &t->gid, "Alice's", NULL);
        uint8_t *bytes = NULL;
        size_t len = 0;
        CHECK(marmot_group_data_extension_serialize(gde, &bytes, &len) == 0, "serialize");
        marmot_group_data_extension_free(gde);
        MlsTlsBuf ext;
        CHECK(mls_tls_buf_init(&ext, len + 8) == 0 &&
              mls_tls_write_u16(&ext, MARMOT_EXTENSION_TYPE) == 0 &&
              mls_tls_write_opaque16(&ext, bytes, len) == 0, "extension list");
        free(bytes);
        MlsCommitResult rn, rv;
        memset(&rn, 0, sizeof(rn));
        memset(&rv, 0, sizeof(rv));
        CHECK(mls_group_commit_extensions(&a, ext.data, ext.len, &rn) == 0, "rename");
        CHECK(mls_group_remove_member(&b, charlie_leaf, &rv) == 0, "removal");
        mls_tls_buf_free(&ext);
        uint8_t dn[32], dv[32];
        CHECK(mls_crypto_hash(dn, rn.commit_data, rn.commit_len) == 0 &&
              mls_crypto_hash(dv, rv.commit_data, rv.commit_len) == 0, "digests");
        bool ok = memcmp(dv, dn, 32) > 0;
        if (ok) {
            *rename_json = event_for_commit(&rn, exporter, t->nostr_gid);
            *removal_json = event_for_commit(&rv, exporter, t->nostr_gid);
        }
        mls_commit_result_clear(&rn);
        mls_commit_result_clear(&rv);
        mls_group_free(&a);
        mls_group_free(&b);
        sodium_memzero(exporter, sizeof(exporter));
        if (ok) return;
    }
    CHECK(false, "no digest order in 64 attempts");
}

/* A removal of the previous epoch that loses to the Commit applied (review
 * L4): Charlie applied Alice's rename, which Bob could still beat (so the
 * parent is kept in full); Alice's removal sorts after her rename. */
static void
test_previous_epoch_removal_loses(void)
{
    Trio t;
    trio_init(&t);
    char *ren = NULL, *rm = NULL;
    forge_rename_and_removal(&t, &ren, &rm);
    expect_commit(&t.charlie, ren, "Charlie applies Alice's rename");
    MarmotError err;
    deliver(&t.charlie, rm, &err, NULL);
    CHECK(err == MARMOT_ERR_WRONG_EPOCH, "the losing removal: %d", err);
    CHECK(!removal_of(&t.charlie, &t.gid, NULL, NULL), "not removed");
    MarmotGroup *g = NULL;
    OK(marmot_get_group(t.charlie.m, &t.gid, &g));
    CHECK(g->state == MARMOT_GROUP_STATE_ACTIVE && g->name &&
          strcmp(g->name, "Alice's") == 0, "active in the rename's epoch");
    marmot_group_free(g);
    free(ren);
    free(rm);
    trio_clear(&t);
}

/* A removal no other admin can beat (Bob's key is the lowest) is final at
 * once: the removed epoch's secrets are deleted (review N1), and nothing of
 * the group is judged any more. */
static void
test_final_removal_forgets_keys(void)
{
    Trio t;
    trio_init(&t);
    char *rm = NULL;
    OK(marmot_remove_members(t.bob.m, &t.gid, (const uint8_t (*)[32]) t.charlie.pk, 1, &rm));
    MarmotError err;
    CHECK(deliver_state(&t.charlie, rm, &err, NULL) == MARMOT_GROUP_STATE_INACTIVE &&
          err == MARMOT_OK, "the removal: err=%d", err);
    CHECK(removal_final(&t.charlie, &t.gid), "nobody can beat Bob");
    CHECK(!has_state(&t.charlie, &t.gid, "mls_group"), "the MLS state is gone");
    CHECK(!has_state(&t.charlie, &t.gid, MARMOT_MLS_PARENT_LABEL), "the parent is gone");
    uint8_t secret[32];
    for (uint64_t e = 0; e <= t.epoch + 1; e++)
        CHECK(t.charlie.m->storage->get_exporter_secret(t.charlie.m->storage->ctx, &t.gid, e,
                                                        secret) != MARMOT_OK,
              "exporter secret of epoch %llu kept", (unsigned long long)e);
    char *ren = rename_pending(&t.alice, &t.gid, "Alice's");
    deliver(&t.charlie, ren, &err, NULL);
    CHECK(err == MARMOT_ERR_USE_AFTER_EVICTION, "a final removal judges nothing: %d", err);

    /* Invited again: a new Welcome clears it (review L5). */
    merge(&t.bob, &t.gid);   /* Bob's removal was published and won */
    char *kp = key_package(&t.charlie);
    const char *kps[] = { kp };
    char **welcomes = NULL;
    size_t n = 0;
    char *add = NULL;
    OK(marmot_add_members(t.bob.m, &t.gid, kps, 1, &welcomes, &n, &add));
    merge(&t.bob, &t.gid);
    join(&t.charlie, welcomes[0]);
    CHECK(!removal_of(&t.charlie, &t.gid, NULL, NULL), "re-invited: not removed");
    MarmotGroup *g = NULL;
    OK(marmot_get_group(t.charlie.m, &t.gid, &g));
    CHECK(g->state == MARMOT_GROUP_STATE_ACTIVE, "active again");
    marmot_group_free(g);
    free(welcomes[0]);
    free(welcomes);
    free(add);
    free(kp);
    free(ren);
    free(rm);
    trio_clear(&t);
}

/* Charlie made admin by Alice (Charlie < Bob < Alice). */
static void
make_charlie_admin(Trio *t)
{
    uint8_t admins[3][32];
    memcpy(admins[0], t->alice.pk, 32);
    memcpy(admins[1], t->bob.pk, 32);
    memcpy(admins[2], t->charlie.pk, 32);
    MarmotGroupConfig cfg = {0};
    cfg.admin_pubkeys = admins;
    cfg.admin_count = 3;
    char *commit = NULL;
    OK(marmot_update_group_metadata(t->alice.m, &t->gid, &cfg, &commit));
    merge(&t->alice, &t->gid);
    expect_commit(&t->bob, commit, "Bob: Charlie an admin");
    expect_commit(&t->charlie, commit, "Charlie: an admin");
    free(commit);
    t->epoch++;
}

/* Our own pending Commit beats a removal (review L3): Charlie's pending
 * rename sorts before Alice's removal, so the removal waits behind it and
 * loses once Charlie's Commit is merged. */
static void
test_own_pending_beats_removal(void)
{
    g_charlie_lowest = true;
    Trio t;
    trio_init(&t);
    g_charlie_lowest = false;
    make_charlie_admin(&t);
    char *rm = NULL;
    OK(marmot_remove_members(t.alice.m, &t.gid, (const uint8_t (*)[32]) t.charlie.pk, 1, &rm));
    char *mine = rename_pending(&t.charlie, &t.gid, "Charlie's");
    MarmotError err;
    deliver(&t.charlie, rm, &err, NULL);
    CHECK(err == MARMOT_ERR_OWN_COMMIT_PENDING, "the removal waits behind ours: %d", err);
    CHECK(!removal_of(&t.charlie, &t.gid, NULL, NULL), "not removed");
    merge(&t.charlie, &t.gid);
    deliver(&t.charlie, rm, &err, NULL);
    CHECK(err == MARMOT_ERR_WRONG_EPOCH, "then it loses: %d", err);
    CHECK(!removal_of(&t.charlie, &t.gid, NULL, NULL), "still not removed");
    expect_commit(&t.bob, mine, "Bob applies Charlie's rename");
    char *msg = app_message(&t.bob, &t.gid, "hi Charlie");
    expect_app(&t.charlie, msg, "Charlie reads");
    free(msg);
    free(mine);
    free(rm);
    trio_clear(&t);
}

/* Deferred Commits are replayed winner first: a losing removal and the
 * Commit that beats it both wait behind Charlie's pending one; clearing it
 * applies Bob's rename and the removal loses. */
static void
test_deferred_replay_winner_first(void)
{
    g_charlie_lowest = true;
    Trio t;
    trio_init(&t);
    g_charlie_lowest = false;
    make_charlie_admin(&t);
    char *rm = NULL;
    OK(marmot_remove_members(t.alice.m, &t.gid, (const uint8_t (*)[32]) t.charlie.pk, 1, &rm));
    char *ren = rename_pending(&t.bob, &t.gid, "Bob's");
    char *mine = rename_pending(&t.charlie, &t.gid, "Charlie's");
    MarmotError err;
    deliver(&t.charlie, rm, &err, NULL);
    CHECK(err == MARMOT_ERR_OWN_COMMIT_PENDING, "the removal deferred: %d", err);
    deliver(&t.charlie, ren, &err, NULL);
    CHECK(err == MARMOT_ERR_OWN_COMMIT_PENDING, "Bob's rename deferred: %d", err);
    /* The winner (Bob's rename, which arrived second) is replayed first
     * (review F3). */
    size_t order[16], n = 0;
    OK(marmot_commit_deferred_replay_order(t.charlie.m, &t.gid, order, 16, &n));
    CHECK(n == 2 && order[0] == 1 && order[1] == 0, "replay order %zu: %zu, %zu", n,
          order[0], order[1]);
    OK(marmot_clear_pending_commit(t.charlie.m, &t.gid));   /* no relay took Charlie's */
    CHECK(!removal_of(&t.charlie, &t.gid, NULL, NULL), "the removal lost");
    MarmotGroup *g = NULL;
    OK(marmot_get_group(t.charlie.m, &t.gid, &g));
    CHECK(g->state == MARMOT_GROUP_STATE_ACTIVE && g->epoch == t.epoch + 1 &&
          g->name && strcmp(g->name, "Bob's") == 0, "in Bob's epoch");
    marmot_group_free(g);
    merge(&t.bob, &t.gid);
    char *msg = app_message(&t.bob, &t.gid, "hi Charlie");
    expect_app(&t.charlie, msg, "Charlie reads Bob's epoch");
    free(msg);
    free(mine);
    free(ren);
    free(rm);
    trio_clear(&t);
}

/* A removal drops our pending Commit (review L7): it can never merge. */
static void
test_removal_drops_own_pending(void)
{
    Trio t;
    trio_init(&t);
    char *rm = NULL;
    OK(marmot_remove_members(t.bob.m, &t.gid, (const uint8_t (*)[32]) t.charlie.pk, 1, &rm));
    char *upd = NULL;
    OK(marmot_self_update(t.charlie.m, &t.gid, NULL, &upd));   /* ordinary: loses */
    MarmotError err;
    CHECK(deliver_state(&t.charlie, rm, &err, NULL) == MARMOT_GROUP_STATE_INACTIVE &&
          err == MARMOT_OK, "removed: err=%d", err);
    char *pending = NULL;
    OK(marmot_get_pending_commit(t.charlie.m, &t.gid, &pending, NULL));
    CHECK(pending == NULL, "our pending Commit is dropped");
    free(upd);
    free(rm);
    trio_clear(&t);
}

/* A record that does not parse is reported as such, never as "left", and
 * the group judges nothing (review N3). */
static void
test_corrupt_removal_record(void)
{
    Trio t;
    trio_init(&t);
    char *rm = NULL;
    OK(marmot_remove_members(t.alice.m, &t.gid, (const uint8_t (*)[32]) t.charlie.pk, 1, &rm));
    MarmotError err;
    deliver(&t.charlie, rm, &err, NULL);
    OK(err);
    const uint8_t junk[5] = { 2, 0, 0, 0, 0 };
    OK(t.charlie.m->storage->mls_store(t.charlie.m->storage->ctx, "mls_group_removed",
                                       t.gid.data, t.gid.len, junk, sizeof(junk)));
    bool removed = true;
    CHECK(marmot_get_group_removal(t.charlie.m, &t.gid, &removed, NULL, NULL, NULL) ==
          MARMOT_ERR_DESERIALIZATION && !removed, "a corrupt record");
    char *ren = rename_pending(&t.bob, &t.gid, "Bob's");
    deliver(&t.charlie, ren, &err, NULL);
    CHECK(err == MARMOT_ERR_USE_AFTER_EVICTION, "nothing is judged: %d", err);
    free(ren);
    free(rm);
    trio_clear(&t);
}

/* The group moves on after a removal that stays contested (Alice removes
 * Charlie; Bob's key sorts lower): later-epoch events Charlie cannot open
 * make it final after MARMOT_REMOVAL_FINAL_AFTER of them -- each counted
 * once -- and the removed epoch's keys go (W22 review B2). */
static char *
later_event(Trio *t, int i)
{
    if (i % 2 == 0) {
        char name[32];
        snprintf(name, sizeof(name), "Later %d", i);
        char *commit = rename_group(&t->bob, &t->gid, name);
        expect_commit(&t->alice, commit, "Alice follows Bob");
        return commit;
    }
    return app_message(&t->bob, &t->gid, "later");
}

static void
test_contested_removal_becomes_final(void)
{
    Trio t;
    trio_init(&t);
    char *rm = NULL;
    OK(marmot_remove_members(t.alice.m, &t.gid, (const uint8_t (*)[32]) t.charlie.pk, 1, &rm));
    merge(&t.alice, &t.gid);
    expect_commit(&t.bob, rm, "Bob applies the removal");
    MarmotError err;
    CHECK(deliver_state(&t.charlie, rm, &err, NULL) == MARMOT_GROUP_STATE_INACTIVE &&
          err == MARMOT_OK, "removed: err=%d", err);
    CHECK(!removal_final(&t.charlie, &t.gid), "contested: Bob could still win");
    for (int i = 0; i < MARMOT_REMOVAL_FINAL_AFTER; i++) {
        char *later = later_event(&t, i);
        deliver(&t.charlie, later, &err, NULL);
        CHECK(err == MARMOT_ERR_USE_AFTER_EVICTION, "later event %d: %d", i, err);
        if (i == 0) {
            deliver(&t.charlie, later, &err, NULL);   /* another relay's copy */
            CHECK(err == MARMOT_ERR_USE_AFTER_EVICTION, "the copy: %d", err);
        }
        bool last = i + 1 == MARMOT_REMOVAL_FINAL_AFTER;
        CHECK(removal_final(&t.charlie, &t.gid) == last, "final after %d later events: %d",
              i + 1, !last);
        CHECK(has_state(&t.charlie, &t.gid, "mls_group") == !last, "keys after %d", i + 1);
        free(later);
    }
    uint8_t secret[32];
    CHECK(t.charlie.m->storage->get_exporter_secret(t.charlie.m->storage->ctx, &t.gid, t.epoch,
                                                    secret) != MARMOT_OK,
          "the removed epoch's exporter secret is gone");
    free(rm);
    trio_clear(&t);
}

/* Finality also waits for the retained parent (review F2): Charlie applied
 * Alice's rename, which Bob could still beat, and then Bob -- whom no admin
 * can beat -- removes him: not final while that parent is kept in full. */
static void
test_removal_final_waits_for_parent(void)
{
    Trio t;
    trio_init(&t);
    char *ren = rename_group(&t.alice, &t.gid, "Alice's");
    expect_commit(&t.bob, ren, "Bob follows Alice");
    expect_commit(&t.charlie, ren, "Charlie follows Alice");
    char *rm = NULL;
    OK(marmot_remove_members(t.bob.m, &t.gid, (const uint8_t (*)[32]) t.charlie.pk, 1, &rm));
    MarmotError err;
    CHECK(deliver_state(&t.charlie, rm, &err, NULL) == MARMOT_GROUP_STATE_INACTIVE &&
          err == MARMOT_OK, "removed: err=%d", err);
    CHECK(!removal_final(&t.charlie, &t.gid), "the parent could still be replaced");
    CHECK(has_state(&t.charlie, &t.gid, MARMOT_MLS_PARENT_LABEL), "the parent is kept");
    free(rm);
    free(ren);
    trio_clear(&t);
}

/* A rival removal that beats the stored one replaces it (review F6):
 * Alice's removal first (contested by Bob), then Bob's of the same epoch. */
static void
test_rival_removal_replaces(void)
{
    Trio t;
    trio_init(&t);
    char *by_alice = NULL, *by_bob = NULL;
    OK(marmot_remove_members(t.alice.m, &t.gid, (const uint8_t (*)[32]) t.charlie.pk, 1,
                             &by_alice));
    OK(marmot_remove_members(t.bob.m, &t.gid, (const uint8_t (*)[32]) t.charlie.pk, 1, &by_bob));
    MarmotError err;
    CHECK(deliver_state(&t.charlie, by_alice, &err, NULL) == MARMOT_GROUP_STATE_INACTIVE &&
          err == MARMOT_OK, "Alice's removal: err=%d", err);
    uint8_t by[32];
    CHECK(removal_of(&t.charlie, &t.gid, by, NULL) && memcmp(by, t.alice.pk, 32) == 0,
          "by Alice first");
    CHECK(deliver_state(&t.charlie, by_bob, &err, NULL) == MARMOT_GROUP_STATE_INACTIVE &&
          err == MARMOT_OK, "Bob's removal: err=%d", err);
    CHECK(removal_of(&t.charlie, &t.gid, by, NULL) && memcmp(by, t.bob.pk, 32) == 0,
          "Bob's removal replaced it");
    CHECK(removal_final(&t.charlie, &t.gid), "nobody can beat Bob's");
    CHECK(!has_state(&t.charlie, &t.gid, "mls_group"), "the keys are gone");
    free(by_alice);
    free(by_bob);
    trio_clear(&t);
}

/* The residual risk of count finality, pinned (W22 review B3; nostrc-6njv):
 * the winner of the removal's epoch arrives after `behind` messages of its
 * own branch, which Charlie cannot open.  Behind 4 (MARMOT_REMOVAL_FINAL_AFTER
 * - 1) it still re-activates him; behind 5 the
 * removal is final first and the winner is refused -- Charlie stays ended
 * while the group, on the winner, keeps his leaf.  Changing the constant
 * must be a visible decision. */
static void
winner_after_its_branch(int behind, bool reactivates)
{
    Trio t;
    trio_init(&t);
    char *rm = NULL;
    OK(marmot_remove_members(t.alice.m, &t.gid, (const uint8_t (*)[32]) t.charlie.pk, 1, &rm));
    char *ren = rename_pending(&t.bob, &t.gid, "Bob's");
    merge(&t.bob, &t.gid);                  /* Bob's rename wins; Bob writes in its epoch */
    MarmotError err;
    CHECK(deliver_state(&t.charlie, rm, &err, NULL) == MARMOT_GROUP_STATE_INACTIVE &&
          err == MARMOT_OK, "the losing removal first: err=%d", err);
    for (int i = 0; i < behind; i++) {
        char *msg = app_message(&t.bob, &t.gid, "on the winner");
        deliver(&t.charlie, msg, &err, NULL);
        CHECK(err == MARMOT_ERR_USE_AFTER_EVICTION, "branch message %d: %d", i, err);
        free(msg);
    }
    uint64_t epoch = 0;
    MarmotGroupState state = deliver_state(&t.charlie, ren, &err, &epoch);
    if (reactivates) {
        CHECK(err == MARMOT_OK && state == MARMOT_GROUP_STATE_ACTIVE && epoch == t.epoch + 1,
              "behind %d: the winner re-activates (err=%d)", behind, err);
        CHECK(!removal_of(&t.charlie, &t.gid, NULL, NULL), "behind %d: not removed", behind);
    } else {
        CHECK(err == MARMOT_ERR_USE_AFTER_EVICTION,
              "behind %d: the winner is refused (err=%d)", behind, err);
        CHECK(removal_of(&t.charlie, &t.gid, NULL, NULL) && removal_final(&t.charlie, &t.gid),
              "behind %d: removed for good", behind);
    }
    free(ren);
    free(rm);
    trio_clear(&t);
}

static void
test_winner_after_its_branch(void)
{
    /* Literal on purpose: the tolerance the README states. */
    winner_after_its_branch(4, true);
    winner_after_its_branch(5, false);
}

int
main(int argc, char **argv)
{
    if (sodium_init() < 0) return 1;
    g_only = argc > 1 ? argv[1] : NULL;
    printf("libmarmot: Commit publication and ingestion (nostrc-9ata)\n");
    RUN(test_sends_use_distinct_generations);
    RUN(test_live_replay_in_new_envelope_rejected);
    RUN(test_failed_receive_keeps_the_message);
    RUN(test_member_cannot_post_as_another);
    RUN(test_inner_event_delivered_once);
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
    RUN(test_group_context_required_capabilities);
    RUN(test_stale_pending_commit_cannot_merge);
    RUN(test_pending_commit_recovered_by_echo);
    RUN(test_merge_idempotent_after_crash);
    RUN(test_failed_merge_stays_clearable);
    RUN(test_outbox_keeps_every_welcome_until_marked);
    RUN(test_duplicate_welcome_does_not_roll_back);
    RUN(test_unauthenticated_events_rejected);
    RUN(test_rumor_path_accepts_unsigned);
    RUN(test_late_messages_use_retained_parent);
    RUN(test_media_source_epoch_of_late_message);
    RUN(test_media_check_epoch_reconciles);
    RUN(test_media_check_epoch_transaction);
    RUN(test_operations_run_in_one_transaction);
    RUN(test_send_stores_step_before_event);
    RUN(test_forged_member_identity_rejected);
    RUN(test_welcome_with_forged_member_rejected);
    RUN(test_welcome_sender_from_seal);
    RUN(test_account_proof_enrollment);
    RUN(test_add_refused_when_joiners_would_reject);
    RUN(test_create_group_needs_enrollment);
    RUN(test_members_prove_existing_leaves_by_self_update);
    RUN(test_retained_parent_retires_once_settled);
    RUN(test_competitor_within_window_still_wins);
    RUN(test_retained_parent_retired_at_once);
    RUN(test_retained_parent_without_trailer_migrates);
    RUN(test_settling_write_failure_keeps_everything);
    RUN(test_witness_must_be_the_listed_member);
    RUN(test_signer_only_key_packages_share_one_leaf_key);
    RUN(test_group_members_follow_the_epoch);
    RUN(test_removed_member_learns_it);
    RUN(test_losing_removal_first_is_undone);
    RUN(test_losing_removal_after_winner);
    RUN(test_previous_epoch_removal_wins);
    RUN(test_previous_epoch_removal_loses);
    RUN(test_final_removal_forgets_keys);
    RUN(test_own_pending_beats_removal);
    RUN(test_deferred_replay_winner_first);
    RUN(test_removal_drops_own_pending);
    RUN(test_corrupt_removal_record);
    RUN(test_contested_removal_becomes_final);
    RUN(test_removal_final_waits_for_parent);
    RUN(test_rival_removal_replaces);
    RUN(test_winner_after_its_branch);
    printf("All commit tests passed\n");
    return 0;
}
