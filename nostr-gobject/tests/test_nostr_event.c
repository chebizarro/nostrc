/**
 * Unit tests for GNostrEvent GObject wrapper
 */

#include <glib.h>
#include <nostr-gobject-1.0/nostr_event.h>

static void test_gnostr_event_properties(void) {
    g_autoptr(GNostrEvent) event = gnostr_event_new();
    g_assert_nonnull(event);

    /* Test created_at property */
    gnostr_event_set_created_at(event, 1234567890);
    g_assert_cmpint(gnostr_event_get_created_at(event), ==, 1234567890);

    /* Test kind property */
    gnostr_event_set_kind(event, 1);
    g_assert_cmpuint(gnostr_event_get_kind(event), ==, 1);

    /* Test content property */
    gnostr_event_set_content(event, "test_content");
    g_assert_cmpstr(gnostr_event_get_content(event), ==, "test_content");

    /* id, pubkey, sig are read-only (set by signing) */
    g_assert_null(gnostr_event_get_id(event));
    g_assert_null(gnostr_event_get_pubkey(event));
    g_assert_null(gnostr_event_get_sig(event));
}

static void test_gnostr_event_json_roundtrip(void) {
    /* Create an event with some data */
    g_autoptr(GNostrEvent) event = gnostr_event_new();
    g_assert_nonnull(event);

    gnostr_event_set_kind(event, 1);
    gnostr_event_set_content(event, "Hello, Nostr!");
    gnostr_event_set_created_at(event, 1700000000);

    /* Serialize to JSON */
    g_autofree gchar *json = gnostr_event_to_json(event);
    g_assert_nonnull(json);

    /* Deserialize from JSON */
    GError *error = NULL;
    g_autoptr(GNostrEvent) event2 = gnostr_event_new_from_json(json, &error);
    g_assert_no_error(error);
    g_assert_nonnull(event2);

    /* Verify properties match */
    g_assert_cmpuint(gnostr_event_get_kind(event2), ==, 1);
    g_assert_cmpstr(gnostr_event_get_content(event2), ==, "Hello, Nostr!");
    g_assert_cmpint(gnostr_event_get_created_at(event2), ==, 1700000000);
}

static void test_gnostr_event_invalid_json(void) {
    GError *error = NULL;
    GNostrEvent *event = gnostr_event_new_from_json("not valid json", &error);
    g_assert_null(event);
    g_assert_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED);
    g_clear_error(&error);
}

static void test_gnostr_event_sign_invalid_key(void) {
    g_autoptr(GNostrEvent) event = gnostr_event_new();
    gnostr_event_set_kind(event, 1);
    gnostr_event_set_content(event, "test");
    gnostr_event_set_created_at(event, 1700000000);

    GError *error = NULL;
    gboolean result = gnostr_event_sign(event, "short", &error);
    g_assert_false(result);
    g_assert_error(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_KEY);
    g_clear_error(&error);
}

static gboolean signed_signal_received = FALSE;

static void on_signed(GNostrEvent *event G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED) {
    signed_signal_received = TRUE;
}

static void test_gnostr_event_notify_signals(void) {
    g_autoptr(GNostrEvent) event = gnostr_event_new();
    gint notify_count = 0;

    /* Connect to notify signal for content property */
    g_signal_connect_swapped(event, "notify::content",
                             G_CALLBACK(g_atomic_int_inc), &notify_count);

    /* Setting content should emit notify */
    gnostr_event_set_content(event, "first");
    g_assert_cmpint(notify_count, ==, 1);

    /* Setting to same value should not emit notify */
    gnostr_event_set_content(event, "first");
    g_assert_cmpint(notify_count, ==, 1);

    /* Setting to different value should emit notify */
    gnostr_event_set_content(event, "second");
    g_assert_cmpint(notify_count, ==, 2);
}

int main(int argc, char *argv[]) {
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/gnostr/event/properties", test_gnostr_event_properties);
    g_test_add_func("/gnostr/event/json-roundtrip", test_gnostr_event_json_roundtrip);
    g_test_add_func("/gnostr/event/invalid-json", test_gnostr_event_invalid_json);
    g_test_add_func("/gnostr/event/sign-invalid-key", test_gnostr_event_sign_invalid_key);
    g_test_add_func("/gnostr/event/notify-signals", test_gnostr_event_notify_signals);

    return g_test_run();
}
