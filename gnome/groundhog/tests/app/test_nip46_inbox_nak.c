/* Bulk NIP-17 decrypt through a real remote signer, on the app's own path
 * (relogin-signer-groups): `nak bunker` on a local relay, a GhNip46Session
 * paired with it over bunker://, adopted by the real GhAccountController
 * (as the pairing dialog does), and the real GhAccountRelays + GhDmInbox
 * reading N gift wraps from a second local relay, every connection through
 * the network-mode dispatcher (GhNet, System mode) as in the app.
 *
 * It measures what the owner saw: decrypt throughput and how many WebSocket
 * connections the signer relay accepted for it. Each wrap costs two NIP-46
 * nip44_decrypt round trips. Before the fix every request dialled its own
 * connection (connect, EVENT, OK, disconnect) and the inbox unwrapped one
 * wrap at a time: ~1 wrap/s on a real relay, one connection per request.
 * Now the requests are pipelined over the session's single connection.
 *
 * Exits 77 (skipped) when nak is not on PATH. Waits iterate the main
 * context; deadlines are failure bounds only. */
#include "gh-account-controller.h"
#include "gh-account-relays.h"
#include "gh-conversation-store.h"
#include "gh-dm-inbox.h"
#include "gh-identity.h"
#include "gh-nip46-session.h"
#include "gh-nip46-session-private.h"
#include "gh-net-session.h"
#include "gh-relay-net.h"
#include "wire-relay.h"

#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-tag.h>
#include <nostr-utils.h>
#include <nostr/nip19/nip19.h>
#include <nostr/nip44/nip44.h>
#include <nostr/nip59/nip59.h>
#include <glib/gstdio.h>

#define ACCOUNT_SECRET "7f4c11a9742721d66e40e321ca50b682c27f7422190c14a187525e69e604836a"
#define PEER_SECRET    "0000000000000000000000000000000000000000000000000000000000000001"
#define WRAP_SECRET    "0000000000000000000000000000000000000000000000000000000000000004"
#define DEFAULT_WRAPS  200

typedef struct { gboolean done; gchar *text; GError *error; } Result;

static void
pair_done(GObject *source, GAsyncResult *result, gpointer data)
{
  Result *out = data;
  out->text = gh_nip46_session_pair_finish(GH_NIP46_SESSION(source), result, &out->error);
  out->done = TRUE;
}

static void
spin_until(gboolean (*predicate)(gpointer), gpointer data, guint seconds, const gchar *what)
{
  gint64 deadline = g_get_monotonic_time() + (gint64)seconds * G_USEC_PER_SEC;
  while (!predicate(data) && g_get_monotonic_time() < deadline)
    g_main_context_iteration(NULL, FALSE);
  if (!predicate(data))
    g_error("timed out waiting for %s", what);
}

static gboolean result_done(gpointer data) { return ((Result *)data)->done; }
static gboolean relay_has_req(gpointer data) { return ((WireRelay *)data)->reqs > 0; }

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

static gchar *
sign_json(NostrEvent *event, const gchar *secret)
{
  g_assert_cmpint(nostr_event_sign(event, secret), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  gchar *out = g_strdup(json);
  free(json);
  return out;
}

/* A NIP-17 kind-14 from the peer to the account, sealed and gift wrapped. */
static gchar *
craft_wrap(const gchar *peer_pub, const gchar *account_pub, gint64 created_at, guint n)
{
  NostrEvent *rumor = nostr_event_new();
  nostr_event_set_kind(rumor, 14);
  nostr_event_set_pubkey(rumor, peer_pub);
  nostr_event_set_created_at(rumor, created_at);
  g_autofree gchar *text = g_strdup_printf("backfill message %u", n);
  nostr_event_set_content(rumor, text);
  nostr_event_set_tags(rumor, nostr_tags_new(1, nostr_tag_new("p", account_pub, NULL)));
  rumor->id = nostr_event_get_id(rumor);
  char *rumor_json = nostr_event_serialize_compact(rumor);
  nostr_event_free(rumor);
  guint8 sk[32], pk[32];
  g_assert_true(nostr_hex2bin(sk, PEER_SECRET, sizeof sk));
  g_assert_true(nostr_hex2bin(pk, account_pub, sizeof pk));
  char *ciphertext = NULL;
  g_assert_cmpint(nostr_nip44_encrypt_v2(sk, pk, (const guint8 *)rumor_json,
                                          strlen(rumor_json), &ciphertext), ==, 0);
  free(rumor_json);
  NostrEvent *seal = nostr_event_new();
  nostr_event_set_kind(seal, 13);
  nostr_event_set_pubkey(seal, peer_pub);
  nostr_event_set_content(seal, ciphertext);
  nostr_event_set_created_at(seal, created_at);
  nostr_event_set_tags(seal, nostr_tags_new(0));
  free(ciphertext);
  g_assert_cmpint(nostr_event_sign(seal, PEER_SECRET), ==, 0);
  guint8 ephemeral[32];
  g_assert_true(nostr_hex2bin(ephemeral, WRAP_SECRET, sizeof ephemeral));
  NostrEvent *wrap = nostr_nip59_wrap_with_key(seal, account_pub, ephemeral);
  nostr_event_free(seal);
  g_assert_nonnull(wrap);
  char *json = nostr_event_serialize_compact(wrap);
  nostr_event_free(wrap);
  gchar *out = g_strdup(json);
  free(json);
  return out;
}

static GPtrArray *
no_identities(gpointer data, GError **error)
{
  (void)data; (void)error;
  return g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
}

static gboolean
listed(gpointer data)
{
  return gh_account_controller_get_state(data) != GH_ACCOUNT_STATE_DISCOVERING;
}

typedef struct { GhDmInbox *inbox; guint want; } Admitted;

static gboolean
all_admitted(gpointer data)
{
  Admitted *a = data;
  GhDmInboxCounters counters;
  gh_dm_inbox_get_counters(a->inbox, &counters);
  return counters.admitted + counters.rejected + counters.deferred >= a->want;
}

static void
test_bulk_decrypt(gconstpointer nak)
{
  guint wraps = DEFAULT_WRAPS;
  const gchar *env = g_getenv("GROUNDHOG_NIP46_INBOX_WRAPS");
  if (env && *env) wraps = (guint)g_ascii_strtoull(env, NULL, 10);
  g_autofree gchar *account_pub = NULL, *peer_pub = NULL;
  { char *k = nostr_key_get_public(ACCOUNT_SECRET); account_pub = g_strdup(k); free(k); }
  { char *k = nostr_key_get_public(PEER_SECRET); peer_pub = g_strdup(k); free(k); }

  WireRelay signer_relay = { .serve = TRUE };
  WireRelay inbox_relay = { .serve = TRUE };
  relay_init(&signer_relay);
  relay_init(&inbox_relay);

  /* The account's inbox list and the backlog, on the inbox relay. */
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  NostrEvent *list = nostr_event_new();
  nostr_event_set_kind(list, 10050);
  nostr_event_set_created_at(list, now - 3600);
  nostr_event_set_content(list, "");
  nostr_event_set_tags(list, nostr_tags_new(1, nostr_tag_new("relay", inbox_relay.url, NULL)));
  g_autofree gchar *list_json = sign_json(list, ACCOUNT_SECRET);
  nostr_event_free(list);
  wire_relay_inject(&inbox_relay, list_json);
  for (guint i = 0; i < wraps; i++) {
    g_autofree gchar *wrap = craft_wrap(peer_pub, account_pub, now - 7200 + i, i);
    wire_relay_inject(&inbox_relay, wrap);
  }

  /* nak bunker for the account, and a paired session (the pairing dialog). */
  gchar *home = g_dir_make_tmp("groundhog-nak-inbox-XXXXXX", NULL);
  g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
    G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  g_subprocess_launcher_setenv(launcher, "HO" "ME", home, TRUE);
  g_subprocess_launcher_unsetenv(launcher, "XDG_CONFIG_" "HO" "ME");
  g_autoptr(GError) error = NULL;
  GSubprocess *signer = g_subprocess_launcher_spawn(launcher, &error, nak, "bunker",
    "--sec", ACCOUNT_SECRET, "--authorized-secrets", "groundhogpairing",
    signer_relay.url, NULL);
  g_assert_no_error(error);
  spin_until(relay_has_req, &signer_relay, 10, "nak bunker REQ");
  g_autofree gchar *escaped = g_uri_escape_string(signer_relay.url, NULL, FALSE);
  g_autofree gchar *uri = g_strdup_printf("bunker://%s?relay=%s&secret=groundhogpairing",
                                          account_pub, escaped);
  g_autoptr(GhNip46Session) session = gh_nip46_session_new_bunker(uri, NULL, NULL,
    NULL, NULL, NULL, &error);
  g_assert_no_error(error);
  Result paired = { 0 };
  gh_nip46_session_pair_async(session, NULL, pair_done, &paired);
  spin_until(result_done, &paired, 20, "pairing");
  g_assert_no_error(paired.error);
  g_assert_cmpstr(paired.text, ==, account_pub);
  g_free(paired.text);

  /* The app's account stack. */
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  const gchar *discovery[] = { inbox_relay.url, NULL };
  g_settings_set_strv(settings, "discovery-relays", discovery);
  g_settings_set_string(settings, "current-backend", "grotto");
  g_settings_set_string(settings, "current-npub", "");
  g_settings_set_int(settings, "backend-migration-version", 1);
  GhAccountController *accounts = gh_account_controller_new_full(settings, NULL,
                                                                 no_identities, NULL);
  spin_until(listed, accounts, 10, "account listing");
  guint8 raw[32];
  g_assert_true(nostr_hex2bin(raw, account_pub, sizeof raw));
  gchar *npub = NULL;
  g_assert_cmpint(nostr_nip19_encode_npub(raw, &npub), ==, 0);
  guint connections_before = signer_relay.connections->len;
  gint64 started = g_get_monotonic_time();
  g_assert_true(gh_account_controller_adopt_remote(accounts, npub, session, &error));
  g_assert_no_error(error);
  g_assert_cmpint(gh_account_controller_get_active_backend(accounts), ==,
                  GH_SIGNER_BACKEND_NIP46);
  /* What the account store does once the encrypted store is open. */
  gh_account_controller_set_remote_storage_ready(accounts,
    gh_account_controller_get_generation(accounts), TRUE);
  GhAccountRelays *relays = gh_account_relays_new(accounts, settings, NULL, NULL);
  GhConversationStore *store = gh_conversation_store_new();
  gchar *state_dir = g_dir_make_tmp("groundhog-nip46-inbox-XXXXXX", NULL);
  GhDmInbox *inbox = gh_dm_inbox_new(accounts, relays, store, state_dir, NULL, NULL, NULL);
  Admitted want = { inbox, wraps };
  spin_until(all_admitted, &want, 600, "every wrap unwrapped");
  gdouble seconds = (g_get_monotonic_time() - started) / (gdouble)G_USEC_PER_SEC;
  GhDmInboxCounters counters;
  gh_dm_inbox_get_counters(inbox, &counters);
  guint connections = signer_relay.connections->len - connections_before;
  g_printerr("nip46 bulk decrypt: %u wraps (%u signer requests) in %.2f s = %.1f wraps/s; "
             "signer relay connections opened: %u; admitted %u rejected %u deferred %u\n",
             wraps, 2 * wraps, seconds, wraps / seconds, connections,
             counters.admitted, counters.rejected, counters.deferred);
  g_assert_cmpuint(counters.admitted, ==, wraps);
  /* The session's one connection carried every request: nothing new was
   * dialled for them (the adopted session was already connected). */
  g_assert_cmpuint(connections, ==, 0);
  g_assert_cmpuint(signer_relay.connections->len, ==, 2); /* nak's and the session's */

  g_object_run_dispose(G_OBJECT(inbox));
  g_object_unref(inbox);
  g_object_run_dispose(G_OBJECT(relays));
  g_object_unref(relays);
  g_object_unref(store);
  g_object_run_dispose(G_OBJECT(accounts));
  g_object_unref(accounts);
  gh_nip46_session_cancel(session);
  free(npub);
  g_subprocess_force_exit(signer);
  g_subprocess_wait(signer, NULL, NULL);
  g_object_unref(signer);
  remove_tree(home);
  g_free(home);
  remove_tree(state_dir);
  g_free(state_dir);
  relay_clear(&inbox_relay);
  relay_clear(&signer_relay);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  gchar *nak = g_find_program_in_path("nak");
  if (nak == NULL)
    return 77; /* skipped: nak is not installed */
  GhNetSession *network = gh_net_session_new(NULL); /* SYSTEM mode, as the app */
  gh_relay_net_install(network);
  g_test_add_data_func("/groundhog/nip46/nak/inbox-bulk-decrypt", nak, test_bulk_decrypt);
  int status = g_test_run();
  gh_relay_net_install(NULL);
  g_object_unref(network);
  g_free(nak);
  return status;
}
