/**
 * GnostrSignerService - Unified Signing Service for NIP-55L and NIP-46
 *
 * Abstracts the signing mechanism so the app can use either:
 * - NIP-55L: Local signer via D-Bus (gnostr-signer)
 * - NIP-46: Remote signer via relay communication
 *
 * The service automatically uses the appropriate method based on how
 * the user authenticated.
 */

#ifndef GNOSTR_SIGNER_SERVICE_H
#define GNOSTR_SIGNER_SERVICE_H

#include <glib-object.h>
#include <gio/gio.h>
#include "nostr/nip46/nip46_client.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_SIGNER_SERVICE (gnostr_signer_service_get_type())

G_DECLARE_FINAL_TYPE(GnostrSignerService, gnostr_signer_service, GNOSTR, SIGNER_SERVICE, GObject)

/**
 * Signing method enum
 */
typedef enum {
  GNOSTR_SIGNER_METHOD_NONE,      /* Not authenticated */
  GNOSTR_SIGNER_METHOD_NIP55L,    /* Local D-Bus signer */
  GNOSTR_SIGNER_METHOD_NIP46,     /* Remote signer via NIP-46 */
} GnostrSignerMethod;

/**
 * Callback for async signing operations
 */
typedef void (*GnostrSignerCallback)(GnostrSignerService *service,
                                      const char *signed_event_json,
                                      GError *error,
                                      gpointer user_data);

/**
 * gnostr_signer_service_new:
 *
 * Creates a new signer service instance.
 *
 * Returns: (transfer full): A new #GnostrSignerService
 */
GnostrSignerService *gnostr_signer_service_new(void);

/**
 * gnostr_signer_service_get_default:
 *
 * Gets the default (global) signer service instance.
 * Creates one if it doesn't exist.
 *
 * Returns: (transfer none): The default #GnostrSignerService
 */
GnostrSignerService *gnostr_signer_service_get_default(void);

/**
 * gnostr_signer_service_set_nip46_session:
 * @self: The signer service
 * @session: (transfer full) (nullable): The NIP-46 session to use
 *
 * Sets the NIP-46 session for remote signing. Takes ownership of the session.
 * When a NIP-46 session is set, signing operations will use NIP-46.
 * Pass NULL to clear the session.
 */
void gnostr_signer_service_set_nip46_session(GnostrSignerService *self,
                                              NostrNip46Session *session);

/**
 * gnostr_signer_service_get_method:
 * @self: The signer service
 *
 * Gets the current signing method.
 *
 * Returns: The current #GnostrSignerMethod
 */
GnostrSignerMethod gnostr_signer_service_get_method(GnostrSignerService *self);

/**
 * gnostr_signer_service_is_available:
 * @self: The signer service
 *
 * Checks if any signing method is available.
 *
 * Returns: TRUE if signing is available
 */
gboolean gnostr_signer_service_is_available(GnostrSignerService *self);

/**
 * gnostr_signer_service_sign_event_async:
 * @self: The signer service
 * @event_json: The unsigned event JSON
 * @cancellable: (nullable): A #GCancellable
 * @callback: Callback for when signing completes
 * @user_data: User data for callback
 *
 * Signs an event asynchronously using the current signing method.
 */
void gnostr_signer_service_sign_event_async(GnostrSignerService *self,
                                             const char *event_json,
                                             GCancellable *cancellable,
                                             GnostrSignerCallback callback,
                                             gpointer user_data);

/**
 * gnostr_signer_service_get_pubkey:
 * @self: The signer service
 *
 * Gets the current user's public key (hex format).
 *
 * Returns: (transfer none) (nullable): The pubkey hex or NULL
 */
const char *gnostr_signer_service_get_pubkey(GnostrSignerService *self);

/**
 * gnostr_signer_service_set_pubkey:
 * @self: The signer service
 * @pubkey_hex: The user's public key in hex format
 *
 * Sets the current user's public key.
 */
void gnostr_signer_service_set_pubkey(GnostrSignerService *self,
                                       const char *pubkey_hex);

/**
 * gnostr_signer_service_clear:
 * @self: The signer service
 *
 * Clears all authentication state (for logout).
 */
void gnostr_signer_service_clear(GnostrSignerService *self);

/* ---- Convenience wrapper matching D-Bus proxy pattern ---- */

/**
 * GAsyncReadyCallback-compatible callback type for easy migration.
 * The result can be obtained with gnostr_sign_event_finish().
 */
typedef void (*GAsyncReadyCallback)(GObject *source_object,
                                     GAsyncResult *res,
                                     gpointer user_data);

/**
 * gnostr_sign_event_async:
 * @event_json: The unsigned event JSON
 * @current_user: Ignored (for API compatibility)
 * @app_id: Ignored (for API compatibility)
 * @cancellable: (nullable): A #GCancellable
 * @callback: GAsyncReadyCallback for completion
 * @user_data: User data for callback
 *
 * Signs an event using the default signer service.
 * This is a drop-in replacement for nostr_org_nostr_signer_call_sign_event()
 * that automatically uses NIP-46 or NIP-55L based on login method.
 */
void gnostr_sign_event_async(const char *event_json,
                              const char *current_user,
                              const char *app_id,
                              GCancellable *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer user_data);

/**
 * gnostr_sign_event_finish:
 * @res: The #GAsyncResult
 * @out_signed_event: (out) (transfer full): Location for signed event JSON
 * @error: (out) (optional): Location for error
 *
 * Finishes a signing operation started with gnostr_sign_event_async().
 *
 * Returns: TRUE on success
 */
gboolean gnostr_sign_event_finish(GAsyncResult *res,
                                   char **out_signed_event,
                                   GError **error);

G_END_DECLS

#endif /* GNOSTR_SIGNER_SERVICE_H */
