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
  GhTestHeldPort held = { 0 };
  WireRelay relay = { .url = held_relay_url(&held) };
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
  relay_init_held(&relay, &held);
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

/* nostrc-gem9, nostrc-f56o: a held port is never free. While down, nobody
 * else can bind it as libsoup and GSocketListener do (loopback,
 * SO_REUSEADDR), so a parallel test cannot take it; the relay then comes
 * up on it at the same URL. (A port reserved by binding and closing a
 * listener is free again at once: that was the flake.) */
static void
test_held_port(void)
{
  GhTestHeldPort held = { 0 };
  WireRelay relay = { .url = held_relay_url(&held) };
  g_autoptr(GError) error = NULL;
  g_autoptr(GSocket) stranger = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM,
                                             G_SOCKET_PROTOCOL_DEFAULT, &error);
  g_assert_no_error(error);
  g_autoptr(GInetAddress) loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
  g_autoptr(GSocketAddress) taken = g_inet_socket_address_new(loopback, held.port);
  g_assert_false(g_socket_bind(stranger, taken, TRUE, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_ADDRESS_IN_USE);
  relay_init_held(&relay, &held);
  g_autofree gchar *expected = g_strdup_printf("ws://127.0.0.1:%u/relay", held.port);
  g_assert_cmpstr(relay.url, ==, expected);
  relay_clear(&relay);
  g_assert_null(held.service);
}

static gboolean
count_incoming(GSocketService *service, GSocketConnection *connection, GObject *source,
               gpointer data)
{
  (void)service;
  (void)source;
  (*(guint *)data)++;
  (void)g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
  return TRUE;
}

/* nostrc-vzls: a test's own listener owns its port for dials to 127.0.0.1.
 * Another process's listener at 127.0.0.1 on a port it chose (bd starts
 * `dolt sql-server -H 127.0.0.1 -P <port>` for each beads workspace) never
 * shares it, and the dial reaches the test's. macOS hands out ephemeral
 * ports in order, so the ports just after the next one are taken first by
 * such listeners: a dual-stack [::] listener
 * (g_socket_listener_add_any_inet_port()) is given the first of them there,
 * and the dial goes to the holder. */
static void
test_loopback_listener(void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GInetAddress) loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
  g_autoptr(GSocketListener) probe = g_socket_listener_new();
  guint next = gh_test_listen_loopback(probe);
  g_socket_listener_close(probe);
  g_autoptr(GPtrArray) holders = g_ptr_array_new_with_free_func(g_object_unref);
  g_autoptr(GHashTable) held = g_hash_table_new(NULL, NULL);
  for (guint i = 1; i <= 64; i++) {
    guint candidate = next + i;
    guint16 port = (guint16)(candidate > G_MAXUINT16 ? 49152 + (candidate - 65536) : candidate);
    GSocket *holder = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM,
                                   G_SOCKET_PROTOCOL_DEFAULT, &error);
    g_assert_no_error(error);
    g_autoptr(GSocketAddress) address = g_inet_socket_address_new(loopback, port);
    if (g_socket_bind(holder, address, TRUE, NULL) && g_socket_listen(holder, NULL)) {
      g_hash_table_add(held, GUINT_TO_POINTER((guint)port));
      g_ptr_array_add(holders, holder);
    } else {
      g_object_unref(holder); /* someone else's already */
    }
  }
  g_assert_cmpuint(holders->len, >, 0);

  guint accepted = 0;
  g_autoptr(GSocketService) service = g_socket_service_new();
  guint16 port = gh_test_listen_loopback(G_SOCKET_LISTENER(service));
  g_assert_false(g_hash_table_contains(held, GUINT_TO_POINTER((guint)port)));
  g_signal_connect(service, "incoming", G_CALLBACK(count_incoming), &accepted);
  g_socket_service_start(service);
  g_autoptr(GSocketClient) client = g_socket_client_new();
  g_autofree gchar *host = g_strdup_printf("127.0.0.1:%u", port);
  g_autoptr(GSocketConnection) connection = g_socket_client_connect_to_host(client, host, 0, NULL,
                                                                            &error);
  g_assert_no_error(error);
  wait_for_count(&accepted, 1);

  g_signal_handlers_disconnect_by_data(service, &accepted);
  g_socket_service_stop(service);
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_socket_listener_close(G_SOCKET_LISTENER(service));
  for (guint i = 0; i < holders->len; i++)
    g_socket_close(g_ptr_array_index(holders, i), NULL);
}

/* ---- Backfill paging past a relay's cap (nostrc-cpwf) ---------------------------- */

#define PAGED_BACKLOG 300
#define PAGED_CAP 50

typedef struct {
  GHashTable *ids;         /* distinct event ids delivered */
  guint events;            /* EVENT notices, repeats included */
  guint live;              /* EVENT notices with backfill FALSE */
  guint at_eose;           /* distinct ids when the EOSE came */
  guint eoses;
  gboolean incomplete;
} PagedUpdates;

static void
on_paged_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  (void)scope;
  PagedUpdates *updates = data;
  switch (update->notice) {
  case GH_RELAY_NOTICE_EVENT:
    updates->events++;
    if (!update->backfill)
      updates->live++;
    g_hash_table_add(updates->ids, g_strdup(update->event_id));
    break;
  case GH_RELAY_NOTICE_EOSE:
    updates->eoses++;
    updates->incomplete = update->incomplete;
    updates->at_eose = g_hash_table_size(updates->ids);
    break;
  default:
    break;
  }
}

/* A signed kind 1, three to a second (so relay pages end inside a second). */
static gchar *
backlog_event(const gchar *key, guint i, gint64 base)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 1);
  nostr_event_set_created_at(event, base + i / 3);
  g_autofree gchar *content = g_strdup_printf("backlog %u", i);
  nostr_event_set_content(event, content);
  g_assert_cmpint(nostr_event_sign(event, key), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  gchar *copy = g_strdup(json);
  free(json);
  return copy;
}

/* The relay holds PAGED_BACKLOG events since `since` and answers any REQ with
 * at most PAGED_CAP of them per filter (newest first), whatever it asks.
 * @page_limit 0: no paging. Returns the scope, started and at its EOSE. */
static GhRelayScope *
paged_scope(WireRelay *relay, PagedUpdates *updates, guint page_limit, guint max_pages)
{
  relay->serve = TRUE;
  relay->max_limit = PAGED_CAP;
  relay_init(relay);
  char *key = nostr_key_generate_private();
  gint64 base = g_get_real_time() / G_USEC_PER_SEC - 3600;
  for (guint i = 0; i < PAGED_BACKLOG; i++) {
    g_autofree gchar *json = backlog_event(key, i, base);
    wire_relay_inject(relay, json);
  }
  free(key);
  NostrFilters *filters = nostr_filters_new();
  NostrFilter *filter = nostr_filter_new();
  const int kinds[] = { 1 };
  nostr_filter_set_kinds(filter, kinds, G_N_ELEMENTS(kinds));
  nostr_filter_set_since_i64(filter, base - 60);
  g_assert_true(nostr_filters_add(filters, filter));
  nostr_filter_free(filter);
  updates->ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  GhRelayScope *scope = gh_relay_scope_new(7, filters, on_paged_update, updates);
  if (page_limit)
    gh_relay_scope_set_backfill_paging(scope, page_limit, max_pages);
  g_assert_true(gh_relay_scope_add_url(scope, relay->url, NULL));
  gh_relay_scope_start(scope);
  wait_for_count(&updates->eoses, 1);
  return scope;
}

/* Paging reads the whole backlog behind a relay whose cap (50) is far below
 * the REQ limit (500), reports one EOSE after all of it (complete), keeps the
 * live REQ open, and every paged REQ keeps the filter's since. */
static void
test_paging_past_relay_cap(void)
{
  WireRelay relay = { .record = TRUE };
  PagedUpdates updates = { 0 };
  GhRelayScope *scope = paged_scope(&relay, &updates, 500, 64);
  g_assert_cmpuint(updates.at_eose, ==, PAGED_BACKLOG);
  g_assert_false(updates.incomplete);
  g_assert_cmpuint(updates.live, ==, 0);
  /* Six full pages and a short one; the first REQ is the live one. */
  g_assert_cmpuint(relay.reqs, >=, 1 + PAGED_BACKLOG / PAGED_CAP);
  g_assert_cmpuint(relay.reqs, <=, 2 + PAGED_BACKLOG / PAGED_CAP + 1);
  guint with_until = 0;
  for (guint i = 0; i < relay.frames->len; i++) {
    WireFrame *frame = g_ptr_array_index(relay.frames, i);
    if (!frame->inbound || !g_str_has_prefix(frame->text, "[\"REQ\""))
      continue;
    g_assert_nonnull(strstr(frame->text, "\"since\":"));
    g_assert_nonnull(strstr(frame->text, "\"limit\":500"));
    with_until += strstr(frame->text, "\"until\":") != NULL;
  }
  g_assert_cmpuint(with_until, ==, relay.reqs - 1);

  /* The live REQ is still open: a new event arrives, not as backfill. */
  char *key = nostr_key_generate_private();
  g_autofree gchar *json = backlog_event(key, 0, g_get_real_time() / G_USEC_PER_SEC);
  free(key);
  wire_relay_inject(&relay, json);
  wait_for_count(&updates.live, 1);
  g_assert_cmpuint(g_hash_table_size(updates.ids), ==, PAGED_BACKLOG + 1);
  g_assert_cmpuint(updates.eoses, ==, 1);
  gh_relay_scope_cancel(scope);
  gh_relay_scope_unref(scope);
  relay_clear(&relay);
  g_hash_table_unref(updates.ids);
}

/* Without paging the same relay gives only its newest 50 (the bug), and a
 * page budget too small for the backlog ends with an incomplete EOSE. */
static void
test_paging_off_or_exhausted(void)
{
  WireRelay relay = { 0 };
  PagedUpdates updates = { 0 };
  GhRelayScope *scope = paged_scope(&relay, &updates, 0, 0);
  g_assert_cmpuint(updates.at_eose, ==, PAGED_CAP);
  g_assert_false(updates.incomplete);
  gh_relay_scope_cancel(scope);
  gh_relay_scope_unref(scope);
  relay_clear(&relay);
  g_hash_table_unref(updates.ids);

  WireRelay budget = { 0 };
  PagedUpdates short_updates = { 0 };
  scope = paged_scope(&budget, &short_updates, 500, 2);
  g_assert_true(short_updates.incomplete);
  g_assert_cmpuint(budget.reqs, ==, 3);
  g_assert_cmpuint(short_updates.at_eose, >, 2 * PAGED_CAP);
  g_assert_cmpuint(short_updates.at_eose, <=, 3 * PAGED_CAP);
  gh_relay_scope_cancel(scope);
  gh_relay_scope_unref(scope);
  relay_clear(&budget);
  g_hash_table_unref(short_updates.ids);
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
    { "/groundhog/relay/paging/past-relay-cap", test_paging_past_relay_cap, FALSE },
    { "/groundhog/relay/paging/off-or-exhausted", test_paging_off_or_exhausted, FALSE },
  };
  wire_add_tests(cases, G_N_ELEMENTS(cases));
  /* The harness itself, no traffic: not a wire case (no Tor variant). */
  g_test_add_func("/groundhog/relay/held-port", test_held_port);
  g_test_add_func("/groundhog/relay/loopback-listener", test_loopback_listener);
  return g_test_run();
}
