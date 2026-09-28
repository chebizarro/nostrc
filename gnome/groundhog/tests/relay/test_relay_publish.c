#include "gh-relay-publish.h"

#include <gio/gio.h>
#include <nostr-event.h>
#include <stdlib.h>
#include <string.h>

#define OTHER_ID "0000000000000000000000000000000000000000000000000000000000000000"

typedef struct {
  GPtrArray *opened;      /* URLs handed to the transport */
  GPtrArray *closed;      /* URLs whose handle was closed */
  const gchar *fail_open; /* URL whose open fails synchronously */
  GPtrArray *results;     /* ResultCopy */
  guint done;
  GhRelayPublishSummary summary;
  gboolean cancel_in_update;
} Fixture;

typedef struct {
  gchar *url;
  GhRelayPublishOutcome outcome;
  GhRelayOkPrefix prefix;
  gchar *message;
} ResultCopy;

static void
result_copy_free(gpointer data)
{
  ResultCopy *copy = data;
  g_free(copy->url);
  g_free(copy->message);
  g_free(copy);
}

static gpointer
fake_open(GhRelayPublish *publish, const gchar *url, const gchar *event_json,
          gpointer data, GError **error)
{
  Fixture *fixture = data;
  g_assert_nonnull(strstr(event_json, gh_relay_publish_get_event_id(publish)));
  g_ptr_array_add(fixture->opened, g_strdup(url));
  if (g_strcmp0(url, fixture->fail_open) == 0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED,
                        "connection refused");
    return NULL;
  }
  return g_strdup(url); /* one simulated private connection per URL */
}

static void
fake_close(gpointer handle, gpointer data)
{
  Fixture *fixture = data;
  g_ptr_array_add(fixture->closed, handle); /* takes the URL copy */
}

static const GhRelayPublishTransport fake_transport = {
  .open = fake_open,
  .close = fake_close,
};

static void
on_update(GhRelayPublish *publish, const GhRelayPublishResult *result,
          gpointer data)
{
  Fixture *fixture = data;
  g_assert_cmpint(result->outcome, !=, GH_RELAY_PUBLISH_PENDING);
  g_assert_cmpint(result->outcome, !=, GH_RELAY_PUBLISH_CANCELLED);
  /* The URL's connection is already released when its outcome is reported. */
  g_assert_true(g_ptr_array_find_with_equal_func(fixture->closed, result->url,
                                                 g_str_equal, NULL) ||
                g_strcmp0(result->url, fixture->fail_open) == 0);
  ResultCopy *copy = g_new0(ResultCopy, 1);
  copy->url = g_strdup(result->url);
  copy->outcome = result->outcome;
  copy->prefix = result->prefix;
  copy->message = g_strdup(result->message);
  g_ptr_array_add(fixture->results, copy);
  if (fixture->cancel_in_update)
    gh_relay_publish_cancel(publish);
}

static void
on_done(GhRelayPublish *publish, const GhRelayPublishSummary *summary,
        gpointer data)
{
  Fixture *fixture = data;
  g_assert_true(gh_relay_publish_is_complete(publish));
  fixture->done++;
  fixture->summary = *summary;
}

static void
fixture_init(Fixture *fixture)
{
  memset(fixture, 0, sizeof *fixture);
  fixture->opened = g_ptr_array_new_with_free_func(g_free);
  fixture->closed = g_ptr_array_new_with_free_func(g_free);
  fixture->results = g_ptr_array_new_with_free_func(result_copy_free);
}

static void
fixture_clear(Fixture *fixture)
{
  g_ptr_array_unref(fixture->opened);
  g_ptr_array_unref(fixture->closed);
  g_ptr_array_unref(fixture->results);
}

static ResultCopy *
result_at(Fixture *fixture, guint index)
{
  g_assert_cmpuint(index, <, fixture->results->len);
  return g_ptr_array_index(fixture->results, index);
}

static NostrEvent *
note_event(const gchar *content)
{
  NostrEvent *event = nostr_event_new();
  g_assert_nonnull(event);
  nostr_event_set_created_at(event, 1700000000);
  nostr_event_set_kind(event, 1);
  nostr_event_set_content(event, content);
  return event;
}

static gchar *
take_json(NostrEvent *event)
{
  char *json = nostr_event_serialize_compact(event);
  g_assert_nonnull(json);
  gchar *copy = g_strdup(json);
  free(json);
  nostr_event_free(event);
  return copy;
}

static gchar *
signed_json(const gchar *content)
{
  NostrEvent *event = note_event(content);
  g_assert_cmpint(nostr_event_sign(event,
    "0000000000000000000000000000000000000000000000000000000000000001"), ==, 0);
  return take_json(event);
}

static GhRelayPublish *
new_publish(Fixture *fixture, const gchar *json)
{
  g_autoptr(GError) error = NULL;
  GhRelayPublish *publish = gh_relay_publish_new_with_transport(9, json,
    &fake_transport, fixture, on_update, on_done, fixture, &error);
  g_assert_no_error(error);
  g_assert_nonnull(publish);
  return publish;
}

static void
test_requires_signed_event(void)
{
  Fixture fixture;
  fixture_init(&fixture);
  g_autofree gchar *unsigned_json = take_json(note_event("unsigned"));
  g_autofree gchar *good = signed_json("signed");
  g_autofree gchar *tampered = g_strdup(good);
  gchar *content = strstr(tampered, "\"signed\"");
  g_assert_nonnull(content);
  content[1] = 'S'; /* id and sig no longer match the content */
  const gchar *bad[] = { NULL, "", "{bad json", unsigned_json, tampered };
  for (gsize i = 0; i < G_N_ELEMENTS(bad); i++) {
    g_autoptr(GError) error = NULL;
    g_assert_null(gh_relay_publish_new_with_transport(1, bad[i],
      &fake_transport, &fixture, on_update, on_done, &fixture, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  }
  g_assert_cmpuint(fixture.opened->len, ==, 0);
  fixture_clear(&fixture);
}

static void
test_url_set_is_explicit_and_bounded(void)
{
  Fixture fixture;
  fixture_init(&fixture);
  g_autofree gchar *json = signed_json("bounded");
  GhRelayPublish *publish = new_publish(&fixture, json);
  g_autoptr(GError) error = NULL;
  /* No URL, no publish: there are no default or fallback relays. */
  g_assert_false(gh_relay_publish_start(publish, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);
  g_assert_cmpuint(fixture.opened->len, ==, 0);
  const gchar *invalid[] = { NULL, "https://nos.lol", "wss://user:pw@nos.lol",
                             "nos.lol", "ws://" };
  for (gsize i = 0; i < G_N_ELEMENTS(invalid); i++) {
    g_assert_false(gh_relay_publish_add_url(publish, invalid[i], &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
  }
  for (guint i = 0; i < 16; i++) {
    g_autofree gchar *url = g_strdup_printf("wss://relay%u.example", i);
    g_assert_true(gh_relay_publish_add_url(publish, url, &error));
  }
  g_assert_true(gh_relay_publish_add_url(publish, "wss://relay0.example", &error));
  g_assert_false(gh_relay_publish_add_url(publish, "wss://overflow.example", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
  g_clear_error(&error);
  g_assert_true(gh_relay_publish_start(publish, &error));
  g_assert_cmpuint(fixture.opened->len, ==, 16);
  for (guint i = 0; i < 16; i++) {
    g_autofree gchar *url = g_strdup_printf("wss://relay%u.example", i);
    g_assert_cmpstr(g_ptr_array_index(fixture.opened, i), ==, url);
  }
  /* The set is fixed once started. */
  g_assert_false(gh_relay_publish_add_url(publish, "wss://late.example", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY);
  g_clear_error(&error);
  g_assert_false(gh_relay_publish_start(publish, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY);
  g_assert_cmpuint(fixture.opened->len, ==, 16);
  gh_relay_publish_unref(publish); /* last ref cancels: all 16 closed */
  g_assert_cmpuint(fixture.closed->len, ==, 16);
  g_assert_cmpuint(fixture.results->len, ==, 0);
  g_assert_cmpuint(fixture.done, ==, 0);
  fixture_clear(&fixture);
}

/* One OK per URL, matched by event id; a relay's answer never touches
 * another relay's outcome; partial success is reported in the aggregate. */
static void
test_per_url_outcomes_and_partial_success(void)
{
  Fixture fixture;
  fixture_init(&fixture);
  fixture.fail_open = "wss://down.example";
  g_autofree gchar *json = signed_json("partial");
  GhRelayPublish *publish = new_publish(&fixture, json);
  const gchar *id = gh_relay_publish_get_event_id(publish);
  g_assert_cmpuint(strlen(id), ==, 64);
  g_assert_true(gh_relay_publish_add_url(publish, "wss://nos.lol", NULL));
  g_assert_true(gh_relay_publish_add_url(publish, "wss://relay.nostr.band", NULL));
  g_assert_true(gh_relay_publish_add_url(publish, "wss://auth.example", NULL));
  g_assert_true(gh_relay_publish_add_url(publish, "wss://down.example", NULL));
  /* Deliveries before start are not admitted. */
  gh_relay_publish_ok(publish, "wss://nos.lol", id, TRUE, "");
  g_assert_true(gh_relay_publish_start(publish, NULL));
  g_assert_cmpuint(fixture.opened->len, ==, 4);
  /* A failed open is CONNECTION_FAILED for that URL only. */
  g_assert_cmpuint(fixture.results->len, ==, 1);
  g_assert_cmpstr(result_at(&fixture, 0)->url, ==, "wss://down.example");
  g_assert_cmpint(result_at(&fixture, 0)->outcome, ==,
                  GH_RELAY_PUBLISH_CONNECTION_FAILED);
  g_assert_cmpstr(result_at(&fixture, 0)->message, ==, "connection refused");

  /* OK for a URL outside the set, and OK for another event id: ignored. */
  gh_relay_publish_ok(publish, "wss://other.example", id, TRUE, "");
  gh_relay_publish_ok(publish, "wss://nos.lol", OTHER_ID, FALSE, "blocked: no");
  gh_relay_publish_ok(publish, "wss://nos.lol", NULL, FALSE, "blocked: no");
  g_assert_cmpuint(fixture.results->len, ==, 1);
  g_assert_cmpint(gh_relay_publish_get_outcome(publish, "wss://nos.lol"), ==,
                  GH_RELAY_PUBLISH_PENDING);

  gh_relay_publish_ok(publish, "wss://nos.lol", id, TRUE, "");
  g_assert_cmpuint(fixture.results->len, ==, 2);
  g_assert_cmpint(result_at(&fixture, 1)->outcome, ==, GH_RELAY_PUBLISH_ACCEPTED);
  g_assert_cmpint(result_at(&fixture, 1)->prefix, ==, GH_RELAY_OK_PREFIX_NONE);
  /* Only the accepting relay's connection was released. */
  g_assert_cmpuint(fixture.closed->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(fixture.closed, 0), ==, "wss://nos.lol");
  /* Repeated OKs (either verdict) after the terminal outcome count once. */
  gh_relay_publish_ok(publish, "wss://nos.lol", id, FALSE, "error: late");
  gh_relay_publish_ok(publish, "wss://nos.lol", id, TRUE, "");
  gh_relay_publish_failed(publish, "wss://nos.lol", "lost");
  g_assert_cmpuint(fixture.results->len, ==, 2);
  g_assert_cmpint(gh_relay_publish_get_outcome(publish, "wss://nos.lol"), ==,
                  GH_RELAY_PUBLISH_ACCEPTED);
  g_assert_cmpint(gh_relay_publish_get_outcome(publish, "wss://relay.nostr.band"),
                  ==, GH_RELAY_PUBLISH_PENDING);

  gh_relay_publish_ok(publish, "wss://relay.nostr.band", id, FALSE,
                      "rate-limited: slow down");
  g_assert_cmpint(result_at(&fixture, 2)->outcome, ==, GH_RELAY_PUBLISH_REJECTED);
  g_assert_cmpint(result_at(&fixture, 2)->prefix, ==, GH_RELAY_OK_PREFIX_RATE_LIMITED);
  g_assert_cmpstr(result_at(&fixture, 2)->message, ==, "rate-limited: slow down");
  g_assert_cmpuint(fixture.done, ==, 0);

  /* AUTH is surfaced, not performed. */
  gh_relay_publish_ok(publish, "wss://auth.example", id, FALSE,
                      "auth-required: sign in first");
  g_assert_cmpint(result_at(&fixture, 3)->outcome, ==,
                  GH_RELAY_PUBLISH_AUTH_REQUIRED);
  g_assert_cmpint(result_at(&fixture, 3)->prefix, ==,
                  GH_RELAY_OK_PREFIX_AUTH_REQUIRED);
  g_assert_cmpuint(fixture.done, ==, 1);
  g_assert_cmpuint(fixture.summary.total, ==, 4);
  g_assert_cmpuint(fixture.summary.accepted, ==, 1);
  g_assert_cmpuint(fixture.summary.rejected, ==, 1);
  g_assert_cmpuint(fixture.summary.auth_required, ==, 1);
  g_assert_cmpuint(fixture.summary.connection_failed, ==, 1);
  g_assert_true(fixture.summary.any_accepted);
  g_assert_false(fixture.summary.all_failed);
  g_assert_cmpuint(fixture.closed->len, ==, 3); /* down.example never opened */

  gh_relay_publish_ok(publish, "wss://auth.example", id, TRUE, "");
  gh_relay_publish_cancel(publish);
  g_assert_cmpuint(fixture.results->len, ==, 4);
  g_assert_cmpuint(fixture.done, ==, 1);
  /* Terminal outcomes survive a later cancel. */
  g_assert_cmpint(gh_relay_publish_get_outcome(publish, "wss://nos.lol"), ==,
                  GH_RELAY_PUBLISH_ACCEPTED);
  gh_relay_publish_unref(publish);
  fixture_clear(&fixture);
}

static void
test_ok_prefix_classification(void)
{
  static const struct {
    const gchar *message;
    GhRelayOkPrefix prefix;
  } cases[] = {
    { "duplicate: already have this event", GH_RELAY_OK_PREFIX_DUPLICATE },
    { "pow: difficulty 25>=24", GH_RELAY_OK_PREFIX_POW },
    { "blocked: you are banned", GH_RELAY_OK_PREFIX_BLOCKED },
    { "rate-limited: slow down", GH_RELAY_OK_PREFIX_RATE_LIMITED },
    { "invalid: bad signature", GH_RELAY_OK_PREFIX_INVALID },
    { "restricted: not allowed to write", GH_RELAY_OK_PREFIX_RESTRICTED },
    { "mute: no one was listening", GH_RELAY_OK_PREFIX_MUTE },
    { "error: could not connect to the database", GH_RELAY_OK_PREFIX_ERROR },
    { "auth-required: we only accept events from registered users",
      GH_RELAY_OK_PREFIX_AUTH_REQUIRED },
    { "", GH_RELAY_OK_PREFIX_NONE },
    { "stored", GH_RELAY_OK_PREFIX_NONE },
    { "Blocked: case matters", GH_RELAY_OK_PREFIX_NONE },
    { " blocked: leading space", GH_RELAY_OK_PREFIX_NONE },
    { "duplicate", GH_RELAY_OK_PREFIX_NONE },
  };
  g_assert_cmpint(gh_relay_ok_prefix_classify(NULL), ==, GH_RELAY_OK_PREFIX_NONE);
  for (gsize i = 0; i < G_N_ELEMENTS(cases); i++)
    g_assert_cmpint(gh_relay_ok_prefix_classify(cases[i].message), ==,
                    cases[i].prefix);

  /* Every rejection prefix, one relay each, and an accepted duplicate. */
  Fixture fixture;
  fixture_init(&fixture);
  g_autofree gchar *json = signed_json("prefixes");
  GhRelayPublish *publish = new_publish(&fixture, json);
  const gchar *id = gh_relay_publish_get_event_id(publish);
  for (gsize i = 0; i < 9; i++) {
    g_autofree gchar *url = g_strdup_printf("wss://r%zu.example", i);
    g_assert_true(gh_relay_publish_add_url(publish, url, NULL));
  }
  g_assert_true(gh_relay_publish_start(publish, NULL));
  for (gsize i = 0; i < 9; i++) {
    g_autofree gchar *url = g_strdup_printf("wss://r%zu.example", i);
    gh_relay_publish_ok(publish, url, id, FALSE, cases[i].message);
    ResultCopy *result = result_at(&fixture, i);
    g_assert_cmpstr(result->url, ==, url);
    g_assert_cmpint(result->prefix, ==, cases[i].prefix);
    g_assert_cmpint(result->outcome, ==,
                    cases[i].prefix == GH_RELAY_OK_PREFIX_AUTH_REQUIRED
                      ? GH_RELAY_PUBLISH_AUTH_REQUIRED
                      : GH_RELAY_PUBLISH_REJECTED);
  }
  g_assert_cmpuint(fixture.done, ==, 1);
  g_assert_false(fixture.summary.any_accepted);
  g_assert_true(fixture.summary.all_failed);
  g_assert_cmpuint(fixture.summary.rejected, ==, 8);
  g_assert_cmpuint(fixture.summary.auth_required, ==, 1);
  gh_relay_publish_unref(publish);

  Fixture dup;
  fixture_init(&dup);
  publish = new_publish(&dup, json);
  /* The first publish (and the id buffer it owned) is gone. */
  id = gh_relay_publish_get_event_id(publish);
  g_assert_true(gh_relay_publish_add_url(publish, "wss://nos.lol", NULL));
  g_assert_true(gh_relay_publish_start(publish, NULL));
  gh_relay_publish_ok(publish, "wss://nos.lol", id, TRUE,
                      "duplicate: already have this event");
  g_assert_cmpint(result_at(&dup, 0)->outcome, ==, GH_RELAY_PUBLISH_ACCEPTED);
  g_assert_cmpint(result_at(&dup, 0)->prefix, ==, GH_RELAY_OK_PREFIX_DUPLICATE);
  g_assert_true(dup.summary.any_accepted);
  gh_relay_publish_unref(publish);
  fixture_clear(&dup);
  fixture_clear(&fixture);
}

/* Revoking the generation closes every connection, admits no later OK and
 * runs no callback; pending URLs read back as CANCELLED. */
static void
test_cancel_discards_late_ok(void)
{
  Fixture fixture;
  fixture_init(&fixture);
  g_autofree gchar *json = signed_json("cancel");
  GhRelayPublish *publish = new_publish(&fixture, json);
  const gchar *id = gh_relay_publish_get_event_id(publish);
  g_assert_true(gh_relay_publish_add_url(publish, "wss://nos.lol", NULL));
  g_assert_true(gh_relay_publish_add_url(publish, "wss://relay.nostr.band", NULL));
  g_assert_true(gh_relay_publish_start(publish, NULL));
  gh_relay_publish_ok(publish, "wss://nos.lol", id, TRUE, "");
  g_assert_cmpuint(fixture.results->len, ==, 1);
  gh_relay_publish_cancel(publish);
  g_assert_cmpuint(gh_relay_publish_get_generation(publish), ==, 10);
  g_assert_cmpuint(fixture.closed->len, ==, 2);
  gh_relay_publish_ok(publish, "wss://relay.nostr.band", id, TRUE, "");
  gh_relay_publish_failed(publish, "wss://relay.nostr.band", "late");
  g_assert_cmpuint(fixture.results->len, ==, 1);
  g_assert_cmpuint(fixture.done, ==, 0);
  g_assert_false(gh_relay_publish_is_complete(publish));
  g_assert_cmpint(gh_relay_publish_get_outcome(publish, "wss://relay.nostr.band"),
                  ==, GH_RELAY_PUBLISH_CANCELLED);
  g_assert_cmpint(gh_relay_publish_get_outcome(publish, "wss://nos.lol"), ==,
                  GH_RELAY_PUBLISH_ACCEPTED);
  g_autoptr(GError) error = NULL;
  g_assert_false(gh_relay_publish_start(publish, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  gh_relay_publish_unref(publish);
  g_assert_cmpuint(fixture.closed->len, ==, 2);
  fixture_clear(&fixture);

  /* Cancelling from inside an update stops the remaining callbacks,
   * including the completion that this very outcome would have triggered. */
  Fixture inner;
  fixture_init(&inner);
  inner.cancel_in_update = TRUE;
  publish = new_publish(&inner, json);
  id = gh_relay_publish_get_event_id(publish); /* the previous owner is gone */
  g_assert_true(gh_relay_publish_add_url(publish, "wss://nos.lol", NULL));
  g_assert_true(gh_relay_publish_start(publish, NULL));
  gh_relay_publish_ok(publish, "wss://nos.lol", id, TRUE, "");
  g_assert_cmpuint(inner.results->len, ==, 1);
  g_assert_cmpuint(inner.done, ==, 0);
  gh_relay_publish_unref(publish);
  fixture_clear(&inner);
}

typedef struct {
  guint updates;
  GhRelayPublishOutcome outcome;
  gchar *message;
  gboolean done;
} DeadlineState;

static void
on_deadline_update(GhRelayPublish *publish, const GhRelayPublishResult *result,
                   gpointer data)
{
  (void)publish;
  DeadlineState *state = data;
  state->updates++;
  state->outcome = result->outcome;
  g_free(state->message);
  state->message = g_strdup(result->message);
}

static void
on_deadline_done(GhRelayPublish *publish, const GhRelayPublishSummary *summary,
                 gpointer data)
{
  (void)publish;
  DeadlineState *state = data;
  g_assert_true(summary->all_failed);
  state->done = TRUE;
}

static gboolean
deadline_test_expired(gpointer data)
{
  *(gboolean *)data = TRUE;
  return G_SOURCE_REMOVE;
}

/* A relay that never answers ends as CONNECTION_FAILED at the failure bound.
 * The test waits on the publish's own completion, not on a sleep. */
static void
test_deadline_is_failure_only(void)
{
  Fixture fixture;
  fixture_init(&fixture);
  DeadlineState state = {0};
  g_autofree gchar *json = signed_json("deadline");
  GhRelayPublish *publish = gh_relay_publish_new_with_transport(3, json,
    &fake_transport, &fixture, on_deadline_update, on_deadline_done, &state, NULL);
  gh_relay_publish_set_deadline(publish, 0); /* clamped to the 1 s minimum */
  g_assert_true(gh_relay_publish_add_url(publish, "wss://silent.example", NULL));
  g_assert_true(gh_relay_publish_start(publish, NULL));
  gboolean timed_out = FALSE;
  guint guard = g_timeout_add_seconds(10, deadline_test_expired, &timed_out);
  while (!state.done && !timed_out)
    g_main_context_iteration(NULL, TRUE);
  g_assert_false(timed_out);
  g_source_remove(guard);
  g_assert_cmpuint(state.updates, ==, 1);
  g_assert_cmpint(state.outcome, ==, GH_RELAY_PUBLISH_CONNECTION_FAILED);
  g_assert_nonnull(strstr(state.message, "failure bound"));
  g_assert_cmpuint(fixture.closed->len, ==, 1);
  /* A late OK after the bound does not flip the outcome. */
  gh_relay_publish_ok(publish, "wss://silent.example",
                      gh_relay_publish_get_event_id(publish), TRUE, "");
  g_assert_cmpuint(state.updates, ==, 1);
  gh_relay_publish_unref(publish);
  g_free(state.message);
  fixture_clear(&fixture);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/relay-publish/requires-signed-event", test_requires_signed_event);
  g_test_add_func("/groundhog/relay-publish/url-set", test_url_set_is_explicit_and_bounded);
  g_test_add_func("/groundhog/relay-publish/per-url-outcomes", test_per_url_outcomes_and_partial_success);
  g_test_add_func("/groundhog/relay-publish/ok-prefixes", test_ok_prefix_classification);
  g_test_add_func("/groundhog/relay-publish/cancel-discards-late-ok", test_cancel_discards_late_ok);
  g_test_add_func("/groundhog/relay-publish/deadline-failure-only", test_deadline_is_failure_only);
  return g_test_run();
}
