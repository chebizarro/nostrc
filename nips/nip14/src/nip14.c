#include "nostr/nip14/nip14.h"
#include <errno.h>
#include <string.h>

const char *nostr_nip14_get_subject(const NostrEvent *ev) {
    if (!ev) return NULL;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return NULL;

    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;
        const char *k = nostr_tag_get(t, 0);
        if (k && strcmp(k, "subject") == 0)
            return nostr_tag_get(t, 1);
    }
    return NULL;
}

bool nostr_nip14_has_subject(const NostrEvent *ev) {
    return nostr_nip14_get_subject(ev) != NULL;
}

int nostr_nip14_set_subject(NostrEvent *ev, const char *subject) {
    if (!ev || !subject) return -EINVAL;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("subject", subject, NULL));
    return 0;
}
