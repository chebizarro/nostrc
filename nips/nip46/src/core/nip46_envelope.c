#include "nostr/nip46/nip46_types.h"
#include "nostr/nip46/nip46_envelope.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int set_p_tag(NostrEvent *ev, const char *receiver_pubkey_hex) {
    if (!receiver_pubkey_hex) return -1;
    NostrTag *ptag = nostr_tag_new("p", receiver_pubkey_hex, NULL);
    if (!ptag) return -1;
    NostrTags *tags = nostr_tags_new(1, ptag);
    if (!tags) { nostr_tag_free(ptag); return -1; }
    nostr_event_set_tags(ev, tags);
    return 0;
}

static int common_init(NostrEvent **out_event,
                       const char *sender_pubkey_hex,
                       const char *receiver_pubkey_hex,
                       const char *payload_json) {
    if (!out_event || !sender_pubkey_hex || !receiver_pubkey_hex || !payload_json)
        return -1;
    NostrEvent *ev = nostr_event_new();
    if (!ev) return -1;
    nostr_event_set_kind(ev, NOSTR_EVENT_KIND_NIP46);
    nostr_event_set_pubkey(ev, sender_pubkey_hex);
    nostr_event_set_content(ev, payload_json);
    /* created_at now for reasonable defaults */
    nostr_event_set_created_at(ev, (int64_t)time(NULL));
    if (set_p_tag(ev, receiver_pubkey_hex) != 0) {
        nostr_event_free(ev);
        return -1;
    }
    *out_event = ev;
    return 0;
}

int nostr_nip46_build_request_event(const char *sender_pubkey_hex,
                                    const char *receiver_pubkey_hex,
                                    const char *request_json,
                                    NostrEvent **out_event) {
    return common_init(out_event, sender_pubkey_hex, receiver_pubkey_hex, request_json);
}

int nostr_nip46_build_response_event(const char *sender_pubkey_hex,
                                     const char *receiver_pubkey_hex,
                                     const char *response_json,
                                     NostrEvent **out_event) {
    return common_init(out_event, sender_pubkey_hex, receiver_pubkey_hex, response_json);
}
