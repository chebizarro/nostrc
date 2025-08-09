#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "json.h"
#include "event.h"
#include "tag.h"
#include "nostr-event.h"
#include "nostr-tag.h"

static NostrEvent *make_event_with_tags(void) {
    NostrEvent *e = nostr_event_new();
    e->kind = 1;
    e->created_at = 1234567890;
    e->pubkey = strdup("abcdef");
    e->content = strdup("hello");
    // tags: [["e","val1"],["p","val2","relay"]]
    Tag *t1 = nostr_tag_new("e", "val1", NULL);
    Tag *t2 = nostr_tag_new("p", "val2", "relay", NULL);
    Tags *ts = nostr_tags_new(0);
    ts = nostr_tags_append_unique(ts, t1);
    ts = nostr_tags_append_unique(ts, t2);
    e->tags = ts;
    return e;
}

int main(void) {
    // Ensure libjson is registered (set explicitly for robustness)
    extern NostrJsonInterface *jansson_impl;
    nostr_set_json_interface(jansson_impl);
    nostr_json_init();

    NostrEvent *e = make_event_with_tags();
    char *s = nostr_event_serialize(e);
    assert(s != NULL);

    NostrEvent *e2 = nostr_event_new();
    int rc = nostr_event_deserialize(e2, s);
    assert(rc == 0);
    assert(e2->kind == 1);
    assert(e2->created_at == 1234567890);
    assert(e2->pubkey && strcmp(e2->pubkey, "abcdef") == 0);
    assert(e2->content && strcmp(e2->content, "hello") == 0);
    assert(e2->tags && e2->tags->count == 2);

    free(s);
    nostr_event_free(e);
    nostr_event_free(e2);
    nostr_json_cleanup();
    printf("test_json_event OK\n");
    return 0;
}
