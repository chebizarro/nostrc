#ifndef FILTER_H
#define FILTER_H

#include <stdlib.h>
#include <stdint.h>
#include "timestamp.h"
#include "event.h"

typedef struct _NostrEvent NostrEvent;

typedef struct _Filter {
    char **IDs;
    size_t IDs_count;
    int *Kinds;
    size_t Kinds_count;
    char **Authors;
    size_t Authors_count;
    Tags *Tags;
    Timestamp *Since;
    Timestamp *Until;
    int Limit;
    char *Search;
    bool LimitZero;
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