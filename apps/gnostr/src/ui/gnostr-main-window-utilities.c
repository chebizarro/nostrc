#define G_LOG_DOMAIN "gnostr-main-window-utilities"

#include "gnostr-main-window-private.h"

#include "../model/gn-nostr-event-model.h"

#include <nostr-gobject-1.0/gnostr-mute-list.h>

#include <string.h>

void
gnostr_main_window_show_toast_internal(GnostrMainWindow *self, const char *message)
{
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !message)
    return;

  if (self->toast_overlay) {
    AdwToast *toast = adw_toast_new(message);
    adw_toast_set_timeout(toast, 2);
    adw_toast_overlay_add_toast(self->toast_overlay, toast);
  }
}

void
gnostr_main_window_mute_user(GtkWidget *window, const char *pubkey_hex)
{
  if (!window || !GTK_IS_APPLICATION_WINDOW(window))
    return;

  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;

  if (!pubkey_hex || strlen(pubkey_hex) != 64) {
    g_warning("[MUTE] Invalid pubkey hex for mute user");
    return;
  }

  g_debug("[MUTE] Mute user requested for pubkey=%.16s...", pubkey_hex);

  GNostrMuteList *mute_list = gnostr_mute_list_get_default();
  gnostr_mute_list_add_pubkey(mute_list, pubkey_hex, FALSE);

  if (self->event_model)
    gn_nostr_event_model_refresh_async(GN_NOSTR_EVENT_MODEL(self->event_model));

  gnostr_main_window_show_toast_internal(self, "User muted");
}

void
gnostr_main_window_mute_thread(GtkWidget *window, const char *event_id_hex)
{
  if (!window || !GTK_IS_APPLICATION_WINDOW(window))
    return;

  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;

  if (!event_id_hex || strlen(event_id_hex) != 64) {
    g_warning("[MUTE] Invalid event ID hex for mute thread");
    return;
  }

  g_debug("[MUTE] Mute thread requested for event=%.16s...", event_id_hex);

  GNostrMuteList *mute_list = gnostr_mute_list_get_default();
  gnostr_mute_list_add_event(mute_list, event_id_hex, FALSE);

  if (self->event_model)
    gn_nostr_event_model_refresh_async(GN_NOSTR_EVENT_MODEL(self->event_model));

  gnostr_main_window_show_toast_internal(self, "Thread muted");
}

void
gnostr_main_window_show_toast(GtkWidget *window, const char *message)
{
  if (!window || !GTK_IS_APPLICATION_WINDOW(window))
    return;

  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(window);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;

  gnostr_main_window_show_toast_internal(self, message);
}

void
gnostr_main_window_enqueue_profile_author(GnostrMainWindow *self, const char *pubkey_hex)
{
  gnostr_main_window_enqueue_profile_author_internal(self, pubkey_hex);
}

void
gnostr_main_window_enqueue_profile_authors(GnostrMainWindow *self,
                                           const char **pubkey_hexes,
                                           size_t count)
{
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hexes || count == 0)
    return;

  for (size_t i = 0; i < count; i++) {
    const char *pk = pubkey_hexes[i];
    if (pk && strlen(pk) == 64)
      gnostr_main_window_enqueue_profile_author_internal(self, pk);
  }
}
