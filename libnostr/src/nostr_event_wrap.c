#include <string.h>
#include "event.h"      /* internal */
#include "tag.h"        /* internal Tags */
#include "nostr-tag.h"
#include "nostr-event.h" /* public transitional */

/* Accessors */
const char *nostr_event_get_pubkey(const NostrEvent *event) {
    return event ? event->pubkey : NULL;
}

void nostr_event_set_pubkey(NostrEvent *event, const char *pubkey) {
    if (!event) return;
    free(event->pubkey);
    event->pubkey = pubkey ? strdup(pubkey) : NULL;
}

int64_t nostr_event_get_created_at(const NostrEvent *event) {
    return event ? event->created_at : 0;
}

void nostr_event_set_created_at(NostrEvent *event, int64_t created_at) {
    if (!event) return;
    event->created_at = created_at;
}

int nostr_event_get_kind(const NostrEvent *event) {
    return event ? event->kind : 0;
}

void nostr_event_set_kind(NostrEvent *event, int kind) {
    if (!event) return;
    event->kind = kind;
}

void *nostr_event_get_tags(const NostrEvent *event) {
    return event ? (void *)event->tags : NULL;
}

void nostr_event_set_tags(NostrEvent *event, void *tags) {
    if (!event) return;
    if ((void *)event->tags != tags) {
        if (event->tags) {
            nostr_tags_free(event->tags);
        }
        event->tags = (Tags *)tags;
    }
}

const char *nostr_event_get_content(const NostrEvent *event) {
    return event ? event->content : NULL;
}

void nostr_event_set_content(NostrEvent *event, const char *content) {
    if (!event) return;
    free(event->content);
    event->content = content ? strdup(content) : NULL;
}

const char *nostr_event_get_sig(const NostrEvent *event) {
    return event ? event->sig : NULL;
}

void nostr_event_set_sig(NostrEvent *event, const char *sig) {
    if (!event) return;
    free(event->sig);
    event->sig = sig ? strdup(sig) : NULL;
}
