#include "gh-relay-scope.h"

#include <gio/gio.h>
#include <nostr-event.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
  GPtrArray *opened;
  GPtrArray *closed;
  guint events;
  guint eose;
  guint auth;
  guint closed_notices;
  guint ok;
  guint disconnected;
  gboolean last_backfill;
  gboolean last_accepted;
  gchar *last_url;
  gchar *last_detail;
} Fixture;

static gpointer
fake_open(GhRelayScope *scope, const gchar *url, const NostrFilters *filters,
          gpointer data, GError **error)
{
  (void)scope;
  (void)filters;
  (void)error;
  Fixture *fixture = data;
  g_ptr_array_add(fixture->opened, g_strdup(url));
  return g_strdup(url); /* one simulated REQ destination per open */
}

static void
fake_close(gpointer handle, gpointer data)
{
  Fixture *fixture = data;
  g_ptr_array_add(fixture->closed, g_strdup(handle));
  g_free(handle);
}

static const GhRelayTransport fake_transport = {
  .open = fake_open,
  .close = fake_close,
};

static void
on_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  (void)scope;
  Fixture *fixture = data;
  g_free(fixture->last_url);
  g_free(fixture->last_detail);
  fixture->last_url = g_strdup(update->url);
  fixture->last_detail = g_strdup(update->detail);
  switch (update->notice) {
  case GH_RELAY_NOTICE_EVENT:
    fixture->events++;
    fixture->last_backfill = update->backfill;
    g_assert_nonnull(update->event_id);
    break;
  case GH_RELAY_NOTICE_EOSE: fixture->eose++; break;
  case GH_RELAY_NOTICE_AUTH: fixture->auth++; break;
  case GH_RELAY_NOTICE_CLOSED: fixture->closed_notices++; break;
  case GH_RELAY_NOTICE_OK:
    fixture->ok++;
    fixture->last_accepted = update->accepted;
    break;
  case GH_RELAY_NOTICE_DISCONNECTED: fixture->disconnected++; break;
  case GH_RELAY_NOTICE_ERROR: g_assert_not_reached();
  }
}

static gchar *
signed_json(const gchar *content)
{
  NostrEvent *event = nostr_event_new();
  g_assert_nonnull(event);
  nostr_event_set_created_at(event, 1700000000);
  nostr_event_set_kind(event, 1);
  nostr_event_set_content(event, content);
  g_assert_cmpint(nostr_event_sign(event,
    "0000000000000000000000000000000000000000000000000000000000000001"), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  g_assert_nonnull(json);
  gchar *copy = g_strdup(json);
  free(json);
  nostr_event_free(event);
  return copy;
}

static void
fixture_clear(Fixture *fixture)
{
  g_ptr_array_unref(fixture->opened);
  g_ptr_array_unref(fixture->closed);
  g_free(fixture->last_url);
  g_free(fixture->last_detail);
}

static void
test_destinations_late_add_and_generation(void)
{
  Fixture dm = { .opened = g_ptr_array_new_with_free_func(g_free),
                 .closed = g_ptr_array_new_with_free_func(g_free) };
  Fixture group = { .opened = g_ptr_array_new_with_free_func(g_free),
                    .closed = g_ptr_array_new_with_free_func(g_free) };
  GhRelayScope *dm_scope = gh_relay_scope_new_with_transport(41,
    nostr_filters_new(), &fake_transport, &dm, on_update, &dm);
  GhRelayScope *group_scope = gh_relay_scope_new_with_transport(41,
    nostr_filters_new(), &fake_transport, &group, on_update, &group);
  g_assert_true(gh_relay_scope_add_url(dm_scope, "wss://nos.lol", NULL));
  g_assert_true(gh_relay_scope_add_url(group_scope, "wss://relay.nostr.band", NULL));
  gh_relay_scope_start(dm_scope);
  gh_relay_scope_start(group_scope);
  g_assert_cmpuint(dm.opened->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(dm.opened, 0), ==, "wss://nos.lol");
  g_assert_cmpuint(group.opened->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(group.opened, 0), ==,
                  "wss://relay.nostr.band");
  /* A relay added after the first REQ receives only this scope's REQ. */
  g_assert_true(gh_relay_scope_add_url(dm_scope, "wss://relay.example", NULL));
  g_assert_cmpuint(dm.opened->len, ==, 2);
  g_assert_cmpstr(g_ptr_array_index(dm.opened, 1), ==, "wss://relay.example");
  g_assert_cmpuint(group.opened->len, ==, 1);
  g_assert_true(gh_relay_scope_add_url(dm_scope, "wss://relay.example", NULL));
  g_assert_cmpuint(dm.opened->len, ==, 2);

  g_autofree gchar *first = signed_json("first");
  g_autofree gchar *second = signed_json("second");
  g_autofree gchar *third = signed_json("third");
  gh_relay_scope_event(dm_scope, "wss://relay.nostr.band", first);
  gh_relay_scope_event(dm_scope, "wss://nos.lol", "{bad json");
  g_assert_cmpuint(dm.events, ==, 0);
  gh_relay_scope_event(dm_scope, "wss://nos.lol", first);
  g_assert_cmpuint(dm.events, ==, 1);
  g_assert_true(dm.last_backfill);
  gh_relay_scope_event(dm_scope, "wss://relay.example", first);
  g_assert_cmpuint(dm.events, ==, 1); /* duplicate across two URLs */
  gh_relay_scope_eose(dm_scope, "wss://nos.lol");
  g_assert_cmpuint(dm.eose, ==, 1);
  gh_relay_scope_event(dm_scope, "wss://nos.lol", second);
  g_assert_cmpuint(dm.events, ==, 2);
  g_assert_false(dm.last_backfill);
  gh_relay_scope_eose(dm_scope, "wss://relay.example");
  g_assert_cmpuint(dm.eose, ==, 2);
  gh_relay_scope_notice(dm_scope, "wss://relay.example",
                         GH_RELAY_NOTICE_DISCONNECTED, NULL, FALSE, NULL);
  g_assert_cmpuint(dm.disconnected, ==, 1);
  gh_relay_scope_event(dm_scope, "wss://relay.example", third);
  g_assert_cmpuint(dm.events, ==, 3);
  g_assert_true(dm.last_backfill);
  gh_relay_scope_eose(dm_scope, "wss://relay.example");
  g_assert_cmpuint(dm.eose, ==, 3);
  gh_relay_scope_notice(dm_scope, "wss://nos.lol", GH_RELAY_NOTICE_AUTH,
                         NULL, FALSE, "auth needed");
  gh_relay_scope_notice(dm_scope, "wss://nos.lol", GH_RELAY_NOTICE_CLOSED,
                         NULL, FALSE, "restricted: denied");
  gh_relay_scope_notice(dm_scope, "wss://relay.example", GH_RELAY_NOTICE_OK,
                         "event-id", TRUE, "stored locally");
  g_assert_cmpuint(dm.auth, ==, 1);
  g_assert_cmpuint(dm.closed_notices, ==, 1);
  g_assert_cmpuint(dm.ok, ==, 1);
  g_assert_true(dm.last_accepted); /* explicitly relay-local, not upstream */
  g_assert_cmpstr(dm.last_detail, ==, "stored locally");
  gh_relay_scope_cancel(dm_scope);
  g_assert_cmpuint(gh_relay_scope_get_generation(dm_scope), ==, 42);
  g_assert_cmpuint(dm.closed->len, ==, 2);
  gh_relay_scope_event(dm_scope, "wss://nos.lol", first);
  gh_relay_scope_eose(dm_scope, "wss://nos.lol");
  gh_relay_scope_notice(dm_scope, "wss://nos.lol", GH_RELAY_NOTICE_OK,
                         "event-id", TRUE, NULL);
  g_assert_cmpuint(dm.events, ==, 3);
  g_assert_cmpuint(dm.eose, ==, 3);
  g_assert_cmpuint(dm.ok, ==, 1);
  g_autoptr(GError) error = NULL;
  g_assert_false(gh_relay_scope_add_url(dm_scope, "wss://new.example", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  gh_relay_scope_unref(dm_scope);
  gh_relay_scope_unref(group_scope);
  fixture_clear(&dm);
  fixture_clear(&group);
}

static void
test_url_bound(void)
{
  Fixture fixture = { .opened = g_ptr_array_new_with_free_func(g_free),
                      .closed = g_ptr_array_new_with_free_func(g_free) };
  GhRelayScope *scope = gh_relay_scope_new_with_transport(1,
    nostr_filters_new(), &fake_transport, &fixture, on_update, &fixture);
  g_autoptr(GError) error = NULL;
  const gchar *invalid[] = { "https://nos.lol", "ws://", "wss://user@nos.lol" };
  for (gsize i = 0; i < G_N_ELEMENTS(invalid); i++) {
    g_assert_false(gh_relay_scope_add_url(scope, invalid[i], &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
  }
  for (guint i = 0; i < 16; i++) {
    g_autofree gchar *url = g_strdup_printf("wss://relay%u.example", i);
    g_assert_true(gh_relay_scope_add_url(scope, url, &error));
  }
  g_assert_false(gh_relay_scope_add_url(scope, "wss://overflow.example", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
  gh_relay_scope_start(scope);
  g_assert_cmpuint(fixture.opened->len, ==, 16);
  gh_relay_scope_unref(scope);
  fixture_clear(&fixture);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/relay/destinations", test_destinations_late_add_and_generation);
  g_test_add_func("/groundhog/relay/url-bound", test_url_bound);
  return g_test_run();
}
