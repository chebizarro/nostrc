#include "gh-test-bunker.h"
#include "gh-nip46-session-private.h"
#include <nostr/nip46/nip46_uri.h>

#define RELAY "wss://bunker.test.invalid"
#define CLIENT_SECRET "1111111111111111111111111111111111111111111111111111111111111111"

static const gchar *relays[] = { RELAY, NULL };

typedef struct {
  gboolean done;
  guint calls;
  gchar *result;
  GError *error;
} Await;

static void
call_done(GObject *source, GAsyncResult *result, gpointer data)
{
  Await *wait = data;
  wait->result = gh_nip46_session_call_finish(GH_NIP46_SESSION(source), result,
                                               &wait->error);
  wait->done = TRUE;
  wait->calls++;
}

static void
pair_done(GObject *source, GAsyncResult *result, gpointer data)
{
  Await *wait = data;
  wait->result = gh_nip46_session_pair_finish(GH_NIP46_SESSION(source), result,
                                               &wait->error);
  wait->done = TRUE;
  wait->calls++;
}

static void
until(gboolean (*predicate)(gpointer), gpointer data)
{
  gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
  while (!predicate(data) && g_get_monotonic_time() < deadline)
    g_main_context_iteration(NULL, FALSE);
  g_assert_true(predicate(data));
}

static gboolean
has_publish(gpointer data)
{ return ((TestBunker *)data)->publishes->len > 0; }
static gboolean
await_done(gpointer data)
{ return ((Await *)data)->done; }
typedef struct { TestBunker *bunker; guint count; } PublishCount;
static gboolean
publish_count(gpointer data)
{
  PublishCount *want = data;
  return want->bunker->publishes->len >= want->count;
}

static GhNip46Session *
new_session(TestBunker *bunker)
{
  bunker_init(bunker, CLIENT_SECRET);
  g_autoptr(GError) error = NULL;
  GhNip46Session *session = gh_nip46_session_new(CLIENT_SECRET,
    bunker->signer_pubkey, relays, &bunker_scope_transport, NULL,
    &bunker_publish_transport, NULL, bunker, &error);
  g_assert_no_error(error);
  g_assert_nonnull(session);
  return session;
}

static void
reply_result(TestBunker *bunker, const gchar *value)
{
  g_autofree gchar *request_json = bunker_request_json(bunker);
  NostrNip46Request request = { 0 };
  g_assert_cmpint(nostr_nip46_request_parse(request_json, &request), ==, 0);
  g_autofree gchar *quoted = g_strdup_printf("\"%s\"", value);
  char *reply = nostr_nip46_response_build_ok(request.id, quoted);
  g_assert_nonnull(reply);
  bunker_reply(bunker, reply);
  free(reply);
  nostr_nip46_request_free(&request);
}

static void
test_request_wire_and_eose(void)
{
  TestBunker bunker;
  g_autoptr(GhNip46Session) session = new_session(&bunker);
  gh_nip46_session_start(session);
  g_assert_cmpuint(bunker.scopes->len, ==, 1);
  Await wait = { 0 };
  gh_nip46_session_call_async(session, "get_public_key", NULL, 0, NULL,
                              call_done, &wait);
  while (g_main_context_iteration(NULL, FALSE)) {}
  g_assert_cmpuint(bunker.publishes->len, ==, 0);
  g_assert_false(gh_nip46_session_is_ready(session));
  bunker_eose(&bunker);
  g_assert_true(gh_nip46_session_is_ready(session));
  until(has_publish, &bunker);
  g_autofree gchar *request_json = bunker_request_json(&bunker);
  NostrNip46Request request = { 0 };
  g_assert_cmpint(nostr_nip46_request_parse(request_json, &request), ==, 0);
  g_assert_cmpstr(request.method, ==, "get_public_key");
  nostr_nip46_request_free(&request);
  bunker_accept(&bunker);
  reply_result(&bunker, bunker.user_pubkey);
  until(await_done, &wait);
  g_assert_no_error(wait.error);
  g_assert_cmpstr(wait.result, ==, bunker.user_pubkey);
  g_free(wait.result);
  /* A duplicate terminal response cannot complete a second operation. */
  reply_result(&bunker, bunker.user_pubkey);
  gh_nip46_session_cancel(session);
  /* A response delivered through the old fixture after teardown is inert. */
  reply_result(&bunker, bunker.user_pubkey);
  bunker_clear(&bunker);
}

static void
test_wrong_author_and_cancel(void)
{
  TestBunker bunker;
  g_autoptr(GhNip46Session) session = new_session(&bunker);
  gh_nip46_session_start(session);
  bunker_eose(&bunker);
  Await wait = { 0 };
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  gh_nip46_session_call_async(session, "sign_event", NULL, 0,
                              cancellable, call_done, &wait);
  until(has_publish, &bunker);
  bunker_accept(&bunker);
  g_autofree gchar *request_json = bunker_request_json(&bunker);
  NostrNip46Request request = { 0 };
  g_assert_cmpint(nostr_nip46_request_parse(request_json, &request), ==, 0);
  char *reply = nostr_nip46_response_build_ok(request.id, "\"bad\"");
  char *wrong = nostr_key_generate_private();
  bunker_reply_from(&bunker, wrong, bunker.client_pubkey, reply);
  bunker_reply_from(&bunker, bunker.signer_secret,
                    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                    reply);
  char *unknown = nostr_nip46_response_build_ok("unknown-id", "\"bad\"");
  bunker_reply(&bunker, unknown);
  free(unknown);
  while (g_main_context_iteration(NULL, FALSE)) {}
  g_assert_false(wait.done);
  g_cancellable_cancel(cancellable);
  until(await_done, &wait);
  g_assert_error(wait.error, GH_NIP46_SESSION_ERROR,
                 GH_NIP46_SESSION_ERROR_CANCELLED);
  g_clear_error(&wait.error);
  /* A late valid response after cancellation is ignored. */
  bunker_reply(&bunker, reply);
  nostr_nip46_request_free(&request);
  free(reply); free(wrong);
  gh_nip46_session_cancel(session);
  bunker_clear(&bunker);
}

static void
test_late_replies_after_teardown(void)
{
  TestBunker bunker;
  GhNip46Session *session = new_session(&bunker);
  gpointer weak_session = session;
  g_object_add_weak_pointer(G_OBJECT(session), &weak_session);
  gh_nip46_session_start(session);
  bunker_eose(&bunker);
  Await wait = { 0 };
  gh_nip46_session_call_async(session, "get_public_key", NULL, 0, NULL,
                              call_done, &wait);
  until(has_publish, &bunker);
  bunker_accept(&bunker);
  g_autofree gchar *request_json = bunker_request_json(&bunker);
  NostrNip46Request request = { 0 };
  g_assert_cmpint(nostr_nip46_request_parse(request_json, &request), ==, 0);
  char *reply = nostr_nip46_response_build_ok(request.id, "\"late\"");
  g_assert_nonnull(reply);
  gh_nip46_session_cancel(session);
  g_clear_object(&session); /* exercise disposal before the valid late reply */
  until(await_done, &wait);
  g_assert_cmpuint(wait.calls, ==, 1);
  g_assert_error(wait.error, GH_NIP46_SESSION_ERROR,
                 GH_NIP46_SESSION_ERROR_CANCELLED);
  g_clear_error(&wait.error);
  g_assert_null(wait.result);
  g_assert_null(weak_session);
  BunkerHandle *scope = g_ptr_array_index(bunker.scopes, 0);
  g_assert_true(scope->closed);
  bunker_reply(&bunker, reply); /* matching id, correct author and recipient */
  while (g_main_context_iteration(NULL, FALSE)) {}
  g_assert_cmpuint(wait.calls, ==, 1);
  g_assert_null(wait.result);
  g_assert_null(wait.error);
  nostr_nip46_request_free(&request);
  free(reply);
  bunker_clear(&bunker);
}

static void
test_late_pair_reply_after_cancel(void)
{
  TestBunker bunker;
  bunker_init(&bunker, CLIENT_SECRET);
  g_autoptr(GError) error = NULL;
  gchar *uri = NULL;
  GhNip46Session *session = gh_nip46_session_new_qr(relays,
    &bunker_scope_transport, NULL, &bunker_publish_transport, NULL,
    &bunker, &uri, &error);
  g_assert_no_error(error);
  g_assert_nonnull(session);
  gpointer weak_session = session;
  g_object_add_weak_pointer(G_OBJECT(session), &weak_session);
  g_free(bunker.client_pubkey);
  bunker.client_pubkey = g_strdup(gh_nip46_session_get_client_pubkey(session));
  NostrNip46ConnectURI parsed = { 0 };
  g_assert_cmpint(nostr_nip46_uri_parse_connect(uri, &parsed), ==, 0);
  Await wait = { 0 };
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  gh_nip46_session_pair_async(session, cancellable, pair_done, &wait);
  bunker_eose(&bunker);
  g_assert_true(gh_nip46_session_is_ready(session));
  g_cancellable_cancel(cancellable);
  until(await_done, &wait);
  g_assert_cmpuint(wait.calls, ==, 1);
  g_assert_error(wait.error, GH_NIP46_SESSION_ERROR,
                 GH_NIP46_SESSION_ERROR_CANCELLED);
  g_clear_error(&wait.error);
  g_assert_null(wait.result);
  g_clear_object(&session);
  g_assert_null(weak_session);
  BunkerHandle *scope = g_ptr_array_index(bunker.scopes, 0);
  g_assert_true(scope->closed);
  g_autofree gchar *secret_json = g_strdup_printf("\"%s\"", parsed.secret);
  char *reply = nostr_nip46_response_build_ok("connect", secret_json);
  g_assert_nonnull(reply);
  bunker_reply(&bunker, reply); /* valid secret from the real signer, after cancel */
  while (g_main_context_iteration(NULL, FALSE)) {}
  g_assert_cmpuint(wait.calls, ==, 1);
  g_assert_cmpuint(bunker.publishes->len, ==, 0);
  g_assert_null(wait.result);
  g_assert_null(wait.error);
  free(reply);
  nostr_nip46_uri_connect_free(&parsed);
  g_free(uri);
  bunker_clear(&bunker);
}

static guint launched;
static gboolean
open_auth_url(GhNip46Session *session, const gchar *url, gpointer data)
{
  (void)session; (void)data;
  g_assert_cmpstr(url, ==, "https://bunker.test.invalid/approve");
  launched++;
  return TRUE;
}

static void
test_auth_url_same_id(void)
{
  TestBunker bunker;
  g_autoptr(GhNip46Session) session = new_session(&bunker);
  gh_nip46_session_set_auth_url_handler(session, open_auth_url, NULL);
  gh_nip46_session_start(session);
  bunker_eose(&bunker);
  Await wait = { 0 };
  gh_nip46_session_call_async(session, "sign_event", NULL, 0, NULL,
                              call_done, &wait);
  until(has_publish, &bunker);
  bunker_accept(&bunker);
  g_autofree gchar *request_json = bunker_request_json(&bunker);
  NostrNip46Request request = { 0 };
  g_assert_cmpint(nostr_nip46_request_parse(request_json, &request), ==, 0);
  g_autofree gchar *auth = g_strdup_printf("{\"id\":\"%s\",\"result\":\"auth_url\","
    "\"error\":\"https://bunker.test.invalid/approve\"}", request.id);
  launched = 0;
  bunker_reply(&bunker, auth);
  bunker_reply(&bunker, auth);
  g_assert_cmpuint(launched, ==, 1);
  g_assert_false(wait.done);
  reply_result(&bunker, "signed");
  until(await_done, &wait);
  g_assert_no_error(wait.error);
  g_assert_cmpstr(wait.result, ==, "signed");
  g_free(wait.result);
  nostr_nip46_request_free(&request);
  gh_nip46_session_cancel(session);
  bunker_clear(&bunker);
}

static void
test_qr_pairing(void)
{
  TestBunker bunker;
  bunker_init(&bunker, CLIENT_SECRET);
  g_autoptr(GError) error = NULL;
  gchar *uri = NULL;
  g_autoptr(GhNip46Session) session = gh_nip46_session_new_qr(relays,
    &bunker_scope_transport, NULL, &bunker_publish_transport, NULL,
    &bunker, &uri, &error);
  g_assert_no_error(error);
  g_assert_nonnull(session);
  g_free(bunker.client_pubkey);
  bunker.client_pubkey = g_strdup(gh_nip46_session_get_client_pubkey(session));
  NostrNip46ConnectURI parsed = { 0 };
  g_assert_cmpint(nostr_nip46_uri_parse_connect(uri, &parsed), ==, 0);
  g_assert_cmpstr(parsed.client_pubkey_hex, ==, bunker.client_pubkey);
  g_assert_cmpstr(parsed.perms_csv, ==,
    "get_public_key,sign_event,nip44_encrypt,nip44_decrypt,nip04_decrypt");
  g_assert_cmpuint(parsed.n_relays, ==, 1);
  Await wait = { 0 };
  gh_nip46_session_pair_async(session, NULL, pair_done, &wait);
  g_assert_false(gh_nip46_session_is_ready(session));
  bunker_eose(&bunker);
  g_assert_true(gh_nip46_session_is_ready(session));
  /* Unrelated requests are ignored, not treated as a failed pairing. */
  char *connect = nostr_nip46_request_build("x", "ping", NULL, 0);
  bunker_reply(&bunker, connect);
  g_assert_null(gh_nip46_session_get_remote_pubkey(session));
  g_assert_false(wait.done);
  free(connect);
  g_autofree gchar *secret_json = g_strdup_printf("\"%s\"", parsed.secret);
  connect = nostr_nip46_response_build_ok("connect", secret_json);
  bunker_reply(&bunker, connect);
  free(connect);
  g_assert_cmpstr(gh_nip46_session_get_remote_pubkey(session), ==,
                  bunker.signer_pubkey);
  until(has_publish, &bunker);
  bunker_accept(&bunker);
  reply_result(&bunker, bunker.user_pubkey);
  until(await_done, &wait);
  g_assert_no_error(wait.error);
  g_assert_cmpstr(wait.result, ==, bunker.user_pubkey);
  g_free(wait.result);
  nostr_nip46_uri_connect_free(&parsed);
  g_free(uri);
  gh_nip46_session_cancel(session);
  bunker_clear(&bunker);
}

static void
test_qr_connect_request_ack(void)
{
  TestBunker bunker;
  bunker_init(&bunker, CLIENT_SECRET);
  g_autoptr(GError) error = NULL;
  gchar *uri = NULL;
  g_autoptr(GhNip46Session) session = gh_nip46_session_new_qr(relays,
    &bunker_scope_transport, NULL, &bunker_publish_transport, NULL,
    &bunker, &uri, &error);
  g_assert_no_error(error);
  g_free(bunker.client_pubkey);
  bunker.client_pubkey = g_strdup(gh_nip46_session_get_client_pubkey(session));
  NostrNip46ConnectURI parsed = { 0 };
  g_assert_cmpint(nostr_nip46_uri_parse_connect(uri, &parsed), ==, 0);
  Await wait = { 0 };
  gh_nip46_session_pair_async(session, NULL, pair_done, &wait);
  bunker_eose(&bunker);
  const gchar *params[] = { parsed.secret };
  char *connect = nostr_nip46_request_build("connect-id", "connect", params, 1);
  bunker_reply(&bunker, connect);
  free(connect);
  PublishCount want = { &bunker, 2 };
  until(publish_count, &want);
  BunkerHandle *ack = g_ptr_array_index(bunker.publishes, 0);
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_signed(event, ack->event_json, NULL), ==,
                  NOSTR_EVENT_VALIDATION_OK);
  guint8 sk[32], pk[32], *plain = NULL;
  size_t length = 0;
  g_assert_true(nostr_hex2bin(sk, bunker.signer_secret, sizeof sk));
  g_assert_true(nostr_hex2bin(pk, bunker.client_pubkey, sizeof pk));
  g_assert_cmpint(nostr_nip44_decrypt_v2(sk, pk, nostr_event_get_content(event),
                                        &plain, &length), ==, 0);
  g_autofree gchar *ack_json = g_strndup((gchar *)plain, length);
  g_assert_nonnull(strstr(ack_json, "\"ack\""));
  free(plain);
  nostr_event_free(event);
  /* The latest publish is the get_public_key request. */
  bunker_accept(&bunker);
  reply_result(&bunker, bunker.user_pubkey);
  until(await_done, &wait);
  g_assert_no_error(wait.error);
  g_free(wait.result);
  nostr_nip46_uri_connect_free(&parsed);
  g_free(uri);
  gh_nip46_session_cancel(session);
  bunker_clear(&bunker);
}

static void
test_bunker_pairing(void)
{
  TestBunker bunker;
  bunker_init(&bunker, CLIENT_SECRET);
  g_autofree gchar *uri = g_strdup_printf("bunker://%s?relay=wss%%3A%%2F%%2Fbunker.test.invalid&secret=once",
                                          bunker.signer_pubkey);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhNip46Session) session = gh_nip46_session_new_bunker(uri,
    &bunker_scope_transport, NULL, &bunker_publish_transport, NULL,
    &bunker, &error);
  g_assert_no_error(error);
  g_assert_nonnull(session);
  g_free(bunker.client_pubkey);
  bunker.client_pubkey = g_strdup(gh_nip46_session_get_client_pubkey(session));
  Await wait = { 0 };
  gh_nip46_session_pair_async(session, NULL, pair_done, &wait);
  while (g_main_context_iteration(NULL, FALSE)) {}
  g_assert_cmpuint(bunker.publishes->len, ==, 0);
  bunker_eose(&bunker);
  until(has_publish, &bunker);
  g_autofree gchar *request_json = bunker_request_json(&bunker);
  NostrNip46Request request = { 0 };
  g_assert_cmpint(nostr_nip46_request_parse(request_json, &request), ==, 0);
  g_assert_cmpstr(request.method, ==, "connect");
  g_assert_cmpstr(request.params[0], ==, bunker.signer_pubkey);
  g_assert_cmpstr(request.params[1], ==, "once");
  nostr_nip46_request_free(&request);
  bunker_accept(&bunker);
  reply_result(&bunker, "ack");
  PublishCount next = { &bunker, bunker.publishes->len + 1 };
  until(publish_count, &next);
  bunker_accept(&bunker);
  reply_result(&bunker, bunker.user_pubkey);
  until(await_done, &wait);
  g_assert_no_error(wait.error);
  g_assert_cmpstr(wait.result, ==, bunker.user_pubkey);
  g_free(wait.result);
  gh_nip46_session_cancel(session);
  bunker_clear(&bunker);
}

static void
test_reconnect_and_publish_failure(void)
{
  TestBunker bunker;
  g_autoptr(GhNip46Session) session = new_session(&bunker);
  gh_nip46_session_start(session);
  bunker_eose(&bunker);
  BunkerHandle *scope = g_ptr_array_index(bunker.scopes, 0);
  gh_relay_scope_notice(scope->scope, scope->url, GH_RELAY_NOTICE_DISCONNECTED,
                        NULL, FALSE, "connection lost");
  g_assert_false(gh_nip46_session_is_ready(session));
  Await wait = { 0 };
  gh_nip46_session_call_async(session, "get_public_key", NULL, 0, NULL,
                              call_done, &wait);
  while (g_main_context_iteration(NULL, FALSE)) {}
  g_assert_cmpuint(bunker.publishes->len, ==, 0);
  bunker_eose(&bunker);
  until(has_publish, &bunker);
  BunkerHandle *published = bunker_last_publish(&bunker);
  gh_relay_publish_failed(published->publish, published->url, "offline");
  until(await_done, &wait);
  g_assert_error(wait.error, GH_NIP46_SESSION_ERROR,
                 GH_NIP46_SESSION_ERROR_UNAVAILABLE);
  g_clear_error(&wait.error);
  gh_nip46_session_cancel(session);
  bunker_clear(&bunker);
}

static gboolean
fail_auth_url(GhNip46Session *session, const gchar *url, gpointer data)
{
  (void)session; (void)url; (void)data;
  return FALSE;
}

static void
test_auth_url_launch_failure(void)
{
  TestBunker bunker;
  g_autoptr(GhNip46Session) session = new_session(&bunker);
  gh_nip46_session_set_auth_url_handler(session, fail_auth_url, NULL);
  gh_nip46_session_start(session);
  bunker_eose(&bunker);
  Await wait = { 0 };
  gh_nip46_session_call_async(session, "sign_event", NULL, 0, NULL,
                              call_done, &wait);
  until(has_publish, &bunker);
  bunker_accept(&bunker);
  g_autofree gchar *request_json = bunker_request_json(&bunker);
  NostrNip46Request request = { 0 };
  g_assert_cmpint(nostr_nip46_request_parse(request_json, &request), ==, 0);
  g_autofree gchar *auth = g_strdup_printf("{\"id\":\"%s\",\"result\":\"auth_url\","
    "\"error\":\"https://bunker.test.invalid/approve\"}", request.id);
  bunker_reply(&bunker, auth);
  until(await_done, &wait);
  g_assert_error(wait.error, GH_NIP46_SESSION_ERROR,
                 GH_NIP46_SESSION_ERROR_UNAVAILABLE);
  g_clear_error(&wait.error);
  nostr_nip46_request_free(&request);
  gh_nip46_session_cancel(session);
  bunker_clear(&bunker);
}

static void
test_auth_url_deadline(void)
{
  TestBunker bunker;
  g_autoptr(GhNip46Session) session = new_session(&bunker);
  gh_nip46_session_set_test_deadlines(session, 1, 4, 1, 1);
  gh_nip46_session_set_auth_url_handler(session, open_auth_url, NULL);
  gh_nip46_session_start(session);
  bunker_eose(&bunker);
  Await wait = { 0 };
  gh_nip46_session_call_async(session, "sign_event", NULL, 0, NULL,
                              call_done, &wait);
  until(has_publish, &bunker);
  bunker_accept(&bunker);
  g_autofree gchar *request_json = bunker_request_json(&bunker);
  NostrNip46Request request = { 0 };
  g_assert_cmpint(nostr_nip46_request_parse(request_json, &request), ==, 0);
  g_autofree gchar *auth = g_strdup_printf("{\"id\":\"%s\",\"result\":\"auth_url\","
    "\"error\":\"https://bunker.test.invalid/approve\"}", request.id);
  bunker_reply(&bunker, auth);
  until(await_done, &wait);
  g_assert_error(wait.error, GH_NIP46_SESSION_ERROR,
                 GH_NIP46_SESSION_ERROR_TIMED_OUT);
  g_clear_error(&wait.error);
  nostr_nip46_request_free(&request);
  gh_nip46_session_cancel(session);
  bunker_clear(&bunker);
}

static void
test_invalid_auth_url(void)
{
  TestBunker bunker;
  g_autoptr(GhNip46Session) session = new_session(&bunker);
  gh_nip46_session_set_auth_url_handler(session, open_auth_url, NULL);
  gh_nip46_session_start(session);
  bunker_eose(&bunker);
  Await wait = { 0 };
  gh_nip46_session_call_async(session, "sign_event", NULL, 0, NULL,
                              call_done, &wait);
  until(has_publish, &bunker);
  bunker_accept(&bunker);
  g_autofree gchar *request_json = bunker_request_json(&bunker);
  NostrNip46Request request = { 0 };
  g_assert_cmpint(nostr_nip46_request_parse(request_json, &request), ==, 0);
  g_autofree gchar *reply = g_strdup_printf("{\"id\":\"%s\",\"result\":\"auth_url\","
    "\"error\":\"http://not-secure.invalid/\"}", request.id);
  bunker_reply(&bunker, reply);
  until(await_done, &wait);
  g_assert_error(wait.error, GH_NIP46_SESSION_ERROR,
                 GH_NIP46_SESSION_ERROR_INVALID_RESULT);
  g_clear_error(&wait.error);
  nostr_nip46_request_free(&request);
  gh_nip46_session_cancel(session);
  bunker_clear(&bunker);
}

static void
test_queue_bound(void)
{
  TestBunker bunker;
  g_autoptr(GhNip46Session) session = new_session(&bunker);
  gh_nip46_session_start(session);
  Await wait[65] = { 0 };
  for (guint i = 0; i < G_N_ELEMENTS(wait); i++)
    gh_nip46_session_call_async(session, "sign_event", NULL, 0, NULL,
                                call_done, &wait[i]);
  until(await_done, &wait[64]);
  g_assert_error(wait[64].error, GH_NIP46_SESSION_ERROR,
                 GH_NIP46_SESSION_ERROR_UNAVAILABLE);
  g_clear_error(&wait[64].error);
  g_assert_cmpuint(bunker.publishes->len, ==, 0);
  gh_nip46_session_cancel(session);
  for (guint i = 0; i < 64; i++) {
    until(await_done, &wait[i]);
    g_assert_error(wait[i].error, GH_NIP46_SESSION_ERROR,
                   GH_NIP46_SESSION_ERROR_CANCELLED);
    g_clear_error(&wait[i].error);
  }
  bunker_clear(&bunker);
}

static void
test_remote_error_classes(void)
{
  TestBunker bunker;
  g_autoptr(GhNip46Session) session = new_session(&bunker);
  gh_nip46_session_start(session);
  bunker_eose(&bunker);
  struct { const gchar *message; GhNip46SessionError expected; } cases[] = {
    { "unknown method", GH_NIP46_SESSION_ERROR_INVALID_RESULT },
    { "authorization required", GH_NIP46_SESSION_ERROR_UNAVAILABLE },
    { "user refused", GH_NIP46_SESSION_ERROR_DENIED }
  };
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    Await wait = { 0 };
    gh_nip46_session_call_async(session, "sign_event", NULL, 0, NULL,
                                call_done, &wait);
    PublishCount want = { &bunker, i + 1 };
    until(publish_count, &want);
    bunker_accept(&bunker);
    g_autofree gchar *request_json = bunker_request_json(&bunker);
    NostrNip46Request request = { 0 };
    g_assert_cmpint(nostr_nip46_request_parse(request_json, &request), ==, 0);
    char *reply = nostr_nip46_response_build_err(request.id, cases[i].message);
    bunker_reply(&bunker, reply);
    free(reply);
    nostr_nip46_request_free(&request);
    until(await_done, &wait);
    g_assert_error(wait.error, GH_NIP46_SESSION_ERROR, (gint)cases[i].expected);
    g_assert_null(wait.result);
    g_clear_error(&wait.error);
  }
  gh_nip46_session_cancel(session);
  bunker_clear(&bunker);
}

static void
test_approval_timeout(void)
{
  TestBunker bunker;
  g_autoptr(GhNip46Session) session = new_session(&bunker);
  gh_nip46_session_set_test_deadlines(session, 1, 1, 2, 1);
  gh_nip46_session_start(session);
  bunker_eose(&bunker);
  Await wait = { 0 };
  gh_nip46_session_call_async(session, "sign_event", NULL, 0, NULL,
                              call_done, &wait);
  until(has_publish, &bunker);
  bunker_accept(&bunker);
  until(await_done, &wait);
  g_assert_error(wait.error, GH_NIP46_SESSION_ERROR,
                 GH_NIP46_SESSION_ERROR_TIMED_OUT);
  g_assert_false(gh_nip46_session_is_ready(session));
  g_clear_error(&wait.error);
  Await recovered = { 0 };
  gh_nip46_session_call_async(session, "get_public_key", NULL, 0, NULL,
                              call_done, &recovered);
  PublishCount want = { &bunker, 2 };
  until(publish_count, &want);
  bunker_accept(&bunker);
  reply_result(&bunker, bunker.user_pubkey);
  until(await_done, &recovered);
  g_assert_no_error(recovered.error);
  g_assert_true(gh_nip46_session_is_ready(session));
  g_free(recovered.result);
  gh_nip46_session_cancel(session);
  bunker_clear(&bunker);
}

static void
test_pair_timeout(void)
{
  TestBunker bunker;
  bunker_init(&bunker, CLIENT_SECRET);
  g_autoptr(GError) error = NULL;
  gchar *uri = NULL;
  g_autoptr(GhNip46Session) session = gh_nip46_session_new_qr(relays,
    &bunker_scope_transport, NULL, &bunker_publish_transport, NULL,
    &bunker, &uri, &error);
  g_assert_no_error(error);
  g_free(bunker.client_pubkey);
  bunker.client_pubkey = g_strdup(gh_nip46_session_get_client_pubkey(session));
  gh_nip46_session_set_test_deadlines(session, 1, 1, 2, 1);
  Await wait = { 0 };
  gh_nip46_session_pair_async(session, NULL, pair_done, &wait);
  until(await_done, &wait);
  g_assert_error(wait.error, GH_NIP46_SESSION_ERROR,
                 GH_NIP46_SESSION_ERROR_TIMED_OUT);
  g_assert_null(gh_nip46_session_dup_client_secret(session));
  g_clear_error(&wait.error);
  g_free(uri);
  bunker_clear(&bunker);
}

/* The pairing dialog's pattern: the owner keeps no reference of its own, so
 * when the deadline fires the pairing task holds the last one. pair_timeout
 * must not touch the session after returning the task (alpha 5 crash). */
static void
test_pair_timeout_last_ref(void)
{
  TestBunker bunker;
  bunker_init(&bunker, CLIENT_SECRET);
  g_autoptr(GError) error = NULL;
  gchar *uri = NULL;
  GhNip46Session *session = gh_nip46_session_new_qr(relays,
    &bunker_scope_transport, NULL, &bunker_publish_transport, NULL,
    &bunker, &uri, &error);
  g_assert_no_error(error);
  g_free(bunker.client_pubkey);
  bunker.client_pubkey = g_strdup(gh_nip46_session_get_client_pubkey(session));
  gh_nip46_session_set_test_deadlines(session, 1, 1, 2, 1);
  Await wait = { 0 };
  gh_nip46_session_pair_async(session, NULL, pair_done, &wait);
  GWeakRef weak;
  g_weak_ref_init(&weak, session);
  g_object_unref(session); /* the task's reference is now the only one */
  until(await_done, &wait);
  g_assert_error(wait.error, GH_NIP46_SESSION_ERROR,
                 GH_NIP46_SESSION_ERROR_TIMED_OUT);
  g_assert_null(g_weak_ref_get(&weak));
  g_weak_ref_clear(&weak);
  g_clear_error(&wait.error);
  g_free(uri);
  bunker_clear(&bunker);
}

static void
test_interactive_priority(void)
{
  TestBunker bunker;
  g_autoptr(GhNip46Session) session = new_session(&bunker);
  gh_nip46_session_start(session);
  Await bulk = { 0 }, interactive = { 0 };
  const gchar *params[] = { bunker.user_pubkey, "hello" };
  gh_nip46_session_call_async(session, "nip44_encrypt", params, 2,
                              NULL, call_done, &bulk);
  gh_nip46_session_call_async(session, "sign_event", params, 2,
                              NULL, call_done, &interactive);
  bunker_eose(&bunker);
  until(has_publish, &bunker);
  g_autofree gchar *first = bunker_request_json(&bunker);
  NostrNip46Request request = { 0 };
  g_assert_cmpint(nostr_nip46_request_parse(first, &request), ==, 0);
  g_assert_cmpstr(request.method, ==, "sign_event");
  nostr_nip46_request_free(&request);
  gh_nip46_session_cancel(session);
  until(await_done, &bulk);
  until(await_done, &interactive);
  g_assert_error(bulk.error, GH_NIP46_SESSION_ERROR, GH_NIP46_SESSION_ERROR_CANCELLED);
  g_assert_error(interactive.error, GH_NIP46_SESSION_ERROR,
                 GH_NIP46_SESSION_ERROR_CANCELLED);
  g_clear_error(&bulk.error);
  g_clear_error(&interactive.error);
  bunker_clear(&bunker);
}

#include "nip46-compat-tests.inc"

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/nip46/request-wire-eose", test_request_wire_and_eose);
  g_test_add_func("/groundhog/nip46/wrong-author-cancel", test_wrong_author_and_cancel);
  g_test_add_func("/groundhog/nip46/late-reply-after-teardown", test_late_replies_after_teardown);
  g_test_add_func("/groundhog/nip46/late-pair-reply-after-cancel", test_late_pair_reply_after_cancel);
  g_test_add_func("/groundhog/nip46/auth-url", test_auth_url_same_id);
  g_test_add_func("/groundhog/nip46/qr-pairing", test_qr_pairing);
  g_test_add_func("/groundhog/nip46/qr-connect-request", test_qr_connect_request_ack);
  g_test_add_func("/groundhog/nip46/reconnect-publish-failure", test_reconnect_and_publish_failure);
  g_test_add_func("/groundhog/nip46/invalid-auth-url", test_invalid_auth_url);
  g_test_add_func("/groundhog/nip46/auth-url-deadline", test_auth_url_deadline);
  g_test_add_func("/groundhog/nip46/auth-url-launch-failure", test_auth_url_launch_failure);
  g_test_add_func("/groundhog/nip46/bunker-pairing", test_bunker_pairing);
  g_test_add_func("/groundhog/nip46/interactive-priority", test_interactive_priority);
  g_test_add_func("/groundhog/nip46/queue-bound", test_queue_bound);
  g_test_add_func("/groundhog/nip46/approval-timeout", test_approval_timeout);
  g_test_add_func("/groundhog/nip46/remote-error-classes", test_remote_error_classes);
  g_test_add_func("/groundhog/nip46/qr-ack-rejected", test_qr_ack_rejected);
  g_test_add_func("/groundhog/nip46/qr-wrong-secret", test_qr_wrong_secret_fails);
  g_test_add_func("/groundhog/nip46/qr-wrong-secret-request", test_qr_wrong_secret_request_fails);
  g_test_add_func("/groundhog/nip46/qr-signer-error", test_qr_signer_error_fails);
  g_test_add_func("/groundhog/nip46/qr-undecryptable", test_qr_undecryptable_fails);
  g_test_add_func("/groundhog/nip46/qr-p-tag-relay-hint", test_qr_p_tag_relay_hint);
  g_test_add_func("/groundhog/nip46/reply-window-unknown-ids", test_reply_window_and_unknown_ids);
  g_test_add_func("/groundhog/nip46/pair-timeout-names-relays", test_pair_timeout_names_relays);
  g_test_add_func("/groundhog/nip46/bunker-secret-echo", test_bunker_secret_echo);
  g_test_add_func("/groundhog/nip46/pair-timeout", test_pair_timeout);
  g_test_add_func("/groundhog/nip46/pair-timeout-last-ref", test_pair_timeout_last_ref);
  return g_test_run();
}
