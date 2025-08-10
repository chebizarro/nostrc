#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "nostr-filter.h"
#include "nostr_jansson.h"

static void test_malformed_arrays_should_fail(void) {
    nostr_set_json_interface(jansson_impl);
    const char *bad_kinds = "{\"kinds\":[1,\"bad\"]}";
    const char *bad_ids = "{\"ids\":[123]}";
    const char *bad_authors = "{\"authors\":[true]}";
    const char *bad_tag_array = "{\"#e\":[1,2,3]}"; // must be strings

    NostrFilter *f = nostr_filter_new();
    assert(f);
    assert(nostr_filter_deserialize(f, bad_kinds) != 0);
    nostr_filter_free(f);

    f = nostr_filter_new();
    assert(f);
    assert(nostr_filter_deserialize(f, bad_ids) != 0);
    nostr_filter_free(f);

    f = nostr_filter_new();
    assert(f);
    assert(nostr_filter_deserialize(f, bad_authors) != 0);
    nostr_filter_free(f);

    f = nostr_filter_new();
    assert(f);
    assert(nostr_filter_deserialize(f, bad_tag_array) != 0);
    nostr_filter_free(f);
}

static void build_large_arrays_json(char **out_json, int kinds_n, int ids_n) {
    // Construct a JSON string with large arrays for kinds and ids
    size_t cap = 1024;
    char *buf = malloc(cap);
    size_t len = 0;
    #define APPEND_FMT(fmt, ...) do { \
        char tmp[128]; \
        int n = snprintf(tmp, sizeof(tmp), fmt, __VA_ARGS__); \
        if (len + (size_t)n + 1 > cap) { \
            cap = (len + n + 1) * 2; \
            buf = realloc(buf, cap); \
        } \
        memcpy(buf + len, tmp, (size_t)n); \
        len += (size_t)n; \
        buf[len] = '\0'; \
    } while (0)

    APPEND_FMT("%s", "{");

    // kinds
    APPEND_FMT("%s", "\"kinds\":[");
    for (int i = 0; i < kinds_n; i++) {
        APPEND_FMT("%d%s", i, (i+1<kinds_n)?",":"");
    }
    APPEND_FMT("%s", "],");

    // ids
    APPEND_FMT("%s", "\"ids\":[");
    for (int i = 0; i < ids_n; i++) {
        APPEND_FMT("\"id_%d\"%s", i, (i+1<ids_n)?",":"");
    }
    APPEND_FMT("%s", "]}");

    *out_json = buf;
}

static void test_large_arrays_stress(void) {
    nostr_set_json_interface(jansson_impl);
    char *json = NULL;
    const int K = 10000; // 10k kinds
    const int I = 5000;  // 5k ids
    build_large_arrays_json(&json, K, I);
    assert(json);

    NostrFilter *f = nostr_filter_new();
    assert(f);
    int rc = nostr_filter_deserialize(f, json);
    assert(rc == 0);
    assert((int)int_array_size(&f->kinds) == K);
    assert((int)string_array_size(&f->ids) == I);

    // Serialize back and spot check presence
    char *s = nostr_filter_serialize(f);
    assert(s);
    assert(strstr(s, "\"kinds\"") != NULL);
    assert(strstr(s, "\"ids\"") != NULL);

    free(s);
    free(json);
    nostr_filter_free(f);
}

int main(void) {
    nostr_json_init();
    test_malformed_arrays_should_fail();
    test_large_arrays_stress();
    nostr_json_cleanup();
    printf("test_json_filter_robust OK\n");
    return 0;
}
