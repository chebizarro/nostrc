/*
 * libmarmot - Own sender ratchet high-water mark (nostrc-ow0c)
 *
 * The group's MLS state holds our sender ratchets (nostrc-ai04).  A storage
 * snapshot rolled back within an epoch would rewind them, and the next send
 * would reuse a generation -- the key and nonce of a message already sent.
 * The heads we handed out are kept outside the state, under their own label
 * no snapshot covers, and every send moves a state that is behind them past
 * them first.
 *
 * Record (version 1): u8 version, u64 epoch, [32] confirmed transcript hash,
 * u32 own leaf, u32 application generation, u32 handshake generation (the
 * next generation of each chain; big-endian).
 *
 * SPDX-License-Identifier: MIT
 */

#include "send_guard.h"
#include "mls/mls_key_schedule.h"
#include <sodium.h>
#include <stdlib.h>
#include <string.h>

#define HWM_VERSION 1
#define HWM_LEN     (1 + 8 + MLS_HASH_LEN + 4 + 4 + 4)

static void
put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static uint32_t
get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* The next generation of each own chain (0 for a chain not started yet). */
static void
own_heads(const MlsGroup *g, uint32_t *app, uint32_t *hs)
{
    const MlsSecretTree *st = &g->secret_tree;
    uint32_t leaf = g->own_leaf_index;
    *app = *hs = 0;
    if (st->senders && st->sender_initialized && leaf < st->n_leaves &&
        st->sender_initialized[leaf]) {
        *app = st->senders[leaf].application_generation;
        *hs = st->senders[leaf].handshake_generation;
    }
}

MarmotError
marmot_send_guard_apply(Marmot *m, MlsGroup *g)
{
    if (!m || !g || !m->storage || !m->storage->mls_load) return MARMOT_ERR_INVALID_ARG;
    uint8_t *rec = NULL;
    size_t len = 0;
    MarmotError err = m->storage->mls_load(m->storage->ctx, MARMOT_MLS_SEND_HWM_LABEL,
                                           g->group_id, g->group_id_len, &rec, &len);
    if (err == MARMOT_ERR_STORAGE_NOT_FOUND) return MARMOT_OK;
    if (err != MARMOT_OK) return err;
    if (!rec || len != HWM_LEN || rec[0] != HWM_VERSION) {
        free(rec);
        return MARMOT_ERR_STORAGE;
    }
    uint64_t epoch = 0;
    for (int i = 0; i < 8; i++) epoch = (epoch << 8) | rec[1 + i];
    const uint8_t *cth = rec + 9;
    uint32_t leaf = get_u32(rec + 9 + MLS_HASH_LEN);
    uint32_t mark_app = get_u32(rec + 13 + MLS_HASH_LEN);
    uint32_t mark_hs = get_u32(rec + 17 + MLS_HASH_LEN);
    bool same = epoch == g->epoch && leaf == g->own_leaf_index &&
                sodium_memcmp(cth, g->confirmed_transcript_hash, MLS_HASH_LEN) == 0;
    free(rec);
    if (!same) return MARMOT_OK;

    uint32_t app, hs;
    own_heads(g, &app, &hs);
    uint32_t behind = 0;
    if (mark_app > app) behind = mark_app - app;
    if (mark_hs > hs && mark_hs - hs > behind) behind = mark_hs - hs;
    if (behind == 0) return MARMOT_OK;
    /* Both chains step together: each ends at or above its mark. */
    return mls_secret_tree_skip(&g->secret_tree, g->own_leaf_index, behind) == 0
               ? MARMOT_OK : MARMOT_ERR_MLS;
}

MarmotError
marmot_send_guard_record(Marmot *m, const MlsGroup *g)
{
    if (!m || !g || !m->storage || !m->storage->mls_store) return MARMOT_ERR_INVALID_ARG;
    uint32_t app, hs;
    own_heads(g, &app, &hs);
    uint8_t rec[HWM_LEN];
    rec[0] = HWM_VERSION;
    for (int i = 0; i < 8; i++) rec[1 + i] = (uint8_t)(g->epoch >> (56 - 8 * i));
    memcpy(rec + 9, g->confirmed_transcript_hash, MLS_HASH_LEN);
    put_u32(rec + 9 + MLS_HASH_LEN, g->own_leaf_index);
    put_u32(rec + 13 + MLS_HASH_LEN, app);
    put_u32(rec + 17 + MLS_HASH_LEN, hs);
    return m->storage->mls_store(m->storage->ctx, MARMOT_MLS_SEND_HWM_LABEL, g->group_id,
                                 g->group_id_len, rec, sizeof rec);
}
