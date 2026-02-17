/*
 * libmarmot - C implementation of the Marmot protocol (MLS + Nostr)
 *
 * Storage interface (vtable pattern).
 * Mirrors MDK's MdkStorageProvider trait for interoperability.
 *
 * Implementations must provide all non-NULL function pointers.
 * Built-in backends: marmot_storage_memory_new(), marmot_storage_sqlite_new().
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MARMOT_STORAGE_H
#define MARMOT_STORAGE_H

#include "marmot-types.h"
#include "marmot-error.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * MarmotStorage:
 *
 * Abstract storage interface for Marmot.
 * All function pointers receive @ctx as their first argument.
 *
 * Memory ownership rules:
 * - Functions returning pointers transfer ownership to the caller.
 * - Functions accepting pointers as input do NOT take ownership
 *   (the storage implementation must copy if it needs to retain).
 */
typedef struct MarmotStorage {
    /* ── Opaque backend context ────────────────────────────────────────── */
    void *ctx;

    /* ── Group operations (GroupStorage trait) ─────────────────────────── */

    /** List all groups. Caller frees the returned array and each group. */
    MarmotError (*all_groups)(void *ctx,
                               MarmotGroup ***out_groups, size_t *out_count);

    /** Find a group by MLS group ID. Returns NULL in *out if not found. */
    MarmotError (*find_group_by_mls_id)(void *ctx,
                                         const MarmotGroupId *mls_group_id,
                                         MarmotGroup **out);

    /** Find a group by Nostr group ID. */
    MarmotError (*find_group_by_nostr_id)(void *ctx,
                                           const uint8_t nostr_group_id[32],
                                           MarmotGroup **out);

    /** Save (insert or upsert) a group. Storage copies what it needs. */
    MarmotError (*save_group)(void *ctx, const MarmotGroup *group);

    /** Get messages for a group with pagination. Caller frees array + messages. */
    MarmotError (*messages)(void *ctx,
                             const MarmotGroupId *group_id,
                             const MarmotPagination *pagination,
                             MarmotMessage ***out_msgs, size_t *out_count);

    /** Get the most recent message in a group. */
    MarmotError (*last_message)(void *ctx,
                                 const MarmotGroupId *group_id,
                                 MarmotSortOrder sort_order,
                                 MarmotMessage **out);

    /* ── Message operations (MessageStorage trait) ────────────────────── */

    /** Save a message. Storage copies what it needs. */
    MarmotError (*save_message)(void *ctx, const MarmotMessage *msg);

    /** Find a message by event ID. */
    MarmotError (*find_message_by_id)(void *ctx,
                                       const uint8_t event_id[32],
                                       MarmotMessage **out);

    /** Check if a wrapper event ID has already been processed. */
    MarmotError (*is_message_processed)(void *ctx,
                                         const uint8_t wrapper_event_id[32],
                                         bool *out_processed);

    /** Save a processed message record. */
    MarmotError (*save_processed_message)(void *ctx,
                                           const uint8_t wrapper_event_id[32],
                                           const uint8_t *message_event_id, /* [32] or NULL */
                                           int64_t processed_at,
                                           uint64_t epoch,
                                           const MarmotGroupId *mls_group_id,
                                           int state, /* ProcessedMessageState */
                                           const char *failure_reason);

    /* ── Welcome operations (WelcomeStorage trait) ────────────────────── */

    /** Save a welcome. Storage copies what it needs. */
    MarmotError (*save_welcome)(void *ctx, const MarmotWelcome *welcome);

    /** Find a welcome by rumor event ID. */
    MarmotError (*find_welcome_by_event_id)(void *ctx,
                                              const uint8_t event_id[32],
                                              MarmotWelcome **out);

    /** Get pending welcomes with pagination. Caller frees array + welcomes. */
    MarmotError (*pending_welcomes)(void *ctx,
                                     const MarmotPagination *pagination,
                                     MarmotWelcome ***out_welcomes, size_t *out_count);

    /** Check if a wrapper event ID has already been processed as a welcome. */
    MarmotError (*find_processed_welcome)(void *ctx,
                                           const uint8_t wrapper_event_id[32],
                                           bool *out_found,
                                           int *out_state,
                                           char **out_failure_reason);

    /** Save a processed welcome record. */
    MarmotError (*save_processed_welcome)(void *ctx,
                                           const uint8_t wrapper_event_id[32],
                                           const uint8_t *welcome_event_id, /* [32] or NULL */
                                           int64_t processed_at,
                                           int state,
                                           const char *failure_reason);

    /* ── Relay operations ─────────────────────────────────────────────── */

    /** Get relays for a group. Caller frees the returned array. */
    MarmotError (*group_relays)(void *ctx,
                                 const MarmotGroupId *group_id,
                                 MarmotGroupRelay **out_relays, size_t *out_count);

    /** Replace all relays for a group atomically. */
    MarmotError (*replace_group_relays)(void *ctx,
                                         const MarmotGroupId *group_id,
                                         const char **relay_urls, size_t count);

    /* ── Exporter secret operations ───────────────────────────────────── */

    /** Get exporter secret for a group+epoch. Returns MARMOT_ERR_STORAGE_NOT_FOUND if missing. */
    MarmotError (*get_exporter_secret)(void *ctx,
                                        const MarmotGroupId *group_id,
                                        uint64_t epoch,
                                        uint8_t out_secret[32]);

    /** Save exporter secret for a group+epoch. */
    MarmotError (*save_exporter_secret)(void *ctx,
                                         const MarmotGroupId *group_id,
                                         uint64_t epoch,
                                         const uint8_t secret[32]);

    /* ── Snapshot operations (for commit race resolution) ─────────────── */

    /** Create a named snapshot of a group's state. */
    MarmotError (*create_snapshot)(void *ctx,
                                    const MarmotGroupId *group_id,
                                    const char *name);

    /** Rollback a group to a named snapshot (consumes the snapshot). */
    MarmotError (*rollback_snapshot)(void *ctx,
                                      const MarmotGroupId *group_id,
                                      const char *name);

    /** Release a snapshot without rollback. */
    MarmotError (*release_snapshot)(void *ctx,
                                     const MarmotGroupId *group_id,
                                     const char *name);

    /** Prune snapshots older than min_timestamp. Returns count pruned via out. */
    MarmotError (*prune_expired_snapshots)(void *ctx,
                                            uint64_t min_timestamp,
                                            size_t *out_pruned);

    /* ── MLS key store operations ─────────────────────────────────────── */
    /* These mirror OpenMLS StorageProvider for MLS-internal state.
     * The focused implementation stores: key packages, private keys,
     * group state (tree, epoch secrets), proposals. */

    /** Store MLS key material. Key is a string label, value is opaque bytes. */
    MarmotError (*mls_store)(void *ctx,
                              const char *label,
                              const uint8_t *key, size_t key_len,
                              const uint8_t *value, size_t value_len);

    /** Retrieve MLS key material. Caller frees *out_value. */
    MarmotError (*mls_load)(void *ctx,
                             const char *label,
                             const uint8_t *key, size_t key_len,
                             uint8_t **out_value, size_t *out_value_len);

    /** Delete MLS key material. */
    MarmotError (*mls_delete)(void *ctx,
                               const char *label,
                               const uint8_t *key, size_t key_len);

    /* ── Lifecycle ────────────────────────────────────────────────────── */

    /** Whether this is a persistent backend (for snapshot pruning on startup) */
    bool (*is_persistent)(void *ctx);

    /** Destroy the storage backend and free all resources. */
    void (*destroy)(void *ctx);
} MarmotStorage;

/* ──────────────────────────────────────────────────────────────────────────
 * Built-in storage backends
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * marmot_storage_memory_new:
 *
 * Create an in-memory storage backend. Useful for testing.
 * All data is lost when the storage is destroyed.
 *
 * Returns: (transfer full): a new MarmotStorage, or NULL on error
 */
MarmotStorage *marmot_storage_memory_new(void);

/**
 * marmot_storage_sqlite_new:
 * @path: path to SQLite database file (created if not exists)
 * @encryption_key: (nullable): optional encryption key for SQLCipher (or NULL)
 *
 * Create a SQLite-backed persistent storage.
 *
 * Returns: (transfer full): a new MarmotStorage, or NULL on error
 */
MarmotStorage *marmot_storage_sqlite_new(const char *path,
                                          const char *encryption_key);

/**
 * marmot_storage_nostrdb_new:
 * @ndb_handle: (nullable): pointer to an existing `struct ndb *` instance
 *              (borrowed — caller retains ownership). If NULL, event
 *              ingestion into nostrdb is skipped.
 * @mls_state_dir: path to a directory for MLS state LMDB files
 *
 * Create a nostrdb-backed persistent storage.
 *
 * This hybrid backend uses nostrdb for Nostr event storage (kind 443/444/445)
 * and a separate LMDB environment for MLS internal state (group data, key
 * packages, exporter secrets, snapshots).
 *
 * Benefits:
 * - Events properly indexed by nostrdb (kind, author, tags, fulltext search)
 * - Shared nostrdb instance with the main app (no double storage)
 * - LMDB for MLS state is extremely fast for binary key-value operations
 * - No SQLite dependency
 *
 * Requires nostrdb headers at compile time; returns NULL if not available.
 *
 * Returns: (transfer full) (nullable): a new MarmotStorage, or NULL on error
 */
MarmotStorage *marmot_storage_nostrdb_new(void *ndb_handle,
                                           const char *mls_state_dir);

/**
 * marmot_storage_free:
 * @storage: (transfer full) (nullable): storage to destroy
 *
 * Destroys the storage backend, calling its destroy function.
 */
void marmot_storage_free(MarmotStorage *storage);

#ifdef __cplusplus
}
#endif

#endif /* MARMOT_STORAGE_H */
