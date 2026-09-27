#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "nostr-envelope.h"
#include "nostr-event.h"
#include "nostr_jansson.h"

int main(void) {
    const char *json = "[\"EVENT\",\"sub\",{\"kind\":1,\"created_at\":123,\"content\":\"x\"}]";
    nostr_set_json_interface(jansson_impl);
    nostr_json_init();

    NostrEventEnvelope *compact = calloc(1, sizeof(*compact));
    assert(compact);
    compact->base.type = NOSTR_ENVELOPE_EVENT;
    assert(nostr_envelope_deserialize_compact((NostrEnvelope *)compact, json, NULL) == 0);
    assert(compact->subscription_id == NULL);
    assert(compact->event == NULL);
    nostr_envelope_free((NostrEnvelope *)compact);

    NostrEventEnvelope *fallback = calloc(1, sizeof(*fallback));
    assert(fallback);
    fallback->base.type = NOSTR_ENVELOPE_EVENT;
    assert(nostr_envelope_deserialize((NostrEnvelope *)fallback, json) == 0);
    assert(fallback->subscription_id && strcmp(fallback->subscription_id, "sub") == 0);
    assert(fallback->event && fallback->event->kind == 1);
    nostr_envelope_free((NostrEnvelope *)fallback);

    NostrEventEnvelope *malformed = calloc(1, sizeof(*malformed));
    assert(malformed);
    malformed->base.type = NOSTR_ENVELOPE_EVENT;
    assert(nostr_envelope_deserialize((NostrEnvelope *)malformed,
           "[\"EVENT\"{\"kind\":1,\"created_at\":061,\"content\":\"x\"}]") == -1);
    nostr_envelope_free((NostrEnvelope *)malformed);

    FILE *seed = fopen(SIGNED_EVENT_SEED_PATH, "r");
    assert(seed);
    char signed_json[1024];
    assert(fgets(signed_json, sizeof(signed_json), seed));
    fclose(seed);
    NostrEnvelope *signed_env = nostr_envelope_parse(signed_json);
    assert(signed_env && signed_env->type == NOSTR_ENVELOPE_EVENT);
    assert(nostr_event_check_signature(((NostrEventEnvelope *)signed_env)->event));
    nostr_envelope_free(signed_env);

    nostr_json_cleanup();
    puts("test_envelope_fallback OK");
    return 0;
}
