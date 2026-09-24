/*
 * nh_fuse_table.h — in-memory namespace table for the FUSE overlay.
 *
 * SPDX-License-Identifier: MIT
 *
 * Loads snapshot.json (via nh_syncd_state) into a sorted array of
 * entries plus a directory index derived from path prefixes. All
 * paths and caps enforced per design §7.1 (≤ 500 000 entries,
 * path ≤ 4096, component ≤ 255, depth ≤ 64, ≤ 64 chunks/entry,
 * mode masked & 0777, uid/gid overridden to mount uid/gid).
 *
 * The table is IMMUTABLE. Generation advance is handled by
 * building a new table and swapping the pointer in the mount
 * process (design §7.3).
 */

#ifndef NH_FUSE_TABLE_H
#define NH_FUSE_TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Caps (design §7.1). */
#define NH_FUSE_MAX_ENTRIES        500000u
#define NH_FUSE_MAX_PATH_BYTES     4096u
#define NH_FUSE_MAX_COMPONENT      255u
#define NH_FUSE_MAX_DEPTH          64u
#define NH_FUSE_MAX_CHUNKS         64u

typedef enum {
    NH_FUSE_KIND_FILE    = 1,
    NH_FUSE_KIND_DIR     = 2,
    NH_FUSE_KIND_SYMLINK = 3,
} nh_fuse_kind_t;

typedef struct {
    /* Owned. */
    char           *rel;               /* plaintext, no leading '/' */
    nh_fuse_kind_t  kind;
    uint32_t        mode;              /* masked & 0777 */
    uint64_t        mtime_ns;
    uint64_t        size;
    char            content_hash_hex[65]; /* "" or 64-hex + NUL */
    char          **chunks_hex;        /* array of 64-hex + NUL */
    size_t          n_chunks;
    char           *symlink_target;    /* NULL when non-symlink */
} nh_fuse_entry_t;

typedef struct nh_fuse_table nh_fuse_table;

/* Load from a state_dir (must contain snapshot.json). Returns 0 on
 * success. Enforces §7.1 caps and rejects hostile paths (absolute,
 * '..', empty component, NUL, '\\'). On failure the table is not
 * populated and *out is NULL. */
int  nh_fuse_table_load(const char *state_dir,
                        uint32_t mount_uid, uint32_t mount_gid,
                        nh_fuse_table **out);

void nh_fuse_table_free(nh_fuse_table *t);

/* Introspection. */
uint64_t nh_fuse_table_generation(const nh_fuse_table *t);
size_t   nh_fuse_table_entry_count(const nh_fuse_table *t);
uint32_t nh_fuse_table_uid(const nh_fuse_table *t);
uint32_t nh_fuse_table_gid(const nh_fuse_table *t);

/* Look up by rel (no leading '/'). Empty rel returns the root
 * (synthetic entry of kind=DIR mode=0700). */
const nh_fuse_entry_t *nh_fuse_table_find(const nh_fuse_table *t,
                                          const char *rel);

/* Enumerate children of directory `rel_dir` (empty for root).
 * `cb` receives the child name (component, not full path) and the
 * child entry. Non-zero return from `cb` aborts enumeration. */
typedef int (*nh_fuse_table_dir_cb)(void *ud,
                                    const char *child_name,
                                    const nh_fuse_entry_t *child);
int nh_fuse_table_readdir(const nh_fuse_table *t,
                          const char *rel_dir,
                          nh_fuse_table_dir_cb cb, void *ud);

#ifdef __cplusplus
}
#endif

#endif /* NH_FUSE_TABLE_H */
