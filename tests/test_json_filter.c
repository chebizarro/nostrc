#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "json.h"
#include "filter.h"
#include "nostr_jansson.h"

static void fill_filter(Filter *f) {
    // assumes f was created by create_filter()
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
    Tag *t1 = new_string_array(0);
    string_array_add(t1, "e");
    string_array_add(t1, "x");
    Tag *t2 = new_string_array(0);
    string_array_add(t2, "p");
    string_array_add(t2, "y");
    Tags *tmp = tags_append_unique(f->tags, t1);
    if (tmp) f->tags = tmp;
    tmp = tags_append_unique(f->tags, t2);
    if (tmp) f->tags = tmp;
}

static void assert_filter_eq(Filter *a, Filter *b) {
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
    Filter *f = create_filter();
    assert(f);
    fill_filter(f);

    char *s = nostr_filter_serialize(f);
    assert(s);
    // Non-standard fields must not appear
    assert(strstr(s, "limit_zero") == NULL);

    Filter *g = create_filter();
    assert(g);
    int rc = nostr_filter_deserialize(g, s);
    assert(rc == 0);

    assert_filter_eq(f, g);

    free(s);
    free_filter(g);
    free_filter(f);
}

static void test_filter_minimal_absent_fields(void) {
    nostr_set_json_interface(jansson_impl);
    Filter *f = create_filter();
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

    Filter *g = create_filter();
    assert(g);
    int rc = nostr_filter_deserialize(g, s);
    assert(rc == 0);
    assert(int_array_size(&g->kinds) == 1);
    assert(int_array_get(&g->kinds, 0) == 42);

    free(s);
    free_filter(g);
    free_filter(f);
}

int main(void) {
    nostr_json_init();
    test_filter_roundtrip_full();
    test_filter_minimal_absent_fields();
    nostr_json_cleanup();
    printf("test_json_filter OK\n");
    return 0;
}
