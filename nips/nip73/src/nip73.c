#include "nostr/nip73/nip73.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

NostrNip73Type nostr_nip73_detect_type(const char *value) {
    if (!value) return NOSTR_NIP73_UNKNOWN;

    /* Check for URL types first */
    if (strncmp(value, "http://", 7) == 0 || strncmp(value, "https://", 8) == 0)
        return NOSTR_NIP73_URL;

    /* Find the first colon to get the type prefix */
    const char *colon = strchr(value, ':');
    if (!colon) return NOSTR_NIP73_UNKNOWN;

    size_t prefix_len = (size_t)(colon - value);

    if (prefix_len == 4 && memcmp(value, "isbn", 4) == 0) return NOSTR_NIP73_ISBN;
    if (prefix_len == 3 && memcmp(value, "doi", 3) == 0)  return NOSTR_NIP73_DOI;
    if (prefix_len == 4 && memcmp(value, "imdb", 4) == 0)  return NOSTR_NIP73_IMDB;
    if (prefix_len == 4 && memcmp(value, "tmdb", 4) == 0)  return NOSTR_NIP73_TMDB;
    if (prefix_len == 7 && memcmp(value, "spotify", 7) == 0) return NOSTR_NIP73_SPOTIFY;
    if (prefix_len == 7 && memcmp(value, "youtube", 7) == 0) return NOSTR_NIP73_YOUTUBE;
    if (prefix_len == 7 && memcmp(value, "podcast", 7) == 0) return NOSTR_NIP73_PODCAST_GUID;

    return NOSTR_NIP73_UNKNOWN;
}

NostrNip73Type nostr_nip73_type_from_string(const char *type_str) {
    if (!type_str) return NOSTR_NIP73_UNKNOWN;

    if (strcmp(type_str, "isbn") == 0)    return NOSTR_NIP73_ISBN;
    if (strcmp(type_str, "doi") == 0)     return NOSTR_NIP73_DOI;
    if (strcmp(type_str, "imdb") == 0)    return NOSTR_NIP73_IMDB;
    if (strcmp(type_str, "tmdb") == 0)    return NOSTR_NIP73_TMDB;
    if (strcmp(type_str, "spotify") == 0) return NOSTR_NIP73_SPOTIFY;
    if (strcmp(type_str, "youtube") == 0) return NOSTR_NIP73_YOUTUBE;
    if (strcmp(type_str, "podcast") == 0) return NOSTR_NIP73_PODCAST_GUID;
    if (strcmp(type_str, "http") == 0 || strcmp(type_str, "https") == 0)
        return NOSTR_NIP73_URL;

    return NOSTR_NIP73_UNKNOWN;
}

const char *nostr_nip73_type_to_string(NostrNip73Type type) {
    switch (type) {
        case NOSTR_NIP73_URL:          return "url";
        case NOSTR_NIP73_ISBN:         return "isbn";
        case NOSTR_NIP73_DOI:          return "doi";
        case NOSTR_NIP73_IMDB:         return "imdb";
        case NOSTR_NIP73_TMDB:         return "tmdb";
        case NOSTR_NIP73_SPOTIFY:      return "spotify";
        case NOSTR_NIP73_YOUTUBE:      return "youtube";
        case NOSTR_NIP73_PODCAST_GUID: return "podcast";
        default:                       return "unknown";
    }
}

bool nostr_nip73_is_media_type(NostrNip73Type type) {
    return type == NOSTR_NIP73_SPOTIFY ||
           type == NOSTR_NIP73_YOUTUBE ||
           type == NOSTR_NIP73_PODCAST_GUID;
}

bool nostr_nip73_is_reference_type(NostrNip73Type type) {
    return type == NOSTR_NIP73_ISBN ||
           type == NOSTR_NIP73_DOI;
}

/*
 * Parse a single "i" tag value into an entry.
 */
static void parse_entry(const char *value, NostrNip73Entry *entry) {
    entry->value = value;
    entry->type = nostr_nip73_detect_type(value);

    /* For URL type, identifier is the whole value */
    if (entry->type == NOSTR_NIP73_URL) {
        entry->identifier = value;
        return;
    }

    /* For typed entries, identifier is past "type:" */
    const char *colon = strchr(value, ':');
    entry->identifier = colon ? colon + 1 : value;
}

int nostr_nip73_parse(const NostrEvent *ev, NostrNip73Entry *entries,
                       size_t max_entries, size_t *out_count) {
    if (!ev || !entries || !out_count) return -EINVAL;
    *out_count = 0;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return 0;

    size_t n = nostr_tags_size(tags);
    size_t count = 0;

    for (size_t i = 0; i < n && count < max_entries; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;

        const char *k = nostr_tag_get(t, 0);
        if (!k || strcmp(k, "i") != 0) continue;

        const char *v = nostr_tag_get(t, 1);
        if (!v || *v == '\0') continue;

        parse_entry(v, &entries[count]);
        ++count;
    }

    *out_count = count;
    return 0;
}

size_t nostr_nip73_count(const NostrEvent *ev) {
    if (!ev) return 0;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return 0;

    size_t n = nostr_tags_size(tags);
    size_t count = 0;

    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;

        const char *k = nostr_tag_get(t, 0);
        if (k && strcmp(k, "i") == 0) ++count;
    }
    return count;
}

NostrTag *nostr_nip73_build_tag(const char *type_prefix, const char *identifier) {
    if (!type_prefix || !identifier) return NULL;

    /* Build "type:identifier" value */
    size_t plen = strlen(type_prefix);
    size_t ilen = strlen(identifier);
    char *value = malloc(plen + 1 + ilen + 1);
    if (!value) return NULL;

    memcpy(value, type_prefix, plen);
    value[plen] = ':';
    memcpy(value + plen + 1, identifier, ilen + 1);

    NostrTag *tag = nostr_tag_new("i", value, NULL);
    free(value);
    return tag;
}

NostrTag *nostr_nip73_build_url_tag(const char *url) {
    if (!url) return NULL;
    return nostr_tag_new("i", url, NULL);
}

int nostr_nip73_add(NostrEvent *ev, const char *type_prefix,
                     const char *identifier) {
    if (!ev || !type_prefix || !identifier) return -EINVAL;

    NostrTag *tag = nostr_nip73_build_tag(type_prefix, identifier);
    if (!tag) return -ENOMEM;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, tag);
    return 0;
}
