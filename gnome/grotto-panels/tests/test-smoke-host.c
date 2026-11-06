/* SPDX-License-Identifier: MIT */
#include "fake-service.h"
#include "nostrc-test-gdk-frame.h"

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
    GrottoPanelsHost host = {.show_toast = with_text, .review_request = with_text,
      .add_identity = without_text, .add_app = without_text, .open_backup = with_text,
      .remove_identity = with_text, .replace_identity = with_text, .open_in_signer = with_text};
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
static GtkWidget *identity_child(GtkWidget *panel, const char *name) {
  GtkWidget *child = GTK_WIDGET(gtk_widget_get_template_child(panel, GROTTO_TYPE_IDENTITY_PANEL, name));
  g_assert_nonnull(child); return child;
}
static void identity_mounted(void) {
  g_autoptr(GrottoPanelsFakeService) fake = grotto_panels_fake_service_new();
  grotto_panels_fake_service_set_available(fake, TRUE);
  grotto_panels_fake_service_set_can(fake, GROTTO_PANEL_SET_IDENTITY_LABEL, TRUE);
  grotto_panels_fake_service_set_can(fake, GROTTO_PANEL_SET_DEFAULT_IDENTITY, TRUE);
  GrottoPanelsMetadata a = {.id="npub1first", .public_key="npub1first", .label="<b>A & B</b>", .can_sign=TRUE};
  GrottoPanelsMetadata b = {.id="npub1default", .public_key="npub1default", .label="Default", .is_default=TRUE, .can_sign=TRUE};
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_IDENTITIES, &a, NULL));
  g_autoptr(GrottoPanelsItem) second = grotto_panels_item_new(&b, NULL);
  GListStore *store = G_LIST_STORE(grotto_panels_service_get_model(GROTTO_PANELS_SERVICE(fake), GROTTO_PANELS_IDENTITIES));
  g_list_store_append(store, second);
  GrottoPanelsMetadata observer = {.id="npub1observer", .public_key="npub1observer", .label="Observer", .watch_only=TRUE};
  g_autoptr(GrottoPanelsItem) watch_item = grotto_panels_item_new(&observer, NULL);
  g_list_store_append(store, watch_item);
  for (guint narrow = 0; narrow < 2; narrow++) {
    GtkWidget *window = adw_window_new(); g_object_ref_sink(window);
    gtk_window_set_default_size(GTK_WINDOW(window), narrow ? 360 : 920, 640);
    GtkWidget *panel = grotto_identity_panel_new(GROTTO_PANELS_SERVICE(fake), NULL);
    AdwHeaderBar *header = ADW_HEADER_BAR(identity_child(panel, "header"));
    g_assert_true(adw_header_bar_get_show_end_title_buttons(header));
    g_object_set(panel, "show-window-controls", FALSE, NULL);
    g_assert_false(adw_header_bar_get_show_start_title_buttons(header)); g_assert_false(adw_header_bar_get_show_end_title_buttons(header));
    guint callbacks = 0; GrottoPanelsHost host = {.add_identity=without_text, .open_backup=with_text, .remove_identity=with_text, .open_in_signer=with_text};
    grotto_panel_set_host(GROTTO_PANEL(panel), &host, &callbacks, NULL);
    adw_window_set_content(ADW_WINDOW(window), panel); gtk_window_present(GTK_WINDOW(window));
    g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(identity_child(panel, "key_row"))), ==, b.public_key);
    g_assert_true(gtk_widget_has_css_class(identity_child(panel, "add_identity"), "flat"));
    g_assert_false(gtk_widget_get_visible(identity_child(panel, "advanced_group")));
    g_assert_false(gtk_widget_get_visible(identity_child(panel, "set_default")));
    g_signal_emit_by_name(identity_child(panel, "backup_row"), "activated");
    g_assert_cmpuint(callbacks, ==, 1); g_assert_cmpuint(grotto_panels_fake_service_get_invocations(fake), ==, narrow * 2);
    GtkListBox *others = GTK_LIST_BOX(identity_child(panel, "other_list"));
    g_assert_true(gtk_widget_get_visible(identity_child(panel, "other_group")));
    /* Device count no longer sits beneath the profile actions. */
    g_assert_null(gtk_widget_get_template_child(panel, GROTTO_TYPE_IDENTITY_PANEL, "identity_count"));
    GtkWidget *row = gtk_widget_get_first_child(GTK_WIDGET(others));
    GtkWidget *watch_row = gtk_widget_get_next_sibling(row); g_assert_nonnull(watch_row);
    g_assert_null(gtk_widget_get_next_sibling(watch_row));
    g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(watch_row)), ==, observer.label);
    g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(watch_row)), ==, "Watch only — cannot sign");
    g_signal_emit_by_name(others, "row-activated", watch_row);
    AdwNavigationView *watch_nav = grotto_identity_panel_get_navigation(GROTTO_IDENTITY_PANEL(panel));
    GtkWidget *watch_panel = GTK_WIDGET(adw_navigation_view_get_visible_page(watch_nav));
    g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(identity_child(watch_panel, "key_row"))), ==, observer.public_key);
    g_assert_false(gtk_widget_get_visible(identity_child(watch_panel, "backup_row")));
    g_assert_false(gtk_widget_get_visible(identity_child(watch_panel, "default_row")));
    g_assert_null(gtk_widget_get_template_child(watch_panel, GROTTO_TYPE_IDENTITY_PANEL, "identity_count"));
    g_assert_true(adw_navigation_view_pop(watch_nav));
    g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(row)), ==, a.label);
    g_assert_false(adw_preferences_row_get_use_markup(ADW_PREFERENCES_ROW(row)));
    g_signal_emit_by_name(others, "row-activated", row);
    AdwNavigationView *nav = grotto_identity_panel_get_navigation(GROTTO_IDENTITY_PANEL(panel));
    GtkWidget *pinned = GTK_WIDGET(adw_navigation_view_get_visible_page(nav));
    g_assert_true(GROTTO_IS_IDENTITY_PANEL(pinned));
    g_assert_false(adw_header_bar_get_show_end_title_buttons(ADW_HEADER_BAR(identity_child(pinned, "header"))));
    g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(identity_child(pinned, "key_row"))), ==, a.public_key);
    g_assert_true(gtk_widget_get_visible(identity_child(pinned, "advanced_group")));
    g_assert_false(gtk_widget_get_visible(identity_child(pinned, "add_group")));
    g_signal_emit_by_name(identity_child(pinned, "set_default"), "clicked");
    while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
    g_assert_cmpint(grotto_panels_fake_service_get_last_command(fake), ==, GROTTO_PANEL_SET_DEFAULT_IDENTITY);
    g_assert_cmpstr(grotto_panels_fake_service_get_last_args(fake)->public_key, ==, a.public_key);
    GtkEditable *name = GTK_EDITABLE(identity_child(pinned, "name_row")); gtk_editable_set_text(name, "New local label");
    g_signal_emit_by_name(name, "apply"); while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
    g_assert_cmpint(grotto_panels_fake_service_get_last_command(fake), ==, GROTTO_PANEL_SET_IDENTITY_LABEL);
    g_assert_cmpstr(grotto_panels_fake_service_get_last_args(fake)->label, ==, "New local label");
    grotto_panels_fake_service_set_can(fake, GROTTO_PANEL_SET_IDENTITY_LABEL, FALSE);
    g_assert_false(gtk_widget_get_sensitive(GTK_WIDGET(name)));
    grotto_panels_fake_service_set_can(fake, GROTTO_PANEL_SET_IDENTITY_LABEL, TRUE);
    g_assert_true(adw_navigation_view_pop(nav));
    g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(identity_child(panel, "key_row"))), ==, b.public_key);
    gtk_window_destroy(GTK_WINDOW(window)); g_object_unref(window);
  }
  a.watch_only=TRUE; a.can_sign=FALSE;
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_IDENTITIES, &a, NULL));
  GtkWidget *watch = grotto_identity_panel_new(GROTTO_PANELS_SERVICE(fake), a.public_key); g_object_ref_sink(watch);
  g_assert_false(gtk_widget_get_visible(identity_child(watch, "backup_row")));
  g_assert_false(gtk_widget_get_visible(identity_child(watch, "default_row")));
  g_object_unref(watch);
  a.watch_only = FALSE; a.can_sign = TRUE;
  g_assert_true(grotto_panels_fake_service_replace(fake, GROTTO_PANELS_IDENTITIES, &a, NULL));
  grotto_panels_fake_service_set_deferred(fake, TRUE);
  GtkWidget *inflight_window = adw_window_new(); g_object_ref_sink(inflight_window);
  GtkWidget *inflight = grotto_identity_panel_new(GROTTO_PANELS_SERVICE(fake), a.public_key);
  gpointer weak_panel = inflight; g_object_add_weak_pointer(G_OBJECT(inflight), &weak_panel);
  adw_window_set_content(ADW_WINDOW(inflight_window), inflight);
  gtk_editable_set_text(GTK_EDITABLE(identity_child(inflight, "name_row")), "Pending edit");
  g_signal_emit_by_name(identity_child(inflight, "name_row"), "apply");
  g_assert_cmpuint(grotto_panels_fake_service_get_pending(fake), ==, 1);
  gtk_window_destroy(GTK_WINDOW(inflight_window)); g_object_unref(inflight_window);
  g_assert_null(weak_panel); g_assert_cmpuint(grotto_panels_fake_service_get_pending(fake), ==, 0);
  while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
  grotto_panels_fake_service_set_available(fake, FALSE);
  GtkWidget *unavailable = grotto_identity_panel_new(GROTTO_PANELS_SERVICE(fake), NULL); g_object_ref_sink(unavailable);
  g_assert_false(gtk_widget_get_visible(identity_child(unavailable, "backup_row"))); g_object_unref(unavailable);
}
void register_management_panels_tests(void);
void register_profile_panel_tests(void);
int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();
  adw_init(); /* Missing display is a failure, not a successful skip. */
  g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
  g_test_add_func("/grotto-panels/identity", identity_mounted);
  g_test_add_func("/grotto-panels/mount", mounted);
  g_test_add_func("/grotto-panels/commands", commands);
  g_test_add_func("/grotto-panels/negative-paths", negative_paths);
  register_management_panels_tests();
  register_profile_panel_tests();
  return g_test_run();
}
