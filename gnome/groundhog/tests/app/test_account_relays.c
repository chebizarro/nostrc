#include "gh-account-relays.h"
#include "gh-identity.h"
#include "nostr-event.h"
#include "nostr-keys.h"
#include "nostr-tag.h"
#include "nostr/nip19/nip19.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef GROUNDHOG_TEST_WIRE
#include "../relay/wire-relay.h"
#endif

#define SECRET_ONE "0000000000000000000000000000000000000000000000000000000000000001"
#define SECRET_TWO "0000000000000000000000000000000000000000000000000000000000000002"
#define SOURCE_A "wss://discovery-a.test.invalid"
#define SOURCE_B "wss://discovery-b.test.invalid"

static gchar *npub_one, *npub_two, *hex_one, *hex_two;

static gchar *
npub_for_secret(const gchar *secret)
{
  g_autofree gchar *hex = nostr_key_get_public(secret);
  guint8 bytes[32];
  g_assert_nonnull(hex);
  for (guint i = 0; i < 32; i++) {
    unsigned int value;
    g_assert_cmpint(sscanf(hex + 2 * i, "%2x", &value), ==, 1);
    bytes[i] = value;
  }
  gchar *npub = NULL;
  g_assert_cmpint(nostr_nip19_encode_npub(bytes, &npub), ==, 0);
  return npub;
}

static GhIdentityInfo *
identity(const gchar *npub, const gchar *label)
{
  GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
  info->npub = g_strdup(npub);
  info->label = g_strdup(label);
  return info;
}

static GPtrArray *
fake_list(gpointer data, GError **error)
{
  (void)data;
  (void)error;
  GPtrArray *ids = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  g_ptr_array_add(ids, identity(npub_one, "One"));
  g_ptr_array_add(ids, identity(npub_two, "Two"));
  return ids;
}

/* A signed replaceable list. tag is "r" (NIP-65) or "relay" (NIP-17). */
static gchar *
signed_list(const gchar *secret, int kind, gint64 created_at, const gchar *tag,
            const gchar *url, const gchar *marker, gchar **out_id)
{
  NostrEvent *event = nostr_event_new();
  g_assert_nonnull(event);
  nostr_event_set_kind(event, kind);
  nostr_event_set_created_at(event, created_at);
  nostr_event_set_content(event, "");
  nostr_event_set_tags(event, nostr_tags_new(1, nostr_tag_new(tag, url, marker, NULL)));
  g_assert_cmpint(nostr_event_sign(event, secret), ==, 0);
  if (out_id) {
    char *id = nostr_event_get_id(event);
    *out_id = g_strdup(id);
    free(id);
  }
  char *json = nostr_event_serialize_compact(event);
  g_assert_nonnull(json);
  gchar *copy = g_strdup(json);
  free(json);
  nostr_event_free(event);
  return copy;
}

static gboolean
deadline_hit(gpointer data)
{
  *(gboolean *)data = TRUE;
  return G_SOURCE_REMOVE;
}

static void
spin_until_at(gboolean (*pred)(gpointer), gpointer data, int line)
{
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(15, deadline_hit, &expired);
  while (!pred(data) && !expired)
    g_main_context_iteration(NULL, TRUE);
  if (expired)
    g_error("condition waited for at line %d did not hold within 15s", line);
  g_source_remove(timer);
}
#define spin_until(pred, data) spin_until_at((pred), (data), __LINE__)

static gboolean
listed(gpointer data)
{
  return gh_account_controller_get_state(data) != GH_ACCOUNT_STATE_DISCOVERING;
}

static GSettings *
fresh_settings(const gchar *current, const gchar *const *sources)
{
  GSettings *settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_strv(settings, "discovery-relays", sources);
  g_settings_set_string(settings, "current-npub", current);
  return settings;
}

static GhAccountController *
listed_controller(GSettings *settings)
{
  GhAccountController *controller =
    gh_account_controller_new_full(settings, NULL, fake_list, NULL);
  spin_until(listed, controller);
  return controller;
}

static gboolean
is_null(gpointer data)
{
  return *(gpointer *)data == NULL;
}

/* Waits for the listing task to drop its controller reference. */
static void
release(gpointer object)
{
  gpointer weak = object;
  g_object_add_weak_pointer(G_OBJECT(object), &weak);
  g_object_run_dispose(G_OBJECT(object));
  g_object_unref(object);
  spin_until(is_null, &weak);
}

static void
assert_strv(const gchar *const *actual, const gchar *first, ...)
{
  g_assert_nonnull(actual);
  va_list args;
  va_start(args, first);
  guint i = 0;
  for (const gchar *want = first; want; want = va_arg(args, const gchar *), i++)
    g_assert_cmpstr(actual[i], ==, want);
  va_end(args);
  g_assert_null(actual[i]);
}

/* Records every REQ the account layer asks for, and keeps each scope alive
 * so that late traffic can be replayed into it after teardown. */
typedef struct {
  GPtrArray *urls;
  GPtrArray *authors;
  GPtrArray *scopes;
  guint closed;
} Recorder;

static gpointer
recorder_open(GhRelayScope *scope, const gchar *url, const NostrFilters *filters,
              gpointer data, GError **error)
{
  Recorder *rec = data;
  (void)error;
  g_assert_cmpuint(filters->count, ==, 1);
  const NostrFilter *filter = &filters->filters[0];
  g_assert_cmpuint(nostr_filter_kinds_len(filter), ==, 2);
  g_assert_cmpint(nostr_filter_kinds_get(filter, 0), ==, 10002);
  g_assert_cmpint(nostr_filter_kinds_get(filter, 1), ==, 10050);
  g_assert_cmpuint(nostr_filter_authors_len(filter), ==, 1);
  g_ptr_array_add(rec->authors, g_strdup(nostr_filter_authors_get(filter, 0)));
  g_ptr_array_add(rec->urls, g_strdup(url));
  g_ptr_array_add(rec->scopes, gh_relay_scope_ref(scope));
  return GUINT_TO_POINTER(rec->urls->len);
}

static void
recorder_close(gpointer handle, gpointer data)
{
  Recorder *rec = data;
  (void)handle;
  rec->closed++;
}

static const GhRelayTransport recorder_transport = { recorder_open, recorder_close };

static void
recorder_init(Recorder *rec)
{
  rec->urls = g_ptr_array_new_with_free_func(g_free);
  rec->authors = g_ptr_array_new_with_free_func(g_free);
  rec->scopes = g_ptr_array_new_with_free_func((GDestroyNotify)gh_relay_scope_unref);
  rec->closed = 0;
}

static void
recorder_clear(Recorder *rec)
{
  g_ptr_array_unref(rec->urls);
  g_ptr_array_unref(rec->authors);
  g_ptr_array_unref(rec->scopes);
}

static GhRelayScope *
scope_at(Recorder *rec, guint index)
{
  return g_ptr_array_index(rec->scopes, index);
}

typedef struct {
  gboolean armed;
  gboolean ran;
  GhAccountRelays *relays;
  GhRelayScope *old_scope;
  gchar *event;
} StaleProbe;

/* Connected before GhAccountRelays, so it runs after the controller revoked
 * the old generation but before the old scope is torn down. */
static void
inject_before_teardown(GhAccountController *controller, gpointer data)
{
  StaleProbe *probe = data;
  (void)controller;
  if (!probe->armed)
    return;
  probe->armed = FALSE;
  probe->ran = TRUE;
  gh_relay_scope_event(probe->old_scope, SOURCE_A, probe->event);
  assert_strv(gh_account_relays_get_read_relays(probe->relays),
              "wss://one-read.test.invalid", NULL);
}

static void
test_switch_teardown(void)
{
  const gchar *sources[] = { SOURCE_A, SOURCE_B, NULL };
  g_autoptr(GSettings) settings = fresh_settings(npub_one, sources);
  GhAccountController *controller = listed_controller(settings);
  g_assert_cmpint(gh_account_controller_get_state(controller), ==, GH_ACCOUNT_STATE_ACTIVE);
  StaleProbe probe = { 0 };
  g_signal_connect(controller, "changed", G_CALLBACK(inject_before_teardown), &probe);
  Recorder rec;
  recorder_init(&rec);
  GhAccountRelays *relays =
    gh_account_relays_new(controller, settings, &recorder_transport, &rec);

  guint64 first = gh_account_controller_get_generation(controller);
  g_assert_cmpuint(gh_account_relays_get_generation(relays), ==, first);
  g_assert_cmpint(gh_account_relays_get_state(relays), ==, GH_ACCOUNT_RELAYS_DISCOVERING);
  g_assert_cmpuint(rec.urls->len, ==, 2);
  g_assert_cmpstr(g_ptr_array_index(rec.authors, 0), ==, hex_one);
  g_assert_cmpstr(g_ptr_array_index(rec.authors, 1), ==, hex_one);
  g_assert_null(gh_account_relays_get_read_relays(relays));
  GhRelayScope *old_scope = scope_at(&rec, 0);

  /* Discovery settles once every source either answered or failed. */
  gh_relay_scope_eose(old_scope, SOURCE_A);
  g_assert_cmpint(gh_account_relays_get_state(relays), ==, GH_ACCOUNT_RELAYS_DISCOVERING);
  gh_relay_scope_notice(old_scope, SOURCE_B, GH_RELAY_NOTICE_ERROR, NULL, FALSE, "refused");
  g_assert_cmpint(gh_account_relays_get_state(relays), ==, GH_ACCOUNT_RELAYS_COMPLETE);

  g_autofree gchar *list_one = signed_list(SECRET_ONE, 10002, 100, "r",
                                           "wss://one-read.test.invalid", "read", NULL);
  gh_relay_scope_event(old_scope, SOURCE_A, list_one);
  assert_strv(gh_account_relays_get_read_relays(relays), "wss://one-read.test.invalid", NULL);
  assert_strv(gh_account_relays_get_write_relays(relays), NULL);
  g_autofree gchar *inbox_one = signed_list(SECRET_ONE, 10050, 100, "relay",
                                            "wss://one-inbox.test.invalid", NULL, NULL);
  gh_relay_scope_event(old_scope, SOURCE_A, inbox_one);
  assert_strv(gh_account_relays_get_inbox_relays(relays), "wss://one-inbox.test.invalid", NULL);

  /* A relay answering with someone else's newer list is not the account's. */
  g_autofree gchar *forged = signed_list(SECRET_TWO, 10002, 900, "r",
                                         "wss://forged.test.invalid", NULL, NULL);
  gh_relay_scope_event(old_scope, SOURCE_A, forged);
  assert_strv(gh_account_relays_get_read_relays(relays), "wss://one-read.test.invalid", NULL);

  /* Switch: the old account's REQs close before the new account's open, and
   * traffic in the revoke-to-teardown window is discarded. */
  probe.armed = TRUE;
  probe.relays = relays;
  probe.old_scope = old_scope;
  probe.event = signed_list(SECRET_ONE, 10002, 500, "r", "wss://stale.test.invalid",
                            NULL, NULL);
  g_settings_set_string(settings, "current-npub", npub_two);
  g_assert_true(probe.ran);
  guint64 second = gh_account_controller_get_generation(controller);
  g_assert_cmpuint(second, !=, first);
  g_assert_cmpuint(gh_account_relays_get_generation(relays), ==, second);
  g_assert_cmpuint(rec.closed, ==, 2);
  g_assert_cmpuint(rec.urls->len, ==, 4);
  g_assert_cmpstr(g_ptr_array_index(rec.authors, 2), ==, hex_two);
  g_assert_cmpstr(g_ptr_array_index(rec.authors, 3), ==, hex_two);
  g_assert_null(gh_account_relays_get_read_relays(relays));
  g_assert_null(gh_account_relays_get_write_relays(relays));
  g_assert_null(gh_account_relays_get_inbox_relays(relays));
  g_assert_cmpint(gh_account_relays_get_state(relays), ==, GH_ACCOUNT_RELAYS_DISCOVERING);

  /* Late traffic on the torn-down scope never reaches the new account. */
  g_autofree gchar *late = signed_list(SECRET_ONE, 10002, 600, "r",
                                       "wss://late.test.invalid", NULL, NULL);
  gh_relay_scope_event(old_scope, SOURCE_A, late);
  gh_relay_scope_eose(old_scope, SOURCE_B);
  g_assert_null(gh_account_relays_get_read_relays(relays));
  g_assert_cmpint(gh_account_relays_get_state(relays), ==, GH_ACCOUNT_RELAYS_DISCOVERING);

  /* The new scope admits only the new account's own list. */
  GhRelayScope *new_scope = scope_at(&rec, 2);
  g_assert_true(new_scope != old_scope);
  g_autofree gchar *wrong_author = signed_list(SECRET_ONE, 10002, 700, "r",
                                               "wss://wrong.test.invalid", NULL, NULL);
  gh_relay_scope_event(new_scope, SOURCE_A, wrong_author);
  g_assert_null(gh_account_relays_get_read_relays(relays));
  g_autofree gchar *list_two = signed_list(SECRET_TWO, 10002, 100, "r",
                                           "wss://two.test.invalid", NULL, NULL);
  gh_relay_scope_event(new_scope, SOURCE_A, list_two);
  assert_strv(gh_account_relays_get_read_relays(relays), "wss://two.test.invalid", NULL);
  assert_strv(gh_account_relays_get_write_relays(relays), "wss://two.test.invalid", NULL);

  /* Deselecting closes everything and opens nothing. */
  g_settings_set_string(settings, "current-npub", "");
  g_assert_cmpint(gh_account_relays_get_state(relays), ==, GH_ACCOUNT_RELAYS_INACTIVE);
  g_assert_cmpuint(gh_account_relays_get_generation(relays), ==, 0);
  g_assert_cmpuint(rec.closed, ==, 4);
  g_assert_cmpuint(rec.urls->len, ==, 4);
  g_assert_null(gh_account_relays_get_read_relays(relays));

  g_signal_handlers_disconnect_by_data(controller, &probe);
  g_free(probe.event);
  release(relays);
  release(controller);
  recorder_clear(&rec);
}

static void
test_sources_and_reachability(void)
{
  const gchar *none[] = { NULL };
  g_autoptr(GSettings) settings = fresh_settings(npub_one, none);
  GhAccountController *controller = listed_controller(settings);
  Recorder rec;
  recorder_init(&rec);
  GhAccountRelays *relays =
    gh_account_relays_new(controller, settings, &recorder_transport, &rec);

  /* No configured relay: nothing is contacted. */
  g_assert_cmpint(gh_account_relays_get_state(relays), ==, GH_ACCOUNT_RELAYS_NO_SOURCES);
  g_assert_cmpuint(rec.urls->len, ==, 0);
  const gchar *invalid[] = { "https://discovery.test.invalid",
                             "wss://user:secret@discovery.test.invalid", NULL };
  g_settings_set_strv(settings, "discovery-relays", invalid);
  g_assert_cmpint(gh_account_relays_get_state(relays), ==, GH_ACCOUNT_RELAYS_NO_SOURCES);
  g_assert_cmpuint(rec.urls->len, ==, 0);

  const gchar *one[] = { SOURCE_A, NULL };
  g_settings_set_strv(settings, "discovery-relays", one);
  g_assert_cmpint(gh_account_relays_get_state(relays), ==, GH_ACCOUNT_RELAYS_DISCOVERING);
  g_assert_cmpuint(rec.urls->len, ==, 1);
  GhRelayScope *scope = scope_at(&rec, 0);
  gh_relay_scope_notice(scope, SOURCE_A, GH_RELAY_NOTICE_ERROR, NULL, FALSE, "refused");
  g_assert_cmpint(gh_account_relays_get_state(relays), ==, GH_ACCOUNT_RELAYS_UNREACHABLE);
  /* The transport keeps redialling; a later answer completes discovery. */
  gh_relay_scope_eose(scope, SOURCE_A);
  g_assert_cmpint(gh_account_relays_get_state(relays), ==, GH_ACCOUNT_RELAYS_COMPLETE);
  gh_relay_scope_notice(scope, SOURCE_A, GH_RELAY_NOTICE_ERROR, NULL, FALSE, "redial");
  g_assert_cmpint(gh_account_relays_get_state(relays), ==, GH_ACCOUNT_RELAYS_COMPLETE);
  g_autofree gchar *list = signed_list(SECRET_ONE, 10002, 100, "r",
                                       "wss://one.test.invalid", "write", NULL);
  gh_relay_scope_event(scope, SOURCE_A, list);
  assert_strv(gh_account_relays_get_write_relays(relays), "wss://one.test.invalid", NULL);
  assert_strv(gh_account_relays_get_read_relays(relays), NULL);

  /* Changing sources rebuilds the same account's scope from scratch. */
  guint64 generation = gh_account_relays_get_generation(relays);
  const gchar *two[] = { SOURCE_A, SOURCE_B, NULL };
  g_settings_set_strv(settings, "discovery-relays", two);
  g_assert_cmpuint(rec.closed, ==, 1);
  g_assert_cmpuint(rec.urls->len, ==, 3);
  g_assert_cmpuint(gh_account_relays_get_generation(relays), ==, generation);
  g_assert_cmpint(gh_account_relays_get_state(relays), ==, GH_ACCOUNT_RELAYS_DISCOVERING);
  g_assert_null(gh_account_relays_get_write_relays(relays));

  release(relays); /* closes both endpoints of the rebuilt scope */
  g_assert_cmpuint(rec.closed, ==, 3);
  release(controller);
  recorder_clear(&rec);
}

/* A relay CLOSED REQ is terminal for that source, before or after EOSE. */
static void
test_closed_is_terminal(void)
{
  const gchar *sources[] = { SOURCE_A, SOURCE_B, NULL };
  g_autoptr(GSettings) settings = fresh_settings(npub_one, sources);
  GhAccountController *controller = listed_controller(settings);
  Recorder rec;
  recorder_init(&rec);
  GhAccountRelays *relays =
    gh_account_relays_new(controller, settings, &recorder_transport, &rec);
  GhRelayScope *scope = scope_at(&rec, 0);

  /* Closed before answering: discovery must not wait on it forever. */
  gh_relay_scope_eose(scope, SOURCE_A);
  gh_relay_scope_notice(scope, SOURCE_B, GH_RELAY_NOTICE_CLOSED, NULL, FALSE,
                        "blocked: not allowed");
  g_assert_cmpint(gh_account_relays_get_state(relays), ==, GH_ACCOUNT_RELAYS_COMPLETE);

  /* Closed after answering: the admitted list stays, but no source is live. */
  g_autofree gchar *list = signed_list(SECRET_ONE, 10002, 100, "r",
                                       "wss://one.test.invalid", NULL, NULL);
  gh_relay_scope_event(scope, SOURCE_A, list);
  gh_relay_scope_notice(scope, SOURCE_A, GH_RELAY_NOTICE_CLOSED, NULL, FALSE,
                        "error: shutting down");
  g_assert_cmpint(gh_account_relays_get_state(relays), ==, GH_ACCOUNT_RELAYS_UNREACHABLE);
  assert_strv(gh_account_relays_get_read_relays(relays), "wss://one.test.invalid", NULL);

  /* A reconnect re-issues the REQ; its EOSE makes the source live again. */
  gh_relay_scope_notice(scope, SOURCE_A, GH_RELAY_NOTICE_DISCONNECTED, NULL, FALSE, NULL);
  gh_relay_scope_eose(scope, SOURCE_A);
  g_assert_cmpint(gh_account_relays_get_state(relays), ==, GH_ACCOUNT_RELAYS_COMPLETE);

  release(relays);
  release(controller);
  recorder_clear(&rec);
}

static void
test_newest_wins(void)
{
  const gchar *sources[] = { SOURCE_A, SOURCE_B, NULL };
  g_autoptr(GSettings) settings = fresh_settings(npub_one, sources);
  GhAccountController *controller = listed_controller(settings);
  Recorder rec;
  recorder_init(&rec);
  GhAccountRelays *relays =
    gh_account_relays_new(controller, settings, &recorder_transport, &rec);
  GhRelayScope *scope = scope_at(&rec, 0);

  g_autofree gchar *newer = signed_list(SECRET_ONE, 10002, 200, "r",
                                        "wss://newer.test.invalid", NULL, NULL);
  g_autofree gchar *older = signed_list(SECRET_ONE, 10002, 100, "r",
                                        "wss://older.test.invalid", NULL, NULL);
  gh_relay_scope_event(scope, SOURCE_A, newer);
  gh_relay_scope_event(scope, SOURCE_B, older);
  assert_strv(gh_account_relays_get_read_relays(relays), "wss://newer.test.invalid", NULL);

  /* Same created_at: the lower id wins regardless of arrival order. */
  g_autofree gchar *id_x = NULL, *id_y = NULL;
  g_autofree gchar *x = signed_list(SECRET_ONE, 10002, 300, "r",
                                    "wss://tie-x.test.invalid", NULL, &id_x);
  g_autofree gchar *y = signed_list(SECRET_ONE, 10002, 300, "r",
                                    "wss://tie-y.test.invalid", NULL, &id_y);
  gboolean x_lower = strcmp(id_x, id_y) < 0;
  const gchar *lower = x_lower ? x : y, *higher = x_lower ? y : x;
  const gchar *lower_url = x_lower ? "wss://tie-x.test.invalid" : "wss://tie-y.test.invalid";
  gh_relay_scope_event(scope, SOURCE_A, lower);
  gh_relay_scope_event(scope, SOURCE_B, higher);
  assert_strv(gh_account_relays_get_read_relays(relays), lower_url, NULL);

  g_autofree gchar *id_p = NULL, *id_q = NULL;
  g_autofree gchar *p = signed_list(SECRET_ONE, 10002, 400, "r",
                                    "wss://tie-p.test.invalid", NULL, &id_p);
  g_autofree gchar *q = signed_list(SECRET_ONE, 10002, 400, "r",
                                    "wss://tie-q.test.invalid", NULL, &id_q);
  gboolean p_lower = strcmp(id_p, id_q) < 0;
  gh_relay_scope_event(scope, SOURCE_A, p_lower ? q : p);
  gh_relay_scope_event(scope, SOURCE_B, p_lower ? p : q);
  assert_strv(gh_account_relays_get_read_relays(relays),
              p_lower ? "wss://tie-p.test.invalid" : "wss://tie-q.test.invalid", NULL);

  /* An unmarked NIP-65 entry is read and write; no inbox list was sent. */
  assert_strv(gh_account_relays_get_write_relays(relays),
              p_lower ? "wss://tie-p.test.invalid" : "wss://tie-q.test.invalid", NULL);
  g_assert_null(gh_account_relays_get_inbox_relays(relays));

  release(relays);
  release(controller);
  recorder_clear(&rec);
}

/* Disposing the account-relay layer (app shutdown) closes its REQs while the
 * controller lives on; later account changes open nothing. */
static void
test_dispose_closes(void)
{
  const gchar *sources[] = { SOURCE_A, NULL };
  g_autoptr(GSettings) settings = fresh_settings(npub_one, sources);
  GhAccountController *controller = listed_controller(settings);
  Recorder rec;
  recorder_init(&rec);
  GhAccountRelays *relays =
    gh_account_relays_new(controller, settings, &recorder_transport, &rec);
  g_assert_cmpuint(rec.urls->len, ==, 1);
  g_object_run_dispose(G_OBJECT(relays));
  g_assert_cmpuint(rec.closed, ==, 1);
  g_assert_cmpint(gh_account_relays_get_state(relays), ==, GH_ACCOUNT_RELAYS_INACTIVE);
  gh_relay_scope_eose(scope_at(&rec, 0), SOURCE_A);
  g_settings_set_string(settings, "current-npub", npub_two);
  g_assert_cmpuint(rec.urls->len, ==, 1);
  g_assert_cmpint(gh_account_relays_get_state(relays), ==, GH_ACCOUNT_RELAYS_INACTIVE);
  g_object_unref(relays);
  release(controller);
  recorder_clear(&rec);
}

#ifdef GROUNDHOG_TEST_WIRE
/* A local NIP-01 relay that answers each REQ with the requested author's
 * stored relay list, then EOSE. */
typedef struct {
  SoupServer *server;
  GPtrArray *connections;
  gchar *url;
  gchar *list_one;
  gchar *list_two;
  guint reqs;
} ListRelay;

static void
on_list_message(SoupWebsocketConnection *connection, SoupWebsocketDataType type,
                GBytes *message, gpointer data)
{
  ListRelay *relay = data;
  gsize length;
  const gchar *bytes = g_bytes_get_data(message, &length);
  if (type != SOUP_WEBSOCKET_DATA_TEXT)
    return;
  g_autofree gchar *text = g_strndup(bytes, length);
  if (!g_str_has_prefix(text, "[\"REQ\""))
    return;
  relay->reqs++;
  const gchar *start = strchr(text + 6, '"');
  const gchar *end = start ? strchr(start + 1, '"') : NULL;
  g_assert_nonnull(end);
  g_autofree gchar *sub_id = g_strndup(start + 1, end - start - 1);
  const gchar *list = strstr(text, hex_one) ? relay->list_one
                    : strstr(text, hex_two) ? relay->list_two
                                            : NULL;
  if (list) {
    g_autofree gchar *event = g_strdup_printf("[\"EVENT\",\"%s\",%s]", sub_id, list);
    soup_websocket_connection_send_text(connection, event);
  }
  g_autofree gchar *eose = g_strdup_printf("[\"EOSE\",\"%s\"]", sub_id);
  soup_websocket_connection_send_text(connection, eose);
}

static void
on_list_socket(SoupServer *server, SoupServerMessage *message, const char *path,
               SoupWebsocketConnection *connection, gpointer data)
{
  (void)server;
  (void)message;
  (void)path;
  ListRelay *relay = data;
  g_ptr_array_add(relay->connections, g_object_ref(connection));
  g_signal_connect(connection, "message", G_CALLBACK(on_list_message), relay);
}

typedef struct {
  GhAccountRelays *relays;
  const gchar *read;
} ListWait;

static gboolean
has_read_list(gpointer data)
{
  ListWait *wait = data;
  const gchar *const *read = gh_account_relays_get_read_relays(wait->relays);
  return gh_account_relays_get_state(wait->relays) == GH_ACCOUNT_RELAYS_COMPLETE &&
         read && g_strcmp0(read[0], wait->read) == 0;
}

static void
test_wire_account_switch(void)
{
  ListRelay relay = { 0 };
  relay.server = soup_server_new(NULL, NULL);
  relay.connections = g_ptr_array_new_with_free_func(g_object_unref);
  soup_server_add_websocket_handler(relay.server, "/relay", NULL, NULL,
                                    on_list_socket, &relay, NULL);
  g_autoptr(GError) error = NULL;
  g_assert_true(soup_server_listen_local(relay.server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY,
                                         &error));
  GSList *uris = soup_server_get_uris(relay.server);
  relay.url = g_strdup_printf("ws://127.0.0.1:%d/relay", g_uri_get_port(uris->data));
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  relay.list_one = signed_list(SECRET_ONE, 10002, 100, "r", "wss://one-home.test.invalid",
                               NULL, NULL);
  relay.list_two = signed_list(SECRET_TWO, 10002, 100, "r", "wss://two-home.test.invalid",
                               NULL, NULL);

  const gchar *sources[] = { relay.url, NULL };
  g_autoptr(GSettings) settings = fresh_settings(npub_one, sources);
  GhAccountController *controller = listed_controller(settings);
  GhAccountRelays *relays = gh_account_relays_new(controller, settings, NULL, NULL);
  ListWait wait = { relays, "wss://one-home.test.invalid" };
  spin_until(has_read_list, &wait);
  g_assert_cmpuint(relay.reqs, ==, 1);

  g_settings_set_string(settings, "current-npub", npub_two);
  g_assert_null(gh_account_relays_get_read_relays(relays));
  wait.read = "wss://two-home.test.invalid";
  spin_until(has_read_list, &wait);
  g_assert_cmpuint(relay.reqs, ==, 2);
  /* The next account's REQ used its own socket; closing the previous
   * account's endpoint cannot cut it (shared-by-URL wrappers raced here).
   * Whether the relay sees the old CLOSE is a libnostr close-path matter. */
  g_assert_cmpuint(relay.connections->len, ==, 2);

  release(relays);
  release(controller);
  for (guint i = 0; i < relay.connections->len; i++) {
    SoupWebsocketConnection *connection = g_ptr_array_index(relay.connections, i);
    g_signal_handlers_disconnect_by_data(connection, &relay);
    if (soup_websocket_connection_get_state(connection) == SOUP_WEBSOCKET_STATE_OPEN)
      soup_websocket_connection_close(connection, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
  }
  g_ptr_array_unref(relay.connections);
  soup_server_disconnect(relay.server);
  g_object_unref(relay.server);
  g_free(relay.url);
  g_free(relay.list_one);
  g_free(relay.list_two);
}

typedef struct {
  GhAccountRelays *relays;
  const gchar *inbox;
} InboxWait;

static gboolean
has_inbox_list(gpointer data)
{
  InboxWait *wait = data;
  const gchar *const *inbox = gh_account_relays_get_inbox_relays(wait->relays);
  return gh_account_relays_get_state(wait->relays) == GH_ACCOUNT_RELAYS_COMPLETE &&
         inbox && g_strcmp0(inbox[0], wait->inbox) == 0;
}

/* nostrc-qp24.67, charter §4.3: own list discovery on a discovery relay that
 * demands NIP-42 AUTH for every REQ (wire-relay.h, store-and-serve). The
 * first REQ is refused "auth-required:"; GhAccountRelays signs in with a
 * throwaway key (GhAuthPolicy, OWN_LIST_DISCOVERY), never as the account,
 * and the REQ issued again is served, so the account's lists are found. */
static void
test_wire_auth_gated_source(void)
{
  WireRelay gated = { 0 };
  gated.serve = gated.require_auth = gated.record = TRUE;
  relay_init(&gated);
  g_autofree gchar *inbox = signed_list(SECRET_ONE, 10050, 100, "relay",
                                        "wss://one-inbox.test.invalid", NULL, NULL);
  wire_relay_inject(&gated, inbox);

  const gchar *sources[] = { gated.url, NULL };
  g_autoptr(GSettings) settings = fresh_settings(npub_one, sources);
  GhAccountController *controller = listed_controller(settings);
  GhAccountRelays *relays = gh_account_relays_new(controller, settings, NULL, NULL);
  InboxWait wait = { relays, "wss://one-inbox.test.invalid" };
  spin_until(has_inbox_list, &wait);
  /* Refused once, one AUTH with a key that is no account's, then served. */
  g_assert_cmpuint(gated.closed_reqs, ==, 1);
  g_assert_cmpuint(gated.auth_pubkeys->len, ==, 1);
  const gchar *key = g_ptr_array_index(gated.auth_pubkeys, 0);
  g_assert_cmpstr(key, !=, hex_one);
  g_assert_cmpstr(key, !=, hex_two);
  g_assert_cmpuint(gated.auth_ok, ==, 1);
  g_assert_cmpuint(gated.served, ==, 1);

  release(relays);
  release(controller);
  relay_clear(&gated);
}
#endif

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  npub_one = npub_for_secret(SECRET_ONE);
  npub_two = npub_for_secret(SECRET_TWO);
  hex_one = gh_identity_pubkey_hex(npub_one);
  hex_two = gh_identity_pubkey_hex(npub_two);
  g_test_add_func("/groundhog/account-relays/switch-teardown", test_switch_teardown);
  g_test_add_func("/groundhog/account-relays/sources-and-reachability",
                  test_sources_and_reachability);
  g_test_add_func("/groundhog/account-relays/closed-is-terminal", test_closed_is_terminal);
  g_test_add_func("/groundhog/account-relays/newest-wins", test_newest_wins);
  g_test_add_func("/groundhog/account-relays/dispose-closes", test_dispose_closes);
#ifdef GROUNDHOG_TEST_WIRE
  g_test_add_func("/groundhog/account-relays/wire-account-switch", test_wire_account_switch);
  g_test_add_func("/groundhog/account-relays/wire-auth-gated-source",
                  test_wire_auth_gated_source);
#endif
  int status = g_test_run();
  g_free(npub_one);
  g_free(npub_two);
  g_free(hex_one);
  g_free(hex_two);
  return status;
}
