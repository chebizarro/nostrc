/* GhContactDirectory (privacy charter G10): NT-11 directory cache, NT-12 no
 * startup fan-out with the S1 batch and jitter bounds, PT-8 (no kind-0 REQ
 * for a request until it is accepted), S2 (stale entries publish, refresh in
 * the background, "changed"), strict admission (forged, foreign, stale and
 * future events), restore across a restart, and the account switch.
 *
 * Harnesses: a recording GhRelayScope transport with its NIP-42 half (H1),
 * the fake GhClock (H6) for every delay, jitter and deadline, real SQLCipher
 * stores in a private directory (store test hooks build), a real
 * GhConversationStore fed with local rumors, and a real account controller
 * with the mock signer on the private test bus. Waits iterate the main
 * context; their deadlines are failure bounds only. */
#include "gh-contact-directory.h"
#include "gh-store-directory.h"
#include "gh-test-signer.h"

#include "nostr-tag.h"

#include <glib/gstdio.h>

#define DISC_A  "wss://discovery-a.test.invalid"
#define DISC_B  "wss://discovery-b.test.invalid"
#define R1      "wss://r1.test.invalid"
#define R2      "wss://r2.test.invalid"
#define R7      "wss://r7.test.invalid"
#define R8      "wss://r8.test.invalid"
#define R9      "wss://r9.test.invalid"
#define STORE_ID "7c1e2a90-4b3d-4f5e-8a6b-1c2d3e4f5a6b"
#define T0      G_GINT64_CONSTANT(1900000000)
#define N_PEOPLE 30

typedef struct {
  gchar *sk;
  gchar *pk;
} Person;

static Person people[N_PEOPLE];
static gchar *hex_alice, *npub_alice, *npub_bob;
static GhTestBus shared_bus;

static void
drain(void)
{
  while (g_main_context_iteration(NULL, FALSE))
    ;
}

static void
rm_rf(const gchar *path)
{
  if (g_file_test(path, G_FILE_TEST_IS_DIR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
    GDir *dir = g_dir_open(path, 0, NULL);
    const gchar *name;
    while (dir && (name = g_dir_read_name(dir))) {
      g_autofree gchar *child = g_build_filename(path, name, NULL);
      rm_rf(child);
    }
    if (dir)
      g_dir_close(dir);
    g_rmdir(path);
  } else {
    g_unlink(path);
  }
}

/* ---- events ------------------------------------------------------------------ */

static gchar *
signed_by(const gchar *sk, int kind, gint64 created_at, NostrTags *tags, const gchar *content)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, kind);
  nostr_event_set_created_at(event, created_at);
  nostr_event_set_content(event, content ? content : "");
  nostr_event_set_tags(event, tags ? tags : nostr_tags_new(0));
  g_assert_cmpint(nostr_event_sign(event, sk), ==, 0);
  char *json = nostr_event_serialize_compact(event);
  gchar *copy = g_strdup(json);
  free(json);
  nostr_event_free(event);
  return copy;
}

/* A kind-10050 of who naming the NULL-terminated relay URLs. */
static gchar *
inbox_list(const Person *who, gint64 created_at, ...)
{
  NostrTags *tags = nostr_tags_new(0);
  va_list args;
  va_start(args, created_at);
  for (const gchar *url = va_arg(args, const gchar *); url; url = va_arg(args, const gchar *))
    nostr_tags_append(tags, nostr_tag_new("relay", url, NULL));
  va_end(args);
  return signed_by(who->sk, 10050, created_at, tags, "");
}

static gchar *
profile(const Person *who, gint64 created_at, const gchar *content)
{
  return signed_by(who->sk, 0, created_at, NULL, content);
}

static gchar *
replace_once(const gchar *text, const gchar *old, const gchar *new_text)
{
  const gchar *at = strstr(text, old);
  g_assert_nonnull(at);
  return g_strdup_printf("%.*s%s%s", (int)(at - text), text, new_text, at + strlen(old));
}

/* ---- recording scope transport --------------------------------------------------- */

typedef struct {
  GhRelayScope *scope;
  gchar *url;
  GPtrArray *authors;
  gboolean profiles; /* kind 0 asked for */
  gboolean closed;
  GPtrArray *auth;
  guint resubscribes;
  gint64 opened_at;  /* fake clock, unix seconds */
} Req;

typedef struct {
  GPtrArray *reqs;
  GhClock *clock;
} Rec;

static void
req_free(gpointer data)
{
  Req *req = data;
  gh_relay_scope_unref(req->scope);
  g_free(req->url);
  g_ptr_array_unref(req->authors);
  g_ptr_array_unref(req->auth);
  g_free(req);
}

static gpointer
rec_open(GhRelayScope *scope, const gchar *url, const NostrFilters *filters, gpointer data,
         GError **error)
{
  (void)error;
  Rec *rec = data;
  Req *req = g_new0(Req, 1);
  req->scope = gh_relay_scope_ref(scope);
  req->url = g_strdup(url);
  req->authors = g_ptr_array_new_with_free_func(g_free);
  req->auth = g_ptr_array_new_with_free_func(g_free);
  req->opened_at = gh_clock_get_unix(rec->clock);
  g_assert_cmpuint(filters->count, ==, 1);
  const NostrFilter *filter = &filters->filters[0];
  for (size_t i = 0; i < nostr_filter_authors_len(filter); i++)
    g_ptr_array_add(req->authors, g_strdup(nostr_filter_authors_get(filter, i)));
  gboolean inbox = FALSE;
  for (size_t i = 0; i < nostr_filter_kinds_len(filter); i++) {
    int kind = nostr_filter_kinds_get(filter, i);
    g_assert_true(kind == 0 || kind == 10050); /* nothing else is ever asked for */
    req->profiles |= kind == 0;
    inbox |= kind == 10050;
  }
  g_assert_true(inbox);
  /* Discovery relays only (PD-12). */
  g_assert_true(g_str_equal(url, DISC_A) || g_str_equal(url, DISC_B));
  g_ptr_array_add(rec->reqs, req);
  return req;
}

static void
rec_close(gpointer handle, gpointer data)
{
  (void)data;
  ((Req *)handle)->closed = TRUE;
}

static gboolean
rec_send_auth(gpointer handle, const gchar *signed_json, gpointer data, GError **error)
{
  (void)data; (void)error;
  g_ptr_array_add(((Req *)handle)->auth, g_strdup(signed_json));
  return TRUE;
}

static void
rec_resubscribe(gpointer handle, gpointer data)
{
  (void)data;
  ((Req *)handle)->resubscribes++;
}

static const GhRelayTransport rec_transport = { rec_open, rec_close };
static const GhRelayAuthTransport rec_auth = { rec_send_auth, rec_resubscribe };

static gboolean
req_auth_sent(gpointer data)
{
  return ((Req *)data)->auth->len > 0;
}

static gboolean
req_asks(Req *req, const gchar *pubkey)
{
  return g_ptr_array_find_with_equal_func(req->authors, pubkey, g_str_equal, NULL);
}

/* Delivers the NULL-terminated events, then EOSE. */
static void
answer(Req *req, ...)
{
  va_list args;
  va_start(args, req);
  for (const gchar *json = va_arg(args, const gchar *); json; json = va_arg(args, const gchar *))
    gh_relay_scope_event(req->scope, req->url, json);
  va_end(args);
  gh_relay_scope_eose(req->scope, req->url);
}

/* ---- fixture ------------------------------------------------------------------ */

typedef struct {
  GhTestSigner mock;
  GSettings *settings;
  GhAccountController *accounts;
  GhClock *clock;
  GhConversationStore *model;
  gchar *data_dir;
  GhStore *store;
  Rec rec;
  GhContactDirectory *dir;
  GPtrArray *changed;  /* resolver "changed" pubkeys */
  GPtrArray *profiles; /* "profile-changed" pubkeys */
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

static GhStore *
store_open(const gchar *data_dir, GhClock *clock)
{
  guint8 key[GH_STORE_KEY_SIZE];
  for (guint i = 0; i < sizeof key; i++)
    key[i] = (guint8)(0x3c ^ i);
  g_autoptr(GBytes) bytes = g_bytes_new(key, sizeof key);
  GhStoreConfig config = { data_dir, hex_alice, NULL, NULL, clock };
  g_autoptr(GError) error = NULL;
  GhStore *store = gh_store_open_with_key(&config, bytes, STORE_ID, GH_STORE_OPEN_CREATE, &error);
  g_assert_no_error(error);
  g_assert_nonnull(store);
  return store;
}

static void
on_changed(GhInboxResolver *resolver, const gchar *pubkey, gpointer data)
{
  (void)resolver;
  g_ptr_array_add(data, g_strdup(pubkey));
}

static void
new_directory(Fixture *f)
{
  GhContactDirectoryConfig config = {
    .accounts = f->accounts,
    .settings = f->settings,
    .clock = f->clock,
    .transport = &rec_transport,
    .auth_transport = &rec_auth,
    .transport_data = &f->rec,
  };
  f->dir = gh_contact_directory_new(&config);
  g_signal_connect(f->dir, "changed", G_CALLBACK(on_changed), f->changed);
  g_signal_connect(f->dir, "profile-changed", G_CALLBACK(on_changed), f->profiles);
  gh_contact_directory_set_conversations(f->dir, f->model);
}

static void
bind_store(Fixture *f)
{
  if (!f->store)
    f->store = store_open(f->data_dir, f->clock);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_contact_directory_set_store(f->dir, f->store, &error));
  g_assert_no_error(error);
}

static void
fixture_up(Fixture *f, gboolean with_store)
{
  memset(f, 0, sizeof *f);
  gh_test_signer_up(&shared_bus, &f->mock);
  f->settings = g_settings_new("org.nostr.Groundhog");
  const gchar *sources[] = { DISC_A, DISC_B, NULL };
  g_settings_set_strv(f->settings, "discovery-relays", sources);
  g_settings_set_string(f->settings, "current-npub", npub_alice);
  f->accounts = gh_account_controller_new_full(f->settings, shared_bus.client, fake_list, NULL);
  gh_test_spin_until(listed, f->accounts);
  g_assert_cmpint(gh_account_controller_get_state(f->accounts), ==, GH_ACCOUNT_STATE_ACTIVE);
  f->clock = gh_clock_new_fake(T0 * G_USEC_PER_SEC);
  f->model = gh_conversation_store_new();
  gh_conversation_store_set_account(f->model, hex_alice, NULL, NULL, NULL);
  f->data_dir = g_dir_make_tmp("groundhog-directory-XXXXXX", NULL);
  g_assert_nonnull(f->data_dir);
  f->rec.reqs = g_ptr_array_new_with_free_func(req_free);
  f->rec.clock = f->clock;
  f->changed = g_ptr_array_new_with_free_func(g_free);
  f->profiles = g_ptr_array_new_with_free_func(g_free);
  new_directory(f);
  if (with_store)
    bind_store(f);
}

static void
fixture_down(Fixture *f)
{
  gh_test_release(f->dir);
  drain();
  /* Every lookup's connections closed with it. */
  for (guint i = 0; i < f->rec.reqs->len; i++)
    g_assert_true(((Req *)g_ptr_array_index(f->rec.reqs, i))->closed);
  g_ptr_array_unref(f->rec.reqs);
  g_object_unref(f->model);
  if (f->store)
    gh_store_close(f->store);
  gh_test_release(f->accounts);
  GhTestSenders check = { &shared_bus, &f->mock };
  gh_test_spin_until(gh_test_signer_senders_closed, &check);
  g_object_unref(f->settings);
  gh_test_signer_down(&shared_bus, &f->mock);
  gh_clock_unref(f->clock);
  g_ptr_array_unref(f->changed);
  g_ptr_array_unref(f->profiles);
  rm_rf(f->data_dir);
  g_free(f->data_dir);
}

static void
advance(Fixture *f, gint64 seconds)
{
  gh_clock_fake_advance(f->clock, seconds * G_USEC_PER_SEC);
  drain();
}

/* Moves the fake clock to the next pending timeout; returns how far. */
static gint64
advance_to_next(Fixture *f)
{
  gint64 next = gh_clock_fake_get_next_deadline(f->clock);
  g_assert_cmpint(next, >=, 0);
  gint64 delta = MAX(next - gh_clock_get_monotonic_time(f->clock), 0);
  gh_clock_fake_advance(f->clock, delta);
  drain();
  return delta / G_USEC_PER_SEC;
}

/* A local rumor between the account and who: from the account (an accepted
 * conversation), or from who (a message request). Returns the room. */
static GhConversation *
room_with(Fixture *f, const Person *who, gboolean from_account, gint64 created_at)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 14);
  nostr_event_set_pubkey(event, from_account ? hex_alice : who->pk);
  nostr_event_set_created_at(event, created_at);
  nostr_event_set_content(event, "hello");
  nostr_event_set_tags(event, nostr_tags_new(1, nostr_tag_new("p", from_account ? who->pk
                                                                              : hex_alice, NULL)));
  event->id = nostr_event_get_id(event);
  char *raw = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMessage) message = gh_message_new_from_rumor(hex_alice, raw, &error);
  free(raw);
  g_assert_no_error(error);
  g_assert_cmpint(gh_conversation_store_add_message(f->model, message, &error), ==,
                  GH_CONVERSATION_ADD_NEW);
  GhConversation *room = gh_conversation_store_lookup(f->model, gh_message_get_room_id(message));
  g_assert_nonnull(room);
  g_assert_cmpint(gh_conversation_get_is_request(room), ==, !from_account);
  return room;
}

typedef struct {
  gboolean done;
  GhInboxResult *result;
  GError *error;
} Resolved;

static void
on_resolved(GObject *source, GAsyncResult *result, gpointer data)
{
  Resolved *r = data;
  r->result = gh_inbox_resolver_resolve_finish(GH_INBOX_RESOLVER(source), result, &r->error);
  r->done = TRUE;
}

static void
resolved_clear(Resolved *r)
{
  g_clear_pointer(&r->result, gh_inbox_result_free);
  g_clear_error(&r->error);
  r->done = FALSE;
}

static void
resolve_start(Fixture *f, const gchar *pubkey, Resolved *r, GCancellable *cancellable)
{
  resolved_clear(r);
  gh_inbox_resolver_resolve_async(GH_INBOX_RESOLVER(f->dir), pubkey, cancellable, on_resolved, r);
}

static gboolean
is_done(gpointer data)
{
  return ((Resolved *)data)->done;
}

static void
resolve(Fixture *f, const gchar *pubkey, Resolved *r)
{
  resolve_start(f, pubkey, r, NULL);
  gh_test_spin_until(is_done, r);
  g_assert_no_error(r->error);
  g_assert_nonnull(r->result);
}

static Req *
req_at(Fixture *f, guint index)
{
  g_assert_cmpuint(index, <, f->rec.reqs->len);
  return g_ptr_array_index(f->rec.reqs, index);
}

static void
assert_relays(const GhInboxResult *result, ...)
{
  va_list args;
  va_start(args, result);
  guint n = 0;
  for (const gchar *url = va_arg(args, const gchar *); url; url = va_arg(args, const gchar *))
    g_assert_cmpstr(result->relays[n++], ==, url);
  va_end(args);
  g_assert_null(result->relays[n]);
}

/* ---- NT-11 / S2 ---------------------------------------------------------------- */

/* A fresh cache answers with no REQ; a stale one still answers at once and
 * is refreshed U(5, 60) s later off the send path; a changed list is
 * announced ("changed") and answered from then on. */
static void
test_nt11_cache(void)
{
  Fixture f;
  fixture_up(&f, TRUE);
  const Person *carol = &people[0];
  room_with(&f, carol, TRUE, T0 - 500);

  /* The first contact: one lookup the send waits for, on both sources. */
  Resolved r = { 0 };
  resolve_start(&f, carol->pk, &r, NULL);
  drain();
  g_assert_false(r.done);
  g_assert_cmpuint(f.rec.reqs->len, ==, 2);
  for (guint i = 0; i < 2; i++) {
    g_assert_true(req_asks(req_at(&f, i), carol->pk));
    g_assert_cmpuint(req_at(&f, i)->authors->len, ==, 1);
    g_assert_true(req_at(&f, i)->profiles); /* accepted: the name may be asked for */
  }
  g_autofree gchar *list = inbox_list(carol, T0 - 100, R1, NULL);
  g_autofree gchar *name = profile(carol, T0 - 100, "{\"name\":\"carol\",\"display_name\":\"Carol\"}");
  answer(req_at(&f, 0), list, name, NULL);
  answer(req_at(&f, 1), NULL);
  gh_test_spin_until(is_done, &r);
  g_assert_no_error(r.error);
  g_assert_cmpint(r.result->status, ==, GH_INBOX_FOUND);
  g_assert_false(r.result->cached);
  g_assert_cmpuint(r.result->sources, ==, 2);
  g_assert_cmpuint(r.result->answered, ==, 2);
  assert_relays(r.result, R1, NULL);
  g_assert_cmpstr(gh_contact_directory_get_display_name(f.dir, carol->pk), ==, "Carol");
  g_assert_true(req_at(&f, 0)->closed && req_at(&f, 1)->closed);

  /* Fresh: no discovery REQ at send. */
  resolve(&f, carol->pk, &r);
  g_assert_true(r.result->cached);
  g_assert_cmpuint(r.result->sources, ==, 0);
  assert_relays(r.result, R1, NULL);
  g_assert_cmpuint(f.rec.reqs->len, ==, 2);

  /* 24 h later the entry is stale, and still no scheduled run is due yet. */
  advance(&f, 24 * 3600 + 1);
  g_assert_cmpuint(f.rec.reqs->len, ==, 2);
  resolve(&f, carol->pk, &r);
  g_assert_true(r.result->cached);
  assert_relays(r.result, R1, NULL); /* the send uses the cached list */
  g_assert_cmpuint(f.rec.reqs->len, ==, 2); /* nothing at send time */
  gint64 sent_at = gh_clock_get_unix(f.clock);
  advance_to_next(&f); /* the refresh timer, before its deadline */
  g_assert_cmpuint(f.rec.reqs->len, ==, 4);
  Req *refresh = req_at(&f, 2);
  g_assert_cmpint(refresh->opened_at - sent_at, >=, 5);
  g_assert_cmpint(refresh->opened_at - sent_at, <=, 60);
  g_assert_true(req_asks(refresh, carol->pk));
  g_autofree gchar *moved = inbox_list(carol, T0 + 10, R1, R2, NULL);
  answer(req_at(&f, 2), moved, NULL);
  answer(req_at(&f, 3), NULL);
  drain();
  g_assert_cmpuint(f.changed->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(f.changed, 0), ==, carol->pk);
  resolve(&f, carol->pk, &r);
  g_assert_true(r.result->cached);
  assert_relays(r.result, R1, R2, NULL);
  g_assert_cmpuint(f.rec.reqs->len, ==, 4);
  g_assert_cmpuint(f.mock.calls, ==, 0);
  resolved_clear(&r);
  fixture_down(&f);
}

/* ---- NT-12 / S1 ----------------------------------------------------------------- */

/* With 25 accepted contacts and 2 requests restored before the store opens,
 * nothing is looked up in the first 60 s; the first run starts within
 * [2, 30] min, asks each accepted contact once in batches of <= 10 on fresh
 * scopes spaced [10, 120] s apart, never a request, and signs in (if asked)
 * with a throwaway key only. */
static void
test_nt12_startup(void)
{
  Fixture f;
  fixture_up(&f, FALSE);
  for (guint i = 0; i < 25; i++)
    room_with(&f, &people[i], TRUE, T0 - 1000 + i);
  room_with(&f, &people[25], FALSE, T0 - 10);
  room_with(&f, &people[26], FALSE, T0 - 9);
  bind_store(&f);
  gint64 bound_at = gh_clock_get_unix(f.clock);
  advance(&f, 60);
  g_assert_cmpuint(f.rec.reqs->len, ==, 0);
  g_assert_cmpuint(gh_clock_fake_get_n_timeouts(f.clock), ==, 1);

  g_autoptr(GHashTable) asked = g_hash_table_new(g_str_hash, g_str_equal);
  g_autoptr(GHashTable) scopes = g_hash_table_new(NULL, NULL);
  guint batches = 0;
  gint64 previous = 0;
  while (g_hash_table_size(asked) < 25) {
    guint before = f.rec.reqs->len;
    advance_to_next(&f);
    g_assert_cmpuint(f.rec.reqs->len, ==, before + 2); /* one REQ per discovery relay */
    Req *a = req_at(&f, before), *b = req_at(&f, before + 1);
    g_assert_true(a->scope == b->scope);
    g_assert_false(g_hash_table_contains(scopes, a->scope)); /* a fresh scope per batch */
    g_hash_table_add(scopes, a->scope);
    g_assert_cmpuint(a->authors->len, <=, 10);
    g_assert_true(a->profiles);
    if (batches == 0) {
      g_assert_cmpint(a->opened_at - bound_at, >=, 2 * 60);
      g_assert_cmpint(a->opened_at - bound_at, <=, 30 * 60);
      /* This discovery relay demands AUTH: an ephemeral key, never the account. */
      gh_relay_scope_auth_challenge(a->scope, a->url, "directory-challenge");
      gh_relay_scope_notice(a->scope, a->url, GH_RELAY_NOTICE_CLOSED, NULL, FALSE,
                            "auth-required: sign in");
      gh_test_spin_until(req_auth_sent, a);
      gchar id[65] = { 0 };
      g_autoptr(GError) error = NULL;
      g_assert_true(gh_relay_auth_verify_signed(g_ptr_array_index(a->auth, 0), a->url,
                                                "directory-challenge", NULL,
                                                g_get_real_time() / G_USEC_PER_SEC, id, &error));
      NostrEvent *auth = nostr_event_new();
      g_assert_cmpint(nostr_event_deserialize_compact(auth, g_ptr_array_index(a->auth, 0), NULL),
                      ==, 1);
      g_assert_cmpstr(nostr_event_get_pubkey(auth), !=, hex_alice);
      nostr_event_free(auth);
      gh_relay_scope_notice(a->scope, a->url, GH_RELAY_NOTICE_OK, id, TRUE, "");
      g_assert_cmpuint(a->resubscribes, ==, 1);
    } else {
      g_assert_cmpint(a->opened_at - previous, >=, 10);
      g_assert_cmpint(a->opened_at - previous, <=, 120);
    }
    previous = a->opened_at;
    for (guint i = 0; i < a->authors->len; i++) {
      const gchar *pk = g_ptr_array_index(a->authors, i);
      g_assert_false(g_hash_table_contains(asked, pk)); /* each once */
      g_hash_table_add(asked, (gpointer)pk);
    }
    answer(a, NULL);
    answer(b, NULL);
    drain();
    batches++;
  }
  g_assert_cmpuint(batches, ==, 3);
  g_assert_cmpuint(g_hash_table_size(asked), ==, 25);
  /* The run is over: the next one is due when these turn stale (S2). */
  g_assert_cmpuint(gh_clock_fake_get_n_timeouts(f.clock), ==, 1);
  gint64 next = (gh_clock_fake_get_next_deadline(f.clock) -
                 gh_clock_get_monotonic_time(f.clock)) / G_USEC_PER_SEC;
  g_assert_cmpint(next, >=, 24 * 3600 - 3 * 120);
  g_assert_cmpint(next, <=, 24 * 3600 + 30 * 60);
  for (guint i = 0; i < 25; i++)
    g_assert_true(g_hash_table_contains(asked, people[i].pk));
  g_assert_false(g_hash_table_contains(asked, people[25].pk));
  g_assert_false(g_hash_table_contains(asked, people[26].pk));
  g_assert_cmpuint(f.mock.calls, ==, 0);
  fixture_down(&f);
}

/* ---- PT-8 -------------------------------------------------------------------- */

/* A request's sender is never looked up by the schedule, and a reply's
 * one-shot lookup asks for its 10050 only (no kind 0) and admits no name.
 * Accepting lets the name be fetched, alone, within U(5, 60) s. */
static void
test_pt8_requests(void)
{
  Fixture f;
  fixture_up(&f, FALSE);
  const Person *xavier = &people[0], *yvonne = &people[1];
  GhConversation *request = room_with(&f, xavier, FALSE, T0 - 100);
  room_with(&f, yvonne, TRUE, T0 - 90);
  bind_store(&f);
  g_assert_false(gh_contact_directory_is_accepted(f.dir, xavier->pk));
  g_assert_true(gh_contact_directory_is_accepted(f.dir, yvonne->pk));
  advance_to_next(&f); /* the first run */
  g_assert_cmpuint(f.rec.reqs->len, ==, 2);
  for (guint i = 0; i < f.rec.reqs->len; i++) {
    g_assert_false(req_asks(req_at(&f, i), xavier->pk));
    g_assert_true(req_asks(req_at(&f, i), yvonne->pk));
    answer(req_at(&f, i), NULL);
  }
  drain();

  /* Replying to the request needs its inbox: the 10050 only. */
  Resolved r = { 0 };
  resolve_start(&f, xavier->pk, &r, NULL);
  drain();
  g_assert_cmpuint(f.rec.reqs->len, ==, 4);
  g_assert_false(req_at(&f, 2)->profiles);
  g_autofree gchar *list = inbox_list(xavier, T0 - 50, R1, NULL);
  g_autofree gchar *name = profile(xavier, T0 - 50, "{\"name\":\"Xavier\"}");
  answer(req_at(&f, 2), list, name, NULL);
  answer(req_at(&f, 3), NULL);
  gh_test_spin_until(is_done, &r);
  g_assert_cmpint(r.result->status, ==, GH_INBOX_FOUND);
  g_assert_null(gh_contact_directory_get_display_name(f.dir, xavier->pk));
  g_autofree gchar *no_title = gh_contact_directory_dup_conversation_title(f.dir, request);
  g_assert_null(no_title);

  /* Accepted: its name is fetched, on its own, soon. */
  g_ptr_array_set_size(f.profiles, 0);
  gh_conversation_accept(request);
  g_assert_true(gh_contact_directory_is_accepted(f.dir, xavier->pk));
  g_assert_null(gh_contact_directory_get_display_name(f.dir, xavier->pk)); /* never admitted */
  gint64 accepted_at = gh_clock_get_unix(f.clock);
  advance_to_next(&f); /* the refresh timer, before its deadline */
  g_assert_cmpuint(f.rec.reqs->len, ==, 6);
  Req *named = req_at(&f, 4);
  g_assert_cmpint(named->opened_at - accepted_at, >=, 5);
  g_assert_cmpint(named->opened_at - accepted_at, <=, 60);
  g_assert_true(named->profiles);
  g_assert_cmpuint(named->authors->len, ==, 1);
  g_assert_true(req_asks(named, xavier->pk));
  answer(named, name, NULL);
  answer(req_at(&f, 5), NULL);
  drain();
  g_assert_cmpstr(gh_contact_directory_get_display_name(f.dir, xavier->pk), ==, "Xavier");
  g_assert_cmpuint(f.profiles->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(f.profiles, 0), ==, xavier->pk);
  g_autofree gchar *title = gh_contact_directory_dup_conversation_title(f.dir, request);
  g_assert_cmpstr(title, ==, "Xavier");
  resolved_clear(&r);
  fixture_down(&f);
}

/* ---- strict admission ------------------------------------------------------------ */

/* Forged, foreign, other-kind, future-dated and older lists never win;
 * newest wins by created_at; profile text is cleaned for display. */
static void
test_forged_and_stale(void)
{
  Fixture f;
  fixture_up(&f, TRUE);
  const Person *carol = &people[0], *mallory = &people[1];
  room_with(&f, carol, TRUE, T0 - 500);
  Resolved r = { 0 };
  resolve_start(&f, carol->pk, &r, NULL);
  drain();
  g_autofree gchar *good = inbox_list(carol, T0 - 100, R1, NULL);
  g_autofree gchar *older = inbox_list(carol, T0 - 200, R9, NULL);
  g_autofree gchar *future = inbox_list(carol, T0 + 3600, R8, NULL);
  g_autofree gchar *foreign = inbox_list(mallory, T0 - 10, R7, NULL);
  /* Mallory's list rewritten to claim Carol as its author, and Carol's own
   * list with a relay swapped in after signing. */
  g_autofree gchar *claimed = replace_once(foreign, mallory->pk, carol->pk);
  g_autofree gchar *tampered = replace_once(good, R1, R7);
  NostrTags *tags = nostr_tags_new(1, nostr_tag_new("relay", R7, NULL));
  g_autofree gchar *other_kind = signed_by(carol->sk, 10002, T0 - 5, tags, "");
  g_autofree gchar *name = profile(carol, T0 - 100,
    "{\"display_name\":\"\\u202ecarol\\tthe\\n\\u0007great\",\"nip05\":\"Carol@Example.org\"}");
  answer(req_at(&f, 0), claimed, tampered, other_kind, foreign, good, future, older, name, NULL);
  answer(req_at(&f, 1), NULL);
  gh_test_spin_until(is_done, &r);
  g_assert_cmpint(r.result->status, ==, GH_INBOX_FOUND);
  assert_relays(r.result, R1, NULL);
  g_assert_cmpint(r.result->created_at, ==, T0 - 100);
  g_assert_cmpstr(gh_contact_directory_get_display_name(f.dir, carol->pk), ==, "carol the great");
  g_assert_cmpstr(gh_contact_directory_get_nip05(f.dir, carol->pk), ==, "carol@example.org");
  g_assert_null(gh_contact_directory_get_display_name(f.dir, mallory->pk));

  /* A later refresh that only finds an older list changes nothing. */
  advance(&f, 24 * 3600 + 1);
  resolve(&f, carol->pk, &r);
  advance_to_next(&f); /* the refresh timer, before its deadline */
  Req *refresh = req_at(&f, f.rec.reqs->len - 2);
  g_autofree gchar *stale = inbox_list(carol, T0 - 150, R9, NULL);
  answer(refresh, stale, NULL);
  answer(req_at(&f, f.rec.reqs->len - 1), NULL);
  drain();
  g_assert_cmpuint(f.changed->len, ==, 0);
  resolve(&f, carol->pk, &r);
  assert_relays(r.result, R1, NULL);

  /* A list naming no usable relay is EMPTY, not an older list's relays. */
  gh_inbox_resolver_forget(GH_INBOX_RESOLVER(f.dir), carol->pk);
  resolve_start(&f, carol->pk, &r, NULL);
  drain();
  NostrTags *bad = nostr_tags_new(1, nostr_tag_new("relay", "http://r1.test.invalid", NULL));
  g_autofree gchar *unusable = signed_by(carol->sk, 10050, T0 + 20, bad, "");
  answer(req_at(&f, f.rec.reqs->len - 2), unusable, good, NULL);
  answer(req_at(&f, f.rec.reqs->len - 1), NULL);
  gh_test_spin_until(is_done, &r);
  g_assert_cmpint(r.result->status, ==, GH_INBOX_EMPTY);
  g_assert_null(r.result->relays);
  resolved_clear(&r);
  fixture_down(&f);
}

/* ---- restore across a restart ------------------------------------------------------ */

static void
test_restore(void)
{
  Fixture f;
  fixture_up(&f, TRUE);
  const Person *carol = &people[0], *mallory = &people[1], *eve = &people[2];
  room_with(&f, carol, TRUE, T0 - 500);
  Resolved r = { 0 };
  resolve_start(&f, carol->pk, &r, NULL);
  drain();
  g_autofree gchar *list = inbox_list(carol, T0 - 100, R1, R2, NULL);
  g_autofree gchar *name = profile(carol, T0 - 100, "{\"name\":\"Carol\"}");
  answer(req_at(&f, 0), list, name, NULL);
  answer(req_at(&f, 1), NULL);
  gh_test_spin_until(is_done, &r);
  /* A planted row that is not Eve's own signed list. */
  g_autofree gchar *foreign = inbox_list(mallory, T0 - 10, R7, NULL);
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(event, foreign, NULL), ==, 1);
  char *foreign_id = nostr_event_get_id(event);
  nostr_event_free(event);
  GhStoreDirectoryEntry planted = { eve->pk, 10050, foreign_id, T0 - 10, foreign, T0 };
  g_assert_true(gh_store_directory_put(f.store, &planted, NULL));
  free(foreign_id);

  /* Restart: a new directory over the reopened store. */
  g_assert_true(gh_contact_directory_set_store(f.dir, NULL, NULL));
  gh_test_release(f.dir);
  gh_store_close(f.store);
  f.store = NULL;
  advance(&f, 3600);
  guint reqs = f.rec.reqs->len;
  new_directory(&f);
  bind_store(&f);
  g_autoptr(GPtrArray) rows = gh_store_directory_load(f.store, NULL);
  g_assert_cmpuint(rows->len, ==, 2); /* Carol's 10050 and kind 0; Eve's row is gone */
  for (guint i = 0; i < rows->len; i++)
    g_assert_cmpstr(((GhStoreDirectoryEntry *)g_ptr_array_index(rows, i))->pubkey, ==, carol->pk);
  resolve(&f, carol->pk, &r);
  g_assert_true(r.result->cached);
  assert_relays(r.result, R1, R2, NULL);
  g_assert_cmpstr(gh_contact_directory_get_display_name(f.dir, carol->pk), ==, "Carol");
  g_assert_cmpuint(f.rec.reqs->len, ==, reqs);
  /* When it was fetched survived too: stale 24 h after the fetch, not after
   * the restart. */
  advance(&f, 23 * 3600);
  resolve(&f, carol->pk, &r);
  advance_to_next(&f); /* the refresh timer, before its deadline */
  g_assert_cmpuint(f.rec.reqs->len, ==, reqs + 2);
  resolved_clear(&r);
  fixture_down(&f);
}

/* ---- generation, sources ---------------------------------------------------------- */

static void
test_account_switch_and_sources(void)
{
  Fixture f;
  fixture_up(&f, TRUE);
  const Person *carol = &people[0];
  Resolved r = { 0 };
  resolve_start(&f, carol->pk, &r, NULL);
  drain();
  g_assert_cmpuint(f.rec.reqs->len, ==, 2);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_account_controller_select(f.accounts, npub_bob, &error));
  gh_test_spin_until(is_done, &r);
  g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_null(r.result);
  g_assert_true(req_at(&f, 0)->closed && req_at(&f, 1)->closed);
  /* Alice's store is not Bob's. */
  g_assert_false(gh_contact_directory_set_store(f.dir, f.store, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_clear_error(&error);
  /* No discovery relay: nothing to ask, nothing opened. */
  g_settings_set_strv(f.settings, "discovery-relays", NULL);
  guint reqs = f.rec.reqs->len;
  resolve(&f, carol->pk, &r);
  g_assert_cmpint(r.result->status, ==, GH_INBOX_NO_SOURCES);
  g_assert_cmpuint(f.rec.reqs->len, ==, reqs);
  /* A caller's cancellation ends its resolve (the lookup itself may go on:
   * another send to the same recipient would share it). */
  const gchar *sources[] = { DISC_A, NULL };
  g_settings_set_strv(f.settings, "discovery-relays", sources);
  g_autoptr(GCancellable) cancel = g_cancellable_new();
  resolve_start(&f, carol->pk, &r, cancel);
  drain();
  g_assert_cmpuint(f.rec.reqs->len, ==, reqs + 1);
  g_cancellable_cancel(cancel);
  gh_test_spin_until(is_done, &r);
  g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  resolved_clear(&r);
  fixture_down(&f);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  hex_alice = gh_test_pub(1);
  npub_alice = gh_test_npub(1);
  npub_bob = gh_test_npub(2);
  for (guint i = 0; i < N_PEOPLE; i++) {
    char *sk = nostr_key_generate_private();
    char *pk = nostr_key_get_public(sk);
    people[i].sk = g_strdup(sk);
    people[i].pk = g_strdup(pk);
    free(sk);
    free(pk);
  }
  gh_test_bus_up(&shared_bus);
  g_test_add_func("/groundhog/contact-directory/nt11-cache", test_nt11_cache);
  g_test_add_func("/groundhog/contact-directory/nt12-startup", test_nt12_startup);
  g_test_add_func("/groundhog/contact-directory/pt8-requests", test_pt8_requests);
  g_test_add_func("/groundhog/contact-directory/forged-and-stale", test_forged_and_stale);
  g_test_add_func("/groundhog/contact-directory/restore", test_restore);
  g_test_add_func("/groundhog/contact-directory/account-switch", test_account_switch_and_sources);
  int result = g_test_run();
  gh_test_bus_down(&shared_bus);
  for (guint i = 0; i < N_PEOPLE; i++) {
    g_free(people[i].sk);
    g_free(people[i].pk);
  }
  g_free(hex_alice);
  g_free(npub_alice);
  g_free(npub_bob);
  return result;
}
