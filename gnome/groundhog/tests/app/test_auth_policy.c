/* GhAuthPolicy (privacy charter G08, §4.3, §4.4): NT-1 AUTH matrix, NT-2
 * AUTH event shape, NT-3 stale challenge, NT-4 one AUTH per challenge and no
 * escalation (R7), and R6 across the callers that share the policy.
 *
 * Harnesses: recording GhRelayScope/GhRelayPublish transports with their
 * NIP-42 halves (H1: every AUTH frame is kept), a real GhAccountController on
 * a private bus with the mock org.nostr.Signer (gh-test-signer.h), which
 * counts every signer call. Waits iterate the main context; their deadlines
 * are failure bounds only. */
#include "gh-auth-policy.h"
#include "gh-test-signer.h"

#include "nostr-tag.h"

#define KEY_ALICE 1
#define KEY_BOB   2
#define KEY_OTHER 4

static gchar *hex_alice, *npub_alice, *npub_bob;
static GhTestBus shared_bus;

static void
drain(void)
{
  while (g_main_context_iteration(NULL, FALSE))
    ;
}

/* ---- recording connection (one per scope URL or publish URL) --------------- */

typedef struct {
  GhRelayScope *scope;     /* one of scope/publish */
  GhRelayPublish *publish;
  gchar *url;
  gchar *challenge;
  GPtrArray *auth;         /* signed AUTH events sent on this connection */
  guint resubscribes;      /* REQs re-issued after an accepted AUTH */
  guint resends;           /* EVENTs re-sent after an accepted AUTH */
  gboolean closed;
} Conn;

typedef struct {
  GPtrArray *conns;        /* Conn, in open order */
  GPtrArray *results;      /* publish outcomes, GINT_TO_POINTER */
} Rec;

static void
conn_free(gpointer data)
{
  Conn *conn = data;
  if (conn->scope)
    gh_relay_scope_unref(conn->scope);
  if (conn->publish)
    gh_relay_publish_unref(conn->publish);
  g_free(conn->url);
  g_free(conn->challenge);
  g_ptr_array_unref(conn->auth);
  g_free(conn);
}

static Conn *
conn_new(Rec *rec, const gchar *url)
{
  Conn *conn = g_new0(Conn, 1);
  conn->url = g_strdup(url);
  conn->auth = g_ptr_array_new_with_free_func(g_free);
  conn->challenge = g_strdup_printf("challenge-%u", rec->conns->len + 1);
  g_ptr_array_add(rec->conns, conn);
  return conn;
}

static gpointer
scope_open(GhRelayScope *scope, const gchar *url, const NostrFilters *filters, gpointer data,
           GError **error)
{
  (void)filters; (void)error;
  Conn *conn = conn_new(data, url);
  conn->scope = gh_relay_scope_ref(scope);
  return conn;
}

static void
conn_close(gpointer handle, gpointer data)
{
  (void)data;
  ((Conn *)handle)->closed = TRUE;
}

static gboolean
conn_send_auth(gpointer handle, const gchar *signed_json, gpointer data, GError **error)
{
  (void)data; (void)error;
  g_ptr_array_add(((Conn *)handle)->auth, g_strdup(signed_json));
  return TRUE;
}

static void
scope_resubscribe(gpointer handle, gpointer data)
{
  (void)data;
  ((Conn *)handle)->resubscribes++;
}

static const GhRelayTransport scope_transport = { scope_open, conn_close };
static const GhRelayAuthTransport scope_auth = { conn_send_auth, scope_resubscribe };

static gpointer
pub_open(GhRelayPublish *publish, const gchar *url, const gchar *event_json, gpointer data,
         GError **error)
{
  (void)event_json; (void)error;
  Conn *conn = conn_new(data, url);
  conn->publish = gh_relay_publish_ref(publish);
  return conn;
}

static gboolean
pub_resend(gpointer handle, gpointer data, GError **error)
{
  (void)data; (void)error;
  ((Conn *)handle)->resends++;
  return TRUE;
}

static const GhRelayPublishTransport pub_transport = { pub_open, conn_close };
static const GhRelayPublishAuthTransport pub_auth = { conn_send_auth, pub_resend };

static void
on_scope_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  (void)scope; (void)update; (void)data;
}

static void
on_pub_update(GhRelayPublish *publish, const GhRelayPublishResult *result, gpointer data)
{
  (void)publish;
  g_ptr_array_add(((Rec *)data)->results, GINT_TO_POINTER(result->outcome));
}

static void
on_pub_done(GhRelayPublish *publish, const GhRelayPublishSummary *summary, gpointer data)
{
  (void)publish; (void)summary; (void)data;
}

/* ---- fixture ---------------------------------------------------------------- */

typedef struct {
  GhTestSigner mock;
  GSettings *settings;
  GhAccountController *accounts;
  GhAuthPolicy *policy;
  Rec rec;
  gchar *event_json; /* a signed event to publish (any kind will do) */
} Fixture;

static GPtrArray *
fake_list(gpointer data, GError **error)
{
  (void)data; (void)error;
  GPtrArray *ids = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  const gchar *npubs[] = { npub_alice, npub_bob };
  for (guint i = 0; i < G_N_ELEMENTS(npubs); i++) {
    GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
    info->npub = g_strdup(npubs[i]);
    info->label = g_strdup(i ? "Bob" : "Alice");
    g_ptr_array_add(ids, info);
  }
  return ids;
}

static gboolean
listed(gpointer data)
{
  return gh_account_controller_get_state(data) != GH_ACCOUNT_STATE_DISCOVERING;
}

static gchar *
signed_event(guint key)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 1059);
  nostr_event_set_created_at(event, g_get_real_time() / G_USEC_PER_SEC);
  nostr_event_set_content(event, "sealed");
  nostr_event_set_tags(event, nostr_tags_new(1, nostr_tag_new("p", hex_alice, NULL)));
  g_assert_cmpint(nostr_event_sign(event, gh_test_secret[key]), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  gchar *copy = g_strdup(json);
  free(json);
  nostr_event_free(event);
  return copy;
}

static void
fixture_up(Fixture *f)
{
  memset(f, 0, sizeof *f);
  gh_test_signer_up(&shared_bus, &f->mock);
  f->settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(f->settings, "current-npub", npub_alice);
  f->accounts = gh_account_controller_new_full(f->settings, shared_bus.client, fake_list, NULL);
  gh_test_spin_until(listed, f->accounts);
  g_assert_cmpint(gh_account_controller_get_state(f->accounts), ==, GH_ACCOUNT_STATE_ACTIVE);
  f->policy = gh_auth_policy_get_for_accounts(f->accounts);
  f->rec.conns = g_ptr_array_new_with_free_func(conn_free);
  f->rec.results = g_ptr_array_new();
  f->event_json = signed_event(KEY_OTHER);
}

static void
fixture_down(Fixture *f)
{
  /* Scopes and publishes first: they hold the policy's account signer. */
  for (guint i = 0; i < f->rec.conns->len; i++) {
    Conn *conn = g_ptr_array_index(f->rec.conns, i);
    if (conn->scope)
      gh_relay_scope_cancel(conn->scope);
    if (conn->publish)
      gh_relay_publish_cancel(conn->publish);
  }
  g_ptr_array_unref(f->rec.conns);
  g_ptr_array_unref(f->rec.results);
  gh_test_release(f->accounts); /* the policy goes with it */
  GhTestSenders check = { &shared_bus, &f->mock };
  gh_test_spin_until(gh_test_signer_senders_closed, &check);
  drain();
  g_object_unref(f->settings);
  gh_test_signer_down(&shared_bus, &f->mock);
  g_free(f->event_json);
}

static guint64
generation(Fixture *f)
{
  return gh_account_controller_get_generation(f->accounts);
}

/* A started scope for url whose identity the policy set for purpose. */
static Conn *
open_scope(Fixture *f, guint64 gen, GhAuthPurpose purpose, const gchar *url)
{
  NostrFilters *filters = nostr_filters_new();
  NostrFilter *filter = nostr_filter_new();
  const int kinds[] = { 1059 };
  nostr_filter_set_kinds(filter, kinds, 1);
  g_assert_true(nostr_filters_add(filters, filter));
  nostr_filter_free(filter);
  GhRelayScope *scope = gh_relay_scope_new_with_transport(gen, filters, &scope_transport,
                                                          &f->rec, on_scope_update, NULL);
  gh_relay_scope_set_auth_transport(scope, &scope_auth);
  g_assert_true(gh_relay_scope_add_url(scope, url, NULL));
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_auth_policy_apply_scope(f->policy, scope, purpose, url, &error));
  g_assert_no_error(error);
  guint before = f->rec.conns->len;
  gh_relay_scope_start(scope);
  g_assert_cmpuint(f->rec.conns->len, ==, before + 1);
  gh_relay_scope_unref(scope); /* the connection holds it */
  return g_ptr_array_index(f->rec.conns, before);
}

static Conn *
open_publish(Fixture *f, GhAuthPurpose purpose, const gchar *url)
{
  g_autoptr(GError) error = NULL;
  GhRelayPublish *publish = gh_relay_publish_new_with_transport(generation(f), f->event_json,
    &pub_transport, &f->rec, on_pub_update, on_pub_done, &f->rec, &error);
  g_assert_no_error(error);
  gh_relay_publish_set_auth_transport(publish, &pub_auth);
  g_assert_true(gh_relay_publish_add_url(publish, url, NULL));
  g_assert_true(gh_auth_policy_apply_publish(f->policy, publish, purpose, url, &error));
  g_assert_no_error(error);
  guint before = f->rec.conns->len;
  g_assert_true(gh_relay_publish_start(publish, &error));
  g_assert_cmpuint(f->rec.conns->len, ==, before + 1);
  gh_relay_publish_unref(publish);
  return g_ptr_array_index(f->rec.conns, before);
}

/* The relay challenges and refuses the REQ or EVENT as auth-required. */
static void
demand_auth(Conn *conn)
{
  if (conn->scope) {
    gh_relay_scope_auth_challenge(conn->scope, conn->url, conn->challenge);
    gh_relay_scope_notice(conn->scope, conn->url, GH_RELAY_NOTICE_CLOSED, NULL, FALSE,
                          "auth-required: sign in first");
  } else {
    gh_relay_publish_auth_challenge(conn->publish, conn->url, conn->challenge);
    gh_relay_publish_ok(conn->publish, conn->url, gh_relay_publish_get_event_id(conn->publish),
                        FALSE, "auth-required: sign in first");
  }
}

static gboolean
auth_sent(gpointer data)
{
  return ((Conn *)data)->auth->len > 0;
}

/* NT-2: kind 22242, relay = this connection's URL, the challenge echoed,
 * empty content, a valid signature and created_at within 60 s of now.
 * Returns the signer's pubkey. */
static gchar *
checked_auth(Conn *conn, guint index, gchar *event_id)
{
  const gchar *json = g_ptr_array_index(conn->auth, index);
  g_autoptr(GError) error = NULL;
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_assert_true(gh_relay_auth_verify_signed(json, conn->url, conn->challenge, NULL, now,
                                            event_id, &error));
  g_assert_no_error(error);
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(event, json, NULL), ==, 1);
  g_assert_cmpint(nostr_event_get_kind(event), ==, 22242);
  g_assert_cmpint(ABS(nostr_event_get_created_at(event) - now), <=, 60);
  g_assert_cmpstr(nostr_event_get_content(event), ==, "");
  gchar *pubkey = g_strdup(nostr_event_get_pubkey(event));
  nostr_event_free(event);
  return pubkey;
}

/* ---- tests -------------------------------------------------------------------- */

/* The §4.3 table itself. */
static void
test_decide(void)
{
  const GhRelayAuthMode expected[GH_AUTH_N_PURPOSES] = {
    [GH_AUTH_PURPOSE_OWN_INBOX_READ] = GH_RELAY_AUTH_ACCOUNT,
    [GH_AUTH_PURPOSE_OWN_LIST_DISCOVERY] = GH_RELAY_AUTH_EPHEMERAL,
    [GH_AUTH_PURPOSE_OWN_LIST_PUBLISH] = GH_RELAY_AUTH_ACCOUNT,
    [GH_AUTH_PURPOSE_CONTACT_DIRECTORY] = GH_RELAY_AUTH_EPHEMERAL,
    [GH_AUTH_PURPOSE_RECIPIENT_WRAP] = GH_RELAY_AUTH_EPHEMERAL,
    [GH_AUTH_PURPOSE_SELF_WRAP] = GH_RELAY_AUTH_ACCOUNT,
    [GH_AUTH_PURPOSE_GROUP] = GH_RELAY_AUTH_ACCOUNT,
    [GH_AUTH_PURPOSE_MLS_ROUTING] = GH_RELAY_AUTH_EPHEMERAL,
  };
  for (guint p = 0; p < GH_AUTH_N_PURPOSES; p++) {
    g_assert_cmpint(gh_auth_policy_decide(p), ==, expected[p]);
    g_assert_nonnull(gh_auth_purpose_to_string(p));
  }
  /* Nothing unnamed is ever the account. */
  g_assert_cmpint(gh_auth_policy_decide(GH_AUTH_N_PURPOSES), ==, GH_RELAY_AUTH_EPHEMERAL);
  g_assert_cmpint(gh_auth_policy_decide((GhAuthPurpose)-1), ==, GH_RELAY_AUTH_EPHEMERAL);
  g_assert_null(gh_auth_purpose_to_string(GH_AUTH_N_PURPOSES));
}

/* NT-1 + NT-2: a challenge on every purpose, on a REQ and on an EVENT
 * connection each. Account-signed 22242 only for the four R1 purposes, the
 * signer asked exactly once for each of those connections and never for the
 * others, and a different ephemeral key on every other connection. */
static void
test_matrix(void)
{
  Fixture f;
  fixture_up(&f);
  g_autoptr(GHashTable) ephemeral = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  guint account_connections = 0;
  for (guint p = 0; p < GH_AUTH_N_PURPOSES; p++) {
    for (guint write = 0; write < 2; write++) {
      g_autofree gchar *url = g_strdup_printf("wss://%s-%s.test.invalid",
                                              gh_auth_purpose_to_string(p), write ? "w" : "r");
      Conn *conn = write ? open_publish(&f, p, url) : open_scope(&f, generation(&f), p, url);
      guint calls = f.mock.calls;
      demand_auth(conn);
      gh_test_spin_until(auth_sent, conn);
      drain();
      g_assert_cmpuint(conn->auth->len, ==, 1);
      gchar id[65];
      g_autofree gchar *pubkey = checked_auth(conn, 0, id);
      if (gh_auth_policy_decide(p) == GH_RELAY_AUTH_ACCOUNT) {
        g_assert_cmpstr(pubkey, ==, hex_alice);
        g_assert_cmpuint(f.mock.calls, ==, calls + 1);
        g_assert_cmpint(gh_auth_policy_get_account_state(f.policy, url), ==,
                        GH_AUTH_ACCOUNT_STATE_APPROVED);
        account_connections++;
      } else {
        g_assert_cmpstr(pubkey, !=, hex_alice);
        g_assert_cmpuint(f.mock.calls, ==, calls); /* the signer never saw it */
        g_assert_false(g_hash_table_contains(ephemeral, pubkey)); /* fresh per connection */
        g_hash_table_add(ephemeral, g_steal_pointer(&pubkey));
        g_assert_cmpint(gh_auth_policy_get_account_state(f.policy, url), ==,
                        GH_AUTH_ACCOUNT_STATE_NONE);
      }
      /* The accepted AUTH lets the REQ/EVENT through once. */
      if (conn->scope) {
        gh_relay_scope_notice(conn->scope, url, GH_RELAY_NOTICE_OK, id, TRUE, "");
        g_assert_cmpuint(conn->resubscribes, ==, 1);
      } else {
        gh_relay_publish_ok(conn->publish, url, id, TRUE, "");
        g_assert_cmpuint(conn->resends, ==, 1);
      }
    }
  }
  g_assert_cmpuint(account_connections, ==, 8);
  g_assert_cmpuint(g_hash_table_size(ephemeral), ==, 8);
  g_assert_cmpuint(f.mock.calls, ==, 8);
  fixture_down(&f);
}

typedef struct {
  GhTestSigner *mock;
  guint count;
} HeldWait;

typedef struct {
  GhAuthPolicy *policy;
  const gchar *url;
} StateWait;

static gboolean
declined(gpointer data)
{
  StateWait *want = data;
  return gh_auth_policy_get_account_state(want->policy, want->url) ==
         GH_AUTH_ACCOUNT_STATE_DECLINED;
}

static gboolean
publish_terminal(gpointer data)
{
  Conn *conn = data;
  return gh_relay_publish_get_outcome(conn->publish, conn->url) != GH_RELAY_PUBLISH_PENDING;
}

static gboolean
held_reached(gpointer data)
{
  HeldWait *want = data;
  return want->mock->held->len >= want->count;
}

/* NT-3: account AUTH belongs to one generation. A scope of an older
 * generation is refused an account identity outright; a challenge answered
 * after a switch (even with the approval already pending) sends nothing,
 * and the new account's first connection gets a new signer. */
static void
test_stale_challenge(void)
{
  Fixture f;
  fixture_up(&f);
  guint64 first = generation(&f);
  Conn *inbox = open_scope(&f, first, GH_AUTH_PURPOSE_OWN_INBOX_READ, "wss://inbox.test.invalid");
  Conn *later = open_scope(&f, first, GH_AUTH_PURPOSE_OWN_INBOX_READ, "wss://later.test.invalid");
  g_assert_cmpuint(gh_auth_policy_get_generation(f.policy), ==, first);

  /* The approval is pending when the account switches. */
  f.mock.hold = TRUE;
  demand_auth(inbox);
  HeldWait one = { &f.mock, 1 };
  gh_test_spin_until(held_reached, &one);
  g_assert_cmpint(gh_auth_policy_get_account_state(f.policy, inbox->url), ==,
                  GH_AUTH_ACCOUNT_STATE_WAITING);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_account_controller_select(f.accounts, npub_bob, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(generation(&f), >, first);
  g_assert_cmpuint(gh_auth_policy_get_generation(f.policy), ==, 0);
  g_assert_cmpint(gh_auth_policy_get_account_state(f.policy, inbox->url), ==,
                  GH_AUTH_ACCOUNT_STATE_NONE);
  /* The user approves too late: nothing reaches the relay. */
  gh_test_signer_release_all(&f.mock);
  f.mock.hold = FALSE;
  GhTestSenders check = { &shared_bus, &f.mock };
  gh_test_spin_until(gh_test_signer_senders_closed, &check);
  drain();
  g_assert_cmpuint(inbox->auth->len, ==, 0);

  /* A later challenge on the old generation's scope asks nobody. */
  guint calls = f.mock.calls;
  demand_auth(later);
  drain();
  g_assert_cmpuint(later->auth->len, ==, 0);
  g_assert_cmpuint(f.mock.calls, ==, calls);

  /* An old-generation connection cannot be given the account at all. */
  NostrFilters *filters = nostr_filters_new();
  GhRelayScope *stale = gh_relay_scope_new_with_transport(first, filters, &scope_transport,
                                                          &f.rec, on_scope_update, NULL);
  g_assert_true(gh_relay_scope_add_url(stale, "wss://stale.test.invalid", NULL));
  g_assert_false(gh_auth_policy_apply_scope(f.policy, stale, GH_AUTH_PURPOSE_OWN_INBOX_READ,
                                            "wss://stale.test.invalid", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_clear_error(&error);
  /* Ephemeral identities need no account and stay available. */
  g_assert_true(gh_auth_policy_apply_scope(f.policy, stale, GH_AUTH_PURPOSE_CONTACT_DIRECTORY,
                                           "wss://stale.test.invalid", &error));
  gh_relay_scope_cancel(stale);
  gh_relay_scope_unref(stale);
  /* An unknown URL is refused. */
  Conn *bob = open_scope(&f, generation(&f), GH_AUTH_PURPOSE_OWN_INBOX_READ,
                         "wss://bob-inbox.test.invalid");
  g_assert_false(gh_auth_policy_apply_scope(f.policy, bob->scope, GH_AUTH_PURPOSE_RECIPIENT_WRAP,
                                            "wss://elsewhere.test.invalid", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error(&error);

  /* The new account signs for its own connection, as itself. */
  demand_auth(bob);
  gh_test_spin_until(auth_sent, bob);
  gchar id[65];
  g_autofree gchar *pubkey = checked_auth(bob, 0, id);
  g_autofree gchar *hex_bob = gh_test_pub(KEY_BOB);
  g_assert_cmpstr(pubkey, ==, hex_bob);
  g_assert_cmpuint(gh_auth_policy_get_generation(f.policy), ==, generation(&f));

  /* No account at all (read-only): nothing account-signed can be set up. */
  GhRelayPublish *publish = gh_relay_publish_new_with_transport(generation(&f), f.event_json,
    &pub_transport, &f.rec, on_pub_update, on_pub_done, &f.rec, &error);
  g_assert_no_error(error);
  g_assert_true(gh_relay_publish_add_url(publish, "wss://x.test.invalid", NULL));
  g_assert_true(gh_account_controller_select(f.accounts, "", &error));
  g_assert_no_error(error);
  g_assert_false(gh_auth_policy_apply_publish(f.policy, publish, GH_AUTH_PURPOSE_SELF_WRAP,
                                              "wss://x.test.invalid", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_cmpuint(gh_auth_policy_get_generation(f.policy), ==, 0);
  gh_relay_publish_cancel(publish);
  gh_relay_publish_unref(publish);
  fixture_down(&f);
}

/* NT-4 and R7: one AUTH per challenge. A recipient relay that still refuses
 * the EVENT after the ephemeral AUTH, or refuses the AUTH itself
 * ("restricted:"), ends AUTH_REQUIRED; it is never asked again on that
 * challenge and never escalated to the account. */
static void
test_one_auth_no_escalation(void)
{
  Fixture f;
  fixture_up(&f);
  Conn *again = open_publish(&f, GH_AUTH_PURPOSE_RECIPIENT_WRAP, "wss://again.test.invalid");
  demand_auth(again);
  gh_test_spin_until(auth_sent, again);
  gchar id[65];
  g_autofree gchar *pubkey = checked_auth(again, 0, id);
  g_assert_cmpstr(pubkey, !=, hex_alice);
  gh_relay_publish_ok(again->publish, again->url, id, TRUE, "");
  g_assert_cmpuint(again->resends, ==, 1);
  /* The re-sent EVENT is refused as auth-required again: terminal. */
  gh_relay_publish_ok(again->publish, again->url, gh_relay_publish_get_event_id(again->publish),
                      FALSE, "auth-required: still no");
  drain();
  g_assert_cmpint(gh_relay_publish_get_outcome(again->publish, again->url), ==,
                  GH_RELAY_PUBLISH_AUTH_REQUIRED);
  g_assert_cmpuint(again->auth->len, ==, 1);

  Conn *restricted = open_publish(&f, GH_AUTH_PURPOSE_RECIPIENT_WRAP,
                                  "wss://restricted.test.invalid");
  demand_auth(restricted);
  gh_test_spin_until(auth_sent, restricted);
  g_autofree gchar *pubkey2 = checked_auth(restricted, 0, id);
  g_assert_cmpstr(pubkey2, !=, pubkey);
  gh_relay_publish_ok(restricted->publish, restricted->url, id, FALSE,
                      "restricted: members only");
  drain();
  g_assert_cmpint(gh_relay_publish_get_outcome(restricted->publish, restricted->url), ==,
                  GH_RELAY_PUBLISH_AUTH_REQUIRED);
  g_assert_cmpuint(restricted->auth->len, ==, 1);
  g_assert_cmpuint(restricted->resends, ==, 0);

  /* A REQ refused again after its AUTH is reported, not re-authenticated. */
  Conn *req = open_scope(&f, generation(&f), GH_AUTH_PURPOSE_CONTACT_DIRECTORY,
                         "wss://directory.test.invalid");
  demand_auth(req);
  gh_test_spin_until(auth_sent, req);
  g_autofree gchar *pubkey3 = checked_auth(req, 0, id);
  gh_relay_scope_notice(req->scope, req->url, GH_RELAY_NOTICE_OK, id, TRUE, "");
  g_assert_cmpuint(req->resubscribes, ==, 1);
  gh_relay_scope_notice(req->scope, req->url, GH_RELAY_NOTICE_CLOSED, NULL, FALSE,
                        "auth-required: no");
  drain();
  g_assert_cmpuint(req->auth->len, ==, 1);

  g_assert_cmpuint(f.mock.calls, ==, 0); /* no escalation, ever */
  fixture_down(&f);
}

/* R6 across callers: the inbox's own-inbox REQ and the outbox's self-copy
 * publish to the same relay share the policy's one signer request per relay;
 * a declined relay is not asked again by either for this generation. */
static void
test_shared_prompt(void)
{
  Fixture f;
  fixture_up(&f);
  g_assert_true(gh_auth_policy_get_for_accounts(f.accounts) == f.policy);
  const gchar *url = "wss://own-inbox.test.invalid";
  Conn *read = open_scope(&f, generation(&f), GH_AUTH_PURPOSE_OWN_INBOX_READ, url);
  Conn *self_copy = open_publish(&f, GH_AUTH_PURPOSE_SELF_WRAP, url);
  f.mock.hold = TRUE;
  demand_auth(read);
  demand_auth(self_copy);
  HeldWait one = { &f.mock, 1 };
  gh_test_spin_until(held_reached, &one);
  drain();
  g_assert_cmpuint(f.mock.held->len, ==, 1); /* one request per relay at a time */
  gh_test_signer_release_one(&f.mock);
  HeldWait two = { &f.mock, 1 };
  gh_test_spin_until(held_reached, &two); /* the second one asks after the first */
  g_assert_cmpuint(f.mock.max_held, ==, 1);
  gh_test_signer_release_all(&f.mock);
  f.mock.hold = FALSE;
  gh_test_spin_until(auth_sent, read);
  gh_test_spin_until(auth_sent, self_copy);
  g_assert_cmpuint(f.mock.calls, ==, 2);

  /* Declined on another relay: asked once, then never again this generation. */
  const gchar *other = "wss://declined.test.invalid";
  Conn *r2 = open_scope(&f, generation(&f), GH_AUTH_PURPOSE_OWN_INBOX_READ, other);
  f.mock.deny = TRUE;
  demand_auth(r2);
  StateWait refused = { f.policy, other };
  gh_test_spin_until(declined, &refused);
  f.mock.deny = FALSE;
  Conn *p2 = open_publish(&f, GH_AUTH_PURPOSE_SELF_WRAP, other);
  demand_auth(p2);
  gh_test_spin_until(publish_terminal, p2);
  g_assert_cmpint(gh_relay_publish_get_outcome(p2->publish, other), ==,
                  GH_RELAY_PUBLISH_AUTH_REQUIRED);
  g_assert_cmpuint(f.mock.calls, ==, 3);
  g_assert_cmpuint(r2->auth->len + p2->auth->len, ==, 0);
  fixture_down(&f);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  hex_alice = gh_test_pub(KEY_ALICE);
  npub_alice = gh_test_npub(KEY_ALICE);
  npub_bob = gh_test_npub(KEY_BOB);
  gh_test_bus_up(&shared_bus);
  g_test_add_func("/groundhog/auth-policy/decide", test_decide);
  g_test_add_func("/groundhog/auth-policy/nt1-nt2-matrix", test_matrix);
  g_test_add_func("/groundhog/auth-policy/nt3-stale-challenge", test_stale_challenge);
  g_test_add_func("/groundhog/auth-policy/nt4-one-auth-no-escalation", test_one_auth_no_escalation);
  g_test_add_func("/groundhog/auth-policy/r6-shared-prompt", test_shared_prompt);
  int result = g_test_run();
  gh_test_bus_down(&shared_bus);
  g_free(hex_alice);
  g_free(npub_alice);
  g_free(npub_bob);
  return result;
}
