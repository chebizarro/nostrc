/* GhNip46Session pairing against a real third-party signer: nak bunker
 * on a local relay (and on a second one for nostrconnect), through the
 * network-mode dispatcher as in the app: bunker:// and nostrconnect://
 * pairing, then get_public_key, sign_event and NIP-44 through the paired
 * session. Exits 77 (skipped)
 * when nak is not on PATH. (nostrc-p15n5.3) */
#include "gh-nip46-session.h"
#include "gh-nip46-session-private.h"
#include "wire-relay.h"
#include <nostr-event.h>
#include <nostr-keys.h>
#include <glib/gstdio.h>
#ifdef GROUNDHOG_NIP46_NET_TEST
#include "gh-net-session.h"
#include "gh-relay-net.h"
#endif

#define SIGNER_SECRET "7f4c11a9742721d66e40e321ca50b682c27f7422190c14a187525e69e604836a"

typedef struct { gboolean done; gchar *text; GError *error; } Result;

static void
pair_done(GObject *source, GAsyncResult *result, gpointer data)
{
  Result *out = data;
  out->text = gh_nip46_session_pair_finish(GH_NIP46_SESSION(source), result, &out->error);
  out->done = TRUE;
}

static void
call_done(GObject *source, GAsyncResult *result, gpointer data)
{
  Result *out = data;
  out->text = gh_nip46_session_call_finish(GH_NIP46_SESSION(source), result, &out->error);
  out->done = TRUE;
}

static void
wait_result(Result *result)
{
  gint64 deadline = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;
  while (!result->done && g_get_monotonic_time() < deadline)
    g_main_context_iteration(NULL, FALSE);
  g_assert_true(result->done);
}

static gchar *
rpc(GhNip46Session *session, const gchar *method, const gchar *const *params, gsize n)
{
  Result result = { 0 };
  gh_nip46_session_call_async(session, method, params, n, NULL, call_done, &result);
  wait_result(&result);
  g_assert_no_error(result.error);
  g_assert_nonnull(result.text);
  return result.text;
}

/* nak keeps its bunker state, and the control socket that nak bunker
 * connect talks to, under the home config dir: one home per run. */
static GSubprocess *
launch_signer(const gchar *home, const gchar *const *args)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
    G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  g_subprocess_launcher_setenv(launcher, "HO" "ME", home, TRUE);
  g_subprocess_launcher_unsetenv(launcher, "XDG_CONFIG_" "HO" "ME");
  GSubprocess *signer = g_subprocess_launcher_spawnv(launcher, args, &error);
  g_assert_no_error(error);
  return signer;
}

static void
stop_signer(GSubprocess *signer)
{
  g_subprocess_force_exit(signer);
  g_subprocess_wait(signer, NULL, NULL);
  g_object_unref(signer);
}

static void
await_reqs(WireRelay *relay, guint count)
{
  gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
  while (relay->reqs < count && g_get_monotonic_time() < deadline)
    g_main_context_iteration(NULL, FALSE);
  g_assert_cmpuint(relay->reqs, >=, count);
}

static GSubprocess *
start_bunker(const gchar *nak, const gchar *home, WireRelay *relay)
{
  const gchar *args[8];
  args[0] = nak;
  args[1] = "bunker";
  args[2] = "--sec";
  args[3] = SIGNER_SECRET;
  args[4] = "--authorized-secrets";
  args[5] = "groundhogpairing";
  args[6] = relay->url;
  args[7] = NULL;
  guint before = relay->reqs;
  GSubprocess *signer = launch_signer(home, args);
  await_reqs(relay, before + 1);
  return signer;
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

static void
exercise(GhNip46Session *session, const gchar *signer_pubkey)
{
  g_autofree gchar *pubkey = rpc(session, "get_public_key", NULL, 0);
  g_assert_cmpstr(pubkey, ==, signer_pubkey);
  gchar *unsigned_event = g_strdup_printf(
    "{'kind':1,'content':'hello from groundhog','tags':[],'created_at':%" G_GINT64_FORMAT "}",
    g_get_real_time() / G_USEC_PER_SEC);
  g_strdelimit(unsigned_event, "'", 34);
  const gchar *sign_params[] = { unsigned_event };
  g_autofree gchar *signed_json = rpc(session, "sign_event", sign_params, 1);
  g_free(unsigned_event);
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_signed(event, signed_json, NULL), ==,
                  NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpstr(nostr_event_get_pubkey(event), ==, signer_pubkey);
  g_assert_cmpstr(nostr_event_get_content(event), ==, "hello from groundhog");
  nostr_event_free(event);
  char *peer_secret = nostr_key_generate_private();
  char *peer = nostr_key_get_public(peer_secret);
  const gchar *enc_params[2];
  enc_params[0] = peer;
  enc_params[1] = "secret note";
  g_autofree gchar *ciphertext = rpc(session, "nip44_encrypt", enc_params, 2);
  const gchar *dec_params[2];
  dec_params[0] = peer;
  dec_params[1] = ciphertext;
  g_autofree gchar *plaintext = rpc(session, "nip44_decrypt", dec_params, 2);
  g_assert_cmpstr(plaintext, ==, "secret note");
  free(peer);
  free(peer_secret);
}

static void
test_bunker_uri(gconstpointer nak)
{
  WireRelay relay = { .serve = TRUE };
  relay_init(&relay);
  gchar *home = g_dir_make_tmp("groundhog-nak-XXXXXX", NULL);
  g_assert_nonnull(home);
  char *signer_pubkey = nostr_key_get_public(SIGNER_SECRET);
  GSubprocess *signer = start_bunker(nak, home, &relay);
  g_autofree gchar *escaped = g_uri_escape_string(relay.url, NULL, FALSE);
  g_autofree gchar *uri = g_strdup_printf("bunker://%s?relay=%s&secret=groundhogpairing",
                                          signer_pubkey, escaped);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhNip46Session) session = gh_nip46_session_new_bunker(uri, NULL, NULL,
    NULL, NULL, NULL, &error);
  g_assert_no_error(error);
  Result paired = { 0 };
  gh_nip46_session_pair_async(session, NULL, pair_done, &paired);
  wait_result(&paired);
  g_assert_no_error(paired.error);
  g_assert_cmpstr(paired.text, ==, signer_pubkey);
  exercise(session, signer_pubkey);
  gh_nip46_session_cancel(session);
  g_free(paired.text);
  stop_signer(signer);
  free(signer_pubkey);
  remove_tree(home);
  g_free(home);
  relay_clear(&relay);
}

static void
nostrconnect(const gchar *nak, gboolean other_relay)
{
  WireRelay relay = { .serve = TRUE };
  relay_init(&relay);
  WireRelay pairing = { .serve = TRUE };
  if (other_relay) relay_init(&pairing);
  gchar *home = g_dir_make_tmp("groundhog-nak-XXXXXX", NULL);
  g_assert_nonnull(home);
  char *signer_pubkey = nostr_key_get_public(SIGNER_SECRET);
  GSubprocess *signer = start_bunker(nak, home, &relay);
  const gchar *relays[2];
  relays[0] = other_relay ? pairing.url : relay.url;
  relays[1] = NULL;
  g_autofree gchar *uri = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GhNip46Session) session = gh_nip46_session_new_qr(relays, NULL, NULL,
    NULL, NULL, NULL, &uri, &error);
  g_assert_no_error(error);
  Result paired = { 0 };
  gh_nip46_session_pair_async(session, NULL, pair_done, &paired);
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  while (!gh_nip46_session_is_ready(session) && g_get_monotonic_time() < deadline)
    g_main_context_iteration(NULL, FALSE);
  g_assert_true(gh_nip46_session_is_ready(session));
  const gchar *args[5];
  args[0] = nak;
  args[1] = "bunker";
  args[2] = "connect";
  args[3] = uri;
  args[4] = NULL;
  GSubprocess *connect = launch_signer(home, args);
  wait_result(&paired);
  g_assert_no_error(paired.error);
  g_assert_cmpstr(paired.text, ==, signer_pubkey);
  exercise(session, signer_pubkey);
  gh_nip46_session_cancel(session);
  g_free(paired.text);
  stop_signer(connect);
  stop_signer(signer);
  free(signer_pubkey);
  remove_tree(home);
  g_free(home);
  relay_clear(&relay);
  if (other_relay) relay_clear(&pairing);
}

static void
test_nostrconnect(gconstpointer nak)
{
  nostrconnect(nak, FALSE);
}

static void
test_nostrconnect_other_relay(gconstpointer nak)
{
  nostrconnect(nak, TRUE);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  gchar *nak = g_find_program_in_path("nak");
  if (nak == NULL)
    return 77; /* skipped: nak is not installed */
#ifdef GROUNDHOG_NIP46_NET_TEST
  GhNetSession *network = gh_net_session_new(NULL); /* SYSTEM mode, as the app */
  gh_relay_net_install(network);
#endif
  g_test_add_data_func("/groundhog/nip46/nak/bunker-uri", nak, test_bunker_uri);
  g_test_add_data_func("/groundhog/nip46/nak/nostrconnect", nak, test_nostrconnect);
  g_test_add_data_func("/groundhog/nip46/nak/nostrconnect-other-relay", nak,
                       test_nostrconnect_other_relay);
  return g_test_run();
}
