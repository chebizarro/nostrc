/*
 * libmarmot - Main Marmot instance lifecycle
 *
 * SPDX-License-Identifier: MIT
 */

#include "marmot-internal.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sodium.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Internal helpers
 * ──────────────────────────────────────────────────────────────────────── */

char *
marmot_hex_encode(const uint8_t *data, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    if (!data || len == 0) return NULL;
    char *out = malloc(len * 2 + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = hex[(data[i] >> 4) & 0x0f];
        out[i * 2 + 1] = hex[data[i] & 0x0f];
    }
    out[len * 2] = '\0';
    return out;
}

int
marmot_hex_decode(const char *hex, uint8_t *out, size_t out_len)
{
    if (!hex || !out) return -1;
    size_t hex_len = strlen(hex);
    if (hex_len != out_len * 2) return -1;

    /* First pass: validate all characters are valid hex */
    for (size_t i = 0; i < hex_len; i++) {
        char ch = hex[i];
        if (!((ch >= '0' && ch <= '9') ||
              (ch >= 'a' && ch <= 'f') ||
              (ch >= 'A' && ch <= 'F'))) {
            return -1;
        }
    }

    /* Second pass: decode */
    for (size_t i = 0; i < out_len; i++) {
        uint8_t hi, lo;
        char ch = hex[i * 2];
        if      (ch >= '0' && ch <= '9') hi = (uint8_t)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') hi = (uint8_t)(ch - 'a' + 10);
        else    hi = (uint8_t)(ch - 'A' + 10);

        ch = hex[i * 2 + 1];
        if      (ch >= '0' && ch <= '9') lo = (uint8_t)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') lo = (uint8_t)(ch - 'a' + 10);
        else    lo = (uint8_t)(ch - 'A' + 10);

        out[i] = (hi << 4) | lo;
    }
    return 0;
}

int
marmot_constant_time_eq(const uint8_t *a, const uint8_t *b, size_t n)
{
    volatile uint8_t d = 0;
    for (size_t i = 0; i < n; i++)
        d |= a[i] ^ b[i];
    return d == 0 ? 0 : -1;
}

int64_t
marmot_now(void)
{
    return (int64_t)time(NULL);
}

/* The newest created_at of the group's kind:445 events we published, or of
 * Commits we applied, per nostr_group_id (nostrc-2lrz).  MDK 0.8 never
 * retries a kind:445 it once failed, so a Commit of epoch n+1 it reads
 * before the one of epoch n strands it for good; created_at is the only
 * order relays and clients give events, and a tie falls back to the id. */
static const char GROUP_EVENT_TIME_LABEL[] = "group_event_created_at";

static MarmotError
group_event_time_load(Marmot *m, const uint8_t nostr_group_id[32], int64_t *last)
{
    *last = 0;
    uint8_t *data = NULL;
    size_t len = 0;
    MarmotError err = m->storage->mls_load(m->storage->ctx, GROUP_EVENT_TIME_LABEL,
                                           nostr_group_id, 32, &data, &len);
    if (err == MARMOT_ERR_STORAGE_NOT_FOUND) return MARMOT_OK;
    if (err != MARMOT_OK) return err;
    uint64_t v = 0;
    for (size_t i = 0; data && len == 8 && i < 8; i++) v = (v << 8) | data[i];
    free(data);
    if (len != 8 || v > (uint64_t)INT64_MAX) return MARMOT_ERR_STORAGE;
    *last = (int64_t)v;
    return MARMOT_OK;
}

static MarmotError
group_event_time_store(Marmot *m, const uint8_t nostr_group_id[32], int64_t t)
{
    uint8_t encoded[8];
    for (size_t i = 0; i < 8; i++) encoded[i] = (uint8_t)((uint64_t)t >> (56 - 8 * i));
    return m->storage->mls_store(m->storage->ctx, GROUP_EVENT_TIME_LABEL, nostr_group_id, 32,
                                 encoded, sizeof(encoded));
}

static bool
group_event_time_usable(const Marmot *m, const uint8_t *nostr_group_id)
{
    return m && nostr_group_id && m->storage && m->storage->mls_load &&
           m->storage->mls_store;
}

MarmotError
marmot_next_group_event_time(Marmot *m, const uint8_t nostr_group_id[32],
                             int64_t *created_at)
{
    if (!created_at || !group_event_time_usable(m, nostr_group_id))
        return MARMOT_ERR_INVALID_ARG;
    int64_t last = 0;
    MarmotError err = group_event_time_load(m, nostr_group_id, &last);
    if (err != MARMOT_OK) return err;
    if (last == INT64_MAX) return MARMOT_ERR_STORAGE;
    int64_t next = marmot_now();
    if (next <= last) next = last + 1;
    err = group_event_time_store(m, nostr_group_id, next);
    if (err == MARMOT_OK) *created_at = next;
    return err;
}

/* How far ahead of our clock another member's Commit may lift our next
 * event: a burst of events in one second runs ahead one second per event,
 * and a peer's clock must not push ours past what relays accept. */
#define GROUP_EVENT_MAX_LEAD 60

MarmotError
marmot_observe_group_event_time(Marmot *m, const uint8_t nostr_group_id[32],
                                int64_t created_at)
{
    if (!group_event_time_usable(m, nostr_group_id)) return MARMOT_ERR_INVALID_ARG;
    int64_t cap = marmot_now() + GROUP_EVENT_MAX_LEAD;
    if (created_at > cap) created_at = cap;
    int64_t last = 0;
    MarmotError err = group_event_time_load(m, nostr_group_id, &last);
    if (err != MARMOT_OK || created_at <= last) return err;
    return group_event_time_store(m, nostr_group_id, created_at);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Storage transactions (nostrc-qp24.7)
 * ──────────────────────────────────────────────────────────────────────── */

static bool
txn_supported(const Marmot *m)
{
    return m->storage && m->storage->begin && m->storage->commit &&
           m->storage->rollback;
}

MarmotError
marmot_txn_begin(Marmot *m)
{
    if (!m) return MARMOT_ERR_INVALID_ARG;
    if (m->txn_depth++ > 0) return MARMOT_OK;
    m->txn_keep = false;
    if (!txn_supported(m)) return MARMOT_OK;
    MarmotError err = m->storage->begin(m->storage->ctx);
    if (err != MARMOT_OK) {
        m->txn_depth = 0;
        /* Fail closed: nothing runs outside a transaction the backend offers. */
        return err == MARMOT_ERR_STORAGE_NOT_FOUND ? MARMOT_ERR_STORAGE : err;
    }
    return MARMOT_OK;
}

void
marmot_txn_keep(Marmot *m)
{
    if (m && m->txn_depth > 0) m->txn_keep = true;
}

MarmotError
marmot_txn_end(Marmot *m, MarmotError result)
{
    if (!m || m->txn_depth == 0) return result;
    if (--m->txn_depth > 0) return result;
    bool keep = m->txn_keep;
    m->txn_keep = false;
    if (!txn_supported(m)) return result;
    if (result != MARMOT_OK && !keep) {
        m->storage->rollback(m->storage->ctx);
        return result;
    }
    MarmotError err = m->storage->commit(m->storage->ctx);
    if (err != MARMOT_OK)   /* the backend undid it all */
        return err == MARMOT_ERR_STORAGE_NOT_FOUND ? MARMOT_ERR_STORAGE : err;
    return result;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Lifecycle
 * ──────────────────────────────────────────────────────────────────────── */

Marmot *
marmot_new(MarmotStorage *storage)
{
    MarmotConfig config = marmot_config_default();
    return marmot_new_with_config(storage, &config);
}

Marmot *
marmot_new_with_config(MarmotStorage *storage, const MarmotConfig *config)
{
    if (!storage || !config) return NULL;
    /* Transaction hooks come as a set: a backend that could begin but not
     * commit (or roll back) would lose or half-apply every operation. */
    int hooks = (storage->begin != NULL) + (storage->commit != NULL) +
                (storage->rollback != NULL);
    if (hooks != 0 && hooks != 3) return NULL;

    Marmot *m = calloc(1, sizeof(Marmot));
    if (!m) return NULL;

    m->storage = storage;
    m->config  = *config;
    m->identity_ready = false;

    /* Prune expired snapshots on startup if persistent backend */
    if (storage->is_persistent && storage->is_persistent(storage->ctx)) {
        if (storage->prune_expired_snapshots) {
            int64_t cutoff = marmot_now() - (int64_t)config->snapshot_ttl_seconds;
            if (cutoff > 0) {
                size_t pruned = 0;
                storage->prune_expired_snapshots(storage->ctx,
                                                  (uint64_t)cutoff, &pruned);
                /* Pruning failure is non-fatal */
            }
        }
    }

    return m;
}

void
marmot_free(Marmot *m)
{
    if (!m) return;

    /* Securely wipe key material using libsodium */
    sodium_memzero(m->ed25519_sk, sizeof(m->ed25519_sk));
    sodium_memzero(m->hpke_sk, sizeof(m->hpke_sk));
    sodium_memzero(m->account_proof, sizeof(m->account_proof));

    /* Free storage backend */
    marmot_storage_free(m->storage);

    free(m);
}

/* ──────────────────────────────────────────────────────────────────────────
 * MIP-00 through MIP-03 implementations
 *
 * The real implementations live in:
 *   credentials.c  (MIP-00: Key Packages)
 *   groups.c       (MIP-01: Group Construction)
 *   welcome.c      (MIP-02: Welcome Events)
 *   messages.c     (MIP-03: Group Messages)
 *
 * Only marmot_get_pending_welcomes is here (pass-through to storage).
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_get_pending_welcomes(Marmot *m,
                             const MarmotPagination *pagination,
                             MarmotWelcome ***out_welcomes, size_t *out_count)
{
    if (!m || !out_welcomes || !out_count)
        return MARMOT_ERR_INVALID_ARG;

    if (!m->storage || !m->storage->pending_welcomes)
        return MARMOT_ERR_STORAGE;

    MarmotPagination pg = pagination ? *pagination : marmot_pagination_default();
    return m->storage->pending_welcomes(m->storage->ctx,
                                         &pg, out_welcomes, out_count);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Group queries
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_get_group(Marmot *m, const MarmotGroupId *mls_group_id, MarmotGroup **out)
{
    if (!m || !mls_group_id || !out)
        return MARMOT_ERR_INVALID_ARG;
    if (!m->storage || !m->storage->find_group_by_mls_id)
        return MARMOT_ERR_STORAGE;
    return m->storage->find_group_by_mls_id(m->storage->ctx, mls_group_id, out);
}

MarmotError
marmot_get_all_groups(Marmot *m, MarmotGroup ***out_groups, size_t *out_count)
{
    if (!m || !out_groups || !out_count)
        return MARMOT_ERR_INVALID_ARG;
    if (!m->storage || !m->storage->all_groups)
        return MARMOT_ERR_STORAGE;
    return m->storage->all_groups(m->storage->ctx, out_groups, out_count);
}

MarmotError
marmot_get_messages(Marmot *m,
                     const MarmotGroupId *mls_group_id,
                     const MarmotPagination *pagination,
                     MarmotMessage ***out_msgs, size_t *out_count)
{
    if (!m || !mls_group_id || !out_msgs || !out_count)
        return MARMOT_ERR_INVALID_ARG;

    if (!m->storage || !m->storage->messages)
        return MARMOT_ERR_STORAGE;

    MarmotPagination pg = pagination ? *pagination : marmot_pagination_default();
    return m->storage->messages(m->storage->ctx,
                                mls_group_id, &pg, out_msgs, out_count);
}
