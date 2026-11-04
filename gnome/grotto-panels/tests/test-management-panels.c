/* SPDX-License-Identifier: MIT */
#include "fake-service.h"
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
static void management_panels(void) {
  g_autoptr(GrottoPanelsFakeService) fake = grotto_panels_fake_service_new();
  GrottoPanelsService *service = GROTTO_PANELS_SERVICE(fake);
  grotto_panels_fake_service_set_available(fake, TRUE);
  for (guint i = 0; i < GROTTO_PANEL_N_COMMANDS; i++) grotto_panels_fake_service_set_can(fake, i, TRUE);
  const char *relays[] = {"wss://nos.lol", NULL}, *methods[] = {"sign_event", NULL};
  const char *effective[] = {"Public key after consent", "Public publishing", NULL};
  GrottoPanelsMetadata app = {.effective_permissions = effective, .id = "app:test", .label = "<b>Plain App</b>", .trust = "medium", .access_policy = "Ask for sensitive operations", .transport = "Local App"};
  GrottoPanelsMetadata status = {.id = "status", .state = "LISTENING", .remote_enabled = TRUE, .relays = relays, .methods = methods};
  GrottoPanelsMetadata request = {.id = "request-1", .label = "Plain App", .operation = "Sign Event", .deadline_usec = g_get_monotonic_time() + 60000000};
  GrottoPanelsMetadata record = {.id = "11", .principal = "app:test", .operation = "event", .outcome = "success", .sequence = 11, .has_event_kind = TRUE, .event_kind = 1};
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_APPS, &app, NULL));
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_STATUS, &status, NULL));
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_REQUESTS, &request, NULL));
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_ACTIVITY, &record, NULL));
  gsize count = 0; const GrottoPanelDescriptor *descriptors = grotto_panels_get_descriptors(&count); g_assert_cmpuint(count, ==, 4);
  GtkWidget *(*constructors[])(GrottoPanelsService *) = {grotto_apps_panel_new, grotto_activity_panel_new, grotto_remote_panel_new};
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
      g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(first)), ==, app.label);
      g_signal_emit_by_name(first, "activated");
      AdwNavigationView *nav = grotto_apps_panel_get_navigation(GROTTO_APPS_PANEL(panel));
      GtkWidget *detail = GTK_WIDGET(adw_navigation_view_get_visible_page(nav));
      GtkWidget *low_row = find_title(detail, "Ask unless permitted"); g_assert_nonnull(low_row);
      GtkWidget *low = adw_action_row_get_activatable_widget(ADW_ACTION_ROW(low_row));
      gtk_check_button_set_active(GTK_CHECK_BUTTON(low), TRUE);
      WAIT(grotto_panels_fake_service_get_invocations(fake) == 1);
      g_assert_cmpint(grotto_panels_fake_service_get_last_command(fake), ==, GROTTO_PANEL_SET_CLIENT_TRUST);
      g_assert_cmpstr(grotto_panels_fake_service_get_last_args(fake)->trust, ==, "low");
      GrottoPanelsMetadata permissions_data[] = {{.id = "0", .principal = app.id, .public_key = "public-identity", .operation = "event", .decision = "allow", .state = "active", .has_event_kind = TRUE, .event_kind = 3, .revision = 7},
        {.id = "1", .principal = app.id, .public_key = "public-identity", .operation = "event", .decision = "policy-review", .state = "inactive", .reason = "Pending policy review", .event_kind = 1, .revision = 7}};
      grotto_panels_fake_service_set_query_result(fake, GROTTO_PANELS_QUERY_PERMISSIONS, permissions_data, G_N_ELEMENTS(permissions_data), -1);
      GtkWidget *permissions = find_title(detail, "Permissions"); g_assert_nonnull(permissions); g_signal_emit_by_name(permissions, "activated");
      WAIT(find_title(detail, "Allow · Follow list (3)"));
      GtkWidget *derived = find_title(detail, effective[0]); g_assert_nonnull(derived);
      g_assert_false(gtk_list_box_row_get_activatable(GTK_LIST_BOX_ROW(derived)));
      g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(derived)), ==, "From Medium trust");
      /* Kind wording comes from the generated registry; locate the second row by metadata. */
      GtkWidget *explicit_row = find_title(detail, "Allow · Follow list (3)");
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
    }
    if (i == 1) {
      GrottoPanelsMetadata older[] = {
        {.id = "11", .sequence = 11, .principal = "app:test", .operation = "event", .outcome = "success"},
        {.id = "10", .sequence = 10, .principal = "app:other", .operation = "event", .outcome = "denied"}
      };
      grotto_panels_fake_service_set_query_result(fake, GROTTO_PANELS_QUERY_ACTIVITY, older, 2, -1);
      GtkListView *records = GTK_LIST_VIEW(gtk_widget_get_template_child(GTK_WIDGET(panel), GROTTO_TYPE_ACTIVITY_PANEL, "records"));
      GtkWidget *load = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(panel), GROTTO_TYPE_ACTIVITY_PANEL, "older"));
      g_signal_emit_by_name(load, "clicked");
      WAIT(g_list_model_get_n_items(G_LIST_MODEL(gtk_list_view_get_model(records))) == 2); /* Sequence 11 is deduplicated. */
      grotto_activity_panel_filter_app(GROTTO_ACTIVITY_PANEL(panel), "app:test");
      WAIT(g_list_model_get_n_items(G_LIST_MODEL(gtk_list_view_get_model(records))) == 1);
      grotto_activity_panel_filter_app(GROTTO_ACTIVITY_PANEL(panel), NULL);
      WAIT(g_list_model_get_n_items(G_LIST_MODEL(gtk_list_view_get_model(records))) == 2);
      grotto_panels_fake_service_set_query_result(fake, GROTTO_PANELS_QUERY_ACTIVITY, NULL, 0, -1);
      g_signal_emit_by_name(load, "clicked"); WAIT(!gtk_widget_get_sensitive(load));
    } else if (i == 2) {
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
void register_management_panels_tests(void) { g_test_add_func("/panels/management-real-widgets", management_panels); }
