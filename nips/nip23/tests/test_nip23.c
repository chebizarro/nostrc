#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr/nip23/nip23.h"

static void test_parse_article(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, NOSTR_NIP23_KIND_LONG_FORM);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("d", "my-article-2024", NULL));
    nostr_tags_append(tags, nostr_tag_new("title", "My Great Article", NULL));
    nostr_tags_append(tags, nostr_tag_new("summary", "A short summary", NULL));
    nostr_tags_append(tags, nostr_tag_new("image", "https://example.com/cover.jpg", NULL));
    nostr_tags_append(tags, nostr_tag_new("published_at", "1700000000", NULL));
    nostr_tags_append(tags, nostr_tag_new("client", "gnostr", NULL));

    NostrNip23Article article;
    assert(nostr_nip23_parse(ev, &article) == 0);
    assert(strcmp(article.identifier, "my-article-2024") == 0);
    assert(strcmp(article.title, "My Great Article") == 0);
    assert(strcmp(article.summary, "A short summary") == 0);
    assert(strcmp(article.image, "https://example.com/cover.jpg") == 0);
    assert(article.published_at == 1700000000);
    assert(strcmp(article.client, "gnostr") == 0);

    nostr_event_free(ev);
}

static void test_parse_article_minimal(void) {
    NostrEvent *ev = nostr_event_new();

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("d", "slug-only", NULL));

    NostrNip23Article article;
    assert(nostr_nip23_parse(ev, &article) == 0);
    assert(strcmp(article.identifier, "slug-only") == 0);
    assert(article.title == NULL);
    assert(article.summary == NULL);
    assert(article.image == NULL);
    assert(article.published_at == 0);
    assert(article.client == NULL);

    nostr_event_free(ev);
}

static void test_parse_article_no_d_tag(void) {
    NostrEvent *ev = nostr_event_new();

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("title", "No D Tag", NULL));

    NostrNip23Article article;
    assert(nostr_nip23_parse(ev, &article) == -ENOENT);

    nostr_event_free(ev);
}

static void test_is_article(void) {
    NostrEvent *ev = nostr_event_new();

    nostr_event_set_kind(ev, NOSTR_NIP23_KIND_LONG_FORM);
    assert(nostr_nip23_is_article(ev));
    assert(!nostr_nip23_is_draft(ev));
    assert(nostr_nip23_is_long_form(ev));

    nostr_event_set_kind(ev, NOSTR_NIP23_KIND_DRAFT);
    assert(!nostr_nip23_is_article(ev));
    assert(nostr_nip23_is_draft(ev));
    assert(nostr_nip23_is_long_form(ev));

    nostr_event_set_kind(ev, 1);
    assert(!nostr_nip23_is_article(ev));
    assert(!nostr_nip23_is_draft(ev));
    assert(!nostr_nip23_is_long_form(ev));

    nostr_event_free(ev);
}

static void test_hashtags(void) {
    NostrEvent *ev = nostr_event_new();

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("d", "test", NULL));
    nostr_tags_append(tags, nostr_tag_new("t", "nostr", NULL));
    nostr_tags_append(tags, nostr_tag_new("t", "bitcoin", NULL));
    nostr_tags_append(tags, nostr_tag_new("t", "programming", NULL));
    nostr_tags_append(tags, nostr_tag_new("p", "somepubkey", NULL)); /* not a t tag */

    assert(nostr_nip23_count_hashtags(ev) == 3);

    const char *ht[4];
    size_t count = 0;
    assert(nostr_nip23_get_hashtags(ev, ht, 4, &count) == 0);
    assert(count == 3);
    assert(strcmp(ht[0], "nostr") == 0);
    assert(strcmp(ht[1], "bitcoin") == 0);
    assert(strcmp(ht[2], "programming") == 0);

    /* Test max limit */
    count = 0;
    assert(nostr_nip23_get_hashtags(ev, ht, 2, &count) == 0);
    assert(count == 2);

    nostr_event_free(ev);
}

static void test_reading_time(void) {
    /* 200 words at default 200 WPM = 1 minute */
    char buf[2000];
    buf[0] = '\0';
    for (int i = 0; i < 200; ++i) {
        strcat(buf, "word ");
    }
    assert(nostr_nip23_estimate_reading_time(buf, 0) == 1);

    /* 400 words at 200 WPM = 2 minutes */
    char buf2[4000];
    buf2[0] = '\0';
    for (int i = 0; i < 400; ++i) {
        strcat(buf2, "word ");
    }
    assert(nostr_nip23_estimate_reading_time(buf2, 0) == 2);

    /* Custom WPM */
    assert(nostr_nip23_estimate_reading_time(buf2, 400) == 1);

    /* Empty content */
    assert(nostr_nip23_estimate_reading_time("", 0) == 0);
    assert(nostr_nip23_estimate_reading_time(NULL, 0) == 0);

    /* Single word = 1 minute */
    assert(nostr_nip23_estimate_reading_time("hello", 0) == 1);
}

static void test_create_article(void) {
    NostrEvent *ev = nostr_event_new();

    NostrNip23Article article = {
        .identifier = "my-post",
        .title = "My Post",
        .summary = "A summary",
        .image = "https://example.com/img.jpg",
        .published_at = 1700000000,
        .client = "test-client",
    };

    assert(nostr_nip23_create_article(ev, &article) == 0);
    assert(nostr_event_get_kind(ev) == NOSTR_NIP23_KIND_LONG_FORM);

    /* Round-trip parse */
    NostrNip23Article parsed;
    assert(nostr_nip23_parse(ev, &parsed) == 0);
    assert(strcmp(parsed.identifier, "my-post") == 0);
    assert(strcmp(parsed.title, "My Post") == 0);
    assert(strcmp(parsed.summary, "A summary") == 0);
    assert(strcmp(parsed.image, "https://example.com/img.jpg") == 0);
    assert(parsed.published_at == 1700000000);
    assert(strcmp(parsed.client, "test-client") == 0);

    nostr_event_free(ev);
}

static void test_create_draft(void) {
    NostrEvent *ev = nostr_event_new();

    NostrNip23Article article = {
        .identifier = "draft-post",
        .title = "Draft",
    };

    assert(nostr_nip23_create_draft(ev, &article) == 0);
    assert(nostr_event_get_kind(ev) == NOSTR_NIP23_KIND_DRAFT);
    assert(nostr_nip23_is_draft(ev));

    nostr_event_free(ev);
}

static void test_add_hashtag(void) {
    NostrEvent *ev = nostr_event_new();

    NostrNip23Article article = { .identifier = "test" };
    assert(nostr_nip23_create_article(ev, &article) == 0);

    assert(nostr_nip23_add_hashtag(ev, "nostr") == 0);
    assert(nostr_nip23_add_hashtag(ev, "dev") == 0);
    assert(nostr_nip23_count_hashtags(ev) == 2);

    nostr_event_free(ev);
}

static void test_invalid_inputs(void) {
    assert(!nostr_nip23_is_article(NULL));
    assert(!nostr_nip23_is_draft(NULL));
    assert(!nostr_nip23_is_long_form(NULL));
    assert(nostr_nip23_count_hashtags(NULL) == 0);

    NostrNip23Article article;
    assert(nostr_nip23_parse(NULL, &article) == -EINVAL);

    const char *ht[2];
    size_t count;
    assert(nostr_nip23_get_hashtags(NULL, ht, 2, &count) == -EINVAL);

    assert(nostr_nip23_create_article(NULL, &article) == -EINVAL);
    assert(nostr_nip23_add_hashtag(NULL, "x") == -EINVAL);
}

int main(void) {
    test_parse_article();
    test_parse_article_minimal();
    test_parse_article_no_d_tag();
    test_is_article();
    test_hashtags();
    test_reading_time();
    test_create_article();
    test_create_draft();
    test_add_hashtag();
    test_invalid_inputs();
    printf("nip23 ok\n");
    return 0;
}
