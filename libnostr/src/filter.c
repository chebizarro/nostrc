#include "nostr.h"
#include <string.h>
#include <stdlib.h>

Filter *create_filter() {
    Filter *filter = (Filter *)malloc(sizeof(Filter));
    if (!filter) return NULL;

    filter->ids = NULL;
    filter->ids_count = 0;
    filter->kinds = NULL;
    filter->kinds_count = 0;
    filter->authors = NULL;
    filter->authors_count = 0;
    filter->tags = NULL;
    filter->since = NULL;
    filter->until = NULL;
    filter->limit = 0;
    filter->search = NULL;
    filter->limit_zero = false;

    return filter;
}

void free_filter(Filter *filter) {
	for (size_t i = 0; i < filter->ids_count; i++) {
		free(filter->ids[i]);
	}
	free(filter->ids);
	free(filter->kinds);
	for (size_t i = 0; i < filter->authors_count; i++) {
		free(filter->authors[i]);
	}
	free(filter->authors);
	free(filter->tags);
	free(filter->since);
	free(filter->until);
	free(filter->search);
	free(filter);
}

Filters *create_filters(size_t count) {
    Filters *filters = (Filters *)malloc(sizeof(Filters));
    if (!filters) return NULL;

    filters->filters = (Filter *)malloc(count * sizeof(Filter));
    if (!filters->filters) {
        free(filters);
        return NULL;
    }

    filters->count = count;
    for (size_t i = 0; i < count; i++) {
        filters->filters[i] = *create_filter();
    }

    return filters;
}

void free_filters(Filters *filters) {
	for (size_t i = 0; i < filters->count; i++) {
		free_filter(&filters->filters[i]);
	}
	free(filters->filters);
	free(filters);
}

bool filter_matches(Filter *filter, NostrEvent *event) {
    if (!filter || !event) return false;

    bool match = true;
	if (!filter_match_ignoring_timestamp(filter, event)) return false;
    if (filter->since && event->created_at < *(filter->since)) return false;
    if (filter->until && event->created_at > *(filter->until)) return false;
    return match;
}

bool filter_match_ignoring_timestamp(Filter *filter, NostrEvent *event) {
    if (!filter || !event) return false;

    bool match = true;

    if (filter->ids && filter->ids_count > 0) {
        match = false;
        for (size_t i = 0; i < filter->ids_count; i++) {
            if (strcmp(filter->ids[i], event->id) == 0) {
                match = true;
                break;
            }
        }
        if (!match) return false;
    }

    if (filter->kinds && filter->kinds_count > 0) {
        match = false;
        for (size_t i = 0; i < filter->kinds_count; i++) {
            if (filter->kinds[i] == event->kind) {
                match = true;
                break;
            }
        }
        if (!match) return false;
    }

    if (filter->authors && filter->authors_count > 0) {
        match = false;
        for (size_t i = 0; i < filter->authors_count; i++) {
            if (strcmp(filter->authors[i], event->pubkey) == 0) {
                match = true;
                break;
            }
        }
        if (!match) return false;
    }

	if (filter->tags && filter->tags->count > 0) {
		// TODO implement
	}

	return match;

}

bool filters_match(Filters *filters, NostrEvent *event) {
    if (!filters || !event) return false;

    for (size_t i = 0; i < filters->count; i++) {
        if (filter_matches(&filters->filters[i], event)) {
            return true;
        }
    }

    return false;
}