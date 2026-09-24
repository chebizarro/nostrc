/*
 * nh_porthome_status.h — unified porthome-status.json writer.
 *
 * SPDX-License-Identifier: MIT
 *
 * The syncd, FUSE overlay and the provisioner each contribute their
 * own top-level key to a single per-user status file at
 *   $XDG_STATE_HOME/nostr-homed/porthome-status.json
 * (fallback $HOME/.local/state/nostr-homed/porthome-status.json).
 *
 * Schema (stable within v1):
 *   {
 *     "schema": 1,
 *     "syncd": { ... state|last_push_gen|last_error_class|...  },
 *     "fuse":  { ... mounted|mountpoint|generation|... },
 *     "provisioner": { ... last_provisioned_ts|last_state ... }
 *   }
 *
 * A key body is a raw pre-serialised JSON fragment (an object). The
 * merger reads the current file, replaces the caller's key, and
 * atomically renames a new tmp file into place. A lockfile scoped to
 * the destination path serialises concurrent writers so two processes
 * writing distinct keys never stomp each other. Read failures (missing
 * file, corrupted JSON) are treated as an empty document.
 *
 * The public API is intentionally tiny — the CLI reader (nostr-home-
 * status) uses jansson directly; the daemon writers do NOT link
 * jansson through this seam so a syncd contribution never leaks a
 * jansson dep into the FUSE binary or vice-versa.
 *
 * Beads: nostrc-h10m.1.
 */

#ifndef NH_PORTHOME_STATUS_H
#define NH_PORTHOME_STATUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default path resolver. Returns a heap string the caller frees on
 * success, or NULL on OOM / missing HOME. */
char *nh_porthome_status_default_path(void);

/* Directory that holds porthome-status.json AND the notifier's on-disk
 * quiet-hours config (see nh_porthome_notify.h). Ensures the parent
 * dir exists (mkdir -p, mode 0700). Returns 0 on success. */
int nh_porthome_status_ensure_dir(void);

/* Read-modify-write a single top-level key. `key_body_json` is a raw
 * JSON fragment starting with '{' and ending with '}'. NULL clears
 * the key (rare; primarily for testing).
 *
 * Locking: acquires an flock() on <path>.lock while writing.
 * Atomicity: writes to <path>.tmp.<pid> then rename(2)s into place.
 *
 * Returns 0 on success, -errno on failure. Never partially rewrites
 * the destination file.
 */
int nh_porthome_status_write_key(const char *path,
                                 const char *key,
                                 const char *key_body_json);

/* Convenience: same as nh_porthome_status_write_key with the default
 * path. Returns -ENOMEM if the default path resolver fails. */
int nh_porthome_status_write_key_default(const char *key,
                                         const char *key_body_json);

/* Read the whole file into a heap buffer (caller frees on success).
 * On missing / corrupt input the buffer is a minimal "{\"schema\":1}"
 * document. Returns 0 on success, -errno on hard I/O failure. */
int nh_porthome_status_read(const char *path,
                            char **out_body,
                            size_t *out_len);

/* Extract a single top-level key body as a heap string (raw JSON
 * fragment). Returns 0 on hit, -ENOENT if the key is absent, -EINVAL
 * on parse failure. Uses a minimal internal parser — no jansson. */
int nh_porthome_status_get_key(const char *body,
                               size_t body_len,
                               const char *key,
                               char **out_body_json);

/* JSON string escape into a caller-provided buffer. Returns the
 * number of bytes written (excluding NUL), or -1 on overflow. */
int nh_porthome_json_escape(const char *in,
                            char *out,
                            size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* NH_PORTHOME_STATUS_H */
