/* SPDX-License-Identifier: MIT */
#include "fake-service.h"
#include <string.h>
static gboolean timeout(gpointer data) { g_error("Profile panel barrier: %s", (char *)data); return G_SOURCE_REMOVE; }
#define WAIT(c) do { guint timer = g_timeout_add_seconds(10, timeout, #c); while (!(c)) g_main_context_iteration(NULL, TRUE); g_source_remove(timer); } while (0)
static GtkWidget *find(GtkWidget *widget, const char *text) {
  if ((GTK_IS_BUTTON(widget) && !g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), text)) ||
      (GTK_IS_LABEL(widget) && !g_strcmp0(gtk_label_get_text(GTK_LABEL(widget)), text)) ||
      (ADW_IS_PREFERENCES_ROW(widget) && !g_strcmp0(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(widget)), text)) ||
      !g_strcmp0(gtk_widget_get_tooltip_text(widget), text)) return widget;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find(child, text); if (found) return found;
  }
  return NULL;
}
static GtkTextView *about_view(GtkWidget *widget) {
  if (GTK_IS_TEXT_VIEW(widget)) return GTK_TEXT_VIEW(widget);
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    GtkTextView *found = about_view(child); if (found) return found;
  }
  return NULL;
}
static gboolean presented(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
  (void)widget; (void)clock; return ++*(guint *)data < 3 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}
typedef struct { GrottoProfileInfo *info, *saved; guint calls[5]; gboolean upload_fail, save_fail, offline, hold; GTask *pending; } Host;
static void profile(gpointer data, GrottoProfileAction action, const char *key, const GrottoProfileInfo *info,
  GCancellable *cancel, GAsyncReadyCallback callback, gpointer user) {
  Host *host = data; g_assert_cmpstr(key, ==, "public-profile-key"); host->calls[action]++;
  GTask *task = g_task_new(NULL, cancel, callback, user);
  if (action == GROTTO_PROFILE_SAVE && host->hold) { host->pending = task; return; }
  if ((action == GROTTO_PROFILE_CHOOSE_AVATAR || action == GROTTO_PROFILE_CHOOSE_BANNER) && host->upload_fail)
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Image upload failed. Try again");
  else if (action == GROTTO_PROFILE_SAVE && host->save_fail)
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE, "Offline. Your edits have not been saved");
  else {
    GrottoProfileInfo *result = grotto_profile_info_copy(action == GROTTO_PROFILE_FETCH ? host->info : info);
    result->cached = host->offline;
    if (action == GROTTO_PROFILE_SAVE) {
      g_clear_pointer(&host->saved, grotto_profile_info_free); host->saved = grotto_profile_info_copy(info);
      result->published = TRUE; result->created_at++;
    }
    g_task_return_pointer(task, result, (GDestroyNotify)grotto_profile_info_free);
  }
  g_object_unref(task);
}
static void profile_panel(void) {
  g_autoptr(GrottoPanelsFakeService) fake = grotto_panels_fake_service_new();
  grotto_panels_fake_service_set_available(fake, TRUE);
  GrottoPanelsMetadata identity = {.id = "identity", .public_key = "public-profile-key", .label = "Everyday", .is_default = TRUE, .can_sign = TRUE, .backup_acknowledged = TRUE};
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_IDENTITIES, &identity, NULL));
  Host host = {.info = grotto_profile_info_copy(NULL)};
  GtkWidget *panel = grotto_identity_panel_new(GROTTO_PANELS_SERVICE(fake), NULL);
  const GrottoPanelsHost callbacks = {.profile_async = profile};
  grotto_panel_set_host(GROTTO_PANEL(panel), &callbacks, &host, NULL);
  AdwWindow *window = ADW_WINDOW(adw_window_new()); g_object_ref_sink(window);
  gtk_window_set_default_size(GTK_WINDOW(window), 360, 720); adw_window_set_content(window, panel); gtk_window_present(GTK_WINDOW(window));
  WAIT(host.calls[GROTTO_PROFILE_FETCH] == 1 && find(panel, "Edit Profile…") && gtk_widget_get_sensitive(find(panel, "Edit Profile…")));
  GtkWidget *row = find(panel, "Public Profile"); g_assert_nonnull(row);
  g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(row)), ==, "Not published yet");
  g_signal_emit_by_name(find(panel, "Edit Profile…"), "clicked");
  AdwDialog *dialog = adw_window_get_visible_dialog(window); g_assert_nonnull(dialog);
  /* libadwaita 1.5 opens its sheet after the initial frame ticks. Drive the
   * presented form, not a programmatic click before the sheet can be seen. */
  guint frames = 0; gtk_widget_add_tick_callback(GTK_WIDGET(dialog), presented, &frames, NULL); WAIT(frames == 3);
  const char *titles[] = {"Name", "Display Name", "Avatar URL", "Banner URL", "Nostr Address", "Lightning Address", "Lightning LNURL", "Website"};
  const char *values[] = {"alice", "Alice", "https://media.example/avatar.png", "https://media.example/banner.png", "alice@example.com", "alice@pay.example", "lnurl1example", "https://example.com"};
  for (guint i = 0; i < G_N_ELEMENTS(titles); i++) {
    GtkWidget *entry = find(GTK_WIDGET(dialog), titles[i]); g_assert_true(ADW_IS_ENTRY_ROW(entry)); gtk_editable_set_text(GTK_EDITABLE(entry), values[i]);
  }
  GtkTextView *about = about_view(GTK_WIDGET(dialog)); g_assert_nonnull(about);
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(about), "First line\nSecond line", -1);
  host.upload_fail = TRUE;
  g_signal_emit_by_name(find(GTK_WIDGET(dialog), "Choose Avatar…"), "clicked");
  WAIT(find(GTK_WIDGET(dialog), "Image upload failed. Try again"));
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(find(GTK_WIDGET(dialog), "Avatar URL"))), ==, values[2]);
  g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(row)), ==, "Not published yet");
  host.save_fail = TRUE;
  g_signal_emit_by_name(find(GTK_WIDGET(dialog), "Save"), "clicked");
  WAIT(find(GTK_WIDGET(dialog), "Offline. Your edits have not been saved"));
  g_assert_nonnull(adw_window_get_visible_dialog(window)); g_assert_null(host.saved);
  host.save_fail = FALSE; host.hold = TRUE;
  g_signal_emit_by_name(find(GTK_WIDGET(dialog), "Save"), "clicked");
  g_assert_nonnull(host.pending); g_assert_nonnull(find(GTK_WIDGET(dialog), "Stop"));
  g_assert_true(gtk_widget_is_sensitive(find(GTK_WIDGET(dialog), "Stop")));
  g_signal_emit_by_name(find(GTK_WIDGET(dialog), "Stop"), "clicked");
  g_assert_true(g_cancellable_is_cancelled(g_task_get_cancellable(host.pending)));
  g_assert_true(g_task_return_error_if_cancelled(host.pending)); g_clear_object(&host.pending);
  WAIT(find(GTK_WIDGET(dialog), "Stopped. Data may already have reached a server or relay. Refresh before saving again"));
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(find(GTK_WIDGET(dialog), "Name"))), ==, "alice");
  host.hold = FALSE;
  g_signal_emit_by_name(find(GTK_WIDGET(dialog), "Save"), "clicked");
  WAIT(host.saved);
  WAIT(!adw_window_get_visible_dialog(window));
  g_assert_cmpstr(host.saved->fields[GROTTO_PROFILE_NAME], ==, "alice");
  g_assert_cmpstr(host.saved->fields[GROTTO_PROFILE_ABOUT], ==, "First line\nSecond line");
  g_assert_cmpstr(host.saved->fields[GROTTO_PROFILE_DISPLAY_NAME], ==, "Alice");
  for (guint i = 2; i < G_N_ELEMENTS(values); i++) g_assert_cmpstr(host.saved->fields[i + 1], ==, values[i]);
  g_assert_nonnull(find(panel, "Alice"));
  grotto_profile_info_free(host.info); host.info = grotto_profile_info_copy(host.saved); host.info->published = TRUE; host.offline = TRUE;
  g_signal_emit_by_name(find(panel, "Refresh Profile"), "clicked");
  WAIT(find(panel, "Offline — showing the saved profile"));
  identity.watch_only = TRUE; identity.can_sign = FALSE;
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_IDENTITIES, &identity, NULL));
  g_assert_false(gtk_widget_get_visible(find(panel, "Edit Profile…")));
  g_assert_nonnull(find(panel, "Nostr Address"));
  g_assert_cmpuint(host.calls[GROTTO_PROFILE_LOAD_IMAGES], ==, 0);
  g_signal_emit_by_name(find(panel, "Load Profile Images…"), "clicked");
  WAIT(host.calls[GROTTO_PROFILE_LOAD_IMAGES] == 1 && gtk_widget_is_sensitive(find(panel, "Load Profile Images…")));
  host.info->tor_blocked = TRUE;
  g_signal_emit_by_name(find(panel, "Refresh Profile"), "clicked");
  WAIT(find(panel, "Tor mode — showing the saved profile without network access"));
  g_assert_cmpuint(host.calls[GROTTO_PROFILE_SAVE], ==, 3);
  gtk_window_destroy(GTK_WINDOW(window)); g_object_unref(window);
  grotto_profile_info_free(host.info); grotto_profile_info_free(host.saved);
}
void register_profile_panel_tests(void) { g_test_add_func("/grotto-panels/profile", profile_panel); }
