#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr/nip92/nip92.h"

static void test_no_imeta_tags(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    /* No tags → count = 0 */
    assert(nostr_nip92_count(ev) == 0);

    NostrNip92Entry entries[4];
    size_t count = 99;
    assert(nostr_nip92_parse(ev, entries, 4, &count) == 0);
    assert(count == 0);

    /* find_url should return ENOENT */
    NostrNip92Entry found;
    assert(nostr_nip92_find_url(ev, "https://example.com/img.jpg", &found) == -ENOENT);

    nostr_event_free(ev);
}

static void test_non_imeta_tags_ignored(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("t", "nothing", NULL));
    nostr_tags_append(tags, nostr_tag_new("p", "abc123", NULL));

    assert(nostr_nip92_count(ev) == 0);

    nostr_event_free(ev);
}

static void test_imeta_too_few_elements(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    /* imeta with only 1 field (needs 3+: "imeta" + at least 2 fields) */
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("imeta", "nothing", NULL));

    assert(nostr_nip92_count(ev) == 0);

    nostr_event_free(ev);
}

static void test_parse_single_entry(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    /* Build an imeta tag manually */
    NostrTag *tag = nostr_tag_new("imeta", NULL);
    nostr_tag_append(tag, "url https://i.nostr.build/test.gif");
    nostr_tag_append(tag, "blurhash eDG*7p~AE34;E29x");
    nostr_tag_append(tag, "dim 225x191");
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, tag);

    assert(nostr_nip92_count(ev) == 1);

    NostrNip92Entry entries[4];
    size_t count = 0;
    assert(nostr_nip92_parse(ev, entries, 4, &count) == 0);
    assert(count == 1);

    assert(strcmp(entries[0].url, "https://i.nostr.build/test.gif") == 0);
    assert(strcmp(entries[0].blurhash, "eDG*7p~AE34;E29x") == 0);
    assert(entries[0].width == 225);
    assert(entries[0].height == 191);
    assert(entries[0].alt == NULL);

    nostr_event_free(ev);
}

static void test_parse_multiple_entries(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);

    /* First imeta */
    NostrTag *t1 = nostr_tag_new("imeta", NULL);
    nostr_tag_append(t1, "url https://example.com/img1.jpg");
    nostr_tag_append(t1, "dim 800x600");
    nostr_tags_append(tags, t1);

    /* Non-imeta tag in between */
    nostr_tags_append(tags, nostr_tag_new("p", "someone", NULL));

    /* Second imeta */
    NostrTag *t2 = nostr_tag_new("imeta", NULL);
    nostr_tag_append(t2, "url https://example.com/img2.jpg");
    nostr_tag_append(t2, "alt A nice photo");
    nostr_tag_append(t2, "dim 1920x1080");
    nostr_tags_append(tags, t2);

    assert(nostr_nip92_count(ev) == 2);

    NostrNip92Entry entries[4];
    size_t count = 0;
    assert(nostr_nip92_parse(ev, entries, 4, &count) == 0);
    assert(count == 2);

    assert(strcmp(entries[0].url, "https://example.com/img1.jpg") == 0);
    assert(entries[0].width == 800);
    assert(entries[0].height == 600);

    assert(strcmp(entries[1].url, "https://example.com/img2.jpg") == 0);
    assert(strcmp(entries[1].alt, "A nice photo") == 0);
    assert(entries[1].width == 1920);
    assert(entries[1].height == 1080);

    nostr_event_free(ev);
}

static void test_malformed_dim_rejects_all(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);

    /* Good tag first */
    NostrTag *t1 = nostr_tag_new("imeta", NULL);
    nostr_tag_append(t1, "url https://example.com/good.jpg");
    nostr_tag_append(t1, "dim 100x200");
    nostr_tags_append(tags, t1);

    /* Bad tag: dim "oxo" instead of "WxH" */
    NostrTag *t2 = nostr_tag_new("imeta", NULL);
    nostr_tag_append(t2, "blurhash qwkueh");
    nostr_tag_append(t2, "dim oxo");
    nostr_tags_append(tags, t2);

    /* Go behavior: malformed dim → return nil (reject all) */
    NostrNip92Entry entries[4];
    size_t count = 99;
    assert(nostr_nip92_parse(ev, entries, 4, &count) == 0);
    assert(count == 0);

    nostr_event_free(ev);
}

static void test_find_url(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);

    NostrTag *t1 = nostr_tag_new("imeta", NULL);
    nostr_tag_append(t1, "url https://example.com/first.jpg");
    nostr_tag_append(t1, "alt First image");
    nostr_tags_append(tags, t1);

    NostrTag *t2 = nostr_tag_new("imeta", NULL);
    nostr_tag_append(t2, "url https://example.com/second.jpg");
    nostr_tag_append(t2, "blurhash abc123");
    nostr_tag_append(t2, "dim 640x480");
    nostr_tags_append(tags, t2);

    /* Find second image */
    NostrNip92Entry found;
    assert(nostr_nip92_find_url(ev, "https://example.com/second.jpg", &found) == 0);
    assert(strcmp(found.url, "https://example.com/second.jpg") == 0);
    assert(strcmp(found.blurhash, "abc123") == 0);
    assert(found.width == 640);
    assert(found.height == 480);

    /* Not found */
    assert(nostr_nip92_find_url(ev, "https://example.com/missing.jpg", &found) == -ENOENT);

    nostr_event_free(ev);
}

static void test_build_tag(void) {
    NostrNip92Entry entry = {
        .url = "https://example.com/img.jpg",
        .blurhash = "eDG*7p~AE34",
        .alt = "A test image",
        .width = 1024,
        .height = 768,
    };

    NostrTag *tag = nostr_nip92_build_tag(&entry);
    assert(tag != NULL);

    /* Verify structure */
    assert(strcmp(nostr_tag_get(tag, 0), "imeta") == 0);

    /* Find url field */
    bool found_url = false, found_bh = false, found_dim = false, found_alt = false;
    for (size_t i = 1; i < nostr_tag_size(tag); ++i) {
        const char *v = nostr_tag_get(tag, i);
        if (strncmp(v, "url ", 4) == 0) {
            assert(strcmp(v + 4, "https://example.com/img.jpg") == 0);
            found_url = true;
        } else if (strncmp(v, "blurhash ", 9) == 0) {
            assert(strcmp(v + 9, "eDG*7p~AE34") == 0);
            found_bh = true;
        } else if (strncmp(v, "dim ", 4) == 0) {
            assert(strcmp(v + 4, "1024x768") == 0);
            found_dim = true;
        } else if (strncmp(v, "alt ", 4) == 0) {
            assert(strcmp(v + 4, "A test image") == 0);
            found_alt = true;
        }
    }
    assert(found_url);
    assert(found_bh);
    assert(found_dim);
    assert(found_alt);

    nostr_tag_free(tag);
}

static void test_build_tag_minimal(void) {
    /* Only URL set */
    NostrNip92Entry entry = { .url = "https://example.com/min.jpg" };

    NostrTag *tag = nostr_nip92_build_tag(&entry);
    assert(tag != NULL);
    assert(strcmp(nostr_tag_get(tag, 0), "imeta") == 0);
    /* Should have exactly 2 elements: "imeta" + "url ..." */
    assert(nostr_tag_size(tag) == 2);
    assert(strncmp(nostr_tag_get(tag, 1), "url ", 4) == 0);

    nostr_tag_free(tag);
}

static void test_add_imeta(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    /* Add an existing non-imeta tag */
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("p", "abc123", NULL));

    assert(nostr_nip92_count(ev) == 0);

    /* Add imeta entry */
    NostrNip92Entry entry = {
        .url = "https://example.com/added.jpg",
        .width = 100,
        .height = 50,
    };
    assert(nostr_nip92_add(ev, &entry) == 0);

    assert(nostr_nip92_count(ev) == 1);

    /* p tag should still be there */
    tags = (NostrTags *)nostr_event_get_tags(ev);
    assert(nostr_tags_size(tags) == 2);

    /* Verify via find */
    NostrNip92Entry found;
    assert(nostr_nip92_find_url(ev, "https://example.com/added.jpg", &found) == 0);
    assert(found.width == 100);
    assert(found.height == 50);

    nostr_event_free(ev);
}

static void test_invalid_inputs(void) {
    /* NULL event */
    assert(nostr_nip92_count(NULL) == 0);

    NostrNip92Entry entries[2];
    size_t count = 0;
    assert(nostr_nip92_parse(NULL, entries, 2, &count) == -EINVAL);

    NostrNip92Entry found;
    assert(nostr_nip92_find_url(NULL, "x", &found) == -EINVAL);

    assert(nostr_nip92_add(NULL, &found) == -EINVAL);

    /* NULL entry for build */
    assert(nostr_nip92_build_tag(NULL) == NULL);

    /* Entry without URL */
    NostrNip92Entry no_url = { .blurhash = "abc" };
    assert(nostr_nip92_build_tag(&no_url) == NULL);
}

int main(void) {
    test_no_imeta_tags();
    test_non_imeta_tags_ignored();
    test_imeta_too_few_elements();
    test_parse_single_entry();
    test_parse_multiple_entries();
    test_malformed_dim_rejects_all();
    test_find_url();
    test_build_tag();
    test_build_tag_minimal();
    test_add_imeta();
    test_invalid_inputs();
    printf("nip92 ok\n");
    return 0;
}
