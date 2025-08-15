#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr/nip31/nip31.h"

static void test_set_get_alt_basic(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    // initially not present
    char *alt = NULL;
    int rc = nostr_nip31_get_alt(ev, &alt);
    assert(rc == -ENOENT);
    assert(alt == NULL);

    // set and get
    assert(nostr_nip31_set_alt(ev, "An alternative description") == 0);
    rc = nostr_nip31_get_alt(ev, &alt);
    assert(rc == 0 && alt);
    assert(strcmp(alt, "An alternative description") == 0);
    free(alt);

    nostr_event_free(ev);
}

static void test_set_alt_replaces_existing(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    // add an unrelated tag and an alt tag
    NostrTags *tags = (NostrTags*)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("client", "gnostr", NULL));
    nostr_tags_append(tags, nostr_tag_new("alt", "old", NULL));

    // replace
    assert(nostr_nip31_set_alt(ev, "new") == 0);

    // ensure exactly one alt and client tag preserved
    tags = (NostrTags*)nostr_event_get_tags(ev);
    size_t n = nostr_tags_size(tags);
    int alt_count = 0, client_count = 0;
    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        const char *k = nostr_tag_get(t, 0);
        if (k && strcmp(k, "alt") == 0) {
            ++alt_count;
            assert(strcmp(nostr_tag_get(t, 1), "new") == 0);
        } else if (k && strcmp(k, "client") == 0) {
            ++client_count;
            assert(strcmp(nostr_tag_get(t, 1), "gnostr") == 0);
        }
    }
    assert(alt_count == 1);
    assert(client_count == 1);

    nostr_event_free(ev);
}

int main(void) {
    test_set_get_alt_basic();
    test_set_alt_replaces_existing();
    printf("nip31 ok\n");
    return 0;
}
