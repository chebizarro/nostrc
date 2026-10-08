/* Real local kind-24133 REQ/EOSE, signed EVENT/OK and NIP-42 client-key AUTH. */
#include "gh-nip46-session.h"
#include "wire-relay.h"
#include <nostr-keys.h>
#include <nostr-utils.h>
#include <nostr/nip44/nip44.h>
#include <nostr/nip46/nip46_msg.h>
#include <nostr/nip46/nip46_envelope.h>
#ifdef GROUNDHOG_NIP46_TOR_TEST
#include "gh-net-session.h"
#include "gh-relay-net.h"
#include "socks5-fixture.h"
#endif

#define CLIENT_SECRET "1111111111111111111111111111111111111111111111111111111111111111"

typedef struct { gboolean done; gchar *text; GError *error; } Result;

static void
completed(GObject *source, GAsyncResult *result, gpointer data)
{
  Result *out = data;
  out->text = gh_nip46_session_call_finish(GH_NIP46_SESSION(source), result,
                                            &out->error);
  out->done = TRUE;
}

static gboolean
ready(gpointer data)
{ return gh_nip46_session_is_ready(data); }
static gboolean
received(gpointer data)
{ return ((WireRelay *)data)->events > 0; }
static gboolean
finished(gpointer data)
{ return ((Result *)data)->done; }

static void
wait_until(gboolean (*predicate)(gpointer), gpointer data)
{
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  while (!predicate(data) && g_get_monotonic_time() < deadline)
    g_main_context_iteration(NULL, FALSE);
  g_assert_true(predicate(data));
}

static void
test_wire(void)
{
  WireRelay relay = { .serve = TRUE, .record = TRUE, .require_auth = TRUE };
  relay_init(&relay);
  char *signer_secret = nostr_key_generate_private();
  char *signer_pubkey = nostr_key_get_public(signer_secret);
  const gchar *relays[] = { relay.url, NULL };
  g_autoptr(GError) error = NULL;
  g_autoptr(GhNip46Session) session = gh_nip46_session_new(CLIENT_SECRET,
    signer_pubkey, relays, NULL, NULL, NULL, NULL, NULL, &error);
  g_assert_no_error(error);
  g_assert_nonnull(session);
  gh_nip46_session_start(session);
  wait_until(ready, session);
  g_assert_cmpuint(relay.reqs, >=, 1);
  Result result = { 0 };
  gh_nip46_session_call_async(session, "get_public_key", NULL, 0, NULL,
                              completed, &result);
  wait_until(received, &relay);
  g_assert_cmpuint(relay.stored->len, ==, 1);
  WireStored *request_event = g_ptr_array_index(relay.stored, 0);
  g_assert_cmpint(nostr_event_get_kind(request_event->event), ==, 24133);
  g_assert_cmpstr(nostr_event_get_pubkey(request_event->event), ==,
                  gh_nip46_session_get_client_pubkey(session));
  guint8 signer_sk[32], client_pk[32], *plain = NULL;
  size_t length = 0;
  g_assert_true(nostr_hex2bin(signer_sk, signer_secret, sizeof signer_sk));
  g_assert_true(nostr_hex2bin(client_pk, gh_nip46_session_get_client_pubkey(session),
                               sizeof client_pk));
  g_assert_cmpint(nostr_nip44_decrypt_v2(signer_sk, client_pk,
    nostr_event_get_content(request_event->event), &plain, &length), ==, 0);
  g_autofree gchar *request_json = g_strndup((gchar *)plain, length);
  free(plain);
  NostrNip46Request request = { 0 };
  g_assert_cmpint(nostr_nip46_request_parse(request_json, &request), ==, 0);
  g_assert_cmpstr(request.method, ==, "get_public_key");
  g_autofree gchar *quoted = g_strdup_printf("\"%s\"", signer_pubkey);
  char *response_json = nostr_nip46_response_build_ok(request.id, quoted);
  char *ciphertext = NULL;
  g_assert_cmpint(nostr_nip44_encrypt_v2(signer_sk, client_pk,
    (guint8 *)response_json, strlen(response_json), &ciphertext), ==, 0);
  NostrEvent *response = NULL;
  g_assert_cmpint(nostr_nip46_build_response_event(signer_pubkey,
    gh_nip46_session_get_client_pubkey(session), response_json, &response), ==, 0);
  nostr_event_set_content(response, ciphertext);
  g_assert_cmpint(nostr_event_sign(response, signer_secret), ==, 0);
  char *response_event_json = nostr_event_serialize_compact(response);
  wire_relay_inject(&relay, response_event_json);
  wait_until(finished, &result);
  g_assert_no_error(result.error);
  g_assert_cmpstr(result.text, ==, signer_pubkey);
  g_assert_cmpuint(relay.auth_ok, >=, 2); /* REQ and private publish channel */
  for (guint i = 0; i < relay.auth_pubkeys->len; i++)
    g_assert_cmpstr(g_ptr_array_index(relay.auth_pubkeys, i), ==,
                    gh_nip46_session_get_client_pubkey(session));
  gh_nip46_session_cancel(session);
  g_assert_false(gh_nip46_session_is_ready(session));
  g_free(result.text);
  free(response_event_json);
  nostr_event_free(response);
  free(ciphertext);
  free(response_json);
  nostr_nip46_request_free(&request);
  free(signer_pubkey);
  free(signer_secret);
  relay_clear(&relay);
}

#ifdef GROUNDHOG_NIP46_TOR_TEST
static void
test_tor_socks_and_mode_switch(void)
{
  WireRelay relay = { .serve = TRUE };
  relay_init(&relay);
  const gchar *port_text = strrchr(relay.url, ':');
  g_assert_nonnull(port_text);
  guint16 relay_port = (guint16)g_ascii_strtoull(port_text + 1, NULL, 10);
  Socks5Fixture *socks = socks5_fixture_new();
  socks5_fixture_set_domain_port(socks, relay_port);
  g_autoptr(GhNetSession) network = gh_net_session_new(NULL);
  gh_net_session_set_mode(network, GH_NET_MODE_TOR,
                          socks5_fixture_address(socks));
  gh_relay_net_install(network);
  char *signer_secret = nostr_key_generate_private();
  char *signer_pubkey = nostr_key_get_public(signer_secret);
  const gchar *onion = "ws://signerrelaytest.onion/relay";
  const gchar *relays[] = { onion, NULL };
  g_autoptr(GError) error = NULL;
  g_autoptr(GhNip46Session) session = gh_nip46_session_new(CLIENT_SECRET,
    signer_pubkey, relays, NULL, NULL, NULL, NULL, NULL, &error);
  g_assert_no_error(error);
  gh_nip46_session_start(session);
  wait_until(ready, session);
  GPtrArray *requests = socks5_fixture_requests(socks);
  g_assert_cmpuint(requests->len, >=, 1);
  Socks5Request *request = g_ptr_array_index(requests, 0);
  g_assert_cmpint(request->atyp, ==, 3); /* proxy, not local DNS */
  g_assert_cmpstr(request->host, ==, "signerrelaytest.onion");
  g_assert_cmpuint(relay.reqs, >=, 1);
  guint old_reqs = relay.reqs;
  gh_net_session_set_mode(network, GH_NET_MODE_NONE, NULL);
  gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
  while (gh_nip46_session_is_ready(session) && g_get_monotonic_time() < deadline)
    g_main_context_iteration(NULL, FALSE);
  g_assert_false(gh_nip46_session_is_ready(session));
  g_assert_cmpuint(relay.reqs, ==, old_reqs); /* no direct .onion fallback */
  gh_nip46_session_cancel(session);
  gh_relay_net_install(NULL);
  free(signer_pubkey);
  free(signer_secret);
  socks5_fixture_free(socks);
  relay_clear(&relay);
}
#endif

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/nip46/wire-local-relay", test_wire);
#ifdef GROUNDHOG_NIP46_TOR_TEST
  g_test_add_func("/groundhog/nip46/tor-socks-mode-switch",
                  test_tor_socks_and_mode_switch);
#endif
  return g_test_run();
}
