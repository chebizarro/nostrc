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
#include "convergence.h"
#include "proposals.h"
#include "members.h"
#include "kp_profile.h"
#include "adopted.h"
#include "mls/mls-internal.h"
#include "mls/mls_framing.h"
#include <marmot/marmot-media.h>
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
#define PENDING_TRAILER_DEPARTURES 1
#define PENDING_TRAILER_COMMIT     2
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
/* Since 0.12.0 (nostrc-2um6): the Commit committed our own SelfRemove, and
 * its key is ordinary (SelfRemoves only, any member's).  libmarmot 0.11.0
 * refuses such a record (it reports the group ended, not why). */
#define REMOVED_LEFT        4
#define REMOVED_ORDINARY    8

/* ──────────────────────────────────────────────────────────────────────────
 * Helpers
 * ──────────────────────────────────────────────────────────────────────── */

static MarmotError load_current(Marmot *m, const MarmotGroupId *gid, MlsGroup *cur);

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
 * member may produce: a GroupData that lists no admins (MIP-01 legacy) lets
 * anyone commit.  A group WITHOUT GroupData has no admin at all: it is not
 * a legacy group whose rules this function knows (an adopted group keeps its
 * admins in 0x8003), and "no GroupData" must never read as "everyone is an
 * admin" (W24 review H1).  Callers that bound who could still win an epoch
 * (could_win()) count every member when there is no GroupData instead. */
static bool
gde_is_admin(const MarmotGroupDataExtension *gde, const uint8_t pk[32])
{
    if (!gde) return false;
    if (gde->admin_count == 0 || !gde->admins) return true;
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

/* nostrc-7vyi: `after`, the leaf a Commit left in a slot, is bound to the
 * account its credential names.  `before` is the slot's previous leaf when
 * `after` RENEWS it -- the holder's own new leaf, signed in by its previous
 * key -- and NULL when `after` is a new identity claim (an Add, whether into
 * a blank slot or into the slot of a leaf the same Commit removed: W24
 * review B1).  An unchanged leaf was checked when it joined. */
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
    /* The holder's renewed leaf: the MLS layer pinned its identity and the
     * old key signed it in; it may stay unproven, but not drop a proof
     * (account-identity-proof-v2.md, "Lifecycle"). */
    if (before)
        return marmot_leaf_proof_status(before, MARMOT_CIPHERSUITE) == MARMOT_LEAF_PROOF_ABSENT
                   ? MARMOT_OK : MARMOT_ERR_KEY_PACKAGE_IDENTITY;
    /* A new identity claim: only the account's own proof supports it, or
     * the policy (legacy groups only: marmot_commit_authorize()). */
    return allow_unproven ? MARMOT_OK : MARMOT_ERR_KEY_PACKAGE_IDENTITY;
}

MarmotError
marmot_tree_members_bound(const MlsGroup *g, uint32_t exempt, bool allow_unproven)
{
    if (!g) return MARMOT_ERR_INVALID_ARG;
    /* Only a legacy-profile group may hold leaves without the proof, the
     * Welcome sender's included (nostrc-6ukh). */
    if (!marmot_mls_group_is_legacy(g)) {
        allow_unproven = false;
        exempt = UINT32_MAX;
    }
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

/* nostrc-2um6: a Commit of SelfRemove proposals and nothing else. */
static bool
self_remove_only(const MlsCommitSummary *d)
{
    return d && d->self_remove_count > 0 && d->self_remove_count == d->proposal_count;
}

static bool
summary_self_removed(const MlsCommitSummary *d, uint32_t leaf)
{
    for (size_t i = 0; d && i < d->self_remove_count; i++)
        if (d->self_removed[i] == leaf) return true;
    return false;
}

/* `leaf` left on its own request in this Commit: its SelfRemove, or a
 * Remove it sent for itself (MDK 0.8's leave; review M1). */
static bool
summary_departs(const MlsCommitSummary *d, uint32_t leaf)
{
    if (summary_self_removed(d, leaf)) return true;
    for (size_t i = 0; d && i < d->left_count; i++)
        if (d->left[i] == leaf) return true;
    return false;
}

/* The accounts (hex) that left on their own request in a Commit applied on
 * `base` (review L4: the application shows each, whether or not it saw the
 * proposal). */
static void
departed_hexes(const MlsGroup *base, const MlsCommitSummary *d, char ***out, size_t *n)
{
    *out = NULL;
    *n = 0;
    size_t total = d ? d->self_remove_count + d->left_count : 0;
    if (total == 0) return;
    char **list = calloc(total, sizeof(*list));
    if (!list) return;
    size_t k = 0;
    for (size_t i = 0; i < total; i++) {
        uint32_t leaf = i < d->self_remove_count ? d->self_removed[i]
                                                 : d->left[i - d->self_remove_count];
        uint8_t id[32];
        if (marmot_mls_sender_identity(base, leaf, id) != 0) continue;
        char *hex = marmot_hex_encode(id, 32);
        if (hex) list[k++] = hex;
    }
    if (k == 0) {
        free(list);
        return;
    }
    *out = list;
    *n = k;
}

/* Every SelfRemove's sender is a non-admin of `pre` (MIP-03: an admin steps
 * down first; member-departure.md "Validation"), per the group's admin
 * policy hook. */
static MarmotError
self_removers_not_admins(const MlsGroup *pre, const MlsCommitSummary *d)
{
    for (size_t i = 0; d && i < d->self_remove_count; i++) {
        uint8_t id[32];
        bool admin = true;
        if (marmot_mls_sender_identity(pre, d->self_removed[i], id) != 0)
            return MARMOT_ERR_FROM_NON_MEMBER;
        MarmotError err = marmot_policy_is_admin(pre, id, &admin);
        if (err != MARMOT_OK) return err;
        if (admin) return MARMOT_ERR_ADMIN_CANNOT_LEAVE;
    }
    return MARMOT_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Authorization (adopted profile, nostrc-qp24.5.1.3)
 *
 * marmot-protocol/marmot 07da8ffb, judged as MDK v0.11.0 (cgka-engine
 * app_components.rs) judges a staged Commit:
 *   - group-messaging.md "Commit authorization": an active admin of the
 *     candidate parent (0x8003) may commit; a non-admin only a self-update
 *     (no proposals, an UpdatePath on its own leaf) or a SelfRemove-only
 *     Commit, never both (MDK is_allowed_non_admin_commit).  Those two are
 *     ordinary in the ordering key, everything else privileged
 *     (commit_ordering_priority_for_staged).
 *   - app-components/README.md "Authorization Evaluation": a by-reference
 *     proposal's sender is judged in its source epoch -- the parent, since
 *     only that epoch's proposals resolve -- independently of the
 *     committer: anything but a SelfRemove needs an admin (authorize_proposal),
 *     a SelfRemove a non-admin (admin-policy-v1.md).  A lifecycle update is
 *     inline only.
 *   - every leaf the Commit adds carries a verified account proof; the
 *     committer's own leaf is renewed in place and keeps one (W24 slice A).
 *   - every app_data_dictionary entry that changed holds bytes its
 *     component's validator accepts, or is a removal the component allows;
 *     a component libmarmot cannot validate as MDK does is refused
 *     (MARMOT_ERR_UNSUPPORTED) rather than followed blind.
 *   - the resulting epoch (required components present, canonical
 *     dictionary, every admin a member, routing valid, every leaf
 *     capable): mls_group_profile_check_entered(), as the MLS layer checks
 *     it before any install.
 * ──────────────────────────────────────────────────────────────────────── */

/* A component's new state (MDK validate_app_component_bytes): slice I's
 * mls_adopted_component_state_valid(), the one definition admission and
 * the AppDataUpdate path share (slice I review M1), so a valid 0x8006 or
 * 0x800b update is followed.  One exception, fail-closed: a disband
 * (0x800c `disbanded`) is a terminal convergence pass libmarmot does not
 * implement (group-lifecycle-v1.md), MARMOT_ERR_UNSUPPORTED. */
static MarmotError
adopted_component_valid(uint16_t id, const uint8_t *data, size_t len)
{
    if (id == MARMOT_COMPONENT_GROUP_LIFECYCLE_V1 && len == 1 && data[0] == 1)
        return MARMOT_ERR_UNSUPPORTED;
    return (MarmotError)mls_adopted_component_state_valid(id, data, len);
}

/* The app_data_dictionary entries of `g`'s GroupContext (borrowed). */
static MarmotError
adopted_dictionary(const MlsGroup *g, MarmotComponentData **entries, size_t *n)
{
    *entries = NULL;
    *n = 0;
    const uint8_t *dict = NULL;
    size_t dict_len = 0, count = 0;
    if (marmot_extensions_find(g->extensions_data, g->extensions_len,
                               MARMOT_EXT_APP_DATA_DICTIONARY, &dict, &dict_len, &count) != 0 ||
        count != 1 || marmot_app_data_dict_parse(dict, dict_len, entries, n) != 0)
        return MARMOT_ERR_EXTENSION_FORMAT;
    return MARMOT_OK;
}

/* Every dictionary entry the Commit changed (it can only be through its
 * AppDataUpdates: the MLS layer applies nothing else to an adopted
 * GroupContext) is a value its component accepts, or a removal it allows
 * (MDK validate_app_component_remove_against).  *changed: anything did. */
static MarmotError
adopted_dictionary_changes_valid(const MlsGroup *pre, const MlsGroup *post, bool *changed)
{
    *changed = false;
    MarmotComponentData *a = NULL, *b = NULL;
    size_t na = 0, nb = 0;
    MarmotError err = adopted_dictionary(pre, &a, &na);
    if (err == MARMOT_OK) err = adopted_dictionary(post, &b, &nb);
    size_t i = 0, j = 0;
    while (err == MARMOT_OK && (i < na || j < nb)) {
        const MarmotComponentData *x = i < na ? &a[i] : NULL;
        const MarmotComponentData *y = j < nb ? &b[j] : NULL;
        if (x && y && x->component_id == y->component_id) {
            i++;
            j++;
            if (x->len == y->len && (x->len == 0 || memcmp(x->data, y->data, x->len) == 0))
                continue;
            *changed = true;
            err = adopted_component_valid(y->component_id, y->data, y->len);
        } else if (x && (!y || x->component_id < y->component_id)) {
            i++;   /* removed */
            *changed = true;
            switch (x->component_id) {
            case MARMOT_COMPONENT_APP_COMPONENTS:
            case MARMOT_COMPONENT_SAFE_AAD:
            case MARMOT_COMPONENT_ADMIN_POLICY_V1:
            case MARMOT_COMPONENT_GROUP_LIFECYCLE_V1:
                err = MARMOT_ERR_VALIDATION;   /* never removable */
                break;
            default:
                break;   /* a required one fails the resulting-epoch check */
            }
        } else {
            j++;   /* added */
            *changed = true;
            err = adopted_component_valid(y->component_id, y->data, y->len);
        }
    }
    free(a);
    free(b);
    return err;
}

/* Lifecycle (0x800c) of `g`: *state -1 (no state), 0 active, 1
 * disbanded; *required: 0x0001 lists it. */
static MarmotError
adopted_lifecycle(const MlsGroup *g, int *state, bool *required)
{
    *state = -1;
    *required = false;
    MarmotComponentData *e = NULL;
    size_t n = 0;
    MarmotError err = adopted_dictionary(g, &e, &n);
    for (size_t i = 0; err == MARMOT_OK && i < n; i++) {
        if (e[i].component_id == MARMOT_COMPONENT_GROUP_LIFECYCLE_V1) {
            if (e[i].len != 1 || e[i].data[0] > 1) err = MARMOT_ERR_EXTENSION_FORMAT;
            else *state = e[i].data[0];
        } else if (e[i].component_id == MARMOT_COMPONENT_APP_COMPONENTS) {
            uint16_t ids[MLS_ADOPTED_MAX_IDS];
            size_t k = 0;
            if (mls_components_list_decode_strict(e[i].data, e[i].len, ids,
                                                  MLS_ADOPTED_MAX_IDS, &k) != 0)
                err = MARMOT_ERR_EXTENSION_FORMAT;
            for (size_t j = 0; j < k; j++)
                *required |= ids[j] == MARMOT_COMPONENT_GROUP_LIFECYCLE_V1;
        }
    }
    free(e);
    return err;
}

/* MDK v0.11.0 validate_group_lifecycle_transition()
 * (cgka-engine/src/app_components.rs:859-1019), run on every staged Commit
 * (slice H review M1): group-lifecycle-v1.md "Once required, this component
 * MUST remain present and required for the remainder of the group's
 * lifetime".  In MDK's order:
 *   - a disbanded parent has no outgoing transition;
 *   - required -> not required: refused ("cannot be un-required");
 *   - not required -> required (the enablement): the state must become
 *     active, and every proposal be inline and an AppDataUpdate of 0x0001
 *     or 0x800c;
 *   - otherwise, unless active -> disbanded: the state may not change, and
 *     no proposal may be an AppDataUpdate of 0x800c, even one restating
 *     the present state ("redundant lifecycle update");
 *   - active -> disbanded (a disband): MARMOT_ERR_UNSUPPORTED, libmarmot
 *     does not implement it (MDK's own shape rules are not needed to refuse
 *     it).
 * A refusal is MARMOT_ERR_VALIDATION.  For our own Commit (a producer's:
 * shape unknown) what needs the shape fails closed; our producers never
 * write 0x800c. */
static MarmotError
adopted_lifecycle_transition(const MlsGroup *pre, const MlsGroup *post,
                             const MlsCommitSummary *sum)
{
    int before = -1, after = -1;
    bool before_required = false, after_required = false;
    MarmotError err = adopted_lifecycle(pre, &before, &before_required);
    if (err == MARMOT_OK) err = adopted_lifecycle(post, &after, &after_required);
    if (err != MARMOT_OK) return err;
    bool shape = sum && sum->shape_known;

    if (before == 1) return MARMOT_ERR_VALIDATION;
    if (before_required && !after_required) return MARMOT_ERR_VALIDATION;
    if (!before_required && after_required) {
        if (after != 0) return MARMOT_ERR_VALIDATION;
        if (!shape || sum->adu_inline_enablement_count != sum->proposal_count)
            return MARMOT_ERR_VALIDATION;
        return MARMOT_OK;
    }
    if (before == 0 && after == 1) return MARMOT_ERR_UNSUPPORTED;   /* disband */
    if (before != after) return MARMOT_ERR_VALIDATION;
    if (shape && sum->adu_lifecycle_count > 0) return MARMOT_ERR_VALIDATION;
    return MARMOT_OK;
}

static MarmotError
adopted_commit_authorize(const MlsGroup *pre, const MlsGroup *post, uint32_t committer_leaf,
                         const MlsCommitSummary *sum, MarmotCommitKey *key)
{
    /* The epoch entered is a valid adopted epoch (also checked by the MLS
     * layer before any install; again here for every caller). */
    int prc = mls_group_profile_check_entered(post);
    if (prc != 0) return (MarmotError)prc;

    const MlsLeafNode *committer = leaf_at(pre, committer_leaf);
    if (!committer || committer->credential_identity_len != 32 ||
        !committer->credential_identity)
        return MARMOT_ERR_FROM_NON_MEMBER;
    memcpy(key->committer, committer->credential_identity, 32);
    key->committer_leaf = committer_leaf;
    const MlsLeafNode *committer_after = leaf_at(post, committer_leaf);
    if (!committer_after || !same_identity(committer, committer_after))
        return MARMOT_ERR_IDENTITY_CHANGE;
    bool admin = false;
    MarmotError err = marmot_policy_is_admin(pre, key->committer, &admin);
    if (err != MARMOT_OK) return err;

    /* What changed, from the states: membership (any slot but the
     * committer's renewal; a SelfRemove-only Commit only blanks its leavers'
     * slots) and the dictionary. */
    bool sr_only = self_remove_only(sum);
    bool members_changed = false;
    uint32_t n = pre->tree.n_leaves > post->tree.n_leaves ? pre->tree.n_leaves
                                                          : post->tree.n_leaves;
    for (uint32_t i = 0; i < n && !members_changed; i++) {
        const MlsLeafNode *a = leaf_at(pre, i);
        const MlsLeafNode *b = leaf_at(post, i);
        if (sr_only && a && !b && summary_self_removed(sum, i)) continue;
        members_changed = (!a != !b) || (a && i != committer_leaf && !same_leaf(a, b));
    }
    bool dict_changed = false;
    err = adopted_dictionary_changes_valid(pre, post, &dict_changed);
    if (err != MARMOT_OK) return err;
    err = adopted_lifecycle_transition(pre, post, sum);
    if (err != MARMOT_OK) return err;
    /* Nothing but the dictionary may differ in an adopted GroupContext:
     * required_capabilities changes only by GroupContextExtensions, which
     * libmarmot does not apply there. */
    {
        const uint8_t *rc_pre = NULL, *rc_post = NULL;
        size_t l_pre = 0, l_post = 0, c_pre = 0, c_post = 0;
        if (marmot_extensions_find(pre->extensions_data, pre->extensions_len,
                                   MLS_EXT_REQUIRED_CAPABILITIES, &rc_pre, &l_pre, &c_pre) != 0 ||
            marmot_extensions_find(post->extensions_data, post->extensions_len,
                                   MLS_EXT_REQUIRED_CAPABILITIES, &rc_post, &l_post, &c_post) != 0 ||
            c_pre != c_post || l_pre != l_post ||
            (l_pre && memcmp(rc_pre, rc_post, l_pre) != 0))
            return MARMOT_ERR_UNSUPPORTED;
    }

    /* The Commit's shape: known from the processor for a Commit we
     * received; for our own (a producer's), what the states show. */
    bool self_update = false;
    if (sum && sum->shape_known) {
        /* The MLS layer applies none of these in an adopted group. */
        if (sum->update_count || sum->gce_count || sum->other_count) return MARMOT_ERR_UNSUPPORTED;
        self_update = sum->proposal_count == 0 && sum->has_path;
    } else {
        self_update = !sr_only && (!sum || sum->proposal_count == 0) && !members_changed &&
                      !dict_changed;
    }
    bool ordinary = self_update != sr_only;
    /* An ordinary shape changes nothing else (the processor guarantees it;
     * a producer's states must agree). */
    if (ordinary && (members_changed || dict_changed)) ordinary = false;
    key->privileged = !ordinary;
    if (key->privileged && !admin) return MARMOT_ERR_COMMIT_FROM_NON_ADMIN;

    /* SelfRemove senders: non-admins of their source epoch. */
    err = self_removers_not_admins(pre, sum);
    if (err != MARMOT_OK) return err;
    /* Other by-reference proposals: an admin's of their source epoch. */
    for (size_t i = 0; sum && sum->shape_known && i < sum->ref_count; i++) {
        uint8_t id[32];
        bool sender_admin = false;
        if (marmot_mls_sender_identity(pre, sum->ref_sender[i], id) != 0)
            return MARMOT_ERR_FROM_NON_MEMBER;
        if (sum->ref_type[i] == MLS_PROPOSAL_APP_DATA_UPDATE &&
            sum->ref_component[i] == MARMOT_COMPONENT_GROUP_LIFECYCLE_V1)
            return MARMOT_ERR_VALIDATION;   /* inline only (group-lifecycle-v1.md) */
        err = marmot_policy_is_admin(pre, id, &sender_admin);
        if (err != MARMOT_OK) return err;
        if (!sender_admin) return MARMOT_ERR_COMMIT_FROM_NON_ADMIN;
    }

    /* Every account the Commit brings in is its own: no unproven leaf in an
     * adopted group, ever (allow_unproven is legacy-only). */
    for (uint32_t i = 0; i < post->tree.n_leaves; i++) {
        const MlsLeafNode *a = leaf_at(pre, i);
        const MlsLeafNode *b = leaf_at(post, i);
        if (!b) continue;
        bool kept = a && same_leaf(a, b);
        bool renewed = a && i == committer_leaf;
        err = leaf_binding_check(kept || renewed ? a : NULL, b, false);
        if (err != MARMOT_OK) return err;
    }
    return MARMOT_OK;
}

MarmotError
marmot_commit_authorize(const MlsGroup *pre, const MlsGroup *post,
                        uint32_t committer_leaf, bool allow_unproven,
                        MarmotCommitKey *key,
                        MarmotGroupDataExtension **post_gde)
{
    return marmot_commit_authorize_ex(pre, post, committer_leaf, allow_unproven, NULL, key,
                                      post_gde);
}

MarmotError
marmot_commit_authorize_ex(const MlsGroup *pre, const MlsGroup *post,
                           uint32_t committer_leaf, bool allow_unproven,
                           const MlsCommitSummary *departures,
                           MarmotCommitKey *key,
                           MarmotGroupDataExtension **post_gde)
{
    if (!pre || !post || !key || !post_gde) return MARMOT_ERR_INVALID_ARG;
    *post_gde = NULL;
    memset(key, 0, sizeof(*key));

    /* An adopted-profile group's Commits are authorized by its components
     * (nostrc-qp24.5.1.3, above) -- never by the MIP-01 rules below, under
     * which a group without GroupData lets any member commit (nostrc-
     * qp24.5.1) and a leaf without the account proof may be admitted
     * (nostrc-6ukh).  A group keeps the profile it was admitted with: a
     * Commit into another profile, or of anything that is not one of the
     * two, is refused (W24 review L3; the MLS layer refuses it first,
     * mls_group_process_commit()). */
    if (pre->profile == MARMOT_GROUP_PROFILE_ADOPTED &&
        post->profile == MARMOT_GROUP_PROFILE_ADOPTED)
        return adopted_commit_authorize(pre, post, committer_leaf, departures, key);
    if (!marmot_mls_group_is_legacy(pre) || !marmot_mls_group_is_legacy(post))
        return MARMOT_ERR_UNSUPPORTED;

    const MlsLeafNode *committer = leaf_at(pre, committer_leaf);
    if (!committer || committer->credential_identity_len != 32 ||
        !committer->credential_identity)
        return MARMOT_ERR_FROM_NON_MEMBER;
    memcpy(key->committer, committer->credential_identity, 32);
    key->committer_leaf = committer_leaf;

    /* Membership: a removed leaf, or a slot an Add filled, makes the Commit
     * privileged (admins only).  Only the committer's own leaf is renewed in
     * place, by its UpdatePath, which the MLS layer verified under the
     * committer's previous key and whose identity it pinned.  Every other
     * slot whose leaf changed was filled by an Add -- into a blank slot, or
     * into the slot of a leaf this Commit removed: RFC 9420 applies Removes
     * first and an Add takes the leftmost blank leaf, so Remove(Y) +
     * Add(KeyPackage claiming Y) lands in Y's slot under the same identity.
     * Judged by identity alone that swap passed as unprivileged and as Y's
     * own new leaf: any member could take over a proof-less member's device
     * (W24 review B1).  libmarmot applies no Update proposals (no proposal
     * store on this path; a by-reference Update is MARMOT_ERR_UNSUPPORTED);
     * whoever adds them must pass their leaves in as renewals too. */
    const MlsLeafNode *committer_after = leaf_at(post, committer_leaf);
    if (!committer_after || !same_identity(committer, committer_after))
        return MARMOT_ERR_IDENTITY_CHANGE;
    /* nostrc-2um6: the leaves a SelfRemove-only Commit blanks left on their
     * own request; that Commit is ordinary (any member may commit it). */
    bool sr_only = self_remove_only(departures);
    MarmotError derr = self_removers_not_admins(pre, departures);
    if (derr != MARMOT_OK) return derr;
    bool members_changed = false;
    uint32_t n = pre->tree.n_leaves > post->tree.n_leaves ? pre->tree.n_leaves
                                                          : post->tree.n_leaves;
    for (uint32_t i = 0; i < n && !members_changed; i++) {
        const MlsLeafNode *a = leaf_at(pre, i);
        const MlsLeafNode *b = leaf_at(post, i);
        /* A departure (nostrc-2um6: a SelfRemove-only Commit, which any
         * member may commit) only blanks the leaf; it never refills it. */
        if (sr_only && a && !b && summary_self_removed(departures, i)) continue;
        members_changed = (!a != !b) ||
                          (a && i != committer_leaf && !same_leaf(a, b));
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
        const MlsLeafNode *a = leaf_at(pre, i);
        const MlsLeafNode *b = leaf_at(post, i);
        if (!b) continue;
        bool kept = a && same_leaf(a, b);
        bool renewed = a && i == committer_leaf;
        err = leaf_binding_check(kept || renewed ? a : NULL, b, allow_unproven);
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
 * Retained history (nostrc-yuj2; nostrc-w1m0)
 *
 * The retained-parent record ("mls_group_parent", layout in convergence.c)
 * keeps, for each of the last CONV_MAX_REWIND_COMMITS epochs before the
 * canonical tip, the state the epoch started in and the canonical Commit
 * that left it; and the Commits retained off the canonical branch, the
 * app-payload witnesses and the exporter secrets of candidate states
 * (convergence.h, "Convergence" below).
 *
 * Marmot protocol-core/retained-history.md, "Retained cryptographic
 * material": a retained state is kept whole -- it authenticates a Commit
 * from its epoch (membership key, sender-data secret) and processes it
 * (init secret, private path keys) -- for as long as a branch forking there
 * can still be selected, i.e. while it is inside the rollback horizon; its
 * secret tree also reads that epoch's late application messages
 * (app_payload_past_epoch_limit, the same five epochs).  An entry leaves
 * the record as soon as its epoch leaves the horizon, and with it every
 * secret it held.
 *
 * Forward secrecy, the tradeoff the adopted protocol makes (and libmarmot
 * with it since W25): whoever obtains the whole store holds the init
 * secrets and private path keys of the last five epochs and, with the
 * Commits relays carry, derives every epoch since -- every message of those
 * five epochs and of the current one.  The exporter secrets of candidate
 * states (bounded by CONV_MAX_CANDIDATES) open the kind:445 layer of losing
 * branches for as long as they are retained.  Until 0.12.0 (nostrc-yuj2)
 * the parent was reduced to a reader once every member that could publish a
 * winning same-epoch competitor had been seen in the new epoch; with
 * multi-Commit branches any member can still extend a branch past the one
 * applied, and that retirement would make this member refuse the branch
 * the others select.  A record of an earlier version whose parent was
 * reduced still reads late messages and judges no Commit from its epoch.
 * ──────────────────────────────────────────────────────────────────────── */

static void
history_free(ConvHistory *h)
{
    if (!h) return;
    conv_history_clear(h);
    free(h);
}

/* The retained history of `gid` (*out NULL when there is none, or when the
 * record is unreadable: nothing to judge with).  A storage error fails. */
static MarmotError
history_load(Marmot *m, const uint8_t *gid, size_t gid_len, ConvHistory **out)
{
    *out = NULL;
    MarmotStorage *s = m->storage;
    if (!s || !s->mls_load) return MARMOT_ERR_STORAGE;
    uint8_t *data = NULL;
    size_t len = 0;
    MarmotError err = s->mls_load(s->ctx, PARENT_LABEL, gid, gid_len, &data, &len);
    if (err == MARMOT_ERR_STORAGE_NOT_FOUND || (err == MARMOT_OK && !data)) {
        free_secret(data, len);
        return MARMOT_OK;
    }
    if (err != MARMOT_OK) return err;
    ConvHistory *h = calloc(1, sizeof(*h));
    if (!h) {
        free_secret(data, len);
        return MARMOT_ERR_MEMORY;
    }
    if (conv_history_decode(data, len, h) != 0) {
        free(h);   /* cleared by the decoder */
        h = NULL;
    }
    free_secret(data, len);
    *out = h;
    return MARMOT_OK;
}

static MarmotError
history_store(Marmot *m, const uint8_t *gid, size_t gid_len, const ConvHistory *h)
{
    uint8_t *blob = NULL;
    size_t len = 0;
    if (conv_history_encode(h, &blob, &len) != 0) return MARMOT_ERR_SERIALIZATION;
    MarmotError err = m->storage->mls_store(m->storage->ctx, PARENT_LABEL, gid, gid_len,
                                            blob, len);
    free_secret(blob, len);
    return err;
}

/* The oldest epoch the canonical branch can be replayed from: every
 * retained state from it up to `tip` (the canonical tip's epoch) is full
 * and the entries are contiguous.  `tip` itself when nothing is. */
static uint64_t
history_anchor(const ConvHistory *h, uint64_t tip)
{
    uint64_t a = tip;
    for (size_t i = 0; h && i < h->n_entries; i++) {
        const ConvEntry *e = &h->entries[i];
        if (e->reader || e->epoch + 1 != a || !e->state.group_id) break;
        a = e->epoch;
    }
    return a;
}

/* The canonical state of `epoch`: `cur` at its own epoch, else a full
 * retained one (NULL when not retained or not replayable). */
static const MlsGroup *
canonical_state_at(ConvHistory *h, const MlsGroup *cur, uint64_t epoch)
{
    if (epoch == cur->epoch) return cur;
    if (!h || epoch < history_anchor(h, cur->epoch) || epoch > cur->epoch) return NULL;
    ConvEntry *e = conv_history_entry(h, epoch);
    return e ? &e->state : NULL;
}

/* Could an account (`admin` in the parent or not) publish a Commit from the
 * parent that beats `key` in a same-epoch race (CommitOrderingSuffix)? */
static bool
could_win_as(bool admin, const uint8_t id[32], const MarmotCommitKey *key)
{
    int cmp = memcmp(id, key->committer, 32);
    return key->privileged ? (admin && cmp <= 0) : (admin || cmp <= 0);
}

/* Could the account `id` publish a Commit from `parent` that beats `key`? */
static bool
could_win(const MarmotGroupDataExtension *gde, const uint8_t id[32],
          const MarmotCommitKey *key)
{
    return could_win_as(gde_is_admin(gde, id), id, key);
}

/* could_win() for an adopted parent (nostrc-qp24.5.1.3): its admins are the
 * 0x8003 component's; an unreadable policy counts the account as one. */
static bool
adopted_could_win(const MlsGroup *parent, const uint8_t id[32], const MarmotCommitKey *key)
{
    bool admin = true;
    if (marmot_policy_is_admin(parent, id, &admin) != MARMOT_OK) admin = true;
    return could_win_as(admin, id, key);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Late application messages (nostrc-qp24.7; nostrc-w1m0)
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_commit_decrypt_late(Marmot *m, const MarmotGroupId *gid, uint64_t epoch,
                           const uint8_t *msg, size_t msg_len,
                           uint8_t **out_plaintext, size_t *out_len,
                           uint32_t *out_sender,
                           uint8_t out_sender_identity[32],
                           uint8_t out_tag[32],
                           uint8_t **out_replaced, size_t *out_replaced_len)
{
    if (!m || !gid || !msg || !out_plaintext || !out_len || !out_sender ||
        !out_sender_identity || !out_tag || !out_replaced || !out_replaced_len)
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
    ConvHistory *h = calloc(1, sizeof(*h));
    if (!h) {
        free_secret(probe, probe_len);
        return MARMOT_ERR_MEMORY;
    }
    if (conv_history_decode(probe, probe_len, h) != 0) {
        free(h);
        free_secret(probe, probe_len);
        return MARMOT_ERR_DESERIALIZATION;
    }
    /* Any retained epoch: the app-payload window (five epochs back) is the
     * rollback horizon's. */
    ConvEntry *e = conv_history_entry(h, epoch);
    if (!e) {
        free_secret(probe, probe_len);
        history_free(h);
        return MARMOT_ERR_STORAGE_NOT_FOUND;   /* not an epoch we retain */
    }
    int rc = mls_group_decrypt(&e->state, msg, msg_len, out_plaintext, out_len, out_sender);
    if (rc == 0 &&
        marmot_mls_sender_identity(&e->state, *out_sender, out_sender_identity) != 0) {
        /* A leaf without an account identity cannot author anything. */
        free_secret(*out_plaintext, *out_len);
        *out_plaintext = NULL;
        *out_len = 0;
        rc = MARMOT_ERR_AUTHOR_MISMATCH;
    }
    if (rc == 0) {
        memcpy(out_tag, e->state.confirmed_transcript_hash, 32);
        /* The state's ratchet moved on (no key is ever used twice): store it
         * in the same transaction as the message. */
        err = history_store(m, gid->data, gid->len, h);
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
    history_free(h);
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

/* ──────────────────────────────────────────────────────────────────────────
 * Adopted groups: the record and the routing history (nostrc-qp24.5.1.3)
 *
 * An adopted group's name, description and admins live in its components
 * (0x8001, 0x8003) and its address in 0x8004.  A Commit may change any of
 * them; the record mirrors the epoch entered.  A routing change (a rotation
 * of nostr_group_id; nostr-routing-v1.md) leaves traffic of the epochs
 * before it at the old address: the rotating Commit's competitors, late
 * application messages.  So every address the group had stays routable
 * (ROUTING_ALIAS_LABEL, keyed by the old nostr_group_id, names the MLS
 * group) and listed (ROUTING_HISTORY_LABEL, keyed by the MLS group id:
 * u8 n, then n old addresses, oldest first; at most ROUTING_HISTORY_MAX,
 * the oldest alias dropped beyond), for marmot_get_group_routing().
 * ──────────────────────────────────────────────────────────────────────── */

#define ROUTING_ALIAS_LABEL   "nostr_group_id_alias"
#define ROUTING_HISTORY_LABEL "nostr_group_id_history"
#define ROUTING_HISTORY_MAX   16

static MarmotError
routing_history_load(Marmot *m, const uint8_t *gid, size_t gid_len,
                     uint8_t ids[ROUTING_HISTORY_MAX][32], size_t *n)
{
    *n = 0;
    uint8_t *rec = NULL;
    size_t len = 0;
    MarmotError err = m->storage->mls_load(m->storage->ctx, ROUTING_HISTORY_LABEL, gid, gid_len,
                                           &rec, &len);
    if (err == MARMOT_ERR_STORAGE_NOT_FOUND) return MARMOT_OK;
    if (err != MARMOT_OK) return err;
    if (!rec || len < 1 || rec[0] > ROUTING_HISTORY_MAX || len != 1 + (size_t)rec[0] * 32) {
        free(rec);
        return MARMOT_ERR_DESERIALIZATION;
    }
    *n = rec[0];
    memcpy(ids, rec + 1, *n * 32);
    free(rec);
    return MARMOT_OK;
}

/* The group `gid` leaves the address `old_id` for `new_id`.  An earlier
 * address it returns to (a competing Commit undid a rotation) is current
 * again, so no longer a previous one (slice H review N5). */
static MarmotError
routing_remember(Marmot *m, const MarmotGroupId *gid, const uint8_t old_id[32],
                 const uint8_t new_id[32])
{
    MarmotStorage *s = m->storage;
    uint8_t ids[ROUTING_HISTORY_MAX][32];
    size_t n = 0;
    MarmotError err = routing_history_load(m, gid->data, gid->len, ids, &n);
    if (err == MARMOT_ERR_DESERIALIZATION) {
        n = 0;   /* a damaged history: start it again */
        err = MARMOT_OK;
    }
    if (err != MARMOT_OK) return err;
    for (size_t i = 0; i < n; i++) {
        if (memcmp(ids[i], new_id, 32) != 0) continue;
        MarmotError derr = s->mls_delete(s->ctx, ROUTING_ALIAS_LABEL, new_id, 32);
        if (derr != MARMOT_OK && derr != MARMOT_ERR_STORAGE_NOT_FOUND) return derr;
        memmove(ids[i], ids[i + 1], (n - i - 1) * 32);
        n--;
        break;
    }
    bool known = false;
    for (size_t i = 0; i < n && !known; i++) known = memcmp(ids[i], old_id, 32) == 0;
    if (!known) {
        err = s->mls_store(s->ctx, ROUTING_ALIAS_LABEL, old_id, 32, gid->data, gid->len);
        if (err != MARMOT_OK) return err;
    }
    if (known) {
        uint8_t rec[1 + ROUTING_HISTORY_MAX * 32];
        rec[0] = (uint8_t)n;
        memcpy(rec + 1, ids, n * 32);
        return s->mls_store(s->ctx, ROUTING_HISTORY_LABEL, gid->data, gid->len, rec,
                            1 + n * 32);
    }
    if (n == ROUTING_HISTORY_MAX) {
        MarmotError derr = s->mls_delete(s->ctx, ROUTING_ALIAS_LABEL, ids[0], 32);
        if (derr != MARMOT_OK && derr != MARMOT_ERR_STORAGE_NOT_FOUND) return derr;
        memmove(ids[0], ids[1], (n - 1) * 32);
        n--;
    }
    memcpy(ids[n++], old_id, 32);
    uint8_t rec[1 + ROUTING_HISTORY_MAX * 32];
    rec[0] = (uint8_t)n;
    memcpy(rec + 1, ids, n * 32);
    return s->mls_store(s->ctx, ROUTING_HISTORY_LABEL, gid->data, gid->len, rec, 1 + n * 32);
}

MarmotError
marmot_commit_find_group_by_alias(Marmot *m, const uint8_t nostr_group_id[32],
                                  MarmotGroup **out)
{
    *out = NULL;
    MarmotStorage *s = m ? m->storage : NULL;
    if (!s || !s->mls_load || !s->find_group_by_mls_id) return MARMOT_ERR_GROUP_NOT_FOUND;
    uint8_t *gid = NULL;
    size_t gid_len = 0;
    MarmotError err = s->mls_load(s->ctx, ROUTING_ALIAS_LABEL, nostr_group_id, 32, &gid,
                                  &gid_len);
    if (err != MARMOT_OK || !gid || gid_len == 0) {
        free(gid);
        return err == MARMOT_OK || err == MARMOT_ERR_STORAGE_NOT_FOUND
                   ? MARMOT_ERR_GROUP_NOT_FOUND : err;
    }
    MarmotGroupId mgid = marmot_group_id_new(gid, gid_len);
    free(gid);
    err = s->find_group_by_mls_id(s->ctx, &mgid, out);
    marmot_group_id_free(&mgid);
    if (err == MARMOT_OK && !*out) err = MARMOT_ERR_GROUP_NOT_FOUND;
    return err == MARMOT_ERR_STORAGE_NOT_FOUND ? MARMOT_ERR_GROUP_NOT_FOUND : err;
}

/* Whether the adopted epoch `post` routes otherwise than `pre` (the 0x8004
 * state differs: nostr_group_id, relays or both). */
static bool
adopted_routing_changed(const MlsGroup *pre, const MlsGroup *post)
{
    if (pre->profile != MARMOT_GROUP_PROFILE_ADOPTED ||
        post->profile != MARMOT_GROUP_PROFILE_ADOPTED)
        return false;
    MlsAdoptedGroupContext a, b;
    if (mls_adopted_group_context_parse(pre->extensions_data, pre->extensions_len, &a) != 0 ||
        mls_adopted_group_context_parse(post->extensions_data, post->extensions_len, &b) != 0)
        return false;
    return a.routing_len != b.routing_len ||
           (a.routing_len && memcmp(a.routing, b.routing, a.routing_len) != 0);
}

/* The record's mirror of adopted epoch `g`: name, description, admins and
 * nostr_group_id.  When the address changes, the old one is remembered and
 * the group's created_at floor carries over first (nothing is written to
 * the record here; the caller saves it). */
static MarmotError
adopted_record_apply(Marmot *m, MarmotGroup *group, const MlsGroup *g)
{
    MarmotGroup *fresh = NULL;
    char **relays = NULL;
    size_t n_relays = 0;
    MarmotError err = marmot_adopted_group_from_mls(g, &fresh, &relays, &n_relays);
    marmot_adopted_relays_free(relays, n_relays);
    if (err != MARMOT_OK) return err;
    if (memcmp(fresh->nostr_group_id, group->nostr_group_id, 32) != 0) {
        /* The new address must be no other group's (current or earlier):
         * routing ids are public h tags, and one shared with another group
         * would misroute that group's traffic here (fail closed). */
        MarmotGroup *other = NULL;
        MarmotError ferr = m->storage->find_group_by_nostr_id
                               ? m->storage->find_group_by_nostr_id(m->storage->ctx,
                                                                    fresh->nostr_group_id, &other)
                               : MARMOT_ERR_STORAGE;
        if (ferr == MARMOT_OK && !other)
            ferr = MARMOT_ERR_GROUP_NOT_FOUND;
        if (ferr == MARMOT_ERR_GROUP_NOT_FOUND || ferr == MARMOT_ERR_STORAGE_NOT_FOUND)
            ferr = marmot_commit_find_group_by_alias(m, fresh->nostr_group_id, &other);
        bool taken = ferr == MARMOT_OK && other && !marmot_group_id_equal(&other->mls_group_id,
                                                                          &group->mls_group_id);
        marmot_group_free(other);
        if (ferr != MARMOT_OK && ferr != MARMOT_ERR_GROUP_NOT_FOUND &&
            ferr != MARMOT_ERR_STORAGE_NOT_FOUND) {
            marmot_group_free(fresh);
            return ferr;
        }
        if (taken) {
            marmot_group_free(fresh);
            return MARMOT_ERR_PROTOCOL_GROUP_MISMATCH;
        }
        err = routing_remember(m, &group->mls_group_id, group->nostr_group_id,
                               fresh->nostr_group_id);
        if (err == MARMOT_OK && m->storage->mls_load && m->storage->mls_store)
            err = marmot_carry_group_event_time(m, group->nostr_group_id, fresh->nostr_group_id);
        if (err != MARMOT_OK) {
            marmot_group_free(fresh);
            return err;
        }
        memcpy(group->nostr_group_id, fresh->nostr_group_id, 32);
    }
    free(group->name);
    free(group->description);
    free(group->admin_pubkeys);
    group->name = fresh->name;
    group->description = fresh->description;
    group->admin_pubkeys = fresh->admin_pubkeys;
    group->admin_count = fresh->admin_count;
    fresh->name = fresh->description = NULL;
    fresh->admin_pubkeys = NULL;
    fresh->admin_count = 0;
    marmot_group_free(fresh);
    return MARMOT_OK;
}

MarmotError
marmot_get_group_routing(Marmot *m, const MarmotGroupId *mls_group_id,
                         uint8_t out_nostr_group_id[32], char ***out_relays,
                         size_t *out_relay_count, uint8_t (**out_previous)[32],
                         size_t *out_previous_count)
{
    if (!m || !mls_group_id || !out_nostr_group_id || !out_relays || !out_relay_count)
        return MARMOT_ERR_INVALID_ARG;
    *out_relays = NULL;
    *out_relay_count = 0;
    if (out_previous) *out_previous = NULL;
    if (out_previous_count) *out_previous_count = 0;
    if (!m->storage || !m->storage->mls_load) return MARMOT_ERR_STORAGE;
    MlsGroup g;
    MarmotError err = load_current(m, mls_group_id, &g);
    if (err != MARMOT_OK) return err == MARMOT_ERR_STORAGE_NOT_FOUND ? MARMOT_ERR_GROUP_NOT_FOUND
                                                                     : err;
    if (g.profile != MARMOT_GROUP_PROFILE_ADOPTED) {
        mls_group_free(&g);
        return MARMOT_ERR_UNSUPPORTED;   /* a legacy group's are its GroupData's */
    }
    MarmotGroup *rec = NULL;
    err = marmot_adopted_group_from_mls(&g, &rec, out_relays, out_relay_count);
    mls_group_free(&g);
    if (err != MARMOT_OK) return err;
    memcpy(out_nostr_group_id, rec->nostr_group_id, 32);
    marmot_group_free(rec);
    if (out_previous && out_previous_count) {
        uint8_t ids[ROUTING_HISTORY_MAX][32];
        size_t n = 0;
        err = routing_history_load(m, mls_group_id->data, mls_group_id->len, ids, &n);
        if (err == MARMOT_OK && n > 0) {
            *out_previous = malloc(n * 32);
            if (!*out_previous) err = MARMOT_ERR_MEMORY;
            else {
                memcpy(*out_previous, ids, n * 32);
                *out_previous_count = n;
            }
        }
        if (err != MARMOT_OK) {
            marmot_adopted_relays_free(*out_relays, *out_relay_count);
            *out_relays = NULL;
            *out_relay_count = 0;
        }
    }
    return err;
}

/* The history once the canonical Commit `key` (MLSMessage `commit`, NULL
 * when not known; ours when `own`) took `pre` to `post`: `pre` becomes the
 * newest entry and the older retained epochs follow while contiguous, up to
 * the horizon; candidates, witnesses and branch secrets of epochs that left
 * it go, and so does a candidate that is the applied Commit.  `old` (may be
 * NULL) is consumed: what is kept is moved out of it. */
static MarmotError
history_advance(ConvHistory *old, const MlsGroup *pre, const MlsGroup *post,
                const MarmotCommitKey *key, const uint8_t *commit, size_t commit_len,
                bool own, ConvHistory **out)
{
    *out = NULL;
    ConvHistory *h = calloc(1, sizeof(*h));
    if (!h) return MARMOT_ERR_MEMORY;
    ConvEntry *e0 = &h->entries[0];
    h->n_entries = 1;
    e0->epoch = pre->epoch;
    e0->key = *key;
    e0->own = own;
    if (mls_clone(pre, &e0->state) != 0) {
        history_free(h);
        return MARMOT_ERR_MLS;
    }
    if (commit && commit_len > 0) {
        e0->commit = malloc(commit_len);
        if (!e0->commit) {
            history_free(h);
            return MARMOT_ERR_MEMORY;
        }
        memcpy(e0->commit, commit, commit_len);
        e0->commit_len = commit_len;
    }
    uint64_t next = pre->epoch;
    for (size_t i = 0; old && i < old->n_entries && h->n_entries < CONV_MAX_REWIND_COMMITS; i++) {
        ConvEntry *e = &old->entries[i];
        if (e->epoch >= pre->epoch) continue;   /* `pre` replaces it (an older parent) */
        if (e->epoch + 1 != next) break;
        next = e->epoch;
        h->entries[h->n_entries++] = *e;        /* moved */
        memset(e, 0, sizeof(*e));
    }
    uint64_t anchor = h->entries[h->n_entries - 1].epoch;
    for (size_t i = 0; old && i < old->n_cands; i++) {
        ConvCandidate *c = &old->cands[i];
        if (c->source_epoch < anchor || c->source_epoch + CONV_MAX_REWIND_COMMITS < post->epoch ||
            memcmp(c->digest, key->digest, 32) == 0)
            continue;   /* stale, or now canonical */
        h->cands[h->n_cands++] = *c;            /* moved */
        memset(c, 0, sizeof(*c));
    }
    for (size_t i = 0; old && i < old->n_wits; i++)
        if (old->wits[i].epoch >= anchor) h->wits[h->n_wits++] = old->wits[i];
    for (size_t i = 0; old && i < old->n_secrets; i++)
        if (old->secrets[i].epoch >= anchor) h->secrets[h->n_secrets++] = old->secrets[i];
    *out = h;
    return MARMOT_OK;
}

MarmotError
marmot_commit_persist(Marmot *m, const MlsGroup *pre, const MlsGroup *post,
                      const MarmotCommitKey *key,
                      const MarmotGroupDataExtension *post_gde,
                      MarmotGroup *group)
{
    return marmot_commit_persist_ex(m, pre, post, key, post_gde, group, NULL, 0, false);
}

MarmotError
marmot_commit_persist_ex(Marmot *m, const MlsGroup *pre, const MlsGroup *post,
                         const MarmotCommitKey *key,
                         const MarmotGroupDataExtension *post_gde,
                         MarmotGroup *group, const uint8_t *commit, size_t commit_len,
                         bool own)
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
    ConvHistory *old_h = NULL, *new_h = NULL;
    uint64_t keep_from = post->epoch > 0 ? post->epoch - 1 : 0;

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

    /* An unreadable old record is replaced by a history of one epoch. */
    if (had_parent) {
        old_h = calloc(1, sizeof(*old_h));
        if (!old_h) {
            err = MARMOT_ERR_MEMORY;
            goto out;
        }
        if (conv_history_decode(old_parent, old_parent_len, old_h) != 0) {
            free(old_h);
            old_h = NULL;
        }
    }
    err = history_advance(old_h, pre, post, key, commit, commit_len, own, &new_h);
    if (err != MARMOT_OK) goto out;
    keep_from = new_h->entries[new_h->n_entries - 1].epoch;
    if (conv_history_encode(new_h, &new_parent, &new_parent_len) != 0 ||
        mls_group_serialize(post, &new_state, &new_state_len) != 0) {
        err = MARMOT_ERR_SERIALIZATION;
        goto out;
    }
    if (post_gde) {
        err = marmot_group_apply_group_data(group, post_gde);
        if (err != MARMOT_OK) goto out;
    } else if (post->profile == MARMOT_GROUP_PROFILE_ADOPTED) {
        /* The routing history it may write first is harmless if the rest
         * fails: an address the group had, routed to it. */
        err = adopted_record_apply(m, group, post);
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
    /* Proposals of epochs before the oldest retained one can no longer be
     * referenced by a Commit we could judge (nostrc-2um6); the exporter
     * secret of the epoch that left the window opens nothing any more
     * (retained-history.md: released with it).  Best effort: a failure
     * leaves records the epoch checks ignore. */
    if (err == MARMOT_OK) {
        marmot_proposals_prune(m, post->group_id, post->group_id_len, keep_from);
        if (post->epoch > CONV_MAX_REWIND_COMMITS)
            (void)s->delete_exporter_secret(s->ctx, &mgid,
                                            post->epoch - CONV_MAX_REWIND_COMMITS - 1);
    }
    sodium_memzero(old_exporter, sizeof(old_exporter));
    free_secret(old_state, old_state_len);
    free_secret(old_parent, old_parent_len);
    free_secret(new_state, new_state_len);
    free_secret(new_parent, new_parent_len);
    history_free(old_h);
    history_free(new_h);
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
        if (mls.profile == MARMOT_GROUP_PROFILE_ADOPTED) {
            err = adopted_record_apply(m, group, &mls);   /* nostrc-qp24.5.1.3 */
        } else {
            err = group_data_of(&mls, &mls, &gde);
            if (err == MARMOT_OK && gde) err = marmot_group_apply_group_data(group, gde);
        }
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
 *   since 0.12.0, only for a Commit of departures (nostrc-2um6), a trailer:
 *   u8  1, u32 proposal count, u8 n + n u32 SelfRemove leaves,
 *       u8 n + n u32 leaves that removed themselves by a Remove
 *   (libmarmot 0.11.0 cannot read such a record: after a downgrade it fails
 *   closed and the Commit is cleared.)
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
    bool            has_departures;   /* a Commit of departures (trailer) */
    MlsCommitSummary departures;
    uint8_t        *commit;           /* its MLSMessage (trailer, since W25; NULL
                                         in an earlier record) */
    size_t          commit_len;
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

/* pending_status(), knowing the retained history: a Commit of ours that a
 * reorg made canonical below the tip is merged too (nostrc-w1m0). */
static PendingStatus
pending_status_in(Marmot *m, const PendingCommit *p, const MlsGroup *cur)
{
    PendingStatus st = pending_status(p, cur);
    if (st != PENDING_STALE) return st;
    ConvHistory *h = NULL;
    if (history_load(m, cur->group_id, cur->group_id_len, &h) == MARMOT_OK && h)
        for (size_t i = 0; i < h->n_entries && st == PENDING_STALE; i++)
            if (memcmp(h->entries[i].key.digest, p->key.digest, 32) == 0) st = PENDING_MERGED;
    history_free(h);
    return st;
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
    free(p->commit);
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
    if (ok && p->has_departures) {
        const MlsCommitSummary *dp = &p->departures;
        ok = mls_tls_write_u8(&buf, PENDING_TRAILER_DEPARTURES) == 0 &&
             mls_tls_write_u32(&buf, (uint32_t)dp->proposal_count) == 0 &&
             mls_tls_write_u8(&buf, (uint8_t)dp->self_remove_count) == 0;
        for (size_t i = 0; ok && i < dp->self_remove_count; i++)
            ok = mls_tls_write_u32(&buf, dp->self_removed[i]) == 0;
        ok = ok && mls_tls_write_u8(&buf, (uint8_t)dp->left_count) == 0;
        for (size_t i = 0; ok && i < dp->left_count; i++)
            ok = mls_tls_write_u32(&buf, dp->left[i]) == 0;
    }
    /* W25 (nostrc-w1m0): the Commit's MLSMessage, kept in the retained
     * history once merged (a later reorg retains it as a candidate). */
    if (ok && p->commit && p->commit_len > 0)
        ok = mls_tls_write_u8(&buf, PENDING_TRAILER_COMMIT) == 0 &&
             mls_tls_write_opaque32(&buf, p->commit, p->commit_len) == 0;
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
        /* Optional trailers, each once and in this order: the departures
         * (1), the Commit's MLSMessage (2). */
        uint8_t last = 0;
        while (ok && !mls_tls_reader_done(&r)) {
            uint8_t marker = 0;
            ok = mls_tls_read_u8(&r, &marker) == 0 && marker > last;
            last = marker;
            if (ok && marker == PENDING_TRAILER_DEPARTURES) {
                MlsCommitSummary *dp = &out->departures;
                uint32_t pc = 0;
                uint8_t nsr = 0, nleft = 0;
                ok = mls_tls_read_u32(&r, &pc) == 0 && mls_tls_read_u8(&r, &nsr) == 0 &&
                     nsr <= MLS_COMMIT_SUMMARY_MAX;
                for (uint8_t i = 0; ok && i < nsr; i++)
                    ok = mls_tls_read_u32(&r, &dp->self_removed[i]) == 0;
                ok = ok && mls_tls_read_u8(&r, &nleft) == 0 && nleft <= MLS_COMMIT_SUMMARY_MAX;
                for (uint8_t i = 0; ok && i < nleft; i++)
                    ok = mls_tls_read_u32(&r, &dp->left[i]) == 0;
                if (ok) {
                    dp->proposal_count = pc;
                    dp->self_remove_count = nsr;
                    dp->left_count = nleft;
                    out->has_departures = true;
                }
            } else if (ok && marker == PENDING_TRAILER_COMMIT) {
                ok = mls_tls_read_opaque32(&r, &out->commit, &out->commit_len) == 0 &&
                     out->commit_len > 0;
            } else {
                ok = false;
            }
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
    return marmot_commit_stage_pending_ex(m, pre, post, commit, commit_len, event_json,
                                          welcomes, welcome_count, NULL);
}

MarmotError
marmot_commit_stage_pending_ex(Marmot *m, const MlsGroup *pre, const MlsGroup *post,
                               const uint8_t *commit, size_t commit_len,
                               const char *event_json,
                               const MarmotUnsentWelcome *welcomes, size_t welcome_count,
                               const MlsCommitSummary *departures)
{
    if (!m || !pre || !post || !commit || commit_len == 0 || !event_json ||
        welcome_count > PENDING_MAX_WELCOMES || (welcome_count && !welcomes))
        return MARMOT_ERR_INVALID_ARG;
    PendingCommit p;
    memset(&p, 0, sizeof(p));
    MarmotGroupDataExtension *gde = NULL;
    /* The same policy every receiver applies: never publish what the group
     * rejects. */
    MarmotError err = marmot_commit_authorize_ex(pre, post, pre->own_leaf_index,
                                                 m->config.allow_unproven_members,
                                                 departures, &p.key, &gde);
    if (departures) {
        p.has_departures = true;
        p.departures = *departures;
    }
    marmot_group_data_extension_free(gde);
    if (err != MARMOT_OK) return err;
    if (mls_crypto_hash(p.key.digest, commit, commit_len) != 0) return MARMOT_ERR_CRYPTO;
    p.parent_epoch = pre->epoch;
    memcpy(p.parent_transcript, pre->confirmed_transcript_hash, MLS_HASH_LEN);
    /* pending_store() only reads these: shallow views are enough. */
    p.post = *post;
    p.event_json = (char *)event_json;
    p.commit = (uint8_t *)commit;
    p.commit_len = commit_len;
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
        PendingStatus st = pending_status_in(m, &p, &cur);
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

static MarmotError pending_converge(Marmot *m, MarmotGroup *group, const MlsGroup *pre,
                                    const PendingCommit *p, MarmotMessageResult *result);

/* Apply the loaded LIVE pending Commit `p` on top of `cur` (`result`, may be
 * NULL: reports a change of the selected branch, see pending_converge()). */
static MarmotError
pending_apply(Marmot *m, MarmotGroup *group, const MlsGroup *cur, const PendingCommit *p,
              MarmotMessageResult *result)
{
    MarmotCommitKey key;
    MarmotGroupDataExtension *gde = NULL;
    MarmotError err = marmot_commit_authorize_ex(cur, &p->post, cur->own_leaf_index,
                                                 m->config.allow_unproven_members,
                                                 p->has_departures ? &p->departures : NULL,
                                                 &key, &gde);
    if (err != MARMOT_OK) {
        /* It can never apply: drop it rather than wedge the group. */
        if (pending_delete(m, group->mls_group_id.data, group->mls_group_id.len) == MARMOT_OK)
            marmot_txn_keep(m);   /* the drop is the outcome, not a failure */
        return err;
    }
    memcpy(key.digest, p->key.digest, 32);
    err = marmot_commit_persist_ex(m, cur, &p->post, &key, gde, group, p->commit, p->commit_len,
                                   true);
    marmot_group_data_extension_free(gde);
    /* On a storage error the record stays: merge again or clear it. */
    if (err == MARMOT_OK) err = pending_finish_merged(m, &group->mls_group_id, p);
    if (err == MARMOT_OK) err = pending_converge(m, group, cur, p, result);
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
        switch (pending_status_in(m, &p, &cur)) {
        case PENDING_LIVE:
            err = pending_apply(m, group, &cur, &p, NULL);
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
    if (err == MARMOT_OK && pending_status_in(m, &p, &cur) == PENDING_MERGED) {
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
              MarmotGroupDataExtension **gde, MlsCommitSummary *departures_out)
{
    if (mls_clone(parent, post) != 0) return MARMOT_ERR_MLS;
    /* Its references resolve against the proposals of the parent's epoch
     * (nostrc-2um6). */
    MarmotProposalSet set;
    MarmotError lerr = marmot_proposals_load_epoch((Marmot *)m, parent->group_id,
                                                   parent->group_id_len, parent->epoch, &set);
    if (lerr != MARMOT_OK) {
        mls_group_free(post);
        return lerr;
    }
    MlsCommitSummary departures;
    int rc = mls_group_process_commit_by_ref(post, msg, msg_len, sender, set.acs, set.ac_lens,
                                             set.ac_count, &departures);
    marmot_proposals_clear(&set);
    if (rc != 0) {
        mls_group_free(post);
        if (rc == MARMOT_ERR_UNSUPPORTED || rc == MARMOT_ERR_OWN_COMMIT_PENDING ||
            rc == MARMOT_ERR_MEMORY || rc == MARMOT_ERR_PROPOSAL_UNKNOWN)
            return (MarmotError)rc;
        return MARMOT_ERR_MLS_PROCESS_MESSAGE;
    }
    MarmotError err = marmot_commit_authorize_ex(parent, post, sender,
                                                 m->config.allow_unproven_members, &departures,
                                                 key, gde);
    if (err != MARMOT_OK) mls_group_free(post);
    else if (departures_out) *departures_out = departures;
    return err;
}

/* mls_group_commit_removes_self_by_ref() against the proposals of `base`'s
 * epoch.  *departures: what the Commit did with departure requests. */
static int
removes_self(const Marmot *m, const MlsGroup *base, const uint8_t *msg, size_t msg_len,
             uint32_t sender, bool *removed, MlsCommitSummary *departures)
{
    *removed = false;
    memset(departures, 0, sizeof(*departures));
    MarmotProposalSet set;
    if (marmot_proposals_load_epoch((Marmot *)m, base->group_id, base->group_id_len,
                                    base->epoch, &set) != MARMOT_OK)
        return -1;
    int rc = mls_group_commit_removes_self_by_ref(base, msg, msg_len, sender, set.acs,
                                                  set.ac_lens, set.ac_count, removed,
                                                  departures);
    marmot_proposals_clear(&set);
    return rc;
}

/* Slice H review L2: `err`, the refusal of an inbound Commit of the current
 * epoch, as MARMOT_ERR_COMMIT_REFUSED when it is for good and the group's
 * own -- an adopted group, an authenticated Commit, one of its admins' (a
 * non-admin's junk, which every member refuses, keeps its error), refused
 * for its content -- so that the application says the group stopped there
 * instead of waiting for nothing.  Transient outcomes (a proposal not
 * received yet, storage, our own pending Commit) keep theirs, as does
 * MARMOT_ERR_KEY_PACKAGE_IDENTITY (its own refusal state, nostrc-prrl). */
static MarmotError
refused_for_good(const MlsGroup *cur, const uint8_t *msg, size_t msg_len, uint32_t sender,
                 MarmotError err)
{
    if (cur->profile != MARMOT_GROUP_PROFILE_ADOPTED) return err;
    switch (err) {
    case MARMOT_ERR_MLS_PROCESS_MESSAGE:
    case MARMOT_ERR_UNSUPPORTED:
    case MARMOT_ERR_VALIDATION:
    case MARMOT_ERR_EXTENSION_FORMAT:
    case MARMOT_ERR_PROTOCOL_GROUP_MISMATCH:
    case MARMOT_ERR_IDENTITY_CHANGE:
    case MARMOT_ERR_COMMIT_FROM_NON_ADMIN:
    case MARMOT_ERR_FROM_NON_MEMBER:
        break;
    default:
        return err;
    }
    uint8_t id[32];
    bool admin = false;
    if (mls_group_commit_authentic(cur, msg, msg_len, sender) != 0 ||
        marmot_mls_sender_identity(cur, sender, id) != 0 ||
        marmot_policy_is_admin(cur, id, &admin) != MARMOT_OK || !admin)
        return err;
    return MARMOT_ERR_COMMIT_REFUSED;
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
 * removes our leaf (mls_group_commit_removes_self()), which we cannot apply
 * (its UpdatePath is encrypted to the others).
 *
 * It is judged whole, as a Commit we could apply (slice H review L1; MDK
 * runs its full staged-Commit validation before it acts on a removal,
 * message_processor/ingest.rs): its public result -- every proposal
 * applied, the committer's UpdatePath leaf, the GroupContext
 * (mls_group_commit_public_result_by_ref()), which a removed member can
 * compute -- passes the resulting-epoch check and marmot_commit_authorize_ex()
 * of its profile: admin authority, every GroupContext change (an adopted
 * group's dictionary and lifecycle rules, a legacy group's GroupData), and
 * every new leaf's proof (an absent one allowed, an invalid one never: R1
 * below), and the UpdatePath's parent-hash chain.  A Commit the other
 * members refuse never ends the group for us.  Before, only the committer's
 * authority was judged.  What a removed member can never check is the
 * confirmation tag (it lacks the new epoch's secrets; MDK has the same
 * limit): an admin determined to can still make a removed member alone see
 * its removal. */
static MarmotError
removal_key(const Marmot *m, const MlsGroup *pre, const uint8_t *msg, size_t msg_len,
            uint32_t committer_leaf, const uint8_t digest[32],
            const MlsCommitSummary *departures, MarmotCommitKey *key)
{
    (void)departures;   /* the public result's summary is the same Commit's */
    memset(key, 0, sizeof(*key));
    if (pre->profile != MARMOT_GROUP_PROFILE_LEGACY &&
        pre->profile != MARMOT_GROUP_PROFILE_ADOPTED)
        return MARMOT_ERR_UNSUPPORTED;
    MarmotProposalSet set;
    MarmotError err = marmot_proposals_load_epoch((Marmot *)m, pre->group_id,
                                                  pre->group_id_len, pre->epoch, &set);
    if (err != MARMOT_OK) return err;
    MlsGroup pub;
    MlsCommitSummary sum;
    memset(&sum, 0, sizeof(sum));
    int rc = mls_group_commit_public_result_by_ref(pre, msg, msg_len, committer_leaf, set.acs,
                                                   set.ac_lens, set.ac_count, &pub, &sum);
    marmot_proposals_clear(&set);
    if (rc != 0)
        return rc == MARMOT_ERR_UNSUPPORTED || rc == MARMOT_ERR_MEMORY ||
                       rc == MARMOT_ERR_PROPOSAL_UNKNOWN
                   ? (MarmotError)rc : MARMOT_ERR_MLS_PROCESS_MESSAGE;
    /* Leaves without the account proof are allowed here even when the
     * account requires proofs (slice H re-review R1): the other members
     * may admit them (legacy default mode, MDK 0.8), so such a Commit
     * really does end the group for us, and "only join verified groups"
     * cannot keep us in one.  A proof that does not verify is still
     * refused (in an adopted group every leaf's proof is checked anyway):
     * a forged Add never makes us believe we were removed. */
    MarmotGroupDataExtension *gde = NULL;
    err = marmot_commit_authorize_ex(pre, &pub, committer_leaf, true, &sum, key, &gde);
    marmot_group_data_extension_free(gde);
    mls_group_free(&pub);
    memcpy(key->digest, digest, 32);
    return err;
}

static void
fill_commit_result(Marmot *m, MarmotGroup *group, MarmotMessageResult *result)
{
    result->type = MARMOT_RESULT_COMMIT;
    result->commit.updated_group = NULL;
    result->commit.committer_leaf = UINT32_MAX;
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
    bool            left;         /* it committed our own SelfRemove (nostrc-2um6) */
    MarmotCommitKey key;          /* privileged, unless it commits SelfRemoves only */
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
                       (rm->final ? REMOVED_FINAL : 0) | (rm->left ? REMOVED_LEFT : 0) |
                       (rm->key.privileged ? 0 : REMOVED_ORDINARY));
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
              (rec[9] & ~(REMOVED_FROM_PARENT | REMOVED_FINAL | REMOVED_LEFT |
                          REMOVED_ORDINARY)) == 0;
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
    out->left = (rec[9] & REMOVED_LEFT) != 0;
    out->key.privileged = (rec[9] & REMOVED_ORDINARY) == 0;
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
    /* An adopted group's admins are its 0x8003 component's (nostrc-
     * qp24.5.1.3); a group of neither profile never makes a removal final
     * (W24 review H1). */
    bool adopted = base->profile == MARMOT_GROUP_PROFILE_ADOPTED;
    if (!adopted && base->profile != MARMOT_GROUP_PROFILE_LEGACY) return true;
    MarmotGroupDataExtension *gde = NULL;
    bool gde_ok = !adopted && group_data_of(base, base, &gde) == MARMOT_OK;
    bool contested = false;
    for (uint32_t i = 0; i < base->tree.n_leaves && !contested; i++) {
        const MlsLeafNode *leaf = leaf_at(base, i);
        if (!leaf || i == base->own_leaf_index || leaf->credential_identity_len != 32 ||
            !leaf->credential_identity ||
            memcmp(leaf->credential_identity, key->committer, 32) == 0)
            continue;
        contested = adopted ? adopted_could_win(base, leaf->credential_identity, key)
                            : (!gde_ok || !gde || could_win(gde, leaf->credential_identity, key));
    }
    marmot_group_data_extension_free(gde);
    return contested;
}

/* A removal of the current epoch (`on_parent` false) also stands only once
 * the Commit that led to that epoch (left `parent`, the retained full
 * parent, NULL when none) cannot lose a same-epoch race any more: no other
 * member of the parent could publish one that beats it.  A realized removal
 * is terminal for our leaf (member-departure.md "Realizing removal": no
 * convergence runs to resurrect it), so deeper branches are not waited for.
 * Until 0.12.0 a member seen at the new epoch also settled the parent
 * (nostrc-yuj2); now only MARMOT_REMOVAL_FINAL_AFTER does (above). */
static bool
removal_final(const MlsGroup *base, const MarmotCommitKey *key, bool on_parent,
              const ConvEntry *parent)
{
    if (removal_contested(base, key)) return false;
    return on_parent || !parent || !removal_contested(&parent->state, &parent->key);
}

/* The newest retained entry, when it is the full parent of `epoch`. */
static ConvEntry *
history_parent_of(ConvHistory *h, uint64_t epoch)
{
    if (!h || h->n_entries == 0) return NULL;
    ConvEntry *e = &h->entries[0];
    return !e->reader && e->epoch + 1 == epoch ? e : NULL;
}

/* N1: the removed epoch's secrets go once the removal is final. */
static MarmotError
forget_keys(Marmot *m, const MarmotGroup *group)
{
    MarmotStorage *s = m->storage;
    const uint8_t *gid = group->mls_group_id.data;
    size_t gid_len = group->mls_group_id.len;
    /* Who vouched for which device at our join (members.c) goes too. */
    MarmotError werr = s->mls_delete(s->ctx, "welcome_signer", gid, gid_len);
    if (werr != MARMOT_OK && werr != MARMOT_ERR_STORAGE_NOT_FOUND) return werr;
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
    /* Its proposals and our leave request (nostrc-2um6) end with it. */
    if (err == MARMOT_OK) marmot_proposals_forget(m, gid, gid_len);
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
      const MarmotCommitKey *key, uint64_t epoch, bool left)
{
    MarmotStorage *s = m->storage;
    if (!s->mls_store || !s->mls_delete || !s->save_group) return MARMOT_ERR_STORAGE;
    if (base->profile != MARMOT_GROUP_PROFILE_LEGACY &&
        base->profile != MARMOT_GROUP_PROFILE_ADOPTED)
        return MARMOT_ERR_UNSUPPORTED;
    const uint8_t *gid = group->mls_group_id.data;
    size_t gid_len = group->mls_group_id.len;
    ConvHistory *h = NULL;
    if (!on_parent && history_load(m, gid, gid_len, &h) != MARMOT_OK) h = NULL;
    Removal rm = { .epoch = epoch, .from_parent = on_parent, .key = *key, .left = left };
    rm.final = removal_final(base, key, on_parent,
                             on_parent ? NULL : history_parent_of(h, base->epoch));
    history_free(h);
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
marmot_get_group_left(Marmot *m, const MarmotGroupId *mls_group_id, bool *out_left)
{
    if (!m || !mls_group_id || !out_left) return MARMOT_ERR_INVALID_ARG;
    *out_left = false;
    Removal rm;
    MarmotError err = removal_load(m, mls_group_id->data, mls_group_id->len, &rm);
    if (err == MARMOT_ERR_STORAGE_NOT_FOUND) return MARMOT_OK;
    if (err != MARMOT_OK) return err;
    *out_left = rm.left;
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

MarmotError
marmot_commit_judge(Marmot *m, const MarmotGroupId *gid, const uint8_t *msg, size_t msg_len)
{
    if (!m || !gid || !msg) return MARMOT_ERR_INVALID_ARG;
    CommitRoute route;
    if (!commit_route(msg, msg_len, gid->data, gid->len, &route) || !route.commit)
        return MARMOT_ERR_MLS_FRAMING;
    MlsGroup cur;
    MarmotError err = load_current(m, gid, &cur);
    if (err != MARMOT_OK) {
        mls_group_free(&cur);
        return err;
    }
    uint32_t sender = UINT32_MAX;
    MlsGroup post;
    memset(&post, 0, sizeof(post));
    MarmotCommitKey key;
    MarmotGroupDataExtension *gde = NULL;
    if (route.epoch != cur.epoch || !commit_sender_on(&cur, msg, msg_len, &route, &sender)) {
        err = MARMOT_ERR_WRONG_EPOCH;
    } else {
        err = stage_inbound(m, &cur, msg, msg_len, sender, &post, &key, &gde, NULL);
        bool removed = false;
        MlsCommitSummary dep;
        uint8_t digest[32] = {0};
        if ((err == MARMOT_ERR_MLS_PROCESS_MESSAGE || err == MARMOT_ERR_PROPOSAL_UNKNOWN) &&
            removes_self(m, &cur, msg, msg_len, sender, &removed, &dep) == 0 && removed)
            err = removal_key(m, &cur, msg, msg_len, sender, digest, &dep, &key);
    }
    mls_group_free(&post);
    mls_group_free(&cur);
    marmot_group_data_extension_free(gde);
    return err;
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
    MarmotError err = stage_inbound(m, base, msg, msg_len, sender, &post, key, &gde, NULL);
    mls_group_free(&post);
    marmot_group_data_extension_free(gde);
    bool removed = false;
    MlsCommitSummary dep;
    if ((err == MARMOT_ERR_MLS_PROCESS_MESSAGE || err == MARMOT_ERR_PROPOSAL_UNKNOWN) &&
        removes_self(m, base, msg, msg_len, sender, &removed, &dep) == 0 && removed)
        err = removal_key(m, base, msg, msg_len, sender, digest, &dep, key);
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
inbound_mark(Marmot *m, MarmotGroup *group, uint64_t epoch, const char *event_id_hex)
{
    MarmotStorage *s = m->storage;
    if (event_id_hex && s->save_processed_message) {
        uint8_t id[32];
        if (strlen(event_id_hex) == 64 && marmot_hex_decode(event_id_hex, id, 32) == 0)
            (void)s->save_processed_message(s->ctx, id, id, marmot_now(), epoch,
                                            &group->mls_group_id,
                                            MARMOT_MSG_STATE_PROCESSED, NULL);
    }
}

static void
inbound_done(Marmot *m, MarmotGroup *group, uint64_t epoch, const char *event_id_hex,
             MarmotMessageResult *result)
{
    inbound_mark(m, group, epoch, event_id_hex);
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
    ConvHistory *h = NULL;
    (void)history_load(m, gid, gid_len, &h);   /* none (or unreadable): no parent to judge on */
    err = load_current(m, &group->mls_group_id, &cur);
    if (err != MARMOT_OK) {
        history_free(h);
        mls_group_free(&cur);
        return MARMOT_ERR_MLS;
    }
    if (cur.profile != MARMOT_GROUP_PROFILE_LEGACY &&
        cur.profile != MARMOT_GROUP_PROFILE_ADOPTED) {   /* W24 review H1 */
        mls_group_free(&cur);
        history_free(h);
        return MARMOT_ERR_UNSUPPORTED;
    }
    const MlsGroup *base = NULL;
    const MarmotCommitKey *beat = NULL;
    bool on_parent = false;
    /* The retained parent of the current state, in full. */
    ConvEntry *parent = history_parent_of(h, cur.epoch);
    bool full_parent = parent && parent->epoch == epoch;
    if (epoch == rm.epoch && !rm.from_parent && cur.epoch == epoch) {
        base = &cur;
        beat = &rm.key;
    } else if (epoch == rm.epoch && rm.from_parent && full_parent) {
        base = &parent->state;
        beat = &rm.key;
        on_parent = true;
    } else if (!rm.from_parent && epoch + 1 == rm.epoch && full_parent &&
               memcmp(digest, parent->key.digest, 32) != 0) {
        base = &parent->state;   /* it would replace the Commit that led there */
        beat = &parent->key;
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
        err = stage_inbound(m, base, msg, msg_len, sender, &post, &key, &gde, NULL);
        if (err == MARMOT_OK) {
            memcpy(key.digest, digest, 32);
            if (commit_key_cmp(&key, beat) < 0) {
                /* It wins that epoch and keeps us: the removal is undone. */
                group->state = MARMOT_GROUP_STATE_ACTIVE;
                err = marmot_commit_persist_ex(m, base, &post, &key, gde, group, msg, msg_len,
                                               false);
                if (err == MARMOT_OK) err = removal_delete(m, gid, gid_len);
                if (err != MARMOT_OK) group->state = MARMOT_GROUP_STATE_INACTIVE;
            } else {
                err = MARMOT_ERR_WRONG_EPOCH;
            }
        } else if (err == MARMOT_ERR_MLS_PROCESS_MESSAGE || err == MARMOT_ERR_PROPOSAL_UNKNOWN) {
            bool removed = false;
            MlsCommitSummary dep;
            if (removes_self(m, base, msg, msg_len, sender, &removed, &dep) == 0 && removed) {
                err = removal_key(m, base, msg, msg_len, sender, digest, &dep, &key);
                if (err == MARMOT_OK && commit_key_cmp(&key, beat) < 0) {
                    /* Another removal wins that epoch. */
                    Removal next = { .epoch = epoch, .from_parent = on_parent, .key = key,
                                     .left = summary_departs(&dep, base->own_leaf_index) };
                    next.final = removal_final(base, &key, on_parent,
                                               on_parent ? NULL
                                                         : history_parent_of(h, base->epoch));
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
    history_free(h);
    return err;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Convergence (nostrc-w1m0; Marmot protocol-core/convergence.md)
 *
 * Commits are the consensus log, and transport order decides nothing.  A
 * Commit that does not linearly advance the canonical tip -- a competitor
 * of an applied Commit, a child of a competitor, one that lost -- is
 * retained as a candidate (convergence.h) while its source epoch is inside
 * the rollback horizon, and every admitted input that can change the
 * selection resolves the retained set again, exactly as MDK v0.11.0's
 * canonicalizer does (openmls_projection.rs, canonicalization.rs):
 *
 *   - replay start (every branch's fork_epoch): the oldest source epoch of a
 *     retained candidate, or the tip;
 *   - candidate branches: from the canonical state of that epoch, every
 *     maximal path through the canonical Commits and the candidates, each
 *     Commit staged with full validation against its parent -- the state
 *     whose exporter secret sealed it, which its MLS authentication
 *     (membership tag, sender data) binds it to;
 *   - selection: higher effective depth (tip epoch minus replay start, plus
 *     max_witness_override_depth when one branch epoch has
 *     witness_quorum_senders_per_epoch distinct witnesses), quorum over
 *     none, higher app_witness_score, privileged before ordinary, lower
 *     committer, lower digest of the tip Commit;
 *   - application: the selected branch becomes canonical -- a linear advance
 *     by one or more Commits, or a reorg that supersedes Commits this member
 *     had applied (they are retained as candidates, and the messages of
 *     their epochs are withdrawn: MarmotMessageResult.convergence).  The
 *     tip epoch may go down: a witnessed branch beats a longer one by at
 *     most one Commit.
 *
 * Not implemented (nostrc-w1m0 stays open): the pass timers
 * (settlement_quiescence_ms, max_convergence_pass_ms) and the Syncing and
 * Resolving phases with outbound work held until a pass settles -- here
 * every admitted input resolves at once, which reaches the same branch once
 * input closes (convergence.md: pass partitioning must not change the
 * result) but exposes intermediate selections; the disband lifecycle;
 * withdrawing a Welcome a losing branch's Add sent.
 * ──────────────────────────────────────────────────────────────────────── */

typedef enum { CAND_WAITING = 0, CAND_ATTACHED, CAND_DEAD, CAND_HELD } CandStatus;

typedef struct {
    MlsGroup        own_state;     /* a candidate's state (owned) */
    const MlsGroup *state;         /* canonical: borrowed; NULL: a removal of our leaf */
    uint64_t        epoch;         /* of `state`; of a removal, the epoch it enters */
    int             parent;        /* -1: the root */
    int             cand;          /* the candidate that entered it; -1: canonical */
    bool            canonical;
    bool            has_child;
    MarmotCommitKey key;           /* of the Commit that entered it (not the root) */
    uint8_t         tag[32];       /* the state's confirmed transcript hash */
    MlsCommitSummary departures;   /* what that Commit did with departure requests */
    bool            removal_left;  /* a removal of our leaf: our own departure */
} ConvNode;

#define CONV_MAX_NODES (CONV_MAX_REWIND_COMMITS + 1 + CONV_MAX_CANDIDATES)

typedef struct {
    ConvNode       *nodes;
    size_t          n_nodes;
    uint64_t        root_epoch;    /* replay start: the fork_epoch of every branch */
    int             tip;           /* the canonical tip */
    CandStatus      status[CONV_MAX_CANDIDATES];
    MarmotError     err[CONV_MAX_CANDIDATES];       /* a dead or held one's refusal */
    MarmotCommitKey key[CONV_MAX_CANDIDATES];       /* authorize()'s, also when refused */
    int             node_of[CONV_MAX_CANDIDATES];
} ConvTree;

typedef enum { CONV_UNCHANGED, CONV_ADVANCED, CONV_REMOVED } ConvOutcome;

static void
tree_clear(ConvTree *t)
{
    for (size_t i = 0; t->nodes && i < t->n_nodes; i++) mls_group_free(&t->nodes[i].own_state);
    free(t->nodes);
    memset(t, 0, sizeof(*t));
}

/* Candidate `i` failed against node `p`, its parent, for good (`held`: it
 * waits for a proposal). */
static void
cand_refused(ConvTree *t, size_t i, MarmotError err, bool held)
{
    t->status[i] = held ? CAND_HELD : CAND_DEAD;
    t->err[i] = err;
}

/* Stage candidate `i` of `h` on node `p`.  Only an allocation failure fails
 * the pass; a Commit invalid against its parent is dead (terminal:
 * convergence.md "Candidate branches", authorization_failed). */
static MarmotError
conv_attach(const Marmot *m, ConvTree *t, const ConvHistory *h, size_t i, int p)
{
    const ConvCandidate *c = &h->cands[i];
    const MlsGroup *parent = t->nodes[p].state;
    ConvNode *n = &t->nodes[t->n_nodes];
    memset(n, 0, sizeof(*n));
    n->parent = p;
    n->cand = (int)i;
    n->epoch = c->source_epoch + 1;
    bool removal = false;
    if (c->own) {
        /* Ours: we cannot process our own Commit (its UpdatePath is not
         * encrypted to us); its result and priority were kept. */
        if (mls_group_deserialize(c->own_post, c->own_post_len, &n->own_state) != 0 ||
            n->own_state.epoch != n->epoch ||
            n->own_state.group_id_len != parent->group_id_len ||
            memcmp(n->own_state.group_id, parent->group_id, parent->group_id_len) != 0 ||
            marmot_mls_sender_identity(parent, parent->own_leaf_index, n->key.committer) != 0) {
            mls_group_free(&n->own_state);
            cand_refused(t, i, MARMOT_ERR_MLS, false);
            return MARMOT_OK;
        }
        n->key.privileged = c->own_privileged;
        n->key.committer_leaf = parent->own_leaf_index;
    } else {
        CommitRoute route;
        uint32_t sender = UINT32_MAX;
        if (!commit_route(c->msg, c->msg_len, parent->group_id, parent->group_id_len, &route) ||
            !route.commit || !route.group_ok || route.epoch != c->source_epoch ||
            !commit_sender_on(parent, c->msg, c->msg_len, &route, &sender)) {
            cand_refused(t, i, MARMOT_ERR_MLS_PROCESS_MESSAGE, false);
            return MARMOT_OK;
        }
        MarmotGroupDataExtension *gde = NULL;
        MarmotError err = stage_inbound(m, parent, c->msg, c->msg_len, sender, &n->own_state,
                                        &n->key, &gde, &n->departures);
        marmot_group_data_extension_free(gde);
        t->key[i] = n->key;
        memcpy(t->key[i].digest, c->digest, 32);
        if (err == MARMOT_ERR_MEMORY) return err;
        if (err == MARMOT_ERR_MLS_PROCESS_MESSAGE || err == MARMOT_ERR_PROPOSAL_UNKNOWN) {
            /* A Commit that removes our leaf cannot be processed by us
             * (nostrc-xrya): judged whole, it is a branch that ends here. */
            bool removed = false;
            MlsCommitSummary dep;
            if (removes_self(m, parent, c->msg, c->msg_len, sender, &removed, &dep) == 0 &&
                removed) {
                MarmotError kerr = removal_key(m, parent, c->msg, c->msg_len, sender, c->digest,
                                               &dep, &n->key);
                if (kerr == MARMOT_ERR_MEMORY) return kerr;
                if (kerr != MARMOT_OK) {
                    cand_refused(t, i, kerr, kerr == MARMOT_ERR_PROPOSAL_UNKNOWN);
                    return MARMOT_OK;
                }
                removal = true;
                n->removal_left = summary_departs(&dep, parent->own_leaf_index);
                n->departures = dep;
            } else {
                cand_refused(t, i, err, err == MARMOT_ERR_PROPOSAL_UNKNOWN);
                return MARMOT_OK;
            }
        } else if (err != MARMOT_OK) {
            /* A Commit of our own leaf that we did not make (or lost track
             * of) is not ours to judge: refused like a competitor that
             * loses. */
            cand_refused(t, i, err == MARMOT_ERR_OWN_COMMIT_PENDING ? MARMOT_ERR_WRONG_EPOCH
                                                                     : err, false);
            return MARMOT_OK;
        }
    }
    memcpy(n->key.digest, c->digest, 32);
    t->key[i] = n->key;
    if (!removal) {
        n->state = &n->own_state;
        memcpy(n->tag, n->own_state.confirmed_transcript_hash, 32);
    }
    t->nodes[p].has_child = true;
    t->node_of[i] = (int)t->n_nodes;
    t->status[i] = CAND_ATTACHED;
    t->n_nodes++;
    return MARMOT_OK;
}

/* convergence.md "Candidate branches": the canonical path from the replay
 * start to the tip `cur`, then every retained candidate whose parent is a
 * node, staged against it (a Commit authenticates against one state only,
 * so it enters one node at most, and is staged once per pass).  The
 * candidates of `h` must be inside the horizon. */
static MarmotError
conv_build(const Marmot *m, const MlsGroup *cur, ConvHistory *h, ConvTree *t)
{
    memset(t, 0, sizeof(*t));
    t->nodes = calloc(CONV_MAX_NODES, sizeof(*t->nodes));
    if (!t->nodes) return MARMOT_ERR_MEMORY;
    uint64_t tip = cur->epoch;
    uint64_t anchor = history_anchor(h, tip);
    uint64_t r = tip;
    for (size_t i = 0; i < h->n_cands; i++) {
        t->node_of[i] = -1;
        if (h->cands[i].source_epoch >= anchor && h->cands[i].source_epoch < r)
            r = h->cands[i].source_epoch;
    }
    t->root_epoch = r;
    for (uint64_t e = r; e <= tip; e++) {
        ConvNode *n = &t->nodes[t->n_nodes];
        n->state = canonical_state_at(h, cur, e);
        if (!n->state) {   /* cannot happen at or above the anchor */
            tree_clear(t);
            return MARMOT_ERR_MLS;
        }
        n->epoch = e;
        n->parent = (int)t->n_nodes - 1;
        n->cand = -1;
        n->canonical = true;
        memcpy(n->tag, n->state->confirmed_transcript_hash, 32);
        if (e > r) {
            n->key = conv_history_entry(h, e - 1)->key;
            t->nodes[n->parent].has_child = true;
        }
        t->n_nodes++;
    }
    t->tip = (int)t->n_nodes - 1;
    for (bool progress = true; progress;) {
        progress = false;
        for (size_t i = 0; i < h->n_cands; i++) {
            if (t->status[i] != CAND_WAITING) continue;
            const ConvCandidate *c = &h->cands[i];
            int p = -1;
            for (size_t k = 0; k < t->n_nodes && p < 0; k++)
                if (t->nodes[k].state && t->nodes[k].epoch == c->source_epoch &&
                    memcmp(t->nodes[k].tag, c->parent_tag, 32) == 0)
                    p = (int)k;
            if (p < 0) continue;
            progress = true;
            MarmotError err = conv_attach(m, t, h, i, p);
            if (err != MARMOT_OK) {
                tree_clear(t);
                return err;
            }
        }
    }
    return MARMOT_OK;
}

/* convergence.md "App-payload witnesses": the branch ending at `leaf`. */
static void
conv_score(const ConvTree *t, const ConvHistory *h, int leaf, ConvBranchScore *s)
{
    const ConvNode *tipn = &t->nodes[leaf];
    memset(s, 0, sizeof(*s));
    s->fork_epoch = t->root_epoch;
    s->tip_epoch = tipn->epoch;
    size_t quorum_epochs = 0;
    for (int i = leaf; i >= 0; i = t->nodes[i].parent) {
        const ConvNode *n = &t->nodes[i];
        if (n->epoch <= t->root_epoch) break;          /* at or before fork_epoch */
        if (!n->state) continue;                       /* a removal: unreadable for us */
        if (tipn->epoch - n->epoch > CONV_APP_PAYLOAD_PAST_EPOCH_LIMIT) continue;
        size_t c = conv_witness_count(h, n->epoch, n->tag);
        s->witness_score += c;
        if (c >= CONV_WITNESS_QUORUM_SENDERS) quorum_epochs++;
    }
    s->quorum = quorum_epochs >= CONV_WITNESS_QUORUM_EPOCHS;
    s->privileged = tipn->key.privileged;
    memcpy(s->committer, tipn->key.committer, 32);
    memcpy(s->digest, tipn->key.digest, 32);
}

/* convergence.md "Branch selection": the selected branch's tip node. */
static int
conv_select(const ConvTree *t, const ConvHistory *h)
{
    int best = -1;
    ConvBranchScore bs;
    memset(&bs, 0, sizeof(bs));
    for (size_t k = 0; k < t->n_nodes; k++) {
        const ConvNode *n = &t->nodes[k];
        if (n->has_child) continue;
        if (n->parent < 0) {        /* the tip, alone: nothing else to select */
            if (best < 0) best = (int)k;
            continue;
        }
        ConvBranchScore s;
        conv_score(t, h, (int)k, &s);
        /* Our own pending Commit, superseded before a relay confirmed it,
         * ends no selectable branch until it shows it was published (a
         * witness of its epoch; a Commit on top of it makes it no tip):
         * publish-before-apply (publish-lifecycle.md). */
        if (n->cand >= 0 && h->cands[n->cand].own && h->cands[n->cand].own_unconfirmed &&
            conv_witness_count(h, n->epoch, n->tag) == 0)
            continue;
        if (best < 0 || t->nodes[best].parent < 0 || conv_branch_cmp(&s, &bs) > 0) {
            best = (int)k;
            bs = s;
        }
    }
    return best;
}

static bool
node_on_path(const ConvTree *t, int leaf, int k)
{
    for (int i = leaf; i >= 0; i = t->nodes[i].parent)
        if (i == k) return true;
    return false;
}

/* Copy `len` bytes (NULL stays NULL). */
static int
dup_bytes(const uint8_t *p, size_t len, uint8_t **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    if (!p || len == 0) return 0;
    *out = malloc(len);
    if (!*out) return -1;
    memcpy(*out, p, len);
    *out_len = len;
    return 0;
}

/* Candidate order for the bound: oldest source epoch first (the nearest to
 * expiry), then digest. */
static int
cand_age_cmp(const ConvCandidate *a, const ConvCandidate *b)
{
    if (a->source_epoch != b->source_epoch) return a->source_epoch < b->source_epoch ? -1 : 1;
    return memcmp(a->digest, b->digest, 32);
}

/* The retained state once the branch ending at node `sel` of `t` is
 * canonical (with `tip_epoch`, its tip): candidates that are not on it,
 * still attach and are inside the horizon -- plus, `demote` set, the
 * canonical Commits above node `fork` that it supersedes -- the witnesses
 * of states still retained, and the exporter secrets of every retained
 * candidate state.  `entries` of the result are not filled. */
static MarmotError
conv_retained(const ConvTree *t, const ConvHistory *h, const MlsGroup *cur, int sel,
              int fork, bool demote, uint64_t anchor, uint64_t tip_epoch, ConvHistory *out)
{
    /* Each node's candidate (index into out->cands), or -1. */
    int kept_as[CONV_MAX_NODES];
    for (size_t k = 0; k < CONV_MAX_NODES; k++) kept_as[k] = -1;
    ConvCandidate all[CONV_MAX_CANDIDATES + CONV_MAX_REWIND_COMMITS];
    int node_for[CONV_MAX_CANDIDATES + CONV_MAX_REWIND_COMMITS];
    size_t n = 0;
    memset(all, 0, sizeof(all));
    MarmotError err = MARMOT_OK;
    for (size_t i = 0; i < h->n_cands && err == MARMOT_OK; i++) {
        const ConvCandidate *c = &h->cands[i];
        int k = t->node_of[i];
        /* A dead one is invalid for good; one waiting for its parent (a
         * candidate held for a proposal) stays while inside the horizon. */
        if (t->status[i] == CAND_DEAD) continue;
        if (k >= 0 && node_on_path(t, sel, k)) continue;   /* canonical now */
        if (c->source_epoch < anchor || c->source_epoch + CONV_MAX_REWIND_COMMITS < tip_epoch)
            continue;                                      /* stale */
        ConvCandidate *d = &all[n];
        *d = *c;
        d->msg = NULL;
        d->event_id = NULL;
        d->own_post = NULL;
        if (dup_bytes(c->msg, c->msg_len, &d->msg, &d->msg_len) != 0 ||
            dup_bytes(c->own_post, c->own_post_len, &d->own_post, &d->own_post_len) != 0 ||
            (c->event_id && !(d->event_id = strdup(c->event_id))))
            err = MARMOT_ERR_MEMORY;
        node_for[n++] = k;
    }
    /* The canonical Commits the selected branch supersedes, epochs
     * fork..tip-1 of the old path, stay retained: their branch can still
     * win a later pass (convergence.md "Applying the selected branch"). */
    for (int k = t->tip; demote && err == MARMOT_OK && k > fork; k = t->nodes[k].parent) {
        const ConvNode *child = &t->nodes[k];
        const ConvNode *par = &t->nodes[child->parent];
        ConvEntry *e = conv_history_entry((ConvHistory *)h, par->epoch);
        if (!e || !e->commit || par->epoch < anchor ||
            par->epoch + CONV_MAX_REWIND_COMMITS < tip_epoch)
            continue;   /* not known (an earlier record), or stale */
        ConvCandidate *d = &all[n];
        memset(d, 0, sizeof(*d));
        d->source_epoch = par->epoch;
        memcpy(d->digest, e->key.digest, 32);
        memcpy(d->parent_tag, par->tag, 32);
        d->own = e->own;
        d->own_privileged = e->own && e->key.privileged;
        if (dup_bytes(e->commit, e->commit_len, &d->msg, &d->msg_len) != 0)
            err = MARMOT_ERR_MEMORY;
        if (err == MARMOT_OK && e->own &&
            mls_group_serialize(child->state, &d->own_post, &d->own_post_len) != 0)
            err = MARMOT_ERR_SERIALIZATION;
        node_for[n++] = k;
    }
    (void)cur;
    /* Over the bound (a reorg demotes up to five): the oldest go first. */
    while (err == MARMOT_OK && n > CONV_MAX_CANDIDATES) {
        size_t oldest = 0;
        for (size_t i = 1; i < n; i++)
            if (cand_age_cmp(&all[i], &all[oldest]) < 0) oldest = i;
        conv_candidate_clear(&all[oldest]);
        all[oldest] = all[n - 1];
        node_for[oldest] = node_for[n - 1];
        n--;
    }
    if (err != MARMOT_OK) {
        for (size_t i = 0; i < n; i++) conv_candidate_clear(&all[i]);
        return err;
    }
    for (size_t i = 0; i < n; i++) {
        out->cands[i] = all[i];
        if (node_for[i] >= 0) kept_as[node_for[i]] = (int)i;
    }
    out->n_cands = n;
    /* Witnesses of states still retained: a node of the tree, or a
     * canonical epoch at or before the replay start, inside the horizon. */
    out->n_wits = 0;
    for (size_t i = 0; i < h->n_wits; i++) {
        const ConvWitness *w = &h->wits[i];
        if (w->epoch < anchor) continue;
        bool found = w->epoch <= t->root_epoch;
        for (size_t k = 0; !found && k < t->n_nodes; k++)
            found = t->nodes[k].state && t->nodes[k].epoch == w->epoch &&
                    memcmp(t->nodes[k].tag, w->tag, 32) == 0;
        if (found) out->wits[out->n_wits++] = *w;
    }
    /* The exporter secrets of the retained candidate states. */
    out->n_secrets = 0;
    for (size_t k = 0; k < t->n_nodes && out->n_secrets < CONV_MAX_CANDIDATES; k++) {
        const ConvNode *nd = &t->nodes[k];
        if (!nd->state || kept_as[k] < 0) continue;
        ConvBranchSecret *s = &out->secrets[out->n_secrets++];
        s->epoch = nd->epoch;
        memcpy(s->tag, nd->tag, 32);
        memcpy(s->exporter, nd->state->epoch_secrets.exporter_secret, 32);
    }
    return MARMOT_OK;
}

/* ── Undo log for backends without transactions (nostrc-qp24.7) ───────── */

typedef enum { UNDO_KV, UNDO_EXPORTER, UNDO_GROUP, UNDO_MESSAGE } UndoKind;

typedef struct {
    UndoKind       kind;
    const char    *label;      /* UNDO_KV */
    uint8_t       *old;        /* UNDO_KV: the value (NULL: absent) */
    size_t         old_len;
    uint64_t       epoch;      /* UNDO_EXPORTER */
    bool           existed;
    uint8_t        secret[32];
    MarmotGroup   *group;      /* UNDO_GROUP: the record as it was */
    MarmotMessage *msg;        /* UNDO_MESSAGE: the message, its old state */
    MarmotMessageState state;
} UndoRec;

typedef struct {
    UndoRec *recs;
    size_t   n, cap;
} UndoLog;

static UndoRec *
undo_push(UndoLog *u)
{
    if (u->n == u->cap) {
        size_t cap = u->cap ? u->cap * 2 : 16;
        UndoRec *r = realloc(u->recs, cap * sizeof(*r));
        if (!r) return NULL;
        u->recs = r;
        u->cap = cap;
    }
    UndoRec *r = &u->recs[u->n++];
    memset(r, 0, sizeof(*r));
    return r;
}

static MarmotError
undo_kv(Marmot *m, UndoLog *u, const char *label, const MarmotGroupId *gid)
{
    UndoRec *r = undo_push(u);
    if (!r) return MARMOT_ERR_MEMORY;
    r->kind = UNDO_KV;
    r->label = label;
    MarmotError err = m->storage->mls_load(m->storage->ctx, label, gid->data, gid->len,
                                           &r->old, &r->old_len);
    if (err == MARMOT_ERR_STORAGE_NOT_FOUND || (err == MARMOT_OK && !r->old)) {
        r->old = NULL;
        return MARMOT_OK;
    }
    if (err != MARMOT_OK) u->n--;
    return err;
}

static MarmotError
undo_exporter(Marmot *m, UndoLog *u, const MarmotGroupId *gid, uint64_t epoch)
{
    UndoRec *r = undo_push(u);
    if (!r) return MARMOT_ERR_MEMORY;
    r->kind = UNDO_EXPORTER;
    r->epoch = epoch;
    MarmotError err = m->storage->get_exporter_secret(m->storage->ctx, gid, epoch, r->secret);
    r->existed = err == MARMOT_OK;
    if (err == MARMOT_ERR_STORAGE_NOT_FOUND) err = MARMOT_OK;
    if (err != MARMOT_OK) u->n--;
    return err;
}

static MarmotError
undo_group(Marmot *m, UndoLog *u, const MarmotGroupId *gid)
{
    UndoRec *r = undo_push(u);
    if (!r) return MARMOT_ERR_MEMORY;
    r->kind = UNDO_GROUP;
    MarmotError err = m->storage->find_group_by_mls_id(m->storage->ctx, gid, &r->group);
    if (err != MARMOT_OK || !r->group) {
        u->n--;
        return err != MARMOT_OK ? err : MARMOT_ERR_GROUP_NOT_FOUND;
    }
    return MARMOT_OK;
}

/* Put everything back, newest first.  MARMOT_OK, or MARMOT_ERR_STORAGE when
 * an undo failed (storage is then inconsistent). */
static MarmotError
undo_apply(Marmot *m, UndoLog *u, const MarmotGroupId *gid)
{
    MarmotStorage *s = m->storage;
    bool ok = true;
    for (size_t i = u->n; i-- > 0;) {
        UndoRec *r = &u->recs[i];
        MarmotError e = MARMOT_OK;
        switch (r->kind) {
        case UNDO_KV:
            e = r->old ? s->mls_store(s->ctx, r->label, gid->data, gid->len, r->old, r->old_len)
                       : s->mls_delete(s->ctx, r->label, gid->data, gid->len);
            if (e == MARMOT_ERR_STORAGE_NOT_FOUND) e = MARMOT_OK;
            break;
        case UNDO_EXPORTER:
            e = r->existed ? s->save_exporter_secret(s->ctx, gid, r->epoch, r->secret)
                           : s->delete_exporter_secret(s->ctx, gid, r->epoch);
            if (e == MARMOT_ERR_STORAGE_NOT_FOUND) e = MARMOT_OK;
            break;
        case UNDO_GROUP:
            e = s->save_group(s->ctx, r->group);
            break;
        case UNDO_MESSAGE:
            r->msg->state = r->state;
            e = s->save_message(s->ctx, r->msg);
            break;
        }
        ok &= e == MARMOT_OK;
    }
    return ok ? MARMOT_OK : MARMOT_ERR_STORAGE;
}

static void
undo_free(UndoLog *u)
{
    for (size_t i = 0; i < u->n; i++) {
        UndoRec *r = &u->recs[i];
        free_secret(r->old, r->old_len);
        sodium_memzero(r->secret, sizeof(r->secret));
        marmot_group_free(r->group);
        /* UNDO_MESSAGE borrows its message (freed with its page). */
    }
    free(u->recs);
    memset(u, 0, sizeof(*u));
}

/* Append `hex` to a list. */
static int
list_add(char ***list, size_t *n, char *hex)
{
    if (!hex) return -1;
    char **l = realloc(*list, (*n + 1) * sizeof(*l));
    if (!l) {
        free(hex);
        return -1;
    }
    l[(*n)++] = hex;
    *list = l;
    return 0;
}

/* The stored messages of `gid` received or sent in epochs after..upto of
 * the losing branch: their payloads decrypt only there (inbound-
 * processing.md "Delivered app payloads"), so they are withdrawn
 * (MARMOT_MSG_STATE_EPOCH_INVALIDATED) and reported.  `pages` keeps the
 * messages the undo log points to. */
static MarmotError
invalidate_messages(Marmot *m, const MarmotGroupId *gid, uint64_t after, uint64_t upto,
                    UndoLog *u, MarmotMessage ****pages, size_t **page_lens, size_t *n_pages,
                    char ***ids, size_t *n_ids)
{
    MarmotStorage *s = m->storage;
    if (!s->messages || !s->save_message) return MARMOT_OK;
    MarmotPagination pg = marmot_pagination_default();
    for (;;) {
        MarmotMessage **msgs = NULL;
        size_t count = 0;
        MarmotError err = s->messages(s->ctx, gid, &pg, &msgs, &count);
        if (err == MARMOT_ERR_STORAGE_NOT_FOUND) return MARMOT_OK;
        if (err != MARMOT_OK) return err;
        MarmotMessage ***pp = realloc(*pages, (*n_pages + 1) * sizeof(*pp));
        size_t *pl = realloc(*page_lens, (*n_pages + 1) * sizeof(*pl));
        if (pp) *pages = pp;
        if (pl) *page_lens = pl;
        if (!pp || !pl) {
            for (size_t i = 0; i < count; i++) marmot_message_free(msgs[i]);
            free(msgs);
            return MARMOT_ERR_MEMORY;
        }
        (*pages)[*n_pages] = msgs;
        (*page_lens)[*n_pages] = count;
        (*n_pages)++;
        for (size_t i = 0; i < count; i++) {
            MarmotMessage *msg = msgs[i];
            if (!msg || msg->epoch <= after || msg->epoch > upto ||
                msg->state == MARMOT_MSG_STATE_EPOCH_INVALIDATED ||
                msg->state == MARMOT_MSG_STATE_DELETED)
                continue;
            UndoRec *r = undo_push(u);
            if (!r) return MARMOT_ERR_MEMORY;
            r->kind = UNDO_MESSAGE;
            r->msg = msg;
            r->state = msg->state;
            msg->state = MARMOT_MSG_STATE_EPOCH_INVALIDATED;
            err = s->save_message(s->ctx, msg);
            if (err != MARMOT_OK) {
                msg->state = r->state;
                u->n--;
                return err;
            }
            if (list_add(ids, n_ids, marmot_hex_encode(msg->id, 32)) != 0)
                return MARMOT_ERR_MEMORY;
        }
        if (count < pg.limit) return MARMOT_OK;
        pg.offset += count;
    }
}

/* convergence.md "Applying the selected branch": make the branch ending at
 * node `sel` (a state) canonical in one observer-atomic step -- its states
 * and exporter secrets, the retained history, the group record and, for a
 * reorg, the withdrawal of the losing branch's messages -- and report it in
 * `result` (MARMOT_RESULT_COMMIT). */
static MarmotError
conv_install(Marmot *m, MarmotGroup *group, const MlsGroup *cur, const ConvHistory *h,
             const ConvTree *t, int sel, bool as_commit, MarmotMessageResult *result)
{
    MarmotStorage *s = m->storage;
    const ConvNode *best = &t->nodes[sel];
    const MarmotGroupId *gid = &group->mls_group_id;
    /* The new path, root first, and its last canonical node (the fork). */
    int path[CONV_MAX_NODES];
    size_t plen = 0;
    for (int i = sel; i >= 0; i = t->nodes[i].parent) path[plen++] = i;
    for (size_t a = 0, b = plen - 1; a < b; a++, b--) {
        int x = path[a];
        path[a] = path[b];
        path[b] = x;
    }
    size_t fork_at = 0;
    while (fork_at + 1 < plen && t->nodes[path[fork_at + 1]].canonical) fork_at++;
    int fork = path[fork_at];
    bool reorg = fork != t->tip;
    uint64_t new_tip = best->epoch;

    MarmotError err = MARMOT_OK;
    ConvHistory *nh = calloc(1, sizeof(*nh));
    UndoLog undo = { 0 };
    MarmotMessage ***pages = NULL;
    size_t *page_lens = NULL, n_pages = 0;
    char **invalidated = NULL, **superseded = NULL;
    size_t n_invalidated = 0, n_superseded = 0;
    uint8_t *state_blob = NULL, *rec_blob = NULL;
    size_t state_len = 0, rec_len = 0;
    MarmotGroupDataExtension *gde = NULL;
    if (!nh) return MARMOT_ERR_MEMORY;

    /* Retained entries: the new path's states, newest first, then the
     * canonical ones before the replay start. */
    for (size_t k = plen - 1; k-- > 0 && nh->n_entries < CONV_MAX_REWIND_COMMITS;) {
        const ConvNode *par = &t->nodes[path[k]];
        const ConvNode *child = &t->nodes[path[k + 1]];
        ConvEntry *e = &nh->entries[nh->n_entries++];
        e->epoch = par->epoch;
        e->key = child->key;
        const uint8_t *cb = NULL;
        size_t cl = 0;
        if (child->cand >= 0) {
            const ConvCandidate *c = &h->cands[child->cand];
            cb = c->msg;
            cl = c->msg_len;
            e->own = c->own;
        } else {
            const ConvEntry *old = conv_history_entry((ConvHistory *)h, par->epoch);
            if (old) {
                cb = old->commit;
                cl = old->commit_len;
                e->own = old->own;
            }
        }
        if (dup_bytes(cb, cl, &e->commit, &e->commit_len) != 0 ||
            mls_clone(par->state, &e->state) != 0) {
            err = MARMOT_ERR_MEMORY;
            goto out;
        }
    }
    for (size_t i = 0; i < h->n_entries && nh->n_entries < CONV_MAX_REWIND_COMMITS; i++) {
        const ConvEntry *old = &h->entries[i];
        if (old->epoch >= t->root_epoch) continue;
        if (old->epoch + 1 != nh->entries[nh->n_entries - 1].epoch) break;
        ConvEntry *e = &nh->entries[nh->n_entries++];
        e->epoch = old->epoch;
        e->key = old->key;
        e->own = old->own;
        e->reader = old->reader;
        if (dup_bytes(old->commit, old->commit_len, &e->commit, &e->commit_len) != 0 ||
            mls_clone(&old->state, &e->state) != 0) {
            err = MARMOT_ERR_MEMORY;
            goto out;
        }
    }
    uint64_t anchor = nh->entries[nh->n_entries - 1].epoch;
    err = conv_retained(t, h, cur, sel, fork, reorg, anchor, new_tip, nh);
    if (err != MARMOT_OK) goto out;
    if (conv_history_encode(nh, &rec_blob, &rec_len) != 0 ||
        mls_group_serialize(best->state, &state_blob, &state_len) != 0) {
        err = MARMOT_ERR_SERIALIZATION;
        goto out;
    }

    /* The record's mirror of the new tip. */
    err = undo_group(m, &undo, gid);
    if (err != MARMOT_OK) goto out;
    if (best->state->profile == MARMOT_GROUP_PROFILE_ADOPTED) {
        err = adopted_record_apply(m, group, best->state);
    } else {
        err = group_data_of(best->state, t->nodes[fork].state, &gde);
        if (err == MARMOT_OK && gde) err = marmot_group_apply_group_data(group, gde);
    }
    if (err != MARMOT_OK) goto fail;
    group->epoch = new_tip;

    /* Exporter secrets: the new branch's epochs, and none above its tip. */
    for (size_t k = fork_at + 1; k < plen && err == MARMOT_OK; k++) {
        const ConvNode *nd = &t->nodes[path[k]];
        err = undo_exporter(m, &undo, gid, nd->epoch);
        if (err == MARMOT_OK)
            err = s->save_exporter_secret(s->ctx, gid, nd->epoch,
                                          nd->state->epoch_secrets.exporter_secret);
    }
    for (uint64_t e = new_tip + 1; err == MARMOT_OK && e <= cur->epoch; e++) {
        err = undo_exporter(m, &undo, gid, e);
        if (err == MARMOT_OK) {
            err = s->delete_exporter_secret(s->ctx, gid, e);
            if (err == MARMOT_ERR_STORAGE_NOT_FOUND) err = MARMOT_OK;
        }
    }
    /* And the epochs that leave the window as the tip advances. */
    for (uint64_t e = cur->epoch > CONV_MAX_REWIND_COMMITS ? cur->epoch - CONV_MAX_REWIND_COMMITS : 0;
         err == MARMOT_OK && e + CONV_MAX_REWIND_COMMITS + 1 <= new_tip; e++) {
        err = undo_exporter(m, &undo, gid, e);
        if (err == MARMOT_OK) {
            err = s->delete_exporter_secret(s->ctx, gid, e);
            if (err == MARMOT_ERR_STORAGE_NOT_FOUND) err = MARMOT_OK;
        }
    }
    if (err == MARMOT_OK) err = undo_kv(m, &undo, PARENT_LABEL, gid);
    if (err == MARMOT_OK)
        err = s->mls_store(s->ctx, PARENT_LABEL, gid->data, gid->len, rec_blob, rec_len);
    if (err == MARMOT_OK) err = undo_kv(m, &undo, "mls_group", gid);
    if (err == MARMOT_OK)
        err = s->mls_store(s->ctx, "mls_group", gid->data, gid->len, state_blob, state_len);
    if (err == MARMOT_OK) err = s->save_group(s->ctx, group);
    if (err == MARMOT_OK && reorg) {
        err = invalidate_messages(m, gid, t->nodes[fork].epoch, cur->epoch, &undo, &pages,
                                  &page_lens, &n_pages, &invalidated, &n_invalidated);
        for (int k = t->tip; err == MARMOT_OK && k != fork; k = t->nodes[k].parent)
            if (list_add(&superseded, &n_superseded, marmot_hex_encode(t->nodes[k].key.digest, 32)) != 0)
                err = MARMOT_ERR_MEMORY;
    }
    if (err != MARMOT_OK) goto fail;

    marmot_proposals_prune(m, gid->data, gid->len, anchor);
    if (as_commit) fill_commit_result(m, group, result);
    /* Who left on their own request in the Commits now applied. */
    for (size_t k = fork_at + 1; as_commit && k < plen; k++) {
        const ConvNode *nd = &t->nodes[path[k]];
        char **d = NULL;
        size_t dn = 0;
        departed_hexes(t->nodes[nd->parent].state, &nd->departures, &d, &dn);
        for (size_t i = 0; i < dn; i++)
            if (list_add(&result->commit.departed_pubkey_hexes, &result->commit.departed_count,
                         d[i]) != 0)
                break;
        free(d);
    }
    if (as_commit) {
        result->commit.committer_pubkey_hex = marmot_hex_encode(best->key.committer, 32);
        result->commit.committer_leaf = result->commit.committer_pubkey_hex
                                            ? best->key.committer_leaf : UINT32_MAX;
        result->commit.routing_changed = adopted_routing_changed(cur, best->state);
    }
    if (reorg) {
        /* Superseded oldest first. */
        for (size_t a = 0, b = n_superseded ? n_superseded - 1 : 0; a < b; a++, b--) {
            char *x = superseded[a];
            superseded[a] = superseded[b];
            superseded[b] = x;
        }
        result->convergence.branch_recovered = true;
        result->convergence.fork_epoch = t->nodes[fork].epoch;
        result->convergence.superseded_commit_digests = superseded;
        result->convergence.superseded_count = n_superseded;
        result->convergence.invalidated_message_ids = invalidated;
        result->convergence.invalidated_count = n_invalidated;
        superseded = invalidated = NULL;
        n_superseded = n_invalidated = 0;
    }
    goto out;

fail:
    if (undo_apply(m, &undo, gid) != MARMOT_OK) err = MARMOT_ERR_STORAGE;
    /* The caller's record copy follows the stored one back. */
    if (undo.n > 0 && undo.recs[0].kind == UNDO_GROUP) {
        MarmotGroup *old = undo.recs[0].group;
        free(group->name);
        free(group->description);
        free(group->admin_pubkeys);
        group->name = old->name ? strdup(old->name) : NULL;
        group->description = old->description ? strdup(old->description) : NULL;
        group->admin_pubkeys = NULL;
        group->admin_count = 0;
        if (old->admin_count && old->admin_pubkeys &&
            (group->admin_pubkeys = malloc(old->admin_count * 32)) != NULL) {
            memcpy(group->admin_pubkeys, old->admin_pubkeys, old->admin_count * 32);
            group->admin_count = old->admin_count;
        }
        group->epoch = old->epoch;
        memcpy(group->nostr_group_id, old->nostr_group_id, 32);
    }
out:
    undo_free(&undo);
    for (size_t i = 0; i < n_pages; i++) {
        for (size_t j = 0; j < page_lens[i]; j++) marmot_message_free(pages[i][j]);
        free(pages[i]);
    }
    free(pages);
    free(page_lens);
    for (size_t i = 0; i < n_invalidated; i++) free(invalidated[i]);
    free(invalidated);
    for (size_t i = 0; i < n_superseded; i++) free(superseded[i]);
    free(superseded);
    free_secret(state_blob, state_len);
    free_secret(rec_blob, rec_len);
    marmot_group_data_extension_free(gde);
    history_free(nh);
    return err;
}

/* The canonical branch stays: `h` keeps what still matters (candidates that
 * attach and are inside the horizon, their states' exporter secrets, live
 * witnesses) and is stored. */
static MarmotError
conv_keep(Marmot *m, const MarmotGroupId *gid, const MlsGroup *cur, ConvHistory *h,
          const ConvTree *t)
{
    ConvHistory *nh = calloc(1, sizeof(*nh));
    if (!nh) return MARMOT_ERR_MEMORY;
    uint64_t anchor = history_anchor(h, cur->epoch);
    MarmotError err = conv_retained(t, h, cur, t->tip, t->tip, false, anchor, cur->epoch, nh);
    if (err == MARMOT_OK) {
        /* Swap in the retained parts; the entries stay as they are. */
        for (size_t i = 0; i < h->n_cands; i++) conv_candidate_clear(&h->cands[i]);
        memcpy(h->cands, nh->cands, sizeof(h->cands));
        h->n_cands = nh->n_cands;
        memcpy(h->wits, nh->wits, sizeof(h->wits));
        h->n_wits = nh->n_wits;
        memcpy(h->secrets, nh->secrets, sizeof(h->secrets));
        h->n_secrets = nh->n_secrets;
        nh->n_cands = 0;
        err = h->n_entries > 0 ? history_store(m, gid->data, gid->len, h) : MARMOT_OK;
    }
    history_free(nh);
    return err;
}

static MarmotError pending_retain(Marmot *m, MarmotGroup *group, uint64_t old_epoch,
                                  const uint8_t old_tag[32]);

/* Select among the branches of `t` (built from `h` and `cur`) and apply the
 * selection: the canonical branch stays (`h` refreshed and stored), another
 * becomes canonical, or a branch that removes our leaf wins (the group ends
 * for us, nostrc-xrya). */
static MarmotError
conv_apply(Marmot *m, MarmotGroup *group, const MlsGroup *cur, ConvHistory *h, ConvTree *t,
           bool as_commit, MarmotMessageResult *result, ConvOutcome *out)
{
    *out = CONV_UNCHANGED;
    int sel = conv_select(t, h);
    if (sel < 0 || sel == t->tip) return conv_keep(m, &group->mls_group_id, cur, h, t);
    const ConvNode *best = &t->nodes[sel];
    if (!best->state) {
        const ConvNode *par = &t->nodes[best->parent];
        MarmotError err = conv_keep(m, &group->mls_group_id, cur, h, t);
        if (err == MARMOT_OK)
            err = evict(m, group, par->state, best->parent != t->tip, &best->key, par->epoch,
                        best->removal_left);
        if (err == MARMOT_OK) {
            if (as_commit) fill_commit_result(m, group, result);
            *out = CONV_REMOVED;
        }
        return err;
    }
    MarmotError err = conv_install(m, group, cur, h, t, sel, as_commit, result);
    if (err == MARMOT_OK) {
        *out = CONV_ADVANCED;
        err = pending_retain(m, group, cur->epoch, cur->confirmed_transcript_hash);
    }
    return err;
}

/* The canonical tip, loaded. */
static MarmotError
conv_current(Marmot *m, MarmotGroup *group, MlsGroup *cur)
{
    MarmotError err = load_current(m, &group->mls_group_id, cur);
    if (err != MARMOT_OK) mls_group_free(cur);
    return err == MARMOT_OK ? MARMOT_OK : MARMOT_ERR_MLS;
}

/* Drop the candidates of `h` that left the horizon of the tip `tip`. */
static void
conv_prune_stale(ConvHistory *h, uint64_t tip)
{
    uint64_t anchor = history_anchor(h, tip);
    size_t n = 0;
    for (size_t i = 0; i < h->n_cands; i++) {
        ConvCandidate *c = &h->cands[i];
        if (c->source_epoch < anchor || c->source_epoch + CONV_MAX_REWIND_COMMITS < tip) {
            conv_candidate_clear(c);
            continue;
        }
        if (n != i) {
            h->cands[n] = *c;
            memset(c, 0, sizeof(*c));
        }
        n++;
    }
    h->n_cands = n;
}

/* Resolve again with what `h` retains (a witness was added, our Commit was
 * merged): `h` is consumed. */
static MarmotError
conv_rerun(Marmot *m, MarmotGroup *group, ConvHistory *h, bool as_commit,
           MarmotMessageResult *result, ConvOutcome *out)
{
    *out = CONV_UNCHANGED;
    MlsGroup cur;
    MarmotError err = conv_current(m, group, &cur);
    if (err != MARMOT_OK) return err;
    conv_prune_stale(h, cur.epoch);
    ConvTree t;
    err = conv_build(m, &cur, h, &t);
    if (err == MARMOT_OK) err = conv_apply(m, group, &cur, h, &t, as_commit, result, out);
    tree_clear(&t);
    mls_group_free(&cur);
    return err;
}

/* Admit the received Commit `msg` (`digest`) of `epoch`, sealed under the
 * exporter secret of the state `parent_tag` (NULL: the canonical state of
 * `epoch`), into the retained candidates of `*hp` (created when NULL) and
 * resolve (convergence.md).  The canonical tip `cur` is the group's.  On
 * MARMOT_OK the selection changed (result filled); MARMOT_ERR_WRONG_EPOCH:
 * it is retained but the canonical branch stays (kept: marmot_txn_keep());
 * otherwise its refusal, and nothing is kept. */
static MarmotError
conv_admit(Marmot *m, MarmotGroup *group, const MlsGroup *cur, ConvHistory **hp,
           uint64_t epoch, const uint8_t *parent_tag, const uint8_t *msg, size_t msg_len,
           const uint8_t digest[32], const char *event_id_hex, MarmotMessageResult *result,
           MarmotCommitKey *out_key, MarmotError *out_stage_err)
{
    *out_stage_err = MARMOT_OK;
    if (!*hp && !(*hp = calloc(1, sizeof(**hp)))) return MARMOT_ERR_MEMORY;
    ConvHistory *h = *hp;
    uint64_t tip = cur->epoch;
    uint64_t anchor = history_anchor(h, tip);
    uint8_t ptag[32];
    if (parent_tag) {
        memcpy(ptag, parent_tag, 32);
    } else {
        const MlsGroup *ps = canonical_state_at(h, cur, epoch);
        if (!ps) return MARMOT_ERR_WRONG_EPOCH;   /* older than what we retain */
        memcpy(ptag, ps->confirmed_transcript_hash, 32);
    }
    if (epoch < anchor || epoch + CONV_MAX_REWIND_COMMITS < tip)
        return MARMOT_ERR_WRONG_EPOCH;               /* outside the horizon: stale */
    for (size_t i = 0; i < h->n_cands; i++)
        if (memcmp(h->cands[i].digest, digest, 32) == 0)
            return MARMOT_ERR_WRONG_EPOCH;           /* retained already */
    conv_prune_stale(h, tip);
    if (h->n_cands >= CONV_MAX_CANDIDATES) return MARMOT_ERR_RESOURCE_REFUSED;
    size_t x = h->n_cands;
    ConvCandidate *c = &h->cands[x];
    memset(c, 0, sizeof(*c));
    c->source_epoch = epoch;
    memcpy(c->digest, digest, 32);
    memcpy(c->parent_tag, ptag, 32);
    if (dup_bytes(msg, msg_len, &c->msg, &c->msg_len) != 0 ||
        (event_id_hex && !(c->event_id = strdup(event_id_hex)))) {
        conv_candidate_clear(c);
        return MARMOT_ERR_MEMORY;
    }
    h->n_cands++;

    ConvTree t;
    MarmotError err = conv_build(m, cur, h, &t);
    if (err == MARMOT_OK) {
        if (out_key) *out_key = t.key[x];
        switch (t.status[x]) {
        case CAND_DEAD:
        case CAND_HELD:
            err = *out_stage_err = t.err[x];
            break;
        case CAND_WAITING:
            err = MARMOT_ERR_WRONG_EPOCH;   /* its parent is no longer a candidate state */
            break;
        case CAND_ATTACHED: {
            /* DoS bound: one committer's retained Commits. */
            size_t mine = 0;
            for (size_t i = 0; i < h->n_cands; i++)
                if (t.status[i] == CAND_ATTACHED &&
                    memcmp(t.key[i].committer, t.key[x].committer, 32) == 0)
                    mine++;
            if (mine > CONV_MAX_PER_COMMITTER) err = MARMOT_ERR_RESOURCE_REFUSED;
            break;
        }
        }
    }
    if (err == MARMOT_OK) {
        ConvOutcome out;
        err = conv_apply(m, group, cur, h, &t, true, result, &out);
        if (err == MARMOT_OK && out == CONV_UNCHANGED) {
            marmot_txn_keep(m);   /* retained: its branch may still win */
            err = MARMOT_ERR_WRONG_EPOCH;
        }
    } else {
        /* Not retained. */
        h->n_cands--;
        conv_candidate_clear(&h->cands[x]);
    }
    tree_clear(&t);
    return err;
}

/* ── Witnesses and candidate epochs (messages.c) ───────────────────────── */

MarmotError
marmot_commit_note_witness(Marmot *m, MarmotGroup *group, uint64_t epoch,
                           const uint8_t tag[32], const uint8_t sender[32], bool canonical,
                           uint8_t **out_replaced, size_t *out_replaced_len,
                           MarmotMessageResult *result)
{
    if (!m || !group || !tag || !sender || !out_replaced || !out_replaced_len)
        return MARMOT_ERR_INVALID_ARG;
    *out_replaced = NULL;
    *out_replaced_len = 0;
    MarmotStorage *s = m->storage;
    if (!s || !s->mls_load || !s->mls_store) return MARMOT_ERR_STORAGE;
    const MarmotGroupId *gid = &group->mls_group_id;
    uint8_t *probe = NULL;
    size_t probe_len = 0;
    MarmotError err = s->mls_load(s->ctx, PARENT_LABEL, gid->data, gid->len, &probe, &probe_len);
    if (err == MARMOT_ERR_STORAGE_NOT_FOUND || (err == MARMOT_OK && !probe)) {
        /* No history: no branch forks before this epoch, so no witness of
         * it can count (a branch epoch is after the fork). */
        free_secret(probe, probe_len);
        return MARMOT_OK;
    }
    if (err != MARMOT_OK) return err;
    ConvHistory *h = calloc(1, sizeof(*h));
    if (!h) {
        free_secret(probe, probe_len);
        return MARMOT_ERR_MEMORY;
    }
    if (conv_history_decode(probe, probe_len, h) != 0) {
        free(h);   /* unreadable: there is nothing to score with anyway */
        free_secret(probe, probe_len);
        return MARMOT_OK;
    }
    int added = conv_witness_add(h, epoch, tag, sender);
    if (added <= 0) {   /* known, the epoch's quorum full, or no room */
        history_free(h);
        free_secret(probe, probe_len);
        return MARMOT_OK;
    }
    /* A witness of the canonical tip only strengthens the branches through
     * the tip, the canonical one first: only an earlier canonical epoch's,
     * or a candidate state's, can change the selection. */
    bool at_tip = canonical && epoch == group->epoch;
    if (h->n_cands == 0 || at_tip) {
        err = history_store(m, gid->data, gid->len, h);
        history_free(h);
    } else {
        ConvOutcome out;
        err = conv_rerun(m, group, h, false, result, &out);
        history_free(h);
    }
    if (err == MARMOT_OK) {
        *out_replaced = probe;
        *out_replaced_len = probe_len;
        probe = NULL;
    }
    free_secret(probe, probe_len);
    return err;
}

MarmotError
marmot_commit_branch_secrets(Marmot *m, const MarmotGroupId *gid, MarmotBranchSecret **out,
                             size_t *out_count)
{
    if (!m || !gid || !out || !out_count) return MARMOT_ERR_INVALID_ARG;
    *out = NULL;
    *out_count = 0;
    ConvHistory *h = NULL;
    MarmotError err = history_load(m, gid->data, gid->len, &h);
    if (err != MARMOT_OK || !h) return err;
    if (h->n_secrets > 0) {
        MarmotBranchSecret *list = calloc(h->n_secrets, sizeof(*list));
        if (!list) {
            history_free(h);
            return MARMOT_ERR_MEMORY;
        }
        for (size_t i = 0; i < h->n_secrets; i++) {
            list[i].epoch = h->secrets[i].epoch;
            memcpy(list[i].tag, h->secrets[i].tag, 32);
            memcpy(list[i].exporter, h->secrets[i].exporter, 32);
            list[i].wants_witness = !conv_witness_full(h, list[i].epoch, list[i].tag);
        }
        *out = list;
        *out_count = h->n_secrets;
    }
    history_free(h);
    return MARMOT_OK;
}

void
marmot_branch_secrets_free(MarmotBranchSecret *list, size_t count)
{
    if (!list) return;
    sodium_memzero(list, count * sizeof(*list));
    free(list);
}

bool
marmot_commit_state_canonical(Marmot *m, const MarmotGroupId *gid, uint64_t epoch,
                              const uint8_t tag[32])
{
    MlsGroup cur;
    if (load_current(m, gid, &cur) != MARMOT_OK) {
        mls_group_free(&cur);
        return false;
    }
    bool yes = false;
    if (cur.epoch == epoch) {
        yes = memcmp(cur.confirmed_transcript_hash, tag, 32) == 0;
    } else if (epoch < cur.epoch) {
        ConvHistory *h = NULL;
        if (history_load(m, gid->data, gid->len, &h) == MARMOT_OK && h) {
            ConvEntry *e = conv_history_entry(h, epoch);
            yes = e && memcmp(e->state.confirmed_transcript_hash, tag, 32) == 0;
        }
        history_free(h);
    }
    mls_group_free(&cur);
    return yes;
}

MarmotError
marmot_commit_branch_decrypt(Marmot *m, MarmotGroup *group, uint64_t epoch,
                             const uint8_t tag[32], const uint8_t *msg, size_t msg_len,
                             uint8_t **out_plaintext, size_t *out_len,
                             uint8_t out_sender_identity[32])
{
    if (!m || !group || !tag || !msg || !out_plaintext || !out_len || !out_sender_identity)
        return MARMOT_ERR_INVALID_ARG;
    *out_plaintext = NULL;
    *out_len = 0;
    MlsGroup cur;
    MarmotError err = conv_current(m, group, &cur);
    if (err != MARMOT_OK) return err;
    ConvHistory *h = NULL;
    err = history_load(m, group->mls_group_id.data, group->mls_group_id.len, &h);
    if (err == MARMOT_OK && !h) err = MARMOT_ERR_STORAGE_NOT_FOUND;
    ConvTree t;
    memset(&t, 0, sizeof(t));
    if (err == MARMOT_OK) {
        conv_prune_stale(h, cur.epoch);
        err = conv_build(m, &cur, h, &t);
    }
    if (err == MARMOT_OK) {
        /* The candidate state is rebuilt by replay, so decrypting on it
         * spends nothing that is kept: if its branch is selected later,
         * the message is read again on the canonical state. */
        const ConvNode *n = NULL;
        for (size_t k = 0; k < t.n_nodes && !n; k++)
            if (t.nodes[k].state && t.nodes[k].epoch == epoch &&
                memcmp(t.nodes[k].tag, tag, 32) == 0)
                n = &t.nodes[k];
        MlsGroup work;
        uint32_t sender = UINT32_MAX;
        if (!n) {
            err = MARMOT_ERR_STORAGE_NOT_FOUND;
        } else if (mls_clone(n->state, &work) != 0) {
            err = MARMOT_ERR_MLS;
        } else {
            int rc = mls_group_decrypt(&work, msg, msg_len, out_plaintext, out_len, &sender);
            if (rc == 0 && marmot_mls_sender_identity(&work, sender, out_sender_identity) != 0) {
                free_secret(*out_plaintext, *out_len);
                *out_plaintext = NULL;
                *out_len = 0;
                rc = MARMOT_ERR_AUTHOR_MISMATCH;
            }
            err = rc == 0 ? MARMOT_OK
                          : (rc == MARMOT_ERR_OWN_MESSAGE || rc == MARMOT_ERR_AUTHOR_MISMATCH)
                                ? (MarmotError)rc : MARMOT_ERR_MLS;
            mls_group_free(&work);
        }
    }
    tree_clear(&t);
    history_free(h);
    mls_group_free(&cur);
    return err;
}

/* Another branch was just made canonical over `old_epoch`/`old_tag` (the
 * tip it replaced): a pending Commit of ours built on that tip may already
 * be published -- a relay that stored it and lost its OK -- so it stays
 * retained, as an unconfirmed candidate of ours (durability.md "Publish
 * interruption boundaries": a possibly published transition is never
 * discarded).  It ends no selectable branch until it shows it was published
 * (conv_select()); the pending record stays, reported superseded. */
static MarmotError
pending_retain(Marmot *m, MarmotGroup *group, uint64_t old_epoch, const uint8_t old_tag[32])
{
    const MarmotGroupId *gid = &group->mls_group_id;
    PendingCommit p;
    MarmotError err = pending_load(m, gid->data, gid->len, &p);
    if (err == MARMOT_ERR_STORAGE_NOT_FOUND) return MARMOT_OK;
    if (err != MARMOT_OK) return err;
    if (p.parent_epoch != old_epoch || memcmp(p.parent_transcript, old_tag, 32) != 0 ||
        !p.commit) {
        pending_clear(&p);   /* not built on that tip, or its bytes are unknown */
        return MARMOT_OK;
    }
    ConvHistory *h = NULL;
    err = history_load(m, gid->data, gid->len, &h);
    bool known = !h;
    for (size_t i = 0; h && !known && i < h->n_entries; i++)
        known = memcmp(h->entries[i].key.digest, p.key.digest, 32) == 0;
    for (size_t i = 0; h && !known && i < h->n_cands; i++)
        known = memcmp(h->cands[i].digest, p.key.digest, 32) == 0;
    if (err == MARMOT_OK && !known && h->n_cands < CONV_MAX_CANDIDATES) {
        ConvCandidate *c = &h->cands[h->n_cands];
        memset(c, 0, sizeof(*c));
        c->source_epoch = p.parent_epoch;
        memcpy(c->digest, p.key.digest, 32);
        memcpy(c->parent_tag, p.parent_transcript, 32);
        c->own = true;
        c->own_privileged = p.key.privileged;
        c->own_unconfirmed = true;
        if (dup_bytes(p.commit, p.commit_len, &c->msg, &c->msg_len) != 0 ||
            mls_group_serialize(&p.post, &c->own_post, &c->own_post_len) != 0) {
            conv_candidate_clear(c);
            err = MARMOT_ERR_MEMORY;
        } else {
            h->n_cands++;
            /* Resolve again: its state's exporter secret becomes peelable,
             * for the Commits and messages of members who did apply it. */
            MarmotMessageResult unreported;
            memset(&unreported, 0, sizeof(unreported));
            ConvOutcome out;
            err = conv_rerun(m, group, h, false, &unreported, &out);
            marmot_message_result_free(&unreported);
        }
    }
    history_free(h);
    pending_clear(&p);
    return err;
}

/* Our pending Commit `p`, built on `pre`, is merged: the Commits that lost
 * to it while it waited for a relay (deferred) are competitors from `pre`
 * now -- retained candidates like any other -- and the retained branches
 * are resolved again.  `result` (NULL: nothing to report to) gets a change
 * of the selected branch. */
static MarmotError
pending_converge(Marmot *m, MarmotGroup *group, const MlsGroup *pre, const PendingCommit *p,
                 MarmotMessageResult *result)
{
    ConvHistory *h = NULL;
    MarmotError err = history_load(m, group->mls_group_id.data, group->mls_group_id.len, &h);
    if (err != MARMOT_OK || !h) return err;
    for (size_t i = 0; i < p->deferred_count && err == MARMOT_OK; i++) {
        const DeferredCommit *d = &p->deferred[i];
        uint8_t digest[32];
        if (d->epoch != pre->epoch || mls_crypto_hash(digest, d->msg, d->msg_len) != 0)
            continue;
        bool known = false;
        for (size_t k = 0; k < h->n_cands && !known; k++)
            known = memcmp(h->cands[k].digest, digest, 32) == 0;
        if (known || h->n_cands >= CONV_MAX_CANDIDATES) continue;   /* bound: dropped */
        ConvCandidate *c = &h->cands[h->n_cands];
        memset(c, 0, sizeof(*c));
        c->source_epoch = d->epoch;
        memcpy(c->digest, digest, 32);
        memcpy(c->parent_tag, pre->confirmed_transcript_hash, 32);
        if (dup_bytes(d->msg, d->msg_len, &c->msg, &c->msg_len) != 0 ||
            (d->event_id && !(c->event_id = strdup(d->event_id)))) {
            conv_candidate_clear(c);
            err = MARMOT_ERR_MEMORY;
            break;
        }
        h->n_cands++;
    }
    if (err == MARMOT_OK && h->n_cands > 0) {
        MarmotMessageResult unreported;
        memset(&unreported, 0, sizeof(unreported));
        ConvOutcome out;
        err = conv_rerun(m, group, h, result != NULL, result ? result : &unreported, &out);
        marmot_message_result_free(&unreported);
    }
    history_free(h);
    return err;
}

MarmotError
marmot_commit_process_inbound(Marmot *m, MarmotGroup *group,
                              uint64_t outer_epoch,
                              const uint8_t *msg, size_t msg_len,
                              const char *event_id_hex,
                              MarmotMessageResult *result)
{
    return marmot_commit_process_inbound_ex(m, group, outer_epoch, NULL, msg, msg_len,
                                            event_id_hex, result);
}

/* An inbound Commit refused: the error to report.  A refusal of the parent
 * of the canonical tip that would have lost to the applied Commit anyway is
 * stale (nostrc-prrl); one of the tip's own epoch may be refused for good
 * (refused_for_good()). */
static MarmotError
inbound_refusal(Marmot *m, const MlsGroup *cur, ConvHistory *h, uint64_t epoch,
                const uint8_t *parent_tag, const uint8_t *msg, size_t msg_len,
                const CommitRoute *route, const MarmotCommitKey *key, MarmotError err)
{
    (void)m;
    if (parent_tag) return err;
    if (epoch == cur->epoch) {
        uint32_t sender = UINT32_MAX;
        return commit_sender_on(cur, msg, msg_len, route, &sender)
                   ? refused_for_good(cur, msg, msg_len, sender, err) : err;
    }
    /* A competitor of the Commit we applied that we refuse: stale only if it
     * loses to that Commit.  One that wins its epoch is where members who
     * don't check proofs go, so the group really stops here: say so, never
     * fork silently (W24 review L1).  authorize() filled the ordering key
     * before refusing. */
    ConvEntry *parent = history_parent_of(h, cur->epoch);
    if (err == MARMOT_ERR_KEY_PACKAGE_IDENTITY)
        return parent && parent->epoch == epoch && commit_key_cmp(key, &parent->key) < 0
                   ? err : MARMOT_ERR_WRONG_EPOCH;
    return err;
}

MarmotError
marmot_commit_process_inbound_ex(Marmot *m, MarmotGroup *group,
                                 uint64_t outer_epoch, const uint8_t *parent_tag,
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
    /* A standalone Proposal is kept for the Commit that references it
     * (nostrc-2um6; proposals.c); one of a candidate epoch is not. */
    if (route.proposal) {
        if (!route.group_ok) return MARMOT_ERR_WRONG_GROUP_ID;
        if (route.epoch != outer_epoch || parent_tag) return MARMOT_ERR_WRONG_EPOCH;
        return marmot_proposal_process_inbound(m, group, msg, msg_len, event_id_hex, result);
    }
    if (!route.commit) return MARMOT_ERR_MLS_FRAMING;
    if (!route.group_ok) return MARMOT_ERR_WRONG_GROUP_ID;
    uint64_t epoch = route.epoch;
    uint32_t sender = UINT32_MAX;   /* resolved on the state that judges it */
    /* The Commit is sealed under its own epoch's exporter secret. */
    if (epoch != outer_epoch) return MARMOT_ERR_WRONG_EPOCH;

    MarmotCommitKey key;
    memset(&key, 0, sizeof(key));
    uint8_t digest[32];
    if (mls_crypto_hash(digest, msg, msg_len) != 0) return MARMOT_ERR_CRYPTO;

    /* Removed by a Commit that may still lose its epoch (B1). */
    if (group->state != MARMOT_GROUP_STATE_ACTIVE) {
        if (parent_tag) return MARMOT_ERR_USE_AFTER_EVICTION;
        MarmotError rerr = inbound_removed(m, group, epoch, &route, msg, msg_len, digest);
        if (rerr != MARMOT_OK) return rerr;
        inbound_done(m, group, epoch, event_id_hex, result);
        return MARMOT_OK;
    }

    const uint8_t *gid = group->mls_group_id.data;
    size_t gid_len = group->mls_group_id.len;
    MlsGroup cur;
    if (load_current(m, &group->mls_group_id, &cur) != MARMOT_OK) {
        mls_group_free(&cur);
        return MARMOT_ERR_MLS;
    }
    /* A group of neither profile: its Commits -- every one of them,
     * including one that removes our leaf, which never reaches
     * marmot_commit_authorize() -- are refused before any judgement, removal
     * or key deletion (W24 review H1).  Adopted groups are judged by their
     * components since nostrc-qp24.5.1.3. */
    if (cur.profile != MARMOT_GROUP_PROFILE_LEGACY &&
        cur.profile != MARMOT_GROUP_PROFILE_ADOPTED) {
        mls_group_free(&cur);
        return MARMOT_ERR_UNSUPPORTED;
    }
    ConvHistory *h = NULL;
    MarmotError err = history_load(m, gid, gid_len, &h);
    if (err != MARMOT_OK) {
        mls_group_free(&cur);
        return err;   /* unreadable storage: fail closed */
    }
    /* A Commit of the canonical branch, again: our own echoed by a relay,
     * or a re-delivery (convergence: duplicates are never applied twice). */
    for (size_t i = 0; h && !parent_tag && i < h->n_entries; i++) {
        if (memcmp(digest, h->entries[i].key.digest, 32) != 0) continue;
        history_free(h);
        mls_group_free(&cur);
        result->type = MARMOT_RESULT_OWN_MESSAGE;
        return MARMOT_OK;
    }
    bool routing_changed = false;
    uint8_t previous_route[32];
    memcpy(previous_route, group->nostr_group_id, 32);

    MlsGroup post;
    memset(&post, 0, sizeof(post));
    MarmotGroupDataExtension *gde = NULL;
    MlsCommitSummary applied;   /* what the applied Commit did with departures */
    memset(&applied, 0, sizeof(applied));
    char **departed = NULL;
    size_t n_departed = 0;
    char *committer = NULL;   /* of the Commit applied */
    bool converged = false;   /* convergence filled the result */
    bool linear = !parent_tag && epoch == cur.epoch;

    if (linear) {
        /* Linear advance of the current epoch -- unless our own Commit built
         * on exactly this state awaits a relay: then the two compete now. */
        PendingCommit p;
        MarmotError perr = pending_load(m, gid, gid_len, &p);
        if (perr != MARMOT_OK && perr != MARMOT_ERR_STORAGE_NOT_FOUND) {
            history_free(h);
            mls_group_free(&cur);
            return perr;   /* unreadable pending state: fail closed */
        }
        bool live = perr == MARMOT_OK && pending_status(&p, &cur) == PENDING_LIVE;
        if (perr == MARMOT_OK && memcmp(digest, p.key.digest, 32) == 0) {
            /* Our own pending Commit, back from a relay: a relay stored it,
             * so it is published -- merge it (review R2; this also recovers
             * a committer that crashed or lost the relay's OK). */
            history_free(h);
            err = live ? pending_apply(m, group, &cur, &p, result) : MARMOT_ERR_WRONG_EPOCH;
            char **mine = NULL;
            size_t n_mine = 0;
            if (err == MARMOT_OK && p.has_departures)
                departed_hexes(&cur, &p.departures, &mine, &n_mine);
            pending_clear(&p);
            mls_group_free(&cur);
            if (err != MARMOT_OK) return err;
            if (result->type != MARMOT_RESULT_COMMIT || !result->commit.updated_group)
                fill_commit_result(m, group, result);
            if (!result->commit.departed_pubkey_hexes) {
                result->commit.departed_pubkey_hexes = mine;
                result->commit.departed_count = n_mine;
            } else {
                for (size_t i = 0; i < n_mine; i++) free(mine[i]);
                free(mine);
            }
            return MARMOT_OK;
        }
        bool known = commit_sender_on(&cur, msg, msg_len, &route, &sender);
        err = known ? stage_inbound(m, &cur, msg, msg_len, sender, &post, &key, &gde, &applied)
                    : MARMOT_ERR_MLS_PROCESS_MESSAGE;
        bool removed = false;
        MlsCommitSummary dep;
        /* A Commit citing a proposal we lack (review H1) may still remove us
         * outright; otherwise it is MARMOT_ERR_PROPOSAL_UNKNOWN: kept by the
         * application and offered again once the proposal arrives. */
        if ((err == MARMOT_ERR_MLS_PROCESS_MESSAGE || err == MARMOT_ERR_PROPOSAL_UNKNOWN) &&
            known &&
            removes_self(m, &cur, msg, msg_len, sender, &removed, &dep) == 0 && removed) {
            /* nostrc-xrya: a Commit that removes us cannot be applied (its
             * UpdatePath is encrypted to the others), but an admin's
             * authenticated one -- or any member's committing our own
             * SelfRemove (nostrc-2um6) -- ends the group for us, unless our
             * own pending Commit wins the epoch. */
            err = removal_key(m, &cur, msg, msg_len, sender, digest, &dep, &key);
            if (err == MARMOT_OK && live && commit_key_cmp(&key, &p.key) >= 0)
                err = defer_inbound(m, &p, gid, gid_len, epoch, msg, msg_len, digest,
                                    event_id_hex);
            else if (err == MARMOT_OK && h && h->n_cands > 0)
                linear = false;   /* judged among the retained branches */
            else if (err == MARMOT_OK)
                err = evict(m, group, &cur, false, &key, epoch,
                            summary_departs(&dep, cur.own_leaf_index));
        } else if (err == MARMOT_OK) {
            memcpy(key.digest, digest, 32);
            if (live && commit_key_cmp(&key, &p.key) >= 0) {
                err = defer_inbound(m, &p, gid, gid_len, epoch, msg, msg_len,
                                    digest, event_id_hex);
            } else if (h && h->n_cands > 0) {
                linear = false;   /* judged among the retained branches */
            } else {
                err = marmot_commit_persist_ex(m, &cur, &post, &key, gde, group, msg, msg_len,
                                               false);
                if (err == MARMOT_OK) {
                    departed_hexes(&cur, &applied, &departed, &n_departed);
                    committer = marmot_hex_encode(key.committer, 32);
                    routing_changed = adopted_routing_changed(&cur, &post);
                }
                /* It beat our pending Commit, which may be published. */
                if (err == MARMOT_OK && live)
                    err = pending_retain(m, group, cur.epoch, cur.confirmed_transcript_hash);
            }
            /* A winner replaces the state our pending Commit was built on:
             * from now on it is STALE and merging it fails (review R1). */
        }
        if (linear && err != MARMOT_OK && known)
            err = refused_for_good(&cur, msg, msg_len, sender, err);
        if (perr == MARMOT_OK) pending_clear(&p);
        if (!linear) {
            mls_group_free(&post);
            marmot_group_data_extension_free(gde);
            gde = NULL;
        }
    }
    if (!linear) {
        /* convergence.md: a Commit off the canonical tip -- a competitor of
         * an applied Commit, a child of a retained branch -- or any Commit
         * while branches are retained. */
        MarmotCommitKey xkey;
        memset(&xkey, 0, sizeof(xkey));
        MarmotError stage_err = MARMOT_OK;
        ConvCandidate *echo = NULL;   /* our superseded pending Commit, from a relay */
        if (h)
            for (size_t i = 0; i < h->n_cands; i++) {
                if (memcmp(h->cands[i].digest, digest, 32) != 0) continue;
                stage_err = MARMOT_ERR_WRONG_EPOCH;
                if (h->cands[i].own && h->cands[i].own_unconfirmed) echo = &h->cands[i];
            }
        if (echo) {
            /* A relay stored it: it was published, and its branch may win. */
            echo->own_unconfirmed = false;
            ConvOutcome out;
            err = conv_rerun(m, group, h, true, result, &out);
            if (err == MARMOT_OK && out == CONV_UNCHANGED) {
                marmot_txn_keep(m);
                err = MARMOT_ERR_WRONG_EPOCH;
            }
            converged = err == MARMOT_OK;
        } else if (stage_err != MARMOT_OK) {
            err = stage_err;   /* retained already: no change */
        } else if (!parent_tag && epoch > cur.epoch) {
            err = MARMOT_ERR_WRONG_EPOCH;   /* an epoch we lack */
        } else {
            err = conv_admit(m, group, &cur, &h, epoch, parent_tag, msg, msg_len, digest,
                             event_id_hex, result, &xkey, &stage_err);
            if (err != MARMOT_OK && stage_err != MARMOT_OK)
                err = inbound_refusal(m, &cur, h, epoch, parent_tag, msg, msg_len, &route,
                                      &xkey, err);
            converged = err == MARMOT_OK;
        }
    }

    mls_group_free(&post);
    mls_group_free(&cur);
    marmot_group_data_extension_free(gde);
    history_free(h);
    if (err != MARMOT_OK) {
        for (size_t i = 0; i < n_departed; i++) free(departed[i]);
        free(departed);
        free(committer);
        return err;
    }

    if (converged) {
        inbound_mark(m, group, epoch, event_id_hex);
        if (result->type == MARMOT_RESULT_COMMIT)
            memcpy(result->commit.previous_nostr_group_id, previous_route, 32);
        return MARMOT_OK;
    }
    inbound_done(m, group, epoch, event_id_hex, result);
    result->commit.departed_pubkey_hexes = departed;
    result->commit.departed_count = n_departed;
    result->commit.committer_pubkey_hex = committer;
    /* The leaf that committed it, as authorization established: the one
     * leaf renewed in place (nostrc-6ukh, W24 review M1). */
    result->commit.committer_leaf = committer ? key.committer_leaf : UINT32_MAX;
    result->commit.routing_changed = routing_changed;
    memcpy(result->commit.previous_nostr_group_id, previous_route, 32);
    return MARMOT_OK;
}
