/* Remote-account feature paths use the real account controller and NIP-46
 * adapter. The fake bunker checks every requested method and event author. */
#include "gh-account-auth.h"
#include "gh-nip17-envelope.h"
#include "gh-nip17-inbox.h"
#include "gh-test-bunker.h"
#include "gh-signer.h"
#ifdef GH_REMOTE_MATRIX_NIP29
#include "gh-nip29-outbox.h"
#include "gh-nip29-template.h"
#include "gh-store-nip29.h"
#endif
#include <nostr/nip19/nip19.h>
#include <glib/gstdio.h>
#include <stdio.h>

#define CLIENT_SECRET "1111111111111111111111111111111111111111111111111111111111111111"
#define RELAY "wss://bunker.test.invalid"

static const gchar *relays[] = { RELAY, NULL };

typedef enum {
  PERMIT_SIGN = 1 << 0,
  PERMIT_ENCRYPT = 1 << 1,
  PERMIT_DECRYPT = 1 << 2,
  PERMIT_NIP04 = 1 << 3
} Permit;

typedef struct {
  TestBunker bunker;
  GhNip46Session *session;
  GhAccountController *accounts;
  GSettings *settings;
  gchar *npub;
  guint allowed;
  guint handled;
  guint calls[4];
  GArray *signed_kinds;
#ifdef GH_REMOTE_MATRIX_NIP29
  GPtrArray *group_publishes;
  guint handled_group;
#endif
} Fixture;

typedef struct {
  gboolean done;
  gpointer value;
  GError *error;
} Result;

static gboolean
deadline(gpointer data)
{
  *(gboolean *)data = TRUE;
  return G_SOURCE_REMOVE;
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

static GPtrArray *
list_empty(gpointer data, GError **error)
{
  (void)data; (void)error;
  return g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
}

static GPtrArray *
list_remote(gpointer data, GError **error)
{
  Fixture *f = data;
  (void)error;
  GPtrArray *items = list_empty(NULL, NULL);
  GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
  info->npub = g_strdup(f->npub);
  g_ptr_array_add(items, info);
  return items;
}

static GhNip46Session *
new_session(const gchar *npub, gpointer data)
{
  Fixture *f = data;
  g_assert_cmpstr(npub, ==, f->npub);
  g_assert_null(f->session);
  g_autoptr(GError) error = NULL;
  GhNip46Session *session = gh_nip46_session_new(CLIENT_SECRET,
    f->bunker.signer_pubkey, relays, &bunker_scope_transport, NULL,
    &bunker_publish_transport, NULL, &f->bunker, &error);
  g_assert_no_error(error);
  f->session = g_object_ref(session);
  return session;
}

static void
fixture_up(Fixture *f)
{
  bunker_init(&f->bunker, CLIENT_SECRET);
  f->npub = npub_for_pubkey(f->bunker.user_pubkey);
  f->signed_kinds = g_array_new(FALSE, FALSE, sizeof(gint));
#ifdef GH_REMOTE_MATRIX_NIP29
  f->group_publishes = g_ptr_array_new_with_free_func(bunker_handle_free);
#endif
  f->allowed = PERMIT_SIGN | PERMIT_ENCRYPT | PERMIT_DECRYPT | PERMIT_NIP04;
  f->settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(f->settings, "current-npub", "");
  g_settings_set_string(f->settings, "current-backend", "grotto");
  g_settings_set_int(f->settings, "backend-migration-version", 1);
  f->accounts = gh_account_controller_new_full_with_remote_list(f->settings,
    NULL, list_empty, f, list_remote, f);
  gh_account_controller_set_session_factory_for_test(f->accounts, new_session, f);
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(5, deadline, &expired);
  while (gh_account_controller_get_state(f->accounts) == GH_ACCOUNT_STATE_DISCOVERING && !expired)
    g_main_context_iteration(NULL, TRUE);
  if (!expired) g_source_remove(timer);
  g_assert_false(expired);
  g_assert_true(gh_account_controller_select_backend(f->accounts,
    GH_SIGNER_BACKEND_NIP46, f->npub, NULL));
  bunker_eose(&f->bunker);
  g_assert_true(gh_nip46_session_is_ready(f->session));
  gh_account_controller_set_remote_storage_ready(f->accounts,
    gh_account_controller_get_generation(f->accounts), TRUE);
}

static void
fixture_down(Fixture *f)
{
  g_clear_object(&f->accounts);
  g_clear_object(&f->session);
  g_clear_object(&f->settings);
  g_array_unref(f->signed_kinds);
#ifdef GH_REMOTE_MATRIX_NIP29
  g_ptr_array_unref(f->group_publishes);
#endif
  bunker_clear(&f->bunker);
  g_free(f->npub);
}

static guint
permit_for_method(const gchar *method)
{
  if (g_str_equal(method, "sign_event")) return PERMIT_SIGN;
  if (g_str_equal(method, "nip44_encrypt")) return PERMIT_ENCRYPT;
  if (g_str_equal(method, "nip44_decrypt")) return PERMIT_DECRYPT;
  if (g_str_equal(method, "nip04_decrypt")) return PERMIT_NIP04;
  g_error("unexpected remote signer request method: %s", method);
  return 0;
}

static gchar *
sign_unsigned(const gchar *json, const gchar *secret, gint *kind)
{
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(event, json, NULL), ==, 1);
  *kind = nostr_event_get_kind(event);
  g_assert_cmpint(nostr_event_sign(event, secret), ==, 0);
  gchar *signed_json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  return signed_json;
}

static void
answer_one(Fixture *f)
{
  guint index = f->handled++;
  BunkerHandle *handle = g_ptr_array_index(f->bunker.publishes, index);
  gh_relay_publish_ok(handle->publish, handle->url, handle->event_id, TRUE, "");
  g_autofree gchar *json = bunker_request_json_at(&f->bunker, index);
  NostrNip46Request request = { 0 };
  g_assert_cmpint(nostr_nip46_request_parse(json, &request), ==, 0);
  guint permit = permit_for_method(request.method);
  guint slot = permit == PERMIT_SIGN ? 0 : permit == PERMIT_ENCRYPT ? 1 :
               permit == PERMIT_DECRYPT ? 2 : 3;
  f->calls[slot]++;
  char *response = NULL;
  if (!(f->allowed & permit)) {
    response = nostr_nip46_response_build_err(request.id, "Permission denied");
  } else if (permit == PERMIT_SIGN) {
    g_assert_cmpuint(request.n_params, ==, 1);
    NostrEvent *unsigned_event = nostr_event_new();
    g_assert_cmpint(nostr_event_deserialize_compact(unsigned_event, request.params[0], NULL), ==, 1);
    g_assert_cmpstr(nostr_event_get_pubkey(unsigned_event), ==, f->bunker.user_pubkey);
    nostr_event_free(unsigned_event);
    gint kind = 0;
    g_autofree gchar *signed_json = sign_unsigned(request.params[0],
      f->bunker.signer_secret, &kind);
    g_array_append_val(f->signed_kinds, kind);
    g_autofree gchar *quoted = g_strescape(signed_json, NULL);
    g_autofree gchar *result = g_strdup_printf("\"%s\"", quoted);
    response = nostr_nip46_response_build_ok(request.id, result);
  } else if (permit == PERMIT_ENCRYPT || permit == PERMIT_DECRYPT) {
    g_assert_cmpuint(request.n_params, ==, 2);
    guint8 sk[32], pk[32];
    g_assert_true(nostr_hex2bin(sk, f->bunker.signer_secret, sizeof sk));
    g_assert_true(nostr_hex2bin(pk, request.params[0], sizeof pk));
    gchar *value = NULL;
    if (permit == PERMIT_ENCRYPT) {
      g_assert_cmpint(nostr_nip44_encrypt_v2(sk, pk,
        (const guint8 *)request.params[1], strlen(request.params[1]), &value), ==, 0);
    } else {
      guint8 *plain = NULL;
      size_t length = 0;
      g_assert_cmpint(nostr_nip44_decrypt_v2(sk, pk, request.params[1],
        &plain, &length), ==, 0);
      value = g_strndup((const gchar *)plain, length);
      free(plain);
    }
    g_autofree gchar *quoted = g_strescape(value, NULL);
    g_autofree gchar *result = g_strdup_printf("\"%s\"", quoted);
    response = nostr_nip46_response_build_ok(request.id, result);
    if (permit == PERMIT_ENCRYPT) free(value); else g_free(value);
  } else {
    g_assert_cmpuint(request.n_params, ==, 2);
    g_assert_cmpstr(request.params[0], ==, f->bunker.client_pubkey);
    response = nostr_nip46_response_build_ok(request.id, "\"old message\"");
  }
  g_assert_nonnull(response);
  bunker_reply(&f->bunker, response);
  free(response);
  nostr_nip46_request_free(&request);
}

static void
run_until(Fixture *f, Result *result)
{
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(10, deadline, &expired);
  while (!result->done && !expired) {
    while (f->handled < f->bunker.publishes->len)
      answer_one(f);
    if (!result->done) g_main_context_iteration(NULL, TRUE);
  }
  if (!expired) g_source_remove(timer);
  g_assert_false(expired);
  g_assert_true(result->done);
}

static void
auth_done(gpointer owner, const gchar *signed_json,
          const gchar *event_id, const GError *error)
{
  Result *result = owner;
  result->value = g_strdup(signed_json);
  if (error) result->error = g_error_copy(error);
  g_assert_true(error != NULL || event_id != NULL);
  result->done = TRUE;
}

static void
envelope_done(GObject *source, GAsyncResult *reply, gpointer data)
{
  Result *result = data;
  (void)source;
  result->value = gh_nip17_envelope_build_finish(reply, &result->error);
  result->done = TRUE;
}

static void
unwrap_done(GObject *source, GAsyncResult *reply, gpointer data)
{
  Result *result = data;
  (void)source;
  result->value = gh_nip17_unwrap_finish(reply, &result->error);
  result->done = TRUE;
}

static void
nip04_done(GObject *source, GAsyncResult *reply, gpointer data)
{
  Result *result = data;
  (void)source;
  result->value = gh_account_controller_nip44_finish(reply, &result->error);
  result->done = TRUE;
}

static void
test_remote_auth_dm_and_nip04(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  g_autoptr(GhAccountAuth) auth = gh_account_auth_new(f.accounts);
  g_assert_nonnull(auth);
  Result result = { 0 };
  g_autoptr(GError) error = NULL;
  GhRelayAuthAttempt *attempt = gh_relay_auth_attempt_start(GH_RELAY_AUTH_ACCOUNT,
    gh_account_auth_get_signer(auth), gh_account_controller_get_generation(f.accounts),
    RELAY, "matrix-challenge", auth_done, &result, &error);
  g_assert_no_error(error);
  g_assert_nonnull(attempt);
  run_until(&f, &result);
  g_assert_no_error(result.error);
  g_assert_cmpuint(f.calls[0], ==, 1);
  g_assert_cmpint(g_array_index(f.signed_kinds, gint, 0), ==, GH_RELAY_AUTH_KIND);
  g_free(result.value);

  result = (Result){ 0 };
  gh_nip17_envelope_build_self_async(f.accounts, "remote DM", NULL,
                                      envelope_done, &result);
  run_until(&f, &result);
  g_assert_no_error(result.error);
  GhNip17Envelope *envelope = result.value;
  g_assert_nonnull(envelope);
  g_assert_nonnull(envelope->sender_wrap_json);
  g_assert_cmpuint(f.calls[0], ==, 2); /* kind-13 seal */
  g_assert_cmpuint(f.calls[1], ==, 1); /* signer encrypts rumor */
  g_assert_cmpint(g_array_index(f.signed_kinds, gint, 1), ==, 13);

  result = (Result){ 0 };
  gh_nip17_unwrap_async(f.accounts, envelope->sender_wrap_json, NULL,
                         unwrap_done, &result);
  run_until(&f, &result);
  g_assert_no_error(result.error);
  GhNip17Message *message = result.value;
  g_assert_nonnull(message);
  g_assert_true(message->self_copy);
  g_assert_cmpstr(message->sender_pubkey, ==, f.bunker.user_pubkey);
  g_assert_cmpuint(f.calls[2], ==, 2); /* wrap and seal decrypt */
  gh_nip17_message_free(message);
  gh_nip17_envelope_free(envelope);

  result = (Result){ 0 };
  gh_account_controller_nip04_decrypt_async(f.accounts, "YWJj?iv=YWJj",
    f.bunker.client_pubkey, NULL, nip04_done, &result);
  run_until(&f, &result);
  g_assert_no_error(result.error);
  g_assert_cmpstr(result.value, ==, "old message");
  g_assert_cmpuint(f.calls[3], ==, 1);
  g_free(result.value);
  fixture_down(&f);
}

static void
test_remote_permissions_queue_and_cancel(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  f.allowed &= ~PERMIT_NIP04;
  Result denied = { 0 };
  gh_account_controller_nip04_decrypt_async(f.accounts, "YWJj?iv=YWJj",
    f.bunker.client_pubkey, NULL, nip04_done, &denied);
  run_until(&f, &denied);
  g_assert_error(denied.error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_DENIED);
  g_assert_null(denied.value);
  g_clear_error(&denied.error);

  Result pending[5] = { 0 };
  GCancellable *cancellables[5] = { 0 };
  guint baseline = f.bunker.publishes->len;
  for (guint i = 0; i < G_N_ELEMENTS(pending); i++) {
    cancellables[i] = g_cancellable_new();
    gh_account_controller_nip04_decrypt_async(f.accounts, "YWJj?iv=YWJj",
      f.bunker.client_pubkey, cancellables[i], nip04_done, &pending[i]);
  }
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(5, deadline, &expired);
  while (f.bunker.publishes->len < baseline + 4 && !expired)
    g_main_context_iteration(NULL, TRUE);
  if (!expired) g_source_remove(timer);
  g_assert_false(expired);
  g_assert_cmpuint(f.bunker.publishes->len, ==, baseline + 4);
  for (guint i = 0; i < G_N_ELEMENTS(pending); i++)
    g_cancellable_cancel(cancellables[i]);
  expired = FALSE;
  timer = g_timeout_add_seconds(5, deadline, &expired);
  for (guint i = 0; i < G_N_ELEMENTS(pending); i++) {
    while (!pending[i].done && !expired)
      g_main_context_iteration(NULL, TRUE);
    g_assert_false(expired);
    g_assert_error(pending[i].error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED);
    g_assert_null(pending[i].value);
    g_clear_error(&pending[i].error);
    g_object_unref(cancellables[i]);
  }
  if (!expired) g_source_remove(timer);
  g_assert_cmpuint(f.bunker.publishes->len, ==, baseline + 4);
  fixture_down(&f);
}

#ifdef GH_REMOTE_MATRIX_NIP29
static gpointer
group_publish_open(GhRelayPublish *publish, const gchar *url,
                   const gchar *event_json, gpointer data, GError **error)
{
  Fixture *f = data;
  (void)error;
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_signed(event, event_json, NULL), ==,
                  NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpint(nostr_event_get_kind(event), ==, 9);
  g_assert_cmpstr(nostr_event_get_pubkey(event), ==, f->bunker.user_pubkey);
  BunkerHandle *handle = g_new0(BunkerHandle, 1);
  handle->publish = gh_relay_publish_ref(publish);
  handle->url = g_strdup(url);
  handle->event_json = g_strdup(event_json);
  handle->event_id = g_strdup(nostr_event_get_id(event));
  g_ptr_array_add(f->group_publishes, handle);
  nostr_event_free(event);
  return handle;
}

static const GhRelayPublishTransport group_publish_transport = {
  group_publish_open, bunker_close
};

static void
remove_tree(const gchar *path)
{
  GDir *dir = g_dir_open(path, 0, NULL);
  const gchar *name;
  while (dir && (name = g_dir_read_name(dir))) {
    g_autofree gchar *child = g_build_filename(path, name, NULL);
    if (g_file_test(child, G_FILE_TEST_IS_DIR)) remove_tree(child);
    else g_assert_cmpint(g_unlink(child), ==, 0);
  }
  if (dir) g_dir_close(dir);
  g_assert_cmpint(g_rmdir(path), ==, 0);
}

static void
test_remote_nip29_outbox(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  g_autofree gchar *dir = g_dir_make_tmp("groundhog-remote-nip29-XXXXXX", NULL);
  g_assert_nonnull(dir);
  guint8 key_bytes[GH_STORE_KEY_SIZE];
  for (guint i = 0; i < sizeof key_bytes; i++) key_bytes[i] = (guint8)(i + 1);
  g_autoptr(GBytes) key = g_bytes_new(key_bytes, sizeof key_bytes);
  GhStoreConfig store_config = { dir, f.bunker.user_pubkey, NULL, NULL, NULL };
  g_autoptr(GError) error = NULL;
  GhStore *store = gh_store_open_with_key(&store_config, key,
    "7d0c2a51-3b8e-4f6a-9c1d-2e4f5a6b7c8d", GH_STORE_OPEN_CREATE, &error);
  g_assert_no_error(error);
  g_assert_nonnull(store);
  gint64 conversation_id = 0;
  g_assert_true(gh_store_nip29_save_group(store, RELAY, "matrix", "", NULL,
    "Matrix", &conversation_id, &error));
  g_assert_no_error(error);
  GhNip29OutboxConfig config = {
    .store = store, .accounts = f.accounts,
    .transport = &group_publish_transport, .transport_data = &f,
  };
  g_autoptr(GhNip29Outbox) outbox = gh_nip29_outbox_new(&config, &error);
  g_assert_no_error(error);
  g_assert_nonnull(outbox);
  GhNip29GroupKey *group = gh_nip29_group_key_new(RELAY, "matrix", &error);
  g_assert_no_error(error);
  GhNip29TemplateContext context = {
    .author_pubkey = f.bunker.user_pubkey,
    .created_at = g_get_real_time() / G_USEC_PER_SEC,
  };
  g_autofree gchar *unsigned_json = gh_nip29_template_chat(group, &context,
    "remote group message", &error);
  g_assert_no_error(error);
  g_assert_nonnull(unsigned_json);
  g_autoptr(GhNip29Op) op = gh_nip29_outbox_enqueue(outbox, conversation_id,
    unsigned_json, &error);
  g_assert_no_error(error);
  g_assert_nonnull(op);
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(10, deadline, &expired);
  while (gh_nip29_op_get_result(op) != GH_NIP29_OP_ACCEPTED && !expired) {
    while (f.handled < f.bunker.publishes->len) answer_one(&f);
    while (f.handled_group < f.group_publishes->len) {
      BunkerHandle *handle = g_ptr_array_index(f.group_publishes, f.handled_group++);
      gh_relay_publish_ok(handle->publish, handle->url, handle->event_id, TRUE, "");
    }
    if (gh_nip29_op_get_result(op) != GH_NIP29_OP_ACCEPTED)
      g_main_context_iteration(NULL, TRUE);
  }
  if (!expired) g_source_remove(timer);
  g_assert_false(expired);
  g_assert_cmpint(gh_nip29_op_get_result(op), ==, GH_NIP29_OP_ACCEPTED);
  g_assert_cmpuint(f.calls[0], ==, 1);
  g_assert_cmpint(g_array_index(f.signed_kinds, gint, 0), ==, 9);
  g_assert_cmpuint(f.group_publishes->len, ==, 1);
  gh_nip29_group_key_free(group);
  g_clear_object(&op);
  g_clear_object(&outbox);
  gh_store_close(store);
  fixture_down(&f);
  remove_tree(dir);
}
#endif

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/remote-feature-matrix/auth-dm-nip04",
                  test_remote_auth_dm_and_nip04);
  g_test_add_func("/groundhog/remote-feature-matrix/permissions-queue-cancel",
                  test_remote_permissions_queue_and_cancel);
#ifdef GH_REMOTE_MATRIX_NIP29
  g_test_add_func("/groundhog/remote-feature-matrix/nip29-outbox",
                  test_remote_nip29_outbox);
#endif
  return g_test_run();
}
