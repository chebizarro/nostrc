/* SPDX-License-Identifier: MIT
 * Inspect live bytes at the deallocation boundary, never freed heap storage. */
#include <jansson.h>
#include <secure_buf.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

typedef struct { void *ptr; size_t len; int custom; } Span;
static Span spans[1024];
static size_t n_spans, wiped;
static void expect_wipe(void *ptr, size_t len, int custom) {
    CHECK(n_spans < sizeof(spans) / sizeof(spans[0]));
    spans[n_spans++] = (Span){ptr, len, custom};
}
static void check_wipe(void *ptr, int custom) {
    if (!ptr) return;
    for (size_t i = 0; i < n_spans; i++) if (spans[i].ptr == ptr) {
        CHECK(spans[i].custom == custom);
        const unsigned char *bytes = ptr;
        for (size_t j = 0; j < spans[i].len; j++) CHECK(bytes[j] == 0);
        spans[i].ptr = NULL; wiped++;
    }
}
static char *observed_strdup(const char *text) {
    char *p = strdup(text);
    CHECK(p); expect_wipe(p, strlen(p), 0); return p;
}
static void observed_free(void *ptr) { check_wipe(ptr, 0); free(ptr); }
static void check_tree(json_t *root) {
    if (json_is_string(root)) {
        const char *p = json_string_value(root);
        for (size_t i = 0; i < json_string_length(root); i++) CHECK(p[i] == 0);
    } else if (json_is_array(root)) {
        size_t i; json_t *v; json_array_foreach(root, i, v) check_tree(v);
    } else if (json_is_object(root)) {
        const char *k; json_t *v; json_object_foreach(root, k, v) check_tree(v);
    }
}
static void observed_decref(json_t *root) {
    if (root && root->refcount == 1) check_tree(root);
    json_decref(root);
}
/* Deliberately not free-compatible: catches freeing Jansson dump buffers
 * with libc instead of the embedding application's configured deallocator. */
typedef union { max_align_t align; struct { size_t magic; } h; } Header;
static size_t json_live;
static void *json_alloc(size_t n) {
    Header *h = malloc(sizeof(*h) + n); CHECK(h);
    h->h.magic = 0x46a110c; json_live++; return h + 1;
}
static void json_release(void *p) {
    if (!p) return;
    Header *h = (Header *)p - 1; CHECK(h->h.magic == 0x46a110c);
    check_wipe(p, 1); CHECK(json_live > 0); json_live--; free(h);
}
/* Register every parsed value BEFORE production teardown, independently of
 * whether its wipe traversal later visits that value. */
static void expect_tree_wipe(json_t *value) {
    if (json_is_string(value))
        expect_wipe((void *)json_string_value(value), json_string_length(value), 1);
    else if (json_is_array(value)) {
        size_t i; json_t *v; json_array_foreach(value, i, v) expect_tree_wipe(v);
    } else if (json_is_object(value)) {
        const char *k; json_t *v; json_object_foreach(value, k, v) expect_tree_wipe(v);
    }
}
static json_t *observed_loads(const char *text, size_t flags, json_error_t *err) {
    json_t *value = json_loads(text, flags, err);
    expect_tree_wipe(value); return value;
}
static json_t *observed_string(const char *text) {
    json_t *value = json_string(text);
    expect_tree_wipe(value); return value;
}
static char *observed_dumps(const json_t *root, size_t flags) {
    char *p = json_dumps(root, flags);
    if (p) expect_wipe(p, strlen(p), 1);
    return p;
}

#define strdup observed_strdup
#define free observed_free
#define json_decref observed_decref
#define json_dumps observed_dumps
#define json_loads observed_loads
#define json_string observed_string
#include "../src/core/nip46_msg.c"
#undef strdup
#undef free
#undef json_decref
#undef json_dumps
#undef json_loads
#undef json_string

int main(void) {
    NostrNip46Request req = {0};
    req.n_params = 1; req.params = calloc(1, sizeof(*req.params)); CHECK(req.params);
    req.params[0] = observed_strdup("one-time-connect-secret");
    expect_wipe(req.params, sizeof(*req.params), 0);
    nostr_nip46_request_free(&req);
    CHECK(!req.params && !req.n_params);
    json_malloc_t old_alloc; json_free_t old_free;
    json_get_alloc_funcs(&old_alloc, &old_free);
    json_set_alloc_funcs(json_alloc, json_release);

    CHECK(nostr_nip46_request_parse("{\"id\":\"id\",\"method\":\"connect\",\"params\":[\"secret\",{\"nested\":[\"secret\"]}]}", &req) == 0);
    CHECK(strcmp(req.params[0], "secret") == 0);
    CHECK(strstr(req.params[1], "secret"));
    expect_wipe(req.params[1], strlen(req.params[1]), 0);
    nostr_nip46_request_free(&req);
    /* Partial decode: the first duplicated parameter must be wiped on error. */
    CHECK(nostr_nip46_request_parse("{\"id\":\"id\",\"method\":\"connect\",\"params\":[\"secret\",\"bad\\u0000suffix\"]}", &req) != 0);
    CHECK(!req.params && !req.n_params);
    const char *params[] = {"secret"};
    char *text = nostr_nip46_request_build("id", "connect", params, 1); CHECK(text);
    CHECK(strstr(text, "secret")); secure_wipe(text, strlen(text)); free(text);
    NostrNip46Response res = {0};
    CHECK(nostr_nip46_response_parse("{\"id\":\"id\",\"result\":{\"secret\":\"value\"}}", &res) == 0);
    expect_wipe(res.result, strlen(res.result), 0); nostr_nip46_response_free(&res);
    CHECK(nostr_nip46_response_parse("{\"id\":\"id\",\"result\":\"secret\",\"error\":\"detail\"}", &res) == 0);
    nostr_nip46_response_free(&res);
    text = nostr_nip46_response_build_ok("id", "{\"nested\":[\"secret\"]}"); CHECK(text);
    CHECK(strstr(text, "secret")); secure_wipe(text, strlen(text)); free(text);
    text = nostr_nip46_response_build_err("id", "secret"); CHECK(text);
    secure_wipe(text, strlen(text)); free(text);
    CHECK(json_live == 0);
    json_set_alloc_funcs(old_alloc, old_free);
    for (size_t i = 0; i < n_spans; i++) CHECK(spans[i].ptr == NULL);
    CHECK(wiped > 10); puts("NIP-46 tree/parameter wipe and custom allocator: PASS");
    return 0;
}
