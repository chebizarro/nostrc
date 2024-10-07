#include "nostr.h"
#include <string.h>
#include <stdlib.h>

Filter *create_filter() {
    Filter *filter = (Filter *)malloc(sizeof(Filter));
    if (!filter) return NULL;

    string_array_init(&filter->ids);
    int_array_init(&filter->kinds);
    string_array_init(&filter->authors);
    filter->tags = create_tags(0);
    filter->since = 0;
    filter->until = 0;
    filter->limit = 0;
    filter->search = NULL;
    filter->limit_zero = false;

    return filter;
}

void free_filter(Filter *filter) {
	string_array_free(&filter->ids);
	int_array_free(&filter->kinds);
	string_array_free(&filter->authors);
	free_tags(filter->tags);
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
    if (filter->since && event->created_at < (filter->since)) return false;
    if (filter->until && event->created_at > (filter->until)) return false;
    return match;
}

bool filter_match_ignoring_timestamp(Filter *filter, NostrEvent *event) {
    if (!filter || !event) return false;

    bool match = true;

    if (string_array_size(&filter->ids) > 0) {
		if (!string_array_contains(&filter->ids, event->id)) return false;
    }

    if (int_array_size(&filter->kinds) > 0) {
		if (!int_array_contains(&filter->kinds, event->kind)) return false;
    }

    if (string_array_size(&filter->authors) > 0) {
		if (!string_array_contains(&filter->authors, event->pubkey)) return false;
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