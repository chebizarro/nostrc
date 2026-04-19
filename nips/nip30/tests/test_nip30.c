#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr/nip30/nip30.h"

static void test_parse_emoji_tags(void) {
    NostrEvent *ev = nostr_event_new();

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("emoji", "smiley",
        "https://example.com/smiley.png", NULL));
    nostr_tags_append(tags, nostr_tag_new("emoji", "heart",
        "https://example.com/heart.gif", NULL));
    nostr_tags_append(tags, nostr_tag_new("p", "somepubkey", NULL));

    assert(nostr_nip30_count(ev) == 2);

    NostrNip30Emoji emojis[4];
    size_t count = 0;
    assert(nostr_nip30_parse(ev, emojis, 4, &count) == 0);
    assert(count == 2);
    assert(strcmp(emojis[0].shortcode, "smiley") == 0);
    assert(strcmp(emojis[0].url, "https://example.com/smiley.png") == 0);
    assert(strcmp(emojis[1].shortcode, "heart") == 0);

    nostr_event_free(ev);
}

static void test_get_url(void) {
    NostrEvent *ev = nostr_event_new();

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("emoji", "rocket",
        "https://example.com/rocket.png", NULL));

    const char *url = nostr_nip30_get_url(ev, "rocket");
    assert(url != NULL);
    assert(strcmp(url, "https://example.com/rocket.png") == 0);

    assert(nostr_nip30_get_url(ev, "nonexistent") == NULL);

    nostr_event_free(ev);
}

static void test_find_all(void) {
    const char *content = "Hello :smiley: world :heart: end";

    NostrNip30Match matches[4];
    size_t count = 0;
    assert(nostr_nip30_find_all(content, matches, 4, &count) == 0);
    assert(count == 2);

    assert(matches[0].name_len == 6);
    assert(memcmp(matches[0].name, "smiley", 6) == 0);
    assert(matches[0].start == 6);
    assert(matches[0].end == 14);

    assert(matches[1].name_len == 5);
    assert(memcmp(matches[1].name, "heart", 5) == 0);
}

static void test_find_all_edge_cases(void) {
    NostrNip30Match matches[4];
    size_t count = 0;

    /* Adjacent colons with no content */
    assert(nostr_nip30_find_all("::", matches, 4, &count) == 0);
    assert(count == 0);

    /* Unclosed shortcode */
    assert(nostr_nip30_find_all(":unclosed", matches, 4, &count) == 0);
    assert(count == 0);

    /* Empty content */
    assert(nostr_nip30_find_all("", matches, 4, &count) == 0);
    assert(count == 0);

    /* Shortcode at start and end */
    assert(nostr_nip30_find_all(":start: and :end:", matches, 4, &count) == 0);
    assert(count == 2);

    /* Underscores and digits in shortcode */
    assert(nostr_nip30_find_all(":my_emoji_2:", matches, 4, &count) == 0);
    assert(count == 1);
    assert(matches[0].name_len == 10);
}

static const char *test_replacer(const char *name, size_t name_len,
                                  void *user_data) {
    (void)user_data;
    if (name_len == 6 && memcmp(name, "smiley", 6) == 0)
        return "😊";
    if (name_len == 5 && memcmp(name, "heart", 5) == 0)
        return "❤️";
    return NULL; /* keep original */
}

static void test_replace_all(void) {
    const char *content = "Hello :smiley: :unknown: :heart:!";

    char *result = nostr_nip30_replace_all(content, test_replacer, NULL);
    assert(result != NULL);
    assert(strstr(result, "😊") != NULL);
    assert(strstr(result, "❤️") != NULL);
    assert(strstr(result, ":unknown:") != NULL); /* kept original */
    free(result);
}

static void test_replace_all_no_matches(void) {
    const char *content = "No emoji here";
    char *result = nostr_nip30_replace_all(content, test_replacer, NULL);
    assert(result != NULL);
    assert(strcmp(result, content) == 0);
    free(result);
}

static void test_add_emoji(void) {
    NostrEvent *ev = nostr_event_new();

    assert(nostr_nip30_add_emoji(ev, "custom", "https://example.com/custom.png") == 0);
    assert(nostr_nip30_count(ev) == 1);

    const char *url = nostr_nip30_get_url(ev, "custom");
    assert(url != NULL);
    assert(strcmp(url, "https://example.com/custom.png") == 0);

    nostr_event_free(ev);
}

static void test_invalid_inputs(void) {
    assert(nostr_nip30_count(NULL) == 0);
    assert(nostr_nip30_get_url(NULL, "x") == NULL);

    NostrNip30Emoji emojis[2];
    size_t count;
    assert(nostr_nip30_parse(NULL, emojis, 2, &count) == -EINVAL);

    NostrNip30Match matches[2];
    assert(nostr_nip30_find_all(NULL, matches, 2, &count) == -EINVAL);

    assert(nostr_nip30_add_emoji(NULL, "x", "y") == -EINVAL);
}

int main(void) {
    test_parse_emoji_tags();
    test_get_url();
    test_find_all();
    test_find_all_edge_cases();
    test_replace_all();
    test_replace_all_no_matches();
    test_add_emoji();
    test_invalid_inputs();
    printf("nip30 ok\n");
    return 0;
}
