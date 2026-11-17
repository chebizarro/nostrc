/* Grotto bunker_test_relay's main-context/Soup NIP-01 fixture, adapted to the
 * FIPS gate's existing Jansson dependency. Testing/mock_relay_server instead
 * uses startup sleeps/volatile polling; neither those nor JSON-GLib are needed
 * here. All server state and all socket operations have one context owner. */
#define _POSIX_C_SOURCE 200809L
#include "nostr_relay_fixture.h"
#include <libsoup/soup.h>
#include <jansson.h>
#include <nostr-event.h>
#include <json.h>
#include <nostr-filter.h>
#include <nostr-kinds.h>
#include <channel.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
typedef struct relay relay;
typedef struct { relay *owner; SoupWebsocketConnection *socket; json_t *subscriptions; } client;
struct relay {
    SoupServer *server;
    GPtrArray *clients;
    json_t *events;
    char *url;
    bool hold_eose, reject_publish;
};
struct fixture { GMainContext *context; GMainLoop *loop; pthread_t thread; GoChannel *ready; relay relays[2]; };
static void send_frame(client *c, json_t *frame)
{
    char *text = json_dumps(frame, JSON_COMPACT); CHECK(text); json_decref(frame);
    if (soup_websocket_connection_get_state(c->socket) == SOUP_WEBSOCKET_STATE_OPEN)
        soup_websocket_connection_send_text(c->socket, text);
    free(text);
}
static NostrEvent *parse_event(json_t *object)
{
    char *raw = json_dumps(object, JSON_COMPACT); CHECK(raw);
    NostrEvent *e = nostr_event_new(); CHECK(e && nostr_event_deserialize(e, raw) == 0); free(raw); return e;
}
static bool matches(json_t *filter, NostrEvent *e)
{
    char *raw = json_dumps(filter, JSON_COMPACT); CHECK(raw);
    NostrFilter *f = nostr_filter_new(); CHECK(f && nostr_filter_deserialize(f, raw) == 0); free(raw);
    bool match = nostr_filter_matches(f, e); nostr_filter_free(f); return match;
}
static void deliver(client *c, const char *id, json_t *event)
{ send_frame(c, json_pack("[sso]", "EVENT", id, json_incref(event))); }
static void eose(client *c, const char *id) { send_frame(c, json_pack("[ss]", "EOSE", id)); }
static void broadcast(relay *r, json_t *event, bool hostile)
{
    NostrEvent *e = parse_event(event);
    for (guint i = 0; i < r->clients->len; ++i) {
        client *c = g_ptr_array_index(r->clients, i); const char *id; json_t *request;
        json_object_foreach(c->subscriptions, id, request) {
            bool match = hostile;
            for (size_t j = 2; !match && j < json_array_size(request); ++j) match = matches(json_array_get(request, j), e);
            if (match) deliver(c, id, event);
        }
    }
    nostr_event_free(e);
}
static void backfill(client *c, const char *id, json_t *request)
{
    GHashTable *sent = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (size_t f = 2; f < json_array_size(request); ++f) {
        json_t *filter = json_array_get(request, f), *limit = json_object_get(filter, "limit"), *best = NULL;
        if (json_is_integer(limit) && json_integer_value(limit) == 0) continue;
        for (size_t i = 0; i < json_array_size(c->owner->events); ++i) {
            json_t *event = json_array_get(c->owner->events, i); NostrEvent *e = parse_event(event);
            bool match = matches(filter, e); nostr_event_free(e);
            if (!match) continue;
            if (json_integer_value(limit) == 1) {
                if (!best || json_integer_value(json_object_get(event, "created_at")) > json_integer_value(json_object_get(best, "created_at"))) best = event;
            } else {
                const char *key = json_string_value(json_object_get(event, "id"));
                if (!g_hash_table_contains(sent, key)) { deliver(c, id, event); g_hash_table_add(sent, g_strdup(key)); }
            }
        }
        if (best) deliver(c, id, best);
    }
    g_hash_table_unref(sent);
    if (!c->owner->hold_eose) eose(c, id);
}
static bool auth_tag(const NostrEvent *e, const char *key, const char *value)
{
    for (size_t i = 0; i < nostr_tags_size(e->tags); ++i) {
        NostrTag *tag = nostr_tags_get(e->tags, i);
        if (!g_strcmp0(nostr_tag_get_key(tag), key) && !g_strcmp0(nostr_tag_get_value(tag), value)) return true;
    }
    return false;
}
static void message_received(SoupWebsocketConnection *socket, SoupWebsocketDataType type, GBytes *bytes, gpointer user)
{
    (void)socket; CHECK(type == SOUP_WEBSOCKET_DATA_TEXT); client *c = user; relay *r = c->owner;
    gsize len; const char *text = g_bytes_get_data(bytes, &len); json_t *frame = json_loadb(text, len, 0, NULL); CHECK(frame);
    const char *verb = json_string_value(json_array_get(frame, 0)); CHECK(verb);
    if (!strcmp(verb, "REQ")) {
        const char *id = json_string_value(json_array_get(frame, 1)); CHECK(id);
        CHECK(json_object_set(c->subscriptions, id, frame) == 0); backfill(c, id, frame);
    } else if (!strcmp(verb, "CLOSE")) {
        json_object_del(c->subscriptions, json_string_value(json_array_get(frame, 1)));
    } else if (!strcmp(verb, "EVENT") || !strcmp(verb, "AUTH")) {
        json_t *object = json_array_get(frame, 1); NostrEvent *e = parse_event(object);
        bool valid = nostr_event_validate(e, NULL) == NOSTR_EVENT_VALIDATION_OK;
        bool auth = !strcmp(verb, "AUTH");
        if (auth) valid = valid && e->kind == NOSTR_KIND_CLIENT_AUTHENTICATION &&
            auth_tag(e, "challenge", "test-challenge") && auth_tag(e, "relay", r->url);
        bool accepted = valid && (auth || !r->reject_publish);
        send_frame(c, json_pack("[ssbs]", "OK", e->id, accepted, accepted ? "" : "blocked: test fixture"));
        if (accepted && !auth) {
            if (!nostr_kind_is_ephemeral(e->kind)) CHECK(json_array_append(r->events, object) == 0);
            broadcast(r, object, false);
        }
        nostr_event_free(e);
    } else CHECK(false);
    json_decref(frame);
}
static void client_free(gpointer user)
{
    client *c = user; g_signal_handlers_disconnect_by_data(c->socket, c);
    if (soup_websocket_connection_get_state(c->socket) == SOUP_WEBSOCKET_STATE_OPEN)
        soup_websocket_connection_close(c->socket, SOUP_WEBSOCKET_CLOSE_GOING_AWAY, NULL);
    g_object_unref(c->socket); json_decref(c->subscriptions); free(c);
}
static void connected(SoupServer *server, SoupServerMessage *message, const char *path, SoupWebsocketConnection *socket, gpointer user)
{
    (void)server; (void)message; (void)path; relay *r = user; client *c = calloc(1, sizeof(*c)); CHECK(c);
    c->owner = r; c->socket = g_object_ref(socket); c->subscriptions = json_object(); CHECK(c->subscriptions);
    g_ptr_array_add(r->clients, c); g_signal_connect(socket, "message", G_CALLBACK(message_received), c);
}
static void nip11(SoupServer *server, SoupServerMessage *message, const char *path, GHashTable *query, gpointer user)
{
    (void)server; (void)path; (void)query; (void)user;
    const char *accept = soup_message_headers_get_one(soup_server_message_get_request_headers(message), "Accept");
    if (g_strcmp0(accept, "application/nostr+json")) return;
    const char *doc = "{\"supported_nips\":[1,11,40,42],\"limitation\":{\"max_filters\":2,\"max_subscriptions\":8}}";
    soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
    soup_server_message_set_response(message, "application/nostr+json", SOUP_MEMORY_COPY, doc, strlen(doc));
}
static void *serve(void *user)
{
    fixture *f = user; g_main_context_push_thread_default(f->context);
    for (size_t i = 0; i < 2; ++i) {
        relay *r = &f->relays[i]; r->server = soup_server_new(NULL, NULL); r->events = json_array();
        r->clients = g_ptr_array_new_with_free_func(client_free);
        soup_server_add_handler(r->server, "/", nip11, r, NULL);
        soup_server_add_websocket_handler(r->server, "/", NULL, NULL, connected, r, NULL);
        CHECK(soup_server_listen_local(r->server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, NULL));
        GSList *uris = soup_server_get_uris(r->server); CHECK(uris);
        r->url = g_strdup_printf("ws://127.0.0.1:%d/", g_uri_get_port(uris->data));
        g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
    }
    CHECK(go_channel_send(f->ready, f) == 0); g_main_loop_run(f->loop);
    for (size_t i = 0; i < 2; ++i) {
        relay *r = &f->relays[i]; soup_server_disconnect(r->server); g_ptr_array_unref(r->clients);
        g_object_unref(r->server); json_decref(r->events); g_free(r->url);
    }
    g_main_context_pop_thread_default(f->context); return NULL;
}
typedef struct {
    fixture *f; operation op; int relay_index; const char *json; GoChannel *done;
    int kind; const char *author; size_t count; char *latest; /* QUERY */
} request;
static gboolean on_server(gpointer user)
{
    request *q = user;
    for (int i = 0; i < 2; ++i) if (q->relay_index < 0 || i == q->relay_index) {
        relay *r = &q->f->relays[i];
        switch (q->op) {
        case INJECT: case SEED: {
            json_t *e = json_loads(q->json, 0, NULL); CHECK(e); NostrEvent *parsed = parse_event(e);
            CHECK(nostr_event_check_signature(parsed)); nostr_event_free(parsed);
            CHECK(json_array_append(r->events, e) == 0);
            if (q->op == INJECT) broadcast(r, e, true);
            json_decref(e); break;
        }
        case CLEAR: json_decref(r->events); r->events = json_array(); break;
        case REJECT_PUBLISH: r->reject_publish = true; break;
        case HOLD_EOSE: r->hold_eose = true; break;
        case STOP: g_main_loop_quit(q->f->loop); break;
        case QUERY:
            for (size_t k = 0; k < json_array_size(r->events); ++k) {
                json_t *event = json_array_get(r->events, k);
                if (json_integer_value(json_object_get(event, "kind")) != q->kind ||
                    g_strcmp0(json_string_value(json_object_get(event, "pubkey")), q->author)) continue;
                ++q->count; free(q->latest); q->latest = json_dumps(event, JSON_COMPACT); CHECK(q->latest);
            }
            break;
        default:
            for (guint j = 0; j < r->clients->len; ++j) {
                client *c = g_ptr_array_index(r->clients, j);
                if (q->op == CHALLENGE || q->op == AUTH_CLOSED) send_frame(c, json_pack("[ss]", "AUTH", "test-challenge"));
                if (q->op == DISCONNECT) soup_websocket_connection_close(c->socket, SOUP_WEBSOCKET_CLOSE_SERVER_ERROR, "network reconnect test");
                const char *id; json_t *sub;
                json_object_foreach(c->subscriptions, id, sub) {
                    (void)sub;
                    if (q->op == RELEASE_EOSE) eose(c, id);
                    if (q->op == CLOSE_SUBS || q->op == AUTH_CLOSED)
                        send_frame(c, json_pack("[sss]", "CLOSED", id, "auth-required: authenticate first"));
                }
                if (q->op == CLOSE_SUBS || q->op == AUTH_CLOSED) json_object_clear(c->subscriptions);
            }
            if (q->op == RELEASE_EOSE) r->hold_eose = false;
            break;
        }
    }
    CHECK(go_channel_send(q->done, q) == 0); return G_SOURCE_REMOVE;
}
void fixture_call(fixture *f, operation op, int relay_index, const char *json)
{
    request q = {.f = f, .op = op, .relay_index = relay_index, .json = json, .done = go_channel_create(1)}; CHECK(q.done);
    g_main_context_invoke(f->context, on_server, &q);
    void *done; CHECK(go_channel_receive(q.done, &done) == 0 && done == &q); go_channel_unref(q.done);
}
fixture *fixture_start(void)
{
    fixture *f = calloc(1, sizeof(*f)); CHECK(f); f->context = g_main_context_new(); f->ready = go_channel_create(1);
    f->loop = g_main_loop_new(f->context, FALSE); CHECK(pthread_create(&f->thread, NULL, serve, f) == 0);
    void *ready; CHECK(go_channel_receive(f->ready, &ready) == 0 && ready == f); return f;
}
const char *fixture_url(fixture *f, size_t i) { CHECK(i < 2); return f->relays[i].url; }
char *fixture_latest(fixture *f, size_t index, int kind, const char *author, size_t *count)
{
    CHECK(index < 2 && author && count);
    request q = {.f = f, .op = QUERY, .relay_index = (int)index, .done = go_channel_create(1), .kind = kind, .author = author};
    CHECK(q.done);
    g_main_context_invoke(f->context, on_server, &q);
    void *done; CHECK(go_channel_receive(q.done, &done) == 0 && done == &q); go_channel_unref(q.done);
    *count = q.count; return q.latest;
}
void fixture_destroy(fixture *f)
{
    fixture_call(f, STOP, 0, NULL); CHECK(pthread_join(f->thread, NULL) == 0);
    g_main_loop_unref(f->loop); g_main_context_unref(f->context); go_channel_unref(f->ready); free(f);
}
