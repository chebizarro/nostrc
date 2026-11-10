/* NIP-58 badges (plan §3.12, nostrc-9lwl7.13) over the real signer, SQLCipher
 * store, Groundhog adapter and list service; relay frames are deterministic
 * fakes and no helper substitutes for OK or EOSE. Covers the validation
 * chain against forged awards, unmatched pairs, relay hints needing explicit
 * approval, no default artwork fetch, explicit public publication, legacy
 * 30008 to canonical 10008, an empty canonical list winning, and the
 * offline cache. */
#include "gh-badge-service.h"
#include "gh-web-content.h"
#include "gh-identity.h"
#include "gh-relay-deny-policy.h"
#include "gh-test-signer.h"
#include <glib/gstdio.h>
#include <nostr-kinds.h>
#include <nostr-tag.h>
#include <nostr-filter.h>
#include <string.h>

static GhTestBus bus;
static gchar *account, *npub, *peer, *issuer, *other;
#define DISCOVERY "wss://discovery.test.invalid"
#define HINT "wss://hint.test.invalid"
static const gchar *writes[] = { "wss://write-a.test.invalid", "wss://write-b.test.invalid", "wss://write-c.test.invalid", NULL };
typedef struct { GhRelayScope *scope; gchar *url; gchar *filters; gint kind; gboolean closed; } Read;
typedef struct { GhRelayPublish *publish; gchar *url, *json, *id; gint kind; gboolean closed; } Write;
typedef struct {
  GhTestSigner signer;
  GSettings *settings;
  GhAccountController *accounts;
  GhAccountRelays *relays;
  GhSocialLists *social;
  GhBadgeService *service;
  GhStore *store;
  gchar *directory;
  GPtrArray *reads, *sent;
  guint http_calls;
  gchar *http_url;
} Fixture;
static void drain(void) { while (g_main_context_iteration(NULL, FALSE)); }
static void remove_tree(const gchar *path)
{
  if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
    GDir *dir = g_dir_open(path, 0, NULL);
    const gchar *name;
    while (dir && (name = g_dir_read_name(dir))) {
      g_autofree gchar *child = g_build_filename(path, name, NULL);
      remove_tree(child);
    }
    if (dir) g_dir_close(dir);
    g_rmdir(path);
  } else g_unlink(path);
}
static void read_free(gpointer p) { Read *r = p; g_free(r->url); g_free(r->filters); g_free(r); }
static void write_free(gpointer p) { Write *w = p; g_free(w->url); g_free(w->json); g_free(w->id); g_free(w); }
static gpointer read_open(GhRelayScope *scope, const gchar *url, const NostrFilters *filters, gpointer data, GError **error)
{
  (void)error;
  Fixture *f = data;
  Read *r = g_new0(Read, 1);
  r->scope = scope; r->url = g_strdup(url);
  r->kind = filters->count && filters->filters[0].kinds.size ? filters->filters[0].kinds.data[0] : 0;
  g_autoptr(GString) all = g_string_new(NULL);
  for (size_t i = 0; i < filters->count; i++) {
    char *raw = nostr_filter_serialize_compact(&filters->filters[i]);
    g_string_append(all, raw); g_string_append_c(all, '\n'); free(raw);
  }
  r->filters = g_strdup(all->str);
  g_ptr_array_add(f->reads, r);
  return r;
}
static void read_close(gpointer handle, gpointer data) { (void)data; ((Read *)handle)->closed = TRUE; }
static gpointer write_open(GhRelayPublish *publish, const gchar *url, const gchar *json, gpointer data, GError **error)
{
  (void)error;
  Fixture *f = data;
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_signed(event, json, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  Write *w = g_new0(Write, 1);
  w->publish = publish; w->url = g_strdup(url); w->json = g_strdup(json);
  w->id = nostr_event_get_id(event); w->kind = nostr_event_get_kind(event);
  nostr_event_free(event);
  g_ptr_array_add(f->sent, w);
  return w;
}
static void write_close(gpointer handle, gpointer data) { (void)data; ((Write *)handle)->closed = TRUE; }
static const GhRelayTransport read_transport = { read_open, read_close };
static const GhRelayPublishTransport write_transport = { write_open, write_close };
static void http_get_async(gpointer data, const gchar *uri, gsize max_bytes, GCancellable *cancel,
                           GAsyncReadyCallback callback, gpointer user_data)
{
  (void)max_bytes;
  Fixture *f = data; f->http_calls++; g_free(f->http_url); f->http_url = g_strdup(uri);
  GTask *task = g_task_new(NULL, cancel, callback, user_data);
  g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "fixture: no artwork served");
  g_object_unref(task);
}
static GBytes *http_get_finish(gpointer data, GAsyncResult *result, GError **error)
{ (void)data; return g_task_propagate_pointer(G_TASK(result), error); }
static const GhHttpTransport http_transport = { http_get_async, http_get_finish };

/* A signed event of key (1 account, 2 peer, 3 issuer, 4 other). */
static gchar *event_json(guint key, gint kind, gint64 at, NostrTags *tags, const gchar *content)
{
  NostrEvent *event = nostr_event_new();
  g_autofree gchar *pubkey = gh_test_pub(key);
  nostr_event_set_kind(event, kind); nostr_event_set_created_at(event, at);
  nostr_event_set_pubkey(event, pubkey); nostr_event_set_tags(event, tags);
  nostr_event_set_content(event, content ? content : "");
  g_assert_cmpint(nostr_event_sign(event, gh_test_secret[key]), ==, 0);
  char *raw = nostr_event_serialize_compact(event);
  gchar *json = g_strdup(raw); free(raw); nostr_event_free(event);
  return json;
}
static gchar *id_of(const gchar *json)
{
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_signed(event, json, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  gchar *id = nostr_event_get_id(event);
  nostr_event_free(event);
  return id;
}
static gchar *address(const gchar *author, const gchar *identifier)
{ return g_strdup_printf("30009:%s:%s", author, identifier); }
/* A definition by key with name. */
static gchar *definition(guint key, const gchar *identifier, const gchar *name, const gchar *image)
{
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("d", identifier, NULL));
  nostr_tags_append(tags, nostr_tag_new("name", name, NULL));
  if (image) nostr_tags_append(tags, nostr_tag_new("image", image, "256x256", NULL));
  return event_json(key, NOSTR_KIND_BADGE_DEFINITION, 1000, tags, "");
}
/* An award by key of the address to recipient. */
static gchar *award(guint key, const gchar *addr, const gchar *recipient)
{
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("a", addr, NULL));
  nostr_tags_append(tags, nostr_tag_new("p", recipient, NULL));
  return event_json(key, NOSTR_KIND_BADGE_AWARD, 1001, tags, "");
}
static GPtrArray *identities(gpointer data, GError **error)
{
  (void)data; (void)error;
  GPtrArray *result = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  GhIdentityInfo *id = g_new0(GhIdentityInfo, 1); id->npub = g_strdup(npub);
  g_ptr_array_add(result, id);
  return result;
}
static gboolean active(gpointer data) { return gh_account_controller_get_state(data) == GH_ACCOUNT_STATE_ACTIVE; }
static gboolean discovered(gpointer data) { return gh_account_relays_get_state(data) == GH_ACCOUNT_RELAYS_COMPLETE; }
static Read *read_for(Fixture *f, gint kind, const gchar *url)
{
  for (guint i = f->reads->len; i > 0; i--) {
    Read *r = f->reads->pdata[i - 1];
    if (!r->closed && r->kind == kind && !g_strcmp0(r->url, url)) return r;
  }
  g_error("Missing read for kind %d", kind);
  return NULL;
}
static guint reads_for_url(Fixture *f, const gchar *url)
{
  guint n = 0;
  for (guint i = 0; i < f->reads->len; i++) n += !g_strcmp0(((Read *)f->reads->pdata[i])->url, url);
  return n;
}
static void bind(Fixture *f)
{
  f->social = gh_social_lists_new(NULL);
  GhListAdapterNetwork network = { .accounts = f->accounts, .scope_transport = &read_transport,
    .scope_data = f, .publish_transport = &write_transport, .publish_data = f, .publish_deadline = 10 };
  gh_social_lists_set_network(f->social, &network, f->relays);
  g_autoptr(GError) error = NULL;
  guint64 generation = gh_account_controller_get_generation(f->accounts);
  g_assert_true(gh_social_lists_bind_store(f->social, f->store, generation, &error));
  g_assert_no_error(error);
  f->service = gh_badge_service_new(f->social, NULL, &network, f->settings);
  gh_badge_service_set_http_transport(f->service, &http_transport, f);
  gh_badge_service_bind(f->service, f->store, generation);
  drain();
}
static void open_store(Fixture *f)
{
  guint8 key[GH_STORE_KEY_SIZE] = { 1 };
  g_autoptr(GBytes) bytes = g_bytes_new(key, sizeof key);
  GhStoreConfig config = { f->directory, account, NULL, NULL, NULL };
  g_autoptr(GError) error = NULL;
  f->store = gh_store_open_with_key(&config, bytes, "9f1cbb0e-7e0a-4a35-9a0e-2a4e0d7e58b1", GH_STORE_OPEN_CREATE, &error);
  g_assert_no_error(error);
}
static void setup(Fixture *f)
{
  memset(f, 0, sizeof *f);
  f->reads = g_ptr_array_new_with_free_func(read_free); f->sent = g_ptr_array_new_with_free_func(write_free);
  gh_test_signer_up(&bus, &f->signer);
  f->settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(f->settings, "signer-method", "auto");
  g_settings_set_string(f->settings, "current-npub", npub);
  const gchar *discovery[] = { DISCOVERY, NULL };
  g_settings_set_strv(f->settings, "discovery-relays", discovery);
  f->accounts = gh_account_controller_new_full(f->settings, bus.client, identities, NULL);
  gh_test_spin_until(active, f->accounts);
  f->relays = gh_account_relays_new(f->accounts, f->settings, &read_transport, f);
  NostrTags *tags = nostr_tags_new(0);
  for (guint i = 0; writes[i]; i++) nostr_tags_append(tags, nostr_tag_new("r", writes[i], "write", NULL));
  g_autofree gchar *list = event_json(1, NOSTR_KIND_RELAY_LIST_METADATA, g_get_real_time()/G_USEC_PER_SEC - 60, tags, "");
  Read *disc = read_for(f, NOSTR_KIND_RELAY_LIST_METADATA, DISCOVERY);
  gh_relay_scope_event(disc->scope, disc->url, list); gh_relay_scope_eose(disc->scope, disc->url);
  gh_test_spin_until(discovered, f->relays);
  f->directory = g_dir_make_tmp("groundhog-badges-XXXXXX", NULL);
  open_store(f); bind(f);
}
static void teardown(Fixture *f)
{
  gh_badge_service_close(f->service); gh_test_release(g_steal_pointer(&f->service));
  gh_social_lists_close(f->social); gh_test_release(g_steal_pointer(&f->social));
  drain(); if (f->store) gh_store_close(f->store);
  gh_test_release(g_steal_pointer(&f->relays)); gh_test_release(g_steal_pointer(&f->accounts));
  GhTestSenders senders = { &bus, &f->signer };
  gh_test_spin_until(gh_test_signer_senders_closed, &senders);
  gh_test_signer_down(&bus, &f->signer);
  g_object_unref(f->settings); g_ptr_array_unref(f->reads); g_ptr_array_unref(f->sent);
  g_free(f->http_url);
  remove_tree(f->directory); g_free(f->directory);
}

/* ---- driving a lookup ---------------------------------------------------------- */

typedef struct { gboolean done, ok; GError *error; } Done;
static void lookup_done(GObject *source, GAsyncResult *result, gpointer data)
{
  Done *d = data;
  d->ok = gh_badge_service_lookup_finish(GH_BADGE_SERVICE(source), result, &d->error); d->done = TRUE;
}
static gboolean finished(gpointer data) { return ((Done *)data)->done; }
typedef struct { Fixture *f; guint count; } ReadCount;
static gboolean reads_reached(gpointer data) { ReadCount *c = data; return c->f->reads->len >= c->count; }
/* The lookup's first REQ on url (the lists), or the second (awards and
 * definitions): the latest unclosed read of kind. */
static Read *phase_read(Fixture *f, const gchar *url, gboolean second)
{
  return read_for(f, second ? 0 : NOSTR_KIND_PROFILE_BADGE_LIST, url);
}
/* A full lookup: lists on the first REQ, chain events on the second. */
static void run_lookup(Fixture *f, const gchar *pubkey, const gchar *const *approved,
                       GPtrArray *lists, GPtrArray *chain, Done *d)
{
  memset(d, 0, sizeof *d);
  guint before = f->reads->len;
  gh_badge_service_lookup_async(f->service, pubkey, approved, NULL, lookup_done, d);
  ReadCount first = { f, before + 1 };
  gh_test_spin_until(reads_reached, &first);
  Read *r = phase_read(f, DISCOVERY, FALSE);
  for (guint i = 0; lists && i < lists->len; i++) gh_relay_scope_event(r->scope, r->url, lists->pdata[i]);
  gh_relay_scope_eose(r->scope, r->url);
  if (approved && approved[0]) {
    Read *h = phase_read(f, approved[0], FALSE);
    gh_relay_scope_eose(h->scope, h->url);
  }
  if (!chain) { gh_test_spin_until(finished, d); return; }
  guint second_count = before + 1 + (approved && approved[0] ? 2 : 1);
  ReadCount second = { f, second_count };
  gh_test_spin_until(reads_reached, &second);
  Read *s = phase_read(f, DISCOVERY, TRUE);
  for (guint i = 0; i < chain->len; i++) gh_relay_scope_event(s->scope, s->url, chain->pdata[i]);
  gh_relay_scope_eose(s->scope, s->url);
  if (approved && approved[0]) {
    Read *h = phase_read(f, approved[0], TRUE);
    gh_relay_scope_eose(h->scope, h->url);
  }
  gh_test_spin_until(finished, d);
}
static GPtrArray *strings(void) { return g_ptr_array_new_with_free_func(g_free); }
static void pair(NostrTags *tags, const gchar *addr, const gchar *award_id, const gchar *relay)
{
  nostr_tags_append(tags, nostr_tag_new("a", addr, NULL));
  nostr_tags_append(tags, relay ? nostr_tag_new("e", award_id, relay, NULL) : nostr_tag_new("e", award_id, NULL));
}

/* ---- forged chains ---------------------------------------------------------------- */

/* Five pairs on the peer's canonical list: one whose chain holds, one whose
 * award another key signed, one whose award does not name the peer, one
 * whose award names a different definition, and one whose definition is not
 * by the issuer in the address. Only the first shows (§3.12 steps 4-9). */
static void test_forged_chains(void)
{
  Fixture f; setup(&f);
  g_autofree gchar *gold = address(issuer, "gold"), *silver = address(issuer, "silver");
  g_autofree gchar *bronze = address(issuer, "bronze"), *iron = address(issuer, "iron");
  g_autofree gchar *steel = address(issuer, "steel"), *copper = address(other, "copper");
  g_autoptr(GPtrArray) chain = strings();
  g_ptr_array_add(chain, definition(3, "gold", "Gold Star", NULL));
  g_ptr_array_add(chain, definition(3, "silver", "Silver", NULL));
  g_ptr_array_add(chain, definition(3, "bronze", "Bronze", NULL));
  g_ptr_array_add(chain, definition(3, "iron", "Iron", NULL));
  g_ptr_array_add(chain, definition(3, "steel", "Steel", NULL));
  g_ptr_array_add(chain, definition(3, "copper", "Copper by the wrong author", NULL));
  gchar *valid = award(3, gold, peer), *forged_author = award(4, silver, peer);
  gchar *wrong_recipient = award(3, bronze, other), *wrong_address = award(3, steel, peer);
  gchar *forged_definition = award(4, copper, peer);
  g_autofree gchar *valid_id = id_of(valid), *forged_author_id = id_of(forged_author);
  g_autofree gchar *wrong_recipient_id = id_of(wrong_recipient), *wrong_address_id = id_of(wrong_address);
  g_autofree gchar *forged_definition_id = id_of(forged_definition);
  g_ptr_array_add(chain, valid); g_ptr_array_add(chain, forged_author); g_ptr_array_add(chain, wrong_recipient);
  g_ptr_array_add(chain, wrong_address); g_ptr_array_add(chain, forged_definition);
  NostrTags *tags = nostr_tags_new(0);
  pair(tags, gold, valid_id, NULL); pair(tags, silver, forged_author_id, NULL);
  pair(tags, bronze, wrong_recipient_id, NULL); pair(tags, iron, wrong_address_id, NULL);
  pair(tags, copper, forged_definition_id, NULL);
  g_autoptr(GPtrArray) lists = strings();
  g_ptr_array_add(lists, event_json(2, NOSTR_KIND_PROFILE_BADGE_LIST, 2000, tags, ""));
  g_assert_cmpint(gh_badge_service_get_list_kind(f.service, peer), ==, GH_BADGE_LIST_UNKNOWN);
  g_assert_null(gh_badge_service_get_badges(f.service, peer));
  Done d; run_lookup(&f, peer, NULL, lists, chain, &d);
  g_assert_no_error(d.error); g_assert_true(d.ok);
  g_autoptr(GPtrArray) badges = gh_badge_service_get_badges(f.service, peer);
  g_assert_nonnull(badges); g_assert_cmpuint(badges->len, ==, 1);
  GhBadge *badge = badges->pdata[0];
  g_assert_cmpstr(badge->name, ==, "Gold Star"); g_assert_cmpstr(badge->issuer, ==, issuer);
  g_assert_cmpstr(badge->award_id, ==, valid_id); g_assert_cmpstr(badge->address, ==, gold);
  g_assert_cmpint(gh_badge_service_get_list_kind(f.service, peer), ==, GH_BADGE_LIST_CANONICAL);
  /* The second REQ asked for the awards by exact id and the definitions by
   * author, kind and d; only the discovery relay was contacted. */
  Read *second = NULL;
  for (guint i = 0; i < f.reads->len; i++) {
    Read *r = f.reads->pdata[i];
    g_assert_cmpstr(r->url, ==, DISCOVERY);
    if (strstr(r->filters, valid_id)) second = r;
  }
  g_assert_nonnull(second);
  g_assert_nonnull(strstr(second->filters, "\"#d\""));
  g_assert_nonnull(strstr(second->filters, "30009"));
  g_assert_nonnull(strstr(second->filters, issuer));
  g_assert_cmpuint(f.http_calls, ==, 0);
  /* Offline: a fresh service over the same store shows the cached badge
   * without a request; the forged chains stay out. */
  guint reads = f.reads->len;
  GhListAdapterNetwork network = { .accounts = f.accounts, .scope_transport = &read_transport, .scope_data = &f };
  GhBadgeService *again = gh_badge_service_new(f.social, NULL, &network, f.settings);
  gh_badge_service_bind(again, f.store, gh_account_controller_get_generation(f.accounts));
  g_autoptr(GPtrArray) cached = gh_badge_service_get_badges(again, peer);
  g_assert_nonnull(cached); g_assert_cmpuint(cached->len, ==, 1);
  g_assert_cmpstr(((GhBadge *)cached->pdata[0])->name, ==, "Gold Star");
  g_assert_cmpint(gh_badge_service_get_list_kind(again, peer), ==, GH_BADGE_LIST_CANONICAL);
  g_assert_cmpuint(f.reads->len, ==, reads);
  gh_badge_service_close(again); g_object_unref(again);
  /* Relays that close or fail instead of answering: a failed lookup, and
   * the cached answer stands. */
  Done failed = { 0 };
  gh_badge_service_lookup_async(f.service, peer, NULL, NULL, lookup_done, &failed);
  ReadCount next = { &f, reads + 1 };
  gh_test_spin_until(reads_reached, &next);
  Read *r = phase_read(&f, DISCOVERY, FALSE);
  gh_relay_scope_notice(r->scope, r->url, GH_RELAY_NOTICE_CLOSED, NULL, FALSE, "error: fixture");
  gh_test_spin_until(finished, &failed);
  g_assert_error(failed.error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE); g_clear_error(&failed.error);
  g_autoptr(GPtrArray) kept = gh_badge_service_get_badges(f.service, peer);
  g_assert_nonnull(kept); g_assert_cmpuint(kept->len, ==, 1);
  g_assert_true(((GhBadge *)kept->pdata[0])->verified);
  teardown(&f);
}

/* ---- unmatched pairs ---------------------------------------------------------------- */

static void test_unmatched_pairs(void)
{
  Fixture f; setup(&f);
  g_autofree gchar *gold = address(issuer, "gold"), *silver = address(issuer, "silver");
  g_autoptr(GPtrArray) chain = strings();
  g_ptr_array_add(chain, definition(3, "gold", "Gold", NULL));
  g_ptr_array_add(chain, definition(3, "silver", "Silver", NULL));
  gchar *gold_award = award(3, gold, peer), *silver_award = award(3, silver, peer);
  g_autofree gchar *gold_id = id_of(gold_award), *silver_id = id_of(silver_award);
  g_ptr_array_add(chain, gold_award); g_ptr_array_add(chain, silver_award);
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("e", silver_id, NULL));             /* lone e */
  nostr_tags_append(tags, nostr_tag_new("a", silver, NULL));                /* a, then metadata, then e: no pair */
  nostr_tags_append(tags, nostr_tag_new("title", "between", NULL));
  nostr_tags_append(tags, nostr_tag_new("e", silver_id, NULL));
  nostr_tags_append(tags, nostr_tag_new("a", "30008:" , NULL));              /* malformed set reference */
  pair(tags, gold, gold_id, NULL);                                           /* the one pair */
  nostr_tags_append(tags, nostr_tag_new("a", silver, NULL));                /* trailing a */
  g_autoptr(GPtrArray) lists = strings();
  g_ptr_array_add(lists, event_json(2, NOSTR_KIND_PROFILE_BADGE_LIST, 2000, tags, ""));
  Done d; run_lookup(&f, peer, NULL, lists, chain, &d);
  g_assert_no_error(d.error);
  g_autoptr(GPtrArray) badges = gh_badge_service_get_badges(f.service, peer);
  g_assert_cmpuint(badges->len, ==, 1);
  g_assert_cmpstr(((GhBadge *)badges->pdata[0])->name, ==, "Gold");
  teardown(&f);
}

/* ---- relay hints need approval --------------------------------------------------------- */

static void test_hints_need_approval(void)
{
  Fixture f; setup(&f);
  g_autofree gchar *gold = address(issuer, "gold");
  g_autoptr(GPtrArray) chain = strings();
  g_ptr_array_add(chain, definition(3, "gold", "Gold", NULL));
  gchar *gold_award = award(3, gold, peer);
  g_autofree gchar *gold_id = id_of(gold_award);
  g_ptr_array_add(chain, gold_award);
  NostrTags *tags = nostr_tags_new(0);
  pair(tags, gold, gold_id, HINT);
  g_autoptr(GPtrArray) lists = strings();
  g_ptr_array_add(lists, event_json(2, NOSTR_KIND_PROFILE_BADGE_LIST, 2000, tags, ""));
  Done d; run_lookup(&f, peer, NULL, lists, chain, &d);
  g_assert_no_error(d.error);
  /* The hint was reported, not contacted. */
  g_assert_cmpuint(reads_for_url(&f, HINT), ==, 0);
  g_auto(GStrv) hints = gh_badge_service_dup_hints(f.service, peer);
  g_assert_cmpuint(g_strv_length(hints), ==, 1); g_assert_cmpstr(hints[0], ==, HINT);
  g_autoptr(GPtrArray) badges = gh_badge_service_get_badges(f.service, peer);
  g_assert_cmpuint(badges->len, ==, 1);
  g_assert_cmpstr(((GhBadge *)badges->pdata[0])->award_relay, ==, HINT);
  /* Explicitly approved: both REQs also go to the hinted relay. */
  const gchar *approved[] = { HINT, NULL };
  run_lookup(&f, peer, approved, lists, chain, &d);
  g_assert_no_error(d.error);
  g_assert_cmpuint(reads_for_url(&f, HINT), ==, 2);
  g_auto(GStrv) none = gh_badge_service_dup_hints(f.service, peer);
  g_assert_cmpuint(g_strv_length(none), ==, 0);
  /* A denied relay is never a source, approved or not. */
  const gchar *denied[] = { HINT, NULL };
  gh_relay_deny_policy_bind(gh_relay_deny_policy_get_default(), gh_account_controller_get_generation(f.accounts), denied);
  guint before = reads_for_url(&f, HINT);
  run_lookup(&f, peer, NULL, lists, chain, &d);
  g_assert_cmpuint(reads_for_url(&f, HINT), ==, before);
  gh_relay_deny_policy_revoke(gh_relay_deny_policy_get_default());
  teardown(&f);
}

/* ---- no default image fetch ------------------------------------------------------------ */

typedef struct { gboolean done; GdkTexture *texture; GError *error; } Art;
static void art_done(GObject *source, GAsyncResult *result, gpointer data)
{
  Art *a = data;
  a->texture = gh_badge_service_load_artwork_finish(GH_BADGE_SERVICE(source), result, &a->error); a->done = TRUE;
}
static gboolean art_finished(gpointer data) { return ((Art *)data)->done; }
static void test_no_default_image_fetch(void)
{
  Fixture f; setup(&f);
  g_assert_cmpstr(gh_web_content_setting(GH_WEB_BADGE), ==, "load-remote-images");
  g_assert_false(g_settings_get_boolean(f.settings, "load-remote-images"));
  g_assert_false(gh_badge_service_get_artwork_enabled(f.service));
  /* A verified badge with artwork: the lookup fetched nothing over HTTP. */
  g_autofree gchar *gold = address(issuer, "gold");
  g_autoptr(GPtrArray) chain = strings();
  g_ptr_array_add(chain, definition(3, "gold", "Gold", "https://img.test.invalid/gold.png"));
  gchar *gold_award = award(3, gold, peer);
  g_autofree gchar *gold_id = id_of(gold_award);
  g_ptr_array_add(chain, gold_award);
  NostrTags *tags = nostr_tags_new(0); pair(tags, gold, gold_id, NULL);
  g_autoptr(GPtrArray) lists = strings();
  g_ptr_array_add(lists, event_json(2, NOSTR_KIND_PROFILE_BADGE_LIST, 2000, tags, ""));
  Done d; run_lookup(&f, peer, NULL, lists, chain, &d);
  g_assert_no_error(d.error);
  g_autoptr(GPtrArray) badges = gh_badge_service_get_badges(f.service, peer);
  g_assert_cmpuint(badges->len, ==, 1);
  GhBadge *badge = badges->pdata[0];
  g_assert_cmpstr(badge->image_url, ==, "https://img.test.invalid/gold.png");
  g_assert_cmpuint(f.http_calls, ==, 0);
  /* Asked for while the switch is off: refused before any request. */
  Art a = { 0 };
  gh_badge_service_load_artwork_async(f.service, badge->image_url, NULL, art_done, &a);
  gh_test_spin_until(art_finished, &a);
  g_assert_error(a.error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED); g_clear_error(&a.error);
  g_assert_cmpuint(f.http_calls, ==, 0);
  /* Switched on: the GH_WEB_BADGE fetch goes out, and a malformed answer is
   * an error, not a crash: the text stands. */
  g_settings_set_boolean(f.settings, "load-remote-images", TRUE);
  g_assert_true(gh_badge_service_get_artwork_enabled(f.service));
  Art b = { 0 };
  gh_badge_service_load_artwork_async(f.service, badge->image_url, NULL, art_done, &b);
  gh_test_spin_until(art_finished, &b);
  g_assert_cmpuint(f.http_calls, ==, 1);
  g_assert_cmpstr(f.http_url, ==, badge->image_url);
  g_assert_null(b.texture); g_assert_nonnull(b.error); g_clear_error(&b.error);
  /* Never over plain http. */
  Art c = { 0 };
  gh_badge_service_load_artwork_async(f.service, "http://img.test.invalid/gold.png", NULL, art_done, &c);
  gh_test_spin_until(art_finished, &c);
  g_assert_error(c.error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT); g_clear_error(&c.error);
  g_assert_cmpuint(f.http_calls, ==, 1);
  g_settings_reset(f.settings, "load-remote-images");
  teardown(&f);
}

/* ---- explicit public confirmation ----------------------------------------------------- */

typedef struct { gboolean done, ok; GError *error; } Pub;
static void publish_done(GObject *source, GAsyncResult *result, gpointer data)
{
  Pub *p = data;
  p->ok = gh_badge_service_publish_finish(GH_BADGE_SERVICE(source), result, &p->error); p->done = TRUE;
}
static gboolean publish_finished(gpointer data) { return ((Pub *)data)->done; }
static gboolean sent_all(gpointer data) { return ((Fixture *)data)->sent->len >= 3; }
static void accept_writes(Fixture *f)
{
  for (guint i = 0; i < f->sent->len; i++) {
    Write *w = f->sent->pdata[i];
    if (!w->closed) gh_relay_publish_ok(w->publish, w->url, w->id, TRUE, "");
  }
  drain();
}
static GhBadge *badge_of(const gchar *addr, const gchar *award_id)
{
  GhBadge *badge = g_new0(GhBadge, 1);
  badge->address = g_strdup(addr); badge->award_id = g_strdup(award_id);
  badge->issuer = g_strndup(addr + 6, 64); badge->identifier = g_strdup(addr + 6 + 65);
  return badge;
}
static void test_explicit_publication(void)
{
  Fixture f; setup(&f);
  g_autofree gchar *gold = address(issuer, "gold");
  gchar *gold_award = award(3, gold, account);
  g_autofree gchar *gold_id = id_of(gold_award); g_free(gold_award);
  guint calls = f.signer.calls;
  g_autoptr(GPtrArray) selection = g_ptr_array_new_with_free_func((GDestroyNotify)gh_badge_free);
  g_ptr_array_add(selection, badge_of(gold, gold_id));
  g_autoptr(GError) error = NULL;
  /* Choosing is local: nothing signed, nothing sent. */
  g_assert_true(gh_badge_service_set_selection(f.service, selection, &error)); g_assert_no_error(error);
  drain();
  gh_test_signer_assert_no_calls_since(&f.signer, calls);
  g_assert_cmpuint(f.sent->len, ==, 0);
  GhBadgeSelectionSource source = GH_BADGE_SELECTION_EMPTY;
  g_autoptr(GPtrArray) chosen = gh_badge_service_get_selection(f.service, &source, &error);
  g_assert_no_error(error); g_assert_cmpint(source, ==, GH_BADGE_SELECTION_LOCAL);
  g_assert_cmpuint(chosen->len, ==, 1);
  g_assert_cmpstr(((GhBadge *)chosen->pdata[0])->award_id, ==, gold_id);
  /* Chosen, but its award and definition were never seen: not attributed. */
  g_assert_false(((GhBadge *)chosen->pdata[0])->verified);
  /* The explicit publication: one kind 10008 to each write relay. */
  g_auto(GStrv) targets = gh_badge_service_dup_targets(f.service);
  g_assert_cmpuint(g_strv_length(targets), ==, 3);
  Pub p = { 0 };
  gh_badge_service_publish_async(f.service, NULL, publish_done, &p);
  gh_test_spin_until(sent_all, &f);
  for (guint i = 0; i < f.sent->len; i++) {
    Write *w = f.sent->pdata[i];
    g_assert_cmpint(w->kind, ==, NOSTR_KIND_PROFILE_BADGE_LIST);
    g_assert_true(g_strv_contains(writes, w->url));
    g_assert_nonnull(strstr(w->json, gold)); g_assert_nonnull(strstr(w->json, gold_id));
    g_assert_null(strstr(w->json, "[\"d\""));
  }
  accept_writes(&f);
  gh_test_spin_until(publish_finished, &p);
  g_assert_no_error(p.error); g_assert_true(p.ok);
  teardown(&f);
}

/* ---- legacy 30008 to canonical 10008 ------------------------------------------------- */

static void test_legacy_to_canonical(void)
{
  Fixture f; setup(&f);
  g_autofree gchar *gold = address(issuer, "gold");
  g_autoptr(GPtrArray) chain = strings();
  g_ptr_array_add(chain, definition(3, "gold", "Gold", NULL));
  gchar *gold_award = award(3, gold, account);
  g_autofree gchar *gold_id = id_of(gold_award);
  g_ptr_array_add(chain, gold_award);
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("d", "profile_badges", NULL));
  pair(tags, gold, gold_id, NULL);
  g_autoptr(GPtrArray) lists = strings();
  g_ptr_array_add(lists, event_json(1, NOSTR_KIND_BADGE_SETS, 2000, tags, ""));
  g_ptr_array_add(lists, g_strdup(gold_award)); /* kind 8 #p=account on the first REQ */
  Done d; run_lookup(&f, account, NULL, lists, chain, &d);
  g_assert_no_error(d.error);
  /* Displayed as the equivalent of a canonical list. */
  g_assert_cmpint(gh_badge_service_get_list_kind(f.service, account), ==, GH_BADGE_LIST_LEGACY);
  g_autoptr(GPtrArray) shown = gh_badge_service_get_badges(f.service, account);
  g_assert_cmpuint(shown->len, ==, 1);
  g_autoptr(GPtrArray) awards = gh_badge_service_get_awards(f.service);
  g_assert_nonnull(awards); g_assert_cmpuint(awards->len, ==, 1);
  g_assert_cmpstr(((GhBadge *)awards->pdata[0])->name, ==, "Gold");
  GhBadgeSelectionSource source = GH_BADGE_SELECTION_EMPTY;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) seeded = gh_badge_service_get_selection(f.service, &source, &error);
  g_assert_no_error(error); g_assert_cmpint(source, ==, GH_BADGE_SELECTION_LEGACY);
  g_assert_cmpuint(seeded->len, ==, 1);
  g_assert_true(((GhBadge *)seeded->pdata[0])->verified);
  /* No local document was created for the legacy event, and nothing was
   * published on its account. */
  GNostrListService *service = gh_social_lists_get_service(f.social);
  g_autoptr(GNostrListDocument) legacy = gnostr_list_service_lookup(service, NOSTR_KIND_BADGE_SETS, "profile_badges");
  g_assert_null(legacy);
  g_assert_cmpuint(f.sent->len, ==, 0);
  /* The next explicit publication is canonical 10008 with the same pair; no
   * deletion of the legacy event is ever sent. */
  Pub p = { 0 };
  gh_badge_service_publish_async(f.service, NULL, publish_done, &p);
  gh_test_spin_until(sent_all, &f);
  for (guint i = 0; i < f.sent->len; i++) {
    Write *w = f.sent->pdata[i];
    g_assert_cmpint(w->kind, ==, NOSTR_KIND_PROFILE_BADGE_LIST);
    g_assert_nonnull(strstr(w->json, gold_id));
    g_assert_null(strstr(w->json, "profile_badges"));
  }
  accept_writes(&f);
  gh_test_spin_until(publish_finished, &p);
  g_assert_no_error(p.error); g_assert_true(p.ok);
  for (guint i = 0; i < f.sent->len; i++)
    g_assert_cmpint(((Write *)f.sent->pdata[i])->kind, !=, NOSTR_KIND_DELETION);
  g_autoptr(GPtrArray) local = gh_badge_service_get_selection(f.service, &source, &error);
  g_assert_cmpint(source, ==, GH_BADGE_SELECTION_LOCAL); g_assert_cmpuint(local->len, ==, 1);
  g_autoptr(GNostrListDocument) still = gnostr_list_service_lookup(service, NOSTR_KIND_BADGE_SETS, "profile_badges");
  g_assert_null(still);
  teardown(&f);
}

/* ---- an empty canonical list wins ------------------------------------------------------ */

static void test_empty_canonical_wins(void)
{
  Fixture f; setup(&f);
  g_autofree gchar *gold = address(issuer, "gold");
  gchar *gold_award = award(3, gold, peer);
  g_autofree gchar *gold_id = id_of(gold_award); g_free(gold_award);
  NostrTags *legacy_tags = nostr_tags_new(0);
  nostr_tags_append(legacy_tags, nostr_tag_new("d", "profile_badges", NULL));
  pair(legacy_tags, gold, gold_id, NULL);
  g_autoptr(GPtrArray) lists = strings();
  g_ptr_array_add(lists, event_json(2, NOSTR_KIND_BADGE_SETS, 2500, legacy_tags, ""));
  g_ptr_array_add(lists, event_json(2, NOSTR_KIND_PROFILE_BADGE_LIST, 2000, nostr_tags_new(0), ""));
  /* No pairs to resolve: the lookup ends after the lists. */
  guint reads = f.reads->len;
  Done d; run_lookup(&f, peer, NULL, lists, NULL, &d);
  g_assert_no_error(d.error);
  g_assert_cmpuint(f.reads->len, ==, reads + 1);
  g_assert_cmpint(gh_badge_service_get_list_kind(f.service, peer), ==, GH_BADGE_LIST_CANONICAL);
  g_autoptr(GPtrArray) badges = gh_badge_service_get_badges(f.service, peer);
  g_assert_nonnull(badges); g_assert_cmpuint(badges->len, ==, 0);
  /* A later answer carrying only the legacy list does not resurrect it: the
   * known canonical list still wins. */
  g_autoptr(GPtrArray) legacy_only = strings();
  g_ptr_array_add(legacy_only, g_strdup(lists->pdata[0]));
  run_lookup(&f, peer, NULL, legacy_only, NULL, &d);
  g_assert_no_error(d.error);
  g_assert_cmpint(gh_badge_service_get_list_kind(f.service, peer), ==, GH_BADGE_LIST_CANONICAL);
  g_autoptr(GPtrArray) still = gh_badge_service_get_badges(f.service, peer);
  g_assert_cmpuint(still->len, ==, 0);
  /* The own account: a legacy list seeds the selection only until a local
   * canonical document exists; an intentionally empty one clears it. */
  NostrTags *own_tags = nostr_tags_new(0);
  nostr_tags_append(own_tags, nostr_tag_new("d", "profile_badges", NULL));
  g_autofree gchar *own_award = award(3, gold, account);
  g_autofree gchar *own_id = id_of(own_award);
  pair(own_tags, gold, own_id, NULL);
  g_autoptr(GPtrArray) own_lists = strings();
  g_ptr_array_add(own_lists, event_json(1, NOSTR_KIND_BADGE_SETS, 2000, own_tags, ""));
  g_autoptr(GPtrArray) chain = strings();
  g_ptr_array_add(chain, definition(3, "gold", "Gold", NULL)); g_ptr_array_add(chain, g_strdup(own_award));
  run_lookup(&f, account, NULL, own_lists, chain, &d);
  g_assert_no_error(d.error);
  GhBadgeSelectionSource source = GH_BADGE_SELECTION_EMPTY;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) seeded = gh_badge_service_get_selection(f.service, &source, &error);
  g_assert_cmpint(source, ==, GH_BADGE_SELECTION_LEGACY); g_assert_cmpuint(seeded->len, ==, 1);
  g_autoptr(GPtrArray) none = g_ptr_array_new_with_free_func((GDestroyNotify)gh_badge_free);
  g_assert_true(gh_badge_service_set_selection(f.service, none, &error)); g_assert_no_error(error);
  g_autoptr(GPtrArray) cleared = gh_badge_service_get_selection(f.service, &source, &error);
  g_assert_cmpint(source, ==, GH_BADGE_SELECTION_LOCAL); g_assert_cmpuint(cleared->len, ==, 0);
  teardown(&f);
}

/* ---- the account binding ------------------------------------------------------------- */

static void test_close_cancels(void)
{
  Fixture f; setup(&f);
  Done d = { 0 };
  gh_badge_service_lookup_async(f.service, peer, NULL, NULL, lookup_done, &d);
  ReadCount first = { &f, f.reads->len };
  gh_test_spin_until(reads_reached, &first);
  gh_badge_service_close(f.service);
  gh_test_spin_until(finished, &d);
  g_assert_error(d.error, G_IO_ERROR, G_IO_ERROR_CLOSED); g_clear_error(&d.error);
  g_assert_null(gh_badge_service_get_badges(f.service, peer));
  g_autoptr(GError) error = NULL;
  g_assert_null(gh_badge_service_get_selection(f.service, NULL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CLOSED);
  teardown(&f);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  account = gh_test_pub(1); npub = gh_test_npub(1);
  peer = gh_test_pub(2); issuer = gh_test_pub(3); other = gh_test_pub(4);
  gh_test_bus_up(&bus);
  g_test_add_func("/privacy/badges/forged-award-chains", test_forged_chains);
  g_test_add_func("/privacy/badges/unmatched-pairs", test_unmatched_pairs);
  g_test_add_func("/privacy/badges/relay-hints-need-approval", test_hints_need_approval);
  g_test_add_func("/privacy/badges/no-default-image-fetch", test_no_default_image_fetch);
  g_test_add_func("/privacy/badges/explicit-public-confirmation", test_explicit_publication);
  g_test_add_func("/groundhog/badges/legacy-to-canonical", test_legacy_to_canonical);
  g_test_add_func("/groundhog/badges/empty-canonical-wins", test_empty_canonical_wins);
  g_test_add_func("/groundhog/badges/close-cancels", test_close_cancels);
  gint result = g_test_run();
  gh_test_bus_down(&bus);
  g_free(account); g_free(npub); g_free(peer); g_free(issuer); g_free(other);
  return result;
}