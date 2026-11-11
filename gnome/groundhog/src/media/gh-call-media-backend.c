#include "../app/gh-call-media-backend.h"

/* Available even in builds without the optional GStreamer call backend. */
G_DEFINE_INTERFACE(GhCallMediaBackend, gh_call_media_backend, G_TYPE_OBJECT)
static void gh_call_media_backend_default_init(GhCallMediaBackendInterface *iface)
{
  (void)iface;
  g_signal_new("local-ice", GH_TYPE_CALL_MEDIA_BACKEND, G_SIGNAL_RUN_LAST, 0,
      NULL, NULL, NULL, G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT);
  g_signal_new("connection-state", GH_TYPE_CALL_MEDIA_BACKEND, G_SIGNAL_RUN_LAST, 0,
      NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
}
