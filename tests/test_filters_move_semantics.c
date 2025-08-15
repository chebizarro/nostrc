#include "nostr-filter.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    /* Create destination vector */
    NostrFilters *fs = nostr_filters_new();
    assert(fs);

    /* Create a heap filter and populate */
    NostrFilter *f = nostr_filter_new();
    assert(f);
    nostr_filter_add_id(f, "deadbeef");
    nostr_filter_add_kind(f, 1);
    nostr_filter_add_author(f, "npub1...");

    /* Move into fs; source should be zeroed */
    bool ok = nostr_filters_add(fs, f);
    assert(ok);

    /* Verify source zeroed to prevent double-free */
    NostrFilter zero = (NostrFilter){0};
    assert(memcmp(f, &zero, sizeof(NostrFilter)) == 0);

    /* Free the emptied shell safely */
    nostr_filter_free(f);

    /* Verify destination received contents */
    assert(fs->count == 1);
    NostrFilter *dst = &fs->filters[0];
    assert(nostr_filter_ids_len(dst) == 1);
    assert(nostr_filter_kinds_len(dst) == 1);
    assert(nostr_filter_authors_len(dst) == 1);

    /* Cleanup */
    nostr_filters_free(fs);

    printf("test_filters_move_semantics OK\n");
    return 0;
}
