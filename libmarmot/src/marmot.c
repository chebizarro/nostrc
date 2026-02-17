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
    return m->storage->find_group_by_mls_id(m->storage->ctx, mls_group_id, out);
}

MarmotError
marmot_get_all_groups(Marmot *m, MarmotGroup ***out_groups, size_t *out_count)
{
    if (!m || !out_groups || !out_count)
        return MARMOT_ERR_INVALID_ARG;
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

    MarmotPagination pg = pagination ? *pagination : marmot_pagination_default();
    return m->storage->messages(m->storage->ctx,
                                mls_group_id, &pg, out_msgs, out_count);
}
