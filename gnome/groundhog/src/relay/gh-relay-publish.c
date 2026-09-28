#include "gh-relay-publish.h"
#include "gh-relay-scope.h"

#include <gio/gio.h>
#include <nostr-event.h>

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
} GhPublishEndpoint;

struct _GhRelayPublish {
  gint refs;
  guint64 generation;
  gchar *event_json;
  gchar event_id[65];
  GhRelayPublishTransport transport;
  gpointer transport_data;
  GhRelayPublishUpdateFunc update;
  GhRelayPublishDoneFunc done;
  gpointer user_data;
  GMainContext *context;
  GPtrArray *endpoints;  /* GhPublishEndpoint, in add order */
  GHashTable *by_url;    /* url -> borrowed endpoint */
  guint deadline_seconds;
  guint terminal;
  GhRelayPublishSummary summary;
  gboolean started;
  gboolean cancelled;
  gboolean completed;
};

extern const GhRelayPublishTransport gh_relay_publish_gnostr_transport;

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
  return publish;
}

GhRelayPublish *
gh_relay_publish_new(guint64 generation, const gchar *event_json,
                     GhRelayPublishUpdateFunc update, GhRelayPublishDoneFunc done,
                     gpointer user_data, GError **error)
{
  return gh_relay_publish_new_with_transport(generation, event_json,
                                             &gh_relay_publish_gnostr_transport,
                                             NULL, update, done, user_data,
                                             error);
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
  g_ptr_array_unref(publish->endpoints);
  g_hash_table_unref(publish->by_url);
  g_main_context_unref(publish->context);
  g_free(publish->event_json);
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

void
gh_relay_publish_set_deadline(GhRelayPublish *publish, guint seconds)
{
  g_return_if_fail(publish != NULL);
  g_return_if_fail(!publish->started);
  publish->deadline_seconds = CLAMP(seconds, 1, GH_PUBLISH_MAX_DEADLINE_SECONDS);
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
deadline_expired(gpointer data)
{
  GhPublishEndpoint *endpoint = data;
  GSource *source = g_steal_pointer(&endpoint->deadline);
  g_autofree gchar *detail = g_strdup_printf(
    "no relay OK within %u s (failure bound; relay state unknown)",
    endpoint->publish->deadline_seconds);
  finish_endpoint(endpoint->publish, endpoint,
                  GH_RELAY_PUBLISH_CONNECTION_FAILED, GH_RELAY_OK_PREFIX_NONE,
                  detail);
  g_source_unref(source);
  return G_SOURCE_REMOVE;
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
  endpoint->deadline = g_timeout_source_new_seconds(publish->deadline_seconds);
  g_source_set_callback(endpoint->deadline, deadline_expired, endpoint, NULL);
  g_source_attach(endpoint->deadline, publish->context);
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
  if (!endpoint || g_strcmp0(event_id, publish->event_id) != 0)
    return;
  GhRelayOkPrefix prefix = gh_relay_ok_prefix_classify(message);
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
