/*
 * libmarmot - Convergence state and branch selection (nostrc-w1m0)
 *
 * See convergence.h.  The record layout:
 *
 *   u8  version (1)                         -- prefix, as since 0.5.0:
 *   u64 parent_epoch                           entries[0] (the state the
 *   u8  privileged, [32] committer,            canonical tip was entered
 *   [32] digest                                from, and its Commit's key)
 *   opaque32 parent_state
 *   u8  tier (0 full, 1 reader)             -- since 0.10.0 (nostrc-yuj2)
 *   u32 pending_count, u32 leaf * count        (always written as 0 now)
 *   -- since 0.12.0 (W25, nostrc-w1m0):
 *   u8  CONV_TRAILER_V2
 *   u8  flags of entries[0] (CONV_OWN)
 *   opaque32 commit of entries[0]
 *   u8  n, then n older entries (epoch descending):
 *       u64 epoch, u8 flags (CONV_PRIVILEGED|CONV_OWN|CONV_READER),
 *       [32] committer, [32] digest, opaque32 commit, opaque32 state
 *   u8  n, then n candidates:
 *       u64 source_epoch, [32] digest, [32] parent_tag,
 *       u8 flags (CONV_OWN; with it CONV_PRIVILEGED, CONV_UNCONFIRMED), opaque32 msg,
 *       opaque8 event_id, and when own: opaque32 post
 *   u8  n, then n witnesses: u64 epoch, [32] tag, [32] sender, u32 leaf
 *   u8  n, then n branch secrets: u64 epoch, [32] tag, [32] exporter,
 *       [32] sender_data
 *
 * libmarmot 0.10.0 and 0.11.0 refuse a record with the trailer (their
 * reader wants the end after the pending list): after a downgrade, late
 * messages and competing Commits of the retained epochs fail closed until
 * the next Commit, as for every earlier trailer.
 *
 * SPDX-License-Identifier: MIT
 */

#include "convergence.h"
#include "mls/mls-internal.h"
#include <sodium.h>
#include <stdlib.h>
#include <string.h>

#define CONV_RECORD_VERSION 1
#define CONV_TIER_FULL      0
#define CONV_TIER_READER    1
/* 0xC1 was this branch's first layout (never released, not read). */
#define CONV_TRAILER_V2     0xC2

#define CONV_PRIVILEGED  0x01
#define CONV_OWN         0x02
#define CONV_READER      0x04
#define CONV_UNCONFIRMED 0x08

static void
wipe_free(uint8_t *p, size_t len)
{
    if (!p) return;
    sodium_memzero(p, len);
    free(p);
}

void
conv_entry_clear(ConvEntry *e)
{
    if (!e) return;
    mls_group_free(&e->state);
    wipe_free(e->commit, e->commit_len);
    memset(e, 0, sizeof(*e));
}

void
conv_candidate_clear(ConvCandidate *c)
{
    if (!c) return;
    free(c->msg);
    free(c->event_id);
    wipe_free(c->own_post, c->own_post_len);
    memset(c, 0, sizeof(*c));
}

void
conv_history_clear(ConvHistory *h)
{
    if (!h) return;
    for (size_t i = 0; i < h->n_entries; i++) conv_entry_clear(&h->entries[i]);
    for (size_t i = 0; i < h->n_cands; i++) conv_candidate_clear(&h->cands[i]);
    sodium_memzero(h, sizeof(*h));
}

ConvEntry *
conv_history_entry(ConvHistory *h, uint64_t epoch)
{
    for (size_t i = 0; i < h->n_entries; i++)
        if (h->entries[i].epoch == epoch) return &h->entries[i];
    return NULL;
}

/* ── Encoding ─────────────────────────────────────────────────────────── */

static bool
write_state(MlsTlsBuf *buf, const MlsGroup *g)
{
    uint8_t *blob = NULL;
    size_t len = 0;
    if (mls_group_serialize(g, &blob, &len) != 0) return false;
    bool ok = mls_tls_write_opaque32(buf, blob, len) == 0;
    wipe_free(blob, len);
    return ok;
}

static bool
write_bytes32(MlsTlsBuf *buf, const uint8_t *p, size_t len)
{
    static const uint8_t none = 0;
    return mls_tls_write_opaque32(buf, p ? p : &none, p ? len : 0) == 0;
}

int
conv_history_encode(const ConvHistory *h, uint8_t **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    if (!h || h->n_entries == 0 || h->n_entries > CONV_MAX_REWIND_COMMITS ||
        h->n_cands > CONV_MAX_CANDIDATES || h->n_wits > CONV_MAX_WITNESSES ||
        h->n_secrets > CONV_MAX_CANDIDATES)
        return -1;
    MlsTlsBuf buf;
    if (mls_tls_buf_init(&buf, 4096) != 0) return -1;
    const ConvEntry *e0 = &h->entries[0];
    bool ok = mls_tls_write_u8(&buf, CONV_RECORD_VERSION) == 0 &&
              mls_tls_write_u64(&buf, e0->epoch) == 0 &&
              mls_tls_write_u8(&buf, e0->key.privileged ? 1 : 0) == 0 &&
              mls_tls_buf_append(&buf, e0->key.committer, 32) == 0 &&
              mls_tls_buf_append(&buf, e0->key.digest, 32) == 0 &&
              write_state(&buf, &e0->state) &&
              mls_tls_write_u8(&buf, e0->reader ? CONV_TIER_READER : CONV_TIER_FULL) == 0 &&
              mls_tls_write_u32(&buf, 0) == 0 &&
              mls_tls_write_u8(&buf, CONV_TRAILER_V2) == 0 &&
              mls_tls_write_u8(&buf, e0->own ? CONV_OWN : 0) == 0 &&
              write_bytes32(&buf, e0->commit, e0->commit_len) &&
              mls_tls_write_u8(&buf, (uint8_t)(h->n_entries - 1)) == 0;
    for (size_t i = 1; ok && i < h->n_entries; i++) {
        const ConvEntry *e = &h->entries[i];
        uint8_t flags = (uint8_t)((e->key.privileged ? CONV_PRIVILEGED : 0) |
                                  (e->own ? CONV_OWN : 0) | (e->reader ? CONV_READER : 0));
        ok = mls_tls_write_u64(&buf, e->epoch) == 0 && mls_tls_write_u8(&buf, flags) == 0 &&
             mls_tls_buf_append(&buf, e->key.committer, 32) == 0 &&
             mls_tls_buf_append(&buf, e->key.digest, 32) == 0 &&
             write_bytes32(&buf, e->commit, e->commit_len) && write_state(&buf, &e->state);
    }
    ok = ok && mls_tls_write_u8(&buf, (uint8_t)h->n_cands) == 0;
    for (size_t i = 0; ok && i < h->n_cands; i++) {
        const ConvCandidate *c = &h->cands[i];
        const char *id = c->event_id ? c->event_id : "";
        uint8_t flags = (uint8_t)(c->own ? (CONV_OWN | (c->own_privileged ? CONV_PRIVILEGED : 0) |
                                            (c->own_unconfirmed ? CONV_UNCONFIRMED : 0))
                                         : 0);
        ok = mls_tls_write_u64(&buf, c->source_epoch) == 0 &&
             mls_tls_buf_append(&buf, c->digest, 32) == 0 &&
             mls_tls_buf_append(&buf, c->parent_tag, 32) == 0 &&
             mls_tls_write_u8(&buf, flags) == 0 &&
             write_bytes32(&buf, c->msg, c->msg_len) &&
             mls_tls_write_opaque8(&buf, (const uint8_t *)id, strlen(id)) == 0;
        if (ok && c->own) ok = write_bytes32(&buf, c->own_post, c->own_post_len);
    }
    ok = ok && mls_tls_write_u8(&buf, (uint8_t)h->n_wits) == 0;
    for (size_t i = 0; ok && i < h->n_wits; i++)
        ok = mls_tls_write_u64(&buf, h->wits[i].epoch) == 0 &&
             mls_tls_buf_append(&buf, h->wits[i].tag, 32) == 0 &&
             mls_tls_buf_append(&buf, h->wits[i].sender, 32) == 0 &&
             mls_tls_write_u32(&buf, h->wits[i].leaf) == 0;
    ok = ok && mls_tls_write_u8(&buf, (uint8_t)h->n_secrets) == 0;
    for (size_t i = 0; ok && i < h->n_secrets; i++)
        ok = mls_tls_write_u64(&buf, h->secrets[i].epoch) == 0 &&
             mls_tls_buf_append(&buf, h->secrets[i].tag, 32) == 0 &&
             mls_tls_buf_append(&buf, h->secrets[i].exporter, 32) == 0 &&
             mls_tls_buf_append(&buf, h->secrets[i].sender_data, 32) == 0;
    if (!ok) {
        if (buf.data) sodium_memzero(buf.data, buf.len);
        mls_tls_buf_free(&buf);
        return -1;
    }
    *out = buf.data;
    *out_len = buf.len;
    return 0;
}

/* ── Decoding ─────────────────────────────────────────────────────────── */

static bool
read_state(MlsTlsReader *r, MlsGroup *out)
{
    uint8_t *blob = NULL;
    size_t len = 0;
    if (mls_tls_read_opaque32(r, &blob, &len) != 0) return false;
    bool ok = mls_group_deserialize(blob, len, out) == 0;
    wipe_free(blob, len);
    return ok;
}

/* opaque32 bytes; an empty one reads as NULL. */
static bool
read_bytes32(MlsTlsReader *r, uint8_t **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    uint8_t *p = NULL;
    size_t len = 0;
    if (mls_tls_read_opaque32(r, &p, &len) != 0) return false;
    if (len == 0) {
        free(p);
        return true;
    }
    *out = p;
    *out_len = len;
    return true;
}

static bool
read_trailer(MlsTlsReader *r, ConvHistory *out)
{
    ConvEntry *e0 = &out->entries[0];
    uint8_t flags0 = 0, n = 0;
    if (mls_tls_read_u8(r, &flags0) != 0 || (flags0 & ~CONV_OWN) != 0 ||
        !read_bytes32(r, &e0->commit, &e0->commit_len) || mls_tls_read_u8(r, &n) != 0 ||
        n >= CONV_MAX_REWIND_COMMITS)
        return false;
    e0->own = (flags0 & CONV_OWN) != 0;
    for (uint8_t i = 0; i < n; i++) {
        ConvEntry *e = &out->entries[out->n_entries];
        uint8_t flags = 0;
        out->n_entries++;   /* cleared by conv_history_clear() from here */
        if (mls_tls_read_u64(r, &e->epoch) != 0 || mls_tls_read_u8(r, &flags) != 0 ||
            (flags & ~(CONV_PRIVILEGED | CONV_OWN | CONV_READER)) != 0 ||
            mls_tls_read_fixed(r, e->key.committer, 32) != 0 ||
            mls_tls_read_fixed(r, e->key.digest, 32) != 0 ||
            !read_bytes32(r, &e->commit, &e->commit_len) || !read_state(r, &e->state) ||
            e->state.epoch != e->epoch ||
            e->epoch + 1 != out->entries[out->n_entries - 2].epoch)
            return false;
        e->key.privileged = (flags & CONV_PRIVILEGED) != 0;
        e->key.committer_leaf = UINT32_MAX;
        e->own = (flags & CONV_OWN) != 0;
        e->reader = (flags & CONV_READER) != 0;
    }
    if (mls_tls_read_u8(r, &n) != 0 || n > CONV_MAX_CANDIDATES) return false;
    for (uint8_t i = 0; i < n; i++) {
        ConvCandidate *c = &out->cands[out->n_cands++];
        uint8_t flags = 0;
        uint8_t *id = NULL;
        size_t id_len = 0;
        if (mls_tls_read_u64(r, &c->source_epoch) != 0 ||
            mls_tls_read_fixed(r, c->digest, 32) != 0 ||
            mls_tls_read_fixed(r, c->parent_tag, 32) != 0 || mls_tls_read_u8(r, &flags) != 0 ||
            (flags & ~(CONV_OWN | CONV_PRIVILEGED | CONV_UNCONFIRMED)) != 0 ||
            ((flags & (CONV_PRIVILEGED | CONV_UNCONFIRMED)) && !(flags & CONV_OWN)) ||
            !read_bytes32(r, &c->msg, &c->msg_len) || !c->msg ||
            mls_tls_read_opaque8(r, &id, &id_len) != 0)
            return false;
        if (id_len > 0) {
            c->event_id = calloc(1, id_len + 1);
            if (!c->event_id) {
                free(id);
                return false;
            }
            memcpy(c->event_id, id, id_len);
        }
        free(id);
        c->own = (flags & CONV_OWN) != 0;
        c->own_privileged = (flags & CONV_PRIVILEGED) != 0;
        c->own_unconfirmed = (flags & CONV_UNCONFIRMED) != 0;
        if (c->own && (!read_bytes32(r, &c->own_post, &c->own_post_len) || !c->own_post))
            return false;
    }
    if (mls_tls_read_u8(r, &n) != 0 || n > CONV_MAX_WITNESSES) return false;
    for (uint8_t i = 0; i < n; i++) {
        ConvWitness *w = &out->wits[out->n_wits++];
        if (mls_tls_read_u64(r, &w->epoch) != 0 || mls_tls_read_fixed(r, w->tag, 32) != 0 ||
            mls_tls_read_fixed(r, w->sender, 32) != 0 || mls_tls_read_u32(r, &w->leaf) != 0)
            return false;
    }
    if (mls_tls_read_u8(r, &n) != 0 || n > CONV_MAX_CANDIDATES) return false;
    for (uint8_t i = 0; i < n; i++) {
        ConvBranchSecret *s = &out->secrets[out->n_secrets++];
        if (mls_tls_read_u64(r, &s->epoch) != 0 || mls_tls_read_fixed(r, s->tag, 32) != 0 ||
            mls_tls_read_fixed(r, s->exporter, 32) != 0 ||
            mls_tls_read_fixed(r, s->sender_data, 32) != 0)
            return false;
    }
    return mls_tls_reader_done(r);
}

int
conv_history_decode(const uint8_t *data, size_t len, ConvHistory *out)
{
    memset(out, 0, sizeof(*out));
    if (!data) return -1;
    MlsTlsReader r;
    mls_tls_reader_init(&r, data, len);
    ConvEntry *e0 = &out->entries[0];
    uint8_t version = 0, privileged = 0, tier = CONV_TIER_FULL;
    bool ok = mls_tls_read_u8(&r, &version) == 0 && version == CONV_RECORD_VERSION &&
              mls_tls_read_u64(&r, &e0->epoch) == 0 &&
              mls_tls_read_u8(&r, &privileged) == 0 && privileged <= 1 &&
              mls_tls_read_fixed(&r, e0->key.committer, 32) == 0 &&
              mls_tls_read_fixed(&r, e0->key.digest, 32) == 0;
    if (ok) {
        out->n_entries = 1;
        ok = read_state(&r, &e0->state) && e0->state.epoch == e0->epoch;
    }
    e0->key.privileged = privileged == 1;
    e0->key.committer_leaf = UINT32_MAX;
    /* Without a tier (0.5.0-0.9.0): a full state, nothing else. */
    if (ok && !mls_tls_reader_done(&r)) {
        uint32_t n_pending = 0, leaf = 0;
        ok = mls_tls_read_u8(&r, &tier) == 0 && tier <= CONV_TIER_READER &&
             mls_tls_read_u32(&r, &n_pending) == 0 &&
             n_pending <= mls_tls_reader_remaining(&r) / 4;
        /* The 0.10.0 pending list no longer means anything: skip it. */
        for (uint32_t i = 0; ok && i < n_pending; i++) ok = mls_tls_read_u32(&r, &leaf) == 0;
        e0->reader = tier == CONV_TIER_READER;
        if (ok && !mls_tls_reader_done(&r)) {
            uint8_t marker = 0;
            ok = mls_tls_read_u8(&r, &marker) == 0 && marker == CONV_TRAILER_V2 &&
                 read_trailer(&r, out);
        }
    }
    if (!ok) {
        conv_history_clear(out);
        return -1;
    }
    return 0;
}

/* ── Witnesses ────────────────────────────────────────────────────────── */

static bool
witness_is(const ConvWitness *w, uint64_t epoch, const uint8_t tag[32])
{
    return w->epoch == epoch && memcmp(w->tag, tag, 32) == 0;
}

size_t
conv_witness_count(const ConvHistory *h, uint64_t epoch, const uint8_t tag[32])
{
    size_t n = 0;
    for (size_t i = 0; i < h->n_wits; i++)
        if (witness_is(&h->wits[i], epoch, tag)) n++;   /* senders are distinct */
    return n < CONV_WITNESS_QUORUM_SENDERS ? n : CONV_WITNESS_QUORUM_SENDERS;
}

bool
conv_witness_known(const ConvHistory *h, uint64_t epoch, const uint8_t tag[32],
                   const uint8_t sender[32])
{
    for (size_t i = 0; i < h->n_wits; i++)
        if (witness_is(&h->wits[i], epoch, tag) && memcmp(h->wits[i].sender, sender, 32) == 0)
            return true;
    return false;
}

bool
conv_witness_full(const ConvHistory *h, uint64_t epoch, const uint8_t tag[32])
{
    return conv_witness_count(h, epoch, tag) >= CONV_WITNESS_QUORUM_SENDERS;
}

bool
conv_witness_leaf_known(const ConvHistory *h, uint64_t epoch, const uint8_t tag[32], uint32_t leaf)
{
    for (size_t i = 0; i < h->n_wits; i++)
        if (witness_is(&h->wits[i], epoch, tag) && h->wits[i].leaf == leaf) return true;
    return false;
}

int
conv_witness_add(ConvHistory *h, uint64_t epoch, const uint8_t tag[32], const uint8_t sender[32],
                 uint32_t leaf)
{
    if (conv_witness_known(h, epoch, tag, sender) || conv_witness_full(h, epoch, tag)) return 0;
    if (h->n_wits >= CONV_MAX_WITNESSES) return -1;
    ConvWitness *w = &h->wits[h->n_wits++];
    w->epoch = epoch;
    memcpy(w->tag, tag, 32);
    memcpy(w->sender, sender, 32);
    w->leaf = leaf;
    return 1;
}

void
conv_score_witnesses(const ConvHistory *h, const ConvPathState *path, size_t n_path,
                     ConvBranchScore *s)
{
    size_t quorum_epochs = 0;
    s->witness_score = 0;
    for (size_t i = 0; i < n_path; i++) {
        const ConvPathState *p = &path[i];
        if (!p->tag || p->epoch <= s->fork_epoch || p->epoch > s->tip_epoch) continue;
        /* A witness of an epoch outside the branch tip's app-payload window
         * counts for nothing (retained-history.md "App-payload retention"). */
        if (s->tip_epoch - p->epoch > CONV_APP_PAYLOAD_PAST_EPOCH_LIMIT) continue;
        size_t c = conv_witness_count(h, p->epoch, p->tag);
        s->witness_score += c;
        if (c >= CONV_WITNESS_QUORUM_SENDERS) quorum_epochs++;
    }
    s->quorum = quorum_epochs >= CONV_WITNESS_QUORUM_EPOCHS;
}

/* ── Branch comparison ────────────────────────────────────────────────── */

uint64_t
conv_effective_depth(const ConvBranchScore *s)
{
    uint64_t raw = s->tip_epoch > s->fork_epoch ? s->tip_epoch - s->fork_epoch : 0;
    return raw + (s->quorum ? CONV_MAX_WITNESS_OVERRIDE_DEPTH : 0);
}

int
conv_branch_cmp(const ConvBranchScore *a, const ConvBranchScore *b)
{
    uint64_t da = conv_effective_depth(a), db = conv_effective_depth(b);
    if (da != db) return da > db ? 1 : -1;
    if (a->quorum != b->quorum) return a->quorum ? 1 : -1;
    if (a->witness_score != b->witness_score) return a->witness_score > b->witness_score ? 1 : -1;
    if (a->privileged != b->privileged) return a->privileged ? 1 : -1;
    int c = memcmp(a->committer, b->committer, 32);
    if (c != 0) return c < 0 ? 1 : -1;
    c = memcmp(a->digest, b->digest, 32);
    if (c != 0) return c < 0 ? 1 : -1;
    return 0;
}
