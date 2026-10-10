/* GhNip46PairDialog end to end against a real signer (nak bunker on a local
 * relay) and the REAL credential store: a temporary Keychain on macOS, a
 * private Secret Service (gnome-keyring on a private bus) elsewhere. Drives
 * bunker:// and nostrconnect:// pairing through the inline confirmation,
 * clicking its own "Save Remote Signer" button: the account is active at
 * once on the live session (no second keyring lookup, no new subscription),
 * the credential is saved, and the dialog closes. Then a restart restores
 * the account (with Grotto hanging) and signs a real event through nak, a
 * locked Keychain is unlocked with the explicit Unlock, and an unreliable
 * relay (no EOSE, a rate-limited NACK, a dropped connection) and an
 * Amber-style signer (bare "ack", a stray event first, re-stamped
 * created_at) are handled (nostrc-8xfib.1).
 * Run with G_MESSAGES_DEBUG=all to see every pairing state transition.
 * Exits 77 when nak, a temporary keychain or a keyring daemon is missing.
 * (nostrc-p15n5.3) */
#include "gh-nip46-pair-dialog.h"
#include "gh-identity.h"
#include "gh-relay-publish.h"
#include "wire-relay.h"
#include "nostrc-test-gdk-frame.h"
#include <nostr-keys.h>
#include <nostr-utils.h>
#include <nostr/nip19/nip19.h>
#include <nostr/nip44/nip44.h>
#include <nostr/nip46/nip46_msg.h>
#include <nostr/nip46/nip46_envelope.h>
#include <nostr/nip46/nip46_uri.h>
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

static gchar *
npub_of(const gchar *hex)
{
  guint8 bytes[32];
  g_assert_true(nostr_hex2bin(bytes, hex, sizeof bytes));
  char *npub = NULL;
  g_assert_cmpint(nostr_nip19_encode_npub(bytes, &npub), ==, 0);
  gchar *out = g_strdup(npub);
  free(npub);
  return out;
}

/* A Grotto D-Bus listing that never answers until released. */
typedef struct { GMutex lock; GCond cond; gboolean release; guint calls; } Hang;
static Hang hang;

static GPtrArray *
hanging_grotto(gpointer data, GError **error)
{
  (void)error;
  Hang *h = data;
  g_mutex_lock(&h->lock);
  h->calls++;
  while (!h->release) g_cond_wait(&h->cond, &h->lock);
  g_mutex_unlock(&h->lock);
  return g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
}

static void
release_grotto(void)
{
  g_mutex_lock(&hang.lock);
  hang.release = TRUE;
  g_cond_broadcast(&hang.cond);
  g_mutex_unlock(&hang.lock);
}

typedef struct { gboolean done; gchar *value; GError *error; } Signed;

static void
signed_cb(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  Signed *out = data;
  out->value = gh_account_controller_sign_finish(result, &out->error);
  out->done = TRUE;
}

/* Signs a kind-1 note through the active remote signer, as the composer
 * does, and checks the signer key and signature. */
static gchar *
sign_note(GhAccountController *accounts, const gchar *pubkey, const gchar *content)
{
  gh_account_controller_set_remote_storage_ready(accounts,
    gh_account_controller_get_generation(accounts), TRUE);
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree gchar *unsigned_json = g_strdup_printf(
    "{\"pubkey\":\"%s\",\"created_at\":%lld,\"kind\":1,\"tags\":[],\"content\":\"%s\"}",
    pubkey, (long long)now, content);
  Signed out = { 0 };
  gh_account_controller_sign_async(accounts, unsigned_json, signed_cb, &out);
  WAIT_UNTIL(out.done, 40);
  g_assert_no_error(out.error);
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(event, out.value, NULL), ==, 1);
  g_assert_cmpint(nostr_event_validate(event, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpstr(nostr_event_get_pubkey(event), ==, pubkey);
  g_assert_cmpstr(nostr_event_get_content(event), ==, content);
  nostr_event_free(event);
  return out.value;
}

typedef struct { gboolean done; gboolean accepted; } Published;

static void
published_cb(GhRelayPublish *publish, const GhRelayPublishSummary *summary, gpointer data)
{
  (void)publish;
  Published *out = data;
  out->accepted = summary->any_accepted;
  out->done = TRUE;
}

/* The send half of the composer: the signed note reaches the relay. */
static void
publish_note(WireRelay *relay, const gchar *json)
{
  g_autoptr(GError) error = NULL;
  Published out = { 0 };
  GhRelayPublish *publish = gh_relay_publish_new(1, json, NULL, published_cb, &out, &error);
  g_assert_no_error(error);
  g_assert_true(gh_relay_publish_add_url(publish, relay->url, NULL));
  g_assert_true(gh_relay_publish_start(publish, NULL));
  WAIT_UNTIL(out.done, 20);
  g_assert_true(out.accepted);
  gh_relay_publish_unref(publish);
}

static gboolean
remote_state_is(GhAccountController *accounts, GhRemoteSignerState state)
{
  return gh_account_controller_get_remote_state(accounts) == state;
}

static void
drain(void)
{
  for (guint i = 0; i < 200 && g_main_context_pending(NULL); i++)
    g_main_context_iteration(NULL, FALSE);
}

/* ---- an Amber-style signer, scripted on the relay ---------------------- */

#define AMBER_SECRET "5c0c523f52a5b6fad39ed2403092df8cebc36318b39383bca6c00808626fab3a"
static gchar *amber_pubkey;
static guint amber_requests;
static gboolean amber_restamp;

/* Publishes a NIP-46 message from @secret to @to, NIP-44 encrypted. */
static void
amber_send(WireRelay *relay, const gchar *secret, const gchar *to, const gchar *plaintext)
{
  guint8 sk[32], pk[32];
  g_assert_true(nostr_hex2bin(sk, secret, sizeof sk));
  g_assert_true(nostr_hex2bin(pk, to, sizeof pk));
  char *ciphertext = NULL;
  g_assert_cmpint(nostr_nip44_encrypt_v2(sk, pk, (const guint8 *)plaintext,
                                         strlen(plaintext), &ciphertext), ==, 0);
  char *from = nostr_key_get_public(secret);
  NostrEvent *event = NULL;
  g_assert_cmpint(nostr_nip46_build_response_event(from, to, ciphertext, &event), ==, 0);
  nostr_event_set_content(event, ciphertext);
  g_assert_cmpint(nostr_event_sign(event, secret), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  wire_relay_inject(relay, json);
  free(json);
  nostr_event_free(event);
  free(ciphertext);
  free(from);
}

typedef struct { WireRelay *relay; gchar *json; } AmberJob;

static gboolean
amber_answer(gpointer data)
{
  AmberJob *job = data;
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_signed(event, job->json, NULL), ==,
                  NOSTR_EVENT_VALIDATION_OK);
  g_autofree gchar *client = g_strdup(nostr_event_get_pubkey(event));
  guint8 sk[32], pk[32];
  g_assert_true(nostr_hex2bin(sk, AMBER_SECRET, sizeof sk));
  g_assert_true(nostr_hex2bin(pk, client, sizeof pk));
  guint8 *plain = NULL;
  size_t plain_len = 0;
  g_assert_cmpint(nostr_nip44_decrypt_v2(sk, pk, nostr_event_get_content(event),
                                         &plain, &plain_len), ==, 0);
  nostr_event_free(event);
  g_autofree gchar *request_json = g_strndup((const gchar *)plain, plain_len);
  free(plain);
  NostrNip46Request request = { 0 };
  g_assert_cmpint(nostr_nip46_request_parse(request_json, &request), ==, 0);
  amber_requests++;
  g_autofree gchar *value = NULL;
  if (g_str_equal(request.method, "get_public_key")) {
    value = g_strdup(amber_pubkey);
  } else if (g_str_equal(request.method, "ping")) {
    value = g_strdup("pong");
  } else if (g_str_equal(request.method, "sign_event")) {
    NostrEvent *note = nostr_event_new();
    g_assert_cmpint(nostr_event_deserialize_compact(note, request.params[0], NULL), ==, 1);
    /* Some signers re-stamp created_at. */
    if (amber_restamp)
      nostr_event_set_created_at(note, nostr_event_get_created_at(note) + 5);
    g_assert_cmpint(nostr_event_sign(note, AMBER_SECRET), ==, 0);
    char *signed_json = nostr_event_serialize_compact(note);
    value = g_strdup(signed_json);
    free(signed_json);
    nostr_event_free(note);
  } else {
    g_error("unexpected request to the Amber-style signer: %s", request.method);
  }
  g_autofree gchar *escaped = g_strescape(value, NULL);
  g_autofree gchar *result = g_strdup_printf("\"%s\"", escaped);
  char *response = nostr_nip46_response_build_ok(request.id, result);
  amber_send(job->relay, AMBER_SECRET, client, response);
  free(response);
  nostr_nip46_request_free(&request);
  g_free(job->json);
  g_free(job);
  return G_SOURCE_REMOVE;
}

/* on_event: a request addressed to the Amber-style signer is answered. */
static void
amber_on_event(WireRelay *relay, SoupWebsocketConnection *connection,
               const gchar *event_id, gpointer data)
{
  (void)connection; (void)data;
  for (guint i = 0; i < relay->stored->len; i++) {
    WireStored *stored = g_ptr_array_index(relay->stored, i);
    if (!g_str_equal(stored->id, event_id)) continue;
    if (nostr_event_get_kind(stored->event) != 24133 ||
        g_strcmp0(nostr_event_get_pubkey(stored->event), amber_pubkey) == 0) return;
    const gchar *p = wire_tag_value(stored->event, "p");
    if (g_strcmp0(p, amber_pubkey) != 0) return;
    AmberJob *job = g_new0(AmberJob, 1);
    job->relay = relay;
    job->json = g_strdup(stored->json);
    g_idle_add(amber_answer, job);
    return;
  }
}

/* The signer scans the QR: a stray reply from someone else first, then a
 * bare "ack" without the secret, as older Amber builds send. */
static void
amber_answer_qr(WireRelay *relay, const gchar *uri)
{
  NostrNip46ConnectURI parsed = { 0 };
  g_assert_cmpint(nostr_nip46_uri_parse_connect(uri, &parsed), ==, 0);
  char *stray_key = nostr_key_generate_private();
  amber_send(relay, stray_key, parsed.client_pubkey_hex,
             "{\"id\":\"stale\",\"result\":\"not-the-secret\"}");
  free(stray_key);
  amber_send(relay, AMBER_SECRET, parsed.client_pubkey_hex,
             "{\"id\":\"connect\",\"result\":\"ack\"}");
  nostr_nip46_uri_connect_free(&parsed);
}

/* Pairs through the dialog and clicks the real Save button: the account is
 * active at once; the dialog closes once the credential is stored. */
static GhNip46PairDialog *
pair_and_save(GhAccountController *accounts, GSettings *settings, WireRelay *relay,
              GtkWidget *window, const gchar *bunker_uri, GSubprocess **connect,
              const gchar *home, gboolean *closed, guint *reqs_at_save)
{
  GhNip46PairConfig config = { .accounts = accounts, .settings = settings };
  GhNip46PairDialog *dialog = g_object_ref_sink(gh_nip46_pair_dialog_new(&config));
  AdwAlertDialog *alert = NULL;
  g_signal_connect(dialog, "confirmation-presented", G_CALLBACK(capture_confirmation), &alert);
  g_signal_connect(dialog, "closed", G_CALLBACK(mark_closed), closed);
  adw_dialog_present(ADW_DIALOG(dialog), window);
  GtkLabel *status;
  if (!bunker_uri) {
    status = GTK_LABEL(child(dialog, "qr_status"));
    gtk_editable_set_text(GTK_EDITABLE(child(dialog, "relay_row")), relay->url);
    g_signal_emit_by_name(child(dialog, "regenerate_button"), "clicked");
    GtkEditable *uri_row = GTK_EDITABLE(child(dialog, "uri_row"));
    WAIT_UNTIL(g_str_has_prefix(gtk_editable_get_text(uri_row), "nostrconnect://"), 10);
    g_autofree gchar *uri = g_strdup(gtk_editable_get_text(uri_row));
    if (connect) {
      const gchar *connect_args[] = { nak, "bunker", "connect", uri, NULL };
      *connect = launch(home, connect_args);
    } else {
      amber_answer_qr(relay, uri);
    }
  } else {
    status = GTK_LABEL(child(dialog, "bunker_details"));
    gtk_stack_set_visible_child_name(GTK_STACK(child(dialog, "mode_stack")), "bunker");
    gtk_editable_set_text(GTK_EDITABLE(child(dialog, "bunker_row")), bunker_uri);
    GtkWidget *button = child(dialog, "connect_button");
    g_assert_true(gtk_widget_get_sensitive(button));
    g_signal_emit_by_name(button, "clicked");
  }
  WAIT_UNTIL(alert != NULL, 30);
  WAIT_UNTIL(gtk_widget_get_mapped(GTK_WIDGET(alert)), 5);
  GtkButton *save = find_button(GTK_WIDGET(alert), "Save Remote Signer");
  g_assert_nonnull(save);
  *reqs_at_save = relay->reqs;
  g_signal_emit_by_name(save, "clicked");
  g_clear_object(&alert);
  /* Active at once on the live session; saving runs alongside. */
  g_assert_cmpint(gh_account_controller_get_state(accounts), ==, GH_ACCOUNT_STATE_ACTIVE);
  g_assert_cmpint(gh_account_controller_get_active_backend(accounts), ==,
                  GH_SIGNER_BACKEND_NIP46);
  g_assert_cmpint(gh_account_controller_get_remote_state(accounts), ==,
                  GH_REMOTE_SIGNER_READY);
  g_assert_nonnull(strstr(gtk_label_get_text(status), "Saving"));
  g_assert_true(gtk_widget_get_visible(child(dialog, "cancel_button")));
  WAIT_UNTIL(*closed, 30);
  g_assert_nonnull(strstr(gtk_label_get_text(status), "Connected as"));
  return dialog;
}

static GhAccountController *
new_controller(GSettings *settings, gboolean grotto_hangs)
{
  return gh_account_controller_new_full_with_credentials(settings, NULL,
    grotto_hangs ? hanging_grotto : no_grotto, grotto_hangs ? (gpointer)&hang : NULL, store);
}

static void
reset_settings(GSettings *settings)
{
  g_settings_set_string(settings, "current-npub", "");
  g_settings_set_string(settings, "current-backend", "grotto");
}

static gboolean
relay_has(WireRelay *relay, const gchar *json)
{
  NostrEvent *event = nostr_event_new();
  gchar id[65] = { 0 };
  g_assert_cmpint(nostr_event_deserialize_compact(event, json, NULL), ==, 1);
  g_assert_cmpint(nostr_event_validate(event, id), ==, NOSTR_EVENT_VALIDATION_OK);
  nostr_event_free(event);
  for (guint i = 0; i < relay->stored->len; i++)
    if (g_str_equal(((WireStored *)g_ptr_array_index(relay->stored, i))->id, id))
      return TRUE;
  return FALSE;
}

static void
assert_credential_saved(const gchar *signer_pubkey)
{
  Lookup found = { 0 };
  gh_nip46_credential_store_lookup_async(store, signer_pubkey, NULL, looked_up, &found);
  WAIT_UNTIL(found.done, 10);
  g_assert_no_error(found.error);
  g_assert_cmpstr(gh_nip46_credential_get_remote_signer_pubkey_hex(found.credential), ==,
                  signer_pubkey);
  gh_nip46_credential_free(found.credential);
}

/* bunker:// against nak on an unreliable relay (never EOSE, one
 * rate-limited NACK, later one dropped connection), with Grotto hanging. */
static void
test_bunker(void)
{
  WireRelay relay = { .serve = TRUE };
  relay_init(&relay);
  /* nak binds a unix socket under its home: keep the path short. */
  gchar *home = g_strdup("/tmp/ghpn-XXXXXX");
  g_assert_nonnull(g_mkdtemp(home));
  char *signer_pubkey = nostr_key_get_public(SIGNER_SECRET);
  relay.no_eose = TRUE;
  relay.exempt_pubkey = signer_pubkey;
  const gchar *bunker_args[] = { nak, "bunker", "--sec", SIGNER_SECRET,
    "--authorized-secrets", "groundhogpairing", relay.url, NULL };
  guint before = relay.reqs;
  GSubprocess *signer = launch(home, bunker_args);
  WAIT_UNTIL(relay.reqs > before, 10);

  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  reset_settings(settings);
  GhAccountController *accounts = new_controller(settings, TRUE);
  GtkWidget *window = gtk_window_new();
  gtk_window_present(GTK_WINDOW(window));
  g_autofree gchar *escaped = g_uri_escape_string(relay.url, NULL, FALSE);
  g_autofree gchar *uri = g_strdup_printf("bunker://%s?relay=%s&secret=groundhogpairing",
                                          signer_pubkey, escaped);
  relay.nack_events = 1;
  gboolean closed = FALSE;
  guint reqs_at_save = 0;
  GhNip46PairDialog *dialog = pair_and_save(accounts, settings, &relay, window, uri,
                                            NULL, home, &closed, &reqs_at_save);
  g_assert_cmpuint(relay.nacked, ==, 1);
  g_autofree gchar *expected = npub_of(signer_pubkey);
  g_autofree gchar *current = g_settings_get_string(settings, "current-npub");
  g_assert_cmpstr(current, ==, expected);
  /* The first send uses the live session: no keyring read, no new REQ. */
  g_autofree gchar *note = sign_note(accounts, signer_pubkey, "first note");
  g_assert_cmpuint(relay.reqs, ==, reqs_at_save);
  g_assert_cmpuint(gh_account_controller_get_credential_lookups_for_test(accounts), ==, 0);
  assert_credential_saved(signer_pubkey);
  gtk_window_destroy(GTK_WINDOW(window));
  g_object_unref(dialog);
  g_clear_object(&accounts);
  drain();

  /* Restart: restored from the keyring while Grotto still hangs; READY
   * means the signer answered a ping, not just that a relay is up. */
  accounts = new_controller(settings, TRUE);
  WAIT_UNTIL(gh_account_controller_get_state(accounts) == GH_ACCOUNT_STATE_ACTIVE, 20);
  WAIT_UNTIL(remote_state_is(accounts, GH_REMOTE_SIGNER_READY), 40);
  g_assert_false(hang.release);
  g_assert_cmpuint(gh_account_controller_get_credential_lookups_for_test(accounts), ==, 1);
  /* A connection dropped before the OK: re-sent on a new connection. */
  relay.drop_events = 1;
  g_autofree gchar *restored = sign_note(accounts, signer_pubkey, "after restart");
  g_assert_cmpuint(relay.dropped, ==, 1);
  publish_note(&relay, restored);
  g_assert_true(relay_has(&relay, restored));
#ifdef __APPLE__
  /* A locked Keychain at startup: listed but LOCKED, then Unlock. */
  g_clear_object(&accounts);
  drain();
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  g_assert_cmpint(SecKeychainLock(keychain), ==, errSecSuccess);
  accounts = new_controller(settings, TRUE);
  WAIT_UNTIL(remote_state_is(accounts, GH_REMOTE_SIGNER_LOCKED), 20);
  g_assert_cmpint(gh_account_controller_get_state(accounts), ==, GH_ACCOUNT_STATE_ACTIVE);
  g_assert_cmpint(SecKeychainUnlock(keychain, 4, "test", TRUE), ==, errSecSuccess);
#pragma clang diagnostic pop
  g_assert_true(gh_account_controller_unlock_remote(accounts));
  WAIT_UNTIL(remote_state_is(accounts, GH_REMOTE_SIGNER_READY), 40);
#endif
  g_clear_object(&accounts);
  reset_settings(settings);
  drain();
  stop(signer);
  free(signer_pubkey);
  remove_tree(home);
  g_free(home);
  relay_clear(&relay);
}

/* nostrconnect:// scanned by nak (replies with the secret). */
static void
test_qr(void)
{
  WireRelay relay = { .serve = TRUE };
  relay_init(&relay);
  gchar *home = g_strdup("/tmp/ghpn-XXXXXX");
  g_assert_nonnull(g_mkdtemp(home));
  char *signer_pubkey = nostr_key_get_public(SIGNER_SECRET);
  const gchar *bunker_args[] = { nak, "bunker", "--sec", SIGNER_SECRET, relay.url, NULL };
  guint before = relay.reqs;
  GSubprocess *signer = launch(home, bunker_args);
  WAIT_UNTIL(relay.reqs > before, 10);
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  reset_settings(settings);
  GhAccountController *accounts = new_controller(settings, FALSE);
  GtkWidget *window = gtk_window_new();
  gtk_window_present(GTK_WINDOW(window));
  gboolean closed = FALSE;
  guint reqs_at_save = 0;
  GSubprocess *connect = NULL;
  GhNip46PairDialog *dialog = pair_and_save(accounts, settings, &relay, window, NULL,
                                            &connect, home, &closed, &reqs_at_save);
  /* (nak connect opens its own REQs here, so REQs are not counted.) */
  g_autofree gchar *note = sign_note(accounts, signer_pubkey, "qr note");
  g_assert_cmpuint(gh_account_controller_get_credential_lookups_for_test(accounts), ==, 0);
  assert_credential_saved(signer_pubkey);
  gtk_window_destroy(GTK_WINDOW(window));
  g_object_unref(dialog);
  g_clear_object(&accounts);
  reset_settings(settings);
  drain();
  if (connect) stop(connect);
  stop(signer);
  free(signer_pubkey);
  remove_tree(home);
  g_free(home);
  relay_clear(&relay);
}

/* An Amber-style signer: a stray reply first, then a bare "ack", and a
 * signed event with a re-stamped created_at. */
static void
test_qr_amber(void)
{
  WireRelay relay = { .serve = TRUE, .on_event = amber_on_event };
  relay_init(&relay);
  amber_pubkey = nostr_key_get_public(AMBER_SECRET);
  amber_restamp = TRUE;
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  reset_settings(settings);
  GhAccountController *accounts = new_controller(settings, FALSE);
  GtkWidget *window = gtk_window_new();
  gtk_window_present(GTK_WINDOW(window));
  gboolean closed = FALSE;
  guint reqs_at_save = 0;
  GhNip46PairDialog *dialog = pair_and_save(accounts, settings, &relay, window, NULL,
                                            NULL, NULL, &closed, &reqs_at_save);
  g_autofree gchar *expected = npub_of(amber_pubkey);
  g_autofree gchar *current = g_settings_get_string(settings, "current-npub");
  g_assert_cmpstr(current, ==, expected);
  g_autofree gchar *note = sign_note(accounts, amber_pubkey, "restamped note");
  g_assert_cmpuint(relay.reqs, ==, reqs_at_save);
  g_assert_cmpuint(gh_account_controller_get_credential_lookups_for_test(accounts), ==, 0);
  g_assert_cmpuint(amber_requests, ==, 2); /* get_public_key, sign_event */
  assert_credential_saved(amber_pubkey);
  gtk_window_destroy(GTK_WINDOW(window));
  g_object_unref(dialog);
  g_clear_object(&accounts);
  reset_settings(settings);
  drain();
  free(amber_pubkey);
  amber_pubkey = NULL;
  relay_clear(&relay);
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
  g_mutex_init(&hang.lock);
  g_cond_init(&hang.cond);
  g_test_add_func("/groundhog/nip46/pair-nak/bunker-active-restore-unlock", test_bunker);
  g_test_add_func("/groundhog/nip46/pair-nak/qr-active", test_qr);
  g_test_add_func("/groundhog/nip46/pair-nak/qr-amber-ack", test_qr_amber);
  int status = g_test_run();
  release_grotto();
  teardown_store();
  g_free(nak);
  return status;
}
