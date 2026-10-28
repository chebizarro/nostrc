/* SPDX-License-Identifier: MIT */
#ifndef NH_SYNCD_VAULT_ENGINE_H
#define NH_SYNCD_VAULT_ENGINE_H
#include "nh_syncd_vault_config.h"
#include "nh_syncd_vault_session.h"
#include "nh_syncd_conflicts.h"

typedef struct nh_vault_engine nh_vault_engine;
typedef int (*nh_vault_publish_fn)(void *, NostrEvent *);
int nh_vault_engine_open(const nh_syncd_vault_config_entry *, nh_vault_session *, int home_fd,
                         nh_syncd_state *, const nh_syncd_ignore *, const char *identity_hex,
                         nh_vault_engine **);
/* Prepare an owner bootstrap from the v1 inventory (no chunk I/O), or capture a
 * new subtree when no v1 base exists. Only manifests are uploaded for migration. */
int nh_vault_engine_owner_init(nh_vault_engine *, int legacy_known, const uint8_t root_key[32]);
/* Never capture an old policy/key over an unseen owner rotation from another
 * client. The subsequent normal admission/fold preserves or refuses dirty HOME. */
int nh_vault_engine_can_capture(const nh_vault_engine *, NostrEvent *const *, size_t);
int nh_vault_engine_observe_sequence(nh_vault_engine *, NostrEvent *const *, size_t);
int nh_vault_engine_events(nh_vault_engine *, NostrEvent *const *, size_t);
int nh_vault_engine_publish_pending(nh_vault_engine *, nh_vault_publish_fn, void *);
/* Called BEFORE admitting remote events, so pending local watcher edits cannot
 * be overwritten by an incoming authoritative view. Never captures before a
 * verified initial view has been installed. */
int nh_vault_engine_capture(nh_vault_engine *, const nh_syncd_batch *, nh_vault_publish_fn, void *);
int nh_vault_engine_fold_apply(nh_vault_engine *);
int nh_vault_engine_checkpoint(nh_vault_engine *, nh_vault_publish_fn, void *);
int nh_vault_engine_retention(nh_vault_engine *, uint64_t budget, nh_vault_publish_fn, void *,
                              uint64_t *reserved);
typedef int (*nh_vault_prefetch_fn)(void *, const uint8_t hash[32], uint64_t sealed_len,
                                    const uint8_t *known_manifest);
/* Enumerate only closed verified graphs selected by checkpoint fold and durable
 * conflict references. known_manifest is exact reconstructed sealed metadata;
 * chunk bodies are fetched separately through the caller's bounded cache I/O. */
int nh_vault_engine_prefetch(nh_vault_engine *, nh_vault_prefetch_fn, void *, int check_chunks);
const uint8_t *nh_vault_engine_writer(const nh_vault_engine *);
const vault_grant *nh_vault_engine_grant(const nh_vault_engine *);
const porthome_vault_info *nh_vault_engine_policy(const nh_vault_engine *);
const vault_tree *nh_vault_engine_view(const nh_vault_engine *);
const json_t *nh_vault_engine_conflicts(const nh_vault_engine *);
/* Conflict resolution (nostrc-h10m.6). Returns 0 once the resolution head is accepted
 * by a relay and the row is resolved (its saved copy removed), the positive
 * NH_CONFLICT_QUEUED when the head is signed and journalled but publication
 * failed (the row commits on a later successful publish), or a negative
 * error with *why set to a status slug ("not-found", "already-resolved",
 * "busy", "unsupported-choice", "stale", "offline", "io"). Read-only
 * members may only keep-current, which dismisses locally without a head.
 * Re-sending the same request id resumes rather than repeats. */
int nh_vault_engine_resolve(nh_vault_engine *, const char *conflict_id, nh_conflict_choice,
                            const char *expect, const char *request_id, nh_vault_publish_fn,
                            void *, const char **why);
int nh_vault_engine_has_conflict(const nh_vault_engine *, const char *conflict_id);
/* Append unresolved rows (allowlisted fields + id/choices/mine/expect) to `out`. */
int nh_vault_engine_project_conflicts(const nh_vault_engine *, json_t *out);
void nh_vault_engine_close(nh_vault_engine *);
#endif
