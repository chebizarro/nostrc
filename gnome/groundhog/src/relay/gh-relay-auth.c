#include "gh-relay-auth.h"

#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-tag.h>
#include <secure_buf.h>
#include <stdlib.h>
#include <string.h>

struct _GhRelayAuthSigner {
  gint refs;
  guint64 generation;
  gint revoked;                     /* atomic */
  GCancellable *generation_cancellable;
  gulong generation_handler;
  gchar *expected_pubkey;
  GhRelayAuthSignAsyncFunc sign_async;
  GhRelayAuthSignFinishFunc sign_finish;
  gpointer user_data;
  GDestroyNotify user_data_destroy;
  GMutex lock;
  GPtrArray *pending;               /* GCancellable of attempts in flight */
};

struct _GhRelayAuthAttempt {
  gint refs;
  GhRelayAuthSigner *signer;        /* ACCOUNT only; NULL for EPHEMERAL */
  GCancellable *cancellable;
  gchar *url;
  gchar *challenge;
  gchar *unsigned_json;
  GhRelayAuthAttemptFunc callback;
  gpointer owner;                   /* NULL once dropped or completed */
};

static void
on_generation_cancelled(GCancellable *cancellable, gpointer data)
{
  (void)cancellable;
  gh_relay_auth_signer_revoke(data);
}

GhRelayAuthSigner *
gh_relay_auth_signer_new(guint64 generation, GCancellable *generation_cancellable,
                         const gchar *expected_pubkey,
                         GhRelayAuthSignAsyncFunc sign_async,
                         GhRelayAuthSignFinishFunc sign_finish,
                         gpointer user_data, GDestroyNotify user_data_destroy)
{
  g_return_val_if_fail(sign_async != NULL && sign_finish != NULL, NULL);
  g_return_val_if_fail(!generation_cancellable ||
                       G_IS_CANCELLABLE(generation_cancellable), NULL);
  GhRelayAuthSigner *signer = g_new0(GhRelayAuthSigner, 1);
  signer->refs = 1;
  signer->generation = generation;
  signer->expected_pubkey = g_strdup(expected_pubkey);
  signer->sign_async = sign_async;
  signer->sign_finish = sign_finish;
  signer->user_data = user_data;
  signer->user_data_destroy = user_data_destroy;
  g_mutex_init(&signer->lock);
  signer->pending = g_ptr_array_new_with_free_func(g_object_unref);
  if (generation_cancellable) {
    /* A plain signal handler rather than g_cancellable_connect(): it may be
     * disconnected from inside a "cancelled" emission without deadlocking. */
    signer->generation_cancellable = g_object_ref(generation_cancellable);
    signer->generation_handler = g_signal_connect(generation_cancellable,
      "cancelled", G_CALLBACK(on_generation_cancelled), signer);
    if (g_cancellable_is_cancelled(generation_cancellable))
      gh_relay_auth_signer_revoke(signer);
  }
  return signer;
}

GhRelayAuthSigner *
gh_relay_auth_signer_ref(GhRelayAuthSigner *signer)
{
  g_return_val_if_fail(signer != NULL, NULL);
  g_atomic_int_inc(&signer->refs);
  return signer;
}

void
gh_relay_auth_signer_unref(GhRelayAuthSigner *signer)
{
  if (!signer || !g_atomic_int_dec_and_test(&signer->refs))
    return;
  if (signer->generation_handler)
    g_signal_handler_disconnect(signer->generation_cancellable,
                                signer->generation_handler);
  g_clear_object(&signer->generation_cancellable);
  g_ptr_array_unref(signer->pending); /* attempts hold a signer reference */
  g_mutex_clear(&signer->lock);
  if (signer->user_data_destroy)
    signer->user_data_destroy(signer->user_data);
  g_free(signer->expected_pubkey);
  g_free(signer);
}

void
gh_relay_auth_signer_revoke(GhRelayAuthSigner *signer)
{
  g_return_if_fail(signer != NULL);
  if (!g_atomic_int_compare_and_exchange(&signer->revoked, FALSE, TRUE))
    return;
  /* Cancel outside the lock: a sign implementation may complete (and an
   * attempt release itself) from inside its cancellable's handler. */
  GPtrArray *pending = g_ptr_array_new_with_free_func(g_object_unref);
  g_mutex_lock(&signer->lock);
  for (guint i = 0; i < signer->pending->len; i++)
    g_ptr_array_add(pending, g_object_ref(g_ptr_array_index(signer->pending, i)));
  g_mutex_unlock(&signer->lock);
  for (guint i = 0; i < pending->len; i++)
    g_cancellable_cancel(g_ptr_array_index(pending, i));
  g_ptr_array_unref(pending);
}

gboolean
gh_relay_auth_signer_is_revoked(const GhRelayAuthSigner *signer)
{
  return !signer || g_atomic_int_get(&signer->revoked);
}

guint64
gh_relay_auth_signer_get_generation(const GhRelayAuthSigner *signer)
{
  return signer ? signer->generation : 0;
}

gboolean
gh_relay_auth_is_required(const gchar *message)
{
  return message && g_str_has_prefix(message, "auth-required:");
}

gchar *
gh_relay_auth_build_unsigned(const gchar *relay_url, const gchar *challenge,
                             const gchar *pubkey, gint64 created_at)
{
  g_return_val_if_fail(relay_url != NULL && challenge != NULL, NULL);
  NostrEvent *event = nostr_event_new();
  NostrTags *tags = nostr_tags_new(2, nostr_tag_new("relay", relay_url, NULL),
                                   nostr_tag_new("challenge", challenge, NULL));
  if (!event || !tags) {
    if (tags)
      nostr_tags_free(tags);
    if (event)
      nostr_event_free(event);
    return NULL;
  }
  nostr_event_set_kind(event, GH_RELAY_AUTH_KIND);
  nostr_event_set_created_at(event, created_at);
  nostr_event_set_content(event, "");
  if (pubkey)
    nostr_event_set_pubkey(event, pubkey);
  nostr_event_set_tags(event, tags);
  char *raw = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  gchar *json = raw ? g_strdup(raw) : NULL;
  free(raw);
  return json;
}

/* Hex secret -> 32 bytes in locked, wiped-on-free memory. */
static gboolean
secret_from_hex(const gchar *hex, nostr_secure_buf *out)
{
  if (!hex || strlen(hex) != 64)
    return FALSE;
  *out = secure_alloc(32);
  if (!out->ptr)
    return FALSE;
  guint8 *bytes = out->ptr;
  for (gsize i = 0; i < 32; i++) {
    gint hi = g_ascii_xdigit_value(hex[2 * i]);
    gint lo = g_ascii_xdigit_value(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      secure_free(out);
      return FALSE;
    }
    bytes[i] = (guint8)((hi << 4) | lo);
  }
  return TRUE;
}

gchar *
gh_relay_auth_sign_ephemeral(const gchar *relay_url, const gchar *challenge,
                             gint64 created_at, GError **error)
{
  g_return_val_if_fail(relay_url != NULL && challenge != NULL, NULL);
  gchar *signed_json = NULL;
  NostrEvent *event = NULL;
  NostrTags *tags = NULL;
  nostr_secure_buf secret = {0};
  char *secret_hex = nostr_key_generate_private();
  char *pubkey = secret_hex ? nostr_key_get_public(secret_hex) : NULL;
  gboolean have_secret = secret_from_hex(secret_hex, &secret);
  if (secret_hex) {
    secure_wipe(secret_hex, strlen(secret_hex));
    free(secret_hex);
  }
  if (!pubkey || !have_secret)
    goto out;
  event = nostr_event_new();
  tags = nostr_tags_new(2, nostr_tag_new("relay", relay_url, NULL),
                        nostr_tag_new("challenge", challenge, NULL));
  if (!event || !tags)
    goto out;
  nostr_event_set_kind(event, GH_RELAY_AUTH_KIND);
  nostr_event_set_created_at(event, created_at);
  nostr_event_set_content(event, "");
  nostr_event_set_pubkey(event, pubkey);
  nostr_event_set_tags(event, g_steal_pointer(&tags));
  if (nostr_event_sign_secure(event, &secret) == 0) {
    char *raw = nostr_event_serialize_compact(event);
    signed_json = raw ? g_strdup(raw) : NULL;
    free(raw);
  }
out:
  if (have_secret)
    secure_free(&secret);
  if (tags)
    nostr_tags_free(tags);
  if (event)
    nostr_event_free(event);
  free(pubkey);
  if (!signed_json)
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "could not sign an ephemeral AUTH event");
  return signed_json;
}

/* Every tag named @key must carry @value, and there must be one. */
static gboolean
tags_bind(NostrTags *tags, const gchar *key, const gchar *value)
{
  gboolean seen = FALSE;
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (g_strcmp0(nostr_tag_get_key(tag), key) != 0)
      continue;
    if (nostr_tag_size(tag) < 2 || g_strcmp0(nostr_tag_get_value(tag), value) != 0)
      return FALSE;
    seen = TRUE;
  }
  return seen;
}

static gboolean
reject(GError **error, const gchar *why)
{
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
              "signed AUTH event rejected: %s", why);
  return FALSE;
}

gboolean
gh_relay_auth_verify_signed(const gchar *signed_json, const gchar *relay_url,
                            const gchar *challenge, const gchar *expected_pubkey,
                            gint64 now, gchar *event_id, GError **error)
{
  if (!signed_json || !relay_url || !challenge)
    return reject(error, "missing");
  NostrEvent *event = nostr_event_new();
  if (!event)
    return reject(error, "allocation failed");
  gchar id[65] = {0};
  gboolean ok = FALSE;
  const gchar *why = NULL;
  if (nostr_event_deserialize_signed(event, signed_json, NULL) != NOSTR_EVENT_VALIDATION_OK ||
      nostr_event_validate(event, id) != NOSTR_EVENT_VALIDATION_OK)
    why = "invalid id or signature";
  else if (nostr_event_get_kind(event) != GH_RELAY_AUTH_KIND)
    why = "not kind 22242";
  else if (!tags_bind(nostr_event_get_tags(event), "relay", relay_url))
    why = "relay tag does not name this relay";
  else if (!tags_bind(nostr_event_get_tags(event), "challenge", challenge))
    why = "challenge tag does not match";
  else if (ABS(nostr_event_get_created_at(event) - now) > GH_RELAY_AUTH_MAX_SKEW_SECONDS)
    why = "created_at too far from now";
  else if (g_strcmp0(nostr_event_get_content(event), "") != 0)
    why = "content is not empty";
  else if (expected_pubkey && g_strcmp0(nostr_event_get_pubkey(event), expected_pubkey) != 0)
    why = "signed by another pubkey";
  else
    ok = TRUE;
  nostr_event_free(event);
  if (!ok)
    return reject(error, why);
  if (event_id)
    memcpy(event_id, id, sizeof id);
  return TRUE;
}

static void
attempt_unref(GhRelayAuthAttempt *attempt)
{
  if (!g_atomic_int_dec_and_test(&attempt->refs))
    return;
  GhRelayAuthSigner *signer = attempt->signer;
  if (signer) {
    g_mutex_lock(&signer->lock);
    g_ptr_array_remove(signer->pending, attempt->cancellable);
    g_mutex_unlock(&signer->lock);
    gh_relay_auth_signer_unref(signer);
  }
  g_clear_object(&attempt->cancellable);
  g_free(attempt->url);
  g_free(attempt->challenge);
  g_free(attempt->unsigned_json);
  g_free(attempt);
}

/* Verifies and hands the result to a still-attached owner, then drops the
 * operation's reference. */
static void
complete(GhRelayAuthAttempt *attempt, const gchar *signed_json, GError *error)
{
  GhRelayAuthSigner *signer = attempt->signer;
  if (attempt->owner) {
    gchar id[65] = {0};
    if (!error && signer && gh_relay_auth_signer_is_revoked(signer))
      g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                          "AUTH signer revoked with its account generation");
    if (!error && !signed_json)
      g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                          "AUTH signer returned no event");
    if (!error)
      gh_relay_auth_verify_signed(signed_json, attempt->url, attempt->challenge,
                                  signer ? signer->expected_pubkey : NULL,
                                  g_get_real_time() / G_USEC_PER_SEC, id, &error);
    gpointer owner = attempt->owner;
    attempt->owner = NULL;
    attempt_unref(attempt); /* the owner's reference */
    attempt->callback(owner, error ? NULL : signed_json, error ? NULL : id, error);
  }
  g_clear_error(&error);
  attempt_unref(attempt);   /* the operation's reference */
}

static void
on_signed(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  GhRelayAuthAttempt *attempt = data;
  GError *error = NULL;
  g_autofree gchar *signed_json = attempt->signer->sign_finish(result, &error);
  complete(attempt, signed_json, error);
}

static gboolean
begin_sign(gpointer data)
{
  GhRelayAuthAttempt *attempt = data;
  if (!attempt->owner || g_cancellable_is_cancelled(attempt->cancellable)) {
    attempt_unref(attempt); /* dropped before anything was signed */
    return G_SOURCE_REMOVE;
  }
  if (!attempt->signer) {
    /* EPHEMERAL: a new key for this attempt only, wiped once signed. */
    GError *error = NULL;
    g_autofree gchar *signed_json = gh_relay_auth_sign_ephemeral(
      attempt->url, attempt->challenge, g_get_real_time() / G_USEC_PER_SEC, &error);
    complete(attempt, signed_json, error);
    return G_SOURCE_REMOVE;
  }
  attempt->signer->sign_async(attempt->signer->user_data, attempt->unsigned_json,
                              attempt->cancellable, on_signed, attempt);
  return G_SOURCE_REMOVE;
}

GhRelayAuthAttempt *
gh_relay_auth_attempt_start(GhRelayAuthMode mode, GhRelayAuthSigner *signer,
                            guint64 generation, const gchar *relay_url,
                            const gchar *challenge, GhRelayAuthAttemptFunc callback,
                            gpointer owner, GError **error)
{
  g_return_val_if_fail(relay_url && challenge && callback && owner, NULL);
  if (mode == GH_RELAY_AUTH_NONE) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        "AUTH is not enabled for this relay");
    return NULL;
  }
  gchar *unsigned_json = NULL;
  if (mode == GH_RELAY_AUTH_ACCOUNT) {
    if (!signer || gh_relay_auth_signer_is_revoked(signer) ||
        signer->generation != generation) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                          "AUTH signer does not belong to this account generation");
      return NULL;
    }
    unsigned_json = gh_relay_auth_build_unsigned(relay_url, challenge,
      signer->expected_pubkey, g_get_real_time() / G_USEC_PER_SEC);
    if (!unsigned_json) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                          "could not build the AUTH event");
      return NULL;
    }
  } else {
    signer = NULL; /* EPHEMERAL never touches the account signer */
  }
  GhRelayAuthAttempt *attempt = g_new0(GhRelayAuthAttempt, 1);
  attempt->refs = 2; /* the owner, and the pending idle/sign operation */
  attempt->signer = signer ? gh_relay_auth_signer_ref(signer) : NULL;
  attempt->cancellable = g_cancellable_new();
  attempt->url = g_strdup(relay_url);
  attempt->challenge = g_strdup(challenge);
  attempt->unsigned_json = unsigned_json;
  attempt->callback = callback;
  attempt->owner = owner;
  if (signer) {
    g_mutex_lock(&signer->lock);
    g_ptr_array_add(signer->pending, g_object_ref(attempt->cancellable));
    g_mutex_unlock(&signer->lock);
  }

  GSource *source = g_idle_source_new();
  g_source_set_priority(source, G_PRIORITY_DEFAULT);
  g_source_set_callback(source, begin_sign, attempt, NULL);
  g_source_attach(source, g_main_context_get_thread_default());
  g_source_unref(source);
  return attempt;
}

void
gh_relay_auth_attempt_drop(GhRelayAuthAttempt *attempt)
{
  if (!attempt)
    return;
  attempt->owner = NULL;
  g_cancellable_cancel(attempt->cancellable);
  attempt_unref(attempt);
}
