#include "ui/onboarding-assistant.h"
#include "ui/page-identity-detail.h"
#include "backup-recovery.h"
#include "accounts_store.h"
#include "secret-clipboard.h"
#include "secure-mem.h"
#include "nostrc-test-gdk-frame.h"
#include "tests/common/nostrc-test-bus.h"
#include <string.h>

typedef struct {
  OnboardingAssistant *assistant;
  GDBusMethodInvocation *store, *test, *seed;
  guint finished, cancel_calls;
  gboolean verified;
  gchar *npub, *phrase;
} Fixture;
static Fixture *active;
static GDBusConnection *server;
static gchar *stored_identity, *inventory_first;
static gboolean supports_management;
static gboolean bound(gpointer data) { (void)data; g_error("Assistant did not complete"); return G_SOURCE_REMOVE; }
static void wait_invocation(GDBusMethodInvocation **inv) {
  guint timeout = g_timeout_add_seconds(10, bound, NULL);
  while (!*inv) g_main_context_iteration(NULL, TRUE);
  g_source_remove(timeout);
}
static void wait_state(Fixture *f, OnboardingState state) {
  guint timeout = g_timeout_add_seconds(10, bound, NULL);
  while (onboarding_assistant_get_state(f->assistant) != state) g_main_context_iteration(NULL, TRUE);
  g_source_remove(timeout);
}
static GtkWidget *child(Fixture *f, const gchar *name) {
  GtkWidget *widget = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(f->assistant), TYPE_ONBOARDING_ASSISTANT, name));
  g_assert_nonnull(widget); return widget;
}
static void click(Fixture *f, const gchar *name) { g_signal_emit_by_name(child(f, name), "clicked"); }
static void finish_cb(gboolean verified, gpointer data) { Fixture *f = data; f->finished++; f->verified = verified; }
static void mocked_io(GDBusConnection *bus, const gchar *sender, const gchar *path, const gchar *interface,
    const gchar *method, GVariant *args, GDBusMethodInvocation *inv, gpointer data) {
  (void)bus; (void)sender; (void)path; (void)interface; (void)data;
  if (!strcmp(method, "GetStatus")) {
    GVariantBuilder caps, row; g_variant_builder_init(&caps, G_VARIANT_TYPE("a{sb}"));
    g_variant_builder_add(&caps, "{sb}", "store-mnemonic", supports_management);
    g_variant_builder_add(&caps, "{sb}", "identity-self-test", supports_management);
    g_variant_builder_add(&caps, "{sb}", "mnemonic-export", supports_management);
    g_variant_builder_init(&row, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&row, "{sv}", "control_version", g_variant_new_uint32(1));
    g_variant_builder_add(&row, "{sv}", "capabilities", g_variant_builder_end(&caps));
    g_dbus_method_invocation_return_value(inv, g_variant_new("(@a{sv})", g_variant_builder_end(&row))); return;
  }
  if (!strcmp(method, "ListIdentities")) {
    const gchar *ids[] = {inventory_first ? inventory_first : stored_identity, inventory_first ? stored_identity : NULL, NULL};
    g_dbus_method_invocation_return_value(inv, g_variant_new("(^as)", ids)); return;
  }
  if (!strcmp(method, "CancelIdentitySelfTest")) {
    const gchar *identity; g_variant_get(args, "(&s)", &identity);
    if (active && !g_strcmp0(identity, active->npub)) {
      active->cancel_calls++;
      if (active->test) {
        g_dbus_method_invocation_return_dbus_error(active->test, "org.nostr.Signer.Error.Cancelled", "cancelled"); g_clear_object(&active->test);
      }
    }
    g_dbus_method_invocation_return_value(inv, NULL); return;
  }
  g_assert_nonnull(active);
  if (!strcmp(method, "StoreKeyWithMnemonic")) {
    const gchar *key, *label, *phrase; g_variant_get(args, "(&s&s&s)", &key, &label, &phrase);
    g_assert_true(gn_backup_get_npub(key, &active->npub, NULL));
    gchar *derived = NULL; g_assert_true(gn_backup_import_mnemonic(phrase, "", 0, &derived, NULL));
    g_assert_cmpstr(key, ==, derived); gnostr_secure_clear(derived, strlen(derived)); g_free(derived);
    active->phrase = g_strdup(phrase); active->store = g_object_ref(inv);
  } else if (!strcmp(method, "ImportIdentity")) {
    const gchar *format, *input, *pass, *label; g_variant_get(args, "(&s&s&s&s)", &format, &input, &pass, &label);
    g_assert_cmpstr(format, ==, "key"); g_assert_cmpstr(pass, ==, "");
    g_assert_true(gn_backup_get_npub(input, &active->npub, NULL)); active->store = g_object_ref(inv);
  } else if (!strcmp(method, "SelfTestIdentity")) {
    const gchar *identity; g_variant_get(args, "(&s)", &identity); g_assert_cmpstr(identity, ==, active->npub);
    active->test = g_object_ref(inv);
  } else if (!strcmp(method, "ExportIdentity")) {
    const gchar *identity, *format, *pass; g_variant_get(args, "(&s&s&s)", &identity, &format, &pass);
    g_assert_cmpstr(identity, ==, active->npub); g_assert_cmpstr(format, ==, "mnemonic"); g_assert_cmpstr(pass, ==, "");
    active->seed = g_object_ref(inv);
  } else g_assert_not_reached(); /* no CreateProfile/ImportMnemonic/ImportNip49 */
}
static const GDBusInterfaceVTable vtable = {mocked_io, NULL, NULL, {0}};
static void setup(Fixture *f, gconstpointer data) {
  active = f; supports_management = g_strcmp0(data, "older") != 0;
  g_clear_pointer(&stored_identity, g_free);
  f->assistant = onboarding_assistant_new(); g_object_ref(f->assistant);
  onboarding_assistant_set_on_finished(f->assistant, finish_cb, f);
  gtk_window_present(GTK_WINDOW(f->assistant));
  guint timeout = g_timeout_add_seconds(10, bound, NULL);
  while (!gtk_widget_get_mapped(GTK_WIDGET(f->assistant)) || !gtk_widget_get_sensitive(child(f, "btn_skip")))
    g_main_context_iteration(NULL, TRUE);
  g_source_remove(timeout);
}
static void weak_gone(gpointer data, GObject *object);
static void teardown(Fixture *f, gconstpointer data) {
  (void)data; guint gone = 0;
  gboolean had_assistant = f->assistant != NULL;
  if (f->assistant) {
    g_object_weak_ref(G_OBJECT(f->assistant), weak_gone, &gone);
    onboarding_assistant_set_on_finished(f->assistant, NULL, NULL);
    gtk_window_close(GTK_WINDOW(f->assistant));
    gtk_window_destroy(GTK_WINDOW(f->assistant)); g_object_unref(f->assistant); f->assistant = NULL;
  }
  GDBusMethodInvocation **pending[] = {&f->store, &f->test, &f->seed};
  for (guint i = 0; i < G_N_ELEMENTS(pending); i++) if (*pending[i]) {
    g_dbus_method_invocation_return_dbus_error(*pending[i], "org.nostr.Signer.Error.Cancelled", "test cleanup"); g_clear_object(pending[i]);
  }
  guint timeout = g_timeout_add_seconds(10, bound, NULL);
  while (had_assistant && !gone) g_main_context_iteration(NULL, TRUE);
  g_source_remove(timeout);
  if (f->phrase) { gnostr_secure_clear(f->phrase, strlen(f->phrase)); g_free(f->phrase); }
  g_free(f->npub); active = NULL;
}
static void create_waiting(Fixture *f) {
  click(f, "btn_next"); click(f, "btn_next"); g_assert_cmpint(onboarding_assistant_get_state(f->assistant), ==, ONBOARDING_CREATE);
  /* No creation password or meter exists in the production template. */
  g_assert_null(gtk_widget_get_template_child(GTK_WIDGET(f->assistant), TYPE_ONBOARDING_ASSISTANT, "entry_passphrase"));
  click(f, "btn_next"); wait_invocation(&f->store);
}
static void stored(Fixture *f) {
  g_free(stored_identity); stored_identity = g_strdup(f->npub);
  g_dbus_method_invocation_return_value(f->store, g_variant_new("(s)", f->npub)); g_clear_object(&f->store);
  wait_invocation(&f->test);
  g_assert_true(onboarding_assistant_identity_stored(f->assistant));
  g_assert_cmpstr(onboarding_assistant_get_identity(f->assistant), ==, f->npub);
}
static void delayed_commit(Fixture *f, gconstpointer data) {
  (void)data; create_waiting(f);
  g_assert_false(onboarding_assistant_identity_stored(f->assistant));
  g_assert_false(gtk_widget_get_sensitive(child(f, "btn_back"))); g_assert_false(gtk_widget_get_sensitive(child(f, "btn_skip")));
  click(f, "btn_back"); click(f, "btn_skip"); g_assert_cmpuint(f->finished, ==, 0);
  g_assert_cmpint(onboarding_assistant_get_state(f->assistant), ==, ONBOARDING_CREATE);
  stored(f); g_assert_false(onboarding_assistant_self_test_passed(f->assistant));
}
static void weak_gone(gpointer data, GObject *object) { (void)object; (*(guint *)data)++; }
static void close_during_commit(Fixture *f, gconstpointer data) {
  (void)data; create_waiting(f); guint gone = 0;
  g_object_weak_ref(G_OBJECT(f->assistant), weak_gone, &gone);
  gtk_window_close(GTK_WINDOW(f->assistant)); g_object_unref(f->assistant); f->assistant = NULL;
  /* The late store really succeeded in the daemon; UI cancellation must not delete it. */
  g_free(stored_identity); stored_identity = g_strdup(f->npub);
  g_dbus_method_invocation_return_value(f->store, g_variant_new("(s)", f->npub)); g_clear_object(&f->store);
  guint timeout = g_timeout_add_seconds(10, bound, NULL); while (!gone) g_main_context_iteration(NULL, TRUE); g_source_remove(timeout);
  g_assert_cmpuint(f->finished, ==, 0); g_assert_cmpstr(stored_identity, ==, f->npub);
}
static void successful_seed_cleanup(Fixture *f, gconstpointer data) {
  (void)data; create_waiting(f); stored(f);
  g_dbus_connection_emit_signal(server, g_dbus_method_invocation_get_sender(f->test), "/org/nostr/signer/grotto",
      "org.nostr.Grotto.Control1", "IdentitySelfTestTesting", g_variant_new("(s)", f->npub), NULL);
  wait_state(f, ONBOARDING_TESTING);
  g_dbus_method_invocation_return_value(f->test, g_variant_new("(s)", "passed")); g_clear_object(&f->test);
  wait_invocation(&f->seed); g_dbus_method_invocation_return_value(f->seed, g_variant_new("(s)", f->phrase)); g_clear_object(&f->seed);
  guint timeout = g_timeout_add_seconds(10, bound, NULL);
  while (!*gtk_label_get_text(GTK_LABEL(child(f, "seed_words")))) g_main_context_iteration(NULL, TRUE); g_source_remove(timeout);
  g_assert_true(onboarding_assistant_self_test_passed(f->assistant));
  gtk_check_button_set_active(GTK_CHECK_BUTTON(child(f, "seed_written_down")), TRUE); click(f, "btn_next");
  g_assert_cmpint(onboarding_assistant_get_state(f->assistant), ==, ONBOARDING_READY);
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(child(f, "seed_words"))), ==, "");
  AccountEntry *entry = accounts_store_find(accounts_store_get_default(), f->npub); g_assert_nonnull(entry);
  g_assert_cmpstr(entry->setup_state, ==, "ready"); accounts_store_entry_free(entry);
}
static void failed_test(Fixture *f, gconstpointer data) {
  const gchar *code = data; create_waiting(f); stored(f);
  g_dbus_method_invocation_return_dbus_error(f->test, code, "test outcome"); g_clear_object(&f->test);
  wait_state(f, ONBOARDING_TEST_FAILED); g_assert_false(onboarding_assistant_self_test_passed(f->assistant));
  OnboardingTestResult expected = g_str_has_suffix(code, "ApprovalDenied") ? ONBOARDING_TEST_DENIED :
    g_str_has_suffix(code, "ApprovalTimedOut") ? ONBOARDING_TEST_EXPIRED :
    g_str_has_suffix(code, "VerificationFailed") ? ONBOARDING_TEST_VERIFICATION_FAILED : ONBOARDING_TEST_KEYRING_UNAVAILABLE;
  g_assert_cmpint(onboarding_assistant_get_test_result(f->assistant), ==, expected);
  g_assert_true(gtk_widget_get_visible(child(f, "btn_retry"))); click(f, "btn_retry"); wait_invocation(&f->test);
  g_assert_cmpint(onboarding_assistant_get_state(f->assistant), ==, ONBOARDING_TEST_WAITING);
}
static void cancelled_test(Fixture *f, gconstpointer data) {
  (void)data; create_waiting(f); stored(f); gtk_window_close(GTK_WINDOW(f->assistant));
  guint timeout = g_timeout_add_seconds(10, bound, NULL);
  while (!f->cancel_calls) g_main_context_iteration(NULL, TRUE); g_source_remove(timeout);
  g_assert_cmpstr(stored_identity, ==, f->npub); g_assert_cmpuint(f->finished, ==, 0);
}
static void imported_input_cleared(Fixture *f, gconstpointer data) {
  (void)data; click(f, "btn_next"); gtk_check_button_set_active(GTK_CHECK_BUTTON(child(f, "radio_import")), TRUE); click(f, "btn_next");
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(child(f, "text_import_data")));
  gtk_text_buffer_set_text(buffer, "0000000000000000000000000000000000000000000000000000000000000002", -1);
  click(f, "btn_next"); wait_invocation(&f->store); g_assert_cmpint(gtk_text_buffer_get_char_count(buffer), ==, 0);
  stored(f); g_assert_cmpint(gtk_text_buffer_get_char_count(buffer), ==, 0);
}
static void readonly_older_daemon(Fixture *f, gconstpointer data) {
  (void)data; g_assert_false(gtk_widget_get_sensitive(child(f, "btn_next")));
  click(f, "btn_next"); g_assert_cmpint(onboarding_assistant_get_state(f->assistant), ==, ONBOARDING_WELCOME);
  g_assert_false(onboarding_assistant_identity_stored(f->assistant)); g_assert_null(f->store);
  click(f, "btn_skip"); g_assert_cmpuint(f->finished, ==, 1); g_assert_false(f->verified);
}
static void pinned_backup_cleanup(Fixture *f, gconstpointer data) {
  (void)data;
  g_assert_true(gn_backup_get_npub("0000000000000000000000000000000000000000000000000000000000000001", &f->npub, NULL));
  g_assert_true(gn_backup_get_npub("0000000000000000000000000000000000000000000000000000000000000002", &inventory_first, NULL));
  stored_identity = g_strdup(f->npub);
  GtkWidget *page = page_identity_detail_new(f->npub); g_object_ref_sink(page);
  GtkWindow *window = GTK_WINDOW(gtk_window_new()); g_object_ref(window);
  gtk_window_set_child(window, page); gtk_window_present(window);
  GtkWidget *seed = GTK_WIDGET(gtk_widget_get_template_child(page, GROTTO_TYPE_IDENTITY_DETAIL, "btn_seed"));
  GtkWidget *public_key = GTK_WIDGET(gtk_widget_get_template_child(page, GROTTO_TYPE_IDENTITY_DETAIL, "public_key"));
  GtkWidget *words = GTK_WIDGET(gtk_widget_get_template_child(page, GROTTO_TYPE_IDENTITY_DETAIL, "seed_words"));
  guint timeout = g_timeout_add_seconds(10, bound, NULL);
  while (!gtk_widget_get_sensitive(seed)) g_main_context_iteration(NULL, TRUE);
  g_source_remove(timeout);
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(public_key)), ==, f->npub);
  g_signal_emit_by_name(seed, "clicked"); wait_invocation(&f->seed);
  g_dbus_method_invocation_return_value(f->seed, g_variant_new("(s)", "disposable seed words")); g_clear_object(&f->seed);
  timeout = g_timeout_add_seconds(10, bound, NULL);
  while (!*gtk_label_get_text(GTK_LABEL(words))) g_main_context_iteration(NULL, TRUE);
  g_source_remove(timeout);
  gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(words)), ==, "");
  gtk_window_destroy(window); g_object_unref(window); g_object_unref(page);
  g_clear_pointer(&inventory_first, g_free);
}
static void clipboard_ownership(void) {
  GdkClipboard *clipboard = gdk_display_get_clipboard(gdk_display_get_default());
  guint timer = grotto_secret_clipboard_copy(clipboard, "disposable phrase");
  gdk_clipboard_set_text(clipboard, "user's unrelated content");
  GdkContentProvider *provider = g_object_ref(gdk_clipboard_get_content(clipboard));
  GSource *source = g_main_context_find_source_by_id(NULL, timer); g_assert_nonnull(source); g_source_set_ready_time(source, 0);
  while (g_main_context_find_source_by_id(NULL, timer)) g_main_context_iteration(NULL, TRUE);
  g_assert_true(gdk_clipboard_get_content(clipboard) == provider); g_object_unref(provider);
  timer = grotto_secret_clipboard_copy(clipboard, "another disposable phrase");
  source = g_main_context_find_source_by_id(NULL, timer); g_source_set_ready_time(source, 0);
  while (g_main_context_find_source_by_id(NULL, timer)) g_main_context_iteration(NULL, TRUE);
  g_assert_null(gdk_clipboard_get_content(clipboard));
}
int main(int argc, char **argv) {
#ifdef __APPLE__
  if (g_getenv("GNOSTR_SKIP_UI_TESTS")) return 77;
#endif
  g_test_init(&argc, &argv, NULL); nostrc_test_tolerate_gdk_frame_warning(); nostrc_test_bus_tolerate_ebadf();
  /* Portal integration is tested separately; its global settings proxy must
   * not attach to either the outer gate bus or this mock signer's bus. */
  g_setenv("ADW_DISABLE_PORTAL", "1", TRUE);
  NostrcTestBus *bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE); nostrc_test_bus_up(bus); server = nostrc_test_bus_connect(bus);
  GDBusNodeInfo *info = g_dbus_node_info_new_for_xml("<node><interface name='org.nostr.Signer'><method name='ListIdentities'><arg type='as' direction='out'/></method></interface><interface name='org.nostr.Grotto.Control1'><method name='GetStatus'><arg type='a{sv}' direction='out'/></method><method name='StoreKeyWithMnemonic'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='out'/></method><method name='ImportIdentity'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='out'/></method><method name='SelfTestIdentity'><arg type='s' direction='in'/><arg type='s' direction='out'/></method><method name='CancelIdentitySelfTest'><arg type='s' direction='in'/></method><method name='ExportIdentity'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='out'/></method><signal name='IdentitySelfTestTesting'><arg type='s'/></signal></interface></node>", NULL);
  guint legacy = g_dbus_connection_register_object(server, "/org/nostr/signer", info->interfaces[0], &vtable, NULL, NULL, NULL);
  guint control = g_dbus_connection_register_object(server, "/org/nostr/signer/grotto", info->interfaces[1], &vtable, NULL, NULL, NULL);
  g_autoptr(GVariant) name = g_dbus_connection_call_sync(server, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", "org.nostr.Signer", 0), NULL, 0, 5000, NULL, NULL);
  gtk_init(); adw_init(); g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
#define ADD(name, data, fn) g_test_add("/onboarding/" name, Fixture, data, setup, fn, teardown)
  ADD("older-daemon-readonly", "older", readonly_older_daemon);
  ADD("delayed-storage", NULL, delayed_commit); ADD("close-during-storage", NULL, close_during_commit);
  ADD("seed-cleanup", NULL, successful_seed_cleanup); ADD("denied", "org.nostr.Signer.Error.ApprovalDenied", failed_test);
  ADD("expired", "org.nostr.Signer.Error.ApprovalTimedOut", failed_test); ADD("keyring-unavailable", "org.nostr.Signer.Error.BackendUnavailable", failed_test);
  ADD("verification-failed", "org.nostr.Signer.Error.VerificationFailed", failed_test);
  ADD("close-during-self-test", NULL, cancelled_test); ADD("import-input-cleanup", NULL, imported_input_cleared);
  ADD("pinned-backup-cleanup", NULL, pinned_backup_cleanup);
#undef ADD
  g_test_add_func("/onboarding/clipboard-ownership", clipboard_ownership);
  int result = g_test_run(); accounts_store_free(accounts_store_get_default()); g_dbus_connection_unregister_object(server, legacy); g_dbus_connection_unregister_object(server, control);
  g_dbus_node_info_unref(info); nostrc_test_bus_down(bus); g_free(stored_identity); return result;
}
