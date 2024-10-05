#include <glib.h>
#include "nostr_event.h"

static void test_nostr_event() {
    NostrEvent *event = nostr_event_new();
    nostr_event_set_id(event, "test_id");
    g_assert_cmpstr(nostr_event_get_id(event), ==, "test_id");

    nostr_event_set_pubkey(event, "test_pubkey");
    g_assert_cmpstr(nostr_event_get_pubkey(event), ==, "test_pubkey");

    nostr_event_set_created_at(event, 1234567890);
    g_assert_cmpint(nostr_event_get_created_at(event), ==, 1234567890);

    nostr_event_set_kind(event, 1);
    g_assert_cmpint(nostr_event_get_kind(event), ==, 1);

    nostr_event_set_content(event, "test_content");
    g_assert_cmpstr(nostr_event_get_content(event), ==, "test_content");

    nostr_event_set_sig(event, "test_sig");
    g_assert_cmpstr(nostr_event_get_sig(event), ==, "test_sig");

    g_object_unref(event);
}

int main(int argc, char *argv[]) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/nostr/event", test_nostr_event);
    return g_test_run();
}