#include "nostr/nip99/nip99.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int nostr_nip99_parse(const NostrEvent *ev, NostrNip99Listing *out) {
    if (!ev || !out) return -EINVAL;
    memset(out, 0, sizeof(*out));

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return -ENOENT;

    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;

        const char *k = nostr_tag_get(t, 0);
        const char *v = nostr_tag_get(t, 1);
        if (!k || !v) continue;

        if (strcmp(k, "d") == 0 && !out->identifier) {
            out->identifier = v;
        } else if (strcmp(k, "title") == 0 && !out->title) {
            out->title = v;
        } else if (strcmp(k, "summary") == 0 && !out->summary) {
            out->summary = v;
        } else if (strcmp(k, "location") == 0 && !out->location) {
            out->location = v;
        } else if (strcmp(k, "published_at") == 0 && out->published_at == 0) {
            char *endptr;
            long long ts = strtoll(v, &endptr, 10);
            if (endptr != v && *endptr == '\0' && ts > 0)
                out->published_at = (int64_t)ts;
        } else if (strcmp(k, "price") == 0 && !out->price.amount) {
            out->price.amount = v;
            if (nostr_tag_size(t) >= 3)
                out->price.currency = nostr_tag_get(t, 2);
            if (nostr_tag_size(t) >= 4)
                out->price.frequency = nostr_tag_get(t, 3);
        }
    }

    return out->identifier ? 0 : -ENOENT;
}

bool nostr_nip99_validate(const NostrEvent *ev) {
    if (!ev) return false;

    int kind = nostr_event_get_kind(ev);
    if (kind != NOSTR_NIP99_KIND_LISTING && kind != NOSTR_NIP99_KIND_DRAFT_LISTING)
        return false;

    NostrNip99Listing listing;
    if (nostr_nip99_parse(ev, &listing) != 0)
        return false;

    /* Required: d, title, summary, published_at, location, price */
    return listing.identifier &&
           listing.title &&
           listing.summary &&
           listing.published_at > 0 &&
           listing.location &&
           listing.price.amount &&
           listing.price.currency;
}

bool nostr_nip99_is_listing(const NostrEvent *ev) {
    if (!ev) return false;
    int kind = nostr_event_get_kind(ev);
    return kind == NOSTR_NIP99_KIND_LISTING ||
           kind == NOSTR_NIP99_KIND_DRAFT_LISTING;
}

int nostr_nip99_get_images(const NostrEvent *ev, NostrNip99Image *images,
                            size_t max, size_t *out_count) {
    if (!ev || !images || !out_count) return -EINVAL;
    *out_count = 0;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return 0;

    size_t n = nostr_tags_size(tags);
    size_t count = 0;

    for (size_t i = 0; i < n && count < max; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;
        const char *k = nostr_tag_get(t, 0);
        if (!k || strcmp(k, "image") != 0) continue;

        const char *v = nostr_tag_get(t, 1);
        if (!v) continue;

        images[count].url = v;
        images[count].dimensions =
            (nostr_tag_size(t) >= 3) ? nostr_tag_get(t, 2) : NULL;
        ++count;
    }

    *out_count = count;
    return 0;
}

size_t nostr_nip99_count_images(const NostrEvent *ev) {
    if (!ev) return 0;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return 0;

    size_t n = nostr_tags_size(tags);
    size_t count = 0;

    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;
        const char *k = nostr_tag_get(t, 0);
        if (k && strcmp(k, "image") == 0) ++count;
    }
    return count;
}

int nostr_nip99_get_categories(const NostrEvent *ev, const char **tags,
                                size_t max, size_t *out_count) {
    if (!ev || !tags || !out_count) return -EINVAL;
    *out_count = 0;

    NostrTags *etags = (NostrTags *)nostr_event_get_tags(ev);
    if (!etags) return 0;

    size_t n = nostr_tags_size(etags);
    size_t count = 0;

    for (size_t i = 0; i < n && count < max; ++i) {
        NostrTag *t = nostr_tags_get(etags, i);
        if (!t || nostr_tag_size(t) < 2) continue;
        const char *k = nostr_tag_get(t, 0);
        if (!k || strcmp(k, "t") != 0) continue;
        const char *v = nostr_tag_get(t, 1);
        if (v && *v != '\0')
            tags[count++] = v;
    }

    *out_count = count;
    return 0;
}

int nostr_nip99_create_listing(NostrEvent *ev, const NostrNip99Listing *listing) {
    if (!ev || !listing || !listing->identifier) return -EINVAL;

    nostr_event_set_kind(ev, NOSTR_NIP99_KIND_LISTING);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);

    /* Required: d tag */
    nostr_tags_append(tags, nostr_tag_new("d", listing->identifier, NULL));

    if (listing->title)
        nostr_tags_append(tags, nostr_tag_new("title", listing->title, NULL));

    if (listing->summary)
        nostr_tags_append(tags, nostr_tag_new("summary", listing->summary, NULL));

    if (listing->published_at > 0) {
        char ts_buf[24];
        snprintf(ts_buf, sizeof(ts_buf), "%lld", (long long)listing->published_at);
        nostr_tags_append(tags, nostr_tag_new("published_at", ts_buf, NULL));
    }

    if (listing->location)
        nostr_tags_append(tags, nostr_tag_new("location", listing->location, NULL));

    if (listing->price.amount && listing->price.currency) {
        if (listing->price.frequency)
            nostr_tags_append(tags, nostr_tag_new("price",
                listing->price.amount, listing->price.currency,
                listing->price.frequency, NULL));
        else
            nostr_tags_append(tags, nostr_tag_new("price",
                listing->price.amount, listing->price.currency, NULL));
    }

    return 0;
}

int nostr_nip99_add_image(NostrEvent *ev, const char *url,
                           const char *dimensions) {
    if (!ev || !url) return -EINVAL;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (dimensions)
        nostr_tags_append(tags, nostr_tag_new("image", url, dimensions, NULL));
    else
        nostr_tags_append(tags, nostr_tag_new("image", url, NULL));
    return 0;
}

int nostr_nip99_add_category(NostrEvent *ev, const char *category) {
    if (!ev || !category || *category == '\0') return -EINVAL;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("t", category, NULL));
    return 0;
}
