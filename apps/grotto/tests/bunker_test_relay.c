/* Minimal NIP-01 fixture: signed EVENT/OK, filtered REQ/backfill/EOSE, CLOSE.
 * Unlike a URI-only loopback setting, listen_local never exposes the fixture
 * on the machine's external interfaces. All state is main-context owned. */
#include "bunker_test_relay.h"
#include <libsoup/soup.h>
#include "nostr-event.h"
#include "json.h"

typedef struct {
    BunkerTestRelay *relay;
    SoupWebsocketConnection *socket;
    GHashTable *subscriptions; /* sub id -> complete REQ array */
} RelayClient;

typedef struct { RelayClient *client; JsonArray *frame; } HeldAck;
static void held_ack_free(gpointer data) { HeldAck *ack = data; json_array_unref(ack->frame); g_free(ack); }

struct BunkerTestRelay {
    SoupServer *server;
    gchar *url;
    GPtrArray *clients;
    JsonArray *events, *deliveries;
    gint64 last_since;
    guint connections, requests;
    gboolean reject_subscriptions, reject_publishes, hold_acks;
    GPtrArray *held_acks;
    BunkerTestRelayEventCb event_cb; gpointer event_data;
    BunkerTestRelayRequestCb request_cb; gpointer request_data;
};

static void send_frame(RelayClient *client, JsonArray *frame)
{
    g_autoptr(JsonNode) node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, frame);
    g_autofree gchar *text = json_to_string(node, FALSE);
    if (soup_websocket_connection_get_state(client->socket) == SOUP_WEBSOCKET_STATE_OPEN)
        soup_websocket_connection_send_text(client->socket, text);
}

static gboolean list_contains(JsonArray *list, const gchar *value)
{
    for (guint i = 0; i < json_array_get_length(list); i++)
        if (g_strcmp0(json_array_get_string_element(list, i), value) == 0) return TRUE;
    return FALSE;
}

static gboolean matches(JsonArray *request, JsonObject *event)
{
    for (guint i = 2; i < json_array_get_length(request); i++) {
        JsonObject *filter = json_array_get_object_element(request, i);
        gboolean match = TRUE;
        if (json_object_has_member(filter, "kinds")) {
            JsonArray *kinds = json_object_get_array_member(filter, "kinds");
            match = FALSE;
            for (guint k = 0; k < json_array_get_length(kinds); k++)
                if (json_array_get_int_element(kinds, k) == json_object_get_int_member(event, "kind")) match = TRUE;
        }
        if (json_object_has_member(filter, "authors"))
            match &= list_contains(json_object_get_array_member(filter, "authors"),
                json_object_get_string_member(event, "pubkey"));
        if (json_object_has_member(filter, "#p")) {
            JsonArray *targets = json_object_get_array_member(filter, "#p");
            JsonArray *tags = json_object_get_array_member(event, "tags");
            gboolean target_match = FALSE;
            for (guint t = 0; t < json_array_get_length(tags); t++) {
                JsonArray *tag = json_array_get_array_element(tags, t);
                if (json_array_get_length(tag) >= 2 &&
                    g_strcmp0(json_array_get_string_element(tag, 0), "p") == 0 &&
                    list_contains(targets, json_array_get_string_element(tag, 1))) target_match = TRUE;
            }
            match &= target_match;
        }
        if (json_object_has_member(filter, "since"))
            match &= json_object_get_int_member(event, "created_at") >= json_object_get_int_member(filter, "since");
        if (json_object_has_member(filter, "until"))
            match &= json_object_get_int_member(event, "created_at") <= json_object_get_int_member(filter, "until");
        if (match) return TRUE;
    }
    return FALSE;
}

static void deliver(RelayClient *client, const gchar *id, JsonNode *event)
{
    if (soup_websocket_connection_get_state(client->socket) != SOUP_WEBSOCKET_STATE_OPEN) return;
    JsonArray *request = g_hash_table_lookup(client->subscriptions, id);
    /* Count each delivery to its intended recipient subscription, including
     * intentional retry/backfill deliveries, not just retained event IDs. */
    JsonArray *tags = json_object_get_array_member(json_node_get_object(event), "tags");
    gboolean addressed = FALSE;
    for (guint i = 2; request && i < json_array_get_length(request); i++) {
        JsonObject *filter = json_array_get_object_element(request, i);
        if (!json_object_has_member(filter, "#p")) continue;
        JsonArray *targets = json_object_get_array_member(filter, "#p");
        for (guint j = 0; tags && j < json_array_get_length(tags); j++) {
            JsonArray *tag = json_array_get_array_element(tags, j);
            if (json_array_get_length(tag) >= 2 && !g_strcmp0(json_array_get_string_element(tag, 0), "p") &&
                list_contains(targets, json_array_get_string_element(tag, 1))) addressed = TRUE;
        }
    }
    if (addressed) json_array_add_element(client->relay->deliveries, json_node_copy(event));
    JsonArray *frame = json_array_new();
    json_array_add_string_element(frame, "EVENT");
    json_array_add_string_element(frame, id);
    json_array_add_element(frame, json_node_copy(event));
    send_frame(client, frame);
}

static void message(SoupWebsocketConnection *socket, SoupWebsocketDataType type, GBytes *bytes, gpointer data)
{
    (void)socket;
    RelayClient *client = data;
    BunkerTestRelay *relay = client->relay;
    g_assert_cmpint(type, ==, SOUP_WEBSOCKET_DATA_TEXT);
    gsize size;
    const gchar *text = g_bytes_get_data(bytes, &size);
    g_autoptr(JsonParser) parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, text, size, NULL));
    JsonArray *frame = json_node_get_array(json_parser_get_root(parser));
    const gchar *verb = json_array_get_string_element(frame, 0);
    if (g_strcmp0(verb, "REQ") == 0) {
        const gchar *id = json_array_get_string_element(frame, 1);
        relay->requests++;
        JsonObject *filter = json_array_get_object_element(frame, 2);
        relay->last_since = json_object_has_member(filter, "since") ? json_object_get_int_member(filter, "since") : 0;
        g_autofree gchar *filters = json_to_string(json_parser_get_root(parser), FALSE);
        g_print("relay REQ %s\n", filters);
        if (relay->reject_subscriptions) {
            JsonArray *closed = json_array_new();
            json_array_add_string_element(closed, "CLOSED"); json_array_add_string_element(closed, id);
            json_array_add_string_element(closed, "auth-required: test fixture");
            send_frame(client, closed); return;
        }
        g_hash_table_replace(client->subscriptions, g_strdup(id), json_array_ref(frame));
        if (relay->request_cb) relay->request_cb(frame, relay->request_data);
        for (guint i = 0; i < json_array_get_length(relay->events); i++) {
            JsonNode *event = json_array_get_element(relay->events, i);
            if (matches(frame, json_node_get_object(event))) deliver(client, id, event);
        }
        JsonArray *eose = json_array_new();
        json_array_add_string_element(eose, "EOSE");
        json_array_add_string_element(eose, id);
        send_frame(client, eose);
    } else if (g_strcmp0(verb, "EVENT") == 0) {
        JsonNode *event = json_array_get_element(frame, 1);
        g_autofree gchar *serialized = json_to_string(event, FALSE);
        NostrEvent *parsed = nostr_event_new();
        gboolean valid = nostr_event_deserialize(parsed, serialized) == 0 && nostr_event_check_signature(parsed);
        nostr_event_free(parsed);
        JsonArray *ok = json_array_new();
        json_array_add_string_element(ok, "OK");
        json_array_add_string_element(ok, json_object_get_string_member(json_node_get_object(event), "id"));
        gboolean accepted = valid && !relay->reject_publishes;
        json_array_add_boolean_element(ok, accepted);
        json_array_add_string_element(ok, !valid ? "invalid: bad signature" : !accepted ? "blocked: test fixture" : "");
        if (relay->hold_acks) {
            HeldAck *ack = g_new0(HeldAck, 1); ack->client = client; ack->frame = ok;
            g_ptr_array_add(relay->held_acks, ack);
        } else send_frame(client, ok);
        if (!valid) return;
        if (relay->event_cb) relay->event_cb(serialized, relay->event_data);
        if (!accepted) return;
        json_array_add_element(relay->events, json_node_copy(event));
        for (guint i = 0; i < relay->clients->len; i++) {
            RelayClient *recipient = g_ptr_array_index(relay->clients, i);
            GHashTableIter iter; gpointer id, request;
            g_hash_table_iter_init(&iter, recipient->subscriptions);
            while (g_hash_table_iter_next(&iter, &id, &request))
                if (matches(request, json_node_get_object(event))) deliver(recipient, id, event);
        }
    } else if (g_strcmp0(verb, "CLOSE") == 0) {
        g_hash_table_remove(client->subscriptions, json_array_get_string_element(frame, 1));
    } else {
        g_error("unexpected relay frame: %s", verb);
    }
}

static void client_free(gpointer data)
{
    RelayClient *client = data;
    g_signal_handlers_disconnect_by_data(client->socket, client);
    if (soup_websocket_connection_get_state(client->socket) == SOUP_WEBSOCKET_STATE_OPEN)
        soup_websocket_connection_close(client->socket, SOUP_WEBSOCKET_CLOSE_GOING_AWAY, NULL);
    g_object_unref(client->socket);
    g_hash_table_unref(client->subscriptions);
    g_free(client);
}

static void connected(SoupServer *server, SoupServerMessage *request, const gchar *path,
                      SoupWebsocketConnection *socket, gpointer data)
{
    (void)server; (void)request; (void)path;
    BunkerTestRelay *relay = data;
    RelayClient *client = g_new0(RelayClient, 1);
    client->relay = relay;
    client->socket = g_object_ref(socket);
    client->subscriptions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_array_unref);
    g_ptr_array_add(relay->clients, client);
    relay->connections++;
    g_signal_connect(socket, "message", G_CALLBACK(message), client);
}

BunkerTestRelay *bunker_test_relay_new(void)
{
    BunkerTestRelay *relay = g_new0(BunkerTestRelay, 1);
    relay->clients = g_ptr_array_new_with_free_func(client_free);
    relay->held_acks = g_ptr_array_new_with_free_func(held_ack_free);
    relay->events = json_array_new();
    relay->deliveries = json_array_new();
    relay->server = soup_server_new(NULL, NULL);
    soup_server_add_websocket_handler(relay->server, "/", NULL, NULL, connected, relay, NULL);
    g_assert_true(soup_server_listen_local(relay->server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, NULL));
    GSList *uris = soup_server_get_uris(relay->server);
    g_assert_nonnull(uris);
    relay->url = g_strdup_printf("ws://127.0.0.1:%d/", g_uri_get_port(uris->data));
    g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
    return relay;
}

const gchar *bunker_test_relay_url(BunkerTestRelay *relay) { return relay->url; }
JsonArray *bunker_test_relay_events(BunkerTestRelay *relay) { return relay->events; }
JsonArray *bunker_test_relay_deliveries(BunkerTestRelay *relay) { return relay->deliveries; }
gint64 bunker_test_relay_last_since(BunkerTestRelay *relay) { return relay->last_since; }
void bunker_test_relay_disconnect(BunkerTestRelay *relay) {
    for (guint i = 0; i < relay->clients->len; i++) {
        RelayClient *client = relay->clients->pdata[i];
        if (soup_websocket_connection_get_state(client->socket) == SOUP_WEBSOCKET_STATE_OPEN)
            soup_websocket_connection_close(client->socket, SOUP_WEBSOCKET_CLOSE_SERVER_ERROR, "reconnect test");
    }
}
void bunker_test_relay_set_request_callback(BunkerTestRelay *relay, BunkerTestRelayRequestCb cb, gpointer data) {
    relay->request_cb = cb; relay->request_data = data;
}
void bunker_test_relay_disconnect_recipient(BunkerTestRelay *relay, const gchar *pubkey) {
    for (guint i = 0; i < relay->clients->len; i++) {
        RelayClient *client = relay->clients->pdata[i]; GHashTableIter it; gpointer request;
        gboolean addressed = FALSE;
        g_hash_table_iter_init(&it, client->subscriptions);
        while (g_hash_table_iter_next(&it, NULL, &request)) {
            JsonArray *filters = request;
            for (guint j = 2; j < json_array_get_length(filters); j++) {
                JsonObject *filter = json_array_get_object_element(filters, j);
                if (json_object_has_member(filter, "#p") && list_contains(json_object_get_array_member(filter, "#p"), pubkey))
                    addressed = TRUE;
            }
        }
        if (addressed && soup_websocket_connection_get_state(client->socket) == SOUP_WEBSOCKET_STATE_OPEN)
            soup_websocket_connection_close(client->socket, SOUP_WEBSOCKET_CLOSE_SERVER_ERROR, "reconnect recipient test");
    }
}
void bunker_test_relay_stats(BunkerTestRelay *relay, guint *connections, guint *requests, guint *published)
{
    *connections = relay->connections; *requests = relay->requests;
    *published = json_array_get_length(relay->events);
}
void bunker_test_relay_set_event_callback(BunkerTestRelay *relay, BunkerTestRelayEventCb cb, gpointer data) {
    relay->event_cb = cb; relay->event_data = data;
}
void bunker_test_relay_inject(BunkerTestRelay *relay, const gchar *event_json) {
    g_autoptr(JsonParser) parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, event_json, -1, NULL));
    JsonNode *event = json_parser_get_root(parser);
    NostrEvent *parsed = nostr_event_new();
    g_assert_cmpint(nostr_event_deserialize(parsed, event_json), ==, 0);
    g_assert_true(nostr_event_check_signature(parsed)); nostr_event_free(parsed);
    json_array_add_element(relay->events, json_node_copy(event));
    for (guint i = 0; i < relay->clients->len; i++) {
        RelayClient *client = relay->clients->pdata[i];
        GHashTableIter it; gpointer id;
        g_hash_table_iter_init(&it, client->subscriptions);
        while (g_hash_table_iter_next(&it, &id, NULL)) deliver(client, id, event);
    }
}
void bunker_test_relay_reject_subscriptions(BunkerTestRelay *r, gboolean enabled) { r->reject_subscriptions = enabled; }
void bunker_test_relay_reject_publishes(BunkerTestRelay *r, gboolean enabled) { r->reject_publishes = enabled; }
void bunker_test_relay_hold_acks(BunkerTestRelay *r, gboolean enabled) {
    r->hold_acks = enabled;
    if (!enabled) {
        for (guint i = 0; i < r->held_acks->len; i++) {
            HeldAck *ack = r->held_acks->pdata[i]; send_frame(ack->client, json_array_ref(ack->frame));
        }
        g_ptr_array_set_size(r->held_acks, 0);
    }
}
void bunker_test_relay_challenge(BunkerTestRelay *r) {
    for (guint i = 0; i < r->clients->len; i++) {
        JsonArray *frame = json_array_new(); json_array_add_string_element(frame, "AUTH");
        json_array_add_string_element(frame, "test-challenge"); send_frame(r->clients->pdata[i], frame);
    }
}
void bunker_test_relay_free(BunkerTestRelay *relay)
{
    soup_server_disconnect(relay->server);
    g_ptr_array_unref(relay->held_acks);
    g_ptr_array_unref(relay->clients);
    g_object_unref(relay->server);
    json_array_unref(relay->events); json_array_unref(relay->deliveries);
    g_free(relay->url); g_free(relay);
}
