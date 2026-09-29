/* GhDmInbox against recording relay transports (with their NIP-42 half) and
 * a mock signer on a private bus. Every relay frame is injected through the
 * scopes the inbox opened; nothing sleeps. */
#include "gh-account-auth.h"
#include "gh-dm-inbox.h"
#include "gh-nip17-inbox.h"
#include "gh-signer.h"
#include "gh-test-signer.h"

#include "nostr-tag.h"
#include "nostr/nip59/nip59.h"

#include <glib/gstdio.h>

#define DISCOVERY "wss://discovery.test.invalid"
#define HOME "wss://home.test.invalid"
#define INBOX_A "wss://inbox-a.test.invalid"
#define INBOX_B "wss://inbox-b.test.invalid"
#define INBOX_C "wss://inbox-c.test.invalid"
#define HOUR 3600

static gchar *npub[GH_TEST_KEYS];
static gchar *hex[GH_TEST_KEYS];

/* ---- recording transport --------------------------------------------------- */

/* One REQ connection: the live REQ (until 0) or an older page (until > 0). */
typedef struct {
  GhRelayScope *scope;
  gchar *url;
  gchar *p;
  gint64 since;
  gint64 until;
  int limit;
  gboolean closed;
  GPtrArray *auth;    /* signed AUTH events sent on this connection */
  guint resubscribes; /* REQs re-issued after an accepted AUTH */
} Req;

typedef struct {
  GPtrArray *reqs;      /* Req: every DM inbox REQ ever opened */
  GPtrArray *discovery; /* GhRelayScope: every relay-list discovery REQ */
} Recorder;

static gchar discovery_handle;

static void
req_free(gpointer data)
{
  Req *req = data;
  gh_relay_scope_unref(req->scope);
  g_ptr_array_unref(req->auth);
  g_free(req->url);
  g_free(req->p);
  g_free(req);
}

/* Asserts the exact DM inbox filter: kinds [1059], one #p, since, limit, an
 * until only on older pages, and nothing else. */
static gpointer
recorder_open(GhRelayScope *scope, const gchar *url, const NostrFilters *filters,
              gpointer data, GError **error)
{
  Recorder *rec = data;
  (void)error;
  g_assert_cmpuint(filters->count, ==, 1);
  const NostrFilter *filter = &filters->filters[0];
  if (nostr_filter_kinds_len(filter) == 2) {
    g_assert_cmpstr(url, ==, DISCOVERY);
    g_ptr_array_add(rec->discovery, gh_relay_scope_ref(scope));
    return &discovery_handle;
  }
  g_assert_cmpuint(nostr_filter_kinds_len(filter), ==, 1);
  g_assert_cmpint(nostr_filter_kinds_get(filter, 0), ==, 1059);
  g_assert_cmpuint(nostr_filter_authors_len(filter), ==, 0);
  g_assert_cmpuint(nostr_filter_ids_len(filter), ==, 0);
  g_assert_cmpuint(nostr_filter_tags_len(filter), ==, 1);
  g_assert_cmpuint(nostr_filter_tag_len(filter, 0), ==, 2);
  g_assert_cmpstr(nostr_filter_tag_get(filter, 0, 0), ==, "p");
  g_assert_null(nostr_filter_get_search(filter));
  char *json = nostr_filter_serialize_compact(filter);
  g_autofree gchar *wire_p = g_strdup_printf("\"#p\":[\"%s\"]",
                                             nostr_filter_tag_get(filter, 0, 1));
  g_assert_nonnull(strstr(json, wire_p));
  g_assert_null(strstr(json, "\"authors\""));
  free(json);
  Req *req = g_new0(Req, 1);
  req->scope = gh_relay_scope_ref(scope);
  req->url = g_strdup(url);
  req->p = g_strdup(nostr_filter_tag_get(filter, 0, 1));
  req->since = nostr_filter_get_since_i64(filter);
  req->until = nostr_filter_get_until_i64(filter);
  req->limit = nostr_filter_get_limit(filter);
  req->auth = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(rec->reqs, req);
  return req;
}

static void
recorder_close(gpointer handle, gpointer data)
{
  (void)data;
  if (handle != &discovery_handle)
    ((Req *)handle)->closed = TRUE;
}

static const GhRelayTransport recorder_transport = { recorder_open, recorder_close };

/* The NIP-42 half. Only DM inbox REQs (own 10050 relays) ever authenticate:
 * the discovery scope has no AUTH transport, and nothing else is opened. */
static gboolean
recorder_send_auth(gpointer handle, const gchar *signed_event_json, gpointer data,
                   GError **error)
{
  (void)data;
  (void)error;
  g_assert_true(handle != &discovery_handle);
  Req *req = handle;
  g_assert_false(req->closed);
  g_ptr_array_add(req->auth, g_strdup(signed_event_json));
  return TRUE;
}

static void
recorder_resubscribe(gpointer handle, gpointer data)
{
  (void)data;
  ((Req *)handle)->resubscribes++;
}

static const GhRelayAuthTransport recorder_auth = { recorder_send_auth,
                                                    recorder_resubscribe };

/* Open inbox REQs, sorted by URL. */
static GPtrArray *
open_reqs(Recorder *rec)
{
  GPtrArray *open = g_ptr_array_new();
  for (guint i = 0; i < rec->reqs->len; i++) {
    Req *req = g_ptr_array_index(rec->reqs, i);
    if (!req->closed)
      g_ptr_array_add(open, req);
  }
  return open;
}

/* The open live REQ (page FALSE) or older page (page TRUE) on url, if any. */
static Req *
find_req(Recorder *rec, const gchar *url, gboolean page)
{
  Req *found = NULL;
  for (guint i = 0; i < rec->reqs->len; i++) {
    Req *req = g_ptr_array_index(rec->reqs, i);
    if (!req->closed && g_str_equal(req->url, url) && (req->until != 0) == page) {
      g_assert_null(found);
      found = req;
    }
  }
  return found;
}

static Req *
open_req(Recorder *rec, const gchar *url)
{
  Req *found = find_req(rec, url, FALSE);
  g_assert_nonnull(found);
  return found;
}

static Req *
open_page(Recorder *rec, const gchar *url)
{
  Req *found = find_req(rec, url, TRUE);
  g_assert_nonnull(found);
  return found;
}

static guint
count_pages(Recorder *rec, const gchar *url)
{
  guint count = 0;
  for (guint i = 0; i < rec->reqs->len; i++) {
    Req *req = g_ptr_array_index(rec->reqs, i);
    count += req->until != 0 && g_str_equal(req->url, url);
  }
  return count;
}

/* ---- fixtures ---------------------------------------------------------------- */

typedef struct {
  GhTestBus bus;
  GhTestSigner signer;
  Recorder rec;
  GSettings *settings;
  GhAccountController *accounts;
  GhAccountRelays *relays;
  GhConversationStore *store;
  GhDmInbox *inbox;
  gchar *state_dir;
  gint64 list_time;
} Fixture;

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

static gboolean
listed(gpointer data)
{
  return gh_account_controller_get_state(data) != GH_ACCOUNT_STATE_DISCOVERING;
}

static GhDmInbox *
new_inbox(Fixture *f)
{
  GhDmInbox *inbox = gh_dm_inbox_new(f->accounts, f->relays, f->store, f->state_dir,
                                     &recorder_transport, &recorder_auth, &f->rec);
  g_assert_nonnull(inbox);
  return inbox;
}

static void
fixture_up(Fixture *f)
{
  g_autoptr(GError) error = NULL;
  gh_test_bus_up(&f->bus);
  gh_test_signer_up(&f->bus, &f->signer);
  f->rec.reqs = g_ptr_array_new_with_free_func(req_free);
  f->rec.discovery = g_ptr_array_new_with_free_func((GDestroyNotify)gh_relay_scope_unref);
  f->settings = g_settings_new("org.nostr.Groundhog");
  const gchar *sources[] = { DISCOVERY, NULL };
  g_settings_set_strv(f->settings, "discovery-relays", sources);
  g_settings_set_string(f->settings, "signer-method", "auto");
  g_settings_set_string(f->settings, "current-npub", npub[2]);
  f->accounts = gh_account_controller_new_full(f->settings, f->bus.client, fake_list, NULL);
  gh_test_spin_until(listed, f->accounts);
  g_assert_cmpint(gh_account_controller_get_state(f->accounts), ==, GH_ACCOUNT_STATE_ACTIVE);
  f->relays = gh_account_relays_new(f->accounts, f->settings, &recorder_transport, &f->rec);
  f->store = gh_conversation_store_new();
  f->state_dir = g_dir_make_tmp("groundhog-dm-inbox-XXXXXX", &error);
  g_assert_no_error(error);
  f->inbox = new_inbox(f);
  f->list_time = 1700000000;
}

static void
remove_tree(const gchar *path)
{
  GDir *dir = g_dir_open(path, 0, NULL);
  for (const gchar *name; dir && (name = g_dir_read_name(dir));) {
    g_autofree gchar *child = g_build_filename(path, name, NULL);
    g_assert_cmpint(g_unlink(child), ==, 0);
  }
  if (dir)
    g_dir_close(dir);
  g_assert_cmpint(g_rmdir(path), ==, 0);
}

static void
fixture_down(Fixture *f)
{
  if (f->inbox)
    gh_test_release(f->inbox);
  gh_test_release(f->relays);
  g_object_unref(f->store);
  gh_test_release(f->accounts);
  GhTestSenders check = { &f->bus, &f->signer };
  gh_test_spin_until(gh_test_signer_senders_closed, &check);
  gh_test_signer_down(&f->bus, &f->signer);
  g_ptr_array_unref(f->rec.reqs);
  g_ptr_array_unref(f->rec.discovery);
  g_object_unref(f->settings);
  gh_test_bus_down(&f->bus);
  remove_tree(f->state_dir);
  g_free(f->state_dir);
}

/* Publishes (through the discovery scope) a signed list authored by key:
 * kind 10050 with "relay" tags, or 10002 with "r" tags. */
static void
publish_list(Fixture *f, guint key, int kind, const gchar *first, ...)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, kind);
  nostr_event_set_created_at(event, ++f->list_time);
  nostr_event_set_content(event, "");
  NostrTags *tags = nostr_tags_new(0);
  va_list args;
  va_start(args, first);
  for (const gchar *url = first; url; url = va_arg(args, const gchar *))
    nostr_tags_append(tags, nostr_tag_new(kind == 10050 ? "relay" : "r", url, NULL));
  va_end(args);
  nostr_event_set_tags(event, tags);
  g_assert_cmpint(nostr_event_sign(event, gh_test_secret[key]), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  g_assert_cmpuint(f->rec.discovery->len, >, 0);
  gh_relay_scope_event(g_ptr_array_index(f->rec.discovery, f->rec.discovery->len - 1),
                       DISCOVERY, json);
  free(json);
}

static void
eose(Fixture *f, const gchar *url)
{
  Req *req = open_req(&f->rec, url);
  gh_relay_scope_eose(req->scope, url);
}

static void
deliver(Fixture *f, const gchar *url, const gchar *wrap)
{
  Req *req = open_req(&f->rec, url);
  gh_relay_scope_event(req->scope, url, wrap);
}

static GhDmInboxCounters
counters(GhDmInbox *inbox)
{
  GhDmInboxCounters out;
  gh_dm_inbox_get_counters(inbox, &out);
  return out;
}

/* Everything delivered so far has been settled (admitted, rejected, ...). */
static gboolean
settled(gpointer data)
{
  return counters(data).pending == 0;
}

static gboolean
is_live(gpointer data)
{
  return gh_dm_inbox_get_state(data) == GH_DM_INBOX_LIVE;
}

static gboolean
live_and_settled(gpointer data)
{
  return is_live(data) && settled(data);
}

/* Runs everything already dispatchable (EOSE judgments run at low priority). */
static void
drain(void)
{
  while (g_main_context_iteration(NULL, FALSE))
    ;
}

typedef struct {
  Recorder *rec;
  const gchar *url;
  guint count;
} PageWait;

static gboolean
pages_opened(gpointer data)
{
  PageWait *wait = data;
  return count_pages(wait->rec, wait->url) >= wait->count;
}

/* Waits for the count-th older page on url (the earlier ones are closed by
 * then) and returns it. */
static Req *
wait_page(Recorder *rec, const gchar *url, guint count)
{
  PageWait wait = { rec, url, count };
  gh_test_spin_until(pages_opened, &wait);
  g_assert_cmpuint(count_pages(rec, url), ==, count);
  return open_page(rec, url);
}

static GhDmInboxRelayState
relay_state(GhDmInbox *inbox, const gchar *url, const gchar **detail)
{
  return gh_dm_inbox_get_relay_state(inbox, url, detail);
}

typedef struct {
  GhDmInbox *inbox;
  const gchar *url;
  GhDmInboxRelayState state;
} RelayWait;

static gboolean
relay_reached(gpointer data)
{
  RelayWait *wait = data;
  return relay_state(wait->inbox, wait->url, NULL) == wait->state;
}

static gboolean
auth_sent(gpointer data)
{
  Req *req = data;
  return req->auth->len > 0;
}

typedef struct {
  GhTestSigner *signer;
  guint held;
} HeldWait;

static gboolean
held_reached(gpointer data)
{
  HeldWait *wait = data;
  return wait->signer->held->len >= wait->held;
}

/* ---- crafted wraps ------------------------------------------------------------ */

/* Test-only local keys stand in for remote peers and for the sender side of
 * the account's own copies. */
typedef struct {
  guint author;      /* rumor author */
  guint seal_signer; /* 0: the author; otherwise a forgery */
  guint to;          /* wrap p tag and outer NIP-44 peer */
  guint p[4];        /* rumor p tags, 0-terminated */
  gint64 created_at;
  const gchar *content;
  const gchar *subject;
  int kind;          /* rumor kind; 0: 14 */
  gint64 wrap_created_at; /* 0: as NIP-59 randomized it */
} Craft;

static gchar *
craft_wrap(const Craft *c, gchar **rumor_id)
{
  guint signer = c->seal_signer ? c->seal_signer : c->author;
  NostrEvent *rumor = nostr_event_new();
  nostr_event_set_kind(rumor, c->kind ? c->kind : 14);
  nostr_event_set_pubkey(rumor, hex[c->author]);
  nostr_event_set_created_at(rumor, c->created_at);
  nostr_event_set_content(rumor, c->content);
  NostrTags *tags = nostr_tags_new(0);
  for (guint i = 0; c->p[i]; i++)
    nostr_tags_append(tags, nostr_tag_new("p", hex[c->p[i]], NULL));
  if (c->subject)
    nostr_tags_append(tags, nostr_tag_new("subject", c->subject, NULL));
  nostr_event_set_tags(rumor, tags);
  rumor->id = nostr_event_get_id(rumor);
  if (rumor_id)
    *rumor_id = g_strdup(rumor->id);
  char *rumor_json = nostr_event_serialize_compact(rumor);
  nostr_event_free(rumor);

  guint8 sk[32], pk[32];
  g_assert_true(nostr_hex2bin(sk, gh_test_secret[signer], sizeof sk));
  g_assert_true(nostr_hex2bin(pk, hex[c->to], sizeof pk));
  char *ciphertext = NULL;
  g_assert_cmpint(nostr_nip44_encrypt_v2(sk, pk, (const guint8 *)rumor_json,
                                          strlen(rumor_json), &ciphertext), ==, 0);
  free(rumor_json);
  NostrEvent *seal = nostr_event_new();
  nostr_event_set_kind(seal, 13);
  nostr_event_set_pubkey(seal, hex[signer]);
  nostr_event_set_content(seal, ciphertext);
  nostr_event_set_created_at(seal, c->created_at);
  nostr_event_set_tags(seal, nostr_tags_new(0));
  free(ciphertext);
  g_assert_cmpint(nostr_event_sign(seal, gh_test_secret[signer]), ==, 0);

  guint8 ephemeral[32];
  g_assert_true(nostr_hex2bin(ephemeral, gh_test_secret[4], sizeof ephemeral));
  NostrEvent *wrap = nostr_nip59_wrap_with_key(seal, hex[c->to], ephemeral);
  nostr_event_free(seal);
  g_assert_nonnull(wrap);
  if (c->wrap_created_at) {
    /* What relays filter since/until on; re-signed with the wrap key. */
    nostr_event_set_created_at(wrap, c->wrap_created_at);
    g_assert_cmpint(nostr_event_sign(wrap, gh_test_secret[4]), ==, 0);
  }
  char *json = nostr_event_serialize_compact(wrap);
  nostr_event_free(wrap);
  gchar *out = g_strdup(json);
  free(json);
  return out;
}

/* A signed kind 1059 addressed to key 1, not the account: every inbox
 * rejects it before any signer call. Cheap filler for paging tests. */
static gchar *
junk_wrap(gint64 created_at, guint salt)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 1059);
  nostr_event_set_created_at(event, created_at);
  g_autofree gchar *content = g_strdup_printf("junk-%u", salt);
  nostr_event_set_content(event, content);
  nostr_event_set_tags(event, nostr_tags_new(1, nostr_tag_new("p", hex[1], NULL)));
  g_assert_cmpint(nostr_event_sign(event, gh_test_secret[4]), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  gchar *out = g_strdup(json);
  free(json);
  return out;
}

static gint
compare_hex(gconstpointer a, gconstpointer b)
{
  return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

static gchar *
room_id(guint a, guint b, guint c)
{
  g_autoptr(GPtrArray) members = g_ptr_array_new();
  g_ptr_array_add(members, hex[a]);
  g_ptr_array_add(members, hex[b]);
  if (c)
    g_ptr_array_add(members, hex[c]);
  g_ptr_array_sort(members, compare_hex);
  g_ptr_array_add(members, NULL);
  return g_strjoinv(",", (gchar **)members->pdata);
}

static void
go_live(Fixture *f)
{
  publish_list(f, 2, 10050, INBOX_A, INBOX_B, NULL);
  eose(f, INBOX_A);
  eose(f, INBOX_B);
  gh_test_spin_until(is_live, f->inbox);
}

/* ---- tests -------------------------------------------------------------------- */

/* A state file of Groundhog 0.6.0, named by the pubkey. */
static gchar *
legacy_file(Fixture *f, guint key, const gchar *suffix)
{
  g_autofree gchar *name = g_strconcat(hex[key], suffix, NULL);
  return g_build_filename(f->state_dir, name, NULL);
}

/* The account's pseudonymous rejected-wrap file. */
static gchar *
rejected_file(Fixture *f, guint key)
{
  g_autofree gchar *name = gh_nip17_seen_file_name(hex[key]);
  g_assert_nonnull(name);
  g_assert_null(strstr(name, hex[key]));
  return g_build_filename(f->state_dir, name, NULL);
}

/* The settled checkpoint: memory-only, it lives in this process only. */
static gboolean
has_checkpoint(Fixture *f)
{
  return gh_dm_inbox_get_checkpoint(f->inbox) > 0;
}

/* Rejected-file lines of one kind: "w ", "r " or "x " (rejected). */
static guint
seen_lines(Fixture *f, guint key, const gchar *prefix)
{
  g_autofree gchar *path = rejected_file(f, key);
  g_autofree gchar *contents = NULL;
  if (!g_file_get_contents(path, &contents, NULL, NULL))
    return 0;
  g_auto(GStrv) lines = g_strsplit(contents, "\n", -1);
  guint count = 0;
  for (guint i = 1; lines[i]; i++)
    count += g_str_has_prefix(lines[i], prefix);
  return count;
}

/* B1: the memory-only inbox leaves at most the account's rejected-wrap file
 * (no "w"/"r" key, no checkpoint, nothing named by a pubkey). */
static void
assert_state_files(Fixture *f, guint key)
{
  g_autofree gchar *allowed = gh_nip17_seen_file_name(hex[key]);
  GDir *dir = g_dir_open(f->state_dir, 0, NULL);
  g_assert_nonnull(dir);
  for (const gchar *name; (name = g_dir_read_name(dir));)
    g_assert_cmpstr(name, ==, allowed);
  g_dir_close(dir);
  g_assert_cmpuint(seen_lines(f, key, "w ") + seen_lines(f, key, "r "), ==, 0);
}

/* The since of a first session: nothing carried over, the initial window. */
static void
assert_initial_since(gint64 since, gint64 before, gint64 after)
{
  g_assert_cmpint(since, >=, before - GH_DM_INBOX_INITIAL_BACKFILL);
  g_assert_cmpint(since, <=, after - GH_DM_INBOX_INITIAL_BACKFILL);
}

static gchar *
wrap_id_of(const gchar *wrap_json)
{
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(event, wrap_json, NULL), ==, 1);
  char *id = nostr_event_get_id(event);
  nostr_event_free(event);
  gchar *out = g_strdup(id);
  free(id);
  return out;
}

static void
test_req_exact(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  /* Active account, but no 10050 list: no inbox REQ anywhere. */
  g_assert_cmpint(gh_dm_inbox_get_state(f.inbox), ==, GH_DM_INBOX_NO_INBOX_RELAYS);
  g_assert_cmpuint(gh_dm_inbox_get_generation(f.inbox), ==,
                   gh_account_controller_get_generation(f.accounts));
  g_assert_cmpuint(f.rec.reqs->len, ==, 0);
  g_assert_cmpuint(f.rec.discovery->len, ==, 1);
  g_assert_cmpstr(gh_conversation_store_get_account(f.store), ==, hex[2]);

  /* A NIP-65 list is not an inbox list, and neither is another author's. */
  publish_list(&f, 2, 10002, "wss://home.test.invalid", NULL);
  publish_list(&f, 1, 10050, INBOX_C, NULL);
  g_assert_cmpuint(f.rec.reqs->len, ==, 0);
  g_assert_cmpint(gh_dm_inbox_get_state(f.inbox), ==, GH_DM_INBOX_NO_INBOX_RELAYS);

  gint64 before = g_get_real_time() / G_USEC_PER_SEC;
  publish_list(&f, 2, 10050, INBOX_B, "https://not-a-relay.test.invalid", INBOX_A, INBOX_B,
               NULL);
  gint64 after = g_get_real_time() / G_USEC_PER_SEC;
  /* Exactly the account's own valid 10050 relays; never discovery or home. */
  g_autoptr(GPtrArray) open = open_reqs(&f.rec);
  g_assert_cmpuint(open->len, ==, 2);
  g_assert_cmpuint(f.rec.reqs->len, ==, 2);
  const gchar *const *relays = gh_dm_inbox_get_relays(f.inbox);
  g_assert_cmpstr(relays[0], ==, INBOX_A);
  g_assert_cmpstr(relays[1], ==, INBOX_B);
  g_assert_null(relays[2]);
  for (guint i = 0; i < open->len; i++) {
    Req *req = g_ptr_array_index(open, i);
    g_assert_true(g_str_equal(req->url, INBOX_A) || g_str_equal(req->url, INBOX_B));
    g_assert_cmpstr(req->p, ==, hex[2]);
    g_assert_cmpint(req->limit, ==, GH_DM_INBOX_REQ_LIMIT);
    /* No checkpoint yet: the bounded initial backfill. */
    g_assert_cmpint(req->since, >=, before - GH_DM_INBOX_INITIAL_BACKFILL);
    g_assert_cmpint(req->since, <=, after - GH_DM_INBOX_INITIAL_BACKFILL);
    g_assert_cmpint(req->since, ==, gh_dm_inbox_get_since(f.inbox));
  }
  g_assert_cmpint(gh_dm_inbox_get_state(f.inbox), ==, GH_DM_INBOX_CONNECTING);
  g_assert_cmpint(relay_state(f.inbox, INBOX_A, NULL), ==, GH_DM_INBOX_RELAY_CONNECTING);
  g_assert_cmpint(relay_state(f.inbox, HOME, NULL), ==, GH_DM_INBOX_RELAY_NONE);
  eose(&f, INBOX_A);
  g_assert_cmpint(gh_dm_inbox_get_state(f.inbox), ==, GH_DM_INBOX_BACKFILLING);
  eose(&f, INBOX_B);
  /* Each EOSE is judged (fewer than a page: nothing older) before LIVE. */
  g_assert_cmpint(gh_dm_inbox_get_state(f.inbox), ==, GH_DM_INBOX_BACKFILLING);
  gh_test_spin_until(is_live, f.inbox);
  g_assert_cmpint(relay_state(f.inbox, INBOX_A, NULL), ==, GH_DM_INBOX_RELAY_LIVE);
  g_assert_cmpint(relay_state(f.inbox, INBOX_B, NULL), ==, GH_DM_INBOX_RELAY_LIVE);
  g_assert_cmpuint(f.rec.reqs->len, ==, 2); /* no older page was needed */
  g_assert_cmpuint(counters(f.inbox).pages, ==, 0);

  /* Settled and live: the checkpoint advances (a relay-set change in this
   * process asks for since = checkpoint - two days plus slack, see
   * relay-change-reopens). */
  gint64 checkpoint = gh_dm_inbox_get_checkpoint(f.inbox);
  g_assert_cmpint(checkpoint, >=, after);
  g_assert_cmpint(checkpoint, <=, g_get_real_time() / G_USEC_PER_SEC);
  gh_test_release(f.inbox);
  g_assert_cmpuint(f.rec.reqs->len, ==, 2);
  g_autoptr(GPtrArray) none = open_reqs(&f.rec);
  g_assert_cmpuint(none->len, ==, 0); /* disposal closed both */
  /* A restart does not start from it: the messages it covered were kept in
   * memory only (W13 review B1), so the next process fetches the initial
   * window again. No checkpoint was written anywhere. */
  assert_state_files(&f, 2);
  gint64 restart_before = g_get_real_time() / G_USEC_PER_SEC;
  f.inbox = new_inbox(&f);
  gint64 restart_after = g_get_real_time() / G_USEC_PER_SEC;
  g_assert_cmpint(gh_dm_inbox_get_checkpoint(f.inbox), ==, 0);
  g_autoptr(GPtrArray) reopened = open_reqs(&f.rec);
  g_assert_cmpuint(reopened->len, ==, 2);
  for (guint i = 0; i < reopened->len; i++)
    assert_initial_since(((Req *)g_ptr_array_index(reopened, i))->since, restart_before,
                         restart_after);

  /* All inbox relays failing is an error state, not "live". An auth-required
   * CLOSED before any challenge has nothing to answer: reported at once. */
  gh_relay_scope_notice(open_req(&f.rec, INBOX_A)->scope, INBOX_A,
                        GH_RELAY_NOTICE_CLOSED, NULL, FALSE, "auth-required: test");
  gh_relay_scope_notice(open_req(&f.rec, INBOX_B)->scope, INBOX_B,
                        GH_RELAY_NOTICE_ERROR, NULL, FALSE, "refused");
  g_assert_cmpint(gh_dm_inbox_get_state(f.inbox), ==, GH_DM_INBOX_ERROR);
  g_assert_nonnull(strstr(gh_dm_inbox_get_error(f.inbox), "sign in"));
  const gchar *detail = NULL;
  g_assert_cmpint(relay_state(f.inbox, INBOX_A, &detail), ==, GH_DM_INBOX_RELAY_AUTH_REQUIRED);
  g_assert_cmpstr(detail, ==, "auth-required: test");
  g_assert_cmpint(relay_state(f.inbox, INBOX_B, &detail), ==, GH_DM_INBOX_RELAY_FAILED);
  g_assert_cmpstr(detail, ==, "refused");
  g_assert_cmpuint(f.signer.calls, ==, 0);
  fixture_down(&f);
}

static void
test_rooms_and_dedup(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  go_live(&f);

  /* 1 -> 2, delivered by both inbox relays: two copies, one unwrap. */
  g_autofree gchar *first_id = NULL;
  Craft first = { .author = 1, .to = 2, .p = { 2 }, .created_at = 1000, .content = "hi" };
  g_autofree gchar *first_wrap = craft_wrap(&first, &first_id);
  deliver(&f, INBOX_A, first_wrap);
  deliver(&f, INBOX_B, first_wrap);
  gh_test_spin_until(settled, f.inbox);
  g_assert_cmpuint(counters(f.inbox).admitted, ==, 1);
  g_assert_cmpuint(counters(f.inbox).received, ==, 2);
  g_assert_cmpuint(counters(f.inbox).skipped, ==, 1);
  g_assert_cmpuint(f.signer.calls, ==, 2);

  /* The sender re-wrapped the same rumor: unwrapped, but stored once; the
   * relay that carried the second copy is recorded. */
  g_autofree gchar *rewrap = craft_wrap(&first, NULL);
  deliver(&f, INBOX_B, rewrap);
  gh_test_spin_until(settled, f.inbox);
  g_assert_cmpuint(counters(f.inbox).duplicates, ==, 1);
  g_assert_cmpuint(counters(f.inbox).admitted, ==, 1);
  GhMessage *stored = gh_conversation_store_lookup_message(f.store, first_id);
  g_assert_nonnull(stored);
  const gchar *const *seen_on = gh_message_get_relays(stored);
  g_assert_cmpstr(seen_on[0], ==, INBOX_A);
  g_assert_cmpstr(seen_on[1], ==, INBOX_B);
  g_assert_null(seen_on[2]);

  /* 2 -> 1 (this account's own self-copy) and 1 -> 2 share one room. */
  guint calls = f.signer.calls;
  Craft reply = { .author = 2, .to = 2, .p = { 1 }, .created_at = 900, .content = "earlier" };
  g_autofree gchar *reply_wrap = craft_wrap(&reply, NULL);
  deliver(&f, INBOX_A, reply_wrap);
  /* A three-party room is a different room, whoever writes in it. */
  Craft group = { .author = 3, .to = 2, .p = { 2, 1 }, .created_at = 2000,
                  .content = "all", .subject = "Trip" };
  g_autofree gchar *group_wrap = craft_wrap(&group, NULL);
  deliver(&f, INBOX_A, group_wrap);
  Craft group_reply = { .author = 1, .to = 2, .p = { 3, 2 }, .created_at = 1500,
                        .content = "older subject", .subject = "Old" };
  g_autofree gchar *group_reply_wrap = craft_wrap(&group_reply, NULL);
  deliver(&f, INBOX_B, group_reply_wrap);

  /* Forgeries: addressed to someone else (rejected before the signer), and a
   * seal signed by 3 carrying a rumor that claims 1 as its author. */
  Craft wrong_p = { .author = 3, .to = 1, .p = { 2 }, .created_at = 3000, .content = "x" };
  g_autofree gchar *wrong_p_wrap = craft_wrap(&wrong_p, NULL);
  Craft forged = { .author = 1, .seal_signer = 3, .to = 2, .p = { 2 }, .created_at = 3000,
                   .content = "impostor" };
  g_autofree gchar *forged_id = NULL;
  g_autofree gchar *forged_wrap = craft_wrap(&forged, &forged_id);
  deliver(&f, INBOX_A, wrong_p_wrap);
  gh_test_spin_until(settled, f.inbox);
  g_assert_cmpuint(f.signer.calls, ==, calls + 6); /* three unwraps, not four */
  deliver(&f, INBOX_A, forged_wrap);
  gh_test_spin_until(settled, f.inbox);
  g_assert_cmpuint(f.signer.calls, ==, calls + 8);
  g_assert_false(gh_conversation_store_has_message(f.store, forged_id));

  GhDmInboxCounters c = counters(f.inbox);
  g_assert_cmpuint(c.admitted, ==, 4);
  g_assert_cmpuint(c.rejected, ==, 2);
  g_assert_cmpuint(c.duplicates, ==, 1);
  g_assert_cmpuint(c.deferred, ==, 0);
  g_assert_cmpuint(c.skipped, ==, 1);
  g_assert_cmpuint(c.received, ==, 8);
  /* The forgery cost two signer calls, so it was recorded as rejected: the
   * other relay's copy is skipped without a third. */
  deliver(&f, INBOX_B, forged_wrap);
  g_assert_cmpuint(counters(f.inbox).skipped, ==, 2);
  g_assert_cmpuint(f.signer.calls, ==, calls + 8);

  /* Two rooms, newest activity first. */
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(f.store)), ==, 2);
  g_autofree gchar *pair_id = room_id(1, 2, 0);
  g_autofree gchar *trio_id = room_id(1, 2, 3);
  g_autoptr(GhConversation) newest = g_list_model_get_item(G_LIST_MODEL(f.store), 0);
  g_assert_cmpstr(gh_conversation_get_room_id(newest), ==, trio_id);
  GhConversation *pair = gh_conversation_store_lookup(f.store, pair_id);
  g_assert_nonnull(pair);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(pair)), ==, 2);
  g_autoptr(GhMessage) m0 = g_list_model_get_item(G_LIST_MODEL(pair), 0);
  g_autoptr(GhMessage) m1 = g_list_model_get_item(G_LIST_MODEL(pair), 1);
  g_assert_cmpstr(gh_message_get_content(m0), ==, "earlier");
  g_assert_true(gh_message_is_self(m0));
  g_assert_cmpstr(gh_message_get_content(m1), ==, "hi");
  g_assert_false(gh_message_is_self(m1));
  g_assert_cmpstr(gh_conversation_get_peers(pair)[0], ==, hex[1]);
  g_assert_null(gh_conversation_get_peers(pair)[1]);
  /* The own reply is older than the incoming message: one unread. */
  g_assert_cmpuint(gh_conversation_get_unread_count(pair), ==, 1);
  GhConversation *trio = gh_conversation_store_lookup(f.store, trio_id);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(trio)), ==, 2);
  /* Nobody on this account wrote in the trio: a request. The pair holds the
   * account's own self-copy, so it is a conversation. */
  g_assert_false(gh_conversation_get_is_request(pair));
  g_assert_true(gh_conversation_get_is_request(trio));
  /* The latest subject by created_at wins, whatever the arrival order. */
  g_assert_cmpstr(gh_conversation_get_subject(trio), ==, "Trip");
  g_assert_cmpint(gh_conversation_get_last_activity(trio), ==, 2000);
  fixture_down(&f);
}

static void
test_seen_restart(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  go_live(&f);
  Craft one = { .author = 1, .to = 2, .p = { 2 }, .created_at = 1000, .content = "one" };
  Craft two = { .author = 3, .to = 2, .p = { 2 }, .created_at = 1001, .content = "two" };
  g_autofree gchar *one_wrap = craft_wrap(&one, NULL);
  g_autofree gchar *two_wrap = craft_wrap(&two, NULL);
  deliver(&f, INBOX_A, one_wrap);
  deliver(&f, INBOX_A, two_wrap);
  gh_test_spin_until(settled, f.inbox);
  g_assert_cmpuint(counters(f.inbox).admitted, ==, 2);
  g_assert_cmpuint(f.signer.calls, ==, 4);

  /* In this process a replay costs nothing. */
  deliver(&f, INBOX_B, one_wrap);
  g_assert_cmpuint(counters(f.inbox).skipped, ==, 1);
  g_assert_cmpuint(f.signer.calls, ==, 4);
  /* W13 review B1: the messages were only in memory, so nothing that would
   * make them count as seen was written, and no checkpoint either. */
  assert_state_files(&f, 2);

  /* Restart: a fresh inbox and store over the same state directory. The
   * messages are gone with the old process; the relays still hold them, so
   * the new session asks for the initial window, unwraps them again and
   * lists them again (never hides them for good). */
  gh_test_release(f.inbox);
  g_object_unref(f.store);
  f.store = gh_conversation_store_new();
  gint64 before = g_get_real_time() / G_USEC_PER_SEC;
  f.inbox = new_inbox(&f);
  gint64 after = g_get_real_time() / G_USEC_PER_SEC;
  g_assert_cmpint(gh_dm_inbox_get_state(f.inbox), ==, GH_DM_INBOX_CONNECTING);
  assert_initial_since(open_req(&f.rec, INBOX_A)->since, before, after);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(f.store)), ==, 0);
  deliver(&f, INBOX_B, one_wrap);
  deliver(&f, INBOX_A, two_wrap);
  gh_test_spin_until(settled, f.inbox);
  GhDmInboxCounters c = counters(f.inbox);
  g_assert_cmpuint(c.skipped, ==, 0);
  g_assert_cmpuint(c.admitted, ==, 2);
  g_assert_cmpuint(f.signer.calls, ==, 8);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(f.store)), ==, 2);
  g_autofree gchar *pair = room_id(1, 2, 0);
  g_autofree gchar *other = room_id(3, 2, 0);
  g_assert_nonnull(gh_conversation_store_lookup(f.store, pair));
  g_assert_nonnull(gh_conversation_store_lookup(f.store, other));

  /* A new message is unwrapped and admitted like any other. */
  Craft three = { .author = 1, .to = 2, .p = { 2 }, .created_at = 1002, .content = "three" };
  g_autofree gchar *three_wrap = craft_wrap(&three, NULL);
  deliver(&f, INBOX_A, three_wrap);
  gh_test_spin_until(settled, f.inbox);
  g_assert_cmpuint(counters(f.inbox).admitted, ==, 3);
  g_assert_cmpuint(f.signer.calls, ==, 10);
  assert_state_files(&f, 2);
  fixture_down(&f);
}

/* Groundhog 0.6.0 left <pubkey>.seen ("w"/"r" keys of messages it held only
 * in memory, and "x" rejected wraps) and <pubkey>.checkpoint. The memory-only
 * inbox keeps only the rejected ids, under the pseudonymous name, deletes
 * both files, and ignores the checkpoint: the relays' copies of those
 * messages are fetched and listed again. */
static void
test_legacy_state_files(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  gh_test_release(f.inbox);
  g_autofree gchar *rumor = NULL;
  Craft shown = { .author = 1, .to = 2, .p = { 2 }, .created_at = 1000, .content = "old" };
  g_autofree gchar *shown_wrap = craft_wrap(&shown, &rumor);
  g_autofree gchar *shown_id = wrap_id_of(shown_wrap);
  Craft forged = { .author = 1, .seal_signer = 3, .to = 2, .p = { 2 }, .created_at = 1001,
                   .content = "impostor" };
  g_autofree gchar *forged_wrap = craft_wrap(&forged, NULL);
  g_autofree gchar *forged_id = wrap_id_of(forged_wrap);
  g_autofree gchar *seen = g_strdup_printf("groundhog-nip17-seen 1 %s\nw %s\nr %s\nx %s\n",
                                           hex[2], shown_id, rumor, forged_id);
  gint64 old_checkpoint = g_get_real_time() / G_USEC_PER_SEC - HOUR;
  g_autofree gchar *mark = g_strdup_printf("groundhog-dm-inbox-checkpoint 1 %s %" G_GINT64_FORMAT
                                           "\n", hex[2], old_checkpoint);
  g_autofree gchar *seen_path = legacy_file(&f, 2, ".seen");
  g_autofree gchar *mark_path = legacy_file(&f, 2, ".checkpoint");
  g_assert_true(g_file_set_contents_full(seen_path, seen, -1, G_FILE_SET_CONTENTS_CONSISTENT,
                                         0600, NULL));
  g_assert_true(g_file_set_contents_full(mark_path, mark, -1, G_FILE_SET_CONTENTS_CONSISTENT,
                                         0600, NULL));
  /* Another account's 0.6.0 file is not this account's to touch. */
  g_autofree gchar *other_path = legacy_file(&f, 1, ".seen");
  g_autofree gchar *other = g_strdup_printf("groundhog-nip17-seen 1 %s\nx %s\n", hex[1],
                                            forged_id);
  g_assert_true(g_file_set_contents(other_path, other, -1, NULL));

  gint64 before = g_get_real_time() / G_USEC_PER_SEC;
  f.inbox = new_inbox(&f);
  publish_list(&f, 2, 10050, INBOX_A, NULL);
  gint64 after = g_get_real_time() / G_USEC_PER_SEC;
  g_assert_false(g_file_test(seen_path, G_FILE_TEST_EXISTS));
  g_assert_false(g_file_test(mark_path, G_FILE_TEST_EXISTS));
  g_assert_true(g_file_test(other_path, G_FILE_TEST_EXISTS));
  g_assert_cmpint(g_unlink(other_path), ==, 0);
  assert_state_files(&f, 2);
  g_assert_cmpuint(seen_lines(&f, 2, "x "), ==, 1);
  /* The old checkpoint is not trusted: the initial window. */
  assert_initial_since(open_req(&f.rec, INBOX_A)->since, before, after);

  /* The rejected wrap stays skipped before any signer call; the message the
   * old process showed once is unwrapped and listed again. */
  deliver(&f, INBOX_A, forged_wrap);
  deliver(&f, INBOX_A, shown_wrap);
  gh_test_spin_until(settled, f.inbox);
  GhDmInboxCounters c = counters(f.inbox);
  g_assert_cmpuint(c.skipped, ==, 1);
  g_assert_cmpuint(c.admitted, ==, 1);
  g_assert_cmpuint(f.signer.calls, ==, 2);
  g_assert_true(gh_conversation_store_has_message(f.store, rumor));
  fixture_down(&f);
}

static gboolean
held_or_done(gpointer data)
{
  Fixture *f = data;
  return f->signer.held->len > 0 || counters(f->inbox).admitted == 5;
}

static void
check_in_flight(GhDmInbox *inbox, gpointer data)
{
  guint limit = GPOINTER_TO_UINT(data);
  g_assert_cmpuint(counters(inbox).in_flight, <=, limit);
}

static void
test_bounded_concurrency(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  gh_dm_inbox_set_max_in_flight(f.inbox, 2);
  g_signal_connect(f.inbox, "changed", G_CALLBACK(check_in_flight), GUINT_TO_POINTER(2));
  go_live(&f);
  f.signer.hold = TRUE;
  for (guint i = 0; i < 5; i++) {
    Craft craft = { .author = 1, .to = 2, .p = { 2 }, .created_at = 1000 + i,
                    .content = "queued" };
    g_autofree gchar *wrap = craft_wrap(&craft, NULL);
    deliver(&f, INBOX_A, wrap);
  }
  HeldWait two = { &f.signer, 2 };
  gh_test_spin_until(held_reached, &two);
  GhDmInboxCounters c = counters(f.inbox);
  g_assert_cmpuint(c.in_flight, ==, 2);
  g_assert_cmpuint(c.pending, ==, 5);
  g_assert_cmpuint(f.signer.calls, ==, 2);
  /* Approving one at a time never lets a third approval open. */
  while (counters(f.inbox).admitted < 5) {
    gh_test_spin_until(held_or_done, &f);
    g_assert_cmpuint(f.signer.held->len, <=, 2);
    if (f.signer.held->len)
      gh_test_signer_release_one(&f.signer);
  }
  g_assert_cmpuint(f.signer.max_held, ==, 2);
  g_assert_cmpuint(f.signer.calls, ==, 10);
  g_assert_cmpuint(counters(f.inbox).pending, ==, 0);
  fixture_down(&f);
}

static void
test_switch_mid_unwrap(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  go_live(&f);
  guint64 first_generation = gh_dm_inbox_get_generation(f.inbox);
  f.signer.hold = TRUE;
  g_autofree gchar *rumor_id = NULL;
  Craft craft = { .author = 1, .to = 2, .p = { 2 }, .created_at = 1000, .content = "late" };
  g_autofree gchar *wrap = craft_wrap(&craft, &rumor_id);
  deliver(&f, INBOX_A, wrap);
  HeldWait one = { &f.signer, 1 };
  gh_test_spin_until(held_reached, &one);
  g_assert_cmpuint(counters(f.inbox).in_flight, ==, 1);
  GhRelayScope *old_scope = gh_relay_scope_ref(open_req(&f.rec, INBOX_A)->scope);

  /* Switch to key 1 while the unwrap waits on the user. */
  g_assert_true(gh_account_controller_select(f.accounts, npub[1], NULL));
  g_assert_cmpuint(gh_dm_inbox_get_generation(f.inbox), !=, first_generation);
  g_assert_cmpuint(gh_dm_inbox_get_generation(f.inbox), ==,
                   gh_account_controller_get_generation(f.accounts));
  g_autoptr(GPtrArray) open = open_reqs(&f.rec);
  g_assert_cmpuint(open->len, ==, 0);
  g_assert_cmpint(gh_dm_inbox_get_state(f.inbox), ==, GH_DM_INBOX_NO_INBOX_RELAYS);
  g_assert_cmpstr(gh_conversation_store_get_account(f.store), ==, hex[1]);
  GhDmInboxCounters c = counters(f.inbox);
  g_assert_cmpuint(c.in_flight, ==, 0);
  g_assert_cmpuint(c.received, ==, 0);
  /* The pending approval was revoked by closing its private sender. */
  GhTestSenders check = { &f.bus, &f.signer };
  gh_test_spin_until(gh_test_signer_senders_closed, &check);
  /* A late approval and late relay traffic for the old account are dropped. */
  gh_test_signer_release_all(&f.signer);
  f.signer.hold = FALSE;
  gh_relay_scope_event(old_scope, INBOX_A, wrap);
  gh_relay_scope_unref(old_scope);
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_cmpuint(counters(f.inbox).received, ==, 0);
  g_assert_cmpuint(counters(f.inbox).admitted, ==, 0);
  g_assert_false(gh_conversation_store_has_message(f.store, rumor_id));

  /* The next account gets its own REQ on its own inbox relays. */
  publish_list(&f, 1, 10050, INBOX_C, NULL);
  g_autoptr(GPtrArray) next = open_reqs(&f.rec);
  g_assert_cmpuint(next->len, ==, 1);
  Req *req = g_ptr_array_index(next, 0);
  g_assert_cmpstr(req->url, ==, INBOX_C);
  g_assert_cmpstr(req->p, ==, hex[1]);
  Craft for_one = { .author = 3, .to = 1, .p = { 1 }, .created_at = 1000, .content = "one" };
  g_autofree gchar *for_one_wrap = craft_wrap(&for_one, NULL);
  deliver(&f, INBOX_C, for_one_wrap);
  gh_test_spin_until(settled, f.inbox);
  g_assert_cmpuint(counters(f.inbox).admitted, ==, 1);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(f.store)), ==, 1);
  g_autofree gchar *room = room_id(1, 3, 0);
  g_assert_nonnull(gh_conversation_store_lookup(f.store, room));
  fixture_down(&f);
}

static void
test_relay_change_reopens(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  go_live(&f);
  gint64 checkpoint = gh_dm_inbox_get_checkpoint(f.inbox);
  g_assert_cmpint(checkpoint, >, 0);
  f.signer.hold = TRUE;
  Craft craft = { .author = 1, .to = 2, .p = { 2 }, .created_at = 1000, .content = "moved" };
  g_autofree gchar *wrap = craft_wrap(&craft, NULL);
  deliver(&f, INBOX_A, wrap);
  HeldWait one = { &f.signer, 1 };
  gh_test_spin_until(held_reached, &one);

  /* A newer list naming the same set (reordered) keeps the session. */
  publish_list(&f, 2, 10050, INBOX_B, INBOX_A, NULL);
  g_assert_cmpuint(f.rec.reqs->len, ==, 2);
  g_assert_cmpuint(counters(f.inbox).in_flight, ==, 1);

  /* A different set tears down (REQs and the pending approval) and reopens
   * only on the new relays, from the last settled checkpoint. */
  publish_list(&f, 2, 10050, INBOX_C, NULL);
  g_autoptr(GPtrArray) open = open_reqs(&f.rec);
  g_assert_cmpuint(open->len, ==, 1);
  Req *req = g_ptr_array_index(open, 0);
  g_assert_cmpstr(req->url, ==, INBOX_C);
  g_assert_cmpstr(req->p, ==, hex[2]);
  g_assert_cmpint(req->since, ==, checkpoint - GH_DM_INBOX_WRAP_SKEW);
  g_assert_cmpint(gh_dm_inbox_get_state(f.inbox), ==, GH_DM_INBOX_CONNECTING);
  GhDmInboxCounters c = counters(f.inbox);
  g_assert_cmpuint(c.in_flight, ==, 0);
  g_assert_cmpuint(c.pending, ==, 0);
  GhTestSenders check = { &f.bus, &f.signer };
  gh_test_spin_until(gh_test_signer_senders_closed, &check);
  gh_test_signer_release_all(&f.signer);
  f.signer.hold = FALSE;
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_cmpuint(counters(f.inbox).admitted, ==, 0);

  /* The cancelled wrap was never recorded as seen: the new relay's copy is
   * unwrapped and admitted. */
  deliver(&f, INBOX_C, wrap);
  gh_test_spin_until(settled, f.inbox);
  g_assert_cmpuint(counters(f.inbox).admitted, ==, 1);

  /* A newer list with no usable relay closes the inbox REQ entirely. */
  publish_list(&f, 2, 10050, "https://not-a-relay.test.invalid", NULL);
  g_autoptr(GPtrArray) after_loss = open_reqs(&f.rec);
  g_assert_cmpuint(after_loss->len, ==, 0);
  g_assert_cmpint(gh_dm_inbox_get_state(f.inbox), ==, GH_DM_INBOX_NO_INBOX_RELAYS);
  g_assert_null(gh_dm_inbox_get_relays(f.inbox));
  g_assert_cmpint(gh_dm_inbox_get_since(f.inbox), ==, 0);
  fixture_down(&f);
}

static void
test_signer_denied_defers(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  go_live(&f);
  gint64 checkpoint = gh_dm_inbox_get_checkpoint(f.inbox);
  g_assert_cmpint(checkpoint, >, 0);
  f.signer.deny = TRUE;
  Craft craft = { .author = 1, .to = 2, .p = { 2 }, .created_at = 1000, .content = "no" };
  g_autofree gchar *wrap = craft_wrap(&craft, NULL);
  deliver(&f, INBOX_A, wrap);
  gh_test_spin_until(settled, f.inbox);
  GhDmInboxCounters c = counters(f.inbox);
  g_assert_cmpuint(c.deferred, ==, 1);
  g_assert_cmpuint(c.rejected, ==, 0);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(f.store)), ==, 0);
  g_assert_cmpuint(f.signer.calls, ==, 1);
  /* Not asked again this session, whichever relay delivers it next. */
  deliver(&f, INBOX_B, wrap);
  g_assert_cmpuint(counters(f.inbox).skipped, ==, 1);
  drain();
  g_assert_cmpuint(f.signer.calls, ==, 1);
  /* A denial is transient, never recorded as rejected, and holds the
   * checkpoint: a later session offers the wrap again (a new signer prompt).
   * Memory-only, that later session is the next process, which starts from
   * the initial window. */
  g_assert_cmpint(gh_dm_inbox_get_checkpoint(f.inbox), ==, checkpoint);
  g_assert_cmpuint(seen_lines(&f, 2, "x "), ==, 0);
  f.signer.deny = FALSE;
  gh_test_release(f.inbox);
  gint64 before = g_get_real_time() / G_USEC_PER_SEC;
  f.inbox = new_inbox(&f);
  gint64 after = g_get_real_time() / G_USEC_PER_SEC;
  assert_initial_since(gh_dm_inbox_get_since(f.inbox), before, after);
  deliver(&f, INBOX_A, wrap);
  gh_test_spin_until(settled, f.inbox);
  g_assert_cmpuint(counters(f.inbox).admitted, ==, 1);
  g_assert_cmpuint(f.signer.calls, ==, 3);
  fixture_down(&f);
}

/* ---- backfill paging (nostrc-qp24.10.10) ------------------------------------- */

static void
deliver_page(Fixture *f, const gchar *url, const gchar *wrap)
{
  Req *req = open_page(&f->rec, url);
  gh_relay_scope_event(req->scope, url, wrap);
}

/* A relay that fills the REQ limit is paged backwards with until = the
 * oldest created_at seen, repeats skipped by id, until a page comes back
 * short; only then may the checkpoint move. */
static void
test_backfill_paging(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  gh_dm_inbox_set_backfill_limits(f.inbox, 3, GH_DM_INBOX_MAX_PAGES);
  publish_list(&f, 2, 10050, INBOX_A, INBOX_B, NULL);
  g_assert_cmpint(open_req(&f.rec, INBOX_A)->limit, ==, 3);
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  gchar *wraps[6];
  for (guint i = 0; i < G_N_ELEMENTS(wraps); i++) {
    g_autofree gchar *text = g_strdup_printf("message %u", i);
    Craft craft = { .author = 1, .to = 2, .p = { 2 }, .created_at = 1000 + i,
                    .content = text, .wrap_created_at = now - (gint64)(i + 1) * 10 * HOUR };
    wraps[i] = craft_wrap(&craft, NULL);
  }

  /* The live REQ is full: the newest three, then EOSE. */
  for (guint i = 0; i < 3; i++)
    deliver(&f, INBOX_A, wraps[i]);
  eose(&f, INBOX_A);
  eose(&f, INBOX_B);
  Req *page = wait_page(&f.rec, INBOX_A, 1);
  g_assert_cmpint(page->until, ==, now - 30 * HOUR);
  g_assert_cmpint(page->since, ==, gh_dm_inbox_get_since(f.inbox));
  g_assert_cmpint(page->limit, ==, 3);
  g_assert_cmpstr(page->p, ==, hex[2]);
  g_assert_null(find_req(&f.rec, INBOX_B, TRUE)); /* B held fewer than a page */
  g_assert_cmpint(gh_dm_inbox_get_state(f.inbox), ==, GH_DM_INBOX_BACKFILLING);
  g_assert_cmpint(relay_state(f.inbox, INBOX_A, NULL), ==, GH_DM_INBOX_RELAY_BACKFILLING);
  g_assert_cmpint(relay_state(f.inbox, INBOX_B, NULL), ==, GH_DM_INBOX_RELAY_LIVE);
  gh_test_spin_until(settled, f.inbox);
  g_assert_false(has_checkpoint(&f)); /* older wraps may be unfetched */

  /* until is inclusive: the boundary wrap comes again and is skipped. The
   * page is full, so the next one starts at its oldest. */
  deliver_page(&f, INBOX_A, wraps[2]);
  deliver_page(&f, INBOX_A, wraps[3]);
  deliver_page(&f, INBOX_A, wraps[4]);
  gh_relay_scope_eose(page->scope, INBOX_A);
  Req *next = wait_page(&f.rec, INBOX_A, 2);
  g_assert_true(page->closed);
  g_assert_cmpint(next->until, ==, now - 50 * HOUR);
  g_assert_cmpint(next->since, ==, gh_dm_inbox_get_since(f.inbox));
  gh_test_spin_until(settled, f.inbox);
  g_assert_false(has_checkpoint(&f));

  /* A short page: the relay is fully fetched and the checkpoint moves. */
  deliver_page(&f, INBOX_A, wraps[4]);
  deliver_page(&f, INBOX_A, wraps[5]);
  gh_relay_scope_eose(next->scope, INBOX_A);
  gh_test_spin_until(live_and_settled, f.inbox);
  g_assert_true(next->closed);
  g_assert_cmpuint(count_pages(&f.rec, INBOX_A), ==, 2);
  g_assert_cmpint(relay_state(f.inbox, INBOX_A, NULL), ==, GH_DM_INBOX_RELAY_LIVE);
  GhDmInboxCounters c = counters(f.inbox);
  g_assert_cmpuint(c.admitted, ==, 6);
  g_assert_cmpuint(c.skipped, ==, 2);
  g_assert_cmpuint(c.pages, ==, 2);
  g_assert_cmpuint(c.backfill_incomplete, ==, 0);
  g_assert_cmpuint(f.signer.calls, ==, 12);
  g_assert_true(has_checkpoint(&f));
  for (guint i = 0; i < G_N_ELEMENTS(wraps); i++)
    g_free(wraps[i]);
  fixture_down(&f);
}

/* At most max_pages pages per run: a relay that still returns full pages is
 * INCOMPLETE and holds the checkpoint. Its next EOSE (after a reconnect)
 * pages it again; a failed page is incomplete too. */
static void
test_backfill_page_bound(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  gh_dm_inbox_set_backfill_limits(f.inbox, 2, 2);
  publish_list(&f, 2, 10050, INBOX_A, NULL);
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  guint salt = 0;
  for (guint i = 1; i <= 2; i++) {
    g_autofree gchar *junk = junk_wrap(now - i * HOUR, salt++);
    deliver(&f, INBOX_A, junk);
  }
  eose(&f, INBOX_A);
  for (guint n = 1; n <= 2; n++) {
    Req *page = wait_page(&f.rec, INBOX_A, n);
    g_assert_cmpint(page->until, ==, now - (gint64)(2 * n) * HOUR);
    for (guint i = 1; i <= 2; i++) {
      g_autofree gchar *junk = junk_wrap(now - (gint64)(2 * n + i) * HOUR, salt++);
      gh_relay_scope_event(page->scope, INBOX_A, junk);
    }
    gh_relay_scope_eose(page->scope, INBOX_A);
  }
  gh_test_spin_until(live_and_settled, f.inbox);
  const gchar *detail = NULL;
  g_assert_cmpint(relay_state(f.inbox, INBOX_A, &detail), ==, GH_DM_INBOX_RELAY_INCOMPLETE);
  g_assert_nonnull(detail);
  g_assert_null(find_req(&f.rec, INBOX_A, TRUE)); /* no third page */
  GhDmInboxCounters c = counters(f.inbox);
  g_assert_cmpuint(c.pages, ==, 2);
  g_assert_cmpuint(c.backfill_incomplete, ==, 1);
  g_assert_cmpuint(c.rejected, ==, 6);
  g_assert_cmpuint(f.signer.calls, ==, 0);
  g_assert_false(has_checkpoint(&f));

  /* A reconnect: the relay has delivered a page's worth, so its EOSE pages
   * it again, from now (nothing new was delivered on this connection). This
   * page fails: still incomplete. */
  Req *live = open_req(&f.rec, INBOX_A);
  gh_relay_scope_notice(live->scope, INBOX_A, GH_RELAY_NOTICE_DISCONNECTED, NULL, FALSE, NULL);
  g_assert_cmpint(relay_state(f.inbox, INBOX_A, NULL), ==, GH_DM_INBOX_RELAY_CONNECTING);
  gint64 before = g_get_real_time() / G_USEC_PER_SEC;
  gh_relay_scope_eose(live->scope, INBOX_A);
  Req *page = wait_page(&f.rec, INBOX_A, 3);
  g_assert_cmpint(page->until, >=, before);
  gh_relay_scope_notice(page->scope, INBOX_A, GH_RELAY_NOTICE_CLOSED, NULL, FALSE,
                        "error: shutting down");
  RelayWait incomplete = { f.inbox, INBOX_A, GH_DM_INBOX_RELAY_INCOMPLETE };
  gh_test_spin_until(relay_reached, &incomplete);
  g_assert_cmpuint(counters(f.inbox).backfill_incomplete, ==, 2);
  g_assert_false(has_checkpoint(&f));

  /* The next reconnect's page comes back short: complete at last. */
  gh_relay_scope_notice(live->scope, INBOX_A, GH_RELAY_NOTICE_DISCONNECTED, NULL, FALSE, NULL);
  gh_relay_scope_eose(live->scope, INBOX_A);
  page = wait_page(&f.rec, INBOX_A, 4);
  gh_relay_scope_eose(page->scope, INBOX_A);
  RelayWait live_again = { f.inbox, INBOX_A, GH_DM_INBOX_RELAY_LIVE };
  gh_test_spin_until(relay_reached, &live_again);
  g_assert_true(has_checkpoint(&f));
  fixture_down(&f);
}

/* More than a page in one second cannot be paged with until: the run steps
 * over that second and the relay stays incomplete. */
static void
test_backfill_tied_second(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  gh_dm_inbox_set_backfill_limits(f.inbox, 2, GH_DM_INBOX_MAX_PAGES);
  publish_list(&f, 2, 10050, INBOX_A, NULL);
  gint64 tied = g_get_real_time() / G_USEC_PER_SEC - 5 * HOUR;
  g_autofree gchar *one = junk_wrap(tied, 1);
  g_autofree gchar *two = junk_wrap(tied, 2);
  deliver(&f, INBOX_A, one);
  deliver(&f, INBOX_A, two);
  eose(&f, INBOX_A);
  Req *page = wait_page(&f.rec, INBOX_A, 1);
  g_assert_cmpint(page->until, ==, tied);
  gh_relay_scope_event(page->scope, INBOX_A, one);
  gh_relay_scope_event(page->scope, INBOX_A, two);
  gh_relay_scope_eose(page->scope, INBOX_A);
  Req *next = wait_page(&f.rec, INBOX_A, 2);
  g_assert_true(page->closed);
  g_assert_cmpint(next->until, ==, tied - 1);
  gh_relay_scope_eose(next->scope, INBOX_A);
  RelayWait incomplete = { f.inbox, INBOX_A, GH_DM_INBOX_RELAY_INCOMPLETE };
  gh_test_spin_until(relay_reached, &incomplete);
  g_assert_cmpint(gh_dm_inbox_get_state(f.inbox), ==, GH_DM_INBOX_LIVE);
  g_assert_cmpuint(counters(f.inbox).backfill_incomplete, ==, 1);
  gh_test_spin_until(settled, f.inbox);
  g_assert_false(has_checkpoint(&f));
  fixture_down(&f);
}

/* ---- rejected wraps (nostrc-qp24.10.11) -------------------------------------- */

/* A wrap finally rejected after a signer call is recorded, so a restart
 * skips it before any signer call; one rejected before any signer call is
 * not recorded (rejecting it again is free). */
static void
test_rejected_not_reprompted(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  go_live(&f);
  Craft forged = { .author = 1, .seal_signer = 3, .to = 2, .p = { 2 }, .created_at = 10,
                   .content = "impostor" };
  Craft kind15 = { .author = 1, .to = 2, .p = { 2 }, .created_at = 11, .content = "file",
                   .kind = 15 };
  Craft outsider = { .author = 3, .to = 2, .p = { 1 }, .created_at = 12, .content = "cc" };
  Craft wrong_p = { .author = 3, .to = 1, .p = { 1 }, .created_at = 13, .content = "x" };
  g_autofree gchar *forged_wrap = craft_wrap(&forged, NULL);
  g_autofree gchar *kind15_wrap = craft_wrap(&kind15, NULL);
  g_autofree gchar *outsider_wrap = craft_wrap(&outsider, NULL);
  g_autofree gchar *wrong_p_wrap = craft_wrap(&wrong_p, NULL);
  const gchar *all[] = { forged_wrap, kind15_wrap, outsider_wrap, wrong_p_wrap };
  for (guint i = 0; i < G_N_ELEMENTS(all); i++)
    deliver(&f, INBOX_A, all[i]);
  gh_test_spin_until(settled, f.inbox);
  /* Sender mismatch, kind 15 and a rumor not addressed to the account each
   * cost two signer calls; the wrong outer p none. */
  g_assert_cmpuint(counters(f.inbox).rejected, ==, 4);
  g_assert_cmpuint(f.signer.calls, ==, 6);
  /* Kept on disk under the pseudonymous name, and nothing else is. */
  g_assert_cmpuint(seen_lines(&f, 2, "x "), ==, 3);
  assert_state_files(&f, 2);
  g_autofree gchar *path = rejected_file(&f, 2);
  GStatBuf st;
  g_assert_cmpint(g_stat(path, &st), ==, 0);
  g_assert_cmpint(st.st_mode & 0777, ==, 0600);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(f.store)), ==, 0);

  /* Restart: the rejected wraps are skipped before any signer call. */
  gh_test_release(f.inbox);
  g_object_unref(f.store);
  f.store = gh_conversation_store_new();
  f.inbox = new_inbox(&f);
  for (guint i = 0; i < G_N_ELEMENTS(all); i++)
    deliver(&f, INBOX_B, all[i]);
  gh_test_spin_until(settled, f.inbox);
  GhDmInboxCounters c = counters(f.inbox);
  g_assert_cmpuint(c.skipped, ==, 3);
  g_assert_cmpuint(c.rejected, ==, 1);
  g_assert_cmpuint(c.admitted, ==, 0);
  g_assert_cmpuint(f.signer.calls, ==, 6);
  fixture_down(&f);
}

/* ---- NIP-42 AUTH on own inbox relays (nostrc-qp24.10.12) ---------------------- */

static gchar *
verified_auth_id(Req *req, guint index, const gchar *url, const gchar *challenge)
{
  g_assert_cmpuint(index, <, req->auth->len);
  gchar id[65] = { 0 };
  g_autoptr(GError) error = NULL;
  /* Signed as the account itself, for exactly this relay and challenge. */
  g_assert_true(gh_relay_auth_verify_signed(g_ptr_array_index(req->auth, index), url,
                                            challenge, hex[2],
                                            g_get_real_time() / G_USEC_PER_SEC, id,
                                            &error));
  g_assert_no_error(error);
  return g_strdup(id);
}

static void
require_auth(Req *req, const gchar *challenge)
{
  gh_relay_scope_auth_challenge(req->scope, req->url, challenge);
  gh_relay_scope_notice(req->scope, req->url, GH_RELAY_NOTICE_CLOSED, NULL, FALSE,
                        "auth-required: sign in to read your messages");
}

typedef struct {
  Req *req;
  guint count;
} AuthWait;

static gboolean
auths_sent(gpointer data)
{
  AuthWait *wait = data;
  return wait->req->auth->len >= wait->count;
}

/* An own inbox relay that serves kind 1059 only to its signed-in owner gets
 * one AUTH signed by the account and serves the re-issued REQ; a declined
 * sign-in is explained per relay and not asked again this session. */
static void
test_auth_own_inbox(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  publish_list(&f, 2, 10002, HOME, NULL);
  publish_list(&f, 2, 10050, INBOX_A, INBOX_B, NULL);
  Req *a = open_req(&f.rec, INBOX_A);
  Req *b = open_req(&f.rec, INBOX_B);

  /* A challenges and refuses the REQ; the user is asked once. */
  f.signer.hold = TRUE;
  require_auth(a, "challenge-a");
  HeldWait one = { &f.signer, 1 };
  gh_test_spin_until(held_reached, &one);
  g_assert_cmpint(relay_state(f.inbox, INBOX_A, NULL), ==,
                  GH_DM_INBOX_RELAY_WAITING_FOR_APPROVAL);
  g_assert_cmpint(gh_dm_inbox_get_state(f.inbox), ==, GH_DM_INBOX_CONNECTING);
  gh_test_signer_release_one(&f.signer);
  f.signer.hold = FALSE;
  gh_test_spin_until(auth_sent, a);
  g_autofree gchar *id = verified_auth_id(a, 0, INBOX_A, "challenge-a");
  g_assert_cmpuint(a->resubscribes, ==, 0);
  gh_relay_scope_notice(a->scope, INBOX_A, GH_RELAY_NOTICE_OK, id, TRUE, "");
  g_assert_cmpuint(a->resubscribes, ==, 1);

  /* The re-issued REQ is served. */
  Craft craft = { .author = 1, .to = 2, .p = { 2 }, .created_at = 1000, .content = "behind auth" };
  g_autofree gchar *wrap = craft_wrap(&craft, NULL);
  deliver(&f, INBOX_A, wrap);
  eose(&f, INBOX_A);
  eose(&f, INBOX_B);
  gh_test_spin_until(live_and_settled, f.inbox);
  g_assert_cmpuint(counters(f.inbox).admitted, ==, 1);
  g_assert_cmpint(relay_state(f.inbox, INBOX_A, NULL), ==, GH_DM_INBOX_RELAY_LIVE);
  g_assert_cmpuint(f.signer.calls, ==, 3); /* one AUTH, two decrypts */

  /* B, after a reconnect, wants AUTH too; the user declines. */
  gh_relay_scope_notice(b->scope, INBOX_B, GH_RELAY_NOTICE_DISCONNECTED, NULL, FALSE, NULL);
  f.signer.deny = TRUE;
  require_auth(b, "challenge-b");
  RelayWait refused = { f.inbox, INBOX_B, GH_DM_INBOX_RELAY_AUTH_REQUIRED };
  gh_test_spin_until(relay_reached, &refused);
  const gchar *detail = NULL;
  relay_state(f.inbox, INBOX_B, &detail);
  g_assert_nonnull(strstr(detail, "declined"));
  g_assert_cmpuint(b->auth->len, ==, 0);
  g_assert_cmpuint(f.signer.calls, ==, 4);
  g_assert_cmpint(gh_dm_inbox_get_state(f.inbox), ==, GH_DM_INBOX_LIVE);

  /* Declined is remembered for the session: the next challenge on B fails
   * without asking the signer. */
  f.signer.deny = FALSE;
  gh_relay_scope_notice(b->scope, INBOX_B, GH_RELAY_NOTICE_DISCONNECTED, NULL, FALSE, NULL);
  require_auth(b, "challenge-b2");
  gh_test_spin_until(relay_reached, &refused);
  drain();
  g_assert_cmpuint(f.signer.calls, ==, 4);
  g_assert_cmpuint(b->auth->len, ==, 0);

  /* A, approved before, may sign again after its reconnect. */
  gh_relay_scope_notice(a->scope, INBOX_A, GH_RELAY_NOTICE_DISCONNECTED, NULL, FALSE, NULL);
  require_auth(a, "challenge-a2");
  AuthWait second = { a, 2 };
  gh_test_spin_until(auths_sent, &second);
  g_autofree gchar *id2 = verified_auth_id(a, 1, INBOX_A, "challenge-a2");
  g_assert_cmpuint(f.signer.calls, ==, 5);

  /* Nothing but the account's own 10050 relays was ever asked for DMs or
   * sent an AUTH (the recorder refuses AUTH anywhere else). */
  for (guint i = 0; i < f.rec.reqs->len; i++) {
    Req *req = g_ptr_array_index(f.rec.reqs, i);
    g_assert_true(g_str_equal(req->url, INBOX_A) || g_str_equal(req->url, INBOX_B));
  }
  fixture_down(&f);
}

typedef struct {
  gboolean done;
  gchar *signed_json;
  GError *error;
} AttemptResult;

static void
on_attempt(gpointer owner, const gchar *signed_json, const gchar *event_id,
           const GError *error)
{
  AttemptResult *result = owner;
  (void)event_id;
  result->done = TRUE;
  result->signed_json = g_strdup(signed_json);
  result->error = error ? g_error_copy(error) : NULL;
}

static gboolean
attempt_done(gpointer data)
{
  return ((AttemptResult *)data)->done;
}

static void
attempt_clear(AttemptResult *result)
{
  g_free(result->signed_json);
  g_clear_error(&result->error);
}

/* Approves the parked AUTH signature for challenge: each signer call opens
 * its own connection, so parked calls of different relays arrive in any
 * order. */
static void
release_challenge(GhTestSigner *mock, const gchar *challenge)
{
  for (guint i = 0; i < mock->held->len; i++) {
    GDBusMethodInvocation *invocation = g_ptr_array_index(mock->held, i);
    const gchar *event = NULL;
    g_variant_get(g_dbus_method_invocation_get_parameters(invocation), "(&sss)", &event,
                  NULL, NULL);
    if (!strstr(event, challenge))
      continue;
    g_object_ref(invocation);
    g_ptr_array_remove_index(mock->held, i);
    gh_test_signer_answer(mock, invocation);
    g_object_unref(invocation);
    return;
  }
  g_assert_not_reached();
}

static GhRelayAuthAttempt *
start_attempt(GhAccountAuth *auth, const gchar *url, const gchar *challenge,
              AttemptResult *result)
{
  g_autoptr(GError) error = NULL;
  GhRelayAuthAttempt *attempt = gh_relay_auth_attempt_start(
    GH_RELAY_AUTH_ACCOUNT, gh_account_auth_get_signer(auth), gh_account_auth_get_generation(auth),
    url, challenge, on_attempt, result, &error);
  g_assert_no_error(error);
  g_assert_nonnull(attempt);
  return attempt;
}

/* R6: one signer request per relay at a time; a denial fails the requests
 * waiting behind it and every later one without a signer call; relays are
 * independent; an account switch revokes the adapter. */
static void
test_auth_one_prompt_per_relay(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  g_autoptr(GhAccountAuth) auth = gh_account_auth_new(f.accounts);
  g_assert_nonnull(auth);
  AttemptResult a1 = { 0 }, a2 = { 0 }, b1 = { 0 }, a3 = { 0 };
  f.signer.hold = TRUE;
  start_attempt(auth, INBOX_A, "challenge-a1", &a1);
  start_attempt(auth, INBOX_A, "challenge-a2", &a2); /* another connection to A */
  start_attempt(auth, INBOX_B, "challenge-b1", &b1);
  HeldWait both = { &f.signer, 2 };
  gh_test_spin_until(held_reached, &both);
  drain();
  g_assert_cmpuint(f.signer.calls, ==, 2); /* A once, B once: A's second waits */
  g_assert_cmpint(gh_account_auth_get_relay_state(auth, INBOX_A), ==,
                  GH_ACCOUNT_AUTH_RELAY_WAITING);
  g_assert_cmpint(gh_account_auth_get_relay_state(auth, INBOX_C), ==,
                  GH_ACCOUNT_AUTH_RELAY_NONE);

  /* Approving A's first lets A's second ask. */
  release_challenge(&f.signer, "challenge-a1");
  gh_test_spin_until(attempt_done, &a1);
  g_assert_no_error(a1.error);
  g_assert_nonnull(a1.signed_json);
  HeldWait again = { &f.signer, 2 };
  gh_test_spin_until(held_reached, &again);
  g_assert_cmpuint(f.signer.calls, ==, 3);

  /* Both denied: A becomes DECLINED although it was approved before, and a
   * later challenge fails without a signer call. */
  f.signer.deny = TRUE;
  gh_test_signer_release_all(&f.signer);
  gh_test_spin_until(attempt_done, &a2);
  gh_test_spin_until(attempt_done, &b1);
  g_assert_error(a2.error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_DENIED);
  g_assert_error(b1.error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_DENIED);
  g_assert_cmpint(gh_account_auth_get_relay_state(auth, INBOX_A), ==,
                  GH_ACCOUNT_AUTH_RELAY_DECLINED);
  f.signer.deny = FALSE;
  start_attempt(auth, INBOX_A, "challenge-a3", &a3);
  gh_test_spin_until(attempt_done, &a3);
  g_assert_error(a3.error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_DENIED);
  g_assert_cmpuint(f.signer.calls, ==, 3);

  /* Requests waiting behind a denial share it. */
  AttemptResult c1 = { 0 }, c2 = { 0 };
  f.signer.hold = TRUE;
  start_attempt(auth, INBOX_C, "challenge-c1", &c1);
  start_attempt(auth, INBOX_C, "challenge-c2", &c2);
  HeldWait c_held = { &f.signer, 1 };
  gh_test_spin_until(held_reached, &c_held);
  drain();
  f.signer.deny = TRUE;
  gh_test_signer_release_all(&f.signer);
  gh_test_spin_until(attempt_done, &c1);
  gh_test_spin_until(attempt_done, &c2);
  g_assert_error(c1.error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_DENIED);
  g_assert_error(c2.error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_DENIED);
  g_assert_cmpuint(f.signer.calls, ==, 4); /* c2 never asked */
  f.signer.deny = FALSE;

  /* An account switch revokes the adapter: a pending request fails, and the
   * signer of the old generation signs nothing more. */
  AttemptResult d1 = { 0 };
  start_attempt(auth, "wss://inbox-d.test.invalid", "challenge-d1", &d1);
  HeldWait d_held = { &f.signer, 1 };
  gh_test_spin_until(held_reached, &d_held);
  g_assert_true(gh_account_controller_select(f.accounts, npub[1], NULL));
  gh_test_spin_until(attempt_done, &d1);
  g_assert_nonnull(d1.error);
  g_assert_null(d1.signed_json);
  g_assert_true(gh_relay_auth_signer_is_revoked(gh_account_auth_get_signer(auth)));
  g_autoptr(GError) error = NULL;
  g_assert_null(gh_relay_auth_attempt_start(GH_RELAY_AUTH_ACCOUNT,
    gh_account_auth_get_signer(auth), gh_account_auth_get_generation(auth), INBOX_A, "late",
    on_attempt, &d1, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  f.signer.hold = FALSE;
  gh_test_signer_release_all(&f.signer);
  drain();
  g_assert_cmpuint(f.signer.calls, ==, 5);
  AttemptResult *all[] = { &a1, &a2, &b1, &a3, &c1, &c2, &d1 };
  for (guint i = 0; i < G_N_ELEMENTS(all); i++)
    attempt_clear(all[i]);
  g_clear_object(&auth);
  fixture_down(&f);
}

static void
test_unusable_seen_set(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  gh_test_release(f.inbox);
  g_autofree gchar *path = rejected_file(&f, 2);
  g_assert_true(g_file_set_contents(path, "not a seen-set\n", -1, NULL));
  f.inbox = new_inbox(&f);
  publish_list(&f, 2, 10050, INBOX_A, NULL);
  /* Fail closed: no REQ that would turn every replay into a prompt. */
  g_assert_cmpint(gh_dm_inbox_get_state(f.inbox), ==, GH_DM_INBOX_ERROR);
  g_assert_nonnull(gh_dm_inbox_get_error(f.inbox));
  g_assert_cmpuint(f.rec.reqs->len, ==, 0);
  fixture_down(&f);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  for (guint key = 1; key < GH_TEST_KEYS; key++) {
    npub[key] = gh_test_npub(key);
    hex[key] = gh_test_pub(key);
  }
  g_test_add_func("/groundhog/dm-inbox/req-exact", test_req_exact);
  g_test_add_func("/groundhog/dm-inbox/rooms-and-dedup", test_rooms_and_dedup);
  g_test_add_func("/groundhog/dm-inbox/seen-restart", test_seen_restart);
  g_test_add_func("/groundhog/dm-inbox/legacy-state-files", test_legacy_state_files);
  g_test_add_func("/groundhog/dm-inbox/bounded-concurrency", test_bounded_concurrency);
  g_test_add_func("/groundhog/dm-inbox/switch-mid-unwrap", test_switch_mid_unwrap);
  g_test_add_func("/groundhog/dm-inbox/relay-change-reopens", test_relay_change_reopens);
  g_test_add_func("/groundhog/dm-inbox/signer-denied-defers", test_signer_denied_defers);
  g_test_add_func("/groundhog/dm-inbox/unusable-seen-set", test_unusable_seen_set);
  g_test_add_func("/groundhog/dm-inbox/backfill-paging", test_backfill_paging);
  g_test_add_func("/groundhog/dm-inbox/backfill-page-bound", test_backfill_page_bound);
  g_test_add_func("/groundhog/dm-inbox/backfill-tied-second", test_backfill_tied_second);
  g_test_add_func("/groundhog/dm-inbox/rejected-not-reprompted", test_rejected_not_reprompted);
  g_test_add_func("/groundhog/dm-inbox/auth-own-inbox", test_auth_own_inbox);
  g_test_add_func("/groundhog/dm-inbox/auth-one-prompt-per-relay",
                  test_auth_one_prompt_per_relay);
  int status = g_test_run();
  for (guint key = 1; key < GH_TEST_KEYS; key++) {
    g_free(npub[key]);
    g_free(hex[key]);
  }
  return status;
}
