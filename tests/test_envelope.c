#include "nostr-envelope.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void test_parse_envelope() {
    const char *message = "[\"EVENT\",\"subid\",{\"id\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"pubkey\":\"test_pubkey\",\"created_at\":1234567890,\"kind\":1,\"tags\":[],\"content\":\"Hello, Nostr!\",\"sig\":\"test_sig\"}]";
    NostrEnvelope *envelope = nostr_envelope_parse(message);
    assert(envelope != NULL);
    assert(envelope->type == NOSTR_ENVELOPE_EVENT);
    NostrEventEnvelope *event_envelope = (NostrEventEnvelope *)envelope;
    assert(strcmp(event_envelope->subscription_id, "subid") == 0);
    assert(event_envelope->event != NULL);
    nostr_envelope_free(envelope);
    printf("test_parse_envelope: PASSED\n");
}

void test_envelope_serialize_roundtrip() {
    /* Parse an EVENT envelope from JSON */
    const char *original = "[\"EVENT\",\"subid\",{\"id\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"pubkey\":\"test_pubkey\",\"created_at\":1234567890,\"kind\":1,\"tags\":[],\"content\":\"Hello, Nostr!\",\"sig\":\"test_sig\"}]";
    NostrEnvelope *envelope = nostr_envelope_parse(original);
    assert(envelope != NULL);
    assert(envelope->type == NOSTR_ENVELOPE_EVENT);

    /* Serialize back to JSON */
    char *json = nostr_envelope_serialize_compact(envelope);
    assert(json != NULL);
    printf("Serialized EventEnvelope: %s\n", json);

    /* Verify the JSON contains expected content */
    assert(strstr(json, "EVENT") != NULL);
    assert(strstr(json, "subid") != NULL);
    assert(strstr(json, "test_pubkey") != NULL);

    free(json);
    nostr_envelope_free(envelope);
    printf("test_envelope_serialize_roundtrip: PASSED\n");
}

void test_ok_envelope_serialize() {
    /* Parse an OK envelope */
    const char *ok_json = "[\"OK\",\"event_id_here\",true,\"accepted\"]";
    NostrEnvelope *envelope = nostr_envelope_parse(ok_json);
    assert(envelope != NULL);
    assert(envelope->type == NOSTR_ENVELOPE_OK);

    /* Serialize */
    char *json = nostr_envelope_serialize_compact(envelope);
    assert(json != NULL);
    printf("Serialized OKEnvelope: %s\n", json);
    assert(strstr(json, "OK") != NULL);

    free(json);
    nostr_envelope_free(envelope);
    printf("test_ok_envelope_serialize: PASSED\n");
}

int main() {
    test_parse_envelope();
    test_envelope_serialize_roundtrip();
    test_ok_envelope_serialize();
    printf("\nAll envelope tests passed!\n");
    return 0;
}
