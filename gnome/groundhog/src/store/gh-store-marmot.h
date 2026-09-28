#ifndef GH_STORE_MARMOT_H
#define GH_STORE_MARMOT_H

#include <marmot/marmot-storage.h>

#include "gh-store.h"

G_BEGIN_DECLS

/* GhStoreMarmot: libmarmot's MarmotStorage (marmot-storage.h) over the
 * account's encrypted GhStore (privacy charter §3.9, decision D5 = vtable).
 * MLS state lives in the same SQLCipher database, on the same connection, as
 * the conversations and the outbox, so one GhStore transaction can hold MLS
 * state, a snapshot and outbox events together (T-mls, §3.5). Groundhog is
 * the only writer; Gnostr's marmot.db is never read or imported.
 *
 * Tables (schema v1 mls_kv and mls_snapshots, the rest v2; see
 * gh-store-schema.c): mls_kv (mls_store/load/delete), mls_group_info,
 * mls_group_relays, mls_exporter_secrets, mls_messages, mls_processed_messages,
 * mls_welcomes, mls_processed_welcomes, mls_key_packages, mls_snapshots and
 * mls_snapshot_rows. v1's mls_groups (conversation <-> MLS group link) belongs
 * to the caller and is never written here.
 *
 * Transactions. Every write runs as its own transaction or, inside the
 * caller's gh_store_begin()/gh_store_commit(), as a savepoint of it. A write
 * that fails is undone completely and nothing outside it is touched. To make
 * a libmarmot operation atomic with its snapshot and outbox rows (T-mls), wrap
 * all of it:
 *
 *   gh_store_begin(store, &error);
 *   storage->create_snapshot(storage->ctx, gid, "pending-commit");
 *   marmot_add_members(marmot, gid, ...);      MLS state, group, secrets
 *   gh_store_enqueue(store, ...); gh_store_seal(store, ...);
 *   gh_store_commit(store, &error);            all of it, or none of it
 *
 * If SQLite abandons the caller's transaction (disk full, I/O error), every
 * later operation fails until the caller rolls back, so no fragment of it can
 * be committed on its own. Reads see the caller's uncommitted writes.
 *
 * Snapshots (commit race resolution) are exact row copies, in the caller's
 * transaction: create_snapshot copies the group's mls_group_info row, its
 * mls_group_relays and mls_exporter_secrets rows and its MLS group state (the
 * mls_kv rows with a group-scoped label, i.e. "mls_group", whose key is the
 * group id) into mls_snapshot_rows under a header row in mls_snapshots.
 * rollback_snapshot deletes the group's current rows of those tables, copies
 * the snapshot back byte for byte and deletes the snapshot; other snapshots
 * are kept. release_snapshot deletes one (a missing one is not an error);
 * prune_expired_snapshots deletes those created (GhClock) before a time.
 * Creating a snapshot needs the group's mls_group_info row
 * (MARMOT_ERR_GROUP_NOT_FOUND otherwise) and replaces a snapshot of the same
 * name. Rolling back a missing snapshot is MARMOT_ERR_STORAGE_NOT_FOUND. Rows
 * keyed by anything else (messages, welcomes, key packages, other mls_kv
 * labels) are outside every snapshot, so a group id that happens to equal a
 * KeyPackageRef or public key can never capture or restore those.
 *
 * Semantics follow the reference backends where they agree: find functions
 * return MARMOT_OK with *out NULL when nothing matches; saves are upserts
 * (a welcome replaces the row with the same id or the same wrapper id);
 * mls_load and get_exporter_secret return MARMOT_ERR_STORAGE_NOT_FOUND when
 * missing, and so does mls_delete; deleting a missing exporter secret succeeds.
 * messages() and last_message() order newest first by the requested key
 * (created_at, then processed_at, then id; or processed_at first).
 * find_key_packages_by_pubkey() returns every package of the owner, active
 * or not (newest first), as all three reference backends do. delete_group
 * removes the group's info and relays; its MLS state, secrets, messages and
 * snapshots stay until the caller removes them (leave/forget flows).
 *
 * Privacy. Nothing is written outside the encrypted database: no files, no
 * temporary tables, no logs of values, no caches beyond one call. Returned
 * buffers are plain malloc() memory that libmarmot owns and frees.
 *
 * Errors. Every vtable function returns a MarmotError (INVALID_ARG for an
 * argument out of the bounds below, STORAGE_CONSTRAINT for a constraint
 * failure, STORAGE otherwise). The underlying GhStore error, which keeps
 * e.g. GH_STORE_ERROR_FULL distinct, is kept until
 * gh_store_marmot_take_error(). MARMOT_ERR_STORAGE_NOT_FOUND and
 * MARMOT_ERR_GROUP_NOT_FOUND are answers, not failures, and are not kept.
 *
 * Threading: like its GhStore, one thread at a time. */

/* ---- Bounds ------------------------------------------------------------------
 * At least what libmarmot itself accepts (GroupData extension: 65535-byte
 * name and description, 1000 admins, 100 relays of up to 4096 bytes), so a
 * group libmarmot joins is always storable. Beyond them a call fails with
 * MARMOT_ERR_INVALID_ARG before any SQL runs. */
#define GH_STORE_MARMOT_MAX_GROUP_ID       256
#define GH_STORE_MARMOT_MAX_LABEL          128
#define GH_STORE_MARMOT_MAX_KEY            1024
#define GH_STORE_MARMOT_MAX_VALUE          (16 * 1024 * 1024) /* mls_kv value */
#define GH_STORE_MARMOT_MAX_TEXT           65535  /* group name and description */
#define GH_STORE_MARMOT_MAX_JSON           GH_STORE_MAX_EVENT_JSON /* content, tags, events */
#define GH_STORE_MARMOT_MAX_ADMINS         1000
#define GH_STORE_MARMOT_MAX_RELAYS         100
#define GH_STORE_MARMOT_MAX_RELAY_URL      4096
#define GH_STORE_MARMOT_MAX_SNAPSHOT_NAME  256
#define GH_STORE_MARMOT_MAX_MESSAGE_ID     GH_STORE_MAX_ID /* last_message_id (hex) */
/* Failure reasons are diagnostics: longer ones are cut on a UTF-8 character
 * boundary instead of failing the record. */
#define GH_STORE_MARMOT_MAX_REASON         1024

/* A MarmotStorage over @store, for marmot_new(), which takes ownership of the
 * returned struct (marmot_free() or marmot_storage_free() destroys it). The
 * store is borrowed: it must stay open until the storage is destroyed, and
 * destroying the storage never closes it. An ephemeral store is refused
 * (GH_STORE_ERROR_STATE): MLS is disabled without durable storage (KC-4),
 * because state lost on exit would fork the group's epochs. A read-only
 * (STORE_CORRUPT) store is accepted; its writes fail. */
MarmotStorage *gh_store_marmot_new(GhStore *store, GError **error);

/* The first storage failure of @storage since the last call, as a
 * GH_STORE_ERROR (e.g. GH_STORE_ERROR_FULL when the disk filled up), or NULL.
 * Transfer full. Call it before a libmarmot operation to start clean and
 * after a failed one to learn why its storage failed. @storage must come from
 * gh_store_marmot_new(). */
GError *gh_store_marmot_take_error(MarmotStorage *storage);

G_END_DECLS
#endif
