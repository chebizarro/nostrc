#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "event.h"

// LibFuzzer entry point
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // Ensure NUL-terminated buffer for JSON parser
    char *buf = (char*)malloc(size + 1);
    if (!buf) return 0;
    memcpy(buf, data, size);
    buf[size] = '\0';

    // Initialize JSON interface once per process. It's safe to call repeatedly.
    extern NostrJsonInterface *jansson_impl;
    nostr_set_json_interface(jansson_impl);
    nostr_json_init();

    NostrEvent *e = create_event();
    if (e) {
        (void)nostr_event_deserialize(e, buf);
        free_event(e);
    }

    free(buf);
    return 0;
}
