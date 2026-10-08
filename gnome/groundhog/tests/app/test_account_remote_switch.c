#include "gh-account-controller.h"
#include "gh-signer.h"
#include "gh-test-bunker.h"
#include <nostr/nip19/nip19.h>
#include <stdio.h>

#define RELAY "wss://bunker.test.invalid"
#define CLIENT_SECRET "1111111111111111111111111111111111111111111111111111111111111111"

static const gchar *relays[] = { RELAY, NULL };

typedef struct {
  TestBunker bunkers[2];
  gchar *npubs[2];
  GhNip46Session *sessions[2];
} Fixture;

typedef struct {
  gboolean done;
  guint calls;
  gboolean sign;
  gchar *value;
  GError *error;
} Await;

static void
operation_done(GObject *source, GAsyncResult *result, gpointer data)
{
  Await *wait = data;
  (void)source;
  wait->value = wait->sign ? gh_account_controller_sign_finish(result, &wait->error) :
                             gh_account_controller_nip44_finish(result, &wait->error);
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
spin_until(gboolean (*predicate)(gpointer), gpointer data)
{
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(5, deadline, &expired);
  while (!predicate(data) && !expired)
    g_main_context_iteration(NULL, TRUE);
  if (!expired) g_source_remove(timer);
  g_assert_false(expired);
}

static gboolean
listed(gpointer data)
{
  return gh_account_controller_get_state(data) != GH_ACCOUNT_STATE_DISCOVERING;
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
  for (guint i = 0; i < 2; i++) {
    bunker_init(&fixture->bunkers[i], CLIENT_SECRET);
    fixture->npubs[i] = npub_for_pubkey(fixture->bunkers[i].user_pubkey);
  }
  g_assert_cmpstr(fixture->npubs[0], !=, fixture->npubs[1]);
}

static void
fixture_down(Fixture *fixture)
{
  for (guint i = 0; i < 2; i++) {
    if (fixture->sessions[i]) g_object_unref(fixture->sessions[i]);
    bunker_clear(&fixture->bunkers[i]);
    g_free(fixture->npubs[i]);
  }
}

static GPtrArray *
list_remote(gpointer data, GError **error)
{
  Fixture *fixture = data;
  (void)error;
  GPtrArray *items = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  for (guint i = 0; i < 2; i++) {
    GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
    info->npub = g_strdup(fixture->npubs[i]);
    g_ptr_array_add(items, info);
  }
  return items;
}

static GPtrArray *
list_local(gpointer data, GError **error)
{
  Fixture *fixture = data;
  (void)error;
  GPtrArray *items = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
  info->npub = g_strdup(fixture->npubs[0]);
  g_ptr_array_add(items, info);
  return items;
}

static GhNip46Session *
new_session(const gchar *npub, gpointer data)
{
  Fixture *fixture = data;
  guint index = g_strcmp0(npub, fixture->npubs[0]) == 0 ? 0 : 1;
  g_assert_cmpstr(npub, ==, fixture->npubs[index]);
  g_assert_null(fixture->sessions[index]);
  TestBunker *bunker = &fixture->bunkers[index];
  g_autoptr(GError) error = NULL;
  GhNip46Session *session = gh_nip46_session_new(CLIENT_SECRET,
    bunker->signer_pubkey, relays, &bunker_scope_transport, NULL,
    &bunker_publish_transport, NULL, bunker, &error);
  g_assert_no_error(error);
  g_assert_nonnull(session);
  fixture->sessions[index] = g_object_ref(session);
  return session;
}

static gchar *
response_for_last(TestBunker *bunker, const gchar *method)
{
  g_autofree gchar *json = bunker_request_json(bunker);
  NostrNip46Request request = { 0 };
  g_assert_cmpint(nostr_nip46_request_parse(json, &request), ==, 0);
  g_assert_cmpstr(request.method, ==, method);
  char *response = nostr_nip46_response_build_ok(request.id, "\"late\"");
  g_assert_nonnull(response);
  nostr_nip46_request_free(&request);
  return response;
}

static gchar *
valid_ciphertext(void)
{
  guint8 bytes[99] = { 2 };
  return g_base64_encode(bytes, sizeof bytes);
}

static void
test_switch_cancels_remote_operations(void)
{
  Fixture fixture = { 0 };
  fixture_up(&fixture);
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(settings, "current-npub", "");
  g_settings_set_string(settings, "current-backend", "grotto");
  g_settings_set_int(settings, "backend-migration-version", 1);
  g_autoptr(GhAccountController) controller =
    gh_account_controller_new_full_with_remote_list(settings, NULL,
      list_local, &fixture, list_remote, &fixture);
  gh_account_controller_set_session_factory_for_test(controller, new_session, &fixture);
  spin_until(listed, controller);

  g_assert_true(gh_account_controller_select_backend(controller,
    GH_SIGNER_BACKEND_NIP46, fixture.npubs[0], NULL));
  bunker_eose(&fixture.bunkers[0]);
  g_assert_cmpint(gh_account_controller_get_remote_state(controller), ==,
                  GH_REMOTE_SIGNER_READY);
  guint64 first_generation = gh_account_controller_get_generation(controller);
  gh_account_controller_set_remote_storage_ready(controller, first_generation, TRUE);
  g_autofree gchar *request = g_strdup_printf(
    "{\"pubkey\":\"%s\",\"created_at\":1700000000,\"kind\":1,"
    "\"tags\":[],\"content\":\"hello\"}", fixture.bunkers[0].user_pubkey);
  Await first = { .sign = TRUE };
  gh_account_controller_sign_async(controller, request, operation_done, &first);
  PublishCount first_publish = { &fixture.bunkers[0], 1 };
  spin_until(published, &first_publish);
  bunker_accept(&fixture.bunkers[0]);
  g_autofree gchar *late_sign = response_for_last(&fixture.bunkers[0], "sign_event");
  g_assert_false(first.done);

  g_assert_true(gh_account_controller_select_backend(controller,
    GH_SIGNER_BACKEND_NIP46, fixture.npubs[1], NULL));
  spin_until(await_done, &first);
  g_assert_error(first.error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED);
  g_assert_null(first.value);
  g_assert_cmpuint(first.calls, ==, 1);
  g_clear_error(&first.error);
  g_assert_cmpuint(gh_account_controller_get_generation(controller), ==,
                   first_generation + 1);
  bunker_eose(&fixture.bunkers[1]);
  guint64 second_generation = gh_account_controller_get_generation(controller);
  gh_account_controller_set_remote_storage_ready(controller, second_generation, TRUE);
  g_autofree gchar *ciphertext = valid_ciphertext();
  Await second = { 0 };
  gh_account_controller_nip44_decrypt_async(controller, ciphertext,
    fixture.bunkers[1].client_pubkey, operation_done, &second);
  PublishCount second_publish = { &fixture.bunkers[1], 1 };
  spin_until(published, &second_publish);
  bunker_accept(&fixture.bunkers[1]);
  g_autofree gchar *active_json = bunker_request_json(&fixture.bunkers[1]);
  NostrNip46Request active_request = { 0 };
  g_assert_cmpint(nostr_nip46_request_parse(active_json, &active_request), ==, 0);
  g_assert_cmpstr(active_request.method, ==, "nip44_decrypt");
  bunker_reply(&fixture.bunkers[0], late_sign); /* canceled old scope */
  char *cross_reply = nostr_nip46_response_build_ok(active_request.id,
                                                     "\"wrong account\"");
  g_assert_nonnull(cross_reply);
  char *cross_event = bunker_response_event_json_from(&fixture.bunkers[0],
    fixture.bunkers[0].signer_secret, fixture.bunkers[0].client_pubkey, cross_reply);
  BunkerHandle *active_scope = g_ptr_array_index(fixture.bunkers[1].scopes, 0);
  gh_relay_scope_event(active_scope->scope, active_scope->url, cross_event);
  free(cross_event);
  free(cross_reply);
  while (g_main_context_iteration(NULL, FALSE)) {}
  g_assert_false(second.done); /* old author cannot answer the new request ID */
  g_assert_cmpuint(first.calls, ==, 1);
  g_assert_true(gh_account_controller_is_current(controller, second_generation));
  char *good_reply = nostr_nip46_response_build_ok(active_request.id,
                                                    "\"decrypted\"");
  g_assert_nonnull(good_reply);
  bunker_reply(&fixture.bunkers[1], good_reply);
  spin_until(await_done, &second);
  g_assert_no_error(second.error);
  g_assert_cmpstr(second.value, ==, "decrypted");
  g_assert_cmpuint(second.calls, ==, 1);
  g_free(second.value);
  free(good_reply);
  nostr_nip46_request_free(&active_request);

  Await third = { 0 };
  gh_account_controller_nip44_decrypt_async(controller, ciphertext,
    fixture.bunkers[1].client_pubkey, operation_done, &third);
  second_publish.count = 2;
  spin_until(published, &second_publish);
  bunker_accept(&fixture.bunkers[1]);
  g_autofree gchar *late_decrypt = response_for_last(&fixture.bunkers[1],
                                                     "nip44_decrypt");
  g_assert_false(third.done);
  g_assert_true(gh_account_controller_select_backend(controller,
    GH_SIGNER_BACKEND_GROTTO, fixture.npubs[0], NULL));
  spin_until(await_done, &third);
  g_assert_error(third.error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED);
  g_assert_null(third.value);
  g_assert_cmpuint(third.calls, ==, 1);
  g_clear_error(&third.error);
  guint64 grotto_generation = gh_account_controller_get_generation(controller);
  g_assert_cmpuint(grotto_generation, ==, second_generation + 1);
  bunker_reply(&fixture.bunkers[1], late_decrypt);
  while (g_main_context_iteration(NULL, FALSE)) {}
  g_assert_cmpuint(first.calls, ==, 1);
  g_assert_cmpuint(second.calls, ==, 1);
  g_assert_cmpuint(third.calls, ==, 1);
  g_assert_cmpuint(gh_account_controller_get_generation(controller), ==,
                   grotto_generation);
  g_assert_cmpint(gh_account_controller_get_active_backend(controller), ==,
                  GH_SIGNER_BACKEND_GROTTO);
  g_assert_cmpstr(gh_account_controller_get_active_npub(controller), ==,
                  fixture.npubs[0]);
  g_clear_object(&controller);
  fixture_down(&fixture);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/backends/remote-switch-cancels",
                  test_switch_cancels_remote_operations);
  return g_test_run();
}
