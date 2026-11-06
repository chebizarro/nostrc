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
static guint assert_profile_header(GtkWidget *root, gboolean has_banner) {
  if (GTK_IS_OVERLAY(root)) {
    GtkWidget *box = gtk_overlay_get_child(GTK_OVERLAY(root));
    GtkWidget *avatar = gtk_widget_get_next_sibling(box);
    if (ADW_IS_AVATAR(avatar)) {
      GtkWidget *banner = gtk_widget_get_first_child(box);
      graphene_rect_t a, b;
      g_assert_true(GTK_IS_PICTURE(banner));
      g_assert_cmpint(gtk_widget_get_visible(banner), ==, has_banner);
      g_assert_true(gtk_widget_compute_bounds(avatar, root, &a));
      g_assert_cmpfloat_with_epsilon(a.origin.x + a.size.width / 2, gtk_widget_get_width(root) / 2.0, 1);
      if (has_banner) {
        g_assert_true(gtk_widget_compute_bounds(banner, root, &b));
        g_assert_cmpfloat_with_epsilon(a.origin.y + a.size.height / 2, b.origin.y + b.size.height, 1);
      } else {
        g_assert_cmpint(gtk_widget_get_height(root), ==, adw_avatar_get_size(ADW_AVATAR(avatar)));
      }
      return 1;
    }
  }
  guint count = 0;
  for (GtkWidget *child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child))
    count += assert_profile_header(child, has_banner);
  return count;
}
static void assert_detail_rows(GtkWidget *root) {
  const char *titles[] = {"Publication", "Discovery", "Name", "Display Name", "About"};
  for (guint i = 0; i < G_N_ELEMENTS(titles); i++) {
    GtkWidget *row = find(root, titles[i]); g_assert_true(ADW_IS_ACTION_ROW(row));
    g_assert_true(adw_action_row_get_subtitle_selectable(ADW_ACTION_ROW(row)));
    g_assert_false(adw_preferences_row_get_use_markup(ADW_PREFERENCES_ROW(row)));
  }
}
static void dialog_finalized(gpointer data, GObject *object) { (void)object; *(gboolean *)data = TRUE; }
static gboolean presented(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
  (void)widget; (void)clock; return ++*(guint *)data < 3 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}
typedef struct { GrottoProfileInfo *info, *saved; guint calls[GROTTO_PROFILE_FETCH_CACHED + 1]; GrottoNip05Status nip05; gboolean upload_fail, save_fail, fetch_fail, offline, hold, hold_fetch; GTask *pending; } Host;
static void profile(gpointer data, GrottoProfileAction action, const char *key, const GrottoProfileInfo *info,
  GCancellable *cancel, GAsyncReadyCallback callback, gpointer user) {
  Host *host = data; g_assert_cmpstr(key, ==, "public-profile-key"); host->calls[action]++;
  GTask *task = g_task_new(NULL, cancel, callback, user);
  if (action == GROTTO_PROFILE_FETCH && host->hold_fetch) { host->pending = task; return; }
  if (action == GROTTO_PROFILE_SAVE && host->hold) { host->pending = task; return; }
  if (action == GROTTO_PROFILE_FETCH_CACHED)
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No cached profile");
  else if (action == GROTTO_PROFILE_FETCH && host->fetch_fail)
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE, "Profile unavailable. Check your relays and try again");
  else if ((action == GROTTO_PROFILE_CHOOSE_AVATAR || action == GROTTO_PROFILE_CHOOSE_BANNER) && host->upload_fail)
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Image upload failed. Try again");
  else if (action == GROTTO_PROFILE_SAVE && host->save_fail)
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE, "Offline. Your edits have not been saved");
  else {
    GrottoProfileInfo *result = grotto_profile_info_copy(action == GROTTO_PROFILE_FETCH ? host->info : info);
    result->cached = host->offline;
    if (action == GROTTO_PROFILE_VERIFY_NIP05) result->nip05_status = host->nip05;
    if (action == GROTTO_PROFILE_SAVE) {
      result->published = TRUE; result->created_at++;
      g_clear_pointer(&host->saved, grotto_profile_info_free); host->saved = grotto_profile_info_copy(result);
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
  Host host = {.info = grotto_profile_info_copy(NULL), .nip05 = GROTTO_NIP05_VERIFIED, .hold_fetch = TRUE};
  host.info->auth_required_relays = g_strsplit("wss://auth.discovery.example/", "\n", -1);
  GtkWidget *panel = grotto_identity_panel_new(GROTTO_PANELS_SERVICE(fake), NULL);
  const GrottoPanelsHost callbacks = {.profile_async = profile};
  grotto_panel_set_host(GROTTO_PANEL(panel), &callbacks, &host, NULL);
  AdwWindow *window = ADW_WINDOW(adw_window_new()); g_object_ref_sink(window);
  gtk_window_set_default_size(GTK_WINDOW(window), 360, 720); adw_window_set_content(window, panel); gtk_window_present(GTK_WINDOW(window));
  WAIT(host.pending);
  GtkWidget *loading = find(panel, "Loading profile…"); g_assert_nonnull(loading);
  GtkWidget *spinner = gtk_widget_get_prev_sibling(loading); g_assert_true(GTK_IS_SPINNER(spinner));
  g_assert_true(gtk_spinner_get_spinning(GTK_SPINNER(spinner)));
  g_assert_false(gtk_widget_get_sensitive(find(panel, "Edit Profile…")));
  host.hold_fetch = FALSE;
  g_task_return_new_error(host.pending, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE, "Profile unavailable. Check your relays and try again"); g_clear_object(&host.pending);
  WAIT(host.calls[GROTTO_PROFILE_FETCH] == 1 && find(panel, "Edit Profile…") && gtk_widget_get_sensitive(find(panel, "Edit Profile…")));
  g_assert_false(gtk_spinner_get_spinning(GTK_SPINNER(spinner)));
  g_assert_false(gtk_widget_get_visible(spinner));
  host.fetch_fail = TRUE;
  g_signal_emit_by_name(find(panel, "Refresh Profile"), "clicked");
  WAIT(host.calls[GROTTO_PROFILE_FETCH] == 2 && find(panel, "Profile unavailable. Check your relays and try again") &&
    gtk_widget_get_sensitive(find(panel, "Edit Profile…")));
  g_assert_true(gtk_widget_get_sensitive(find(panel, "Edit Profile…")));
  GtkWidget *failed_summary = find(panel, "Public Profile");
  g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(failed_summary)), ==, "Profile not loaded");
  /* Address verification checks the draft, not whether a kind 0 exists. */
  g_signal_emit_by_name(find(panel, "Edit Profile…"), "clicked");
  AdwDialog *check_dialog = adw_window_get_visible_dialog(window); g_assert_nonnull(check_dialog);
  guint check_frames = 0; gtk_widget_add_tick_callback(GTK_WIDGET(check_dialog), presented, &check_frames, NULL); WAIT(check_frames == 3);
  g_assert_cmpint(gtk_widget_get_width(GTK_WIDGET(window)), >, 0);
  g_assert_cmpint(gtk_widget_get_width(GTK_WIDGET(window)), <=, 360);
  g_assert_cmpuint(assert_profile_header(panel, FALSE), ==, 1);
  adw_expander_row_set_expanded(ADW_EXPANDER_ROW(find(GTK_WIDGET(check_dialog), "Profile Preview")), TRUE);
  check_frames = 0; gtk_widget_add_tick_callback(GTK_WIDGET(check_dialog), presented, &check_frames, NULL); WAIT(check_frames == 3);
  g_assert_cmpuint(assert_profile_header(GTK_WIDGET(check_dialog), FALSE), ==, 1);
  gtk_window_set_default_size(GTK_WINDOW(window), 920, 720);
  check_frames = 0; gtk_widget_add_tick_callback(GTK_WIDGET(window), presented, &check_frames, NULL); WAIT(check_frames == 3);
  g_assert_cmpint(gtk_widget_get_width(GTK_WIDGET(window)), >, 360);
  g_assert_cmpuint(assert_profile_header(panel, FALSE), ==, 1);
  g_assert_cmpuint(assert_profile_header(GTK_WIDGET(check_dialog), FALSE), ==, 1);
  gtk_window_set_default_size(GTK_WINDOW(window), 360, 720);
  check_frames = 0; gtk_widget_add_tick_callback(GTK_WIDGET(window), presented, &check_frames, NULL); WAIT(check_frames == 3);
  g_assert_cmpint(gtk_widget_get_width(GTK_WIDGET(window)), <=, 360);
  gtk_editable_set_text(GTK_EDITABLE(find(GTK_WIDGET(check_dialog), "Nostr Address")), "alice@example.com");
  g_signal_emit_by_name(find(GTK_WIDGET(check_dialog), "Check"), "clicked");
  WAIT(host.calls[GROTTO_PROFILE_VERIFY_NIP05] == 1 && find(GTK_WIDGET(check_dialog), "Address matches this public key"));
  g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(failed_summary)), ==, "Profile not loaded");
  g_assert_nonnull(find(panel, "Profile unavailable. Check your relays and try again"));
  g_assert_cmpuint(host.calls[GROTTO_PROFILE_FETCH], ==, 2); g_assert_cmpuint(host.calls[GROTTO_PROFILE_SAVE], ==, 0);
  gboolean check_dialog_gone = FALSE; g_object_weak_ref(G_OBJECT(check_dialog), dialog_finalized, &check_dialog_gone);
  g_signal_emit_by_name(find(GTK_WIDGET(check_dialog), "Cancel"), "clicked");
  WAIT(!adw_window_get_visible_dialog(window) && check_dialog_gone);
  g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(failed_summary)), ==, "Profile not loaded");
  g_assert_nonnull(find(panel, "Profile unavailable. Check your relays and try again"));
  g_assert_true(gtk_widget_get_sensitive(find(panel, "Edit Profile…")));
  g_assert_true(gtk_widget_get_sensitive(find(panel, "Retry")));
  host.fetch_fail = FALSE;
  static const guint8 pixels[] = {80, 120, 180, 255, 80, 120, 180, 255, 80, 120, 180, 255};
  g_autoptr(GBytes) bytes = g_bytes_new_static(pixels, sizeof pixels);
  host.info->images[1] = gdk_memory_texture_new(3, 1, GDK_MEMORY_R8G8B8A8, bytes, 12);
  g_signal_emit_by_name(find(panel, "Refresh Profile"), "clicked");
  WAIT(find(panel, "No profile published yet") && gtk_widget_get_sensitive(find(panel, "Edit Profile…")));
  g_assert_nonnull(find(panel, "Profile discovery relays\nNo relays contacted\n\nSkipped — relay requires authentication (bootstrap never signs or sends AUTH):\nwss://auth.discovery.example/"));
  GtkWidget *row = find(panel, "Public Profile"); g_assert_nonnull(row);
  g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(row)), ==, "No profile published yet");
  g_signal_emit_by_name(find(panel, "Edit Profile…"), "clicked");
  AdwDialog *dialog = adw_window_get_visible_dialog(window); g_assert_nonnull(dialog);
  /* libadwaita 1.5 opens its sheet after the initial frame ticks. Drive the
   * presented form, not a programmatic click before the sheet can be seen. */
  guint frames = 0; gtk_widget_add_tick_callback(GTK_WIDGET(dialog), presented, &frames, NULL); WAIT(frames == 3);
  GtkWidget *header = gtk_widget_get_parent(find(GTK_WIDGET(dialog), "Save"));
  while (header && !ADW_IS_HEADER_BAR(header)) header = gtk_widget_get_parent(header);
  g_assert_nonnull(header);
  g_assert_false(adw_header_bar_get_show_start_title_buttons(ADW_HEADER_BAR(header)));
  g_assert_false(adw_header_bar_get_show_end_title_buttons(ADW_HEADER_BAR(header)));
  g_assert_null(find(GTK_WIDGET(dialog), "Republish"));
  assert_detail_rows(panel);
  g_assert_cmpint(gtk_widget_get_width(GTK_WIDGET(window)), <=, 360);
  g_assert_cmpuint(assert_profile_header(panel, TRUE), ==, 1);
  adw_expander_row_set_expanded(ADW_EXPANDER_ROW(find(GTK_WIDGET(dialog), "Profile Preview")), TRUE);
  frames = 0; gtk_widget_add_tick_callback(GTK_WIDGET(dialog), presented, &frames, NULL); WAIT(frames == 3);
  g_assert_cmpuint(assert_profile_header(GTK_WIDGET(dialog), TRUE), ==, 1);
  gtk_window_set_default_size(GTK_WINDOW(window), 920, 720);
  frames = 0; gtk_widget_add_tick_callback(GTK_WIDGET(window), presented, &frames, NULL); WAIT(frames == 3);
  g_assert_cmpint(gtk_widget_get_width(GTK_WIDGET(window)), >, 360);
  g_assert_cmpuint(assert_profile_header(panel, TRUE), ==, 1); g_assert_cmpuint(assert_profile_header(GTK_WIDGET(dialog), TRUE), ==, 1);
  adw_expander_row_set_expanded(ADW_EXPANDER_ROW(find(GTK_WIDGET(dialog), "Profile Preview")), FALSE);
  GtkTextView *about_field = about_view(GTK_WIDGET(dialog));
  GtkWidget *about_row = gtk_widget_get_ancestor(GTK_WIDGET(about_field), GTK_TYPE_LIST_BOX_ROW);
  g_assert_nonnull(about_row);
  g_assert_true(gtk_widget_get_parent(about_row) == gtk_widget_get_parent(find(GTK_WIDGET(dialog), "Name")));
  g_assert_false(gtk_text_view_get_accepts_tab(about_field));
  GtkWidget *placeholder = find(GTK_WIDGET(dialog), "Write something about yourself…");
  g_assert_true(gtk_widget_get_visible(placeholder));
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(about_field), "A draft", -1);
  g_assert_false(gtk_widget_get_visible(placeholder));
  gtk_editable_set_text(GTK_EDITABLE(find(GTK_WIDGET(dialog), "Name")), "discard-me");
  g_signal_emit_by_name(find(GTK_WIDGET(dialog), "Discard Local Changes"), "activated");
  g_assert_true(gtk_widget_get_visible(placeholder));
  GtkWidget *provenance = find(GTK_WIDGET(dialog), "Publication and Discovery Details");
  g_assert_true(ADW_IS_ACTION_ROW(find(provenance, "Publication")));
  g_assert_true(ADW_IS_ACTION_ROW(find(provenance, "Discovery")));
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(find(GTK_WIDGET(dialog), "Name"))), ==, "");
  const char *titles[] = {"Name", "Display Name", "Avatar URL", "Banner URL", "Nostr Address", "Lightning Address", "Lightning LNURL", "Website"};
  const char *values[] = {"alice", "Alice", "https://media.example/avatar.png", "https://media.example/banner.png", "alice@example.com", "alice@pay.example", "lnurl1example", "https://example.com"};
  for (guint i = 0; i < G_N_ELEMENTS(titles); i++) {
    GtkWidget *entry = find(GTK_WIDGET(dialog), titles[i]); g_assert_true(ADW_IS_ENTRY_ROW(entry)); gtk_editable_set_text(GTK_EDITABLE(entry), values[i]);
  }
  GtkTextView *about = about_view(GTK_WIDGET(dialog)); g_assert_nonnull(about);
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(about), "First line\nSecond line", -1);
  adw_switch_row_set_active(ADW_SWITCH_ROW(find(GTK_WIDGET(dialog), "Automated Account (Bot)")), TRUE);
  g_assert_nonnull(find(GTK_WIDGET(dialog), "@alice · Bot"));
  g_assert_nonnull(find(GTK_WIDGET(dialog), "First line\nSecond line"));
  g_signal_emit_by_name(find(GTK_WIDGET(dialog), "Check"), "clicked");
  WAIT(host.calls[GROTTO_PROFILE_VERIFY_NIP05] == 2 && find(GTK_WIDGET(dialog), "Address matches this public key"));
  const GrottoNip05Status states[] = {GROTTO_NIP05_MISMATCH, GROTTO_NIP05_INVALID, GROTTO_NIP05_UNAVAILABLE};
  const char *messages[] = {"Address does not match this public key", "Invalid Nostr address", "Could not check address — offline or server unavailable"};
  for (guint i = 0; i < G_N_ELEMENTS(states); i++) {
    host.nip05 = states[i]; g_signal_emit_by_name(find(GTK_WIDGET(dialog), "Check"), "clicked");
    WAIT(host.calls[GROTTO_PROFILE_VERIFY_NIP05] == i + 3 && find(GTK_WIDGET(dialog), messages[i]));
  }
  gtk_editable_set_text(GTK_EDITABLE(find(GTK_WIDGET(dialog), "Nostr Address")), "changed@example.com");
  g_assert_nonnull(find(GTK_WIDGET(dialog), "Address not checked"));
  gtk_editable_set_text(GTK_EDITABLE(find(GTK_WIDGET(dialog), "Nostr Address")), values[4]);
  host.upload_fail = TRUE;
  g_signal_emit_by_name(find(GTK_WIDGET(dialog), "Choose Avatar…"), "clicked");
  WAIT(find(GTK_WIDGET(dialog), "Image upload failed. Try again"));
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(find(GTK_WIDGET(dialog), "Avatar URL"))), ==, values[2]);
  g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(row)), ==, "No profile published yet");
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
  gboolean saved_dialog_gone = FALSE; g_object_weak_ref(G_OBJECT(dialog), dialog_finalized, &saved_dialog_gone);
  g_signal_emit_by_name(find(GTK_WIDGET(dialog), "Save"), "clicked");
  WAIT(host.saved);
  WAIT(!adw_window_get_visible_dialog(window) && saved_dialog_gone);
  g_assert_true(host.saved->bot);
  g_assert_cmpstr(host.saved->fields[GROTTO_PROFILE_NAME], ==, "alice");
  g_assert_cmpstr(host.saved->fields[GROTTO_PROFILE_ABOUT], ==, "First line\nSecond line");
  g_assert_cmpstr(host.saved->fields[GROTTO_PROFILE_DISPLAY_NAME], ==, "Alice");
  for (guint i = 2; i < G_N_ELEMENTS(values); i++) g_assert_cmpstr(host.saved->fields[i + 1], ==, values[i]);
  g_assert_nonnull(find(panel, "Alice"));
  g_signal_emit_by_name(find(panel, "Edit Profile…"), "clicked");
  dialog = adw_window_get_visible_dialog(window); frames = 0;
  gtk_widget_add_tick_callback(GTK_WIDGET(dialog), presented, &frames, NULL); WAIT(frames == 3);
  gboolean cancelled_dialog_gone = FALSE; g_object_weak_ref(G_OBJECT(dialog), dialog_finalized, &cancelled_dialog_gone);
  gtk_editable_set_text(GTK_EDITABLE(find(GTK_WIDGET(dialog), "Name")), "unsaved-draft");
  g_signal_emit_by_name(find(GTK_WIDGET(dialog), "Cancel"), "clicked");
  WAIT(!adw_window_get_visible_dialog(window) && cancelled_dialog_gone);
  g_assert_cmpuint(host.calls[GROTTO_PROFILE_SAVE], ==, 3);
  g_assert_cmpstr(host.saved->fields[GROTTO_PROFILE_NAME], ==, "alice");
  g_signal_emit_by_name(find(panel, "Edit Profile…"), "clicked");
  dialog = adw_window_get_visible_dialog(window); frames = 0;
  gtk_widget_add_tick_callback(GTK_WIDGET(dialog), presented, &frames, NULL); WAIT(frames == 3);
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(find(GTK_WIDGET(dialog), "Name"))), ==, "alice");
  gboolean unchanged_dialog_gone = FALSE; g_object_weak_ref(G_OBJECT(dialog), dialog_finalized, &unchanged_dialog_gone);
  g_signal_emit_by_name(find(GTK_WIDGET(dialog), "Save"), "clicked");
  WAIT(!adw_window_get_visible_dialog(window) && unchanged_dialog_gone);
  grotto_profile_info_free(host.info); host.info = grotto_profile_info_copy(host.saved); host.info->published = TRUE; host.offline = TRUE;
  g_signal_emit_by_name(find(panel, "Refresh Profile"), "clicked");
  WAIT(find(panel, "Offline — showing the saved profile"));
  identity.watch_only = TRUE; identity.can_sign = FALSE;
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_IDENTITIES, &identity, NULL));
  g_assert_false(gtk_widget_get_visible(find(panel, "Edit Profile…")));
  g_assert_nonnull(find(panel, "Nostr Address"));
  g_assert_cmpuint(host.calls[GROTTO_PROFILE_LOAD_IMAGES], ==, 0);
  g_assert_null(find(panel, "Load Profile Images…"));
  host.info->tor_blocked = TRUE;
  g_signal_emit_by_name(find(panel, "Refresh Profile"), "clicked");
  WAIT(find(panel, "Tor mode — showing the saved profile without network access"));
  g_assert_cmpuint(host.calls[GROTTO_PROFILE_SAVE], ==, 4);
  gtk_window_destroy(GTK_WINDOW(window)); g_object_unref(window);
  grotto_profile_info_free(host.info); grotto_profile_info_free(host.saved);
}
static AdwAvatar *row_avatar(GtkWidget *widget) {
  if (ADW_IS_AVATAR(widget)) return ADW_AVATAR(widget);
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    AdwAvatar *avatar = row_avatar(child); if (avatar) return avatar;
  }
  return NULL;
}
typedef struct { guint calls; GTask *pending; } AvatarHost;
static void cached_avatar_host(gpointer data, GrottoProfileAction action, const char *key, const GrottoProfileInfo *info,
  GCancellable *cancel, GAsyncReadyCallback callback, gpointer user) {
  (void)info; AvatarHost *host = data; host->calls++;
  g_assert_cmpint(action, ==, GROTTO_PROFILE_FETCH_CACHED); g_assert_cmpstr(key, ==, "other-key");
  g_assert_null(host->pending); host->pending = g_task_new(NULL, cancel, callback, user);
}
static void cached_identity_avatar(void) {
  g_autoptr(GrottoPanelsFakeService) fake = grotto_panels_fake_service_new();
  GrottoPanelsService *service = GROTTO_PANELS_SERVICE(fake);
  grotto_panels_fake_service_set_available(fake, TRUE);
  GrottoPanelsMetadata identity = {.id = "main", .public_key = "main-key", .label = "Main", .is_default = TRUE};
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_IDENTITIES, &identity, NULL));
  identity.id = "other"; identity.public_key = "other-key"; identity.label = "Other"; identity.is_default = FALSE;
  g_autoptr(GrottoPanelsItem) other = grotto_panels_item_new(&identity, NULL);
  g_list_store_append(G_LIST_STORE(grotto_panels_service_get_model(service, GROTTO_PANELS_IDENTITIES)), other);
  g_autoptr(GtkWidget) panel = g_object_ref_sink(grotto_identity_panel_new(service, NULL));
  AvatarHost host = {0}; const GrottoPanelsHost callbacks = {.profile_async = cached_avatar_host};
  grotto_panel_set_host(GROTTO_PANEL(panel), &callbacks, &host, NULL);
  WAIT(host.calls == 1);
  AdwAvatar *avatar = row_avatar(find(panel, "Other")); g_assert_nonnull(avatar);
  g_assert_null(adw_avatar_get_custom_image(avatar));
  g_assert_true(adw_avatar_get_show_initials(avatar));
  g_autoptr(GdkPixbuf) pixels = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 2, 2); gdk_pixbuf_fill(pixels, 0x224488ff);
  g_autoptr(GrottoProfileInfo) profile = grotto_profile_info_copy(NULL);
  profile->published = TRUE; profile->created_at = 200; profile->images[0] = gdk_texture_new_for_pixbuf(pixels);
  grotto_panels_service_set_profile(service, "other-key", profile);
  g_assert_true(adw_avatar_get_custom_image(avatar) == GDK_PAINTABLE(profile->images[0]));
  /* A delayed, older cache-only reply must not clear the refreshed avatar. */
  g_autoptr(GrottoProfileInfo) older = grotto_profile_info_copy(NULL); older->published = TRUE; older->created_at = 100;
  gboolean reply_gone = FALSE; g_object_weak_ref(G_OBJECT(host.pending), dialog_finalized, &reply_gone);
  g_task_return_pointer(host.pending, grotto_profile_info_copy(older), (GDestroyNotify)grotto_profile_info_free); g_clear_object(&host.pending);
  WAIT(reply_gone);
  while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
  g_assert_true(adw_avatar_get_custom_image(avatar) == GDK_PAINTABLE(profile->images[0]));
  /* Relay-only changes must update the snapshot even when kind 0 is unchanged. */
  profile->publish_relays = g_strsplit("wss://outbox.example/", " ", -1);
  profile->bootstrap_used = TRUE; profile->bootstrap_relays = g_strsplit("wss://discovery.example/", " ", -1);
  grotto_panels_service_set_profile(service, "other-key", profile);
  g_autoptr(GrottoProfileInfo) relays = grotto_panels_service_dup_profile(service, "other-key");
  g_assert_true(relays->bootstrap_used); g_assert_cmpstr(relays->publish_relays[0], ==, "wss://outbox.example/");
  g_assert_cmpstr(relays->bootstrap_relays[0], ==, "wss://discovery.example/");
  /* Model rebuild/identity switch reuses the cache; it must not call FETCH. */
  g_signal_emit_by_name(service, "capabilities-changed");
  avatar = row_avatar(find(panel, "Other"));
  g_assert_true(adw_avatar_get_custom_image(avatar) == GDK_PAINTABLE(profile->images[0])); g_assert_cmpuint(host.calls, ==, 1);
  grotto_panels_service_set_profile(service, "other-key", older);
  g_autoptr(GrottoProfileInfo) winner = grotto_panels_service_dup_profile(service, "other-key");
  g_assert_cmpuint(winner->created_at, ==, 200);
  g_assert_true(winner->images[0] == profile->images[0]);
}
static void assert_no_loading(GtkWidget *widget) {
  g_assert_null(find(widget, "Loading profile…"));
  if (GTK_IS_SPINNER(widget)) {
    g_assert_false(gtk_spinner_get_spinning(GTK_SPINNER(widget)));
    g_assert_false(gtk_widget_get_visible(widget));
  }
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) assert_no_loading(child);
}
typedef struct { GTask *pending, *cache_pending; guint fetches, checks; } ViewHost;
static void view_profile(gpointer data, GrottoProfileAction action, const char *key, const GrottoProfileInfo *info,
  GCancellable *cancel, GAsyncReadyCallback callback, gpointer user) {
  (void)key; (void)info; ViewHost *host = data;
  if (action == GROTTO_PROFILE_FETCH_CACHED) {
    host->checks++; g_assert_null(host->cache_pending); host->cache_pending = g_task_new(NULL, cancel, callback, user);
  } else {
    g_assert_cmpint(action, ==, GROTTO_PROFILE_FETCH);
    host->fetches++; g_assert_null(host->pending); host->pending = g_task_new(NULL, cancel, callback, user);
  }
}
static void finish_view_read(ViewHost *host, GtkWidget *panel, const GrottoProfileInfo *info) {
  g_assert_nonnull(host->pending);
  g_task_return_pointer(host->pending, grotto_profile_info_copy(info), (GDestroyNotify)grotto_profile_info_free); g_clear_object(&host->pending);
  WAIT(gtk_widget_get_sensitive(find(panel, "Refresh Profile")));
}
static void cache_first_live_view(void) {
  g_autoptr(GrottoPanelsFakeService) fake = grotto_panels_fake_service_new();
  GrottoPanelsService *service = GROTTO_PANELS_SERVICE(fake); grotto_panels_fake_service_set_available(fake, TRUE);
  GrottoPanelsMetadata identity = {.id = "identity", .public_key = "cached-key", .label = "Local", .is_default = TRUE, .can_sign = TRUE, .backup_acknowledged = TRUE};
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_IDENTITIES, &identity, NULL));
  g_autoptr(GrottoProfileInfo) cached = grotto_profile_info_copy(NULL); cached->published = TRUE; cached->created_at = 100;
  g_free(cached->fields[GROTTO_PROFILE_DISPLAY_NAME]); cached->fields[GROTTO_PROFILE_DISPLAY_NAME] = g_strdup("Cached profile");
  g_free(cached->fields[GROTTO_PROFILE_PICTURE]); cached->fields[GROTTO_PROFILE_PICTURE] = g_strdup("https://images.example/avatar.png");
  g_free(cached->fields[GROTTO_PROFILE_BANNER]); cached->fields[GROTTO_PROFILE_BANNER] = g_strdup("https://images.example/banner.png");
  static const guint8 pixel[] = {80, 120, 180, 255}; g_autoptr(GBytes) bytes = g_bytes_new_static(pixel, sizeof pixel);
  cached->images[0] = gdk_memory_texture_new(1, 1, GDK_MEMORY_R8G8B8A8, bytes, 4); cached->images[1] = g_object_ref(cached->images[0]);
  grotto_panels_service_set_profile(service, "cached-key", cached);
  GtkWidget *panel = grotto_identity_panel_new(service, NULL);
  ViewHost host = {0}; const GrottoPanelsHost callbacks = {.profile_async = view_profile};
  grotto_panel_set_host(GROTTO_PANEL(panel), &callbacks, &host, NULL);
  g_assert_nonnull(find(panel, "Cached profile")); assert_no_loading(panel);
  AdwWindow *window = ADW_WINDOW(g_object_ref_sink(adw_window_new())); adw_window_set_content(window, panel); gtk_window_present(GTK_WINDOW(window));
  WAIT(host.pending); g_assert_cmpuint(host.checks, ==, 0); assert_no_loading(panel);
  g_assert_true(adw_avatar_get_custom_image(row_avatar(panel)) == GDK_PAINTABLE(cached->images[0]));
  g_autoptr(GrottoProfileInfo) newer = grotto_profile_info_copy(cached); newer->created_at = 200;
  g_free(newer->fields[GROTTO_PROFILE_DISPLAY_NAME]); newer->fields[GROTTO_PROFILE_DISPLAY_NAME] = g_strdup("Live newer profile");
  grotto_panels_service_set_profile(service, "cached-key", newer);
  g_assert_nonnull(find(panel, "Live newer profile")); assert_no_loading(panel);
  finish_view_read(&host, panel, cached); /* A delayed older FETCH cannot regress the signal's winner. */
  g_assert_nonnull(find(panel, "Live newer profile")); g_assert_null(find(panel, "Cached profile"));
  g_signal_emit_by_name(find(panel, "Edit Profile…"), "clicked");
  AdwDialog *dialog = adw_window_get_visible_dialog(window); g_assert_nonnull(dialog);
  guint frames = 0; gtk_widget_add_tick_callback(GTK_WIDGET(dialog), presented, &frames, NULL); WAIT(frames == 3);
  gtk_editable_set_text(GTK_EDITABLE(find(GTK_WIDGET(dialog), "Name")), "unsaved-draft");
  GtkTextBuffer *about = gtk_text_view_get_buffer(about_view(GTK_WIDGET(dialog))); gtk_text_buffer_set_text(about, "Unsaved about", -1);
  guint fetches = host.fetches;
  GtkWidget *profile_box = GTK_WIDGET(gtk_widget_get_template_child(panel, GROTTO_TYPE_IDENTITY_PANEL, "profile_box"));
  gtk_widget_set_visible(profile_box, FALSE); gtk_widget_set_visible(profile_box, TRUE);
  g_assert_true(gtk_widget_get_mapped(profile_box)); g_assert_cmpuint(host.fetches, ==, fetches); g_assert_null(host.pending);
  newer->created_at = 300; grotto_panels_service_set_profile(service, "cached-key", newer);
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(find(GTK_WIDGET(dialog), "Name"))), ==, "unsaved-draft");
  GtkTextIter start, end; gtk_text_buffer_get_bounds(about, &start, &end);
  g_autofree char *draft = gtk_text_buffer_get_text(about, &start, &end, FALSE); g_assert_cmpstr(draft, ==, "Unsaved about");
  g_signal_emit_by_name(find(GTK_WIDGET(dialog), "Cancel"), "clicked"); WAIT(!adw_window_get_visible_dialog(window));
  grotto_panels_service_set_profile(service, "other-key", cached);
  identity.public_key = "other-key"; g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_IDENTITIES, &identity, NULL));
  g_assert_nonnull(find(panel, "Cached profile")); assert_no_loading(panel);
  g_assert_true(adw_avatar_get_custom_image(row_avatar(panel)) == GDK_PAINTABLE(cached->images[0])); finish_view_read(&host, panel, cached);
  identity.public_key = "cached-key"; g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_IDENTITIES, &identity, NULL));
  g_assert_nonnull(find(panel, "Live newer profile")); assert_no_loading(panel);
  g_assert_true(adw_avatar_get_custom_image(row_avatar(panel)) == GDK_PAINTABLE(cached->images[0])); finish_view_read(&host, panel, newer);
  /* An initially cold in-process cache probes disk before showing any spinner. */
  identity.public_key = "disk-key"; g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_IDENTITIES, &identity, NULL));
  g_assert_nonnull(host.cache_pending); assert_no_loading(panel);
  g_autoptr(GrottoProfileInfo) missing = grotto_profile_info_copy(cached);
  g_clear_object(&missing->images[0]); g_clear_object(&missing->images[1]);
  /* Hydration completes before a held, same-revision cache-only snapshot. Both
   * that reply and a delayed FETCH must preserve the images already loaded. */
  grotto_panels_service_set_profile(service, "disk-key", cached);
  g_task_return_pointer(host.cache_pending, grotto_profile_info_copy(missing), (GDestroyNotify)grotto_profile_info_free); g_clear_object(&host.cache_pending);
  WAIT(host.pending); g_assert_nonnull(find(panel, "Cached profile")); assert_no_loading(panel);
  g_assert_true(adw_avatar_get_custom_image(row_avatar(panel)) == GDK_PAINTABLE(cached->images[0]));
  finish_view_read(&host, panel, missing); g_assert_cmpuint(host.checks, ==, 1);
  g_autoptr(GrottoProfileInfo) hydrated = grotto_panels_service_dup_profile(service, "disk-key");
  g_assert_true(hydrated->images[0] == cached->images[0]); g_assert_true(hydrated->images[1] == cached->images[1]);
  g_assert_true(adw_avatar_get_custom_image(row_avatar(panel)) == GDK_PAINTABLE(cached->images[0]));
  /* Matching timestamps alone must not keep an image belonging to another URL. */
  g_autoptr(GrottoProfileInfo) changed = grotto_profile_info_copy(missing);
  g_free(changed->fields[GROTTO_PROFILE_PICTURE]); changed->fields[GROTTO_PROFILE_PICTURE] = g_strdup("https://images.example/new-avatar.png");
  g_free(changed->fields[GROTTO_PROFILE_BANNER]); changed->fields[GROTTO_PROFILE_BANNER] = g_strdup("https://images.example/new-banner.png");
  grotto_panels_service_set_profile(service, "disk-key", changed);
  g_autoptr(GrottoProfileInfo) changed_cached = grotto_panels_service_dup_profile(service, "disk-key");
  g_assert_null(changed_cached->images[0]); g_assert_null(changed_cached->images[1]);
  g_assert_null(adw_avatar_get_custom_image(row_avatar(panel)));
  /* Switching away from a held cold FETCH must not carry its loading label
   * into another identity's disk-cache probe or accept its cancelled reply. */
  identity.public_key = "uncached-key"; g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_IDENTITIES, &identity, NULL));
  g_assert_nonnull(host.cache_pending); g_task_return_new_error(host.cache_pending, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No cached profile"); g_clear_object(&host.cache_pending);
  WAIT(host.pending); g_assert_nonnull(find(panel, "Loading profile…"));
  identity.public_key = "disk-again"; g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_IDENTITIES, &identity, NULL));
  g_assert_nonnull(host.cache_pending); assert_no_loading(panel);
  g_assert_true(g_cancellable_is_cancelled(g_task_get_cancellable(host.pending)));
  g_assert_true(g_task_return_error_if_cancelled(host.pending)); g_clear_object(&host.pending);
  g_task_return_pointer(host.cache_pending, grotto_profile_info_copy(cached), (GDestroyNotify)grotto_profile_info_free); g_clear_object(&host.cache_pending);
  WAIT(host.pending); g_assert_nonnull(find(panel, "Cached profile")); assert_no_loading(panel); finish_view_read(&host, panel, cached);
  gtk_window_destroy(GTK_WINDOW(window)); g_object_unref(window);
  newer->created_at++; grotto_panels_service_set_profile(service, "disk-key", newer); /* Disconnected on disposal. */
}
void register_profile_panel_tests(void) {
  g_test_add_func("/grotto-panels/profile/editor", profile_panel);
  g_test_add_func("/grotto-panels/profile/cached-identity-avatar", cached_identity_avatar);
  g_test_add_func("/grotto-panels/profile/cache-first-live", cache_first_live_view);
}
