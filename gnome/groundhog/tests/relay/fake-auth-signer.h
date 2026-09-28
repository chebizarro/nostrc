/* Scriptable NIP-42 signer for Groundhog relay tests: a GhRelayAuthSigner
 * whose async sign either signs the kind-22242 template with a fixed test
 * key, refuses, returns a tampered event, or holds the operation until the
 * test releases it. Completion goes through GTask, i.e. the caller's
 * thread-default main context. */
#ifndef GH_TEST_FAKE_AUTH_SIGNER_H
#define GH_TEST_FAKE_AUTH_SIGNER_H

#include "gh-relay-auth.h"

#include <gio/gio.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-tag.h>
#include <stdlib.h>
#include <string.h>

#define FAKE_SIGNER_SECRET \
  "0000000000000000000000000000000000000000000000000000000000000002"

typedef enum {
  FAKE_SIGN_OK,
  FAKE_SIGN_DENY,
  FAKE_SIGN_WRONG_CHALLENGE,
  FAKE_SIGN_WRONG_RELAY,
  FAKE_SIGN_BAD_SIG,
  FAKE_SIGN_HOLD
} FakeSignMode;

typedef struct {
  FakeSignMode mode;
  guint calls;
  gchar *last_unsigned;
  GTask *held;
} FakeSigner;

static G_GNUC_UNUSED gchar *
fake_sign_json(const gchar *unsigned_json, FakeSignMode mode)
{
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(event, unsigned_json, NULL), ==, 1);
  if (mode == FAKE_SIGN_WRONG_CHALLENGE || mode == FAKE_SIGN_WRONG_RELAY) {
    NostrTags *tags = nostr_event_get_tags(event);
    const gchar *relay = NULL, *challenge = NULL;
    for (size_t i = 0; i < nostr_tags_size(tags); i++) {
      NostrTag *tag = nostr_tags_get(tags, i);
      if (g_strcmp0(nostr_tag_get_key(tag), "relay") == 0)
        relay = nostr_tag_get_value(tag);
      else if (g_strcmp0(nostr_tag_get_key(tag), "challenge") == 0)
        challenge = nostr_tag_get_value(tag);
    }
    g_autofree gchar *keep_relay = g_strdup(relay);
    g_autofree gchar *keep_challenge = g_strdup(challenge);
    nostr_event_set_tags(event, nostr_tags_new(2,
      nostr_tag_new("relay", mode == FAKE_SIGN_WRONG_RELAY
                               ? "wss://elsewhere.example" : keep_relay, NULL),
      nostr_tag_new("challenge", mode == FAKE_SIGN_WRONG_CHALLENGE
                                   ? "not-the-challenge" : keep_challenge, NULL)));
  }
  g_assert_cmpint(nostr_event_sign(event, FAKE_SIGNER_SECRET), ==, 0);
  char *raw = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  g_assert_nonnull(raw);
  gchar *json = g_strdup(raw);
  free(raw);
  if (mode == FAKE_SIGN_BAD_SIG) {
    gchar *sig = strstr(json, "\"sig\":\"");
    g_assert_nonnull(sig);
    sig += strlen("\"sig\":\"");
    *sig = *sig == '0' ? '1' : '0'; /* no longer verifies */
  }
  return json;
}

static G_GNUC_UNUSED void
fake_sign_async(gpointer user_data, const gchar *unsigned_json,
                GCancellable *cancellable, GAsyncReadyCallback callback,
                gpointer callback_data)
{
  FakeSigner *signer = user_data;
  signer->calls++;
  g_free(signer->last_unsigned);
  signer->last_unsigned = g_strdup(unsigned_json);
  GTask *task = g_task_new(NULL, cancellable, callback, callback_data);
  switch (signer->mode) {
  case FAKE_SIGN_HOLD:
    g_assert_null(signer->held);
    signer->held = task;
    return;
  case FAKE_SIGN_DENY:
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "user refused to sign");
    break;
  case FAKE_SIGN_OK:
  case FAKE_SIGN_WRONG_CHALLENGE:
  case FAKE_SIGN_WRONG_RELAY:
  case FAKE_SIGN_BAD_SIG:
    g_task_return_pointer(task, fake_sign_json(unsigned_json, signer->mode), g_free);
    break;
  }
  g_object_unref(task);
}

static G_GNUC_UNUSED gchar *
fake_sign_finish(GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer(G_TASK(result), error);
}

static G_GNUC_UNUSED GhRelayAuthSigner *
fake_signer_new(FakeSigner *signer, guint64 generation,
                GCancellable *generation_cancellable)
{
  return gh_relay_auth_signer_new(generation, generation_cancellable, NULL,
                                  fake_sign_async, fake_sign_finish, signer, NULL);
}

/* Completes a held sign with a valid signature; returns whether the held
 * operation had been cancelled by then. */
static G_GNUC_UNUSED gboolean
fake_signer_release(FakeSigner *signer)
{
  GTask *task = g_steal_pointer(&signer->held);
  g_assert_nonnull(task);
  gboolean cancelled = g_cancellable_is_cancelled(g_task_get_cancellable(task));
  g_task_return_pointer(task, fake_sign_json(signer->last_unsigned, FAKE_SIGN_OK),
                        g_free);
  g_object_unref(task);
  return cancelled;
}

/* The pubkey the fake ACCOUNT signer signs as. */
static G_GNUC_UNUSED gchar *
fake_account_pubkey(void)
{
  char *pubkey = nostr_key_get_public(FAKE_SIGNER_SECRET);
  g_assert_nonnull(pubkey);
  gchar *copy = g_strdup(pubkey);
  free(pubkey);
  return copy;
}

/* The pubkey of a signed event JSON. */
static G_GNUC_UNUSED gchar *
auth_event_pubkey(const gchar *signed_json)
{
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_signed(event, signed_json, NULL), ==,
                  NOSTR_EVENT_VALIDATION_OK);
  gchar *pubkey = g_strdup(nostr_event_get_pubkey(event));
  nostr_event_free(event);
  g_assert_cmpuint(strlen(pubkey), ==, 64);
  return pubkey;
}

static G_GNUC_UNUSED void
fake_signer_clear(FakeSigner *signer)
{
  g_assert_null(signer->held);
  g_free(signer->last_unsigned);
}

/* Iterates the default context until *counter reaches count; the bound is
 * a failure bound only. */
static G_GNUC_UNUSED void
iterate_until(const guint *counter, guint count)
{
  gint64 bound = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
  while (*counter < count && g_get_monotonic_time() < bound)
    g_main_context_iteration(NULL, g_main_context_pending(NULL));
  g_assert_cmpuint(*counter, ==, count);
}

static G_GNUC_UNUSED void
drain_pending(void)
{
  while (g_main_context_pending(NULL))
    g_main_context_iteration(NULL, FALSE);
}

#endif
