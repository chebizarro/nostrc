/* GhNip46PairDialog end to end against a real signer (nak bunker on a local
 * relay) and the REAL credential store: a temporary Keychain on macOS, a
 * private Secret Service (gnome-keyring on a private bus) elsewhere. Drives
 * bunker:// and nostrconnect:// pairing through the confirmation, clicking
 * the alert's own "Save Remote Signer" button, then save, account refresh,
 * selection, and the dialog closing with the remote account active.
 * Run with G_MESSAGES_DEBUG=all to see every pairing state transition.
 * Exits 77 when nak, a temporary keychain or a keyring daemon is missing.
 * (nostrc-p15n5.3) */
#include "gh-nip46-pair-dialog.h"
#include "gh-identity.h"
#include "wire-relay.h"
#include "nostrc-test-gdk-frame.h"
#include <nostr-keys.h>
#include <nostr/nip19/nip19.h>
#include <glib/gstdio.h>
#ifdef __APPLE__
#include "gh-nip46-credentials-private.h"
#include "../../../../tests/common/nostrc-test-keychain-guard.h"
#include <Security/Security.h>
#include <unistd.h>
#else
#include "nostrc-test-bus.h"
#endif

void groundhog_register_resource(void);

#define SIGNER_SECRET "7f4c11a9742721d66e40e321ca50b682c27f7422190c14a187525e69e604836a"

static gchar *nak;
static GhNip46CredentialStore *store;

#ifdef __APPLE__
static SecKeychainRef keychain;
static char directory[256], keychain_path[512];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static gboolean
setup_store(void)
{
  nostrc_test_keychain_guard_begin();
  g_snprintf(directory, sizeof directory, "%s/nip46-pair-kc-XXXXXX", g_get_tmp_dir());
  if (!mkdtemp(directory)) return FALSE;
  g_snprintf(keychain_path, sizeof keychain_path, "%s/test.keychain", directory);
  if (SecKeychainCreate(keychain_path, 4, "test", FALSE, NULL, &keychain) != errSecSuccess)
    return FALSE;
  if (SecKeychainUnlock(keychain, 4, "test", TRUE) != errSecSuccess) return FALSE;
  store = gh_nip46_credential_store_new_keychain(keychain);
  return TRUE;
}
static void
teardown_store(void)
{
  g_clear_object(&store);
  if (keychain) { SecKeychainDelete(keychain); CFRelease(keychain); }
  unlink(keychain_path);
  char db[600]; g_snprintf(db, sizeof db, "%s-db", keychain_path); unlink(db);
  rmdir(directory);
  nostrc_test_keychain_guard_end();
}
#pragma clang diagnostic pop
#else
static NostrcTestBus *bus;
static gboolean
setup_store(void)
{
  gchar *daemon = g_find_program_in_path("gnome-keyring-daemon");
  if (!nostrc_test_bus_available() || !daemon) { g_free(daemon); return FALSE; }
  bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
  nostrc_test_bus_up(bus);
  const gchar *root = nostrc_test_bus_get_dir(bus);
  static const gchar *vars[] = { "HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME",
                                 "XDG_CACHE_HOME", "XDG_RUNTIME_DIR" };
  for (guint i = 0; i < G_N_ELEMENTS(vars); i++) {
    gchar *dir = g_build_filename(root, vars[i], NULL);
    g_assert_cmpint(g_mkdir(dir, 0700), ==, 0);
    g_setenv(vars[i], dir, TRUE);
    g_free(dir);
  }
  gchar *password = g_build_filename(root, "password", NULL);
  GError *error = NULL;
  g_assert_true(g_file_set_contents_full(password, "nip46-test", 10,
                                         G_FILE_SET_CONTENTS_NONE, 0600, &error));
  g_assert_no_error(error);
  const gchar *argv[] = { daemon, "--foreground", "--unlock", "--components=secrets", NULL };
  nostrc_test_bus_spawn_supervised(bus, "gnome-keyring.log", password, argv);
  g_free(password);
  GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
  g_assert_no_error(error);
  gboolean appeared = FALSE;
  for (guint i = 0; i < 300 && !appeared; i++) {
    GVariant *owner = g_dbus_connection_call_sync(connection, "org.freedesktop.DBus",
      "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
      g_variant_new("(s)", "org.freedesktop.secrets"), G_VARIANT_TYPE("(b)"),
      G_DBUS_CALL_FLAGS_NONE, 1000, NULL, &error);
    g_assert_no_error(error);
    g_variant_get(owner, "(b)", &appeared);
    g_variant_unref(owner);
    if (!appeared) g_usleep(100000);
  }
  g_object_unref(connection);
  g_free(daemon);
  if (!appeared) { nostrc_test_bus_dump_log(bus, "gnome-keyring.log"); return FALSE; }
  store = gh_nip46_credential_store_new_secret_service();
  return TRUE;
}
static void
teardown_store(void)
{
  g_clear_object(&store);
  if (bus) nostrc_test_bus_down(bus);
}
#endif

static GPtrArray *
no_grotto(gpointer data, GError **error)
{
  (void)data; (void)error;
  return g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
}

static gboolean
expired_cb(gpointer data)
{
  *(gboolean *)data = TRUE;
  return G_SOURCE_REMOVE;
}

/* Iterates until *flag is set or the deadline passes. */
#define WAIT_UNTIL(cond, seconds) G_STMT_START {                              \
    gboolean expired_ = FALSE;                                                \
    guint timer_ = g_timeout_add_seconds((seconds), expired_cb, &expired_);   \
    while (!(cond) && !expired_) g_main_context_iteration(NULL, TRUE);        \
    if (!expired_) g_source_remove(timer_);                                   \
    g_assert_true(cond);                                                      \
  } G_STMT_END

static GSubprocess *
launch(const gchar *home, const gchar *const *args)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
    G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  g_subprocess_launcher_setenv(launcher, "HO" "ME", home, TRUE);
  g_subprocess_launcher_unsetenv(launcher, "XDG_CONFIG_" "HO" "ME");
  GSubprocess *process = g_subprocess_launcher_spawnv(launcher, args, &error);
  g_assert_no_error(error);
  return process;
}

static void
stop(GSubprocess *process)
{
  g_subprocess_force_exit(process);
  g_subprocess_wait(process, NULL, NULL);
  g_object_unref(process);
}

static void
remove_tree(const gchar *path)
{
  GDir *dir = g_dir_open(path, 0, NULL);
  if (dir) {
    const gchar *name;
    while ((name = g_dir_read_name(dir))) {
      g_autofree gchar *child = g_build_filename(path, name, NULL);
      remove_tree(child);
    }
    g_dir_close(dir);
  }
  g_remove(path);
}

static GtkButton *
find_button(GtkWidget *widget, const gchar *label)
{
  if (GTK_IS_BUTTON(widget) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), label) == 0)
    return GTK_BUTTON(widget);
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child;
       child = gtk_widget_get_next_sibling(child)) {
    GtkButton *found = find_button(child, label);
    if (found) return found;
  }
  return NULL;
}

static void
capture_confirmation(GhNip46PairDialog *dialog, AdwAlertDialog *alert, gpointer data)
{
  (void)dialog;
  *(AdwAlertDialog **)data = g_object_ref(alert);
}

static void
mark_closed(AdwDialog *dialog, gpointer data)
{
  (void)dialog;
  *(gboolean *)data = TRUE;
}

static GtkWidget *
child(GhNip46PairDialog *dialog, const gchar *name)
{
  return GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(dialog),
                                                  GH_TYPE_NIP46_PAIR_DIALOG, name));
}

typedef struct { gboolean done; GhNip46Credential *credential; GError *error; } Lookup;

static void
looked_up(GObject *source, GAsyncResult *result, gpointer data)
{
  Lookup *out = data;
  out->credential = gh_nip46_credential_store_lookup_finish(
    GH_NIP46_CREDENTIAL_STORE(source), result, &out->error);
  out->done = TRUE;
}

static void
pair_end_to_end(gboolean qr)
{
  WireRelay relay = { .serve = TRUE };
  relay_init(&relay);
  /* nak binds a unix socket under its home: keep the path short. */
  gchar *home = g_strdup("/tmp/ghpn-XXXXXX");
  g_assert_nonnull(g_mkdtemp(home));
  char *signer_pubkey = nostr_key_get_public(SIGNER_SECRET);
  const gchar *bunker_args[] = { nak, "bunker", "--sec", SIGNER_SECRET,
    "--authorized-secrets", "groundhogpairing", relay.url, NULL };
  guint before = relay.reqs;
  GSubprocess *signer = launch(home, bunker_args);
  WAIT_UNTIL(relay.reqs > before, 10);

  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(settings, "current-npub", "");
  g_settings_set_string(settings, "current-backend", "grotto");
  /* As in the app: the controller lists remote identities from the store,
   * and the dialog is given no store, so it must save into that one. */
  g_autoptr(GhAccountController) accounts = gh_account_controller_new_full_with_credentials(
    settings, NULL, no_grotto, NULL, store);
  GhNip46PairConfig config = { .accounts = accounts, .settings = settings };
  GtkWidget *window = gtk_window_new();
  gtk_window_present(GTK_WINDOW(window));
  GhNip46PairDialog *dialog = g_object_ref_sink(gh_nip46_pair_dialog_new(&config));
  AdwAlertDialog *alert = NULL;
  gboolean closed = FALSE;
  g_signal_connect(dialog, "confirmation-presented", G_CALLBACK(capture_confirmation), &alert);
  g_signal_connect(dialog, "closed", G_CALLBACK(mark_closed), &closed);
  adw_dialog_present(ADW_DIALOG(dialog), window);
  GSubprocess *connect = NULL;
  GtkLabel *status;
  if (qr) {
    status = GTK_LABEL(child(dialog, "qr_status"));
    gtk_editable_set_text(GTK_EDITABLE(child(dialog, "relay_row")), relay.url);
    g_signal_emit_by_name(child(dialog, "regenerate_button"), "clicked");
    GtkEditable *uri_row = GTK_EDITABLE(child(dialog, "uri_row"));
    WAIT_UNTIL(g_str_has_prefix(gtk_editable_get_text(uri_row), "nostrconnect://"), 10);
    g_autofree gchar *uri = g_strdup(gtk_editable_get_text(uri_row));
    const gchar *connect_args[] = { nak, "bunker", "connect", uri, NULL };
    connect = launch(home, connect_args);
  } else {
    status = GTK_LABEL(child(dialog, "bunker_details"));
    gtk_stack_set_visible_child_name(GTK_STACK(child(dialog, "mode_stack")), "bunker");
    g_autofree gchar *escaped = g_uri_escape_string(relay.url, NULL, FALSE);
    g_autofree gchar *uri = g_strdup_printf("bunker://%s?relay=%s&secret=groundhogpairing",
                                            signer_pubkey, escaped);
    gtk_editable_set_text(GTK_EDITABLE(child(dialog, "bunker_row")), uri);
    GtkWidget *button = child(dialog, "connect_button");
    g_assert_true(gtk_widget_get_sensitive(button));
    g_signal_emit_by_name(button, "clicked");
  }
  WAIT_UNTIL(alert != NULL, 20);
  WAIT_UNTIL(gtk_widget_get_mapped(GTK_WIDGET(alert)), 5);
  GtkButton *save = find_button(GTK_WIDGET(alert), "Save Remote Signer");
  g_assert_nonnull(save);
  g_signal_emit_by_name(save, "clicked");
  g_clear_object(&alert);
  /* Progress is shown on the tab in use, with Cancel still available. */
  g_assert_nonnull(strstr(gtk_label_get_text(status), "Saving"));
  g_assert_true(gtk_widget_get_visible(child(dialog, "cancel_button")));

  char *expected = NULL;
  guint8 bytes[32];
  for (guint i = 0; i < 32; i++) {
    gchar pair[3] = { signer_pubkey[i * 2], signer_pubkey[i * 2 + 1], 0 };
    bytes[i] = (guint8)strtoul(pair, NULL, 16);
  }
  g_assert_cmpint(nostr_nip19_encode_npub(bytes, &expected), ==, 0);
  WAIT_UNTIL(closed, 30);
  g_autofree gchar *current = g_settings_get_string(settings, "current-npub");
  g_assert_cmpstr(current, ==, expected);
  g_assert_cmpint(gh_account_controller_get_active_backend(accounts), ==,
                  GH_SIGNER_BACKEND_NIP46);
  g_assert_nonnull(strstr(gtk_label_get_text(status), "Connected as"));
  Lookup found = { 0 };
  gh_nip46_credential_store_lookup_async(store, signer_pubkey, NULL, looked_up, &found);
  WAIT_UNTIL(found.done, 10);
  g_assert_no_error(found.error);
  g_assert_cmpstr(gh_nip46_credential_get_remote_signer_pubkey_hex(found.credential), ==,
                  signer_pubkey);
  gh_nip46_credential_free(found.credential);

  g_settings_set_string(settings, "current-npub", "");
  g_settings_set_string(settings, "current-backend", "grotto");
  gtk_window_destroy(GTK_WINDOW(window));
  g_object_unref(dialog);
  for (guint i = 0; i < 100 && g_main_context_pending(NULL); i++)
    g_main_context_iteration(NULL, FALSE);
  if (connect) stop(connect);
  stop(signer);
  free(expected);
  free(signer_pubkey);
  remove_tree(home);
  g_free(home);
  relay_clear(&relay);
}

static void
test_bunker(void)
{
  pair_end_to_end(FALSE);
}

static void
test_qr(void)
{
  pair_end_to_end(TRUE);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  nak = g_find_program_in_path("nak");
  if (!nak) { g_print("SKIP: nak is not installed\n"); return 77; }
  if (!gtk_init_check()) return 77;
  adw_init();
  groundhog_register_resource();
  nostrc_test_tolerate_gdk_frame_warning();
  if (!setup_store()) { g_print("SKIP: no private credential store\n"); return 77; }
  g_test_add_func("/groundhog/nip46/pair-nak/bunker-save-select", test_bunker);
  g_test_add_func("/groundhog/nip46/pair-nak/qr-save-select", test_qr);
  int status = g_test_run();
  teardown_store();
  g_free(nak);
  return status;
}
