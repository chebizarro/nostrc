/* hq-8p6sw: GObject wrapper for NIP-46 remote signer client.
 *
 * Wraps the core NostrNip46Session client API in a GObject type with
 * properties, signals, and GTask-based async methods for GTK integration.
 */
#ifndef NOSTR_GOBJECT_NOSTR_NIP46_CLIENT_H
#define NOSTR_GOBJECT_NOSTR_NIP46_CLIENT_H

#include <glib-object.h>
#include <gio/gio.h>
#include "nostr-enums.h"
#include "nostr-error.h"

G_BEGIN_DECLS

/* Forward declare opaque core type */
typedef struct NostrNip46Session NostrNip46Session;

#define GNOSTR_TYPE_NIP46_CLIENT (gnostr_nip46_client_get_type())
G_DECLARE_FINAL_TYPE(GNostrNip46Client, gnostr_nip46_client, GNOSTR, NIP46_CLIENT, GObject)

/**
 * gnostr_nip46_client_new:
 *
 * Creates a new NIP-46 client instance.
 *
 * Returns: (transfer full): a new #GNostrNip46Client
 */
GNostrNip46Client *gnostr_nip46_client_new(void);

/**
 * gnostr_nip46_client_connect_to_bunker:
 * @self: a #GNostrNip46Client
 * @bunker_uri: bunker:// or nostrconnect:// URI
 * @perms: (nullable): requested permissions CSV
 * @error: (nullable): return location for a #GError
 *
 * Parses the bunker URI and configures the session. Does not start
 * the relay connection — call gnostr_nip46_client_start() after this.
 *
 * Returns: %TRUE on success
 */
gboolean gnostr_nip46_client_connect_to_bunker(GNostrNip46Client *self,
                                                const gchar *bunker_uri,
                                                const gchar *perms,
                                                GError **error);

/**
 * gnostr_nip46_client_start:
 * @self: a #GNostrNip46Client
 * @error: (nullable): return location for a #GError
 *
 * Starts the persistent relay connection pool. Must be called after
 * connect_to_bunker(). Transitions state to CONNECTING then CONNECTED.
 *
 * Returns: %TRUE on success
 */
gboolean gnostr_nip46_client_start(GNostrNip46Client *self,
                                    GError **error);

/**
 * gnostr_nip46_client_start_async:
 * @self: a #GNostrNip46Client
 * @cancellable: (nullable): optional #GCancellable
 * @callback: (scope async): callback when complete
 * @user_data: (closure): user data for @callback
 *
 * Asynchronously starts the persistent relay connection.
 */
void gnostr_nip46_client_start_async(GNostrNip46Client *self,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data);

gboolean gnostr_nip46_client_start_finish(GNostrNip46Client *self,
                                           GAsyncResult *result,
                                           GError **error);

/**
 * gnostr_nip46_client_stop:
 * @self: a #GNostrNip46Client
 *
 * Stops the persistent relay connection. Safe to call multiple times.
 */
void gnostr_nip46_client_stop(GNostrNip46Client *self);

/* ── RPC Methods (synchronous) ─────────────────────────────────── */

gboolean gnostr_nip46_client_connect_rpc(GNostrNip46Client *self,
                                          const gchar *connect_secret,
                                          const gchar *perms,
                                          gchar **out_result,
                                          GError **error);

gboolean gnostr_nip46_client_get_public_key_rpc(GNostrNip46Client *self,
                                                  gchar **out_pubkey_hex,
                                                  GError **error);

gboolean gnostr_nip46_client_sign_event(GNostrNip46Client *self,
                                         const gchar *event_json,
                                         gchar **out_signed_json,
                                         GError **error);

gboolean gnostr_nip46_client_ping(GNostrNip46Client *self,
                                   GError **error);

gboolean gnostr_nip46_client_nip04_encrypt(GNostrNip46Client *self,
                                            const gchar *peer_pubkey_hex,
                                            const gchar *plaintext,
                                            gchar **out_ciphertext,
                                            GError **error);

gboolean gnostr_nip46_client_nip04_decrypt(GNostrNip46Client *self,
                                            const gchar *peer_pubkey_hex,
                                            const gchar *ciphertext,
                                            gchar **out_plaintext,
                                            GError **error);

gboolean gnostr_nip46_client_nip44_encrypt(GNostrNip46Client *self,
                                            const gchar *peer_pubkey_hex,
                                            const gchar *plaintext,
                                            gchar **out_ciphertext,
                                            GError **error);

gboolean gnostr_nip46_client_nip44_decrypt(GNostrNip46Client *self,
                                            const gchar *peer_pubkey_hex,
                                            const gchar *ciphertext,
                                            gchar **out_plaintext,
                                            GError **error);

/* ── RPC Methods (async with GTask) ───────────────────────────── */

void gnostr_nip46_client_connect_rpc_async(GNostrNip46Client *self,
                                            const gchar *connect_secret,
                                            const gchar *perms,
                                            GCancellable *cancellable,
                                            GAsyncReadyCallback callback,
                                            gpointer user_data);
gchar *gnostr_nip46_client_connect_rpc_finish(GNostrNip46Client *self,
                                               GAsyncResult *result,
                                               GError **error);

void gnostr_nip46_client_get_public_key_rpc_async(GNostrNip46Client *self,
                                                    GCancellable *cancellable,
                                                    GAsyncReadyCallback callback,
                                                    gpointer user_data);
gchar *gnostr_nip46_client_get_public_key_rpc_finish(GNostrNip46Client *self,
                                                       GAsyncResult *result,
                                                       GError **error);

void gnostr_nip46_client_sign_event_async(GNostrNip46Client *self,
                                           const gchar *event_json,
                                           GCancellable *cancellable,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data);
gchar *gnostr_nip46_client_sign_event_finish(GNostrNip46Client *self,
                                              GAsyncResult *result,
                                              GError **error);

/* ── Configuration ─────────────────────────────────────────────── */

void     gnostr_nip46_client_set_timeout(GNostrNip46Client *self, guint timeout_ms);
guint    gnostr_nip46_client_get_timeout(GNostrNip46Client *self);

/* ── Property Accessors ───────────────────────────────────────── */

GNostrNip46State   gnostr_nip46_client_get_state(GNostrNip46Client *self);
const gchar       *gnostr_nip46_client_get_bunker_uri(GNostrNip46Client *self);
const gchar       *gnostr_nip46_client_get_remote_pubkey(GNostrNip46Client *self);

/**
 * gnostr_nip46_client_get_session:
 * @self: a #GNostrNip46Client
 *
 * Gets the underlying core NostrNip46Session pointer for advanced use.
 *
 * Returns: (transfer none) (nullable): the core session pointer
 */
NostrNip46Session *gnostr_nip46_client_get_session(GNostrNip46Client *self);

G_END_DECLS
#endif /* NOSTR_GOBJECT_NOSTR_NIP46_CLIENT_H */
