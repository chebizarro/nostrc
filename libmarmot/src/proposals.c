/*
 * libmarmot - Standalone proposals and leaving (nostrc-2um6)
 *
 * MIP-03 "Leaving a group" (legacy text cc73aa8) and the adopted
 * protocol-core/member-departure.md: a member leaves with a SelfRemove
 * proposal (draft-ietf-mls-extensions, 0x000a), sent as an MLS
 * PublicMessage; another member commits it by reference.  MDK 0.8 also
 * leaves a group that does not require SelfRemove with a Remove of itself
 * (OpenMLS leave_group(), a PrivateMessage in MDK's MIXED_CIPHERTEXT
 * groups), which only an admin may commit.
 *
 * Proposals.  A standalone Proposal of the current epoch that
 * authenticates (mls_group_open_proposal(): either wire format) and that
 * this profile can act on is kept as its AuthenticatedContent, with its
 * ProposalRef and the SHA-256 of its complete MLSMessage (review M2):
 *
 *   - Kept types: a SelfRemove from a non-admin, and in a legacy group a
 *     Remove a non-admin sent for itself (what MDK 0.8 sends standalone).
 *     Anything else is MARMOT_ERR_UNSUPPORTED and not kept: libmarmot never
 *     commits it, and an admin's change comes as a Commit.
 *   - One slot per (epoch, sender leaf), at most MARMOT_PROPOSALS_PER_SENDER
 *     records: a member flooding the group fills only its own slot, never
 *     another member's SelfRemove.
 *   - "mls_group_proposal_slot", key group id || u64 epoch || u32 sender
 *     leaf: u8 version (1), u8 count, then per record: u8 flags (1 own,
 *     2 has target), u16 type, u32 target leaf, [32] ref, [32] digest,
 *     [32] sender account, [32] target account, opaque
 *     authenticated_content<V>.
 *   - "mls_group_proposals", key group id: the index of the slots, u8
 *     version (2), u32 count, then u64 epoch, u32 sender leaf each.  A new
 *     record rewrites only its sender's slot (and the small index when the
 *     slot is new), never the whole store.
 *
 * Who commits (W24 decision, recorded in the README): any remaining member
 * may commit a SelfRemove-only Commit -- MDK 0.8 (messages/proposal.rs)
 * and 0.11 (cgka-engine auto_committer.rs) auto-commit them from any
 * member, and the adopted spec says the same ("Any remaining member whose
 * authenticated account is authorized ... MAY commit the retained
 * SelfRemove proposals").  A Remove a member sent for itself is a Remove:
 * only an admin commits it (MDK 0.8 auto-commits it as an admin, keeps it
 * pending otherwise).  A SelfRemove from an admin is invalid (MIP-03:
 * admins step down first) and is refused on receipt and in a Commit.
 *
 * Leaving.  As MDK 0.8 (try_self_remove()): a SelfRemove when the group's
 * required_capabilities list it, else a Remove of ourselves as a
 * PrivateMessage (review M1).  Our own leave request is
 * "mls_group_leaving": u8 version (1), u64 epoch, opaque event_json<V> (the
 * sealed kind:445 of that epoch's proposal, republished byte for byte).  It stands until the Commit that
 * removes us is final (then the group's records are forgotten) and gates
 * every send meanwhile (MARMOT_ERR_LEAVING).
 *
 * SPDX-License-Identifier: MIT
 */

#include "proposals.h"
#include "commits.h"
#include "kp_profile.h"
#include "mls/mls-internal.h"
#include <sodium.h>
#include <stdlib.h>
#include <string.h>

#define INDEX_VERSION   2
#define SLOT_VERSION    1
#define LEAVING_VERSION 1
#define FLAG_OWN        1
#define FLAG_TARGET     2
#define SLOT_LABEL      "mls_group_proposal_slot"

static void
free_secret(uint8_t *p, size_t len)
{
    if (!p) return;
    sodium_memzero(p, len);
    free(p);
}

/* ── The records ─────────────────────────────────────────────────────── */

void
marmot_proposals_clear(MarmotProposalSet *set)
{
    if (!set) return;
    for (size_t i = 0; i < set->count; i++) free(set->items[i].ac);
    free(set->items);
    free(set->acs);
    free(set->ac_lens);
    memset(set, 0, sizeof(*set));
}

typedef struct {
    uint64_t epoch;
    uint32_t sender;
} SlotId;

typedef struct {
    SlotId *ids;
    size_t  count;
} SlotIndex;

/* group id || u64 epoch || u32 sender leaf (big endian) */
static uint8_t *
slot_key(const uint8_t *gid, size_t gid_len, SlotId id, size_t *out_len)
{
    uint8_t *k = malloc(gid_len + 12);
    if (!k) return NULL;
    memcpy(k, gid, gid_len);
    for (int i = 0; i < 8; i++) k[gid_len + i] = (uint8_t)(id.epoch >> (56 - 8 * i));
    for (int i = 0; i < 4; i++) k[gid_len + 8 + i] = (uint8_t)(id.sender >> (24 - 8 * i));
    *out_len = gid_len + 12;
    return k;
}

/* None stored: an empty index.  A record of another version (the first
 * 0.12.0 draft's single blob) is dropped, not misread. */
static MarmotError
index_load(Marmot *m, const uint8_t *gid, size_t gid_len, SlotIndex *out)
{
    memset(out, 0, sizeof(*out));
    MarmotStorage *s = m->storage;
    if (!s || !s->mls_load) return MARMOT_ERR_STORAGE;
    uint8_t *data = NULL;
    size_t len = 0;
    MarmotError err = s->mls_load(s->ctx, MARMOT_MLS_PROPOSALS_LABEL, gid, gid_len, &data, &len);
    if (err == MARMOT_ERR_STORAGE_NOT_FOUND || (err == MARMOT_OK && !data)) return MARMOT_OK;
    if (err != MARMOT_OK) return err;
    MlsTlsReader r;
    mls_tls_reader_init(&r, data, len);
    uint8_t version = 0;
    uint32_t n = 0;
    err = MARMOT_OK;
    if (mls_tls_read_u8(&r, &version) != 0 || version != INDEX_VERSION) {
        free(data);
        return MARMOT_OK;
    }
    if (mls_tls_read_u32(&r, &n) != 0 || n > MARMOT_PROPOSAL_SLOTS_MAX) {
        err = MARMOT_ERR_DESERIALIZATION;
    } else if (n > 0) {
        out->ids = calloc(n, sizeof(*out->ids));
        if (!out->ids) err = MARMOT_ERR_MEMORY;
        for (uint32_t i = 0; err == MARMOT_OK && i < n; i++) {
            if (mls_tls_read_u64(&r, &out->ids[i].epoch) != 0 ||
                mls_tls_read_u32(&r, &out->ids[i].sender) != 0)
                err = MARMOT_ERR_DESERIALIZATION;
            else
                out->count++;
        }
    }
    if (err == MARMOT_OK && !mls_tls_reader_done(&r)) err = MARMOT_ERR_DESERIALIZATION;
    free(data);
    if (err != MARMOT_OK) {
        free(out->ids);
        memset(out, 0, sizeof(*out));
    }
    return err;
}

static MarmotError
index_store(Marmot *m, const uint8_t *gid, size_t gid_len, const SlotIndex *idx)
{
    MarmotStorage *s = m->storage;
    if (!s || !s->mls_store || !s->mls_delete) return MARMOT_ERR_STORAGE;
    if (idx->count == 0) {
        MarmotError err = s->mls_delete(s->ctx, MARMOT_MLS_PROPOSALS_LABEL, gid, gid_len);
        return err == MARMOT_ERR_STORAGE_NOT_FOUND ? MARMOT_OK : err;
    }
    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, 8 + idx->count * 12) != 0) return MARMOT_ERR_MEMORY;
    bool ok = mls_tls_write_u8(&buf, INDEX_VERSION) == 0 &&
              mls_tls_write_u32(&buf, (uint32_t)idx->count) == 0;
    for (size_t i = 0; ok && i < idx->count; i++)
        ok = mls_tls_write_u64(&buf, idx->ids[i].epoch) == 0 &&
             mls_tls_write_u32(&buf, idx->ids[i].sender) == 0;
    MarmotError err = ok ? s->mls_store(s->ctx, MARMOT_MLS_PROPOSALS_LABEL, gid, gid_len,
                                        buf.data, buf.len)
                         : MARMOT_ERR_SERIALIZATION;
    mls_tls_buf_free(&buf);
    return err;
}

/* Append slot `id`'s records to `out` (none stored: nothing). */
static MarmotError
slot_load(Marmot *m, const uint8_t *gid, size_t gid_len, SlotId id, MarmotProposalSet *out)
{
    MarmotStorage *s = m->storage;
    size_t klen = 0;
    uint8_t *key = slot_key(gid, gid_len, id, &klen);
    if (!key) return MARMOT_ERR_MEMORY;
    uint8_t *data = NULL;
    size_t len = 0;
    MarmotError err = s->mls_load(s->ctx, SLOT_LABEL, key, klen, &data, &len);
    free(key);
    if (err == MARMOT_ERR_STORAGE_NOT_FOUND || (err == MARMOT_OK && !data)) return MARMOT_OK;
    if (err != MARMOT_OK) return err;
    MlsTlsReader r;
    mls_tls_reader_init(&r, data, len);
    uint8_t version = 0, count = 0;
    err = MARMOT_ERR_DESERIALIZATION;
    if (mls_tls_read_u8(&r, &version) == 0 && version == SLOT_VERSION &&
        mls_tls_read_u8(&r, &count) == 0 && count <= MARMOT_PROPOSALS_PER_SENDER) {
        MarmotStoredProposal *grown =
            count ? realloc(out->items, (out->count + count) * sizeof(*grown)) : out->items;
        bool ok = !count || grown;
        if (grown) out->items = grown;
        for (uint8_t i = 0; ok && i < count; i++) {
            MarmotStoredProposal *p = &out->items[out->count];
            memset(p, 0, sizeof(*p));
            uint8_t flags = 0;
            ok = mls_tls_read_u8(&r, &flags) == 0 && (flags & ~(FLAG_OWN | FLAG_TARGET)) == 0 &&
                 mls_tls_read_u16(&r, &p->type) == 0 &&
                 mls_tls_read_u32(&r, &p->target_leaf) == 0 &&
                 mls_tls_read_fixed(&r, p->ref, 32) == 0 &&
                 mls_tls_read_fixed(&r, p->digest, 32) == 0 &&
                 mls_tls_read_fixed(&r, p->sender, 32) == 0 &&
                 mls_tls_read_fixed(&r, p->target, 32) == 0 &&
                 mls_tls_read_opaque32(&r, &p->ac, &p->ac_len) == 0;
            if (ok) {
                p->epoch = id.epoch;
                p->sender_leaf = id.sender;
                p->own = (flags & FLAG_OWN) != 0;
                p->has_target = (flags & FLAG_TARGET) != 0;
                out->count++;
            } else {
                free(p->ac);
            }
        }
        if (ok && mls_tls_reader_done(&r)) err = MARMOT_OK;
    }
    free(data);
    return err;
}

static MarmotError
slot_store(Marmot *m, const uint8_t *gid, size_t gid_len, SlotId id,
           const MarmotStoredProposal *items, size_t count)
{
    MarmotStorage *s = m->storage;
    size_t klen = 0;
    uint8_t *key = slot_key(gid, gid_len, id, &klen);
    if (!key) return MARMOT_ERR_MEMORY;
    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, 256) != 0) {
        free(key);
        return MARMOT_ERR_MEMORY;
    }
    bool ok = mls_tls_write_u8(&buf, SLOT_VERSION) == 0 &&
              mls_tls_write_u8(&buf, (uint8_t)count) == 0;
    for (size_t i = 0; ok && i < count; i++) {
        const MarmotStoredProposal *p = &items[i];
        uint8_t flags = (uint8_t)((p->own ? FLAG_OWN : 0) | (p->has_target ? FLAG_TARGET : 0));
        ok = mls_tls_write_u8(&buf, flags) == 0 && mls_tls_write_u16(&buf, p->type) == 0 &&
             mls_tls_write_u32(&buf, p->target_leaf) == 0 &&
             mls_tls_buf_append(&buf, p->ref, 32) == 0 &&
             mls_tls_buf_append(&buf, p->digest, 32) == 0 &&
             mls_tls_buf_append(&buf, p->sender, 32) == 0 &&
             mls_tls_buf_append(&buf, p->target, 32) == 0 &&
             mls_tls_write_opaque32(&buf, p->ac, p->ac_len) == 0;
    }
    MarmotError err = ok ? s->mls_store(s->ctx, SLOT_LABEL, key, klen, buf.data, buf.len)
                         : MARMOT_ERR_SERIALIZATION;
    mls_tls_buf_free(&buf);
    free(key);
    return err;
}

static void
slot_delete(Marmot *m, const uint8_t *gid, size_t gid_len, SlotId id)
{
    size_t klen = 0;
    uint8_t *key = slot_key(gid, gid_len, id, &klen);
    if (!key) return;
    (void)m->storage->mls_delete(m->storage->ctx, SLOT_LABEL, key, klen);
    free(key);
}

MarmotError
marmot_proposals_load(Marmot *m, const uint8_t *gid, size_t gid_len, MarmotProposalSet *out)
{
    memset(out, 0, sizeof(*out));
    SlotIndex idx;
    MarmotError err = index_load(m, gid, gid_len, &idx);
    for (size_t i = 0; err == MARMOT_OK && i < idx.count; i++)
        err = slot_load(m, gid, gid_len, idx.ids[i], out);
    free(idx.ids);
    if (err != MARMOT_OK) marmot_proposals_clear(out);
    return err;
}

MarmotError
marmot_proposals_load_epoch(Marmot *m, const uint8_t *gid, size_t gid_len, uint64_t epoch,
                            MarmotProposalSet *out)
{
    memset(out, 0, sizeof(*out));
    SlotIndex idx;
    MarmotError err = index_load(m, gid, gid_len, &idx);
    for (size_t i = 0; err == MARMOT_OK && i < idx.count; i++)
        if (idx.ids[i].epoch == epoch) err = slot_load(m, gid, gid_len, idx.ids[i], out);
    free(idx.ids);
    if (err == MARMOT_OK && out->count > 0) {
        out->acs = calloc(out->count, sizeof(*out->acs));
        out->ac_lens = calloc(out->count, sizeof(*out->ac_lens));
        if (!out->acs || !out->ac_lens) err = MARMOT_ERR_MEMORY;
        for (size_t i = 0; err == MARMOT_OK && i < out->count; i++) {
            out->acs[out->ac_count] = out->items[i].ac;
            out->ac_lens[out->ac_count++] = out->items[i].ac_len;
        }
    }
    if (err != MARMOT_OK) marmot_proposals_clear(out);
    return err;
}

void
marmot_proposals_prune(Marmot *m, const uint8_t *gid, size_t gid_len, uint64_t keep_from)
{
    SlotIndex idx;
    if (index_load(m, gid, gid_len, &idx) != MARMOT_OK) return;
    size_t kept = 0;
    for (size_t i = 0; i < idx.count; i++) {
        if (idx.ids[i].epoch >= keep_from)
            idx.ids[kept++] = idx.ids[i];
        else
            slot_delete(m, gid, gid_len, idx.ids[i]);
    }
    bool changed = kept != idx.count;
    idx.count = kept;
    if (changed) (void)index_store(m, gid, gid_len, &idx);
    free(idx.ids);
}

void
marmot_proposals_forget(Marmot *m, const uint8_t *gid, size_t gid_len)
{
    MarmotStorage *s = m->storage;
    if (!s || !s->mls_delete) return;
    SlotIndex idx;
    if (index_load(m, gid, gid_len, &idx) == MARMOT_OK) {
        for (size_t i = 0; i < idx.count; i++) slot_delete(m, gid, gid_len, idx.ids[i]);
        free(idx.ids);
    }
    (void)s->mls_delete(s->ctx, MARMOT_MLS_PROPOSALS_LABEL, gid, gid_len);
    (void)s->mls_delete(s->ctx, MARMOT_MLS_LEAVING_LABEL, gid, gid_len);
}

/* ── Admin policy (profile hook) ───────────────────────────────────────── */

/* The group's admitted profile (W24 slice E): mls_group_context_profile_of()
 * classified its GroupContext when it was admitted, created or loaded, so
 * anything that is not legacy is judged by the adopted (stricter) rules. */
bool
marmot_policy_is_adopted(const MlsGroup *g)
{
    return g && g->profile != MARMOT_GROUP_PROFILE_LEGACY;
}

/* marmot.group.admin-policy.v1: admins<V> of sorted 32-byte keys. */
static MarmotError
adopted_is_admin(const MlsGroup *g, const uint8_t account[32], bool *out)
{
    const uint8_t *dict = NULL;
    size_t dict_len = 0, count = 0;
    if (marmot_extensions_find(g->extensions_data, g->extensions_len,
                               MARMOT_EXT_APP_DATA_DICTIONARY, &dict, &dict_len, &count) != 0 ||
        count != 1)
        return MARMOT_ERR_EXTENSION_FORMAT;
    MarmotComponentData *entries = NULL;
    size_t n = 0;
    if (marmot_app_data_dict_parse(dict, dict_len, &entries, &n) != 0)
        return MARMOT_ERR_EXTENSION_FORMAT;
    MarmotError err = MARMOT_ERR_EXTENSION_FORMAT;   /* no admin policy: fail closed */
    for (size_t i = 0; i < n; i++) {
        if (entries[i].component_id != MARMOT_COMPONENT_ADMIN_POLICY_V1) continue;
        MlsTlsReader r;
        size_t keys_len = 0;
        mls_tls_reader_init(&r, entries[i].data, entries[i].len);
        if (mls_tls_read_vli(&r, &keys_len) != 0 || keys_len != mls_tls_reader_remaining(&r) ||
            keys_len % 32 != 0)
            break;
        *out = false;
        for (size_t k = 0; k < keys_len; k += 32)
            if (memcmp(r.data + r.pos + k, account, 32) == 0) *out = true;
        err = MARMOT_OK;
        break;
    }
    free(entries);
    return err;
}

MarmotError
marmot_policy_is_admin(const MlsGroup *g, const uint8_t account[32], bool *out)
{
    if (!g || !account || !out) return MARMOT_ERR_INVALID_ARG;
    *out = false;
    if (marmot_policy_is_adopted(g)) return adopted_is_admin(g, account, out);
    /* Legacy: listed in marmot_group_data's admins (MDK 0.8's
     * `group_data.admins.contains(..)`); none listed, nobody is. */
    const uint8_t *data = NULL;
    size_t len = 0, count = 0;
    if (marmot_extensions_find(g->extensions_data, g->extensions_len, MARMOT_EXTENSION_TYPE,
                               &data, &len, &count) != 0 ||
        count > 1)
        return MARMOT_ERR_EXTENSION_FORMAT;
    if (count == 0) return MARMOT_OK;
    MarmotGroupDataExtension *gde = marmot_group_data_extension_deserialize(data, len);
    if (!gde) return MARMOT_ERR_EXTENSION_FORMAT;
    for (size_t i = 0; i < gde->admin_count && gde->admins; i++)
        if (memcmp(gde->admins[i], account, 32) == 0) *out = true;
    marmot_group_data_extension_free(gde);
    return MARMOT_OK;
}

/* Who may commit a privileged Commit (a Remove): an admin, or anyone in a
 * legacy group whose GroupData lists none (commits.c gde_is_admin()).  A
 * group without GroupData has no admin at all (W24 slice E review H1). */
static bool
may_commit_privileged(const MlsGroup *g, const uint8_t account[32])
{
    bool admin = false;
    if (marmot_policy_is_admin(g, account, &admin) != MARMOT_OK) return false;
    if (admin || marmot_policy_is_adopted(g)) return admin;
    const uint8_t *data = NULL;
    size_t len = 0, count = 0;
    if (marmot_extensions_find(g->extensions_data, g->extensions_len, MARMOT_EXTENSION_TYPE,
                               &data, &len, &count) != 0)
        return false;
    if (count != 1) return false;
    MarmotGroupDataExtension *gde = marmot_group_data_extension_deserialize(data, len);
    bool none = gde && gde->admin_count == 0;
    marmot_group_data_extension_free(gde);
    return none;
}

/* ── Leave request ──────────────────────────────────────────────────────── */

static MarmotError
leaving_load(Marmot *m, const uint8_t *gid, size_t gid_len, uint64_t *epoch, char **json)
{
    if (json) *json = NULL;
    MarmotStorage *s = m->storage;
    if (!s || !s->mls_load) return MARMOT_ERR_STORAGE;
    uint8_t *data = NULL;
    size_t len = 0;
    MarmotError err = s->mls_load(s->ctx, MARMOT_MLS_LEAVING_LABEL, gid, gid_len, &data, &len);
    if (err == MARMOT_OK && !data) err = MARMOT_ERR_STORAGE_NOT_FOUND;
    if (err != MARMOT_OK) return err;
    MlsTlsReader r;
    mls_tls_reader_init(&r, data, len);
    uint8_t version = 0;
    uint8_t *ev = NULL;
    size_t ev_len = 0;
    err = MARMOT_ERR_DESERIALIZATION;
    if (mls_tls_read_u8(&r, &version) == 0 && version == LEAVING_VERSION &&
        mls_tls_read_u64(&r, epoch) == 0 && mls_tls_read_opaque32(&r, &ev, &ev_len) == 0 &&
        mls_tls_reader_done(&r)) {
        err = MARMOT_OK;
        if (json) {
            *json = calloc(1, ev_len + 1);
            if (!*json) err = MARMOT_ERR_MEMORY;
            else if (ev_len) memcpy(*json, ev, ev_len);
        }
    }
    free(ev);
    free(data);
    return err;
}

static MarmotError
leaving_store(Marmot *m, const uint8_t *gid, size_t gid_len, uint64_t epoch, const char *json)
{
    MarmotStorage *s = m->storage;
    if (!s || !s->mls_store) return MARMOT_ERR_STORAGE;
    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, strlen(json) + 16) != 0) return MARMOT_ERR_MEMORY;
    MarmotError err = MARMOT_ERR_SERIALIZATION;
    if (mls_tls_write_u8(&buf, LEAVING_VERSION) == 0 && mls_tls_write_u64(&buf, epoch) == 0 &&
        mls_tls_write_opaque32(&buf, (const uint8_t *)json, strlen(json)) == 0)
        err = s->mls_store(s->ctx, MARMOT_MLS_LEAVING_LABEL, gid, gid_len, buf.data, buf.len);
    mls_tls_buf_free(&buf);
    return err;
}

MarmotError
marmot_leaving_gate(Marmot *m, const MarmotGroupId *gid)
{
    if (!m || !gid || !m->storage || !m->storage->mls_load) return MARMOT_OK;
    uint64_t epoch = 0;
    MarmotError err = leaving_load(m, gid->data, gid->len, &epoch, NULL);
    if (err == MARMOT_ERR_STORAGE_NOT_FOUND) return MARMOT_OK;
    return err == MARMOT_OK ? MARMOT_ERR_LEAVING : err;
}

MarmotError
marmot_is_leaving(Marmot *m, const MarmotGroupId *mls_group_id, bool *out_leaving)
{
    if (!m || !mls_group_id || !out_leaving) return MARMOT_ERR_INVALID_ARG;
    *out_leaving = false;
    MarmotError err = marmot_leaving_gate(m, mls_group_id);
    if (err == MARMOT_ERR_LEAVING) {
        *out_leaving = true;
        return MARMOT_OK;
    }
    return err;
}

/* ── Selection ──────────────────────────────────────────────────────────── */

static bool
leaf_identity(const MlsGroup *g, uint32_t leaf, uint8_t out[32])
{
    return marmot_mls_sender_identity(g, leaf, out) == 0;
}

bool
marmot_proposal_committable(const MlsGroup *cur, const MarmotStoredProposal *p)
{
    uint8_t sender[32], own[32];
    if (p->epoch != cur->epoch || p->own || p->target_leaf == cur->own_leaf_index ||
        !leaf_identity(cur, p->sender_leaf, sender) || memcmp(sender, p->sender, 32) != 0 ||
        !leaf_identity(cur, cur->own_leaf_index, own))
        return false;
    bool sender_admin = true;
    if (marmot_policy_is_admin(cur, sender, &sender_admin) != MARMOT_OK || sender_admin)
        return false;   /* an admin steps down first (MIP-03) */
    if (p->type == MLS_PROPOSAL_SELF_REMOVE)
        return p->target_leaf == p->sender_leaf &&
               mls_group_members_support_proposal(cur, MLS_PROPOSAL_SELF_REMOVE);
    if (p->type == MLS_PROPOSAL_REMOVE)
        return p->target_leaf == p->sender_leaf && may_commit_privileged(cur, own);
    return false;
}

MarmotError
marmot_proposals_select(const MlsGroup *cur, const MarmotProposalSet *set, size_t **out,
                        size_t *out_count)
{
    *out = NULL;
    *out_count = 0;
    if (set->count == 0) return MARMOT_OK;
    size_t *pick = calloc(set->count, sizeof(*pick));
    if (!pick) return MARMOT_ERR_MEMORY;
    size_t n = 0;
    for (size_t i = 0; i < set->count; i++) {
        const MarmotStoredProposal *p = &set->items[i];
        if (!marmot_proposal_committable(cur, p)) continue;
        /* One per leaving leaf: a SelfRemove over a Remove, then the lowest
         * digest (member-departure.md; never arrival order). */
        size_t j = 0;
        while (j < n && set->items[pick[j]].target_leaf != p->target_leaf) j++;
        if (j == n) {
            if (n == MLS_COMMIT_SUMMARY_MAX) continue;   /* the rest go next time */
            pick[n++] = i;
            continue;
        }
        const MarmotStoredProposal *q = &set->items[pick[j]];
        bool better = (p->type == MLS_PROPOSAL_SELF_REMOVE) != (q->type == MLS_PROPOSAL_SELF_REMOVE)
                          ? p->type == MLS_PROPOSAL_SELF_REMOVE
                          : memcmp(p->digest, q->digest, 32) < 0;
        if (better) pick[j] = i;
    }
    /* Leaf order: independent of arrival. */
    for (size_t a = 1; a < n; a++)
        for (size_t b = a; b > 0 && set->items[pick[b - 1]].target_leaf >
                                        set->items[pick[b]].target_leaf; b--) {
            size_t t = pick[b];
            pick[b] = pick[b - 1];
            pick[b - 1] = t;
        }
    if (n == 0) {
        free(pick);
        return MARMOT_OK;
    }
    *out = pick;
    *out_count = n;
    return MARMOT_OK;
}

/* ── Keeping one ────────────────────────────────────────────────────────── */

static MarmotError
load_mls(Marmot *m, const MarmotGroupId *gid, MlsGroup *out)
{
    memset(out, 0, sizeof(*out));
    uint8_t *blob = NULL;
    size_t len = 0;
    MarmotError err = m->storage->mls_load(m->storage->ctx, "mls_group", gid->data, gid->len,
                                           &blob, &len);
    if (err == MARMOT_OK && (!blob || mls_group_deserialize(blob, len, out) != 0))
        err = MARMOT_ERR_MLS;
    free_secret(blob, len);
    return err;
}

/* Append `op` (taking its bytes) to its sender's slot of this epoch unless
 * one with its ref is there (*out_dup).  Only that slot is rewritten, and
 * the index when the slot is new (review M2); a full slot refuses it
 * (MARMOT_ERR_STORAGE_CONSTRAINT) without touching any other sender's. */
static MarmotError
keep_opened(Marmot *m, const MlsGroup *cur, const uint8_t *msg, size_t msg_len,
            MlsOpenedProposal *op, bool own, bool *out_dup)
{
    *out_dup = false;
    const uint8_t *gid = cur->group_id;
    size_t gid_len = cur->group_id_len;
    SlotId id = { cur->epoch, op->sender_leaf };
    MarmotProposalSet slot;
    memset(&slot, 0, sizeof(slot));
    MarmotError err = slot_load(m, gid, gid_len, id, &slot);
    if (err != MARMOT_OK) {
        marmot_proposals_clear(&slot);
        return err;
    }
    for (size_t i = 0; i < slot.count; i++)
        if (memcmp(slot.items[i].ref, op->ref, 32) == 0) *out_dup = true;
    if (*out_dup) {
        marmot_proposals_clear(&slot);
        return MARMOT_OK;
    }
    if (slot.count >= MARMOT_PROPOSALS_PER_SENDER) {
        marmot_proposals_clear(&slot);
        return MARMOT_ERR_STORAGE_CONSTRAINT;
    }
    bool new_slot = slot.count == 0;
    MarmotStoredProposal *grown = realloc(slot.items, (slot.count + 1) * sizeof(*grown));
    if (!grown) {
        marmot_proposals_clear(&slot);
        return MARMOT_ERR_MEMORY;
    }
    slot.items = grown;
    MarmotStoredProposal *p = &slot.items[slot.count];
    memset(p, 0, sizeof(*p));
    p->epoch = cur->epoch;
    p->own = own;
    p->sender_leaf = op->sender_leaf;
    p->type = op->type;
    p->target_leaf = op->target_leaf;
    memcpy(p->ref, op->ref, 32);
    if (mls_crypto_hash(p->digest, msg, msg_len) != 0 ||
        !leaf_identity(cur, op->sender_leaf, p->sender)) {
        marmot_proposals_clear(&slot);
        return MARMOT_ERR_CRYPTO;
    }
    p->has_target = op->target_leaf != UINT32_MAX && leaf_identity(cur, op->target_leaf, p->target);
    p->ac = op->ac;
    p->ac_len = op->ac_len;
    op->ac = NULL;
    op->ac_len = 0;
    slot.count++;
    err = slot_store(m, gid, gid_len, id, slot.items, slot.count);
    marmot_proposals_clear(&slot);
    if (err != MARMOT_OK || !new_slot) return err;
    SlotIndex idx;
    err = index_load(m, gid, gid_len, &idx);
    if (err != MARMOT_OK) return err;
    if (idx.count >= MARMOT_PROPOSAL_SLOTS_MAX) {
        free(idx.ids);
        return MARMOT_ERR_STORAGE_CONSTRAINT;
    }
    SlotId *ids = realloc(idx.ids, (idx.count + 1) * sizeof(*ids));
    if (!ids) {
        free(idx.ids);
        return MARMOT_ERR_MEMORY;
    }
    idx.ids = ids;
    idx.ids[idx.count++] = id;
    err = index_store(m, gid, gid_len, &idx);
    free(idx.ids);
    return err;
}

static char *
hex32(const uint8_t k[32])
{
    return marmot_hex_encode(k, 32);
}

MarmotError
marmot_proposal_process_inbound(Marmot *m, MarmotGroup *group, const uint8_t *msg,
                                size_t msg_len, const char *event_id_hex,
                                MarmotMessageResult *result)
{
    if (!m || !group || !msg || !result) return MARMOT_ERR_INVALID_ARG;
    if (group->state != MARMOT_GROUP_STATE_ACTIVE) return MARMOT_ERR_USE_AFTER_EVICTION;
    MlsGroup cur;
    MarmotError err = load_mls(m, &group->mls_group_id, &cur);
    if (err != MARMOT_OK) return MARMOT_ERR_MLS;
    /* An adopted group cannot process Commits yet (nostrc-qp24.5.1.3), so a
     * departure it keeps could never be committed or followed: refused
     * before it is opened, like the group's Commits (W24 slice E). */
    if (cur.profile != MARMOT_GROUP_PROFILE_LEGACY) {
        mls_group_free(&cur);
        return MARMOT_ERR_UNSUPPORTED;
    }
    MlsOpenedProposal op;
    int rc = mls_group_open_proposal(&cur, msg, msg_len, &op);
    if (rc == MARMOT_ERR_OWN_MESSAGE) {
        /* Our own PrivateMessage leave, back from a relay: kept when made. */
        mls_group_free(&cur);
        result->type = MARMOT_RESULT_OWN_MESSAGE;
        return MARMOT_OK;
    }
    if (rc != 0) {
        mls_group_free(&cur);
        return (MarmotError)rc;
    }
    bool own = op.sender_leaf == cur.own_leaf_index;
    uint8_t sender[32];
    bool admin = false;
    err = leaf_identity(&cur, op.sender_leaf, sender) ? MARMOT_OK : MARMOT_ERR_FROM_NON_MEMBER;
    if (err == MARMOT_OK) err = marmot_policy_is_admin(&cur, sender, &admin);
    /* Kept types (review M2): a leave, the only proposal a member sends
     * standalone in either profile (MIP-03, group-messaging.md; MDK 0.8
     * sends nothing else standalone) -- a SelfRemove, or in a legacy group a
     * Remove the sender made for itself.  An admin's leave is invalid
     * (MIP-03: step down first).  Anything else is never committed here,
     * so it is not kept. */
    bool self_remove = op.type == MLS_PROPOSAL_SELF_REMOVE;
    bool remove_self = op.type == MLS_PROPOSAL_REMOVE && op.target_leaf == op.sender_leaf &&
                       !marmot_policy_is_adopted(&cur);
    if (err == MARMOT_OK && !self_remove && !remove_self) err = MARMOT_ERR_UNSUPPORTED;
    if (err == MARMOT_OK && admin) err = MARMOT_ERR_ADMIN_CANNOT_LEAVE;
    bool dup = false;
    uint8_t target[32];
    bool has_target = op.target_leaf != UINT32_MAX && leaf_identity(&cur, op.target_leaf, target);
    uint16_t type = op.type;
    bool leave = (type == MLS_PROPOSAL_SELF_REMOVE ||
                  (type == MLS_PROPOSAL_REMOVE && op.target_leaf == op.sender_leaf));
    if (err == MARMOT_OK) err = keep_opened(m, &cur, msg, msg_len, &op, own, &dup);
    mls_opened_proposal_clear(&op);
    mls_group_free(&cur);
    if (err != MARMOT_OK) return err;

    MarmotStorage *s = m->storage;
    if (event_id_hex && s->save_processed_message && strlen(event_id_hex) == 64) {
        uint8_t id[32];
        if (marmot_hex_decode(event_id_hex, id, 32) == 0)
            (void)s->save_processed_message(s->ctx, id, id, marmot_now(), group->epoch,
                                            &group->mls_group_id, MARMOT_MSG_STATE_PROCESSED,
                                            NULL);
    }
    if (own) {
        result->type = MARMOT_RESULT_OWN_MESSAGE;
        return MARMOT_OK;
    }
    result->type = MARMOT_RESULT_PROPOSAL;
    result->proposal.proposal_type = type;
    result->proposal.leave = leave;
    result->proposal.sender_pubkey_hex = hex32(sender);
    result->proposal.target_pubkey_hex = has_target ? hex32(target) : NULL;
    if (!result->proposal.sender_pubkey_hex || (has_target && !result->proposal.target_pubkey_hex)) {
        marmot_message_result_free(result);
        return MARMOT_ERR_MEMORY;
    }
    (void)dup;   /* a repeat reports the same proposal again */
    return MARMOT_OK;
}

/* ── Public: pending proposals ──────────────────────────────────────────── */

MarmotError
marmot_get_pending_proposals(Marmot *m, const MarmotGroupId *mls_group_id,
                             MarmotPendingProposal **out_proposals, size_t *out_count)
{
    if (!m || !mls_group_id || !out_proposals || !out_count) return MARMOT_ERR_INVALID_ARG;
    *out_proposals = NULL;
    *out_count = 0;
    if (!m->storage || !m->storage->mls_load) return MARMOT_ERR_STORAGE;
    MlsGroup cur;
    MarmotError err = load_mls(m, mls_group_id, &cur);
    if (err == MARMOT_ERR_STORAGE_NOT_FOUND) return MARMOT_OK;   /* no state: none */
    if (err != MARMOT_OK) return err;
    MarmotProposalSet set;
    err = marmot_proposals_load(m, mls_group_id->data, mls_group_id->len, &set);
    if (err == MARMOT_OK && set.count > 0) {
        MarmotPendingProposal *list = calloc(set.count, sizeof(*list));
        if (!list) err = MARMOT_ERR_MEMORY;
        size_t n = 0;
        for (size_t i = 0; list && i < set.count; i++) {
            const MarmotStoredProposal *p = &set.items[i];
            if (p->epoch != cur.epoch) continue;
            MarmotPendingProposal *o = &list[n++];
            o->type = p->type;
            memcpy(o->sender, p->sender, 32);
            o->has_target = p->has_target;
            if (p->has_target) memcpy(o->target, p->target, 32);
            o->leave = p->type == MLS_PROPOSAL_SELF_REMOVE ||
                       (p->type == MLS_PROPOSAL_REMOVE && p->target_leaf == p->sender_leaf);
            o->own = p->own;
            o->committable = marmot_proposal_committable(&cur, p);
            memcpy(o->ref, p->ref, 32);
        }
        if (list && n == 0) {
            free(list);
            list = NULL;
        }
        *out_proposals = list;
        *out_count = n;
    }
    marmot_proposals_clear(&set);
    mls_group_free(&cur);
    return err;
}

void
marmot_pending_proposals_free(MarmotPendingProposal *proposals)
{
    free(proposals);
}

/* ── Public: our own SelfRemove ─────────────────────────────────────────── */

/* The leave this group takes (review M1, as MDK 0.8's try_self_remove()):
 * a SelfRemove when its required_capabilities list it -- then every member,
 * now and later, processes it -- else a Remove of ourselves, which an admin
 * commits. */
static MarmotLeaveKind
leave_kind_of(const MlsGroup *g)
{
    return mls_group_requires_proposal(g, MLS_PROPOSAL_SELF_REMOVE) ? MARMOT_LEAVE_SELF_REMOVE
                                                                    : MARMOT_LEAVE_REMOVE_REQUEST;
}

static MarmotError
store_mls(Marmot *m, const MlsGroup *g)
{
    uint8_t *blob = NULL;
    size_t len = 0;
    if (mls_group_serialize(g, &blob, &len) != 0) return MARMOT_ERR_SERIALIZATION;
    MarmotError err = m->storage->mls_store(m->storage->ctx, "mls_group", g->group_id,
                                            g->group_id_len, blob, len);
    free_secret(blob, len);
    return err;
}

/* `out_event_json` NULL: a dry run that writes nothing (marmot_can_self_remove()). */
static MarmotError
self_remove(Marmot *m, const MarmotGroupId *gid, char **out_event_json, MarmotLeaveKind *out_kind)
{
    if (!m || !gid) return MARMOT_ERR_INVALID_ARG;
    bool dry = out_event_json == NULL;
    if (out_event_json) *out_event_json = NULL;
    MarmotStorage *s = m->storage;
    if (!s || !s->find_group_by_mls_id || !s->mls_load || !s->mls_store || !s->mls_delete)
        return MARMOT_ERR_STORAGE;
    MarmotGroup *group = NULL;
    MarmotError err = s->find_group_by_mls_id(s->ctx, gid, &group);
    if (err != MARMOT_OK || !group) return MARMOT_ERR_GROUP_NOT_FOUND;
    MlsGroup cur;
    memset(&cur, 0, sizeof(cur));
    MlsOpenedProposal own;
    memset(&own, 0, sizeof(own));
    uint8_t *msg = NULL;
    size_t msg_len = 0;
    char *json = NULL;
    bool pending = false;
    if (group->state != MARMOT_GROUP_STATE_ACTIVE) {
        err = MARMOT_ERR_USE_AFTER_EVICTION;
        goto out;
    }
    err = dry ? MARMOT_OK : marmot_group_reconcile(m, group);
    if (err == MARMOT_OK) err = load_mls(m, gid, &cur);
    if (err != MARMOT_OK) goto out;
    /* No leave for everyone from an adopted group (W24 slice E): it cannot
     * process the Commit that would follow (nostrc-qp24.5.1.3), and a Remove
     * of ourselves is no adopted leave at all.  Leave on this device only. */
    if (cur.profile != MARMOT_GROUP_PROFILE_LEGACY) {
        err = MARMOT_ERR_UNSUPPORTED;
        goto out;
    }
    /* Admin first (review N1): an admin's answer must not depend on a
     * pending change of theirs. */
    uint8_t me[32];
    bool admin = false;
    err = leaf_identity(&cur, cur.own_leaf_index, me) ? MARMOT_OK : MARMOT_ERR_OWN_LEAF_NOT_FOUND;
    if (err == MARMOT_OK) err = marmot_policy_is_admin(&cur, me, &admin);
    if (err == MARMOT_OK && admin) err = MARMOT_ERR_ADMIN_CANNOT_LEAVE;
    if (err == MARMOT_OK) err = marmot_commit_has_pending(m, group, &pending);
    if (err == MARMOT_OK && pending) err = MARMOT_ERR_OWN_COMMIT_PENDING;
    if (err != MARMOT_OK) goto out;
    MarmotLeaveKind kind = leave_kind_of(&cur);
    if (out_kind) *out_kind = kind;

    /* One proposal per source epoch: the same bytes again within it. */
    uint64_t epoch = 0;
    err = leaving_load(m, gid->data, gid->len, &epoch, &json);
    if (err == MARMOT_OK && epoch == cur.epoch) {
        if (!dry) *out_event_json = json;
        else free(json);
        json = NULL;
        goto out;
    }
    free(json);
    json = NULL;
    if (err != MARMOT_OK && err != MARMOT_ERR_STORAGE_NOT_FOUND) goto out;
    err = MARMOT_OK;
    if (kind == MARMOT_LEAVE_SELF_REMOVE &&
        !mls_group_members_support_proposal(&cur, MLS_PROPOSAL_SELF_REMOVE))
        err = MARMOT_ERR_UNSUPPORTED;   /* required, yet a leaf lacks it: refuse */
    if (dry || err != MARMOT_OK) goto out;

    int rc = kind == MARMOT_LEAVE_SELF_REMOVE
                 ? mls_group_self_remove_proposal(&cur, &msg, &msg_len, &own)
                 : mls_group_remove_self_proposal(&cur, &msg, &msg_len, &own);
    if (rc != 0) {
        err = rc == MARMOT_ERR_UNSUPPORTED ? MARMOT_ERR_UNSUPPORTED : MARMOT_ERR_MLS;
        goto out;
    }
    /* Dated after the group's previous event of ours, like a message: a
     * proposal is never refused for the rate (nostrc-2lrz). */
    int64_t created_at = 0;
    err = marmot_next_group_event_time(m, group->nostr_group_id, false, &created_at);
    if (err != MARMOT_OK) goto out;
    json = marmot_commit_build_event(msg, msg_len, cur.epoch_secrets.exporter_secret,
                                     group->nostr_group_id, created_at);
    if (!json) {
        err = MARMOT_ERR_EVENT_BUILD;
        goto out;
    }
    bool dup = false;
    /* A Remove of ourselves took a step of our handshake ratchet. */
    if (kind == MARMOT_LEAVE_REMOVE_REQUEST) err = store_mls(m, &cur);
    if (err == MARMOT_OK) err = keep_opened(m, &cur, msg, msg_len, &own, true, &dup);
    if (err == MARMOT_OK) err = leaving_store(m, gid->data, gid->len, cur.epoch, json);
    if (err == MARMOT_OK) {
        *out_event_json = json;
        json = NULL;
    }
out:
    free(json);
    free(msg);
    mls_opened_proposal_clear(&own);
    mls_group_free(&cur);
    marmot_group_free(group);
    return err;
}

MarmotError
marmot_self_remove_impl(Marmot *m, const MarmotGroupId *gid, char **out_event_json)
{
    if (!out_event_json) return MARMOT_ERR_INVALID_ARG;
    return self_remove(m, gid, out_event_json, NULL);
}

MarmotError
marmot_can_self_remove(Marmot *m, const MarmotGroupId *mls_group_id, MarmotLeaveKind *out_kind)
{
    return self_remove(m, mls_group_id, NULL, out_kind);
}

MarmotError
marmot_cancel_leave(Marmot *m, const MarmotGroupId *mls_group_id)
{
    if (!m || !mls_group_id || !m->storage || !m->storage->mls_delete)
        return MARMOT_ERR_INVALID_ARG;
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    err = m->storage->mls_delete(m->storage->ctx, MARMOT_MLS_LEAVING_LABEL, mls_group_id->data,
                                 mls_group_id->len);
    if (err == MARMOT_ERR_STORAGE_NOT_FOUND) err = MARMOT_OK;
    return marmot_txn_end(m, err);
}

MarmotError
marmot_self_remove(Marmot *m, const MarmotGroupId *mls_group_id, char **out_event_json)
{
    MarmotError err = marmot_txn_begin(m);
    if (err != MARMOT_OK) return err;
    err = marmot_self_remove_impl(m, mls_group_id, out_event_json);
    MarmotError end = marmot_txn_end(m, err);
    if (err == MARMOT_OK && end != MARMOT_OK) {
        free(*out_event_json);
        *out_event_json = NULL;
    }
    return end;
}
