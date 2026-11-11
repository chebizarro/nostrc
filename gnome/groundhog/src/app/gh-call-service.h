#ifndef GH_CALL_SERVICE_H
#define GH_CALL_SERVICE_H

#include "gh-call-signalling.h"
#include "gh-conversation-store.h"
#include "gh-net-session.h"

G_BEGIN_DECLS

typedef enum {
  GH_CALL_IDLE, GH_CALL_RINGING, GH_CALL_OUTGOING,
  GH_CALL_CONNECTING, GH_CALL_ACTIVE, GH_CALL_ENDED
} GhCallState;
typedef enum { GH_CALL_INCOMING, GH_CALL_OUTBOUND } GhCallDirection;
typedef enum {
  GH_CALL_END_NONE, GH_CALL_END_HANGUP, GH_CALL_END_DECLINED, GH_CALL_END_BUSY,
  GH_CALL_END_TIMEOUT, GH_CALL_END_REQUEST, GH_CALL_END_ELSEWHERE,
  GH_CALL_END_ACCOUNT_CHANGED, GH_CALL_END_MEDIA_FAILED,
  GH_CALL_END_SIGNALLING_FAILED, GH_CALL_END_PRIVACY
} GhCallEndReason;

#define GH_TYPE_CALL (gh_call_get_type())
G_DECLARE_FINAL_TYPE(GhCall, gh_call, GH, CALL, GObject)
const gchar *gh_call_get_id(GhCall *call);
const gchar *gh_call_get_peer(GhCall *call);
GhCallType gh_call_get_call_type(GhCall *call);
GhCallDirection gh_call_get_direction(GhCall *call);
GhCallState gh_call_get_state(GhCall *call);
GhCallEndReason gh_call_get_end_reason(GhCall *call);
/* GhCall emits changed () after any observable change. */

#define GH_TYPE_CALL_SERVICE (gh_call_service_get_type())
G_DECLARE_FINAL_TYPE(GhCallService, gh_call_service, GH, CALL_SERVICE, GObject)
/* Factory returns a new backend for one call. NULL factory is deliberately
 * unavailable, never a fake media implementation in production. */
typedef GhCallMediaBackend *(*GhCallMediaFactory)(gpointer data, GError **error);
GhCallService *gh_call_service_new(GhCallSignalling *signalling,
    GhConversationStore *conversations, GSettings *settings, GhNetSession *network);
void gh_call_service_set_media_factory(GhCallService *self, GhCallMediaFactory factory,
                                        gpointer data, GDestroyNotify destroy);
/* Transfer none; service retains recent calls only in memory, never gh-store. */
GhCall *gh_call_service_get_current(GhCallService *self);
GhCall *gh_call_service_place(GhCallService *self, const gchar *peer,
                              GhCallType type, GError **error);
gboolean gh_call_service_answer(GhCallService *self, GError **error);
void gh_call_service_decline(GhCallService *self);
void gh_call_service_hang_up(GhCallService *self);
gboolean gh_call_service_upgrade_video(GhCallService *self, GError **error);
void gh_call_service_stop(GhCallService *self);
/* Shared dispatcher, also usable by deterministic conformance drivers. Still
 * validates signatures, recipient, freshness and dedupe before any effects.
 * bind revokes all pending work before changing identity. */
void gh_call_service_bind(GhCallService *self, const gchar *pubkey);
void gh_call_service_receive(GhCallService *self, const gchar *signed_inner, gint64 now);
void gh_call_service_expire(GhCallService *self, gint64 now);
/* Signals: incoming-ring (GhCall), missed-call (GhCall), state-changed (GhCall).
 * send (peer, call-id, kind, call-type, content) is diagnostic: emitted for
 * attempted sends, NOT relay acknowledgement or recipient delivery. */
G_END_DECLS
#endif
