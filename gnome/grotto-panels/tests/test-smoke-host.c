/* SPDX-License-Identifier: MIT */
#include "fake-service.h"

typedef struct { GrottoPanel parent; GtkListBox *list; GtkWidget *edit; AdwNavigationView *navigation; } SmokePanel;
typedef struct { GrottoPanelClass parent; } SmokePanelClass;
G_DEFINE_TYPE(SmokePanel, smoke_panel, GROTTO_TYPE_PANEL)
static GtkWidget *make_row(gpointer item, gpointer data) {
  (void)data;
  return grotto_panel_create_metadata_row(GROTTO_PANELS_ITEM(item));
}
static void update(SmokePanel *self) {
  GrottoPanelsService *service = grotto_panel_get_service(GROTTO_PANEL(self));
  gtk_list_box_bind_model(self->list, service ? grotto_panels_service_get_model(service, GROTTO_PANELS_APPS) : NULL,
                         make_row, NULL, NULL);
  gtk_widget_set_visible(self->edit, grotto_panels_service_can(service, GROTTO_PANEL_SET_CLIENT_LABEL));
}
static void smoke_panel_class_init(SmokePanelClass *klass) { (void)klass; }
static void smoke_panel_init(SmokePanel *self) {
  self->navigation = ADW_NAVIGATION_VIEW(adw_navigation_view_new());
  GtkWidget *toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
  GtkWidget *page = adw_preferences_page_new();
  GtkWidget *group = adw_preferences_group_new();
  self->list = GTK_LIST_BOX(gtk_list_box_new());
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), GTK_WIDGET(self->list));
  self->edit = gtk_button_new_with_label("Edit local label");
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), self->edit);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group));
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);
  adw_navigation_view_add(self->navigation, adw_navigation_page_new(toolbar, "Scaffold"));
  adw_navigation_page_set_child(ADW_NAVIGATION_PAGE(self), GTK_WIDGET(self->navigation));
  adw_navigation_page_set_title(ADW_NAVIGATION_PAGE(self), "Scaffold");
  g_signal_connect(self, "service-changed", G_CALLBACK(update), NULL);
  update(self);
}
static void with_text(gpointer data, const char *text) {
  g_assert_nonnull(text);
  (*(guint *)data)++;
}
static void without_text(gpointer data) { (*(guint *)data)++; }
static void destroyed(gpointer data) { (*(guint *)data)++; }
static void mounted(void) {
  g_autoptr(GrottoPanelsFakeService) fake = grotto_panels_fake_service_new();
  grotto_panels_fake_service_set_available(fake, TRUE);
  grotto_panels_fake_service_set_can(fake, GROTTO_PANEL_SET_CLIENT_LABEL, TRUE);
  GrottoPanelsMetadata metadata = {.id="app", .label="<b>Untrusted & text</b>", .reason="<i>daemon reason</i>",
    .state="inactive", .access_policy="future-policy", .blocked=TRUE, .needs_review=TRUE};
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_APPS, &metadata, NULL));
  for (guint narrow = 0; narrow < 2; narrow++) {
    g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_APPS, &metadata, NULL));
    GtkWidget *window = adw_window_new();
    g_object_ref_sink(window);
    gtk_window_set_default_size(GTK_WINDOW(window), narrow ? 360 : 920, 640);
    AdwNavigationSplitView *split = ADW_NAVIGATION_SPLIT_VIEW(adw_navigation_split_view_new());
    adw_navigation_split_view_set_sidebar(split, adw_navigation_page_new(gtk_label_new("Panels"), "Host"));
    SmokePanel *panel = g_object_new(smoke_panel_get_type(), "service", fake, NULL);
    gpointer weak = panel;
    g_object_add_weak_pointer(G_OBJECT(panel), &weak);
    guint callbacks = 0, releases = 0;
    GrottoPanelsHost host = {with_text, with_text, without_text, without_text, with_text, with_text, with_text, with_text};
    grotto_panel_set_host(GROTTO_PANEL(panel), &host, &callbacks, destroyed);
    const char *payloads[] = {"nsec1not-public", "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about", "token=not-public"};
    for (guint i = 0; i < G_N_ELEMENTS(payloads); i++) {
      for (guint action = 0; action < GROTTO_PANEL_HOST_N_ACTIONS; action++) {
        g_autoptr(GError) error = NULL;
        g_assert_false(grotto_panel_host_invoke(GROTTO_PANEL(panel), action, payloads[i], &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
      }
    }
    g_assert_cmpuint(callbacks, ==, 0);
    for (guint action = 0; action < GROTTO_PANEL_HOST_N_ACTIONS; action++)
      g_assert_true(grotto_panel_host_invoke(GROTTO_PANEL(panel), action,
        action == GROTTO_PANEL_HOST_ADD_IDENTITY || action == GROTTO_PANEL_HOST_ADD_APP ? NULL : "public-target", NULL));
    g_assert_cmpuint(callbacks, ==, 8);
    grotto_panel_set_host(GROTTO_PANEL(panel), NULL, &releases, destroyed);
    g_assert_cmpuint(callbacks, ==, 9);
    g_assert_false(grotto_panel_host_can(GROTTO_PANEL(panel), GROTTO_PANEL_HOST_BACKUP));
    adw_navigation_split_view_set_content(split, ADW_NAVIGATION_PAGE(panel));
    adw_navigation_split_view_set_collapsed(split, narrow);
    adw_navigation_split_view_set_show_content(split, TRUE);
    adw_window_set_content(ADW_WINDOW(window), GTK_WIDGET(split));
    gtk_window_present(GTK_WINDOW(window));
    g_assert_true(gtk_widget_get_root(GTK_WIDGET(panel)) == GTK_ROOT(window));
    AdwActionRow *row = ADW_ACTION_ROW(gtk_list_box_get_row_at_index(panel->list, 0));
    g_assert_false(adw_preferences_row_get_use_markup(ADW_PREFERENCES_ROW(row)));
    g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(row)), ==, metadata.label);
    g_assert_cmpstr(adw_action_row_get_subtitle(row), ==, metadata.reason);
    g_autoptr(GrottoPanelsItem) item = g_list_model_get_item(
      grotto_panels_service_get_model(GROTTO_PANELS_SERVICE(fake), GROTTO_PANELS_APPS), 0);
    const GrottoPanelsMetadata *roundtrip = grotto_panels_item_get_metadata(item);
    g_assert_cmpstr(roundtrip->access_policy, ==, "future-policy");
    g_assert_cmpstr(roundtrip->state, ==, "inactive");
    g_assert_true(roundtrip->blocked); g_assert_true(roundtrip->needs_review);
    metadata.label = narrow ? "Changed twice" : "Changed";
    g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_APPS, &metadata, NULL));
    row = ADW_ACTION_ROW(gtk_list_box_get_row_at_index(panel->list, 0));
    g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(row)), ==, metadata.label);
    g_assert_true(gtk_widget_get_visible(panel->edit));
    grotto_panels_fake_service_set_can(fake, GROTTO_PANEL_SET_CLIENT_LABEL, FALSE);
    g_assert_false(gtk_widget_get_visible(panel->edit));
    grotto_panels_fake_service_set_can(fake, GROTTO_PANEL_SET_CLIENT_LABEL, TRUE);
    grotto_panels_fake_service_set_available(fake, FALSE);
    g_assert_false(gtk_widget_get_visible(panel->edit));
    grotto_panels_fake_service_set_available(fake, TRUE);
    grotto_panels_fake_service_set_can(fake, GROTTO_PANEL_SET_CLIENT_LABEL, TRUE);
    AdwNavigationPage *sub = adw_navigation_page_new(gtk_label_new("Subpage"), "Subpage");
    adw_navigation_view_push(panel->navigation, sub);
    g_assert_true(adw_navigation_view_get_visible_page(panel->navigation) == sub);
    g_assert_true(adw_navigation_view_pop(panel->navigation));
    g_autoptr(GCancellable) old_cancel = g_object_ref(grotto_panel_get_cancellable(GROTTO_PANEL(panel)));
    grotto_panel_set_service(GROTTO_PANEL(panel), NULL);
    g_assert_true(g_cancellable_is_cancelled(old_cancel));
    g_assert_null(gtk_list_box_get_row_at_index(panel->list, 0));
    gtk_window_destroy(GTK_WINDOW(window));
    g_object_unref(window);
    g_assert_null(weak);
    g_assert_cmpuint(releases, ==, 1);
    /* Detached/destroyed views receive no late callback; service survives. */
    grotto_panels_fake_service_set_can(fake, GROTTO_PANEL_SET_CLIENT_LABEL, TRUE);
  }
}
typedef struct { gboolean done, ok; GError *error; } Completion;
static void complete(GObject *source, GAsyncResult *result, gpointer data) {
  Completion *c = data;
  c->ok = grotto_panels_service_invoke_finish(source ? GROTTO_PANELS_SERVICE(source) : NULL, result, &c->error);
  c->done = TRUE;
}
static void commands(void) {
  g_autoptr(GrottoPanelsFakeService) fake = grotto_panels_fake_service_new();
  for (gint command = -1; command <= GROTTO_PANEL_N_COMMANDS; command++) {
    Completion c = {0};
    const GrottoPanelCommandArgs args = {0};
    grotto_panels_service_invoke_async(GROTTO_PANELS_SERVICE(fake), command, &args, NULL, complete, &c);
    while (!c.done) g_main_context_iteration(NULL, TRUE);
    g_assert_false(c.ok); g_assert_nonnull(c.error); g_clear_error(&c.error);
  }
  Completion absent = {0};
  const GrottoPanelCommandArgs args = {0};
  grotto_panels_service_invoke_async(NULL, GROTTO_PANEL_PAUSE, &args, NULL, complete, &absent);
  while (!absent.done) g_main_context_iteration(NULL, TRUE);
  g_assert_error(absent.error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED);
  g_clear_error(&absent.error);
  grotto_panels_fake_service_set_available(fake, TRUE);
  Completion denied = {0};
  grotto_panels_service_invoke_async(GROTTO_PANELS_SERVICE(fake), GROTTO_PANEL_PAUSE, &args, NULL, complete, &denied);
  while (!denied.done) g_main_context_iteration(NULL, TRUE);
  g_assert_error(denied.error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED); g_clear_error(&denied.error);
  grotto_panels_fake_service_set_can(fake, GROTTO_PANEL_PAUSE, TRUE);
  Completion allowed = {0};
  grotto_panels_service_invoke_async(GROTTO_PANELS_SERVICE(fake), GROTTO_PANEL_PAUSE, &args, NULL, complete, &allowed);
  while (!allowed.done) g_main_context_iteration(NULL, TRUE);
  g_assert_true(allowed.ok); g_assert_no_error(allowed.error);
  g_assert_cmpint(grotto_panel_command_get_access(GROTTO_PANEL_SET_CLIENT_LABEL), ==, GROTTO_PANELS_ACCESS_MANAGER);
  g_assert_cmpint(grotto_panel_command_get_access(GROTTO_PANEL_RESUME), ==, GROTTO_PANELS_ACCESS_APPROVER);
  g_assert_cmpint(grotto_panel_command_get_access(GROTTO_PANEL_PAUSE), ==, GROTTO_PANELS_ACCESS_SESSION);
}
static void negative_paths(void) {
  g_autoptr(GrottoPanelsFakeService) fake = grotto_panels_fake_service_new();
  grotto_panels_fake_service_set_available(fake, TRUE);
  grotto_panels_fake_service_set_can(fake, GROTTO_PANEL_SET_CLIENT_LABEL, TRUE);
  grotto_panels_fake_service_set_deferred(fake, TRUE);
  SmokePanel *panel = g_object_ref_sink(g_object_new(smoke_panel_get_type(), "service", fake, NULL));
  const GrottoPanelCommandArgs args = {.principal = "app", .label = "Cosmetic name"};
  GrottoPanelsMetadata metadata = {.id = "app", .label = "Unchanged after refusal"};
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_APPS, &metadata, NULL));
  const GIOErrorEnum failures[] = {G_IO_ERROR_BUSY, G_IO_ERROR_PERMISSION_DENIED,
    G_IO_ERROR_NOT_SUPPORTED, G_IO_ERROR_INVALID_ARGUMENT, G_IO_ERROR_FAILED};
  for (guint i = 0; i < G_N_ELEMENTS(failures); i++) {
    Completion c = {0};
    grotto_panels_fake_service_set_failure(fake, GROTTO_PANEL_SET_CLIENT_LABEL, failures[i]);
    grotto_panels_service_invoke_async(GROTTO_PANELS_SERVICE(fake), GROTTO_PANEL_SET_CLIENT_LABEL,
      &args, grotto_panel_get_cancellable(GROTTO_PANEL(panel)), complete, &c);
    g_assert_false(c.done); g_assert_cmpuint(grotto_panels_fake_service_get_pending(fake), ==, 1);
    g_assert_cmpint(grotto_panels_fake_service_get_last_command(fake), ==, GROTTO_PANEL_SET_CLIENT_LABEL);
    g_assert_cmpstr(grotto_panels_fake_service_get_last_args(fake)->label, ==, args.label);
    g_assert_null(grotto_panels_fake_service_get_last_args(fake)->trust);
    g_assert_true(grotto_panels_fake_service_complete_next(fake));
    while (!c.done) g_main_context_iteration(NULL, TRUE);
    g_assert_false(c.ok); g_assert_error(c.error, G_IO_ERROR, (gint)failures[i]); g_clear_error(&c.error);
    AdwActionRow *row = ADW_ACTION_ROW(gtk_list_box_get_row_at_index(panel->list, 0));
    g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(row)), ==, metadata.label);
  }
  grotto_panels_fake_service_set_failure(fake, GROTTO_PANEL_SET_CLIENT_LABEL, -1);
  Completion cancelled = {0};
  grotto_panels_service_invoke_async(GROTTO_PANELS_SERVICE(fake), GROTTO_PANEL_SET_CLIENT_LABEL,
    &args, grotto_panel_get_cancellable(GROTTO_PANEL(panel)), complete, &cancelled);
  g_assert_false(cancelled.done);
  grotto_panel_set_service(GROTTO_PANEL(panel), NULL);
  while (!cancelled.done) g_main_context_iteration(NULL, TRUE);
  g_assert_false(cancelled.ok); g_assert_error(cancelled.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error(&cancelled.error);
  g_assert_cmpuint(grotto_panels_fake_service_get_pending(fake), ==, 0);
  Completion owner_lost = {0};
  grotto_panels_service_invoke_async(GROTTO_PANELS_SERVICE(fake), GROTTO_PANEL_SET_CLIENT_LABEL,
    &args, NULL, complete, &owner_lost);
  grotto_panels_fake_service_set_available(fake, FALSE);
  while (!owner_lost.done) g_main_context_iteration(NULL, TRUE);
  g_assert_error(owner_lost.error, G_IO_ERROR, G_IO_ERROR_CANCELLED); g_clear_error(&owner_lost.error);
  g_assert_false(grotto_panels_service_can(GROTTO_PANELS_SERVICE(fake), GROTTO_PANEL_SET_CLIENT_LABEL));
  g_object_unref(panel);
  gpointer weak_fake = fake;
  g_object_add_weak_pointer(G_OBJECT(fake), &weak_fake);
  g_clear_object(&fake);
  g_assert_null(weak_fake);
}
int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  adw_init(); /* Missing display is a failure, not a successful skip. */
  g_test_add_func("/grotto-panels/mount", mounted);
  g_test_add_func("/grotto-panels/commands", commands);
  g_test_add_func("/grotto-panels/negative-paths", negative_paths);
  return g_test_run();
}
