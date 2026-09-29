#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "nostr-envelope.h"
#include "nostr_jansson.h"

/* Round-trip property (nostrc-ptwq): an envelope the strict parser accepted
 * must serialize to a frame the parser accepts again, as the same type, and
 * serialize(parse(frame)) must be that frame. A serializer that drops or
 * overwrites a byte (REQ/COUNT lost their closing ']') fails here. */
static void check_roundtrip(const NostrEnvelope *env) {
    char *frame = nostr_envelope_serialize_compact(env);
    if (!frame) return; /* some parsed shapes have no compact form */
    NostrEnvelope *again = nostr_envelope_parse(frame);
    if (!again || again->type != env->type) {
        fprintf(stderr, "reserialized envelope does not parse: %.256s\n", frame);
        abort();
    }
    char *frame2 = nostr_envelope_serialize_compact(again);
    if (!frame2 || strcmp(frame, frame2) != 0) {
        fprintf(stderr, "serialize(parse(x)) is not a fixed point:\n  %.256s\n  %.256s\n",
                frame, frame2 ? frame2 : "(null)");
        abort();
    }
    free(frame2);
    nostr_envelope_free(again);
    free(frame);
}

// LibFuzzer entry point for envelope parse
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // Ensure NUL-terminated buffer for JSON parser
    char *buf = (char*)malloc(size + 1);
    if (!buf) return 0;
    memcpy(buf, data, size);
    buf[size] = '\0';

    NostrEnvelope *env = nostr_envelope_parse(buf);
    if (env) check_roundtrip(env);
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
