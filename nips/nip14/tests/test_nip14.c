#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr/nip14/nip14.h"

static void test_get_subject(void) {
    NostrEvent *ev = nostr_event_new();

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("subject", "Hello World", NULL));

    const char *subj = nostr_nip14_get_subject(ev);
    assert(subj != NULL);
    assert(strcmp(subj, "Hello World") == 0);

    nostr_event_free(ev);
}

static void test_no_subject(void) {
    NostrEvent *ev = nostr_event_new();

    assert(nostr_nip14_get_subject(ev) == NULL);
    assert(!nostr_nip14_has_subject(ev));

    nostr_event_free(ev);
}

static void test_has_subject(void) {
    NostrEvent *ev = nostr_event_new();

    assert(!nostr_nip14_has_subject(ev));

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("subject", "Test", NULL));

    assert(nostr_nip14_has_subject(ev));

    nostr_event_free(ev);
}

static void test_set_subject(void) {
    NostrEvent *ev = nostr_event_new();

    assert(nostr_nip14_set_subject(ev, "My Subject") == 0);
    assert(nostr_nip14_has_subject(ev));

    const char *subj = nostr_nip14_get_subject(ev);
    assert(subj != NULL);
    assert(strcmp(subj, "My Subject") == 0);

    nostr_event_free(ev);
}

static void test_subject_with_other_tags(void) {
    NostrEvent *ev = nostr_event_new();

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("p", "pubkey123", NULL));
    nostr_tags_append(tags, nostr_tag_new("subject", "Important", NULL));
    nostr_tags_append(tags, nostr_tag_new("e", "eventid", NULL));

    const char *subj = nostr_nip14_get_subject(ev);
    assert(subj != NULL);
    assert(strcmp(subj, "Important") == 0);

    nostr_event_free(ev);
}

static void test_invalid_inputs(void) {
    assert(nostr_nip14_get_subject(NULL) == NULL);
    assert(!nostr_nip14_has_subject(NULL));
    assert(nostr_nip14_set_subject(NULL, "x") == -EINVAL);

    NostrEvent *ev = nostr_event_new();
    assert(nostr_nip14_set_subject(ev, NULL) == -EINVAL);
    nostr_event_free(ev);
}

int main(void) {
    test_get_subject();
    test_no_subject();
    test_has_subject();
    test_set_subject();
    test_subject_with_other_tags();
    test_invalid_inputs();
    printf("nip14 ok\n");
    return 0;
}
