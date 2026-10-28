/* SPDX-License-Identifier: MIT */
#ifndef NH_SYNCD_GC_H
#define NH_SYNCD_GC_H
#include "nh_syncd_vault_session.h"
#include "nh_syncd_vault_config.h"

/* Internal to syncd. Callers hold sync.lock throughout the pass. A dry run
 * does not write journals, caches, budget, status, or upload/delete anything. */
typedef struct {
    const char *state_dir, *pin_path, *identity, *d_tag;
    const char *const *relays, *const *servers;
    size_t relays_len, servers_len;
    const nh_syncd_vault_config *vaults;
    const json_t *extra_refs; /* Manager's active and prospective cache pins. */
    int (*forget)(void *, const char *hash); /* Invalidate in-memory PUT receipts. */
    void *forget_ctx;
    uint8_t home_key[32];
    uint64_t budget;
    int test_mode, dry_run;
} nh_syncd_gc_options;
int nh_syncd_gc_run(const nh_syncd_gc_options *, json_t **report);
/* Weekly maintenance scheduling, not relay polling. Refusals are recorded too. */
int nh_syncd_gc_due(const char *state_dir, uint64_t now);
int nh_syncd_gc_forget_receipt(nh_vault_session *, const char *hash);
/* The SAME durable weekly byte class used by prefetch and lazy reseal. */
int nh_syncd_retention_load(int dir, const uint8_t key[32], uint64_t *, uint64_t *);
int nh_syncd_retention_write(int dir, const uint8_t key[32], uint64_t, uint64_t);

/* Reachability primitive shared with focused graph tests. No storage mutation.
 * `roots` and `parents` allow the caller to refuse unknown parent closures. */
int nh_syncd_gc_mark_snapshot(nh_vault_session *, const vault_grant *, const json_t *,
                             json_t *marked, json_t *roots, json_t *parents);
#endif
