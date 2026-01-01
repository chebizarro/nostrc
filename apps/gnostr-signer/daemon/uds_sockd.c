#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

#include "nostr/nip5f/nip5f.h"

// Forward declarations
int gnostr_uds_sockd_start(const char *socket_path);
void gnostr_uds_sockd_stop(void);

// Unix domain socket server for NIP-5F protocol
// Provides secure local IPC for Nostr signing operations

static void *g_sockd_handle = NULL;
static gchar *g_socket_path = NULL;

// Statistics tracking
typedef struct {
  guint64 connections_total;
  guint64 requests_total;
  guint64 errors_total;
  gint64 start_time;
} UdsStats;

static UdsStats g_uds_stats = {0};
static GMutex g_stats_mutex;

int gnostr_uds_sockd_run(const char *path) {
  return gnostr_uds_sockd_start(path);
}

int gnostr_uds_sockd_start(const char *socket_path) {
  // If already running, stop first.
  if (g_sockd_handle) {
    g_warning("uds_sockd: server already running, stopping first");
    (void)nostr_nip5f_server_stop(g_sockd_handle);
    g_sockd_handle = NULL;
  }
  
  if (!socket_path || !*socket_path) {
    g_critical("uds_sockd: socket_path is NULL or empty");
    return -1;
  }
  
  // Initialize statistics
  g_mutex_init(&g_stats_mutex);
  g_mutex_lock(&g_stats_mutex);
  memset(&g_uds_stats, 0, sizeof(g_uds_stats));
  g_uds_stats.start_time = g_get_monotonic_time();
  g_mutex_unlock(&g_stats_mutex);
  
  // Ensure parent directory exists with secure permissions
  gchar *dir = g_path_get_dirname(socket_path);
  if (g_mkdir_with_parents(dir, 0700) != 0) {
    g_critical("uds_sockd: failed to create directory %s: %s", dir, g_strerror(errno));
    g_free(dir);
    g_mutex_clear(&g_stats_mutex);
    return -1;
  }
  g_free(dir);
  
  // Remove stale socket file if it exists
  if (g_file_test(socket_path, G_FILE_TEST_EXISTS)) {
    g_message("uds_sockd: removing stale socket at %s", socket_path);
    if (g_unlink(socket_path) != 0) {
      g_warning("uds_sockd: failed to remove stale socket: %s", g_strerror(errno));
    }
  }
  
  void *h = NULL;
  if (nostr_nip5f_server_start(socket_path, &h) != 0) {
    g_critical("uds_sockd: failed to start NIP-5F server at %s", socket_path);
    g_mutex_clear(&g_stats_mutex);
    return -1;
  }
  
  // Set socket file permissions to 0600 for security
  if (chmod(socket_path, 0600) != 0) {
    g_warning("uds_sockd: failed to set socket permissions: %s", g_strerror(errno));
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
  g_socket_path = g_strdup(socket_path);
  
  g_message("uds_sockd: NIP-5F server started at %s", socket_path);
  return 0;
}

void gnostr_uds_sockd_stop(void) {
  if (g_sockd_handle) {
    // Log final statistics
    g_mutex_lock(&g_stats_mutex);
    gint64 uptime = (g_get_monotonic_time() - g_uds_stats.start_time) / 1000000;
    g_message("uds_sockd: stopping server (uptime=%" G_GINT64_FORMAT "s, "
              "total_connections=%" G_GUINT64_FORMAT ", "
              "total_requests=%" G_GUINT64_FORMAT ", "
              "total_errors=%" G_GUINT64_FORMAT ")",
              uptime,
              g_uds_stats.connections_total,
              g_uds_stats.requests_total,
              g_uds_stats.errors_total);
    g_mutex_unlock(&g_stats_mutex);
    
    (void)nostr_nip5f_server_stop(g_sockd_handle);
    g_sockd_handle = NULL;
    
    // Clean up socket file
    if (g_socket_path) {
      if (g_file_test(g_socket_path, G_FILE_TEST_EXISTS)) {
        g_message("uds_sockd: removing socket file %s", g_socket_path);
        g_unlink(g_socket_path);
      }
      g_free(g_socket_path);
      g_socket_path = NULL;
    }
    
    g_mutex_clear(&g_stats_mutex);
    g_message("uds_sockd: NIP-5F server stopped");
  }
}
