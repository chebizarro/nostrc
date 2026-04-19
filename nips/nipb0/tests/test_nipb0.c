#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr/nipb0/nipb0.h"

/* ---- URL building tests ---- */

static void test_url_upload(void) {
    char *url = nostr_blossom_url_upload("https://cdn.example.com");
    assert(url != NULL);
    assert(strcmp(url, "https://cdn.example.com/upload") == 0);
    free(url);
}

static void test_url_upload_trailing_slash(void) {
    char *url = nostr_blossom_url_upload("https://cdn.example.com/");
    assert(url != NULL);
    assert(strcmp(url, "https://cdn.example.com/upload") == 0);
    free(url);
}

static void test_url_download(void) {
    char *url = nostr_blossom_url_download("https://cdn.example.com",
        "abc123def456");
    assert(url != NULL);
    assert(strcmp(url, "https://cdn.example.com/abc123def456") == 0);
    free(url);
}

static void test_url_check(void) {
    char *url = nostr_blossom_url_check("https://cdn.example.com",
        "deadbeef");
    assert(url != NULL);
    assert(strcmp(url, "https://cdn.example.com/deadbeef") == 0);
    free(url);
}

static void test_url_list(void) {
    char *url = nostr_blossom_url_list("https://cdn.example.com",
        "aabbccdd");
    assert(url != NULL);
    assert(strcmp(url, "https://cdn.example.com/list/aabbccdd") == 0);
    free(url);
}

static void test_url_delete(void) {
    char *url = nostr_blossom_url_delete("https://cdn.example.com/",
        "hash123");
    assert(url != NULL);
    assert(strcmp(url, "https://cdn.example.com/hash123") == 0);
    free(url);
}

static void test_url_mirror(void) {
    char *url = nostr_blossom_url_mirror("https://cdn.example.com");
    assert(url != NULL);
    assert(strcmp(url, "https://cdn.example.com/mirror") == 0);
    free(url);
}

static void test_url_null_inputs(void) {
    assert(nostr_blossom_url_upload(NULL) == NULL);
    assert(nostr_blossom_url_download(NULL, "hash") == NULL);
    assert(nostr_blossom_url_download("server", NULL) == NULL);
    assert(nostr_blossom_url_list(NULL, "pk") == NULL);
    assert(nostr_blossom_url_list("server", NULL) == NULL);
    assert(nostr_blossom_url_mirror(NULL) == NULL);
}

/* ---- Auth event tests ---- */

static const char *find_tag_value(const NostrEvent *ev, const char *key) {
    const NostrTags *tags = nostr_event_get_tags(ev);
    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; ++i) {
        const NostrTag *t = nostr_tags_get(tags, i);
        if (nostr_tag_size(t) >= 2 &&
            strcmp(nostr_tag_get(t, 0), key) == 0) {
            return nostr_tag_get(t, 1);
        }
    }
    return NULL;
}

static void test_auth_upload(void) {
    NostrEvent *ev = nostr_blossom_create_auth(
        NOSTR_BLOSSOM_OP_UPLOAD, "abc123", 120);
    assert(ev != NULL);
    assert(nostr_event_get_kind(ev) == NOSTR_BLOSSOM_AUTH_KIND);

    const char *t_val = find_tag_value(ev, "t");
    assert(t_val != NULL);
    assert(strcmp(t_val, "upload") == 0);

    const char *x_val = find_tag_value(ev, "x");
    assert(x_val != NULL);
    assert(strcmp(x_val, "abc123") == 0);

    const char *exp_val = find_tag_value(ev, "expiration");
    assert(exp_val != NULL);
    /* Expiration should be in the future */
    int64_t exp_ts = strtoll(exp_val, NULL, 10);
    assert(exp_ts > time(NULL));

    nostr_event_free(ev);
}

static void test_auth_get(void) {
    NostrEvent *ev = nostr_blossom_create_auth(
        NOSTR_BLOSSOM_OP_GET, "deadbeef", 0);
    assert(ev != NULL);

    const char *t_val = find_tag_value(ev, "t");
    assert(strcmp(t_val, "get") == 0);

    const char *x_val = find_tag_value(ev, "x");
    assert(strcmp(x_val, "deadbeef") == 0);

    nostr_event_free(ev);
}

static void test_auth_list(void) {
    /* list doesn't require hash */
    NostrEvent *ev = nostr_blossom_create_auth(
        NOSTR_BLOSSOM_OP_LIST, NULL, 60);
    assert(ev != NULL);

    const char *t_val = find_tag_value(ev, "t");
    assert(strcmp(t_val, "list") == 0);

    /* No x tag for list */
    const char *x_val = find_tag_value(ev, "x");
    assert(x_val == NULL);

    nostr_event_free(ev);
}

static void test_auth_delete(void) {
    NostrEvent *ev = nostr_blossom_create_auth(
        NOSTR_BLOSSOM_OP_DELETE, "hash456", 30);
    assert(ev != NULL);

    const char *t_val = find_tag_value(ev, "t");
    assert(strcmp(t_val, "delete") == 0);

    const char *x_val = find_tag_value(ev, "x");
    assert(strcmp(x_val, "hash456") == 0);

    nostr_event_free(ev);
}

static void test_auth_null_hash_non_list(void) {
    /* upload requires hash */
    assert(nostr_blossom_create_auth(NOSTR_BLOSSOM_OP_UPLOAD, NULL, 60) == NULL);
    assert(nostr_blossom_create_auth(NOSTR_BLOSSOM_OP_GET, NULL, 60) == NULL);
    assert(nostr_blossom_create_auth(NOSTR_BLOSSOM_OP_DELETE, NULL, 60) == NULL);
}

/* ---- Blob descriptor parsing tests ---- */

static void test_parse_descriptor(void) {
    const char *json =
        "{"
        "\"url\":\"https://cdn.example.com/abc123.png\","
        "\"sha256\":\"abc123\","
        "\"size\":184292,"
        "\"type\":\"image/png\","
        "\"uploaded\":1725909682"
        "}";

    NostrBlossomBlobDescriptor desc;
    int rc = nostr_blossom_parse_descriptor(json, &desc);
    assert(rc == 0);

    assert(strcmp(desc.url, "https://cdn.example.com/abc123.png") == 0);
    assert(strcmp(desc.sha256, "abc123") == 0);
    assert(desc.size == 184292);
    assert(strcmp(desc.content_type, "image/png") == 0);
    assert(desc.uploaded == 1725909682);

    nostr_blossom_descriptor_free(&desc);
}

static void test_parse_descriptor_minimal(void) {
    const char *json = "{\"url\":\"https://cdn.example.com/x\",\"sha256\":\"x\",\"size\":0}";

    NostrBlossomBlobDescriptor desc;
    int rc = nostr_blossom_parse_descriptor(json, &desc);
    assert(rc == 0);

    assert(desc.url != NULL);
    assert(desc.sha256 != NULL);
    assert(desc.content_type == NULL);
    assert(desc.uploaded == 0);

    nostr_blossom_descriptor_free(&desc);
}

static void test_parse_descriptor_null_inputs(void) {
    NostrBlossomBlobDescriptor desc;
    assert(nostr_blossom_parse_descriptor(NULL, &desc) == -EINVAL);
    assert(nostr_blossom_parse_descriptor("{}", NULL) == -EINVAL);
}

static void test_parse_descriptor_list(void) {
    const char *json =
        "["
        "{\"url\":\"https://cdn.example.com/a.png\","
        "\"sha256\":\"aaa\",\"size\":100,\"type\":\"image/png\","
        "\"uploaded\":1000},"
        "{\"url\":\"https://cdn.example.com/b.jpg\","
        "\"sha256\":\"bbb\",\"size\":200,\"type\":\"image/jpeg\","
        "\"uploaded\":2000}"
        "]";

    NostrBlossomBlobDescriptor descs[5];
    size_t count = 0;
    int rc = nostr_blossom_parse_descriptor_list(json, descs, 5, &count);
    assert(rc == 0);
    assert(count == 2);

    assert(strcmp(descs[0].sha256, "aaa") == 0);
    assert(descs[0].size == 100);
    assert(strcmp(descs[1].sha256, "bbb") == 0);
    assert(descs[1].size == 200);

    nostr_blossom_descriptor_free(&descs[0]);
    nostr_blossom_descriptor_free(&descs[1]);
}

static void test_parse_descriptor_list_empty(void) {
    const char *json = "[]";

    NostrBlossomBlobDescriptor descs[5];
    size_t count = 99;
    int rc = nostr_blossom_parse_descriptor_list(json, descs, 5, &count);
    assert(rc == 0);
    assert(count == 0);
}

static void test_parse_descriptor_list_max(void) {
    const char *json =
        "["
        "{\"url\":\"a\",\"sha256\":\"x\",\"size\":1},"
        "{\"url\":\"b\",\"sha256\":\"y\",\"size\":2},"
        "{\"url\":\"c\",\"sha256\":\"z\",\"size\":3}"
        "]";

    NostrBlossomBlobDescriptor descs[2];
    size_t count = 0;
    int rc = nostr_blossom_parse_descriptor_list(json, descs, 2, &count);
    assert(rc == 0);
    assert(count == 2);

    nostr_blossom_descriptor_free(&descs[0]);
    nostr_blossom_descriptor_free(&descs[1]);
}

static void test_parse_descriptor_list_null_inputs(void) {
    NostrBlossomBlobDescriptor descs[5];
    size_t count = 0;
    assert(nostr_blossom_parse_descriptor_list(NULL, descs, 5, &count) == -EINVAL);
    assert(nostr_blossom_parse_descriptor_list("[]", NULL, 5, &count) == -EINVAL);
    assert(nostr_blossom_parse_descriptor_list("[]", descs, 5, NULL) == -EINVAL);
}

/* ---- Utility tests ---- */

static void test_mirror_body(void) {
    char *body = nostr_blossom_mirror_body("https://other.com/abc123.png");
    assert(body != NULL);
    assert(strcmp(body, "{\"url\":\"https://other.com/abc123.png\"}") == 0);
    free(body);
}

static void test_mirror_body_null(void) {
    assert(nostr_blossom_mirror_body(NULL) == NULL);
}

static void test_extract_hash(void) {
    char *h;

    h = nostr_blossom_extract_hash("https://cdn.example.com/abc123.png");
    assert(h != NULL);
    assert(strcmp(h, "abc123") == 0);
    free(h);

    h = nostr_blossom_extract_hash("https://cdn.example.com/abc123");
    assert(h != NULL);
    assert(strcmp(h, "abc123") == 0);
    free(h);

    h = nostr_blossom_extract_hash(
        "https://cdn.example.com/b1674191a88ec5cdd733e4240a81803105dc412d.pdf");
    assert(h != NULL);
    assert(strcmp(h, "b1674191a88ec5cdd733e4240a81803105dc412d") == 0);
    free(h);
}

static void test_extract_hash_null(void) {
    assert(nostr_blossom_extract_hash(NULL) == NULL);
}

static void test_extract_hash_no_slash(void) {
    assert(nostr_blossom_extract_hash("noslash") == NULL);
}

static void test_descriptor_free_null(void) {
    /* Should not crash */
    nostr_blossom_descriptor_free(NULL);
}

int main(void) {
    /* URL building */
    test_url_upload();
    test_url_upload_trailing_slash();
    test_url_download();
    test_url_check();
    test_url_list();
    test_url_delete();
    test_url_mirror();
    test_url_null_inputs();

    /* Auth event creation */
    test_auth_upload();
    test_auth_get();
    test_auth_list();
    test_auth_delete();
    test_auth_null_hash_non_list();

    /* Blob descriptor parsing */
    test_parse_descriptor();
    test_parse_descriptor_minimal();
    test_parse_descriptor_null_inputs();
    test_parse_descriptor_list();
    test_parse_descriptor_list_empty();
    test_parse_descriptor_list_max();
    test_parse_descriptor_list_null_inputs();

    /* Utilities */
    test_mirror_body();
    test_mirror_body_null();
    test_extract_hash();
    test_extract_hash_null();
    test_extract_hash_no_slash();
    test_descriptor_free_null();

    printf("nipb0 ok\n");
    return 0;
}
