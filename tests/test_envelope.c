#include "envelope.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void test_parse_message() {
    char *message = "[\"EVENT\",\"subid\",{\"pubkey\":\"test_pubkey\",\"created_at\":1234567890,\"kind\":1,\"tags\":[],\"content\":\"Hello, Nostr!\",\"sig\":\"test_sig\"}]";
    Envelope *envelope = parse_message(message);
    assert(envelope != NULL);
    assert(envelope->type == ENVELOPE_EVENT);
    EventEnvelope *event_envelope = (EventEnvelope *)envelope;
    assert(strcmp(event_envelope->subscription_id, "subid") == 0);
    assert(strcmp(event_envelope->event.pubkey, "test_pubkey") == 0);
    free_envelope(envelope);
}

void test_envelope_to_json() {
    EventEnvelope *event_envelope = (EventEnvelope *)malloc(sizeof(EventEnvelope));
    event_envelope->base.type = ENVELOPE_EVENT;
    event_envelope->subscription_id = strdup("subid");
    event_envelope->event.pubkey = strdup("test_pubkey");
    // Set other event fields...
    char *json = envelope_to_json((Envelope *)event_envelope);
    assert(json != NULL);
    printf("EventEnvelope JSON: %s\n", json);
    free(json);
    free_envelope((Envelope *)event_envelope);
}

int main() {
    test_parse_message();
    test_envelope_to_json();
    printf("All tests passed!\n");
    return 0;
}