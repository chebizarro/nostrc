/* SPDX-License-Identifier: MIT */
#ifndef NH_SYNCD_VAULT_SESSION_H
#define NH_SYNCD_VAULT_SESSION_H
#include "nh_syncd_vault_fs.h"
#include <jansson.h>
#include <nostr-event.h>
#include "nh_syncd_cache.h"

typedef struct {
    int dir, objects;
    uint8_t home_key[32];
    char remote_scope[65]; /* Hash of endpoints/quorum; binds durable PUT receipts. */
    json_t *doc;           /* Private encrypted journal; never a second name/length index. */
    vault_blob_io remote, io;
    int (*reconcile)(void *, const uint8_t hash[32], const uint8_t *, size_t);
    vault_ring held_scratch; /* Serialized fetch callback view; wiped between lookups. */
    nh_syncd_cache *cache;   /* Existing sealed-blob cache, borrowed. */
} nh_vault_session;
int nh_vault_session_open(const char *state_dir, const uint8_t vault_id[32],
                          const uint8_t home_key[32], nh_vault_session **);
/* Read-only existing journal. Does not create state on a FUSE cache miss. */
int nh_vault_session_load(const char *, const uint8_t[32], const uint8_t[32], nh_vault_session **);
int nh_vault_session_save(nh_vault_session *);
void nh_vault_session_close(nh_vault_session *);
int nh_vault_file_read(int dir, const char *name, uint8_t **, size_t *);
int nh_vault_file_write(int dir, const char *name, const uint8_t *, size_t);
/* Snapshot carries only a signed head, its explicit epoch and exact node rings.
 * All names and lengths are reopened from addressed authenticated manifests. */
int nh_vault_snapshot_new(const NostrEvent *, const vault_tree *, json_t **);
int nh_vault_snapshot_open(nh_vault_session *, const vault_grant *, const json_t *, vault_record **,
                           vault_tree **);
int nh_vault_session_held(void *, const uint8_t[32], uint64_t, porthome_ring_t *);
int nh_vault_snapshot_held(void *, const uint8_t id[32], uint64_t epoch, porthome_ring_t *);
void nh_vault_secret_json_free(json_t *);
#endif
