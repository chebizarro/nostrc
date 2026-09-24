/*
 * nh_fuse_key.h — read-once seed reader for the FUSE mount.
 *
 * SPDX-License-Identifier: MIT
 *
 * Consumes /run/nostr-auth/session/<uid>/home_seed.fuse (the second
 * broker drop; see docs/designs/nostrfs-porthome-overlay.md §4.1
 * decision D6a), unlinks it, mlocks the in-memory value, and hands
 * the caller a derived home_key on the same mlock'd page.
 *
 * Env overrides for headless / test paths:
 *   NH_FUSE_SEED_FILE  — path override
 *   NH_FUSE_SEED_HEX   — inline 64-hex fallback
 */

#ifndef NH_FUSE_KEY_H
#define NH_FUSE_KEY_H

#include "nh_porthome_crypto.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fill `out_home_key` (32 bytes) with the derived home_key.
 * Returns 0 on success, -1 on failure (no seed available). Never
 * logs the seed value. */
int nh_fuse_key_load(uint8_t out_home_key[NH_PORTHOME_KEY_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* NH_FUSE_KEY_H */
