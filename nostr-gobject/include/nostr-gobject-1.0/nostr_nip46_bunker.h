/* hq-8p6sw: GObject wrapper for NIP-46 bunker (remote signer service).
 *
 * Wraps the core NIP-46 bunker API in a GObject type, replacing C function
 * pointer callbacks with GObject signals for authorize and sign requests.
 */

#pragma once

#include <glib-object.h>
#include <gio/gio.h>
#include "nostr-error.h"

G_BEGIN_DECLS

/* Forward declare opaque core type */
typedef struct NostrNip46Session NostrNip46Session;

#define GNOSTR_TYPE_NIP46_BUNKER (gnostr_nip46_bunker_get_type())
G_DECLARE_FINAL_TYPE(GNostrNip46Bunker, gnostr_nip46_bunker, GNOSTR, NIP46_BUNKER, GObject)

/**
 * gnostr_nip46_bunker_new:
 *
 * Creates a new NIP-46 bunker (remote signer) instance.
 *
 * Connect to the "authorize-request" and "sign-request" signals
 * before calling listen to handle incoming client requests.
 *
 * Returns: (transfer full): a new #GNostrNip46Bunker
 */
GNostrNip46Bunker *gnostr_nip46_bunker_new(void);

/**
 * gnostr_nip46_bunker_listen:
 * @self: a #GNostrNip46Bunker
 * @relays: (array length=n_relays): relay URLs to listen on
 * @n_relays: number of relay URLs
 * @error: (nullable): return location for a #GError
 *
 * Starts listening for incoming NIP-46 requests on the given relays.
 *
 * Returns: %TRUE on success
 */
gboolean gnostr_nip46_bunker_listen(GNostrNip46Bunker *self,
                                     const gchar *const *relays,
                                     gsize n_relays,
                                     GError **error);

/**
 * gnostr_nip46_bunker_listen_async:
 * @self: a #GNostrNip46Bunker
 * @relays: (array length=n_relays): relay URLs to listen on
 * @n_relays: number of relay URLs
 * @cancellable: (nullable): optional #GCancellable
 * @callback: (scope async): callback when complete
 * @user_data: (closure): user data for @callback
 *
 * Asynchronously starts listening for NIP-46 requests.
 */
void gnostr_nip46_bunker_listen_async(GNostrNip46Bunker *self,
                                       const gchar *const *relays,
                                       gsize n_relays,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data);

gboolean gnostr_nip46_bunker_listen_finish(GNostrNip46Bunker *self,
                                            GAsyncResult *result,
                                            GError **error);

/**
 * gnostr_nip46_bunker_issue_uri:
 * @self: a #GNostrNip46Bunker
 * @signer_pubkey_hex: the remote signer's public key hex
 * @relays: (array length=n_relays): relay URLs to include
 * @n_relays: number of relay URLs
 * @secret: (nullable): optional auth secret
 * @out_uri: (out) (transfer full): the generated bunker:// URI
 * @error: (nullable): return location for a #GError
 *
 * Generates a bunker:// URI that clients can use to connect.
 *
 * Returns: %TRUE on success
 */
gboolean gnostr_nip46_bunker_issue_uri(GNostrNip46Bunker *self,
                                        const gchar *signer_pubkey_hex,
                                        const gchar *const *relays,
                                        gsize n_relays,
                                        const gchar *secret,
                                        gchar **out_uri,
                                        GError **error);

/**
 * gnostr_nip46_bunker_handle_cipher:
 * @self: a #GNostrNip46Bunker
 * @client_pubkey_hex: the client's pubkey hex
 * @ciphertext: the encrypted request
 * @out_cipher_reply: (out) (transfer full): the encrypted response
 * @error: (nullable): return location for a #GError
 *
 * Decrypts an incoming NIP-46 request, dispatches it (triggering
 * authorize/sign signals), and returns the encrypted response.
 *
 * Returns: %TRUE on success
 */
gboolean gnostr_nip46_bunker_handle_cipher(GNostrNip46Bunker *self,
                                            const gchar *client_pubkey_hex,
                                            const gchar *ciphertext,
                                            gchar **out_cipher_reply,
                                            GError **error);

/**
 * gnostr_nip46_bunker_get_session:
 * @self: a #GNostrNip46Bunker
 *
 * Gets the underlying core NostrNip46Session pointer for advanced use.
 *
 * Returns: (transfer none) (nullable): the core session pointer
 */
NostrNip46Session *gnostr_nip46_bunker_get_session(GNostrNip46Bunker *self);

G_END_DECLS
