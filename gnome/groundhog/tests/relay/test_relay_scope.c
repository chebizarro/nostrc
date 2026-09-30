#include "gh-relay-scope.h"
#include "fake-auth-signer.h"

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

#define OTHER_AUTH_ID "1111111111111111111111111111111111111111111111111111111111111111"

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

/* ---- NIP-42 against a recording transport ---- */

#define AUTH_URL "wss://auth.example"
#define OTHER_URL "wss://other.example"
#define NONE_URL "wss://none.example"

typedef struct {
  Fixture base;            /* first: fake_open/fake_close see a Fixture */
  GPtrArray *sent_auth;    /* signed AUTH event JSON, in order */
  GPtrArray *sent_urls;    /* the handle (URL) each was sent on */
  guint resubscribes;
  gboolean send_fails;
  FakeSigner signer;
} AuthFixture;

static gboolean
record_send_auth(gpointer handle, const gchar *signed_event_json, gpointer data,
                 GError **error)
{
  AuthFixture *fixture = data;
  if (fixture->send_fails) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE, "socket gone");
    return FALSE;
  }
  g_ptr_array_add(fixture->sent_auth, g_strdup(signed_event_json));
  g_ptr_array_add(fixture->sent_urls, g_strdup(handle));
  return TRUE;
}

static void
record_resubscribe(gpointer handle, gpointer data)
{
  AuthFixture *fixture = data;
  (void)handle;
  fixture->resubscribes++;
}

static const GhRelayAuthTransport record_auth_transport = {
  .send_auth = record_send_auth,
  .resubscribe = record_resubscribe,
};

static void
on_auth_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  AuthFixture *fixture = data;
  if (update->notice != GH_RELAY_NOTICE_ERROR)
    on_update(scope, update, &fixture->base);
}

static void
auth_fixture_init(AuthFixture *fixture, FakeSignMode sign_mode)
{
  memset(fixture, 0, sizeof *fixture);
  fixture->base.opened = g_ptr_array_new_with_free_func(g_free);
  fixture->base.closed = g_ptr_array_new_with_free_func(g_free);
  fixture->sent_auth = g_ptr_array_new_with_free_func(g_free);
  fixture->sent_urls = g_ptr_array_new_with_free_func(g_free);
  fixture->signer.mode = sign_mode;
}

/* Every scope here HAS an account signer, so tests show it is used only
 * where the caller chose ACCOUNT for that URL. */
static GhRelayScope *
auth_scope_new(AuthFixture *fixture, FakeSignMode sign_mode, GhRelayAuthMode mode,
               gboolean with_auth_transport)
{
  auth_fixture_init(fixture, sign_mode);
  GhRelayScope *scope = gh_relay_scope_new_with_transport(7, nostr_filters_new(),
    &fake_transport, fixture, on_auth_update, fixture);
  if (with_auth_transport)
    gh_relay_scope_set_auth_transport(scope, &record_auth_transport);
  g_autoptr(GhRelayAuthSigner) signer = fake_signer_new(&fixture->signer, 7, NULL);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_relay_scope_set_account_signer(scope, signer, &error));
  g_assert_true(gh_relay_scope_add_url(scope, AUTH_URL, NULL));
  g_assert_true(gh_relay_scope_set_url_auth(scope, AUTH_URL, mode, &error));
  g_assert_no_error(error);
  gh_relay_scope_start(scope);
  return scope;
}

static void
auth_fixture_clear(AuthFixture *fixture)
{
  g_ptr_array_unref(fixture->sent_auth);
  g_ptr_array_unref(fixture->sent_urls);
  fake_signer_clear(&fixture->signer);
  fixture_clear(&fixture->base);
}

/* The id of AUTH event @index, after checking it is the signed 22242 for
 * its relay and @challenge. */
static gchar *
auth_id_at(AuthFixture *fixture, guint index, const gchar *challenge)
{
  g_assert_cmpuint(index, <, fixture->sent_auth->len);
  gchar id[65] = {0};
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_relay_auth_verify_signed(g_ptr_array_index(fixture->sent_auth, index),
                                            g_ptr_array_index(fixture->sent_urls, index),
                                            challenge, NULL,
                                            g_get_real_time() / G_USEC_PER_SEC,
                                            id, &error));
  g_assert_no_error(error);
  return g_strdup(id);
}

static gchar *
last_auth_id(AuthFixture *fixture, const gchar *challenge)
{
  return auth_id_at(fixture, fixture->sent_auth->len - 1, challenge);
}

static void
closed_auth_required_on(GhRelayScope *scope, const gchar *url)
{
  gh_relay_scope_notice(scope, url, GH_RELAY_NOTICE_CLOSED, NULL, FALSE,
                         "auth-required: members only");
}

static void
closed_auth_required(GhRelayScope *scope)
{
  closed_auth_required_on(scope, AUTH_URL);
}

/* Challenge, then the refused REQ: one AUTH as the account, the REQ is
 * re-issued on the relay's OK, and neither the held CLOSED nor the AUTH OK
 * is reported. The retried REQ refused again for the same challenge is
 * reported, with no second AUTH, and a later challenge does not restart it. */
static void
test_auth_retries_req_once(void)
{
  AuthFixture fixture;
  GhRelayScope *scope = auth_scope_new(&fixture, FAKE_SIGN_OK, GH_RELAY_AUTH_ACCOUNT, TRUE);
  gh_relay_scope_auth_challenge(scope, AUTH_URL, "challenge-1");
  g_assert_cmpuint(fixture.base.auth, ==, 1);
  drain_pending();
  g_assert_cmpuint(fixture.signer.calls, ==, 0); /* lazy: nothing asked yet */

  closed_auth_required(scope);
  g_assert_cmpuint(fixture.base.closed_notices, ==, 0);
  iterate_until(&fixture.sent_auth->len, 1);
  g_assert_cmpuint(fixture.signer.calls, ==, 1);
  g_assert_nonnull(strstr(fixture.signer.last_unsigned, "22242"));
  g_autofree gchar *id = last_auth_id(&fixture, "challenge-1");
  g_autofree gchar *account = fake_account_pubkey();
  g_autofree gchar *signed_as = auth_event_pubkey(g_ptr_array_index(fixture.sent_auth, 0));
  g_assert_cmpstr(signed_as, ==, account);

  gh_relay_scope_notice(scope, AUTH_URL, GH_RELAY_NOTICE_OK, OTHER_AUTH_ID,
                         TRUE, "");
  g_assert_cmpuint(fixture.resubscribes, ==, 0);
  g_assert_cmpuint(fixture.base.ok, ==, 1); /* not ours: reported */
  gh_relay_scope_notice(scope, AUTH_URL, GH_RELAY_NOTICE_OK, id, TRUE, "");
  g_assert_cmpuint(fixture.resubscribes, ==, 1);
  g_assert_cmpuint(fixture.base.ok, ==, 1);
  g_assert_cmpuint(fixture.base.closed_notices, ==, 0);
  gh_relay_scope_eose(scope, AUTH_URL);
  g_assert_cmpuint(fixture.base.eose, ==, 1);

  closed_auth_required(scope);
  g_assert_cmpuint(fixture.base.closed_notices, ==, 1);
  g_assert_cmpstr(fixture.base.last_detail, ==, "auth-required: members only");
  gh_relay_scope_auth_challenge(scope, AUTH_URL, "challenge-2");
  drain_pending();
  g_assert_cmpuint(fixture.signer.calls, ==, 1);
  g_assert_cmpuint(fixture.sent_auth->len, ==, 1);
  g_assert_cmpuint(fixture.resubscribes, ==, 1);
  gh_relay_scope_unref(scope);
  auth_fixture_clear(&fixture);
}

/* A CLOSED before any challenge is reported at once; the challenge that
 * follows still earns the single authenticated retry. */
static void
test_auth_challenge_after_closed(void)
{
  AuthFixture fixture;
  GhRelayScope *scope = auth_scope_new(&fixture, FAKE_SIGN_OK, GH_RELAY_AUTH_ACCOUNT, TRUE);
  closed_auth_required(scope);
  g_assert_cmpuint(fixture.base.closed_notices, ==, 1);
  gh_relay_scope_auth_challenge(scope, AUTH_URL, "late");
  iterate_until(&fixture.sent_auth->len, 1);
  g_autofree gchar *id = last_auth_id(&fixture, "late");
  gh_relay_scope_notice(scope, AUTH_URL, GH_RELAY_NOTICE_OK, id, TRUE, "");
  g_assert_cmpuint(fixture.resubscribes, ==, 1);
  g_assert_cmpuint(fixture.base.closed_notices, ==, 1);
  gh_relay_scope_unref(scope);
  auth_fixture_clear(&fixture);
}

/* Refused signing, a signed event that fails local verification, a failed
 * AUTH write and the relay's OK false all end in the relay's CLOSED, with
 * no REQ retry. Only the verified event is ever handed to the transport. */
static void
test_auth_failures_report_closed(void)
{
  const FakeSignMode local[] = { FAKE_SIGN_DENY, FAKE_SIGN_WRONG_CHALLENGE,
                                 FAKE_SIGN_WRONG_RELAY, FAKE_SIGN_BAD_SIG };
  for (gsize i = 0; i < G_N_ELEMENTS(local); i++) {
    AuthFixture fixture;
    GhRelayScope *scope = auth_scope_new(&fixture, local[i], GH_RELAY_AUTH_ACCOUNT, TRUE);
    gh_relay_scope_auth_challenge(scope, AUTH_URL, "challenge");
    closed_auth_required(scope);
    iterate_until(&fixture.base.closed_notices, 1);
    g_assert_cmpuint(fixture.signer.calls, ==, 1);
    g_assert_cmpuint(fixture.sent_auth->len, ==, 0);
    g_assert_cmpuint(fixture.resubscribes, ==, 0);
    g_assert_cmpstr(fixture.base.last_detail, ==, "auth-required: members only");
    gh_relay_scope_unref(scope);
    auth_fixture_clear(&fixture);
  }

  AuthFixture write;
  GhRelayScope *scope = auth_scope_new(&write, FAKE_SIGN_OK, GH_RELAY_AUTH_ACCOUNT, TRUE);
  write.send_fails = TRUE;
  gh_relay_scope_auth_challenge(scope, AUTH_URL, "challenge");
  closed_auth_required(scope);
  iterate_until(&write.base.closed_notices, 1);
  g_assert_cmpuint(write.signer.calls, ==, 1);
  gh_relay_scope_unref(scope);
  auth_fixture_clear(&write);

  /* The relay refusing an EPHEMERAL AUTH is terminal: never escalated to
   * the account. */
  const GhRelayAuthMode refused_modes[] = { GH_RELAY_AUTH_ACCOUNT, GH_RELAY_AUTH_EPHEMERAL };
  for (gsize i = 0; i < G_N_ELEMENTS(refused_modes); i++) {
    AuthFixture refused;
    scope = auth_scope_new(&refused, FAKE_SIGN_OK, refused_modes[i], TRUE);
    gh_relay_scope_auth_challenge(scope, AUTH_URL, "challenge");
    closed_auth_required(scope);
    iterate_until(&refused.sent_auth->len, 1);
    g_autofree gchar *id = last_auth_id(&refused, "challenge");
    gh_relay_scope_notice(scope, AUTH_URL, GH_RELAY_NOTICE_OK, id, FALSE,
                           "restricted: not on the list");
    g_assert_cmpuint(refused.base.closed_notices, ==, 1);
    g_assert_cmpstr(refused.base.last_detail, ==, "auth-required: members only");
    g_assert_cmpuint(refused.base.ok, ==, 0);
    g_assert_cmpuint(refused.resubscribes, ==, 0);
    drain_pending();
    g_assert_cmpuint(refused.sent_auth->len, ==, 1);
    g_assert_cmpuint(refused.signer.calls, ==, refused_modes[i] == GH_RELAY_AUTH_ACCOUNT ? 1 : 0);
    gh_relay_scope_unref(scope);
    auth_fixture_clear(&refused);
  }
}

/* NONE (the default, even with an account signer present) or no AUTH
 * transport: nothing is signed and the CLOSED is reported as before. */
static void
test_auth_off_is_unchanged(void)
{
  for (guint variant = 0; variant < 2; variant++) {
    AuthFixture fixture;
    GhRelayScope *scope = auth_scope_new(&fixture, FAKE_SIGN_OK,
                                         variant == 0 ? GH_RELAY_AUTH_NONE
                                                      : GH_RELAY_AUTH_ACCOUNT,
                                         variant == 0);
    gh_relay_scope_auth_challenge(scope, AUTH_URL, "challenge");
    closed_auth_required(scope);
    g_assert_cmpuint(fixture.base.auth, ==, 1);
    g_assert_cmpuint(fixture.base.closed_notices, ==, 1);
    drain_pending();
    g_assert_cmpuint(fixture.signer.calls, ==, 0);
    g_assert_cmpuint(fixture.sent_auth->len, ==, 0);
    gh_relay_scope_unref(scope);
    auth_fixture_clear(&fixture);
  }
}

/* One scope, three URLs, three identity choices: the account key signs
 * only for the ACCOUNT URL, the EPHEMERAL URL gets an unrelated key, and
 * the default URL gets nothing. */
static void
test_auth_identity_per_url(void)
{
  AuthFixture fixture;
  auth_fixture_init(&fixture, FAKE_SIGN_OK);
  GhRelayScope *scope = gh_relay_scope_new_with_transport(7, nostr_filters_new(),
    &fake_transport, &fixture, on_auth_update, &fixture);
  gh_relay_scope_set_auth_transport(scope, &record_auth_transport);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_relay_scope_add_url(scope, AUTH_URL, NULL));
  g_assert_true(gh_relay_scope_add_url(scope, OTHER_URL, NULL));
  g_assert_true(gh_relay_scope_add_url(scope, NONE_URL, NULL));
  /* ACCOUNT needs the account signer; unknown URLs are refused. */
  g_assert_false(gh_relay_scope_set_url_auth(scope, AUTH_URL, GH_RELAY_AUTH_ACCOUNT, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_clear_error(&error);
  g_assert_false(gh_relay_scope_set_url_auth(scope, "wss://absent.example",
                                             GH_RELAY_AUTH_EPHEMERAL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error(&error);
  g_autoptr(GhRelayAuthSigner) signer = fake_signer_new(&fixture.signer, 7, NULL);
  g_assert_true(gh_relay_scope_set_account_signer(scope, signer, NULL));
  g_assert_true(gh_relay_scope_set_url_auth(scope, AUTH_URL, GH_RELAY_AUTH_ACCOUNT, NULL));
  g_assert_true(gh_relay_scope_set_url_auth(scope, OTHER_URL, GH_RELAY_AUTH_EPHEMERAL, NULL));
  gh_relay_scope_start(scope);

  const gchar *urls[] = { AUTH_URL, OTHER_URL, NONE_URL };
  for (gsize i = 0; i < G_N_ELEMENTS(urls); i++) {
    gh_relay_scope_auth_challenge(scope, urls[i], "challenge");
    closed_auth_required_on(scope, urls[i]);
  }
  iterate_until(&fixture.sent_auth->len, 2);
  drain_pending();
  g_assert_cmpuint(fixture.sent_auth->len, ==, 2);
  g_assert_cmpuint(fixture.signer.calls, ==, 1);          /* the ACCOUNT URL only */
  g_assert_cmpuint(fixture.base.closed_notices, ==, 1);   /* the NONE URL */
  g_assert_cmpstr(fixture.base.last_url, ==, NONE_URL);
  g_autofree gchar *account = fake_account_pubkey();
  for (guint i = 0; i < 2; i++) {
    g_autofree gchar *id = auth_id_at(&fixture, i, "challenge");
    g_autofree gchar *pubkey = auth_event_pubkey(g_ptr_array_index(fixture.sent_auth, i));
    if (g_strcmp0(g_ptr_array_index(fixture.sent_urls, i), AUTH_URL) == 0)
      g_assert_cmpstr(pubkey, ==, account);
    else
      g_assert_cmpstr(pubkey, !=, account);
  }
  gh_relay_scope_unref(scope);
  auth_fixture_clear(&fixture);
}

/* EPHEMERAL: each connection authenticates with a new key, never the
 * account's; the account signer is never asked. */
static void
test_auth_ephemeral_fresh_per_connection(void)
{
  AuthFixture fixture;
  GhRelayScope *scope = auth_scope_new(&fixture, FAKE_SIGN_OK, GH_RELAY_AUTH_EPHEMERAL, TRUE);
  g_autofree gchar *account = fake_account_pubkey();
  for (guint connection = 0; connection < 2; connection++) {
    g_autofree gchar *challenge = g_strdup_printf("challenge-%u", connection);
    gh_relay_scope_auth_challenge(scope, AUTH_URL, challenge);
    closed_auth_required(scope);
    iterate_until(&fixture.sent_auth->len, connection + 1);
    g_autofree gchar *id = last_auth_id(&fixture, challenge);
    gh_relay_scope_notice(scope, AUTH_URL, GH_RELAY_NOTICE_OK, id, TRUE, "");
    g_assert_cmpuint(fixture.resubscribes, ==, connection + 1);
    /* The connection drops; the transport redials. */
    gh_relay_scope_notice(scope, AUTH_URL, GH_RELAY_NOTICE_DISCONNECTED, NULL, FALSE, NULL);
  }
  g_autofree gchar *first = auth_event_pubkey(g_ptr_array_index(fixture.sent_auth, 0));
  g_autofree gchar *second = auth_event_pubkey(g_ptr_array_index(fixture.sent_auth, 1));
  g_assert_cmpstr(first, !=, second);
  g_assert_cmpstr(first, !=, account);
  g_assert_cmpstr(second, !=, account);
  g_assert_cmpuint(fixture.signer.calls, ==, 0);
  g_assert_cmpuint(fixture.base.closed_notices, ==, 0);
  gh_relay_scope_unref(scope);
  auth_fixture_clear(&fixture);
}

/* Cancelling the scope, losing the connection, or switching the URL's
 * identity while the signer is still working drops the AUTH: its operation
 * is cancelled, and its late result is neither sent nor used. */
static void
test_auth_dropped_while_signing(void)
{
  for (guint variant = 0; variant < 3; variant++) {
    AuthFixture fixture;
    GhRelayScope *scope = auth_scope_new(&fixture, FAKE_SIGN_HOLD, GH_RELAY_AUTH_ACCOUNT, TRUE);
    gh_relay_scope_auth_challenge(scope, AUTH_URL, "challenge");
    closed_auth_required(scope);
    iterate_until(&fixture.signer.calls, 1);
    if (variant == 0)
      gh_relay_scope_cancel(scope);
    else if (variant == 1)
      gh_relay_scope_notice(scope, AUTH_URL, GH_RELAY_NOTICE_DISCONNECTED,
                             NULL, FALSE, NULL);
    else
      g_assert_true(gh_relay_scope_set_url_auth(scope, AUTH_URL, GH_RELAY_AUTH_NONE, NULL));
    g_assert_true(fake_signer_release(&fixture.signer));
    drain_pending();
    g_assert_cmpuint(fixture.sent_auth->len, ==, 0);
    g_assert_cmpuint(fixture.base.closed_notices, ==, variant == 2 ? 1 : 0);
    g_assert_cmpuint(fixture.resubscribes, ==, 0);
    gh_relay_scope_unref(scope);
    auth_fixture_clear(&fixture);
  }
}

/* A signer of another generation is refused up front; one revoked with its
 * generation afterwards signs nothing, and the CLOSED is reported. */
static void
test_auth_generation_bound(void)
{
  FakeSigner fake = { .mode = FAKE_SIGN_OK };
  GhRelayScope *scope = gh_relay_scope_new_with_transport(7, nostr_filters_new(),
    &fake_transport, NULL, NULL, NULL);
  g_autoptr(GhRelayAuthSigner) stale = fake_signer_new(&fake, 6, NULL);
  g_autoptr(GError) error = NULL;
  g_assert_false(gh_relay_scope_set_account_signer(scope, stale, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  gh_relay_scope_unref(scope);

  AuthFixture fixture;
  auth_fixture_init(&fixture, FAKE_SIGN_OK);
  GCancellable *generation = g_cancellable_new();
  scope = gh_relay_scope_new_with_transport(7, nostr_filters_new(),
    &fake_transport, &fixture, on_auth_update, &fixture);
  gh_relay_scope_set_auth_transport(scope, &record_auth_transport);
  g_autoptr(GhRelayAuthSigner) signer = fake_signer_new(&fixture.signer, 7, generation);
  g_assert_true(gh_relay_scope_set_account_signer(scope, signer, NULL));
  g_assert_true(gh_relay_scope_add_url(scope, AUTH_URL, NULL));
  g_assert_true(gh_relay_scope_set_url_auth(scope, AUTH_URL, GH_RELAY_AUTH_ACCOUNT, NULL));
  gh_relay_scope_start(scope);
  g_cancellable_cancel(generation); /* the account switched */
  g_assert_true(gh_relay_auth_signer_is_revoked(signer));
  gh_relay_scope_auth_challenge(scope, AUTH_URL, "challenge");
  closed_auth_required(scope);
  g_assert_cmpuint(fixture.base.closed_notices, ==, 1);
  drain_pending();
  g_assert_cmpuint(fixture.signer.calls, ==, 0);
  gh_relay_scope_unref(scope);
  g_object_unref(generation);
  auth_fixture_clear(&fixture);
  fake_signer_clear(&fake);
}

/* ---- Overflow (nostrc-5rfp) ---- */

#define OVERFLOW_DETAIL GH_RELAY_CLOSED_OVERFLOW_PREFIX " too many events waiting to be read"

/* A REQ ended by its backlog ceiling is reported as CLOSED at once (the
 * caller surfaces it; no EOSE for that REQ), then re-issued once per
 * connection from an idle. A second overflow on the same connection, a relay's
 * own CLOSED, a cancelled scope and a scope that cannot resubscribe are only
 * reported; a new connection may retry again. */
static void
test_overflow_reported_then_retried_once(void)
{
  AuthFixture fixture;
  GhRelayScope *scope = auth_scope_new(&fixture, FAKE_SIGN_OK, GH_RELAY_AUTH_NONE, TRUE);
  g_autofree gchar *json = signed_json("before the overflow");
  gh_relay_scope_event(scope, AUTH_URL, json);
  g_assert_true(gh_relay_closed_is_overflow(OVERFLOW_DETAIL));
  g_assert_false(gh_relay_closed_is_overflow("error: shutting down"));
  g_assert_false(gh_relay_closed_is_overflow(NULL));

  gh_relay_scope_notice(scope, AUTH_URL, GH_RELAY_NOTICE_CLOSED, NULL, FALSE, OVERFLOW_DETAIL);
  g_assert_cmpuint(fixture.base.closed_notices, ==, 1);
  g_assert_cmpstr(fixture.base.last_detail, ==, OVERFLOW_DETAIL);
  g_assert_cmpuint(fixture.resubscribes, ==, 0);   /* never inside the callback */
  drain_pending();
  g_assert_cmpuint(fixture.resubscribes, ==, 1);
  gh_relay_scope_eose(scope, AUTH_URL);            /* the re-issued REQ's boundary */
  g_assert_cmpuint(fixture.base.eose, ==, 1);

  /* Again on the same connection: reported only. */
  gh_relay_scope_notice(scope, AUTH_URL, GH_RELAY_NOTICE_CLOSED, NULL, FALSE, OVERFLOW_DETAIL);
  drain_pending();
  g_assert_cmpuint(fixture.base.closed_notices, ==, 2);
  g_assert_cmpuint(fixture.resubscribes, ==, 1);
  /* A relay's CLOSED never triggers it. */
  gh_relay_scope_notice(scope, AUTH_URL, GH_RELAY_NOTICE_DISCONNECTED, NULL, FALSE, NULL);
  gh_relay_scope_notice(scope, AUTH_URL, GH_RELAY_NOTICE_CLOSED, NULL, FALSE,
                        "error: shutting down");
  drain_pending();
  g_assert_cmpuint(fixture.resubscribes, ==, 1);
  /* The next connection may retry once. */
  gh_relay_scope_notice(scope, AUTH_URL, GH_RELAY_NOTICE_CLOSED, NULL, FALSE, OVERFLOW_DETAIL);
  drain_pending();
  g_assert_cmpuint(fixture.resubscribes, ==, 2);

  /* A cancel drops a retry not yet run. */
  gh_relay_scope_notice(scope, AUTH_URL, GH_RELAY_NOTICE_DISCONNECTED, NULL, FALSE, NULL);
  gh_relay_scope_notice(scope, AUTH_URL, GH_RELAY_NOTICE_CLOSED, NULL, FALSE, OVERFLOW_DETAIL);
  gh_relay_scope_cancel(scope);
  drain_pending();
  g_assert_cmpuint(fixture.resubscribes, ==, 2);
  gh_relay_scope_unref(scope);
  auth_fixture_clear(&fixture);

  /* Without a transport that can resubscribe it is reported only. */
  scope = auth_scope_new(&fixture, FAKE_SIGN_OK, GH_RELAY_AUTH_NONE, FALSE);
  gh_relay_scope_notice(scope, AUTH_URL, GH_RELAY_NOTICE_CLOSED, NULL, FALSE, OVERFLOW_DETAIL);
  drain_pending();
  g_assert_cmpuint(fixture.base.closed_notices, ==, 1);
  g_assert_cmpuint(fixture.resubscribes, ==, 0);
  /* A pending retry is dropped when the scope goes away. */
  gh_relay_scope_unref(scope);
  auth_fixture_clear(&fixture);
  scope = auth_scope_new(&fixture, FAKE_SIGN_OK, GH_RELAY_AUTH_NONE, TRUE);
  gh_relay_scope_notice(scope, AUTH_URL, GH_RELAY_NOTICE_CLOSED, NULL, FALSE, OVERFLOW_DETAIL);
  gh_relay_scope_unref(scope);
  drain_pending();
  g_assert_cmpuint(fixture.resubscribes, ==, 0);
  auth_fixture_clear(&fixture);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/relay/destinations", test_destinations_late_add_and_generation);
  g_test_add_func("/groundhog/relay/url-bound", test_url_bound);
  g_test_add_func("/groundhog/relay/auth/retries-req-once", test_auth_retries_req_once);
  g_test_add_func("/groundhog/relay/auth/challenge-after-closed", test_auth_challenge_after_closed);
  g_test_add_func("/groundhog/relay/auth/failures-report-closed", test_auth_failures_report_closed);
  g_test_add_func("/groundhog/relay/auth/off-is-unchanged", test_auth_off_is_unchanged);
  g_test_add_func("/groundhog/relay/auth/identity-per-url", test_auth_identity_per_url);
  g_test_add_func("/groundhog/relay/auth/ephemeral-fresh-per-connection", test_auth_ephemeral_fresh_per_connection);
  g_test_add_func("/groundhog/relay/auth/dropped-while-signing", test_auth_dropped_while_signing);
  g_test_add_func("/groundhog/relay/auth/generation-bound", test_auth_generation_bound);
  g_test_add_func("/groundhog/relay/overflow/reported-then-retried-once",
                  test_overflow_reported_then_retried_once);
  return g_test_run();
}
