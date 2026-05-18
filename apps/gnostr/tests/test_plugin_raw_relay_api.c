#include "gnostr-plugin-api.h"

#include <glib.h>

typedef struct {
  gboolean called;
  gboolean saw_error;
} QueryCbState;

static void
on_query_done(GObject *source,
              GAsyncResult *result,
              gpointer user_data)
{
  (void)source;
  QueryCbState *state = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) events =
      gnostr_plugin_context_query_relays_finish(NULL, result, &error);

  state->called = TRUE;
  state->saw_error = (events == NULL && error != NULL);
}

static void
test_relay_event_copy_preserves_relay_url(void)
{
  GnostrPluginRelayEvent original = {
    .relay_url = (char *)"wss://relay-a.example",
    .event_json = (char *)"{\"id\":\"same\",\"kind\":9}"
  };

  GnostrPluginRelayEvent *copy = gnostr_plugin_relay_event_copy(&original);
  g_assert_nonnull(copy);
  g_assert_cmpstr(copy->relay_url, ==, "wss://relay-a.example");
  g_assert_cmpstr(copy->event_json, ==, original.event_json);
  gnostr_plugin_relay_event_free(copy);
}

static void
test_same_event_id_can_have_two_relay_provenances(void)
{
  g_autoptr(GPtrArray) events =
      g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_plugin_relay_event_free);

  GnostrPluginRelayEvent *a = g_new0(GnostrPluginRelayEvent, 1);
  a->relay_url = g_strdup("wss://relay-a.example");
  a->event_json = g_strdup("{\"id\":\"same\",\"kind\":9,\"tags\":[[\"h\",\"general\"]]}");
  g_ptr_array_add(events, a);

  GnostrPluginRelayEvent *b = g_new0(GnostrPluginRelayEvent, 1);
  b->relay_url = g_strdup("wss://relay-b.example");
  b->event_json = g_strdup(a->event_json);
  g_ptr_array_add(events, b);

  g_assert_cmpuint(events->len, ==, 2);
  g_assert_cmpstr(((GnostrPluginRelayEvent *)g_ptr_array_index(events, 0))->relay_url,
                  ==, "wss://relay-a.example");
  g_assert_cmpstr(((GnostrPluginRelayEvent *)g_ptr_array_index(events, 1))->relay_url,
                  ==, "wss://relay-b.example");
}

static void
test_invalid_query_reports_error_without_network(void)
{
  GnostrPluginContext *ctx = gnostr_plugin_context_new(NULL, "raw-relay-test");
  QueryCbState state = {0};
  GnostrPluginRelayQuery query = {0};

  gnostr_plugin_context_query_relays_async(ctx, &query, NULL, on_query_done, &state);

  g_assert_true(state.called);
  g_assert_true(state.saw_error);

  gnostr_plugin_context_free(ctx);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/gnostr/plugin-raw-relay/event-copy-preserves-relay-url",
                  test_relay_event_copy_preserves_relay_url);
  g_test_add_func("/gnostr/plugin-raw-relay/same-id-two-relays",
                  test_same_event_id_can_have_two_relay_provenances);
  g_test_add_func("/gnostr/plugin-raw-relay/invalid-query-error",
                  test_invalid_query_reports_error_without_network);

  return g_test_run();
}
