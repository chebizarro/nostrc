#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "json.h"
#include "nostr-filter.h"
#include "nostr-tag.h"
#include "nostr_jansson.h"

static void fill_filter(NostrFilter *f) {
    // assumes f was created by nostr_filter_new()
    f->since = 123;
    f->until = 456;
    f->limit = 10;
    f->search = strdup("query");
    f->limit_zero = true; // should not serialize

    string_array_add(&f->ids, "id1");
    string_array_add(&f->ids, "id2");
    int_array_add(&f->kinds, 1);
    int_array_add(&f->kinds, 2);
    string_array_add(&f->authors, "a1");
    string_array_add(&f->authors, "a2");

    // tags: [["e","x"],["p","y"]]
    NostrTag *t1 = nostr_tag_new("e", "x", NULL);
    NostrTag *t2 = nostr_tag_new("p", "y", NULL);
    NostrTags *tmp = nostr_tags_append_unique(f->tags, t1);
    if (tmp) f->tags = tmp;
    tmp = nostr_tags_append_unique(f->tags, t2);
    if (tmp) f->tags = tmp;
}

static void assert_filter_eq(NostrFilter *a, NostrFilter *b) {
    assert(int_array_size(&a->kinds) == int_array_size(&b->kinds));
    assert(string_array_size(&a->ids) == string_array_size(&b->ids));
    assert(string_array_size(&a->authors) == string_array_size(&b->authors));
    assert(a->since == b->since);
    assert(a->until == b->until);
    assert(a->limit == b->limit);
    assert((a->search && b->search && strcmp(a->search,b->search)==0) || (!a->search && !b->search));
    assert((a->tags==NULL && b->tags==NULL) || (a->tags && b->tags));
}

static void test_filter_roundtrip_full(void) {
    nostr_set_json_interface(jansson_impl);
    NostrFilter *f = nostr_filter_new();
    assert(f);
    fill_filter(f);

    char *s = nostr_filter_serialize(f);
    assert(s);
    // Non-standard fields must not appear
    assert(strstr(s, "limit_zero") == NULL);

    NostrFilter *g = nostr_filter_new();
    assert(g);
    int rc = nostr_filter_deserialize(g, s);
    assert(rc == 0);

    assert_filter_eq(f, g);

    free(s);
    nostr_filter_free(g);
    nostr_filter_free(f);
}

static void test_filter_minimal_absent_fields(void) {
    nostr_set_json_interface(jansson_impl);
    NostrFilter *f = nostr_filter_new();
    assert(f);
    // only one field to ensure others are omitted
    int_array_add(&f->kinds, 42);

    char *s = nostr_filter_serialize(f);
    assert(s);
    // Ensure absent keys aren't serialized
    assert(strstr(s, "ids") == NULL);
    assert(strstr(s, "authors") == NULL);
    assert(strstr(s, "tags") == NULL);
    assert(strstr(s, "since") == NULL);
    assert(strstr(s, "until") == NULL);
    assert(strstr(s, "limit") == NULL);
    assert(strstr(s, "search") == NULL);

    NostrFilter *g = nostr_filter_new();
    assert(g);
    int rc = nostr_filter_deserialize(g, s);
    assert(rc == 0);
    assert(int_array_size(&g->kinds) == 1);
    assert(int_array_get(&g->kinds, 0) == 42);

    free(s);
    nostr_filter_free(g);
    nostr_filter_free(f);
}

/* nostrc-pnc7: "limit":0 must set limit_zero and serialize back, on both
 * the compact fast path and the jansson backend (force_fallback). */
static void test_filter_limit_zero_both_paths(void) {
    nostr_set_json_interface(jansson_impl);
    for (int fallback = 0; fallback <= 1; fallback++) {
        nostr_json_force_fallback(fallback != 0);

        NostrFilter *z = nostr_filter_new();
        assert(nostr_filter_deserialize(z, "{\"kinds\":[1],\"limit\":0}") == 0);
        if (!(z->limit == 0 && z->limit_zero)) {
            fprintf(stderr, "limit:0 lost (fallback=%d)\n", fallback);
            abort();
        }
        char *s = nostr_filter_serialize(z);
        if (!s || !strstr(s, "\"limit\":0")) {
            fprintf(stderr, "limit:0 not serialized (fallback=%d): %s\n",
                    fallback, s ? s : "(null)");
            abort();
        }
        free(s);
        nostr_filter_free(z);

        NostrFilter *a = nostr_filter_new();
        assert(nostr_filter_deserialize(a, "{\"kinds\":[1]}") == 0);
        if (a->limit_zero) { fprintf(stderr, "absent limit set limit_zero\n"); abort(); }
        nostr_filter_free(a);
    }
    nostr_json_force_fallback(false);
}

int main(void) {
    nostr_json_init();
    test_filter_roundtrip_full();
    test_filter_minimal_absent_fields();
    test_filter_limit_zero_both_paths();
    nostr_json_cleanup();
    printf("test_json_filter OK\n");
    return 0;
}
