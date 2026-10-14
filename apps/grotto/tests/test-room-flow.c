#include "approval-controller.h"
#include "gh-account-controller.h"
#include "gh-nip17-envelope.h"
#include "gh-signer.h"
#include "nostr-event.h"
#include "nostr-kinds.h"
#include "nostr-keys.h"
#include "nostr-utils.h"
#include "nostr-json.h"
#include "nostr/nip19/nip19.h"
#include "nostr/nip17/nip17.h"
#include "nostrc-test-bus.h"
#include "nostrc-test-gdk-frame.h"
#include "ui/approval_dialog.h"
#include <glib/gstdio.h>
#include <string.h>
#include <stdlib.h>

static NostrcTestBus *test_bus;
static guint prompts;
static gchar *npub;
static const gchar *secret = "0000000000000000000000000000000000000000000000000000000000000001";
static gboolean fail_bound(gpointer data) {
  if (test_bus) {
    nostrc_test_bus_dump_log(test_bus, "room-flow-daemon.log");
    nostrc_test_bus_dump_log(test_bus, "room-flow-gui.log");
  }
  g_error("room flow did not complete at line %d", GPOINTER_TO_INT(data));
  return G_SOURCE_REMOVE;
}
#define WAIT(condition) do { \
  guint bound = g_timeout_add_seconds(20, fail_bound, GINT_TO_POINTER(__LINE__)); \
  while (!(condition)) g_main_context_iteration(NULL, TRUE); \
  g_source_remove(bound); \
} while (0)
static GPtrArray *list_identity(gpointer data, GError **error) {
  (void)data; (void)error;
  GPtrArray *rows = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  GhIdentityInfo *row = g_new0(GhIdentityInfo, 1);
  row->npub = g_strdup(npub); row->label = g_strdup("Disposable room sender");
  g_ptr_array_add(rows, row); return rows;
}
static void prompt(GDBusConnection *bus, const gchar *sender, const gchar *path,
    const gchar *iface, const gchar *signal, GVariant *args, gpointer data) {
  (void)bus; (void)sender; (void)path; (void)iface; (void)signal; (void)args; (void)data;
  prompts++;
}
static void owner_seen(GDBusConnection *bus, const gchar *name, const gchar *owner, gpointer data) {
  (void)bus; (void)name; (void)owner; *(gboolean *)data = TRUE;
}
static gboolean ready(GrottoApprovalController *s) {
  GListModel *pending = grotto_approval_controller_get_pending(s);
  if (!g_list_model_get_n_items(pending)) return FALSE;
  g_autoptr(GrottoApprovalRequest) r = g_list_model_get_item(pending, 0);
  return grotto_approval_request_get_state(r) == GROTTO_APPROVAL_READY;
}
typedef struct { gboolean done, ok; GError *error; } Decision;
static void decided(GObject *source, GAsyncResult *result, gpointer data) {
  Decision *d = data;
  d->ok = grotto_approval_controller_decide_finish(GROTTO_APPROVAL_CONTROLLER(source), result, &d->error);
  d->done = TRUE;
}
typedef struct { gboolean done; GhNip17Envelope *envelope; GError *error; } Envelope;
static void envelope_done(GObject *source, GAsyncResult *result, gpointer data) {
  (void)source; Envelope *e = data;
  e->envelope = gh_nip17_envelope_build_finish(result, &e->error); e->done = TRUE;
}
static void verify_wrap(const gchar *json, const gchar *recipient_secret, const gchar *sender) {
  NostrEvent *wrap = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize(wrap, json), ==, 0);
  g_assert_true(nostr_event_check_signature(wrap));
  gchar *content = NULL, *author = NULL;
  g_assert_cmpint(nostr_nip17_decrypt_dm(wrap, recipient_secret, &content, &author), ==, 0);
  g_assert_cmpstr(content, ==, "private room fixture");
  g_assert_cmpstr(author, ==, sender);
  free(content); free(author); nostr_event_free(wrap);
}
static void signer_done(GObject *source, GAsyncResult *result, gpointer data) {
  (void)source; Decision *d = data;
  g_autofree gchar *value = gh_signer_sign_finish(result, &d->error);
  d->ok = value != NULL; d->done = TRUE;
}
static void run_room_flow(GDBusConnection *bus, gboolean gui) {
  guint subscription = g_dbus_connection_signal_subscribe(bus, "org.nostr.Signer", "org.nostr.Signer",
      "ApprovalRequested", "/org/nostr/signer", NULL, 0, prompt, NULL, NULL);
  g_autofree gchar *sender = nostr_key_get_public(secret);
  guint8 bytes[32];
  g_assert_true(nostr_hex2bin(bytes, sender, sizeof bytes));
  g_assert_cmpint(nostr_nip19_encode_npub(bytes, &npub), ==, 0);
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(settings, "current-npub", npub);
  g_autoptr(GhAccountController) accounts = gh_account_controller_new_full(settings, bus, list_identity, NULL);
  WAIT(gh_account_controller_get_state(accounts) == GH_ACCOUNT_STATE_ACTIVE);
  GApplication *application;
#ifndef __APPLE__
  if (gui) application = G_APPLICATION(adw_application_new("org.test.RoomConsent", G_APPLICATION_NON_UNIQUE));
  else
#else
  (void)gui;
#endif
    application = g_application_new("org.test.RoomConsent", G_APPLICATION_NON_UNIQUE);
  g_autoptr(GApplication) app = application;
#ifndef __APPLE__
  if (gui) g_assert_true(g_application_register(app, NULL, NULL));
#endif
  g_autoptr(GrottoApprovalController) controller = grotto_approval_controller_new(app, bus);
  const gchar *keys[] = {
    "0000000000000000000000000000000000000000000000000000000000000002",
    "0000000000000000000000000000000000000000000000000000000000000003",
    "0000000000000000000000000000000000000000000000000000000000000004"};
  gchar *recipients[4] = {0};
  for (guint i = 0; i < 3; i++) recipients[i] = nostr_key_get_public(keys[i]);
  g_autofree gchar *rumor = gh_nip17_rumor_new_room(sender, (const gchar *const *)recipients,
      "private room fixture", 1, 0, NULL, NULL);
  g_assert_nonnull(rumor);
  Envelope result = {0};
  gh_nip17_envelope_seal_room_async(accounts, rumor, NULL, NULL, envelope_done, &result);
  WAIT(ready(controller) || result.done);
  g_assert_false(result.done);
  g_assert_cmpuint(prompts, ==, 1);
  g_autoptr(GrottoApprovalRequest) request =
      g_list_model_get_item(grotto_approval_controller_get_pending(controller), 0);
  GVariant *info = grotto_approval_request_get_info(request);
  guint size = 0; const gchar *flow = NULL;
  g_assert_true(g_variant_lookup(info, "flow_size", "u", &size)); g_assert_cmpuint(size, ==, 4);
  g_assert_true(g_variant_lookup(info, "flow_id", "&s", &flow)); g_assert_true(*flow != 0);
  g_autofree gchar *flow_id = g_strdup(flow);
  Decision decision = {0};
#ifndef __APPLE__
  if (gui) {
    grotto_approval_controller_review(controller, grotto_approval_request_get_id(request));
    GList *windows = gtk_application_get_windows(GTK_APPLICATION(app));
    g_assert_cmpuint(g_list_length(windows), ==, 1);
    AdwDialog *dialog = adw_window_get_visible_dialog(ADW_WINDOW(windows->data));
    g_assert_true(GNOSTR_IS_APPROVAL_DIALOG(dialog));
    GtkCheckButton *encrypt = GTK_CHECK_BUTTON(gtk_widget_get_template_child(GTK_WIDGET(dialog),
        GNOSTR_TYPE_APPROVAL_DIALOG, "chk_flow_encrypt"));
    GtkCheckButton *seals = GTK_CHECK_BUTTON(gtk_widget_get_template_child(GTK_WIDGET(dialog),
        GNOSTR_TYPE_APPROVAL_DIALOG, "chk_flow_seals"));
    g_assert_false(gtk_check_button_get_active(encrypt)); g_assert_false(gtk_check_button_get_active(seals));
    gtk_check_button_set_active(encrypt, TRUE); gtk_check_button_set_active(seals, TRUE);
    GtkWidget *approve = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(dialog),
        GNOSTR_TYPE_APPROVAL_DIALOG, "btn_approve"));
    g_assert_true(gtk_widget_get_sensitive(approve));
    g_signal_emit_by_name(approve, "clicked"); /* real dialog → controller → daemon grant creation */
    decision.done = decision.ok = TRUE;
  } else
#endif
    grotto_approval_controller_decide_flow_async(controller, grotto_approval_request_get_id(request),
        TRUE, TRUE, NULL, decided, &decision);
  WAIT(result.done && decision.done);
  g_assert_no_error(decision.error); g_assert_true(decision.ok);
  g_assert_no_error(result.error); g_assert_nonnull(result.envelope);
  g_assert_cmpuint(prompts, ==, 1); /* all three recipient seals + self-copy; no extra prompts */
  for (guint i = 0; i < 3; i++) verify_wrap(result.envelope->recipient_wraps[i], keys[i], sender);
  verify_wrap(result.envelope->sender_wrap_json, secret, sender);
  gh_nip17_envelope_free(result.envelope);
  WAIT(g_list_model_get_n_items(grotto_approval_controller_get_pending(controller)) == 0);

  /* An extra seal with the SAME caller/identity/transport/flow/size cannot
   * borrow the exhausted send's consent, even though encryption is remembered. */
  GhSigner *signer = gh_signer_new(bus, npub, NULL);
  g_assert_nonnull(signer);
  GhSignerContext context = {"Sign an extra encrypted message", "nip17-room-send", flow_id, size};
  g_autofree gchar *seal = g_strdup_printf("{\"kind\":%u,\"pubkey\":\"%s\",\"created_at\":1,\"tags\":[],\"content\":\"encrypted\"}", NOSTR_KIND_SEAL, sender);
  Decision extra = {0}, denial = {0};
  gh_signer_sign_with_context_async(signer, seal, &context, NULL, signer_done, &extra);
  WAIT(ready(controller) || extra.done);
  g_assert_false(extra.done); g_assert_cmpuint(prompts, ==, 2);
  g_autoptr(GrottoApprovalRequest) excess =
      g_list_model_get_item(grotto_approval_controller_get_pending(controller), 0);
  grotto_approval_controller_decide_async(controller, grotto_approval_request_get_id(excess),
      FALSE, FALSE, 0, NULL, decided, &denial);
  WAIT(extra.done && denial.done);
  g_assert_no_error(denial.error); g_assert_true(denial.ok);
  g_assert_error(extra.error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_DENIED);
  g_clear_error(&extra.error);
  gh_signer_free(signer);
  grotto_approval_controller_stop(controller);
  g_clear_object(&controller); g_clear_object(&accounts);
  g_dbus_connection_signal_unsubscribe(bus, subscription);
#ifndef __APPLE__
  if (gui) while (gtk_application_get_windows(GTK_APPLICATION(app)))
    gtk_window_destroy(gtk_application_get_windows(GTK_APPLICATION(app))->data);
#endif
  g_free(npub);
  for (guint i = 0; i < 3; i++) free(recipients[i]);
}
#ifndef __APPLE__
static void worker_done(GObject *source, GAsyncResult *result, gpointer data) {
  Decision *d = data;
  d->ok = g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &d->error); d->done = TRUE;
}
#endif
int main(int argc, char **argv) {
  gboolean worker = argc == 3 && !strcmp(argv[2], "--gui-worker");
  g_assert_true(argc == 2 || worker); argc = 2;
  g_assert_cmpstr(g_getenv("NOSTRC_TEST_ISOLATION"), ==, "1");
  if (!nostrc_test_bus_available()) return 77;
  g_test_init(&argc, &argv, NULL);
  nostr_json_init(); nostrc_test_bus_tolerate_ebadf();
  g_setenv("NOSTR_SIGNER_TEST_APPROVERS", argv[0], TRUE);
  g_setenv("NOSTR_SIGNER_SECKEY_HEX", secret, TRUE);
#ifndef __APPLE__
  if (worker) {
    nostrc_test_tolerate_gdk_frame_warning(); gtk_init(); adw_init();
    g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
    g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    g_assert_nonnull(bus); run_room_flow(bus, TRUE); nostr_json_cleanup(); return 0;
  }
#endif
  test_bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
  nostrc_test_bus_up(test_bus);
  g_autofree gchar *config = g_build_filename(nostrc_test_bus_get_dir(test_bus), "config", NULL);
  g_assert_cmpint(g_mkdir(config, 0700), ==, 0);
  g_setenv("XDG_CONFIG_HOME", config, TRUE);
  GDBusConnection *bus = nostrc_test_bus_connect(test_bus);
  g_autoptr(GVariant) owned = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus",
      "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
      g_variant_new("(su)", "org.nostr.Grotto", 0), NULL, 0, 5000, NULL, NULL);
  g_assert_nonnull(owned);
  gboolean started = FALSE;
  guint watch = g_bus_watch_name_on_connection(bus, "org.nostr.Signer", 0, owner_seen, NULL, &started, NULL);
  const gchar *command[] = {argv[1], NULL};
  nostrc_test_bus_spawn_supervised(test_bus, "room-flow-daemon.log", NULL, command);
  WAIT(started);
#ifndef __APPLE__
  if (g_getenv("DISPLAY")) {
    /* GTK owns process-global bus proxies: isolate the GUI in a peer on this
     * private bus so coordinator teardown still verifies its own connections. */
    g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDERR_MERGE);
    g_autofree gchar *log = g_build_filename(nostrc_test_bus_get_dir(test_bus), "room-flow-gui.log", NULL);
    g_subprocess_launcher_set_stdout_file_path(launcher, log);
    g_autoptr(GSubprocess) peer = g_subprocess_launcher_spawn(launcher, NULL, argv[0], argv[1], "--gui-worker", NULL);
    g_assert_nonnull(peer);
    Decision done = {0}; g_subprocess_wait_check_async(peer, NULL, worker_done, &done);
    WAIT(done.done); nostrc_test_bus_dump_log(test_bus, "room-flow-gui.log");
    g_assert_no_error(done.error); g_assert_true(done.ok);
  } else
#endif
    run_room_flow(bus, FALSE);
  g_bus_unwatch_name(watch);
  nostrc_test_bus_down(test_bus); nostr_json_cleanup(); return 0;
}
