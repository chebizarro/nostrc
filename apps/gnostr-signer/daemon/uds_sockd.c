#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>

#include "nostr/nip5f/nip5f.h"

// Forward declarations
int gnostr_uds_sockd_start(const char *socket_path);
void gnostr_uds_sockd_stop(void);

// Minimal Unix domain socket stub for NIP-5F compatibility during scaffolding.
// This will be replaced with the real socket server integrating JSON-RPC.

// Simple wrapper around nostr_nip5f_server_* to run a UDS signer per NIP-5F.
// We use builtin handlers that delegate to libnostr primitives. Secrets are
// resolved the same way as the DBus path (env/libsecret).

static void *g_sockd_handle = NULL;

int gnostr_uds_sockd_run(const char *path) {
  return gnostr_uds_sockd_start(path);
}

int gnostr_uds_sockd_start(const char *socket_path) {
  // If already running, stop first.
  if (g_sockd_handle) {
    (void)nostr_nip5f_server_stop(g_sockd_handle);
    g_sockd_handle = NULL;
  }
  void *h = NULL;
  if (nostr_nip5f_server_start(socket_path, &h) != 0) {
    g_warning("uds_sockd: failed to start NIP-5F server");
    return -1;
  }
  // Use builtin handlers; no custom ACL at UDS layer (DBus path handles approvals).
  (void)nostr_nip5f_server_set_handlers(h,
    /*get_pub*/ NULL,
    /*sign_event*/ NULL,
    /*enc44*/ NULL,
    /*dec44*/ NULL,
    /*list_keys*/ NULL,
    /*ud*/ NULL);
  g_sockd_handle = h;
  g_message("uds_sockd: NIP-5F server started");
  return 0;
}

void gnostr_uds_sockd_stop(void) {
  if (g_sockd_handle) {
    (void)nostr_nip5f_server_stop(g_sockd_handle);
    g_sockd_handle = NULL;
    g_message("uds_sockd: NIP-5F server stopped");
  }
}
