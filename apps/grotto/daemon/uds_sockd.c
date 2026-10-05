/* uds_sockd.c - NIP-5F Unix domain socket for grotto-daemon
 *
 * The listener itself is nips/nip55l's (nip55l_nip5f.h): every request is
 * decided by the same access control as the org.nostr.Signer D-Bus methods,
 * with the caller identified from the socket's kernel credentials
 * (nostrc-q23h).
 *
 * SPDX-License-Identifier: MIT
 */
#include <gio/gio.h>

#include "nip55l_nip5f.h"
#include "ipc.h"

int gnostr_uds_sockd_start(const char *socket_path, GError **error);
void gnostr_uds_sockd_stop(void);

int gnostr_uds_sockd_start(const char *socket_path, GError **error) {
  if (!socket_path || !*socket_path) {
    g_set_error_literal(error, GN_IPC_ERROR, GN_IPC_ERROR_INVALID_ENDPOINT,
                        "Socket path is NULL or empty");
    return -1;
  }
  g_autoptr(GError) err = NULL;
  if (!nip55l_nip5f_start(socket_path, &err)) {
    g_set_error(error, GN_IPC_ERROR, GN_IPC_ERROR_SOCKET_BIND,
                "Failed to start NIP-5F server at %s: %s", socket_path,
                err ? err->message : "unknown error");
    return -1;
  }
  return 0;
}

void gnostr_uds_sockd_stop(void) {
  nip55l_nip5f_stop();
}
