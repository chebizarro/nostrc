#ifndef GH_CALL_SIGNALLING_H
#define GH_CALL_SIGNALLING_H

#include "gh-account-controller.h"
#include "gh-account-relays.h"
#include "gh-inbox-resolver.h"
#include "gh-call-media-backend.h"
#include <nostr-event.h>

G_BEGIN_DECLS

#define GH_CALL_FRESHNESS_SECONDS 60
#define GH_CALL_MAX_PENDING 128

/* Codec is shared by the production receive path and the conformance suite.
 * Validates canonical id/signature, one recipient/call-id, raw SDP or typed ICE.
 * alt is sent, but optional on receive (quartz builders allow its omission).
 * Group-call extensions are rejected: this service owns only 1:1 calls. */
/* UUID on send; also accept Nospeak's 128-bit unhyphenated hex ids. */
gboolean gh_call_id_valid(const gchar *id);
NostrEvent *gh_call_event_parse(const gchar *json, const gchar *recipient,
                                gint64 now, GError **error);
const gchar *gh_call_event_tag(const NostrEvent *event, const gchar *name);
NostrEvent *gh_call_event_new(const gchar *author, const gchar *recipient,
                              const gchar *call_id, int kind, GhCallType type,
                              const gchar *content, gint64 now);
gchar *gh_call_ice_json(const gchar *candidate, const gchar *mid, guint index);
gboolean gh_call_ice_parse(const gchar *json, gchar **candidate, gchar **mid,
                           guint *index);

#define GH_TYPE_CALL_SIGNALLING (gh_call_signalling_get_type())
G_DECLARE_FINAL_TYPE(GhCallSignalling, gh_call_signalling, GH, CALL_SIGNALLING, GObject)

/* Main-context only. A private 21059 scope; no store/outbox/inbox dependencies.
 * Publish uses the relay layer's one-event publisher (same policy boundary as
 * scopes), observes OK, uses ephemeral AUTH and retries at most once.
 * Signals: event (string signed inner JSON), reset (), failed (string call-id),
 * relay-status (int GhRelayNotice, string url, string detail), including EOSE,
 * CLOSED and AUTH. Status is not persisted and is not proof of delivery.
 * Signed jobs are FIFO; publication acknowledgements never block signing the
 * next ICE. Queue bounds/deadlines fail visibly rather than replaying later.
 * A NIP-46 signer costs one round trip per event: >60s queued work expires.
 */
GhCallSignalling *gh_call_signalling_new(GhAccountController *accounts,
    GhAccountRelays *relays, GhInboxResolver *resolver, GSettings *settings);
const gchar *gh_call_signalling_get_pubkey(GhCallSignalling *self);
gboolean gh_call_signalling_send(GhCallSignalling *self, const gchar *peer,
    const gchar *call_id, int kind, GhCallType type, const gchar *content);
/* Cancels unsent jobs for a call, including a pending signer approval. */
void gh_call_signalling_cancel_call(GhCallSignalling *self, const gchar *call_id);
/* Explicit lifecycle stop, before releasing the process-owned service. */
void gh_call_signalling_stop(GhCallSignalling *self);

G_END_DECLS
#endif
