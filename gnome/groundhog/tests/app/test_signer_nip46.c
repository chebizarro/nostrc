#include "gh-signer.h"
#include "gh-test-bunker.h"
#include <nostr/nip19/nip19.h>
#include <stdio.h>

#define RELAY "wss://bunker.test.invalid"
#define CLIENT_SECRET "1111111111111111111111111111111111111111111111111111111111111111"

static const gchar *relays[] = { RELAY, NULL };

typedef struct {
  TestBunker bunker;
  GhNip46Session *session;
  GhSigner *signer;
  gchar *npub;
} Fixture;

typedef struct {
  gboolean done;
  guint calls;
  gboolean sign;
  gchar *value;
  GError *error;
} Await;

static void
signer_done(GObject *source, GAsyncResult *result, gpointer data)
{
  Await *wait = data;
  (void)source;
  wait->value = wait->sign ? gh_signer_sign_finish(result, &wait->error) :
                             gh_signer_nip44_finish(result, &wait->error);
  wait->calls++;
  wait->done = TRUE;
}

static gboolean
deadline(gpointer data)
{
  *(gboolean *)data = TRUE;
  return G_SOURCE_REMOVE;
}

static void
wait_for(gboolean (*predicate)(gpointer), gpointer data)
{
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(5, deadline, &expired);
  while (!predicate(data) && !expired)
    g_main_context_iteration(NULL, TRUE);
  if (!expired) g_source_remove(timer);
  g_assert_false(expired);
}

static gboolean
await_done(gpointer data)
{
  return ((Await *)data)->done;
}

typedef struct { TestBunker *bunker; guint count; } PublishCount;
static gboolean
published(gpointer data)
{
  PublishCount *want = data;
  return want->bunker->publishes->len >= want->count;
}

static gchar *
npub_for_pubkey(const gchar *pubkey)
{
  guint8 bytes[32];
  for (guint i = 0; i < 32; i++) {
    unsigned int value;
    g_assert_cmpint(sscanf(pubkey + i * 2, "%2x", &value), ==, 1);
    bytes[i] = value;
  }
  gchar *npub = NULL;
  g_assert_cmpint(nostr_nip19_encode_npub(bytes, &npub), ==, 0);
  return npub;
}

static void
fixture_up(Fixture *fixture)
{
  bunker_init(&fixture->bunker, CLIENT_SECRET);
  g_autoptr(GError) error = NULL;
  fixture->session = gh_nip46_session_new(CLIENT_SECRET,
    fixture->bunker.signer_pubkey, relays, &bunker_scope_transport, NULL,
    &bunker_publish_transport, NULL, &fixture->bunker, &error);
  g_assert_no_error(error);
  g_assert_nonnull(fixture->session);
  fixture->npub = npub_for_pubkey(fixture->bunker.user_pubkey);
  fixture->signer = gh_signer_new_nip46(fixture->session, fixture->npub, &error);
  g_assert_no_error(error);
  g_assert_nonnull(fixture->signer);
  gh_nip46_session_start(fixture->session);
  bunker_eose(&fixture->bunker);
  g_assert_true(gh_nip46_session_is_ready(fixture->session));
}

static void
fixture_down(Fixture *fixture)
{
  gh_signer_free(fixture->signer);
  gh_nip46_session_cancel(fixture->session);
  g_object_unref(fixture->session);
  bunker_clear(&fixture->bunker);
  g_free(fixture->npub);
}

static NostrNip46Request
last_request(Fixture *fixture, guint count, const gchar *method)
{
  PublishCount want = { &fixture->bunker, count };
  wait_for(published, &want);
  bunker_accept(&fixture->bunker);
  g_autofree gchar *json = bunker_request_json(&fixture->bunker);
  NostrNip46Request request = { 0 };
  g_assert_cmpint(nostr_nip46_request_parse(json, &request), ==, 0);
  g_assert_cmpstr(request.method, ==, method);
  return request;
}

static void
reply_json(Fixture *fixture, const gchar *id, const gchar *result_json)
{
  char *response = nostr_nip46_response_build_ok(id, result_json);
  g_assert_nonnull(response);
  bunker_reply(&fixture->bunker, response);
  free(response);
}

static void
reply_text(Fixture *fixture, const gchar *id, const gchar *text)
{
  g_autofree gchar *escaped = g_strescape(text, NULL);
  g_autofree gchar *json = g_strdup_printf("\"%s\"", escaped);
  reply_json(fixture, id, json);
}

static gchar *
signed_event(const gchar *unsigned_json, const gchar *secret)
{
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(event, unsigned_json, NULL), ==, 1);
  g_autofree gchar *pubkey = nostr_key_get_public(secret);
  nostr_event_set_pubkey(event, pubkey);
  g_assert_cmpint(nostr_event_sign(event, secret), ==, 0);
  gchar *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  return json;
}

static gchar *
valid_ciphertext(void)
{
  guint8 bytes[99] = { 2 };
  return g_base64_encode(bytes, sizeof bytes);
}

static void
test_sign_and_crypto(void)
{
  Fixture fixture = { 0 };
  fixture_up(&fixture);
  g_autofree gchar *unsigned_json = g_strdup_printf(
    "{\"pubkey\":\"%s\",\"created_at\":1700000000,\"kind\":1,"
    "\"tags\":[],\"content\":\"hello\"}", fixture.bunker.user_pubkey);
  Await sign = { .sign = TRUE };
  gh_signer_sign_async(fixture.signer, unsigned_json, NULL, signer_done, &sign);
  NostrNip46Request request = last_request(&fixture, 1, "sign_event");
  g_assert_cmpuint(request.n_params, ==, 1);
  g_assert_cmpstr(request.params[0], ==, unsigned_json);
  g_autofree gchar *signed_json = signed_event(unsigned_json, fixture.bunker.signer_secret);
  reply_text(&fixture, request.id, signed_json);
  wait_for(await_done, &sign);
  g_assert_no_error(sign.error);
  g_assert_cmpstr(sign.value, ==, signed_json);
  g_assert_cmpuint(sign.calls, ==, 1);
  g_free(sign.value);
  nostr_nip46_request_free(&request);

  g_autofree gchar *cipher = valid_ciphertext();
  Await encrypt = { 0 };
  gh_signer_nip44_encrypt_async(fixture.signer, "hello", fixture.bunker.client_pubkey,
                                NULL, signer_done, &encrypt);
  request = last_request(&fixture, 2, "nip44_encrypt");
  g_assert_cmpuint(request.n_params, ==, 2);
  g_assert_cmpstr(request.params[0], ==, fixture.bunker.client_pubkey);
  g_assert_cmpstr(request.params[1], ==, "hello");
  reply_text(&fixture, request.id, cipher);
  wait_for(await_done, &encrypt);
  g_assert_no_error(encrypt.error);
  g_assert_cmpstr(encrypt.value, ==, cipher);
  g_free(encrypt.value);
  nostr_nip46_request_free(&request);

  Await decrypt = { 0 };
  gh_signer_nip44_decrypt_async(fixture.signer, cipher, fixture.bunker.client_pubkey,
                                NULL, signer_done, &decrypt);
  request = last_request(&fixture, 3, "nip44_decrypt");
  g_assert_cmpuint(request.n_params, ==, 2);
  g_assert_cmpstr(request.params[0], ==, fixture.bunker.client_pubkey);
  g_assert_cmpstr(request.params[1], ==, cipher);
  reply_text(&fixture, request.id, "hello");
  wait_for(await_done, &decrypt);
  g_assert_no_error(decrypt.error);
  g_assert_cmpstr(decrypt.value, ==, "hello");
  g_free(decrypt.value);
  nostr_nip46_request_free(&request);

  Await nip04 = { 0 };
  gh_signer_nip04_decrypt_async(fixture.signer, "YWJj?iv=YWJj",
                                fixture.bunker.client_pubkey, NULL, signer_done, &nip04);
  request = last_request(&fixture, 4, "nip04_decrypt");
  g_assert_cmpuint(request.n_params, ==, 2);
  g_assert_cmpstr(request.params[0], ==, fixture.bunker.client_pubkey);
  g_assert_cmpstr(request.params[1], ==, "YWJj?iv=YWJj");
  reply_text(&fixture, request.id, "old message");
  wait_for(await_done, &nip04);
  g_assert_no_error(nip04.error);
  g_assert_cmpstr(nip04.value, ==, "old message");
  g_free(nip04.value);
  nostr_nip46_request_free(&request);
  fixture_down(&fixture);
}

static void
test_bad_replies(void)
{
  Fixture fixture = { 0 };
  fixture_up(&fixture);
  g_autofree gchar *unsigned_json = g_strdup_printf(
    "{\"pubkey\":\"%s\",\"created_at\":1700000000,\"kind\":1,"
    "\"tags\":[],\"content\":\"hello\"}", fixture.bunker.user_pubkey);
  Await wrong_key = { .sign = TRUE };
  gh_signer_sign_async(fixture.signer, unsigned_json, NULL, signer_done, &wrong_key);
  NostrNip46Request request = last_request(&fixture, 1, "sign_event");
  char *other_secret = nostr_key_generate_private();
  g_autofree gchar *wrong_signed = signed_event(unsigned_json, other_secret);
  reply_text(&fixture, request.id, wrong_signed);
  wait_for(await_done, &wrong_key);
  g_assert_error(wrong_key.error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_KEY_MISMATCH);
  g_clear_error(&wrong_key.error);
  nostr_nip46_request_free(&request);
  free(other_secret);

  Await malformed_sign = { .sign = TRUE };
  gh_signer_sign_async(fixture.signer, unsigned_json, NULL, signer_done, &malformed_sign);
  request = last_request(&fixture, 2, "sign_event");
  reply_text(&fixture, request.id, "not an event");
  wait_for(await_done, &malformed_sign);
  g_assert_error(malformed_sign.error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_INVALID_RESULT);
  g_clear_error(&malformed_sign.error);
  nostr_nip46_request_free(&request);

  Await malformed_cipher = { 0 };
  gh_signer_nip44_encrypt_async(fixture.signer, "hello", fixture.bunker.client_pubkey,
                                NULL, signer_done, &malformed_cipher);
  request = last_request(&fixture, 3, "nip44_encrypt");
  reply_text(&fixture, request.id, "not base64");
  wait_for(await_done, &malformed_cipher);
  g_assert_error(malformed_cipher.error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_INVALID_RESULT);
  g_clear_error(&malformed_cipher.error);
  nostr_nip46_request_free(&request);

  /* A missing plaintext result is invalid before a decrypt caller can see it. */
  g_autofree gchar *cipher = valid_ciphertext();
  Await missing_plaintext = { 0 };
  gh_signer_nip44_decrypt_async(fixture.signer, cipher, fixture.bunker.client_pubkey,
                                NULL, signer_done, &missing_plaintext);
  request = last_request(&fixture, 4, "nip44_decrypt");
  char *response = nostr_nip46_response_build_err(request.id, "");
  g_assert_nonnull(response);
  bunker_reply(&fixture.bunker, response);
  wait_for(await_done, &missing_plaintext);
  g_assert_error(missing_plaintext.error, GH_SIGNER_ERROR,
                 GH_SIGNER_ERROR_INVALID_RESULT);
  g_clear_error(&missing_plaintext.error);
  free(response);
  nostr_nip46_request_free(&request);
  fixture_down(&fixture);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/signer/nip46/sign-and-crypto", test_sign_and_crypto);
  g_test_add_func("/groundhog/signer/nip46/bad-replies", test_bad_replies);
  return g_test_run();
}
