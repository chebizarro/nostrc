/* SPDX-License-Identifier: MIT */
#include "fake-service.h"
#include <glib/gstdio.h>
#ifdef __linux__
#include <dlfcn.h>
static guint catalog_reads;
/* Count actual production-DSO catalog calls without replacing desktop data. */
GList *g_app_info_get_all(void) {
  GList *(*original)(void) = (GList *(*)(void))dlsym(RTLD_NEXT, "g_app_info_get_all");
  g_assert_nonnull(original);
  catalog_reads++;
  return original();
}
#endif
static gboolean timeout(gpointer data) { g_error("Management panel barrier: %s", (char *)data); return G_SOURCE_REMOVE; }
#define WAIT(c) do { guint timer = g_timeout_add_seconds(10, timeout, #c); while (!(c)) g_main_context_iteration(NULL, TRUE); g_source_remove(timer); } while (0)
static void gone(gpointer data, GObject *object) { (void)object; *(gboolean *)data = TRUE; }
static gboolean can(gpointer data, GrottoPanelCommand command) { return grotto_panels_service_can(data, command); }
static void command(gpointer data, GrottoPanelCommand cmd, const GrottoPanelCommandArgs *args, GCancellable *cancel, GAsyncReadyCallback callback, gpointer user) {
  grotto_panels_service_invoke_async(data, cmd, args, cancel, callback, user);
}
static GtkWidget *find_title(GtkWidget *widget, const char *title) {
  if (ADW_IS_PREFERENCES_ROW(widget) && !g_strcmp0(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(widget)), title)) return widget;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_title(child, title); if (found) return found;
  }
  return NULL;
}
static gboolean has_label(GtkWidget *widget, const char *text) {
  if (GTK_IS_LABEL(widget) && strstr(gtk_label_get_text(GTK_LABEL(widget)), text)) return TRUE;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
    if (has_label(child, text)) return TRUE;
  return FALSE;
}
static gboolean has_icon(GtkWidget *widget, GIcon *icon) {
  if (GTK_IS_IMAGE(widget) && gtk_image_get_storage_type(GTK_IMAGE(widget)) == GTK_IMAGE_GICON &&
      g_icon_equal(gtk_image_get_gicon(GTK_IMAGE(widget)), icon)) return TRUE;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
    if (has_icon(child, icon)) return TRUE;
  return FALSE;
}
static void management_panels(void) {
  g_autoptr(GIcon) expected_icon = g_themed_icon_new("grotto-test-app");
  const char *principals[] = {"exe:/usr/bin/true", "app:org.nostr.GrottoTest", "flatpak:org.nostr.GrottoTest", "snap:org.nostr.GrottoTest;exe:/usr/bin/true", "app:org.nostr.Missing;exe:/usr/bin/true"};
  for (guint i = 0; i < G_N_ELEMENTS(principals); i++) {
    g_autoptr(GIcon) icon = NULL;
    g_autofree char *name = grotto_panels_app_display_info(principals[i], principals[i], &icon);
    g_assert_cmpstr(name, ==, "Friendly Test App");
    g_assert_true(g_icon_equal(icon, expected_icon));
  }
#ifdef __linux__
  guint reads = catalog_reads;
  for (guint i = 0; i < 200; i++) {
    g_autofree char *name = grotto_panels_app_display_info(principals[0], NULL, NULL);
    g_assert_cmpstr(name, ==, "Friendly Test App");
  }
  g_assert_cmpuint(catalog_reads, ==, reads);
  g_autoptr(GAppInfoMonitor) monitor = g_app_info_monitor_get();
  g_signal_emit_by_name(monitor, "changed");
  g_assert_cmpuint(catalog_reads, ==, reads + 1);
#endif
  g_autofree char *spoof = grotto_panels_app_display_info("exe:/tmp/review-other/true", NULL, NULL);
  g_assert_cmpstr(spoof, ==, "true");
  g_autofree char *mismatch = grotto_panels_app_display_info("app:org.nostr.GrottoTest;exe:/opt/unrelated/client", NULL, NULL);
  g_assert_cmpstr(mismatch, ==, "client");
  g_autoptr(GIcon) fallback_icon = NULL;
  g_autofree char *fallback = grotto_panels_app_display_info("exe:/opt/no-desktop/unknown-client", NULL, &fallback_icon);
  g_assert_cmpstr(fallback, ==, "unknown-client");
  g_autoptr(GIcon) generic = g_themed_icon_new("application-x-executable-symbolic");
  g_assert_true(g_icon_equal(fallback_icon, generic));
  g_autofree char *custom = grotto_panels_app_display_info(principals[0], "<b>Plain App</b>", NULL);
  g_assert_cmpstr(custom, ==, "<b>Plain App</b>");
  g_autoptr(GrottoPanelsFakeService) fake = grotto_panels_fake_service_new();
  GrottoPanelsService *service = GROTTO_PANELS_SERVICE(fake);
  grotto_panels_fake_service_set_available(fake, TRUE);
  for (guint i = 0; i < GROTTO_PANEL_N_COMMANDS; i++) grotto_panels_fake_service_set_can(fake, i, TRUE);
  const char *relays[] = {"wss://nos.lol", NULL}, *methods[] = {"sign_event", NULL};
  const char *effective[] = {"Public key after consent", "Public publishing", NULL};
  GrottoPanelsMetadata app = {.effective_permissions = effective, .id = "exe:/usr/bin/true", .label = "exe:/usr/bin/true", .trust = "medium", .access_policy = "Ask for sensitive operations", .transport = "Local App"};
  GrottoPanelsMetadata status = {.id = "status", .state = "LISTENING", .remote_enabled = TRUE, .relays = relays, .methods = methods};
  GrottoPanelsMetadata request = {.id = "request-1", .label = "Plain App", .operation = "Sign Event", .deadline_usec = g_get_monotonic_time() + 60000000};
  GrottoPanelsMetadata record = {.id = "11", .principal = app.id, .operation = "event", .outcome = "success", .sequence = 11, .has_event_kind = TRUE, .event_kind = 1};
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_APPS, &app, NULL));
  GrottoPanelsMetadata other = app; other.id = "exe:/usr/bin/false"; other.label = other.id;
  g_autoptr(GrottoPanelsItem) other_item = grotto_panels_item_new(&other, NULL);
  g_list_store_append(G_LIST_STORE(grotto_panels_service_get_model(service, GROTTO_PANELS_APPS)), other_item);
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_STATUS, &status, NULL));
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_REQUESTS, &request, NULL));
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_ACTIVITY, &record, NULL));
  gsize count = 0; const GrottoPanelDescriptor *descriptors = grotto_panels_get_descriptors(&count); g_assert_cmpuint(count, ==, 4);
  GtkWidget *(*constructors[])(GrottoPanelsService *) = {grotto_apps_panel_new, grotto_remote_panel_new, grotto_activity_panel_new};
  for (guint i = 0; i < G_N_ELEMENTS(constructors); i++) {
    gboolean destroyed = FALSE; GrottoPanel *panel = GROTTO_PANEL(constructors[i](service));
    g_object_weak_ref(G_OBJECT(panel), gone, &destroyed);
    /* Even an approver-capable read service is not a trusted GUI host. */
    g_assert_false(grotto_panel_can_command(panel, GROTTO_PANEL_SET_CLIENT_TRUST));
    g_assert_false(grotto_panel_can_command(panel, GROTTO_PANEL_CONFIGURE_REMOTE));
    GrottoPanelsHost host = {.can_command = can, .command_async = command}; grotto_panel_set_host(panel, &host, service, NULL);
    GtkWidget *window = adw_window_new(); g_object_ref_sink(window); gtk_window_set_default_size(GTK_WINDOW(window), 360, 640);
    GtkWidget *split = adw_navigation_split_view_new(); adw_navigation_split_view_set_sidebar(ADW_NAVIGATION_SPLIT_VIEW(split), ADW_NAVIGATION_PAGE(adw_navigation_page_new(gtk_label_new("Panels"), "Panels")));
    adw_navigation_split_view_set_content(ADW_NAVIGATION_SPLIT_VIEW(split), ADW_NAVIGATION_PAGE(panel));
    adw_navigation_split_view_set_collapsed(ADW_NAVIGATION_SPLIT_VIEW(split), TRUE); adw_navigation_split_view_set_show_content(ADW_NAVIGATION_SPLIT_VIEW(split), TRUE);
    adw_window_set_content(ADW_WINDOW(window), split); gtk_window_present(GTK_WINDOW(window)); WAIT(gtk_widget_get_mapped(GTK_WIDGET(panel)));
    g_assert_cmpstr(adw_navigation_page_get_tag(ADW_NAVIGATION_PAGE(panel)), ==, descriptors[i + 1].id);
    if (i == 0) {
      GtkWidget *apps = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(panel), GROTTO_TYPE_APPS_PANEL, "apps_list"));
      GtkWidget *first = gtk_widget_get_first_child(apps); g_assert_nonnull(first);
      g_assert_false(adw_preferences_row_get_use_markup(ADW_PREFERENCES_ROW(first)));
      g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(first)), ==, "Friendly Test App");
      g_assert_nonnull(strstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(first)), app.id));
      g_assert_true(has_icon(first, expected_icon));
      GtkWidget *second = gtk_widget_get_next_sibling(first);
      g_assert_nonnull(second);
      g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(second)), ==, "Friendly Test App");
      g_assert_nonnull(strstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(second)), other.id));
      g_assert_cmpstr(g_object_get_data(G_OBJECT(first), "principal"), !=, g_object_get_data(G_OBJECT(second), "principal"));
      g_signal_emit_by_name(first, "activated");
      AdwNavigationView *nav = grotto_apps_panel_get_navigation(GROTTO_APPS_PANEL(panel));
      GtkWidget *detail = GTK_WIDGET(adw_navigation_view_get_visible_page(nav));
      g_assert_cmpstr(adw_navigation_page_get_title(ADW_NAVIGATION_PAGE(detail)), ==, "Friendly Test App");
      GtkWidget *identity = find_title(detail, "Friendly Test App");
      g_assert_nonnull(identity);
      g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(identity)), ==, app.id);
      GtkWidget *fingerprint = find_title(detail, "Fingerprint");
      g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(fingerprint)), ==, app.id);
      g_assert_true(has_icon(detail, expected_icon));
      GtkWidget *low_row = find_title(detail, "Ask Unless Permitted"); g_assert_nonnull(low_row);
      GtkWidget *low = adw_action_row_get_activatable_widget(ADW_ACTION_ROW(low_row));
      gtk_check_button_set_active(GTK_CHECK_BUTTON(low), TRUE);
      WAIT(grotto_panels_fake_service_get_invocations(fake) == 1);
      g_assert_cmpint(grotto_panels_fake_service_get_last_command(fake), ==, GROTTO_PANEL_SET_CLIENT_TRUST);
      g_assert_cmpstr(grotto_panels_fake_service_get_last_args(fake)->trust, ==, "low");
      GrottoPanelsMetadata permissions_data[] = {{.id = "0", .principal = app.id, .public_key = "public-identity", .operation = "event", .decision = "allow", .state = "active", .has_event_kind = TRUE, .event_kind = 3, .revision = 7},
        {.id = "1", .principal = app.id, .public_key = "public-identity", .operation = "event", .decision = "policy-review", .state = "inactive", .reason = "Pending policy review", .event_kind = 1, .revision = 7}};
      grotto_panels_fake_service_set_query_result(fake, GROTTO_PANELS_QUERY_PERMISSIONS, permissions_data, G_N_ELEMENTS(permissions_data), -1);
      GtkWidget *permissions = find_title(detail, "Permissions"); g_assert_nonnull(permissions); g_signal_emit_by_name(permissions, "activated");
      WAIT(find_title(detail, "Allow · Follow list"));
      GtkWidget *derived = find_title(detail, effective[0]); g_assert_nonnull(derived);
      g_assert_false(gtk_list_box_row_get_activatable(GTK_LIST_BOX_ROW(derived)));
      g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(derived)), ==, "From Medium trust");
      /* Kind wording comes from the generated registry; locate the second row by metadata. */
      GtkWidget *explicit_row = find_title(detail, "Allow · Follow list");
      GtkWidget *inactive = gtk_widget_get_next_sibling(explicit_row); g_assert_true(ADW_IS_ACTION_ROW(inactive));
      g_assert_nonnull(strstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(inactive)), "Inactive — pending policy review"));
      g_assert_nonnull(strstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(inactive)), "Until Revoked"));
      GtkWidget *clear = find_title(detail, "Clear Remembered Permissions…"); g_assert_nonnull(clear);
      g_assert_true(gtk_widget_is_sensitive(clear));
      grotto_panels_fake_service_set_can(fake, GROTTO_PANEL_SET_PERMISSIONS, FALSE);
      WAIT(!gtk_list_box_row_get_activatable(GTK_LIST_BOX_ROW(explicit_row)));
      g_assert_false(gtk_widget_is_sensitive(clear));
      g_assert_true(gtk_widget_is_sensitive(explicit_row)); /* Manager reads remain visible. */
      grotto_panels_fake_service_set_can(fake, GROTTO_PANEL_SET_CLIENT_TRUST, FALSE);
      /* Capability loss reaches an already-mounted detail, not only a new panel. */
      WAIT(!gtk_widget_is_sensitive(low));
      app.label = "<b>Plain App</b>";
      g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_APPS, &app, NULL));
      WAIT(!g_strcmp0(adw_navigation_page_get_title(ADW_NAVIGATION_PAGE(detail)), app.label));
      first = gtk_widget_get_first_child(apps);
      g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(first)), ==, app.label);
      g_assert_false(adw_preferences_row_get_use_markup(ADW_PREFERENCES_ROW(first)));
    }
    if (GROTTO_IS_ACTIVITY_PANEL(panel)) {
      WAIT(has_label(GTK_WIDGET(panel), "Friendly Test App · Sign Event"));
      g_assert_true(has_label(GTK_WIDGET(panel), app.id));
      g_assert_true(has_icon(GTK_WIDGET(panel), expected_icon));
      GrottoPanelsMetadata older[] = {
        {.id = "11", .sequence = 11, .principal = app.id, .operation = "event", .outcome = "success"},
        {.id = "10", .sequence = 10, .principal = "app:other", .operation = "event", .outcome = "denied"}
      };
      grotto_panels_fake_service_set_query_result(fake, GROTTO_PANELS_QUERY_ACTIVITY, older, 2, -1);
      GtkListView *records = GTK_LIST_VIEW(gtk_widget_get_template_child(GTK_WIDGET(panel), GROTTO_TYPE_ACTIVITY_PANEL, "records"));
      GtkWidget *load = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(panel), GROTTO_TYPE_ACTIVITY_PANEL, "older"));
      g_signal_emit_by_name(load, "clicked");
      WAIT(g_list_model_get_n_items(G_LIST_MODEL(gtk_list_view_get_model(records))) == 2); /* Sequence 11 is deduplicated. */
      grotto_activity_panel_filter_app(GROTTO_ACTIVITY_PANEL(panel), app.id);
      WAIT(g_list_model_get_n_items(G_LIST_MODEL(gtk_list_view_get_model(records))) == 1);
      grotto_activity_panel_filter_app(GROTTO_ACTIVITY_PANEL(panel), NULL);
      WAIT(g_list_model_get_n_items(G_LIST_MODEL(gtk_list_view_get_model(records))) == 2);
      grotto_panels_fake_service_set_query_result(fake, GROTTO_PANELS_QUERY_ACTIVITY, NULL, 0, -1);
      g_signal_emit_by_name(load, "clicked"); WAIT(!gtk_widget_get_sensitive(load));
    } else if (GROTTO_IS_REMOTE_PANEL(panel)) {
      AdwEntryRow *entry = ADW_ENTRY_ROW(gtk_widget_get_template_child(GTK_WIDGET(panel), GROTTO_TYPE_REMOTE_PANEL, "new_relay"));
      guint before = grotto_panels_fake_service_get_invocations(fake);
      gtk_editable_set_text(GTK_EDITABLE(entry), "wss://relay.nostr.band"); g_signal_emit_by_name(entry, "apply");
      WAIT(grotto_panels_fake_service_get_invocations(fake) == before + 1 && gtk_widget_get_sensitive(GTK_WIDGET(entry)));
      g_assert_cmpint(grotto_panels_fake_service_get_last_command(fake), ==, GROTTO_PANEL_CONFIGURE_REMOTE);
      const GrottoPanelCommandArgs *args = grotto_panels_fake_service_get_last_args(fake);
      g_assert_true(args->remote_enabled); g_assert_cmpstr(args->relays[0], ==, relays[0]);
      g_assert_cmpstr(args->relays[1], ==, "wss://relay.nostr.band"); g_assert_null(args->relays[2]);
      g_assert_cmpstr(args->methods[0], ==, methods[0]);
      /* Invalid URL fails in public admission, before the host or fake sees it. */
      gtk_editable_set_text(GTK_EDITABLE(entry), "https://app.example?token=private"); g_signal_emit_by_name(entry, "apply");
      WAIT(gtk_widget_get_sensitive(GTK_WIDGET(entry)));
      g_assert_cmpuint(grotto_panels_fake_service_get_invocations(fake), ==, before + 1);
      grotto_panels_fake_service_set_can(fake, GROTTO_PANEL_CONFIGURE_REMOTE, FALSE);
      WAIT(!gtk_widget_get_sensitive(GTK_WIDGET(entry)));
    }
    grotto_panel_set_service(panel, NULL);
    gtk_window_destroy(GTK_WINDOW(window)); g_object_unref(window); WAIT(destroyed);
  }
}
static GtkWidget *identity_child(GtkWidget *panel, const char *name) {
  return GTK_WIDGET(gtk_widget_get_template_child(panel, GROTTO_TYPE_IDENTITY_PANEL, name));
}
static void identity_actions(void) {
  g_autoptr(GrottoPanelsFakeService) fake = grotto_panels_fake_service_new();
  grotto_panels_fake_service_set_available(fake, TRUE);
  grotto_panels_fake_service_set_can(fake, GROTTO_PANEL_SET_DEFAULT_IDENTITY, TRUE);
  grotto_panels_fake_service_set_can(fake, GROTTO_PANEL_DISMISS_BACKUP_BANNER, TRUE);
  GrottoPanelsMetadata m = {.id = "identity", .public_key = "identity-public-key", .can_sign = TRUE};
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_IDENTITIES, &m, NULL));
  GtkWidget *panel = grotto_identity_panel_new(GROTTO_PANELS_SERVICE(fake), m.public_key);
  GtkWidget *window = adw_window_new(); g_object_ref_sink(window); adw_window_set_content(ADW_WINDOW(window), panel);
  gtk_window_present(GTK_WINDOW(window)); WAIT(gtk_widget_get_mapped(panel));
  GtkWidget *use = identity_child(panel, "set_default"), *badge = identity_child(panel, "default_check");
  g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(use)), ==, "Use as Default");
  g_assert_true(gtk_widget_is_visible(use)); g_assert_false(gtk_widget_get_visible(badge));
  g_signal_emit_by_name(use, "clicked");
  WAIT(grotto_panels_fake_service_get_invocations(fake) == 1 && gtk_widget_get_sensitive(use));
  g_assert_cmpint(grotto_panels_fake_service_get_last_command(fake), ==, GROTTO_PANEL_SET_DEFAULT_IDENTITY);
  g_assert_cmpstr(grotto_panels_fake_service_get_last_args(fake)->public_key, ==, m.public_key);
  m.is_default = TRUE;
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_IDENTITIES, &m, NULL));
  g_assert_false(gtk_widget_get_visible(use)); g_assert_true(gtk_widget_get_visible(badge));
  GtkWidget *dismiss = identity_child(panel, "dismiss_backup");
  AdwBanner *banner = ADW_BANNER(identity_child(panel, "banner"));
  g_assert_true(adw_banner_get_revealed(banner)); g_assert_true(gtk_widget_get_visible(dismiss));
  g_signal_emit_by_name(dismiss, "clicked");
  WAIT(grotto_panels_fake_service_get_invocations(fake) == 2 && gtk_widget_get_sensitive(dismiss));
  g_assert_cmpint(grotto_panels_fake_service_get_last_command(fake), ==, GROTTO_PANEL_DISMISS_BACKUP_BANNER);
  g_assert_cmpstr(grotto_panels_fake_service_get_last_args(fake)->public_key, ==, m.public_key);
  g_autoptr(GrottoPanelsItem) item = grotto_panels_item_new_with_backup_banner_state(&m, TRUE, NULL);
  g_assert_true(grotto_panels_item_get_backup_banner_dismissed(item));
  GListStore *model = G_LIST_STORE(grotto_panels_service_get_model(GROTTO_PANELS_SERVICE(fake), GROTTO_PANELS_IDENTITIES));
  gpointer items[] = {item}; g_list_store_splice(model, 0, 1, items, 1);
  g_assert_false(adw_banner_get_revealed(banner));
  g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(identity_child(panel, "backup_row"))), ==, "Not backed up");
  g_assert_false(grotto_panels_item_get_metadata(item)->backup_acknowledged);
  gtk_window_destroy(GTK_WINDOW(window)); g_object_unref(window);
}
typedef struct { GrottoPanelsFakeService *service; const char *path; guint writes, pairings; } RelayHost;
static gboolean relay_host_can(gpointer data, GrottoPanelCommand cmd) {
  (void)data; return cmd == GROTTO_PANEL_CONFIGURE_REMOTE;
}
static void relay_host_read(RelayHost *host) {
  g_autoptr(GKeyFile) file = g_key_file_new();
  g_assert_true(g_key_file_load_from_file(file, host->path, G_KEY_FILE_NONE, NULL));
  g_auto(GStrv) relays = g_key_file_get_string_list(file, "remote", "relays", NULL, NULL);
  const char *methods[] = {"sign_event", NULL};
  GrottoPanelsMetadata status = {.id = "status", .state = "LISTENING", .remote_enabled = TRUE,
    .relays = (const char *const *)relays, .methods = methods};
  g_assert_true(grotto_panels_fake_service_replace(host->service, GROTTO_PANELS_STATUS, &status, NULL));
}
static void relay_host_command(gpointer data, GrottoPanelCommand cmd, const GrottoPanelCommandArgs *args,
  GCancellable *cancel, GAsyncReadyCallback callback, gpointer user) {
  RelayHost *host = data; g_assert_cmpint(cmd, ==, GROTTO_PANEL_CONFIGURE_REMOTE);
  g_autoptr(GKeyFile) file = g_key_file_new();
  g_key_file_set_string_list(file, "remote", "relays", args->relays, g_strv_length((char **)args->relays));
  g_autofree char *text = g_key_file_to_data(file, NULL, NULL);
  g_assert_true(g_file_set_contents(host->path, text, -1, NULL)); host->writes++;
  GTask *task = g_task_new(NULL, cancel, callback, user); g_task_return_boolean(task, TRUE); g_object_unref(task);
  relay_host_read(host);
}
static void relay_host_pair(GrottoRemotePanel *panel, gpointer data) { (void)panel; ((RelayHost *)data)->pairings++; }
static GtkWidget *remove_button(GtkWidget *widget) {
  if (GTK_IS_BUTTON(widget) && !g_strcmp0(gtk_widget_get_tooltip_text(widget), "Remove Relay")) return widget;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = remove_button(child); if (found) return found;
  }
  return NULL;
}
static void remote_persistence(void) {
  g_autoptr(GrottoPanelsFakeService) fake = grotto_panels_fake_service_new();
  grotto_panels_fake_service_set_available(fake, TRUE);
  const char *initial[] = {"wss://nos.lol", NULL}, *methods[] = {"sign_event", NULL};
  GrottoPanelsMetadata status = {.id = "status", .state = "ERROR", .reason = "Could not connect to wss://example.com/a&b (OFFLINE)",
    .remote_enabled = TRUE, .relays = initial, .methods = methods};
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_STATUS, &status, NULL));
  g_autofree char *dir = g_dir_make_tmp("grotto-relays-XXXXXX", NULL);
  g_autofree char *path = g_build_filename(dir, "relays.ini", NULL);
  RelayHost state = {.service = fake, .path = path};
  GrottoPanelsHost host = {.can_command = relay_host_can, .command_async = relay_host_command};
  GtkWidget *window = adw_window_new(); g_object_ref_sink(window);
  GrottoPanel *panel = GROTTO_PANEL(grotto_remote_panel_new(GROTTO_PANELS_SERVICE(fake)));
  g_signal_connect(panel, "show-connection-link", G_CALLBACK(relay_host_pair), &state);
  grotto_panel_set_host(panel, &host, &state, NULL);
  adw_window_set_content(ADW_WINDOW(window), GTK_WIDGET(panel)); gtk_window_present(GTK_WINDOW(window));
  WAIT(gtk_widget_get_mapped(GTK_WIDGET(panel)));
  GtkWidget *status_row = find_title(GTK_WIDGET(panel), "Status");
  g_assert_false(adw_preferences_row_get_use_markup(ADW_PREFERENCES_ROW(status_row)));
  g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(status_row)), ==, status.reason);
  g_signal_emit_by_name(find_title(GTK_WIDGET(panel), "Show QR Code"), "activated"); g_assert_cmpuint(state.pairings, ==, 1);
  GtkWidget *entry = find_title(GTK_WIDGET(panel), "Add Relay URL");
  const char *invalid[] = {"https://example.com", "ws://example.com", "wss://", "wss://user@example.com",
    "wss://example.onion/", "wss://EXAMPLE.ONION/", "wss://example.com/?opaque=credential",
    "wss://example.com/?", "wss://example.com/#credential", NULL};
  for (guint i = 0; invalid[i]; i++) {
    gtk_editable_set_text(GTK_EDITABLE(entry), invalid[i]); g_signal_emit_by_name(entry, "apply");
    g_assert_cmpuint(state.writes, ==, 0);
  }
  const char *valid[] = {"wss://example.com/relay", "ws://localhost:7777/", "ws://[::1]:7777/", NULL};
  for (guint i = 0; valid[i]; i++) {
    const char *relays[] = {valid[i], NULL};
    GrottoPanelCommandArgs args = {.relays = relays};
    g_assert_true(grotto_panel_command_args_validate(GROTTO_PANEL_CONFIGURE_REMOTE, &args, NULL));
  }
  gtk_editable_set_text(GTK_EDITABLE(entry), " ws://127.0.0.1:7777/ "); g_signal_emit_by_name(entry, "apply");
  WAIT(state.writes == 1 && gtk_widget_get_sensitive(entry));
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(entry)), ==, "");
  GtkWidget *added = find_title(GTK_WIDGET(panel), "ws://127.0.0.1:7777/"); g_assert_nonnull(added);
  g_signal_emit_by_name(remove_button(added), "clicked");
  WAIT(state.writes == 2 && gtk_widget_get_sensitive(entry));
  g_assert_null(find_title(GTK_WIDGET(panel), "ws://127.0.0.1:7777/"));
  gtk_editable_set_text(GTK_EDITABLE(entry), "wss://relay.nostr.band"); g_signal_emit_by_name(entry, "apply");
  WAIT(state.writes == 3 && gtk_widget_get_sensitive(entry));
  gtk_window_destroy(GTK_WINDOW(window)); g_object_unref(window);
  /* Rebuild the model and widget from only the host's durable configuration. */
  g_list_store_remove_all(G_LIST_STORE(grotto_panels_service_get_model(GROTTO_PANELS_SERVICE(fake), GROTTO_PANELS_STATUS))); relay_host_read(&state);
  window = adw_window_new(); g_object_ref_sink(window);
  GtkWidget *restored = grotto_remote_panel_new(GROTTO_PANELS_SERVICE(fake));
  adw_window_set_content(ADW_WINDOW(window), restored);
  g_assert_nonnull(find_title(restored, "wss://nos.lol"));
  g_assert_nonnull(find_title(restored, "wss://relay.nostr.band"));
  g_assert_null(find_title(restored, "ws://127.0.0.1:7777/"));
  gtk_window_destroy(GTK_WINDOW(window)); g_object_unref(window);
  g_unlink(path); g_rmdir(dir);
}
static void connection_requested(GrottoRemotePanel *panel, gpointer data) {
  guint *requests = data; (*requests)++;
  g_autofree char *uri = g_strdup_printf("bunker://79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798?relay=wss%%3A%%2F%%2Fnos.lol&secret=fixture-%u", *requests);
  grotto_remote_panel_set_connection_link(panel, uri);
}
static void connection_states(void) {
  g_autoptr(GrottoPanelsFakeService) fake = grotto_panels_fake_service_new();
  grotto_panels_fake_service_set_available(fake, TRUE);
  grotto_panels_fake_service_set_can(fake, GROTTO_PANEL_CONFIGURE_REMOTE, TRUE);
  GrottoPanelsMetadata status = {.id = "status", .state = "DISABLED"};
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_STATUS, &status, NULL));
  GtkWidget *window = adw_window_new(); g_object_ref_sink(window);
  GrottoRemotePanel *panel = GROTTO_REMOTE_PANEL(grotto_remote_panel_new(GROTTO_PANELS_SERVICE(fake)));
  GrottoPanelsHost host = {.can_command = can, .command_async = command};
  grotto_panel_set_host(GROTTO_PANEL(panel), &host, fake, NULL);
  guint requests = 0; g_signal_connect(panel, "connection-link-requested", G_CALLBACK(connection_requested), &requests);
  adw_window_set_content(ADW_WINDOW(window), GTK_WIDGET(panel)); gtk_window_present(GTK_WINDOW(window));
  WAIT(gtk_widget_get_mapped(GTK_WIDGET(panel)));
  GtkWidget *link = find_title(GTK_WIDGET(panel), "Connection Link");
  GtkWidget *qr = find_title(GTK_WIDGET(panel), "Show QR Code");
  g_assert_false(gtk_widget_get_visible(link)); g_assert_false(gtk_widget_get_visible(qr)); g_assert_cmpuint(requests, ==, 0);
  g_assert_true(has_label(GTK_WIDGET(panel), "Enable remote signing to produce a link"));
  status.remote_enabled = TRUE; status.state = "STARTING";
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_STATUS, &status, NULL));
  g_assert_cmpuint(requests, ==, 0); g_assert_true(has_label(GTK_WIDGET(panel), "when remote signing is ready"));
  status.state = "LISTENING";
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_STATUS, &status, NULL));
  g_assert_cmpuint(requests, ==, 1); g_assert_true(gtk_widget_get_visible(link));
  g_autofree char *first = g_strdup(adw_action_row_get_subtitle(ADW_ACTION_ROW(link)));
  g_assert_nonnull(strstr(first, "secret=fixture-1"));
  GtkWidget *copy = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(panel), GROTTO_TYPE_REMOTE_PANEL, "copy_link"));
  g_assert_true(gtk_widget_is_sensitive(copy)); g_signal_emit_by_name(copy, "clicked");
  GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(panel));
  g_assert_nonnull(gdk_clipboard_get_content(clipboard));
  grotto_remote_panel_set_connection_link(panel, first);
  g_assert_nonnull(gdk_clipboard_get_content(clipboard));
  /* The host replaces a consumed offer after the daemon publishes a snapshot. */
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_STATUS, &status, NULL));
  g_assert_cmpuint(requests, ==, 2);
  g_assert_cmpstr(first, !=, adw_action_row_get_subtitle(ADW_ACTION_ROW(link)));
  g_assert_null(gdk_clipboard_get_content(clipboard));
  status.remote_enabled = FALSE; status.state = "DISABLED";
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_STATUS, &status, NULL));
  g_assert_false(gtk_widget_get_visible(link)); g_assert_false(gtk_widget_is_sensitive(copy));
  gtk_window_destroy(GTK_WINDOW(window)); g_object_unref(window);
}
static void remote_app_subtitle(void) {
  g_autoptr(GrottoPanelsFakeService) fake = grotto_panels_fake_service_new();
  grotto_panels_fake_service_set_available(fake, TRUE);
  GrottoPanelsMetadata app = {.id = "nip46:79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798",
    .label = "Remote client", .transport = "NIP46", .relay = "wss://nos.lol", .trust = "medium"};
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_APPS, &app, NULL));
  GtkWidget *panel = grotto_apps_panel_new(GROTTO_PANELS_SERVICE(fake)); g_object_ref_sink(panel);
  GtkWidget *row = find_title(panel, "Remote client"); g_assert_nonnull(row);
  g_assert_nonnull(strstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(row)), "Remote · wss://nos.lol"));
  g_assert_cmpint(adw_action_row_get_subtitle_lines(ADW_ACTION_ROW(row)), ==, 0);
  g_object_unref(panel);
}
static guint picker_avatars(GtkWidget *widget, const char *name, const char *description, GdkTexture *texture) {
  guint count = 0;
  if (ADW_IS_AVATAR(widget) && !g_strcmp0(adw_avatar_get_text(ADW_AVATAR(widget)), name) &&
      has_label(gtk_widget_get_parent(widget), description)) {
    g_assert_true(adw_avatar_get_custom_image(ADW_AVATAR(widget)) == (GdkPaintable *)texture);
    g_assert_true(adw_avatar_get_show_initials(ADW_AVATAR(widget)));
    GtkWidget *option = gtk_widget_get_parent(gtk_widget_get_parent(widget));
    gtk_test_accessible_assert_role(GTK_ACCESSIBLE(option), GTK_ACCESSIBLE_ROLE_LIST_ITEM);
    gtk_test_accessible_assert_property(GTK_ACCESSIBLE(option), GTK_ACCESSIBLE_PROPERTY_LABEL, name);
    gtk_test_accessible_assert_property(GTK_ACCESSIBLE(option), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, description);
    count++;
  }
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
    count += picker_avatars(child, name, description, texture);
  return count;
}
static void identity_picker(void) {
  g_autoptr(GrottoPanelsFakeService) fake = grotto_panels_fake_service_new();
  GrottoPanelsService *service = GROTTO_PANELS_SERVICE(fake);
  const char *key = "npub1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqzqujme";
  const char *other_key = "npub1lllllllllllllllllllllllllllllllllllllllllllllllllllsq7lrjw";
  g_autofree char *short_key = g_strdup_printf("%.12s…%.6s", key, key + strlen(key) - 6);
  g_autofree char *other_short_key = g_strdup_printf("%.12s…%.6s", other_key, other_key + strlen(other_key) - 6);
  GrottoPanelsMetadata identity = {.id = key, .public_key = key, .label = "Device label"};
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_IDENTITIES, &identity, NULL));
  GrottoPanelsMetadata duplicate = {.id = other_key, .public_key = other_key, .label = "Device label"};
  g_autoptr(GrottoPanelsItem) duplicate_item = grotto_panels_item_new(&duplicate, NULL);
  g_list_store_append(G_LIST_STORE(grotto_panels_service_get_model(service, GROTTO_PANELS_IDENTITIES)), duplicate_item);
  const guint8 pixel[] = {255, 0, 0, 255};
  g_autoptr(GBytes) bytes = g_bytes_new_static(pixel, sizeof pixel);
  g_autoptr(GdkTexture) texture = GDK_TEXTURE(gdk_memory_texture_new(1, 1, GDK_MEMORY_R8G8B8A8, bytes, 4));
  g_autoptr(GrottoProfileInfo) profile = grotto_profile_info_copy(NULL);
  g_free(profile->fields[GROTTO_PROFILE_DISPLAY_NAME]); profile->fields[GROTTO_PROFILE_DISPLAY_NAME] = g_strdup("Profile display name");
  g_free(profile->fields[GROTTO_PROFILE_NAME]); profile->fields[GROTTO_PROFILE_NAME] = g_strdup("Profile name");
  profile->images[0] = g_object_ref(texture);
  grotto_panels_service_set_profile(service, key, profile);
  GtkWidget *picker = grotto_identity_picker_new(service);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(picker), "Identity");
  const char *keys[] = {key, other_key, NULL};
  g_autoptr(GtkStringList) model = gtk_string_list_new(keys);
  adw_combo_row_set_model(ADW_COMBO_ROW(picker), G_LIST_MODEL(model));
  GtkWidget *group = adw_preferences_group_new();
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), picker);
  AdwWindow *window = ADW_WINDOW(adw_window_new()); g_object_ref_sink(window);
  gtk_window_set_default_size(GTK_WINDOW(window), 360, 640);
  adw_window_set_content(window, group); gtk_window_present(GTK_WINDOW(window));
  WAIT(picker_avatars(picker, "Device label", short_key, texture) > 0);
  /* Selected presentation and the popup list both use the same cache-only factory. */
  g_signal_emit_by_name(picker, "activated");
  WAIT(picker_avatars(picker, "Device label", short_key, texture) >= 2);
  g_assert_cmpuint(picker_avatars(picker, "Device label", other_short_key, NULL), >=, 1);
  g_assert_true(has_label(picker, short_key));
  identity.label = "";
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_IDENTITIES, &identity, NULL));
  WAIT(picker_avatars(picker, "Profile display name", short_key, texture) >= 2);
  g_free(profile->fields[GROTTO_PROFILE_DISPLAY_NAME]); profile->fields[GROTTO_PROFILE_DISPLAY_NAME] = g_strdup("");
  grotto_panels_service_set_profile(service, key, profile);
  /* A nonempty profile.name must not insert another fallback tier. */
  WAIT(picker_avatars(picker, short_key, short_key, texture) >= 2);
  g_assert_false(has_label(picker, "Profile name"));
  g_clear_object(&profile->images[0]); profile->created_at++;
  grotto_panels_service_set_profile(service, key, profile);
  WAIT(picker_avatars(picker, short_key, short_key, NULL) >= 2);
  g_list_store_append(G_LIST_STORE(grotto_panels_service_get_model(service, GROTTO_PANELS_IDENTITIES)), duplicate_item);
  adw_combo_row_set_selected(ADW_COMBO_ROW(picker), 1);
  WAIT(picker_avatars(picker, "Device label", other_short_key, NULL) >= 2);
  g_assert_cmpstr(gtk_string_object_get_string(GTK_STRING_OBJECT(adw_combo_row_get_selected_item(ADW_COMBO_ROW(picker)))), ==, keys[1]);
  g_assert_cmpuint(grotto_panels_fake_service_get_invocations(fake), ==, 0);
  gtk_window_destroy(GTK_WINDOW(window)); g_object_unref(window);
}
void register_management_panels_tests(void) { g_test_add_func("/panels/identity-picker", identity_picker); g_test_add_func("/panels/connection-states", connection_states); g_test_add_func("/panels/remote-client-subtitle", remote_app_subtitle); g_test_add_func("/panels/remote-persistence", remote_persistence); g_test_add_func("/panels/management-real-widgets", management_panels); g_test_add_func("/panels/identity-actions", identity_actions); }
