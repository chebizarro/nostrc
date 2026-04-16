#define G_LOG_DOMAIN "gnostr-main-window-init"

#include "gnostr-main-window-private.h"

#include "gnostr-session-view.h"
#include "gnostr-dm-inbox-view.h"
#include "gnostr-dm-service.h"

void
gnostr_main_window_init_widget_state_internal(GnostrMainWindow *self)
{
  g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));

  self->compact = FALSE;
  gtk_widget_init_template(GTK_WIDGET(self));

  if (self->session_view && self->toast_overlay) {
    gnostr_session_view_set_toast_overlay(self->session_view, self->toast_overlay);
  }

  if (self->session_view) {
    g_object_bind_property(self, "compact", self->session_view, "compact",
                           G_BINDING_SYNC_CREATE);
  }

  gnostr_main_window_set_page(self, GNOSTR_MAIN_WINDOW_PAGE_LOADING);

  /* nostrc-e03f.4: Install the persistent Following tab alongside the
   * default Global tab synchronously, during widget init. We install here
   * rather than from the deferred heavy-init callback so the tab bar is
   * fully populated before the window becomes visible and before any
   * G_PRIORITY_DEFAULT user input (e.g. typing in the search entry) can
   * race with G_PRIORITY_LOW setup. Safe to call this early because it
   * only manipulates the GTK widget tree established by init_template. */
  gnostr_main_window_setup_initial_tabs_internal(self);
}

void
gnostr_main_window_init_runtime_state_internal(GnostrMainWindow *self)
{
  g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));

  self->seen_texts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->liked_events = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->reconnection_in_progress = FALSE;

  self->profile_fetch_queue = g_ptr_array_new_with_free_func(g_free);
  self->profile_fetch_requested = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->profile_fetch_source_id = 0;
  self->profile_fetch_debounce_ms = 50;
  self->profile_fetch_cancellable = g_cancellable_new();
  self->profile_fetch_active = 0;
  self->profile_fetch_max_concurrent = 5;

  self->ndb_sweep_source_id = 0;
  self->ndb_sweep_debounce_ms = 1000;

  self->sub_gift_wrap = 0;
  self->user_pubkey_hex = NULL;
  self->gift_wrap_queue = NULL;

  self->dm_service = gnostr_dm_service_new();
}

void
gnostr_main_window_init_dm_internal(GnostrMainWindow *self)
{
  g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));

  GtkWidget *dm_inbox = (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view))
                          ? gnostr_session_view_get_dm_inbox(self->session_view)
                          : NULL;
  if (dm_inbox && GNOSTR_IS_DM_INBOX_VIEW(dm_inbox)) {
    gnostr_dm_service_set_inbox_view(self->dm_service, GNOSTR_DM_INBOX_VIEW(dm_inbox));
    g_debug("[DM_SERVICE] Connected DM service to inbox view");
  }

  gnostr_main_window_connect_dm_handlers_internal(self);
}
