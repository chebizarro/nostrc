/* nd-signer.h - Signer bridge for the nostr-dav publish worker
 *
 * SPDX-License-Identifier: MIT
 *
 * Abstract interface over `org.nostr.Signer.SignEventJson` (plan Track 1
 * D1.a). Consumers hand an unsigned event JSON string; the signer returns
 * the fully signed JSON (id, pubkey, sig included). Two implementations:
 *   - Real DBus proxy against the session bus (see nd-signer-dbus.c);
 *   - Test double built from a vtable, so the publisher can be exercised
 *     against deterministic responses and error paths in unit tests
 *     without spinning a `GTestDBus`.
 */
#ifndef ND_SIGNER_H
#define ND_SIGNER_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define ND_SIGNER_ERROR (nd_signer_error_quark())
GQuark nd_signer_error_quark(void);

typedef enum {
  /* The signer explicitly rejected the request (denied approval, invalid
   * payload). Non-retryable — the publisher marks the row
   * `failed_permanent`. */
  ND_SIGNER_ERROR_DENIED = 1,
  /* Transient failure (DBus timeout, name has no owner, IO). The
   * publisher retries with backoff. */
  ND_SIGNER_ERROR_TRANSIENT,
  /* Signed JSON was malformed / missing required fields. Non-retryable. */
  ND_SIGNER_ERROR_MALFORMED
} NdSignerError;

typedef struct _NdSigner NdSigner;

typedef struct {
  /* Return the signed event JSON for @unsigned_json, or NULL with @error
   * set. Must be safe to call from a worker thread. */
  gchar *(*sign_event_json)(gpointer     user_data,
                            const gchar *unsigned_json,
                            GCancellable *cancellable,
                            GError     **error);

  /* Optional: release @user_data when the signer is finalised. */
  GDestroyNotify user_data_destroy;
} NdSignerVTable;

/**
 * nd_signer_new_from_vtable:
 * @vtable: function-pointer table; must live at least as long as the signer
 * @user_data: (transfer none): passed to every vtable call
 *
 * Returns: (transfer full): a signer that delegates to @vtable. Intended
 *   for tests; production code uses nd_signer_new_dbus().
 */
NdSigner *nd_signer_new_from_vtable(const NdSignerVTable *vtable,
                                    gpointer              user_data);

/**
 * nd_signer_new_dbus:
 * @connection: (transfer none): session bus connection; the signer
 *   builds its GDBusProxy on top of it
 * @error: (out) (optional): location for error
 *
 * Returns: (transfer full) (nullable): a signer that calls the
 *   `SignEventJson` method on `org.nostr.Signer` at `/org/nostr/signer`.
 *   NULL with @error set on proxy-build failure.
 */
NdSigner *nd_signer_new_dbus(GDBusConnection *connection, GError **error);

NdSigner *nd_signer_ref  (NdSigner *self);
void      nd_signer_unref(NdSigner *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NdSigner, nd_signer_unref)

/**
 * nd_signer_sign_event_json:
 * @self: the signer
 * @unsigned_json: unsigned event JSON (kind, content, tags, created_at
 *   set; id/pubkey/sig computed by the signer)
 * @cancellable: (nullable): cancellation token
 * @error: (out) (optional): location for error; %ND_SIGNER_ERROR domain
 *
 * Returns: (transfer full) (nullable): signed event JSON, or NULL with
 *   @error set.
 */
gchar *nd_signer_sign_event_json(NdSigner     *self,
                                 const gchar  *unsigned_json,
                                 GCancellable *cancellable,
                                 GError      **error);

G_END_DECLS
#endif /* ND_SIGNER_H */
