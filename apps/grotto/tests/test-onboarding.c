#include "ui/onboarding-assistant.h"
#include "ui/page-identity-detail.h"
#include "ui/sheets/sheet-select-account.h"
#include "ui/widgets/gn-secure-entry.h"
#include "secret-variant.h"
#include "backup-recovery.h"
#include "accounts_store.h"
#include "secret-clipboard.h"
#include "secure-mem.h"
#include "secure-memory.h"
#include "nostrc-test-gdk-frame.h"
#include "tests/common/nostrc-test-bus.h"
#include <string.h>
#include <glib/gstdio.h>
#include <sys/stat.h>

typedef struct {
  OnboardingAssistant *assistant;
  GDBusMethodInvocation *store, *test, *seed, *encrypted;
  guint finished, cancel_calls, remove_calls;
  const gchar *expected_format, *expected_pass;
  gboolean remove_failure;
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
    g_variant_builder_add(&caps, "{sb}", "typed-identity-removal", supports_management);
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
    g_assert_cmpstr(format, ==, active->expected_format ? active->expected_format : "key");
    g_assert_cmpstr(pass, ==, active->expected_pass ? active->expected_pass : "");
    if (!strcmp(format, "key")) g_assert_true(gn_backup_get_npub(input, &active->npub, NULL));
    else if (!strcmp(format, "mnemonic")) {
      gchar *derived = NULL; g_assert_true(gn_backup_import_mnemonic(input, pass, 0, &derived, NULL));
      g_assert_true(gn_backup_get_npub(derived, &active->npub, NULL)); gnostr_secure_clear(derived, strlen(derived)); g_free(derived);
    } else { g_assert_true(g_str_has_prefix(input, "ncryptsec1")); g_assert_true(gn_backup_get_npub("0000000000000000000000000000000000000000000000000000000000000002", &active->npub, NULL)); }
    active->store = g_object_ref(inv);
  } else if (!strcmp(method, "SelfTestIdentity")) {
    const gchar *identity; g_variant_get(args, "(&s)", &identity); g_assert_cmpstr(identity, ==, active->npub);
    active->test = g_object_ref(inv);
  } else if (!strcmp(method, "ExportIdentity")) {
    const gchar *identity, *format, *pass; g_variant_get(args, "(&s&s&s)", &identity, &format, &pass);
    g_assert_cmpstr(identity, ==, active->npub);
    if (!strcmp(format, "mnemonic")) { g_assert_cmpstr(pass, ==, ""); active->seed = g_object_ref(inv); }
    else { g_assert_cmpstr(format, ==, "ncryptsec"); g_assert_cmpstr(pass, ==, active->expected_pass); active->encrypted = g_object_ref(inv); }
  } else if (!strcmp(method, "RemoveIdentity")) {
    const gchar *identity, *typed; g_variant_get(args, "(&s&s)", &identity, &typed);
    g_assert_cmpstr(identity, ==, active->npub); g_assert_cmpstr(typed, ==, identity); active->remove_calls++;
    if (active->remove_failure) g_dbus_method_invocation_return_dbus_error(inv, "org.nostr.Grotto.Error.IdentityBusy", "request pending");
    else { g_clear_pointer(&stored_identity, g_free); g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", TRUE)); }
  } else g_assert_not_reached(); /* no ClearKey/CreateProfile/ImportMnemonic/ImportNip49 */
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
  GDBusMethodInvocation **pending[] = {&f->store, &f->test, &f->seed, &f->encrypted};
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
  while (!gtk_widget_get_first_child(child(f, "seed_words"))) g_main_context_iteration(NULL, TRUE); g_source_remove(timeout);
  g_assert_true(onboarding_assistant_self_test_passed(f->assistant));
  gtk_check_button_set_active(GTK_CHECK_BUTTON(child(f, "seed_written_down")), TRUE); click(f, "btn_next");
  g_assert_cmpint(onboarding_assistant_get_state(f->assistant), ==, ONBOARDING_READY);
  g_assert_null(gtk_widget_get_first_child(child(f, "seed_words")));
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
  GtkEditable *input = GTK_EDITABLE(child(f, "text_import_data"));
  gtk_editable_set_text(input, "  0000000000000000000000000000000000000000000000000000000000000002  ");
  click(f, "btn_next"); wait_invocation(&f->store); g_assert_cmpstr(gtk_editable_get_text(input), ==, "");
  stored(f); g_assert_cmpstr(gtk_editable_get_text(input), ==, "");
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
  while (!gtk_widget_get_first_child(words)) g_main_context_iteration(NULL, TRUE);
  g_source_remove(timeout);
  gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
  g_assert_null(gtk_widget_get_first_child(words));
  gtk_window_destroy(window); g_object_unref(window); g_object_unref(page);
  g_clear_pointer(&inventory_first, g_free);
}
static GtkWidget *detail_child(GtkWidget *page, const gchar *name) {
  GtkWidget *widget = GTK_WIDGET(gtk_widget_get_template_child(page, GROTTO_TYPE_IDENTITY_DETAIL, name));
  g_assert_nonnull(widget); return widget;
}
static void wait_sensitive(GtkWidget *widget, gboolean value) {
  guint timeout = g_timeout_add_seconds(10, bound, NULL);
  while (gtk_widget_get_sensitive(widget) != value) g_main_context_iteration(NULL, TRUE);
  g_source_remove(timeout);
}
static void backup_failure_blocks_ready(Fixture *f, gconstpointer data) {
  (void)data; create_waiting(f); stored(f);
  g_dbus_method_invocation_return_value(f->test, g_variant_new("(s)", "passed")); g_clear_object(&f->test); wait_invocation(&f->seed);
  g_dbus_method_invocation_return_dbus_error(f->seed, "org.nostr.Grotto.Error.BackendUnavailable", "locked"); g_clear_object(&f->seed);
  wait_sensitive(child(f, "btn_backup_retry"), TRUE);
  gtk_check_button_set_active(GTK_CHECK_BUTTON(child(f, "seed_written_down")), TRUE); click(f, "btn_next");
  g_assert_cmpint(onboarding_assistant_get_state(f->assistant), ==, ONBOARDING_BACKUP);
  g_assert_false(gtk_widget_get_sensitive(child(f, "btn_next")));
  AccountEntry *entry = accounts_store_find(accounts_store_get_default(), f->npub);
  g_assert_cmpstr(entry->setup_state, ==, "backup-pending"); accounts_store_entry_free(entry);
  click(f, "btn_backup_retry"); wait_invocation(&f->seed);
  g_dbus_method_invocation_return_value(f->seed, g_variant_new("(s)", f->phrase)); g_clear_object(&f->seed);
  wait_sensitive(child(f, "seed_written_down"), TRUE);
  gtk_check_button_set_active(GTK_CHECK_BUTTON(child(f, "seed_written_down")), TRUE); click(f, "btn_next");
  g_assert_cmpint(onboarding_assistant_get_state(f->assistant), ==, ONBOARDING_READY);
}
static void management_warning_handoff(Fixture *f, gconstpointer data) {
  const gchar *mode = data; create_waiting(f); stored(f);
  gboolean backup = !g_strcmp0(mode, "backup");
  if (backup) {
    g_dbus_method_invocation_return_value(f->test, g_variant_new("(s)", "passed")); g_clear_object(&f->test); wait_invocation(&f->seed);
    g_dbus_method_invocation_return_dbus_error(f->seed, "org.nostr.Grotto.Error.BackendUnavailable", "locked"); g_clear_object(&f->seed);
    wait_sensitive(child(f, "btn_backup_retry"), TRUE);
  } else {
    g_dbus_method_invocation_return_dbus_error(f->test, "org.nostr.Signer.Error.ApprovalDenied", "denied"); g_clear_object(&f->test);
    wait_state(f, ONBOARDING_TEST_FAILED);
  }
  AdwWindow *window = ADW_WINDOW(adw_window_new()); g_object_ref(window); gtk_window_set_default_size(GTK_WINDOW(window), 560, 640); gtk_window_present(GTK_WINDOW(window));
  gtk_window_set_transient_for(GTK_WINDOW(f->assistant), GTK_WINDOW(window));
  if (backup) click(f, "btn_skip"); else click(f, "btn_next");
  g_assert_false(f->verified); g_assert_cmpuint(f->finished, ==, 1);
  AdwDialog *dialog = adw_window_get_visible_dialog(window); g_assert_nonnull(dialog); g_object_ref(dialog);
  GtkWidget *page = GTK_WIDGET(adw_navigation_view_get_visible_page(ADW_NAVIGATION_VIEW(adw_dialog_get_child(dialog))));
  wait_sensitive(detail_child(page, "btn_test"), TRUE);
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(detail_child(page, "public_key"))), ==, f->npub);
  g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(detail_child(page, "setup_warning"))), backup ? "Backup incomplete" : "Identity not verified"));
  g_assert_true(gtk_widget_get_sensitive(detail_child(page, "btn_export"))); /* maintenance still available */
  adw_dialog_force_close(dialog); g_object_unref(dialog); gtk_window_destroy(GTK_WINDOW(window)); g_object_unref(window);
}
static GtkWidget *button_named(GtkWidget *widget, const gchar *label) {
  if (GTK_IS_BUTTON(widget) && !g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), label)) return widget;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = button_named(child, label); if (found) return found;
  }
  return NULL;
}
static void alert_response(AdwAlertDialog *dialog, const gchar *response) {
  GtkWidget *button = button_named(GTK_WIDGET(dialog), adw_alert_dialog_get_response_label(dialog, response));
  g_assert_nonnull(button); g_assert_true(gtk_widget_get_sensitive(button)); g_signal_emit_by_name(button, "clicked");
}
static void typed_deletion_cancel_failure(Fixture *f, gconstpointer data) {
  (void)data; g_assert_true(gn_backup_get_npub("0000000000000000000000000000000000000000000000000000000000000002", &f->npub, NULL));
  stored_identity = g_strdup(f->npub);
  AdwWindow *window = ADW_WINDOW(adw_window_new()); g_object_ref(window); gtk_window_set_default_size(GTK_WINDOW(window), 560, 640);
  GtkWidget *page = page_identity_detail_new(f->npub); adw_window_set_content(window, page); gtk_window_present(GTK_WINDOW(window));
  GtkWidget *remove = detail_child(page, "btn_remove"); wait_sensitive(remove, TRUE);
  g_signal_emit_by_name(remove, "clicked");
  AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_window_get_visible_dialog(window)); g_assert_nonnull(dialog); g_object_ref(dialog);
  g_assert_false(adw_alert_dialog_get_response_enabled(dialog, "proceed"));
  gtk_editable_set_text(GTK_EDITABLE(adw_alert_dialog_get_extra_child(dialog)), "wrong identity");
  g_assert_false(adw_alert_dialog_get_response_enabled(dialog, "proceed"));
  alert_response(dialog, "cancel"); g_object_unref(dialog);
  g_assert_cmpuint(f->remove_calls, ==, 0); g_assert_true(accounts_store_exists(accounts_store_get_default(), f->npub));
  g_signal_emit_by_name(remove, "clicked"); dialog = ADW_ALERT_DIALOG(adw_window_get_visible_dialog(window)); g_object_ref(dialog);
  gtk_editable_set_text(GTK_EDITABLE(adw_alert_dialog_get_extra_child(dialog)), f->npub);
  g_assert_true(adw_alert_dialog_get_response_enabled(dialog, "proceed")); f->remove_failure = TRUE;
  alert_response(dialog, "proceed"); g_object_unref(dialog); wait_sensitive(remove, TRUE);
  g_assert_cmpuint(f->remove_calls, ==, 1); g_assert_true(accounts_store_exists(accounts_store_get_default(), f->npub));
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(detail_child(page, "public_key"))), ==, f->npub);
  gtk_window_destroy(GTK_WINDOW(window)); g_object_unref(window);
}
static GtkWidget *manage_button(GtkWidget *widget) {
  if (GTK_IS_BUTTON(widget) && !g_strcmp0(gtk_widget_get_tooltip_text(widget), "Manage identity and backup")) return widget;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = manage_button(child); if (found) return found;
  }
  return NULL;
}
static void picker_cannot_delete(Fixture *f, gconstpointer data) {
  (void)data; g_assert_true(gn_backup_get_npub("0000000000000000000000000000000000000000000000000000000000000002", &f->npub, NULL));
  stored_identity = g_strdup(f->npub); accounts_store_add(accounts_store_get_default(), f->npub, "Picker identity", NULL);
  accounts_store_set_label(accounts_store_get_default(), f->npub, "Picker identity", NULL);
  AdwWindow *window = ADW_WINDOW(adw_window_new()); g_object_ref(window); gtk_window_set_default_size(GTK_WINDOW(window), 560, 640); gtk_window_present(GTK_WINDOW(window));
  SheetSelectAccount *picker = sheet_select_account_new(); g_object_ref_sink(picker); adw_dialog_present(ADW_DIALOG(picker), GTK_WIDGET(window));
  GtkWidget *list = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(picker), TYPE_SHEET_SELECT_ACCOUNT, "list_accounts"));
  GtkWidget *button = NULL;
  for (GtkWidget *row = gtk_widget_get_first_child(list); row; row = gtk_widget_get_next_sibling(row))
    if (!g_strcmp0(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(row)), "Picker identity")) { button = manage_button(row); break; }
  g_assert_nonnull(button); g_signal_emit_by_name(button, "clicked");
  g_assert_cmpuint(f->remove_calls, ==, 0); g_assert_true(accounts_store_exists(accounts_store_get_default(), f->npub));
  AdwDialog *detail = adw_window_get_visible_dialog(window); g_assert_true(detail != ADW_DIALOG(picker)); g_object_ref(detail);
  GtkWidget *page = GTK_WIDGET(adw_navigation_view_get_visible_page(ADW_NAVIGATION_VIEW(adw_dialog_get_child(detail))));
  wait_sensitive(detail_child(page, "btn_test"), TRUE);
  adw_dialog_force_close(detail); g_object_unref(detail); adw_dialog_force_close(ADW_DIALOG(picker)); g_object_unref(picker);
  gtk_window_destroy(GTK_WINDOW(window)); g_object_unref(window);
}
static void distinct_import_passphrase(Fixture *f, gconstpointer data) {
  const gchar *format = data; f->expected_format = format; f->expected_pass = "distinct recovery passphrase";
  click(f, "btn_next"); gtk_check_button_set_active(GTK_CHECK_BUTTON(child(f, "radio_import")), TRUE); click(f, "btn_next");
  gboolean mnemonic = !strcmp(format, "mnemonic");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(child(f, "radio_import_seed")), mnemonic);
  g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(child(f, "import_passphrase_label"))), mnemonic ? "Mnemonic passphrase" : "NIP-49 decryption"));
  GtkWidget *box = child(f, "box_import_passphrase_container");
  gn_secure_entry_set_text(GN_SECURE_ENTRY(gtk_widget_get_first_child(box)), f->expected_pass);
  gtk_editable_set_text(GTK_EDITABLE(child(f, "text_import_data")), mnemonic ? "  abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about  " : "ncryptsec1fixture");
  click(f, "btn_next"); wait_invocation(&f->store);
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(child(f, "text_import_data"))), ==, ""); stored(f);
}
static void delayed_import_failure(Fixture *f, gconstpointer data) {
  click(f, "btn_next"); gtk_check_button_set_active(GTK_CHECK_BUTTON(child(f, "radio_import")), TRUE); click(f, "btn_next");
  gtk_editable_set_text(GTK_EDITABLE(child(f, "text_import_data")), "0000000000000000000000000000000000000000000000000000000000000002");
  click(f, "btn_next"); wait_invocation(&f->store);
  g_assert_false(gtk_widget_get_sensitive(child(f, "btn_skip"))); click(f, "btn_skip"); g_assert_cmpuint(f->finished, ==, 0);
  if (data) {
    gtk_window_close(GTK_WINDOW(f->assistant));
    g_dbus_method_invocation_return_value(f->store, g_variant_new("(s)", f->npub)); g_clear_object(&f->store);
  } else {
    g_dbus_method_invocation_return_dbus_error(f->store, "org.nostr.Grotto.Error.BackendUnavailable", "not stored"); g_clear_object(&f->store);
    wait_sensitive(child(f, "btn_next"), TRUE); g_assert_false(onboarding_assistant_identity_stored(f->assistant));
  }
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(child(f, "text_import_data"))), ==, "");
  g_assert_cmpuint(f->remove_calls, ==, 0);
}
static GtkFileChooser *wait_chooser(GtkWindow *parent) {
  guint timeout = g_timeout_add_seconds(10, bound, NULL);
  GtkFileChooser *chooser = NULL; GListModel *windows = gtk_window_get_toplevels();
  while (!chooser) {
    for (guint i = 0; i < g_list_model_get_n_items(windows); i++) {
      GtkWidget *window = g_list_model_get_item(windows, i);
      if (GTK_IS_FILE_CHOOSER(window) && gtk_widget_get_mapped(window) && gtk_window_get_transient_for(GTK_WINDOW(window)) == parent) chooser = GTK_FILE_CHOOSER(window);
      else g_object_unref(window);
      if (chooser) break;
    }
    if (!chooser) g_main_context_iteration(NULL, TRUE);
  }
  g_source_remove(timeout); return chooser;
}
static void encrypted_file_cancel_failure(Fixture *f, gconstpointer data) {
  gboolean cancel = !g_strcmp0(data, "cancel"), success = !g_strcmp0(data, "success");
  g_assert_true(gn_backup_get_npub("0000000000000000000000000000000000000000000000000000000000000002", &f->npub, NULL));
  stored_identity = g_strdup(f->npub); f->expected_pass = "encrypted file passphrase";
  AccountsStore *as = accounts_store_get_default(); accounts_store_add(as, f->npub, "File export", NULL);
  accounts_store_set_setup_state(as, f->npub, "backup-pending", NULL);
  AdwWindow *window = ADW_WINDOW(adw_window_new()); g_object_ref(window); gtk_window_set_default_size(GTK_WINDOW(window), 560, 640);
  GtkWidget *page = page_identity_detail_new(f->npub); adw_window_set_content(window, page); gtk_window_present(GTK_WINDOW(window));
  GtkWidget *export = detail_child(page, "btn_export"); wait_sensitive(export, TRUE);
  GtkWidget *box = detail_child(page, "export_passphrase_box"); gn_secure_entry_set_text(GN_SECURE_ENTRY(gtk_widget_get_first_child(box)), f->expected_pass);
  g_signal_emit_by_name(export, "clicked"); GtkFileChooser *chooser = wait_chooser(GTK_WINDOW(window));
  g_autofree gchar *destination = NULL;
  if (cancel) { gtk_dialog_response(GTK_DIALOG(chooser), GTK_RESPONSE_CANCEL); }
  else {
    guint timeout = g_timeout_add_seconds(10, bound, NULL);
    GFile *folder = NULL;
    while (!(folder = gtk_file_chooser_get_current_folder(chooser))) g_main_context_iteration(NULL, TRUE);
    g_object_unref(folder); g_source_remove(timeout);
    g_autofree gchar *uuid = g_uuid_string_random();
    g_autofree gchar *filename = g_strconcat("g09-", uuid, ".ncryptsec", NULL);
    gtk_file_chooser_set_current_name(chooser, filename);
    g_autoptr(GFile) selected_file = gtk_file_chooser_get_file(chooser);
    destination = selected_file ? g_file_get_path(selected_file) : NULL; g_assert_nonnull(destination);
    g_assert_false(g_file_test(destination, G_FILE_TEST_EXISTS));
    GtkWidget *save = gtk_dialog_get_widget_for_response(GTK_DIALOG(chooser), GTK_RESPONSE_ACCEPT);
    wait_sensitive(save, TRUE); gtk_widget_activate(save);
    wait_invocation(&f->encrypted);
    if (!success) g_assert_cmpint(g_mkdir(destination, 0700), ==, 0); /* chosen destination became a directory */
    g_dbus_method_invocation_return_value(f->encrypted, g_variant_new("(s)", "ncryptsec1fixture")); g_clear_object(&f->encrypted);
  }
  g_object_unref(chooser); wait_sensitive(export, TRUE);
  g_test_message("File completion status: %s", gtk_label_get_text(GTK_LABEL(detail_child(page, "status_label"))));
  g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(detail_child(page, "status_label"))), cancel ? "cancelled" : success ? "Export saved" : "not saved"));
  if (success) {
    g_autofree gchar *contents = NULL; g_assert_true(g_file_get_contents(destination, &contents, NULL, NULL));
    g_assert_cmpstr(contents, ==, "ncryptsec1fixture"); GStatBuf statbuf;
    g_assert_cmpint(g_stat(destination, &statbuf), ==, 0); g_assert_cmpuint(statbuf.st_mode & 0777, ==, 0600);
  }
  gtk_check_button_set_active(GTK_CHECK_BUTTON(detail_child(page, "backup_written_down")), TRUE);
  g_signal_emit_by_name(detail_child(page, "btn_backup_done"), "clicked");
  AccountEntry *entry = accounts_store_find(as, f->npub); g_assert_cmpstr(entry->setup_state, ==, success ? "ready" : "backup-pending"); accounts_store_entry_free(entry);
  g_assert_null(f->encrypted); gtk_window_destroy(GTK_WINDOW(window)); g_object_unref(window);
  if (destination) { if (success) g_unlink(destination); else g_rmdir(destination); }
}
static void secure_widget_buffer(void) {
  GnSecureStats before = gn_secure_get_stats();
  g_autoptr(GtkEntryBuffer) buffer = gn_secure_entry_buffer_new();
  gtk_entry_buffer_set_text(buffer, "disposable recovery words with tail", -1);
  gsize size = gtk_entry_buffer_get_bytes(buffer) + 1;
  const gchar *bytes = gtk_entry_buffer_get_text(buffer);
  gtk_entry_buffer_set_text(buffer, "", -1);
  for (gsize i = 0; i < size; i++) g_assert_cmpint(bytes[i], ==, 0);
  gtk_entry_buffer_set_text(buffer, "aéz", -1); gtk_entry_buffer_delete_text(buffer, 1, 1);
  g_assert_cmpstr(gtk_entry_buffer_get_text(buffer), ==, "az");
  const gchar *strings[] = {"secret key", "mnemonic words", "passphrase"};
  g_autoptr(GVariant) tuple = g_variant_ref_sink(grotto_secret_tuple(strings, 3));
  const gchar *key, *phrase, *pass; g_variant_get(tuple, "(&s&s&s)", &key, &phrase, &pass);
  g_assert_cmpstr(key, ==, strings[0]); g_assert_cmpstr(phrase, ==, strings[1]); g_assert_cmpstr(pass, ==, strings[2]);
  g_assert_true(key != strings[0]); /* serialized payload owns wiping storage */
  g_clear_object(&buffer);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); g_object_ref_sink(box);
  g_assert_true(gn_secure_phrase_set_text(GTK_BOX(box), "abandon about"));
  GtkWidget *flow = gtk_widget_get_first_child(box);
  GtkWidget *entry = gtk_flow_box_child_get_child(GTK_FLOW_BOX_CHILD(gtk_widget_get_first_child(flow))); g_object_ref(entry);
  g_assert_false(gtk_widget_get_can_focus(entry)); g_assert_false(gtk_widget_get_can_target(entry));
  gn_secure_phrase_set_text(GTK_BOX(box), ""); g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(entry)), ==, "");
  g_object_unref(entry); g_object_unref(box);
  GnSecureStats after = gn_secure_get_stats();
  g_assert_cmpuint(after.allocation_count, ==, before.allocation_count);
  g_assert_cmpuint(after.total_locked, ==, before.total_locked);
  gnostr_secure_mem_init(); GnostrSecureMemStats simple_before = gnostr_secure_mem_get_stats();
  gpointer secret = gnostr_secure_alloc(32); g_assert_nonnull(secret); gnostr_secure_free(secret, 32);
  GnostrSecureMemStats simple_after = gnostr_secure_mem_get_stats();
  g_assert_cmpuint(simple_after.allocation_count, ==, simple_before.allocation_count);
  g_assert_cmpuint(simple_after.total_locked, ==, simple_before.total_locked);
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
  g_setenv("GDK_DEBUG", "no-portals", TRUE); /* native chooser; portal acceptance is G08/G12 */
  g_setenv("GIO_USE_VOLUME_MONITOR", "unix", TRUE); /* no desktop/global remote-volume proxies in this private-bus fixture */
  NostrcTestBus *bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE); nostrc_test_bus_up(bus); server = nostrc_test_bus_connect(bus);
  GDBusNodeInfo *info = g_dbus_node_info_new_for_xml("<node><interface name='org.nostr.Signer'><method name='ListIdentities'><arg type='as' direction='out'/></method></interface><interface name='org.nostr.Grotto.Control1'><method name='GetStatus'><arg type='a{sv}' direction='out'/></method><method name='StoreKeyWithMnemonic'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='out'/></method><method name='ImportIdentity'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='out'/></method><method name='SelfTestIdentity'><arg type='s' direction='in'/><arg type='s' direction='out'/></method><method name='CancelIdentitySelfTest'><arg type='s' direction='in'/></method><method name='ExportIdentity'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='out'/></method><method name='RemoveIdentity'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='b' direction='out'/></method><signal name='IdentitySelfTestTesting'><arg type='s'/></signal></interface></node>", NULL);
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
  ADD("backup-failure-blocks-ready", NULL, backup_failure_blocks_ready);
  ADD("verification-warning-handoff", NULL, management_warning_handoff);
  ADD("backup-warning-handoff", "backup", management_warning_handoff);
  ADD("typed-deletion-cancel-failure", NULL, typed_deletion_cancel_failure);
  ADD("picker-cannot-delete", NULL, picker_cannot_delete);
  ADD("mnemonic-passphrase", "mnemonic", distinct_import_passphrase);
  ADD("nip49-passphrase", "ncryptsec", distinct_import_passphrase);
  ADD("delayed-import-failure", NULL, delayed_import_failure);
  ADD("delayed-import-close", "close", delayed_import_failure);
  ADD("encrypted-file-cancel", "cancel", encrypted_file_cancel_failure);
  ADD("encrypted-file-write-failure", NULL, encrypted_file_cancel_failure);
  ADD("encrypted-file-backup-success", "success", encrypted_file_cancel_failure);
#undef ADD
  g_test_add_func("/onboarding/secure-widget-and-variant", secure_widget_buffer);
  g_test_add_func("/onboarding/clipboard-ownership", clipboard_ownership);
  int result = g_test_run(); accounts_store_free(accounts_store_get_default()); g_dbus_connection_unregister_object(server, legacy); g_dbus_connection_unregister_object(server, control);
  g_dbus_node_info_unref(info); nostrc_test_bus_down(bus); g_free(stored_identity); return result;
}
