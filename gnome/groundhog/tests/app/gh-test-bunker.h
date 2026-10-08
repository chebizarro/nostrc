#ifndef GH_TEST_BUNKER_H
#define GH_TEST_BUNKER_H

/* Recording NIP-46 transport. The test controls EOSE, relay OK and signer
 * replies independently; nothing is delivered by a timer or a live network. */
#include "gh-nip46-session.h"
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-filter.h>
#include <nostr-tag.h>
#include <nostr-utils.h>
#include <nostr/nip44/nip44.h>
#include <nostr/nip46/nip46_msg.h>
#include <nostr/nip46/nip46_envelope.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  GhRelayScope *scope;
  GhRelayPublish *publish;
  gchar *url;
  gchar *event_json;
  gchar *event_id;
  gboolean closed;
} BunkerHandle;

typedef struct {
  GPtrArray *scopes;
  GPtrArray *publishes;
  gchar *signer_secret;
  gchar *signer_pubkey;
  gchar *user_pubkey;
  gchar *client_pubkey;
  gchar *client_secret;
} TestBunker;

static void
bunker_handle_free(gpointer data)
{
  BunkerHandle *handle = data;
  if (handle->scope) gh_relay_scope_unref(handle->scope);
  if (handle->publish) gh_relay_publish_unref(handle->publish);
  g_free(handle->url);
  g_free(handle->event_json);
  g_free(handle->event_id);
  g_free(handle);
}

static void
bunker_init(TestBunker *bunker, const gchar *client_secret)
{
  memset(bunker, 0, sizeof *bunker);
  bunker->scopes = g_ptr_array_new_with_free_func(bunker_handle_free);
  bunker->publishes = g_ptr_array_new_with_free_func(bunker_handle_free);
  bunker->signer_secret = nostr_key_generate_private();
  bunker->signer_pubkey = nostr_key_get_public(bunker->signer_secret);
  bunker->user_pubkey = nostr_key_get_public(bunker->signer_secret);
  bunker->client_secret = g_strdup(client_secret);
  bunker->client_pubkey = nostr_key_get_public(client_secret);
}

static void G_GNUC_UNUSED
bunker_set_signer_secret(TestBunker *bunker, const gchar *secret)
{
  g_free(bunker->signer_secret);
  g_free(bunker->signer_pubkey);
  g_free(bunker->user_pubkey);
  bunker->signer_secret = g_strdup(secret);
  bunker->signer_pubkey = nostr_key_get_public(secret);
  bunker->user_pubkey = nostr_key_get_public(secret);
}

static void
bunker_clear(TestBunker *bunker)
{
  g_ptr_array_unref(bunker->scopes);
  g_ptr_array_unref(bunker->publishes);
  g_free(bunker->signer_secret);
  g_free(bunker->signer_pubkey);
  g_free(bunker->user_pubkey);
  g_free(bunker->client_secret);
  g_free(bunker->client_pubkey);
}

static gpointer
bunker_scope_open(GhRelayScope *scope, const gchar *url,
                  const NostrFilters *filters, gpointer data, GError **error)
{
  TestBunker *bunker = data;
  (void)error;
  g_assert_cmpuint(filters->count, ==, 1);
  const NostrFilter *filter = &filters->filters[0];
  g_assert_cmpint(nostr_filter_kinds_get(filter, 0), ==, 24133);
  g_assert_cmpstr(nostr_filter_tag_get(filter, 0, 1), ==, bunker->client_pubkey);
  BunkerHandle *handle = g_new0(BunkerHandle, 1);
  handle->scope = gh_relay_scope_ref(scope);
  handle->url = g_strdup(url);
  g_ptr_array_add(bunker->scopes, handle);
  return handle;
}

static gpointer
bunker_publish_open(GhRelayPublish *publish, const gchar *url,
                    const gchar *event_json, gpointer data, GError **error)
{
  TestBunker *bunker = data;
  (void)error;
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_signed(event, event_json, NULL), ==,
                  NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpint(nostr_event_get_kind(event), ==, 24133);
  g_assert_cmpstr(nostr_event_get_pubkey(event), ==, bunker->client_pubkey);
  BunkerHandle *handle = g_new0(BunkerHandle, 1);
  handle->publish = gh_relay_publish_ref(publish);
  handle->url = g_strdup(url);
  handle->event_json = g_strdup(event_json);
  handle->event_id = g_strdup(nostr_event_get_id(event));
  g_ptr_array_add(bunker->publishes, handle);
  nostr_event_free(event);
  return handle;
}

static void
bunker_close(gpointer handle, gpointer data)
{
  (void)data;
  ((BunkerHandle *)handle)->closed = TRUE;
}

static const GhRelayTransport bunker_scope_transport = {
  bunker_scope_open, bunker_close
};
static const GhRelayPublishTransport bunker_publish_transport = {
  bunker_publish_open, bunker_close
};

static void
bunker_eose(TestBunker *bunker)
{
  g_assert_cmpuint(bunker->scopes->len, >, 0);
  BunkerHandle *handle = g_ptr_array_index(bunker->scopes, bunker->scopes->len - 1);
  gh_relay_scope_eose(handle->scope, handle->url);
}

static BunkerHandle *
bunker_last_publish(TestBunker *bunker)
{
  g_assert_cmpuint(bunker->publishes->len, >, 0);
  return g_ptr_array_index(bunker->publishes, bunker->publishes->len - 1);
}

static G_GNUC_UNUSED void
bunker_accept(TestBunker *bunker)
{
  BunkerHandle *handle = bunker_last_publish(bunker);
  gh_relay_publish_ok(handle->publish, handle->url, handle->event_id, TRUE, "");
}

static gchar *
bunker_request_json_at(TestBunker *bunker, guint index)
{
  g_assert_cmpuint(index, <, bunker->publishes->len);
  BunkerHandle *handle = g_ptr_array_index(bunker->publishes, index);
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_signed(event, handle->event_json, NULL), ==,
                  NOSTR_EVENT_VALIDATION_OK);
  guint8 sk[32], pk[32], *plain = NULL;
  size_t length = 0;
  g_assert_true(nostr_hex2bin(sk, bunker->signer_secret, sizeof sk));
  g_assert_true(nostr_hex2bin(pk, bunker->client_pubkey, sizeof pk));
  g_assert_cmpint(nostr_nip44_decrypt_v2(sk, pk, nostr_event_get_content(event),
                                        &plain, &length), ==, 0);
  gchar *json = g_strndup((const gchar *)plain, length);
  free(plain);
  nostr_event_free(event);
  return json;
}

static G_GNUC_UNUSED gchar *
bunker_request_json(TestBunker *bunker)
{
  return bunker_request_json_at(bunker, bunker->publishes->len - 1);
}

static gchar *
bunker_response_event_json_from(TestBunker *bunker, const gchar *signer_secret,
                                const gchar *p_tag, const gchar *response_json)
{
  char *signer_pubkey = nostr_key_get_public(signer_secret);
  guint8 sk[32], pk[32];
  g_assert_true(nostr_hex2bin(sk, signer_secret, sizeof sk));
  g_assert_true(nostr_hex2bin(pk, bunker->client_pubkey, sizeof pk));
  char *ciphertext = NULL;
  g_assert_cmpint(nostr_nip44_encrypt_v2(sk, pk, (const guint8 *)response_json,
                strlen(response_json), &ciphertext), ==, 0);
  NostrEvent *event = NULL;
  g_assert_cmpint(nostr_nip46_build_response_event(signer_pubkey, p_tag,
                                                   response_json, &event), ==, 0);
  nostr_event_set_content(event, ciphertext);
  g_assert_cmpint(nostr_event_sign(event, signer_secret), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  free(ciphertext);
  nostr_event_free(event);
  free(signer_pubkey);
  return json;
}

static void
bunker_reply_from(TestBunker *bunker, const gchar *signer_secret,
                  const gchar *p_tag, const gchar *response_json)
{
  gchar *json = bunker_response_event_json_from(bunker, signer_secret,
                                                  p_tag, response_json);
  BunkerHandle *scope = g_ptr_array_index(bunker->scopes, 0);
  gh_relay_scope_event(scope->scope, scope->url, json);
  free(json);
}

static void
bunker_reply(TestBunker *bunker, const gchar *response_json)
{
  bunker_reply_from(bunker, bunker->signer_secret, bunker->client_pubkey,
                    response_json);
}

#endif
