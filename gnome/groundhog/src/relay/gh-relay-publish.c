#include "gh-relay-publish.h"
#include "gh-relay-scope.h"

#include <gio/gio.h>
#include <nostr-event.h>
#include <string.h>

#define GH_PUBLISH_MAX_RELAYS 16
#define GH_PUBLISH_DEFAULT_DEADLINE_SECONDS 30
#define GH_PUBLISH_MAX_DEADLINE_SECONDS 300

typedef struct {
  GhRelayPublish *publish; /* owner; endpoints never outlive it */
  gchar *url;
  gpointer handle;
  gboolean opened;
  GhRelayPublishOutcome outcome;
  GSource *deadline;
  GhRelayAuthMode auth_mode;   /* caller's identity choice; NONE default */
  /* NIP-42 state of this URL's connection. */
  gchar *challenge;            /* latest challenge */
  gchar *auth_challenge;       /* challenge an AUTH was attempted for */
  GhRelayAuthAttempt *attempt; /* signing in flight */
  gchar auth_event_id[65];     /* AUTH sent, its OK pending; "" otherwise */
  gchar *auth_message;         /* the relay's auth-required OK message */
  gboolean auth_needed;        /* the EVENT awaits an authenticated re-send */
  gboolean resent;             /* the one re-send has been made */
} GhPublishEndpoint;

struct _GhRelayPublish {
  gint refs;
  guint64 generation;
  gchar *event_json;
  gchar event_id[65];
  GhRelayPublishTransport transport;
  GhRelayPublishAuthTransport auth_transport;
  gpointer transport_data;
  GhRelayPublishUpdateFunc update;
  GhRelayPublishDoneFunc done;
  gpointer user_data;
  GMainContext *context;
  GPtrArray *endpoints;  /* GhPublishEndpoint, in add order */
  GHashTable *by_url;    /* url -> borrowed endpoint */
  guint deadline_seconds;
  guint terminal;
  GhRelayAuthSigner *signer;   /* account signer, for ACCOUNT URLs only */
  GhRelayPublishSummary summary;
  gchar *isolation;      /* Tor stream isolation label, random per publish */
  gboolean started;
  gboolean cancelled;
  gboolean completed;
};

/* gh_relay_publish_new()'s transport (gh_relay_publish_set_default_transport()). */
static GMutex default_lock;
static GhRelayPublishTransport default_transport;
static GhRelayPublishAuthTransport default_auth;
static gpointer default_data;
static gboolean default_set;

static const struct {
  const gchar *prefix;
  GhRelayOkPrefix value;
} ok_prefixes[] = {
  { "duplicate:", GH_RELAY_OK_PREFIX_DUPLICATE },
  { "pow:", GH_RELAY_OK_PREFIX_POW },
  { "blocked:", GH_RELAY_OK_PREFIX_BLOCKED },
  { "rate-limited:", GH_RELAY_OK_PREFIX_RATE_LIMITED },
  { "invalid:", GH_RELAY_OK_PREFIX_INVALID },
  { "restricted:", GH_RELAY_OK_PREFIX_RESTRICTED },
  { "mute:", GH_RELAY_OK_PREFIX_MUTE },
  { "error:", GH_RELAY_OK_PREFIX_ERROR },
  { "auth-required:", GH_RELAY_OK_PREFIX_AUTH_REQUIRED },
};

GhRelayOkPrefix
gh_relay_ok_prefix_classify(const gchar *message)
{
  if (!message)
    return GH_RELAY_OK_PREFIX_NONE;
  for (gsize i = 0; i < G_N_ELEMENTS(ok_prefixes); i++) {
    if (g_str_has_prefix(message, ok_prefixes[i].prefix))
      return ok_prefixes[i].value;
  }
  return GH_RELAY_OK_PREFIX_NONE;
}

static gboolean
on_owner_context(const GhRelayPublish *publish)
{
  GMainContext *context = g_main_context_get_thread_default();
  return (context ? context : g_main_context_default()) == publish->context;
}

static void
clear_deadline(GhPublishEndpoint *endpoint)
{
  if (!endpoint->deadline)
    return;
  g_source_destroy(endpoint->deadline);
  g_clear_pointer(&endpoint->deadline, g_source_unref);
}

static void
close_transport(GhRelayPublish *publish, GhPublishEndpoint *endpoint)
{
  clear_deadline(endpoint);
  g_clear_pointer(&endpoint->attempt, gh_relay_auth_attempt_drop);
  endpoint->auth_event_id[0] = '\0';
  if (!endpoint->opened)
    return;
  gpointer handle = endpoint->handle;
  endpoint->opened = FALSE;
  endpoint->handle = NULL;
  publish->transport.close(handle, publish->transport_data);
}

static void
endpoint_free(gpointer data)
{
  GhPublishEndpoint *endpoint = data;
  clear_deadline(endpoint);
  g_clear_pointer(&endpoint->attempt, gh_relay_auth_attempt_drop);
  g_free(endpoint->challenge);
  g_free(endpoint->auth_challenge);
  g_free(endpoint->auth_message);
  g_free(endpoint->url);
  g_free(endpoint);
}

static gboolean
validate_signed(const gchar *event_json, gchar id[65])
{
  if (!event_json)
    return FALSE;
  NostrEvent *event = nostr_event_new();
  if (!event)
    return FALSE;
  gboolean valid = nostr_event_deserialize_signed(event, event_json, NULL) ==
                     NOSTR_EVENT_VALIDATION_OK &&
                   nostr_event_validate(event, id) == NOSTR_EVENT_VALIDATION_OK;
  nostr_event_free(event);
  return valid;
}

GhRelayPublish *
gh_relay_publish_new_with_transport(guint64 generation, const gchar *event_json,
                                    const GhRelayPublishTransport *transport,
                                    gpointer transport_data,
                                    GhRelayPublishUpdateFunc update,
                                    GhRelayPublishDoneFunc done,
                                    gpointer user_data, GError **error)
{
  g_return_val_if_fail(transport && transport->open && transport->close, NULL);
  gchar id[65] = {0};
  if (!validate_signed(event_json, id)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "publish requires a signed event with a valid id and sig");
    return NULL;
  }
  GhRelayPublish *publish = g_new0(GhRelayPublish, 1);
  publish->refs = 1;
  publish->generation = generation;
  publish->event_json = g_strdup(event_json);
  g_strlcpy(publish->event_id, id, sizeof publish->event_id);
  publish->transport = *transport;
  publish->transport_data = transport_data;
  publish->update = update;
  publish->done = done;
  publish->user_data = user_data;
  publish->context = g_main_context_ref_thread_default();
  publish->endpoints = g_ptr_array_new_with_free_func(endpoint_free);
  publish->by_url = g_hash_table_new(g_str_hash, g_str_equal);
  publish->deadline_seconds = GH_PUBLISH_DEFAULT_DEADLINE_SECONDS;
  publish->isolation = g_uuid_string_random();
  return publish;
}

void
gh_relay_publish_set_default_transport(const GhRelayPublishTransport *transport,
                                       const GhRelayPublishAuthTransport *auth,
                                       gpointer transport_data)
{
  g_return_if_fail(!transport || (transport->open && transport->close));
  g_return_if_fail(!auth || (auth->send_auth && auth->resend));
  g_mutex_lock(&default_lock);
  default_set = transport != NULL;
  memset(&default_transport, 0, sizeof default_transport);
  memset(&default_auth, 0, sizeof default_auth);
  default_data = NULL;
  if (transport) {
    default_transport = *transport;
    if (auth)
      default_auth = *auth;
    default_data = transport_data;
  }
  g_mutex_unlock(&default_lock);
}

GhRelayPublish *
gh_relay_publish_new(guint64 generation, const gchar *event_json,
                     GhRelayPublishUpdateFunc update, GhRelayPublishDoneFunc done,
                     gpointer user_data, GError **error)
{
  GhRelayPublishTransport transport = gh_relay_publish_gnostr_transport;
  GhRelayPublishAuthTransport auth = gh_relay_publish_gnostr_auth_transport;
  gpointer data = NULL;
  g_mutex_lock(&default_lock);
  if (default_set) {
    transport = default_transport;
    auth = default_auth;
    data = default_data;
  }
  g_mutex_unlock(&default_lock);
  GhRelayPublish *publish = gh_relay_publish_new_with_transport(generation,
    event_json, &transport, data, update, done, user_data, error);
  if (publish && auth.send_auth)
    publish->auth_transport = auth;
  return publish;
}

const gchar *
gh_relay_publish_get_isolation(const GhRelayPublish *publish)
{
  g_return_val_if_fail(publish != NULL, NULL);
  return publish->isolation;
}

void
gh_relay_publish_set_auth_transport(GhRelayPublish *publish,
                                    const GhRelayPublishAuthTransport *auth)
{
  g_return_if_fail(publish != NULL && !publish->started);
  g_return_if_fail(!auth || (auth->send_auth && auth->resend));
  if (auth)
    publish->auth_transport = *auth;
  else
    memset(&publish->auth_transport, 0, sizeof publish->auth_transport);
}

GhRelayPublish *
gh_relay_publish_ref(GhRelayPublish *publish)
{
  g_return_val_if_fail(publish != NULL, NULL);
  g_atomic_int_inc(&publish->refs);
  return publish;
}

static void
revoke_generation(GhRelayPublish *publish)
{
  if (publish->cancelled)
    return;
  publish->cancelled = TRUE;
  publish->generation++;
  for (guint i = 0; i < publish->endpoints->len; i++) {
    GhPublishEndpoint *endpoint = g_ptr_array_index(publish->endpoints, i);
    if (endpoint->outcome == GH_RELAY_PUBLISH_PENDING)
      endpoint->outcome = GH_RELAY_PUBLISH_CANCELLED;
    close_transport(publish, endpoint);
  }
}

void
gh_relay_publish_cancel(GhRelayPublish *publish)
{
  g_return_if_fail(publish != NULL);
  g_return_if_fail(on_owner_context(publish));
  revoke_generation(publish);
}

void
gh_relay_publish_unref(GhRelayPublish *publish)
{
  if (!publish || !g_atomic_int_dec_and_test(&publish->refs))
    return;
  revoke_generation(publish);
  gh_relay_auth_signer_unref(publish->signer);
  g_ptr_array_unref(publish->endpoints);
  g_hash_table_unref(publish->by_url);
  g_main_context_unref(publish->context);
  g_free(publish->event_json);
  g_free(publish->isolation);
  g_free(publish);
}

guint64
gh_relay_publish_get_generation(const GhRelayPublish *publish)
{
  return publish ? publish->generation : 0;
}

const gchar *
gh_relay_publish_get_event_id(const GhRelayPublish *publish)
{
  return publish ? publish->event_id : NULL;
}

GhRelayPublishOutcome
gh_relay_publish_get_outcome(const GhRelayPublish *publish, const gchar *url)
{
  GhPublishEndpoint *endpoint =
    publish && url ? g_hash_table_lookup(publish->by_url, url) : NULL;
  return endpoint ? endpoint->outcome : GH_RELAY_PUBLISH_PENDING;
}

gboolean
gh_relay_publish_is_complete(const GhRelayPublish *publish)
{
  return publish && publish->completed;
}

gboolean
gh_relay_publish_is_signing_in(const GhRelayPublish *publish, const gchar *url)
{
  GhPublishEndpoint *endpoint =
    publish && url ? g_hash_table_lookup(publish->by_url, url) : NULL;
  return endpoint && !publish->cancelled && endpoint->outcome == GH_RELAY_PUBLISH_PENDING &&
         endpoint->attempt != NULL;
}

void
gh_relay_publish_set_deadline(GhRelayPublish *publish, guint seconds)
{
  g_return_if_fail(publish != NULL);
  g_return_if_fail(!publish->started);
  publish->deadline_seconds = CLAMP(seconds, 1, GH_PUBLISH_MAX_DEADLINE_SECONDS);
}

gboolean
gh_relay_publish_set_account_signer(GhRelayPublish *publish,
                                    GhRelayAuthSigner *signer, GError **error)
{
  g_return_val_if_fail(publish != NULL, FALSE);
  if (publish->cancelled) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                        "relay publish cancelled");
    return FALSE;
  }
  if (publish->started) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                        "set the AUTH signer before the publish starts");
    return FALSE;
  }
  if (signer && (gh_relay_auth_signer_is_revoked(signer) ||
                 gh_relay_auth_signer_get_generation(signer) != publish->generation)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        "AUTH signer does not belong to this account generation");
    return FALSE;
  }
  for (guint i = 0; !signer && i < publish->endpoints->len; i++) {
    GhPublishEndpoint *endpoint = g_ptr_array_index(publish->endpoints, i);
    if (endpoint->auth_mode == GH_RELAY_AUTH_ACCOUNT) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                          "a URL still authenticates as the account");
      return FALSE;
    }
  }
  if (signer)
    gh_relay_auth_signer_ref(signer);
  gh_relay_auth_signer_unref(publish->signer);
  publish->signer = signer;
  return TRUE;
}

gboolean
gh_relay_publish_set_url_auth(GhRelayPublish *publish, const gchar *url,
                              GhRelayAuthMode mode, GError **error)
{
  g_return_val_if_fail(publish != NULL, FALSE);
  if (publish->cancelled) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                        "relay publish cancelled");
    return FALSE;
  }
  if (publish->started) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                        "choose AUTH identities before the publish starts");
    return FALSE;
  }
  GhPublishEndpoint *endpoint = url ? g_hash_table_lookup(publish->by_url, url) : NULL;
  if (!endpoint) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "relay URL is not in this publish");
    return FALSE;
  }
  if (mode == GH_RELAY_AUTH_ACCOUNT && !publish->signer) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        "account AUTH needs this generation's account signer");
    return FALSE;
  }
  endpoint->auth_mode = mode;
  return TRUE;
}

gboolean
gh_relay_publish_add_url(GhRelayPublish *publish, const gchar *url,
                         GError **error)
{
  g_return_val_if_fail(publish != NULL, FALSE);
  if (publish->cancelled) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                        "relay publish cancelled");
    return FALSE;
  }
  if (publish->started) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                        "relay publish already started");
    return FALSE;
  }
  if (!gh_relay_url_validate(url, error))
    return FALSE;
  if (g_hash_table_contains(publish->by_url, url))
    return TRUE;
  if (publish->endpoints->len >= GH_PUBLISH_MAX_RELAYS) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                        "relay publish exceeds 16 URLs");
    return FALSE;
  }
  GhPublishEndpoint *endpoint = g_new0(GhPublishEndpoint, 1);
  endpoint->publish = publish;
  endpoint->url = g_strdup(url);
  g_ptr_array_add(publish->endpoints, endpoint);
  g_hash_table_insert(publish->by_url, endpoint->url, endpoint);
  return TRUE;
}

/* Records a URL's single terminal outcome, releases its connection, and emits
 * the update and, once every URL is terminal, the completion. */
static void
finish_endpoint(GhRelayPublish *publish, GhPublishEndpoint *endpoint,
                GhRelayPublishOutcome outcome, GhRelayOkPrefix prefix,
                const gchar *message)
{
  if (publish->cancelled || endpoint->outcome != GH_RELAY_PUBLISH_PENDING)
    return;
  endpoint->outcome = outcome;
  publish->terminal++;
  switch (outcome) {
  case GH_RELAY_PUBLISH_ACCEPTED: publish->summary.accepted++; break;
  case GH_RELAY_PUBLISH_REJECTED: publish->summary.rejected++; break;
  case GH_RELAY_PUBLISH_AUTH_REQUIRED: publish->summary.auth_required++; break;
  case GH_RELAY_PUBLISH_CONNECTION_FAILED: publish->summary.connection_failed++; break;
  case GH_RELAY_PUBLISH_PENDING:
  case GH_RELAY_PUBLISH_CANCELLED: g_assert_not_reached();
  }

  gh_relay_publish_ref(publish); /* keeps the endpoint alive, too */
  close_transport(publish, endpoint);
  if (!publish->cancelled && publish->update) {
    GhRelayPublishResult result = { .url = endpoint->url, .outcome = outcome,
                                    .prefix = prefix, .message = message };
    publish->update(publish, &result, publish->user_data);
  }
  if (!publish->cancelled && !publish->completed &&
      publish->terminal == publish->endpoints->len) {
    publish->completed = TRUE;
    publish->summary.total = publish->endpoints->len;
    publish->summary.any_accepted = publish->summary.accepted > 0;
    publish->summary.all_failed = publish->summary.accepted == 0;
    if (publish->done)
      publish->done(publish, &publish->summary, publish->user_data);
  }
  gh_relay_publish_unref(publish);
}

static gboolean
auth_enabled(const GhRelayPublish *publish, const GhPublishEndpoint *endpoint)
{
  if (!publish->auth_transport.send_auth || !publish->auth_transport.resend)
    return FALSE;
  switch (endpoint->auth_mode) {
  case GH_RELAY_AUTH_EPHEMERAL: return TRUE;
  case GH_RELAY_AUTH_ACCOUNT: return publish->signer != NULL;
  case GH_RELAY_AUTH_NONE: return FALSE;
  }
  return FALSE;
}

/* The authenticated re-send will not happen: AUTH_REQUIRED, with the
 * relay's auth-required message and the local reason. */
static void
fail_auth(GhRelayPublish *publish, GhPublishEndpoint *endpoint, const gchar *why)
{
  const gchar *relay_message = endpoint->auth_message ? endpoint->auth_message
                                                      : "auth-required:";
  g_autofree gchar *message = why ? g_strdup_printf("%s (AUTH not completed: %s)",
                                                    relay_message, why)
                                  : g_strdup(relay_message);
  finish_endpoint(publish, endpoint, GH_RELAY_PUBLISH_AUTH_REQUIRED,
                  GH_RELAY_OK_PREFIX_AUTH_REQUIRED, message);
}

static gboolean
deadline_expired(gpointer data)
{
  GhPublishEndpoint *endpoint = data;
  GSource *source = g_steal_pointer(&endpoint->deadline);
  if (endpoint->auth_needed) {
    g_autofree gchar *why = g_strdup_printf("no AUTH completed within %u s",
                                            endpoint->publish->deadline_seconds);
    fail_auth(endpoint->publish, endpoint, why);
  } else {
    g_autofree gchar *detail = g_strdup_printf(
      "no relay OK within %u s (failure bound; relay state unknown)",
      endpoint->publish->deadline_seconds);
    finish_endpoint(endpoint->publish, endpoint,
                    GH_RELAY_PUBLISH_CONNECTION_FAILED, GH_RELAY_OK_PREFIX_NONE,
                    detail);
  }
  g_source_unref(source);
  return G_SOURCE_REMOVE;
}

/* (Re)starts url's failure deadline: deadline_seconds from now. */
static void
arm_deadline(GhRelayPublish *publish, GhPublishEndpoint *endpoint)
{
  clear_deadline(endpoint);
  endpoint->deadline = g_timeout_source_new_seconds(publish->deadline_seconds);
  g_source_set_callback(endpoint->deadline, deadline_expired, endpoint, NULL);
  g_source_attach(endpoint->deadline, publish->context);
}

static void
on_auth_signed(gpointer owner, const gchar *signed_json, const gchar *event_id,
               const GError *error)
{
  GhPublishEndpoint *endpoint = owner;
  GhRelayPublish *publish = endpoint->publish;
  endpoint->attempt = NULL; /* the attempt released itself */
  if (publish->cancelled || endpoint->outcome != GH_RELAY_PUBLISH_PENDING ||
      !endpoint->opened)
    return;
  if (error) {
    fail_auth(publish, endpoint, error->message);
    return;
  }
  g_autoptr(GError) send_error = NULL;
  g_strlcpy(endpoint->auth_event_id, event_id, sizeof endpoint->auth_event_id);
  if (!publish->auth_transport.send_auth(endpoint->handle, signed_json,
                                         publish->transport_data, &send_error)) {
    fail_auth(publish, endpoint, send_error ? send_error->message : "AUTH not sent");
    return;
  }
  /* The relay's turn again: its OK for the AUTH, then for the re-sent EVENT. */
  arm_deadline(publish, endpoint);
}

/* One AUTH per challenge, only after the relay refused the EVENT. */
static void
maybe_auth(GhRelayPublish *publish, GhPublishEndpoint *endpoint)
{
  if (!auth_enabled(publish, endpoint) || !endpoint->auth_needed ||
      !endpoint->challenge || endpoint->attempt || endpoint->auth_event_id[0])
    return;
  if (g_strcmp0(endpoint->auth_challenge, endpoint->challenge) == 0) {
    fail_auth(publish, endpoint, "AUTH already attempted for this challenge");
    return;
  }
  g_free(endpoint->auth_challenge);
  endpoint->auth_challenge = g_strdup(endpoint->challenge);
  g_autoptr(GError) error = NULL;
  endpoint->attempt = gh_relay_auth_attempt_start(endpoint->auth_mode, publish->signer,
                                                  publish->generation, endpoint->url,
                                                  endpoint->challenge, on_auth_signed,
                                                  endpoint, &error);
  if (!endpoint->attempt) {
    fail_auth(publish, endpoint, error ? error->message : "AUTH not started");
    return;
  }
  /* The deadline bounds the relay, not the signer: an account AUTH may wait
   * for the user in Nostr Signer (charter §4.4 R6), whose own call timeout
   * bounds it. on_auth_signed() starts the deadline again. */
  clear_deadline(endpoint);
}

static void
open_endpoint(GhRelayPublish *publish, GhPublishEndpoint *endpoint)
{
  g_autoptr(GError) error = NULL;
  gpointer handle = publish->transport.open(publish, endpoint->url,
                                            publish->event_json,
                                            publish->transport_data, &error);
  if (!handle) {
    finish_endpoint(publish, endpoint, GH_RELAY_PUBLISH_CONNECTION_FAILED,
                    GH_RELAY_OK_PREFIX_NONE,
                    error ? error->message : "relay open failed");
    return;
  }
  if (publish->cancelled || endpoint->outcome != GH_RELAY_PUBLISH_PENDING) {
    /* Cancelled, or terminal already while opening. */
    publish->transport.close(handle, publish->transport_data);
    return;
  }
  endpoint->handle = handle;
  endpoint->opened = TRUE;
  arm_deadline(publish, endpoint);
}

gboolean
gh_relay_publish_start(GhRelayPublish *publish, GError **error)
{
  g_return_val_if_fail(publish != NULL, FALSE);
  g_return_val_if_fail(on_owner_context(publish), FALSE);
  if (publish->cancelled) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                        "relay publish cancelled");
    return FALSE;
  }
  if (publish->started) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                        "relay publish already started");
    return FALSE;
  }
  if (publish->endpoints->len == 0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "relay publish has no relay URLs");
    return FALSE;
  }
  publish->started = TRUE;
  gh_relay_publish_ref(publish);
  for (guint i = 0; i < publish->endpoints->len && !publish->cancelled; i++)
    open_endpoint(publish, g_ptr_array_index(publish->endpoints, i));
  gh_relay_publish_unref(publish);
  return TRUE;
}

static GhPublishEndpoint *
pending_endpoint(GhRelayPublish *publish, const gchar *url)
{
  if (!publish || publish->cancelled || !publish->started || !url)
    return NULL;
  GhPublishEndpoint *endpoint = g_hash_table_lookup(publish->by_url, url);
  return endpoint && endpoint->outcome == GH_RELAY_PUBLISH_PENDING ? endpoint
                                                                   : NULL;
}

void
gh_relay_publish_ok(GhRelayPublish *publish, const gchar *url,
                    const gchar *event_id, gboolean accepted,
                    const gchar *message)
{
  g_return_if_fail(publish != NULL);
  g_return_if_fail(on_owner_context(publish));
  GhPublishEndpoint *endpoint = pending_endpoint(publish, url);
  if (!endpoint)
    return;
  if (endpoint->auth_event_id[0] &&
      g_strcmp0(event_id, endpoint->auth_event_id) == 0) {
    endpoint->auth_event_id[0] = '\0';
    if (!accepted) {
      fail_auth(publish, endpoint, message ? message : "relay refused AUTH");
      return;
    }
    endpoint->auth_needed = FALSE;
    endpoint->resent = TRUE;
    g_autoptr(GError) error = NULL;
    if (!publish->auth_transport.resend(endpoint->handle, publish->transport_data,
                                        &error))
      finish_endpoint(publish, endpoint, GH_RELAY_PUBLISH_CONNECTION_FAILED,
                      GH_RELAY_OK_PREFIX_NONE,
                      error ? error->message : "EVENT re-send failed");
    return;
  }
  if (g_strcmp0(event_id, publish->event_id) != 0)
    return;
  GhRelayOkPrefix prefix = gh_relay_ok_prefix_classify(message);
  if (!accepted && prefix == GH_RELAY_OK_PREFIX_AUTH_REQUIRED &&
      auth_enabled(publish, endpoint) && !endpoint->resent) {
    /* Not terminal yet: authenticate and re-send once. */
    if (!endpoint->auth_needed) {
      endpoint->auth_needed = TRUE;
      g_free(endpoint->auth_message);
      endpoint->auth_message = g_strdup(message);
      maybe_auth(publish, endpoint);
    }
    return;
  }
  GhRelayPublishOutcome outcome = GH_RELAY_PUBLISH_ACCEPTED;
  if (!accepted)
    outcome = prefix == GH_RELAY_OK_PREFIX_AUTH_REQUIRED
                ? GH_RELAY_PUBLISH_AUTH_REQUIRED
                : GH_RELAY_PUBLISH_REJECTED;
  finish_endpoint(publish, endpoint, outcome, prefix, message);
}

void
gh_relay_publish_failed(GhRelayPublish *publish, const gchar *url,
                        const gchar *detail)
{
  g_return_if_fail(publish != NULL);
  g_return_if_fail(on_owner_context(publish));
  GhPublishEndpoint *endpoint = pending_endpoint(publish, url);
  if (endpoint)
    finish_endpoint(publish, endpoint, GH_RELAY_PUBLISH_CONNECTION_FAILED,
                    GH_RELAY_OK_PREFIX_NONE,
                    detail ? detail : "relay connection failed");
}

void
gh_relay_publish_auth_challenge(GhRelayPublish *publish, const gchar *url,
                                const gchar *challenge)
{
  g_return_if_fail(publish != NULL);
  g_return_if_fail(on_owner_context(publish));
  GhPublishEndpoint *endpoint = pending_endpoint(publish, url);
  if (!endpoint || !endpoint->opened || !challenge || !*challenge)
    return;
  g_free(endpoint->challenge);
  endpoint->challenge = g_strdup(challenge);
  maybe_auth(publish, endpoint);
}
