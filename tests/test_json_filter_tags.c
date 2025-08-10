#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "json.h"
#include "nostr-tag.h"
#include "nostr-filter.h"
#include "nostr_jansson.h"

static int tags_count_value(const NostrTags *tags, const char *name, const char *value) {
    if (!tags) return 0;
    int c = 0;
    for (size_t i = 0; i < nostr_tags_size(tags); i++) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (t && t->size >= 2 && t->data[0] && t->data[1]) {
            if (strcmp(t->data[0], name) == 0 && strcmp(t->data[1], value) == 0) c++;
        }
    }
    return c;
}

static void test_tags_serialize_to_hash_keys(void) {
    nostr_set_json_interface(jansson_impl);
    NostrFilter *f = nostr_filter_new();
    assert(f);

    // tags: [ ["e","x1"], ["e","x2"], ["p","y"] ]
    NostrTag *t1 = nostr_tag_new("e", "x1", NULL);
    NostrTag *t2 = nostr_tag_new("e", "x2", NULL);
    NostrTag *t3 = nostr_tag_new("p", "y", NULL);
    NostrTags *tmp = nostr_tags_append_unique(f->tags, t1); if (tmp) f->tags = tmp;
    tmp = nostr_tags_append_unique(f->tags, t2); if (tmp) f->tags = tmp;
    tmp = nostr_tags_append_unique(f->tags, t3); if (tmp) f->tags = tmp;

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
    nostr_filter_free(f);
}

static void test_tags_roundtrip_from_hash_keys(void) {
    nostr_set_json_interface(jansson_impl);
    const char *js = "{\"#e\":[\"x1\",\"x2\"],\"#p\":[\"y\"]}";
    NostrFilter *f = nostr_filter_new();
    assert(f);
    assert(nostr_filter_deserialize(f, js) == 0);

    assert(tags_count_value(f->tags, "e", "x1") == 1);
    assert(tags_count_value(f->tags, "e", "x2") == 1);
    assert(tags_count_value(f->tags, "p", "y") == 1);

    nostr_filter_free(f);
}

int main(void) {
    nostr_json_init();
    test_tags_serialize_to_hash_keys();
    test_tags_roundtrip_from_hash_keys();
    nostr_json_cleanup();
    printf("test_json_filter_tags OK\n");
    return 0;
}
