/* SPDX-License-Identifier: MIT — private CLI assembly, not installed. */
#ifndef VAULTCTL_VC_H
#define VAULTCTL_VC_H
#include <vault/tree.h>
#include <vault/nip59.h>
#include <nostr-event.h>
#include <json.h>
#include <go.h>
#include <error.h>
#include <blossom/client.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#define VC_MAX_ENDPOINTS 8u
#define VC_MAX_STATE (64u * 1024u * 1024u)
#define VC_PHASE_NONE 0
#define VC_PHASE_SHARE 1
#define VC_PHASE_GRANT 2
#define VC_PHASE_REVOKE 3
#define VC_EXIT_INCOMPLETE 2
#define VC_EXIT_CONFLICT 3
#define VC_EXIT_REMAINING 4
#define VC_EXIT_UNCERTAIN 5

typedef struct {
    NostrEvent *event;
    uint64_t epoch;
} vc_head;
typedef struct {
    int dirfd, lockfd, objectsfd, acksfd, ringsfd;
    vault_tree *tree;
    vc_head heads[VAULT_MAX_HEADS];
    size_t heads_len, current;
    char *relays[VC_MAX_ENDPOINTS], *blossom[VC_MAX_ENDPOINTS];
    size_t relays_len, blossom_len;
    unsigned phase;
    uint8_t member[32];
    unsigned cap, history, eager, migrating, rotation, distribution, needs_publication;
    size_t delivery_cursor;
    NostrEvent *outgoing;
    uint64_t outgoing_epoch;
    uint64_t ack, intent, rejected;
    int pending_blob;
    uint8_t blob_hash[32];
    unsigned blob_server;
    char last_grant[65];
} vc_state;
typedef struct {
    const char *command, *state_path, *key_file, *source, *member_hex;
    const char *relays[VC_MAX_ENDPOINTS], *blossom[VC_MAX_ENDPOINTS];
    size_t relays_len, blossom_len;
    uint64_t budget, deadline_ms;
    unsigned cap;
    int eager, resume, history, allow_local, reconcile;
} vc_options;
typedef struct {
    vc_options options;
    vc_state state;
    uint8_t sk[32], pub[32];
    char secret[65], public_key[65];
    blossom_client_t *clients[VC_MAX_ENDPOINTS];
    blossom_signer_t signer;
    int local_put, chunk_io;
    uint8_t (*verified)[32];
    size_t verified_len, verified_cap;
    vault_ring resolved_ring;
    char error[256];
} vc_app;

void vc_error(vc_app *, const char *, ...);
int vc_state_open(vc_app *, int create);
int vc_state_load(vc_app *);
int vc_state_save(vc_app *);
void vc_state_free(vc_state *);
int vc_identity_load(vc_app *);
int vc_private_read(int dirfd, const char *, size_t limit, uint8_t **, size_t *);
int vc_private_write(int dirfd, const char *, const uint8_t *, size_t);
int vc_cache_put(vc_app *, const uint8_t hash[32], const uint8_t *, size_t);
int vc_cache_get(vc_app *, const uint8_t hash[32], uint8_t **, size_t *);
int vc_ack_get(vc_app *, const uint8_t hash[32], uint64_t *);
int vc_ack_put(vc_app *, const uint8_t hash[32], uint64_t);
int vc_head_add(vc_app *, const NostrEvent *, uint64_t epoch, size_t *index);
int vc_set_outgoing(vc_app *, const NostrEvent *, uint64_t epoch);
int vc_held_ring(void *, const uint8_t id[32], uint64_t epoch, porthome_ring_t *);
int vc_record_load(vc_app *, const NostrEvent *, uint64_t epoch, vault_record **, vault_tree **);
int vc_collect_records(vc_app *, vault_record ***, size_t *, vault_fold_result **);
void vc_records_free(vault_record **, size_t);
int vc_import(vc_app *, const char *path, vault_tree **);
int vc_blobs_open(vc_app *);
void vc_blobs_close(vc_app *);
int vc_blob_read(void *, const uint8_t hash[32], uint8_t **, size_t *);
int vc_blob_put(void *, const uint8_t *, size_t, const uint8_t hash[32]);
int vc_reconcile_blob(vc_app *);
int vc_publish(vc_app *);
int vc_query_heads(vc_app *, const porthome_vault_info *);
int vc_query_id(vc_app *, const NostrEvent *, uint64_t *found_mask);
int vc_command_share(vc_app *);
int vc_command_grant(vc_app *);
int vc_command_revoke(vc_app *);
int vc_command_fold(vc_app *);
int vc_command_fsck(vc_app *);
#endif
