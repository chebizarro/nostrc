#ifndef GH_NIP46_SESSION_H
#define GH_NIP46_SESSION_H

#include <gio/gio.h>
#include "gh-relay-scope.h"
#include "gh-relay-publish.h"

G_BEGIN_DECLS

#define GH_TYPE_NIP46_SESSION (gh_nip46_session_get_type())
G_DECLARE_FINAL_TYPE(GhNip46Session, gh_nip46_session, GH, NIP46_SESSION, GObject)

typedef enum {
  GH_NIP46_SESSION_ERROR_INVALID_INPUT,
  GH_NIP46_SESSION_ERROR_UNAVAILABLE,
  GH_NIP46_SESSION_ERROR_DENIED,
  GH_NIP46_SESSION_ERROR_TIMED_OUT,
  GH_NIP46_SESSION_ERROR_CANCELLED,
  GH_NIP46_SESSION_ERROR_INVALID_RESULT
} GhNip46SessionError;
#define GH_NIP46_SESSION_ERROR (gh_nip46_session_error_quark())
GQuark gh_nip46_session_error_quark(void);

/* All methods and callbacks run on the construction thread's main context.
 * The secret is copied, never logged, and wiped at disposal. @relays is a
 * NULL-terminated list of one to four explicit signer relay URLs. */
GhNip46Session *gh_nip46_session_new(const gchar *client_secret_hex,
                                     const gchar *remote_signer_pubkey_hex,
                                     const gchar *const *relays,
                                     const GhRelayTransport *scope_transport,
                                     const GhRelayAuthTransport *scope_auth,
                                     const GhRelayPublishTransport *publish_transport,
                                     const GhRelayPublishAuthTransport *publish_auth,
                                     gpointer transport_data,
                                     GError **error);
/* A new attempt has a fresh transport key and a distinct 16-byte bearer token.
 * The returned URI is sensitive; the caller owns it and must clear it. */
GhNip46Session *gh_nip46_session_new_qr(const gchar *const *relays,
                                        const GhRelayTransport *scope_transport,
                                        const GhRelayAuthTransport *scope_auth,
                                        const GhRelayPublishTransport *publish_transport,
                                        const GhRelayPublishAuthTransport *publish_auth,
                                        gpointer transport_data,
                                        gchar **out_uri,
                                        GError **error);
GhNip46Session *gh_nip46_session_new_bunker(const gchar *bunker_uri,
                                            const GhRelayTransport *scope_transport,
                                            const GhRelayAuthTransport *scope_auth,
                                            const GhRelayPublishTransport *publish_transport,
                                            const GhRelayPublishAuthTransport *publish_auth,
                                            gpointer transport_data,
                                            GError **error);

const gchar *gh_nip46_session_get_client_pubkey(GhNip46Session *self);
/* For the credential store only: transfer-full; wipe the returned buffer after
 * use. Returns NULL after session cancellation. */
gchar *gh_nip46_session_dup_client_secret(GhNip46Session *self);
const gchar *gh_nip46_session_get_remote_pubkey(GhNip46Session *self);
gboolean gh_nip46_session_is_ready(GhNip46Session *self);
/* TRUE once requests are being published: after the first EOSE, or after a
 * short grace once the listening REQ was sent (a relay need never EOSE). */
gboolean gh_nip46_session_is_listening(GhNip46Session *self);
void gh_nip46_session_start(GhNip46Session *self);
/* The owner calls cancel before dropping its final reference: outstanding
 * GTasks retain their source session until completed. Cancellation revokes
 * the scope and every request, drops pairing secrets, and is idempotent. */
void gh_nip46_session_cancel(GhNip46Session *self);

/* "ready" is emitted after the first complete REQ/EOSE. "listening" is
 * emitted once, at the first EOSE or after a short grace from the REQ being
 * sent, whichever is first: requests are published from then on and a UI may
 * show the QR (the since window of the REQ still delivers an early reply).
 * "offline" is emitted after all ready relays drop.
 * "pair-progress" (int GhNip46PairStage, nullable string detail) reports
 * pairing stages for the UI. */
typedef enum {
  GH_NIP46_PAIR_STAGE_LISTENING,      /* listening; requests go out now */
  GH_NIP46_PAIR_STAGE_DELIVERED,      /* a relay accepted our request */
  GH_NIP46_PAIR_STAGE_RELAYS_SLOW,    /* bunker: no relay reachable yet (~15 s) */
  GH_NIP46_PAIR_STAGE_NO_ANSWER,      /* bunker: no answer from the signer (~45 s) */
  GH_NIP46_PAIR_STAGE_IGNORED_REPLY   /* QR: a non-matching reply was ignored; detail */
} GhNip46PairStage;
/* Sanitised, truncated copy of a signer-supplied error string, or NULL. */
gchar *gh_nip46_sanitize_signer_text(const gchar *text);
typedef gboolean (*GhNip46AuthUrlFunc)(GhNip46Session *self,
                                       const gchar *https_url,
                                       gpointer user_data);
void gh_nip46_session_set_auth_url_handler(GhNip46Session *self,
                                           GhNip46AuthUrlFunc callback,
                                           gpointer user_data);
void gh_nip46_session_call_async(GhNip46Session *self, const gchar *method,
                                 const gchar *const *params, gsize n_params,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback, gpointer user_data);
gchar *gh_nip46_session_call_finish(GhNip46Session *self, GAsyncResult *result,
                                    GError **error);
/* Pairing returns the hex pubkey of the user, which may differ from the
 * bunker key. A QR attempt waits for a reply carrying the secret (or, after
 * a short window, a bare "ack"), ignoring stray replies; bunker pairing
 * sends connect once listening, then requires ack or the secret. Bunker
 * pairing gives up after 90 s, QR after 300 s. */
void gh_nip46_session_pair_async(GhNip46Session *self, GCancellable *cancellable,
                                 GAsyncReadyCallback callback, gpointer user_data);
gchar *gh_nip46_session_pair_finish(GhNip46Session *self, GAsyncResult *result,
                                    GError **error);

G_END_DECLS
#endif
