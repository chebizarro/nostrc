/* G01 startup-only regression against the real service. The broad lifecycle
 * and policy suite belongs to G05 (nostrc-u6v55.6), not this test. */
#include <glib.h>
#include "bunker_service.h"

static const gchar *identity =
    "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";

typedef struct {
    guint errors;
    const gchar *expected_error;
} StateEvidence;

static void state_changed(BunkerState state, const gchar *error, gpointer data)
{
    StateEvidence *evidence = data;
    g_assert_cmpint(state, !=, BUNKER_STATE_RUNNING);
    if (state == BUNKER_STATE_ERROR) {
        evidence->errors++;
        g_assert_cmpstr(error, ==, evidence->expected_error);
    }
}

static void test_no_relays(void)
{
    BunkerService *service = bunker_service_new();
    StateEvidence evidence = {0, "No bunker relays configured"};
    const gchar *empty[] = {NULL};
    bunker_service_set_state_callback(service, state_changed, &evidence);
    g_assert_false(bunker_service_start(service, NULL, identity));
    g_assert_cmpint(bunker_service_get_state(service), ==, BUNKER_STATE_ERROR);
    g_assert_false(bunker_service_start(service, empty, identity));
    g_assert_cmpint(bunker_service_get_state(service), ==, BUNKER_STATE_ERROR);
    g_assert_cmpuint(evidence.errors, ==, 2);
    bunker_service_free(service);
}

static void test_listener_failure(void)
{
    BunkerService *service = bunker_service_new();
    StateEvidence evidence = {0, "Failed to start bunker listener"};
    const gchar *relays[] = {"ws://127.0.0.1:1/", NULL};
    bunker_service_set_state_callback(service, state_changed, &evidence);
    /* G00 proved this real wrapper has no transport key. Retrying must not
     * retain a failed listener or publish a RUNNING transition. */
    for (guint i = 0; i < 2; i++) {
        g_assert_false(bunker_service_start(service, relays, identity));
        g_assert_cmpint(bunker_service_get_state(service), ==, BUNKER_STATE_ERROR);
    }
    g_assert_cmpuint(evidence.errors, ==, 2);
    bunker_service_free(service);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_assert_cmpstr(g_getenv("NOSTRC_TEST_ISOLATION"), ==, "1");
    g_test_add_func("/bunker-startup/no-relays", test_no_relays);
    g_test_add_func("/bunker-startup/listener-failure", test_listener_failure);
    return g_test_run();
}
