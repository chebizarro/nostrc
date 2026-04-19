#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr/nip39/nip39.h"

static void test_platform_from_string(void) {
    assert(nostr_nip39_platform_from_string("github") == NOSTR_NIP39_PLATFORM_GITHUB);
    assert(nostr_nip39_platform_from_string("twitter") == NOSTR_NIP39_PLATFORM_TWITTER);
    assert(nostr_nip39_platform_from_string("mastodon") == NOSTR_NIP39_PLATFORM_MASTODON);
    assert(nostr_nip39_platform_from_string("telegram") == NOSTR_NIP39_PLATFORM_TELEGRAM);
    assert(nostr_nip39_platform_from_string("keybase") == NOSTR_NIP39_PLATFORM_KEYBASE);
    assert(nostr_nip39_platform_from_string("dns") == NOSTR_NIP39_PLATFORM_DNS);
    assert(nostr_nip39_platform_from_string("reddit") == NOSTR_NIP39_PLATFORM_REDDIT);
    assert(nostr_nip39_platform_from_string("website") == NOSTR_NIP39_PLATFORM_WEBSITE);
    assert(nostr_nip39_platform_from_string("foobar") == NOSTR_NIP39_PLATFORM_UNKNOWN);
    assert(nostr_nip39_platform_from_string(NULL) == NOSTR_NIP39_PLATFORM_UNKNOWN);
}

static void test_platform_to_string(void) {
    assert(strcmp(nostr_nip39_platform_to_string(NOSTR_NIP39_PLATFORM_GITHUB), "github") == 0);
    assert(strcmp(nostr_nip39_platform_to_string(NOSTR_NIP39_PLATFORM_TWITTER), "twitter") == 0);
    assert(strcmp(nostr_nip39_platform_to_string(NOSTR_NIP39_PLATFORM_DNS), "dns") == 0);
    assert(strcmp(nostr_nip39_platform_to_string(NOSTR_NIP39_PLATFORM_UNKNOWN), "unknown") == 0);
}

static void test_detect_platform(void) {
    assert(nostr_nip39_detect_platform("github:jb55") == NOSTR_NIP39_PLATFORM_GITHUB);
    assert(nostr_nip39_detect_platform("twitter:jack") == NOSTR_NIP39_PLATFORM_TWITTER);
    assert(nostr_nip39_detect_platform("mastodon:user@server.social") == NOSTR_NIP39_PLATFORM_MASTODON);
    assert(nostr_nip39_detect_platform("dns:example.com") == NOSTR_NIP39_PLATFORM_DNS);
    assert(nostr_nip39_detect_platform("custom:something") == NOSTR_NIP39_PLATFORM_UNKNOWN);
    assert(nostr_nip39_detect_platform("nocolon") == NOSTR_NIP39_PLATFORM_UNKNOWN);
    assert(nostr_nip39_detect_platform(":empty") == NOSTR_NIP39_PLATFORM_UNKNOWN);
    assert(nostr_nip39_detect_platform(NULL) == NOSTR_NIP39_PLATFORM_UNKNOWN);
}

static void test_parse_identities(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);
    nostr_event_set_kind(ev, 0);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("i", "github:jb55",
        "https://gist.github.com/jb55/abc123/raw", NULL));
    nostr_tags_append(tags, nostr_tag_new("p", "deadbeef", NULL)); /* non-i tag */
    nostr_tags_append(tags, nostr_tag_new("i", "twitter:jack", NULL));
    nostr_tags_append(tags, nostr_tag_new("i", "mastodon:user@server.social",
        "https://server.social/@user/12345", NULL));

    assert(nostr_nip39_count(ev) == 3);

    NostrNip39Identity entries[8];
    size_t count = 0;
    assert(nostr_nip39_parse(ev, entries, 8, &count) == 0);
    assert(count == 3);

    /* GitHub with proof */
    assert(entries[0].platform == NOSTR_NIP39_PLATFORM_GITHUB);
    assert(strcmp(entries[0].value, "github:jb55") == 0);
    assert(strcmp(entries[0].identity, "jb55") == 0);
    assert(entries[0].proof_url != NULL);
    assert(strcmp(entries[0].proof_url, "https://gist.github.com/jb55/abc123/raw") == 0);

    /* Twitter without proof */
    assert(entries[1].platform == NOSTR_NIP39_PLATFORM_TWITTER);
    assert(strcmp(entries[1].identity, "jack") == 0);
    assert(entries[1].proof_url == NULL);

    /* Mastodon with proof */
    assert(entries[2].platform == NOSTR_NIP39_PLATFORM_MASTODON);
    assert(strcmp(entries[2].identity, "user@server.social") == 0);
    assert(entries[2].proof_url != NULL);

    nostr_event_free(ev);
}

static void test_parse_max_entries(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("i", "github:alice", NULL));
    nostr_tags_append(tags, nostr_tag_new("i", "twitter:alice", NULL));
    nostr_tags_append(tags, nostr_tag_new("i", "reddit:alice", NULL));

    /* Request only 2 entries */
    NostrNip39Identity entries[2];
    size_t count = 0;
    assert(nostr_nip39_parse(ev, entries, 2, &count) == 0);
    assert(count == 2);
    assert(entries[0].platform == NOSTR_NIP39_PLATFORM_GITHUB);
    assert(entries[1].platform == NOSTR_NIP39_PLATFORM_TWITTER);

    nostr_event_free(ev);
}

static void test_no_identities(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    assert(nostr_nip39_count(ev) == 0);

    NostrNip39Identity entries[4];
    size_t count = 99;
    assert(nostr_nip39_parse(ev, entries, 4, &count) == 0);
    assert(count == 0);

    nostr_event_free(ev);
}

static void test_malformed_tags_skipped(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    /* Missing colon - invalid identity format */
    nostr_tags_append(tags, nostr_tag_new("i", "nocolon", NULL));
    /* Empty identity after colon */
    nostr_tags_append(tags, nostr_tag_new("i", "github:", NULL));
    /* Valid one */
    nostr_tags_append(tags, nostr_tag_new("i", "github:valid", NULL));

    NostrNip39Identity entries[4];
    size_t count = 0;
    assert(nostr_nip39_parse(ev, entries, 4, &count) == 0);
    assert(count == 1);
    assert(strcmp(entries[0].identity, "valid") == 0);

    nostr_event_free(ev);
}

static void test_build_tag(void) {
    /* With proof URL */
    NostrTag *tag = nostr_nip39_build_tag("github", "jb55",
        "https://gist.github.com/jb55/abc/raw");
    assert(tag != NULL);
    assert(nostr_tag_size(tag) == 3);
    assert(strcmp(nostr_tag_get(tag, 0), "i") == 0);
    assert(strcmp(nostr_tag_get(tag, 1), "github:jb55") == 0);
    assert(strcmp(nostr_tag_get(tag, 2), "https://gist.github.com/jb55/abc/raw") == 0);
    nostr_tag_free(tag);

    /* Without proof URL */
    tag = nostr_nip39_build_tag("twitter", "jack", NULL);
    assert(tag != NULL);
    assert(nostr_tag_size(tag) == 2);
    assert(strcmp(nostr_tag_get(tag, 0), "i") == 0);
    assert(strcmp(nostr_tag_get(tag, 1), "twitter:jack") == 0);
    nostr_tag_free(tag);
}

static void test_build_tag_invalid(void) {
    assert(nostr_nip39_build_tag(NULL, "user", NULL) == NULL);
    assert(nostr_nip39_build_tag("github", NULL, NULL) == NULL);
}

static void test_add_to_event(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    assert(nostr_nip39_add(ev, "github", "alice",
        "https://gist.github.com/alice/proof/raw") == 0);
    assert(nostr_nip39_add(ev, "twitter", "alice", NULL) == 0);
    assert(nostr_nip39_count(ev) == 2);

    NostrNip39Identity entries[4];
    size_t count = 0;
    assert(nostr_nip39_parse(ev, entries, 4, &count) == 0);
    assert(count == 2);
    assert(entries[0].platform == NOSTR_NIP39_PLATFORM_GITHUB);
    assert(strcmp(entries[0].identity, "alice") == 0);
    assert(entries[0].proof_url != NULL);
    assert(entries[1].platform == NOSTR_NIP39_PLATFORM_TWITTER);
    assert(entries[1].proof_url == NULL);

    nostr_event_free(ev);
}

static void test_find_identity(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("i", "github:alice",
        "https://gist.github.com/alice/proof/raw", NULL));
    nostr_tags_append(tags, nostr_tag_new("i", "twitter:alice_t", NULL));

    NostrNip39Identity found;

    /* Find existing */
    assert(nostr_nip39_find(ev, "github", &found));
    assert(found.platform == NOSTR_NIP39_PLATFORM_GITHUB);
    assert(strcmp(found.identity, "alice") == 0);
    assert(found.proof_url != NULL);

    assert(nostr_nip39_find(ev, "twitter", &found));
    assert(strcmp(found.identity, "alice_t") == 0);
    assert(found.proof_url == NULL);

    /* Not found */
    assert(!nostr_nip39_find(ev, "mastodon", &found));
    assert(!nostr_nip39_find(ev, "reddit", &found));

    nostr_event_free(ev);
}

static void test_invalid_inputs(void) {
    assert(nostr_nip39_count(NULL) == 0);

    NostrNip39Identity entries[2];
    size_t count;
    assert(nostr_nip39_parse(NULL, entries, 2, &count) == -EINVAL);

    assert(nostr_nip39_add(NULL, "github", "x", NULL) == -EINVAL);

    NostrNip39Identity out;
    assert(!nostr_nip39_find(NULL, "github", &out));
}

static void test_unknown_platform_preserved(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("i", "bluesky:user.bsky.social", NULL));

    NostrNip39Identity entries[4];
    size_t count = 0;
    assert(nostr_nip39_parse(ev, entries, 4, &count) == 0);
    assert(count == 1);
    assert(entries[0].platform == NOSTR_NIP39_PLATFORM_UNKNOWN);
    assert(strcmp(entries[0].value, "bluesky:user.bsky.social") == 0);
    assert(strcmp(entries[0].identity, "user.bsky.social") == 0);

    nostr_event_free(ev);
}

int main(void) {
    test_platform_from_string();
    test_platform_to_string();
    test_detect_platform();
    test_parse_identities();
    test_parse_max_entries();
    test_no_identities();
    test_malformed_tags_skipped();
    test_build_tag();
    test_build_tag_invalid();
    test_add_to_event();
    test_find_identity();
    test_invalid_inputs();
    test_unknown_platform_preserved();
    printf("nip39 ok\n");
    return 0;
}
