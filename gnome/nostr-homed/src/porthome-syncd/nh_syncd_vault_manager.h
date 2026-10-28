/* SPDX-License-Identifier: MIT */
#ifndef NH_SYNCD_VAULT_MANAGER_H
#define NH_SYNCD_VAULT_MANAGER_H
#include "nh_syncd_vault_engine.h"
#include "nh_syncd_status.h"
#include "nh_porthome_notify.h"
typedef struct nh_vault_manager nh_vault_manager;
typedef struct {
    const nh_syncd_vault_config *config;
    const char *home, *state_dir, *identity;
    const char *const *relays, *const *blossom;
    size_t relays_len, blossom_len, replication;
    uint8_t home_key[32], root_key[32];
    int test_mode, legacy_known;
    nh_syncd_state *state;
    const nh_syncd_ignore *ignore;
    nh_syncd_cache *cache;
    nh_syncd_status_writer *status;
    nh_porthome_notifier *notifier;
    uint64_t retention_budget; /* Sealed payload bytes per weekly retention class. */
} nh_vault_manager_options;
int nh_vault_manager_open(const nh_vault_manager_options *, nh_vault_manager **);
/* One initial EOSE-bounded bootstrap. once disables only the subsequent live
 * subscription; it never turns a timeout/denial into a successful catch-up. */
int nh_vault_manager_bootstrap(nh_vault_manager *, int once);
int nh_vault_manager_fd(const nh_vault_manager *);
int nh_vault_manager_capture(nh_vault_manager *, const nh_syncd_batch *);
int nh_vault_manager_service(nh_vault_manager *);
int nh_vault_manager_retention(nh_vault_manager *);
/* Borrowed pins include in-flight installation/reseal views, not just frontier. */
const json_t *nh_vault_manager_gc_refs(const nh_vault_manager *);
int nh_vault_manager_gc_forget(void *, const char *hash);
void nh_vault_manager_close(nh_vault_manager *);
#endif
