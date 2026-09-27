#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "nostr-envelope.h"
#include "nostr_jansson.h"

// LibFuzzer entry point for envelope parse
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // Ensure NUL-terminated buffer for JSON parser
    char *buf = (char*)malloc(size + 1);
    if (!buf) return 0;
    memcpy(buf, data, size);
    buf[size] = '\0';

    NostrEnvelope *env = nostr_envelope_parse(buf);
    nostr_envelope_free(env);

    /* Exercise the public compact-to-Jansson fallback with an owned EVENT receiver. */
    if (size >= 8 && memcmp(buf, "[\"EVENT\"", 8) == 0) {
        nostr_set_json_interface(jansson_impl);
        NostrEventEnvelope *event_env = calloc(1, sizeof(*event_env));
        if (event_env) {
            event_env->base.type = NOSTR_ENVELOPE_EVENT;
            (void)nostr_envelope_deserialize((NostrEnvelope *)event_env, buf);
            nostr_envelope_free((NostrEnvelope *)event_env);
        }
    }

    free(buf);
    return 0;
}
