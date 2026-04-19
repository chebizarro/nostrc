#include "nostr/nip92/nip92.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * Parse a single imeta tag into an entry.
 * Each tag element after "imeta" is "key value" separated by a space.
 * Returns true if the entry has at least a URL.
 */
static bool parse_imeta_tag(const NostrTag *tag, NostrNip92Entry *entry) {
    memset(entry, 0, sizeof(*entry));

    size_t n = nostr_tag_size(tag);
    /* imeta tag must have at least 3 elements: "imeta" + 2 fields */
    if (n < 3) return false;

    for (size_t i = 1; i < n; ++i) {
        const char *item = nostr_tag_get(tag, i);
        if (!item) continue;

        /* Find the space separator between key and value */
        const char *sp = strchr(item, ' ');
        if (!sp) continue;

        size_t key_len = (size_t)(sp - item);
        const char *val = sp + 1;

        if (key_len == 3 && memcmp(item, "url", 3) == 0) {
            entry->url = val;
        } else if (key_len == 8 && memcmp(item, "blurhash", 8) == 0) {
            entry->blurhash = val;
        } else if (key_len == 3 && memcmp(item, "alt", 3) == 0) {
            entry->alt = val;
        } else if (key_len == 3 && memcmp(item, "dim", 3) == 0) {
            /* Parse "WxH" */
            const char *x = strchr(val, 'x');
            if (!x) return false;  /* malformed dim → reject entire tag set */

            char *endptr = NULL;
            long w = strtol(val, &endptr, 10);
            if (endptr != x || w < 0) return false;

            long h = strtol(x + 1, &endptr, 10);
            if (*endptr != '\0' || h < 0) return false;

            entry->width = (int)w;
            entry->height = (int)h;
        }
    }

    return true;
}

int nostr_nip92_parse(const NostrEvent *ev, NostrNip92Entry *entries,
                       size_t max_entries, size_t *out_count) {
    if (!ev || !entries || !out_count) return -EINVAL;
    *out_count = 0;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return 0;

    size_t n = nostr_tags_size(tags);
    size_t count = 0;

    for (size_t i = 0; i < n && count < max_entries; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 3) continue;

        const char *k = nostr_tag_get(t, 0);
        if (!k || strcmp(k, "imeta") != 0) continue;

        NostrNip92Entry entry;
        if (parse_imeta_tag(t, &entry)) {
            entries[count++] = entry;
        } else {
            /* Go implementation returns nil on any parse error */
            *out_count = 0;
            return 0;
        }
    }

    *out_count = count;
    return 0;
}

int nostr_nip92_find_url(const NostrEvent *ev, const char *url,
                          NostrNip92Entry *out) {
    if (!ev || !url || !out) return -EINVAL;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return -ENOENT;

    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 3) continue;

        const char *k = nostr_tag_get(t, 0);
        if (!k || strcmp(k, "imeta") != 0) continue;

        NostrNip92Entry entry;
        if (!parse_imeta_tag(t, &entry)) continue;

        if (entry.url && strcmp(entry.url, url) == 0) {
            *out = entry;
            return 0;
        }
    }
    return -ENOENT;
}

size_t nostr_nip92_count(const NostrEvent *ev) {
    if (!ev) return 0;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return 0;

    size_t n = nostr_tags_size(tags);
    size_t count = 0;

    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 3) continue;

        const char *k = nostr_tag_get(t, 0);
        if (!k || strcmp(k, "imeta") != 0) continue;

        NostrNip92Entry entry;
        if (parse_imeta_tag(t, &entry))
            ++count;
    }
    return count;
}

NostrTag *nostr_nip92_build_tag(const NostrNip92Entry *entry) {
    if (!entry || !entry->url) return NULL;

    NostrTag *tag = nostr_tag_new("imeta", NULL);
    if (!tag) return NULL;

    /* url */
    char *url_field = malloc(4 + strlen(entry->url) + 1);
    if (!url_field) { nostr_tag_free(tag); return NULL; }
    sprintf(url_field, "url %s", entry->url);
    nostr_tag_append(tag, url_field);
    free(url_field);

    /* blurhash */
    if (entry->blurhash) {
        char *bh_field = malloc(9 + strlen(entry->blurhash) + 1);
        if (!bh_field) { nostr_tag_free(tag); return NULL; }
        sprintf(bh_field, "blurhash %s", entry->blurhash);
        nostr_tag_append(tag, bh_field);
        free(bh_field);
    }

    /* dim */
    if (entry->width > 0 && entry->height > 0) {
        char dim_buf[64];
        snprintf(dim_buf, sizeof(dim_buf), "dim %dx%d", entry->width, entry->height);
        nostr_tag_append(tag, dim_buf);
    }

    /* alt */
    if (entry->alt) {
        char *alt_field = malloc(4 + strlen(entry->alt) + 1);
        if (!alt_field) { nostr_tag_free(tag); return NULL; }
        sprintf(alt_field, "alt %s", entry->alt);
        nostr_tag_append(tag, alt_field);
        free(alt_field);
    }

    return tag;
}

int nostr_nip92_add(NostrEvent *ev, const NostrNip92Entry *entry) {
    if (!ev || !entry) return -EINVAL;

    NostrTag *tag = nostr_nip92_build_tag(entry);
    if (!tag) return -EINVAL;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, tag);
    return 0;
}
