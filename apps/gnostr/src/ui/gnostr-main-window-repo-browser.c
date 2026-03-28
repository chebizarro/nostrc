#define G_LOG_DOMAIN "gnostr-main-window-repo-browser"

#include "gnostr-main-window-private.h"

#include "gnostr-session-view.h"
#include "gnostr-repo-browser.h"
#include "../util/gnostr-plugin-manager.h"

void
gnostr_main_window_on_repo_selected_internal(GnostrRepoBrowser *browser,
                                             const char *repo_id,
                                             gpointer user_data)
{
  (void)browser;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);

  if (!GNOSTR_IS_MAIN_WINDOW(self) || !repo_id)
    return;

  g_debug("[REPO] Repository selected: %s", repo_id);

  if (ADW_IS_TOAST_OVERLAY(self->toast_overlay)) {
    g_autofree char *msg = g_strdup_printf("Selected repository: %.16s...", repo_id);
    adw_toast_overlay_add_toast(self->toast_overlay, adw_toast_new(msg));
  }
}

void
gnostr_main_window_on_clone_requested_internal(GnostrRepoBrowser *browser,
                                               const char *clone_url,
                                               gpointer user_data)
{
  (void)browser;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);

  if (!GNOSTR_IS_MAIN_WINDOW(self) || !clone_url)
    return;

  g_debug("[REPO] Clone requested: %s", clone_url);

  GnostrPluginManager *manager = gnostr_plugin_manager_get_default();
  GVariant *param = g_variant_new_string(clone_url);

  if (gnostr_plugin_manager_dispatch_action(manager, "nip34-git",
                                            "open-git-client", param)) {
    g_debug("[REPO] Dispatched to nip34-git plugin");
  } else {
    GdkClipboard *clipboard = gdk_display_get_clipboard(gdk_display_get_default());
    gdk_clipboard_set_text(clipboard, clone_url);

    if (ADW_IS_TOAST_OVERLAY(self->toast_overlay)) {
      adw_toast_overlay_add_toast(self->toast_overlay,
                                  adw_toast_new("Clone URL copied to clipboard"));
    }
  }
}

void
gnostr_main_window_on_repo_browser_need_profile_internal(GnostrRepoBrowser *browser,
                                                         const char *pubkey_hex,
                                                         gpointer user_data)
{
  (void)browser;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);

  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex)
    return;
  if (strlen(pubkey_hex) != 64)
    return;

  g_debug("[REPO] Profile fetch requested for maintainer: %.16s...", pubkey_hex);
  gnostr_main_window_enqueue_profile_author(self, pubkey_hex);
}

void
gnostr_main_window_on_repo_browser_open_profile_internal(GnostrRepoBrowser *browser,
                                                         const char *pubkey_hex,
                                                         gpointer user_data)
{
  (void)browser;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);

  if (!GNOSTR_IS_MAIN_WINDOW(self) || !pubkey_hex)
    return;
  if (strlen(pubkey_hex) != 64)
    return;

  g_debug("[REPO] Open profile requested for maintainer: %.16s...", pubkey_hex);
  gnostr_main_window_open_profile(GTK_WIDGET(self), pubkey_hex);
}

void
gnostr_main_window_on_repo_refresh_requested_internal(GnostrRepoBrowser *browser,
                                                      gpointer user_data)
{
  (void)browser;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);

  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;

  g_debug("[REPO] Refresh requested - dispatching to NIP-34 plugin");

  GnostrPluginManager *manager = gnostr_plugin_manager_get_default();
  if (gnostr_plugin_manager_dispatch_action(manager, "nip34-git",
                                            "nip34-refresh", NULL)) {
    g_debug("[REPO] Dispatched nip34-refresh action to plugin");

    if (ADW_IS_TOAST_OVERLAY(self->toast_overlay)) {
      adw_toast_overlay_add_toast(self->toast_overlay,
                                  adw_toast_new("Fetching repositories from relays..."));
    }
  } else {
    g_warning("[REPO] Failed to dispatch refresh - NIP-34 plugin not available");

    if (ADW_IS_TOAST_OVERLAY(self->toast_overlay)) {
      adw_toast_overlay_add_toast(self->toast_overlay,
                                  adw_toast_new("NIP-34 plugin not available"));
    }
  }
}
