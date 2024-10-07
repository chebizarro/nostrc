#ifndef FILTER_H
#define FILTER_H

#include <stdlib.h>
#include <stdint.h>
#include "timestamp.h"
#include "event.h"
#include "go.h"

typedef struct _NostrEvent NostrEvent;

typedef struct _Filter {
    StringArray ids;
    IntArray kinds;
    StringArray authors;
    Tags *tags;
    Timestamp since;
    Timestamp until;
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