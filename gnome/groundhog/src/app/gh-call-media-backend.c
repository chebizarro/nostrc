#include "gh-call-media-backend.h"

G_DEFINE_INTERFACE(GhCallMediaBackend, gh_call_media_backend, G_TYPE_OBJECT)

static void
gh_call_media_backend_default_init(GhCallMediaBackendInterface *iface)
{
  g_signal_new("local-ice", G_TYPE_FROM_INTERFACE(iface), G_SIGNAL_RUN_LAST,
               0, NULL, NULL, NULL, G_TYPE_NONE, 3,
               G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT);
  g_signal_new("connection-state", G_TYPE_FROM_INTERFACE(iface), G_SIGNAL_RUN_LAST,
               0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
}
