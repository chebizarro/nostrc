#include "nostr.h"
#include <stdlib.h>
#include <string.h>
#include "filter.h"
#include "nostr-tag.h"

#define INITIAL_CAPACITY 4  // Initial capacity for Filters array

Filter *nostr_filter_new(void) {
    Filter *filter = (Filter *)malloc(sizeof(Filter));
    if (!filter)
        return NULL;

    string_array_init(&filter->ids);
    int_array_init(&filter->kinds);
    string_array_init(&filter->authors);
    filter->tags = nostr_tags_new(0);
    filter->since = 0;
    filter->until = 0;
    filter->limit = 0;
    filter->search = NULL;
    filter->limit_zero = false;

    return filter;
}

/* --- Legacy symbol compatibility (remove after full migration) --- */
Filter *create_filter(void) {
    return nostr_filter_new();
}

bool filter_matches(Filter *filter, NostrEvent *event) {
    return nostr_filter_matches(filter, event);
}

bool filter_match_ignoring_timestamp(Filter *filter, NostrEvent *event) {
    return nostr_filter_match_ignoring_timestamp(filter, event);
}

bool filters_match(Filters *filters, NostrEvent *event) {
    return nostr_filters_match(filters, event);
}

bool filters_match_ignoring_timestamp(Filters *filters, NostrEvent *event) {
    return nostr_filters_match_ignoring_timestamp(filters, event);
}

void free_filters(Filters *filters) {
    nostr_filters_free(filters);
}

Filters *create_filters(void) {
    return nostr_filters_new();
}

bool filters_add(Filters *filters, Filter *filter) {
    return nostr_filters_add(filters, filter);
}

static void free_filter_contents(Filter *filter) {
    string_array_free(&filter->ids);
    int_array_free(&filter->kinds);
    string_array_free(&filter->authors);
    if (filter->tags) {
        nostr_tags_free(filter->tags);
        filter->tags = NULL;
    }
    free(filter->search);
    filter->search = NULL;
}

void free_filter(Filter *filter) {
    if (!filter) return;
    free_filter_contents(filter);
    free(filter);
}

/* New API free wrapper (needed for GBoxed) */
void nostr_filter_free(NostrFilter *filter) {
    free_filter(filter);
}

/* Deep-copy helpers for tags */
static Tag *filter_tag_clone(const Tag *src) {
    if (!src) return NULL;
    size_t n = string_array_size((StringArray *)src);
    StringArray *dst = new_string_array((int)n);
    for (size_t i = 0; i < n; i++) {
        const char *s = string_array_get((const StringArray *)src, i);
        if (s) string_array_add(dst, s);
    }
    return (Tag *)dst;
}

static Tags *filter_tags_clone(const Tags *src) {
    if (!src) return NULL;
    Tags *dst = (Tags *)malloc(sizeof(Tags));
    if (!dst) return NULL;
    dst->count = src->count;
    dst->data = (Tag **)calloc(dst->count, sizeof(Tag *));
    if (!dst->data) { free(dst); return NULL; }
    for (size_t i = 0; i < dst->count; i++) {
        dst->data[i] = filter_tag_clone(src->data[i]);
    }
    return dst;
}

NostrFilter *nostr_filter_copy(const NostrFilter *src) {
    if (!src) return NULL;
    NostrFilter *f = nostr_filter_new();
    if (!f) return NULL;
    /* ids */
    for (size_t i = 0, n = string_array_size((StringArray *)&src->ids); i < n; i++) {
        const char *s = string_array_get(&src->ids, i);
        if (s) string_array_add(&f->ids, s);
    }
    /* kinds */
    for (size_t i = 0, n = int_array_size((IntArray *)&src->kinds); i < n; i++) {
        int_array_add(&f->kinds, int_array_get(&src->kinds, i));
    }
    /* authors */
    for (size_t i = 0, n = string_array_size((StringArray *)&src->authors); i < n; i++) {
        const char *s = string_array_get(&src->authors, i);
        if (s) string_array_add(&f->authors, s);
    }
    /* tags */
    if (f->tags) { nostr_tags_free(f->tags); f->tags = NULL; }
    f->tags = filter_tags_clone(src->tags);
    /* scalars */
    f->since = src->since;
    f->until = src->until;
    f->limit = src->limit;
    f->limit_zero = src->limit_zero;
    /* search */
    if (src->search) f->search = strdup(src->search);
    return f;
}

Filters *nostr_filters_new(void) {
    Filters *filters = (Filters *)malloc(sizeof(Filters));
    if (!filters)
        return NULL;

    filters->count = 0;
    filters->capacity = INITIAL_CAPACITY;
    filters->filters = (Filter *)malloc(filters->capacity * sizeof(Filter));
    if (!filters->filters) {
        free(filters);
        return NULL;
    }

    return filters;
}

// Resizes the internal array when needed
static bool filters_resize(Filters *filters) {
    size_t new_capacity = filters->capacity * 2;
    Filter *new_filters = (Filter *)realloc(filters->filters, new_capacity * sizeof(Filter));
    if (!new_filters)
        return false;

    filters->filters = new_filters;
    filters->capacity = new_capacity;
    return true;
}

bool nostr_filters_add(Filters *filters, Filter *filter) {
    if (!filters || !filter)
        return false;

    // Resize the array if necessary
    if (filters->count == filters->capacity) {
        if (!filters_resize(filters)) {
            return false;
        }
    }

    filters->filters[filters->count] = *filter; // Copy the filter into the array
    filters->count++;
    return true;
}


void nostr_filters_free(Filters *filters) {
    for (size_t i = 0; i < filters->count; i++) {
        free_filter_contents(&filters->filters[i]);
    }
    free(filters->filters);
    free(filters);
}

bool nostr_filter_matches(Filter *filter, NostrEvent *event) {
    if (!filter || !event)
        return false;

    bool match = true;
    if (!filter_match_ignoring_timestamp(filter, event))
        return false;
    if (filter->since && event->created_at < (filter->since))
        return false;
    if (filter->until && event->created_at > (filter->until))
        return false;
    return match;
}

bool nostr_filter_match_ignoring_timestamp(Filter *filter, NostrEvent *event) {
    if (!filter || !event)
        return false;

    bool match = true;

    if (string_array_size(&filter->ids) > 0) {
        if (!string_array_contains(&filter->ids, event->id))
            return false;
    }

    if (int_array_size(&filter->kinds) > 0) {
        if (!int_array_contains(&filter->kinds, event->kind))
            return false;
    }

    if (string_array_size(&filter->authors) > 0) {
        if (!string_array_contains(&filter->authors, event->pubkey))
            return false;
    }

    if (filter->tags && filter->tags->count > 0) {
        // TODO implement
    }

    return match;
}

bool nostr_filters_match(Filters *filters, NostrEvent *event) {
    if (!filters || !event)
        return false;

    for (size_t i = 0; i < filters->count; i++) {
        if (filter_matches(&filters->filters[i], event)) {
            return true;
        }
    }

    return false;
}

bool nostr_filters_match_ignoring_timestamp(Filters *filters, NostrEvent *event) {
    if (!filters || !event)
        return false;

    for (size_t i = 0; i < filters->count; i++) {
        if (filter_match_ignoring_timestamp(&filters->filters[i], event)) {
            return true;
        }
    }

    return false;
}

/* Getters/Setters for Filter fields (public API via nostr-filter.h) */

const StringArray *nostr_filter_get_ids(const NostrFilter *filter) {
    return filter ? &filter->ids : NULL;
}

void nostr_filter_set_ids(NostrFilter *filter, const char *const *ids, size_t count) {
    if (!filter) return;
    string_array_free(&filter->ids);
    string_array_init(&filter->ids);
    if (!ids) return;
    for (size_t i = 0; i < count; i++) {
        if (ids[i]) string_array_add(&filter->ids, ids[i]);
    }
}

const IntArray *nostr_filter_get_kinds(const NostrFilter *filter) {
    return filter ? &filter->kinds : NULL;
}

void nostr_filter_set_kinds(NostrFilter *filter, const int *kinds, size_t count) {
    if (!filter) return;
    int_array_free(&filter->kinds);
    int_array_init(&filter->kinds);
    if (!kinds) return;
    for (size_t i = 0; i < count; i++) {
        int_array_add(&filter->kinds, kinds[i]);
    }
}

const StringArray *nostr_filter_get_authors(const NostrFilter *filter) {
    return filter ? &filter->authors : NULL;
}

void nostr_filter_set_authors(NostrFilter *filter, const char *const *authors, size_t count) {
    if (!filter) return;
    string_array_free(&filter->authors);
    string_array_init(&filter->authors);
    if (!authors) return;
    for (size_t i = 0; i < count; i++) {
        if (authors[i]) string_array_add(&filter->authors, authors[i]);
    }
}

NostrTags *nostr_filter_get_tags(const NostrFilter *filter) {
    return filter ? filter->tags : NULL;
}

void nostr_filter_set_tags(NostrFilter *filter, NostrTags *tags) {
    if (!filter) return;
    if (filter->tags && filter->tags != tags) {
        nostr_tags_free(filter->tags);
    }
    filter->tags = tags; /* takes ownership */
}

Timestamp nostr_filter_get_since(const NostrFilter *filter) {
    return filter ? filter->since : 0;
}

void nostr_filter_set_since(NostrFilter *filter, Timestamp since) {
    if (!filter) return;
    filter->since = since;
}

Timestamp nostr_filter_get_until(const NostrFilter *filter) {
    return filter ? filter->until : 0;
}

void nostr_filter_set_until(NostrFilter *filter, Timestamp until) {
    if (!filter) return;
    filter->until = until;
}

int nostr_filter_get_limit(const NostrFilter *filter) {
    return filter ? filter->limit : 0;
}

void nostr_filter_set_limit(NostrFilter *filter, int limit) {
    if (!filter) return;
    filter->limit = limit;
}

const char *nostr_filter_get_search(const NostrFilter *filter) {
    return filter ? filter->search : NULL;
}

void nostr_filter_set_search(NostrFilter *filter, const char *search) {
    if (!filter) return;
    if (filter->search) {
        free(filter->search);
        filter->search = NULL;
    }
    if (search) {
        filter->search = strdup(search);
    }
}

/* limit_zero accessors are defined once below */

bool nostr_filter_get_limit_zero(const NostrFilter *filter) {
    return filter ? filter->limit_zero : false;
}

void nostr_filter_set_limit_zero(NostrFilter *filter, bool limit_zero) {
    if (!filter) return;
    filter->limit_zero = limit_zero;
}
