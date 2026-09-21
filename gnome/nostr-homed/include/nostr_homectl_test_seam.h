/* nostr_homectl_test_seam.h -- internal test seam for nostr-homectl.c
 *
 * Focused roaming tests link against nostr-homectl.c and use these hook
 * function pointers to inject failures at the boundaries of nh_warm_cache and
 * nh_open_session. Do NOT use these hooks from production code paths.
 *
 * Ownership: strictly internal to nostr-homed. Not installed. Not stable API.
 *
 * Introduced for beads nostrc-nxpb.7 (relay UAF) and nostrc-nxpb.8
 * (OpenSession false-success) so each failure site can be exercised.
 */
#ifndef NOSTR_HOMECTL_TEST_SEAM_H
#define NOSTR_HOMECTL_TEST_SEAM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return 0 on success and set *out_npub to a g_strdup'd bech32 npub. */
extern int (*nh_hook_dbus_get_signer_npub)(char **out_npub);

/* Return non-zero iff `path` is on a distinct filesystem than its parent. */
extern int (*nh_hook_is_mountpoint)(const char *path);

/* Ensure directory `path` exists with mode. Return 0 on success, -1 on
 * failure (mirrors g_mkdir_with_parents semantics). */
extern int (*nh_hook_mkdir_p)(const char *path, int mode);

/* Run `systemctl start <unit>` and wait for completion. Return 0 iff
 * systemctl spawned, waited cleanly, and exited with status 0. */
extern int (*nh_hook_systemctl_start)(const char *unit);

/* Run `systemctl stop <unit>` and wait for completion. Return 0 iff
 * systemctl spawned, waited cleanly, and exited with status 0. */
extern int (*nh_hook_systemctl_stop)(const char *unit);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_HOMECTL_TEST_SEAM_H */
