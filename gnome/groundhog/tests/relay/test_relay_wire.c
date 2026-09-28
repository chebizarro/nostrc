#include "gh-relay-scope.h"
#include "wire-relay.h"

typedef struct {
  guint errors;
  guint eoses;
  guint events;
  gchar *eose_url;
} WireUpdates;

static void
on_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  (void)scope;
  WireUpdates *updates = data;
  if (update->notice == GH_RELAY_NOTICE_ERROR)
    updates->errors++;
  if (update->notice == GH_RELAY_NOTICE_EVENT)
    updates->events++;
  if (update->notice == GH_RELAY_NOTICE_EOSE) {
    updates->eoses++;
    g_free(updates->eose_url);
    updates->eose_url = g_strdup(update->url);
  }
}

static void
test_wire_destinations(void)
{
  WireRelay inbox = {0}, late = {0}, group = {0};
  relay_init(&inbox);
  relay_init(&late);
  relay_init(&group);
  NostrFilters *inbox_filters = nostr_filters_new();
  NostrFilter *inbox_filter = nostr_filter_new();
  g_assert_true(nostr_filters_add(inbox_filters, inbox_filter));
  nostr_filter_free(inbox_filter);
  NostrFilters *group_filters = nostr_filters_new();
  NostrFilter *group_filter = nostr_filter_new();
  g_assert_true(nostr_filters_add(group_filters, group_filter));
  nostr_filter_free(group_filter);
  GhRelayScope *inbox_scope = gh_relay_scope_new(1, inbox_filters, NULL, NULL);
  GhRelayScope *group_scope = gh_relay_scope_new(1, group_filters, NULL, NULL);
  g_assert_true(gh_relay_scope_add_url(inbox_scope, inbox.url, NULL));
  g_assert_true(gh_relay_scope_add_url(group_scope, group.url, NULL));
  gh_relay_scope_start(inbox_scope);
  gh_relay_scope_start(group_scope);
  wait_for_reqs(&inbox, 1);
  wait_for_reqs(&group, 1);
  g_assert_cmpuint(late.reqs, ==, 0);
  g_assert_true(gh_relay_scope_add_url(inbox_scope, late.url, NULL));
  wait_for_reqs(&late, 1);
  g_assert_cmpuint(inbox.reqs, ==, 1);
  g_assert_cmpuint(group.reqs, ==, 1);
  gh_relay_scope_cancel(inbox_scope);
  gh_relay_scope_cancel(group_scope);
  gh_relay_scope_unref(inbox_scope);
  gh_relay_scope_unref(group_scope);
  relay_clear(&inbox);
  relay_clear(&late);
  relay_clear(&group);
}

static void
test_offline_at_start_reconnect(void)
{
  guint16 port = 0;
  WireRelay relay = { .url = unused_relay_url(&port) };
  WireUpdates updates = {0};
  NostrFilters *filters = nostr_filters_new();
  NostrFilter *filter = nostr_filter_new();
  g_assert_true(nostr_filters_add(filters, filter));
  nostr_filter_free(filter);
  GhRelayScope *scope = gh_relay_scope_new(7, filters, on_update, &updates);
  g_assert_true(gh_relay_scope_add_url(scope, relay.url, NULL));
  gh_relay_scope_start(scope);
  /* The first dial must fail before the server is made available. */
  wait_for_count(&updates.errors, 1);
  relay_init_port(&relay, port);
  wait_for_reqs(&relay, 1);
  wait_for_count(&updates.eoses, 1);
  g_assert_cmpstr(updates.eose_url, ==, relay.url);
  gh_relay_scope_cancel(scope);
  gh_relay_scope_unref(scope);
  relay_clear(&relay);
  g_free(updates.eose_url);
}

static NostrFilters *
any_filters(void)
{
  NostrFilters *filters = nostr_filters_new();
  NostrFilter *filter = nostr_filter_new();
  g_assert_true(nostr_filters_add(filters, filter));
  nostr_filter_free(filter);
  return filters;
}

/* Two scopes on one URL (e.g. the previous and next account) must not share
 * a socket: cancelling one must leave the other's live REQ delivering. */
static void
test_same_url_scopes_isolated(void)
{
  WireRelay relay = {0};
  relay_init(&relay);
  WireUpdates old_updates = {0}, new_updates = {0};
  GhRelayScope *old_scope = gh_relay_scope_new(1, any_filters(), on_update, &old_updates);
  GhRelayScope *new_scope = gh_relay_scope_new(2, any_filters(), on_update, &new_updates);
  g_assert_true(gh_relay_scope_add_url(old_scope, relay.url, NULL));
  g_assert_true(gh_relay_scope_add_url(new_scope, relay.url, NULL));
  gh_relay_scope_start(old_scope);
  gh_relay_scope_start(new_scope);
  wait_for_count(&old_updates.eoses, 1);
  wait_for_count(&new_updates.eoses, 1);
  g_assert_cmpuint(relay.connections->len, ==, 2);

  gh_relay_scope_cancel(old_scope);
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 1);
  nostr_event_set_created_at(event, 1700000000);
  nostr_event_set_content(event, "after cancel");
  g_assert_cmpint(nostr_event_sign(event,
    "0000000000000000000000000000000000000000000000000000000000000001"), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  for (guint i = 0; i < relay.connections->len; i++) {
    SoupWebsocketConnection *connection = g_ptr_array_index(relay.connections, i);
    const gchar *sub_id = g_object_get_data(G_OBJECT(connection), "sub-id");
    if (sub_id &&
        soup_websocket_connection_get_state(connection) == SOUP_WEBSOCKET_STATE_OPEN) {
      g_autofree gchar *frame = g_strdup_printf("[\"EVENT\",\"%s\",%s]", sub_id, json);
      soup_websocket_connection_send_text(connection, frame);
    }
  }
  free(json);
  wait_for_count(&new_updates.events, 1);
  g_assert_cmpuint(old_updates.events, ==, 0);

  gh_relay_scope_cancel(new_scope);
  gh_relay_scope_unref(old_scope);
  gh_relay_scope_unref(new_scope);
  relay_clear(&relay);
  g_free(old_updates.eose_url);
  g_free(new_updates.eose_url);
}

/* Cancellation must close the relay-side socket even if the queued Nostr
 * CLOSE loses the race with relay teardown. Requiring the real socket's closed
 * signal makes this fail against the former detach-only implementation. The
 * deadline is only a failure bound; WebSocket signals drive progress. */
static void
test_cancel_closes_relay_subscription(void)
{
  WireRelay relay = {0};
  relay_init(&relay);
  GhRelayScope *first = gh_relay_scope_new(1, any_filters(), NULL, NULL);
  GhRelayScope *second = gh_relay_scope_new(2, any_filters(), NULL, NULL);
  g_assert_true(gh_relay_scope_add_url(first, relay.url, NULL));
  g_assert_true(gh_relay_scope_add_url(second, relay.url, NULL));
  gh_relay_scope_start(first);
  gh_relay_scope_start(second);
  wait_for_reqs(&relay, 2);
  gh_relay_scope_cancel(first);
  gh_relay_scope_cancel(second);
  wait_for_close(&relay, 2);
  gh_relay_scope_unref(first);
  gh_relay_scope_unref(second);
  relay_clear(&relay);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/relay/wire-destinations", test_wire_destinations);
  g_test_add_func("/groundhog/relay/offline-at-start", test_offline_at_start_reconnect);
  g_test_add_func("/groundhog/relay/same-url-scopes-isolated", test_same_url_scopes_isolated);
  g_test_add_func("/groundhog/relay/cancel-closes-subscription", test_cancel_closes_relay_subscription);
  return g_test_run();
}
