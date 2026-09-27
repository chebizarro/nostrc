#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "nostr-envelope.h"

// LibFuzzer entry point for envelope parse
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // Ensure NUL-terminated buffer for JSON parser
    char *buf = (char*)malloc(size + 1);
    if (!buf) return 0;
    memcpy(buf, data, size);
    buf[size] = '\0';

    NostrEnvelope *env = nostr_envelope_parse(buf);
    nostr_envelope_free(env);

    free(buf);
    return 0;
}
