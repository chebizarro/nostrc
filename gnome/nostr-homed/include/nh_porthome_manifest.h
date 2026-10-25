/* One-release forwarding header; portable API is <porthome/manifest.h>. */
#ifndef NH_PORTHOME_MANIFEST_H
#define NH_PORTHOME_MANIFEST_H
#include <porthome/compat-nh.h>

#ifdef __cplusplus
extern "C" {
#endif
/* ────────────────────────────────────────────────────────────────────
 * Rename walk (nostrc-bms6) — post-materialise convergence
 *
 * After `nh_porthome_materialize_into_fd` writes every entry under its
 * path_enc name into `staging_fd`, call this to rename each `path_enc`
 * leaf to its `name_plain` counterpart. Depth-first (children before
 * parents), openat/renameat under `staging_fd`, RENAME_EXCHANGE where
 * available. On v1 manifests (name_plain absent) returns OK with no
 * work done — path_enc names stay on disk.
 *
 * Idempotency: not partial-safe. A crash mid-walk leaves a mixed tree
 * that the caller MUST discard (nostr-homed-provision pull writes
 * into a mktemp destination and installs atomically; that discipline
 * makes the walk effectively one-shot). Collisions (rename target
 * already exists in the parent) return NH_PORTHOME_ERR_PATH — this
 * asserts the path_enc-uniqueness invariant the strict parser already
 * checks at the wire level.
 *
 * `out_renamed_count` (optional) receives the number of entries
 * renamed. `out_missed_count` (optional) receives the number of
 * entries skipped due to missing name_plain (v1-mixed). */
int nh_porthome_rename_walk(int staging_fd,
                            const nh_porthome_manifest *m,
                            size_t *out_renamed_count,
                            size_t *out_missed_count);

#ifdef __cplusplus
}
#endif
#endif
