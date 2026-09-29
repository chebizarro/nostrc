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
#include <sodium.h>
#include <stdlib.h>
#include <string.h>

#define PARENT_LABEL   "mls_group_parent"
#define PARENT_VERSION 1

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
    bool had_exporter = false, had_parent = false;
    MarmotError err = MARMOT_ERR_STORAGE;

    /* What the writes below replace, so a failure can put it back. */
    if (s->mls_load(s->ctx, "mls_group", gid, gid_len, &old_state, &old_state_len)
            != MARMOT_OK || !old_state)
        goto out;
    had_parent = s->mls_load(s->ctx, PARENT_LABEL, gid, gid_len,
                             &old_parent, &old_parent_len) == MARMOT_OK && old_parent;
    had_exporter = s->get_exporter_secret(s->ctx, &mgid, post->epoch,
                                          old_exporter) == MARMOT_OK;

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

    /* Compensation, newest write first (best effort: the backends have no
     * transactions yet, nostrc-qp24.7). */
    s->mls_store(s->ctx, "mls_group", gid, gid_len, old_state, old_state_len);
undo_state:
    if (had_parent)
        s->mls_store(s->ctx, PARENT_LABEL, gid, gid_len, old_parent, old_parent_len);
    else
        s->mls_delete(s->ctx, PARENT_LABEL, gid, gid_len);
undo_parent:
    if (had_exporter)
        s->save_exporter_secret(s->ctx, &mgid, post->epoch, old_exporter);
    else
        s->delete_exporter_secret(s->ctx, &mgid, post->epoch);
undo_exporter:
    ;
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
 * Outbound event
 * ──────────────────────────────────────────────────────────────────────── */

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
        json = nostr_event_serialize_compact(event);
    }
    if (h) nostr_tag_free(h);
    if (tags) nostr_tags_free(tags);
    if (event) nostr_event_free(event);
    free(content);
    return json;
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

    uint8_t *cur_blob = NULL;
    size_t cur_len = 0;
    MlsGroup cur;
    memset(&cur, 0, sizeof(cur));
    if (s->mls_load(s->ctx, "mls_group", group->mls_group_id.data,
                    group->mls_group_id.len, &cur_blob, &cur_len) != MARMOT_OK ||
        !cur_blob || mls_group_deserialize(cur_blob, cur_len, &cur) != 0) {
        free_secret(cur_blob, cur_len);
        return MARMOT_ERR_MLS;
    }
    free_secret(cur_blob, cur_len);

    MarmotError err;
    MlsGroup post;
    memset(&post, 0, sizeof(post));
    MarmotGroupDataExtension *gde = NULL;

    if (epoch == cur.epoch) {
        /* Linear advance of the current epoch. */
        err = stage_inbound(&cur, msg, msg_len, sender, &post, &key, &gde);
        if (err == MARMOT_OK) {
            memcpy(key.digest, digest, 32);
            err = marmot_commit_persist(m, &cur, &post, &key, gde, group);
        }
    } else if (cur.epoch > 0 && epoch == cur.epoch - 1) {
        /* A Commit from the parent of the one we applied. */
        RetainedParent rp;
        if (retained_load(m, group->mls_group_id.data, group->mls_group_id.len,
                          &rp) != 0) {
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

    result->type = MARMOT_RESULT_COMMIT;
    result->commit.updated_group = NULL;
    (void)s->find_group_by_mls_id(s->ctx, &group->mls_group_id,
                                  &result->commit.updated_group);
    return MARMOT_OK;
}
