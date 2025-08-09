#include "nostr-filter-wrap.h"
#include "filter.h"
#include "tag.h"
#include "nostr-tag.h"
#include "nostr.h"

size_t nostr_filter_ids_len(const NostrFilter *filter) {
    return filter ? string_array_size((StringArray *)&filter->ids) : 0;
}

const char *nostr_filter_ids_get(const NostrFilter *filter, size_t index) {
    if (!filter) return NULL;
    size_t n = string_array_size((StringArray *)&filter->ids);
    if (index >= n) return NULL;
    return string_array_get(&filter->ids, index);
}

size_t nostr_filter_kinds_len(const NostrFilter *filter) {
    return filter ? int_array_size((IntArray *)&filter->kinds) : 0;
}

int nostr_filter_kinds_get(const NostrFilter *filter, size_t index) {
    if (!filter) return 0;
    size_t n = int_array_size((IntArray *)&filter->kinds);
    if (index >= n) return 0;
    return int_array_get(&filter->kinds, index);
}

size_t nostr_filter_authors_len(const NostrFilter *filter) {
    return filter ? string_array_size((StringArray *)&filter->authors) : 0;
}

const char *nostr_filter_authors_get(const NostrFilter *filter, size_t index) {
    if (!filter) return NULL;
    size_t n = string_array_size((StringArray *)&filter->authors);
    if (index >= n) return NULL;
    return string_array_get(&filter->authors, index);
}

int64_t nostr_filter_get_since_i64(const NostrFilter *filter) {
    return filter ? (int64_t)filter->since : 0;
}

void nostr_filter_set_since_i64(NostrFilter *filter, int64_t since) {
    if (!filter) return;
    filter->since = (Timestamp)since;
}

int64_t nostr_filter_get_until_i64(const NostrFilter *filter) {
    return filter ? (int64_t)filter->until : 0;
}

void nostr_filter_set_until_i64(NostrFilter *filter, int64_t until) {
    if (!filter) return;
    filter->until = (Timestamp)until;
}

size_t nostr_filter_tags_len(const NostrFilter *filter) {
    return (filter && filter->tags) ? filter->tags->count : 0;
}

size_t nostr_filter_tag_len(const NostrFilter *filter, size_t tag_index) {
    if (!filter || !filter->tags) return 0;
    if (tag_index >= filter->tags->count) return 0;
    Tag *t = filter->tags->data[tag_index];
    return t ? string_array_size((StringArray *)t) : 0;
}

const char *nostr_filter_tag_get(const NostrFilter *filter, size_t tag_index, size_t item_index) {
    if (!filter || !filter->tags) return NULL;
    if (tag_index >= filter->tags->count) return NULL;
    Tag *t = filter->tags->data[tag_index];
    if (!t) return NULL;
    size_t n = string_array_size((StringArray *)t);
    if (item_index >= n) return NULL;
    return string_array_get((StringArray *)t, item_index);
}

void nostr_filter_add_id(NostrFilter *filter, const char *id) {
    if (!filter || !id) return;
    string_array_add(&filter->ids, id);
}

void nostr_filter_add_kind(NostrFilter *filter, int kind) {
    if (!filter) return;
    int_array_add(&filter->kinds, kind);
}

void nostr_filter_add_author(NostrFilter *filter, const char *author) {
    if (!filter || !author) return;
    string_array_add(&filter->authors, author);
}

void nostr_filter_tags_append(NostrFilter *filter, const char *key, const char *value, const char *relay) {
    if (!filter || !key) return;
    Tag *t = NULL;
    if (relay && *relay) {
        t = nostr_tag_new(key, value ? value : "", relay, NULL);
    } else {
        t = nostr_tag_new(key, value ? value : "", NULL);
    }
    if (!t) return;
    if (!filter->tags) filter->tags = nostr_tags_new(0);
    filter->tags = nostr_tags_append_unique(filter->tags, t);
}
