/* Offline host backend for the Concord plugin's service tests: no relay, no
 * storage, no signer. The service must stay fully exercisable without them —
 * everything Concord needs to *read* a plane is pure derivation. */

#include <gnostr-plugin-api.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-utils.h>
#include <nostr/nip44/nip44.h>

#include <stdlib.h>
#include <string.h>

/* Test hooks. With no signer key set the backend refuses to sign, which is
 * the deactivated-host case; setting one lets a test drive the whole mint
 * path and feed the resulting wrap back through the reader. */
const char *gn_concord_test_signer_sk = NULL;
const char *gn_concord_test_user_pubkey = NULL;
char *gn_concord_test_published_json = NULL;
/* Every event this backend published, oldest first, so a test can assert on a
 * whole exchange rather than only its last event. */
GPtrArray *gn_concord_test_published = NULL;
/* Set to serve a relay's answer to one filter; NULL means an empty relay. */
GPtrArray *(*gn_concord_test_query_hook)(const char *filter_json) = NULL;
/* Every query fails: the unreachable-relay case, which a client must never
 * mistake for an empty relay. */
gboolean gn_concord_test_query_fails = FALSE;

void gn_concord_test_reset(void) {
  gn_concord_test_signer_sk = NULL;
  gn_concord_test_user_pubkey = NULL;
  gn_concord_test_query_hook = NULL;
  gn_concord_test_query_fails = FALSE;
  free(gn_concord_test_published_json);
  gn_concord_test_published_json = NULL;
  g_clear_pointer(&gn_concord_test_published, g_ptr_array_unref);
}

GPtrArray *gnostr_plugin_context_query_events(GnostrPluginContext *context,
                                              const char *filter_json,
                                              GError **error) {
  (void)context;
  if (gn_concord_test_query_fails) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE,
                "Offline test backend cannot reach a relay");
    return NULL;
  }
  if (gn_concord_test_query_hook) {
    GPtrArray *events = gn_concord_test_query_hook(filter_json);
    if (events) return events;
  }
  return g_ptr_array_new_with_free_func(g_free);
}

char *gnostr_plugin_context_get_event_by_id(GnostrPluginContext *context,
                                            const char *event_id_hex,
                                            GError **error) {
  (void)context;
  (void)event_id_hex;
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
              "Offline test backend has no event");
  return NULL;
}

guint64 gnostr_plugin_context_subscribe_events(GnostrPluginContext *context,
                                               const char *filter_json,
                                               GCallback callback,
                                               gpointer user_data,
                                               GDestroyNotify destroy_notify) {
  (void)context;
  (void)filter_json;
  (void)callback;
  /* Matches the host: a failed subscribe (0) leaves @user_data with the
   * caller and never runs @destroy_notify — that only fires on unsubscribe
   * after a successful subscription. */
  (void)user_data;
  (void)destroy_notify;
  return 0;
}

void gnostr_plugin_context_unsubscribe_events(GnostrPluginContext *context,
                                              guint64 subscription_id) {
  (void)context;
  (void)subscription_id;
}

const char *gnostr_plugin_context_get_user_pubkey(
    GnostrPluginContext *context) {
  (void)context;
  return gn_concord_test_user_pubkey;
}

void gnostr_plugin_context_request_sign_event(GnostrPluginContext *context,
                                              const char *unsigned_event_json,
                                              GCancellable *cancellable,
                                              GAsyncReadyCallback callback,
                                              gpointer user_data) {
  (void)context;
  (void)unsigned_event_json;
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  if (!gn_concord_test_signer_sk) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "Offline test backend cannot sign");
    g_object_unref(task);
    return;
  }
  NostrEvent *event = nostr_event_new();
  if (!event ||
      !nostr_event_deserialize_compact(event, unsigned_event_json, NULL) ||
      nostr_event_sign(event, gn_concord_test_signer_sk) != 0) {
    nostr_event_free(event);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Offline test backend failed to sign");
    g_object_unref(task);
    return;
  }
  char *signed_json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  g_task_return_pointer(task, signed_json, free);
  g_object_unref(task);
}

char *gnostr_plugin_context_request_sign_event_finish(
    GnostrPluginContext *context, GAsyncResult *result, GError **error) {
  (void)context;
  return g_task_propagate_pointer(G_TASK(result), error);
}

void gnostr_plugin_context_publish_event_async(GnostrPluginContext *context,
                                               const char *event_json,
                                               GCancellable *cancellable,
                                               GAsyncReadyCallback callback,
                                               gpointer user_data) {
  (void)context;
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  if (!gn_concord_test_signer_sk) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "Offline test backend cannot publish");
    g_object_unref(task);
    return;
  }
  free(gn_concord_test_published_json);
  gn_concord_test_published_json =
    event_json ? strdup(event_json) : NULL;
  if (!gn_concord_test_published)
    gn_concord_test_published = g_ptr_array_new_with_free_func(g_free);
  if (event_json)
    g_ptr_array_add(gn_concord_test_published, g_strdup(event_json));
  g_task_return_boolean(task, TRUE);
  g_object_unref(task);
}

gboolean gnostr_plugin_context_publish_event_finish(
    GnostrPluginContext *context, GAsyncResult *result, GError **error) {
  (void)context;
  return g_task_propagate_boolean(G_TASK(result), error);
}

gboolean gnostr_plugin_context_store_data(GnostrPluginContext *context,
                                          const char *key, GBytes *data,
                                          GError **error) {
  (void)context;
  (void)key;
  (void)data;
  (void)error;
  return TRUE;
}

GBytes *gnostr_plugin_context_load_data(GnostrPluginContext *context,
                                        const char *key, GError **error) {
  (void)context;
  (void)key;
  (void)error;
  return NULL;
}

gboolean gnostr_plugin_context_delete_data(GnostrPluginContext *context,
                                           const char *key) {
  (void)context;
  (void)key;
  return FALSE;
}

/* NIP-44 to one's own key: the real host routes this through whichever signer
 * owns the key, so the offline backend does the same self-ECDH in-process
 * with the test key. Without a key there is no identity to encrypt to, which
 * is the logged-out case. */
static gboolean test_convkey(const char *peer_pubkey, uint8_t convkey[32],
                             GError **error) {
  if (!gn_concord_test_signer_sk) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "Offline test backend holds no key");
    return FALSE;
  }
  char *own = peer_pubkey ? NULL
                          : nostr_key_get_public(gn_concord_test_signer_sk);
  const char *peer = peer_pubkey ? peer_pubkey : own;
  uint8_t sk[32], pk[32];
  gboolean ok =
    peer && nostr_hex2bin(sk, gn_concord_test_signer_sk, sizeof(sk)) &&
    nostr_hex2bin(pk, peer, sizeof(pk)) &&
    nostr_nip44_convkey(sk, pk, convkey) == 0;
  free(own);
  memset(sk, 0, sizeof(sk));
  if (!ok)
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Offline test backend could not derive the conversation key");
  return ok;
}

static void test_nip44_async(const char *peer_pubkey, const char *text,
                             gboolean encrypt, GCancellable *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer user_data) {
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  uint8_t convkey[32];
  GError *error = NULL;
  if (!test_convkey(peer_pubkey, convkey, &error)) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  if (encrypt) {
    char *payload = NULL;
    int rc = nostr_nip44_encrypt_v2_with_convkey(
      convkey, (const uint8_t *)text, strlen(text), &payload);
    memset(convkey, 0, sizeof(convkey));
    if (rc != 0 || !payload) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                              "Offline test backend failed to encrypt");
      g_object_unref(task);
      return;
    }
    g_task_return_pointer(task, payload, free);
  } else {
    uint8_t *plaintext = NULL;
    size_t len = 0;
    int rc =
      nostr_nip44_decrypt_v2_with_convkey(convkey, text, &plaintext, &len);
    memset(convkey, 0, sizeof(convkey));
    if (rc != 0 || !plaintext) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                              "Offline test backend failed to decrypt");
      g_object_unref(task);
      return;
    }
    char *result = g_strndup((const char *)plaintext, len);
    free(plaintext);
    g_task_return_pointer(task, result, g_free);
  }
  g_object_unref(task);
}

void gnostr_plugin_context_nip44_encrypt_async(
    GnostrPluginContext *context, const char *peer_pubkey_hex,
    const char *plaintext, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data) {
  (void)context;
  test_nip44_async(peer_pubkey_hex, plaintext, TRUE, cancellable, callback,
                   user_data);
}

char *gnostr_plugin_context_nip44_encrypt_finish(GnostrPluginContext *context,
                                                 GAsyncResult *result,
                                                 GError **error) {
  (void)context;
  return g_task_propagate_pointer(G_TASK(result), error);
}

void gnostr_plugin_context_nip44_decrypt_async(
    GnostrPluginContext *context, const char *peer_pubkey_hex,
    const char *ciphertext, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data) {
  (void)context;
  test_nip44_async(peer_pubkey_hex, ciphertext, FALSE, cancellable, callback,
                   user_data);
}

char *gnostr_plugin_context_nip44_decrypt_finish(GnostrPluginContext *context,
                                                 GAsyncResult *result,
                                                 GError **error) {
  (void)context;
  return g_task_propagate_pointer(G_TASK(result), error);
}

void gnostr_plugin_context_nip44_self_encrypt_async(
    GnostrPluginContext *context, const char *plaintext,
    GCancellable *cancellable, GAsyncReadyCallback callback,
    gpointer user_data) {
  (void)context;
  test_nip44_async(NULL, plaintext, TRUE, cancellable, callback, user_data);
}

char *gnostr_plugin_context_nip44_self_encrypt_finish(
    GnostrPluginContext *context, GAsyncResult *result, GError **error) {
  (void)context;
  return g_task_propagate_pointer(G_TASK(result), error);
}

void gnostr_plugin_context_nip44_self_decrypt_async(
    GnostrPluginContext *context, const char *ciphertext,
    GCancellable *cancellable, GAsyncReadyCallback callback,
    gpointer user_data) {
  (void)context;
  test_nip44_async(NULL, ciphertext, FALSE, cancellable, callback, user_data);
}

char *gnostr_plugin_context_nip44_self_decrypt_finish(
    GnostrPluginContext *context, GAsyncResult *result, GError **error) {
  (void)context;
  return g_task_propagate_pointer(G_TASK(result), error);
}
