#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr/nip28/nip28.h"

static void test_create_channel(void) {
    NostrEvent *ev = nostr_event_new();

    const char *meta = "{\"name\":\"test\",\"about\":\"A test channel\",\"picture\":\"https://example.com/pic.png\"}";
    assert(nostr_nip28_create_channel(ev, meta) == 0);
    assert(nostr_event_get_kind(ev) == NOSTR_NIP28_KIND_CHANNEL_CREATE);
    assert(strcmp(nostr_event_get_content(ev), meta) == 0);
    assert(nostr_nip28_is_channel_event(ev));

    nostr_event_free(ev);
}

static void test_create_channel_metadata(void) {
    NostrEvent *ev = nostr_event_new();

    const char *meta = "{\"name\":\"updated\"}";
    assert(nostr_nip28_create_channel_metadata(ev, "abc123", "wss://relay.example.com", meta) == 0);
    assert(nostr_event_get_kind(ev) == NOSTR_NIP28_KIND_CHANNEL_METADATA);
    assert(strcmp(nostr_event_get_content(ev), meta) == 0);

    /* Parse channel reference */
    NostrNip28ChannelRef ref;
    assert(nostr_nip28_parse_channel_ref(ev, &ref) == 0);
    assert(strcmp(ref.channel_id, "abc123") == 0);
    assert(strcmp(ref.relay, "wss://relay.example.com") == 0);

    nostr_event_free(ev);
}

static void test_create_channel_metadata_no_relay(void) {
    NostrEvent *ev = nostr_event_new();

    assert(nostr_nip28_create_channel_metadata(ev, "abc123", NULL, "{}") == 0);

    NostrNip28ChannelRef ref;
    assert(nostr_nip28_parse_channel_ref(ev, &ref) == 0);
    assert(strcmp(ref.channel_id, "abc123") == 0);
    assert(ref.relay == NULL);

    nostr_event_free(ev);
}

static void test_create_message(void) {
    NostrEvent *ev = nostr_event_new();

    assert(nostr_nip28_create_message(ev, "channel_id",
        "wss://relay.example.com", "Hello world!", NULL) == 0);
    assert(nostr_event_get_kind(ev) == NOSTR_NIP28_KIND_CHANNEL_MESSAGE);
    assert(strcmp(nostr_event_get_content(ev), "Hello world!") == 0);

    NostrNip28MessageRef ref;
    assert(nostr_nip28_parse_message_ref(ev, &ref) == 0);
    assert(strcmp(ref.channel_id, "channel_id") == 0);
    assert(strcmp(ref.channel_relay, "wss://relay.example.com") == 0);
    assert(ref.reply_to == NULL);

    nostr_event_free(ev);
}

static void test_create_message_with_reply(void) {
    NostrEvent *ev = nostr_event_new();

    assert(nostr_nip28_create_message(ev, "channel_id",
        "wss://relay.example.com", "Replying!", "prev_msg_id") == 0);

    NostrNip28MessageRef ref;
    assert(nostr_nip28_parse_message_ref(ev, &ref) == 0);
    assert(strcmp(ref.channel_id, "channel_id") == 0);
    assert(ref.reply_to != NULL);
    assert(strcmp(ref.reply_to, "prev_msg_id") == 0);
    assert(strcmp(ref.reply_relay, "wss://relay.example.com") == 0);

    nostr_event_free(ev);
}

static void test_parse_message_no_marker_fallback(void) {
    /* Test fallback: first e tag without marker = root */
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, NOSTR_NIP28_KIND_CHANNEL_MESSAGE);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("e", "channel_abc",
        "wss://relay.example.com", NULL));

    NostrNip28MessageRef ref;
    assert(nostr_nip28_parse_message_ref(ev, &ref) == 0);
    assert(strcmp(ref.channel_id, "channel_abc") == 0);
    assert(ref.reply_to == NULL);

    nostr_event_free(ev);
}

static void test_create_hide_message(void) {
    NostrEvent *ev = nostr_event_new();

    assert(nostr_nip28_create_hide_message(ev, "bad_msg_id", "spam") == 0);
    assert(nostr_event_get_kind(ev) == NOSTR_NIP28_KIND_HIDE_MESSAGE);
    assert(strstr(nostr_event_get_content(ev), "spam") != NULL);

    const char *hidden = nostr_nip28_get_hidden_message_id(ev);
    assert(hidden != NULL);
    assert(strcmp(hidden, "bad_msg_id") == 0);

    nostr_event_free(ev);
}

static void test_create_hide_message_no_reason(void) {
    NostrEvent *ev = nostr_event_new();

    assert(nostr_nip28_create_hide_message(ev, "msg_id", NULL) == 0);
    assert(strcmp(nostr_event_get_content(ev), "") == 0);

    nostr_event_free(ev);
}

static void test_create_mute_user(void) {
    NostrEvent *ev = nostr_event_new();

    assert(nostr_nip28_create_mute_user(ev, "spammer_pk", "trolling") == 0);
    assert(nostr_event_get_kind(ev) == NOSTR_NIP28_KIND_MUTE_USER);
    assert(strstr(nostr_event_get_content(ev), "trolling") != NULL);

    const char *muted = nostr_nip28_get_muted_pubkey(ev);
    assert(muted != NULL);
    assert(strcmp(muted, "spammer_pk") == 0);

    nostr_event_free(ev);
}

static void test_create_mute_user_no_reason(void) {
    NostrEvent *ev = nostr_event_new();

    assert(nostr_nip28_create_mute_user(ev, "pk", NULL) == 0);
    assert(strcmp(nostr_event_get_content(ev), "") == 0);

    nostr_event_free(ev);
}

static void test_is_channel_event(void) {
    NostrEvent *ev = nostr_event_new();

    nostr_event_set_kind(ev, 40);
    assert(nostr_nip28_is_channel_event(ev));
    nostr_event_set_kind(ev, 42);
    assert(nostr_nip28_is_channel_event(ev));
    nostr_event_set_kind(ev, 44);
    assert(nostr_nip28_is_channel_event(ev));

    nostr_event_set_kind(ev, 1);
    assert(!nostr_nip28_is_channel_event(ev));
    nostr_event_set_kind(ev, 39);
    assert(!nostr_nip28_is_channel_event(ev));
    nostr_event_set_kind(ev, 45);
    assert(!nostr_nip28_is_channel_event(ev));

    nostr_event_free(ev);
}

static void test_invalid_inputs(void) {
    assert(!nostr_nip28_is_channel_event(NULL));
    assert(nostr_nip28_get_hidden_message_id(NULL) == NULL);
    assert(nostr_nip28_get_muted_pubkey(NULL) == NULL);

    NostrNip28ChannelRef cref;
    assert(nostr_nip28_parse_channel_ref(NULL, &cref) == -EINVAL);

    NostrNip28MessageRef mref;
    assert(nostr_nip28_parse_message_ref(NULL, &mref) == -EINVAL);

    assert(nostr_nip28_create_channel(NULL, "{}") == -EINVAL);
    assert(nostr_nip28_create_message(NULL, "a", "b", "c", NULL) == -EINVAL);
    assert(nostr_nip28_create_hide_message(NULL, "a", NULL) == -EINVAL);
    assert(nostr_nip28_create_mute_user(NULL, "a", NULL) == -EINVAL);
}

static void test_parse_channel_ref_no_e_tag(void) {
    NostrEvent *ev = nostr_event_new();

    NostrNip28ChannelRef ref;
    assert(nostr_nip28_parse_channel_ref(ev, &ref) == -ENOENT);

    nostr_event_free(ev);
}

int main(void) {
    test_create_channel();
    test_create_channel_metadata();
    test_create_channel_metadata_no_relay();
    test_create_message();
    test_create_message_with_reply();
    test_parse_message_no_marker_fallback();
    test_create_hide_message();
    test_create_hide_message_no_reason();
    test_create_mute_user();
    test_create_mute_user_no_reason();
    test_is_channel_event();
    test_invalid_inputs();
    test_parse_channel_ref_no_e_tag();
    printf("nip28 ok\n");
    return 0;
}
