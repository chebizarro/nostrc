#include "gh-relay-scope.h"
#include "fake-auth-signer.h"
#include "wire-relay.h"
#include "wire-tor.h"

typedef struct {
  guint errors;
  guint eoses;
  guint events;
  guint closed;
  guint auth;
  gchar *eose_url;
  gchar *closed_reason;
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
  if (update->notice == GH_RELAY_NOTICE_AUTH)
    updates->auth++;
  if (update->notice == GH_RELAY_NOTICE_CLOSED) {
    updates->closed++;
    g_free(updates->closed_reason);
    updates->closed_reason = g_strdup(update->detail);
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

/* ---- NIP-42 against a real relay that requires AUTH for REQ ---- */

/* The scope always has the account signer; @mode is the URL's identity. */
static GhRelayScope *
auth_scope(WireRelay *relay, WireUpdates *updates, FakeSigner *fake,
           GhRelayAuthMode mode)
{
  GhRelayScope *scope = gh_relay_scope_new(11, any_filters(), on_update, updates);
  g_autoptr(GhRelayAuthSigner) signer = fake_signer_new(fake, 11, NULL);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_relay_scope_set_account_signer(scope, signer, &error));
  g_assert_true(gh_relay_scope_add_url(scope, relay->url, NULL));
  g_assert_true(gh_relay_scope_set_url_auth(scope, relay->url, mode, &error));
  g_assert_no_error(error);
  gh_relay_scope_start(scope);
  return scope;
}

static void
wire_updates_clear(WireUpdates *updates)
{
  g_free(updates->eose_url);
  g_free(updates->closed_reason);
}

/* ACCOUNT on the account's own relay: the refused REQ is served after one
 * AUTH, which the relay checked (signature, challenge, relay tag) and which
 * carries the account key; the scope reports EOSE, not CLOSED. */
static void
test_wire_auth_account_serves_req(void)
{
  WireRelay relay = { .require_auth = TRUE };
  relay_init(&relay);
  WireUpdates updates = {0};
  FakeSigner fake = { .mode = FAKE_SIGN_OK };
  GhRelayScope *scope = auth_scope(&relay, &updates, &fake, GH_RELAY_AUTH_ACCOUNT);
  wait_for_count(&updates.eoses, 1);
  g_assert_cmpuint(relay.closed_reqs, ==, 1);
  g_assert_cmpuint(relay.auth_frames, ==, 1);
  g_assert_cmpuint(relay.auth_ok, ==, 1);
  g_assert_cmpuint(relay.reqs, ==, 2);
  g_assert_cmpuint(fake.calls, ==, 1);
  g_assert_cmpuint(updates.closed, ==, 0);
  g_assert_cmpuint(updates.auth, ==, 1);
  g_assert_cmpuint(relay.connections->len, ==, 1); /* same connection */
  g_autofree gchar *account = fake_account_pubkey();
  g_assert_cmpstr(g_ptr_array_index(relay.auth_pubkeys, 0), ==, account);
  gh_relay_scope_cancel(scope);
  gh_relay_scope_unref(scope);
  relay_clear(&relay);
  fake_signer_clear(&fake);
  wire_updates_clear(&updates);
}

/* EPHEMERAL: two connections to the relay (two scopes) authenticate with
 * two unrelated keys, neither the account's, and are both served. */
static void
test_wire_auth_ephemeral_per_connection(void)
{
  WireRelay relay = { .require_auth = TRUE };
  relay_init(&relay);
  WireUpdates first_updates = {0}, second_updates = {0};
  FakeSigner fake = { .mode = FAKE_SIGN_OK };
  GhRelayScope *first = auth_scope(&relay, &first_updates, &fake, GH_RELAY_AUTH_EPHEMERAL);
  GhRelayScope *second = auth_scope(&relay, &second_updates, &fake, GH_RELAY_AUTH_EPHEMERAL);
  wait_for_count(&first_updates.eoses, 1);
  wait_for_count(&second_updates.eoses, 1);
  g_assert_cmpuint(relay.connections->len, ==, 2);
  g_assert_cmpuint(relay.auth_ok, ==, 2);
  g_assert_cmpuint(fake.calls, ==, 0);
  g_autofree gchar *account = fake_account_pubkey();
  const gchar *a = g_ptr_array_index(relay.auth_pubkeys, 0);
  const gchar *b = g_ptr_array_index(relay.auth_pubkeys, 1);
  g_assert_cmpstr(a, !=, b);
  g_assert_cmpstr(a, !=, account);
  g_assert_cmpstr(b, !=, account);
  gh_relay_scope_cancel(first);
  gh_relay_scope_cancel(second);
  gh_relay_scope_unref(first);
  gh_relay_scope_unref(second);
  relay_clear(&relay);
  fake_signer_clear(&fake);
  wire_updates_clear(&first_updates);
  wire_updates_clear(&second_updates);
}

/* Refused signing, a signed event rejected locally, the relay refusing an
 * ephemeral AUTH, and the default NONE each surface the relay's CLOSED;
 * nothing unverified is ever sent (the relay asserts on it), and under
 * NONE nothing is signed at all even though an account signer exists. */
static void
test_wire_auth_failures_surface_closed(void)
{
  static const struct {
    GhRelayAuthMode mode;
    FakeSignMode sign;
    gboolean refuse_auth;
    guint auth_frames;
    guint sign_calls;
  } cases[] = {
    { GH_RELAY_AUTH_ACCOUNT, FAKE_SIGN_DENY, FALSE, 0, 1 },
    { GH_RELAY_AUTH_ACCOUNT, FAKE_SIGN_WRONG_CHALLENGE, FALSE, 0, 1 },
    { GH_RELAY_AUTH_EPHEMERAL, FAKE_SIGN_OK, TRUE, 1, 0 },
    { GH_RELAY_AUTH_NONE, FAKE_SIGN_OK, FALSE, 0, 0 },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
    WireRelay relay = { .require_auth = TRUE, .refuse_auth = cases[i].refuse_auth };
    relay_init(&relay);
    WireUpdates updates = {0};
    FakeSigner fake = { .mode = cases[i].sign };
    GhRelayScope *scope = auth_scope(&relay, &updates, &fake, cases[i].mode);
    wait_for_count(&updates.closed, 1);
    g_assert_cmpstr(updates.closed_reason, ==, "auth-required: members only");
    g_assert_cmpuint(relay.auth_frames, ==, cases[i].auth_frames);
    g_assert_cmpuint(relay.auth_ok, ==, 0);
    g_assert_cmpuint(relay.reqs, ==, 1);
    g_assert_cmpuint(updates.eoses, ==, 0);
    g_assert_cmpuint(fake.calls, ==, cases[i].sign_calls);
    gh_relay_scope_cancel(scope);
    gh_relay_scope_unref(scope);
    relay_clear(&relay);
    fake_signer_clear(&fake);
    wire_updates_clear(&updates);
  }
}

/* Cancelled while the account signer is still working: nothing reaches the
 * relay, and the late signature is dropped. */
static void
test_wire_auth_cancel_while_signing(void)
{
  WireRelay relay = { .require_auth = TRUE };
  relay_init(&relay);
  WireUpdates updates = {0};
  FakeSigner fake = { .mode = FAKE_SIGN_HOLD };
  GhRelayScope *scope = auth_scope(&relay, &updates, &fake, GH_RELAY_AUTH_ACCOUNT);
  wait_for_count(&fake.calls, 1);
  gh_relay_scope_cancel(scope);
  g_assert_true(fake_signer_release(&fake));
  wait_for_close(&relay, 1);
  drain_pending();
  g_assert_cmpuint(relay.auth_frames, ==, 0);
  g_assert_cmpuint(updates.closed, ==, 0);
  g_assert_cmpuint(updates.eoses, ==, 0);
  gh_relay_scope_unref(scope);
  relay_clear(&relay);
  fake_signer_clear(&fake);
  wire_updates_clear(&updates);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  static const WireCase cases[] = {
    { "/groundhog/relay/wire-auth/account-serves-req", test_wire_auth_account_serves_req, FALSE },
    { "/groundhog/relay/wire-auth/ephemeral-per-connection", test_wire_auth_ephemeral_per_connection, FALSE },
    { "/groundhog/relay/wire-auth/failures-surface-closed", test_wire_auth_failures_surface_closed, FALSE },
    { "/groundhog/relay/wire-auth/cancel-while-signing", test_wire_auth_cancel_while_signing, FALSE },
    { "/groundhog/relay/wire-destinations", test_wire_destinations, FALSE },
    { "/groundhog/relay/offline-at-start", test_offline_at_start_reconnect, FALSE },
    { "/groundhog/relay/same-url-scopes-isolated", test_same_url_scopes_isolated, FALSE },
    { "/groundhog/relay/cancel-closes-subscription", test_cancel_closes_relay_subscription, FALSE },
  };
  wire_add_tests(cases, G_N_ELEMENTS(cases));
  return g_test_run();
}
