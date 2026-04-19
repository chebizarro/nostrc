#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr/nip22/nip22.h"

static void test_is_comment(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    /* Default kind is not a comment */
    assert(!nostr_nip22_is_comment(ev));

    /* Kind 1111 is a comment */
    nostr_event_set_kind(ev, 1111);
    assert(nostr_nip22_is_comment(ev));

    /* Other kinds are not comments */
    nostr_event_set_kind(ev, 1);
    assert(!nostr_nip22_is_comment(ev));

    nostr_event_free(ev);
}

static void test_get_thread_root_event(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);
    nostr_event_set_kind(ev, 1111);

    /* Add uppercase E tag (thread root) */
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("E", "abc123", "wss://relay.example.com", NULL));

    NostrNip22Ref root = nostr_nip22_get_thread_root(ev);
    assert(root.type == NOSTR_NIP22_REF_EVENT);
    assert(strcmp(root.value, "abc123") == 0);
    assert(root.relay != NULL);
    assert(strcmp(root.relay, "wss://relay.example.com") == 0);

    nostr_event_free(ev);
}

static void test_get_thread_root_addr(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);
    nostr_event_set_kind(ev, 1111);

    /* Add uppercase A tag (addressable root) */
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("A", "30023:pubkey123:my-article", "wss://relay.example.com", NULL));

    NostrNip22Ref root = nostr_nip22_get_thread_root(ev);
    assert(root.type == NOSTR_NIP22_REF_ADDR);
    assert(strcmp(root.value, "30023:pubkey123:my-article") == 0);

    nostr_event_free(ev);
}

static void test_get_thread_root_external(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);
    nostr_event_set_kind(ev, 1111);

    /* Add uppercase I tag (external content root — NIP-73) */
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("I", "podcast:guid:abc", NULL));

    NostrNip22Ref root = nostr_nip22_get_thread_root(ev);
    assert(root.type == NOSTR_NIP22_REF_EXTERNAL);
    assert(strcmp(root.value, "podcast:guid:abc") == 0);
    assert(root.relay == NULL);

    nostr_event_free(ev);
}

static void test_get_immediate_parent(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);
    nostr_event_set_kind(ev, 1111);

    /* Add root and parent tags */
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("E", "root123", NULL));
    nostr_tags_append(tags, nostr_tag_new("e", "parent456", "wss://relay.example.com", NULL));

    NostrNip22Ref parent = nostr_nip22_get_immediate_parent(ev);
    assert(parent.type == NOSTR_NIP22_REF_EVENT);
    assert(strcmp(parent.value, "parent456") == 0);
    assert(parent.relay != NULL);
    assert(strcmp(parent.relay, "wss://relay.example.com") == 0);

    /* Root should still work */
    NostrNip22Ref root = nostr_nip22_get_thread_root(ev);
    assert(root.type == NOSTR_NIP22_REF_EVENT);
    assert(strcmp(root.value, "root123") == 0);

    nostr_event_free(ev);
}

static void test_no_references(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);
    nostr_event_set_kind(ev, 1111);

    /* No tags at all */
    NostrNip22Ref root = nostr_nip22_get_thread_root(ev);
    assert(root.type == NOSTR_NIP22_REF_NONE);
    assert(root.value == NULL);

    NostrNip22Ref parent = nostr_nip22_get_immediate_parent(ev);
    assert(parent.type == NOSTR_NIP22_REF_NONE);
    assert(parent.value == NULL);

    nostr_event_free(ev);
}

static void test_get_root_kind(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);
    nostr_event_set_kind(ev, 1111);

    /* Add K tag for root kind */
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("K", "30023", NULL));

    int kind = -1;
    assert(nostr_nip22_get_root_kind(ev, &kind) == 0);
    assert(kind == 30023);

    nostr_event_free(ev);
}

static void test_get_parent_kind(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);
    nostr_event_set_kind(ev, 1111);

    /* Add k tag for parent kind */
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("k", "1111", NULL));

    int kind = -1;
    assert(nostr_nip22_get_parent_kind(ev, &kind) == 0);
    assert(kind == 1111);

    nostr_event_free(ev);
}

static void test_no_kind_tags(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    int kind = -1;
    assert(nostr_nip22_get_root_kind(ev, &kind) == -ENOENT);
    assert(nostr_nip22_get_parent_kind(ev, &kind) == -ENOENT);

    nostr_event_free(ev);
}

static void test_create_comment_basic(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    /* Create a direct comment on a kind-1 note */
    int rc = nostr_nip22_create_comment(ev, "Great post!",
                                         "rootid123", "wss://relay.example.com",
                                         1,      /* root kind */
                                         NULL, NULL,
                                         -1);    /* no parent kind */
    assert(rc == 0);
    assert(nostr_nip22_is_comment(ev));
    assert(strcmp(nostr_event_get_content(ev), "Great post!") == 0);

    /* Verify root reference */
    NostrNip22Ref root = nostr_nip22_get_thread_root(ev);
    assert(root.type == NOSTR_NIP22_REF_EVENT);
    assert(strcmp(root.value, "rootid123") == 0);
    assert(root.relay != NULL);
    assert(strcmp(root.relay, "wss://relay.example.com") == 0);

    /* No parent */
    NostrNip22Ref parent = nostr_nip22_get_immediate_parent(ev);
    assert(parent.type == NOSTR_NIP22_REF_NONE);

    /* Verify root kind */
    int kind = -1;
    assert(nostr_nip22_get_root_kind(ev, &kind) == 0);
    assert(kind == 1);

    nostr_event_free(ev);
}

static void test_create_comment_nested(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    /* Create a nested reply (comment on a comment) */
    int rc = nostr_nip22_create_comment(ev, "I agree!",
                                         "rootid123", NULL,
                                         1,           /* root is kind 1 */
                                         "parentid456", "wss://relay2.example.com",
                                         1111);       /* parent is kind 1111 */
    assert(rc == 0);

    /* Verify both root and parent */
    NostrNip22Ref root = nostr_nip22_get_thread_root(ev);
    assert(root.type == NOSTR_NIP22_REF_EVENT);
    assert(strcmp(root.value, "rootid123") == 0);

    NostrNip22Ref parent = nostr_nip22_get_immediate_parent(ev);
    assert(parent.type == NOSTR_NIP22_REF_EVENT);
    assert(strcmp(parent.value, "parentid456") == 0);

    /* Verify both kind tags */
    int rk = -1, pk = -1;
    assert(nostr_nip22_get_root_kind(ev, &rk) == 0);
    assert(rk == 1);
    assert(nostr_nip22_get_parent_kind(ev, &pk) == 0);
    assert(pk == 1111);

    nostr_event_free(ev);
}

static void test_empty_relay_treated_as_null(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);
    nostr_event_set_kind(ev, 1111);

    /* Add tag with empty relay string */
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("E", "abc123", "", NULL));

    NostrNip22Ref root = nostr_nip22_get_thread_root(ev);
    assert(root.type == NOSTR_NIP22_REF_EVENT);
    assert(strcmp(root.value, "abc123") == 0);
    assert(root.relay == NULL);  /* empty string → NULL */

    nostr_event_free(ev);
}

static void test_invalid_inputs(void) {
    /* NULL event */
    assert(!nostr_nip22_is_comment(NULL));

    NostrNip22Ref ref = nostr_nip22_get_thread_root(NULL);
    assert(ref.type == NOSTR_NIP22_REF_NONE);

    ref = nostr_nip22_get_immediate_parent(NULL);
    assert(ref.type == NOSTR_NIP22_REF_NONE);

    int kind = -1;
    assert(nostr_nip22_get_root_kind(NULL, &kind) == -EINVAL);
    assert(nostr_nip22_get_parent_kind(NULL, &kind) == -EINVAL);

    assert(nostr_nip22_create_comment(NULL, "test", NULL, NULL, 1, NULL, NULL, -1) == -EINVAL);

    /* NULL output pointer */
    NostrEvent *ev = nostr_event_new();
    assert(nostr_nip22_get_root_kind(ev, NULL) == -EINVAL);
    nostr_event_free(ev);
}

int main(void) {
    test_is_comment();
    test_get_thread_root_event();
    test_get_thread_root_addr();
    test_get_thread_root_external();
    test_get_immediate_parent();
    test_no_references();
    test_get_root_kind();
    test_get_parent_kind();
    test_no_kind_tags();
    test_create_comment_basic();
    test_create_comment_nested();
    test_empty_relay_treated_as_null();
    test_invalid_inputs();
    printf("nip22 ok\n");
    return 0;
}
