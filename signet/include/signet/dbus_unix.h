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

/**
 * SignetDbusServer:
 * Opaque D-Bus Unix transport server.
 *
 * Since: 1.0
 */
typedef struct SignetDbusServer SignetDbusServer;

struct SignetKeyStore;
struct SignetPolicyRegistry;
struct SignetStore;
struct SignetAuditLogger;
struct SignetFidoService;

/* UID → agent_id mapping callback.
 * Returns agent_id string (caller frees with g_free), or NULL if unknown. */
/**
 * SignetUidResolver:
 * @uid: uid
 * @user_data: (not nullable): user data
 *
 * Callback that maps a Unix uid to an agent identifier.
 *
 * Returns: (transfer full) (nullable): resolved agent identifier, or %NULL if unknown
 *
 * Since: 1.0
 */
typedef char *(*SignetUidResolver)(uid_t uid, void *user_data);

/**
 * SignetDbusServerConfig:
 * @keys: borrowed key-store dependency.
 * @policy: borrowed policy dependency.
 * @store: borrowed store dependency.
 * @audit: borrowed audit logger dependency.
 * @fido: borrowed FIDO service dependency.
 * @uid_resolver: uid resolver value.
 * @uid_resolver_data: uid resolver data value.
 * @use_system_bus: true = system bus, false = session bus.
 *
 * Configuration for the D-Bus Unix transport.
 *
 * Since: 1.0
 */
typedef struct {
  struct SignetKeyStore *keys;
  struct SignetPolicyRegistry *policy;
  struct SignetStore *store;
  struct SignetAuditLogger *audit;
  struct SignetFidoService *fido;
  SignetUidResolver uid_resolver;
  void *uid_resolver_data;
  bool use_system_bus;  /* true = system bus, false = session bus */
} SignetDbusServerConfig;

/* Create D-Bus server. Does not connect yet. Returns NULL on OOM. */
/**
 * signet_dbus_server_new:
 * @cfg: (nullable): configuration to use
 *
 * Create D-Bus server. Does not connect yet. Returns NULL on OOM.
 *
 * Returns: (transfer full) (nullable): a newly allocated object, or %NULL on failure
 *
 * Since: 1.0
 */
SignetDbusServer *signet_dbus_server_new(const SignetDbusServerConfig *cfg);

/* Free D-Bus server. Disconnects if connected. Safe on NULL. */
/**
 * signet_dbus_server_free:
 * @ds: (nullable): a D-Bus server
 *
 * Free D-Bus server. Disconnects if connected. Safe on NULL.
 *
 * Since: 1.0
 */
void signet_dbus_server_free(SignetDbusServer *ds);

/* Connect to D-Bus and register interfaces.
 * Returns 0 on success, -1 on failure. */
/**
 * signet_dbus_server_start:
 * @ds: (not nullable): a D-Bus server
 *
 * Connect to D-Bus and register interfaces. Returns 0 on success, -1 on failure.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_dbus_server_start(SignetDbusServer *ds);

/* Disconnect from D-Bus. Safe to call multiple times. */
/**
 * signet_dbus_server_stop:
 * @ds: (nullable): a D-Bus server
 *
 * Disconnect from D-Bus. Safe to call multiple times.
 *
 * Since: 1.0
 */
void signet_dbus_server_stop(SignetDbusServer *ds);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_DBUS_UNIX_H */
