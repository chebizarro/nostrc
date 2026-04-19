#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr/nip73/nip73.h"

static void test_detect_type(void) {
    assert(nostr_nip73_detect_type("isbn:978-0-13-468599-1") == NOSTR_NIP73_ISBN);
    assert(nostr_nip73_detect_type("doi:10.1000/xyz123") == NOSTR_NIP73_DOI);
    assert(nostr_nip73_detect_type("imdb:tt0111161") == NOSTR_NIP73_IMDB);
    assert(nostr_nip73_detect_type("tmdb:movie/278") == NOSTR_NIP73_TMDB);
    assert(nostr_nip73_detect_type("spotify:track:abc123") == NOSTR_NIP73_SPOTIFY);
    assert(nostr_nip73_detect_type("youtube:dQw4w9WgXcQ") == NOSTR_NIP73_YOUTUBE);
    assert(nostr_nip73_detect_type("podcast:guid:abc") == NOSTR_NIP73_PODCAST_GUID);
    assert(nostr_nip73_detect_type("https://example.com/article") == NOSTR_NIP73_URL);
    assert(nostr_nip73_detect_type("http://example.com") == NOSTR_NIP73_URL);
    assert(nostr_nip73_detect_type("unknown:something") == NOSTR_NIP73_UNKNOWN);
    assert(nostr_nip73_detect_type("nocolon") == NOSTR_NIP73_UNKNOWN);
    assert(nostr_nip73_detect_type(NULL) == NOSTR_NIP73_UNKNOWN);
}

static void test_type_from_string(void) {
    assert(nostr_nip73_type_from_string("isbn") == NOSTR_NIP73_ISBN);
    assert(nostr_nip73_type_from_string("doi") == NOSTR_NIP73_DOI);
    assert(nostr_nip73_type_from_string("imdb") == NOSTR_NIP73_IMDB);
    assert(nostr_nip73_type_from_string("tmdb") == NOSTR_NIP73_TMDB);
    assert(nostr_nip73_type_from_string("spotify") == NOSTR_NIP73_SPOTIFY);
    assert(nostr_nip73_type_from_string("youtube") == NOSTR_NIP73_YOUTUBE);
    assert(nostr_nip73_type_from_string("podcast") == NOSTR_NIP73_PODCAST_GUID);
    assert(nostr_nip73_type_from_string("garbage") == NOSTR_NIP73_UNKNOWN);
    assert(nostr_nip73_type_from_string(NULL) == NOSTR_NIP73_UNKNOWN);
}

static void test_type_to_string(void) {
    assert(strcmp(nostr_nip73_type_to_string(NOSTR_NIP73_ISBN), "isbn") == 0);
    assert(strcmp(nostr_nip73_type_to_string(NOSTR_NIP73_DOI), "doi") == 0);
    assert(strcmp(nostr_nip73_type_to_string(NOSTR_NIP73_URL), "url") == 0);
    assert(strcmp(nostr_nip73_type_to_string(NOSTR_NIP73_UNKNOWN), "unknown") == 0);
}

static void test_is_media_type(void) {
    assert(nostr_nip73_is_media_type(NOSTR_NIP73_SPOTIFY));
    assert(nostr_nip73_is_media_type(NOSTR_NIP73_YOUTUBE));
    assert(nostr_nip73_is_media_type(NOSTR_NIP73_PODCAST_GUID));
    assert(!nostr_nip73_is_media_type(NOSTR_NIP73_ISBN));
    assert(!nostr_nip73_is_media_type(NOSTR_NIP73_URL));
}

static void test_is_reference_type(void) {
    assert(nostr_nip73_is_reference_type(NOSTR_NIP73_ISBN));
    assert(nostr_nip73_is_reference_type(NOSTR_NIP73_DOI));
    assert(!nostr_nip73_is_reference_type(NOSTR_NIP73_SPOTIFY));
    assert(!nostr_nip73_is_reference_type(NOSTR_NIP73_URL));
}

static void test_parse_entries(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("i", "isbn:978-0-13-468599-1", NULL));
    nostr_tags_append(tags, nostr_tag_new("p", "abc123", NULL));  /* non-i tag */
    nostr_tags_append(tags, nostr_tag_new("i", "imdb:tt0111161", NULL));
    nostr_tags_append(tags, nostr_tag_new("i", "https://example.com/article", NULL));

    assert(nostr_nip73_count(ev) == 3);

    NostrNip73Entry entries[8];
    size_t count = 0;
    assert(nostr_nip73_parse(ev, entries, 8, &count) == 0);
    assert(count == 3);

    /* ISBN */
    assert(entries[0].type == NOSTR_NIP73_ISBN);
    assert(strcmp(entries[0].value, "isbn:978-0-13-468599-1") == 0);
    assert(strcmp(entries[0].identifier, "978-0-13-468599-1") == 0);

    /* IMDB */
    assert(entries[1].type == NOSTR_NIP73_IMDB);
    assert(strcmp(entries[1].identifier, "tt0111161") == 0);

    /* URL */
    assert(entries[2].type == NOSTR_NIP73_URL);
    assert(strcmp(entries[2].value, "https://example.com/article") == 0);
    assert(entries[2].identifier == entries[2].value);  /* URL: identifier == value */

    nostr_event_free(ev);
}

static void test_no_i_tags(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    assert(nostr_nip73_count(ev) == 0);

    NostrNip73Entry entries[4];
    size_t count = 99;
    assert(nostr_nip73_parse(ev, entries, 4, &count) == 0);
    assert(count == 0);

    nostr_event_free(ev);
}

static void test_build_tag(void) {
    NostrTag *tag = nostr_nip73_build_tag("isbn", "978-0-13-468599-1");
    assert(tag != NULL);
    assert(strcmp(nostr_tag_get(tag, 0), "i") == 0);
    assert(strcmp(nostr_tag_get(tag, 1), "isbn:978-0-13-468599-1") == 0);
    nostr_tag_free(tag);
}

static void test_build_url_tag(void) {
    NostrTag *tag = nostr_nip73_build_url_tag("https://example.com");
    assert(tag != NULL);
    assert(strcmp(nostr_tag_get(tag, 0), "i") == 0);
    assert(strcmp(nostr_tag_get(tag, 1), "https://example.com") == 0);
    nostr_tag_free(tag);
}

static void test_add_to_event(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    assert(nostr_nip73_add(ev, "doi", "10.1000/xyz123") == 0);
    assert(nostr_nip73_count(ev) == 1);

    NostrNip73Entry entries[4];
    size_t count = 0;
    assert(nostr_nip73_parse(ev, entries, 4, &count) == 0);
    assert(count == 1);
    assert(entries[0].type == NOSTR_NIP73_DOI);
    assert(strcmp(entries[0].identifier, "10.1000/xyz123") == 0);

    nostr_event_free(ev);
}

static void test_invalid_inputs(void) {
    assert(nostr_nip73_count(NULL) == 0);

    NostrNip73Entry entries[2];
    size_t count;
    assert(nostr_nip73_parse(NULL, entries, 2, &count) == -EINVAL);

    assert(nostr_nip73_build_tag(NULL, "x") == NULL);
    assert(nostr_nip73_build_tag("x", NULL) == NULL);
    assert(nostr_nip73_build_url_tag(NULL) == NULL);

    assert(nostr_nip73_add(NULL, "isbn", "x") == -EINVAL);
}

int main(void) {
    test_detect_type();
    test_type_from_string();
    test_type_to_string();
    test_is_media_type();
    test_is_reference_type();
    test_parse_entries();
    test_no_i_tags();
    test_build_tag();
    test_build_url_tag();
    test_add_to_event();
    test_invalid_inputs();
    printf("nip73 ok\n");
    return 0;
}
