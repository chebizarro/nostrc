#include "nostr/nip46/nip46_envelope.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include <stdio.h>
#include <string.h>

static int has_p_tag(const NostrTags *tags, const char *receiver) {
    if (!tags || !receiver) return 0;
    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t) continue;
        const char *k = nostr_tag_get(t, 0);
        const char *v = nostr_tag_get(t, 1);
        if (k && v && strcmp(k, "p") == 0 && strcmp(v, receiver) == 0) return 1;
    }
    return 0;
}

int main(void) {
    const char *sender = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const char *receiver = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
    const char *req_json = "{\"id\":\"1\",\"method\":\"get_public_key\",\"params\":[]}";

    NostrEvent *ev = NULL;
    if (nostr_nip46_build_request_event(sender, receiver, req_json, &ev) != 0 || !ev) {
        printf("build_request: FAIL\n");
        return 1;
    }
    if (ev->kind != NOSTR_EVENT_KIND_NIP46) { printf("wrong kind\n"); return 2; }
    if (!ev->pubkey || strcmp(ev->pubkey, sender) != 0) { printf("wrong pubkey\n"); return 3; }
    if (!ev->content || strcmp(ev->content, req_json) != 0) { printf("wrong content\n"); return 4; }
    if (!has_p_tag((NostrTags*)ev->tags, receiver)) { printf("missing p tag\n"); return 5; }

    // response
    const char *resp_json = "{\"id\":\"1\",\"result\":\"ok\"}";
    NostrEvent *ev2 = NULL;
    if (nostr_nip46_build_response_event(receiver, sender, resp_json, &ev2) != 0 || !ev2) {
        printf("build_response: FAIL\n");
        return 6;
    }
    if (ev2->kind != NOSTR_EVENT_KIND_NIP46) { printf("wrong kind 2\n"); return 7; }
    if (!has_p_tag((NostrTags*)ev2->tags, sender)) { printf("missing p tag 2\n"); return 8; }

    nostr_event_free(ev);
    nostr_event_free(ev2);
    printf("test_nip46_envelope_build: OK\n");
    return 0;
}
