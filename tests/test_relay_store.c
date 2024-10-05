#include "relay_store.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

// Dummy implementations for testing
int dummy_publish(void *self, void *ctx, NostrEvent *event) {
    return 0;
}

int dummy_query_sync(void *self, void *ctx, Filter *filter, NostrEvent ***events, size_t *events_count) {
    *events_count = 1;
    *events = (NostrEvent **)malloc(sizeof(NostrEvent *));
    (*events)[0] = create_event();
    (*events)[0]->content = strdup("dummy event");
    return 0;
}

int main() {
    MultiStore *multi = create_multi_store(2);
    assert(multi != NULL);

    RelayStore store1 = {dummy_publish, dummy_query_sync};
    RelayStore store2 = {dummy_publish, dummy_query_sync};
    multi->stores[multi->stores_count++] = &store1;
    multi->stores[multi->stores_count++] = &store2;

    NostrEvent *event = create_event();
    event->content = strdup("test event");

    int pub_result = multi_store_publish(multi, NULL, event);
    assert(pub_result == 0);

    NostrEvent **events = NULL;
    size_t events_count = 0;
    int query_result = multi_store_query_sync(multi, NULL, NULL, &events, &events_count);
    assert(query_result == 0);
    assert(events_count == 2);
    assert(strcmp(events[0]->content, "dummy event") == 0);
    assert(strcmp(events[1]->content, "dummy event") == 0);

    for (size_t i = 0; i < events_count; i++) {
        free_event(events[i]);
    }
    free(events);

    free_event(event);
    free_multi_store(multi);

    printf("All tests passed!\n");
    return 0;
}