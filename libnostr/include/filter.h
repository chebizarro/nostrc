#ifndef FILTER_H
#define FILTER_H

#include <stdlib.h>
#include <stdint.h>
#include "timestamp.h"
#include "event.h"

typedef struct _NostrEvent NostrEvent;

typedef struct _Filter {
    char **ids;
    size_t ids_count;
    int *kinds;
    size_t kinds_count;
    char **authors;
    size_t authors_count;
    Tags *tags;
    Timestamp *since;
    Timestamp *until;
    int limit;
    char *search;
    bool limit_zero;
} Filter;

typedef struct _Filters {
    Filter *filters;
    size_t count;
} Filters;

Filter *create_filter();
void free_filter(Filter *filter);
Filters *create_filters(size_t count);
void free_filters(Filters *filters);
bool filter_matches(Filter *filter, NostrEvent *event);
bool filters_match(Filters *filters, NostrEvent *event);

#endif // FILTER_H