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
/* The call's media backend (transfer none) for the UI's controls of the
 * concrete backend: NULL until media starts (placing, or answering), and
 * already torn down once the call is ENDED. */
GhCallMediaBackend *gh_call_get_media(GhCall *call);
/* TRUE while an ACTIVE call's media connection is interrupted. It returns to
 * FALSE when media reconnects, or the call ends (MEDIA_FAILED) after
 * GH_CALL_RECONNECT_SECONDS. */
#define GH_CALL_RECONNECT_SECONDS 15
gboolean gh_call_get_reconnecting(GhCall *call);
/* g_get_monotonic_time() when the call first became ACTIVE, 0 before. */
gint64 gh_call_get_connected_time(GhCall *call);
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
/* Whether a call could be placed now, before any peer check. Errors:
 * G_IO_ERROR_NOT_INITIALIZED (no account, or stopped), G_IO_ERROR_PERMISSION_DENIED
 * (Tor mode) and G_IO_ERROR_NOT_SUPPORTED (no media factory). The answer can
 * change when "availability-changed" is emitted. */
gboolean gh_call_service_check_available(GhCallService *self, GError **error);
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
/* Signals: incoming-ring (GhCall), missed-call (GhCall), state-changed (GhCall),
 * availability-changed () (account, network mode, settings or factory).
 * send (peer, call-id, kind, call-type, content) is diagnostic: emitted for
 * attempted sends, NOT relay acknowledgement or recipient delivery. */
G_END_DECLS
#endif
