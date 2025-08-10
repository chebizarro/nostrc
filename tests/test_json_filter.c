#include <assert.h>
#include <stdio.h>
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

int main(void) {
    nostr_json_init();
    test_filter_roundtrip_full();
    test_filter_minimal_absent_fields();
    nostr_json_cleanup();
    printf("test_json_filter OK\n");
    return 0;
}
