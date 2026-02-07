/* nostrc-8ya7: GLib async wrappers for NIP-46 client RPC.
 *
 * These wrap the synchronous NIP-46 client API with GAsyncReadyCallback
 * and GTask, suitable for use from GTK applications.
 *
 * Pattern:
 *   nostr_nip46_client_sign_event_g_async(session, json, cancel, cb, ud);
 *   // ... callback fires on GMainContext thread ...
 *   char *result = nostr_nip46_client_sign_event_g_finish(res, &error);
 */

#ifndef NOSTR_NIP46_CLIENT_G_H
#define NOSTR_NIP46_CLIENT_G_H

#include "nostr/nip46/nip46_types.h"
#include <gio/gio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sign event: async wrapper for nostr_nip46_client_sign_event().
 * Returns signed event JSON via g_task_propagate_pointer (caller frees with free()). */
void  nostr_nip46_client_sign_event_g_async(NostrNip46Session   *session,
                                             const char          *event_json,
                                             GCancellable        *cancellable,
                                             GAsyncReadyCallback  callback,
                                             gpointer             user_data);
char *nostr_nip46_client_sign_event_g_finish(GAsyncResult *result, GError **error);

/* Connect RPC: async wrapper for nostr_nip46_client_connect_rpc().
 * Returns result string ("ack") via g_task_propagate_pointer (caller frees with free()). */
void  nostr_nip46_client_connect_rpc_g_async(NostrNip46Session   *session,
                                              const char          *connect_secret,
                                              const char          *perms,
                                              GCancellable        *cancellable,
                                              GAsyncReadyCallback  callback,
                                              gpointer             user_data);
char *nostr_nip46_client_connect_rpc_g_finish(GAsyncResult *result, GError **error);

/* Get public key RPC: async wrapper for nostr_nip46_client_get_public_key_rpc().
 * Returns hex pubkey via g_task_propagate_pointer (caller frees with free()). */
void  nostr_nip46_client_get_public_key_rpc_g_async(NostrNip46Session   *session,
                                                     GCancellable        *cancellable,
                                                     GAsyncReadyCallback  callback,
                                                     gpointer             user_data);
char *nostr_nip46_client_get_public_key_rpc_g_finish(GAsyncResult *result, GError **error);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP46_CLIENT_G_H */
