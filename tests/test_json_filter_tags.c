#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "json.h"
#include "filter.h"
#include "nostr_jansson.h"

static int tags_count_value(const Tags *tags, const char *name, const char *value) {
    if (!tags) return 0;
    int c = 0;
    for (size_t i = 0; i < tags->count; i++) {
        Tag *t = tags->data[i];
        if (t && t->size >= 2 && t->data[0] && t->data[1]) {
            if (strcmp(t->data[0], name) == 0 && strcmp(t->data[1], value) == 0) c++;
        }
    }
    return c;
}

static void test_tags_serialize_to_hash_keys(void) {
    nostr_set_json_interface(jansson_impl);
    Filter *f = create_filter();
    assert(f);

    // tags: [ ["e","x1"], ["e","x2"], ["p","y"] ]
    Tag *t1 = new_string_array(0); string_array_add(t1, "e"); string_array_add(t1, "x1");
    Tag *t2 = new_string_array(0); string_array_add(t2, "e"); string_array_add(t2, "x2");
    Tag *t3 = new_string_array(0); string_array_add(t3, "p"); string_array_add(t3, "y");
    Tags *tmp = tags_append_unique(f->tags, t1); if (tmp) f->tags = tmp;
    tmp = tags_append_unique(f->tags, t2); if (tmp) f->tags = tmp;
    tmp = tags_append_unique(f->tags, t3); if (tmp) f->tags = tmp;

    char *s = nostr_filter_serialize(f);
    assert(s);
    // Should contain dynamic keys and not the legacy "tags"
    assert(strstr(s, "\"#e\"") != NULL);
    assert(strstr(s, "\"#p\"") != NULL);
    assert(strstr(s, "\"tags\"") == NULL);

    // Values included
    assert(strstr(s, "\"x1\"") != NULL);
    assert(strstr(s, "\"x2\"") != NULL);
    assert(strstr(s, "\"y\"") != NULL);

    free(s);
    free_filter(f);
}

static void test_tags_roundtrip_from_hash_keys(void) {
    nostr_set_json_interface(jansson_impl);
    const char *js = "{\"#e\":[\"x1\",\"x2\"],\"#p\":[\"y\"]}";
    Filter *f = create_filter();
    assert(f);
    assert(nostr_filter_deserialize(f, js) == 0);

    assert(tags_count_value(f->tags, "e", "x1") == 1);
    assert(tags_count_value(f->tags, "e", "x2") == 1);
    assert(tags_count_value(f->tags, "p", "y") == 1);

    free_filter(f);
}

int main(void) {
    nostr_json_init();
    test_tags_serialize_to_hash_keys();
    test_tags_roundtrip_from_hash_keys();
    nostr_json_cleanup();
    printf("test_json_filter_tags OK\n");
    return 0;
}
