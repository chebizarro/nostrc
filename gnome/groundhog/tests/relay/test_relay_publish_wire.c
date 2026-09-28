#include "gh-relay-publish.h"
#include "fake-auth-signer.h"
#include "wire-relay.h"

#include <nostr-gobject-1.0/nostr_relay.h>

#define OTHER_ID "1111111111111111111111111111111111111111111111111111111111111111"

/* How a local relay answers an EVENT. */
typedef struct {
  gboolean accepted;
  const gchar *message;
  gboolean mismatched_first; /* an OK for another event id comes first */
  gboolean repeat_after;     /* a second, contradicting OK follows */
  gboolean hold;             /* no answer; keep the connection for later */
  gchar *held_id;
  SoupWebsocketConnection *held;
} Script;

typedef struct {
  guint updates;
  guint done;
  GhRelayPublishSummary summary;
  GHashTable *outcomes; /* url -> GINT_TO_POINTER(outcome) */
  GHashTable *prefixes; /* url -> GINT_TO_POINTER(prefix) */
  GHashTable *messages; /* url -> message copy */
  GThread *expected_thread;
  guint wrong_thread;
} Outcomes;

static void
send_ok(SoupWebsocketConnection *connection, const gchar *event_id,
        gboolean accepted, const gchar *message)
{
  g_autofree gchar *frame = g_strdup_printf("[\"OK\",\"%s\",%s,\"%s\"]",
                                            event_id,
                                            accepted ? "true" : "false",
                                            message ? message : "");
  soup_websocket_connection_send_text(connection, frame);
}

static void
scripted_event(WireRelay *relay, SoupWebsocketConnection *connection,
               const gchar *event_id, gpointer data)
{
  (void)relay;
  Script *script = data;
  if (script->hold) {
    g_free(script->held_id);
    script->held_id = g_strdup(event_id);
    g_set_object(&script->held, connection);
    return;
  }
  if (script->mismatched_first)
    send_ok(connection, OTHER_ID, !script->accepted, "blocked: not this event");
  send_ok(connection, event_id, script->accepted, script->message);
  if (script->repeat_after)
    send_ok(connection, event_id, !script->accepted, "error: repeated OK");
}

static void
scripted_relay(WireRelay *relay, Script *script)
{
  relay_init(relay);
  relay->on_event = scripted_event;
  relay->on_event_data = script;
}

static void
script_clear(Script *script)
{
  g_free(script->held_id);
  g_clear_object(&script->held);
}

static void
outcomes_init(Outcomes *outcomes)
{
  memset(outcomes, 0, sizeof *outcomes);
  outcomes->outcomes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  outcomes->prefixes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  outcomes->messages = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

static void
outcomes_clear(Outcomes *outcomes)
{
  g_hash_table_unref(outcomes->outcomes);
  g_hash_table_unref(outcomes->prefixes);
  g_hash_table_unref(outcomes->messages);
}

static GhRelayPublishOutcome
outcome_for(Outcomes *outcomes, const gchar *url)
{
  gpointer value = NULL;
  g_assert_true(g_hash_table_lookup_extended(outcomes->outcomes, url, NULL, &value));
  return GPOINTER_TO_INT(value);
}

static void
on_update(GhRelayPublish *publish, const GhRelayPublishResult *result,
          gpointer data)
{
  (void)publish;
  Outcomes *outcomes = data;
  if (outcomes->expected_thread && g_thread_self() != outcomes->expected_thread)
    outcomes->wrong_thread++;
  outcomes->updates++;
  /* Exactly one terminal outcome per URL. */
  g_assert_false(g_hash_table_contains(outcomes->outcomes, result->url));
  g_hash_table_insert(outcomes->outcomes, g_strdup(result->url),
                      GINT_TO_POINTER(result->outcome));
  g_hash_table_insert(outcomes->prefixes, g_strdup(result->url),
                      GINT_TO_POINTER(result->prefix));
  g_hash_table_insert(outcomes->messages, g_strdup(result->url),
                      g_strdup(result->message ? result->message : ""));
}

static void
on_done(GhRelayPublish *publish, const GhRelayPublishSummary *summary,
        gpointer data)
{
  (void)publish;
  Outcomes *outcomes = data;
  if (outcomes->expected_thread && g_thread_self() != outcomes->expected_thread)
    outcomes->wrong_thread++;
  outcomes->done++;
  outcomes->summary = *summary;
}

static gchar *
signed_json(const gchar *content)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 1);
  nostr_event_set_created_at(event, 1700000000);
  nostr_event_set_content(event, content);
  g_assert_cmpint(nostr_event_sign(event,
    "0000000000000000000000000000000000000000000000000000000000000001"), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  gchar *copy = g_strdup(json);
  free(json);
  return copy;
}

static void
drain_default_context(void)
{
  while (g_main_context_pending(NULL))
    g_main_context_iteration(NULL, FALSE);
}

/* Each URL answers on its own connection: an accepting relay that also sends
 * an OK for another event and then a contradicting repeat, a blocked relay,
 * an auth-required relay and a relay that is down. A relay outside the set
 * is never contacted. */
static void
test_wire_partial_success(void)
{
  WireRelay accept = {0}, reject = {0}, auth = {0}, bystander = {0};
  Script accept_script = { .accepted = TRUE, .message = "",
                           .mismatched_first = TRUE, .repeat_after = TRUE };
  Script reject_script = { .accepted = FALSE, .message = "blocked: go away" };
  Script auth_script = { .accepted = FALSE,
                         .message = "auth-required: registered users only" };
  Script bystander_script = { .accepted = TRUE, .message = "" };
  scripted_relay(&accept, &accept_script);
  scripted_relay(&reject, &reject_script);
  scripted_relay(&auth, &auth_script);
  scripted_relay(&bystander, &bystander_script);
  g_autofree gchar *down_url = unused_relay_url(NULL);

  Outcomes outcomes;
  outcomes_init(&outcomes);
  g_autofree gchar *json = signed_json("wire partial");
  g_autoptr(GError) error = NULL;
  GhRelayPublish *publish = gh_relay_publish_new(5, json, on_update, on_done,
                                                 &outcomes, &error);
  g_assert_no_error(error);
  g_assert_true(gh_relay_publish_add_url(publish, accept.url, NULL));
  g_assert_true(gh_relay_publish_add_url(publish, reject.url, NULL));
  g_assert_true(gh_relay_publish_add_url(publish, auth.url, NULL));
  g_assert_true(gh_relay_publish_add_url(publish, down_url, NULL));
  g_assert_true(gh_relay_publish_start(publish, &error));
  g_assert_no_error(error);
  wait_for_count(&outcomes.done, 1);

  g_assert_cmpuint(outcomes.updates, ==, 4);
  g_assert_cmpint(outcome_for(&outcomes, accept.url), ==, GH_RELAY_PUBLISH_ACCEPTED);
  g_assert_cmpint(outcome_for(&outcomes, reject.url), ==, GH_RELAY_PUBLISH_REJECTED);
  g_assert_cmpint(GPOINTER_TO_INT(g_hash_table_lookup(outcomes.prefixes, reject.url)),
                  ==, GH_RELAY_OK_PREFIX_BLOCKED);
  g_assert_cmpint(outcome_for(&outcomes, auth.url), ==,
                  GH_RELAY_PUBLISH_AUTH_REQUIRED);
  g_assert_cmpint(outcome_for(&outcomes, down_url), ==,
                  GH_RELAY_PUBLISH_CONNECTION_FAILED);
  g_assert_cmpuint(outcomes.summary.total, ==, 4);
  g_assert_cmpuint(outcomes.summary.accepted, ==, 1);
  g_assert_true(outcomes.summary.any_accepted);
  g_assert_false(outcomes.summary.all_failed);

  /* One EVENT per relay, each on its own connection; none elsewhere. */
  g_assert_cmpuint(accept.events, ==, 1);
  g_assert_cmpuint(reject.events, ==, 1);
  g_assert_cmpuint(auth.events, ==, 1);
  g_assert_cmpuint(accept.connections->len, ==, 1);
  g_assert_cmpuint(reject.connections->len, ==, 1);
  g_assert_cmpuint(auth.connections->len, ==, 1);
  g_assert_cmpuint(bystander.connections->len, ==, 0);
  g_assert_cmpuint(accept.reqs + reject.reqs + auth.reqs, ==, 0);

  /* A terminal outcome releases that relay's private connection. */
  wait_for_close(&accept, 1);
  wait_for_close(&reject, 1);
  wait_for_close(&auth, 1);
  drain_default_context();
  g_assert_cmpuint(outcomes.updates, ==, 4); /* repeated OK counted once */
  g_assert_cmpuint(outcomes.done, ==, 1);

  gh_relay_publish_unref(publish);
  relay_clear(&accept);
  relay_clear(&reject);
  relay_clear(&auth);
  relay_clear(&bystander);
  outcomes_clear(&outcomes);
}

/* Every relay down: no fallback is tried, and the aggregate is all-failed. */
static void
test_wire_relay_down(void)
{
  g_autofree gchar *down_url = unused_relay_url(NULL);
  Outcomes outcomes;
  outcomes_init(&outcomes);
  g_autofree gchar *json = signed_json("wire down");
  GhRelayPublish *publish = gh_relay_publish_new(5, json, on_update, on_done,
                                                 &outcomes, NULL);
  g_assert_true(gh_relay_publish_add_url(publish, down_url, NULL));
  g_assert_true(gh_relay_publish_start(publish, NULL));
  wait_for_count(&outcomes.done, 1);
  g_assert_cmpuint(outcomes.updates, ==, 1);
  g_assert_cmpint(outcome_for(&outcomes, down_url), ==,
                  GH_RELAY_PUBLISH_CONNECTION_FAILED);
  g_assert_false(outcomes.summary.any_accepted);
  g_assert_true(outcomes.summary.all_failed);
  gh_relay_publish_unref(publish);
  outcomes_clear(&outcomes);
}

/* The relay has the EVENT; the account generation is revoked before its OK.
 * Cancellation closes the socket and an OK sent afterwards produces no
 * callback (it usually never reaches the closed client at all; the variant
 * below makes the OK arrive). */
static void
test_wire_cancel_discards_late_ok(void)
{
  WireRelay relay = {0};
  Script script = { .hold = TRUE };
  scripted_relay(&relay, &script);
  Outcomes outcomes;
  outcomes_init(&outcomes);
  g_autofree gchar *json = signed_json("wire cancel");
  GhRelayPublish *publish = gh_relay_publish_new(5, json, on_update, on_done,
                                                 &outcomes, NULL);
  g_assert_true(gh_relay_publish_add_url(publish, relay.url, NULL));
  g_assert_true(gh_relay_publish_start(publish, NULL));
  wait_for_count(&relay.events, 1);
  g_assert_cmpstr(script.held_id, ==, gh_relay_publish_get_event_id(publish));

  gh_relay_publish_cancel(publish);
  if (soup_websocket_connection_get_state(script.held) == SOUP_WEBSOCKET_STATE_OPEN)
    send_ok(script.held, script.held_id, TRUE, "");
  wait_for_close(&relay, 1);
  drain_default_context();
  g_assert_cmpuint(outcomes.updates, ==, 0);
  g_assert_cmpuint(outcomes.done, ==, 0);
  g_assert_cmpint(gh_relay_publish_get_outcome(publish, relay.url), ==,
                  GH_RELAY_PUBLISH_CANCELLED);
  gh_relay_publish_unref(publish);
  script_clear(&script);
  relay_clear(&relay);
  outcomes_clear(&outcomes);
}

typedef struct {
  GhRelayPublish *publish;
  guint hooked;
} CancelOnOk;

static gboolean
cancel_on_ok_hook(GSignalInvocationHint *hint, guint n_values,
                  const GValue *values, gpointer data)
{
  (void)hint;
  CancelOnOk *state = data;
  g_assert_cmpuint(n_values, ==, 4); /* relay, event_id, accepted, message */
  if (g_strcmp0(g_value_get_string(&values[1]),
                gh_relay_publish_get_event_id(state->publish)) != 0)
    return TRUE;
  state->hooked++;
  gh_relay_publish_cancel(state->publish);
  return FALSE; /* removes the hook */
}

/* The relay's OK really reaches the client: the emission hook fires when
 * GNostrRelay emits it, and revokes the generation right there, before the
 * transport's queued delivery reaches the publish. That OK must be dropped. */
static void
test_wire_ok_arriving_after_cancel(void)
{
  WireRelay relay = {0};
  Script script = { .accepted = TRUE, .message = "" };
  scripted_relay(&relay, &script);
  Outcomes outcomes;
  outcomes_init(&outcomes);
  g_autofree gchar *json = signed_json("wire ok after cancel");
  GhRelayPublish *publish = gh_relay_publish_new(5, json, on_update, on_done,
                                                 &outcomes, NULL);
  CancelOnOk state = { .publish = publish };
  guint signal_id = g_signal_lookup("ok", GNOSTR_TYPE_RELAY);
  g_signal_add_emission_hook(signal_id, 0, cancel_on_ok_hook, &state, NULL);
  g_assert_true(gh_relay_publish_add_url(publish, relay.url, NULL));
  g_assert_true(gh_relay_publish_start(publish, NULL));
  wait_for_count(&state.hooked, 1);
  wait_for_close(&relay, 1);
  drain_default_context();
  g_assert_cmpuint(outcomes.updates, ==, 0);
  g_assert_cmpuint(outcomes.done, ==, 0);
  g_assert_cmpint(gh_relay_publish_get_outcome(publish, relay.url), ==,
                  GH_RELAY_PUBLISH_CANCELLED);
  g_assert_cmpuint(gh_relay_publish_get_generation(publish), ==, 6);
  gh_relay_publish_unref(publish);
  relay_clear(&relay);
  outcomes_clear(&outcomes);
}

typedef struct {
  const gchar *url;
  Outcomes outcomes;
  gint finished;
  gboolean timed_out;
} OwnerRun;

static gboolean
owner_expire(gpointer data)
{
  OwnerRun *run = data;
  run->timed_out = TRUE;
  return G_SOURCE_REMOVE;
}

static gpointer
owner_thread(gpointer data)
{
  OwnerRun *run = data;
  GMainContext *context = g_main_context_new();
  g_main_context_push_thread_default(context);
  run->outcomes.expected_thread = g_thread_self();
  g_autofree gchar *json = signed_json("wire owner");
  GhRelayPublish *publish = gh_relay_publish_new(8, json, on_update, on_done,
                                                 &run->outcomes, NULL);
  g_assert_true(gh_relay_publish_add_url(publish, run->url, NULL));
  g_assert_true(gh_relay_publish_start(publish, NULL));
  GSource *bound = g_timeout_source_new_seconds(18);
  g_source_set_callback(bound, owner_expire, run, NULL);
  g_source_attach(bound, context);
  while (run->outcomes.done == 0 && !run->timed_out)
    g_main_context_iteration(context, TRUE);
  g_source_destroy(bound);
  g_source_unref(bound);
  gh_relay_publish_unref(publish);
  g_main_context_pop_thread_default(context);
  g_main_context_unref(context);
  g_atomic_int_set(&run->finished, TRUE);
  g_main_context_wakeup(NULL);
  return NULL;
}

/* nostr-gobject emits "ok" from the global default context, iterated here by
 * the main thread. A publish owned by another thread's context must still see
 * every callback on that thread. */
static void
test_wire_callbacks_on_owner_context(void)
{
  WireRelay relay = {0};
  Script script = { .accepted = TRUE, .message = "" };
  scripted_relay(&relay, &script);
  OwnerRun run = { .url = relay.url };
  outcomes_init(&run.outcomes);
  GThread *thread = g_thread_new("publish-owner", owner_thread, &run);
  while (!g_atomic_int_get(&run.finished))
    g_main_context_iteration(NULL, TRUE);
  g_thread_join(thread);
  g_assert_false(run.timed_out);
  g_assert_cmpuint(run.outcomes.done, ==, 1);
  g_assert_cmpuint(run.outcomes.updates, ==, 1);
  g_assert_cmpuint(run.outcomes.wrong_thread, ==, 0);
  g_assert_cmpint(outcome_for(&run.outcomes, relay.url), ==,
                  GH_RELAY_PUBLISH_ACCEPTED);
  g_assert_cmpuint(relay.events, ==, 1);
  relay_clear(&relay);
  outcomes_clear(&run.outcomes);
}

/* ---- qp24.4.8: a lost connection is reported promptly ---- */

/* The relay drops the socket when the EVENT arrives (before any OK), or
 * right after the WebSocket upgrade. Either way the URL is
 * CONNECTION_FAILED from the transport's own signal, long before the 30 s
 * failure deadline (the wait's bound is 18 s, and the message is not the
 * deadline's). */
static void
test_wire_connection_lost_before_ok(void)
{
  for (guint variant = 0; variant < 2; variant++) {
    WireRelay relay = { .close_on_event = variant == 0,
                        .close_on_connect = variant == 1 };
    relay_init(&relay);
    Outcomes outcomes;
    outcomes_init(&outcomes);
    g_autofree gchar *json = signed_json("wire lost");
    GhRelayPublish *publish = gh_relay_publish_new(5, json, on_update, on_done,
                                                   &outcomes, NULL);
    g_assert_true(gh_relay_publish_add_url(publish, relay.url, NULL));
    g_assert_true(gh_relay_publish_start(publish, NULL));
    wait_for_count(&outcomes.done, 1);
    g_assert_cmpint(outcome_for(&outcomes, relay.url), ==,
                    GH_RELAY_PUBLISH_CONNECTION_FAILED);
    const gchar *message = g_hash_table_lookup(outcomes.messages, relay.url);
    g_test_message("lost-connection outcome: %s", message);
    g_assert_null(strstr(message, "failure bound"));
    if (variant == 0)
      g_assert_cmpuint(relay.events, ==, 1);
    gh_relay_publish_unref(publish);
    relay_clear(&relay);
    outcomes_clear(&outcomes);
  }
}

/* ---- NIP-42 against a real relay that requires AUTH for EVENT ---- */

static GhRelayPublish *
auth_publish(WireRelay *relays, gsize n_relays, Outcomes *outcomes,
             FakeSigner *fake, GhRelayAuthMode mode, const gchar *content)
{
  g_autofree gchar *json = signed_json(content);
  g_autoptr(GError) error = NULL;
  GhRelayPublish *publish = gh_relay_publish_new(12, json, on_update, on_done,
                                                 outcomes, &error);
  g_assert_no_error(error);
  g_autoptr(GhRelayAuthSigner) signer = fake_signer_new(fake, 12, NULL);
  g_assert_true(gh_relay_publish_set_account_signer(publish, signer, &error));
  for (gsize i = 0; i < n_relays; i++) {
    g_assert_true(gh_relay_publish_add_url(publish, relays[i].url, NULL));
    g_assert_true(gh_relay_publish_set_url_auth(publish, relays[i].url, mode, &error));
  }
  g_assert_no_error(error);
  g_assert_true(gh_relay_publish_start(publish, &error));
  return publish;
}

/* One event to two recipient-style relays in EPHEMERAL mode: each accepts
 * it after an AUTH, and the two AUTHs carry two different keys, neither
 * the account's. */
static void
test_wire_auth_ephemeral_two_relays(void)
{
  Script script = { .accepted = TRUE, .message = "" };
  WireRelay relays[2] = { { .require_auth = TRUE }, { .require_auth = TRUE } };
  for (guint i = 0; i < 2; i++)
    scripted_relay(&relays[i], &script);
  Outcomes outcomes;
  outcomes_init(&outcomes);
  FakeSigner fake = { .mode = FAKE_SIGN_OK };
  GhRelayPublish *publish = auth_publish(relays, 2, &outcomes, &fake,
                                         GH_RELAY_AUTH_EPHEMERAL, "wire ephemeral");
  wait_for_count(&outcomes.done, 1);
  g_autofree gchar *account = fake_account_pubkey();
  for (guint i = 0; i < 2; i++) {
    g_assert_cmpint(outcome_for(&outcomes, relays[i].url), ==, GH_RELAY_PUBLISH_ACCEPTED);
    g_assert_cmpuint(relays[i].refused_events, ==, 1);
    g_assert_cmpuint(relays[i].events, ==, 1);
    g_assert_cmpuint(relays[i].auth_ok, ==, 1);
    g_assert_cmpuint(relays[i].connections->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(relays[i].auth_pubkeys, 0), !=, account);
  }
  g_assert_cmpstr(g_ptr_array_index(relays[0].auth_pubkeys, 0), !=,
                  g_ptr_array_index(relays[1].auth_pubkeys, 0));
  g_assert_cmpuint(fake.calls, ==, 0);
  gh_relay_publish_unref(publish);
  for (guint i = 0; i < 2; i++)
    relay_clear(&relays[i]);
  outcomes_clear(&outcomes);
  fake_signer_clear(&fake);
}

/* ACCOUNT on the account's own relay: accepted after an AUTH as the
 * account. */
static void
test_wire_auth_account_own_relay(void)
{
  Script script = { .accepted = TRUE, .message = "" };
  WireRelay relay = { .require_auth = TRUE };
  scripted_relay(&relay, &script);
  Outcomes outcomes;
  outcomes_init(&outcomes);
  FakeSigner fake = { .mode = FAKE_SIGN_OK };
  GhRelayPublish *publish = auth_publish(&relay, 1, &outcomes, &fake,
                                         GH_RELAY_AUTH_ACCOUNT, "wire account");
  wait_for_count(&outcomes.done, 1);
  g_assert_cmpint(outcome_for(&outcomes, relay.url), ==, GH_RELAY_PUBLISH_ACCEPTED);
  g_autofree gchar *account = fake_account_pubkey();
  g_assert_cmpuint(relay.auth_pubkeys->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(relay.auth_pubkeys, 0), ==, account);
  g_assert_cmpuint(fake.calls, ==, 1);
  gh_relay_publish_unref(publish);
  relay_clear(&relay);
  outcomes_clear(&outcomes);
  fake_signer_clear(&fake);
}

/* NONE (the default, with an account signer present), a refused account
 * signature, and the relay refusing an ephemeral AUTH each end as
 * AUTH_REQUIRED; nothing unverified reaches the relay, and a refused
 * ephemeral AUTH is never retried as the account. */
static void
test_wire_auth_failures(void)
{
  static const struct {
    GhRelayAuthMode mode;
    FakeSignMode sign;
    gboolean refuse_auth;
    guint auth_frames;
    guint sign_calls;
  } cases[] = {
    { GH_RELAY_AUTH_NONE, FAKE_SIGN_OK, FALSE, 0, 0 },
    { GH_RELAY_AUTH_ACCOUNT, FAKE_SIGN_DENY, FALSE, 0, 1 },
    { GH_RELAY_AUTH_ACCOUNT, FAKE_SIGN_BAD_SIG, FALSE, 0, 1 },
    { GH_RELAY_AUTH_EPHEMERAL, FAKE_SIGN_OK, TRUE, 1, 0 },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
    Script script = { .accepted = TRUE, .message = "" };
    WireRelay relay = { .require_auth = TRUE, .refuse_auth = cases[i].refuse_auth };
    scripted_relay(&relay, &script);
    Outcomes outcomes;
    outcomes_init(&outcomes);
    FakeSigner fake = { .mode = cases[i].sign };
    GhRelayPublish *publish = auth_publish(&relay, 1, &outcomes, &fake,
                                           cases[i].mode, "wire auth failure");
    wait_for_count(&outcomes.done, 1);
    g_assert_cmpint(outcome_for(&outcomes, relay.url), ==, GH_RELAY_PUBLISH_AUTH_REQUIRED);
    g_assert_cmpint(GPOINTER_TO_INT(g_hash_table_lookup(outcomes.prefixes, relay.url)),
                    ==, GH_RELAY_OK_PREFIX_AUTH_REQUIRED);
    g_assert_cmpuint(relay.auth_frames, ==, cases[i].auth_frames);
    g_assert_cmpuint(relay.refused_events, ==, 1);
    g_assert_cmpuint(relay.events, ==, 0);
    g_assert_cmpuint(fake.calls, ==, cases[i].sign_calls);
    gh_relay_publish_unref(publish);
    relay_clear(&relay);
    outcomes_clear(&outcomes);
    fake_signer_clear(&fake);
  }
}

/* Cancelled while the account signer is still working: no callback, and
 * nothing reaches the relay. */
static void
test_wire_auth_cancel_while_signing(void)
{
  Script script = { .accepted = TRUE, .message = "" };
  WireRelay relay = { .require_auth = TRUE };
  scripted_relay(&relay, &script);
  Outcomes outcomes;
  outcomes_init(&outcomes);
  FakeSigner fake = { .mode = FAKE_SIGN_HOLD };
  GhRelayPublish *publish = auth_publish(&relay, 1, &outcomes, &fake,
                                         GH_RELAY_AUTH_ACCOUNT, "wire auth cancel");
  wait_for_count(&fake.calls, 1);
  gh_relay_publish_cancel(publish);
  g_assert_true(fake_signer_release(&fake));
  wait_for_close(&relay, 1);
  drain_default_context();
  g_assert_cmpuint(outcomes.updates, ==, 0);
  g_assert_cmpuint(outcomes.done, ==, 0);
  g_assert_cmpuint(relay.auth_frames, ==, 0);
  g_assert_cmpint(gh_relay_publish_get_outcome(publish, relay.url), ==,
                  GH_RELAY_PUBLISH_CANCELLED);
  gh_relay_publish_unref(publish);
  relay_clear(&relay);
  outcomes_clear(&outcomes);
  fake_signer_clear(&fake);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/relay-publish-wire/connection-lost-before-ok", test_wire_connection_lost_before_ok);
  g_test_add_func("/groundhog/relay-publish-wire/auth/ephemeral-two-relays", test_wire_auth_ephemeral_two_relays);
  g_test_add_func("/groundhog/relay-publish-wire/auth/account-own-relay", test_wire_auth_account_own_relay);
  g_test_add_func("/groundhog/relay-publish-wire/auth/failures", test_wire_auth_failures);
  g_test_add_func("/groundhog/relay-publish-wire/auth/cancel-while-signing", test_wire_auth_cancel_while_signing);
  g_test_add_func("/groundhog/relay-publish-wire/partial-success", test_wire_partial_success);
  g_test_add_func("/groundhog/relay-publish-wire/relay-down", test_wire_relay_down);
  g_test_add_func("/groundhog/relay-publish-wire/cancel-discards-late-ok", test_wire_cancel_discards_late_ok);
  g_test_add_func("/groundhog/relay-publish-wire/ok-after-cancel", test_wire_ok_arriving_after_cancel);
  g_test_add_func("/groundhog/relay-publish-wire/owner-context", test_wire_callbacks_on_owner_context);
  return g_test_run();
}
