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

/* created_at of a group's kind:445 events (nostrc-2lrz).
 *
 * MDK 0.8 never retries a kind:445 it once failed, so an event of epoch n+1
 * it reads before the Commit that opens epoch n+1 strands it for good, and
 * created_at is the only order relays and clients give events (a tie falls
 * back to the id).  So what we publish to a group is dated after the
 * group's previous event, and after every Commit we applied.
 *
 * Kept per nostr_group_id (immutable, MIP-01) in mls_kv: two big-endian
 * int64, `last`, the newest created_at we published or applied, and
 * `commit`, the newest of a Commit we published or applied or of the
 * Welcome that added us.  A row of another shape is treated as absent and
 * overwritten (review W24 N2).
 *
 * The lead over our clock is bounded (review W24 M2): relays refuse events
 * dated too far ahead (strfry by default past 900 s, nostrc's relayd past
 * 600 s), and a lead is public timing metadata (README, "Timing").
 *   - Nothing is dated more than GROUP_EVENT_MAX_LEAD ahead, and another
 *     member's Commit (or the Welcome that added us) moves us at most that
 *     far: we follow Commits as far ahead as anyone may date them.
 *   - An application message runs at most GROUP_EVENT_SOFT_LEAD ahead on
 *     its own: past it, messages share a second.  They are of one epoch,
 *     which MDK reads in any order; a message is dated after the Commits it
 *     follows, except at the bound (below).  So a Commit always finds a
 *     second after a burst of messages.
 *   - A Commit that cannot be dated within the bound is refused with
 *     MARMOT_ERR_EVENT_RATE (retry in a second): Commits made back to back
 *     for half a minute, or ours right after another member's Commit dated
 *     at the bound.  A message is never refused: past the bound it is dated
 *     at it, sharing a second only with a Commit dated there (a member far
 *     ahead of our clock, or such a run of Commits). */
static const char GROUP_EVENT_TIME_LABEL[] = "group_event_created_at";
#define GROUP_EVENT_SOFT_LEAD 30
#define GROUP_EVENT_MAX_LEAD  60

typedef struct {
    int64_t last, commit;
} GroupEventTimes;

static MarmotError
group_event_times_load(Marmot *m, const uint8_t nostr_group_id[32], GroupEventTimes *t)
{
    t->last = t->commit = 0;
    uint8_t *data = NULL;
    size_t len = 0;
    MarmotError err = m->storage->mls_load(m->storage->ctx, GROUP_EVENT_TIME_LABEL,
                                           nostr_group_id, 32, &data, &len);
    if (err == MARMOT_ERR_STORAGE_NOT_FOUND) return MARMOT_OK;
    if (err != MARMOT_OK) return err;
    uint64_t v[2] = { 0, 0 };
    for (size_t i = 0; data && len == 16 && i < 16; i++)
        v[i / 8] = (v[i / 8] << 8) | data[i];
    free(data);
    if (len == 16 && v[0] <= (uint64_t)INT64_MAX && v[1] <= v[0]) {
        t->last = (int64_t)v[0];
        t->commit = (int64_t)v[1];
    }
    /* Nothing we date passes now + GROUP_EVENT_MAX_LEAD, so a value past it
     * was written while our clock ran ahead and was then set back (or the
     * row is damaged): order against it cannot be kept within what relays
     * and MDK accept.  Read as a second inside the bound, so the next Commit
     * fits (review W24 L6); this also keeps last + 1 from overflowing. */
    int64_t bound = marmot_now() + GROUP_EVENT_MAX_LEAD;
    if (t->last > bound) t->last = bound - 1;
    if (t->commit > bound) t->commit = bound - 1;
    return MARMOT_OK;
}

static MarmotError
group_event_times_store(Marmot *m, const uint8_t nostr_group_id[32], const GroupEventTimes *t)
{
    uint8_t encoded[16];
    for (size_t i = 0; i < 8; i++) {
        encoded[i] = (uint8_t)((uint64_t)t->last >> (56 - 8 * i));
        encoded[8 + i] = (uint8_t)((uint64_t)t->commit >> (56 - 8 * i));
    }
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
marmot_next_group_event_time(Marmot *m, const uint8_t nostr_group_id[32], bool commit,
                             int64_t *created_at)
{
    if (!created_at || !group_event_time_usable(m, nostr_group_id))
        return MARMOT_ERR_INVALID_ARG;
    GroupEventTimes t;
    MarmotError err = group_event_times_load(m, nostr_group_id, &t);
    if (err != MARMOT_OK) return err;
    int64_t now = marmot_now();
    int64_t next = t.last < now ? now : t.last + 1;
    if (!commit && next > now + GROUP_EVENT_SOFT_LEAD)
        next = t.commit < now + GROUP_EVENT_SOFT_LEAD ? now + GROUP_EVENT_SOFT_LEAD
                                                       : t.commit + 1;
    if (next > now + GROUP_EVENT_MAX_LEAD) {
        if (commit) return MARMOT_ERR_EVENT_RATE;
        next = now + GROUP_EVENT_MAX_LEAD;
    }
    if (next > t.last) t.last = next;
    if (commit) t.commit = next;
    err = group_event_times_store(m, nostr_group_id, &t);
    if (err == MARMOT_OK) *created_at = next;
    return err;
}

MarmotError
marmot_observe_group_event_time(Marmot *m, const uint8_t nostr_group_id[32],
                                int64_t created_at)
{
    if (!group_event_time_usable(m, nostr_group_id)) return MARMOT_ERR_INVALID_ARG;
    int64_t cap = marmot_now() + GROUP_EVENT_MAX_LEAD;
    if (created_at > cap) created_at = cap;
    GroupEventTimes t;
    MarmotError err = group_event_times_load(m, nostr_group_id, &t);
    if (err != MARMOT_OK || created_at <= t.commit) return err;
    t.commit = created_at;
    if (created_at > t.last) t.last = created_at;
    return group_event_times_store(m, nostr_group_id, &t);
}

MarmotError
marmot_carry_group_event_time(Marmot *m, const uint8_t from_nostr_group_id[32],
                              const uint8_t to_nostr_group_id[32])
{
    if (!group_event_time_usable(m, from_nostr_group_id) || !to_nostr_group_id)
        return MARMOT_ERR_INVALID_ARG;
    GroupEventTimes from, to;
    MarmotError err = group_event_times_load(m, from_nostr_group_id, &from);
    if (err == MARMOT_OK) err = group_event_times_load(m, to_nostr_group_id, &to);
    if (err != MARMOT_OK) return err;
    if (from.last <= to.last && from.commit <= to.commit) return MARMOT_OK;
    if (from.last > to.last) to.last = from.last;
    if (from.commit > to.commit) to.commit = from.commit;
    return group_event_times_store(m, to_nostr_group_id, &to);
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
