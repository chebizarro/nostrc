#define G_LOG_DOMAIN "gnostr-main-window-profile-update"

#include "gnostr-main-window-private.h"

#include "gnostr-session-view.h"

#include "../model/gn-nostr-event-model.h"

#include <nostr-gobject-1.0/nostr_profile_provider.h>
#include <nostr-gtk-1.0/gnostr-thread-view.h>

void
gnostr_main_window_refresh_thread_view_profiles_if_visible_internal(GnostrMainWindow *self)
{
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;

  GtkWidget *thread_view = self->session_view ? gnostr_session_view_get_thread_view(self->session_view) : NULL;
  if (thread_view && NOSTR_GTK_IS_THREAD_VIEW(thread_view)) {
    if (gnostr_main_window_is_panel_visible_internal(self) &&
        !gnostr_session_view_is_showing_profile(self->session_view)) {
      nostr_gtk_thread_view_update_profiles(NOSTR_GTK_THREAD_VIEW(thread_view));
    }
  }
}

void
gnostr_main_window_update_meta_from_profile_json_internal(GnostrMainWindow *self,
                                                          const char *pubkey_hex,
                                                          const char *content_json)
{
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex || !content_json)
    return;

  gnostr_profile_provider_update(pubkey_hex, content_json);

  extern void gn_nostr_event_model_update_profile(GObject *model,
                                                  const char *pubkey_hex,
                                                  const char *content_json);
  if (self->event_model) {
    gn_nostr_event_model_update_profile(G_OBJECT(self->event_model), pubkey_hex, content_json);
  }

  if (self->user_pubkey_hex && pubkey_hex &&
      g_ascii_strcasecmp(self->user_pubkey_hex, pubkey_hex) == 0) {
    gnostr_main_window_update_login_ui_state_internal(self);

    if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
      GnostrProfileMeta *meta = gnostr_profile_provider_get(pubkey_hex);
      if (meta) {
        const char *display_name = meta->display_name ? meta->display_name : meta->name;
        gnostr_session_view_set_user_profile(self->session_view,
                                             pubkey_hex,
                                             display_name,
                                             meta->picture);
        gnostr_profile_meta_free(meta);
      }
    }
  }
}
