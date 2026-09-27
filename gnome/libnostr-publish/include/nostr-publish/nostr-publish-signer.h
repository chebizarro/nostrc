/* nostr-publish-signer.h - Signer abstraction for libnostr-publish
 *
 * SPDX-License-Identifier: MIT
 *
 * Abstract interface over `org.nostr.Signer.SignEvent (sss)->s` (and,
 * optionally, `NIP44Encrypt (sss)->s`, for NIP-59 seals: nostrc-k95e).
 * Consumers hand an unsigned event JSON string; the signer returns the
 * fully signed JSON (id, pubkey, sig included). The secret key never
 * leaves the signer daemon. Two implementations:
 *   - the real D-Bus proxy against the session bus
 *     (nostr_publish_signer_new_dbus());
 *   - a vtable-backed double so publishers can be exercised against
 *     deterministic responses and error paths without a GTestDBus.
 *
 * Originally nostr-dav's NdSigner (nd-signer.h); nostr-dav keeps those
 * names as aliases.
 */
#ifndef NOSTR_PUBLISH_SIGNER_H
#define NOSTR_PUBLISH_SIGNER_H

#include <glib.h>
#include <gio/gio.h>

#include "nostr-publish-macros.h"

G_BEGIN_DECLS

#define NOSTR_PUBLISH_SIGNER_ERROR (nostr_publish_signer_error_quark())
NOSTR_PUBLISH_API GQuark nostr_publish_signer_error_quark(void);

typedef enum {
  /* The signer explicitly rejected the request (denied approval, invalid
   * payload). Non-retryable. */
  NOSTR_PUBLISH_SIGNER_ERROR_DENIED = 1,
  /* Transient failure (D-Bus timeout, name has no owner, IO). Retry with
   * backoff. */
  NOSTR_PUBLISH_SIGNER_ERROR_TRANSIENT,
  /* Signed JSON was malformed / missing required fields. Non-retryable. */
  NOSTR_PUBLISH_SIGNER_ERROR_MALFORMED
} NostrPublishSignerError;

typedef struct _NostrPublishSigner NostrPublishSigner;

typedef struct {
  /* Return the signed event JSON for @unsigned_json, or NULL with @error
   * set (preferably in the NOSTR_PUBLISH_SIGNER_ERROR domain). */
  gchar *(*sign_event_json)(gpointer      user_data,
                            const gchar  *unsigned_json,
                            GCancellable *cancellable,
                            GError      **error);

  /* Optional: release @user_data when the signer is finalised. */
  GDestroyNotify user_data_destroy;
} NostrPublishSignerVTable;

/* NIP-44 v2 encrypt @plaintext from the signer's key to @peer_pubkey_hex
 * (64-hex x-only); returns the base64 payload, or NULL with @error set.
 * Receives the vtable's @user_data. */
typedef gchar *(*NostrPublishSignerNip44EncryptFunc)(gpointer      user_data,
                                                     const gchar  *plaintext,
                                                     const gchar  *peer_pubkey_hex,
                                                     GCancellable *cancellable,
                                                     GError      **error);

/**
 * nostr_publish_signer_new_from_vtable:
 * @vtable: function-pointer table; copied
 * @user_data: passed to every vtable call; released through
 *   @vtable->user_data_destroy when the last ref drops
 *
 * Returns: (transfer full): a signer that delegates to @vtable.
 */
NOSTR_PUBLISH_API
NostrPublishSigner *nostr_publish_signer_new_from_vtable(
    const NostrPublishSignerVTable *vtable,
    gpointer                        user_data);

/**
 * nostr_publish_signer_new_dbus:
 * @connection: (transfer none): session bus connection
 * @app_id: (not nullable): caller identity passed as the third SignEvent
 *   argument; the signer daemon shows it in approval prompts and keys its
 *   ACL on it (e.g. "nostr-dav", "nostr-share")
 * @error: (out) (optional): location for error
 *
 * Returns: (transfer full) (nullable): a signer that calls
 *   `org.nostr.Signer.SignEvent(eventJson, identity="", app_id)` on
 *   `/org/nostr/signer` and validates the reply carries string `id`,
 *   `pubkey` and `sig` members. NULL with @error set on proxy failure.
 */
NOSTR_PUBLISH_API
NostrPublishSigner *nostr_publish_signer_new_dbus(GDBusConnection *connection,
                                                  const gchar     *app_id,
                                                  GError         **error);

NOSTR_PUBLISH_API NostrPublishSigner *nostr_publish_signer_ref  (NostrPublishSigner *self);
NOSTR_PUBLISH_API void                nostr_publish_signer_unref(NostrPublishSigner *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NostrPublishSigner, nostr_publish_signer_unref)

/**
 * nostr_publish_signer_sign_event_json:
 * @unsigned_json: unsigned event JSON (kind, content, tags, created_at)
 *
 * Synchronous: the D-Bus implementation blocks for up to 30 s while the
 * operator answers an approval prompt.
 *
 * Returns: (transfer full) (nullable): signed event JSON, or NULL with
 *   @error set.
 */
NOSTR_PUBLISH_API
gchar *nostr_publish_signer_sign_event_json(NostrPublishSigner *self,
                                            const gchar        *unsigned_json,
                                            GCancellable       *cancellable,
                                            GError            **error);

/**
 * nostr_publish_signer_set_nip44_encrypt:
 * @func: (nullable): the NIP-44 operation (a separate setter, not a vtable
 *   member, so existing positional vtable initialisers keep compiling)
 *
 * The D-Bus signer sets this itself; vtable doubles opt in here.
 */
NOSTR_PUBLISH_API
void nostr_publish_signer_set_nip44_encrypt(NostrPublishSigner                *self,
                                            NostrPublishSignerNip44EncryptFunc func);

/**
 * nostr_publish_signer_nip44_encrypt:
 * @plaintext: UTF-8 text (NIP-44 caps it at 65535 bytes)
 * @peer_pubkey_hex: 64-hex x-only public key of the other party
 *
 * NIP-44 v2 encryption by the signer's own key, e.g. the content of a
 * NIP-59 kind-13 seal. The D-Bus implementation calls
 * `org.nostr.Signer.NIP44Encrypt(plaintext, peer, identity="")` and may
 * block for up to 30 s on an approval prompt.
 *
 * Returns: (transfer full) (nullable): the base64 NIP-44 payload, or NULL
 *   with @error set (MALFORMED when the signer has no NIP-44 support).
 */
NOSTR_PUBLISH_API
gchar *nostr_publish_signer_nip44_encrypt(NostrPublishSigner *self,
                                          const gchar        *plaintext,
                                          const gchar        *peer_pubkey_hex,
                                          GCancellable       *cancellable,
                                          GError            **error);

/**
 * nostr_publish_signer_error_is_permanent:
 * @error: (nullable): error returned by a sign call
 *
 * Returns: TRUE for DENIED / MALFORMED (do not retry), FALSE for
 *   TRANSIENT, errors from other domains, and NULL.
 */
NOSTR_PUBLISH_API
gboolean nostr_publish_signer_error_is_permanent(const GError *error);

/**
 * nostr_publish_signer_sign_auth_event:
 * @relay_url: relay that issued the challenge (the `relay` tag)
 * @challenge: NIP-42 challenge string (the `challenge` tag)
 * @created_at: unix seconds
 *
 * Builds and signs a NIP-42 kind-22242 client-authentication event.
 *
 * Returns: (transfer full) (nullable): signed event JSON suitable for an
 *   `["AUTH", <event>]` frame, or NULL with @error set.
 */
NOSTR_PUBLISH_API
gchar *nostr_publish_signer_sign_auth_event(NostrPublishSigner *self,
                                            const gchar        *relay_url,
                                            const gchar        *challenge,
                                            gint64              created_at,
                                            GError            **error);

G_END_DECLS
#endif /* NOSTR_PUBLISH_SIGNER_H */
