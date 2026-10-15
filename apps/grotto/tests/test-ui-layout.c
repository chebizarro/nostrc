#include "ui/signer-window.h"
#include "ui/page-home.h"
#include "ui/lock-screen.h"
#include "ui/page-client-detail.h"
#include "ui/page-connect.h"
#include "home-model.h"
#include "ui/page-identity-detail.h"
#include "i18n.h"
#include <nostr-kinds.h>
#include "accounts_store.h"
#include "settings_manager.h"
#include "session-manager.h"
#include "backup-recovery.h"
#include "approval-controller.h"
#include "tests/common/nostrc-test-bus.h"
#include "nostrc-test-gdk-frame.h"
#include <glib/gstdio.h>
#include <string.h>
#include <locale.h>
static GDBusConnection *server; static gchar *npub; static guint frames, tier_writes, permission_writes;
static const gchar *tier = "medium";
static guint64 revision = 12, expires_at;
static GVariant *permission_rows;
static gboolean force_conflict, scoped_saved_before_cleanup, fail_permission_read, conflict_cleanup;
static guint permission_attempts;
static const gchar *row_text(GVariant *row, const gchar *key) { const gchar *value = ""; g_variant_lookup(row, key, "&s", &value); return value; }
static gboolean contains_permission(const gchar *identity, const gchar *decision) {
  for (gsize i = 0; permission_rows && i < g_variant_n_children(permission_rows); i++) {
    g_autoptr(GVariant) row = g_variant_get_child_value(permission_rows, i);
    if (!strcmp(row_text(row, "identity"), identity) && !strcmp(row_text(row, "decision"), decision)) return TRUE;
  }
  return FALSE;
}
static void fixture_permission(GVariantBuilder *rows, const gchar *identity, gint kind, const gchar *decision) {
  GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&b, "{sv}", "principal", g_variant_new_string("https://jank.example"));
  g_variant_builder_add(&b, "{sv}", "identity", g_variant_new_string(identity));
  g_variant_builder_add(&b, "{sv}", "operation", g_variant_new_string("event"));
  g_variant_builder_add(&b, "{sv}", "event-kind", g_variant_new_int32(kind));
  g_variant_builder_add(&b, "{sv}", "decision", g_variant_new_string(decision));
  g_variant_builder_add(&b, "{sv}", "state", g_variant_new_string(!strcmp(decision, "review") ? "needs-review" : "explicit"));
  g_variant_builder_add(&b, "{sv}", "expires-at", g_variant_new_uint64(expires_at));
  g_variant_builder_add_value(rows, g_variant_builder_end(&b));
}
static gboolean bound(gpointer data) { g_error("UI layout did not reach barrier: %s", (const gchar *)data); return G_SOURCE_REMOVE; }
#define WAIT(condition) do { guint timer = g_timeout_add_seconds(15, bound, #condition); while (!(condition)) g_main_context_iteration(NULL, TRUE); g_source_remove(timer); } while (0)
static gboolean tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data) { (void)widget; (void)clock; (void)data; frames++; return G_SOURCE_CONTINUE; }
static void wait_frames(GtkWidget *widget) {
  frames = 0; guint id = gtk_widget_add_tick_callback(widget, tick, NULL, NULL); WAIT(frames >= 3); gtk_widget_remove_tick_callback(widget, id);
}
static void snapshot(GtkWidget *widget, const gchar *name) {
  wait_frames(widget); int width = gtk_widget_get_width(widget), height = gtk_widget_get_height(widget);
  g_assert_cmpint(width, >, 0); g_assert_cmpint(height, >, 0);
  if (width <= 360) {
    GtkWidget *overlay = GTK_WIDGET(gtk_widget_get_template_child(widget, TYPE_SIGNER_WINDOW, "toast_overlay"));
    AdwNavigationSplitView *split = ADW_NAVIGATION_SPLIT_VIEW(gtk_widget_get_template_child(widget, TYPE_SIGNER_WINDOW, "split_view"));
    g_assert_true(adw_navigation_split_view_get_collapsed(split));
    int minimum; gtk_widget_measure(overlay, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, NULL, NULL, NULL);
    g_assert_cmpint(minimum, <=, width);
  }
  g_autoptr(GdkPaintable) paintable = gtk_widget_paintable_new(widget); GtkSnapshot *snapshot = gtk_snapshot_new();
  gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), width, height);
  g_autoptr(GskRenderNode) node = gtk_snapshot_free_to_node(snapshot); g_assert_nonnull(node);
  GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(widget));
  g_autoptr(GdkTexture) texture = gsk_renderer_render_texture(renderer, node, &GRAPHENE_RECT_INIT(0, 0, width, height));
  g_mkdir_with_parents(G10_SCREENSHOT_DIR, 0700); g_autofree gchar *path = g_strdup_printf("%s/%s%s.png", G10_SCREENSHOT_DIR, name, g_getenv("G10_TEST_LANGUAGE") ? "-es" : "");
  g_assert_true(gdk_texture_save_to_png(texture, path)); g_test_message("saved %s (%dx%d)", path, width, height);
}
static void call(GDBusConnection *bus, const gchar *sender, const gchar *path, const gchar *interface, const gchar *method, GVariant *args, GDBusMethodInvocation *inv, gpointer data) {
  (void)bus; (void)sender; (void)path; (void)interface; (void)args; (void)data;
  if (!strcmp(method, "SetClientTrust")) {
    const gchar *principal, *value; g_variant_get(args, "(&s&s)", &principal, &value);
    g_assert_cmpstr(principal, ==, "https://jank.example"); g_assert_cmpstr(value, ==, "low");
    tier = "low"; revision++; tier_writes++; g_dbus_method_invocation_return_value(inv, NULL);
    g_dbus_connection_emit_signal(bus, NULL, path, interface, "ClientsChanged", g_variant_new("(t)", revision), NULL); return;
  }
  if (!strcmp(method, "SetPermissions")) {
    const gchar *principal, *identity; guint64 expected; g_autoptr(GVariant) rows = NULL;
    g_variant_get(args, "(&s&s@aa{sv}t)", &principal, &identity, &rows, &expected);
    g_assert_cmpstr(principal, ==, "https://jank.example"); g_assert_cmpuint(expected, ==, revision); permission_attempts++;
    if (force_conflict) {
      force_conflict = FALSE; revision++;
      GVariantBuilder refreshed; g_variant_builder_init(&refreshed, G_VARIANT_TYPE("aa{sv}"));
      for (gsize i = 0; i < g_variant_n_children(permission_rows); i++) { g_autoptr(GVariant) row = g_variant_get_child_value(permission_rows, i); g_variant_builder_add_value(&refreshed, row); }
      fixture_permission(&refreshed, npub, NOSTR_KIND_REACTION, "deny");
      g_clear_pointer(&permission_rows, g_variant_unref); permission_rows = g_variant_ref_sink(g_variant_builder_end(&refreshed));
      g_dbus_method_invocation_return_dbus_error(inv, "org.nostr.Grotto.Control1.Error.Conflict", "stale snapshot"); return;
    }
    if (conflict_cleanup && !strcmp(identity, "*")) {
      conflict_cleanup = FALSE; revision++;
      g_dbus_method_invocation_return_dbus_error(inv, "org.nostr.Grotto.Control1.Error.Conflict", "cleanup snapshot changed"); return;
    }
    GVariantBuilder next; g_variant_builder_init(&next, G_VARIANT_TYPE("aa{sv}"));
    for (gsize i = 0; i < g_variant_n_children(permission_rows); i++) {
      g_autoptr(GVariant) row = g_variant_get_child_value(permission_rows, i);
      if (strcmp(row_text(row, "identity"), identity)) g_variant_builder_add_value(&next, row);
    }
    for (gsize i = 0; i < g_variant_n_children(rows); i++) {
      g_autoptr(GVariant) row = g_variant_get_child_value(rows, i);
      g_assert_cmpstr(row_text(row, "identity"), ==, identity);
      if (!strcmp(row_text(row, "decision"), "allow")) g_assert_cmpstr(identity, !=, "*");
      gint kind; g_variant_lookup(row, "event-kind", "i", &kind);
      if (kind == NOSTR_KIND_FOLLOW_LIST) { guint64 expiry; g_variant_lookup(row, "expires-at", "t", &expiry); g_assert_cmpuint(expiry, ==, expires_at); }
      GVariantDict d; g_variant_dict_init(&d, row); g_variant_dict_insert(&d, "state", "s", !strcmp(row_text(row, "decision"), "review") ? "needs-review" : "explicit");
      g_variant_builder_add_value(&next, g_variant_dict_end(&d));
    }
    gboolean legacy_present = contains_permission("*", "review");
    g_clear_pointer(&permission_rows, g_variant_unref); permission_rows = g_variant_ref_sink(g_variant_builder_end(&next));
    if (strcmp(identity, "*") && legacy_present && contains_permission(npub, "allow")) scoped_saved_before_cleanup = TRUE;
    if (!strcmp(identity, "*") && !contains_permission("*", "review")) g_assert_true(scoped_saved_before_cleanup || !g_variant_n_children(rows));
    revision++; permission_writes++;
    g_dbus_method_invocation_return_value(inv, NULL); g_dbus_connection_emit_signal(bus, NULL, path, interface, "ClientsChanged", g_variant_new("(t)", revision), NULL); return;
  }
  if (!strcmp(method, "ListIdentities")) { const gchar *ids[] = {npub, NULL}; g_dbus_method_invocation_return_value(inv, g_variant_new("(^as)", ids)); return; }
  if (!strcmp(method, "GetStatus")) {
    GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
#define ADD(k,v) g_variant_builder_add(&b, "{sv}", k, v)
    ADD("control_version", g_variant_new_uint32(1)); ADD("control_epoch", g_variant_new_string("fixture")); ADD("permissions_revision", g_variant_new_uint64(revision));
    ADD("signed_today", g_variant_new_uint32(117)); ADD("client_count", g_variant_new_uint32(1)); ADD("pending_count", g_variant_new_uint32(0)); ADD("bunker_state", g_variant_new_string("DISABLED"));
    g_dbus_method_invocation_return_value(inv, g_variant_new("(@a{sv})", g_variant_builder_end(&b))); return;
  }
  GVariantBuilder rows; g_variant_builder_init(&rows, G_VARIANT_TYPE("aa{sv}"));
  if (!strcmp(method, "ListClients")) {
    GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    ADD("principal", g_variant_new_string("https://jank.example")); ADD("label", g_variant_new_string("Jank")); ADD("transport", g_variant_new_string("DBUS"));
    ADD("trust-tier", g_variant_new_string(tier)); ADD("identity", g_variant_new_string(npub)); ADD("first-consented-at", g_variant_new_uint64(g_get_real_time() / G_USEC_PER_SEC - 600));
    ADD("last-used-at", g_variant_new_uint64(g_get_real_time() / G_USEC_PER_SEC)); ADD("permission-count", g_variant_new_uint32(1));
    g_variant_builder_add_value(&rows, g_variant_builder_end(&b));
  } else if (!strcmp(method, "ListPermissions")) {
    if (fail_permission_read) { fail_permission_read = FALSE; g_variant_builder_clear(&rows); g_dbus_method_invocation_return_dbus_error(inv, "org.nostr.Grotto.Control1.Error.Unavailable", "read failed"); return; }
    g_variant_builder_clear(&rows); g_dbus_method_invocation_return_value(inv, g_variant_new("(@aa{sv})", g_variant_ref(permission_rows))); return;
  } else if (!strcmp(method, "ListActivity")) {
    GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    ADD("principal", g_variant_new_string("https://jank.example")); ADD("operation", g_variant_new_string("sign_event")); ADD("outcome", g_variant_new_string("success"));
    ADD("event_kind", g_variant_new_uint32(NOSTR_KIND_FOLLOW_LIST)); ADD("timestamp", g_variant_new_uint64(g_get_real_time() / G_USEC_PER_SEC)); ADD("sequence", g_variant_new_uint64(1));
    g_variant_builder_add_value(&rows, g_variant_builder_end(&b));
  }
#undef ADD
  g_dbus_method_invocation_return_value(inv, g_variant_new("(@aa{sv})", g_variant_builder_end(&rows)));
}
static const GDBusInterfaceVTable vtable = {call, NULL, NULL, {0}};
static GtkWidget *find_button(GtkWidget *widget, const gchar *label) {
  if (GTK_IS_BUTTON(widget) && !g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), label)) return widget;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) { GtkWidget *found = find_button(child, label); if (found) return found; }
  return NULL;
}
static GtkWidget *find_permission(GtkWidget *widget, const gchar *identity, const gchar *decision) {
  GVariant *row = g_object_get_data(G_OBJECT(widget), "permission");
  if (row && !strcmp(row_text(row, "identity"), identity) && !strcmp(row_text(row, "decision"), decision)) return widget;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) { GtkWidget *found = find_permission(child, identity, decision); if (found) return found; }
  return NULL;
}
static GtkWidget *find_banner(GtkWidget *widget) {
  if (ADW_IS_BANNER(widget)) return widget;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) { GtkWidget *found = find_banner(child); if (found) return found; }
  return NULL;
}
static void assert_visible_focus(SignerWindow *window) {
  GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(window)); g_assert_nonnull(focus);
  g_assert_true(gtk_widget_get_mapped(focus)); g_assert_true(gtk_widget_is_visible(focus)); g_assert_true(gtk_widget_is_sensitive(focus));
}
static GtkWidget *find_decision(GtkWidget *widget) {
  if (GTK_IS_DROP_DOWN(widget)) {
    GListModel *model = gtk_drop_down_get_model(GTK_DROP_DOWN(widget));
    g_autoptr(GtkStringObject) first = model && g_list_model_get_n_items(model) ? g_list_model_get_item(model, 0) : NULL;
    if (first && !strcmp(gtk_string_object_get_string(first), _("Allow"))) return widget;
  }
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) { GtkWidget *found = find_decision(child); if (found) return found; }
  return NULL;
}
static GtkWidget *find_editor_control(GtkWidget *widget, GType type) {
  if (g_type_is_a(G_OBJECT_TYPE(widget), type)) return widget;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) { GtkWidget *found = find_editor_control(child, type); if (found) return found; }
  return NULL;
}
static GtkWidget *find_identity_picker(GtkWidget *widget) {
  if (GTK_IS_DROP_DOWN(widget)) {
    GListModel *model = gtk_drop_down_get_model(GTK_DROP_DOWN(widget));
    g_autoptr(GtkStringObject) first = model && g_list_model_get_n_items(model) ? g_list_model_get_item(model, 0) : NULL;
    if (first && !strcmp(gtk_string_object_get_string(first), _("Choose an identity"))) return widget;
  }
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) { GtkWidget *found = find_identity_picker(child); if (found) return found; }
  return NULL;
}
static void finalised(gpointer data, GObject *object) { (void)object; (*(guint *)data)++; }
static void layout_activate(GApplication *application, gpointer data) {
  (void)data; AdwApplication *app = ADW_APPLICATION(application);
  guint windows_gone = 0, pages_gone = 0, editors_gone = 0;
  AccountsStore *store = accounts_store_get_default(); g_assert_true(accounts_store_add(store, npub, "My identity", NULL)); accounts_store_set_selected(store, npub);
  g_autofree gchar *watch_npub = NULL; gn_backup_get_npub("0000000000000000000000000000000000000000000000000000000000000002", &watch_npub, NULL);
  g_assert_true(accounts_store_import_pubkey(store, watch_npub, "Observer", NULL, NULL));
  SignerWindow *window = signer_window_new(app); g_object_ref(window); g_object_weak_ref(G_OBJECT(window), finalised, &windows_gone); gtk_window_present(GTK_WINDOW(window)); wait_frames(GTK_WIDGET(window));
  AdwViewStack *stack = ADW_VIEW_STACK(gtk_widget_get_template_child(GTK_WIDGET(window), TYPE_SIGNER_WINDOW, "stack"));
  GtkWidget *home = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(window), TYPE_SIGNER_WINDOW, "page_home"));
  const gchar *page_ids[] = {"page_home", "page_history", "page_connect", "page_settings"};
  for (guint i = 0; i < G_N_ELEMENTS(page_ids); i++) g_object_weak_ref(gtk_widget_get_template_child(GTK_WIDGET(window), TYPE_SIGNER_WINDOW, page_ids[i]), finalised, &pages_gone);
  GtkLabel *signed_today = GTK_LABEL(gtk_widget_get_template_child(home, GROTTO_TYPE_PAGE_HOME, "signed_today"));
  WAIT(!strcmp(gtk_label_get_text(signed_today), "117"));
  gboolean accessibility = g_getenv("G10_TEST_LANGUAGE") != NULL;
  g_autofree gchar *clients_name = g_strdup_printf(_("%s: %s"), _("Clients"), "1");
  g_autofree gchar *pending_name = g_strdup_printf(_("%s: %s"), _("Pending"), "0");
  if (accessibility) {
    gtk_test_accessible_assert_property(GTK_ACCESSIBLE(gtk_widget_get_template_child(home, GROTTO_TYPE_PAGE_HOME, "clients_count")), GTK_ACCESSIBLE_PROPERTY_LABEL, clients_name);
    gtk_test_accessible_assert_property(GTK_ACCESSIBLE(gtk_widget_get_template_child(home, GROTTO_TYPE_PAGE_HOME, "pending_count")), GTK_ACCESSIBLE_PROPERTY_LABEL, pending_name);
  }
  g_autofree gchar *signed_name = g_strdup_printf(_("%s: %s"), _("Signed today"), "117");
  if (accessibility) gtk_test_accessible_assert_property(GTK_ACCESSIBLE(signed_today), GTK_ACCESSIBLE_PROPERTY_LABEL, signed_name);
  GtkWidget *client_list = GTK_WIDGET(gtk_widget_get_template_child(home, GROTTO_TYPE_PAGE_HOME, "clients_list"));
  GtkWidget *client_row = gtk_widget_get_first_child(client_list); g_assert_nonnull(client_row);
  g_autofree gchar *client_name = g_strdup_printf(_("%s · %s · %s trust · %s"), "Jank", _("Browser bridge"), _("Medium"), adw_action_row_get_subtitle(ADW_ACTION_ROW(client_row)));
  if (accessibility) gtk_test_accessible_assert_property(GTK_ACCESSIBLE(client_row), GTK_ACCESSIBLE_PROPERTY_LABEL, client_name);
  GtkWidget *avatars = GTK_WIDGET(gtk_widget_get_template_child(home, GROTTO_TYPE_PAGE_HOME, "avatars"));
  GtkWidget *selected_avatar = NULL, *watch_avatar = NULL;
  for (GtkWidget *avatar = gtk_widget_get_first_child(avatars); avatar; avatar = gtk_widget_get_next_sibling(avatar)) {
    if (!g_strcmp0(g_object_get_data(G_OBJECT(avatar), "identity"), npub)) selected_avatar = avatar;
    if (!g_strcmp0(g_object_get_data(G_OBJECT(avatar), "identity"), watch_npub)) watch_avatar = avatar;
  }
  g_assert_nonnull(selected_avatar); g_assert_nonnull(watch_avatar);
  g_autofree gchar *watch_name = g_strdup_printf(_("%s · %s · %s"), "Observer", _("Watch only"), _("Not selected"));
  if (accessibility) gtk_test_accessible_assert_property(GTK_ACCESSIBLE(watch_avatar), GTK_ACCESSIBLE_PROPERTY_LABEL, watch_name);
  g_autofree gchar *identity_name = g_strdup_printf(_("%s · %s · %s"), "My identity", _("Signing identity"), _("Selected"));
  if (accessibility) gtk_test_accessible_assert_property(GTK_ACCESSIBLE(selected_avatar), GTK_ACCESSIBLE_PROPERTY_LABEL, identity_name);
  g_signal_emit_by_name(selected_avatar, "clicked");
  AdwDialog *identity_detail = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)); g_assert_nonnull(identity_detail);
  wait_frames(GTK_WIDGET(identity_detail)); adw_dialog_close(identity_detail);
  WAIT(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == NULL);
  g_assert_true(accounts_store_exists(store, npub)); /* live avatar route cannot delete implicitly */
  g_assert_cmpstr(adw_view_stack_get_visible_child_name(stack), ==, "home");
  g_autoptr(GtkSelectionModel) pages = adw_view_stack_get_pages(stack);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(pages)), ==, 4);
  g_autoptr(AdwViewStackPage) preferences = g_list_model_get_item(G_LIST_MODEL(pages), 3);
  g_assert_cmpstr(adw_view_stack_page_get_title(preferences), ==, _("Preferences"));
  const gchar *names[] = {"home", "activity", "connect", "settings"};
  for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
    signer_window_show_page(window, names[i]); wait_frames(GTK_WIDGET(window)); assert_visible_focus(window); g_assert_cmpstr(adw_view_stack_get_visible_child_name(stack), ==, names[i]);
  }
  signer_window_show_page(window, "home");
  GtkStack *main_stack = GTK_STACK(gtk_widget_get_template_child(GTK_WIDGET(window), TYPE_SIGNER_WINDOW, "main_stack"));
  GtkWidget *lock_screen = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(window), TYPE_SIGNER_WINDOW, "lock_screen"));
  gtk_stack_set_visible_child_name(main_stack, "locked"); wait_frames(GTK_WIDGET(window));
  GtkWidget *unlock = GTK_WIDGET(gtk_widget_get_template_child(lock_screen, GN_TYPE_LOCK_SCREEN, "btn_unlock"));
  g_assert_true(gtk_widget_grab_focus(unlock));
  gtk_window_set_default_size(GTK_WINDOW(window), 760, 640); wait_frames(GTK_WIDGET(window)); assert_visible_focus(window);
  gtk_window_set_default_size(GTK_WINDOW(window), 360, 640); wait_frames(GTK_WIDGET(window)); assert_visible_focus(window);
  gtk_stack_set_visible_child_name(main_stack, "unlocked"); gtk_window_set_default_size(GTK_WINDOW(window), 920, 640); wait_frames(GTK_WIDGET(window));
  WAIT(!strcmp(gtk_label_get_text(signed_today), "117"));
  AdwNavigationSplitView *split = ADW_NAVIGATION_SPLIT_VIEW(gtk_widget_get_template_child(GTK_WIDGET(window), TYPE_SIGNER_WINDOW, "split_view"));
  GtkListBox *sidebar = GTK_LIST_BOX(gtk_widget_get_template_child(GTK_WIDGET(window), TYPE_SIGNER_WINDOW, "sidebar"));
  gtk_window_set_default_size(GTK_WINDOW(window), 760, 640); wait_frames(GTK_WIDGET(window));
  adw_navigation_split_view_set_show_content(split, FALSE); wait_frames(GTK_WIDGET(window));
  g_assert_true(gtk_widget_grab_focus(GTK_WIDGET(gtk_list_box_get_row_at_index(sidebar, 0))));
  gtk_window_set_default_size(GTK_WINDOW(window), 360, 640); wait_frames(GTK_WIDGET(window)); assert_visible_focus(window);
  signer_window_show_page(window, "home"); gtk_window_set_default_size(GTK_WINDOW(window), 920, 640); wait_frames(GTK_WIDGET(window));
  GtkWidget *copy = GTK_WIDGET(gtk_widget_get_template_child(home, GROTTO_TYPE_PAGE_HOME, "copy_key"));
  g_assert_true(gtk_widget_get_sensitive(copy)); g_assert_true(gtk_widget_grab_focus(copy));
  g_assert_true(gtk_widget_child_focus(home, GTK_DIR_TAB_FORWARD));
  GtkWidget *qr = GTK_WIDGET(gtk_widget_get_template_child(home, GROTTO_TYPE_PAGE_HOME, "show_qr"));
  g_assert_true(gtk_root_get_focus(GTK_ROOT(window)) == qr);
  g_assert_cmpint(gtk_accessible_get_accessible_role(GTK_ACCESSIBLE(copy)), ==, GTK_ACCESSIBLE_ROLE_BUTTON);
  g_assert_cmpstr(gtk_widget_get_tooltip_text(copy), ==, _("Copy public key"));
  g_assert_cmpstr(gtk_widget_get_tooltip_text(qr), ==, _("Show public-key QR code"));
  for (guint dark = 0; dark < 2; dark++) {
    adw_style_manager_set_color_scheme(adw_style_manager_get_default(), dark ? ADW_COLOR_SCHEME_FORCE_DARK : ADW_COLOR_SCHEME_FORCE_LIGHT);
    gtk_window_set_default_size(GTK_WINDOW(window), 920, 640); snapshot(GTK_WIDGET(window), dark ? "home-dark" : "home-light");
    g_assert_true(gtk_widget_grab_focus(copy));
    gtk_window_set_default_size(GTK_WINDOW(window), 760, 640); wait_frames(GTK_WIDGET(window));
    assert_visible_focus(window); g_assert_true(gtk_root_get_focus(GTK_ROOT(window)) == copy);
    signer_window_show_page(window, "home"); g_assert_true(gtk_root_get_focus(GTK_ROOT(window)) == copy);
    gtk_window_set_default_size(GTK_WINDOW(window), 740, 640); wait_frames(GTK_WIDGET(window));
    g_assert_true(gtk_root_get_focus(GTK_ROOT(window)) == copy);
    gtk_window_set_default_size(GTK_WINDOW(window), 360, 640); wait_frames(GTK_WIDGET(window));
    /* Fixed GTK may choose another control when switching to bottom navigation. */
    assert_visible_focus(window);
    GtkWidget *toggle = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(window), TYPE_SIGNER_WINDOW, "sidebar_toggle_btn"));
    gtk_window_set_default_size(GTK_WINDOW(window), 740, 640); wait_frames(GTK_WIDGET(window));
    g_assert_true(gtk_widget_grab_focus(toggle));
    gtk_window_set_default_size(GTK_WINDOW(window), 360, 640); wait_frames(GTK_WIDGET(window));
    assert_visible_focus(window); g_assert_true(gtk_root_get_focus(GTK_ROOT(window)) != toggle);
    g_assert_cmpint(gtk_widget_get_width(GTK_WIDGET(window)), <=, 360); snapshot(GTK_WIDGET(window), dark ? "home-360-dark" : "home-360-light");
    g_signal_emit_by_name(home, "client-selected", "https://jank.example"); snapshot(GTK_WIDGET(window), dark ? "client-dark" : "client-light");
    AdwNavigationView *nav = ADW_NAVIGATION_VIEW(gtk_widget_get_template_child(GTK_WIDGET(window), TYPE_SIGNER_WINDOW, "detail_navigation"));
    GtkWidget *detail = GTK_WIDGET(adw_navigation_view_get_visible_page(nav)); g_assert_true(GROTTO_IS_CLIENT_DETAIL(detail));
    g_object_weak_ref(G_OBJECT(detail), finalised, &pages_gone);
    GtkWidget *permission = GTK_WIDGET(gtk_widget_get_template_child(detail, GROTTO_TYPE_CLIENT_DETAIL, "permissions"));
    g_signal_emit_by_name(permission, "activated"); snapshot(GTK_WIDGET(window), dark ? "permissions-dark" : "permissions-light");
    if (!dark) {
      GtkWidget *low = GTK_WIDGET(gtk_widget_get_template_child(detail, GROTTO_TYPE_CLIENT_DETAIL, "low"));
      WAIT(gtk_widget_is_sensitive(low)); gtk_check_button_set_active(GTK_CHECK_BUTTON(low), TRUE);
      WAIT(tier_writes == 1 && gtk_widget_is_sensitive(low));
      GtkWidget *remembered = NULL; WAIT((remembered = find_permission(detail, npub, "allow")) && gtk_widget_is_sensitive(remembered));
      g_signal_emit_by_name(remembered, "activated");
      AdwDialog *editor = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)); g_assert_nonnull(editor);
      g_object_weak_ref(G_OBJECT(editor), finalised, &editors_gone);
      wait_frames(GTK_WIDGET(editor));
      GtkWidget *decision = find_decision(GTK_WIDGET(editor)); g_assert_nonnull(decision); gtk_drop_down_set_selected(GTK_DROP_DOWN(decision), 1);
      GtkWidget *save = find_button(GTK_WIDGET(editor), _("Save Permission")); g_assert_nonnull(save);
      force_conflict = TRUE; g_signal_emit_by_name(save, "clicked");
      WAIT(permission_attempts == 1 && gtk_widget_get_sensitive(save) && gtk_widget_is_sensitive(low));
      wait_frames(GTK_WIDGET(editor));
      AdwBanner *feedback = ADW_BANNER(find_banner(detail)); g_assert_nonnull(feedback); g_assert_true(adw_banner_get_revealed(feedback));
      g_assert_cmpstr(adw_banner_get_title(feedback), ==, _("Permission not saved: another request changed the permissions. Refreshing now; review your edit and retry."));
      g_assert_true(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == editor);
      AdwBanner *editor_feedback = ADW_BANNER(find_banner(GTK_WIDGET(editor))); g_assert_nonnull(editor_feedback);
      g_assert_true(adw_banner_get_revealed(editor_feedback)); g_assert_true(gtk_widget_get_mapped(GTK_WIDGET(editor_feedback)));
      g_assert_cmpstr(adw_banner_get_title(editor_feedback), ==, adw_banner_get_title(feedback));
      snapshot(GTK_WIDGET(window), "permission-conflict");
      g_assert_cmpuint(permission_writes, ==, 0); g_assert_true(contains_permission(npub, "allow"));
      g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(save)), ==, _("Retry Permission"));
      g_signal_emit_by_name(editor_feedback, "button-clicked"); WAIT(permission_writes == 1 && gtk_widget_is_sensitive(low));
      WAIT(editors_gone == 1 && adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == NULL);
      g_assert_false(contains_permission(npub, "allow")); g_assert_true(contains_permission(npub, "deny"));
      g_assert_cmpuint(g_variant_n_children(permission_rows), ==, 4); /* concurrent deny was preserved */
      GtkWidget *legacy = NULL; WAIT((legacy = find_permission(detail, "*", "review")) && gtk_widget_is_sensitive(legacy));
      g_signal_emit_by_name(legacy, "activated"); editor = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
      g_object_weak_ref(G_OBJECT(editor), finalised, &editors_gone); wait_frames(GTK_WIDGET(editor));
      GtkWidget *identity_picker = find_identity_picker(GTK_WIDGET(editor)); g_assert_nonnull(identity_picker);
      g_assert_cmpuint(gtk_drop_down_get_selected(GTK_DROP_DOWN(identity_picker)), ==, 0);
      snapshot(GTK_WIDGET(window), "permission-review-identity");
      g_assert_cmpuint(g_list_model_get_n_items(gtk_drop_down_get_model(GTK_DROP_DOWN(identity_picker))), ==, 2); /* placeholder + signing identity, watch-only excluded */
      GtkWidget *all_types = find_editor_control(GTK_WIDGET(editor), GTK_TYPE_CHECK_BUTTON);
      GtkWidget *kind_input = find_editor_control(GTK_WIDGET(editor), GTK_TYPE_SPIN_BUTTON);
      g_assert_nonnull(all_types); g_assert_nonnull(kind_input); g_assert_true(gtk_check_button_get_active(GTK_CHECK_BUTTON(all_types)));
      gtk_check_button_set_active(GTK_CHECK_BUTTON(all_types), FALSE);
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(kind_input), NOSTR_KIND_TEXT_NOTE);
      save = find_button(GTK_WIDGET(editor), _("Save Permission")); g_signal_emit_by_name(save, "clicked");
      g_assert_cmpuint(permission_attempts, ==, 2); /* no wildcard allow may be submitted */
      gtk_drop_down_set_selected(GTK_DROP_DOWN(identity_picker), 1); conflict_cleanup = TRUE;
      g_signal_emit_by_name(save, "clicked");
      WAIT(permission_writes == 2 && !conflict_cleanup && gtk_widget_is_sensitive(save) && gtk_widget_is_sensitive(low));
      g_assert_true(contains_permission(npub, "allow")); g_assert_true(contains_permission("*", "review"));
      g_assert_cmpstr(adw_banner_get_title(feedback), ==, _("Scoped permission saved, but the legacy review entry was not removed. Review the refreshed entries and retry removal."));
      g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(save)), ==, _("Retry Removing Legacy Entry"));
      g_signal_emit_by_name(save, "clicked"); WAIT(permission_writes == 3);
      WAIT(editors_gone == 2 && adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == NULL);
      g_assert_true(scoped_saved_before_cleanup); g_assert_false(contains_permission("*", "review"));
      g_assert_true(contains_permission(npub, "allow"));
      WAIT((legacy = find_permission(detail, npub, "review")) && gtk_widget_is_sensitive(legacy));
      g_signal_emit_by_name(legacy, "activated"); editor = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
      g_object_weak_ref(G_OBJECT(editor), finalised, &editors_gone); wait_frames(GTK_WIDGET(editor));
      g_assert_null(find_identity_picker(GTK_WIDGET(editor)));
      save = find_button(GTK_WIDGET(editor), _("Save Permission")); g_signal_emit_by_name(save, "clicked");
      WAIT(permission_writes == 4);
      WAIT(editors_gone == 3 && adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == NULL);
      g_assert_false(contains_permission(npub, "review"));
      /* A separate migrated wildcard row is clearable without creating a grant. */
      GVariantBuilder migration; g_variant_builder_init(&migration, G_VARIANT_TYPE("aa{sv}"));
      for (gsize i = 0; i < g_variant_n_children(permission_rows); i++) { g_autoptr(GVariant) row = g_variant_get_child_value(permission_rows, i); g_variant_builder_add_value(&migration, row); }
      fixture_permission(&migration, "*", -1, "review");
      g_clear_pointer(&permission_rows, g_variant_unref); permission_rows = g_variant_ref_sink(g_variant_builder_end(&migration)); revision++;
      g_dbus_connection_emit_signal(server, NULL, "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "ClientsChanged", g_variant_new("(t)", revision), NULL);
      WAIT((legacy = find_permission(detail, "*", "review")) && gtk_widget_is_sensitive(legacy));
      g_signal_emit_by_name(legacy, "activated"); editor = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
      g_object_weak_ref(G_OBJECT(editor), finalised, &editors_gone); wait_frames(GTK_WIDGET(editor));
      g_signal_emit_by_name(find_button(GTK_WIDGET(editor), _("Remove Permission")), "clicked"); WAIT(permission_writes == 5);
      WAIT(editors_gone == 4 && adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == NULL);
      g_assert_false(contains_permission("*", "review"));
      fail_permission_read = TRUE; g_signal_emit_by_name(feedback, "button-clicked");
      WAIT(!strcmp(adw_banner_get_title(feedback), _("Permissions could not be loaded. No edit was saved. Refresh to try again.")));
      g_assert_true(adw_banner_get_revealed(feedback));
      g_signal_emit_by_name(feedback, "button-clicked");
      WAIT(!strcmp(adw_banner_get_title(feedback), _("Permissions refreshed. Review the entries before retrying.")) && gtk_widget_is_sensitive(low));
    }
    signer_window_show_page(window, "connect"); wait_frames(GTK_WIDGET(window)); assert_visible_focus(window); snapshot(GTK_WIDGET(window), dark ? "connect-dark" : "connect-light");
    signer_window_show_page(window, "settings"); wait_frames(GTK_WIDGET(window)); assert_visible_focus(window); snapshot(GTK_WIDGET(window), dark ? "settings-dark" : "settings-light");
    signer_window_show_page(window, "home"); wait_frames(GTK_WIDGET(window)); assert_visible_focus(window);
  }
  GtkBuilder *shortcuts = gtk_builder_new_from_resource("/org/gnostr/signer/ui/shortcuts-window.ui");
  g_assert_true(GTK_IS_SHORTCUTS_WINDOW(gtk_builder_get_object(shortcuts, "shortcuts_window"))); gtk_window_destroy(GTK_WINDOW(gtk_builder_get_object(shortcuts, "shortcuts_window"))); g_object_unref(shortcuts);
  g_clear_object(&pages);
  gtk_window_destroy(GTK_WINDOW(window)); g_object_unref(window);
  WAIT(windows_gone == 1);
  WAIT(pages_gone == 6);
  WAIT(editors_gone == 4);
  g_application_quit(application);
}
static void layout(void) {
  guint apps_gone = 0;
  g_autoptr(AdwApplication) app = adw_application_new("org.nostr.Grotto.LayoutTest", G_APPLICATION_NON_UNIQUE);
  g_object_weak_ref(G_OBJECT(app), finalised, &apps_gone);
  g_signal_connect(app, "activate", G_CALLBACK(layout_activate), NULL);
  g_assert_cmpint(g_application_run(G_APPLICATION(app), 0, NULL), ==, 0);
  g_clear_object(&app);
  WAIT(apps_gone == 1);
}
int main(int argc, char **argv) {
  if (g_getenv("GNOSTR_SKIP_UI_TESTS")) { g_print("1..0 # SKIP native UI disabled\n"); return 77; }
  const gchar *language = g_getenv("G10_TEST_LANGUAGE");
  if (language) {
    g_assert_cmpstr(language, ==, "es"); g_setenv("LANGUAGE", language, TRUE); g_assert_nonnull(setlocale(LC_ALL, "en_US.UTF-8"));
    bindtextdomain(GETTEXT_PACKAGE, G10_TEST_LOCALE_DIR); textdomain(GETTEXT_PACKAGE); bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
    g_assert_cmpstr(_("Allow"), ==, "Permitir");
    g_assert_cmpstr(_("Custom event"), ==, "Evento personalizado");
  }
  g_test_init(&argc, &argv, NULL); nostrc_test_tolerate_gdk_frame_warning();
  if (!gtk_init_check()) { g_print("1..0 # SKIP GTK display unavailable\n"); return 77; }
  adw_init(); g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
  GtkCssProvider *css = gtk_css_provider_new(); gtk_css_provider_load_from_resource(css, "/org/gnostr/signer/css/app.css");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION); g_object_unref(css);
  nostrc_test_bus_tolerate_ebadf(); NostrcTestBus *bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE); nostrc_test_bus_up(bus); server = nostrc_test_bus_connect(bus);
  gn_backup_get_npub("0000000000000000000000000000000000000000000000000000000000000001", &npub, NULL);
  expires_at = g_get_real_time() / G_USEC_PER_SEC + 600;
  GVariantBuilder initial; g_variant_builder_init(&initial, G_VARIANT_TYPE("aa{sv}"));
  fixture_permission(&initial, npub, NOSTR_KIND_FOLLOW_LIST, "allow");
  fixture_permission(&initial, "*", -1, "review");
  fixture_permission(&initial, npub, NOSTR_KIND_PROFILE_METADATA, "review");
  permission_rows = g_variant_ref_sink(g_variant_builder_end(&initial));
  g_autofree gchar *xml = NULL; g_file_get_contents(G10_CONTROL_XML, &xml, NULL, NULL); g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml(xml, NULL);
  guint registration = g_dbus_connection_register_object(server, "/org/nostr/signer/grotto", node->interfaces[0], &vtable, NULL, NULL, NULL);
  g_autoptr(GDBusNodeInfo) identity = g_dbus_node_info_new_for_xml("<node><interface name='org.nostr.Signer'><method name='ListIdentities'><arg type='as' direction='out'/></method></interface></node>", NULL);
  guint inventory = g_dbus_connection_register_object(server, "/org/nostr/signer", identity->interfaces[0], &vtable, NULL, NULL, NULL);
  g_autoptr(GVariant) name = g_dbus_connection_call_sync(server, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", "org.nostr.Signer", 0u), NULL, 0, 5000, NULL, NULL); g_assert_nonnull(name);
  g_test_add_func("/signer/layout/real-templates-navigation-focus-screenshots", layout); int result = g_test_run();
  accounts_store_free(accounts_store_get_default()); settings_manager_free(settings_manager_get_default());
  g_dbus_connection_unregister_object(server, registration); g_dbus_connection_unregister_object(server, inventory); nostrc_test_bus_down(bus); g_variant_unref(permission_rows); g_free(npub); return result;
}
