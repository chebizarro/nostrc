/* Real local NIP-01 relay for Groundhog relay wire tests: a libsoup
 * WebSocket server on 127.0.0.1 that answers every REQ with EOSE and hands
 * every EVENT frame to an optional hook. Waits iterate the default main
 * context; their deadlines are failure bounds only, never progress.
 *
 * With require_auth it is a NIP-42 relay: every connection gets its own
 * ["AUTH",<challenge>] as soon as it opens, and until that connection has
 * authenticated a REQ is answered with CLOSED "auth-required:" and an EVENT
 * with OK false "auth-required:". An ["AUTH",<event>] is accepted (OK true)
 * only if the event is a validly signed kind 22242 carrying that
 * connection's challenge and this relay's URL, and refuse_auth is unset.
 *
 * Store-and-serve mode (privacy charter §9.1 H2; the G24 privacy harness).
 * With serve set the relay is a minimal NIP-01 store instead of a sink: an
 * EVENT is validated, kept once per id and answered OK true itself (a
 * repeat gets "duplicate:"); on_event still runs after that OK. A REQ is
 * answered with every kept event matching one of its filters (newest
 * first, each filter's limit applied, and at most max_limit per filter when
 * set: a relay's own cap such as strfry's 500), then EOSE, and stays live: a later
 * matching event is sent to it until CLOSE or the socket closes. Two
 * NIP-42 gates, each of which also sends every connection a challenge as it
 * opens (as require_auth does, which keeps its meaning and precedence):
 *  - auth_gate_dms: a REQ asking for kind 1059 is refused with CLOSED
 *    "auth-required:" until the connection authenticated, and a kind-1059
 *    event (stored or live) is sent only to a connection that authenticated
 *    as its p-tagged recipient (NIP-17: gift wraps are served to their
 *    recipient only);
 *  - auth_writes: an EVENT is refused with OK false "auth-required:" until
 *    the connection authenticated, as any key.
 * wire_relay_inject() keeps an event as if a client had published it
 * (fixtures), and sends it to matching live REQs. With record set, every
 * text frame in either direction is kept in frames (WireFrame), tagged with
 * its connection's serial (1-based, in accept order), whatever the mode. */
#ifndef GH_TEST_WIRE_RELAY_H
#define GH_TEST_WIRE_RELAY_H

#include "../gh-test-port.h"

#include <gio/gio.h>
#include <libsoup/soup.h>
#include <nostr-envelope.h>
#include <nostr-event.h>
#include <nostr-filter.h>
#include <nostr-tag.h>
#include <stdlib.h>
#include <string.h>

typedef struct _WireRelay WireRelay;
typedef void (*WireEventFunc)(WireRelay *relay,
                              SoupWebsocketConnection *connection,
                              const gchar *event_id, gpointer data);

struct _WireRelay {
  SoupServer *server;
  GPtrArray *connections;
  gchar *url;
  guint reqs;
  guint events;
  guint closed_sockets;
  WireEventFunc on_event;
  gpointer on_event_data;
  /* NIP-42 and failure modes */
  gboolean require_auth;
  gboolean refuse_auth;      /* answer every AUTH with OK false */
  gboolean close_on_event;   /* drop the socket on EVENT, before any OK (serve too:
                              * nothing is kept) */
  gboolean close_on_connect; /* drop the socket right after the upgrade */
  guint challenges_sent;
  guint auth_frames;
  guint auth_ok;
  guint closed_reqs;         /* REQs refused with CLOSED auth-required */
  guint refused_events;      /* EVENTs refused with OK false auth-required */
  GPtrArray *auth_pubkeys;   /* pubkey of every AUTH frame, in order */
  /* Store-and-serve mode (see above) */
  gboolean serve;
  gboolean auth_gate_dms;    /* kind 1059 only to its authenticated recipient */
  gboolean auth_writes;      /* EVENT only from an authenticated connection */
  gboolean record;           /* keep every text frame in frames */
  GPtrArray *stored;         /* WireStored, in arrival order */
  GPtrArray *frames;         /* WireFrame, in order */
  guint served;              /* EVENT frames sent to subscriptions */
  GhTestHeldPort *held;      /* relay_init_held(): the port it serves on */
  GHashTable *withheld;      /* ids kept but served to nobody until released */
  gboolean withhold_new;     /* every event kept from now on is withheld */
  guint max_limit;           /* serve: a REQ's stored answer per filter, at most; 0: none */
};

/* One text frame on one of the relay's connections. */
typedef struct {
  guint connection;          /* its connection's serial, from 1 */
  gboolean inbound;          /* sent by the client; FALSE: by the relay */
  gchar *text;
} WireFrame;

/* One event the relay keeps (store-and-serve mode). */
typedef struct {
  gchar *json;
  gchar *id;
  NostrEvent *event;
} WireStored;

typedef struct {
  gboolean timed_out;
} WaitState;

/* Returns the id of the signed event in an ["EVENT",{...}] frame, or NULL. */
static G_GNUC_UNUSED gchar *
wire_event_frame_id(const gchar *text)
{
  static const gchar prefix[] = "[\"EVENT\",";
  gsize length = strlen(text);
  if (!g_str_has_prefix(text, prefix) || length < sizeof prefix ||
      text[length - 1] != ']')
    return NULL;
  g_autofree gchar *json = g_strndup(text + sizeof prefix - 1,
                                     length - (sizeof prefix - 1) - 1);
  NostrEvent *event = nostr_event_new();
  gchar id[65];
  gboolean valid = nostr_event_deserialize_signed(event, json, NULL) ==
                     NOSTR_EVENT_VALIDATION_OK &&
                   nostr_event_validate(event, id) == NOSTR_EVENT_VALIDATION_OK;
  nostr_event_free(event);
  return valid ? g_strdup(id) : NULL;
}

static G_GNUC_UNUSED gboolean
wire_authed(SoupWebsocketConnection *connection)
{
  return g_object_get_data(G_OBJECT(connection), "authed") != NULL;
}

/* The relay a connection belongs to while that relay lives (for recording
 * frames sent by helpers that only know the connection), else NULL. */
static G_GNUC_UNUSED WireRelay *
wire_relay_of(SoupWebsocketConnection *connection)
{
  return g_object_get_data(G_OBJECT(connection), "wire-relay");
}

static G_GNUC_UNUSED guint
wire_serial(SoupWebsocketConnection *connection)
{
  return GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(connection), "wire-serial"));
}

static G_GNUC_UNUSED void
wire_frame_free(gpointer data)
{
  WireFrame *frame = data;
  g_free(frame->text);
  g_free(frame);
}

static G_GNUC_UNUSED void
wire_stored_free(gpointer data)
{
  WireStored *stored = data;
  g_free(stored->json);
  g_free(stored->id);
  nostr_event_free(stored->event);
  g_free(stored);
}

static G_GNUC_UNUSED void
wire_record(WireRelay *relay, SoupWebsocketConnection *connection, gboolean inbound,
            const gchar *text)
{
  if (!relay || !relay->record)
    return;
  WireFrame *frame = g_new0(WireFrame, 1);
  frame->connection = wire_serial(connection);
  frame->inbound = inbound;
  frame->text = g_strdup(text);
  g_ptr_array_add(relay->frames, frame);
}

/* Every frame the relay sends goes through here, so it can be recorded. */
static G_GNUC_UNUSED void
wire_send(SoupWebsocketConnection *connection, const gchar *text)
{
  wire_record(wire_relay_of(connection), connection, FALSE, text);
  soup_websocket_connection_send_text(connection, text);
}

static G_GNUC_UNUSED gboolean
wire_tag_is(NostrTags *tags, const gchar *key, const gchar *value)
{
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (g_strcmp0(nostr_tag_get_key(tag), key) == 0)
      return g_strcmp0(nostr_tag_get_value(tag), value) == 0;
  }
  return FALSE;
}

/* ["AUTH",{...}]: the relay-side NIP-42 check. Fills id (65 bytes). */
static G_GNUC_UNUSED gboolean
wire_auth_valid(WireRelay *relay, SoupWebsocketConnection *connection,
                const gchar *text, gchar *id, gchar **pubkey)
{
  static const gchar prefix[] = "[\"AUTH\",";
  gsize length = strlen(text);
  if (length < sizeof prefix || text[length - 1] != ']')
    return FALSE;
  g_autofree gchar *json = g_strndup(text + sizeof prefix - 1,
                                     length - (sizeof prefix - 1) - 1);
  NostrEvent *event = nostr_event_new();
  const gchar *challenge = g_object_get_data(G_OBJECT(connection), "challenge");
  gboolean valid =
    nostr_event_deserialize_signed(event, json, NULL) == NOSTR_EVENT_VALIDATION_OK &&
    nostr_event_validate(event, id) == NOSTR_EVENT_VALIDATION_OK &&
    nostr_event_get_kind(event) == 22242 &&
    wire_tag_is(nostr_event_get_tags(event), "challenge", challenge) &&
    wire_tag_is(nostr_event_get_tags(event), "relay", relay->url);
  *pubkey = g_strdup(nostr_event_get_pubkey(event));
  nostr_event_free(event);
  return valid;
}

static G_GNUC_UNUSED void
wire_send_ok(SoupWebsocketConnection *connection, const gchar *id,
             gboolean accepted, const gchar *message)
{
  g_autofree gchar *frame = g_strdup_printf("[\"OK\",\"%s\",%s,\"%s\"]", id,
                                            accepted ? "true" : "false", message);
  wire_send(connection, frame);
}

/* ---- store-and-serve mode ------------------------------------------------------ */

/* The pubkeys a connection authenticated as (a set), created on demand. */
static G_GNUC_UNUSED GHashTable *
wire_keys(SoupWebsocketConnection *connection)
{
  GHashTable *keys = g_object_get_data(G_OBJECT(connection), "wire-keys");
  if (!keys) {
    keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_object_set_data_full(G_OBJECT(connection), "wire-keys", keys,
                           (GDestroyNotify)g_hash_table_unref);
  }
  return keys;
}

/* sub id -> NostrFilters of the connection's live REQs, created on demand. */
static G_GNUC_UNUSED GHashTable *
wire_subs(SoupWebsocketConnection *connection)
{
  GHashTable *subs = g_object_get_data(G_OBJECT(connection), "wire-subs");
  if (!subs) {
    subs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                 (GDestroyNotify)nostr_filters_free);
    g_object_set_data_full(G_OBJECT(connection), "wire-subs", subs,
                           (GDestroyNotify)g_hash_table_unref);
  }
  return subs;
}

/* NIP-17 serving rule: a kind-1059 event goes only to a connection that
 * authenticated as the pubkey of one of its p tags. */
static G_GNUC_UNUSED gboolean
wire_may_serve(WireRelay *relay, SoupWebsocketConnection *connection, WireStored *stored)
{
  if (relay->withheld && g_hash_table_contains(relay->withheld, stored->id))
    return FALSE;
  if (!relay->auth_gate_dms || nostr_event_get_kind(stored->event) != 1059)
    return TRUE;
  GHashTable *keys = wire_keys(connection);
  NostrTags *tags = nostr_event_get_tags(stored->event);
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (g_strcmp0(nostr_tag_get_key(tag), "p") == 0 &&
        g_hash_table_contains(keys, nostr_tag_get_value(tag)))
      return TRUE;
  }
  return FALSE;
}

static G_GNUC_UNUSED gboolean
wire_filters_want_dms(NostrFilters *filters)
{
  for (size_t i = 0; i < filters->count; i++)
    for (size_t k = 0; k < nostr_filter_kinds_len(&filters->filters[i]); k++)
      if (nostr_filter_kinds_get(&filters->filters[i], k) == 1059)
        return TRUE;
  return FALSE;
}

static G_GNUC_UNUSED void
wire_send_event(SoupWebsocketConnection *connection, const gchar *sub_id, WireStored *stored)
{
  WireRelay *relay = wire_relay_of(connection);
  g_autofree gchar *frame = g_strdup_printf("[\"EVENT\",\"%s\",%s]", sub_id, stored->json);
  if (relay)
    relay->served++;
  wire_send(connection, frame);
}

/* Newest first (created_at, then id), as NIP-01 asks of a REQ's answer. */
static G_GNUC_UNUSED gint
wire_newest_first(gconstpointer a, gconstpointer b)
{
  WireStored *x = *(WireStored *const *)a, *y = *(WireStored *const *)b;
  gint64 tx = nostr_event_get_created_at(x->event), ty = nostr_event_get_created_at(y->event);
  if (tx != ty)
    return tx > ty ? -1 : 1;
  return g_strcmp0(x->id, y->id);
}

static G_GNUC_UNUSED void
wire_answer_req(WireRelay *relay, SoupWebsocketConnection *connection, const gchar *sub_id,
                NostrFilters *filters)
{
  /* Borrowed pointers (g_ptr_array_copy() would take the free function). */
  g_autoptr(GPtrArray) order = g_ptr_array_sized_new(relay->stored->len);
  for (guint i = 0; i < relay->stored->len; i++)
    g_ptr_array_add(order, g_ptr_array_index(relay->stored, i));
  g_ptr_array_sort(order, wire_newest_first);
  g_autofree guint *sent = g_new0(guint, filters->count + 1);
  for (guint e = 0; e < order->len; e++) {
    WireStored *stored = g_ptr_array_index(order, e);
    if (!wire_may_serve(relay, connection, stored))
      continue;
    for (size_t i = 0; i < filters->count; i++) {
      NostrFilter *filter = &filters->filters[i];
      gint limit = nostr_filter_get_limit(filter);
      guint cap = limit > 0 ? (guint)limit : G_MAXUINT;
      if (relay->max_limit)
        cap = MIN(cap, relay->max_limit);   /* the relay's own cap, whatever was asked */
      if (nostr_filter_get_limit_zero(filter) || sent[i] >= cap ||
          !nostr_filter_matches(filter, stored->event))
        continue;
      sent[i]++;
      wire_send_event(connection, sub_id, stored);
      break;
    }
  }
  g_autofree gchar *eose = g_strdup_printf("[\"EOSE\",\"%s\"]", sub_id);
  wire_send(connection, eose);
}

/* A newly kept event goes to every live REQ that matches it. */
static G_GNUC_UNUSED void
wire_broadcast(WireRelay *relay, WireStored *stored)
{
  for (guint c = 0; c < relay->connections->len; c++) {
    SoupWebsocketConnection *connection = g_ptr_array_index(relay->connections, c);
    if (soup_websocket_connection_get_state(connection) != SOUP_WEBSOCKET_STATE_OPEN ||
        !wire_may_serve(relay, connection, stored))
      continue;
    GHashTableIter iter;
    gpointer sub_id, filters;
    g_hash_table_iter_init(&iter, wire_subs(connection));
    while (g_hash_table_iter_next(&iter, &sub_id, &filters))
      if (nostr_filters_match(filters, stored->event))
        wire_send_event(connection, sub_id, stored);
  }
}

/* Keeps a signed event once; NULL when its id is already kept. Asserts that
 * it is valid: Groundhog must never publish an unverifiable event. */
/* Serve-mode ordering control: the stored event @id is kept but sent to no
 * subscription (stored answers and live ones) until wire_relay_release(),
 * which then sends it to every matching live subscription, e.g. a later
 * epoch's message delivered before the Commit that opens it. */
static G_GNUC_UNUSED void
wire_relay_withhold(WireRelay *relay, const gchar *id)
{
  if (!relay->withheld)
    relay->withheld = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_hash_table_add(relay->withheld, g_strdup(id));
}

static G_GNUC_UNUSED WireStored *
wire_keep(WireRelay *relay, const gchar *json)
{
  NostrEvent *event = nostr_event_new();
  gchar id[65] = { 0 };
  g_assert_cmpint(nostr_event_deserialize_signed(event, json, NULL), ==,
                  NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpint(nostr_event_validate(event, id), ==, NOSTR_EVENT_VALIDATION_OK);
  for (guint i = 0; i < relay->stored->len; i++) {
    if (g_str_equal(((WireStored *)g_ptr_array_index(relay->stored, i))->id, id)) {
      nostr_event_free(event);
      return NULL;
    }
  }
  WireStored *stored = g_new0(WireStored, 1);
  stored->json = g_strdup(json);
  stored->id = g_strdup(id);
  stored->event = event;
  g_ptr_array_add(relay->stored, stored);
  if (relay->withhold_new)
    wire_relay_withhold(relay, id);
  return stored;
}

/* A fixture event, kept as if a client published it (not recorded as a
 * frame), and sent to matching live REQs. */
static G_GNUC_UNUSED void
wire_relay_release(WireRelay *relay, const gchar *id)
{
  if (!relay->withheld || !g_hash_table_remove(relay->withheld, id))
    return;
  for (guint i = 0; i < relay->stored->len; i++) {
    WireStored *stored = g_ptr_array_index(relay->stored, i);
    if (g_str_equal(stored->id, id))
      wire_broadcast(relay, stored);
  }
}

static G_GNUC_UNUSED void
wire_relay_inject(WireRelay *relay, const gchar *json)
{
  g_assert_true(relay->serve);
  WireStored *stored = wire_keep(relay, json);
  if (stored)
    wire_broadcast(relay, stored);
}

/* The JSON between a frame's prefix (e.g. ["EVENT",) and its final ]. */
static G_GNUC_UNUSED gchar *
wire_frame_payload(const gchar *text, const gchar *prefix)
{
  gsize length = strlen(text), skip = strlen(prefix);
  if (!g_str_has_prefix(text, prefix) || length <= skip || text[length - 1] != ']')
    return NULL;
  return g_strndup(text + skip, length - skip - 1);
}

/* Handles REQ, CLOSE and EVENT in store-and-serve mode; FALSE for anything
 * else (AUTH), which the common path handles. */
static G_GNUC_UNUSED gboolean
wire_serve_message(WireRelay *relay, SoupWebsocketConnection *connection, const gchar *text)
{
  if (g_str_has_prefix(text, "[\"REQ\"")) {
    relay->reqs++;
    NostrEnvelope *envelope = nostr_envelope_parse(text);
    g_assert_nonnull(envelope);
    g_assert_cmpint(nostr_envelope_get_type(envelope), ==, NOSTR_ENVELOPE_REQ);
    NostrReqEnvelope *req = (NostrReqEnvelope *)envelope;
    g_assert_nonnull(req->subscription_id);
    g_assert_nonnull(req->filters);
    const gchar *sub_id = req->subscription_id;
    if ((relay->require_auth || (relay->auth_gate_dms && wire_filters_want_dms(req->filters))) &&
        !wire_authed(connection)) {
      relay->closed_reqs++;
      g_hash_table_remove(wire_subs(connection), sub_id);
      g_autofree gchar *closed = g_strdup_printf(
        "[\"CLOSED\",\"%s\",\"auth-required: sign in to read this\"]", sub_id);
      wire_send(connection, closed);
    } else {
      NostrFilters *filters = g_steal_pointer(&req->filters);
      g_hash_table_replace(wire_subs(connection), g_strdup(sub_id), filters);
      wire_answer_req(relay, connection, sub_id, filters);
    }
    nostr_envelope_free(envelope);
    return TRUE;
  }
  if (g_str_has_prefix(text, "[\"CLOSE\"")) {
    NostrEnvelope *envelope = nostr_envelope_parse(text);
    if (envelope && nostr_envelope_get_type(envelope) == NOSTR_ENVELOPE_CLOSE)
      g_hash_table_remove(wire_subs(connection),
                          nostr_close_envelope_get_message((NostrCloseEnvelope *)envelope));
    if (envelope)
      nostr_envelope_free(envelope);
    return TRUE;
  }
  if (g_str_has_prefix(text, "[\"EVENT\"")) {
    g_autofree gchar *event_id = wire_event_frame_id(text);
    g_assert_nonnull(event_id);
    if (relay->close_on_event) {
      /* The answer is lost: nothing is kept or answered. */
      relay->events++;
      soup_websocket_connection_close(connection, SOUP_WEBSOCKET_CLOSE_GOING_AWAY, NULL);
      return TRUE;
    }
    if ((relay->require_auth || relay->auth_writes) && !wire_authed(connection)) {
      relay->refused_events++;
      wire_send_ok(connection, event_id, FALSE, "auth-required: sign in to publish");
      return TRUE;
    }
    relay->events++;
    g_autofree gchar *json = wire_frame_payload(text, "[\"EVENT\",");
    WireStored *stored = wire_keep(relay, json);
    wire_send_ok(connection, event_id, TRUE, stored ? "" : "duplicate: already have it");
    if (relay->on_event)
      relay->on_event(relay, connection, event_id, relay->on_event_data);
    if (stored)
      wire_broadcast(relay, stored);
    return TRUE;
  }
  return FALSE;
}

static G_GNUC_UNUSED void
wire_on_message(SoupWebsocketConnection *connection, SoupWebsocketDataType type,
                GBytes *message, gpointer data)
{
  WireRelay *relay = data;
  gsize length;
  const gchar *bytes = g_bytes_get_data(message, &length);
  if (type != SOUP_WEBSOCKET_DATA_TEXT)
    return;
  g_autofree gchar *text = g_strndup(bytes, length);
  wire_record(relay, connection, TRUE, text);
  if (relay->serve && wire_serve_message(relay, connection, text))
    return;
  if (g_str_has_prefix(text, "[\"REQ\"")) {
    relay->reqs++;
    const gchar *comma = strchr(text, ',');
    const gchar *start = comma ? strchr(comma, '"') : NULL;
    const gchar *end = start ? strchr(start + 1, '"') : NULL;
    g_assert_nonnull(end);
    gchar *sub_id = g_strndup(start + 1, end - start - 1);
    g_object_set_data_full(G_OBJECT(connection), "sub-id", sub_id, g_free);
    g_autofree gchar *reply = NULL;
    if (relay->require_auth && !wire_authed(connection)) {
      relay->closed_reqs++;
      reply = g_strdup_printf("[\"CLOSED\",\"%s\",\"auth-required: members only\"]",
                              sub_id);
    } else {
      reply = g_strdup_printf("[\"EOSE\",\"%s\"]", sub_id);
    }
    wire_send(connection, reply);
  } else if (g_str_has_prefix(text, "[\"EVENT\"")) {
    g_autofree gchar *event_id = wire_event_frame_id(text);
    g_assert_nonnull(event_id);
    if (relay->close_on_event) {
      relay->events++;
      soup_websocket_connection_close(connection, SOUP_WEBSOCKET_CLOSE_GOING_AWAY, NULL);
    } else if (relay->require_auth && !wire_authed(connection)) {
      relay->refused_events++;
      wire_send_ok(connection, event_id, FALSE, "auth-required: members only");
    } else {
      relay->events++;
      if (relay->on_event)
        relay->on_event(relay, connection, event_id, relay->on_event_data);
    }
  } else if (g_str_has_prefix(text, "[\"AUTH\"")) {
    relay->auth_frames++;
    gchar id[65] = {0};
    gchar *pubkey = NULL;
    gboolean valid = wire_auth_valid(relay, connection, text, id, &pubkey);
    g_assert_true(valid); /* Groundhog must never send an unverified AUTH */
    g_ptr_array_add(relay->auth_pubkeys, pubkey);
    if (relay->refuse_auth) {
      wire_send_ok(connection, id, FALSE, "restricted: not a member");
    } else {
      relay->auth_ok++;
      g_object_set_data(G_OBJECT(connection), "authed", GINT_TO_POINTER(1));
      g_hash_table_add(wire_keys(connection), g_strdup(pubkey));
      wire_send_ok(connection, id, TRUE, "");
    }
  }
}

static G_GNUC_UNUSED void
wire_on_socket_closed(SoupWebsocketConnection *connection, gpointer data)
{
  (void)connection;
  WireRelay *relay = data;
  relay->closed_sockets++;
}

static G_GNUC_UNUSED void
wire_on_websocket(SoupServer *server, SoupServerMessage *message,
                  const char *path, SoupWebsocketConnection *connection,
                  gpointer data)
{
  (void)server;
  (void)message;
  (void)path;
  WireRelay *relay = data;
  g_ptr_array_add(relay->connections, g_object_ref(connection));
  g_object_set_data(G_OBJECT(connection), "wire-relay", relay);
  g_object_set_data(G_OBJECT(connection), "wire-serial",
                    GUINT_TO_POINTER(relay->connections->len));
  g_signal_connect(connection, "message", G_CALLBACK(wire_on_message), relay);
  g_signal_connect(connection, "closed", G_CALLBACK(wire_on_socket_closed), relay);
  if (relay->close_on_connect) {
    soup_websocket_connection_close(connection, SOUP_WEBSOCKET_CLOSE_GOING_AWAY, NULL);
    return;
  }
  if (relay->require_auth || relay->auth_gate_dms || relay->auth_writes) {
    gchar *challenge = g_strdup_printf("challenge-%u-%p", ++relay->challenges_sent,
                                       (void *)connection);
    g_object_set_data_full(G_OBJECT(connection), "challenge", challenge, g_free);
    g_autofree gchar *frame = g_strdup_printf("[\"AUTH\",\"%s\"]", challenge);
    wire_send(connection, frame);
  }
}

static G_GNUC_UNUSED void
relay_setup(WireRelay *relay)
{
  relay->server = soup_server_new(NULL, NULL);
  relay->connections = g_ptr_array_new_with_free_func(g_object_unref);
  if (!relay->auth_pubkeys)
    relay->auth_pubkeys = g_ptr_array_new_with_free_func(g_free);
  relay->stored = g_ptr_array_new_with_free_func(wire_stored_free);
  relay->frames = g_ptr_array_new_with_free_func(wire_frame_free);
  soup_server_add_websocket_handler(relay->server, "/relay", NULL, NULL,
                                    wire_on_websocket, relay, NULL);
}

static G_GNUC_UNUSED void
relay_init(WireRelay *relay)
{
  relay_setup(relay);
  g_autoptr(GError) error = NULL;
  g_assert_true(soup_server_listen_local(relay->server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY,
                                         &error));
  g_assert_no_error(error);
  GSList *uris = soup_server_get_uris(relay->server);
  g_assert_nonnull(uris);
  g_free(relay->url);
  relay->url = g_strdup_printf("ws://127.0.0.1:%d/relay",
                                g_uri_get_port(uris->data));
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
}

static G_GNUC_UNUSED void
wire_serve_held(GSocketConnection *connection, gpointer data)
{
  WireRelay *relay = data;
  g_autoptr(GSocketAddress) local = g_socket_connection_get_local_address(connection, NULL);
  /* NULL when the client is gone already; libsoup then just loses it. */
  g_autoptr(GSocketAddress) remote = g_socket_connection_get_remote_address(connection, NULL);
  g_autoptr(GError) error = NULL;
  if (!soup_server_accept_iostream(relay->server, G_IO_STREAM(connection), local, remote,
                                   &error))
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
}

/* The relay that was down on held's port (held_relay_url()) comes up there,
 * same URL. relay_clear() releases the port. */
static G_GNUC_UNUSED void
relay_init_held(WireRelay *relay, GhTestHeldPort *held)
{
  relay_setup(relay);
  relay->held = held;
  g_free(relay->url);
  relay->url = g_strdup_printf("ws://127.0.0.1:%u/relay", held->port);
  gh_test_held_port_serve(held, wire_serve_held, relay);
}

static G_GNUC_UNUSED void
relay_clear(WireRelay *relay)
{
  if (relay->held) /* no new connection reaches the relay being freed */
    gh_test_held_port_clear(g_steal_pointer(&relay->held));
  for (guint i = 0; i < relay->connections->len; i++) {
    SoupWebsocketConnection *connection = g_ptr_array_index(relay->connections, i);
    /* The connection can outlive this (stack) relay during libsoup's close
     * handshake; its handlers must not write into a later test's frame. */
    g_signal_handlers_disconnect_by_data(connection, relay);
    g_object_set_data(G_OBJECT(connection), "wire-relay", NULL);
    if (soup_websocket_connection_get_state(connection) == SOUP_WEBSOCKET_STATE_OPEN)
      soup_websocket_connection_close(connection, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
  }
  g_ptr_array_unref(relay->connections);
  g_clear_pointer(&relay->auth_pubkeys, g_ptr_array_unref);
  g_clear_pointer(&relay->stored, g_ptr_array_unref);
  g_clear_pointer(&relay->frames, g_ptr_array_unref);
  g_clear_pointer(&relay->withheld, g_hash_table_unref);
  soup_server_disconnect(relay->server);
  g_object_unref(relay->server);
  g_free(relay->url);
}

/* A ws:// URL where nothing ever listens: every dial is refused. */
static G_GNUC_UNUSED gchar *
refused_relay_url(void)
{
  return g_strdup_printf("ws://127.0.0.1:%u/relay", gh_test_refused_port());
}

/* A ws:// URL on held's port: every dial fails until relay_init_held(). */
static G_GNUC_UNUSED gchar *
held_relay_url(GhTestHeldPort *held)
{
  gh_test_held_port_init(held);
  return g_strdup_printf("ws://127.0.0.1:%u/relay", held->port);
}

static G_GNUC_UNUSED gboolean
expire(gpointer data)
{
  WaitState *wait = data;
  wait->timed_out = TRUE;
  return G_SOURCE_REMOVE;
}

static G_GNUC_UNUSED void
wait_for_count(const guint *counter, guint count)
{
  WaitState wait = {0};
  guint timeout_source = g_timeout_add_seconds(18, expire, &wait);
  while (*counter < count && !wait.timed_out)
    g_main_context_iteration(NULL, TRUE);
  if (!wait.timed_out)
    g_source_remove(timeout_source);
  g_assert_cmpuint(*counter, ==, count);
}

static G_GNUC_UNUSED void
wait_for_reqs(WireRelay *relay, guint count)
{
  wait_for_count(&relay->reqs, count);
}

static G_GNUC_UNUSED void
wait_for_close(WireRelay *relay, guint count)
{
  WaitState wait = {0};
  guint timeout_source = g_timeout_add(3000, expire, &wait);
  while (relay->closed_sockets < count && !wait.timed_out)
    g_main_context_iteration(NULL, TRUE);
  if (!wait.timed_out)
    g_source_remove(timeout_source);
  g_assert_cmpuint(relay->closed_sockets, ==, count);
}

#endif
