#include "nostr/nip31/nip31.h"
#include "nostr-tag.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static NostrTag *clone_tag(const NostrTag *src) {
    if (!src) return NULL;
    size_t n = nostr_tag_size(src);
    const char *k0 = nostr_tag_get(src, 0);
    NostrTag *dst = nostr_tag_new(k0 ? k0 : "", NULL);
    for (size_t i = 1; i < n; ++i) {
        const char *v = nostr_tag_get(src, i);
        if (v) nostr_tag_append(dst, v);
    }
    return dst;
}

int nostr_nip31_set_alt(NostrEvent *ev, const char *alt) {
    if (!ev || !alt) return -EINVAL;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    NostrTags *filtered = nostr_tags_new(0);
    if (!filtered) return -ENOMEM;

    // Copy over all non-'alt' tags
    if (tags) {
        size_t n = nostr_tags_size(tags);
        for (size_t i = 0; i < n; ++i) {
            NostrTag *t = nostr_tags_get(tags, i);
            if (!t) continue;
            const char *k = nostr_tag_get(t, 0);
            if (k && strcmp(k, "alt") == 0) continue;
            NostrTag *dup = clone_tag(t);
            if (dup) nostr_tags_append(filtered, dup);
        }
    }

    // Append new alt tag
    NostrTag *alt_tag = nostr_tag_new("alt", alt, NULL);
    if (!alt_tag) { nostr_tags_free(filtered); return -ENOMEM; }
    nostr_tags_append(filtered, alt_tag);

    nostr_event_set_tags(ev, filtered);
    return 0;
}

int nostr_nip31_get_alt(const NostrEvent *ev, char **out_alt) {
    if (!ev || !out_alt) return -EINVAL;
    *out_alt = NULL;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return -ENOENT;

    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t) continue;
        const char *k = nostr_tag_get(t, 0);
        if (!k || strcmp(k, "alt") != 0) continue;
        const char *v = (nostr_tag_size(t) >= 2) ? nostr_tag_get(t, 1) : NULL;
        if (!v) continue;
        char *dup = strdup(v);
        if (!dup) return -ENOMEM;
        *out_alt = dup;
        return 0;
    }
    return -ENOENT;
}
