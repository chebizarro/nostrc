/* GhDmInbox against recording relay transports and a mock signer on a
 * private bus. Every relay frame is injected through the scopes the inbox
 * opened; nothing sleeps. */
#include "gh-dm-inbox.h"
#include "gh-test-signer.h"

#include "nostr-tag.h"
#include "nostr/nip59/nip59.h"

#include <glib/gstdio.h>

#define DISCOVERY "wss://discovery.test.invalid"
#define INBOX_A "wss://inbox-a.test.invalid"
#define INBOX_B "wss://inbox-b.test.invalid"
#define INBOX_C "wss://inbox-c.test.invalid"

static gchar *npub[GH_TEST_KEYS];
static gchar *hex[GH_TEST_KEYS];

/* ---- recording transport --------------------------------------------------- */

typedef struct {
  GhRelayScope *scope;
  gchar *url;
  gchar *p;
  gint64 since;
  int limit;
  gboolean closed;
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
  g_free(req->url);
  g_free(req->p);
  g_free(req);
}

/* Asserts the exact DM inbox filter: kinds [1059], one #p, since, limit, and
 * nothing else. */
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
  g_assert_cmpint(nostr_filter_get_until_i64(filter), ==, 0);
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
  req->limit = nostr_filter_get_limit(filter);
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

static Req *
open_req(Recorder *rec, const gchar *url)
{
  Req *found = NULL;
  for (guint i = 0; i < rec->reqs->len; i++) {
    Req *req = g_ptr_array_index(rec->reqs, i);
    if (!req->closed && g_str_equal(req->url, url)) {
      g_assert_null(found);
      found = req;
    }
  }
  g_assert_nonnull(found);
  return found;
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
                                     &recorder_transport, &f->rec);
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
} Craft;

static gchar *
craft_wrap(const Craft *c, gchar **rumor_id)
{
  guint signer = c->seal_signer ? c->seal_signer : c->author;
  NostrEvent *rumor = nostr_event_new();
  nostr_event_set_kind(rumor, 14);
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
  char *json = nostr_event_serialize_compact(wrap);
  nostr_event_free(wrap);
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
  g_assert_cmpint(gh_dm_inbox_get_state(f->inbox), ==, GH_DM_INBOX_LIVE);
}

/* ---- tests -------------------------------------------------------------------- */

static gint64
read_checkpoint(Fixture *f, guint key)
{
  g_autofree gchar *name = g_strconcat(hex[key], ".checkpoint", NULL);
  g_autofree gchar *path = g_build_filename(f->state_dir, name, NULL);
  g_autofree gchar *contents = NULL;
  g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
  g_auto(GStrv) fields = g_strsplit(g_strstrip(contents), " ", -1);
  g_assert_cmpuint(g_strv_length(fields), ==, 4);
  g_assert_cmpstr(fields[0], ==, "groundhog-dm-inbox-checkpoint");
  g_assert_cmpstr(fields[2], ==, hex[key]);
  return g_ascii_strtoll(fields[3], NULL, 10);
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
  eose(&f, INBOX_A);
  g_assert_cmpint(gh_dm_inbox_get_state(f.inbox), ==, GH_DM_INBOX_BACKFILLING);
  eose(&f, INBOX_B);
  g_assert_cmpint(gh_dm_inbox_get_state(f.inbox), ==, GH_DM_INBOX_LIVE);

  /* Settled and live: the checkpoint is recorded, and the next session asks
   * for since = checkpoint - two days (plus slack) to cover randomized
   * NIP-59 timestamps. */
  gint64 checkpoint = read_checkpoint(&f, 2);
  g_assert_cmpint(checkpoint, >=, after);
  g_assert_cmpint(checkpoint, <=, g_get_real_time() / G_USEC_PER_SEC);
  gh_test_release(f.inbox);
  g_assert_cmpuint(f.rec.reqs->len, ==, 2);
  g_autoptr(GPtrArray) none = open_reqs(&f.rec);
  g_assert_cmpuint(none->len, ==, 0); /* disposal closed both */
  f.inbox = new_inbox(&f);
  g_autoptr(GPtrArray) reopened = open_reqs(&f.rec);
  g_assert_cmpuint(reopened->len, ==, 2);
  for (guint i = 0; i < reopened->len; i++)
    g_assert_cmpint(((Req *)g_ptr_array_index(reopened, i))->since, ==,
                    checkpoint - GH_DM_INBOX_WRAP_SKEW);

  /* All inbox relays failing is an error state, not "live". */
  gh_relay_scope_notice(open_req(&f.rec, INBOX_A)->scope, INBOX_A,
                        GH_RELAY_NOTICE_CLOSED, NULL, FALSE, "auth-required: test");
  gh_relay_scope_notice(open_req(&f.rec, INBOX_B)->scope, INBOX_B,
                        GH_RELAY_NOTICE_ERROR, NULL, FALSE, "refused");
  g_assert_cmpint(gh_dm_inbox_get_state(f.inbox), ==, GH_DM_INBOX_ERROR);
  g_assert_nonnull(gh_dm_inbox_get_error(f.inbox));
  g_assert_cmpuint(f.signer.calls, ==, 0);
  fixture_down(&f);
}

static void
test_rooms_and_dedup(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  go_live(&f);

  /* 1 -> 2, delivered by both inbox relays: one scope event, one unwrap. */
  g_autofree gchar *first_id = NULL;
  Craft first = { .author = 1, .to = 2, .p = { 2 }, .created_at = 1000, .content = "hi" };
  g_autofree gchar *first_wrap = craft_wrap(&first, &first_id);
  deliver(&f, INBOX_A, first_wrap);
  deliver(&f, INBOX_B, first_wrap);
  gh_test_spin_until(settled, f.inbox);
  g_assert_cmpuint(counters(f.inbox).admitted, ==, 1);
  g_assert_cmpuint(counters(f.inbox).received, ==, 1);
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
  g_assert_cmpuint(c.skipped, ==, 0);
  g_assert_cmpuint(c.received, ==, 7);

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

  /* Restart: a fresh inbox and store over the same state directory. The
   * persisted seen-set skips both wraps before any signer call. */
  gh_test_release(f.inbox);
  g_object_unref(f.store);
  f.store = gh_conversation_store_new();
  f.inbox = new_inbox(&f);
  g_assert_cmpint(gh_dm_inbox_get_state(f.inbox), ==, GH_DM_INBOX_CONNECTING);
  deliver(&f, INBOX_B, one_wrap);
  deliver(&f, INBOX_A, two_wrap);
  gh_test_spin_until(settled, f.inbox);
  GhDmInboxCounters c = counters(f.inbox);
  g_assert_cmpuint(c.skipped, ==, 2);
  g_assert_cmpuint(c.admitted, ==, 0);
  g_assert_cmpuint(f.signer.calls, ==, 4);
  /* The in-memory store does not survive a restart (see the store seam). */
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(f.store)), ==, 0);

  /* A new message is still unwrapped and admitted. */
  Craft three = { .author = 1, .to = 2, .p = { 2 }, .created_at = 1002, .content = "three" };
  g_autofree gchar *three_wrap = craft_wrap(&three, NULL);
  deliver(&f, INBOX_A, three_wrap);
  gh_test_spin_until(settled, f.inbox);
  g_assert_cmpuint(counters(f.inbox).admitted, ==, 1);
  g_assert_cmpuint(f.signer.calls, ==, 6);
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
  gint64 checkpoint = read_checkpoint(&f, 2);
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
  gint64 checkpoint = read_checkpoint(&f, 2);
  f.signer.deny = TRUE;
  Craft craft = { .author = 1, .to = 2, .p = { 2 }, .created_at = 1000, .content = "no" };
  g_autofree gchar *wrap = craft_wrap(&craft, NULL);
  deliver(&f, INBOX_A, wrap);
  gh_test_spin_until(settled, f.inbox);
  GhDmInboxCounters c = counters(f.inbox);
  g_assert_cmpuint(c.deferred, ==, 1);
  g_assert_cmpuint(c.rejected, ==, 0);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(f.store)), ==, 0);
  /* A denied wrap is not seen: a later session offers it again, from a
   * checkpoint that did not move past it. */
  f.signer.deny = FALSE;
  gh_test_release(f.inbox);
  f.inbox = new_inbox(&f);
  g_assert_cmpint(gh_dm_inbox_get_since(f.inbox), ==, checkpoint - GH_DM_INBOX_WRAP_SKEW);
  deliver(&f, INBOX_A, wrap);
  gh_test_spin_until(settled, f.inbox);
  g_assert_cmpuint(counters(f.inbox).admitted, ==, 1);
  fixture_down(&f);
}

static void
test_unusable_seen_set(void)
{
  Fixture f = { 0 };
  fixture_up(&f);
  gh_test_release(f.inbox);
  g_autofree gchar *name = g_strconcat(hex[2], ".seen", NULL);
  g_autofree gchar *path = g_build_filename(f.state_dir, name, NULL);
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
  g_test_add_func("/groundhog/dm-inbox/bounded-concurrency", test_bounded_concurrency);
  g_test_add_func("/groundhog/dm-inbox/switch-mid-unwrap", test_switch_mid_unwrap);
  g_test_add_func("/groundhog/dm-inbox/relay-change-reopens", test_relay_change_reopens);
  g_test_add_func("/groundhog/dm-inbox/signer-denied-defers", test_signer_denied_defers);
  g_test_add_func("/groundhog/dm-inbox/unusable-seen-set", test_unusable_seen_set);
  int status = g_test_run();
  for (guint key = 1; key < GH_TEST_KEYS; key++) {
    g_free(npub[key]);
    g_free(hex[key]);
  }
  return status;
}
