/* The whole NIP-17 loop over real sockets (charter G13): two Groundhog
 * accounts in one process (each a full stack, tests/ui/send-stack.h, with
 * its own encrypted store over a FakeSecret, its own GSettings and window),
 * one mock signer bus answering for both keys, and three local storing
 * NIP-01 relays on 127.0.0.1: a discovery relay holding both kind-10050
 * lists, and one DM inbox relay each. Everything relay-facing is the
 * application's own GNostrRelay transport (REQs, publishes, NIP-17 inbox),
 * except recipient 10050 lookups, which a directory answers from the
 * discovery relay's stored lists: a one-shot lookup over GNostrRelay can
 * miss a stored event behind EOSE today (nostrc-qp24.10.6), and the charter's
 * contact directory (G10) serves sends from a cache anyway.
 *
 *  1. A starts a conversation with B (the outbox call G18's New Message
 *     dialog will make); A's window lists it at once.
 *  2. B's inbox receives it from B's relay; B's view shows it; B accepts
 *     (G19's Accept) and replies with the composer.
 *  3. A's inbox receives the reply into the same room; A answers with the
 *     composer; B receives that.
 *  4. Both have exactly one room with the same three messages, A's and B's
 *     own ones "Sent".
 *  5. Both restart: the rooms, messages, reaction and "Sent" come back from
 *     the encrypted stores without duplicate chat messages.
 * Needs a display (77 without one); waits are bounded, nothing sleeps. */
#include "gh-test-signer.h"
#include "send-stack.h"

#include "gh-outbox.h"
#include "gh-nip17-envelope.h"
#include "gh-reaction.h"
#include "gh-store.h"
#include "nostr-envelope.h"

#include <libsoup/soup.h>
#include <sqlite3.h>
#include <string.h>

#include "nostrc-test-gdk-frame.h"

static GhTestBus bus;
static GhTestSigner signer;

/* ---- a storing NIP-01 relay ------------------------------------------------------- */

typedef struct {
  SoupWebsocketConnection *connection;
  gchar *id;
  NostrEnvelope *req; /* owns the filters */
} Sub;

typedef struct {
  SoupServer *server;
  gchar *url;
  GPtrArray *connections; /* SoupWebsocketConnection */
  GPtrArray *jsons;       /* stored signed events, in arrival order */
  GPtrArray *events;      /* NostrEvent, parallel to jsons */
  GHashTable *ids;
  GPtrArray *subs;        /* Sub */
  guint published;        /* EVENT frames stored */
} Relay;

static void
sub_free(gpointer data)
{
  Sub *sub = data;
  g_free(sub->id);
  nostr_envelope_free(sub->req);
  g_free(sub);
}

static gboolean
sub_matches(Sub *sub, NostrEvent *event)
{
  NostrFilters *filters = nostr_req_envelope_get_filters((NostrReqEnvelope *)sub->req);
  for (size_t i = 0; filters && i < filters->count; i++)
    if (nostr_filter_matches(&filters->filters[i], event))
      return TRUE;
  return FALSE;
}

static void
send_event(Sub *sub, const gchar *json)
{
  g_autofree gchar *frame = g_strdup_printf("[\"EVENT\",\"%s\",%s]", sub->id, json);
  soup_websocket_connection_send_text(sub->connection, frame);
}

/* A signed event (verified), stored once, then pushed to open REQs. */
static gboolean
relay_store(Relay *relay, const gchar *json, gchar **id_out)
{
  NostrEvent *event = nostr_event_new();
  gchar id[65] = { 0 };
  if (nostr_event_deserialize_signed(event, json, NULL) != NOSTR_EVENT_VALIDATION_OK ||
      nostr_event_validate(event, id) != NOSTR_EVENT_VALIDATION_OK) {
    nostr_event_free(event);
    return FALSE;
  }
  if (id_out)
    *id_out = g_strdup(id);
  if (g_hash_table_contains(relay->ids, id)) {
    nostr_event_free(event);
    return TRUE;
  }
  g_hash_table_add(relay->ids, g_strdup(id));
  g_ptr_array_add(relay->jsons, g_strdup(json));
  g_ptr_array_add(relay->events, event);
  for (guint i = 0; i < relay->subs->len; i++) {
    Sub *sub = g_ptr_array_index(relay->subs, i);
    if (sub_matches(sub, event))
      send_event(sub, json);
  }
  return TRUE;
}

static void
drop_sub(Relay *relay, SoupWebsocketConnection *connection, const gchar *id)
{
  for (guint i = relay->subs->len; i > 0; i--) {
    Sub *sub = g_ptr_array_index(relay->subs, i - 1);
    if (sub->connection == connection && (!id || g_str_equal(sub->id, id)))
      g_ptr_array_remove_index(relay->subs, i - 1);
  }
}

static void
relay_on_message(SoupWebsocketConnection *connection, SoupWebsocketDataType type,
                 GBytes *message, gpointer data)
{
  Relay *relay = data;
  if (type != SOUP_WEBSOCKET_DATA_TEXT)
    return;
  gsize length;
  const gchar *bytes = g_bytes_get_data(message, &length);
  g_autofree gchar *text = g_strndup(bytes, length);
  static const gchar event_prefix[] = "[\"EVENT\",";
  if (g_str_has_prefix(text, event_prefix) && g_str_has_suffix(text, "]")) {
    g_autofree gchar *json = g_strndup(text + strlen(event_prefix),
                                       strlen(text) - strlen(event_prefix) - 1);
    g_autofree gchar *id = NULL;
    gboolean ok = relay_store(relay, json, &id);
    g_assert_true(ok); /* Groundhog publishes only signed, valid events */
    relay->published++;
    g_autofree gchar *reply = g_strdup_printf("[\"OK\",\"%s\",true,\"\"]", id);
    soup_websocket_connection_send_text(connection, reply);
    return;
  }
  NostrEnvelope *envelope = nostr_envelope_parse(text);
  if (!envelope)
    return;
  if (nostr_envelope_get_type(envelope) == NOSTR_ENVELOPE_REQ) {
    Sub *sub = g_new0(Sub, 1);
    sub->connection = connection;
    sub->id = g_strdup(nostr_req_envelope_get_subscription_id((NostrReqEnvelope *)envelope));
    sub->req = envelope;
    drop_sub(relay, connection, sub->id);
    for (guint i = 0; i < relay->events->len; i++)
      if (sub_matches(sub, g_ptr_array_index(relay->events, i)))
        send_event(sub, g_ptr_array_index(relay->jsons, i));
    g_autofree gchar *eose = g_strdup_printf("[\"EOSE\",\"%s\"]", sub->id);
    soup_websocket_connection_send_text(connection, eose);
    g_ptr_array_add(relay->subs, sub);
    return;
  }
  if (nostr_envelope_get_type(envelope) == NOSTR_ENVELOPE_CLOSE)
    drop_sub(relay, connection,
             nostr_close_envelope_get_message((NostrCloseEnvelope *)envelope));
  nostr_envelope_free(envelope);
}

static void
relay_on_closed(SoupWebsocketConnection *connection, gpointer data)
{
  drop_sub(data, connection, NULL);
}

static void
relay_on_websocket(SoupServer *server, SoupServerMessage *message, const char *path,
                   SoupWebsocketConnection *connection, gpointer data)
{
  Relay *relay = data;
  (void)server;
  (void)message;
  (void)path;
  g_ptr_array_add(relay->connections, g_object_ref(connection));
  g_signal_connect(connection, "message", G_CALLBACK(relay_on_message), relay);
  g_signal_connect(connection, "closed", G_CALLBACK(relay_on_closed), relay);
}

static void
relay_up(Relay *relay)
{
  relay->server = soup_server_new(NULL, NULL);
  relay->connections = g_ptr_array_new_with_free_func(g_object_unref);
  relay->jsons = g_ptr_array_new_with_free_func(g_free);
  relay->events = g_ptr_array_new_with_free_func((GDestroyNotify)nostr_event_free);
  relay->ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  relay->subs = g_ptr_array_new_with_free_func(sub_free);
  soup_server_add_websocket_handler(relay->server, "/relay", NULL, NULL, relay_on_websocket,
                                    relay, NULL);
  g_autoptr(GError) error = NULL;
  g_assert_true(soup_server_listen_local(relay->server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY,
                                         &error));
  g_assert_no_error(error);
  GSList *uris = soup_server_get_uris(relay->server);
  relay->url = g_strdup_printf("ws://127.0.0.1:%d/relay", g_uri_get_port(uris->data));
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
}

static void
relay_down(Relay *relay)
{
  g_ptr_array_set_size(relay->subs, 0);
  for (guint i = 0; i < relay->connections->len; i++) {
    SoupWebsocketConnection *connection = g_ptr_array_index(relay->connections, i);
    g_signal_handlers_disconnect_by_data(connection, relay);
    if (soup_websocket_connection_get_state(connection) == SOUP_WEBSOCKET_STATE_OPEN)
      soup_websocket_connection_close(connection, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
  }
  g_ptr_array_unref(relay->connections);
  g_ptr_array_unref(relay->subs);
  g_ptr_array_unref(relay->jsons);
  g_ptr_array_unref(relay->events);
  g_hash_table_unref(relay->ids);
  soup_server_disconnect(relay->server);
  g_object_unref(relay->server);
  g_free(relay->url);
}

static guint
wraps_for(Relay *relay, const gchar *pubkey)
{
  guint n = 0;
  for (guint i = 0; i < relay->events->len; i++) {
    NostrEvent *event = g_ptr_array_index(relay->events, i);
    if (nostr_event_get_kind(event) != 1059)
      continue;
    char *p = nostr_nip59_get_recipient(event);
    n += g_strcmp0(p, pubkey) == 0;
    free(p);
  }
  return n;
}

typedef struct {
  Relay *relay;
  const gchar *pubkey;
} WrapWait;

static gboolean
has_wrap_for(gpointer data)
{
  WrapWait *wait = data;
  return wraps_for(wait->relay, wait->pubkey) > 0;
}

/* SENT means a recipient's copy was accepted (the product contract); the
 * self-copy to the account's own inbox may still be in flight then
 * (nostrc-qp24.89), so wait for it rather than assert it at once. */
static void
wait_wrap_for(Relay *relay, const gchar *pubkey)
{
  WrapWait wait = { relay, pubkey };
  gh_test_spin_until(has_wrap_for, &wait);
}

/* ---- a directory over the discovery relay's stored kind-10050s (G10's role) ------ */

#define STORE_TYPE_DIRECTORY (store_directory_get_type())
G_DECLARE_FINAL_TYPE(StoreDirectory, store_directory, STORE, DIRECTORY, GObject)

struct _StoreDirectory {
  GObject parent_instance;
  Relay *relay;
};

static void
directory_resolve_async(GhInboxResolver *resolver, const gchar *pubkey,
                        GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  StoreDirectory *self = STORE_DIRECTORY(resolver);
  GTask *task = g_task_new(resolver, cancellable, callback, data);
  GhInboxResult *result = g_new0(GhInboxResult, 1);
  result->recipient = g_strdup(pubkey);
  result->status = GH_INBOX_NOT_FOUND;
  result->cached = TRUE;
  NostrEvent *newest = NULL;
  for (guint i = 0; i < self->relay->events->len; i++) {
    NostrEvent *event = g_ptr_array_index(self->relay->events, i);
    if (nostr_event_get_kind(event) == 10050 &&
        g_strcmp0(nostr_event_get_pubkey(event), pubkey) == 0 &&
        (!newest || nostr_event_get_created_at(event) > nostr_event_get_created_at(newest)))
      newest = event;
  }
  if (newest) {
    NostrTags *tags = nostr_event_get_tags(newest);
    g_autoptr(GPtrArray) urls = g_ptr_array_new_with_free_func(g_free);
    for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
      NostrTag *tag = nostr_tags_get(tags, i);
      if (g_strcmp0(nostr_tag_get_key(tag), "relay") == 0)
        g_ptr_array_add(urls, g_strdup(nostr_tag_get_value(tag)));
    }
    result->status = urls->len ? GH_INBOX_FOUND : GH_INBOX_EMPTY;
    g_ptr_array_add(urls, NULL);
    result->relays = (GStrv)g_ptr_array_free(g_steal_pointer(&urls), FALSE);
    char *id = nostr_event_get_id(newest);
    result->event_id = g_strdup(id);
    free(id);
    result->created_at = nostr_event_get_created_at(newest);
  }
  g_task_return_pointer(task, result, (GDestroyNotify)gh_inbox_result_free);
  g_object_unref(task);
}

static GhInboxResult *
directory_resolve_finish(GhInboxResolver *resolver, GAsyncResult *result, GError **error)
{
  (void)resolver;
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
store_directory_iface_init(GhInboxResolverInterface *iface)
{
  iface->resolve_async = directory_resolve_async;
  iface->resolve_finish = directory_resolve_finish;
}

G_DEFINE_FINAL_TYPE_WITH_CODE(StoreDirectory, store_directory, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GH_TYPE_INBOX_RESOLVER, store_directory_iface_init))

static void store_directory_class_init(StoreDirectoryClass *klass) { (void)klass; }
static void store_directory_init(StoreDirectory *self) { (void)self; }

/* ---- the loop ---------------------------------------------------------------------- */

static gboolean
inbox_live(gpointer data)
{
  SendStack *s = data;
  return gh_account_relays_get_inbox_relays(s->relays) != NULL &&
         gh_dm_inbox_get_state(s->inbox) == GH_DM_INBOX_LIVE &&
         gh_account_store_get_outbox(s->store) != NULL;
}

typedef struct {
  GhConversationStore *model;
  const gchar *room;
  const gchar *content;
} MessageWait;

static gboolean
message_listed(gpointer data)
{
  MessageWait *wait = data;
  GhConversation *room = gh_conversation_store_lookup(wait->model, wait->room);
  if (!room)
    return FALSE;
  GListModel *model = G_LIST_MODEL(room);
  for (guint i = 0; i < g_list_model_get_n_items(model); i++) {
    g_autoptr(GhMessage) message = g_list_model_get_item(model, i);
    if (g_strcmp0(gh_message_get_content(message), wait->content) == 0)
      return TRUE;
  }
  return FALSE;
}

static void
wait_message(SendStack *s, const gchar *room, const gchar *content)
{
  MessageWait wait = { s->model, room, content };
  gh_test_spin_until(message_listed, &wait);
}

typedef struct {
  GhMessage *message;
  GhMessageStatus status;
} StatusWait;

static gboolean
status_is(gpointer data)
{
  StatusWait *wait = data;
  return gh_message_get_status(wait->message) == wait->status;
}

static void
wait_sent(GhConversation *room, const gchar *content)
{
  StatusWait wait = { stack_find(room, content), GH_MESSAGE_STATUS_SENT };
  gh_test_spin_until(status_is, &wait);
}

/* The view shows the message: it is in the shown timeline. */
static gboolean
view_shows(GhConversationView *view, const gchar *content)
{
  GListModel *timeline = gh_conversation_view_get_timeline(view);
  for (guint i = 0; i < g_list_model_get_n_items(timeline); i++) {
    g_autoptr(GhTimelineItem) item = g_list_model_get_item(timeline, i);
    if (g_strcmp0(gh_message_get_content(gh_timeline_item_get_message(item)), content) == 0)
      return TRUE;
  }
  return FALSE;
}

typedef struct {
  GhConversationView *view;
  const gchar *target_id;
  const gchar *emoji;
} ReactionWait;

/* Check the summary bound to the peer's open timeline, not only the store. */
static gboolean
reaction_chip_shows(gpointer data)
{
  ReactionWait *wait = data;
  GListModel *timeline = gh_conversation_view_get_timeline(wait->view);
  for (guint i = 0; i < g_list_model_get_n_items(timeline); i++) {
    g_autoptr(GhTimelineItem) item = g_list_model_get_item(timeline, i);
    GhMessage *message = gh_timeline_item_get_message(item);
    if (!message || g_strcmp0(gh_message_get_rumor_id(message), wait->target_id) != 0)
      continue;
    GhReactionSummary *summary = gh_timeline_item_get_reaction_summary(item);
    const GPtrArray *chips = summary ? gh_reaction_summary_get_chips(summary) : NULL;
    for (guint j = 0; chips && j < chips->len; j++) {
      const GhReactionChip *chip = g_ptr_array_index(chips, j);
      if (g_strcmp0(chip->emoji, wait->emoji) == 0 && chip->count == 1)
        return TRUE;
    }
  }
  return FALSE;
}

typedef struct {
  GhDmInbox *inbox;
  guint admitted_before;
} ReactionInboxWait;

static gboolean
reaction_inbox_admitted(gpointer data)
{
  ReactionInboxWait *wait = data;
  GhDmInboxCounters counters;
  gh_dm_inbox_get_counters(wait->inbox, &counters);
  return counters.admitted > wait->admitted_before;
}

/* A chip received only in memory disappears at restart. Verify that the
 * reaction was linked to the same encrypted conversation as its target. */
static gboolean
reaction_stored_in_room(SendStack *s, const gchar *reaction_id, const gchar *room_id)
{
  sqlite3 *db = gh_store_get_db(gh_account_store_get_store(s->store));
  sqlite3_stmt *stmt = NULL;
  g_assert_cmpint(sqlite3_prepare_v2(db,
    "SELECT 1 FROM reactions r JOIN conversations c ON c.id = r.conversation_id "
    "WHERE r.reaction_msg_id = ? AND r.room_id = ? AND c.backend_key = ?",
    -1, &stmt, NULL), ==, SQLITE_OK);
  sqlite3_bind_text(stmt, 1, reaction_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, room_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, room_id, -1, SQLITE_TRANSIENT);
  gboolean found = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return found;
}

static gboolean
reaction_stored_anywhere(SendStack *s, const gchar *reaction_id)
{
  sqlite3 *db = gh_store_get_db(gh_account_store_get_store(s->store));
  sqlite3_stmt *stmt = NULL;
  g_assert_cmpint(sqlite3_prepare_v2(db,
    "SELECT 1 FROM reactions WHERE reaction_msg_id = ?", -1, &stmt, NULL), ==, SQLITE_OK);
  sqlite3_bind_text(stmt, 1, reaction_id, -1, SQLITE_TRANSIENT);
  gboolean found = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return found;
}

static gboolean
reaction_pending_in_room(SendStack *s, const gchar *reaction_id, const gchar *room_id)
{
  sqlite3 *db = gh_store_get_db(gh_account_store_get_store(s->store));
  sqlite3_stmt *stmt = NULL;
  g_assert_cmpint(sqlite3_prepare_v2(db,
    "SELECT 1 FROM pending_reactions WHERE reaction_msg_id = ? AND room_id = ?",
    -1, &stmt, NULL), ==, SQLITE_OK);
  sqlite3_bind_text(stmt, 1, reaction_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, room_id, -1, SQLITE_TRANSIENT);
  gboolean found = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return found;
}

static void
compose(SendStack *s, const gchar *text)
{
  GhComposer *composer = send_stack_composer(s);
  g_assert_null(gh_composer_get_disabled_reason(composer));
  stack_type(composer, text);
  g_assert_true(stack_press(composer, GDK_KEY_Return, 0));
}

/* The rumor ids of a room, sorted. */
static GStrv
room_ids(GhConversation *room)
{
  GListModel *model = G_LIST_MODEL(room);
  g_autoptr(GPtrArray) ids = g_ptr_array_new();
  for (guint i = 0; i < g_list_model_get_n_items(model); i++) {
    g_autoptr(GhMessage) message = g_list_model_get_item(model, i);
    g_ptr_array_add(ids, g_strdup(gh_message_get_rumor_id(message)));
  }
  g_ptr_array_sort_values(ids, (GCompareFunc)strcmp);
  g_ptr_array_add(ids, NULL);
  return (GStrv)g_ptr_array_free(g_steal_pointer(&ids), FALSE);
}

static void
stack_bring_up(SendStack *s)
{
  send_stack_up(s);
  g_assert_cmpint(gh_account_store_get_state(s->store), ==, GH_ACCOUNT_STORE_OPEN);
  send_stack_window(s, 960, 680);
  /* Opening three real account stacks and waiting for each relay's EOSE can
   * be delayed by the parallel gate. LIVE is the contract, not 10 seconds of
   * host wall time. Keep a bounded, slowdown-aware diagnostic wait. */
  if (!gh_test_wait_until_for_at(inbox_live, s, 30)) {
    const gchar *const *urls = gh_dm_inbox_get_relays(s->inbox);
    const gchar *detail = NULL;
    GhDmInboxRelayState relay_state = gh_dm_inbox_get_relay_state(
      s->inbox, urls && urls[0] ? urls[0] : NULL, &detail);
    g_error("inbox did not become live: state=%d relay=%d detail=%s "
            "account_relays=%p outbox=%p",
            gh_dm_inbox_get_state(s->inbox), relay_state, detail ? detail : "(none)",
            (void *)gh_account_relays_get_inbox_relays(s->relays),
            (void *)gh_account_store_get_outbox(s->store));
  }
}

static void
test_two_accounts(void)
{
  Relay discovery = { 0 }, inbox_a = { 0 }, inbox_b = { 0 }, inbox_c = { 0 };
  relay_up(&discovery);
  relay_up(&inbox_a);
  relay_up(&inbox_b);
  relay_up(&inbox_c);
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  const gchar *urls_a[] = { inbox_a.url, NULL };
  const gchar *urls_b[] = { inbox_b.url, NULL };
  const gchar *urls_c[] = { inbox_c.url, NULL };
  g_autofree gchar *list_a = stack_inbox_list(1, now - 3600, urls_a);
  g_autofree gchar *list_b = stack_inbox_list(2, now - 3600, urls_b);
  g_autofree gchar *list_c = stack_inbox_list(3, now - 3600, urls_c);
  g_assert_true(relay_store(&discovery, list_a, NULL));
  g_assert_true(relay_store(&discovery, list_b, NULL));
  g_assert_true(relay_store(&discovery, list_c, NULL));
  /* One each: an account's GhAppOutbox disposes the resolver it was given. */
  StoreDirectory *directory_a = g_object_new(STORE_TYPE_DIRECTORY, NULL);
  StoreDirectory *directory_b = g_object_new(STORE_TYPE_DIRECTORY, NULL);
  StoreDirectory *directory_c = g_object_new(STORE_TYPE_DIRECTORY, NULL);
  directory_a->relay = directory_b->relay = directory_c->relay = &discovery;

  const gchar *sources[] = { discovery.url, NULL };
  SendStack a = { 0 }, b = { 0 }, c = { 0 };
  send_stack_init(&a, 1, bus.client, sources);
  send_stack_init(&b, 2, bus.client, sources);
  send_stack_init(&c, 3, bus.client, sources);
  a.resolver = GH_INBOX_RESOLVER(directory_a);
  b.resolver = GH_INBOX_RESOLVER(directory_b);
  c.resolver = GH_INBOX_RESOLVER(directory_c);
  stack_bring_up(&a);
  stack_bring_up(&b);
  stack_bring_up(&c);
  gh_conversation_view_set_reaction_store(send_stack_view(&a),
                                          gh_app_outbox_get_reactions(a.sender));
  gh_conversation_view_set_reaction_store(send_stack_view(&b),
                                          gh_app_outbox_get_reactions(b.sender));
  const guint members[] = { 1, 2, 0 };
  g_autofree gchar *room = stack_room(members);

  /* 1. A starts the conversation (G18's New Message will make this call). */
  GhOutbox *outbox_a = GH_OUTBOX(gh_account_store_get_outbox(a.store));
  g_autoptr(GError) error = NULL;
  g_autoptr(GhOutboxItem) first = gh_outbox_send(outbox_a, stack_hex[2], "Hello B, it's A",
                                                 &error);
  g_assert_no_error(error);
  g_assert_nonnull(first);
  GhConversation *room_a = gh_conversation_store_lookup(a.model, room);
  g_assert_nonnull(room_a); /* shown at once, before any signer or relay */
  wait_sent(room_a, "Hello B, it's A");
  g_assert_cmpuint(wraps_for(&inbox_b, stack_hex[2]), ==, 1);
  wait_wrap_for(&inbox_a, stack_hex[1]);
  g_assert_cmpuint(wraps_for(&inbox_a, stack_hex[1]), ==, 1); /* A's self-copy */
  g_assert_cmpuint(wraps_for(&inbox_a, stack_hex[2]) + wraps_for(&inbox_b, stack_hex[1]), ==, 0);

  /* 2. B receives it, sees it, accepts (G19's Accept) and replies. */
  wait_message(&b, room, "Hello B, it's A");
  GhConversation *room_b = gh_conversation_store_lookup(b.model, room);
  g_assert_true(gh_conversation_get_is_request(room_b));
  gh_conversation_accept(room_b);
  send_stack_select(&b, room_b);
  g_assert_true(view_shows(send_stack_view(&b), "Hello B, it's A"));
  compose(&b, "Hi A! Got it.");
  g_assert_true(view_shows(send_stack_view(&b), "Hi A! Got it.")); /* at once */
  wait_sent(room_b, "Hi A! Got it.");

  /* 3. A gets the reply in the same room and answers with the composer. */
  wait_message(&a, room, "Hi A! Got it.");
  g_assert_true(gh_conversation_store_lookup(a.model, room) == room_a);
  send_stack_select(&a, room_a);
  g_assert_true(view_shows(send_stack_view(&a), "Hi A! Got it."));
  compose(&a, "Great, see you Saturday.");
  wait_sent(room_a, "Great, see you Saturday.");
  wait_message(&b, room, "Great, see you Saturday.");
  g_assert_true(view_shows(send_stack_view(&b), "Great, see you Saturday."));

  /* B reacts to an incoming DM. The peer's already-open bubble must gain a
   * chip, and the reaction must survive through the encrypted store. */
  GhMessage *target = stack_find(room_b, "Hello B, it's A");
  g_autofree gchar *target_id = g_strdup(gh_message_get_rumor_id(target));
  GhOutbox *outbox_b = GH_OUTBOX(gh_account_store_get_outbox(b.store));
  g_autofree gchar *reaction_id = gh_outbox_send_reaction_room(
    outbox_b, gh_conversation_get_peers(room_b), "+", target_id, "14", &error);
  g_assert_no_error(error);
  g_assert_nonnull(reaction_id);
  ReactionWait reaction = { send_stack_view(&a), target_id, "+" };
  gh_test_spin_until(reaction_chip_shows, &reaction);
  g_assert_true(reaction_chip_shows(&reaction));
  g_assert_true(reaction_stored_in_room(&a, reaction_id, room));

  /* The room already exists, but A receives B's reaction before the target
   * message's gift wrap. The held reaction must not leak onto another bubble,
   * then must appear when the target is delivered over the local relay. */
  const guint recipients_a[] = { 1, 0 };
  const gchar *to_a_early[] = { stack_hex[1], NULL };
  g_autofree gchar *late_id = NULL;
  g_autofree gchar *late_rumor = gh_nip17_rumor_new_room(
    stack_hex[2], to_a_early, "Delayed hello", now, 0, &late_id, &error);
  g_assert_no_error(error);
  g_assert_nonnull(late_rumor);
  g_autofree gchar *late_wrap_a = stack_craft_wrap(2, 1, recipients_a, now, "Delayed hello");
  g_autofree gchar *late_wrap_b = stack_craft_wrap(2, 2, recipients_a, now, "Delayed hello");
  GhDmInboxCounters before_early;
  gh_dm_inbox_get_counters(a.inbox, &before_early);
  g_autofree gchar *early_id = gh_outbox_send_reaction_room(
    outbox_b, to_a_early, "o", late_id, "14", &error);
  g_assert_no_error(error);
  g_assert_nonnull(early_id);
  ReactionInboxWait early = { a.inbox, before_early.admitted };
  gh_test_spin_until(reaction_inbox_admitted, &early);
  g_assert_true(reaction_pending_in_room(&a, early_id, room));
  g_assert_false(reaction_stored_anywhere(&a, early_id));
  g_assert_true(relay_store(&inbox_a, late_wrap_a, NULL));
  g_assert_true(relay_store(&inbox_b, late_wrap_b, NULL));
  wait_message(&a, room, "Delayed hello");
  wait_message(&b, room, "Delayed hello");
  g_assert_cmpstr(gh_message_get_rumor_id(stack_find(room_a, "Delayed hello")), ==, late_id);
  g_assert_false(reaction_pending_in_room(&a, early_id, room));
  ReactionWait late_chip = { send_stack_view(&a), late_id, "o" };
  gh_test_spin_until(reaction_chip_shows, &late_chip);
  g_assert_true(reaction_stored_in_room(&a, early_id, room));
  g_assert_false(reaction_pending_in_room(&a, early_id, room));

  /* C knows A's message id but is not in A-B's room. Its correctly addressed
   * C-A rumor must not project onto A-B's open bubble. Wait for A to unwrap
   * it before asserting absence, rather than relying on a timing window. */
  GhDmInboxCounters before_c;
  gh_dm_inbox_get_counters(a.inbox, &before_c);
  GhOutbox *outbox_c = GH_OUTBOX(gh_account_store_get_outbox(c.store));
  const gchar *to_a[] = { stack_hex[1], NULL };
  g_autofree gchar *foreign_id = gh_outbox_send_reaction_room(
    outbox_c, to_a, "x", target_id, "14", &error);
  g_assert_no_error(error);
  g_assert_nonnull(foreign_id);
  ReactionInboxWait foreign = { a.inbox, before_c.admitted };
  gh_test_spin_until(reaction_inbox_admitted, &foreign);
  ReactionWait foreign_chip = { send_stack_view(&a), target_id, "x" };
  g_assert_false(reaction_chip_shows(&foreign_chip));
  const guint foreign_members[] = { 1, 3, 0 };
  g_autofree gchar *foreign_room = stack_room(foreign_members);
  g_assert_false(reaction_stored_in_room(&a, foreign_id, foreign_room));
  g_assert_false(reaction_stored_anywhere(&a, foreign_id));

  /* 4. One room each, the same messages; own ones "Sent". */
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(a.model)), ==, 1);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(b.model)), ==, 1);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(room_a)), ==, 4);
  g_auto(GStrv) ids_a = room_ids(room_a);
  g_auto(GStrv) ids_b = room_ids(room_b);
  g_assert_true(g_strv_equal((const gchar *const *)ids_a, (const gchar *const *)ids_b));
  g_assert_false(gh_conversation_get_is_request(room_a));

  /* 5. Both restart; everything comes back from the encrypted stores. */
  send_stack_down(&a);
  send_stack_down(&b);
  stack_bring_up(&a);
  stack_bring_up(&b);
  gh_conversation_view_set_reaction_store(send_stack_view(&a),
                                          gh_app_outbox_get_reactions(a.sender));
  gh_conversation_view_set_reaction_store(send_stack_view(&b),
                                          gh_app_outbox_get_reactions(b.sender));
  room_a = gh_conversation_store_lookup(a.model, room);
  room_b = gh_conversation_store_lookup(b.model, room);
  g_assert_nonnull(room_a);
  g_assert_nonnull(room_b);
  g_auto(GStrv) again_a = room_ids(room_a);
  g_auto(GStrv) again_b = room_ids(room_b);
  g_assert_true(g_strv_equal((const gchar *const *)again_a, (const gchar *const *)ids_a));
  g_assert_true(g_strv_equal((const gchar *const *)again_b, (const gchar *const *)ids_b));
  send_stack_select(&a, room_a);
  send_stack_select(&b, room_b);
  wait_sent(room_a, "Hello B, it's A");
  wait_sent(room_a, "Great, see you Saturday.");
  wait_sent(room_b, "Hi A! Got it.");
  g_assert_cmpint(gh_message_get_status(stack_find(room_a, "Hi A! Got it.")), ==,
                  GH_MESSAGE_STATUS_NONE);
  g_assert_true(view_shows(send_stack_view(&b), "Great, see you Saturday."));
  reaction.view = send_stack_view(&a);
  g_assert_true(reaction_chip_shows(&reaction));
  late_chip.view = send_stack_view(&a);
  g_assert_true(reaction_chip_shows(&late_chip));
  foreign_chip.view = send_stack_view(&a);
  g_assert_false(reaction_chip_shows(&foreign_chip));
  g_assert_false(reaction_stored_anywhere(&a, foreign_id));

  send_stack_clear(&a);
  send_stack_clear(&b);
  send_stack_clear(&c);
  GhTestSenders check = { &bus, &signer };
  gh_test_spin_until(gh_test_signer_senders_closed, &check);
  g_object_unref(directory_a);
  g_object_unref(directory_b);
  g_object_unref(directory_c);
  relay_down(&discovery);
  relay_down(&inbox_a);
  relay_down(&inbox_b);
  relay_down(&inbox_c);
  gh_test_run_until_idle();
}

int
main(int argc, char **argv)
{
  if (!nostrc_test_bus_available()) {
    g_printerr("groundhog-e2e-dm test skipped: dbus-daemon is not installed\n");
    return 77;
  }
  g_autofree gchar *xdg = g_dir_make_tmp("groundhog-e2e-xdg-XXXXXX", NULL);
  g_assert_nonnull(xdg);
  static const gchar *const vars[] = { "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME",
                                       "XDG_CONFIG_HOME" };
  for (guint i = 0; i < G_N_ELEMENTS(vars); i++) {
    g_autofree gchar *dir = g_build_filename(xdg, vars[i], NULL);
    g_assert_cmpint(g_mkdir(dir, 0700), ==, 0);
    g_setenv(vars[i], dir, TRUE);
  }
  if (!stack_gtk_and_bus_up(&bus)) {
    g_printerr("groundhog-e2e-dm test skipped: no graphical display\n");
    gh_test_remove_tree(xdg);
    return 77;
  }
  gh_test_signer_up(&bus, &signer);
  groundhog_register_resource();
  g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
  g_test_init(&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();
  stack_keys_init();
  nostrc_test_bus_add_func("/groundhog/e2e-dm/two-accounts", test_two_accounts);
  int status = g_test_run();
  stack_keys_clear();
  gh_test_signer_down(&bus, &signer);
  gh_test_bus_down(&bus);
  gh_test_remove_tree(xdg);
  return status;
}
