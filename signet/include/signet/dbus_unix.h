/* SPDX-License-Identifier: MIT
 *
 * dbus_unix.h - D-Bus Unix transport for Signet v2.
 *
 * Registers net.signet.Signer and net.signet.Credentials on the system
 * D-Bus. Auth via SO_PEERCRED UID → agent_id mapping. All method calls
 * are checked against the capability engine before dispatch.
 *
 * Extends the NIP-55L org.nostr.Signer interface with Signet-specific
 * credential operations. The underlying signing ops delegate to
 * SignetKeyStore rather than nip55l's libsecret-backed store.
 */

#ifndef SIGNET_DBUS_UNIX_H
#define SIGNET_DBUS_UNIX_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct SignetDbusServer SignetDbusServer;

struct SignetKeyStore;
struct SignetPolicyRegistry;
struct SignetStore;
struct SignetAuditLogger;

/* UID → agent_id mapping callback.
 * Returns agent_id string (caller frees with g_free), or NULL if unknown. */
typedef char *(*SignetUidResolver)(uid_t uid, void *user_data);

typedef struct {
  struct SignetKeyStore *keys;
  struct SignetPolicyRegistry *policy;
  struct SignetStore *store;
  struct SignetAuditLogger *audit;
  SignetUidResolver uid_resolver;
  void *uid_resolver_data;
  bool use_system_bus;  /* true = system bus, false = session bus */
} SignetDbusServerConfig;

/* Create D-Bus server. Does not connect yet. Returns NULL on OOM. */
SignetDbusServer *signet_dbus_server_new(const SignetDbusServerConfig *cfg);

/* Free D-Bus server. Disconnects if connected. Safe on NULL. */
void signet_dbus_server_free(SignetDbusServer *ds);

/* Connect to D-Bus and register interfaces.
 * Returns 0 on success, -1 on failure. */
int signet_dbus_server_start(SignetDbusServer *ds);

/* Disconnect from D-Bus. Safe to call multiple times. */
void signet_dbus_server_stop(SignetDbusServer *ds);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_DBUS_UNIX_H */
