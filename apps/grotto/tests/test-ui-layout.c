#include "ui/signer-window.h"
#include "test-ui-layout-harness.h"
#include "bunker_test_relay.h"
#include "relay_store.h"
#include <grotto-panels.h>
#ifdef GROTTO_TEST_XDOTOOL
#include <gdk/x11/gdkx.h>
#endif
#include "ui/lock-screen.h"
#include "ui/onboarding-assistant.h"
#include "ui/page-connect.h"
#include "ui/page-settings.h"
#include "ui/pairing-flow.h"
#include "ui/widgets/gn-secure-entry.h"
#include "ui/sheets/sheet-qr-display.h"
#include "ui/sheets/sheet-qr-scanner.h"
#include "home-model.h"
#include "ui/page-identity-detail.h"
#include "i18n.h"
#include <nostr-kinds.h>
#include <nostr/nip19/nip19.h>
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
static guint permission_attempts, permission_conflicts, seed_exports, encrypted_exports, pairing_creates;
static gboolean pairing_listening, pairing_multiple;
static gchar *pairing_other;
static const gchar *pairing_selected;
static guint identity_creates, identity_imports;
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
static void wait_for_frames(GtkWidget *widget, guint count) {
  frames = 0; guint id = gtk_widget_add_tick_callback(widget, tick, NULL, NULL); WAIT(frames >= count); gtk_widget_remove_tick_callback(widget, id);
}
static void wait_frames(GtkWidget *widget) { wait_for_frames(widget, 3); }
#ifdef GROTTO_TEST_XDOTOOL
static void focus_window(GtkWindow *window) {
  WAIT(gtk_widget_get_mapped(GTK_WIDGET(window)));
  GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window)); g_assert_true(GDK_IS_X11_SURFACE(surface));
  g_autofree char *id = g_strdup_printf("%lu", gdk_x11_surface_get_xid(surface));
  char *argv[] = {GROTTO_TEST_XDOTOOL, "windowfocus", "--sync", id, NULL};
  g_autoptr(GError) error = NULL; gint status;
  g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL, &status, &error));
  g_assert_no_error(error); g_assert_true(g_spawn_check_wait_status(status, &error)); g_assert_no_error(error);
  WAIT(gtk_window_is_active(window));
}
static void send_key_event(GtkWindow *window, const char *key) {
  focus_window(window);
  char *argv[] = {GROTTO_TEST_XDOTOOL, "key", "--clearmodifiers", "--delay", "0", (char *)key, NULL};
  g_autoptr(GError) error = NULL; gint status;
  g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL, &status, &error));
  g_assert_no_error(error); g_assert_true(g_spawn_check_wait_status(status, &error)); g_assert_no_error(error);
}
static void send_key(GtkWindow *window, const char *key) {
  send_key_event(window, key);
  wait_frames(GTK_WIDGET(window));
}
static graphene_point_t assert_about_caret_visible(GtkTextView *about, SignerWindow *window) {
  GdkRectangle cursor; int x, y;
  gtk_text_view_get_cursor_locations(about, NULL, &cursor, NULL);
  gtk_text_view_buffer_to_window_coords(about, GTK_TEXT_WINDOW_WIDGET, cursor.x, cursor.y, &x, &y);
  graphene_point_t point;
  g_assert_true(gtk_widget_compute_point(GTK_WIDGET(about), GTK_WIDGET(window), &GRAPHENE_POINT_INIT(x, y), &point));
  GtkWidget *scroller = gtk_widget_get_ancestor(GTK_WIDGET(about), GTK_TYPE_SCROLLED_WINDOW);
  g_assert_nonnull(scroller);
  GtkWidget *viewport = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scroller));
  graphene_rect_t clip;
  g_assert_true(gtk_widget_compute_bounds(viewport, GTK_WIDGET(window), &clip));
  g_test_message("About caret y=%.1f..%.1f, page clip=%.1f..%.1f", point.y, point.y + cursor.height, clip.origin.y, clip.origin.y + clip.size.height);
  g_assert_cmpfloat(point.y, >=, clip.origin.y);
  g_assert_cmpfloat(point.y + cursor.height, <=, clip.origin.y + clip.size.height);
  return point;
}
static void scroll_about_with_wheel(SignerWindow *window, graphene_point_t caret) {
  GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window));
  g_autofree char *id = g_strdup_printf("%lu", gdk_x11_surface_get_xid(surface));
  g_autofree char *x = g_strdup_printf("%d", (int)caret.x + 12);
  g_autofree char *y = g_strdup_printf("%d", (int)caret.y + 8);
  char *argv[] = {GROTTO_TEST_XDOTOOL, "mousemove", "--window", id, x, y, "click", "5", NULL};
  g_autoptr(GError) error = NULL; gint status;
  g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL, &status, &error));
  g_assert_no_error(error); g_assert_true(g_spawn_check_wait_status(status, &error)); g_assert_no_error(error);
  wait_frames(GTK_WIDGET(window));
}
#endif
static void return_from_input(GtkWindow *window, GtkWidget *input) {
#ifdef GROTTO_TEST_XDOTOOL
  focus_window(window);
#endif
  if (GN_IS_SECURE_ENTRY(input)) g_assert_true(gn_secure_entry_grab_focus_entry(GN_SECURE_ENTRY(input)));
  else g_assert_true(gtk_widget_grab_focus(input));
  wait_frames(GTK_WIDGET(window));
  GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(window)); g_assert_nonnull(focus);
  g_assert_true(focus == input || gtk_widget_is_ancestor(focus, input));
#ifdef GROTTO_TEST_XDOTOOL
  send_key(GTK_WINDOW(window), "Return");
#else
  gtk_widget_activate_default(input);
  wait_frames(GTK_WIDGET(window));
#endif
}
static void clipboard_text_ready(GObject *source, GAsyncResult *result, gpointer data) {
  g_autoptr(GError) error = NULL;
  *(gchar **)data = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, &error);
  g_assert_no_error(error);
}
static GtkWindow *find_file_chooser(void) {
  GListModel *windows = gtk_window_get_toplevels();
  for (guint i = 0; i < g_list_model_get_n_items(windows); i++) {
    GtkWindow *window = g_list_model_get_item(windows, i);
    if (GTK_IS_FILE_CHOOSER(window) && gtk_widget_get_mapped(GTK_WIDGET(window))) return window;
    g_object_unref(window);
  }
  return NULL;
}
static GtkWidget *find_profile_header(GtkWidget *root, int size) {
  if (GTK_IS_OVERLAY(root)) {
    GtkWidget *avatar = gtk_widget_get_next_sibling(gtk_overlay_get_child(GTK_OVERLAY(root)));
    if (ADW_IS_AVATAR(avatar) && adw_avatar_get_size(ADW_AVATAR(avatar)) == size) return root;
  }
  for (GtkWidget *child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_profile_header(child, size); if (found) return found;
  }
  return NULL;
}
static void assert_profile_overlap(GtkWidget *header) {
  g_assert_nonnull(header);
  GtkWidget *box = gtk_overlay_get_child(GTK_OVERLAY(header));
  GtkWidget *banner = gtk_widget_get_first_child(box), *avatar = gtk_widget_get_next_sibling(box);
  graphene_rect_t a, b;
  g_assert_true(gtk_widget_compute_bounds(avatar, header, &a));
  g_assert_true(gtk_widget_compute_bounds(banner, header, &b));
  g_assert_cmpfloat_with_epsilon(a.origin.x + a.size.width / 2, b.origin.x + b.size.width / 2, 1);
  g_assert_cmpfloat_with_epsilon(a.origin.y + a.size.height / 2, b.origin.y + b.size.height, 1);
}
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
  if (!strcmp(method, "StoreKeyWithMnemonic")) { identity_creates++; g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", npub)); return; }
  if (!strcmp(method, "ImportIdentity")) { identity_imports++; g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", npub)); return; }
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
    g_assert_cmpstr(identity, ==, npub);
    if (!strcmp(format, "ncryptsec")) {
      g_assert_cmpstr(pass, ==, "disposable passphrase"); encrypted_exports++;
      g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", "ncryptsec1fixture")); return;
    }
    g_assert_cmpstr(format, ==, "mnemonic"); g_assert_cmpstr(pass, ==, ""); seed_exports++;
    g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about")); return;
  }
  if (!strcmp(method, "ListIdentities") && !strcmp(interface, "org.nostr.Grotto.Control1")) {
    GVariant *rows = pairing_multiple ? g_variant_new_parsed(
      "[{'identity': <%s>, 'can-sign': <true>, 'watch-only': <false>, 'is-default': <true>}, {'identity': <%s>, 'can-sign': <true>, 'watch-only': <false>, 'is-default': <false>}]", npub, pairing_other) :
      g_variant_new_parsed("[{'identity': <%s>, 'can-sign': <true>, 'watch-only': <false>, 'is-default': <true>}]", npub);
    g_dbus_method_invocation_return_value(inv, g_variant_new("(@aa{sv})", rows)); return;
  }
  if (!strcmp(method, "ConfigureBunker")) {
    g_autoptr(GVariant) config = g_variant_get_child_value(args, 0); g_auto(GStrv) relays = NULL, methods = NULL;
    g_assert_true(g_variant_lookup(config, "enabled", "b", &pairing_listening));
    g_assert_true(pairing_listening);
    g_assert_true(g_variant_lookup(config, "relays", "^as", &relays)); g_assert_cmpstr(relays[0], ==, "wss://nos.lol");
    g_assert_true(g_variant_lookup(config, "allowed-methods", "^as", &methods)); g_assert_cmpstr(methods[0], ==, "sign_event");
    g_dbus_method_invocation_return_value(inv, g_variant_new("(@a{sv})", g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0)));
    g_dbus_connection_emit_signal(bus, NULL, path, interface, "StatusChanged", NULL, NULL); return;
  }
  if (!strcmp(method, "GetConnectionLink")) {
    const gchar *identity; g_variant_get(args, "(&s)", &identity);
    if (*identity) {
      g_assert_true(!strcmp(identity, npub) || (pairing_multiple && !strcmp(identity, pairing_other)));
      pairing_selected = !strcmp(identity, npub) ? npub : pairing_other;
    }
    if (!pairing_selected) pairing_selected = npub;
    pairing_creates++;
    GVariant *offer = g_variant_new_parsed("{'pairing_id': <'fixture'>, 'identity': <%s>, 'uri': <%s>, 'expires_at': <%t>}", pairing_selected,
      "bunker://79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798?relay=wss%3A%2F%2Fnos.lol&secret=fixture",
      (guint64)(g_get_real_time() / G_USEC_PER_SEC + 300));
    g_dbus_method_invocation_return_value(inv, g_variant_new("(@a{sv})", offer)); return;
  }
  if (!strcmp(method, "ListIdentities")) { const gchar *ids[] = {npub, NULL}; g_dbus_method_invocation_return_value(inv, g_variant_new("(^as)", ids)); return; }
  if (!strcmp(method, "CreatePairing")) {
    const gchar *identity, *name; g_variant_get(args, "(&s&s)", &identity, &name);
    g_assert_cmpstr(identity, ==, npub); g_assert_cmpstr(name, ==, "Keyboard App"); pairing_creates++;
    g_dbus_method_invocation_return_dbus_error(inv, "org.nostr.Grotto.Control1.Error.Unavailable", "Fixture declines pairing after recording the request"); return;
  }
  if (!strcmp(method, "GetStatus")) {
    GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
#define ADD(k,v) g_variant_builder_add(&b, "{sv}", k, v)
    GVariantBuilder caps; g_variant_builder_init(&caps, G_VARIANT_TYPE("a{sb}"));
    const char *capabilities[] = {"store-mnemonic", "mnemonic-export", "export-identity", "raw-nsec-export", "identity-self-test", "rotate-identity", "decision-modes"};
    for (guint i = 0; i < G_N_ELEMENTS(capabilities); i++) g_variant_builder_add(&caps, "{sb}", capabilities[i], TRUE);
    ADD("capabilities", g_variant_builder_end(&caps));
    ADD("control_version", g_variant_new_uint32(1)); ADD("permission_policy_version", g_variant_new_uint32(3)); ADD("control_epoch", g_variant_new_string("fixture")); ADD("permissions_revision", g_variant_new_uint64(revision));
    ADD("bunker_enabled", g_variant_new_boolean(pairing_listening));
    const char *relays[] = {"wss://nos.lol", NULL}, *methods[] = {"sign_event", NULL};
    ADD("bunker_relays", g_variant_new_strv(relays, -1)); ADD("bunker_allowed_methods", g_variant_new_strv(methods, -1));
    ADD("signed_today", g_variant_new_uint32(117)); ADD("client_count", g_variant_new_uint32(1)); ADD("pending_count", g_variant_new_uint32(0)); ADD("bunker_state", g_variant_new_string(pairing_listening ? "LISTENING" : "DISABLED"));
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
static void assert_header_control(GtkWidget *control, gboolean affirmative) {
  g_assert_nonnull(control);
  AdwHeaderBar *header = ADW_HEADER_BAR(gtk_widget_get_ancestor(control, ADW_TYPE_HEADER_BAR));
  g_assert_nonnull(header);
  g_assert_false(adw_header_bar_get_show_start_title_buttons(header));
  g_assert_false(adw_header_bar_get_show_end_title_buttons(header));
  if (affirmative) {
    g_assert_true(gtk_widget_has_css_class(control, "suggested-action"));
    g_assert_true(gtk_button_get_use_underline(GTK_BUTTON(control)));
  }
}
static GtkWidget *find_label_containing(GtkWidget *widget, const gchar *text) {
  if (GTK_IS_LABEL(widget) && strstr(gtk_label_get_text(GTK_LABEL(widget)), text)) return widget;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_label_containing(child, text); if (found) return found;
  }
  return NULL;
}
static void assert_header_fits(GtkWidget *control, SignerWindow *window) {
  GtkWidget *header = gtk_widget_get_ancestor(control, ADW_TYPE_HEADER_BAR); g_assert_nonnull(header);
  int minimum = 0; gtk_widget_measure(header, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, NULL, NULL, NULL);
  g_assert_cmpint(minimum, <=, gtk_widget_get_width(GTK_WIDGET(window)));
}
static void collect_dialog_mnemonics(GtkWidget *widget, GHashTable *keys) {
  const gchar *label = NULL;
  if (!gtk_widget_get_mapped(widget)) return;
  if (GTK_IS_BUTTON(widget) && gtk_button_get_use_underline(GTK_BUTTON(widget))) label = gtk_button_get_label(GTK_BUTTON(widget));
  else if (ADW_IS_PREFERENCES_ROW(widget) && adw_preferences_row_get_use_underline(ADW_PREFERENCES_ROW(widget))) label = adw_preferences_row_get_title(ADW_PREFERENCES_ROW(widget));
  if (label) for (const gchar *p = label; *p; p = g_utf8_next_char(p)) {
    if (*p != '_') continue;
    p++; if (!*p) break; if (*p == '_') continue;
    gunichar key = g_unichar_tolower(g_utf8_get_char(p));
    g_test_message("Dialog access key: %s", label);
    g_assert_true(g_hash_table_add(keys, GUINT_TO_POINTER(key))); break;
  }
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) collect_dialog_mnemonics(child, keys);
}
static void assert_dialog_mnemonics(AdwDialog *dialog) {
  g_autoptr(GHashTable) keys = g_hash_table_new(g_direct_hash, g_direct_equal);
  collect_dialog_mnemonics(GTK_WIDGET(dialog), keys);
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
static void assert_pairing_size(AdwDialog *dialog, SignerWindow *window) {
  GtkWidget *scroll = find_editor_control(GTK_WIDGET(dialog), GTK_TYPE_SCROLLED_WINDOW);
  g_assert_nonnull(scroll);
  g_assert_true(gtk_scrolled_window_get_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll)));
  g_assert_cmpint(gtk_scrolled_window_get_min_content_height(GTK_SCROLLED_WINDOW(scroll)), ==, 240);
  g_assert_cmpint(gtk_scrolled_window_get_max_content_height(GTK_SCROLLED_WINDOW(scroll)), ==, 520);
  g_assert_cmpint(gtk_widget_get_height(scroll), >=, 240);
  g_assert_cmpint(gtk_widget_get_width(scroll), <=, gtk_widget_get_width(GTK_WIDGET(window)));
  g_assert_cmpint(gtk_widget_get_height(scroll), <=, gtk_widget_get_height(GTK_WIDGET(window)));
}
static guint assert_kind_rows(GtkWidget *widget, gboolean accessibility) {
  guint count = 0;
  const gchar *kind = g_object_get_data(G_OBJECT(widget), "kind");
  if (GTK_IS_CHECK_BUTTON(widget) && kind) {
    GtkWidget *row = gtk_widget_get_ancestor(widget, ADW_TYPE_ACTION_ROW);
    g_assert_nonnull(row);
    g_autofree gchar *subtitle = g_strdup_printf(_("Kind %u"), (guint)g_ascii_strtoull(kind, NULL, 10));
    g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(row)), ==, subtitle);
    g_assert_true(adw_action_row_get_activatable_widget(ADW_ACTION_ROW(row)) == widget);
    g_assert_true(gtk_check_button_get_active(GTK_CHECK_BUTTON(widget)));
    if (accessibility) {
      g_autofree gchar *name = g_strdup_printf(_("%s, kind %u"),
          adw_preferences_row_get_title(ADW_PREFERENCES_ROW(row)), (guint)g_ascii_strtoull(kind, NULL, 10));
      gtk_test_accessible_assert_property(GTK_ACCESSIBLE(widget), GTK_ACCESSIBLE_PROPERTY_LABEL, name);
    }
    count++;
  }
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) count += assert_kind_rows(child, accessibility);
  return count;
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
static GtkWidget *setup_child(OnboardingAssistant *self, const char *name) {
  return GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(self), TYPE_ONBOARDING_ASSISTANT, name));
}
static void assert_assistant_actions(OnboardingAssistant *setup, gboolean running) {
  wait_frames(GTK_WIDGET(setup));
  GtkWidget *next = setup_child(setup, "btn_next"), *skip = setup_child(setup, "btn_skip");
  AdwNavigationView *view = ADW_NAVIGATION_VIEW(setup_child(setup, "steps"));
  AdwNavigationPage *page = adw_navigation_view_get_visible_page(view);
  GtkWidget *header = gtk_widget_get_ancestor(skip, ADW_TYPE_HEADER_BAR);
  g_assert_nonnull(header);
  g_assert_true(gtk_widget_is_ancestor(header, GTK_WIDGET(page)));
  g_assert_false(adw_header_bar_get_show_start_title_buttons(ADW_HEADER_BAR(header)));
  g_assert_false(adw_header_bar_get_show_end_title_buttons(ADW_HEADER_BAR(header)));
  g_assert_null(gtk_widget_get_template_child(GTK_WIDGET(setup), TYPE_ONBOARDING_ASSISTANT, "btn_back"));
  OnboardingState state = onboarding_assistant_get_state(setup);
  gboolean status_page = state == ONBOARDING_WELCOME || state == ONBOARDING_CONNECT ||
      state == ONBOARDING_TEST_FAILED || state == ONBOARDING_READY;
  g_assert_true(gtk_widget_has_css_class(next, "suggested-action"));
  g_assert_cmpint(gtk_widget_has_css_class(next, "pill"), ==, status_page);
  if (status_page) {
    g_assert_nonnull(gtk_widget_get_ancestor(next, ADW_TYPE_STATUS_PAGE));
    g_assert_null(gtk_widget_get_ancestor(next, ADW_TYPE_HEADER_BAR));
    g_assert_cmpint(gtk_widget_get_halign(next), ==, GTK_ALIGN_CENTER);
  } else g_assert_true(gtk_widget_get_ancestor(next, ADW_TYPE_HEADER_BAR) == header);
  if (running) g_assert_true(gtk_window_get_modal(GTK_WINDOW(setup)));
  if (gtk_widget_get_mapped(next) && gtk_widget_get_mapped(skip) && !status_page) {
    graphene_rect_t affirmative, cancel;
    g_assert_true(gtk_widget_compute_bounds(next, header, &affirmative));
    g_assert_true(gtk_widget_compute_bounds(skip, header, &cancel));
    gboolean rtl = gtk_widget_get_direction(header) == GTK_TEXT_DIR_RTL;
    g_assert_cmpint(cancel.origin.x < affirmative.origin.x, ==, !rtl);
  }
  g_autoptr(GHashTable) keys = g_hash_table_new(g_direct_hash, g_direct_equal);
  collect_dialog_mnemonics(GTK_WIDGET(setup), keys);
}
static void assistant_back(OnboardingAssistant *setup) {
  g_assert_true(adw_navigation_view_pop(ADW_NAVIGATION_VIEW(setup_child(setup, "steps"))));
}
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
  send_key(GTK_WINDOW(window), "Return");
#else
  g_signal_emit_by_name(other_list, "row-activated", other);
#endif
  GtkWidget *pinned_panel = GTK_WIDGET(adw_navigation_view_get_visible_page(root_navigation));
  g_assert_true(GROTTO_IS_IDENTITY_PANEL(pinned_panel));
  GtkWidget *pinned_key = GTK_WIDGET(gtk_widget_get_template_child(pinned_panel, GROTTO_TYPE_IDENTITY_PANEL, "key_row"));
  g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(pinned_key)), ==, watch_npub);
#ifdef GROTTO_TEST_XDOTOOL
  send_key(GTK_WINDOW(window), "alt+Left");
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
  GtkWidget *add_cancel = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(add), TYPE_ONBOARDING_ASSISTANT, "btn_skip"));
  GtkWidget *add_next = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(add), TYPE_ONBOARDING_ASSISTANT, "btn_next"));
  assert_assistant_actions(add, TRUE);
  g_assert_false(gtk_widget_get_visible(add_next));
  g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(add_cancel)), ==, _("_Cancel"));
  g_assert_cmpstr(gtk_widget_get_tooltip_text(add_cancel), ==, _("Cancel Adding an Identity"));
  if (accessibility) gtk_test_accessible_assert_property(GTK_ACCESSIBLE(add_cancel), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, _("Cancel Adding an Identity"));
  GtkWidget *create_choice = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(add), TYPE_ONBOARDING_ASSISTANT, "choose_create"));
  GtkWidget *import_choice = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(add), TYPE_ONBOARDING_ASSISTANT, "choose_import"));
  g_assert_true(gtk_widget_get_visible(create_choice)); g_assert_true(gtk_widget_get_visible(import_choice));
  WAIT(gtk_widget_is_sensitive(import_choice)); g_signal_emit_by_name(import_choice, "activated");
  g_assert_cmpint(onboarding_assistant_get_state(add), ==, ONBOARDING_IMPORT);
  g_assert_true(gtk_widget_get_visible(add_next)); assert_assistant_actions(add, TRUE);
  g_assert_true(gtk_widget_get_visible(add_cancel));
  g_assert_false(gtk_widget_is_sensitive(add_next));
  g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(add_next)), ==, _("_Import"));
  g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(add)), ==, _("Import an Identity"));
  assistant_back(add);
  g_assert_cmpint(onboarding_assistant_get_state(add), ==, ONBOARDING_CHOICE);
  g_assert_true(gtk_widget_get_visible(add_cancel));
  g_signal_emit_by_name(add_cancel, "clicked"); g_object_unref(add);
  WAIT(g_object_get_data(G_OBJECT(window), "grotto-onboarding-assistant") == NULL);
  signer_window_show_page(window, "preferences"); wait_frames(GTK_WIDGET(window));
  AdwDialog *preferences = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_true(ADW_IS_PREFERENCES_DIALOG(preferences));
  PageSettings *settings_page = PAGE_SETTINGS(find_editor_control(GTK_WIDGET(preferences), TYPE_PAGE_SETTINGS)); g_assert_nonnull(settings_page);
  g_assert_null(find_row(GTK_WIDGET(preferences), _("Appearance")));
  g_assert_null(find_row(GTK_WIDGET(preferences), _("Language")));
  g_assert_null(find_row(GTK_WIDGET(preferences), _("Remote Signing")));
  g_assert_nonnull(find_row(GTK_WIDGET(preferences), _("Keep Activity (Days)")));
  adw_dialog_close(preferences); WAIT(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == NULL);
  GSettings *policy_settings = settings_manager_get_gsettings(settings_manager_get_default());
  g_settings_reset(policy_settings, "require-confirmation-kinds");
  signer_window_show_page(window, "apps"); wait_frames(GTK_WIDGET(window));
  GtkWidget *policy_panel = GTK_WIDGET(selected_panel(split));
  g_assert_true(GROTTO_IS_APPS_PANEL(policy_panel));
  GtkWidget *ask_defaults = GTK_WIDGET(gtk_widget_get_template_child(policy_panel, GROTTO_TYPE_APPS_PANEL, "ask_defaults"));
  g_signal_emit_by_name(ask_defaults, "activated"); wait_frames(GTK_WIDGET(window));
  AdwDialog *policy = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_nonnull(policy);
  guint kind_count = assert_kind_rows(GTK_WIDGET(policy), accessibility); g_assert_cmpuint(kind_count, >, 5);
  GtkWidget *first_kind = find_editor_control(GTK_WIDGET(policy), GTK_TYPE_CHECK_BUTTON);
  g_autofree gchar *disabled_kind = g_strdup(g_object_get_data(G_OBJECT(first_kind), "kind"));
  gtk_check_button_set_active(GTK_CHECK_BUTTON(first_kind), FALSE);
  GtkWidget *policy_save = find_button(GTK_WIDGET(policy), _("_Save")); assert_header_control(policy_save, TRUE);
  assert_dialog_mnemonics(policy);
  GtkWidget *custom = find_editor_control(GTK_WIDGET(policy), GTK_TYPE_ENTRY);
  g_assert_nonnull(custom); gtk_editable_set_text(GTK_EDITABLE(custom), "65536");
  g_assert_false(gtk_widget_is_sensitive(policy_save));
  return_from_input(GTK_WINDOW(window), custom);
  g_assert_true(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == policy);
  gtk_editable_set_text(GTK_EDITABLE(custom), ""); g_assert_true(gtk_widget_is_sensitive(policy_save));
  return_from_input(GTK_WINDOW(window), custom);
  WAIT(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == NULL);
  g_auto(GStrv) saved_kinds = g_settings_get_strv(policy_settings, "require-confirmation-kinds");
  g_assert_cmpuint(g_strv_length(saved_kinds), ==, kind_count - 1);
  g_assert_false(g_strv_contains((const gchar *const *)saved_kinds, disabled_kind));
  g_signal_emit_by_name(ask_defaults, "activated"); wait_frames(GTK_WIDGET(window));
  policy = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_signal_emit_by_name(find_row(GTK_WIDGET(policy), _("_Restore Defaults")), "activated");
  g_auto(GStrv) before_cancel = g_settings_get_strv(policy_settings, "require-confirmation-kinds");
  g_assert_cmpuint(g_strv_length(before_cancel), ==, kind_count - 1);
  g_signal_emit_by_name(find_button(GTK_WIDGET(policy), _("_Cancel")), "clicked");
  WAIT(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  g_auto(GStrv) after_cancel = g_settings_get_strv(policy_settings, "require-confirmation-kinds");
  g_assert_cmpuint(g_strv_length(after_cancel), ==, kind_count - 1);
  g_settings_reset(policy_settings, "require-confirmation-kinds");
  gint retention_before = g_settings_get_int(policy_settings, "log-retention-days");
  page_settings_show_retention(settings_page, GTK_WIDGET(window)); wait_frames(GTK_WIDGET(window));
  AdwDialog *retention = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  assert_header_control(find_button(GTK_WIDGET(retention), _("_Save")), TRUE); assert_dialog_mnemonics(retention);
  GtkWidget *days = find_editor_control(GTK_WIDGET(retention), ADW_TYPE_SPIN_ROW); g_assert_nonnull(days);
  adw_spin_row_set_value(ADW_SPIN_ROW(days), retention_before == 1 ? 2 : 1);
#ifdef GROTTO_TEST_XDOTOOL
  send_key(GTK_WINDOW(window), "Escape");
#else
  adw_dialog_close(retention);
#endif
  WAIT(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  g_assert_cmpint(g_settings_get_int(policy_settings, "log-retention-days"), ==, retention_before);
  page_settings_show_retention(settings_page, GTK_WIDGET(window)); wait_frames(GTK_WIDGET(window));
  retention = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  days = find_editor_control(GTK_WIDGET(retention), ADW_TYPE_SPIN_ROW);
  gint retention_after = retention_before == 1 ? 2 : 1;
  GtkWidget *spin = find_editor_control(days, GTK_TYPE_SPIN_BUTTON); g_assert_nonnull(spin);
  g_autofree gchar *typed_days = g_strdup_printf("%d", retention_after);
  gtk_editable_set_text(GTK_EDITABLE(spin), typed_days);
  return_from_input(GTK_WINDOW(window), spin);
  WAIT(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  g_assert_cmpint(g_settings_get_int(policy_settings, "log-retention-days"), ==, retention_after);
  g_settings_set_int(policy_settings, "log-retention-days", retention_before);
  g_assert_false(g_action_get_enabled(g_action_map_lookup_action(G_ACTION_MAP(window), "lock")));
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
    send_key(GTK_WINDOW(window), "Return");
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
    send_key(GTK_WINDOW(window), "alt+Left");
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
    send_key(GTK_WINDOW(window), "Tab"); assert_focus_bounds(window);
#endif
    snapshot(GTK_WIDGET(window), dark ? "identity-360-rtl-dark" : "identity-360-rtl-light");
    g_assert_true(gtk_widget_grab_focus(backup_row));
#ifdef GROTTO_TEST_XDOTOOL
    send_key(GTK_WINDOW(window), "Return");
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
    send_key(GTK_WINDOW(window), "alt+Right");
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
    send_key(GTK_WINDOW(window), "Return");
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
    /* Frame delivery is not completion of the asynchronous permissions query. */
    GtkWidget *effective = NULL;
    WAIT((effective = find_row(detail, _("Share public key (after first consent)"))));
    g_assert_nonnull(effective);
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
    send_key(GTK_WINDOW(window), "Return");
#else
    g_signal_emit_by_name(permission, "activated");
#endif
    WAIT(find_permission(detail, watch_npub, "deny") != NULL); wait_frames(GTK_WIDGET(window));
    g_assert_cmpstr(adw_navigation_page_get_title(adw_navigation_view_get_visible_page(permissions_nav)), ==, P_("Permissions"));
    g_assert_cmpint(gtk_widget_get_direction(detail), ==, GTK_TEXT_DIR_RTL);
#ifdef GROTTO_TEST_XDOTOOL
    send_key(GTK_WINDOW(window), "Tab"); assert_visible_focus(window); assert_focus_bounds(window);
#endif
    /* A toast close button can legitimately receive Tab focus and pause its
     * timeout. Return focus to the permission being inspected before capture. */
    GtkWidget *rtl_permission = find_permission(detail, watch_npub, "deny");
    g_assert_true(gtk_widget_grab_focus(rtl_permission)); wait_frames(GTK_WIDGET(window)); assert_focus_bounds(window);
    snapshot(GTK_WIDGET(window), dark ? "permissions-360-rtl-dark" : "permissions-360-rtl-light");
#ifdef GROTTO_TEST_XDOTOOL
    send_key(GTK_WINDOW(window), "alt+Right");
#else
    g_assert_true(adw_navigation_view_pop(permissions_nav));
#endif
    g_assert_true(gtk_root_get_focus(GTK_ROOT(window)) == permission); assert_focus_bounds(window);
#ifdef GROTTO_TEST_XDOTOOL
    send_key(GTK_WINDOW(window), "alt+Right");
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
      send_key(GTK_WINDOW(window), "ctrl+a"); send_key(GTK_WINDOW(window), "BackSpace");
      g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(input)), ==, "");
#else
      gtk_editable_set_text(GTK_EDITABLE(input), "");
#endif
#ifdef GROTTO_TEST_XDOTOOL
      send_key(GTK_WINDOW(window), "Tab"); assert_visible_focus(window); assert_focus_bounds(window);
      send_key(GTK_WINDOW(window), "shift+Tab"); assert_visible_focus(window); assert_focus_bounds(window);
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
  WAIT(gtk_widget_get_sensitive(edit_profile));
#ifdef GROTTO_TEST_XDOTOOL
  /* ScrolledWindow configures its adjustment animation when it maps. Enable
   * animations before creating the editor so the focus-race test is real. */
  GtkSettings *profile_gtk_settings = gtk_settings_get_default();
  gboolean profile_animations;
  g_object_get(profile_gtk_settings, "gtk-enable-animations", &profile_animations, NULL);
  g_object_set(profile_gtk_settings, "gtk-enable-animations", TRUE, NULL);
#endif
  g_signal_emit_by_name(edit_profile, "clicked");
  AdwDialog *profile_dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_nonnull(profile_dialog); g_assert_cmpstr(adw_dialog_get_title(profile_dialog), ==, P_("Edit Profile"));
  g_assert_nonnull(find_row(GTK_WIDGET(profile_dialog), P_("Avatar URL")));
  g_assert_nonnull(find_row(GTK_WIDGET(profile_dialog), P_("Banner URL")));
  g_assert_nonnull(find_row(GTK_WIDGET(profile_dialog), P_("Automated Account (Bot)")));
  g_assert_nonnull(find_row(GTK_WIDGET(profile_dialog), P_("Profile Preview")));
  g_assert_true(ADW_IS_ACTION_ROW(find_row(GTK_WIDGET(profile_dialog), P_("Discard Local Changes"))));
  g_assert_null(find_button(GTK_WIDGET(profile_dialog), P_("Republish")));
  GtkWidget *profile_save = find_button(GTK_WIDGET(profile_dialog), P_("Save"));
  GtkWidget *profile_cancel = find_button(GTK_WIDGET(profile_dialog), P_("Cancel"));
  g_assert_nonnull(profile_save); g_assert_nonnull(profile_cancel);
  g_assert_true(gtk_widget_has_css_class(profile_save, "suggested-action"));
  GtkWidget *profile_header = gtk_widget_get_ancestor(profile_save, ADW_TYPE_HEADER_BAR);
  g_assert_nonnull(profile_header);
  g_assert_false(adw_header_bar_get_show_start_title_buttons(ADW_HEADER_BAR(profile_header)));
  g_assert_false(adw_header_bar_get_show_end_title_buttons(ADW_HEADER_BAR(profile_header)));
  GtkWidget *about_row = gtk_widget_get_ancestor(GTK_WIDGET(find_text_view(GTK_WIDGET(profile_dialog))), GTK_TYPE_LIST_BOX_ROW);
  g_assert_nonnull(about_row);
  g_assert_true(gtk_widget_get_parent(about_row) == gtk_widget_get_parent(find_row(GTK_WIDGET(profile_dialog), P_("Name"))));
  gtk_window_set_default_size(GTK_WINDOW(window), 360, 640); wait_frames(GTK_WIDGET(window));
  int profile_inset = 360 - gtk_widget_get_width(GTK_WIDGET(window));
  gtk_window_set_default_size(GTK_WINDOW(window), 360 + profile_inset, 640); wait_frames(GTK_WIDGET(window));
  g_assert_cmpint(gtk_widget_get_width(GTK_WIDGET(window)), ==, 360);
#ifdef GROTTO_TEST_XDOTOOL
  GtkTextView *profile_about = find_text_view(GTK_WIDGET(profile_dialog));
  GtkTextBuffer *profile_buffer = gtk_text_view_get_buffer(profile_about); GtkTextIter start, end;
  gtk_text_buffer_get_bounds(profile_buffer, &start, &end);
  g_autofree char *original_about = gtk_text_buffer_get_text(profile_buffer, &start, &end, FALSE);
  if (accessibility) {
    gtk_test_accessible_assert_property(GTK_ACCESSIBLE(profile_about), GTK_ACCESSIBLE_PROPERTY_LABEL, P_("About"));
    gtk_test_accessible_assert_property(GTK_ACCESSIBLE(profile_about), GTK_ACCESSIBLE_PROPERTY_PLACEHOLDER, P_("Write something about yourself…"));
  }
  GtkScrolledWindow *profile_scroll = GTK_SCROLLED_WINDOW(gtk_widget_get_ancestor(GTK_WIDGET(profile_about), GTK_TYPE_SCROLLED_WINDOW));
  g_assert_nonnull(profile_scroll);
  GtkAdjustment *profile_adjustment = gtk_scrolled_window_get_vadjustment(profile_scroll);
  /* The field grows in the page; there must not be a separate text scroller. */
  g_assert_true(gtk_widget_is_ancestor(about_row, GTK_WIDGET(profile_scroll)));
  g_assert_true(gtk_widget_is_ancestor(GTK_WIDGET(profile_scroll), gtk_widget_get_ancestor(about_row, ADW_TYPE_PREFERENCES_PAGE)));
  g_autoptr(GString) biography = g_string_new(NULL);
  for (guint i = 0; i < 80; i++) g_string_append_printf(biography, "Line %u of an editable profile biography\n", i);
  const int profile_widths[] = {360, 798};
  for (guint i = 0; i < G_N_ELEMENTS(profile_widths); i++) {
    gtk_window_set_default_size(GTK_WINDOW(window), profile_widths[i] + profile_inset, 720);
    wait_for_frames(GTK_WIDGET(window), 30);
    g_assert_cmpint(gtk_widget_get_width(GTK_WIDGET(window)), ==, profile_widths[i]);
    gtk_text_buffer_set_text(profile_buffer, biography->str, -1);
    g_assert_true(gtk_widget_grab_focus(GTK_WIDGET(profile_about))); send_key(GTK_WINDOW(window), "ctrl+End");
    wait_for_frames(GTK_WIDGET(window), 30);
    g_assert_cmpint(gtk_widget_get_height(GTK_WIDGET(profile_about)), >, gtk_widget_get_height(GTK_WIDGET(profile_scroll)));
    assert_about_caret_visible(profile_about, window);
    g_assert_cmpfloat(gtk_adjustment_get_value(profile_adjustment), >, 0);
    send_key(GTK_WINDOW(window), "a"); wait_for_frames(GTK_WIDGET(window), 30);
    assert_about_caret_visible(profile_about, window);
    send_key(GTK_WINDOW(window), "Return"); wait_for_frames(GTK_WIDGET(window), 30);
    g_assert_cmpint(gtk_text_buffer_get_line_count(profile_buffer), ==, 82);
    assert_about_caret_visible(profile_about, window);
    g_assert_true(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == profile_dialog);
    g_autofree char *end_capture = g_strdup_printf("profile-about-long-end-%d", profile_widths[i]);
    snapshot(GTK_WIDGET(window), end_capture);
    /* Leave the field, scroll its retained end caret out of view, then use
     * actual backward keyboard traversal to re-enter it without moving the mark. */
    send_key(GTK_WINDOW(window), "Tab"); wait_for_frames(GTK_WIDGET(window), 30);
    g_assert_true(gtk_root_get_focus(GTK_ROOT(window)) != GTK_WIDGET(profile_about));
    gtk_adjustment_set_value(profile_adjustment, gtk_adjustment_get_lower(profile_adjustment));
    wait_for_frames(GTK_WIDGET(window), 30);
    GdkRectangle insertion; int x, y; graphene_point_t offscreen;
    gtk_text_view_get_cursor_locations(profile_about, NULL, &insertion, NULL);
    gtk_text_view_buffer_to_window_coords(profile_about, GTK_TEXT_WINDOW_WIDGET, insertion.x, insertion.y, &x, &y);
    g_assert_true(gtk_widget_compute_point(GTK_WIDGET(profile_about), GTK_WIDGET(window), &GRAPHENE_POINT_INIT(x, y), &offscreen));
    graphene_rect_t clip;
    g_assert_true(gtk_widget_compute_bounds(gtk_scrolled_window_get_child(profile_scroll), GTK_WIDGET(window), &clip));
    g_assert_cmpfloat(offscreen.y, >, clip.origin.y + clip.size.height);
    send_key(GTK_WINDOW(window), "shift+Tab"); wait_for_frames(GTK_WIDGET(window), 30);
    g_assert_true(gtk_root_get_focus(GTK_ROOT(window)) == GTK_WIDGET(profile_about));
    assert_about_caret_visible(profile_about, window);
    g_autofree char *focus_capture = g_strdup_printf("profile-about-focus-reentry-%d", profile_widths[i]);
    snapshot(GTK_WIDGET(window), focus_capture);
    send_key(GTK_WINDOW(window), "ctrl+Home"); wait_for_frames(GTK_WIDGET(window), 30);
    graphene_point_t caret = assert_about_caret_visible(profile_about, window);
    double before_wheel = gtk_adjustment_get_value(profile_adjustment);
    scroll_about_with_wheel(window, caret); wait_for_frames(GTK_WIDGET(window), 30);
    g_assert_cmpfloat(gtk_adjustment_get_value(profile_adjustment), >, before_wheel);
    g_assert_cmpfloat(gtk_adjustment_get_value(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(profile_about))), ==, 0);
  }
  g_object_set(profile_gtk_settings, "gtk-enable-animations", profile_animations, NULL);
  gtk_text_buffer_set_text(profile_buffer, original_about, -1);
  gtk_window_set_default_size(GTK_WINDOW(window), 360 + profile_inset, 640); wait_frames(GTK_WIDGET(window));
  g_assert_true(gtk_widget_grab_focus(GTK_WIDGET(profile_about))); send_key(GTK_WINDOW(window), "Tab");
  GtkWidget *after_about = gtk_root_get_focus(GTK_ROOT(window));
  g_assert_nonnull(after_about); g_assert_true(after_about != GTK_WIDGET(profile_about));
  g_assert_false(gtk_widget_is_ancestor(after_about, GTK_WIDGET(profile_about)));
  g_assert_true(gtk_widget_grab_focus(GTK_WIDGET(profile_about))); send_key(GTK_WINDOW(window), "shift+Tab");
  after_about = gtk_root_get_focus(GTK_ROOT(window));
  g_assert_nonnull(after_about); g_assert_true(after_about != GTK_WIDGET(profile_about));
  g_assert_false(gtk_widget_is_ancestor(after_about, GTK_WIDGET(profile_about)));
  gtk_text_buffer_get_bounds(profile_buffer, &start, &end);
  g_autofree char *after_tab = gtk_text_buffer_get_text(profile_buffer, &start, &end, FALSE);
  g_assert_cmpstr(after_tab, ==, original_about);
  gtk_text_buffer_set_text(profile_buffer, "Discard this draft", -1);
  GtkWidget *discard_row = find_row(GTK_WIDGET(profile_dialog), P_("Discard Local Changes"));
  g_assert_true(gtk_widget_grab_focus(discard_row)); send_key(GTK_WINDOW(window), "Return");
  gtk_text_buffer_get_bounds(profile_buffer, &start, &end);
  g_autofree char *after_discard = gtk_text_buffer_get_text(profile_buffer, &start, &end, FALSE);
  g_assert_cmpstr(after_discard, ==, original_about);
  g_assert_true(gtk_widget_grab_focus(profile_cancel));
  gtk_adjustment_set_value(profile_adjustment, gtk_adjustment_get_lower(profile_adjustment));
#endif
  snapshot(GTK_WIDGET(window), "profile-editor-360"); assert_focus_bounds(window);
  /* Synthetic local paintable: exercise layout without downloading profile images. */
  static const guint8 banner_pixels[] = {80, 120, 180, 255, 80, 120, 180, 255, 80, 120, 180, 255};
  g_autoptr(GBytes) banner_bytes = g_bytes_new_static(banner_pixels, sizeof banner_pixels);
  g_autoptr(GdkTexture) banner_texture = gdk_memory_texture_new(3, 1, GDK_MEMORY_R8G8B8A8, banner_bytes, 12);
  GtkWidget *identity_header = find_profile_header(profile_box, 96);
  GtkWidget *preview_header = find_profile_header(GTK_WIDGET(profile_dialog), 64);
  g_assert_nonnull(identity_header); g_assert_nonnull(preview_header);
  GtkWidget *profile_headers[] = {identity_header, preview_header};
  for (guint i = 0; i < G_N_ELEMENTS(profile_headers); i++) {
    GtkWidget *banner = gtk_widget_get_first_child(gtk_overlay_get_child(GTK_OVERLAY(profile_headers[i])));
    gtk_picture_set_paintable(GTK_PICTURE(banner), GDK_PAINTABLE(banner_texture));
    gtk_widget_set_visible(banner, TRUE);
  }
  AdwExpanderRow *profile_preview = ADW_EXPANDER_ROW(find_row(GTK_WIDGET(profile_dialog), P_("Profile Preview")));
  adw_expander_row_set_expanded(profile_preview, TRUE); wait_frames(GTK_WIDGET(window));
  assert_profile_overlap(identity_header); assert_profile_overlap(preview_header);
  snapshot(GTK_WIDGET(window), "profile-preview-360");
  adw_expander_row_set_expanded(profile_preview, FALSE);
  gtk_window_set_default_size(GTK_WINDOW(window), 920, 640); wait_frames(GTK_WIDGET(window));
  g_assert_cmpint(gtk_widget_get_width(GTK_WIDGET(window)), >, 360);
  snapshot(GTK_WIDGET(window), "profile-editor-wide"); assert_focus_bounds(window);
  adw_expander_row_set_expanded(profile_preview, TRUE); wait_frames(GTK_WIDGET(window));
  assert_profile_overlap(identity_header); assert_profile_overlap(preview_header);
  snapshot(GTK_WIDGET(window), "profile-preview-wide");
  adw_expander_row_set_expanded(profile_preview, FALSE);
  AdwExpanderRow *publication = ADW_EXPANDER_ROW(find_row(GTK_WIDGET(profile_dialog), P_("Publication and Discovery Details")));
  adw_expander_row_set_expanded(publication, TRUE); wait_frames(GTK_WIDGET(window));
  g_assert_true(ADW_IS_ACTION_ROW(find_row(GTK_WIDGET(publication), P_("Publication"))));
  g_assert_true(ADW_IS_ACTION_ROW(find_row(GTK_WIDGET(publication), P_("Discovery"))));
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
  GtkWidget *encrypted_qr_row = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(backup_page), GROTTO_TYPE_IDENTITY_DETAIL, "qr_row"));
  WAIT(gtk_widget_is_sensitive(encrypted_qr_row)); g_signal_emit_by_name(encrypted_qr_row, "activated"); wait_frames(GTK_WIDGET(window));
  AdwDialog *passphrase_dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  GtkWidget *show_qr = find_button(GTK_WIDGET(passphrase_dialog), _("Show _QR Code"));
  assert_header_control(show_qr, TRUE); g_assert_false(gtk_widget_is_sensitive(show_qr)); assert_dialog_mnemonics(passphrase_dialog);
  GtkWidget *secure_entry = find_editor_control(GTK_WIDGET(passphrase_dialog), GN_TYPE_SECURE_ENTRY); g_assert_nonnull(secure_entry);
  GtkWidget *passphrase_focus = gtk_root_get_focus(GTK_ROOT(window));
  g_assert_nonnull(passphrase_focus); g_assert_true(gtk_widget_is_ancestor(passphrase_focus, secure_entry));
  return_from_input(GTK_WINDOW(window), secure_entry);
  g_assert_cmpuint(encrypted_exports, ==, 0);
  g_assert_true(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == passphrase_dialog);
  gn_secure_entry_set_text(GN_SECURE_ENTRY(secure_entry), "disposable passphrase"); g_assert_true(gtk_widget_is_sensitive(show_qr));
  return_from_input(GTK_WINDOW(window), secure_entry);
  WAIT(encrypted_exports == 1);
  WAIT(SHEET_IS_QR_DISPLAY(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window))));
  wait_frames(GTK_WIDGET(window));
  g_test_message("Return exported the encrypted QR; close the mapped result sheet");
  adw_dialog_close(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  WAIT(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  GtkWidget *recovery_row = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(backup_page), GROTTO_TYPE_IDENTITY_DETAIL, "phrase_row"));
  WAIT(gtk_widget_is_sensitive(recovery_row)); g_signal_emit_by_name(recovery_row, "activated"); wait_frames(GTK_WIDGET(window));
  AdwDialog *recovery = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  WAIT(seed_exports == 1);
  GtkWidget *confirm_backup = find_button(GTK_WIDGET(recovery), _("Confirm _Backup"));
  assert_header_control(confirm_backup, TRUE); assert_header_fits(confirm_backup, window);
  g_assert_false(gtk_widget_is_sensitive(confirm_backup)); assert_dialog_mnemonics(recovery);
  snapshot(GTK_WIDGET(window), "recovery-phrase-360");
  adw_dialog_close(recovery); WAIT(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  SheetQrDisplay *qr = sheet_qr_display_new(); sheet_qr_display_set_npub(qr, npub);
  adw_dialog_present(ADW_DIALOG(qr), GTK_WIDGET(window)); snapshot(GTK_WIDGET(window), "public-key-qr-360");
  GtkWidget *copy_qr = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(qr), TYPE_SHEET_QR_DISPLAY, "btn_copy"));
  assert_header_control(copy_qr, FALSE);
  g_assert_cmpstr(gtk_widget_get_tooltip_text(copy_qr), ==, _("Copy to Clipboard"));
  assert_dialog_mnemonics(ADW_DIALOG(qr));
  g_signal_emit_by_name(copy_qr, "clicked");
  g_assert_null(gtk_button_get_label(GTK_BUTTON(copy_qr)));
  g_assert_cmpstr(gtk_button_get_icon_name(GTK_BUTTON(copy_qr)), ==, "object-select-symbolic");
  g_assert_cmpstr(gtk_widget_get_tooltip_text(copy_qr), ==, _("Copied")); assert_header_fits(copy_qr, window);
  GdkClipboard *qr_clipboard = gtk_widget_get_clipboard(copy_qr);
  g_autofree gchar *copied_key = NULL;
  gdk_clipboard_read_text_async(qr_clipboard, NULL, clipboard_text_ready, &copied_key); WAIT(copied_key != NULL);
  g_autofree gchar *expected_clipboard = g_strconcat("nostr:", npub, NULL);
  g_assert_cmpstr(copied_key, ==, expected_clipboard);
  WAIT(!g_strcmp0(gtk_widget_get_tooltip_text(copy_qr), _("Copy to Clipboard")));
  g_assert_null(gtk_button_get_label(GTK_BUTTON(copy_qr)));
  g_assert_cmpstr(gtk_button_get_icon_name(GTK_BUTTON(copy_qr)), ==, "edit-copy-symbolic");
  g_assert_false(gtk_widget_has_css_class(copy_qr, "success")); assert_header_fits(copy_qr, window);
  snapshot(GTK_WIDGET(window), "public-key-qr-copy-reset-360");
  adw_dialog_close(ADW_DIALOG(qr)); WAIT(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  g_assert_null(gdk_clipboard_get_content(qr_clipboard));
  g_action_group_activate_action(G_ACTION_GROUP(window), "pair-application", NULL); wait_frames(GTK_WIDGET(window));
  AdwDialog *add_app = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_cmpstr(adw_dialog_get_title(add_app), ==, _("Add App"));
  GtkWidget *create_link = find_row(GTK_WIDGET(add_app), _("Show My Connection Link")); g_assert_nonnull(create_link);
  assert_dialog_mnemonics(add_app);
  g_assert_null(find_button(GTK_WIDGET(add_app), _("Create _Link…")));
  snapshot(GTK_WIDGET(window), "add-app-360");
  g_signal_emit_by_name(create_link, "activated"); snapshot(GTK_WIDGET(window), "connection-link-360");
  AdwDialog *create_sheet = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_true(SHEET_IS_QR_DISPLAY(create_sheet));
  assert_dialog_mnemonics(create_sheet);
  adw_dialog_close(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  WAIT(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == add_app);
  GtkWidget *connect_page = find_editor_control(GTK_WIDGET(add_app), GROTTO_TYPE_PAGE_CONNECT);
  GtkWidget *paste_row = GTK_WIDGET(gtk_widget_get_template_child(connect_page, GROTTO_TYPE_PAGE_CONNECT, "paste"));
  const gchar *app_link = "nostrconnect://79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798?relay=wss%3A%2F%2Frelay.example&secret=fixture&name=Example%20App";
  GdkClipboard *app_clipboard = gtk_widget_get_clipboard(paste_row);
  gdk_clipboard_set_text(app_clipboard, app_link);
  g_signal_emit_by_name(paste_row, "activated");
  WAIT(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) != add_app);
  AdwDialog *clipboard_preview = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  snapshot(GTK_WIDGET(window), "clipboard-link-preview-360"); assert_pairing_size(clipboard_preview, window);
  g_assert_cmpstr(adw_dialog_get_title(clipboard_preview), ==, _("Connect an App"));
  g_assert_nonnull(find_label_containing(GTK_WIDGET(clipboard_preview), "Example App"));
  g_assert_null(find_label_containing(GTK_WIDGET(clipboard_preview), "secret=fixture"));
  GtkWidget *clipboard_review = find_button(GTK_WIDGET(clipboard_preview), _("_Review"));
  assert_header_control(clipboard_review, TRUE); assert_header_control(find_button(GTK_WIDGET(clipboard_preview), _("_Cancel")), FALSE);
  g_assert_false(gtk_widget_is_sensitive(clipboard_review)); assert_header_fits(clipboard_review, window);
  GtkDropDown *identity_picker = GTK_DROP_DOWN(find_editor_control(GTK_WIDGET(clipboard_preview), GTK_TYPE_DROP_DOWN));
  WAIT(gtk_drop_down_get_model(identity_picker) && g_list_model_get_n_items(gtk_drop_down_get_model(identity_picker)) == 1);
  gtk_drop_down_set_selected(identity_picker, 0);
  snapshot(GTK_WIDGET(window), "clipboard-selected-identity-360");
  g_assert_true(gtk_widget_is_sensitive(clipboard_review));
  g_assert_cmpstr(gtk_string_object_get_string(GTK_STRING_OBJECT(gtk_drop_down_get_selected_item(identity_picker))), ==, npub);
  GtkWidget *identity_label = find_label_containing(GTK_WIDGET(identity_picker), npub);
  g_assert_nonnull(identity_label);
  g_assert_cmpstr(gtk_widget_get_tooltip_text(identity_label), ==, npub);
  int preview_minimum;
  gtk_widget_measure(GTK_WIDGET(clipboard_preview), GTK_ORIENTATION_HORIZONTAL, -1, &preview_minimum, NULL, NULL, NULL);
  g_assert_cmpint(preview_minimum, <=, 360);
  assert_pairing_size(clipboard_preview, window);
  GtkWidget *details = find_editor_control(GTK_WIDGET(clipboard_preview), GTK_TYPE_EXPANDER);
  gtk_expander_set_expanded(GTK_EXPANDER(details), TRUE);
  snapshot(GTK_WIDGET(window), "clipboard-link-details-360"); assert_pairing_size(clipboard_preview, window);
  g_assert_nonnull(find_label_containing(GTK_WIDGET(clipboard_preview), "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"));
  g_assert_nonnull(find_label_containing(GTK_WIDGET(clipboard_preview), "wss://relay.example"));
  adw_dialog_close(clipboard_preview); WAIT(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == add_app);
  gdk_clipboard_set_text(app_clipboard, "nostrconnect://invalid");
  g_signal_emit_by_name(paste_row, "activated");
  WAIT(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) != add_app);
  AdwDialog *invalid_preview = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  snapshot(GTK_WIDGET(window), "clipboard-invalid-link-360"); assert_pairing_size(invalid_preview, window);
  g_assert_cmpstr(adw_dialog_get_title(invalid_preview), ==, _("Invalid App Link"));
  g_assert_nonnull(find_label_containing(GTK_WIDGET(invalid_preview), _("This connection link is invalid. Copy a new link from your app.")));
  GtkWidget *invalid_review = find_button(GTK_WIDGET(invalid_preview), _("_Review"));
  assert_header_control(invalid_review, TRUE); g_assert_false(gtk_widget_is_sensitive(invalid_review));
  GtkWidget *invalid_identity = find_editor_control(GTK_WIDGET(invalid_preview), GTK_TYPE_DROP_DOWN);
  g_assert_false(gtk_widget_is_sensitive(invalid_identity));
  g_signal_emit_by_name(find_button(GTK_WIDGET(invalid_preview), _("_Cancel")), "clicked");
  WAIT(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == add_app);
  gdk_clipboard_set_text(app_clipboard, "");
  adw_dialog_close(add_app); WAIT(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  SheetQrScanner *scanner = sheet_qr_scanner_new();
  const GnQrContentType pairing_types[] = {GN_QR_TYPE_BUNKER_URI, GN_QR_TYPE_UNKNOWN};
  sheet_qr_scanner_set_accepted_types(scanner, pairing_types); adw_dialog_present(ADW_DIALOG(scanner), GTK_WIDGET(window));
  GtkWidget *hint = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(scanner), TYPE_SHEET_QR_SCANNER, "hint_label"));
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(hint)), ==, _("Scan the connection code shown by your app"));
  snapshot(GTK_WIDGET(window), "pairing-scan-360");
  GtkWidget *scanner_submit = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(scanner), TYPE_SHEET_QR_SCANNER, "btn_import"));
  assert_header_control(scanner_submit, TRUE); g_assert_false(gtk_widget_is_sensitive(scanner_submit));
  assert_dialog_mnemonics(ADW_DIALOG(scanner));
  GtkWidget *scanner_page = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(scanner), TYPE_SHEET_QR_SCANNER, "scanner_page"));
  GnQrScanResult scan_result = {.data = "nostrconnect://79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798?relay=wss%3A%2F%2Frelay.example&secret=fixture&name=Example%20App", .type = GN_QR_TYPE_BUNKER_URI};
  g_signal_emit_by_name(gtk_widget_get_first_child(scanner_page), "qr-detected", &scan_result);
  GtkWidget *scanner_stack = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(scanner), TYPE_SHEET_QR_SCANNER, "stack"));
  g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(scanner_stack)), ==, "result");
  GtkWidget *import_button = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(scanner), TYPE_SHEET_QR_SCANNER, "btn_import"));
  g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(import_button)), ==, _("Con_tinue"));
  g_assert_cmpstr(gtk_widget_get_tooltip_text(import_button), ==, _("Choose an Identity"));
  snapshot(GTK_WIDGET(window), "pairing-scan-result-360");
  g_assert_true(gtk_widget_is_sensitive(import_button)); assert_dialog_mnemonics(ADW_DIALOG(scanner));
  GtkWidget *scan_back = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(scanner), TYPE_SHEET_QR_SCANNER, "btn_back"));
  g_signal_emit_by_name(scan_back, "activated"); g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(scanner_stack)), ==, "scanner");
  g_assert_false(gtk_widget_is_sensitive(import_button));
  gtk_widget_set_direction(GTK_WIDGET(window), GTK_TEXT_DIR_RTL); snapshot(GTK_WIDGET(window), "pairing-scan-360-rtl");
  gtk_widget_set_direction(GTK_WIDGET(window), GTK_TEXT_DIR_LTR); adw_dialog_close(ADW_DIALOG(scanner));
  WAIT(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  grotto_pairing_present(GTK_WIDGET(window), "nostrconnect://79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798?relay=wss%3A%2F%2Frelay.example&secret=fixture&name=Example%20App");
  snapshot(GTK_WIDGET(window), "pairing-preview-360");
  AdwDialog *preview = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  GtkWidget *review = find_button(GTK_WIDGET(preview), _("_Review"));
  assert_header_control(review, TRUE); g_assert_false(gtk_widget_is_sensitive(review)); assert_dialog_mnemonics(preview);
  adw_dialog_close(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  WAIT(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  grotto_pairing_present(GTK_WIDGET(window), NULL); wait_frames(GTK_WIDGET(window));
  AdwDialog *paste_sheet = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  assert_header_control(find_button(GTK_WIDGET(paste_sheet), _("_Cancel")), FALSE); assert_dialog_mnemonics(paste_sheet);
  g_assert_nonnull(find_row(GTK_WIDGET(paste_sheet), _("Paste Connection Link")));
  g_assert_nonnull(find_row(GTK_WIDGET(paste_sheet), _("Scan QR Code…")));
  g_assert_nonnull(find_row(GTK_WIDGET(paste_sheet), _("Show My Connection Link")));
  g_assert_null(find_editor_control(GTK_WIDGET(paste_sheet), GTK_TYPE_ENTRY));
  g_assert_null(find_button(GTK_WIDGET(paste_sheet), _("_Preview")));
  adw_dialog_close(paste_sheet); WAIT(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
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
static void setup_snapshot(OnboardingAssistant *setup, const char *stage, gboolean rtl) {
  int minimum; gtk_widget_measure(GTK_WIDGET(setup), GTK_ORIENTATION_HORIZONTAL, -1, &minimum, NULL, NULL, NULL);
  g_test_message("Onboarding %s minimum width: %d", stage, minimum);
  assert_assistant_actions(setup, gtk_window_get_transient_for(GTK_WINDOW(setup)) != NULL);
  g_assert_cmpint(minimum, <=, 360);
  g_autofree char *name = g_strdup_printf("onboarding-%s-360%s", stage, rtl ? "-rtl" : "");
  snapshot(GTK_WIDGET(setup), name);
}
/* Exercise production navigation against the private-bus fixture. The recovery
 * phrase is the published test vector, never an operator's recovery material. */
static void remaining_surfaces(void) {
  adw_style_manager_set_color_scheme(adw_style_manager_get_default(), ADW_COLOR_SCHEME_FORCE_DARK);
  for (guint running = 0; running < 2; running++) for (guint rtl = 0; rtl < 2; rtl++) {
    gtk_widget_set_default_direction(rtl ? GTK_TEXT_DIR_RTL : GTK_TEXT_DIR_LTR);
    GtkWindow *parent = NULL;
    OnboardingAssistant *setup;
    if (running) {
      parent = GTK_WINDOW(adw_window_new()); g_object_ref_sink(parent); gtk_window_present(parent);
      onboarding_assistant_present(parent);
      setup = g_object_ref(g_object_get_data(G_OBJECT(parent), "grotto-onboarding-assistant"));
    } else {
      setup = onboarding_assistant_new(); g_object_ref_sink(setup);
    }
    g_test_message("Assistant HIG page walk, running=%u, rtl=%u", running, rtl);
    gtk_window_set_default_size(GTK_WINDOW(setup), 360, 640);
    gtk_window_present(GTK_WINDOW(setup));
    WAIT(gtk_widget_is_sensitive(setup_child(setup, "btn_next")));
    if (!running) {
      setup_snapshot(setup, "welcome", rtl);
      g_signal_emit_by_name(setup_child(setup, "btn_next"), "clicked");
    }
    setup_snapshot(setup, "choice", rtl);
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
    assistant_back(setup); /* Import returns to Connect, not a synthetic choice page. */
    g_assert_cmpint(onboarding_assistant_get_state(setup), ==, ONBOARDING_CONNECT);
    assistant_back(setup);
    g_signal_emit_by_name(setup_child(setup, "choose_create"), "activated"); setup_snapshot(setup, "create", rtl);
    g_signal_emit_by_name(setup_child(setup, "btn_next"), "clicked"); WAIT(layout_self_test != NULL);
    g_assert_cmpint(onboarding_assistant_get_state(setup), ==, ONBOARDING_TEST_WAITING); setup_snapshot(setup, "test", rtl);
    g_dbus_method_invocation_return_value(layout_self_test, g_variant_new("(s)", "passed")); g_clear_object(&layout_self_test);
    WAIT(onboarding_assistant_get_state(setup) == ONBOARDING_BACKUP);
    WAIT(gtk_widget_get_first_child(setup_child(setup, "seed_words")) != NULL);
    setup_snapshot(setup, "backup", rtl);
    g_assert_true(gtk_widget_get_visible(setup_child(setup, "btn_skip")));
    g_assert_null(gtk_widget_get_ancestor(setup_child(setup, "btn_skip"), GTK_TYPE_POPOVER));
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(setup_child(setup, "btn_skip"))), ==, _("_Cancel"));
    AdwExpanderRow *options = ADW_EXPANDER_ROW(gtk_widget_get_ancestor(setup_child(setup, "btn_copy_seed"), ADW_TYPE_EXPANDER_ROW));
    g_assert_nonnull(options); adw_expander_row_set_expanded(options, TRUE);
    g_autoptr(GHashTable) keys = g_hash_table_new(g_direct_hash, g_direct_equal);
    wait_frames(GTK_WIDGET(setup)); collect_dialog_mnemonics(GTK_WIDGET(setup), keys);
    adw_expander_row_set_expanded(options, FALSE);
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
    if (parent) {
      WAIT(g_object_get_data(G_OBJECT(parent), "grotto-onboarding-assistant") == NULL);
      gtk_window_destroy(parent); g_object_unref(parent);
    }
    gtk_widget_set_default_direction(GTK_TEXT_DIR_LTR);
  }
}
static void assistant_input_keyboard_activate(GApplication *application, gpointer data) {
  (void)data;
  /* Keep this scenario's modality separate from other test applications. */
  g_autoptr(GtkWindowGroup) group = gtk_window_group_new();
  const char *cancel_pages[] = {NULL, "choice", "choose_create", "choose_import", "choose_connect"};
  for (guint running = 0; running < 2; running++) for (guint stage = running ? 1 : 0; stage < G_N_ELEMENTS(cancel_pages); stage++) {
    GtkWindow *parent = NULL;
    OnboardingAssistant *setup;
    onboarding_assistant_reset();
    if (running) {
      parent = GTK_WINDOW(adw_application_window_new(GTK_APPLICATION(application))); g_object_ref_sink(parent);
      gtk_window_group_add_window(group, parent); gtk_window_present(parent);
      onboarding_assistant_present(parent);
      setup = g_object_ref(g_object_get_data(G_OBJECT(parent), "grotto-onboarding-assistant"));
    } else {
      setup = onboarding_assistant_new(); g_object_ref_sink(setup);
      gtk_window_set_application(GTK_WINDOW(setup), GTK_APPLICATION(application));
      gtk_window_present(GTK_WINDOW(setup));
    }
    gtk_window_group_add_window(group, GTK_WINDOW(setup));
    WAIT(gtk_widget_is_sensitive(setup_child(setup, "choose_create")));
    if (!running && stage) g_signal_emit_by_name(setup_child(setup, "btn_next"), "clicked");
    if (stage > 1) g_signal_emit_by_name(setup_child(setup, cancel_pages[stage]), "activated");
    wait_frames(GTK_WIDGET(setup)); assert_assistant_actions(setup, running);
    guint creates_before = identity_creates, imports_before = identity_imports;
#ifdef GROTTO_TEST_XDOTOOL
    if (stage == 2) {
      AdwAlertDialog *nested = ADW_ALERT_DIALOG(adw_alert_dialog_new(_("Check Your Identity"), NULL));
      adw_alert_dialog_add_response(nested, "cancel", _("_Cancel"));
      adw_alert_dialog_set_close_response(nested, "cancel");
      adw_alert_dialog_set_default_response(nested, "cancel");
      adw_dialog_present(ADW_DIALOG(nested), GTK_WIDGET(setup));
      WAIT(gtk_widget_get_mapped(GTK_WIDGET(nested)));
      wait_frames(GTK_WIDGET(nested));
      send_key_event(GTK_WINDOW(setup), "Escape");
      WAIT(adw_window_get_visible_dialog(ADW_WINDOW(setup)) == NULL);
      g_assert_true(gtk_widget_get_mapped(GTK_WIDGET(setup)));
      g_assert_cmpint(onboarding_assistant_get_state(setup), ==, ONBOARDING_CREATE);
    }
#endif
    g_test_message("Escape cancels page %u, running=%u", stage, running);
#ifdef GROTTO_TEST_XDOTOOL
    send_key_event(GTK_WINDOW(setup), "Escape");
    WAIT(!gtk_widget_get_mapped(GTK_WIDGET(setup)));
#else
    g_signal_emit_by_name(setup_child(setup, "btn_skip"), "clicked");
#endif
    g_assert_cmpuint(identity_creates, ==, creates_before);
    g_assert_cmpuint(identity_imports, ==, imports_before);
    g_assert_false(onboarding_assistant_identity_stored(setup));
    g_autoptr(GSettings) settings = g_settings_new("org.nostr.Grotto");
    g_assert_cmpint(g_settings_get_boolean(settings, "onboarding-completed"), ==, !running);
    gtk_window_destroy(GTK_WINDOW(setup)); g_object_unref(setup);
    if (parent) {
      WAIT(g_object_get_data(G_OBJECT(parent), "grotto-onboarding-assistant") == NULL);
      gtk_window_destroy(parent); g_object_unref(parent);
    }
  }
  const char *inputs[] = {"entry_profile_name", "entry_import_label", "text_import_data", "box_import_passphrase_container"};
  for (guint running = 0; running < 2; running++) for (guint mode = 0; mode < G_N_ELEMENTS(inputs); mode++) {
    GtkWindow *parent = NULL;
    OnboardingAssistant *setup;
    if (running) {
      parent = GTK_WINDOW(adw_application_window_new(GTK_APPLICATION(application))); g_object_ref_sink(parent);
      gtk_window_group_add_window(group, parent); gtk_window_present(parent);
      onboarding_assistant_present(parent);
      setup = g_object_ref(g_object_get_data(G_OBJECT(parent), "grotto-onboarding-assistant"));
      gtk_window_group_add_window(group, GTK_WINDOW(setup));
    } else {
      setup = onboarding_assistant_new(); g_object_ref_sink(setup);
      gtk_window_set_application(GTK_WINDOW(setup), GTK_APPLICATION(application));
      gtk_window_group_add_window(group, GTK_WINDOW(setup)); gtk_window_present(GTK_WINDOW(setup));
      WAIT(gtk_widget_is_sensitive(setup_child(setup, "btn_next")));
    }
    WAIT(gtk_widget_get_mapped(GTK_WIDGET(setup))); wait_frames(GTK_WIDGET(setup));
    if (!running) g_signal_emit_by_name(setup_child(setup, "btn_next"), "clicked");
    GtkWidget *choice = setup_child(setup, mode ? "choose_import" : "choose_create");
    WAIT(gtk_widget_is_sensitive(choice)); g_signal_emit_by_name(choice, "activated");
    wait_frames(GTK_WIDGET(setup));
    if (mode) gtk_editable_set_text(GTK_EDITABLE(setup_child(setup, "text_import_data")), mode == 3 ?
        "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about" :
        "0000000000000000000000000000000000000000000000000000000000000001");
    GtkWidget *input = setup_child(setup, inputs[mode]);
    if (mode == 3) {
      input = gtk_widget_get_first_child(input);
      gn_secure_entry_set_text(GN_SECURE_ENTRY(input), "Keyboard passphrase");
    }
    if (ADW_IS_ENTRY_ROW(input)) {
      gtk_editable_set_text(GTK_EDITABLE(input), "Keyboard Identity");
      g_assert_true(adw_entry_row_get_activates_default(ADW_ENTRY_ROW(input)));
    } else if (GTK_IS_ENTRY(input)) g_assert_true(gtk_entry_get_activates_default(GTK_ENTRY(input)));
    g_assert_true(gtk_widget_is_sensitive(setup_child(setup, "btn_next")));
    guint creates_before = identity_creates, imports_before = identity_imports;
    g_assert_true(gtk_window_get_default_widget(GTK_WINDOW(setup)) == setup_child(setup, "btn_next"));
    assert_assistant_actions(setup, running);
    g_test_message("Return input %s, running=%u", inputs[mode], running);
    return_from_input(GTK_WINDOW(setup), input);
    WAIT(layout_self_test != NULL);
#ifdef GROTTO_TEST_XDOTOOL
    send_key(GTK_WINDOW(setup), "Escape");
    g_assert_true(gtk_widget_get_mapped(GTK_WIDGET(setup)));
    g_assert_cmpint(onboarding_assistant_get_state(setup), ==, ONBOARDING_TEST_WAITING);
#endif
    g_assert_cmpuint(identity_creates, ==, creates_before + (mode == 0));
    g_assert_cmpuint(identity_imports, ==, imports_before + (mode != 0));
    g_dbus_method_invocation_return_value(layout_self_test, g_variant_new("(s)", mode ? "denied" : "passed"));
    g_clear_object(&layout_self_test);
    WAIT(onboarding_assistant_get_state(setup) == (mode ? ONBOARDING_TEST_FAILED : ONBOARDING_BACKUP));
    WAIT(gtk_widget_is_sensitive(setup_child(setup, "btn_skip")));
    WAIT(gtk_widget_get_mapped(setup_child(setup, "btn_skip")));
    assert_assistant_actions(setup, running);
    g_autoptr(GHashTable) keys = g_hash_table_new(g_direct_hash, g_direct_equal);
    collect_dialog_mnemonics(GTK_WIDGET(setup), keys);
    g_assert_cmpuint(g_hash_table_size(keys), ==, 2);
    g_assert_nonnull(gtk_widget_get_ancestor(setup_child(setup, "btn_next"), mode ? ADW_TYPE_STATUS_PAGE : ADW_TYPE_HEADER_BAR));
    guint imports_after = identity_imports, creates_after = identity_creates;
    g_object_ref(setup);
#ifdef GROTTO_TEST_XDOTOOL
    send_key_event(GTK_WINDOW(setup), "Escape");
    WAIT(!gtk_widget_get_mapped(GTK_WIDGET(setup)));
#else
    g_signal_emit_by_name(setup_child(setup, "btn_skip"), "clicked");
#endif
    g_assert_true(onboarding_assistant_identity_stored(setup));
    g_assert_cmpuint(identity_imports, ==, imports_after);
    g_assert_cmpuint(identity_creates, ==, creates_after);
    g_object_unref(setup);
    gtk_window_destroy(GTK_WINDOW(setup)); g_object_unref(setup);
    if (parent) {
      WAIT(g_object_get_data(G_OBJECT(parent), "grotto-onboarding-assistant") == NULL);
      gtk_window_destroy(parent); g_object_unref(parent);
    }
  }
  g_application_quit(application);
}
static void assistant_input_keyboard(void) {
  guint apps_gone = 0;
  g_autoptr(AdwApplication) app = adw_application_new("org.nostr.Grotto.AssistantKeyboardTest", G_APPLICATION_NON_UNIQUE);
  g_object_weak_ref(G_OBJECT(app), finalised, &apps_gone);
  g_signal_connect(app, "activate", G_CALLBACK(assistant_input_keyboard_activate), NULL);
  g_assert_cmpint(g_application_run(G_APPLICATION(app), 0, NULL), ==, 0);
  g_clear_object(&app); WAIT(apps_gone == 1);
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
static void connection_link_actions_activate(GApplication *application, gpointer data) {
  (void)data;
  accounts_store_add(accounts_store_get_default(), npub, "My identity", NULL);
  g_settings_set_string(settings_manager_get_gsettings(settings_manager_get_default()), "default-identity", npub);
  SignerWindow *window = signer_window_new(ADW_APPLICATION(application)); g_object_ref(window);
  gtk_window_set_default_size(GTK_WINDOW(window), 360, 640); gtk_window_present(GTK_WINDOW(window)); wait_frames(GTK_WIDGET(window));
  pairing_listening = FALSE; guint before = pairing_creates;
  g_action_group_activate_action(G_ACTION_GROUP(window), "pair-application", NULL); wait_frames(GTK_WIDGET(window));
  AdwDialog *add_app = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_nonnull(find_row(GTK_WIDGET(add_app), _("Paste Connection Link")));
  g_assert_nonnull(find_row(GTK_WIDGET(add_app), _("Scan QR Code…")));
  g_assert_null(find_row(GTK_WIDGET(add_app), _("Open Link File…")));
  g_assert_null(find_row(GTK_WIDGET(add_app), _("Remote Signing")));
  g_assert_null(find_button(GTK_WIDGET(add_app), _("Create _Link…")));
  GtkWidget *show = find_row(GTK_WIDGET(add_app), _("Show My Connection Link")); g_assert_nonnull(show);
  g_signal_emit_by_name(show, "activated"); wait_frames(GTK_WIDGET(window));
  AdwDialog *sheet = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)); g_assert_true(SHEET_IS_QR_DISPLAY(sheet));
  GtkWidget *enable = NULL; WAIT((enable = find_row(GTK_WIDGET(sheet), _("Enable Remote Signing"))) && gtk_widget_is_visible(enable));
  GtkWidget *picture = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(sheet), TYPE_SHEET_QR_DISPLAY, "qr_picture"));
  g_assert_false(gtk_widget_get_visible(picture));
  g_signal_emit_by_name(enable, "activated");
  WAIT(pairing_creates > before); g_assert_true(pairing_listening);
  GtkWidget *identity_choice = find_editor_control(GTK_WIDGET(sheet), ADW_TYPE_COMBO_ROW); g_assert_nonnull(identity_choice);
  g_assert_false(gtk_widget_get_visible(identity_choice));
  GtkWidget *copy = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(sheet), TYPE_SHEET_QR_DISPLAY, "btn_copy"));
  WAIT(gtk_widget_is_sensitive(copy));
  g_assert_true(gtk_widget_get_visible(picture));
  g_assert_cmpstr(adw_dialog_get_title(sheet), ==, _("Connection Link"));
  g_assert_null(find_editor_control(GTK_WIDGET(sheet), GTK_TYPE_ENTRY));
  g_test_message("Show My Connection Link uses the shared QR sheet without a separate create flow");
  pairing_listening = FALSE;
  adw_dialog_close(sheet); WAIT(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == add_app);
  wait_frames(GTK_WIDGET(window));
  adw_dialog_close(add_app); WAIT(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  grotto_pairing_present(GTK_WIDGET(window), NULL); wait_frames(GTK_WIDGET(window));
  AdwDialog *paste = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  GtkWidget *paste_row = find_row(GTK_WIDGET(paste), _("Paste Connection Link")); g_assert_nonnull(paste_row);
  g_assert_null(find_editor_control(GTK_WIDGET(paste), GTK_TYPE_ENTRY));
  gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(paste)), "nostrconnect://79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798?relay=wss%3A%2F%2Frelay.example&secret=fixture&name=Example%20App");
  g_signal_emit_by_name(paste_row, "activated");
  WAIT(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) != paste);
  wait_frames(GTK_WIDGET(window));
  g_test_message("Paste Connection Link opened the connection preview");
  AdwDialog *preview = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  g_assert_cmpstr(adw_dialog_get_title(preview), ==, _("Connect an App"));
  adw_dialog_close(preview); WAIT(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == paste);
  adw_dialog_close(paste); WAIT(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  guint8 other_key[32] = {0}; other_key[31] = 2;
  g_assert_cmpint(nostr_nip19_encode_npub(other_key, &pairing_other), ==, 0);
  pairing_multiple = TRUE; pairing_listening = TRUE;
  g_action_group_activate_action(G_ACTION_GROUP(window), "show-connection-link", NULL); wait_frames(GTK_WIDGET(window));
  sheet = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  identity_choice = find_editor_control(GTK_WIDGET(sheet), ADW_TYPE_COMBO_ROW);
  WAIT(adw_combo_row_get_model(ADW_COMBO_ROW(identity_choice)) && g_list_model_get_n_items(adw_combo_row_get_model(ADW_COMBO_ROW(identity_choice))) == 2);
  g_assert_true(gtk_widget_get_visible(identity_choice));
  adw_combo_row_set_selected(ADW_COMBO_ROW(identity_choice), 1);
  WAIT(!g_strcmp0(pairing_selected, pairing_other));
  adw_dialog_close(sheet); WAIT(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  g_action_group_activate_action(G_ACTION_GROUP(window), "show-connection-link", NULL); wait_frames(GTK_WIDGET(window));
  sheet = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  identity_choice = find_editor_control(GTK_WIDGET(sheet), ADW_TYPE_COMBO_ROW);
  WAIT(adw_combo_row_get_selected(ADW_COMBO_ROW(identity_choice)) == 1);
  adw_dialog_close(sheet); WAIT(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  pairing_multiple = FALSE; pairing_listening = FALSE; pairing_selected = NULL; free(pairing_other); pairing_other = NULL;
  gtk_window_destroy(GTK_WINDOW(window)); g_object_unref(window); g_application_quit(application);
}
static void connection_link_actions(void) {
  guint apps_gone = 0;
  g_autoptr(AdwApplication) app = adw_application_new("org.nostr.Grotto.CreateLinkKeyboardTest", G_APPLICATION_NON_UNIQUE);
  g_object_weak_ref(G_OBJECT(app), finalised, &apps_gone);
  g_signal_connect(app, "activate", G_CALLBACK(connection_link_actions_activate), NULL);
  g_assert_cmpint(g_application_run(G_APPLICATION(app), 0, NULL), ==, 0);
  g_clear_object(&app); WAIT(apps_gone == 1);
}
static void backup_file_keyboard_activate(GApplication *application, gpointer data) {
  (void)data;
  accounts_store_add(accounts_store_get_default(), npub, "My identity", NULL);
  g_settings_set_string(settings_manager_get_gsettings(settings_manager_get_default()), "default-identity", npub);
  SignerWindow *window = signer_window_new(ADW_APPLICATION(application)); g_object_ref(window);
  gtk_window_set_default_size(GTK_WINDOW(window), 360, 640); gtk_window_present(GTK_WINDOW(window)); wait_frames(GTK_WIDGET(window));
  signer_window_show_page(window, "backup"); wait_frames(GTK_WIDGET(window));
  GtkWidget *page = find_editor_control(GTK_WIDGET(window), GROTTO_TYPE_IDENTITY_DETAIL); g_assert_nonnull(page);
  GtkWidget *save = GTK_WIDGET(gtk_widget_get_template_child(page, GROTTO_TYPE_IDENTITY_DETAIL, "save_row"));
  WAIT(gtk_widget_is_sensitive(save)); g_signal_emit_by_name(save, "activated"); wait_frames(GTK_WIDGET(window));
  AdwDialog *dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  GtkWidget *entry = find_editor_control(GTK_WIDGET(dialog), GN_TYPE_SECURE_ENTRY); g_assert_nonnull(entry);
  GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(window)); g_assert_nonnull(focus);
  g_assert_true(gtk_widget_is_ancestor(focus, entry));
  guint before = encrypted_exports;
  gn_secure_entry_set_text(GN_SECURE_ENTRY(entry), "disposable passphrase"); return_from_input(GTK_WINDOW(window), entry);
  g_autoptr(GtkWindow) chooser = NULL; WAIT((chooser = find_file_chooser()) != NULL);
  g_assert_cmpuint(encrypted_exports, ==, before);
  g_assert_true(GTK_IS_DIALOG(chooser)); gtk_dialog_response(GTK_DIALOG(chooser), GTK_RESPONSE_CANCEL);
  WAIT(gtk_widget_is_sensitive(save));
  gtk_window_destroy(chooser); g_clear_object(&chooser);
  wait_frames(GTK_WIDGET(window));
  adw_dialog_close(dialog); WAIT(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
  gtk_window_destroy(GTK_WINDOW(window)); g_object_unref(window); g_application_quit(application);
}
static void backup_file_keyboard(void) {
  guint apps_gone = 0;
  g_autoptr(AdwApplication) app = adw_application_new("org.nostr.Grotto.BackupFileKeyboardTest", G_APPLICATION_NON_UNIQUE);
  g_object_weak_ref(G_OBJECT(app), finalised, &apps_gone);
  g_signal_connect(app, "activate", G_CALLBACK(backup_file_keyboard_activate), NULL);
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
void grotto_test_connection_listening(gboolean listening) {
  pairing_listening = listening;
  g_assert_true(g_dbus_connection_emit_signal(server, NULL, "/org/nostr/signer/grotto",
    "org.nostr.Grotto.Control1", "StatusChanged", g_variant_new("(t)", revision), NULL));
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
    g_assert_cmpstr(_("Show _QR Code"), ==, "Mostrar _QR");
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
  if (g_getenv("GROTTO_TEST_DIALOG_SIZING")) {
    grotto_test_add_dialog_sizing();
  } else {
  g_test_add_func("/signer/layout/real-templates-navigation-focus-screenshots", layout);
  g_test_add_func("/signer/layout/remaining-surfaces", remaining_surfaces);
  g_test_add_func("/signer/layout/first-run-connect-destination", first_run_connect_destination);
  g_test_add_func("/signer/layout/connection-link-actions", connection_link_actions);
  g_test_add_func("/signer/layout/backup-file-keyboard", backup_file_keyboard);
  g_test_add_func("/signer/layout/assistant-input-keyboard", assistant_input_keyboard);
  }
  int result = g_test_run();
  bunker_test_relay_free(profile_relay);
  accounts_store_free(accounts_store_get_default());
  g_dbus_connection_unregister_object(server, registration); g_dbus_connection_unregister_object(server, inventory); nostrc_test_bus_down(bus);
  /* Inventory callbacks drained by bus shutdown still borrow these settings. */
  settings_manager_free(settings_manager_get_default());
  g_variant_unref(permission_rows); g_free(npub); return result;
}
