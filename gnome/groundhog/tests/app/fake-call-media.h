#ifndef FAKE_CALL_MEDIA_H
#define FAKE_CALL_MEDIA_H
#include "gh-call-media-backend.h"
#define GH_TYPE_FAKE_CALL_MEDIA (gh_fake_call_media_get_type())
G_DECLARE_FINAL_TYPE(GhFakeCallMedia, gh_fake_call_media, GH, FAKE_CALL_MEDIA, GObject)
struct _GhFakeCallMedia {
  GObject parent_instance;
  guint offers, answers, remote_offers, remote_answers, ice, teardowns;
  gboolean remote_ready, allow_local, fail, torn_down;
};
GhCallMediaBackend *gh_fake_call_media_factory(gpointer data, GError **error);
#endif
