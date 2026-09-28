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

static void test_empty_tag_prefix(void) {
    NostrTags *tags = nostr_tags_new(0);
    NostrTag *existing = nostr_tag_new("e", "same", NULL);
    NostrTag *empty = nostr_tag_new(NULL);
    NostrTag *prefix = nostr_tag_new("e", "sa", NULL);
    NostrTag *duplicate = nostr_tag_new("e", "same", NULL);
    assert(tags && existing && empty && prefix && duplicate);
    assert(nostr_tags_append_unique(tags, existing) == tags);

    assert(nostr_tags_get_first(tags, empty) == NULL);
    assert(nostr_tags_append_unique(tags, empty) == tags);
    assert(nostr_tags_size(tags) == 2);
    assert(nostr_tags_get_first(tags, prefix) == existing);
    assert(nostr_tags_append_unique(tags, duplicate) == tags);
    assert(nostr_tags_size(tags) == 2);

    nostr_tag_free(duplicate);
    nostr_tag_free(prefix);
    nostr_tags_free(tags);
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

static void test_jansson_duplicate_tags(void) {
    NostrFilter *f = nostr_filter_new();
    assert(f);
    nostr_json_force_fallback(true);
    assert(nostr_filter_deserialize(f, "{\"#e\":[\"same\",\"same\"]}") == 0);
    assert(tags_count_value(f->tags, "e", "same") == 1);
    nostr_filter_free(f);

    f = nostr_filter_new();
    assert(f);
    assert(nostr_filter_deserialize(f, "{\"tags\":[[\"e\",\"same\"],[\"e\",\"same\"]]}") == 0);
    assert(tags_count_value(f->tags, "e", "same") == 1);
    nostr_filter_free(f);
    nostr_json_force_fallback(false);
}

static void test_duplicate_public_tag_append(void) {
    NostrFilter *f = nostr_filter_new();
    assert(f);
    nostr_filter_tags_append(f, "e", "same", NULL);
    nostr_filter_tags_append(f, "e", "same", NULL);
    assert(tags_count_value(f->tags, "e", "same") == 1);
    nostr_filter_free(f);
}

static void test_partial_compact_search_then_fallback(void) {
    const char *json = "{\"search\":\"needle\",\"limit\":1.5}";
    NostrFilter *probe = nostr_filter_new();
    assert(probe);
    assert(nostr_filter_deserialize_compact(probe, json, NULL) == 0);
    assert(probe->search && strcmp(probe->search, "needle") == 0);
    nostr_filter_free(probe);

    NostrFilter *f = nostr_filter_new();
    assert(f);
    assert(nostr_filter_deserialize(f, json) == 0);
    assert(f->search && strcmp(f->search, "needle") == 0);
    nostr_filter_free(f);
}

static void test_duplicate_tag_before_malformed_tail(void) {
    const char *bad = "{\"#e\":[\"e1\",\"e1\"],\"#p\":[\"p1\"],\"li:imt}5\"\n";
    NostrFilter *f = nostr_filter_new();
    assert(f);
    assert(nostr_filter_deserialize_compact(f, bad, NULL) == 0);
    assert(tags_count_value(f->tags, "e", "e1") == 1);
    assert(tags_count_value(f->tags, "p", "p1") == 1);
    nostr_filter_free(f);

    f = nostr_filter_new();
    assert(f);
    assert(nostr_filter_deserialize(f, bad) != 0);
    nostr_filter_free(f);

    f = nostr_filter_new();
    assert(f);
    assert(nostr_filter_deserialize(f, "{\"#e\":[\"e1\",\"e1\"],\"#p\":[\"p1\"]}") == 0);
    assert(tags_count_value(f->tags, "e", "e1") == 1);
    assert(tags_count_value(f->tags, "p", "p1") == 1);
    nostr_filter_free(f);
}

int main(void) {
    nostr_json_init();
    test_empty_tag_prefix();
    test_tags_serialize_to_hash_keys();
    test_tags_roundtrip_from_hash_keys();
    test_jansson_duplicate_tags();
    test_duplicate_public_tag_append();
    test_partial_compact_search_then_fallback();
    test_duplicate_tag_before_malformed_tail();
    nostr_json_cleanup();
    printf("test_json_filter_tags OK\n");
    return 0;
}
