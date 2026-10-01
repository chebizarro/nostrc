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
 *     The parent is kept in full only while a competing Commit could still
 *     win (nostrc-yuj2): see "Retained parent record" below.
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

#define PARENT_LABEL   MARMOT_MLS_PARENT_LABEL
#define PARENT_VERSION 1
#define PENDING_LABEL   "mls_group_pending"
#define PENDING_VERSION 2
/* Losing inbound Commits kept while our own Commit awaits a relay. */
#define PENDING_MAX_DEFERRED 16
/* Who removed our leaf (nostrc-xrya): u8 version, u64 epoch (the one the
 * removing Commit left), u8 flags (1 from the retained parent, 2 final),
 * [32] committer, [32] digest; since version 3, u8 n and n event ids ([32]
 * each) of later-epoch events seen (W22 review B2).  The key is privileged
 * (a Remove).  Version 2 (this branch's first record) reads as n = 0. */
#define REMOVED_LABEL   MARMOT_MLS_REMOVED_LABEL
#define REMOVED_VERSION 3
#define REMOVED_V2_LEN  (1 + 8 + 1 + 32 + 32)
#define REMOVED_FROM_PARENT 1
#define REMOVED_FINAL       2

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
 * legacy group); more than one, or one that does not parse, is an error.
 * `stored` is the state we loaded from storage that `g` is (or derives from):
 * only bytes it already holds may be in the libmarmot 0.10.0 layout, which
 * groups made by those versions keep; GroupData a peer's Commit wrote must
 * be MIP-01 (nostrc-c7ho). */
static MarmotError
group_data_of(const MlsGroup *g, const MlsGroup *stored, MarmotGroupDataExtension **out)
{
    *out = NULL;
    const uint8_t *data = NULL, *base = NULL;
    size_t len = 0, count = 0, base_len = 0, base_count = 0;
    if (marmot_extensions_find(g->extensions_data, g->extensions_len,
                               MARMOT_EXTENSION_TYPE, &data, &len, &count) != 0 ||
        count > 1)
        return MARMOT_ERR_EXTENSION_FORMAT;
    if (count == 0) return MARMOT_OK;
    bool ours = g == stored ||
                (marmot_extensions_find(stored->extensions_data, stored->extensions_len,
                                        MARMOT_EXTENSION_TYPE, &base, &base_len,
                                        &base_count) == 0 &&
                 base_count == 1 && base_len == len && memcmp(base, data, len) == 0);
    *out = ours ? marmot_group_data_extension_deserialize_stored(data, len)
                : marmot_group_data_extension_deserialize(data, len);
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

int
marmot_mls_sender_identity(const MlsGroup *g, uint32_t leaf, uint8_t out[32])
{
    const MlsLeafNode *n = g ? leaf_at(g, leaf) : NULL;
    if (!n || n->credential_identity_len != 32 || !n->credential_identity) return -1;
    memcpy(out, n->credential_identity, 32);
    return 0;
}

static bool
same_identity(const MlsLeafNode *a, const MlsLeafNode *b)
{
    return a->credential_identity_len == b->credential_identity_len &&
           (a->credential_identity_len == 0 ||
            memcmp(a->credential_identity, b->credential_identity,
                   a->credential_identity_len) == 0);
}

/* The same LeafNode, as far as its account binding goes (the signature
 * covers the rest; the MLS layer verified every new one). */
static bool
same_leaf(const MlsLeafNode *a, const MlsLeafNode *b)
{
    return a->signature_len == b->signature_len &&
           memcmp(a->signature, b->signature, a->signature_len) == 0 &&
           memcmp(a->signature_key, b->signature_key, MLS_SIG_PK_LEN) == 0 &&
           same_identity(a, b) && a->extensions_len == b->extensions_len &&
           (a->extensions_len == 0 ||
            memcmp(a->extensions_data, b->extensions_data, a->extensions_len) == 0);
}

/* nostrc-7vyi: `after` (the leaf a Commit left in a slot that held `before`,
 * NULL for a new slot) is bound to the account its credential names.  An
 * unchanged leaf was checked when it joined. */
static MarmotError
leaf_binding_check(const MlsLeafNode *before, const MlsLeafNode *after, bool allow_unproven)
{
    if (before && same_leaf(before, after)) return MARMOT_OK;
    switch (marmot_leaf_proof_status(after, MARMOT_CIPHERSUITE)) {
    case MARMOT_LEAF_PROOF_VALID:
        return MARMOT_OK;
    case MARMOT_LEAF_PROOF_INVALID:
        return MARMOT_ERR_KEY_PACKAGE_IDENTITY;
    case MARMOT_LEAF_PROOF_ABSENT:
        break;
    }
    /* The same member's new leaf: the MLS layer pinned its identity; it may
     * stay unproven, but not drop a proof (account-identity-proof-v2.md,
     * "Lifecycle"). */
    if (before && same_identity(before, after))
        return marmot_leaf_proof_status(before, MARMOT_CIPHERSUITE) == MARMOT_LEAF_PROOF_ABSENT
                   ? MARMOT_OK : MARMOT_ERR_KEY_PACKAGE_IDENTITY;
    /* A new identity claim: only the account's own proof supports it. */
    return allow_unproven ? MARMOT_OK : MARMOT_ERR_KEY_PACKAGE_IDENTITY;
}

MarmotError
marmot_tree_members_bound(const MlsGroup *g, uint32_t exempt, bool allow_unproven)
{
    if (!g) return MARMOT_ERR_INVALID_ARG;
    for (uint32_t i = 0; i < g->tree.n_leaves; i++) {
        const MlsLeafNode *leaf = leaf_at(g, i);
        if (!leaf || i == g->own_leaf_index) continue;
        switch (marmot_leaf_proof_status(leaf, MARMOT_CIPHERSUITE)) {
        case MARMOT_LEAF_PROOF_VALID:
            continue;
        case MARMOT_LEAF_PROOF_INVALID:
            return MARMOT_ERR_KEY_PACKAGE_IDENTITY;
        case MARMOT_LEAF_PROOF_ABSENT:
            if (i == exempt || allow_unproven) continue;
            return MARMOT_ERR_KEY_PACKAGE_IDENTITY;
        }
    }
    return MARMOT_OK;
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
                        uint32_t committer_leaf, bool allow_unproven,
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
    key->committer_leaf = committer_leaf;

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
    MarmotError err = group_data_of(pre, pre, &before);
    if (err == MARMOT_OK) err = group_data_of(post, pre, &after);
    if (err == MARMOT_OK && before && !after)
        err = MARMOT_ERR_EXTENSION_FORMAT;          /* GroupData removed */
    if (err == MARMOT_OK && before && after &&
        memcmp(before->nostr_group_id, after->nostr_group_id, 32) != 0)
        err = MARMOT_ERR_PROTOCOL_GROUP_MISMATCH;   /* nostr_group_id is immutable */
    if (err == MARMOT_OK && key->privileged && !gde_is_admin(before, key->committer))
        err = MARMOT_ERR_COMMIT_FROM_NON_ADMIN;
    /* Every account the Commit brings in is its own (nostrc-7vyi). */
    for (uint32_t i = 0; err == MARMOT_OK && i < post->tree.n_leaves; i++) {
        const MlsLeafNode *b = leaf_at(post, i);
        if (b) err = leaf_binding_check(leaf_at(pre, i), b, allow_unproven);
    }
    marmot_group_data_extension_free(before);
    if (err != MARMOT_OK) {
        marmot_group_data_extension_free(after);
        return err;
    }
    *post_gde = after;
    return MARMOT_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Retained parent record (nostrc-yuj2)
 *
 *   u8  version (1)
 *   u64 parent_epoch
 *   u8  privileged
 *   [32] committer, [32] digest      -- ordering key of the applied Commit
 *   opaque parent_state<V>           -- mls_group_serialize(parent)
 *   since 0.10.0:
 *   u8  tier                         -- 0 convergence, 1 reader
 *   u32 pending_count, then u32 leaf -- (convergence) parent leaves that could
 *                                       still publish a winning competitor
 *                                       and were not yet seen in the new
 *                                       epoch; ascending
 *
 * The trailer extends version 1 without changing its prefix, which
 * applications may read (Groundhog's store tests check version and epoch).
 * The parent serves two purposes (Marmot protocol-core/retained-history.md,
 * "Retained cryptographic material"): judging and applying a competing
 * Commit, which needs its init secret, membership key and private path
 * keys; and reading late application messages of its epoch, which needs only
 * its secret tree.  The first also re-derives the current epoch from the
 * relay-visible Commit, every key of it included (libmarmot-w19 review B1).
 * So that part is kept only while a competitor could still matter:
 *
 * - CONVERGENCE: the full state.  `pending` holds the parent leaves whose
 *   account could still publish a Commit from the parent that beats the
 *   applied one (CommitOrderingSuffix: an admin, for a privileged Commit
 *   with a key not above the committer's; an admin or a key not above the
 *   committer's otherwise), other than our own leaf and the committer's.
 *   A leaf leaves `pending` once an application message from it decrypts in
 *   the new epoch (marmot_commit_note_witness()): that member applied the
 *   Commit, and any Commit of its own from the parent lost to it.
 * - READER: once `pending` is empty -- at once when nobody could win --
 *   mls_group_strip_to_reader() keeps only what reads late messages.  A
 *   competitor then fails with MARMOT_ERR_WRONG_EPOCH.
 *
 * The next Commit replaces the record either way.  No wall-clock bound:
 * Marmot convergence never depends on local time, and members that retired
 * at different moments would disagree about a late winning competitor.
 *
 * A record without the trailer (libmarmot 0.5.0-0.9.0) loads as CONVERGENCE
 * with every parent leaf that could win pending (the committer's leaf
 * unknown).  Libmarmot 0.9.0 and older cannot read a record with the
 * trailer: after a downgrade, late messages and competitors of that epoch
 * fail (closed) until the next Commit.
 * ──────────────────────────────────────────────────────────────────────── */

#define PARENT_TIER_CONVERGENCE 0
#define PARENT_TIER_READER      1

typedef struct {
    uint64_t        parent_epoch;
    MarmotCommitKey key;
    uint8_t         tier;
    uint32_t       *pending;
    size_t          n_pending;
    MlsGroup        parent;
} RetainedParent;

static void
retained_clear(RetainedParent *rp)
{
    mls_group_free(&rp->parent);
    free(rp->pending);
    memset(rp, 0, sizeof(*rp));
}

/* Could the account `id` publish a Commit from `parent` that beats `key`? */
static bool
could_win(const MarmotGroupDataExtension *gde, const uint8_t id[32],
          const MarmotCommitKey *key)
{
    int cmp = memcmp(id, key->committer, 32);
    bool admin = gde_is_admin(gde, id);
    return key->privileged ? (admin && cmp <= 0) : (admin || cmp <= 0);
}

/* The CONVERGENCE `pending` list for `parent` and the applied Commit `key`
 * (`committer_leaf` UINT32_MAX when unknown). */
static int
retained_pending_compute(const MlsGroup *parent, const MarmotCommitKey *key,
                         uint32_t committer_leaf, uint32_t **out, size_t *n_out)
{
    *out = NULL;
    *n_out = 0;
    MarmotGroupDataExtension *gde = NULL;
    /* An unreadable GroupData: count every member as a possible winner. */
    bool gde_ok = group_data_of(parent, parent, &gde) == MARMOT_OK;
    uint32_t *list = parent->tree.n_leaves ? calloc(parent->tree.n_leaves, sizeof(*list)) : NULL;
    if (parent->tree.n_leaves && !list) {
        marmot_group_data_extension_free(gde);
        return -1;
    }
    size_t n = 0;
    for (uint32_t i = 0; i < parent->tree.n_leaves; i++) {
        const MlsLeafNode *leaf = leaf_at(parent, i);
        if (!leaf || i == parent->own_leaf_index || i == committer_leaf ||
            leaf->credential_identity_len != 32 || !leaf->credential_identity)
            continue;   /* blank, us, the committer, or no account to commit */
        if (!gde_ok || could_win(gde, leaf->credential_identity, key))
            list[n++] = i;
    }
    marmot_group_data_extension_free(gde);
    *out = list;
    *n_out = n;
    return 0;
}

/* CONVERGENCE with nothing pending becomes READER. */
static void
retained_settle(RetainedParent *rp)
{
    if (rp->tier == PARENT_TIER_CONVERGENCE && rp->n_pending == 0) {
        mls_group_strip_to_reader(&rp->parent);
        rp->tier = PARENT_TIER_READER;
    }
}

static int
retained_encode(const RetainedParent *rp, uint8_t **out, size_t *out_len)
{
    uint8_t *blob = NULL;
    size_t blob_len = 0;
    if (mls_group_serialize(&rp->parent, &blob, &blob_len) != 0) return -1;
    MlsTlsBuf buf;
    int rc = -1;
    if (mls_tls_buf_init(&buf, blob_len + 128 + 4 * rp->n_pending) == 0) {
        bool ok = mls_tls_write_u8(&buf, PARENT_VERSION) == 0 &&
                  mls_tls_write_u64(&buf, rp->parent_epoch) == 0 &&
                  mls_tls_write_u8(&buf, rp->key.privileged ? 1 : 0) == 0 &&
                  mls_tls_buf_append(&buf, rp->key.committer, 32) == 0 &&
                  mls_tls_buf_append(&buf, rp->key.digest, 32) == 0 &&
                  mls_tls_write_opaque32(&buf, blob, blob_len) == 0 &&
                  mls_tls_write_u8(&buf, rp->tier) == 0 &&
                  mls_tls_write_u32(&buf, (uint32_t)rp->n_pending) == 0;
        for (size_t i = 0; ok && i < rp->n_pending; i++)
            ok = mls_tls_write_u32(&buf, rp->pending[i]) == 0;
        if (ok) {
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

static int
retained_decode(const uint8_t *data, size_t len, RetainedParent *out)
{
    memset(out, 0, sizeof(*out));
    MlsTlsReader r;
    mls_tls_reader_init(&r, data, len);
    uint8_t version = 0, privileged = 0;
    uint8_t *blob = NULL;
    size_t blob_len = 0;
    bool ok = mls_tls_read_u8(&r, &version) == 0 && version == PARENT_VERSION &&
              mls_tls_read_u64(&r, &out->parent_epoch) == 0 &&
              mls_tls_read_u8(&r, &privileged) == 0 && privileged <= 1 &&
              mls_tls_read_fixed(&r, out->key.committer, 32) == 0 &&
              mls_tls_read_fixed(&r, out->key.digest, 32) == 0 &&
              mls_tls_read_opaque32(&r, &blob, &blob_len) == 0;
    out->key.privileged = privileged == 1;
    out->key.committer_leaf = UINT32_MAX;
    bool legacy = ok && mls_tls_reader_done(&r);   /* no trailer: 0.9.0 or older */
    uint32_t n = 0;
    if (ok && !legacy) {
        ok = mls_tls_read_u8(&r, &out->tier) == 0 && out->tier <= PARENT_TIER_READER &&
             mls_tls_read_u32(&r, &n) == 0 && n <= mls_tls_reader_remaining(&r) / 4 &&
             (out->tier == PARENT_TIER_CONVERGENCE || n == 0);
        if (ok && n > 0) {
            out->pending = calloc(n, sizeof(*out->pending));
            ok = out->pending != NULL;
            for (uint32_t i = 0; ok && i < n; i++)
                ok = mls_tls_read_u32(&r, &out->pending[i]) == 0 &&
                     (i == 0 || out->pending[i] > out->pending[i - 1]);
            out->n_pending = n;
        }
        ok = ok && mls_tls_reader_done(&r);
    }
    ok = ok && mls_group_deserialize(blob, blob_len, &out->parent) == 0;
    free_secret(blob, blob_len);
    if (ok) {
        ok = out->parent.epoch == out->parent_epoch;
        for (size_t i = 0; ok && i < out->n_pending; i++)
            ok = out->pending[i] < out->parent.tree.n_leaves;
    }
    if (ok && legacy) {
        out->tier = PARENT_TIER_CONVERGENCE;
        ok = retained_pending_compute(&out->parent, &out->key, UINT32_MAX, &out->pending,
                                      &out->n_pending) == 0;
    }
    if (!ok) {
        retained_clear(out);
        return -1;
    }
    return 0;
}

static int
retained_load(Marmot *m, const uint8_t *gid, size_t gid_len, RetainedParent *out)
{
    memset(out, 0, sizeof(*out));
    uint8_t *data = NULL;
    size_t len = 0;
    if (m->storage->mls_load(m->storage->ctx, PARENT_LABEL, gid, gid_len,
                             &data, &len) != MARMOT_OK || !data)
        return -1;
    int rc = retained_decode(data, len, out);
    free_secret(data, len);
    return rc;
}

/* The record for `parent`, the state the Commit with `key` was applied to. */
static int
retained_new_encoded(const MlsGroup *parent, const MarmotCommitKey *key,
                     uint8_t **out, size_t *out_len)
{
    RetainedParent rp;
    memset(&rp, 0, sizeof(rp));
    rp.parent_epoch = parent->epoch;
    rp.key = *key;
    rp.tier = PARENT_TIER_CONVERGENCE;
    int rc = mls_clone(parent, &rp.parent) == 0 &&
             retained_pending_compute(parent, key, key->committer_leaf, &rp.pending,
                                      &rp.n_pending) == 0
                 ? 0 : -1;
    if (rc == 0) {
        retained_settle(&rp);
        rc = retained_encode(&rp, out, out_len);
    }
    retained_clear(&rp);
    return rc;
}

MarmotError
marmot_commit_note_witness(Marmot *m, const MlsGroup *cur, uint32_t sender_leaf,
                           uint8_t **out_replaced, size_t *out_replaced_len)
{
    if (!m || !cur || !out_replaced || !out_replaced_len) return MARMOT_ERR_INVALID_ARG;
    *out_replaced = NULL;
    *out_replaced_len = 0;
    MarmotStorage *s = m->storage;
    if (!s || !s->mls_load || !s->mls_store) return MARMOT_ERR_STORAGE;
    uint8_t *probe = NULL;
    size_t probe_len = 0;
    MarmotError err = s->mls_load(s->ctx, PARENT_LABEL, cur->group_id, cur->group_id_len,
                                  &probe, &probe_len);
    if (err == MARMOT_ERR_STORAGE_NOT_FOUND || (err == MARMOT_OK && !probe)) {
        free_secret(probe, probe_len);
        return MARMOT_OK;
    }
    if (err != MARMOT_OK) return err;
    RetainedParent rp;
    if (retained_decode(probe, probe_len, &rp) != 0) {
        /* Unreadable: there is nothing to judge a competitor with anyway. */
        free_secret(probe, probe_len);
        return MARMOT_OK;
    }
    size_t at = rp.n_pending;
    for (size_t i = 0; i < rp.n_pending; i++)
        if (rp.pending[i] == sender_leaf) at = i;
    const MlsLeafNode *before = leaf_at(&rp.parent, sender_leaf);
    const MlsLeafNode *now = leaf_at(cur, sender_leaf);
    /* The same member (account and leaf key) the parent listed. */
    if (rp.tier == PARENT_TIER_CONVERGENCE && rp.parent_epoch + 1 == cur->epoch &&
        at < rp.n_pending && before && now && same_identity(before, now) &&
        memcmp(before->signature_key, now->signature_key, MLS_SIG_PK_LEN) == 0) {
        memmove(&rp.pending[at], &rp.pending[at + 1],
                (rp.n_pending - at - 1) * sizeof(*rp.pending));
        rp.n_pending--;
        retained_settle(&rp);
        uint8_t *blob = NULL;
        size_t blob_len = 0;
        err = retained_encode(&rp, &blob, &blob_len) == 0
                  ? s->mls_store(s->ctx, PARENT_LABEL, cur->group_id, cur->group_id_len,
                                 blob, blob_len)
                  : MARMOT_ERR_SERIALIZATION;
        free_secret(blob, blob_len);
        if (err == MARMOT_OK) {
            *out_replaced = probe;
            *out_replaced_len = probe_len;
            probe = NULL;
        }
    }
    free_secret(probe, probe_len);
    retained_clear(&rp);
    return err;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Late application messages (nostrc-qp24.7)
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_commit_decrypt_late(Marmot *m, const MarmotGroupId *gid, uint64_t epoch,
                           const uint8_t *msg, size_t msg_len,
                           uint8_t **out_plaintext, size_t *out_len,
                           uint32_t *out_sender,
                           uint8_t out_sender_identity[32],
                           uint8_t **out_replaced, size_t *out_replaced_len)
{
    if (!m || !gid || !msg || !out_plaintext || !out_len || !out_sender ||
        !out_sender_identity || !out_replaced || !out_replaced_len)
        return MARMOT_ERR_INVALID_ARG;
    *out_plaintext = NULL;
    *out_len = 0;
    *out_replaced = NULL;
    *out_replaced_len = 0;
    MarmotStorage *s = m->storage;
    if (!s || !s->mls_load || !s->mls_store) return MARMOT_ERR_STORAGE;
    /* The record as it is: handed back once replaced (see commits.h). */
    uint8_t *probe = NULL;
    size_t probe_len = 0;
    MarmotError err = s->mls_load(s->ctx, PARENT_LABEL, gid->data, gid->len,
                                  &probe, &probe_len);
    if (err != MARMOT_OK || !probe) {
        free_secret(probe, probe_len);
        return err != MARMOT_OK ? err : MARMOT_ERR_STORAGE_NOT_FOUND;
    }

    RetainedParent rp;
    if (retained_decode(probe, probe_len, &rp) != 0) {
        free_secret(probe, probe_len);
        return MARMOT_ERR_DESERIALIZATION;
    }
    if (rp.parent_epoch != epoch) {
        free_secret(probe, probe_len);
        retained_clear(&rp);
        return MARMOT_ERR_STORAGE_NOT_FOUND;   /* not the epoch we retain */
    }
    int rc = mls_group_decrypt(&rp.parent, msg, msg_len, out_plaintext, out_len, out_sender);
    if (rc == 0 &&
        marmot_mls_sender_identity(&rp.parent, *out_sender, out_sender_identity) != 0) {
        /* A leaf without an account identity cannot author anything. */
        free_secret(*out_plaintext, *out_len);
        *out_plaintext = NULL;
        *out_len = 0;
        rc = MARMOT_ERR_AUTHOR_MISMATCH;
    }
    if (rc == 0) {
        /* The parent's ratchet moved on (no key is ever used twice): store it
         * in the same transaction as the message, in either tier. */
        uint8_t *blob = NULL;
        size_t blob_len = 0;
        err = retained_encode(&rp, &blob, &blob_len) == 0
                  ? s->mls_store(s->ctx, PARENT_LABEL, gid->data, gid->len, blob, blob_len)
                  : MARMOT_ERR_SERIALIZATION;
        free_secret(blob, blob_len);
        if (err != MARMOT_OK) {
            free_secret(*out_plaintext, *out_len);
            *out_plaintext = NULL;
            *out_len = 0;
        } else {
            *out_replaced = probe;
            *out_replaced_len = probe_len;
            probe = NULL;
        }
    } else {
        err = (rc == MARMOT_ERR_OWN_MESSAGE || rc == MARMOT_ERR_AUTHOR_MISMATCH)
                  ? (MarmotError)rc : MARMOT_ERR_MLS;
    }
    free_secret(probe, probe_len);
    retained_clear(&rp);
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

    if (retained_new_encoded(pre, key, &new_parent, &new_parent_len) != 0 ||
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
        err = group_data_of(&mls, &mls, &gde);
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
                          const uint8_t nostr_group_id[32], int64_t created_at)
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
        nostr_event_set_created_at(event, created_at);
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
                                              m->config.allow_unproven_members,
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
                                              m->config.allow_unproven_members,
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

static void deferred_order(const Marmot *m, const MlsGroup *cur, const PendingCommit *p,
                           size_t *order);

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
        /* Commits that lost only to ours now compete among themselves:
         * winner first (review B1), so that no loser -- a removal of our
         * leaf, say -- is judged before the Commit that beats it. */
        size_t order[PENDING_MAX_DEFERRED];
        deferred_order(m, &cur, &p, order);
        for (size_t k = 0; err == MARMOT_OK && k < p.deferred_count; k++) {
            const DeferredCommit *d = &p.deferred[order[k]];
            MarmotMessageResult r;
            memset(&r, 0, sizeof(r));
            (void)marmot_commit_process_inbound(m, group, d->epoch, d->msg, d->msg_len,
                                                d->event_id, &r);
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
stage_inbound(const Marmot *m, const MlsGroup *parent, const uint8_t *msg, size_t msg_len,
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
    MarmotError err = marmot_commit_authorize(parent, post, sender,
                                              m->config.allow_unproven_members, key, gde);
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

/* nostrc-xrya: the ordering key and authority of an authenticated Commit that
 * removes our leaf (mls_group_commit_removes_self()).  A Remove is
 * privileged: the committer must be an admin of the pre-Commit GroupData,
 * exactly as marmot_commit_authorize() demands of a Commit we could apply. */
static MarmotError
removal_key(const MlsGroup *pre, uint32_t committer_leaf, const uint8_t digest[32],
            MarmotCommitKey *key)
{
    memset(key, 0, sizeof(*key));
    const MlsLeafNode *committer = leaf_at(pre, committer_leaf);
    if (!committer || committer->credential_identity_len != 32 ||
        !committer->credential_identity)
        return MARMOT_ERR_FROM_NON_MEMBER;
    memcpy(key->committer, committer->credential_identity, 32);
    key->committer_leaf = committer_leaf;
    key->privileged = true;
    memcpy(key->digest, digest, 32);
    MarmotGroupDataExtension *gde = NULL;
    MarmotError err = group_data_of(pre, pre, &gde);
    if (err == MARMOT_OK && !gde_is_admin(gde, key->committer))
        err = MARMOT_ERR_COMMIT_FROM_NON_ADMIN;
    marmot_group_data_extension_free(gde);
    return err;
}

static void
fill_commit_result(Marmot *m, MarmotGroup *group, MarmotMessageResult *result)
{
    result->type = MARMOT_RESULT_COMMIT;
    result->commit.updated_group = NULL;
    (void)m->storage->find_group_by_mls_id(m->storage->ctx, &group->mls_group_id,
                                           &result->commit.updated_group);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Removal of our own leaf (nostrc-xrya; W22 review B1)
 *
 * A Commit that removes us cannot be applied: its UpdatePath is encrypted
 * to the remaining members.  An admin's authenticated one ends the group
 * for us -- but only if it wins its epoch.  Convergence is judged by the
 * CommitOrderingSuffix, not by arrival: a competing Commit that beats the
 * removal (another admin's privileged Commit with a lower key) is the epoch
 * the others enter, with our leaf still in it.  So the removal keeps its
 * ordering key and the state it was judged on, and while any admin could
 * still beat it (removal_contested()) -- or, for a removal of the current
 * epoch, the Commit that led there could still lose to a competitor from
 * its parent -- the group, though inactive, still judges that epoch's
 * Commits (marmot_process_message() lets Commits of such a group through):
 *   - a Commit that keeps us and beats it re-activates the group, which
 *     enters that Commit's epoch, and the removal is forgotten;
 *   - a removal that beats it replaces it (someone else removed us);
 *   - anything else is MARMOT_ERR_WRONG_EPOCH.
 * A removal nobody can beat is final at once: the removed epoch's secrets
 * (the MLS state, the retained parent, the exporter secrets) are deleted,
 * so a stolen store no longer opens them (review N1).  The group record
 * and the application's history stay.
 *
 * Limits (review N2): the removed member cannot check what needs the new
 * epoch -- the confirmation tag, the UpdatePath, the post-Commit policy --
 * so an admin can end the group for us alone with a removal the others
 * reject; an admin can remove us anyway.
 * ──────────────────────────────────────────────────────────────────────── */

typedef struct {
    uint64_t        epoch;        /* the epoch the removing Commit left */
    bool            from_parent;  /* judged on the retained parent (it beat our Commit) */
    bool            final;        /* nobody can beat it any more */
    MarmotCommitKey key;          /* privileged */
    uint8_t         n_later;      /* later-epoch events seen (distinct ids) */
    uint8_t         later[MARMOT_REMOVAL_FINAL_AFTER][32];
} Removal;

static MarmotError
removal_store(Marmot *m, const uint8_t *gid, size_t gid_len, const Removal *rm)
{
    uint8_t rec[REMOVED_V2_LEN + 1 + MARMOT_REMOVAL_FINAL_AFTER * 32];
    rec[0] = REMOVED_VERSION;
    for (int i = 0; i < 8; i++) rec[1 + i] = (uint8_t)(rm->epoch >> (56 - 8 * i));
    rec[9] = (uint8_t)((rm->from_parent ? REMOVED_FROM_PARENT : 0) |
                       (rm->final ? REMOVED_FINAL : 0));
    memcpy(rec + 10, rm->key.committer, 32);
    memcpy(rec + 42, rm->key.digest, 32);
    rec[REMOVED_V2_LEN] = rm->n_later;
    memcpy(rec + REMOVED_V2_LEN + 1, rm->later, (size_t)rm->n_later * 32);
    size_t len = REMOVED_V2_LEN + 1 + (size_t)rm->n_later * 32;
    return m->storage->mls_store(m->storage->ctx, REMOVED_LABEL, gid, gid_len, rec, len);
}

/* MARMOT_ERR_STORAGE_NOT_FOUND when there is none; MARMOT_ERR_DESERIALIZATION
 * for a record that is not one. */
static MarmotError
removal_load(Marmot *m, const uint8_t *gid, size_t gid_len, Removal *out)
{
    memset(out, 0, sizeof(*out));
    if (!m->storage || !m->storage->mls_load) return MARMOT_ERR_STORAGE;
    uint8_t *rec = NULL;
    size_t len = 0;
    MarmotError err = m->storage->mls_load(m->storage->ctx, REMOVED_LABEL, gid, gid_len,
                                           &rec, &len);
    if (err != MARMOT_OK) return err;
    bool ok = rec && len >= REMOVED_V2_LEN &&
              (rec[9] & ~(REMOVED_FROM_PARENT | REMOVED_FINAL)) == 0;
    if (ok && rec[0] == 2) {
        ok = len == REMOVED_V2_LEN;
    } else if (ok && rec[0] == REMOVED_VERSION) {
        ok = len > REMOVED_V2_LEN && rec[REMOVED_V2_LEN] <= MARMOT_REMOVAL_FINAL_AFTER &&
             len == REMOVED_V2_LEN + 1 + (size_t)rec[REMOVED_V2_LEN] * 32;
        if (ok) {
            out->n_later = rec[REMOVED_V2_LEN];
            memcpy(out->later, rec + REMOVED_V2_LEN + 1, (size_t)out->n_later * 32);
        }
    } else {
        ok = false;
    }
    if (!ok) {
        free(rec);
        memset(out, 0, sizeof(*out));
        return MARMOT_ERR_DESERIALIZATION;
    }
    for (int i = 0; i < 8; i++) out->epoch = (out->epoch << 8) | rec[1 + i];
    out->from_parent = (rec[9] & REMOVED_FROM_PARENT) != 0;
    out->final = (rec[9] & REMOVED_FINAL) != 0;
    out->key.privileged = true;
    out->key.committer_leaf = UINT32_MAX;
    memcpy(out->key.committer, rec + 10, 32);
    memcpy(out->key.digest, rec + 42, 32);
    free(rec);
    return MARMOT_OK;
}

static MarmotError
removal_delete(Marmot *m, const uint8_t *gid, size_t gid_len)
{
    MarmotError err = m->storage->mls_delete(m->storage->ctx, REMOVED_LABEL, gid, gid_len);
    return err == MARMOT_ERR_STORAGE_NOT_FOUND ? MARMOT_OK : err;
}

/* Could another member of `base` publish a Commit of its epoch that beats
 * the removal `key` (yuj2's could_win() for a privileged key: an admin whose
 * key sorts below the remover's)?  Neither we nor the remover count: we
 * cannot commit any more, and a remover racing its own removal is out of
 * scope.  An unreadable GroupData counts everyone. */
static bool
removal_contested(const MlsGroup *base, const MarmotCommitKey *key)
{
    MarmotGroupDataExtension *gde = NULL;
    bool gde_ok = group_data_of(base, base, &gde) == MARMOT_OK;
    bool contested = false;
    for (uint32_t i = 0; i < base->tree.n_leaves && !contested; i++) {
        const MlsLeafNode *leaf = leaf_at(base, i);
        if (!leaf || i == base->own_leaf_index || leaf->credential_identity_len != 32 ||
            !leaf->credential_identity ||
            memcmp(leaf->credential_identity, key->committer, 32) == 0)
            continue;
        contested = !gde_ok || could_win(gde, leaf->credential_identity, key);
    }
    marmot_group_data_extension_free(gde);
    return contested;
}

/* A removal of the current epoch (`on_parent` false) also stands only once
 * the Commit that led to that epoch cannot lose to a competitor from its
 * parent any more (`rp` NULL: no parent kept, or READER). */
static bool
removal_final(const MlsGroup *base, const MarmotCommitKey *key, bool on_parent,
              const RetainedParent *rp)
{
    if (removal_contested(base, key)) return false;
    return on_parent || !rp || rp->tier == PARENT_TIER_READER;
}

/* N1: the removed epoch's secrets go once the removal is final. */
static MarmotError
forget_keys(Marmot *m, const MarmotGroup *group)
{
    MarmotStorage *s = m->storage;
    const uint8_t *gid = group->mls_group_id.data;
    size_t gid_len = group->mls_group_id.len;
    MarmotError err = s->mls_delete(s->ctx, "mls_group", gid, gid_len);
    if (err == MARMOT_ERR_STORAGE_NOT_FOUND) err = MARMOT_OK;
    if (err == MARMOT_OK) {
        err = s->mls_delete(s->ctx, PARENT_LABEL, gid, gid_len);
        if (err == MARMOT_ERR_STORAGE_NOT_FOUND) err = MARMOT_OK;
    }
    for (uint64_t e = 0; err == MARMOT_OK && s->delete_exporter_secret &&
                         e <= group->epoch + 1; e++) {
        err = s->delete_exporter_secret(s->ctx, &group->mls_group_id, e);
        if (err == MARMOT_ERR_STORAGE_NOT_FOUND) err = MARMOT_OK;
    }
    return err;
}

/* Our leaf is removed by `key`'s Commit, which left `epoch` and was judged
 * on `base` (the current state, or the retained parent when `on_parent`):
 * the group turns inactive (as marmot_leave_group() makes it), our pending
 * Commit, if any, is dropped (it can never merge), and the removal is kept
 * for marmot_get_group_removal() -- with its key, so that a winning
 * competitor can still undo it (see above).  In the caller's transaction. */
static MarmotError
evict(Marmot *m, MarmotGroup *group, const MlsGroup *base, bool on_parent,
      const MarmotCommitKey *key, uint64_t epoch)
{
    MarmotStorage *s = m->storage;
    if (!s->mls_store || !s->mls_delete || !s->save_group) return MARMOT_ERR_STORAGE;
    const uint8_t *gid = group->mls_group_id.data;
    size_t gid_len = group->mls_group_id.len;
    RetainedParent rp;
    bool have_rp = !on_parent && retained_load(m, gid, gid_len, &rp) == 0;
    Removal rm = { .epoch = epoch, .from_parent = on_parent, .key = *key };
    rm.final = removal_final(base, key, on_parent, have_rp ? &rp : NULL);
    if (have_rp) retained_clear(&rp);
    MarmotError err = removal_store(m, gid, gid_len, &rm);
    if (err == MARMOT_OK) {
        err = s->mls_delete(s->ctx, PENDING_LABEL, gid, gid_len);
        if (err == MARMOT_ERR_STORAGE_NOT_FOUND) err = MARMOT_OK;
    }
    if (err == MARMOT_OK) {
        group->state = MARMOT_GROUP_STATE_INACTIVE;
        err = s->save_group(s->ctx, group);
    }
    if (err == MARMOT_OK && rm.final) err = forget_keys(m, group);
    return err;
}

MarmotError
marmot_commit_clear_removal(Marmot *m, const MarmotGroupId *gid)
{
    if (!m || !gid || !m->storage) return MARMOT_ERR_STORAGE;
    if (!m->storage->mls_delete) return MARMOT_OK;   /* nothing could have been kept */
    return removal_delete(m, gid->data, gid->len);
}

MarmotError
marmot_get_group_removal(Marmot *m, const MarmotGroupId *mls_group_id, bool *out_removed,
                         uint8_t out_remover[32], uint64_t *out_epoch, bool *out_final)
{
    if (!m || !mls_group_id || !out_removed) return MARMOT_ERR_INVALID_ARG;
    *out_removed = false;
    if (out_epoch) *out_epoch = 0;
    if (out_final) *out_final = false;
    Removal rm;
    MarmotError err = removal_load(m, mls_group_id->data, mls_group_id->len, &rm);
    if (err == MARMOT_ERR_STORAGE_NOT_FOUND) return MARMOT_OK;
    if (err != MARMOT_OK) return err;
    *out_removed = true;
    if (out_remover) memcpy(out_remover, rm.key.committer, 32);
    if (out_epoch) *out_epoch = rm.epoch;
    if (out_final) *out_final = rm.final;
    return MARMOT_OK;
}

MarmotError
marmot_commit_removal_note_later(Marmot *m, MarmotGroup *group, const char *event_id_hex,
                                 bool *out_final)
{
    if (out_final) *out_final = false;
    if (!m || !group || !event_id_hex || strlen(event_id_hex) != 64)
        return MARMOT_ERR_INVALID_ARG;
    uint8_t id[32];
    if (marmot_hex_decode(event_id_hex, id, 32) != 0) return MARMOT_ERR_INVALID_ARG;
    const uint8_t *gid = group->mls_group_id.data;
    size_t gid_len = group->mls_group_id.len;
    Removal rm;
    MarmotError err = removal_load(m, gid, gid_len, &rm);
    if (err != MARMOT_OK) return err;
    if (rm.final) {
        if (out_final) *out_final = true;
        return MARMOT_OK;
    }
    for (uint8_t i = 0; i < rm.n_later; i++)
        if (memcmp(rm.later[i], id, 32) == 0) return MARMOT_OK;   /* counted already */
    memcpy(rm.later[rm.n_later++], id, 32);
    rm.final = rm.n_later >= MARMOT_REMOVAL_FINAL_AFTER;
    if (rm.final) rm.n_later = 0;   /* no longer needed */
    err = removal_store(m, gid, gid_len, &rm);
    if (err == MARMOT_OK && rm.final) err = forget_keys(m, group);
    if (err == MARMOT_OK && out_final) *out_final = rm.final;
    return err;
}

/* The routing header of a handshake MLSMessage; nothing in it is
 * authenticated yet. A Commit comes as a PublicMessage, or as a
 * PrivateMessage (RFC 9420 section 6.3; OpenMLS MIXED_CIPHERTEXT, how MDK
 * sends them, nostrc-7gx7): a PublicMessage names its sender, a
 * PrivateMessage's is resolved on the state that judges it
 * (commit_sender_on()). */
typedef struct {
    bool     commit;     /* one whole MLSMessage: a Commit by a member */
    bool     proposal;   /* a standalone Proposal */
    bool     group_ok;   /* of the group routed to */
    uint64_t epoch;
    uint32_t named;      /* a PublicMessage's sender leaf, else UINT32_MAX */
} CommitRoute;

static bool
commit_route(const uint8_t *msg, size_t msg_len, const uint8_t *gid, size_t gid_len,
             CommitRoute *out)
{
    memset(out, 0, sizeof(*out));
    out->named = UINT32_MAX;
    MlsMLSMessage wm;
    MlsTlsReader r;
    mls_tls_reader_init(&r, msg, msg_len);
    if (mls_message_deserialize(&r, &wm) != 0) return false;
    bool routable = mls_tls_reader_done(&r);
    const uint8_t *g = NULL;
    size_t g_len = 0;
    uint8_t content_type = 0;
    if (wm.wire_format == MLS_WIRE_FORMAT_PUBLIC_MESSAGE) {
        const MlsFramedContent *fc = &wm.public_message.content;
        g = fc->group_id;
        g_len = fc->group_id_len;
        content_type = fc->content_type;
        out->epoch = fc->epoch;
        if (fc->sender.sender_type == MLS_SENDER_TYPE_MEMBER)
            out->named = fc->sender.leaf_index;
        else
            routable = false;
    } else if (wm.wire_format == MLS_WIRE_FORMAT_PRIVATE_MESSAGE) {
        /* Always from a member (section 6.3). */
        g = wm.private_message.group_id;
        g_len = wm.private_message.group_id_len;
        content_type = wm.private_message.content_type;
        out->epoch = wm.private_message.epoch;
    } else {
        routable = false;
    }
    out->proposal = content_type == MLS_CONTENT_TYPE_PROPOSAL;
    out->commit = routable && content_type == MLS_CONTENT_TYPE_COMMIT;
    out->group_ok = g && g_len == gid_len && memcmp(g, gid, gid_len) == 0;
    mls_message_clear(&wm);
    return true;
}

/* The routed Commit's sender leaf as `base` sees it: a PublicMessage's named
 * one, a PrivateMessage's sender data opened with base's sender_data_secret.
 * FALSE when base cannot tell (another epoch, junk): the Commit is not
 * base's. The Commit is authenticated when it is staged. */
static bool
commit_sender_on(const MlsGroup *base, const uint8_t *msg, size_t msg_len,
                 const CommitRoute *route, uint32_t *out)
{
    *out = UINT32_MAX;
    if (route->named != UINT32_MAX) {
        *out = route->named;
        return true;
    }
    return mls_group_handshake_sender(base, msg, msg_len, out) == 0;
}

/* The ordering key of a Commit judged on `base`: one that applies, or an
 * admin's removal of our leaf.  FALSE when it is neither. */
static bool
inbound_order_key(const Marmot *m, const MlsGroup *base, const uint8_t *msg, size_t msg_len,
                  uint32_t sender, MarmotCommitKey *key)
{
    uint8_t digest[32];
    if (mls_crypto_hash(digest, msg, msg_len) != 0) return false;
    MlsGroup post;
    memset(&post, 0, sizeof(post));
    MarmotGroupDataExtension *gde = NULL;
    MarmotError err = stage_inbound(m, base, msg, msg_len, sender, &post, key, &gde);
    mls_group_free(&post);
    marmot_group_data_extension_free(gde);
    bool removed = false;
    if (err == MARMOT_ERR_MLS_PROCESS_MESSAGE &&
        mls_group_commit_removes_self(base, msg, msg_len, sender, &removed) == 0 && removed)
        err = removal_key(base, sender, digest, key);
    else if (err == MARMOT_OK)
        memcpy(key->digest, digest, 32);
    return err == MARMOT_OK;
}

/* The replay order of `p`'s deferred Commits: by their ordering key judged
 * on `cur` (lowest, the winner, first); those with no key (of another
 * epoch, or that do not apply) after them, in arrival order. */
typedef struct {
    size_t          index;
    bool            known;
    MarmotCommitKey key;
} DeferredRank;

static int
deferred_rank_cmp(const void *a, const void *b)
{
    const DeferredRank *x = a, *y = b;
    if (x->known != y->known) return x->known ? -1 : 1;
    if (x->known) {
        int c = commit_key_cmp(&x->key, &y->key);
        if (c != 0) return c;
    }
    return x->index < y->index ? -1 : x->index > y->index;
}

static void
deferred_order(const Marmot *m, const MlsGroup *cur, const PendingCommit *p, size_t *order)
{
    DeferredRank rank[PENDING_MAX_DEFERRED];
    for (size_t i = 0; i < p->deferred_count; i++) {
        const DeferredCommit *d = &p->deferred[i];
        CommitRoute route;
        uint32_t sender = UINT32_MAX;
        rank[i].index = i;
        rank[i].known = commit_route(d->msg, d->msg_len, cur->group_id, cur->group_id_len,
                                     &route) &&
                        route.commit && route.epoch == cur->epoch &&
                        commit_sender_on(cur, d->msg, d->msg_len, &route, &sender) &&
                        inbound_order_key(m, cur, d->msg, d->msg_len, sender, &rank[i].key);
    }
    qsort(rank, p->deferred_count, sizeof(rank[0]), deferred_rank_cmp);
    for (size_t i = 0; i < p->deferred_count; i++) order[i] = rank[i].index;
}

MarmotError
marmot_commit_deferred_replay_order(Marmot *m, const MarmotGroupId *gid, size_t *order,
                                    size_t max, size_t *out_count)
{
    *out_count = 0;
    PendingCommit p;
    MarmotError err = pending_load(m, gid->data, gid->len, &p);
    if (err != MARMOT_OK) return err;
    MlsGroup cur;
    err = load_current(m, gid, &cur);
    if (err == MARMOT_OK && p.deferred_count <= max) {
        deferred_order(m, &cur, &p, order);
        *out_count = p.deferred_count;
    } else if (err == MARMOT_OK) {
        err = MARMOT_ERR_INVALID_ARG;
    }
    mls_group_free(&cur);
    pending_clear(&p);
    return err;
}

/* A Commit processed: marked (best effort: the digest checks catch a
 * re-delivery anyway) and reported with the group as it is now. */
static void
inbound_done(Marmot *m, MarmotGroup *group, uint64_t epoch, const char *event_id_hex,
             MarmotMessageResult *result)
{
    MarmotStorage *s = m->storage;
    if (event_id_hex && s->save_processed_message) {
        uint8_t id[32];
        if (strlen(event_id_hex) == 64 && marmot_hex_decode(event_id_hex, id, 32) == 0)
            (void)s->save_processed_message(s->ctx, id, id, marmot_now(), epoch,
                                            &group->mls_group_id,
                                            MARMOT_MSG_STATE_PROCESSED, NULL);
    }
    fill_commit_result(m, group, result);
}

/* The group is inactive: a removal not final yet may still lose.  Only a
 * Commit of the removal's epoch (or, for a removal of the current epoch, of
 * its parent's, against the Commit that led there) is judged. */
static MarmotError
inbound_removed(Marmot *m, MarmotGroup *group, uint64_t epoch, const CommitRoute *route,
                const uint8_t *msg, size_t msg_len, const uint8_t digest[32])
{
    const uint8_t *gid = group->mls_group_id.data;
    size_t gid_len = group->mls_group_id.len;
    Removal rm;
    MarmotError err = removal_load(m, gid, gid_len, &rm);
    if (err != MARMOT_OK || rm.final || memcmp(digest, rm.key.digest, 32) == 0)
        return MARMOT_ERR_USE_AFTER_EVICTION;   /* left, final, or the removal again */
    MlsGroup cur;
    RetainedParent rp;
    memset(&rp, 0, sizeof(rp));
    bool have_rp = retained_load(m, gid, gid_len, &rp) == 0;
    err = load_current(m, &group->mls_group_id, &cur);
    if (err != MARMOT_OK) {
        if (have_rp) retained_clear(&rp);
        return MARMOT_ERR_MLS;
    }
    const MlsGroup *base = NULL;
    const MarmotCommitKey *beat = NULL;
    bool on_parent = false;
    bool full_parent = have_rp && rp.tier == PARENT_TIER_CONVERGENCE && rp.parent_epoch == epoch;
    if (epoch == rm.epoch && !rm.from_parent && cur.epoch == epoch) {
        base = &cur;
        beat = &rm.key;
    } else if (epoch == rm.epoch && rm.from_parent && full_parent) {
        base = &rp.parent;
        beat = &rm.key;
        on_parent = true;
    } else if (!rm.from_parent && epoch + 1 == rm.epoch && full_parent &&
               memcmp(digest, rp.key.digest, 32) != 0) {
        base = &rp.parent;       /* it would replace the Commit that led there */
        beat = &rp.key;
        on_parent = true;
    }
    err = MARMOT_ERR_USE_AFTER_EVICTION;
    uint32_t sender = UINT32_MAX;
    if (base && !commit_sender_on(base, msg, msg_len, route, &sender)) {
        err = MARMOT_ERR_MLS_PROCESS_MESSAGE;   /* not a Commit of that epoch */
    } else if (base) {
        MlsGroup post;
        memset(&post, 0, sizeof(post));
        MarmotCommitKey key;
        MarmotGroupDataExtension *gde = NULL;
        err = stage_inbound(m, base, msg, msg_len, sender, &post, &key, &gde);
        if (err == MARMOT_OK) {
            memcpy(key.digest, digest, 32);
            if (commit_key_cmp(&key, beat) < 0) {
                /* It wins that epoch and keeps us: the removal is undone. */
                group->state = MARMOT_GROUP_STATE_ACTIVE;
                err = marmot_commit_persist(m, base, &post, &key, gde, group);
                if (err == MARMOT_OK) err = removal_delete(m, gid, gid_len);
                if (err != MARMOT_OK) group->state = MARMOT_GROUP_STATE_INACTIVE;
            } else {
                err = MARMOT_ERR_WRONG_EPOCH;
            }
        } else if (err == MARMOT_ERR_MLS_PROCESS_MESSAGE) {
            bool removed = false;
            if (mls_group_commit_removes_self(base, msg, msg_len, sender, &removed) == 0 &&
                removed) {
                err = removal_key(base, sender, digest, &key);
                if (err == MARMOT_OK && commit_key_cmp(&key, beat) < 0) {
                    /* Another admin's removal wins that epoch. */
                    Removal next = { .epoch = epoch, .from_parent = on_parent, .key = key };
                    next.final = removal_final(base, &key, on_parent,
                                               !on_parent && have_rp ? &rp : NULL);
                    err = removal_store(m, gid, gid_len, &next);
                    if (err == MARMOT_OK && next.final) err = forget_keys(m, group);
                } else if (err == MARMOT_OK) {
                    err = MARMOT_ERR_WRONG_EPOCH;
                }
            }
        }
        mls_group_free(&post);
        marmot_group_data_extension_free(gde);
    }
    mls_group_free(&cur);
    if (have_rp) retained_clear(&rp);
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
    CommitRoute route;
    if (!commit_route(msg, msg_len, group->mls_group_id.data, group->mls_group_id.len, &route))
        return MARMOT_ERR_MLS_FRAMING;
    /* Standalone proposals are not queued yet: Commits carry them inline. */
    if (route.proposal) return MARMOT_ERR_UNSUPPORTED;
    if (!route.commit) return MARMOT_ERR_MLS_FRAMING;
    if (!route.group_ok) return MARMOT_ERR_WRONG_GROUP_ID;
    uint64_t epoch = route.epoch;
    uint32_t sender = UINT32_MAX;   /* resolved on the state that judges it */
    /* The Commit is sealed under its own epoch's exporter secret. */
    if (epoch != outer_epoch) return MARMOT_ERR_WRONG_EPOCH;

    MarmotCommitKey key;
    uint8_t digest[32];
    if (mls_crypto_hash(digest, msg, msg_len) != 0) return MARMOT_ERR_CRYPTO;

    /* Removed by a Commit that may still lose its epoch (B1). */
    if (group->state != MARMOT_GROUP_STATE_ACTIVE) {
        MarmotError rerr = inbound_removed(m, group, epoch, &route, msg, msg_len, digest);
        if (rerr != MARMOT_OK) return rerr;
        inbound_done(m, group, epoch, event_id_hex, result);
        return MARMOT_OK;
    }

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
        bool known = commit_sender_on(&cur, msg, msg_len, &route, &sender);
        err = known ? stage_inbound(m, &cur, msg, msg_len, sender, &post, &key, &gde)
                    : MARMOT_ERR_MLS_PROCESS_MESSAGE;
        bool removed = false;
        if (err == MARMOT_ERR_MLS_PROCESS_MESSAGE && known &&
            mls_group_commit_removes_self(&cur, msg, msg_len, sender, &removed) == 0 &&
            removed) {
            /* nostrc-xrya: a Commit that removes us cannot be applied (its
             * UpdatePath is encrypted to the others), but an admin's
             * authenticated one ends the group for us -- unless our own
             * pending Commit wins the epoch. */
            err = removal_key(&cur, sender, digest, &key);
            if (err == MARMOT_OK && live && commit_key_cmp(&key, &p.key) >= 0)
                err = defer_inbound(m, &p, gid, gid_len, epoch, msg, msg_len, digest,
                                    event_id_hex);
            else if (err == MARMOT_OK)
                err = evict(m, group, &cur, false, &key, epoch);
        } else if (err == MARMOT_OK) {
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
                retained_clear(&rp);
                mls_group_free(&cur);
                result->type = MARMOT_RESULT_OWN_MESSAGE;
                return MARMOT_OK;
            } else if (!commit_sender_on(&rp.parent, msg, msg_len, &route, &sender)) {
                err = MARMOT_ERR_MLS_PROCESS_MESSAGE;
            } else if (sender == rp.parent.own_leaf_index) {
                err = MARMOT_ERR_WRONG_EPOCH;   /* not the Commit we made */
            } else if (rp.tier == PARENT_TIER_READER) {
                /* Retired (nostrc-yuj2): every member that could publish a
                 * winning competitor was seen at the new epoch. */
                err = MARMOT_ERR_WRONG_EPOCH;
            } else {
                err = stage_inbound(m, &rp.parent, msg, msg_len, sender, &post,
                                    &key, &gde);
                bool removed = false;
                if (err == MARMOT_ERR_MLS_PROCESS_MESSAGE &&
                    mls_group_commit_removes_self(&rp.parent, msg, msg_len, sender,
                                                  &removed) == 0 && removed) {
                    /* A competing Commit that removes us (nostrc-xrya): if it
                     * beats the one we applied, the group ends for us. */
                    err = removal_key(&rp.parent, sender, digest, &key);
                    if (err == MARMOT_OK)
                        err = commit_key_cmp(&key, &rp.key) < 0
                                  ? evict(m, group, &rp.parent, true, &key, epoch)
                                  : MARMOT_ERR_WRONG_EPOCH;   /* the applied Commit wins */
                } else if (err == MARMOT_OK) {
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
            retained_clear(&rp);
        }
    } else {
        err = MARMOT_ERR_WRONG_EPOCH;   /* stale, or from an epoch we lack */
    }

    mls_group_free(&post);
    mls_group_free(&cur);
    marmot_group_data_extension_free(gde);
    if (err != MARMOT_OK) return err;

    inbound_done(m, group, epoch, event_id_hex, result);
    return MARMOT_OK;
}
