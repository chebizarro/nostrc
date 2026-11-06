#include "ui/signer-window.h"
#include "bunker_test_relay.h"
#include "relay_store.h"
#include <grotto-panels.h>
#ifdef GROTTO_TEST_XDOTOOL
#include <gdk/x11/gdkx.h>
#endif
#include "ui/lock-screen.h"
#include "ui/onboarding-assistant.h"
#include "ui/page-connect.h"
#include "ui/pairing-flow.h"
#include "ui/widgets/gn-secure-entry.h"
#include "ui/sheets/sheet-create-bunker.h"
#include "ui/sheets/sheet-qr-display.h"
#include "ui/sheets/sheet-qr-scanner.h"
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
#include <stdlib.h>
#include <locale.h>
#define P_(s) g_dgettext("grotto-panels", s)
static GtkWidget *selected_panel(AdwNavigationSplitView *split) {
  return gtk_stack_get_visible_child(GTK_STACK(adw_navigation_page_get_child(adw_navigation_split_view_get_content(split))));
}

static GDBusMethodInvocation *layout_self_test;
static GDBusConnection *server; static gchar *npub; static guint frames, tier_writes, permission_writes;
static const gchar *tier = "medium";
static guint64 revision = 12, expires_at;
static GVariant *permission_rows;
static gboolean force_conflict, scoped_saved_before_cleanup, fail_permission_read, conflict_cleanup, pending_request;
static guint permission_attempts, permission_conflicts, seed_exports;
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
  g_variant_builder_add(&b, "{sv}", "state", g_variant_new_string((!strcmp(decision, "review") || !strcmp(decision, "policy-review")) ? "inactive" : "active"));
  g_variant_builder_add(&b, "{sv}", "expires-at", g_variant_new_uint64(!strcmp(decision, "policy-review") ? 0 : expires_at));
  if (!strcmp(decision, "policy-review")) {
    g_variant_builder_add(&b, "{sv}", "reason", g_variant_new_string("Pending policy review"));
    g_variant_builder_add(&b, "{sv}", "legacy-provenance", g_variant_new_string("pre-policy-v3"));
  }
  g_variant_builder_add_value(rows, g_variant_builder_end(&b));
}
static gboolean bound(gpointer data) { g_error("UI layout did not reach barrier: %s", (const gchar *)data); return G_SOURCE_REMOVE; }
#define WAIT(condition) do { guint timer = g_timeout_add_seconds(15, bound, #condition); while (!(condition)) g_main_context_iteration(NULL, TRUE); g_source_remove(timer); } while (0)
static gboolean tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data) { (void)widget; (void)clock; (void)data; frames++; return G_SOURCE_CONTINUE; }
static void wait_frames(GtkWidget *widget) {
  frames = 0; guint id = gtk_widget_add_tick_callback(widget, tick, NULL, NULL); WAIT(frames >= 3); gtk_widget_remove_tick_callback(widget, id);
}
#ifdef GROTTO_TEST_XDOTOOL
static void send_key(SignerWindow *window, const char *key) {
  GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window)); g_assert_true(GDK_IS_X11_SURFACE(surface));
  g_autofree char *id = g_strdup_printf("%lu", gdk_x11_surface_get_xid(surface));
  char *argv[] = {GROTTO_TEST_XDOTOOL, "windowfocus", id, "key", "--clearmodifiers", "--delay", "0", (char *)key, NULL};
  g_autoptr(GError) error = NULL; gint status;
  g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL, &status, &error));
  g_assert_no_error(error); g_assert_true(g_spawn_check_wait_status(status, &error)); g_assert_no_error(error);
  wait_frames(GTK_WIDGET(window));
}
#endif
static void assert_focus_bounds(SignerWindow *window) {
  GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(window)); g_assert_nonnull(focus);
  graphene_rect_t bounds; g_assert_true(gtk_widget_compute_bounds(focus, GTK_WIDGET(window), &bounds));
  g_assert_cmpfloat(bounds.origin.x, >=, 0); g_assert_cmpfloat(bounds.origin.y, >=, 0);
  g_assert_cmpfloat(bounds.origin.x + bounds.size.width, <=, gtk_widget_get_width(GTK_WIDGET(window)) + 1);
  g_assert_cmpfloat(bounds.origin.y + bounds.size.height, <=, gtk_widget_get_height(GTK_WIDGET(window)) + 1);
}
static void report_wide_widgets(GtkWidget *widget, int width) {
  int minimum = 0; gtk_widget_measure(widget, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, NULL, NULL, NULL);
  if (minimum > width) g_test_message("Too wide: %s name=%s minimum=%d", G_OBJECT_TYPE_NAME(widget), gtk_widget_get_name(widget), minimum);
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) report_wide_widgets(child, width);
}
static gboolean has_visible_toast(GtkWidget *widget) {
  if (!strcmp(G_OBJECT_TYPE_NAME(widget), "AdwToastWidget") && gtk_widget_get_mapped(widget)) return TRUE;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
    if (has_visible_toast(child)) return TRUE;
  return FALSE;
}
static void snapshot(GtkWidget *widget, const gchar *name) {
  /* Observe the actual toast lifecycle. Never dismiss feedback or alter pixels
   * to make documentation captures look settled. Queued writes may take longer. */
  guint toast_deadline = g_timeout_add_seconds(60, bound, "production toasts dismissed naturally");
  for (;;) {
    /* A newly queued toast is not mapped until the next frame. */
    wait_frames(widget);
    if (!has_visible_toast(widget)) break;
    while (has_visible_toast(widget)) g_main_context_iteration(NULL, TRUE);
  }
  g_source_remove(toast_deadline);
  g_assert_false(has_visible_toast(widget)); int width = gtk_widget_get_width(widget), height = gtk_widget_get_height(widget);
  g_assert_cmpint(width, >, 0); g_assert_cmpint(height, >, 0);
  if (width <= 360 && SIGNER_IS_WINDOW(widget)) {
    GtkWidget *overlay = GTK_WIDGET(gtk_widget_get_template_child(widget, TYPE_SIGNER_WINDOW, "toast_overlay"));
    AdwNavigationSplitView *split = ADW_NAVIGATION_SPLIT_VIEW(gtk_widget_get_template_child(widget, TYPE_SIGNER_WINDOW, "split_view"));
    g_assert_true(adw_navigation_split_view_get_collapsed(split));
    int minimum; gtk_widget_measure(overlay, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, NULL, NULL, NULL);
    if (minimum > width) { g_test_message("Narrow snapshot: %s", name); report_wide_widgets(overlay, width); }
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
static GVariant *pending_metadata(void) {
  GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&b, "{sv}", "request_id", g_variant_new_string("w4-request"));
  g_variant_builder_add(&b, "{sv}", "principal", g_variant_new_string("https://jank.example"));
  g_variant_builder_add(&b, "{sv}", "identity", g_variant_new_string(npub));
  g_variant_builder_add(&b, "{sv}", "operation", g_variant_new_string("sign_event"));
  g_variant_builder_add(&b, "{sv}", "event_kind", g_variant_new_uint32(NOSTR_KIND_FOLLOW_LIST));
  g_variant_builder_add(&b, "{sv}", "state", g_variant_new_string("PENDING"));
  g_variant_builder_add(&b, "{sv}", "transport", g_variant_new_string("DBUS"));
  g_variant_builder_add(&b, "{sv}", "remaining_seconds", g_variant_new_uint32(60));
  return g_variant_builder_end(&b);
}
static void call(GDBusConnection *bus, const gchar *sender, const gchar *path, const gchar *interface, const gchar *method, GVariant *args, GDBusMethodInvocation *inv, gpointer data) {
  (void)bus; (void)sender; (void)path; (void)interface; (void)args; (void)data;
  if (!strcmp(method, "StoreKeyWithMnemonic")) { g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", npub)); return; }
  if (!strcmp(method, "SelfTestIdentity")) { g_assert_null(layout_self_test); layout_self_test = g_object_ref(inv); return; }
  if (!strcmp(method, "CancelIdentitySelfTest")) { g_dbus_method_invocation_return_value(inv, NULL); return; }
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
      /* Each conflict represents a distinct concurrently saved scope. */
      fixture_permission(&refreshed, npub, permission_conflicts++ ? NOSTR_KIND_REPOST : NOSTR_KIND_REACTION, "deny");
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
      GVariantDict d; g_variant_dict_init(&d, row); g_variant_dict_insert(&d, "state", "s", (!strcmp(row_text(row, "decision"), "review") || !strcmp(row_text(row, "decision"), "policy-review")) ? "inactive" : "active");
      g_variant_builder_add_value(&next, g_variant_dict_end(&d));
    }
    gboolean legacy_present = contains_permission("*", "review");
    g_clear_pointer(&permission_rows, g_variant_unref); permission_rows = g_variant_ref_sink(g_variant_builder_end(&next));
    if (strcmp(identity, "*") && legacy_present && contains_permission(npub, "allow")) scoped_saved_before_cleanup = TRUE;
    if (!strcmp(identity, "*") && !contains_permission("*", "review")) g_assert_true(scoped_saved_before_cleanup || !g_variant_n_children(rows));
    revision++; permission_writes++;
    g_dbus_method_invocation_return_value(inv, NULL); g_dbus_connection_emit_signal(bus, NULL, path, interface, "ClientsChanged", g_variant_new("(t)", revision), NULL); return;
  }
  if (!strcmp(method, "GetRequest")) { g_dbus_method_invocation_return_value(inv, g_variant_new("(@a{sv})", pending_metadata())); return; }
  if (!strcmp(method, "GetIdentityMigration")) { g_dbus_method_invocation_return_dbus_error(inv, "org.nostr.Grotto.Control1.Error.NotFound", "No saved migration"); return; }
  if (!strcmp(method, "ExportIdentity")) {
    const char *identity, *format, *pass; g_variant_get(args, "(&s&s&s)", &identity, &format, &pass);
    g_assert_cmpstr(identity, ==, npub); g_assert_cmpstr(format, ==, "mnemonic"); g_assert_cmpstr(pass, ==, ""); seed_exports++;
    g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about")); return;
  }
  if (!strcmp(method, "ListIdentities")) { const gchar *ids[] = {npub, NULL}; g_dbus_method_invocation_return_value(inv, g_variant_new("(^as)", ids)); return; }
  if (!strcmp(method, "GetStatus")) {
    GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
#define ADD(k,v) g_variant_builder_add(&b, "{sv}", k, v)
    GVariantBuilder caps; g_variant_builder_init(&caps, G_VARIANT_TYPE("a{sb}"));
    const char *capabilities[] = {"store-mnemonic", "mnemonic-export", "export-identity", "raw-nsec-export", "identity-self-test", "rotate-identity", "decision-modes"};
    for (guint i = 0; i < G_N_ELEMENTS(capabilities); i++) g_variant_builder_add(&caps, "{sb}", capabilities[i], TRUE);
    ADD("capabilities", g_variant_builder_end(&caps));
    ADD("control_version", g_variant_new_uint32(1)); ADD("permission_policy_version", g_variant_new_uint32(3)); ADD("control_epoch", g_variant_new_string("fixture")); ADD("permissions_revision", g_variant_new_uint64(revision));
    ADD("signed_today", g_variant_new_uint32(117)); ADD("client_count", g_variant_new_uint32(1)); ADD("pending_count", g_variant_new_uint32(0)); ADD("bunker_state", g_variant_new_string("DISABLED"));
    g_dbus_method_invocation_return_value(inv, g_variant_new("(@a{sv})", g_variant_builder_end(&b))); return;
  }
  GVariantBuilder rows; g_variant_builder_init(&rows, G_VARIANT_TYPE("aa{sv}"));
  if (!strcmp(method, "ListRequests") && pending_request) g_variant_builder_add_value(&rows, pending_metadata());
  if (!strcmp(method, "ListClients")) {
    GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    ADD("principal", g_variant_new_string("https://jank.example")); ADD("label", g_variant_new_string("Jank")); ADD("transport", g_variant_new_string("DBUS"));
    ADD("trust-tier", g_variant_new_string(tier)); ADD("identity", g_variant_new_string(npub)); ADD("first-consented-at", g_variant_new_uint64(g_get_real_time() / G_USEC_PER_SEC - 600));
    ADD("last-used-at", g_variant_new_uint64(g_get_real_time() / G_USEC_PER_SEC)); ADD("permission-count", g_variant_new_uint32(1));
    g_variant_builder_add_value(&rows, g_variant_builder_end(&b));
  } else if (!strcmp(method, "ListPermissions")) {
    if (fail_permission_read) { g_variant_builder_clear(&rows); g_dbus_method_invocation_return_dbus_error(inv, "org.nostr.Grotto.Control1.Error.Unavailable", "read failed"); return; }
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
static GtkTextView *find_text_view(GtkWidget *widget) {
  if (GTK_IS_TEXT_VIEW(widget)) return GTK_TEXT_VIEW(widget);
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    GtkTextView *found = find_text_view(child); if (found) return found;
  }
  return NULL;
}
static GtkWidget *find_button(GtkWidget *widget, const gchar *label) {
  if (GTK_IS_BUTTON(widget) && !g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), label)) return widget;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) { GtkWidget *found = find_button(child, label); if (found) return found; }
  return NULL;
}
static GtkWidget *find_permission(GtkWidget *widget, const gchar *identity, const gchar *decision) {
  GrottoPanelsItem *item = g_object_get_data(G_OBJECT(widget), "permission");
  const GrottoPanelsMetadata *row = item ? grotto_panels_item_get_metadata(item) : NULL;
  if (row && !strcmp(row->public_key, identity) && !strcmp(row->decision, decision)) return widget;
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
  if (ADW_IS_COMBO_ROW(widget)) {
    GListModel *model = adw_combo_row_get_model(ADW_COMBO_ROW(widget));
    g_autoptr(GtkStringObject) first = model && g_list_model_get_n_items(model) ? g_list_model_get_item(model, 0) : NULL;
    if (first && !strcmp(gtk_string_object_get_string(first), P_("Allow"))) return widget;
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
  if (ADW_IS_COMBO_ROW(widget)) {
    GListModel *model = adw_combo_row_get_model(ADW_COMBO_ROW(widget));
    g_autoptr(GtkStringObject) first = model && g_list_model_get_n_items(model) ? g_list_model_get_item(model, 0) : NULL;
    if (first && !strcmp(gtk_string_object_get_string(first), P_("Choose an Identity"))) return widget;
  }
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) { GtkWidget *found = find_identity_picker(child); if (found) return found; }
  return NULL;
}
static GtkWidget *find_relay_permission(GtkWidget *widget, const gchar *relay) {
  GrottoPanelsItem *item = g_object_get_data(G_OBJECT(widget), "permission");
  const GrottoPanelsMetadata *row = item ? grotto_panels_item_get_metadata(item) : NULL;
  if (row && !strcmp(row->relay, relay)) return widget;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_relay_permission(child, relay); if (found) return found;
  }
  return NULL;
}
static GtkWidget *find_expiry(GtkWidget *widget) {
  if (ADW_IS_COMBO_ROW(widget)) {
    GListModel *model = adw_combo_row_get_model(ADW_COMBO_ROW(widget));
    g_autoptr(GtkStringObject) first = model && g_list_model_get_n_items(model) ? g_list_model_get_item(model, 0) : NULL;
    if (first && !strcmp(gtk_string_object_get_string(first), P_("Keep Current Expiry"))) return widget;
  }
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_expiry(child); if (found) return found;
  }
  return NULL;
}
static GtkWidget *find_row(GtkWidget *widget, const char *title) {
  if (ADW_IS_PREFERENCES_ROW(widget) && !g_strcmp0(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(widget)), title)) return widget;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_row(child, title); if (found) return found;
  }
  return NULL;
}
static GtkWidget *find_clear_scope(GtkWidget *widget, const char *identity) {
  GrottoPanelsItem *item = g_object_get_data(G_OBJECT(widget), "clear-scope");
  if (item && !strcmp(grotto_panels_item_get_metadata(item)->public_key, identity)) return widget;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_clear_scope(child, identity); if (found) return found;
  }
  return NULL;
}
static void finalised(gpointer data, GObject *object) { (void)object; (*(guint *)data)++; }
static void layout_activate(GApplication *application, gpointer data) {
  (void)data; AdwApplication *app = ADW_APPLICATION(application);
  guint windows_gone = 0, pages_gone = 0, editors_gone = 0;
  AccountsStore *store = accounts_store_get_default(); g_assert_true(accounts_store_add(store, npub, "My identity", NULL));
  g_autofree gchar *watch_npub = NULL; gn_backup_get_npub("0000000000000000000000000000000000000000000000000000000000000002", &watch_npub, NULL);
  g_assert_true(accounts_store_import_pubkey(store, watch_npub, "Observer", NULL, NULL));
  g_settings_set_string(settings_manager_get_gsettings(settings_manager_get_default()), "default-identity", npub);
  g_autoptr(GrottoApprovalController) controller = grotto_approval_controller_new(G_APPLICATION(app), server);
  SignerWindow *window = signer_window_new(app);
  signer_window_set_pending_group(window, grotto_approval_pending_group_new(controller)); g_object_ref(window); g_object_weak_ref(G_OBJECT(window), finalised, &windows_gone); gtk_window_present(GTK_WINDOW(window)); wait_frames(GTK_WIDGET(window));
  AdwNavigationSplitView *split = ADW_NAVIGATION_SPLIT_VIEW(gtk_widget_get_template_child(GTK_WIDGET(window), TYPE_SIGNER_WINDOW, "split_view"));
  GtkListBox *sidebar = GTK_LIST_BOX(gtk_widget_get_template_child(GTK_WIDGET(window), TYPE_SIGNER_WINDOW, "sidebar"));
  gboolean accessibility = g_getenv("G10_TEST_LANGUAGE") != NULL;
  GtkWidget *root_panel = GTK_WIDGET(selected_panel(split));
  g_assert_true(GROTTO_IS_IDENTITY_PANEL(root_panel)); /* The real launch default. */
  const char *names[] = {"identity", "apps", "activity", "remote-signing"};
  const char *titles[] = {"Identity", "Connected Apps", "Activity", "Remote Signing"};
  for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
    signer_window_show_page(window, names[i]); wait_frames(GTK_WIDGET(window)); assert_visible_focus(window);
    GrottoPanel *panel = GROTTO_PANEL(selected_panel(split));
    g_object_weak_ref(G_OBJECT(panel), finalised, &pages_gone);
    g_assert_cmpstr(adw_navigation_page_get_title(ADW_NAVIGATION_PAGE(panel)), ==, P_(titles[i]));
    g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(gtk_list_box_get_row_at_index(sidebar, i))), ==, P_(titles[i]));
  }
  g_assert_null(gtk_list_box_get_row_at_index(sidebar, 4));
  signer_window_show_identity(window, NULL); wait_frames(GTK_WIDGET(window));
  WAIT(gtk_widget_get_visible(GTK_WIDGET(gtk_widget_get_template_child(root_panel, GROTTO_TYPE_IDENTITY_PANEL, "other_group"))));
  g_assert_true(gtk_widget_get_visible(GTK_WIDGET(gtk_widget_get_template_child(root_panel, GROTTO_TYPE_IDENTITY_PANEL, "add_group"))));
  g_assert_null(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  AdwNavigationView *root_navigation = grotto_identity_panel_get_navigation(GROTTO_IDENTITY_PANEL(root_panel));
  GtkWidget *other_list = GTK_WIDGET(gtk_widget_get_template_child(root_panel, GROTTO_TYPE_IDENTITY_PANEL, "other_list"));
  GtkWidget *other = gtk_widget_get_first_child(other_list); g_assert_nonnull(other);
  g_assert_null(gtk_widget_get_next_sibling(other)); /* All two adapter identities: default + watch-only. */
  g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(other)), ==, "Observer");
  g_assert_true(gtk_widget_grab_focus(other));
#ifdef GROTTO_TEST_XDOTOOL
  send_key(window, "Return");
#else
  g_signal_emit_by_name(other_list, "row-activated", other);
#endif
  GtkWidget *pinned_panel = GTK_WIDGET(adw_navigation_view_get_visible_page(root_navigation));
  g_assert_true(GROTTO_IS_IDENTITY_PANEL(pinned_panel));
  GtkWidget *pinned_key = GTK_WIDGET(gtk_widget_get_template_child(pinned_panel, GROTTO_TYPE_IDENTITY_PANEL, "key_row"));
  g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(pinned_key)), ==, watch_npub);
#ifdef GROTTO_TEST_XDOTOOL
  send_key(window, "alt+Left");
#else
  g_assert_true(adw_navigation_view_pop(root_navigation));
#endif
  g_assert_false(GROTTO_IS_IDENTITY_PANEL(adw_navigation_view_get_visible_page(root_navigation)));
  g_autofree char *unchanged_default = g_settings_get_string(settings_manager_get_gsettings(settings_manager_get_default()), "default-identity");
  g_assert_cmpstr(unchanged_default, ==, npub);
  g_assert_true(accounts_store_exists(store, npub));
  g_assert_true(GROTTO_IS_IDENTITY_PANEL(selected_panel(split)));
  GtkWidget *add_identity = GTK_WIDGET(gtk_widget_get_template_child(root_panel, GROTTO_TYPE_IDENTITY_PANEL, "add_identity"));
  g_assert_true(gtk_widget_has_css_class(add_identity, "flat"));
  g_signal_emit_by_name(add_identity, "clicked");
  OnboardingAssistant *add = g_object_get_data(G_OBJECT(window), "grotto-onboarding-assistant");
  g_assert_true(ONBOARDING_IS_ASSISTANT(add)); g_object_ref(add);
  g_assert_cmpint(onboarding_assistant_get_state(add), ==, ONBOARDING_CHOICE);
  GtkWidget *create_choice = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(add), TYPE_ONBOARDING_ASSISTANT, "choose_create"));
  GtkWidget *import_choice = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(add), TYPE_ONBOARDING_ASSISTANT, "choose_import"));
  g_assert_true(gtk_widget_get_visible(create_choice)); g_assert_true(gtk_widget_get_visible(import_choice));
  WAIT(gtk_widget_is_sensitive(import_choice)); g_signal_emit_by_name(import_choice, "activated");
  g_assert_cmpint(onboarding_assistant_get_state(add), ==, ONBOARDING_IMPORT);
  gtk_window_close(GTK_WINDOW(add)); g_object_unref(add);
  WAIT(g_object_get_data(G_OBJECT(window), "grotto-onboarding-assistant") == NULL);
  signer_window_show_page(window, "preferences"); wait_frames(GTK_WIDGET(window));
  AdwDialog *preferences = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_true(ADW_IS_PREFERENCES_DIALOG(preferences));
  g_assert_null(find_row(GTK_WIDGET(preferences), _("Appearance")));
  g_assert_null(find_row(GTK_WIDGET(preferences), _("Language")));
  adw_dialog_close(preferences); WAIT(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == NULL);
  GtkStack *main_stack = GTK_STACK(gtk_widget_get_template_child(GTK_WIDGET(window), TYPE_SIGNER_WINDOW, "main_stack"));
  GtkWidget *lock_screen = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(window), TYPE_SIGNER_WINDOW, "lock_screen"));
  gtk_stack_set_visible_child_name(main_stack, "locked"); wait_frames(GTK_WIDGET(window));
  GtkWidget *unlock = GTK_WIDGET(gtk_widget_get_template_child(lock_screen, GN_TYPE_LOCK_SCREEN, "btn_unlock"));
  g_assert_true(gtk_widget_grab_focus(unlock));
  gtk_window_set_default_size(GTK_WINDOW(window), 760, 640); wait_frames(GTK_WIDGET(window)); assert_visible_focus(window);
  gtk_window_set_default_size(GTK_WINDOW(window), 360, 640); wait_frames(GTK_WIDGET(window)); assert_visible_focus(window);
  gtk_stack_set_visible_child_name(main_stack, "unlocked"); gtk_window_set_default_size(GTK_WINDOW(window), 920, 640); wait_frames(GTK_WIDGET(window));
  gtk_window_set_default_size(GTK_WINDOW(window), 760, 640); wait_frames(GTK_WIDGET(window));
  adw_navigation_split_view_set_show_content(split, FALSE); wait_frames(GTK_WIDGET(window));
  g_assert_true(gtk_widget_grab_focus(GTK_WIDGET(gtk_list_box_get_row_at_index(sidebar, 0))));
  gtk_window_set_default_size(GTK_WINDOW(window), 360, 640); wait_frames(GTK_WIDGET(window)); assert_visible_focus(window);
  g_assert_true(adw_navigation_split_view_get_collapsed(split));
  g_signal_emit_by_name(sidebar, "row-activated", gtk_list_box_get_row_at_index(sidebar, 0)); wait_frames(GTK_WIDGET(window));
  g_assert_true(adw_navigation_split_view_get_show_content(split));
  signer_window_show_page(window, "identity");
  for (guint dark = 0; dark < 2; dark++) {
    /* The production shared panel and trusted Backup route, in both styles. */
    adw_style_manager_set_color_scheme(adw_style_manager_get_default(), dark ? ADW_COLOR_SCHEME_FORCE_DARK : ADW_COLOR_SCHEME_FORCE_LIGHT);
    signer_window_show_identity(window, NULL); wait_frames(GTK_WIDGET(window));
    GtkWidget *identity_panel = GTK_WIDGET(selected_panel(split));
    g_assert_true(GROTTO_IS_IDENTITY_PANEL(identity_panel));
    AdwHeaderBar *identity_header = ADW_HEADER_BAR(gtk_widget_get_template_child(identity_panel, GROTTO_TYPE_IDENTITY_PANEL, "header"));
    g_assert_true(adw_header_bar_get_show_start_title_buttons(identity_header));
    g_assert_true(adw_header_bar_get_show_end_title_buttons(identity_header));
    GtkWidget *identity_copy = GTK_WIDGET(gtk_widget_get_template_child(identity_panel, GROTTO_TYPE_IDENTITY_PANEL, "copy_key"));
    g_assert_cmpstr(gtk_widget_get_tooltip_text(identity_copy), ==, g_dgettext("grotto-panels", "Copy Public Key"));
    if (accessibility) {
      g_assert_cmpstr(gtk_widget_get_tooltip_text(identity_copy), ==, "Copiar clave pública");
      g_assert_cmpstr(adw_navigation_page_get_title(ADW_NAVIGATION_PAGE(identity_panel)), ==, "Identidad");
    }
    if (accessibility) gtk_test_accessible_assert_property(GTK_ACCESSIBLE(identity_copy), GTK_ACCESSIBLE_PROPERTY_LABEL, g_dgettext("grotto-panels", "Copy Public Key"));
    GtkWidget *identity_qr = GTK_WIDGET(gtk_widget_get_template_child(identity_panel, GROTTO_TYPE_IDENTITY_PANEL, "show_qr"));
    if (accessibility) gtk_test_accessible_assert_property(GTK_ACCESSIBLE(identity_qr), GTK_ACCESSIBLE_PROPERTY_LABEL, g_dgettext("grotto-panels", "Show QR Code"));
    g_assert_true(gtk_widget_grab_focus(identity_qr));
    g_signal_emit_by_name(identity_qr, "clicked");
    AdwDialog *public_qr = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)); g_assert_nonnull(public_qr);
    wait_frames(GTK_WIDGET(public_qr)); adw_dialog_close(public_qr); WAIT(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == NULL);
    wait_frames(GTK_WIDGET(window)); g_assert_true(gtk_root_get_focus(GTK_ROOT(window)) == identity_qr); assert_focus_bounds(window);
    g_assert_true(gtk_widget_grab_focus(identity_copy));
    gtk_window_set_default_size(GTK_WINDOW(window), 920, 640); wait_frames(GTK_WIDGET(window));
    g_assert_true(gtk_widget_child_focus(find_banner(identity_panel), GTK_DIR_TAB_FORWARD));
    GtkWidget *identity_preferences = gtk_widget_get_ancestor(identity_copy, ADW_TYPE_PREFERENCES_PAGE);
    GtkWidget *identity_scroll = find_editor_control(identity_preferences, GTK_TYPE_SCROLLED_WINDOW);
    g_assert_nonnull(identity_scroll);
    gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(identity_scroll)), 0);
    snapshot(GTK_WIDGET(window), dark ? "identity-dark" : "identity-light");
    gtk_window_set_default_size(GTK_WINDOW(window), 360, 640); wait_frames(GTK_WIDGET(window)); assert_visible_focus(window);
    snapshot(GTK_WIDGET(window), dark ? "identity-360-dark" : "identity-360-light");
    GtkWidget *backup_row = GTK_WIDGET(gtk_widget_get_template_child(identity_panel, GROTTO_TYPE_IDENTITY_PANEL, "backup_row"));
    WAIT(gtk_widget_get_visible(backup_row)); g_assert_cmpuint(seed_exports, ==, 0);
    g_assert_true(gtk_widget_grab_focus(backup_row));
#ifdef GROTTO_TEST_XDOTOOL
    send_key(window, "Return");
#else
    g_signal_emit_by_name(backup_row, "activated"); wait_frames(GTK_WIDGET(window));
#endif
    AdwNavigationView *panel_navigation = grotto_identity_panel_get_navigation(GROTTO_IDENTITY_PANEL(identity_panel));
    GtkWidget *backup = GTK_WIDGET(adw_navigation_view_get_visible_page(panel_navigation)); g_assert_true(GROTTO_IS_IDENTITY_DETAIL(backup));
    AdwHeaderBar *backup_header = ADW_HEADER_BAR(gtk_widget_get_template_child(backup, GROTTO_TYPE_IDENTITY_DETAIL, "header"));
    g_assert_false(adw_header_bar_get_show_start_title_buttons(backup_header)); g_assert_false(adw_header_bar_get_show_end_title_buttons(backup_header));
    GtkWidget *phrase_row = GTK_WIDGET(gtk_widget_get_template_child(backup, GROTTO_TYPE_IDENTITY_DETAIL, "phrase_row"));
    WAIT(gtk_widget_is_sensitive(phrase_row));
    g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(phrase_row)), ==, _("Show Recovery Phrase…"));
    g_assert_true(gtk_widget_grab_focus(phrase_row)); assert_visible_focus(window); g_assert_cmpuint(seed_exports, ==, 0);
    snapshot(GTK_WIDGET(window), dark ? "backup-360-dark" : "backup-360-light");
    gtk_window_set_default_size(GTK_WINDOW(window), 920, 640); snapshot(GTK_WIDGET(window), dark ? "backup-dark" : "backup-light");
#ifdef GROTTO_TEST_XDOTOOL
    send_key(window, "alt+Left");
#else
    g_assert_true(gtk_widget_activate_action(backup, "navigation.pop", NULL)); wait_frames(GTK_WIDGET(window));
#endif
    g_assert_false(GROTTO_IS_IDENTITY_DETAIL(adw_navigation_view_get_visible_page(panel_navigation)));
    g_assert_true(gtk_root_get_focus(GTK_ROOT(window)) == backup_row); assert_focus_bounds(window);
    gtk_window_set_default_size(GTK_WINDOW(window), 360, 640);
    gtk_widget_set_default_direction(GTK_TEXT_DIR_RTL);
    gtk_widget_set_direction(GTK_WIDGET(window), GTK_TEXT_DIR_RTL); wait_frames(GTK_WIDGET(window));
    g_assert_cmpint(gtk_widget_get_direction(identity_panel), ==, GTK_TEXT_DIR_RTL);
    g_assert_cmpint(gtk_widget_get_direction(backup_row), ==, GTK_TEXT_DIR_RTL);
    g_assert_true(gtk_widget_grab_focus(identity_copy)); wait_frames(GTK_WIDGET(window)); assert_focus_bounds(window);
#ifdef GROTTO_TEST_XDOTOOL
    send_key(window, "Tab"); assert_focus_bounds(window);
#endif
    snapshot(GTK_WIDGET(window), dark ? "identity-360-rtl-dark" : "identity-360-rtl-light");
    g_assert_true(gtk_widget_grab_focus(backup_row));
#ifdef GROTTO_TEST_XDOTOOL
    send_key(window, "Return");
#else
    g_signal_emit_by_name(backup_row, "activated"); wait_frames(GTK_WIDGET(window));
#endif
    backup = GTK_WIDGET(adw_navigation_view_get_visible_page(panel_navigation));
    g_assert_true(GROTTO_IS_IDENTITY_DETAIL(backup));
    g_assert_cmpint(gtk_widget_get_direction(backup), ==, GTK_TEXT_DIR_RTL);
    phrase_row = GTK_WIDGET(gtk_widget_get_template_child(backup, GROTTO_TYPE_IDENTITY_DETAIL, "phrase_row"));
    WAIT(gtk_widget_is_sensitive(phrase_row)); g_assert_true(gtk_widget_grab_focus(phrase_row)); assert_focus_bounds(window);
    snapshot(GTK_WIDGET(window), dark ? "backup-360-rtl-dark" : "backup-360-rtl-light");
#ifdef GROTTO_TEST_XDOTOOL
    send_key(window, "alt+Right");
#else
    g_assert_true(gtk_widget_activate_action(backup, "navigation.pop", NULL)); wait_frames(GTK_WIDGET(window));
#endif
    g_assert_true(gtk_root_get_focus(GTK_ROOT(window)) == backup_row); assert_focus_bounds(window);
    g_assert_cmpuint(seed_exports, ==, 0);
    gtk_widget_set_default_direction(GTK_TEXT_DIR_LTR);
    gtk_widget_set_direction(GTK_WIDGET(window), GTK_TEXT_DIR_LTR); wait_frames(GTK_WIDGET(window));
    signer_window_show_page(window, "apps"); wait_frames(GTK_WIDGET(window)); assert_visible_focus(window);
    GrottoAppsPanel *apps = GROTTO_APPS_PANEL(selected_panel(split));
    gtk_window_set_default_size(GTK_WINDOW(window), 920, 640); snapshot(GTK_WIDGET(window), dark ? "apps-dark" : "apps-light");
    gtk_window_set_default_size(GTK_WINDOW(window), 360, 640); snapshot(GTK_WIDGET(window), dark ? "apps-360-dark" : "apps-360-light");
    GtkWidget *app_row = NULL; WAIT((app_row = find_row(GTK_WIDGET(apps), "Jank")));
    g_assert_true(gtk_widget_grab_focus(app_row));
#ifdef GROTTO_TEST_XDOTOOL
    send_key(window, "Return");
#else
    g_signal_emit_by_name(app_row, "activated"); wait_frames(GTK_WIDGET(window));
#endif
    AdwNavigationView *nav = grotto_apps_panel_get_navigation(apps);
    GtkWidget *detail = GTK_WIDGET(adw_navigation_view_get_visible_page(nav)); g_assert_true(GROTTO_IS_PANEL(detail));
    g_object_weak_ref(G_OBJECT(detail), finalised, &pages_gone);
    g_assert_false(adw_banner_get_revealed(ADW_BANNER(find_banner(detail))));
    snapshot(GTK_WIDGET(window), dark ? "client-dark" : "client-light");
    GtkWidget *permission = find_row(detail, P_("Permissions")); g_assert_nonnull(permission);
    GtkWidget *low_row = find_row(detail, P_("Ask Unless Permitted")); g_assert_nonnull(low_row);
    GtkWidget *low = find_editor_control(low_row, GTK_TYPE_CHECK_BUTTON); g_assert_nonnull(low);
    /* Preset access is a host-supplied read-only description, never an editable
     * saved grant. Explicit rows retain their own expiry and edit route. */
    g_signal_emit_by_name(permission, "activated"); wait_frames(GTK_WIDGET(window));
    GtkWidget *effective = find_row(detail, _("Share public key (after first consent)")); g_assert_nonnull(effective);
    g_assert_false(gtk_list_box_row_get_activatable(GTK_LIST_BOX_ROW(effective)));
    g_autofree char *from_tier = g_strdup_printf(P_("From %s trust"), dark ? P_("Low") : P_("Medium"));
    g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(effective)), ==, from_tier);
    AdwNavigationView *permissions_nav = ADW_NAVIGATION_VIEW(adw_navigation_page_get_child(ADW_NAVIGATION_PAGE(detail)));
    g_assert_true(adw_navigation_view_pop(permissions_nav)); wait_frames(GTK_WIDGET(window));
    if (!dark) {
      WAIT(gtk_widget_is_sensitive(low)); gtk_check_button_set_active(GTK_CHECK_BUTTON(low), TRUE);
      WAIT(tier_writes == 1 && gtk_widget_is_sensitive(low));
    }
    g_signal_emit_by_name(permission, "activated"); snapshot(GTK_WIDGET(window), dark ? "permissions-dark" : "permissions-light");
    if (!dark) {
      GtkWidget *inactive = NULL; WAIT((inactive = find_permission(detail, npub, "policy-review")) && gtk_widget_is_sensitive(inactive) && gtk_list_box_row_get_activatable(GTK_LIST_BOX_ROW(inactive)));
      g_assert_nonnull(strstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(inactive)), P_("Inactive — pending policy review")));
      g_assert_nonnull(strstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(inactive)), P_("Until Revoked")));
      g_signal_emit_by_name(inactive, "activated");
      AdwDialog *cancelled = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)); g_assert_nonnull(cancelled);
      g_assert_nonnull(find_button(GTK_WIDGET(cancelled), P_("Activate Saved Permission")));
      g_assert_nonnull(find_button(GTK_WIDGET(cancelled), P_("Discard Saved Permission…")));
      wait_frames(GTK_WIDGET(cancelled)); adw_dialog_close(cancelled); WAIT(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == NULL);
      g_assert_cmpuint(permission_attempts, ==, 0); g_assert_true(contains_permission(npub, "policy-review"));
      GtkWidget *remembered = NULL; WAIT((remembered = find_permission(detail, npub, "allow")) && gtk_widget_is_sensitive(remembered) && gtk_list_box_row_get_activatable(GTK_LIST_BOX_ROW(remembered)));
      g_signal_emit_by_name(remembered, "activated");
      AdwDialog *editor = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)); g_assert_nonnull(editor);
      g_assert_nonnull(editor); g_object_weak_ref(G_OBJECT(editor), finalised, &editors_gone);
      wait_frames(GTK_WIDGET(editor));
      GtkWidget *decision = find_decision(GTK_WIDGET(editor)); g_assert_nonnull(decision); adw_combo_row_set_selected(ADW_COMBO_ROW(decision), 1);
      GtkWidget *save = find_button(GTK_WIDGET(editor), P_("Save Permission")); g_assert_nonnull(save);
      force_conflict = TRUE; g_signal_emit_by_name(save, "clicked");
      WAIT(permission_attempts == 1 && gtk_widget_get_sensitive(save) && gtk_widget_is_sensitive(low));
      wait_frames(GTK_WIDGET(editor));
      g_assert_true(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == editor);
      AdwBanner *editor_feedback = ADW_BANNER(find_banner(GTK_WIDGET(editor))); g_assert_nonnull(editor_feedback);
      g_assert_true(adw_banner_get_revealed(editor_feedback)); g_assert_true(gtk_widget_get_mapped(GTK_WIDGET(editor_feedback)));
      g_assert_cmpstr(adw_banner_get_title(editor_feedback), ==, P_("Permission not saved. Refreshing now; review your edit before retrying."));
      snapshot(GTK_WIDGET(window), "permission-conflict");
      g_assert_cmpuint(permission_writes, ==, 0); g_assert_true(contains_permission(npub, "allow"));
      g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(save)), ==, P_("Retry Permission"));
      g_signal_emit_by_name(save, "clicked"); WAIT(permission_writes == 1 && gtk_widget_is_sensitive(low));
      WAIT(editors_gone == 1 && adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == NULL);
      g_assert_false(contains_permission(npub, "allow")); g_assert_true(contains_permission(npub, "deny"));
      g_assert_cmpuint(g_variant_n_children(permission_rows), ==, 4); /* concurrent deny was preserved */
      GtkWidget *legacy = NULL; WAIT((legacy = find_permission(detail, "*", "review")) && gtk_widget_is_sensitive(legacy) && gtk_list_box_row_get_activatable(GTK_LIST_BOX_ROW(legacy)));
      g_signal_emit_by_name(legacy, "activated"); editor = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
      g_assert_nonnull(editor); g_object_weak_ref(G_OBJECT(editor), finalised, &editors_gone); wait_frames(GTK_WIDGET(editor));
      GtkWidget *identity_picker = find_identity_picker(GTK_WIDGET(editor)); g_assert_nonnull(identity_picker);
      g_assert_cmpuint(adw_combo_row_get_selected(ADW_COMBO_ROW(identity_picker)), ==, 0);
      snapshot(GTK_WIDGET(window), "permission-review-identity");
      g_assert_cmpuint(g_list_model_get_n_items(adw_combo_row_get_model(ADW_COMBO_ROW(identity_picker))), ==, 2); /* placeholder + signing identity, watch-only excluded */
      GtkWidget *kind_input = find_row(GTK_WIDGET(editor), P_("Exact Event Type (Number)")); g_assert_nonnull(kind_input);
      g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(kind_input)), ==, "");
      gtk_editable_set_text(GTK_EDITABLE(kind_input), "1");
      save = find_button(GTK_WIDGET(editor), P_("Activate Saved Permission")); g_signal_emit_by_name(save, "clicked");
      g_assert_cmpuint(permission_attempts, ==, 2); /* no wildcard allow may be submitted */
      adw_combo_row_set_selected(ADW_COMBO_ROW(identity_picker), 1); conflict_cleanup = TRUE;
      g_signal_emit_by_name(save, "clicked");
      WAIT(permission_writes == 2);
      WAIT(!conflict_cleanup);
      WAIT(gtk_widget_is_sensitive(save) && gtk_widget_is_sensitive(low));
      g_assert_true(contains_permission(npub, "allow")); g_assert_true(contains_permission("*", "review"));
      g_assert_true(contains_permission("*", "review")); /* failed cleanup must retain the editor and explicit retry */
      g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(save)), ==, P_("Retry Removing Legacy Entry"));
      g_signal_emit_by_name(save, "clicked"); WAIT(permission_writes == 3);
      WAIT(editors_gone == 2 && adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == NULL);
      g_assert_true(scoped_saved_before_cleanup); g_assert_false(contains_permission("*", "review"));
      g_assert_true(contains_permission(npub, "allow"));
      WAIT((legacy = find_permission(detail, npub, "policy-review")) && gtk_widget_is_sensitive(legacy) && gtk_list_box_row_get_activatable(GTK_LIST_BOX_ROW(legacy)));
      g_signal_emit_by_name(legacy, "activated"); editor = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
      g_assert_nonnull(editor); g_object_weak_ref(G_OBJECT(editor), finalised, &editors_gone); wait_frames(GTK_WIDGET(editor));
      g_assert_null(find_identity_picker(GTK_WIDGET(editor)));
      save = find_button(GTK_WIDGET(editor), P_("Activate Saved Permission")); g_signal_emit_by_name(save, "clicked");
      WAIT(permission_writes == 4);
      WAIT(editors_gone == 3 && adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == NULL);
      g_assert_false(contains_permission(npub, "policy-review"));
      for (gsize i = 0; i < g_variant_n_children(permission_rows); i++) {
        g_autoptr(GVariant) row = g_variant_get_child_value(permission_rows, i); gint kind = -1;
        g_variant_lookup(row, "event-kind", "i", &kind);
        if (kind == NOSTR_KIND_PROFILE_METADATA) { guint64 expiry = 99; g_assert_true(g_variant_lookup(row, "expires-at", "t", &expiry)); g_assert_cmpuint(expiry, ==, 0); }
      }
      /* A separate migrated wildcard row is clearable without creating a grant. */
      GVariantBuilder migration; g_variant_builder_init(&migration, G_VARIANT_TYPE("aa{sv}"));
      for (gsize i = 0; i < g_variant_n_children(permission_rows); i++) { g_autoptr(GVariant) row = g_variant_get_child_value(permission_rows, i); g_variant_builder_add_value(&migration, row); }
      fixture_permission(&migration, "*", -1, "review");
      g_clear_pointer(&permission_rows, g_variant_unref); permission_rows = g_variant_ref_sink(g_variant_builder_end(&migration)); revision++;
      g_dbus_connection_emit_signal(server, NULL, "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "ClientsChanged", g_variant_new("(t)", revision), NULL);
      WAIT((legacy = find_permission(detail, "*", "review")) && gtk_widget_is_sensitive(legacy) && gtk_list_box_row_get_activatable(GTK_LIST_BOX_ROW(legacy)));
      g_signal_emit_by_name(legacy, "activated"); editor = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
      g_assert_nonnull(editor); g_object_weak_ref(G_OBJECT(editor), finalised, &editors_gone); wait_frames(GTK_WIDGET(editor));
      g_signal_emit_by_name(find_button(GTK_WIDGET(editor), P_("Discard Saved Permission…")), "clicked");
      AdwAlertDialog *confirm = ADW_ALERT_DIALOG(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
      g_assert_cmpstr(adw_alert_dialog_get_default_response(confirm), ==, "cancel");
      wait_frames(GTK_WIDGET(confirm));
      g_signal_emit_by_name(find_button(GTK_WIDGET(confirm), P_("Discard")), "clicked"); WAIT(permission_writes == 5);
      WAIT(editors_gone == 4 && adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == NULL);
      g_assert_false(contains_permission("*", "review"));
      /* Duration edits must preserve independent AUTH grants for two relays. */
      GVariantBuilder auth; g_variant_builder_init(&auth, G_VARIANT_TYPE("aa{sv}"));
      for (gsize i = 0; i < g_variant_n_children(permission_rows); i++) {
        g_autoptr(GVariant) row = g_variant_get_child_value(permission_rows, i); g_variant_builder_add_value(&auth, row);
      }
      const gchar *relays[] = {"wss://nos.lol/", "wss://relay.nostr.band/"};
      for (guint i = 0; i < G_N_ELEMENTS(relays); i++) {
        GVariantBuilder one; g_variant_builder_init(&one, G_VARIANT_TYPE("aa{sv}"));
        fixture_permission(&one, npub, NOSTR_KIND_CLIENT_AUTHENTICATION, "allow");
        g_autoptr(GVariant) array = g_variant_ref_sink(g_variant_builder_end(&one));
        g_autoptr(GVariant) row = g_variant_get_child_value(array, 0);
        GVariantDict d; g_variant_dict_init(&d, row); g_variant_dict_insert(&d, "relay", "s", relays[i]);
        g_variant_builder_add_value(&auth, g_variant_dict_end(&d));
      }
      g_clear_pointer(&permission_rows, g_variant_unref); permission_rows = g_variant_ref_sink(g_variant_builder_end(&auth)); revision++;
      g_dbus_connection_emit_signal(server, NULL, "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "ClientsChanged", g_variant_new("(t)", revision), NULL);
      GtkWidget *auth_row = NULL; WAIT((auth_row = find_relay_permission(detail, relays[0])) && gtk_widget_is_sensitive(auth_row) && gtk_list_box_row_get_activatable(GTK_LIST_BOX_ROW(auth_row)));
      g_signal_emit_by_name(auth_row, "activated"); editor = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
      g_assert_nonnull(editor); g_object_weak_ref(G_OBJECT(editor), finalised, &editors_gone); wait_frames(GTK_WIDGET(editor));
      GtkWidget *expiry_picker = find_expiry(GTK_WIDGET(editor)); g_assert_nonnull(expiry_picker);
      adw_combo_row_set_selected(ADW_COMBO_ROW(expiry_picker), 3);
      g_signal_emit_by_name(find_button(GTK_WIDGET(editor), P_("Save Permission")), "clicked");
      WAIT(permission_writes == 6 && editors_gone == 5);
      g_assert_nonnull(find_relay_permission(detail, relays[1]));
      gboolean edited_auth = FALSE;
      for (gsize i = 0; i < g_variant_n_children(permission_rows); i++) {
        g_autoptr(GVariant) row = g_variant_get_child_value(permission_rows, i);
        if (strcmp(row_text(row, "relay"), relays[0])) continue;
        guint64 expiry; g_assert_true(g_variant_lookup(row, "expires-at", "t", &expiry));
        g_assert_cmpint(llabs((gint64)expiry - g_get_real_time() / G_USEC_PER_SEC - 86400), <=, 3); edited_auth = TRUE;
      }
      g_assert_true(edited_auth);
      /* Reload failure is visible and retryable; it never erases saved permissions. */
      AdwNavigationView *detail_nav = ADW_NAVIGATION_VIEW(adw_navigation_page_get_child(ADW_NAVIGATION_PAGE(detail)));
      GtkWidget *permissions_page = GTK_WIDGET(adw_navigation_view_get_visible_page(detail_nav));
      AdwBanner *feedback = ADW_BANNER(find_banner(permissions_page)); g_assert_nonnull(feedback);
      fail_permission_read = TRUE; g_signal_emit_by_name(feedback, "button-clicked");
      WAIT(adw_banner_get_revealed(feedback));
      g_assert_cmpstr(adw_banner_get_title(feedback), ==, P_("Permissions could not be loaded. Refresh to try again."));
      fail_permission_read = FALSE; g_signal_emit_by_name(feedback, "button-clicked");
      WAIT(!adw_banner_get_revealed(feedback) && gtk_widget_is_sensitive(low));
      /* Policy-review Discard is a distinct, confirmed removal, not activation. */
      GVariantBuilder extra; g_variant_builder_init(&extra, G_VARIANT_TYPE("aa{sv}"));
      for (gsize i = 0; i < g_variant_n_children(permission_rows); i++) { g_autoptr(GVariant) one = g_variant_get_child_value(permission_rows, i); g_variant_builder_add_value(&extra, one); }
      fixture_permission(&extra, npub, NOSTR_KIND_REPOST, "policy-review");
      fixture_permission(&extra, watch_npub, NOSTR_KIND_TEXT_NOTE, "deny");
      g_clear_pointer(&permission_rows, g_variant_unref); permission_rows = g_variant_ref_sink(g_variant_builder_end(&extra)); revision++;
      g_dbus_connection_emit_signal(server, NULL, "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "ClientsChanged", g_variant_new("(t)", revision), NULL);
      WAIT((inactive = find_permission(detail, npub, "policy-review")) && gtk_widget_is_sensitive(inactive) && gtk_list_box_row_get_activatable(GTK_LIST_BOX_ROW(inactive)));
      guint before = permission_attempts;
      g_signal_emit_by_name(inactive, "activated"); editor = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
      g_assert_nonnull(editor); wait_frames(GTK_WIDGET(editor));
      g_signal_emit_by_name(find_button(GTK_WIDGET(editor), P_("Discard Saved Permission…")), "clicked");
      confirm = ADW_ALERT_DIALOG(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
      g_assert_cmpstr(adw_alert_dialog_get_default_response(confirm), ==, "cancel");
      wait_frames(GTK_WIDGET(confirm)); adw_dialog_close(ADW_DIALOG(confirm)); WAIT(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == editor);
      g_assert_cmpuint(permission_attempts, ==, before); g_assert_true(contains_permission(npub, "policy-review"));
      g_signal_emit_by_name(find_button(GTK_WIDGET(editor), P_("Discard Saved Permission…")), "clicked");
      confirm = ADW_ALERT_DIALOG(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window))); wait_frames(GTK_WIDGET(confirm));
      g_signal_emit_by_name(find_button(GTK_WIDGET(confirm), P_("Discard")), "clicked");
      WAIT(permission_writes == 7 && adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == NULL);
      g_assert_false(contains_permission(npub, "policy-review")); g_assert_true(contains_permission(npub, "allow")); g_assert_true(contains_permission(npub, "deny"));
      /* Clear is scoped, revision-pinned, default-cancel and never upgrades trust. */
      GtkWidget *clear = NULL; WAIT((clear = find_clear_scope(detail, npub)) && gtk_widget_is_sensitive(clear));
      g_signal_emit_by_name(clear, "activated"); confirm = ADW_ALERT_DIALOG(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
      g_assert_cmpstr(adw_alert_dialog_get_default_response(confirm), ==, "cancel");
      g_assert_nonnull(strstr(adw_alert_dialog_get_body(confirm), npub));
      before = permission_attempts; wait_frames(GTK_WIDGET(confirm)); adw_dialog_close(ADW_DIALOG(confirm)); WAIT(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == NULL);
      g_assert_cmpuint(permission_attempts, ==, before);
      g_signal_emit_by_name(clear, "activated"); confirm = ADW_ALERT_DIALOG(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window))); wait_frames(GTK_WIDGET(confirm));
      force_conflict = TRUE; g_signal_emit_by_name(find_button(GTK_WIDGET(confirm), P_("Clear Permissions")), "clicked");
      WAIT(permission_attempts == before + 1 && adw_banner_get_revealed(feedback) && (clear = find_clear_scope(detail, npub)) && gtk_widget_is_sensitive(clear));
      g_assert_cmpstr(adw_banner_get_title(feedback), ==, P_("Not cleared. Permissions or the signer changed. Review the refreshed entries and confirm again."));
      g_assert_cmpuint(permission_writes, ==, 7); g_assert_true(contains_permission(npub, "deny")); g_assert_true(contains_permission(npub, "allow"));
      g_signal_emit_by_name(clear, "activated"); confirm = ADW_ALERT_DIALOG(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window))); wait_frames(GTK_WIDGET(confirm));
      g_signal_emit_by_name(find_button(GTK_WIDGET(confirm), P_("Clear Permissions")), "clicked");
      WAIT(permission_writes == 8 && !find_clear_scope(detail, npub));
      g_assert_false(contains_permission(npub, "allow")); g_assert_false(contains_permission(npub, "deny"));
      g_assert_true(contains_permission(watch_npub, "deny")); g_assert_cmpstr(tier, ==, "low"); g_assert_cmpuint(tier_writes, ==, 1);

    }
    /* Exercise the nested app/Permissions route with actual RTL keys at 360px. */
    g_assert_true(adw_navigation_view_pop(permissions_nav)); wait_frames(GTK_WIDGET(window));
    gtk_widget_set_default_direction(GTK_TEXT_DIR_RTL); gtk_widget_set_direction(GTK_WIDGET(window), GTK_TEXT_DIR_RTL);
    g_assert_true(gtk_widget_grab_focus(permission)); wait_frames(GTK_WIDGET(window)); assert_focus_bounds(window);
#ifdef GROTTO_TEST_XDOTOOL
    send_key(window, "Return");
#else
    g_signal_emit_by_name(permission, "activated");
#endif
    WAIT(find_permission(detail, watch_npub, "deny") != NULL); wait_frames(GTK_WIDGET(window));
    g_assert_cmpstr(adw_navigation_page_get_title(adw_navigation_view_get_visible_page(permissions_nav)), ==, P_("Permissions"));
    g_assert_cmpint(gtk_widget_get_direction(detail), ==, GTK_TEXT_DIR_RTL);
#ifdef GROTTO_TEST_XDOTOOL
    send_key(window, "Tab"); assert_visible_focus(window); assert_focus_bounds(window);
#endif
    /* A toast close button can legitimately receive Tab focus and pause its
     * timeout. Return focus to the permission being inspected before capture. */
    GtkWidget *rtl_permission = find_permission(detail, watch_npub, "deny");
    g_assert_true(gtk_widget_grab_focus(rtl_permission)); wait_frames(GTK_WIDGET(window)); assert_focus_bounds(window);
    snapshot(GTK_WIDGET(window), dark ? "permissions-360-rtl-dark" : "permissions-360-rtl-light");
#ifdef GROTTO_TEST_XDOTOOL
    send_key(window, "alt+Right");
#else
    g_assert_true(adw_navigation_view_pop(permissions_nav));
#endif
    g_assert_true(gtk_root_get_focus(GTK_ROOT(window)) == permission); assert_focus_bounds(window);
#ifdef GROTTO_TEST_XDOTOOL
    send_key(window, "alt+Right");
#else
    g_assert_true(adw_navigation_view_pop(nav));
#endif
    app_row = find_row(GTK_WIDGET(apps), "Jank"); g_assert_nonnull(app_row);
    g_assert_true(gtk_root_get_focus(GTK_ROOT(window)) == app_row); assert_focus_bounds(window);
    snapshot(GTK_WIDGET(window), dark ? "apps-360-rtl-dark" : "apps-360-rtl-light");
    gtk_widget_set_default_direction(GTK_TEXT_DIR_LTR); gtk_widget_set_direction(GTK_WIDGET(window), GTK_TEXT_DIR_LTR); wait_frames(GTK_WIDGET(window));
    for (guint i = 2; i < G_N_ELEMENTS(names); i++) {
      signer_window_show_page(window, names[i]); wait_frames(GTK_WIDGET(window)); assert_visible_focus(window);
      g_autofree char *narrow = g_strdup_printf("%s-360-%s", names[i], dark ? "dark" : "light");
      snapshot(GTK_WIDGET(window), narrow);
      gtk_widget_set_default_direction(GTK_TEXT_DIR_RTL); gtk_widget_set_direction(GTK_WIDGET(window), GTK_TEXT_DIR_RTL); wait_frames(GTK_WIDGET(window));
      GtkWidget *rtl_panel = selected_panel(split);
      g_assert_cmpint(gtk_widget_get_direction(rtl_panel), ==, GTK_TEXT_DIR_RTL);
      GtkWidget *input = GTK_WIDGET(gtk_widget_get_template_child(rtl_panel, i == 2 ? GROTTO_TYPE_ACTIVITY_PANEL : GROTTO_TYPE_REMOTE_PANEL, i == 2 ? "search" : "new_relay"));
      g_assert_nonnull(input); g_assert_true(gtk_widget_grab_focus(input));
      gtk_editable_set_text(GTK_EDITABLE(input), "اختبار طويل لاتجاه النص — keyboard input"); wait_frames(GTK_WIDGET(window)); assert_focus_bounds(window);
#ifdef GROTTO_TEST_XDOTOOL
      send_key(window, "ctrl+a"); send_key(window, "BackSpace");
      g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(input)), ==, "");
#else
      gtk_editable_set_text(GTK_EDITABLE(input), "");
#endif
#ifdef GROTTO_TEST_XDOTOOL
      send_key(window, "Tab"); assert_visible_focus(window); assert_focus_bounds(window);
      send_key(window, "shift+Tab"); assert_visible_focus(window); assert_focus_bounds(window);
#endif
      g_autofree char *rtl_name = g_strdup_printf("%s-360-rtl-%s", names[i], dark ? "dark" : "light"); snapshot(GTK_WIDGET(window), rtl_name);
      gtk_widget_set_default_direction(GTK_TEXT_DIR_LTR); gtk_widget_set_direction(GTK_WIDGET(window), GTK_TEXT_DIR_LTR);
      gtk_window_set_default_size(GTK_WINDOW(window), 920, 640);
      g_autofree char *wide = g_strdup_printf("%s-%s", names[i], dark ? "dark" : "light"); snapshot(GTK_WIDGET(window), wide);
      gtk_window_set_default_size(GTK_WINDOW(window), 360, 640);
    }
    signer_window_show_page(window, "settings"); wait_frames(GTK_WIDGET(window)); assert_visible_focus(window);
    AdwDialog *preferences = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
    GtkWidget *images = find_row(GTK_WIDGET(preferences), _("Load Profile Images"));
    g_assert_true(ADW_IS_SWITCH_ROW(images));
    g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(images)), ==, _("Fetch avatars and banners from the web addresses in profiles"));
    g_assert_true(adw_switch_row_get_active(ADW_SWITCH_ROW(images)));
    adw_switch_row_set_active(ADW_SWITCH_ROW(images), FALSE);
    g_assert_false(g_settings_get_boolean(settings_manager_get_gsettings(settings_manager_get_default()), "load-profile-images"));
    g_settings_set_boolean(settings_manager_get_gsettings(settings_manager_get_default()), "load-profile-images", TRUE);
    g_assert_true(adw_switch_row_get_active(ADW_SWITCH_ROW(images)));
    GtkWidget *bootstrap = find_row(GTK_WIDGET(preferences), _("Find Profiles Automatically")); g_assert_true(ADW_IS_SWITCH_ROW(bootstrap));
    adw_switch_row_set_active(ADW_SWITCH_ROW(bootstrap), FALSE);
    g_assert_false(g_settings_get_boolean(settings_manager_get_gsettings(settings_manager_get_default()), "profile-bootstrap-enabled"));
    adw_switch_row_set_active(ADW_SWITCH_ROW(bootstrap), TRUE);
    AdwExpanderRow *discovery = ADW_EXPANDER_ROW(find_row(GTK_WIDGET(preferences), _("Discovery-Only Relays")));
    g_assert_nonnull(discovery); adw_expander_row_set_expanded(discovery, TRUE);
    wait_frames(GTK_WIDGET(preferences));
    GtkTextView *relay_editor = find_text_view(GTK_WIDGET(discovery)); g_assert_nonnull(relay_editor);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(relay_editor), "wss://discovery.example/\nwss://second.example/", -1);
    g_signal_emit_by_name(find_button(GTK_WIDGET(preferences), _("Save Discovery Relays")), "clicked");
    GSettings *profile_settings = settings_manager_get_gsettings(settings_manager_get_default());
    g_auto(GStrv) saved_relays = g_settings_get_strv(profile_settings, "profile-bootstrap-relays");
    g_assert_cmpuint(g_strv_length(saved_relays), ==, 2); g_assert_cmpstr(saved_relays[0], ==, "wss://discovery.example/");
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(relay_editor), "https://not-a-relay.example/", -1);
    g_signal_emit_by_name(find_button(GTK_WIDGET(preferences), _("Save Discovery Relays")), "clicked");
    g_auto(GStrv) unchanged_relays = g_settings_get_strv(profile_settings, "profile-bootstrap-relays");
    g_assert_true(g_strv_equal((const char *const *)saved_relays, (const char *const *)unchanged_relays));
    g_signal_emit_by_name(find_button(GTK_WIDGET(preferences), _("Restore Defaults")), "clicked");
    g_autoptr(GVariant) defaults = g_settings_get_default_value(profile_settings, "profile-bootstrap-relays");
    g_autoptr(GVariant) restored = g_settings_get_value(profile_settings, "profile-bootstrap-relays"); g_assert_true(g_variant_equal(defaults, restored));
    snapshot(GTK_WIDGET(window), dark ? "settings-details-dark" : "settings-details-light");
    adw_expander_row_set_expanded(discovery, FALSE);
    snapshot(GTK_WIDGET(window), dark ? "settings-dark" : "settings-light");
    adw_dialog_close(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
    WAIT(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == NULL);
    signer_window_show_page(window, "identity"); wait_frames(GTK_WIDGET(window)); assert_visible_focus(window);
  }
  /* Test the production metadata writer after captures, without a transient
   * success toast covering the screenshots. */
  signer_window_show_identity(window, NULL); wait_frames(GTK_WIDGET(window));
  GtkWidget *identity_panel = GTK_WIDGET(selected_panel(split));
  g_assert_true(grotto_panel_has_profile_host(GROTTO_PANEL(identity_panel)));
  GtkWidget *profile_box = GTK_WIDGET(gtk_widget_get_template_child(identity_panel, GROTTO_TYPE_IDENTITY_PANEL, "profile_box"));
  g_assert_nonnull(profile_box); g_assert_true(gtk_widget_get_visible(profile_box));
  GtkWidget *edit_profile = find_button(profile_box, P_("Edit Profile…")); g_assert_nonnull(edit_profile);
  WAIT(gtk_widget_get_sensitive(edit_profile)); g_signal_emit_by_name(edit_profile, "clicked");
  AdwDialog *profile_dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_nonnull(profile_dialog); g_assert_cmpstr(adw_dialog_get_title(profile_dialog), ==, P_("Edit Profile"));
  g_assert_nonnull(find_row(GTK_WIDGET(profile_dialog), P_("Avatar URL")));
  g_assert_nonnull(find_row(GTK_WIDGET(profile_dialog), P_("Banner URL")));
  g_assert_nonnull(find_row(GTK_WIDGET(profile_dialog), P_("Automated Account (Bot)")));
  g_assert_nonnull(find_row(GTK_WIDGET(profile_dialog), P_("Profile Preview")));
  g_assert_nonnull(find_button(GTK_WIDGET(profile_dialog), P_("Discard Local Changes")));
  g_assert_nonnull(find_button(GTK_WIDGET(profile_dialog), P_("Republish")));
  gtk_window_set_default_size(GTK_WINDOW(window), 360, 640); wait_frames(GTK_WIDGET(window));
  snapshot(GTK_WIDGET(window), "profile-editor-360"); assert_focus_bounds(window);
  AdwExpanderRow *profile_preview = ADW_EXPANDER_ROW(find_row(GTK_WIDGET(profile_dialog), P_("Profile Preview")));
  adw_expander_row_set_expanded(profile_preview, TRUE); wait_frames(GTK_WIDGET(window)); snapshot(GTK_WIDGET(window), "profile-preview-360");
  adw_expander_row_set_expanded(profile_preview, FALSE);
  gtk_window_set_default_size(GTK_WINDOW(window), 920, 640); wait_frames(GTK_WIDGET(window));
  snapshot(GTK_WIDGET(window), "profile-editor-wide"); assert_focus_bounds(window);
  adw_dialog_close(profile_dialog); WAIT(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  GtkWidget *name_row = GTK_WIDGET(gtk_widget_get_template_child(identity_panel, GROTTO_TYPE_IDENTITY_PANEL, "name_row"));
  WAIT(gtk_widget_is_sensitive(name_row));
  gtk_editable_set_text(GTK_EDITABLE(name_row), "Everyday identity"); g_signal_emit_by_name(name_row, "apply");
  WAIT(gtk_widget_is_sensitive(name_row));
  AccountEntry *renamed = accounts_store_find(store, npub); g_assert_cmpstr(renamed->label, ==, "Everyday identity"); accounts_store_entry_free(renamed);
  g_assert_true(accounts_store_set_label(store, npub, "nsec1contaminated-metadata-fixture", NULL));
  GListModel *public_identities = grotto_panels_service_get_model(grotto_panel_get_service(GROTTO_PANEL(identity_panel)), GROTTO_PANELS_IDENTITIES);
  gboolean scrubbed = FALSE;
  for (guint i = 0; i < g_list_model_get_n_items(public_identities); i++) {
    g_autoptr(GrottoPanelsItem) item = g_list_model_get_item(public_identities, i);
    const GrottoPanelsMetadata *metadata = grotto_panels_item_get_metadata(item);
    if (strcmp(metadata->public_key, npub)) continue;
    g_assert_cmpstr(metadata->label, ==, ""); scrubbed = TRUE;
  }
  g_assert_true(scrubbed); g_assert_true(accounts_store_set_label(store, npub, "Everyday identity", NULL));
  g_assert_true(accounts_store_set_setup_state(store, npub, "backup-pending", NULL));
  signer_window_show_page(window, "backup"); wait_frames(GTK_WIDGET(window));
  identity_panel = GTK_WIDGET(selected_panel(split));
  g_assert_true(GROTTO_IS_IDENTITY_PANEL(identity_panel));
  g_assert_true(GROTTO_IS_IDENTITY_DETAIL(adw_navigation_view_get_visible_page(grotto_identity_panel_get_navigation(GROTTO_IDENTITY_PANEL(identity_panel)))));
  g_assert_cmpuint(seed_exports, ==, 0); /* backup route still never reveals */
  gtk_window_set_default_size(GTK_WINDOW(window), 360, 640);
  snapshot(GTK_WIDGET(window), "backup-360");
  AdwNavigationPage *backup_page = adw_navigation_view_get_visible_page(grotto_identity_panel_get_navigation(GROTTO_IDENTITY_PANEL(identity_panel)));
  GtkWidget *raw = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(backup_page), GROTTO_TYPE_IDENTITY_DETAIL, "raw_row"));
  g_assert_false(gtk_widget_get_mapped(raw));
  AdwExpanderRow *advanced = ADW_EXPANDER_ROW(find_row(GTK_WIDGET(backup_page), _("Advanced")));
  adw_expander_row_set_expanded(advanced, TRUE); wait_frames(GTK_WIDGET(window)); g_assert_true(gtk_widget_get_mapped(raw));
  snapshot(GTK_WIDGET(window), "backup-details-360"); adw_expander_row_set_expanded(advanced, FALSE);
  SheetQrDisplay *qr = sheet_qr_display_new(); sheet_qr_display_set_npub(qr, npub);
  adw_dialog_present(ADW_DIALOG(qr), GTK_WIDGET(window)); snapshot(GTK_WIDGET(window), "public-key-qr-360");
  adw_dialog_close(ADW_DIALOG(qr)); WAIT(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  adw_dialog_present(ADW_DIALOG(sheet_create_bunker_new()), GTK_WIDGET(window)); snapshot(GTK_WIDGET(window), "pairing-create-360");
  adw_dialog_close(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  WAIT(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  SheetQrScanner *scanner = sheet_qr_scanner_new();
  const GnQrContentType pairing_types[] = {GN_QR_TYPE_BUNKER_URI, GN_QR_TYPE_UNKNOWN};
  sheet_qr_scanner_set_accepted_types(scanner, pairing_types); adw_dialog_present(ADW_DIALOG(scanner), GTK_WIDGET(window));
  GtkWidget *hint = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(scanner), TYPE_SHEET_QR_SCANNER, "hint_label"));
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(hint)), ==, _("Scan the connection code shown by your app"));
  snapshot(GTK_WIDGET(window), "pairing-scan-360");
  GtkWidget *scanner_page = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(scanner), TYPE_SHEET_QR_SCANNER, "scanner_page"));
  GnQrScanResult scan_result = {.data = "nostrconnect://79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798?relay=wss%3A%2F%2Frelay.example&secret=fixture&name=Example%20App", .type = GN_QR_TYPE_BUNKER_URI};
  g_signal_emit_by_name(gtk_widget_get_first_child(scanner_page), "qr-detected", &scan_result);
  GtkWidget *scanner_stack = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(scanner), TYPE_SHEET_QR_SCANNER, "stack"));
  g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(scanner_stack)), ==, "result");
  GtkWidget *import_button = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(scanner), TYPE_SHEET_QR_SCANNER, "btn_import"));
  g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(import_button)), ==, _("Choose _Identity…"));
  snapshot(GTK_WIDGET(window), "pairing-scan-result-360");
  GtkWidget *scan_back = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(scanner), TYPE_SHEET_QR_SCANNER, "btn_back"));
  g_signal_emit_by_name(scan_back, "clicked"); g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(scanner_stack)), ==, "scanner");
  gtk_widget_set_direction(GTK_WIDGET(window), GTK_TEXT_DIR_RTL); snapshot(GTK_WIDGET(window), "pairing-scan-360-rtl");
  gtk_widget_set_direction(GTK_WIDGET(window), GTK_TEXT_DIR_LTR); adw_dialog_close(ADW_DIALOG(scanner));
  WAIT(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  grotto_pairing_present(GTK_WIDGET(window), "nostrconnect://79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798?relay=wss%3A%2F%2Frelay.example&secret=fixture&name=Example%20App");
  snapshot(GTK_WIDGET(window), "pairing-preview-360");
  adw_dialog_close(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  WAIT(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  pending_request = TRUE;
  grotto_approval_controller_reconcile(controller);
  GListModel *pending = grotto_approval_controller_get_pending(controller);
  WAIT(g_list_model_get_n_items(pending) == 1);
  GtkWidget *apps_sidebar = GTK_WIDGET(gtk_list_box_get_row_at_index(sidebar, 1));
  g_autofree char *pending_name = g_strdup_printf(ngettext("Connected Apps, %u pending request", "Connected Apps, %u pending requests", 1), 1u);
  if (accessibility) gtk_test_accessible_assert_property(GTK_ACCESSIBLE(apps_sidebar), GTK_ACCESSIBLE_PROPERTY_LABEL, pending_name);
  signer_window_show_page(window, "apps"); wait_frames(GTK_WIDGET(window));
  for (guint dark = 0; dark < 2; dark++) {
    adw_style_manager_set_color_scheme(adw_style_manager_get_default(), dark ? ADW_COLOR_SCHEME_FORCE_DARK : ADW_COLOR_SCHEME_FORCE_LIGHT);
    gtk_window_set_default_size(GTK_WINDOW(window), 920, 640); snapshot(GTK_WIDGET(window), dark ? "inbox-dark" : "inbox-light");
    gtk_window_set_default_size(GTK_WINDOW(window), 360, 640); snapshot(GTK_WIDGET(window), dark ? "inbox-360-dark" : "inbox-360-light");
  }
  SignerWindow *pending_launch = signer_window_new(app); g_object_ref(pending_launch);
  signer_window_set_pending_group(pending_launch, grotto_approval_pending_group_new(controller));
  AdwNavigationSplitView *launch_split = ADW_NAVIGATION_SPLIT_VIEW(gtk_widget_get_template_child(GTK_WIDGET(pending_launch), TYPE_SIGNER_WINDOW, "split_view"));
  g_assert_true(GROTTO_IS_APPS_PANEL(selected_panel(launch_split)));
  gtk_window_destroy(GTK_WINDOW(pending_launch)); g_object_unref(pending_launch);
  pending_request = FALSE; grotto_approval_controller_reconcile(controller); WAIT(g_list_model_get_n_items(pending) == 0);
  if (accessibility) gtk_test_accessible_assert_property(GTK_ACCESSIBLE(apps_sidebar), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Connected Apps"));
  grotto_approval_controller_stop(controller);
  GtkBuilder *shortcuts = gtk_builder_new_from_resource("/org/gnostr/signer/ui/shortcuts-window.ui");
  g_assert_true(GTK_IS_SHORTCUTS_WINDOW(gtk_builder_get_object(shortcuts, "shortcuts_window"))); gtk_window_destroy(GTK_WINDOW(gtk_builder_get_object(shortcuts, "shortcuts_window"))); g_object_unref(shortcuts);
  gtk_window_destroy(GTK_WINDOW(window)); g_object_unref(window);
  WAIT(windows_gone == 1);
  WAIT(pages_gone == 6);
  WAIT(editors_gone == 5);
  g_application_quit(application);
}
static GtkWidget *setup_child(OnboardingAssistant *self, const char *name) {
  return GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(self), TYPE_ONBOARDING_ASSISTANT, name));
}
static void setup_snapshot(OnboardingAssistant *setup, const char *stage, gboolean rtl) {
  int minimum; gtk_widget_measure(GTK_WIDGET(setup), GTK_ORIENTATION_HORIZONTAL, -1, &minimum, NULL, NULL, NULL);
  g_assert_cmpint(minimum, <=, 360);
  g_autofree char *name = g_strdup_printf("onboarding-%s-360%s", stage, rtl ? "-rtl" : "");
  snapshot(GTK_WIDGET(setup), name);
}
/* Exercise production navigation against the private-bus fixture. The recovery
 * phrase is the published test vector, never an operator's recovery material. */
static void remaining_surfaces(void) {
  adw_style_manager_set_color_scheme(adw_style_manager_get_default(), ADW_COLOR_SCHEME_FORCE_DARK);
  for (guint rtl = 0; rtl < 2; rtl++) {
    gtk_widget_set_default_direction(rtl ? GTK_TEXT_DIR_RTL : GTK_TEXT_DIR_LTR);
    OnboardingAssistant *setup = onboarding_assistant_new();
    g_object_ref_sink(setup);
    gtk_window_set_default_size(GTK_WINDOW(setup), 360, 640);
    gtk_window_present(GTK_WINDOW(setup));
    WAIT(gtk_widget_is_sensitive(setup_child(setup, "btn_next")));
    setup_snapshot(setup, "welcome", rtl);
    g_signal_emit_by_name(setup_child(setup, "btn_next"), "clicked"); setup_snapshot(setup, "choice", rtl);
    g_assert_false(gtk_widget_get_visible(setup_child(setup, "btn_next")));
    g_signal_emit_by_name(setup_child(setup, "choose_connect"), "activated"); setup_snapshot(setup, "connect", rtl);
    g_signal_emit_by_name(setup_child(setup, "connect_import"), "activated");
    g_assert_cmpint(onboarding_assistant_get_state(setup), ==, ONBOARDING_IMPORT);
    gtk_editable_set_text(GTK_EDITABLE(setup_child(setup, "text_import_data")), "ncryptsec1fixture");
    g_assert_true(gtk_widget_get_visible(setup_child(setup, "import_passphrase_group")));
    if (!g_strcmp0(g_getenv("GTK_A11Y"), "test")) {
      GtkWidget *secure = gtk_widget_get_first_child(setup_child(setup, "box_import_passphrase_container"));
      GtkWidget *entry = GTK_WIDGET(gtk_widget_get_template_child(secure, GN_TYPE_SECURE_ENTRY, "entry_password"));
      gtk_test_accessible_assert_property(GTK_ACCESSIBLE(entry), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Backup passphrase"));
    }
    setup_snapshot(setup, "import", rtl);
    g_signal_emit_by_name(setup_child(setup, "btn_back"), "clicked");
    g_signal_emit_by_name(setup_child(setup, "choose_create"), "activated"); setup_snapshot(setup, "create", rtl);
    g_signal_emit_by_name(setup_child(setup, "btn_next"), "clicked"); WAIT(layout_self_test != NULL);
    g_assert_cmpint(onboarding_assistant_get_state(setup), ==, ONBOARDING_TEST_WAITING); setup_snapshot(setup, "test", rtl);
    g_dbus_method_invocation_return_value(layout_self_test, g_variant_new("(s)", "passed")); g_clear_object(&layout_self_test);
    WAIT(onboarding_assistant_get_state(setup) == ONBOARDING_BACKUP);
    WAIT(gtk_widget_get_first_child(setup_child(setup, "seed_words")) != NULL);
    setup_snapshot(setup, "backup", rtl);
    GtkCheckButton *acknowledgement = GTK_CHECK_BUTTON(setup_child(setup, "seed_written_down"));
    GtkWidget *acknowledgement_label = gtk_check_button_get_child(acknowledgement);
    g_assert_true(GTK_IS_LABEL(acknowledgement_label));
    const char *acknowledgement_text = gtk_label_get_text(GTK_LABEL(acknowledgement_label));
    g_assert_nonnull(acknowledgement_text); g_assert_cmpstr(acknowledgement_text, !=, "");
    if (!g_strcmp0(g_getenv("GTK_A11Y"), "test"))
      gtk_test_accessible_assert_property(GTK_ACCESSIBLE(acknowledgement), GTK_ACCESSIBLE_PROPERTY_LABEL, acknowledgement_text);
    gtk_check_button_set_active(acknowledgement, TRUE);
    g_signal_emit_by_name(setup_child(setup, "btn_next"), "clicked");
    g_assert_cmpint(onboarding_assistant_get_state(setup), ==, ONBOARDING_READY); setup_snapshot(setup, "ready", rtl);
    gtk_window_destroy(GTK_WINDOW(setup)); g_object_unref(setup);
    gtk_widget_set_default_direction(GTK_TEXT_DIR_LTR);
  }
}
typedef struct {
  AdwApplication *app;
  SignerWindow *window;
  guint completions;
} FirstRunConnect;
static void first_run_connect_finished(gboolean verified, gpointer data) {
  FirstRunConnect *first = data;
  g_assert_false(verified); first->completions++;
  /* Match first-run production: create management only after setup completes. */
  first->window = signer_window_new(first->app); g_object_ref(first->window);
  AdwNavigationSplitView *split = ADW_NAVIGATION_SPLIT_VIEW(gtk_widget_get_template_child(
      GTK_WIDGET(first->window), TYPE_SIGNER_WINDOW, "split_view"));
  g_assert_true(GROTTO_IS_IDENTITY_PANEL(selected_panel(split)));
  gtk_window_present(GTK_WINDOW(first->window));
}
static void first_run_connect_activate(GApplication *application, gpointer data) {
  (void)data;
  FirstRunConnect first = {.app = ADW_APPLICATION(application)};
  OnboardingAssistant *setup = onboarding_assistant_new(); g_object_ref_sink(setup);
  gtk_window_set_application(GTK_WINDOW(setup), GTK_APPLICATION(application));
  g_assert_null(gtk_window_get_transient_for(GTK_WINDOW(setup)));
  onboarding_assistant_set_on_finished(setup, first_run_connect_finished, &first);
  gtk_window_present(GTK_WINDOW(setup));
  WAIT(gtk_widget_is_sensitive(setup_child(setup, "btn_next")));
  g_signal_emit_by_name(setup_child(setup, "btn_next"), "clicked");
  g_signal_emit_by_name(setup_child(setup, "choose_connect"), "activated");
  g_assert_cmpint(onboarding_assistant_get_state(setup), ==, ONBOARDING_CONNECT);
  g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(setup_child(setup, "btn_next"))), ==, _("_Open Connected Apps"));
  g_signal_emit_by_name(setup_child(setup, "btn_next"), "clicked");
  g_assert_cmpuint(first.completions, ==, 1); g_assert_nonnull(first.window);
  AdwNavigationSplitView *split = ADW_NAVIGATION_SPLIT_VIEW(gtk_widget_get_template_child(
      GTK_WIDGET(first.window), TYPE_SIGNER_WINDOW, "split_view"));
  g_assert_true(GROTTO_IS_APPS_PANEL(selected_panel(split)));
  g_assert_false(onboarding_assistant_identity_stored(setup)); g_assert_null(layout_self_test);
  guint windows_gone = 0; g_object_weak_ref(G_OBJECT(first.window), finalised, &windows_gone);
  gtk_window_destroy(GTK_WINDOW(first.window)); g_object_unref(first.window);
  gtk_window_destroy(GTK_WINDOW(setup)); g_object_unref(setup);
  WAIT(windows_gone == 1);
  g_application_quit(application);
}
static void first_run_connect_destination(void) {
  guint apps_gone = 0;
  g_autoptr(AdwApplication) app = adw_application_new("org.nostr.Grotto.FirstRunConnectTest", G_APPLICATION_NON_UNIQUE);
  g_object_weak_ref(G_OBJECT(app), finalised, &apps_gone);
  g_signal_connect(app, "activate", G_CALLBACK(first_run_connect_activate), NULL);
  g_assert_cmpint(g_application_run(G_APPLICATION(app), 0, NULL), ==, 0);
  g_clear_object(&app); WAIT(apps_gone == 1);
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
    g_assert_cmpstr(_("Custom event"), ==, "Evento personalizado");
    gtk_disable_setlocale(); /* Preserve the deliberate test locale through gtk_init_check(). */
  }
  g_test_init(&argc, &argv, NULL); nostrc_test_tolerate_gdk_frame_warning();
  /* GTK can open a session connection through GVfs; establish test custody first. */
  nostrc_test_bus_tolerate_ebadf(); NostrcTestBus *bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE); nostrc_test_bus_up(bus); server = nostrc_test_bus_connect(bus);
  if (!gtk_init_check()) { nostrc_test_bus_down(bus); g_print("1..0 # SKIP GTK display unavailable\n"); return 77; }
  adw_init();
  gpointer panel_class = g_type_class_ref(GROTTO_TYPE_IDENTITY_PANEL); g_type_class_unref(panel_class);
  bindtextdomain("grotto-panels", G10_TEST_LOCALE_DIR); bind_textdomain_codeset("grotto-panels", "UTF-8");
  if (language) {
    g_assert_cmpstr(P_("Allow"), ==, "Permitir");
    g_assert_cmpstr(g_dgettext("grotto-panels", "Copy Public Key"), ==, "Copiar clave pública");
    g_assert_cmpstr(_("Show Recovery Phrase…"), ==, "Mostrar frase de recuperación…");
    g_assert_cmpstr(_("Show QR Code"), ==, "Mostrar código QR");
  }
  g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
  gn_backup_get_npub("0000000000000000000000000000000000000000000000000000000000000001", &npub, NULL);
  BunkerTestRelay *profile_relay = bunker_test_relay_new();
  RelayStore *profile_relays = relay_store_new_for_identity(npub);
  g_assert_true(relay_store_add(profile_relays, bunker_test_relay_url(profile_relay), TRUE, TRUE));
  relay_store_save(profile_relays); relay_store_free(profile_relays);
  profile_relays = relay_store_new();
  g_assert_true(relay_store_add(profile_relays, bunker_test_relay_url(profile_relay), TRUE, TRUE));
  relay_store_save(profile_relays); relay_store_free(profile_relays);
  expires_at = g_get_real_time() / G_USEC_PER_SEC + 600;
  GVariantBuilder initial; g_variant_builder_init(&initial, G_VARIANT_TYPE("aa{sv}"));
  fixture_permission(&initial, npub, NOSTR_KIND_FOLLOW_LIST, "allow");
  fixture_permission(&initial, "*", -1, "review");
  fixture_permission(&initial, npub, NOSTR_KIND_PROFILE_METADATA, "policy-review");
  permission_rows = g_variant_ref_sink(g_variant_builder_end(&initial));
  g_autofree gchar *xml = NULL; g_file_get_contents(G10_CONTROL_XML, &xml, NULL, NULL); g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml(xml, NULL);
  guint registration = g_dbus_connection_register_object(server, "/org/nostr/signer/grotto", node->interfaces[0], &vtable, NULL, NULL, NULL);
  g_autoptr(GDBusNodeInfo) identity = g_dbus_node_info_new_for_xml("<node><interface name='org.nostr.Signer'><method name='ListIdentities'><arg type='as' direction='out'/></method></interface></node>", NULL);
  guint inventory = g_dbus_connection_register_object(server, "/org/nostr/signer", identity->interfaces[0], &vtable, NULL, NULL, NULL);
  g_autoptr(GVariant) name = g_dbus_connection_call_sync(server, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", "org.nostr.Signer", 0u), NULL, 0, 5000, NULL, NULL); g_assert_nonnull(name);
  g_test_add_func("/signer/layout/real-templates-navigation-focus-screenshots", layout);
  g_test_add_func("/signer/layout/remaining-surfaces", remaining_surfaces);
  g_test_add_func("/signer/layout/first-run-connect-destination", first_run_connect_destination); int result = g_test_run();
  bunker_test_relay_free(profile_relay);
  accounts_store_free(accounts_store_get_default()); settings_manager_free(settings_manager_get_default());
  g_dbus_connection_unregister_object(server, registration); g_dbus_connection_unregister_object(server, inventory); nostrc_test_bus_down(bus); g_variant_unref(permission_rows); g_free(npub); return result;
}
