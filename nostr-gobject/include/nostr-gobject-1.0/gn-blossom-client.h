/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GN_BLOSSOM_CLIENT_H
#define GN_BLOSSOM_CLIENT_H
#include <gio/gio.h>
G_BEGIN_DECLS

#define GN_TYPE_BLOSSOM_CLIENT (gn_blossom_client_get_type())
G_DECLARE_FINAL_TYPE(GnBlossomClient, gn_blossom_client, GN, BLOSSOM_CLIENT, GObject)

typedef enum {
  GN_BLOSSOM_NETWORK_DEFAULT = 0,
  GN_BLOSSOM_NETWORK_ALLOW_LOCAL = 1 << 0,
  GN_BLOSSOM_NETWORK_TOR = 1 << 1,
  GN_BLOSSOM_NETWORK_LOCAL_TEST = 1 << 2,
  GN_BLOSSOM_NETWORK_PRIVATE_TLS = 1 << 3
} GnBlossomNetworkFlags;

typedef enum {
  GN_BLOSSOM_ERROR_FAILED,
  GN_BLOSSOM_ERROR_AUTH,
  GN_BLOSSOM_ERROR_TOO_LARGE,
  GN_BLOSSOM_ERROR_BAD_RESPONSE
} GnBlossomError;
#define GN_BLOSSOM_ERROR (gn_blossom_error_quark())
GQuark gn_blossom_error_quark(void);

/**
 * gn_blossom_client_new:
 * @endpoint: Blossom base URL
 *
 * Create a client. Configuration and async entrypoints belong to the caller's
 * main context. Each operation snapshots configuration and owns its libblossom
 * handle on a GTask worker; no libcurl work runs on the main context.
 * Returns: (transfer full): a new client
 */
GnBlossomClient *gn_blossom_client_new(const gchar *endpoint);
/**
 * gn_blossom_client_set_network:
 * @self: a client
 * @resolver: (nullable): proxy resolver, or NULL for direct connections
 * @flags: network policy
 *
 * SOCKS destinations are resolved remotely. Only the resolver's first route is
 * used; a failed proxy never falls back to direct. Tor requires a SOCKS5 route
 * and is the only mode that permits onion hosts. ALLOW_LOCAL never enables
 * plain HTTP. LOCAL_TEST permits HTTP to loopback literals only. PRIVATE_TLS
 * disables session IDs and tickets (HTTPS requires OpenSSL-backed libcurl).
 * Changes affect future operations only: cancel active operations on revocation.
 */
void gn_blossom_client_set_network(GnBlossomClient *self, GProxyResolver *resolver,
                                    GnBlossomNetworkFlags flags);
/**
 * gn_blossom_client_set_limits:
 * @self: a client
 * @max_bytes: maximum blob bytes (0 selects libblossom's default)
 * @timeout_seconds: request timeout (0 selects 30 seconds)
 * @user_agent: (nullable): User-Agent; empty string suppresses it
 */
void gn_blossom_client_set_limits(GnBlossomClient *self, gsize max_bytes,
                                 guint timeout_seconds, const gchar *user_agent);
/**
 * gn_blossom_client_upload_async:
 * @self: a client
 * @bytes: exact wire bytes (never probed or shimmed)
 * @sha256: expected SHA-256
 * @content_type: MIME type
 * @signed_event: signed BUD-02 upload event; verified by libblossom
 * @cancellable: (nullable): cancellation
 * @callback: (scope async): completion callback
 * @user_data: (closure): callback data
 */
void gn_blossom_client_upload_async(GnBlossomClient *self, GBytes *bytes,
    const gchar *sha256, const gchar *content_type, const gchar *signed_event,
    GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);
/**
 * gn_blossom_client_upload_finish:
 * @self: a client
 * @result: async result
 * @error: return location for an error
 * Returns: (transfer full): descriptor URL, or an empty string if absent.
 *   The URL is untrusted server data; consumers decide how to use it.
 */
gchar *gn_blossom_client_upload_finish(GnBlossomClient *self, GAsyncResult *result, GError **error);
/**
 * gn_blossom_client_download_async:
 * @self: a client
 * @url: blob URL; redirects are rechecked, at most three hops
 * @sha256: expected wire SHA-256
 * @cancellable: (nullable): cancellation
 * @callback: (scope async): completion callback
 * @user_data: (closure): callback data
 *
 * Public unauthenticated GET. Limits apply during reception; success requires
 * hash verification. The endpoint is not used to reconstruct the supplied URL.
 */
void gn_blossom_client_download_async(GnBlossomClient *self, const gchar *url,
    const gchar *sha256, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);
/**
 * gn_blossom_client_download_finish:
 * @self: a client
 * @result: async result
 * @error: return location for an error
 * Returns: (transfer full): verified wire bytes
 */
GBytes *gn_blossom_client_download_finish(GnBlossomClient *self, GAsyncResult *result, GError **error);
G_END_DECLS
#endif
