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
 * MIP-00: Key Packages (stub)
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_create_key_package(Marmot *m,
                           const uint8_t nostr_pubkey[32],
                           const uint8_t nostr_sk[32],
                           const char **relay_urls, size_t relay_count,
                           MarmotKeyPackageResult *result)
{
    (void)m; (void)nostr_pubkey; (void)nostr_sk;
    (void)relay_urls; (void)relay_count; (void)result;
    return MARMOT_ERR_NOT_IMPLEMENTED;
}

/* ──────────────────────────────────────────────────────────────────────────
 * MIP-01: Group Construction (stubs)
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_create_group(Marmot *m,
                     const uint8_t creator_pubkey[32],
                     const char **key_package_event_jsons, size_t kp_count,
                     const MarmotGroupConfig *config,
                     MarmotCreateGroupResult *result)
{
    (void)m; (void)creator_pubkey;
    (void)key_package_event_jsons; (void)kp_count;
    (void)config; (void)result;
    return MARMOT_ERR_NOT_IMPLEMENTED;
}

MarmotError
marmot_merge_pending_commit(Marmot *m, const MarmotGroupId *mls_group_id)
{
    (void)m; (void)mls_group_id;
    return MARMOT_ERR_NOT_IMPLEMENTED;
}

MarmotError
marmot_add_members(Marmot *m,
                    const MarmotGroupId *mls_group_id,
                    const char **key_package_event_jsons, size_t kp_count,
                    char ***out_welcome_jsons, size_t *out_welcome_count,
                    char **out_commit_json)
{
    (void)m; (void)mls_group_id;
    (void)key_package_event_jsons; (void)kp_count;
    (void)out_welcome_jsons; (void)out_welcome_count;
    (void)out_commit_json;
    return MARMOT_ERR_NOT_IMPLEMENTED;
}

MarmotError
marmot_remove_members(Marmot *m,
                       const MarmotGroupId *mls_group_id,
                       const uint8_t (*member_pubkeys)[32], size_t count,
                       char **out_commit_json)
{
    (void)m; (void)mls_group_id;
    (void)member_pubkeys; (void)count;
    (void)out_commit_json;
    return MARMOT_ERR_NOT_IMPLEMENTED;
}

MarmotError
marmot_leave_group(Marmot *m, const MarmotGroupId *mls_group_id)
{
    (void)m; (void)mls_group_id;
    return MARMOT_ERR_NOT_IMPLEMENTED;
}

/* ──────────────────────────────────────────────────────────────────────────
 * MIP-02: Welcome Events (stubs)
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_process_welcome(Marmot *m,
                        const uint8_t wrapper_event_id[32],
                        const char *rumor_event_json,
                        MarmotWelcome **out_welcome)
{
    (void)m; (void)wrapper_event_id;
    (void)rumor_event_json; (void)out_welcome;
    return MARMOT_ERR_NOT_IMPLEMENTED;
}

MarmotError
marmot_accept_welcome(Marmot *m, const MarmotWelcome *welcome)
{
    (void)m; (void)welcome;
    return MARMOT_ERR_NOT_IMPLEMENTED;
}

MarmotError
marmot_decline_welcome(Marmot *m, const MarmotWelcome *welcome)
{
    (void)m; (void)welcome;
    return MARMOT_ERR_NOT_IMPLEMENTED;
}

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
 * MIP-03: Group Messages (stubs)
 * ──────────────────────────────────────────────────────────────────────── */

MarmotError
marmot_create_message(Marmot *m,
                       const MarmotGroupId *mls_group_id,
                       const char *inner_event_json,
                       MarmotOutgoingMessage *result)
{
    (void)m; (void)mls_group_id;
    (void)inner_event_json; (void)result;
    return MARMOT_ERR_NOT_IMPLEMENTED;
}

MarmotError
marmot_process_message(Marmot *m,
                        const char *group_event_json,
                        MarmotMessageResult *result)
{
    (void)m; (void)group_event_json; (void)result;
    return MARMOT_ERR_NOT_IMPLEMENTED;
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
