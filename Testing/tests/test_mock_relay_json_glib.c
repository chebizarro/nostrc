#include <json-glib/json-glib.h>
#include "nostr/testing/mock_relay_server.h"
#include "nostr-event.h"
#include "nostr-relay.h"
#include "nostr-subscription.h"
#include "nostr-filter.h"
#include "nostr-tag.h"
#include "go.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)

int main(void) {
    /* Force json-glib to be loaded, ahead of jansson in the link closure. */
    JsonParser *parser = json_parser_new();
    CHECK(json_parser_load_from_data(parser, "{}", -1, NULL));
    g_object_unref(parser);
    NostrMockRelayServer *server = nostr_mock_server_new(NULL);
    CHECK(server);
    for (int i = 0; i < 2; i++) {
        NostrEvent *event = nostr_event_new();
        CHECK(event);
        event->kind = 1;
        event->created_at = 1700000000;
        nostr_event_set_content(event, i ? "miss" : "hit");
        nostr_event_set_tags(event, nostr_tags_new(2,
            nostr_tag_new("t", i ? "other" : "keep", NULL),
            nostr_tag_new("T", "upper", NULL)));
        CHECK(nostr_event_sign(event,
            "0000000000000000000000000000000000000000000000000000000000000001") == 0);
        char *json = nostr_event_serialize_compact(event);
        CHECK(json && nostr_mock_server_seed_event(server, json) == 0);
        free(json);
        nostr_event_free(event);
    }
    CHECK(nostr_mock_server_start(server) == 0);
    Error *error = NULL;
    NostrRelay *relay = nostr_relay_new(NULL, nostr_mock_server_get_url(server), &error);
    CHECK(relay && !error);
    nostr_relay_set_auto_reconnect(relay, false);
    CHECK(nostr_relay_connect(relay, &error));
    CHECK(nostr_relay_wait_established(relay, 10000, &error));
    NostrFilters *filters = nostr_filters_new();
    NostrFilter *filter = nostr_filter_new();
    int kind = 1;
    nostr_filter_set_kinds(filter, &kind, 1);
    nostr_filter_set_tags(filter, nostr_tags_new(2,
        nostr_tag_new("t", "keep", NULL), nostr_tag_new("T", "upper", NULL)));
    CHECK(nostr_filters_add(filters, filter));
    nostr_filter_free(filter);
    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, NULL, filters);
    CHECK(sub && nostr_subscription_fire(sub, &error));
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 10; /* failure-only bound; EOSE completes catch-up */
    GoContext *bound = go_with_deadline(NULL, deadline);
    void *signal = NULL;
    CHECK(go_channel_receive_with_context(nostr_subscription_get_eose_channel(sub), &signal, bound) == 0);
    NostrEvent *received = NULL;
    CHECK(go_channel_try_receive(nostr_subscription_get_events_channel(sub), (void **)&received) == 0);
    CHECK(received && strcmp(received->content, "hit") == 0);
    nostr_event_free(received);
    CHECK(go_channel_try_receive(nostr_subscription_get_events_channel(sub), (void **)&received) != 0);
    go_context_unref(bound);
    nostr_subscription_unsubscribe(sub);
    nostr_subscription_free(sub);
    nostr_filters_free(filters);
    nostr_relay_free(relay);
    nostr_mock_server_free(server);
    return 0;
}
