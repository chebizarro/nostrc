/* GhInboxSetup and GhInboxProbe (privacy charter §7.8 step 4, §8.2 G14,
 * D4): relay-address rules, the shipped suggestions, the kind-10050 event,
 * the private-reads check, and publishing against recording scope/publish
 * transports (H1) with the mock org.nostr.Signer on a private test bus
 * (tests/app/gh-test-signer.h). Covered: PT-9 (nothing is contacted before
 * the signer approved; nothing at all after a denial), each relay's outcome
 * recorded, NIP-42 AUTH as the account only on the list's publication
 * connections (never on the check), through GhAuthPolicy's one account
 * signer of the generation (R6: its approvals and refusals are the
 * policy's), the discovery hand-over and account switches. Waits iterate the
 * main context; their deadline is a failure bound, never a source of
 * progress. */
#include "gh-inbox-setup.h"
#include "gh-relay-list-setup.h"
#include "gh-auth-policy.h"
#include "gh-signer.h"
#include "gh-test-signer.h"
#include "nostr-tag.h"

#define INBOX_A "wss://inbox-a.example.org"
#define INBOX_B "wss://inbox-b.example.org"
#define WRITE_W "wss://write.example.org"
#define READ_R "wss://read.example.org"
#define DISC_D "wss://discovery.example.org"

static gchar *npub[GH_TEST_KEYS];
static gchar *hex[GH_TEST_KEYS];

/* ---- recording transports ------------------------------------------------ */

typedef struct {
  GhRelayScope *scope;
  gchar *url;
  gboolean probe;   /* {"kinds":[1059],"#p":[...]} rather than the own-list REQ */
  gchar *p;         /* the probe's #p */
  int limit;
  guint authors;
  gboolean closed;
} ScopeOpen;

typedef struct {
  GhRelayPublish *publish;
  gchar *url;
  gchar *event_json;
  gboolean closed;
} PubOpen;

typedef struct {
  GPtrArray *scopes;       /* ScopeOpen */
  GPtrArray *pubs;         /* PubOpen */
  GPtrArray *sent_auth;    /* signed AUTH JSON, publish connections */
  GPtrArray *sent_auth_on; /* the URL each went to */
  guint scope_auth_sent;   /* AUTH sent on any REQ connection: must stay 0 */
  guint resends;
} Recorder;

static void
scope_open_free(gpointer data)
{
  ScopeOpen *open = data;
  gh_relay_scope_unref(open->scope);
  g_free(open->url);
  g_free(open->p);
  g_free(open);
}

static gpointer
scope_open(GhRelayScope *scope, const gchar *url, const NostrFilters *filters, gpointer data,
           GError **error)
{
  Recorder *recorder = data;
  (void)error;
  g_assert_cmpuint(filters->count, ==, 1);
  const NostrFilter *filter = &filters->filters[0];
  ScopeOpen *open = g_new0(ScopeOpen, 1);
  open->scope = gh_relay_scope_ref(scope);
  open->url = g_strdup(url);
  open->authors = nostr_filter_authors_len(filter);
  open->limit = nostr_filter_get_limit(filter);
  open->probe = nostr_filter_kinds_len(filter) == 1 && nostr_filter_kinds_get(filter, 0) == 1059;
  if (open->probe) {
    g_assert_cmpuint(nostr_filter_tags_len(filter), ==, 1);
    g_assert_cmpstr(nostr_filter_tag_get(filter, 0, 0), ==, "p");
    open->p = g_strdup(nostr_filter_tag_get(filter, 0, 1));
  }
  g_ptr_array_add(recorder->scopes, open);
  return open;
}

static void
scope_close(gpointer handle, gpointer data)
{
  ScopeOpen *open = handle;
  (void)data;
  g_assert_false(open->closed);
  open->closed = TRUE;
}

static gboolean
scope_send_auth(gpointer handle, const gchar *json, gpointer data, GError **error)
{
  Recorder *recorder = data;
  (void)handle;
  (void)json;
  (void)error;
  recorder->scope_auth_sent++;
  return TRUE;
}

static void
scope_resubscribe(gpointer handle, gpointer data)
{
  (void)handle;
  (void)data;
}

static const GhRelayTransport scope_transport = { scope_open, scope_close };
static const GhRelayAuthTransport scope_auth_transport = { scope_send_auth, scope_resubscribe };

static void
pub_open_free(gpointer data)
{
  PubOpen *open = data;
  gh_relay_publish_unref(open->publish);
  g_free(open->url);
  g_free(open->event_json);
  g_free(open);
}

static gpointer
pub_open(GhRelayPublish *publish, const gchar *url, const gchar *event_json, gpointer data,
         GError **error)
{
  Recorder *recorder = data;
  (void)error;
  PubOpen *open = g_new0(PubOpen, 1);
  open->publish = gh_relay_publish_ref(publish);
  open->url = g_strdup(url);
  open->event_json = g_strdup(event_json);
  g_ptr_array_add(recorder->pubs, open);
  return open;
}

static void
pub_close(gpointer handle, gpointer data)
{
  PubOpen *open = handle;
  (void)data;
  g_assert_false(open->closed);
  open->closed = TRUE;
}

static gboolean
pub_send_auth(gpointer handle, const gchar *json, gpointer data, GError **error)
{
  Recorder *recorder = data;
  PubOpen *open = handle;
  (void)error;
  g_ptr_array_add(recorder->sent_auth, g_strdup(json));
  g_ptr_array_add(recorder->sent_auth_on, g_strdup(open->url));
  return TRUE;
}

static gboolean
pub_resend(gpointer handle, gpointer data, GError **error)
{
  Recorder *recorder = data;
  (void)handle;
  (void)error;
  recorder->resends++;
  return TRUE;
}

static const GhRelayPublishTransport pub_transport = { pub_open, pub_close };
static const GhRelayPublishAuthTransport pub_auth_transport = { pub_send_auth, pub_resend };

/* ---- fixture ---------------------------------------------------------------- */

static GPtrArray *
fake_list(gpointer data, GError **error)
{
  (void)data;
  (void)error;
  GPtrArray *ids = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  for (guint key = 1; key <= 2; key++) {
    GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
    info->npub = g_strdup(npub[key]);
    info->label = g_strdup_printf("Key %u", key);
    g_ptr_array_add(ids, info);
  }
  return ids;
}

typedef struct {
  GhTestBus bus;
  GhTestSigner mock;
  Recorder rec;
  GSettings *settings;
  GhAccountController *accounts;
  GhAccountRelays *relays;
  GhInboxSetup *setup;
} Fixture;

static gboolean
is_active(gpointer data)
{
  return gh_account_controller_get_state(data) == GH_ACCOUNT_STATE_ACTIVE;
}

static gboolean
is_unselected(gpointer data)
{
  return gh_account_controller_get_state(data) == GH_ACCOUNT_STATE_UNSELECTED;
}

static void
fixture_setup(Fixture *f, gconstpointer data)
{
  (void)data;
  gh_test_bus_up(&f->bus);
  gh_test_signer_up(&f->bus, &f->mock);
  f->rec.scopes = g_ptr_array_new_with_free_func(scope_open_free);
  f->rec.pubs = g_ptr_array_new_with_free_func(pub_open_free);
  f->rec.sent_auth = g_ptr_array_new_with_free_func(g_free);
  f->rec.sent_auth_on = g_ptr_array_new_with_free_func(g_free);
  f->settings = g_settings_new("org.nostr.Groundhog");
  g_settings_reset(f->settings, "discovery-relays");
  g_settings_set_string(f->settings, "signer-method", "auto");
  g_settings_set_string(f->settings, "current-npub", npub[1]);
  f->accounts = gh_account_controller_new_full(f->settings, f->bus.client, fake_list, NULL);
  gh_test_spin_until(is_active, f->accounts);
  f->relays = gh_account_relays_new(f->accounts, f->settings, &scope_transport, &f->rec);
}

static GhInboxSetupConfig
config_of(Fixture *f)
{
  return (GhInboxSetupConfig){
    .accounts = f->accounts,
    .account_relays = f->relays,
    .settings = f->settings,
    .probe_transport = &scope_transport,
    .probe_auth_transport = &scope_auth_transport,
    .probe_transport_data = &f->rec,
    .publish_transport = &pub_transport,
    .publish_auth_transport = &pub_auth_transport,
    .publish_transport_data = &f->rec,
  };
}

static void
fixture_teardown(Fixture *f, gconstpointer data)
{
  (void)data;
  if (f->setup)
    gh_test_release(g_steal_pointer(&f->setup));
  gh_test_release(g_steal_pointer(&f->relays));
  gh_test_release(g_steal_pointer(&f->accounts));
  GhTestSenders check = { &f->bus, &f->mock };
  gh_test_spin_until(gh_test_signer_senders_closed, &check);
  /* Nothing is left open on any relay. */
  for (guint i = 0; i < f->rec.scopes->len; i++)
    g_assert_true(((ScopeOpen *)g_ptr_array_index(f->rec.scopes, i))->closed);
  for (guint i = 0; i < f->rec.pubs->len; i++)
    g_assert_true(((PubOpen *)g_ptr_array_index(f->rec.pubs, i))->closed);
  g_ptr_array_unref(f->rec.scopes);
  g_ptr_array_unref(f->rec.pubs);
  g_ptr_array_unref(f->rec.sent_auth);
  g_ptr_array_unref(f->rec.sent_auth_on);
  g_settings_reset(f->settings, "discovery-relays");
  g_settings_reset(f->settings, "current-npub");
  g_object_unref(f->settings);
  gh_test_signer_down(&f->bus, &f->mock);
  gh_test_bus_down(&f->bus);
}

static guint
probe_opens(Fixture *f)
{
  guint n = 0;
  for (guint i = 0; i < f->rec.scopes->len; i++)
    n += ((ScopeOpen *)g_ptr_array_index(f->rec.scopes, i))->probe;
  return n;
}

/* The newest check REQ on url. */
static ScopeOpen *
probe_open(Fixture *f, const gchar *url)
{
  for (guint i = f->rec.scopes->len; i > 0; i--) {
    ScopeOpen *open = g_ptr_array_index(f->rec.scopes, i - 1);
    if (open->probe && g_str_equal(open->url, url))
      return open;
  }
  g_error("no check REQ was opened on %s", url);
}

static PubOpen *
pub_find(Fixture *f, const gchar *url)
{
  for (guint i = 0; i < f->rec.pubs->len; i++) {
    PubOpen *open = g_ptr_array_index(f->rec.pubs, i);
    if (g_str_equal(open->url, url))
      return open;
  }
  return NULL;
}

static gboolean
pubs_opened(gpointer data)
{
  Fixture *f = data;
  return f->rec.pubs->len > 0;
}

static gboolean
setup_finished(gpointer data)
{
  GhInboxSetupState state = gh_inbox_setup_get_state(data);
  return state == GH_INBOX_SETUP_DONE || state == GH_INBOX_SETUP_FAILED;
}

static gboolean
held_one(gpointer data)
{
  return ((GhTestSigner *)data)->held->len > 0;
}

static gchar *
signed_event(guint key, int kind, NostrTags *tags)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, kind);
  nostr_event_set_created_at(event, 100);
  nostr_event_set_content(event, "");
  nostr_event_set_tags(event, tags);
  g_assert_cmpint(nostr_event_sign(event, gh_test_secret[key]), ==, 0);
  char *raw = nostr_event_serialize_compact(event);
  gchar *json = g_strdup(raw);
  free(raw);
  nostr_event_free(event);
  return json;
}

/* The account's own 10002 (write W, read R) found on discovery relay D. */
static void
discover_own_lists(Fixture *f)
{
  const gchar *discovery[] = { DISC_D, NULL };
  g_settings_set_strv(f->settings, "discovery-relays", discovery);
  ScopeOpen *own = NULL;
  for (guint i = 0; i < f->rec.scopes->len; i++) {
    ScopeOpen *open = g_ptr_array_index(f->rec.scopes, i);
    if (!open->probe && !open->closed && g_str_equal(open->url, DISC_D))
      own = open;
  }
  g_assert_nonnull(own);
  g_autofree gchar *list = signed_event(1, 10002, nostr_tags_new(2,
    nostr_tag_new("r", WRITE_W, "write", NULL), nostr_tag_new("r", READ_R, "read", NULL)));
  gh_relay_scope_event(own->scope, DISC_D, list);
  gh_relay_scope_eose(own->scope, DISC_D);
  g_assert_cmpint(gh_account_relays_get_state(f->relays), ==, GH_ACCOUNT_RELAYS_COMPLETE);
  g_assert_cmpstr(gh_account_relays_get_write_relays(f->relays)[0], ==, WRITE_W);
}

static gboolean
is_nonempty(gpointer data)
{
  return ((GPtrArray *)data)->len > 0;
}

/* ---- pure functions ------------------------------------------------------ */

static void
test_normalize_url(void)
{
  static const struct {
    const gchar *input;
    const gchar *want; /* NULL: refused */
  } cases[] = {
    { "relay.example.org", "wss://relay.example.org" },
    { "  WSS://Relay.Example.ORG/  ", "wss://relay.example.org" },
    { "wss://relay.example.org:4443/inbox/", "wss://relay.example.org:4443/inbox" },
    { "wss://xn--rlay-6ra.example.org", "wss://xn--rlay-6ra.example.org" },
    { "ws://127.0.0.1:7777", "ws://127.0.0.1:7777" },
    { "ws://localhost:7777/", "ws://localhost:7777" },
    { "ws://[::1]:7777", "ws://[::1]:7777" },
    { "ws://relay.example.org", NULL },      /* PD-5: no plain ws:// off this device */
    { "https://relay.example.org", NULL },
    { "wss://user@relay.example.org", NULL },
    { "wss://user:pw@relay.example.org", NULL },
    { "wss://relay.example.org/?token=1", NULL },
    { "wss://relay.example.org/#x", NULL },
    { "wss://relay example.org", NULL },
    { "wss://", NULL },
    { "", NULL },
    { "   ", NULL },
    { "wss://-relay.example.org", NULL },
    { "wss://relay_example.org", NULL },
    /* An IDN is kept and shown in its ASCII (xn--) form, so a look-alike
     * host can't pass for another (compare PD-3). */
    { "wss://rélay.example.org", "wss://xn--rlay-bpa.example.org" },
    { "wss://relay.example.org:99999", NULL },
  };
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_autoptr(GError) error = NULL;
    g_autofree gchar *got = gh_inbox_setup_normalize_url(cases[i].input, &error);
    g_assert_cmpstr(got, ==, cases[i].want);
    if (!cases[i].want) {
      g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
      g_assert_cmpuint(strlen(error->message), >, 0);
    } else {
      g_assert_no_error(error);
      /* Normalizing is idempotent. */
      g_autofree gchar *again = gh_inbox_setup_normalize_url(got, NULL);
      g_assert_cmpstr(again, ==, got);
    }
  }
}

static GPtrArray *
parse_text(const gchar *text, GError **error)
{
  g_autoptr(GBytes) bytes = g_bytes_new(text, strlen(text));
  return gh_inbox_setup_parse_suggestions(bytes, error);
}

/* The file that ships (data/relay-suggestions.json, D4): short, every
 * address a distinct normalized wss:// relay, and it is the one bundled in
 * the GResource (tests/app/test_resource.c). tests/check_privacy.py
 * (relay-suggestions) also checks it against AGENTS.md's banned relays. */
static void
test_shipped_suggestions(void)
{
  g_autofree gchar *text = NULL;
  g_autoptr(GError) error = NULL;
  g_assert_true(g_file_get_contents(GROUNDHOG_RELAY_SUGGESTIONS, &text, NULL, &error));
  g_assert_no_error(error);
  g_autoptr(GPtrArray) suggestions = parse_text(text, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(suggestions->len, >=, 1);
  g_assert_cmpuint(suggestions->len, <=, GH_INBOX_SETUP_MAX_SUGGESTIONS);
  for (guint i = 0; i < suggestions->len; i++) {
    GhInboxSuggestion *s = g_ptr_array_index(suggestions, i);
    g_assert_true(g_str_has_prefix(s->url, "wss://"));
    g_autofree gchar *normalized = gh_inbox_setup_normalize_url(s->url, NULL);
    g_assert_cmpstr(normalized, ==, s->url);
    g_assert_cmpuint(strlen(s->name), >, 0);
    g_assert_cmpuint(strlen(s->description), >, 0);
  }
}

static void
test_parse_suggestions_refuses(void)
{
  static const gchar *const bad[] = {
    "not json",
    "[]",
    "{\"relays\": []}",
    "{\"relays\": [{\"url\": \"ws://relay.example.org\", \"name\": \"n\", \"description\": \"d\","
    " \"private_reads\": \"yes\"}]}",
    "{\"relays\": [{\"url\": \"wss://Relay.example.org/\", \"name\": \"n\", \"description\": \"d\","
    " \"private_reads\": \"yes\"}]}",
    "{\"relays\": [{\"url\": \"wss://relay.example.org\", \"name\": \"n\", \"description\": \"d\","
    " \"private_reads\": \"maybe\"}]}",
    "{\"relays\": [{\"url\": \"wss://relay.example.org\", \"description\": \"d\","
    " \"private_reads\": \"yes\"}]}",
    "{\"relays\": [{\"url\": \"wss://a.example.org\", \"name\": \"n\", \"description\": \"d\","
    " \"private_reads\": \"no\"}, {\"url\": \"wss://a.example.org\", \"name\": \"n\","
    " \"description\": \"d\", \"private_reads\": \"no\"}]}",
  };
  for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray) got = parse_text(bad[i], &error);
    g_assert_null(got);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  }
  GString *many = g_string_new("{\"relays\": [");
  for (guint i = 0; i <= GH_INBOX_SETUP_MAX_SUGGESTIONS; i++)
    g_string_append_printf(many, "%s{\"url\": \"wss://r%u.example.org\", \"name\": \"n\","
                           " \"description\": \"d\", \"private_reads\": \"unknown\"}",
                           i ? "," : "", i);
  g_string_append(many, "]}");
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) got = parse_text(many->str, &error);
  g_assert_null(got);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_string_free(many, TRUE);
}

static void
test_build_unsigned(void)
{
  const gchar *relays[] = { INBOX_A, INBOX_B, NULL };
  g_autofree gchar *json = gh_inbox_setup_build_unsigned(hex[1], relays, 1234);
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(event, json, NULL), ==, 1);
  g_assert_cmpint(nostr_event_get_kind(event), ==, 10050);
  g_assert_cmpint(nostr_event_get_created_at(event), ==, 1234);
  g_assert_cmpstr(nostr_event_get_content(event), ==, "");
  g_assert_cmpstr(nostr_event_get_pubkey(event), ==, hex[1]);
  NostrTags *tags = nostr_event_get_tags(event);
  g_assert_cmpuint(nostr_tags_size(tags), ==, 2);
  for (guint i = 0; i < 2; i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    g_assert_cmpuint(nostr_tag_size(tag), ==, 2);
    g_assert_cmpstr(nostr_tag_get(tag, 0), ==, "relay");
    g_assert_cmpstr(nostr_tag_get(tag, 1), ==, relays[i]);
  }
  /* Unsigned: the signer adds the id and signature. */
  g_assert_null(event->id);
  g_assert_null(event->sig);
  nostr_event_free(event);
}

static void
test_classify_closed(void)
{
  g_assert_cmpint(gh_inbox_probe_classify_closed("auth-required: you must auth"), ==,
                  GH_INBOX_PROBE_PRIVATE);
  g_assert_cmpint(gh_inbox_probe_classify_closed("restricted: recipients only"), ==,
                  GH_INBOX_PROBE_PRIVATE);
  /* Seen on a live relay on the review date. */
  g_assert_cmpint(gh_inbox_probe_classify_closed(
                    "ERROR: auth-required: requested filter requires authentication"), ==,
                  GH_INBOX_PROBE_PRIVATE);
  g_assert_cmpint(gh_inbox_probe_classify_closed("blocked: go away"), ==, GH_INBOX_PROBE_REFUSED);
  g_assert_cmpint(gh_inbox_probe_classify_closed("error: overloaded"), ==, GH_INBOX_PROBE_REFUSED);
  g_assert_cmpint(gh_inbox_probe_classify_closed("rate-limited: auth-required: no"), ==,
                  GH_INBOX_PROBE_REFUSED);
  g_assert_cmpint(gh_inbox_probe_classify_closed(""), ==, GH_INBOX_PROBE_REFUSED);
  g_assert_cmpint(gh_inbox_probe_classify_closed(NULL), ==, GH_INBOX_PROBE_REFUSED);
}

/* ---- the private-reads check ----------------------------------------------- */

typedef struct {
  guint calls;
  GHashTable *results;
} ProbeLog;

static void
on_probe(GhInboxProbe *probe, const gchar *url, GhInboxProbeResult result, const gchar *detail,
         gpointer data)
{
  ProbeLog *log = data;
  (void)probe;
  (void)detail;
  log->calls++;
  g_assert_false(g_hash_table_contains(log->results, url)); /* once per URL */
  g_hash_table_insert(log->results, g_strdup(url), GUINT_TO_POINTER(result));
}

static gboolean
probe_scopes_closed(gpointer data)
{
  Fixture *f = data;
  for (guint i = 0; i < f->rec.scopes->len; i++) {
    ScopeOpen *open = g_ptr_array_index(f->rec.scopes, i);
    if (open->probe && !open->closed)
      return FALSE;
  }
  return TRUE;
}

static void
test_probe(Fixture *f, gconstpointer data)
{
  (void)data;
  const gchar *urls[] = { INBOX_A, INBOX_B, WRITE_W, READ_R };
  ProbeLog log = { 0, g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL) };
  GhInboxProbe *probe = gh_inbox_probe_new(gh_account_controller_get_generation(f->accounts),
    &scope_transport, &scope_auth_transport, &f->rec, on_probe, &log);
  for (guint i = 0; i < G_N_ELEMENTS(urls); i++)
    g_assert_true(gh_inbox_probe_add_url(probe, urls[i], NULL));
  g_assert_cmpuint(probe_opens(f), ==, 0); /* nothing before start */
  g_assert_false(gh_inbox_probe_is_complete(probe));
  g_assert_true(gh_inbox_probe_start(probe, NULL));
  g_assert_cmpuint(probe_opens(f), ==, G_N_ELEMENTS(urls));
  for (guint i = 0; i < G_N_ELEMENTS(urls); i++) {
    ScopeOpen *open = probe_open(f, urls[i]);
    /* A random recipient, never the account's key, and no author. */
    g_assert_cmpuint(strlen(open->p), ==, 64);
    g_assert_cmpstr(open->p, !=, hex[1]);
    g_assert_cmpuint(open->authors, ==, 0);
    g_assert_cmpint(open->limit, ==, 1);
  }

  /* A: challenge, then refused as auth-required: private. Nothing signs. */
  GhRelayScope *scope = probe_open(f, INBOX_A)->scope;
  gh_relay_scope_auth_challenge(scope, INBOX_A, "challenge-a");
  gh_relay_scope_notice(scope, INBOX_A, GH_RELAY_NOTICE_CLOSED, NULL, FALSE,
                        "auth-required: you must auth");
  /* B answers without any sign-in: open. */
  gh_relay_scope_eose(scope, INBOX_B);
  /* W wraps the prefix; R never answers before its connection fails. */
  gh_relay_scope_notice(scope, WRITE_W, GH_RELAY_NOTICE_CLOSED, NULL, FALSE,
                        "ERROR: auth-required: requested filter requires authentication");
  g_assert_false(gh_inbox_probe_is_complete(probe));
  gh_relay_scope_notice(scope, READ_R, GH_RELAY_NOTICE_ERROR, NULL, FALSE, "refused");
  /* A late answer changes nothing. */
  gh_relay_scope_eose(scope, INBOX_A);

  g_assert_true(gh_inbox_probe_is_complete(probe));
  g_assert_cmpuint(log.calls, ==, 4);
  g_assert_cmpint(gh_inbox_probe_get_result(probe, INBOX_A), ==, GH_INBOX_PROBE_PRIVATE);
  g_assert_cmpint(gh_inbox_probe_get_result(probe, INBOX_B), ==, GH_INBOX_PROBE_OPEN);
  g_assert_cmpint(gh_inbox_probe_get_result(probe, WRITE_W), ==, GH_INBOX_PROBE_PRIVATE);
  g_assert_cmpint(gh_inbox_probe_get_result(probe, READ_R), ==, GH_INBOX_PROBE_UNREACHABLE);
  g_assert_cmpuint(f->rec.scope_auth_sent, ==, 0);
  g_assert_cmpuint(f->mock.calls, ==, 0);
  /* Complete: its connections close (from an idle). */
  gh_test_spin_until(probe_scopes_closed, f);
  gh_inbox_probe_unref(probe);
  g_hash_table_unref(log.results);
}

/* ---- publishing ------------------------------------------------------------ */

static void
new_setup(Fixture *f)
{
  GhInboxSetupConfig config = config_of(f);
  f->setup = gh_inbox_setup_new(&config);
}

/* PT-9 and signer denial: planning and a refused signature contact no
 * relay at all. */
static void
test_nothing_before_signer(Fixture *f, gconstpointer data)
{
  (void)data;
  new_setup(f);
  const gchar *chosen[] = { "inbox-a.example.org", INBOX_B, NULL };
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) plan = gh_inbox_setup_plan(f->setup, chosen, TRUE, &error);
  g_assert_no_error(error);
  /* 2 chosen inboxes + the relay suggestions (adopt with empty discovery). */
  g_autoptr(GPtrArray) suggestions = gh_inbox_setup_load_suggestions(NULL);
  g_assert_cmpuint(plan->len, ==, 2 + (suggestions ? suggestions->len : 0));
  const GhInboxSetupRelay *first = g_ptr_array_index(plan, 0);
  g_assert_cmpstr(first->url, ==, INBOX_A);
  /* Nothing configured to find lists yet: adopting makes them discovery. */
  g_assert_cmpint(first->roles, ==, GH_INBOX_SETUP_ROLE_INBOX | GH_INBOX_SETUP_ROLE_DISCOVERY);
  g_assert_cmpuint(f->rec.scopes->len + f->rec.pubs->len, ==, 0);

  f->mock.hold = TRUE;
  g_assert_true(gh_inbox_setup_start(f->setup, chosen, TRUE, &error));
  g_assert_no_error(error);
  g_assert_cmpint(gh_inbox_setup_get_state(f->setup), ==, GH_INBOX_SETUP_SIGNING);
  gh_test_spin_until(held_one, &f->mock);
  /* The approval is pending: still no connection of any kind. */
  g_assert_cmpuint(f->rec.scopes->len + f->rec.pubs->len, ==, 0);
  f->mock.deny = TRUE;
  gh_test_signer_release_all(&f->mock);
  gh_test_spin_until(setup_finished, f->setup);
  g_assert_cmpint(gh_inbox_setup_get_state(f->setup), ==, GH_INBOX_SETUP_FAILED);
  g_assert_error((GError *)gh_inbox_setup_get_error(f->setup), GH_SIGNER_ERROR,
                 GH_SIGNER_ERROR_DENIED);
  g_assert_null(gh_inbox_setup_get_event_id(f->setup));
  g_assert_cmpuint(f->rec.scopes->len + f->rec.pubs->len, ==, 0);
  g_auto(GStrv) discovery = g_settings_get_strv(f->settings, "discovery-relays");
  g_assert_null(discovery[0]);
  /* One use per setup. */
  g_assert_false(gh_inbox_setup_start(f->setup, chosen, TRUE, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PENDING);
}

static void
test_input_errors(Fixture *f, gconstpointer data)
{
  (void)data;
  new_setup(f);
  static const gchar *const empty[] = { NULL };
  static const gchar *const insecure[] = { "ws://relay.example.org", NULL };
  static const gchar *const nine[] = {
    "wss://r1.example.org", "wss://r2.example.org", "wss://r3.example.org",
    "wss://r4.example.org", "wss://r5.example.org", "wss://r6.example.org",
    "wss://r7.example.org", "wss://r8.example.org", "wss://r9.example.org", NULL,
  };
  const gchar *const *bad[] = { empty, insecure, nine };
  for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
    g_autoptr(GError) error = NULL;
    g_assert_false(gh_inbox_setup_start(f->setup, bad[i], FALSE, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  }
  /* More publication targets than one publish may use (16). */
  g_autoptr(GPtrArray) discovery = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i < 9; i++)
    g_ptr_array_add(discovery, g_strdup_printf("wss://d%u.example.org", i));
  g_ptr_array_add(discovery, NULL);
  g_settings_set_strv(f->settings, "discovery-relays", (const gchar *const *)discovery->pdata);
  g_autoptr(GError) error = NULL;
  const gchar *eight[] = { nine[0], nine[1], nine[2], nine[3], nine[4], nine[5], nine[6],
                           nine[7], NULL };
  g_assert_false(gh_inbox_setup_start(f->setup, eight, FALSE, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);
  g_settings_reset(f->settings, "discovery-relays");
  /* No account chosen. */
  g_assert_true(gh_account_controller_select(f->accounts, "", NULL));
  gh_test_spin_until(is_unselected, f->accounts);
  const gchar *one[] = { INBOX_A, NULL };
  g_assert_false(gh_inbox_setup_start(f->setup, one, FALSE, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED);
  g_assert_cmpint(gh_inbox_setup_get_state(f->setup), ==, GH_INBOX_SETUP_IDLE);
  g_assert_cmpuint(f->mock.calls, ==, 0);
  g_assert_cmpuint(probe_opens(f) + f->rec.pubs->len, ==, 0);
}

/* The list goes to the chosen inbox relays, the own 10002 write relay and
 * the discovery relay, each on its own connection with its own recorded
 * outcome; the 10002 read relay is never contacted. Account AUTH happens
 * only on a publication connection; the check never authenticates. */
static void
test_publish_outcomes_and_auth(Fixture *f, gconstpointer data)
{
  (void)data;
  discover_own_lists(f);
  guint discovery_scopes = f->rec.scopes->len;
  new_setup(f);
  const gchar *chosen[] = { INBOX_A, INBOX_B, NULL };
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_inbox_setup_start(f->setup, chosen, TRUE, &error));
  g_assert_no_error(error);
  gh_test_spin_until(pubs_opened, f);
  g_assert_cmpint(gh_inbox_setup_get_state(f->setup), ==, GH_INBOX_SETUP_PUBLISHING);

  /* Exactly the four targets; READ_R is not one. */
  g_assert_cmpuint(f->rec.pubs->len, ==, 4);
  const gchar *targets[] = { INBOX_A, INBOX_B, WRITE_W, DISC_D };
  for (guint i = 0; i < G_N_ELEMENTS(targets); i++)
    g_assert_nonnull(pub_find(f, targets[i]));
  g_assert_null(pub_find(f, READ_R));
  /* The same signed kind-10050 list everywhere, naming the chosen relays. */
  PubOpen *a = pub_find(f, INBOX_A);
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(event, a->event_json, NULL), ==, 1);
  g_assert_cmpint(nostr_event_get_kind(event), ==, 10050);
  g_assert_cmpstr(nostr_event_get_pubkey(event), ==, hex[1]);
  g_autofree gchar *list_id = nostr_event_get_id(event);
  g_assert_cmpstr(list_id, ==, gh_inbox_setup_get_event_id(f->setup));
  NostrTags *tags = nostr_event_get_tags(event);
  g_assert_cmpuint(nostr_tags_size(tags), ==, 2);
  g_assert_cmpstr(nostr_tag_get(nostr_tags_get(tags, 0), 1), ==, INBOX_A);
  g_assert_cmpstr(nostr_tag_get(nostr_tags_get(tags, 1), 1), ==, INBOX_B);
  nostr_event_free(event);
  for (guint i = 0; i < f->rec.pubs->len; i++)
    g_assert_cmpstr(((PubOpen *)g_ptr_array_index(f->rec.pubs, i))->event_json, ==,
                    a->event_json);
  /* Only the message relays are checked, on their own connections. */
  g_assert_cmpuint(f->rec.scopes->len - discovery_scopes, ==, 2);
  g_assert_cmpuint(probe_opens(f), ==, 2);
  GhRelayScope *probe = probe_open(f, INBOX_A)->scope;

  const gchar *id = gh_inbox_setup_get_event_id(f->setup);
  gh_relay_publish_ok(a->publish, INBOX_A, id, TRUE, "");
  gh_relay_publish_ok(a->publish, INBOX_B, id, FALSE, "blocked: only kind 1059 here");
  gh_relay_publish_failed(a->publish, WRITE_W, "connection refused");
  /* D asks for sign-in: one AUTH as the account on that publication
   * connection, then the list is sent again there. */
  gh_relay_publish_auth_challenge(a->publish, DISC_D, "challenge-d");
  gh_relay_publish_ok(a->publish, DISC_D, id, FALSE, "auth-required: sign in to publish");
  gh_test_spin_until(is_nonempty, f->rec.sent_auth);
  g_assert_cmpuint(f->rec.sent_auth->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(f->rec.sent_auth_on, 0), ==, DISC_D);
  gchar auth_id[65] = { 0 };
  g_assert_true(gh_relay_auth_verify_signed(g_ptr_array_index(f->rec.sent_auth, 0), DISC_D,
                                            "challenge-d", hex[1],
                                            g_get_real_time() / G_USEC_PER_SEC, auth_id, &error));
  g_assert_no_error(error);
  gh_relay_publish_ok(a->publish, DISC_D, auth_id, TRUE, "");
  g_assert_cmpuint(f->rec.resends, ==, 1);
  gh_relay_publish_ok(a->publish, DISC_D, id, TRUE, "");
  /* Signed by the policy's account signer: the inbox and the outbox see the
   * approval too (R6). */
  g_assert_cmpint(gh_auth_policy_get_account_state(gh_auth_policy_get_for_accounts(f->accounts),
                                                   DISC_D), ==, GH_AUTH_ACCOUNT_STATE_APPROVED);

  /* The check: A refuses unauthenticated reads (after a challenge that
   * nobody answers), B serves them. */
  gh_relay_scope_auth_challenge(probe, INBOX_A, "probe-challenge");
  gh_relay_scope_notice(probe, INBOX_A, GH_RELAY_NOTICE_CLOSED, NULL, FALSE,
                        "auth-required: you must auth");
  g_assert_cmpint(gh_inbox_setup_get_state(f->setup), ==, GH_INBOX_SETUP_PUBLISHING);
  gh_relay_scope_eose(probe, INBOX_B);
  gh_test_spin_until(setup_finished, f->setup);

  g_assert_cmpint(gh_inbox_setup_get_state(f->setup), ==, GH_INBOX_SETUP_DONE);
  g_assert_null(gh_inbox_setup_get_error(f->setup));
  g_assert_cmpuint(gh_inbox_setup_get_n_relays(f->setup), ==, 4);
  g_assert_cmpuint(gh_inbox_setup_get_n_accepted(f->setup), ==, 2);
  const GhInboxSetupRelay *r = gh_inbox_setup_lookup(f->setup, INBOX_A);
  g_assert_cmpint(r->roles, ==, GH_INBOX_SETUP_ROLE_INBOX);
  g_assert_cmpint(r->outcome, ==, GH_RELAY_PUBLISH_ACCEPTED);
  g_assert_true(r->probed);
  g_assert_cmpint(r->probe, ==, GH_INBOX_PROBE_PRIVATE);
  r = gh_inbox_setup_lookup(f->setup, INBOX_B);
  g_assert_cmpint(r->outcome, ==, GH_RELAY_PUBLISH_REJECTED);
  g_assert_cmpint(r->prefix, ==, GH_RELAY_OK_PREFIX_BLOCKED);
  g_assert_cmpstr(r->message, ==, "blocked: only kind 1059 here");
  g_assert_cmpint(r->probe, ==, GH_INBOX_PROBE_OPEN);
  r = gh_inbox_setup_lookup(f->setup, WRITE_W);
  g_assert_cmpint(r->roles, ==, GH_INBOX_SETUP_ROLE_WRITE);
  g_assert_cmpint(r->outcome, ==, GH_RELAY_PUBLISH_CONNECTION_FAILED);
  g_assert_false(r->probed);
  r = gh_inbox_setup_lookup(f->setup, DISC_D);
  g_assert_cmpint(r->roles, ==, GH_INBOX_SETUP_ROLE_DISCOVERY);
  g_assert_cmpint(r->outcome, ==, GH_RELAY_PUBLISH_ACCEPTED);
  g_assert_null(gh_inbox_setup_lookup(f->setup, READ_R));

  /* The signer was asked twice: the list and D's AUTH. The check sent none. */
  g_assert_cmpuint(f->mock.calls, ==, 2);
  g_assert_cmpuint(f->rec.scope_auth_sent, ==, 0);
  /* Discovery relays were already set: they are left alone. */
  g_assert_false(gh_inbox_setup_get_adopted_discovery(f->setup));
  g_auto(GStrv) discovery = g_settings_get_strv(f->settings, "discovery-relays");
  g_assert_cmpstr(discovery[0], ==, DISC_D);
  g_assert_null(discovery[1]);
  gh_test_spin_until(probe_scopes_closed, f);
}

static void
settle_all(Fixture *f, gboolean accept_a, gboolean accept_b)
{
  gh_test_spin_until(pubs_opened, f);
  const gchar *id = gh_inbox_setup_get_event_id(f->setup);
  PubOpen *a = pub_find(f, INBOX_A);
  gh_relay_publish_ok(a->publish, INBOX_A, id, accept_a, accept_a ? "" : "invalid: no");
  gh_relay_publish_ok(a->publish, INBOX_B, id, accept_b, accept_b ? "" : "blocked: no");
  /* nostrc-mi1z: suggestion relays may also be targets; match the test's
   * accept/reject intent — accept discovery targets iff any inbox accepted. */
  gboolean accept_others = accept_a || accept_b;
  for (guint i = 0; i < f->rec.pubs->len; i++) {
    PubOpen *p = g_ptr_array_index(f->rec.pubs, i);
    if (!p->closed && g_strcmp0(p->url, INBOX_A) != 0 &&
        g_strcmp0(p->url, INBOX_B) != 0)
      gh_relay_publish_ok(a->publish, p->url, id, accept_others,
                          accept_others ? "" : "blocked: no");
  }
  GhRelayScope *probe = probe_open(f, INBOX_A)->scope;
  gh_relay_scope_eose(probe, INBOX_A);
  gh_relay_scope_eose(probe, INBOX_B);
  gh_test_spin_until(setup_finished, f->setup);
}

/* With nothing set up to find lists, the relays that kept the list become
 * the discovery relays, only when the user agreed and only on success. */
static void
test_adopt_discovery(Fixture *f, gconstpointer data)
{
  (void)data;
  const gchar *chosen[] = { INBOX_A, INBOX_B, NULL };
  for (guint adopt = 0; adopt < 2; adopt++) {
    new_setup(f);
    g_assert_true(gh_inbox_setup_start(f->setup, chosen, adopt, NULL));
    settle_all(f, TRUE, FALSE);
    g_assert_cmpint(gh_inbox_setup_get_state(f->setup), ==, GH_INBOX_SETUP_DONE);
    g_auto(GStrv) discovery = g_settings_get_strv(f->settings, "discovery-relays");
    if (adopt) {
      g_assert_true(gh_inbox_setup_get_adopted_discovery(f->setup));
      g_assert_cmpstr(discovery[0], ==, INBOX_A);
      g_assert_null(discovery[1]);
    } else {
      g_assert_false(gh_inbox_setup_get_adopted_discovery(f->setup));
      g_assert_null(discovery[0]);
    }
    gh_test_release(g_steal_pointer(&f->setup));
    g_ptr_array_set_size(f->rec.pubs, 0);
  }
}

static void
test_nothing_accepted(Fixture *f, gconstpointer data)
{
  (void)data;
  const gchar *chosen[] = { INBOX_A, INBOX_B, NULL };
  new_setup(f);
  g_assert_true(gh_inbox_setup_start(f->setup, chosen, TRUE, NULL));
  settle_all(f, FALSE, FALSE);
  g_assert_cmpint(gh_inbox_setup_get_state(f->setup), ==, GH_INBOX_SETUP_FAILED);
  g_assert_error((GError *)gh_inbox_setup_get_error(f->setup), G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_cmpint(gh_inbox_setup_lookup(f->setup, INBOX_A)->prefix, ==,
                  GH_RELAY_OK_PREFIX_INVALID);
  g_auto(GStrv) discovery = g_settings_get_strv(f->settings, "discovery-relays");
  g_assert_null(discovery[0]);
}

typedef struct {
  GhInboxSetup *setup;
  const gchar *url;
} OutcomeWait;

static gboolean
outcome_known(gpointer data)
{
  OutcomeWait *wait = data;
  return gh_inbox_setup_lookup(wait->setup, wait->url)->outcome != GH_RELAY_PUBLISH_PENDING;
}

typedef struct {
  GhAuthPolicy *policy;
  const gchar *url;
} DeclinedWait;

static gboolean
declined(gpointer data)
{
  DeclinedWait *wait = data;
  return gh_auth_policy_get_account_state(wait->policy, wait->url) ==
         GH_AUTH_ACCOUNT_STATE_DECLINED;
}

/* Signs the list and opens the publication (INBOX_A, INBOX_B) and the check.
 * Returns A's publish connection. */
static PubOpen *
start_publishing(Fixture *f)
{
  const gchar *chosen[] = { INBOX_A, INBOX_B, NULL };
  new_setup(f);
  g_assert_true(gh_inbox_setup_start(f->setup, chosen, FALSE, NULL));
  gh_test_spin_until(pubs_opened, f);
  return pub_find(f, INBOX_A);
}

/* A refuses the list until signed in: a challenge, then OK false
 * "auth-required:". */
static void
demand_auth_on_a(Fixture *f, PubOpen *a, const gchar *challenge)
{
  gh_relay_publish_auth_challenge(a->publish, INBOX_A, challenge);
  gh_relay_publish_ok(a->publish, INBOX_A, gh_inbox_setup_get_event_id(f->setup), FALSE,
                      "auth-required: members only");
}

static void
wait_outcome_of_a(Fixture *f)
{
  OutcomeWait wait = { f->setup, INBOX_A };
  gh_test_spin_until(outcome_known, &wait);
}

/* B keeps the list and both check REQs answer: the setup finishes DONE. */
static void
finish_with_b(Fixture *f, PubOpen *a)
{
  gh_relay_publish_ok(a->publish, INBOX_B, gh_inbox_setup_get_event_id(f->setup), TRUE, "");
  GhRelayScope *probe = probe_open(f, INBOX_A)->scope;
  gh_relay_scope_eose(probe, INBOX_A);
  gh_relay_scope_eose(probe, INBOX_B);
  gh_test_spin_until(setup_finished, f->setup);
  g_assert_cmpint(gh_inbox_setup_get_state(f->setup), ==, GH_INBOX_SETUP_DONE);
}

/* nostrc-qp24.65, charter §4.4 R6: the own list publish signs in through
 * GhAuthPolicy (OWN_LIST_PUBLISH), with the one account signer of the
 * generation that the inbox and the self-copy use. The prompt is the
 * policy's (WAITING there while the user decides), and a relay the user
 * declined is not asked again this generation: publishing the list again
 * (the onboarding's Retry, a new GhInboxSetup) asks the signer for the list
 * only, and that relay ends "requires sign-in" without any AUTH. */
static void
test_auth_shared_with_policy(Fixture *f, gconstpointer data)
{
  (void)data;
  GhAuthPolicy *policy = gh_auth_policy_get_for_accounts(f->accounts);
  PubOpen *a = start_publishing(f);
  f->mock.hold = TRUE;
  demand_auth_on_a(f, a, "challenge-a");
  gh_test_spin_until(held_one, &f->mock);
  g_assert_cmpint(gh_auth_policy_get_account_state(policy, INBOX_A), ==,
                  GH_AUTH_ACCOUNT_STATE_WAITING);
  /* The user declines signing in to A. */
  f->mock.deny = TRUE;
  gh_test_signer_release_all(&f->mock);
  DeclinedWait refused = { policy, INBOX_A };
  gh_test_spin_until(declined, &refused);
  f->mock.deny = FALSE;
  f->mock.hold = FALSE;
  wait_outcome_of_a(f);
  g_assert_cmpint(gh_inbox_setup_lookup(f->setup, INBOX_A)->outcome, ==,
                  GH_RELAY_PUBLISH_AUTH_REQUIRED);
  finish_with_b(f, a);
  g_assert_cmpuint(f->mock.calls, ==, 2); /* the list and A's AUTH */
  g_assert_cmpuint(f->rec.sent_auth->len, ==, 0);

  /* Publishing again: only the list goes to the signer; A is not asked. */
  gh_test_release(g_steal_pointer(&f->setup));
  g_ptr_array_set_size(f->rec.pubs, 0);
  a = start_publishing(f);
  demand_auth_on_a(f, a, "challenge-a2");
  wait_outcome_of_a(f);
  g_assert_cmpint(gh_inbox_setup_lookup(f->setup, INBOX_A)->outcome, ==,
                  GH_RELAY_PUBLISH_AUTH_REQUIRED);
  finish_with_b(f, a);
  g_assert_cmpuint(f->mock.calls, ==, 3);
  g_assert_cmpuint(f->rec.sent_auth->len, ==, 0);
  g_assert_cmpint(gh_auth_policy_get_account_state(policy, INBOX_A), ==,
                  GH_AUTH_ACCOUNT_STATE_DECLINED);
  gh_test_spin_until(probe_scopes_closed, f);
}

static gboolean
all_pubs_closed(gpointer data)
{
  Fixture *f = data;
  for (guint i = 0; i < f->rec.pubs->len; i++)
    if (!((PubOpen *)g_ptr_array_index(f->rec.pubs, i))->closed)
      return FALSE;
  return TRUE;
}

/* An account switch ends the setup: while signing nothing is ever sent;
 * while publishing every connection closes. */
static void
test_account_switch(Fixture *f, gconstpointer data)
{
  (void)data;
  const gchar *chosen[] = { INBOX_A, INBOX_B, NULL };
  new_setup(f);
  f->mock.hold = TRUE;
  g_assert_true(gh_inbox_setup_start(f->setup, chosen, FALSE, NULL));
  gh_test_spin_until(held_one, &f->mock);
  g_assert_true(gh_account_controller_select(f->accounts, npub[2], NULL));
  gh_test_spin_until(setup_finished, f->setup);
  g_assert_error((GError *)gh_inbox_setup_get_error(f->setup), G_IO_ERROR, G_IO_ERROR_CANCELLED);
  f->mock.hold = FALSE;
  gh_test_signer_release_all(&f->mock);
  gh_test_spin_until(is_active, f->accounts);
  g_assert_cmpuint(probe_opens(f) + f->rec.pubs->len, ==, 0);
  gh_test_release(g_steal_pointer(&f->setup));

  new_setup(f);
  g_assert_true(gh_inbox_setup_start(f->setup, chosen, FALSE, NULL));
  gh_test_spin_until(pubs_opened, f);
  g_assert_true(gh_account_controller_select(f->accounts, npub[1], NULL));
  gh_test_spin_until(setup_finished, f->setup);
  g_assert_error((GError *)gh_inbox_setup_get_error(f->setup), G_IO_ERROR, G_IO_ERROR_CANCELLED);
  gh_test_spin_until(all_pubs_closed, f);
  gh_test_spin_until(probe_scopes_closed, f);
}

/* nostrc-mi1z phase 2: onboarding with adopt publishes to the relay
 * suggestions as discovery targets, so other apps find the lists on the
 * well-known relays (not only on the chosen message relays). */
static void
test_onboarding_targets_include_suggestions(Fixture *f, gconstpointer data)
{
  (void)data;
  new_setup(f);
  const gchar *chosen[] = { INBOX_A, NULL };
  /* With adopt and empty discovery-relays, the targets should include the
   * suggestion URLs from the shipped relay-suggestions.json. */
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) plan = gh_inbox_setup_plan(f->setup, chosen, TRUE, &error);
  g_assert_no_error(error);
  /* The plan has at least: INBOX_A + the relay suggestions. */
  g_autoptr(GPtrArray) suggestions = gh_inbox_setup_load_suggestions(NULL);
  g_assert_nonnull(suggestions);
  g_assert_cmpuint(suggestions->len, >=, 1);
  /* Every suggestion URL should appear as a target. */
  for (guint i = 0; i < suggestions->len; i++) {
    GhInboxSuggestion *s = g_ptr_array_index(suggestions, i);
    gboolean found = FALSE;
    for (guint j = 0; !found && j < plan->len; j++) {
      const GhInboxSetupRelay *target = g_ptr_array_index(plan, j);
      if (g_str_equal(target->url, s->url))
        found = TRUE;
    }
    g_assert_true(found);
  }
  g_assert_cmpuint(plan->len, >=, 1 + suggestions->len);
  /* Without adopt, the suggestions are NOT added. */
  g_autoptr(GPtrArray) plan_no_adopt = gh_inbox_setup_plan(f->setup, chosen, FALSE, &error);
  g_assert_no_error(error);
  /* Only INBOX_A is a target (no discovery, no write, no suggestions). */
  g_assert_cmpuint(plan_no_adopt->len, ==, 1);
}

/* nostrc-mi1z phase 2: a relay list edit (start_edit) that finds a newer
 * list on a target ends SKIPPED — never overwrites another client's work. */
static gboolean
list_setup_finished(gpointer data)
{
  GhRelayListSetupState state = gh_relay_list_setup_get_state(data);
  return state == GH_RELAY_LIST_SETUP_DONE || state == GH_RELAY_LIST_SETUP_FAILED ||
         state == GH_RELAY_LIST_SETUP_SKIPPED;
}

static void
test_edit_never_clobbers_newer(Fixture *f, gconstpointer data)
{
  (void)data;
  /* Set up discovery so GhAccountRelays can find the existing list. */
  discover_own_lists(f);

  GhInboxSetupConfig config = config_of(f);
  config.offer_relay_list = TRUE;

  /* Build an unsigned 10002 event: the "edited" list (adds a new relay). */
  const gchar *new_relays[] = { WRITE_W, INBOX_A, NULL };
  g_autofree gchar *unsigned_json =
    gh_inbox_setup_build_relay_list_unsigned(hex[1], new_relays,
                                             g_get_real_time() / G_USEC_PER_SEC);
  g_assert_nonnull(unsigned_json);

  /* Get the base event's id and created_at. */
  const gchar *base_json = gh_account_relays_get_relay_list_json(f->relays);
  g_assert_nonnull(base_json);
  NostrEvent *base_event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(base_event, base_json, NULL), ==, 1);
  char *raw_id = nostr_event_get_id(base_event);
  g_autofree gchar *base_id = g_strdup(raw_id);
  free(raw_id);
  gint64 base_created_at = nostr_event_get_created_at(base_event);
  nostr_event_free(base_event);

  /* Create a newer list "from another client" (created_at > base). */
  NostrEvent *newer = nostr_event_new();
  nostr_event_set_kind(newer, 10002);
  nostr_event_set_created_at(newer, base_created_at + 10);
  nostr_event_set_content(newer, "");
  nostr_event_set_tags(newer, nostr_tags_new(1,
    nostr_tag_new("r", "wss://other.example.org", "write", NULL)));
  g_assert_cmpint(nostr_event_sign(newer, gh_test_secret[1]), ==, 0);
  char *newer_raw = nostr_event_serialize_compact(newer);
  g_autofree gchar *newer_json = g_strdup(newer_raw);
  free(newer_raw);
  nostr_event_free(newer);

  /* Start the edit. Target is DISC_D. */
  const gchar *targets[] = { DISC_D, NULL };
  g_autoptr(GhRelayListSetup) setup = gh_relay_list_setup_new(&config);
  g_autoptr(GError) error = NULL;
  guint scopes_before = f->rec.scopes->len;
  g_assert_true(gh_relay_list_setup_start_edit(setup, unsigned_json, base_id,
                                                base_created_at, 10002, targets, &error));
  g_assert_no_error(error);
  g_assert_cmpint(gh_relay_list_setup_get_state(setup), ==, GH_RELAY_LIST_SETUP_CHECKING);

  /* The check scope asks {kinds:[10002], authors:[account]} on DISC_D.
   * Deliver the newer event and EOSE. */
  ScopeOpen *check = NULL;
  for (guint i = scopes_before; i < f->rec.scopes->len; i++) {
    ScopeOpen *open = g_ptr_array_index(f->rec.scopes, i);
    if (!open->probe && g_str_equal(open->url, DISC_D))
      check = open;
  }
  g_assert_nonnull(check);
  gh_relay_scope_event(check->scope, DISC_D, newer_json);
  gh_relay_scope_eose(check->scope, DISC_D);

  /* The setup should see the newer list and skip — never clobber. */
  gh_test_spin_until(list_setup_finished, setup);
  g_assert_cmpint(gh_relay_list_setup_get_state(setup), ==, GH_RELAY_LIST_SETUP_SKIPPED);
  /* No signer request (the newer list was found before signing). */
  g_assert_cmpuint(f->rec.pubs->len, ==, 0);
}

int
main(int argc, char **argv)
{
  for (guint key = 1; key < GH_TEST_KEYS; key++) {
    npub[key] = gh_test_npub(key);
    hex[key] = gh_test_pub(key);
  }
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/inbox-setup/normalize-url", test_normalize_url);
  g_test_add_func("/groundhog/inbox-setup/shipped-suggestions", test_shipped_suggestions);
  g_test_add_func("/groundhog/inbox-setup/parse-suggestions-refuses",
                  test_parse_suggestions_refuses);
  g_test_add_func("/groundhog/inbox-setup/build-unsigned", test_build_unsigned);
  g_test_add_func("/groundhog/inbox-setup/classify-closed", test_classify_closed);
#define ADD(path, func) \
  g_test_add("/groundhog/inbox-setup/" path, Fixture, NULL, fixture_setup, func, fixture_teardown)
  ADD("probe", test_probe);
  ADD("nothing-before-signer", test_nothing_before_signer);
  ADD("input-errors", test_input_errors);
  ADD("publish-outcomes-and-auth", test_publish_outcomes_and_auth);
  ADD("adopt-discovery", test_adopt_discovery);
  ADD("nothing-accepted", test_nothing_accepted);
  ADD("account-switch", test_account_switch);
  ADD("auth-shared-with-policy", test_auth_shared_with_policy);
  ADD("onboarding-targets-include-suggestions", test_onboarding_targets_include_suggestions);
  ADD("edit-never-clobbers-newer", test_edit_never_clobbers_newer);
#undef ADD
  int status = g_test_run();
  for (guint key = 1; key < GH_TEST_KEYS; key++) {
    g_free(npub[key]);
    g_free(hex[key]);
  }
  return status;
}
