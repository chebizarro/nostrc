#ifndef GH_CALL_MEDIA_BACKEND_H
#define GH_CALL_MEDIA_BACKEND_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum { GH_CALL_VOICE, GH_CALL_VIDEO } GhCallType;
typedef enum {
  GH_CALL_MEDIA_CONNECTING, GH_CALL_MEDIA_CONNECTED,
  GH_CALL_MEDIA_DISCONNECTED, GH_CALL_MEDIA_FAILED
} GhCallMediaState;

#define GH_TYPE_CALL_MEDIA_BACKEND (gh_call_media_backend_get_type())
G_DECLARE_INTERFACE(GhCallMediaBackend, gh_call_media_backend, GH, CALL_MEDIA_BACKEND, GObject)

/* One backend instance per live call; all methods, completions and signals run
 * on the owning main context. No GStreamer types cross this boundary.
 *
 * create_local_async creates AND installs a local offer/answer, including an
 * upgrade/renegotiation on the existing connection. finish returns raw SDP
 * (transfer full). answer=TRUE requires a successfully applied remote offer.
 * apply_remote_async installs raw remote SDP; finish must complete only when
 * remote ICE may safely be added. Implementations honor cancellation.
 *
 * Signals:
 *   local-ice (string candidate, string sdp_mid, uint mline_index)
 *   connection-state (int GhCallMediaState)
 * ICE may arrive during create_local_async: the service queues it behind SDP.
 *
 * configure is called before starting and on privacy-policy changes. FALSE
 * means the policy cannot be enforced: no media sockets may be opened. Host
 * candidates must not be emitted OR accepted when allow_local_network=FALSE.
 * Tor is blocked by the service, not delegated to a media implementation.
 * teardown is idempotent, stops sockets and signals synchronously, and makes
 * pending operations complete cancelled. A torn-down instance is not reused.
 */
struct _GhCallMediaBackendInterface {
  GTypeInterface parent_iface;
  gboolean (*configure)(GhCallMediaBackend *self, gboolean allow_local_network,
                         GError **error);
  void (*create_local_async)(GhCallMediaBackend *self, GhCallType type,
                             gboolean answer, GCancellable *cancellable,
                             GAsyncReadyCallback callback, gpointer user_data);
  gchar *(*create_local_finish)(GhCallMediaBackend *self, GAsyncResult *result,
                                GError **error);
  void (*apply_remote_async)(GhCallMediaBackend *self, const gchar *sdp,
                             gboolean offer, GCancellable *cancellable,
                             GAsyncReadyCallback callback, gpointer user_data);
  gboolean (*apply_remote_finish)(GhCallMediaBackend *self, GAsyncResult *result,
                                  GError **error);
  gboolean (*add_remote_ice)(GhCallMediaBackend *self, const gchar *candidate,
                             const gchar *sdp_mid, guint mline_index,
                             GError **error);
  void (*teardown)(GhCallMediaBackend *self);
};

G_END_DECLS
#endif
