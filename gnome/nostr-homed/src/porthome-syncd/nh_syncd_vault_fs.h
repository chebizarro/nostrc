/* SPDX-License-Identifier: MIT — syncd/FUSE assembly, not the auth closure. */
#ifndef NH_SYNCD_VAULT_FS_H
#define NH_SYNCD_VAULT_FS_H
#include "nh_syncd.h"
#include <vault/tree.h>
#include <sys/types.h>

/* Capture a complete configured subtree relative to an already opened directory.
 * Reuse unchanged authenticated descriptors; modified files use CDC and the
 * current exact node epoch. Puts precede returned metadata; caller journals I/O.
 * Special files and concurrent changes fail rather than yielding partial trees. */
typedef struct {
    const nh_syncd_ignore *ignore;
    const char *home_prefix;
    int (*changed)(void *, const char *relative_path);
    void *changed_ctx;
} nh_syncd_vault_scan;
int nh_syncd_vault_capture(int root_fd, const vault_tree *, const nh_syncd_vault_scan *,
                           const vault_blob_io *, vault_tree **);
/* Convert the existing v1 snapshot inventory, without reading/re-uploading any
 * file blobs. Every existing directory receives home_key as an explicit override.
 * The caller seals only new manifests and publishes the v2 head afterwards. */
int nh_syncd_vault_migrate(const nh_syncd_state *, const char *prefix, const porthome_vault_info *,
                           const uint8_t root_id[32], const uint8_t home_key[32], vault_tree **);
/* Resolve plaintext or slash-joined encrypted relative paths in a closed tree. */
int nh_syncd_vault_lookup(const vault_tree *, const char *, int encrypted,
                          const porthome_node_entry **, porthome_ring_t *);
/* CDC-aware read; no HOME/tier-0 shortcut. The ring comes from the owning node.
 * Negative returns use the libvault/PCS error domain, not errno. */
int nh_syncd_vault_read(const vault_tree *, const char *, uint64_t offset, uint8_t *, size_t,
                        const vault_blob_io *, size_t *);
/* Apply a verified view at a safe relative prefix; no symlink traversal. Updates
 * the existing snapshot rows, not a second name/length inventory. Conflicts use
 * this same reader and an explicit .conflicts target supplied by the reconciler. */
int nh_syncd_vault_materialize(int home_fd, const char *prefix, const vault_tree *,
                               nh_syncd_state *, const vault_blob_io *);
/* Project a captured, acknowledged tree into existing snapshot rows without
 * writing HOME. Used before fold so new local files are tracked and their
 * losing versions can be moved out of the authoritative namespace safely. */
int nh_syncd_vault_inventory(const char *prefix, const vault_tree *, nh_syncd_state *, const vault_blob_io *);
/* Resolve authenticated encrypted components to a malloc'd plaintext path. */
int nh_syncd_vault_path_plain(const vault_tree *, const char *path_enc, char **);
/* Copy one present version (including a directory subtree) to an explicit safe
 * local destination. This does not change the source manifest or its names. */
/* Rebuild an already fsynced conflict's namespace after a pre-publication crash,
 * without rewriting HOME or trusting locally edited conflict bytes. */
int nh_syncd_vault_inventory_one(const char *, const vault_tree *, const char *, nh_syncd_state *, const vault_blob_io *);
int nh_syncd_vault_materialize_one(int home_fd, const char *destination, const vault_tree *,
                                   const char *source_path_enc, nh_syncd_state *, const vault_blob_io *);
#endif
