#include "nostr/nip40/nip40.h"
#include "nostr-tag.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

int nostr_nip40_get_expiration(const NostrEvent *ev, int64_t *out_timestamp) {
    if (!ev || !out_timestamp) return -EINVAL;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return -ENOENT;

    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t) continue;
        const char *k = nostr_tag_get(t, 0);
        if (!k || strcmp(k, "expiration") != 0) continue;
        if (nostr_tag_size(t) < 2) continue;
        const char *v = nostr_tag_get(t, 1);
        if (!v || *v == '\0') continue;

        char *endptr = NULL;
        long long ts = strtoll(v, &endptr, 10);
        if (endptr == v || *endptr != '\0') continue;
        if (ts < 0) continue;

        *out_timestamp = (int64_t)ts;
        return 0;
    }
    return -ENOENT;
}

bool nostr_nip40_is_expired_at(const NostrEvent *ev, int64_t now) {
    int64_t expiration = 0;
    if (nostr_nip40_get_expiration(ev, &expiration) != 0)
        return false;
    return now >= expiration;
}

bool nostr_nip40_is_expired(const NostrEvent *ev) {
    return nostr_nip40_is_expired_at(ev, (int64_t)time(NULL));
}

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

int nostr_nip40_set_expiration(NostrEvent *ev, int64_t timestamp) {
    if (!ev) return -EINVAL;
    if (timestamp < 0) return -EINVAL;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    NostrTags *filtered = nostr_tags_new(0);
    if (!filtered) return -ENOMEM;

    /* Copy over all non-expiration tags */
    if (tags) {
        size_t n = nostr_tags_size(tags);
        for (size_t i = 0; i < n; ++i) {
            NostrTag *t = nostr_tags_get(tags, i);
            if (!t) continue;
            const char *k = nostr_tag_get(t, 0);
            if (k && strcmp(k, "expiration") == 0) continue;
            NostrTag *dup = clone_tag(t);
            if (dup) nostr_tags_append(filtered, dup);
        }
    }

    /* Build the expiration value string */
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)timestamp);

    NostrTag *exp_tag = nostr_tag_new("expiration", buf, NULL);
    if (!exp_tag) { nostr_tags_free(filtered); return -ENOMEM; }
    nostr_tags_append(filtered, exp_tag);

    nostr_event_set_tags(ev, filtered);
    return 0;
}

int nostr_nip40_set_expiration_in(NostrEvent *ev, int64_t seconds_from_now) {
    if (seconds_from_now < 0) return -EINVAL;
    int64_t expiration = (int64_t)time(NULL) + seconds_from_now;
    return nostr_nip40_set_expiration(ev, expiration);
}

bool nostr_nip40_should_relay_accept(const NostrEvent *ev) {
    return !nostr_nip40_is_expired(ev);
}
