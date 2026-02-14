/* ipc.h - IPC server interface for gnostr-signer-daemon
 *
 * Provides cross-platform IPC for Nostr signing operations:
 * - Unix domain sockets (POSIX)
 * - TCP with token authentication (when enabled)
 * - Named pipes (Windows)
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef APPS_GNOSTR_SIGNER_DAEMON_IPC_H
#define APPS_GNOSTR_SIGNER_DAEMON_IPC_H

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

G_BEGIN_DECLS

/* ============================================================================
 * Error Domain
 * ============================================================================ */

/**
 * GnIpcError:
 * @GN_IPC_ERROR_FAILED: General failure
 * @GN_IPC_ERROR_INVALID_ENDPOINT: Invalid or malformed endpoint string
 * @GN_IPC_ERROR_SOCKET_CREATE: Failed to create socket
 * @GN_IPC_ERROR_SOCKET_BIND: Failed to bind socket
 * @GN_IPC_ERROR_SOCKET_LISTEN: Failed to listen on socket
 * @GN_IPC_ERROR_DIRECTORY_CREATE: Failed to create parent directory
 * @GN_IPC_ERROR_PERMISSION: Permission denied
 * @GN_IPC_ERROR_PLATFORM_UNSUPPORTED: Endpoint type not supported on this platform
 * @GN_IPC_ERROR_TOKEN_WRITE: Failed to write authentication token file
 * @GN_IPC_ERROR_THREAD_CREATE: Failed to create accept thread
 * @GN_IPC_ERROR_CONNECTION: Connection error
 * @GN_IPC_ERROR_AUTH: Authentication failed
 * @GN_IPC_ERROR_PROTOCOL: Protocol error (malformed frame, etc.)
 *
 * Error codes for IPC operations.
 */
typedef enum {
  GN_IPC_ERROR_FAILED,
  GN_IPC_ERROR_INVALID_ENDPOINT,
  GN_IPC_ERROR_SOCKET_CREATE,
  GN_IPC_ERROR_SOCKET_BIND,
  GN_IPC_ERROR_SOCKET_LISTEN,
  GN_IPC_ERROR_DIRECTORY_CREATE,
  GN_IPC_ERROR_PERMISSION,
  GN_IPC_ERROR_PLATFORM_UNSUPPORTED,
  GN_IPC_ERROR_TOKEN_WRITE,
  GN_IPC_ERROR_THREAD_CREATE,
  GN_IPC_ERROR_CONNECTION,
  GN_IPC_ERROR_AUTH,
  GN_IPC_ERROR_PROTOCOL
} GnIpcError;

#define GN_IPC_ERROR (gn_ipc_error_quark())
GQuark gn_ipc_error_quark(void);

/* ============================================================================
 * IPC Server
 * ============================================================================ */

typedef struct GnostrIpcServer GnostrIpcServer;

/**
 * gnostr_ipc_server_start:
 * @endpoint: (nullable): IPC endpoint string, or NULL for platform default
 *   - unix:/path/to/socket
 *   - tcp:host:port
 *   - npipe:\\.\pipe\name (Windows)
 * @error: (out) (optional): Return location for a GError
 *
 * Start the IPC server at the specified endpoint.
 *
 * Returns: (transfer full): A new GnostrIpcServer, or NULL on error
 */
GnostrIpcServer* gnostr_ipc_server_start(const char *endpoint, GError **error);

/**
 * gnostr_ipc_server_stop:
 * @srv: (transfer full): The server to stop
 *
 * Stop the IPC server and release all resources.
 */
void gnostr_ipc_server_stop(GnostrIpcServer *srv);

G_END_DECLS

#ifdef __cplusplus
}
#endif
#endif /* APPS_GNOSTR_SIGNER_DAEMON_IPC_H */
