#include "nostr.h"
#include <string.h>
#include <stdlib.h>

Filter *create_filter() {
    Filter *filter = (Filter *)malloc(sizeof(Filter));
    if (!filter) return NULL;

    filter->IDs = NULL;
    filter->IDs_count = 0;
    filter->Kinds = NULL;
    filter->Kinds_count = 0;
    filter->Authors = NULL;
    filter->Authors_count = 0;
    filter->Tags = NULL;
    filter->Since = NULL;
    filter->Until = NULL;
    filter->Limit = 0;
    filter->Search = NULL;
    filter->LimitZero = false;

    return filter;
}

void free_filter(Filter *filter) {
	for (size_t i = 0; i < filter->IDs_count; i++) {
		free(filter->IDs[i]);
	}
	free(filter->IDs);
	free(filter->Kinds);
	for (size_t i = 0; i < filter->Authors_count; i++) {
		free(filter->Authors[i]);
	}
	free(filter->Authors);
	free(filter->Tags);
	free(filter->Since);
	free(filter->Until);
	free(filter->Search);
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
    //if (!filter || !event) return false;

    bool match = true;

    if (filter->IDs && filter->IDs_count > 0) {
        match = false;
        for (size_t i = 0; i < filter->IDs_count; i++) {
            if (strcmp(filter->IDs[i], event->id) == 0) {
                match = true;
                break;
            }
        }
        if (!match) return false;
    }

    if (filter->Kinds && filter->Kinds_count > 0) {
        match = false;
        for (size_t i = 0; i < filter->Kinds_count; i++) {
            if (filter->Kinds[i] == event->kind) {
                match = true;
                break;
            }
        }
        if (!match) return false;
    }

    if (filter->Authors && filter->Authors_count > 0) {
        match = false;
        for (size_t i = 0; i < filter->Authors_count; i++) {
            if (strcmp(filter->Authors[i], event->pubkey) == 0) {
                match = true;
                break;
            }
        }
        if (!match) return false;
    }

    if (filter->Since && event->created_at < *(filter->Since)) return false;
    if (filter->Until && event->created_at > *(filter->Until)) return false;

    return match;
}

bool filters_match(Filters *filters, NostrEvent *event) {
    //if (!filters || !event) return false;

    for (size_t i = 0; i < filters->count; i++) {
        if (filter_matches(&filters->filters[i], event)) {
            return true;
        }
    }

    return false;
}