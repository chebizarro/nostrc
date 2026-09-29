/*
 * libmarmot - Commit publication and ingestion (MIP-01/MIP-03, nostrc-9ata)
 *
 * A Commit travels as a kind:445 event exactly like an application message:
 * the MLSMessage (a PublicMessage, Marmot's pinned handshake wire format) is
 * NIP-44-encrypted with the exporter secret of the epoch it was created in.
 *
 * Receivers apply it through the same validated MLS path the producers use
 * (mls_group_process_commit on a private copy of the stored state), then
 * enforce the Marmot policy on the resulting state (marmot_commit_authorize)
 * before anything is written.
 *
 * Epoch handling (Marmot protocol-core/convergence.md, bounded subset):
 *   - source epoch == current epoch: linear advance, applied.
 *   - source epoch == current - 1: the Commit competes with the one we
 *     applied from the same parent.  The parent state and the applied
 *     Commit's ordering key are retained ("mls_group_parent"); the same
 *     bytes are a duplicate, otherwise the lower CommitOrderingSuffix
 *     (privileged < ordinary, then committer, then SHA-256 digest) wins and
 *     replaces the applied Commit.  Transport metadata never takes part.
 *   - anything older, or a competitor that loses: MARMOT_ERR_WRONG_EPOCH.
 *   - future epochs cannot be recovered from the NIP-44 layer (no exporter
 *     secret yet) and are rejected the same way if they ever reach here.
 * Every rejection leaves the stored group untouched.
 *
 * SPDX-License-Identifier: MIT
 */

#include "commits.h"
#include "kp_profile.h"
#include "mls/mls-internal.h"
#include "mls/mls_framing.h"
#include <nostr-event.h>
#include <nostr-tag.h>
#include <secp256k1.h>
#include <sodium.h>
#include <stdlib.h>
#include <string.h>

#define PARENT_LABEL   "mls_group_parent"
#define PARENT_VERSION 1
#define PENDING_LABEL   "mls_group_pending"
#define PENDING_VERSION 2
/* Losing inbound Commits kept while our own Commit awaits a relay. */
#define PENDING_MAX_DEFERRED 16

/* ──────────────────────────────────────────────────────────────────────────
 * Helpers
 * ──────────────────────────────────────────────────────────────────────── */

static void
free_secret(uint8_t *p, size_t len)
{
    if (!p) return;
    sodium_memzero(p, len);
    free(p);
}

/* Deep copy through the persisted form, which carries every field an epoch
 * transition reads (as mls_group.c stages its own Commits). */
static int
mls_clone(const MlsGroup *src, MlsGroup *dst)
{
    uint8_t *blob = NULL;
    size_t len = 0;
    memset(dst, 0, sizeof(*dst));
    int rc = (mls_group_serialize(src, &blob, &len) == 0 &&
              mls_group_deserialize(blob, len, dst) == 0) ? 0 : -1;
    free_secret(blob, len);
    return rc;
}

/* The group's marmot_group_data: *out is NULL when the group has none (a
 * legacy group); more than one, or one that does not parse, is an error. */
static MarmotError
group_data_of(const MlsGroup *g, MarmotGroupDataExtension **out)
{
    *out = NULL;
    const uint8_t *data = NULL;
    size_t len = 0, count = 0;
    if (marmot_extensions_find(g->extensions_data, g->extensions_len,
                               MARMOT_EXTENSION_TYPE, &data, &len, &count) != 0 ||
        count > 1)
        return MARMOT_ERR_EXTENSION_FORMAT;
    if (count == 0) return MARMOT_OK;
    *out = marmot_group_data_extension_deserialize(data, len);
    return *out ? MARMOT_OK : MARMOT_ERR_EXTENSION_FORMAT;
}

/* Admin authority per the pre-Commit GroupData.  Same rule as the producers'
 * is_admin() in groups.c, so every member accepts exactly the Commits a
 * member may produce: a group without admins (legacy) lets anyone commit. */
static bool
gde_is_admin(const MarmotGroupDataExtension *gde, const uint8_t pk[32])
{
    if (!gde || gde->admin_count == 0 || !gde->admins) return true;
    for (size_t i = 0; i < gde->admin_count; i++)
        if (memcmp(gde->admins[i], pk, 32) == 0) return true;
    return false;
}

static const MlsLeafNode *
leaf_at(const MlsGroup *g, uint32_t leaf)
{
    if (leaf >= g->tree.n_leaves) return NULL;
    const MlsNode *n = &g->tree.nodes[mls_tree_leaf_to_node(leaf)];
    return n->type == MLS_NODE_LEAF ? &n->leaf : NULL;
}

static bool
same_identity(const MlsLeafNode *a, const MlsLeafNode *b)
{
    return a->credential_identity_len == b->credential_identity_len &&
           (a->credential_identity_len == 0 ||
            memcmp(a->credential_identity, b->credential_identity,
                   a->credential_identity_len) == 0);
}

/* Lower wins (CommitOrderingSuffix). */
static int
commit_key_cmp(const MarmotCommitKey *a, const MarmotCommitKey *b)
{
    if (a->privileged != b->privileged) return a->privileged ? -1 : 1;
    int c = memcmp(a->committer, b->committer, 32);
    if (c != 0) return c;
    return memcmp(a->digest, b->digest, 32);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Authorization (MIP-01)
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_commit_authorize(const MlsGroup *pre, const MlsGroup *post,
                        uint32_t committer_leaf,
                        MarmotCommitKey *key,
                        MarmotGroupDataExtension **post_gde)
{
    if (!pre || !post || !key || !post_gde) return MARMOT_ERR_INVALID_ARG;
    *post_gde = NULL;
    memset(key, 0, sizeof(*key));

    const MlsLeafNode *committer = leaf_at(pre, committer_leaf);
    if (!committer || committer->credential_identity_len != 32 ||
        !committer->credential_identity)
        return MARMOT_ERR_FROM_NON_MEMBER;
    memcpy(key->committer, committer->credential_identity, 32);

    /* Membership: an added or removed leaf, or a slot that now holds another
     * account (Remove + Add reusing it), makes the Commit privileged.  The
     * committer stays in the group under its own account (the MLS layer
     * already pins the identity of Update and UpdatePath leaves). */
    const MlsLeafNode *committer_after = leaf_at(post, committer_leaf);
    if (!committer_after || !same_identity(committer, committer_after))
        return MARMOT_ERR_IDENTITY_CHANGE;
    bool members_changed = false;
    uint32_t n = pre->tree.n_leaves > post->tree.n_leaves ? pre->tree.n_leaves
                                                          : post->tree.n_leaves;
    for (uint32_t i = 0; i < n && !members_changed; i++) {
        const MlsLeafNode *a = leaf_at(pre, i);
        const MlsLeafNode *b = leaf_at(post, i);
        members_changed = (!a != !b) || (a && !same_identity(a, b));
    }

    bool ext_changed = pre->extensions_len != post->extensions_len ||
                       (pre->extensions_len > 0 &&
                        memcmp(pre->extensions_data, post->extensions_data,
                               pre->extensions_len) != 0);
    key->privileged = members_changed || ext_changed;

    MarmotGroupDataExtension *before = NULL, *after = NULL;
    MarmotError err = group_data_of(pre, &before);
    if (err == MARMOT_OK) err = group_data_of(post, &after);
    if (err == MARMOT_OK && before && !after)
        err = MARMOT_ERR_EXTENSION_FORMAT;          /* GroupData removed */
    if (err == MARMOT_OK && before && after &&
        memcmp(before->nostr_group_id, after->nostr_group_id, 32) != 0)
        err = MARMOT_ERR_PROTOCOL_GROUP_MISMATCH;   /* nostr_group_id is immutable */
    if (err == MARMOT_OK && key->privileged && !gde_is_admin(before, key->committer))
        err = MARMOT_ERR_COMMIT_FROM_NON_ADMIN;
    marmot_group_data_extension_free(before);
    if (err != MARMOT_OK) {
        marmot_group_data_extension_free(after);
        return err;
    }
    *post_gde = after;
    return MARMOT_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Retained parent record
 *
 *   u8  version (1)
 *   u64 parent_epoch
 *   u8  privileged
 *   [32] committer, [32] digest      -- ordering key of the applied Commit
 *   opaque parent_state<V>           -- mls_group_serialize(parent)
 *
 * It holds the parent epoch's secrets for one extra epoch (the rollback
 * horizon libmarmot implements); buffers are wiped after use.
 * ──────────────────────────────────────────────────────────────────────── */

static int
retained_encode(const MlsGroup *parent, const MarmotCommitKey *key,
                uint8_t **out, size_t *out_len)
{
    uint8_t *blob = NULL;
    size_t blob_len = 0;
    if (mls_group_serialize(parent, &blob, &blob_len) != 0) return -1;
    MlsTlsBuf buf;
    int rc = -1;
    if (mls_tls_buf_init(&buf, blob_len + 96) == 0) {
        if (mls_tls_write_u8(&buf, PARENT_VERSION) == 0 &&
            mls_tls_write_u64(&buf, parent->epoch) == 0 &&
            mls_tls_write_u8(&buf, key->privileged ? 1 : 0) == 0 &&
            mls_tls_buf_append(&buf, key->committer, 32) == 0 &&
            mls_tls_buf_append(&buf, key->digest, 32) == 0 &&
            mls_tls_write_opaque32(&buf, blob, blob_len) == 0) {
            *out = buf.data;
            *out_len = buf.len;
            rc = 0;
        } else {
            if (buf.data) sodium_memzero(buf.data, buf.len);
            mls_tls_buf_free(&buf);
        }
    }
    free_secret(blob, blob_len);
    return rc;
}

typedef struct {
    uint64_t        parent_epoch;
    MarmotCommitKey key;
    MlsGroup        parent;
} RetainedParent;

static int
retained_load(Marmot *m, const uint8_t *gid, size_t gid_len, RetainedParent *out)
{
    memset(out, 0, sizeof(*out));
    uint8_t *data = NULL;
    size_t len = 0;
    if (m->storage->mls_load(m->storage->ctx, PARENT_LABEL, gid, gid_len,
                             &data, &len) != MARMOT_OK || !data)
        return -1;
    MlsTlsReader r;
    mls_tls_reader_init(&r, data, len);
    uint8_t version = 0, privileged = 0;
    uint8_t *blob = NULL;
    size_t blob_len = 0;
    int rc = -1;
    if (mls_tls_read_u8(&r, &version) == 0 && version == PARENT_VERSION &&
        mls_tls_read_u64(&r, &out->parent_epoch) == 0 &&
        mls_tls_read_u8(&r, &privileged) == 0 && privileged <= 1 &&
        mls_tls_read_fixed(&r, out->key.committer, 32) == 0 &&
        mls_tls_read_fixed(&r, out->key.digest, 32) == 0 &&
        mls_tls_read_opaque32(&r, &blob, &blob_len) == 0 &&
        mls_tls_reader_done(&r) &&
        mls_group_deserialize(blob, blob_len, &out->parent) == 0) {
        out->key.privileged = privileged == 1;
        rc = out->parent.epoch == out->parent_epoch ? 0 : -1;
        if (rc != 0) mls_group_free(&out->parent);
    }
    free_secret(blob, blob_len);
    free_secret(data, len);
    return rc;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Late application messages (nostrc-qp24.7)
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_commit_decrypt_late(Marmot *m, const MarmotGroupId *gid, uint64_t epoch,
                           const uint8_t *msg, size_t msg_len,
                           uint8_t **out_plaintext, size_t *out_len,
                           uint32_t *out_sender)
{
    if (!m || !gid || !msg || !out_plaintext || !out_len || !out_sender)
        return MARMOT_ERR_INVALID_ARG;
    *out_plaintext = NULL;
    *out_len = 0;
    MarmotStorage *s = m->storage;
    if (!s || !s->mls_load || !s->mls_store) return MARMOT_ERR_STORAGE;
    uint8_t *probe = NULL;
    size_t probe_len = 0;
    MarmotError err = s->mls_load(s->ctx, PARENT_LABEL, gid->data, gid->len,
                                  &probe, &probe_len);
    free_secret(probe, probe_len);
    if (err != MARMOT_OK) return err;   /* incl. STORAGE_NOT_FOUND: none retained */

    RetainedParent rp;
    if (retained_load(m, gid->data, gid->len, &rp) != 0) return MARMOT_ERR_DESERIALIZATION;
    if (rp.parent_epoch != epoch) {
        mls_group_free(&rp.parent);
        return MARMOT_ERR_STORAGE_NOT_FOUND;   /* not the epoch we retain */
    }
    int rc = mls_group_decrypt(&rp.parent, msg, msg_len, out_plaintext, out_len, out_sender);
    if (rc == 0) {
        /* The parent's ratchet moved on (no key is ever used twice): store it
         * in the same transaction as the message. */
        uint8_t *blob = NULL;
        size_t blob_len = 0;
        err = retained_encode(&rp.parent, &rp.key, &blob, &blob_len) == 0
                  ? s->mls_store(s->ctx, PARENT_LABEL, gid->data, gid->len, blob, blob_len)
                  : MARMOT_ERR_SERIALIZATION;
        free_secret(blob, blob_len);
        if (err != MARMOT_OK) {
            free_secret(*out_plaintext, *out_len);
            *out_plaintext = NULL;
            *out_len = 0;
        }
    } else {
        err = rc == MARMOT_ERR_OWN_MESSAGE ? MARMOT_ERR_OWN_MESSAGE : MARMOT_ERR_MLS;
    }
    mls_group_free(&rp.parent);
    return err;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Persistence
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_group_apply_group_data(MarmotGroup *group, const MarmotGroupDataExtension *gde)
{
    char *name = gde->name ? strdup(gde->name) : NULL;
    char *description = gde->description ? strdup(gde->description) : NULL;
    uint8_t (*admins)[32] = NULL;
    if (gde->admin_count > 0 && gde->admins) admins = malloc(gde->admin_count * 32);
    if ((gde->name && !name) || (gde->description && !description) ||
        (gde->admin_count > 0 && gde->admins && !admins)) {
        free(name);
        free(description);
        free(admins);
        return MARMOT_ERR_MEMORY;
    }
    if (admins) memcpy(admins, gde->admins, gde->admin_count * 32);
    free(group->name);
    free(group->description);
    free(group->admin_pubkeys);
    group->name = name;
    group->description = description;
    group->admin_pubkeys = admins;
    group->admin_count = admins ? gde->admin_count : 0;
    return MARMOT_OK;
}

MarmotError
marmot_commit_persist(Marmot *m, const MlsGroup *pre, const MlsGroup *post,
                      const MarmotCommitKey *key,
                      const MarmotGroupDataExtension *post_gde,
                      MarmotGroup *group)
{
    if (!m || !pre || !post || !key || !group) return MARMOT_ERR_INVALID_ARG;
    MarmotStorage *s = m->storage;
    if (!s || !s->mls_store || !s->mls_load || !s->mls_delete ||
        !s->save_exporter_secret || !s->get_exporter_secret ||
        !s->delete_exporter_secret || !s->save_group)
        return MARMOT_ERR_STORAGE;

    const uint8_t *gid = post->group_id;
    size_t gid_len = post->group_id_len;
    MarmotGroupId mgid = marmot_group_id_new(gid, gid_len);

    uint8_t *old_state = NULL, *old_parent = NULL, *new_state = NULL, *new_parent = NULL;
    size_t old_state_len = 0, old_parent_len = 0, new_state_len = 0, new_parent_len = 0;
    uint8_t old_exporter[32];
    bool had_exporter = false, had_parent = false, undo_ok = true;
    MarmotError err = MARMOT_ERR_STORAGE;

    /* What the writes below replace, so a failure can put it back.  Only a
     * record that is really absent (STORAGE_NOT_FOUND) may be "restored" by
     * deleting it; any other read error aborts before anything is written. */
    MarmotError rerr = s->mls_load(s->ctx, "mls_group", gid, gid_len,
                                   &old_state, &old_state_len);
    if (rerr != MARMOT_OK || !old_state) {
        err = rerr == MARMOT_OK ? MARMOT_ERR_STORAGE : rerr;
        goto out;
    }
    rerr = s->mls_load(s->ctx, PARENT_LABEL, gid, gid_len, &old_parent, &old_parent_len);
    if (rerr == MARMOT_OK && old_parent)
        had_parent = true;
    else if (rerr != MARMOT_OK && rerr != MARMOT_ERR_STORAGE_NOT_FOUND) {
        err = rerr;
        goto out;
    }
    rerr = s->get_exporter_secret(s->ctx, &mgid, post->epoch, old_exporter);
    if (rerr == MARMOT_OK)
        had_exporter = true;
    else if (rerr != MARMOT_ERR_STORAGE_NOT_FOUND) {
        err = rerr;
        goto out;
    }

    if (retained_encode(pre, key, &new_parent, &new_parent_len) != 0 ||
        mls_group_serialize(post, &new_state, &new_state_len) != 0) {
        err = MARMOT_ERR_SERIALIZATION;
        goto out;
    }
    if (post_gde) {
        err = marmot_group_apply_group_data(group, post_gde);
        if (err != MARMOT_OK) goto out;
    }
    group->epoch = post->epoch;

    err = s->save_exporter_secret(s->ctx, &mgid, post->epoch,
                                  post->epoch_secrets.exporter_secret);
    if (err != MARMOT_OK) goto undo_exporter;
    err = s->mls_store(s->ctx, PARENT_LABEL, gid, gid_len, new_parent, new_parent_len);
    if (err != MARMOT_OK) goto undo_parent;
    err = s->mls_store(s->ctx, "mls_group", gid, gid_len, new_state, new_state_len);
    if (err != MARMOT_OK) goto undo_state;
    err = s->save_group(s->ctx, group);
    if (err == MARMOT_OK) goto out;

    /* Compensation, newest write first (the backends have no transactions
     * yet, nostrc-qp24.7).  A failed undo leaves storage inconsistent, so the
     * caller gets MARMOT_ERR_STORAGE rather than the original error. */
    undo_ok &= s->mls_store(s->ctx, "mls_group", gid, gid_len,
                            old_state, old_state_len) == MARMOT_OK;
undo_state:
    if (had_parent)
        undo_ok &= s->mls_store(s->ctx, PARENT_LABEL, gid, gid_len,
                                old_parent, old_parent_len) == MARMOT_OK;
    else
        undo_ok &= s->mls_delete(s->ctx, PARENT_LABEL, gid, gid_len) == MARMOT_OK;
undo_parent:
    if (had_exporter)
        undo_ok &= s->save_exporter_secret(s->ctx, &mgid, post->epoch,
                                           old_exporter) == MARMOT_OK;
    else
        undo_ok &= s->delete_exporter_secret(s->ctx, &mgid, post->epoch) == MARMOT_OK;
undo_exporter:
    if (!undo_ok) err = MARMOT_ERR_STORAGE;
out:
    sodium_memzero(old_exporter, sizeof(old_exporter));
    free_secret(old_state, old_state_len);
    free_secret(old_parent, old_parent_len);
    free_secret(new_state, new_state_len);
    free_secret(new_parent, new_parent_len);
    marmot_group_id_free(&mgid);
    return err;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Crash recovery
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_group_reconcile(Marmot *m, MarmotGroup *group)
{
    if (!m || !group) return MARMOT_ERR_INVALID_ARG;
    MarmotStorage *s = m->storage;
    if (!s || !s->mls_load || !s->save_group) return MARMOT_OK;
    uint8_t *blob = NULL;
    size_t len = 0;
    MarmotError err = s->mls_load(s->ctx, "mls_group", group->mls_group_id.data,
                                  group->mls_group_id.len, &blob, &len);
    if (err == MARMOT_ERR_STORAGE_NOT_FOUND) return MARMOT_OK;  /* legacy groups */
    if (err != MARMOT_OK || !blob) return err == MARMOT_OK ? MARMOT_ERR_STORAGE : err;
    MlsGroup mls;
    int rc = mls_group_deserialize(blob, len, &mls);
    free_secret(blob, len);
    if (rc != 0) return MARMOT_ERR_MLS;
    err = MARMOT_OK;
    /* marmot_commit_persist() writes the MLS state before the group record:
     * a record behind the state is a transition interrupted by a crash.
     * The state (and its exporter secret, written before it) is
     * authoritative; bring the record up to it. */
    if (mls.epoch != group->epoch) {
        MarmotGroupDataExtension *gde = NULL;
        err = group_data_of(&mls, &gde);
        if (err == MARMOT_OK && gde) err = marmot_group_apply_group_data(group, gde);
        marmot_group_data_extension_free(gde);
        if (err == MARMOT_OK) {
            group->epoch = mls.epoch;
            err = s->save_group(s->ctx, group);
        }
    }
    mls_group_free(&mls);
    return err;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Outbound event
 * ──────────────────────────────────────────────────────────────────────── */

int
marmot_sign_ephemeral(NostrEvent *event)
{
    if (!event) return -1;
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!ctx) return -1;
    uint8_t sk[32];
    do randombytes_buf(sk, sizeof(sk)); while (!secp256k1_ec_seckey_verify(ctx, sk));
    secp256k1_context_destroy(ctx);
    char *sk_hex = marmot_hex_encode(sk, sizeof(sk));
    sodium_memzero(sk, sizeof(sk));
    if (!sk_hex) return -1;
    int rc = nostr_event_sign(event, sk_hex);
    sodium_memzero(sk_hex, strlen(sk_hex));
    free(sk_hex);
    if (rc != 0 || !nostr_event_check_signature(event)) return -1;
    return 0;
}

char *
marmot_commit_build_event(const uint8_t *commit_msg, size_t commit_len,
                          const uint8_t source_exporter[32],
                          const uint8_t nostr_group_id[32])
{
    if (!commit_msg || commit_len == 0 || !source_exporter || !nostr_group_id)
        return NULL;
    char *content = NULL;
    if (marmot_group_event_encrypt(source_exporter, commit_msg, commit_len,
                                   &content) != 0 || !content)
        return NULL;

    NostrEvent *event = nostr_event_new();
    NostrTags *tags = nostr_tags_new(0);
    char *gid_hex = marmot_hex_encode(nostr_group_id, 32);
    NostrTag *h = gid_hex ? nostr_tag_new("h", gid_hex, NULL) : NULL;
    free(gid_hex);
    char *json = NULL;
    if (event && tags && h) {
        nostr_event_set_kind(event, MARMOT_KIND_GROUP_MESSAGE);
        nostr_event_set_content(event, content);
        nostr_event_set_created_at(event, marmot_now());
        nostr_tags_append(tags, h);
        h = NULL;
        nostr_event_set_tags(event, tags);
        tags = NULL;
        /* MIP-03: a fresh ephemeral key signs every kind:445. */
        if (marmot_sign_ephemeral(event) == 0)
            json = nostr_event_serialize_compact(event);
    }
    if (h) nostr_tag_free(h);
    if (tags) nostr_tags_free(tags);
    if (event) nostr_event_free(event);
    free(content);
    return json;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Pending local Commit (publish before merge)
 *
 *   u8  version (2)
 *   u64 parent_epoch, [32] parent confirmed_transcript_hash
 *                                       -- the exact state it was built on
 *   u8  privileged, [32] committer, [32] digest   -- the Commit's key
 *   opaque post_state<V>                          -- state it produces
 *   opaque event_json<V>                          -- signed kind:445, to republish
 *   u8  welcome count, then per Welcome: [32] recipient, opaque rumor<V>
 *   u8  deferred count, then per deferred inbound Commit:
 *       u64 outer epoch, opaque msg<V>, opaque event_id<V> (hex or empty)
 *
 * A pending Commit is LIVE while the group is still in the state it was
 * built on (review R1: the epoch number alone is not enough -- a winning
 * competitor can replace that state with another of the same epoch), MERGED
 * once its post-state is installed (a crash between persisting it and
 * dropping the record), and STALE otherwise.
 *
 * Welcomes outlive the pending record: on merge they move to the unsent
 * Welcome outbox ("mls_group_welcomes") until the application confirms it
 * sent them (review R2: a merge by relay echo or after a restart must not
 * lose them).
 * ──────────────────────────────────────────────────────────────────────── */

#define OUTBOX_LABEL   "mls_group_welcomes"
#define OUTBOX_VERSION 2
#define PENDING_MAX_WELCOMES 64

typedef struct {
    uint64_t epoch;
    uint8_t *msg;
    size_t   msg_len;
    char    *event_id;
} DeferredCommit;

typedef struct {
    uint64_t        parent_epoch;
    uint8_t         parent_transcript[MLS_HASH_LEN];
    MarmotCommitKey key;
    MlsGroup        post;
    char           *event_json;
    MarmotUnsentWelcome welcomes[PENDING_MAX_WELCOMES];
    size_t          welcome_count;
    DeferredCommit  deferred[PENDING_MAX_DEFERRED];
    size_t          deferred_count;
} PendingCommit;

typedef enum { PENDING_LIVE, PENDING_MERGED, PENDING_STALE } PendingStatus;

static PendingStatus
pending_status(const PendingCommit *p, const MlsGroup *cur)
{
    if (p->parent_epoch == cur->epoch &&
        memcmp(p->parent_transcript, cur->confirmed_transcript_hash, MLS_HASH_LEN) == 0)
        return PENDING_LIVE;
    if (p->post.epoch == cur->epoch &&
        memcmp(p->post.confirmed_transcript_hash, cur->confirmed_transcript_hash,
               MLS_HASH_LEN) == 0)
        return PENDING_MERGED;
    return PENDING_STALE;
}

static void
pending_clear(PendingCommit *p)
{
    mls_group_free(&p->post);
    free(p->event_json);
    for (size_t i = 0; i < p->welcome_count; i++) free(p->welcomes[i].rumor_json);
    for (size_t i = 0; i < p->deferred_count; i++) {
        free(p->deferred[i].msg);
        free(p->deferred[i].event_id);
    }
    sodium_memzero(p, sizeof(*p));
}

static int
write_welcomes(MlsTlsBuf *buf, const MarmotUnsentWelcome *w, size_t count)
{
    if (mls_tls_write_u8(buf, (uint8_t)count) != 0) return -1;
    for (size_t i = 0; i < count; i++) {
        const char *r = w[i].rumor_json ? w[i].rumor_json : "";
        if (mls_tls_buf_append(buf, w[i].recipient, 32) != 0 ||
            mls_tls_write_opaque32(buf, (const uint8_t *)r, strlen(r)) != 0)
            return -1;
    }
    return 0;
}

/* Reads up to `max` Welcomes into `w`; *count covers every slot to free. */
static int
read_welcomes(MlsTlsReader *r, MarmotUnsentWelcome *w, size_t max, size_t *count)
{
    uint8_t n = 0;
    *count = 0;
    if (mls_tls_read_u8(r, &n) != 0 || n > max) return -1;
    for (uint8_t i = 0; i < n; i++) {
        *count = (size_t)i + 1;
        uint8_t *rumor = NULL;
        size_t rumor_len = 0;
        if (mls_tls_read_fixed(r, w[i].recipient, 32) != 0 ||
            mls_tls_read_opaque32(r, &rumor, &rumor_len) != 0)
            return -1;
        w[i].rumor_json = calloc(1, rumor_len + 1);
        if (!w[i].rumor_json) {
            free(rumor);
            return -1;
        }
        if (rumor_len) memcpy(w[i].rumor_json, rumor, rumor_len);
        free(rumor);
    }
    return 0;
}

static MarmotError
pending_store(Marmot *m, const uint8_t *gid, size_t gid_len, const PendingCommit *p)
{
    uint8_t *blob = NULL;
    size_t blob_len = 0;
    if (mls_group_serialize(&p->post, &blob, &blob_len) != 0)
        return MARMOT_ERR_SERIALIZATION;
    MlsTlsBuf buf;
    MarmotError err = MARMOT_ERR_MEMORY;
    if (mls_tls_buf_init(&buf, blob_len + 512) != 0) {
        free_secret(blob, blob_len);
        return err;
    }
    const char *ev = p->event_json ? p->event_json : "";
    bool ok = mls_tls_write_u8(&buf, PENDING_VERSION) == 0 &&
              mls_tls_write_u64(&buf, p->parent_epoch) == 0 &&
              mls_tls_buf_append(&buf, p->parent_transcript, MLS_HASH_LEN) == 0 &&
              mls_tls_write_u8(&buf, p->key.privileged ? 1 : 0) == 0 &&
              mls_tls_buf_append(&buf, p->key.committer, 32) == 0 &&
              mls_tls_buf_append(&buf, p->key.digest, 32) == 0 &&
              mls_tls_write_opaque32(&buf, blob, blob_len) == 0 &&
              mls_tls_write_opaque32(&buf, (const uint8_t *)ev, strlen(ev)) == 0 &&
              write_welcomes(&buf, p->welcomes, p->welcome_count) == 0 &&
              mls_tls_write_u8(&buf, (uint8_t)p->deferred_count) == 0;
    for (size_t i = 0; ok && i < p->deferred_count; i++) {
        const DeferredCommit *d = &p->deferred[i];
        ok = mls_tls_write_u64(&buf, d->epoch) == 0 &&
             mls_tls_write_opaque32(&buf, d->msg, d->msg_len) == 0 &&
             mls_tls_write_opaque8(&buf, (const uint8_t *)d->event_id,
                                   d->event_id ? strlen(d->event_id) : 0) == 0;
    }
    if (ok)
        err = m->storage->mls_store(m->storage->ctx, PENDING_LABEL, gid, gid_len,
                                    buf.data, buf.len);
    sodium_memzero(buf.data, buf.len);
    mls_tls_buf_free(&buf);
    free_secret(blob, blob_len);
    return err;
}

/* MARMOT_ERR_STORAGE_NOT_FOUND when there is no pending Commit. */
static MarmotError
pending_load(Marmot *m, const uint8_t *gid, size_t gid_len, PendingCommit *out)
{
    memset(out, 0, sizeof(*out));
    uint8_t *data = NULL;
    size_t len = 0;
    MarmotError err = m->storage->mls_load(m->storage->ctx, PENDING_LABEL, gid,
                                           gid_len, &data, &len);
    if (err != MARMOT_OK) return err;
    if (!data) return MARMOT_ERR_STORAGE_NOT_FOUND;
    MlsTlsReader r;
    mls_tls_reader_init(&r, data, len);
    uint8_t version = 0, privileged = 0, count = 0;
    uint8_t *blob = NULL, *ev = NULL;
    size_t blob_len = 0, ev_len = 0;
    err = MARMOT_ERR_DESERIALIZATION;
    if (mls_tls_read_u8(&r, &version) == 0 && version == PENDING_VERSION &&
        mls_tls_read_u64(&r, &out->parent_epoch) == 0 &&
        mls_tls_read_fixed(&r, out->parent_transcript, MLS_HASH_LEN) == 0 &&
        mls_tls_read_u8(&r, &privileged) == 0 && privileged <= 1 &&
        mls_tls_read_fixed(&r, out->key.committer, 32) == 0 &&
        mls_tls_read_fixed(&r, out->key.digest, 32) == 0 &&
        mls_tls_read_opaque32(&r, &blob, &blob_len) == 0 &&
        mls_group_deserialize(blob, blob_len, &out->post) == 0 &&
        mls_tls_read_opaque32(&r, &ev, &ev_len) == 0 &&
        (out->event_json = calloc(1, ev_len + 1)) != NULL &&
        read_welcomes(&r, out->welcomes, PENDING_MAX_WELCOMES, &out->welcome_count) == 0 &&
        mls_tls_read_u8(&r, &count) == 0 && count <= PENDING_MAX_DEFERRED) {
        if (ev_len) memcpy(out->event_json, ev, ev_len);
        out->key.privileged = privileged == 1;
        bool ok = true;
        for (uint8_t i = 0; ok && i < count; i++) {
            DeferredCommit *d = &out->deferred[i];
            out->deferred_count = (size_t)i + 1;   /* freed by pending_clear */
            uint8_t *id = NULL;
            size_t id_len = 0;
            ok = mls_tls_read_u64(&r, &d->epoch) == 0 &&
                 mls_tls_read_opaque32(&r, &d->msg, &d->msg_len) == 0 &&
                 mls_tls_read_opaque8(&r, &id, &id_len) == 0;
            if (ok && id_len > 0) {
                d->event_id = calloc(1, id_len + 1);
                ok = d->event_id != NULL;
                if (ok) memcpy(d->event_id, id, id_len);
            }
            free(id);
        }
        if (ok && mls_tls_reader_done(&r)) err = MARMOT_OK;
    }
    free(ev);
    free_secret(blob, blob_len);
    free_secret(data, len);
    if (err != MARMOT_OK) pending_clear(out);
    return err;
}

static MarmotError
pending_delete(Marmot *m, const uint8_t *gid, size_t gid_len)
{
    MarmotError err = m->storage->mls_delete(m->storage->ctx, PENDING_LABEL, gid, gid_len);
    return err == MARMOT_ERR_STORAGE_NOT_FOUND ? MARMOT_OK : err;
}

static MarmotError
load_current(Marmot *m, const MarmotGroupId *gid, MlsGroup *cur)
{
    memset(cur, 0, sizeof(*cur));
    uint8_t *blob = NULL;
    size_t len = 0;
    MarmotError err = m->storage->mls_load(m->storage->ctx, "mls_group", gid->data,
                                           gid->len, &blob, &len);
    if (err == MARMOT_OK && (!blob || mls_group_deserialize(blob, len, cur) != 0))
        err = MARMOT_ERR_MLS;
    free_secret(blob, len);
    return err;
}

/* ── Unsent Welcome outbox ─────────────────────────────────────────────── *
 *
 *   u8  version (2), u32 count, then per Welcome: [32] recipient, opaque rumor<V>
 *
 * Append-only (W17b addendum C2): each merge adds its Welcomes; an entry
 * leaves only when the application confirms its send by id, so a Welcome is
 * never lost to a later merge or to a mark covering entries it never read.
 * The id is SHA-256(recipient || rumor): stable, and equal copies merge.
 */

/* 0 on success; -1 (nothing usable in `out`) when it cannot be computed --
 * callers fail rather than invent an id that would merge distinct Welcomes. */
static int
welcome_id(const uint8_t recipient[32], const char *rumor, uint8_t out[32])
{
    size_t len = rumor ? strlen(rumor) : 0;
    uint8_t *buf = malloc(32 + len);
    if (!buf) return -1;
    memcpy(buf, recipient, 32);
    if (len) memcpy(buf + 32, rumor, len);
    int rc = mls_crypto_hash(out, buf, 32 + len) == 0 ? 0 : -1;
    free(buf);
    return rc;
}

void
marmot_unsent_welcomes_free(MarmotUnsentWelcome *welcomes, size_t count)
{
    if (!welcomes) return;
    for (size_t i = 0; i < count; i++) free(welcomes[i].rumor_json);
    free(welcomes);
}

/* MARMOT_OK with *out NULL when the outbox is empty. */
static MarmotError
outbox_load(Marmot *m, const MarmotGroupId *gid, MarmotUnsentWelcome **out,
            size_t *out_count)
{
    *out = NULL;
    *out_count = 0;
    uint8_t *data = NULL;
    size_t len = 0;
    MarmotError err = m->storage->mls_load(m->storage->ctx, OUTBOX_LABEL, gid->data,
                                           gid->len, &data, &len);
    if (err == MARMOT_ERR_STORAGE_NOT_FOUND || (err == MARMOT_OK && !data)) return MARMOT_OK;
    if (err != MARMOT_OK) return err;
    MlsTlsReader r;
    mls_tls_reader_init(&r, data, len);
    uint8_t version = 0;
    uint32_t count = 0;
    MarmotUnsentWelcome *w = NULL;
    size_t filled = 0;
    err = MARMOT_ERR_DESERIALIZATION;
    if (mls_tls_read_u8(&r, &version) == 0 && version == OUTBOX_VERSION &&
        mls_tls_read_u32(&r, &count) == 0 && count <= len &&
        (count == 0 || (w = calloc(count, sizeof(*w))) != NULL)) {
        bool ok = true;
        for (uint32_t i = 0; ok && i < count; i++) {
            uint8_t *rumor = NULL;
            size_t rumor_len = 0;
            ok = mls_tls_read_fixed(&r, w[i].recipient, 32) == 0 &&
                 mls_tls_read_opaque32(&r, &rumor, &rumor_len) == 0 &&
                 (w[i].rumor_json = calloc(1, rumor_len + 1)) != NULL;
            if (ok) {
                filled = i + 1;
                if (rumor_len) memcpy(w[i].rumor_json, rumor, rumor_len);
                if (welcome_id(w[i].recipient, w[i].rumor_json, w[i].id) != 0) {
                    ok = false;
                    free(rumor);
                    rumor = NULL;
                    err = MARMOT_ERR_MEMORY;
                    break;
                }
            }
            free(rumor);
        }
        if (ok && mls_tls_reader_done(&r)) err = MARMOT_OK;
    }
    free(data);
    if (err != MARMOT_OK || filled == 0) {
        marmot_unsent_welcomes_free(w, filled);
        return err;
    }
    *out = w;
    *out_count = filled;
    return MARMOT_OK;
}

/* Replace the outbox with `w` (deleted when empty). */
static MarmotError
outbox_store(Marmot *m, const MarmotGroupId *gid, const MarmotUnsentWelcome *w,
             size_t count)
{
    if (count == 0) {
        MarmotError err = m->storage->mls_delete(m->storage->ctx, OUTBOX_LABEL,
                                                 gid->data, gid->len);
        return err == MARMOT_ERR_STORAGE_NOT_FOUND ? MARMOT_OK : err;
    }
    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, 256) != 0) return MARMOT_ERR_MEMORY;
    bool ok = mls_tls_write_u8(&buf, OUTBOX_VERSION) == 0 &&
              mls_tls_write_u32(&buf, (uint32_t)count) == 0;
    for (size_t i = 0; ok && i < count; i++) {
        const char *r = w[i].rumor_json ? w[i].rumor_json : "";
        ok = mls_tls_buf_append(&buf, w[i].recipient, 32) == 0 &&
             mls_tls_write_opaque32(&buf, (const uint8_t *)r, strlen(r)) == 0;
    }
    MarmotError err = ok ? m->storage->mls_store(m->storage->ctx, OUTBOX_LABEL,
                                                 gid->data, gid->len, buf.data, buf.len)
                         : MARMOT_ERR_MEMORY;
    mls_tls_buf_free(&buf);
    return err;
}

/* Append `add` to the outbox, skipping Welcomes already in it. */
static MarmotError
outbox_append(Marmot *m, const MarmotGroupId *gid, const MarmotUnsentWelcome *add,
              size_t add_count)
{
    MarmotUnsentWelcome *cur = NULL;
    size_t cur_count = 0;
    MarmotError err = outbox_load(m, gid, &cur, &cur_count);
    if (err != MARMOT_OK) return err;
    MarmotUnsentWelcome *all = calloc(cur_count + add_count, sizeof(*all));
    if (!all) {
        marmot_unsent_welcomes_free(cur, cur_count);
        return MARMOT_ERR_MEMORY;
    }
    size_t n = 0;
    for (size_t i = 0; i < cur_count; i++) all[n++] = cur[i];   /* borrowed */
    for (size_t i = 0; i < add_count; i++) {
        MarmotUnsentWelcome e = add[i];
        if (welcome_id(e.recipient, e.rumor_json, e.id) != 0) {
            free(all);
            marmot_unsent_welcomes_free(cur, cur_count);
            return MARMOT_ERR_MEMORY;
        }
        bool dup = false;
        for (size_t j = 0; j < n && !dup; j++) dup = memcmp(all[j].id, e.id, 32) == 0;
        if (!dup) all[n++] = e;
    }
    err = outbox_store(m, gid, all, n);
    free(all);
    marmot_unsent_welcomes_free(cur, cur_count);
    return err;
}

MarmotError
marmot_commit_get_unsent_welcomes(Marmot *m, const MarmotGroupId *gid,
                                  MarmotUnsentWelcome **out, size_t *out_count)
{
    return outbox_load(m, gid, out, out_count);
}

MarmotError
marmot_commit_mark_welcomes_sent(Marmot *m, const MarmotGroupId *gid,
                                 const uint8_t (*ids)[32], size_t id_count)
{
    if (id_count > 0 && !ids) return MARMOT_ERR_INVALID_ARG;
    MarmotUnsentWelcome *cur = NULL;
    size_t cur_count = 0;
    MarmotError err = outbox_load(m, gid, &cur, &cur_count);
    if (err != MARMOT_OK || cur_count == 0) return err;
    MarmotUnsentWelcome *keep = calloc(cur_count, sizeof(*keep));
    if (!keep) {
        marmot_unsent_welcomes_free(cur, cur_count);
        return MARMOT_ERR_MEMORY;
    }
    size_t n = 0;
    for (size_t i = 0; i < cur_count; i++) {
        bool sent = false;
        for (size_t j = 0; j < id_count && !sent; j++)
            sent = memcmp(cur[i].id, ids[j], 32) == 0;
        if (!sent) keep[n++] = cur[i];   /* borrowed */
    }
    err = n == cur_count ? MARMOT_OK : outbox_store(m, gid, keep, n);
    free(keep);
    marmot_unsent_welcomes_free(cur, cur_count);
    return err;
}

/* The pending Commit is applied: hand its Welcomes to the outbox, then drop
 * the record.  A failed delete leaves a MERGED record that the next access
 * finishes the same way. */
static MarmotError
pending_finish_merged(Marmot *m, const MarmotGroupId *gid, const PendingCommit *p)
{
    if (p->welcome_count > 0) {
        MarmotError err = outbox_append(m, gid, p->welcomes, p->welcome_count);
        if (err != MARMOT_OK) return err;
    }
    (void)pending_delete(m, gid->data, gid->len);
    return MARMOT_OK;
}

MarmotError
marmot_commit_stage_pending(Marmot *m, const MlsGroup *pre, const MlsGroup *post,
                            const uint8_t *commit, size_t commit_len,
                            const char *event_json,
                            const MarmotUnsentWelcome *welcomes, size_t welcome_count)
{
    if (!m || !pre || !post || !commit || commit_len == 0 || !event_json ||
        welcome_count > PENDING_MAX_WELCOMES || (welcome_count && !welcomes))
        return MARMOT_ERR_INVALID_ARG;
    PendingCommit p;
    memset(&p, 0, sizeof(p));
    MarmotGroupDataExtension *gde = NULL;
    /* The same policy every receiver applies: never publish what the group
     * rejects. */
    MarmotError err = marmot_commit_authorize(pre, post, pre->own_leaf_index,
                                              &p.key, &gde);
    marmot_group_data_extension_free(gde);
    if (err != MARMOT_OK) return err;
    if (mls_crypto_hash(p.key.digest, commit, commit_len) != 0) return MARMOT_ERR_CRYPTO;
    p.parent_epoch = pre->epoch;
    memcpy(p.parent_transcript, pre->confirmed_transcript_hash, MLS_HASH_LEN);
    /* pending_store() only reads these: shallow views are enough. */
    p.post = *post;
    p.event_json = (char *)event_json;
    if (welcome_count > 0) /* welcomes may be NULL when there are none (UBSAN) */
        memcpy(p.welcomes, welcomes, welcome_count * sizeof(*welcomes));
    p.welcome_count = welcome_count;
    err = pending_store(m, post->group_id, post->group_id_len, &p);
    sodium_memzero(&p, sizeof(p));
    return err;
}

MarmotError
marmot_commit_get_pending(Marmot *m, MarmotGroup *group, char **out_event_json,
                          bool *out_live)
{
    const MarmotGroupId *gid = &group->mls_group_id;
    *out_event_json = NULL;
    *out_live = false;
    PendingCommit p;
    MarmotError err = pending_load(m, gid->data, gid->len, &p);
    if (err == MARMOT_ERR_STORAGE_NOT_FOUND) return MARMOT_OK;
    if (err != MARMOT_OK) return err;
    MlsGroup cur;
    err = load_current(m, gid, &cur);
    if (err == MARMOT_OK) {
        PendingStatus st = pending_status(&p, &cur);
        if (st == PENDING_MERGED) {
            err = pending_finish_merged(m, gid, &p);   /* nothing pending any more */
        } else {
            *out_live = st == PENDING_LIVE;
            *out_event_json = p.event_json;
            p.event_json = NULL;
        }
        mls_group_free(&cur);
    }
    pending_clear(&p);
    return err;
}

MarmotError
marmot_commit_has_pending(Marmot *m, MarmotGroup *group, bool *out)
{
    char *ev = NULL;
    bool live = false;
    MarmotError err = marmot_commit_get_pending(m, group, &ev, &live);
    *out = err == MARMOT_OK && ev != NULL;
    free(ev);
    return err;
}

/* Apply the loaded LIVE pending Commit `p` on top of `cur`. */
static MarmotError
pending_apply(Marmot *m, MarmotGroup *group, const MlsGroup *cur, const PendingCommit *p)
{
    MarmotCommitKey key;
    MarmotGroupDataExtension *gde = NULL;
    MarmotError err = marmot_commit_authorize(cur, &p->post, cur->own_leaf_index,
                                              &key, &gde);
    if (err != MARMOT_OK) {
        /* It can never apply: drop it rather than wedge the group. */
        if (pending_delete(m, group->mls_group_id.data, group->mls_group_id.len) == MARMOT_OK)
            marmot_txn_keep(m);   /* the drop is the outcome, not a failure */
        return err;
    }
    memcpy(key.digest, p->key.digest, 32);
    err = marmot_commit_persist(m, cur, &p->post, &key, gde, group);
    marmot_group_data_extension_free(gde);
    /* On a storage error the record stays: merge again or clear it. */
    if (err == MARMOT_OK) err = pending_finish_merged(m, &group->mls_group_id, p);
    return err;
}

MarmotError
marmot_commit_merge_pending(Marmot *m, MarmotGroup *group)
{
    const MarmotGroupId *gid = &group->mls_group_id;
    PendingCommit p;
    MarmotError err = pending_load(m, gid->data, gid->len, &p);
    if (err != MARMOT_OK) return err;   /* includes STORAGE_NOT_FOUND */
    MlsGroup cur;
    err = load_current(m, gid, &cur);
    if (err == MARMOT_OK) {
        switch (pending_status(&p, &cur)) {
        case PENDING_LIVE:
            err = pending_apply(m, group, &cur, &p);
            break;
        case PENDING_MERGED:
            /* Persisted before a crash (or by our relay echo). */
            err = pending_finish_merged(m, gid, &p);
            break;
        case PENDING_STALE:
            /* The state it was built on was replaced: another member's
             * Commit won.  Its deferred Commits were built on that state too. */
            err = pending_delete(m, gid->data, gid->len);
            if (err == MARMOT_OK) {
                marmot_txn_keep(m);   /* dropped for good: keep that */
                err = MARMOT_ERR_WRONG_EPOCH;
            }
            break;
        }
        mls_group_free(&cur);
    }
    pending_clear(&p);
    return err;
}

MarmotError
marmot_commit_clear_pending(Marmot *m, MarmotGroup *group)
{
    const MarmotGroupId *gid = &group->mls_group_id;
    PendingCommit p;
    MarmotError err = pending_load(m, gid->data, gid->len, &p);
    if (err == MARMOT_ERR_STORAGE_NOT_FOUND) return MARMOT_OK;
    if (err != MARMOT_OK) return err;
    MlsGroup cur;
    err = load_current(m, gid, &cur);
    if (err == MARMOT_OK && pending_status(&p, &cur) == PENDING_MERGED) {
        /* Already applied: there is nothing to discard; keep its Welcomes. */
        err = pending_finish_merged(m, gid, &p);
    } else if (err == MARMOT_OK) {
        err = pending_delete(m, gid->data, gid->len);
        /* Commits that lost only to ours now compete among themselves. */
        for (size_t i = 0; err == MARMOT_OK && i < p.deferred_count; i++) {
            MarmotMessageResult r;
            memset(&r, 0, sizeof(r));
            (void)marmot_commit_process_inbound(m, group, p.deferred[i].epoch,
                                                p.deferred[i].msg,
                                                p.deferred[i].msg_len,
                                                p.deferred[i].event_id, &r);
            marmot_message_result_free(&r);
        }
    }
    mls_group_free(&cur);   /* zeroed by load_current() even on failure */
    pending_clear(&p);
    return err;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Inbound
 * ──────────────────────────────────────────────────────────────────────── */

/* Apply `msg` from `sender` to a copy of `parent`, then authorize it. */
static MarmotError
stage_inbound(const MlsGroup *parent, const uint8_t *msg, size_t msg_len,
              uint32_t sender, MlsGroup *post, MarmotCommitKey *key,
              MarmotGroupDataExtension **gde)
{
    if (mls_clone(parent, post) != 0) return MARMOT_ERR_MLS;
    int rc = mls_group_process_commit(post, msg, msg_len, sender);
    if (rc != 0) {
        mls_group_free(post);
        if (rc == MARMOT_ERR_UNSUPPORTED || rc == MARMOT_ERR_OWN_COMMIT_PENDING ||
            rc == MARMOT_ERR_MEMORY)
            return (MarmotError)rc;
        return MARMOT_ERR_MLS_PROCESS_MESSAGE;
    }
    MarmotError err = marmot_commit_authorize(parent, post, sender, key, gde);
    if (err != MARMOT_OK) mls_group_free(post);
    return err;
}

/* Keep an inbound Commit that lost to our pending one, for
 * marmot_clear_pending_commit(). */
static MarmotError
defer_inbound(Marmot *m, PendingCommit *p, const uint8_t *gid, size_t gid_len,
              uint64_t epoch, const uint8_t *msg, size_t msg_len,
              const uint8_t digest[32], const char *event_id_hex)
{
    for (size_t i = 0; i < p->deferred_count; i++) {
        uint8_t d[32];
        if (mls_crypto_hash(d, p->deferred[i].msg, p->deferred[i].msg_len) == 0 &&
            memcmp(d, digest, 32) == 0)
            return MARMOT_ERR_OWN_COMMIT_PENDING;   /* already kept */
    }
    if (p->deferred_count == PENDING_MAX_DEFERRED) return MARMOT_ERR_OWN_COMMIT_PENDING;
    DeferredCommit *d = &p->deferred[p->deferred_count];
    d->msg = malloc(msg_len);
    d->event_id = event_id_hex ? strdup(event_id_hex) : NULL;
    if (!d->msg || (event_id_hex && !d->event_id)) {
        free(d->msg);
        free(d->event_id);
        memset(d, 0, sizeof(*d));
        return MARMOT_ERR_MEMORY;
    }
    memcpy(d->msg, msg, msg_len);
    d->msg_len = msg_len;
    d->epoch = epoch;
    p->deferred_count++;
    MarmotError err = pending_store(m, gid, gid_len, p);
    if (err != MARMOT_OK) return err;
    marmot_txn_keep(m);   /* kept for marmot_clear_pending_commit() */
    return MARMOT_ERR_OWN_COMMIT_PENDING;
}

static void
fill_commit_result(Marmot *m, MarmotGroup *group, MarmotMessageResult *result)
{
    result->type = MARMOT_RESULT_COMMIT;
    result->commit.updated_group = NULL;
    (void)m->storage->find_group_by_mls_id(m->storage->ctx, &group->mls_group_id,
                                           &result->commit.updated_group);
}

MarmotError
marmot_commit_process_inbound(Marmot *m, MarmotGroup *group,
                              uint64_t outer_epoch,
                              const uint8_t *msg, size_t msg_len,
                              const char *event_id_hex,
                              MarmotMessageResult *result)
{
    if (!m || !group || !msg || !result) return MARMOT_ERR_INVALID_ARG;
    MarmotStorage *s = m->storage;
    if (!s || !s->mls_load || !s->find_group_by_mls_id) return MARMOT_ERR_STORAGE;

    /* Authenticated header fields are checked by mls_group_process_commit;
     * here they only route the Commit. */
    MlsMLSMessage wm;
    MlsTlsReader r;
    mls_tls_reader_init(&r, msg, msg_len);
    if (mls_message_deserialize(&r, &wm) != 0) return MARMOT_ERR_MLS_FRAMING;
    bool is_proposal = wm.wire_format == MLS_WIRE_FORMAT_PUBLIC_MESSAGE &&
                       wm.public_message.content.content_type == MLS_CONTENT_TYPE_PROPOSAL;
    bool shape_ok = mls_tls_reader_done(&r) &&
                    wm.wire_format == MLS_WIRE_FORMAT_PUBLIC_MESSAGE &&
                    wm.public_message.content.content_type == MLS_CONTENT_TYPE_COMMIT &&
                    wm.public_message.content.sender.sender_type == MLS_SENDER_TYPE_MEMBER;
    bool group_ok = wm.public_message.content.group_id_len == group->mls_group_id.len &&
                    memcmp(wm.public_message.content.group_id, group->mls_group_id.data,
                           group->mls_group_id.len) == 0;
    uint64_t epoch = wm.public_message.content.epoch;
    uint32_t sender = wm.public_message.content.sender.leaf_index;
    mls_message_clear(&wm);
    /* Standalone proposals are not queued yet: Commits carry them inline. */
    if (is_proposal) return MARMOT_ERR_UNSUPPORTED;
    if (!shape_ok) return MARMOT_ERR_MLS_FRAMING;
    if (!group_ok) return MARMOT_ERR_WRONG_GROUP_ID;
    /* The Commit is sealed under its own epoch's exporter secret. */
    if (epoch != outer_epoch) return MARMOT_ERR_WRONG_EPOCH;

    MarmotCommitKey key;
    uint8_t digest[32];
    if (mls_crypto_hash(digest, msg, msg_len) != 0) return MARMOT_ERR_CRYPTO;

    const uint8_t *gid = group->mls_group_id.data;
    size_t gid_len = group->mls_group_id.len;
    MlsGroup cur;
    if (load_current(m, &group->mls_group_id, &cur) != MARMOT_OK)
        return MARMOT_ERR_MLS;

    MarmotError err;
    MlsGroup post;
    memset(&post, 0, sizeof(post));
    MarmotGroupDataExtension *gde = NULL;

    if (epoch == cur.epoch) {
        /* Linear advance of the current epoch -- unless our own Commit built
         * on exactly this state awaits a relay: then the two compete now. */
        PendingCommit p;
        MarmotError perr = pending_load(m, gid, gid_len, &p);
        if (perr != MARMOT_OK && perr != MARMOT_ERR_STORAGE_NOT_FOUND) {
            mls_group_free(&cur);
            return perr;   /* unreadable pending state: fail closed */
        }
        bool live = perr == MARMOT_OK && pending_status(&p, &cur) == PENDING_LIVE;
        if (perr == MARMOT_OK && memcmp(digest, p.key.digest, 32) == 0) {
            /* Our own pending Commit, back from a relay: a relay stored it,
             * so it is published -- merge it (review R2; this also recovers
             * a committer that crashed or lost the relay's OK). */
            err = live ? pending_apply(m, group, &cur, &p) : MARMOT_ERR_WRONG_EPOCH;
            pending_clear(&p);
            mls_group_free(&cur);
            if (err != MARMOT_OK) return err;
            fill_commit_result(m, group, result);
            return MARMOT_OK;
        }
        err = stage_inbound(&cur, msg, msg_len, sender, &post, &key, &gde);
        if (err == MARMOT_OK) {
            memcpy(key.digest, digest, 32);
            if (live && commit_key_cmp(&key, &p.key) >= 0)
                err = defer_inbound(m, &p, gid, gid_len, epoch, msg, msg_len,
                                    digest, event_id_hex);
            else
                err = marmot_commit_persist(m, &cur, &post, &key, gde, group);
            /* A winner replaces the state our pending Commit was built on:
             * from now on it is STALE and merging it fails (review R1). */
        }
        if (perr == MARMOT_OK) pending_clear(&p);
    } else if (cur.epoch > 0 && epoch == cur.epoch - 1) {
        /* A Commit from the parent of the one we applied. */
        RetainedParent rp;
        if (retained_load(m, gid, gid_len, &rp) != 0) {
            err = MARMOT_ERR_WRONG_EPOCH;
        } else {
            if (rp.parent_epoch != epoch) {
                err = MARMOT_ERR_WRONG_EPOCH;
            } else if (memcmp(digest, rp.key.digest, 32) == 0) {
                /* The Commit we applied (e.g. our own, echoed by a relay). */
                mls_group_free(&rp.parent);
                mls_group_free(&cur);
                result->type = MARMOT_RESULT_OWN_MESSAGE;
                return MARMOT_OK;
            } else if (sender == rp.parent.own_leaf_index) {
                err = MARMOT_ERR_WRONG_EPOCH;   /* not the Commit we made */
            } else {
                err = stage_inbound(&rp.parent, msg, msg_len, sender, &post,
                                    &key, &gde);
                if (err == MARMOT_OK) {
                    memcpy(key.digest, digest, 32);
                    /* Replacing the applied Commit also replaces the state
                     * any pending Commit of ours was built on: that one
                     * becomes STALE (bound to the replaced transcript). */
                    if (commit_key_cmp(&key, &rp.key) < 0)
                        err = marmot_commit_persist(m, &rp.parent, &post, &key,
                                                    gde, group);
                    else
                        err = MARMOT_ERR_WRONG_EPOCH;   /* the applied Commit wins */
                }
            }
            mls_group_free(&rp.parent);
        }
    } else {
        err = MARMOT_ERR_WRONG_EPOCH;   /* stale, or from an epoch we lack */
    }

    mls_group_free(&post);
    mls_group_free(&cur);
    marmot_group_data_extension_free(gde);
    if (err != MARMOT_OK) return err;

    /* Best effort: the digest check above catches a re-delivery anyway. */
    if (event_id_hex && s->save_processed_message) {
        uint8_t id[32];
        if (strlen(event_id_hex) == 64 && marmot_hex_decode(event_id_hex, id, 32) == 0)
            (void)s->save_processed_message(s->ctx, id, id, marmot_now(), epoch,
                                            &group->mls_group_id,
                                            MARMOT_MSG_STATE_PROCESSED, NULL);
    }
    fill_commit_result(m, group, result);
    return MARMOT_OK;
}
