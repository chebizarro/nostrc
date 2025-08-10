#include "nostr-envelope.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void test_parse_message() {
    char *message = "[\"EVENT\",\"subid\",{\"pubkey\":\"test_pubkey\",\"created_at\":1234567890,\"kind\":1,\"tags\":[],\"content\":\"Hello, Nostr!\",\"sig\":\"test_sig\"}]";
    NostrEnvelope *envelope = parse_message(message);
    assert(envelope != NULL);
    assert(envelope->type == NOSTR_ENVELOPE_EVENT);
    NostrEventEnvelope *event_envelope = (NostrEventEnvelope *)envelope;
    assert(strcmp(event_envelope->subscription_id, "subid") == 0);
    assert(strcmp(event_envelope->event.pubkey, "test_pubkey") == 0);
    free_envelope(envelope);
}

void test_envelope_to_json() {
    NostrEventEnvelope *event_envelope = (NostrEventEnvelope *)malloc(sizeof(NostrEventEnvelope));
    event_envelope->base.type = NOSTR_ENVELOPE_EVENT;
    event_envelope->subscription_id = strdup("subid");
    event_envelope->event.pubkey = strdup("test_pubkey");
    event_envelope->event.created_at = 1234567890;
    event_envelope->event.kind = 1;
    event_envelope->event.tags = NULL;
    event_envelope->event.content = strdup("Hello, Nostr!");
    event_envelope->event.sig = strdup("test_sig");
    // Set other event fields...
    char *json = envelope_to_json((NostrEnvelope *)event_envelope);
    assert(json != NULL);
    printf("EventEnvelope JSON: %s\n", json);
    free(json);
    free_envelope((NostrEnvelope *)event_envelope);
}

int main() {
    test_parse_message();
    test_envelope_to_json();
    printf("All tests passed!\n");
    return 0;
}