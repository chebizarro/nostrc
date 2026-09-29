#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 /* memmem */
#endif

#include "gh-store-conversations.h"

#include "crash-harness.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib/gstdio.h>
#include <sodium.h>
#include <sqlite3.h>

#include "nostr-event.h"
#include "nostr-tag.h"

#ifndef GH_STORE_TEST_HOOKS
#error "test_store_conversations needs the store's test hooks (GH_STORE_TEST_HOOKS)"
#endif

/* G05 (privacy charter §8.2): GhStoreConversations, the encrypted store's
 * persistence delegate for GhConversationStore. ST-6 admission cut points
 * under the H8 crash harness, ST-7 idempotent admission, ST-9 forget, ST-12
 * legacy seen import, EX-4/EX-6 expiry at admission and no resurrection,
 * restart restore (order, unread and read markers, requests, names, drafts,
 * paged history), the rejected-wrap namespace, per-account isolation and an
 * H7-style canary scan of every file the tests leave. Each test runs in
 * isolated XDG directories; nothing sleeps (time is a fake GhClock, crashes
 * are SIGKILLs at named cut points). */

#define T0 ((gint64)1790000000)

static gchar *ACCOUNT_A, *ACCOUNT_B, *PEER_P, *PEER_Q, *PEER_R, *PEER_S;

static gchar *
hex_of(const gchar *seed)
{
  return g_compute_checksum_for_string(G_CHECKSUM_SHA256, seed, -1);
}

/* ---- Rumors ------------------------------------------------------------------------ */

typedef struct {
  const gchar *author;
  const gchar *to[4];   /* NULL-terminated p tags */
  gint64 created_at;
  const gchar *content;
  const gchar *subject;
  gint64 expiration;    /* rumor expiration tag; 0 none */
} Rumor;

static gchar *
rumor_json(const Rumor *r)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 14);
  nostr_event_set_pubkey(event, r->author);
  nostr_event_set_created_at(event, r->created_at);
  nostr_event_set_content(event, r->content ? r->content : "text");
  NostrTags *tags = nostr_tags_new(0);
  for (guint i = 0; r->to[i]; i++)
    nostr_tags_append(tags, nostr_tag_new("p", r->to[i], NULL));
  if (r->subject)
    nostr_tags_append(tags, nostr_tag_new("subject", r->subject, NULL));
  if (r->expiration) {
    g_autofree gchar *value = g_strdup_printf("%" G_GINT64_FORMAT, r->expiration);
    nostr_tags_append(tags, nostr_tag_new("expiration", value, NULL));
  }
  nostr_event_set_tags(event, tags);
  event->id = nostr_event_get_id(event);
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  gchar *copy = g_strdup(json);
  free(json);
  return copy;
}

static GhMessage *
message_new(const gchar *account, const Rumor *r)
{
  g_autofree gchar *json = rumor_json(r);
  g_autoptr(GError) error = NULL;
  GhMessage *message = gh_message_new_from_rumor(account, json, &error);
  g_assert_no_error(error);
  g_assert_nonnull(message);
  return message;
}

static gchar *
rumor_id(const gchar *account, const Rumor *r)
{
  g_autoptr(GhMessage) message = message_new(account, r);
  return g_strdup(gh_message_get_rumor_id(message));
}

static gint
compare_strings(gconstpointer a, gconstpointer b)
{
  return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

/* The canonical room id of a participant set. */
static gchar *
room_of(const gchar *a, const gchar *b, const gchar *c)
{
  g_autoptr(GPtrArray) members = g_ptr_array_new();
  g_ptr_array_add(members, (gpointer)a);
  if (b)
    g_ptr_array_add(members, (gpointer)b);
  if (c)
    g_ptr_array_add(members, (gpointer)c);
  g_ptr_array_sort(members, compare_strings);
  g_ptr_array_add(members, NULL);
  return g_strjoinv(",", (gchar **)members->pdata);
}

/* ---- Accounts, stores and the model ------------------------------------------------------ */

typedef struct {
  gchar *pubkey;
  GBytes *key;      /* the raw store key (a Secret Service item in the app) */
  gchar *store_id;
} TestAccount;

static void
test_account_init(TestAccount *account, const gchar *pubkey)
{
  guint8 raw[GH_STORE_KEY_SIZE];
  randombytes_buf(raw, sizeof raw);
  account->pubkey = g_strdup(pubkey);
  account->key = g_bytes_new(raw, sizeof raw);
  account->store_id = g_uuid_string_random();
  sodium_memzero(raw, sizeof raw);
}

static void
test_account_clear(TestAccount *account)
{
  g_clear_pointer(&account->pubkey, g_free);
  g_clear_pointer(&account->key, g_bytes_unref);
  g_clear_pointer(&account->store_id, g_free);
}

typedef struct {
  TestAccount account;
  GhClock *clock;       /* fake; survives restarts */
  guint page_size;
  GhStore *store;
  GhStoreConversations *conversations;
  GhConversationStore *model;
} Fixture;

static GhStore *
store_open(const TestAccount *account, GhClock *clock, GhStoreOpenFlags flags, GError **error)
{
  GhStoreConfig config = { .account_pubkey = account->pubkey, .clock = clock };
  return gh_store_open_with_key(&config, account->key, account->store_id, flags, error);
}

static void
fixture_open(Fixture *f)
{
  g_autoptr(GError) error = NULL;
  f->store = store_open(&f->account, f->clock, GH_STORE_OPEN_CREATE, &error);
  g_assert_no_error(error);
  g_assert_nonnull(f->store);
  f->conversations = gh_store_conversations_new(f->store);
  f->model = gh_conversation_store_new();
  g_assert_true(gh_store_conversations_attach(f->conversations, f->model, f->page_size, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(gh_conversation_store_get_account(f->model), ==, f->account.pubkey);
}

/* Quit: everything released, the store closed last. */
static void
fixture_close(Fixture *f)
{
  if (f->conversations)
    gh_store_conversations_close(f->conversations);
  g_clear_object(&f->model);
  g_clear_object(&f->conversations);
  g_clear_pointer(&f->store, gh_store_close);
}

static void
fixture_restart(Fixture *f)
{
  fixture_close(f);
  fixture_open(f);
}

static void
fixture_init(Fixture *f, const gchar *pubkey, guint page_size)
{
  memset(f, 0, sizeof *f);
  test_account_init(&f->account, pubkey);
  f->clock = gh_clock_new_fake(T0 * G_USEC_PER_SEC);
  f->page_size = page_size;
  fixture_open(f);
}

static void
fixture_clear(Fixture *f)
{
  fixture_close(f);
  g_clear_pointer(&f->clock, gh_clock_unref);
  test_account_clear(&f->account);
}

static GhConversationAddResult
deliver_message(GhConversationStore *model, GhMessage *message, const gchar *wrap_seed)
{
  g_autofree gchar *wrap = wrap_seed ? hex_of(wrap_seed) : NULL;
  return gh_conversation_store_admit(model, message, wrap, NULL);
}

/* A verified wrap (wrap_seed names its id) or, with NULL, the local echo. */
static GhConversationAddResult
deliver(GhConversationStore *model, const Rumor *r, const gchar *wrap_seed)
{
  g_autoptr(GhMessage) message = message_new(gh_conversation_store_get_account(model), r);
  return deliver_message(model, message, wrap_seed);
}

static GhConversation *
room(Fixture *f, const gchar *room_id)
{
  return gh_conversation_store_lookup(f->model, room_id);
}

static guint
n_items(gpointer model)
{
  return g_list_model_get_n_items(G_LIST_MODEL(model));
}

/* "room|room|..." in store order. */
static gchar *
room_order(GhConversationStore *model)
{
  GString *out = g_string_new(NULL);
  for (guint i = 0; i < n_items(model); i++) {
    g_autoptr(GhConversation) c = g_list_model_get_item(G_LIST_MODEL(model), i);
    g_string_append_printf(out, "%s%s", i ? "|" : "", gh_conversation_get_room_id(c));
  }
  return g_string_free(out, FALSE);
}

/* Everything the list shows of every room, in order. */
static gchar *
model_snapshot(GhConversationStore *model)
{
  GString *out = g_string_new(NULL);
  for (guint i = 0; i < n_items(model); i++) {
    g_autoptr(GhConversation) c = g_list_model_get_item(G_LIST_MODEL(model), i);
    g_string_append_printf(out, "%s title=%s preview=%s last=%" G_GINT64_FORMAT
                           " unread=%u request=%d n=%u\n",
                           gh_conversation_get_room_id(c), gh_conversation_get_title(c),
                           gh_conversation_get_preview(c), gh_conversation_get_last_activity(c),
                           gh_conversation_get_unread_count(c),
                           gh_conversation_get_is_request(c), n_items(c));
  }
  return g_string_free(out, FALSE);
}

/* ---- SQL inspection ------------------------------------------------------------------------ */

static gint64 sql_int(GhStore *store, const gchar *format, ...) G_GNUC_PRINTF(2, 3);

static gint64
sql_int(GhStore *store, const gchar *format, ...)
{
  va_list args;
  va_start(args, format);
  g_autofree gchar *sql = g_strdup_vprintf(format, args);
  va_end(args);
  sqlite3_stmt *stmt = NULL;
  g_assert_cmpint(sqlite3_prepare_v2(gh_store_get_db(store), sql, -1, &stmt, NULL), ==, SQLITE_OK);
  int rc = sqlite3_step(stmt);
  g_assert_true(rc == SQLITE_ROW || rc == SQLITE_DONE);
  gint64 value = rc == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : -1;
  sqlite3_finalize(stmt);
  return value;
}

static void
sql_exec(GhStore *store, const gchar *sql)
{
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_exec(store, sql, &error));
  g_assert_no_error(error);
}

static gboolean
is_seen(GhStore *store, GhStoreSeenNs ns, const gchar *id)
{
  gboolean seen = FALSE;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_seen_contains(store, ns, id, &seen, &error));
  g_assert_no_error(error);
  return seen;
}

static gint64
stored_messages(GhStore *store, const gchar *rumor)
{
  return sql_int(store, "SELECT count(*) FROM messages WHERE backend_msg_id = '%s'", rumor);
}

static gint64
stored_unread(GhStore *store, const gchar *room_id)
{
  return sql_int(store, "SELECT unread_count FROM conversations "
                 "WHERE backend = 1 AND backend_key = '%s'", room_id);
}

static void
assert_integrity(GhStore *store)
{
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_check_integrity(store, TRUE, &error));
  g_assert_no_error(error);
}

/* The durable invariants: every message has its rumor seen key; no own
 * message lies after its room's read marker (replying reads what came
 * before) and no room holding one is a request; every room's unread count is
 * the messages from others after its read marker. */
static void
assert_store_consistent(GhStore *store)
{
  g_assert_cmpint(sql_int(store,
    "SELECT count(*) FROM messages m WHERE NOT EXISTS "
    "(SELECT 1 FROM seen s WHERE s.ns = 2 AND s.id = m.backend_msg_id)"), ==, 0);
  g_assert_cmpint(sql_int(store,
    "SELECT count(*) FROM messages m JOIN conversations c ON c.id = m.conversation_id "
    "LEFT JOIN messages r ON r.id = c.last_read_msg WHERE m.direction = 1 AND "
    "(c.last_read_msg IS NULL OR m.created_at > r.created_at OR "
    "(m.created_at = r.created_at AND m.backend_msg_id > r.backend_msg_id))"), ==, 0);
  g_assert_cmpint(sql_int(store,
    "SELECT count(*) FROM conversations c WHERE c.request_state = 1 AND EXISTS "
    "(SELECT 1 FROM messages m WHERE m.conversation_id = c.id AND m.direction = 1)"), ==, 0);
  g_assert_cmpint(sql_int(store,
    "SELECT count(*) FROM conversations c WHERE c.unread_count != "
    "(SELECT count(*) FROM messages m LEFT JOIN messages r ON r.id = c.last_read_msg "
    "WHERE m.conversation_id = c.id AND m.direction = 0 AND (c.last_read_msg IS NULL OR "
    "m.created_at > r.created_at OR (m.created_at = r.created_at AND "
    "m.backend_msg_id > r.backend_msg_id)))"), ==, 0);
}

/* The model shows a room with n messages and this unread/request state, and
 * the store agrees on the unread count. */
static void
assert_room(Fixture *f, const gchar *room_id, guint n, guint unread, gboolean request)
{
  GhConversation *c = room(f, room_id);
  g_assert_nonnull(c);
  g_assert_cmpuint(n_items(c), ==, n);
  g_assert_cmpuint(gh_conversation_get_unread_count(c), ==, unread);
  g_assert_cmpint(gh_conversation_get_is_request(c), ==, request);
  g_assert_cmpint(stored_unread(f->store, room_id), ==, unread);
}

/* ---- ST-6: crash cut points of T-admit ------------------------------------------------------ */

typedef struct {
  TestAccount *account;
  const Rumor *rumor;
  const gchar *wrap_seed;
} CrashScript;

/* The child: open, attach, admit one message through the model. */
static void
script_admit(gpointer data)
{
  CrashScript *s = data;
  GhClock *clock = gh_clock_new_fake(T0 * G_USEC_PER_SEC);
  GhStore *store = store_open(s->account, clock, GH_STORE_OPEN_NONE, NULL);
  if (!store)
    _exit(3);
  GhStoreConversations *conversations = gh_store_conversations_new(store);
  GhConversationStore *model = gh_conversation_store_new();
  if (!gh_store_conversations_attach(conversations, model, 0, NULL))
    _exit(4);
  deliver(model, s->rumor, s->wrap_seed);
  /* Returning means the cut point was never reached (COMPLETED). */
}

static void
test_st6_crash_admit(void)
{
  /* Every cut point inside the delegate's one transaction: gh_store_admit()
   * runs as a savepoint of it, so its own commit points are the outer
   * transaction's ("txn"). */
  static const gchar *const cuts[] = {
    "admit:seen-wrap", "admit:seen-message", "admit:conversation", "admit:message",
    "admit:conversation-updated", "admit:participants",
    "txn:before-commit", "txn:after-commit", NULL
  };
  /* B is someone's message in a new room, or the account's own self-copy in
   * A's room, whose read marker and request state move in the same
   * transaction. */
  for (guint scenario = 0; scenario < 2; scenario++)
  for (guint i = 0; cuts[i]; i++) {
    const gboolean own = scenario == 1;
    g_autofree gchar *seed = g_strdup_printf("st6/%u/%s", scenario, cuts[i]);
    g_autofree gchar *account = hex_of(seed);
    Fixture f;
    fixture_init(&f, account, 0);
    Rumor a = { .author = PEER_P, .to = { account }, .created_at = T0 - 20, .content = "first" };
    g_assert_cmpint(deliver(f.model, &a, "st6/wrap-a"), ==, GH_CONVERSATION_ADD_NEW);
    fixture_close(&f);

    Rumor b = { .author = own ? account : PEER_Q, .to = { own ? PEER_P : account },
                .created_at = T0 - 10, .content = "second" };
    CrashScript script = { &f.account, &b, "st6/wrap-b" };
    GhCrashOutcome outcome = gh_crash_harness_run(cuts[i], 1, script_admit, &script);
    if (outcome != GH_CRASH_KILLED)
      g_error("%s: %s", cuts[i], gh_crash_outcome_to_string(outcome));

    fixture_open(&f);
    assert_integrity(f.store);
    const gboolean committed = g_str_equal(cuts[i], "txn:after-commit");
    g_autofree gchar *rumor_a = rumor_id(account, &a);
    g_autofree gchar *rumor_b = rumor_id(account, &b);
    g_autofree gchar *wrap_a = hex_of("st6/wrap-a");
    g_autofree gchar *wrap_b = hex_of("st6/wrap-b");
    g_autofree gchar *room_a = room_of(account, PEER_P, NULL);
    g_autofree gchar *room_b = room_of(account, own ? PEER_P : PEER_Q, NULL);
    /* A is untouched; B is all or nothing: message, both seen keys, room,
     * read state. */
    g_assert_cmpint(stored_messages(f.store, rumor_a), ==, 1);
    g_assert_true(is_seen(f.store, GH_STORE_SEEN_WRAP, wrap_a));
    g_assert_cmpint(stored_messages(f.store, rumor_b), ==, committed);
    g_assert_cmpint(is_seen(f.store, GH_STORE_SEEN_RUMOR, rumor_b), ==, committed);
    g_assert_cmpint(is_seen(f.store, GH_STORE_SEEN_WRAP, wrap_b), ==, committed);
    g_assert_cmpint(sql_int(f.store, "SELECT count(*) FROM conversations"), ==,
                    1 + (committed && !own));
    g_assert_cmpint(sql_int(f.store, "SELECT count(*) FROM seen WHERE ns = 1"), ==, 1 + committed);
    g_assert_cmpint(sql_int(f.store, "SELECT count(*) FROM seen WHERE ns = 2"), ==, 1 + committed);
    assert_store_consistent(f.store);
    /* The restored model agrees, and the pre-check skips B only if stored. */
    if (own)
      assert_room(&f, room_a, 1 + committed, committed ? 0 : 1, !committed);
    else
      assert_room(&f, room_a, 1, 1, TRUE);
    g_assert_cmpint(gh_conversation_store_has_message(f.model, rumor_b), ==, committed);
    g_assert_cmpint(gh_conversation_store_has_wrap(f.model, wrap_b), ==, committed);

    /* Relays deliver B again: it ends up stored and listed exactly once. */
    g_assert_cmpint(deliver(f.model, &b, "st6/wrap-b"), ==,
                    committed ? GH_CONVERSATION_ADD_DUPLICATE : GH_CONVERSATION_ADD_NEW);
    g_assert_cmpint(stored_messages(f.store, rumor_b), ==, 1);
    if (own)
      assert_room(&f, room_a, 2, 0, FALSE);
    else
      assert_room(&f, room_b, 1, 1, TRUE);
    assert_store_consistent(f.store);
    fixture_clear(&f);
  }
}

/* ---- ST-7: idempotent admission -------------------------------------------------------------- */

static void
test_st7_idempotent(void)
{
  Fixture f;
  fixture_init(&f, ACCOUNT_A, 0);
  Rumor r = { .author = PEER_P, .to = { ACCOUNT_A }, .created_at = T0 - 10, .content = "once" };
  g_autofree gchar *id = rumor_id(ACCOUNT_A, &r);
  g_autofree gchar *pair = room_of(ACCOUNT_A, PEER_P, NULL);
  g_assert_cmpint(deliver(f.model, &r, "st7/1"), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(deliver(f.model, &r, "st7/2"), ==, GH_CONVERSATION_ADD_DUPLICATE);
  g_assert_cmpint(stored_messages(f.store, id), ==, 1);
  g_assert_cmpint(sql_int(f.store, "SELECT count(*) FROM seen WHERE ns = 1"), ==, 2);
  g_autofree gchar *wrap1 = hex_of("st7/1");
  g_autofree gchar *wrap2 = hex_of("st7/2");
  g_assert_true(gh_conversation_store_has_wrap(f.model, wrap1));
  g_assert_true(gh_conversation_store_has_wrap(f.model, wrap2));
  assert_room(&f, pair, 1, 1, TRUE);

  fixture_restart(&f);
  assert_room(&f, pair, 1, 1, TRUE);
  g_assert_true(gh_conversation_store_has_wrap(f.model, wrap2));
  /* A third wrap after the restart: its id is recorded, nothing else. */
  g_assert_cmpint(deliver(f.model, &r, "st7/3"), ==, GH_CONVERSATION_ADD_DUPLICATE);
  g_assert_cmpint(stored_messages(f.store, id), ==, 1);
  g_assert_cmpint(sql_int(f.store, "SELECT count(*) FROM seen WHERE ns = 1"), ==, 3);
  assert_room(&f, pair, 1, 1, TRUE);
  assert_store_consistent(f.store);
  fixture_clear(&f);
}

/* ---- Restart restores order, unread, requests, names and drafts -------------------------------- */

static gchar *
draft_of(Fixture *f, const gchar *room_id)
{
  gchar *draft = NULL;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_conversations_get_draft(f->conversations, room_id, &draft, &error));
  g_assert_no_error(error);
  return draft;
}

static void
set_draft(Fixture *f, const gchar *room_id, const gchar *draft)
{
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_conversations_set_draft(f->conversations, room_id, draft, &error));
  g_assert_no_error(error);
}

static void
test_restart_restores(void)
{
  Fixture f;
  fixture_init(&f, ACCOUNT_A, 0);
  g_autofree gchar *ap = room_of(ACCOUNT_A, PEER_P, NULL);
  g_autofree gchar *aq = room_of(ACCOUNT_A, PEER_Q, NULL);
  g_autofree gchar *ar = room_of(ACCOUNT_A, PEER_R, NULL);
  g_autofree gchar *as = room_of(ACCOUNT_A, PEER_S, NULL);
  g_autofree gchar *apq = room_of(ACCOUNT_A, PEER_P, PEER_Q);

  /* A conversation: a named thread, a reply (read marker), one new message. */
  Rumor ap1 = { .author = PEER_P, .to = { ACCOUNT_A }, .created_at = T0 - 100, .content = "hello" };
  Rumor ap2 = { .author = PEER_P, .to = { ACCOUNT_A }, .created_at = T0 - 90,
                .content = "about the trip", .subject = "Plans" };
  Rumor ap3 = { .author = ACCOUNT_A, .to = { PEER_P }, .created_at = T0 - 80, .content = "sounds good" };
  Rumor ap4 = { .author = PEER_P, .to = { ACCOUNT_A }, .created_at = T0 - 70, .content = "see you" };
  g_assert_cmpint(deliver(f.model, &ap1, "ap1"), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(deliver(f.model, &ap2, "ap2"), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(deliver(f.model, &ap3, "ap3"), ==, GH_CONVERSATION_ADD_NEW); /* self-copy */
  g_assert_cmpint(deliver(f.model, &ap4, "ap4"), ==, GH_CONVERSATION_ADD_NEW);
  assert_room(&f, ap, 4, 1, FALSE);
  g_assert_cmpstr(gh_conversation_get_title(room(&f, ap)), ==, "Plans");
  /* A request the user accepts. */
  Rumor aq1 = { .author = PEER_Q, .to = { ACCOUNT_A }, .created_at = T0 - 60, .content = "hi" };
  Rumor aq2 = { .author = PEER_Q, .to = { ACCOUNT_A }, .created_at = T0 - 50, .content = "there?" };
  g_assert_cmpint(deliver(f.model, &aq1, "aq1"), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(deliver(f.model, &aq2, "aq2"), ==, GH_CONVERSATION_ADD_NEW);
  assert_room(&f, aq, 2, 2, TRUE);
  gh_conversation_accept(room(&f, aq));
  g_assert_cmpint(sql_int(f.store, "SELECT request_state FROM conversations WHERE backend_key = '%s'",
                          aq), ==, GH_STORE_REQUEST_ACCEPTED);
  /* A request the user read but did not accept. */
  Rumor ar1 = { .author = PEER_R, .to = { ACCOUNT_A }, .created_at = T0 - 40, .content = "spam?" };
  g_assert_cmpint(deliver(f.model, &ar1, "ar1"), ==, GH_CONVERSATION_ADD_NEW);
  gh_conversation_mark_read(room(&f, ar));
  assert_room(&f, ar, 1, 0, TRUE);
  /* A group request. */
  Rumor apq1 = { .author = PEER_P, .to = { ACCOUNT_A, PEER_Q }, .created_at = T0 - 30,
                 .content = "group hello", .subject = "Group" };
  g_assert_cmpint(deliver(f.model, &apq1, "apq1"), ==, GH_CONVERSATION_ADD_NEW);
  assert_room(&f, apq, 1, 1, TRUE);

  /* Drafts: one per room, a room that is not stored yet, a cleared one. */
  set_draft(&f, ap, "draft ✍ for P");
  set_draft(&f, as, "hi S");
  set_draft(&f, ar, "maybe");
  set_draft(&f, ar, "");
  g_autoptr(GError) error = NULL;
  g_autofree gchar *not_mine = room_of(PEER_P, PEER_Q, NULL);
  g_assert_false(gh_store_conversations_set_draft(f.conversations, not_mine, "x", &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
  g_clear_error(&error);
  /* Room ids are canonical: sorted, unique, including the account. */
  g_autofree gchar *unsorted = strcmp(ACCOUNT_A, PEER_P) < 0
                                 ? g_strconcat(PEER_P, ",", ACCOUNT_A, NULL)
                                 : g_strconcat(ACCOUNT_A, ",", PEER_P, NULL);
  g_assert_false(gh_store_conversations_set_draft(f.conversations, unsorted, "x", &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
  g_clear_error(&error);
  g_autofree gchar *twice = g_strconcat(ACCOUNT_A, ",", ACCOUNT_A, NULL);
  g_assert_false(gh_store_conversations_set_draft(f.conversations, twice, "x", &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
  g_clear_error(&error);
  gchar *no_draft = NULL;
  g_assert_false(gh_store_conversations_get_draft(f.conversations, "abc", &no_draft, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
  g_assert_null(no_draft);
  g_clear_error(&error);

  g_autofree gchar *order = room_order(f.model);
  g_autofree gchar *expected_order = g_strjoin("|", apq, ar, aq, ap, NULL);
  g_assert_cmpstr(order, ==, expected_order);
  g_autofree gchar *before = model_snapshot(f.model);
  assert_store_consistent(f.store);

  fixture_restart(&f);
  g_autofree gchar *after = model_snapshot(f.model);
  g_assert_cmpstr(after, ==, before);
  g_assert_null(room(&f, as)); /* a draft alone lists nothing */
  g_autofree gchar *draft_ap = draft_of(&f, ap);
  g_autofree gchar *draft_as = draft_of(&f, as);
  g_autofree gchar *draft_ar = draft_of(&f, ar);
  g_autofree gchar *draft_aq = draft_of(&f, aq);
  g_assert_cmpstr(draft_ap, ==, "draft ✍ for P");
  g_assert_cmpstr(draft_as, ==, "hi S");
  g_assert_null(draft_ar);
  g_assert_null(draft_aq);
  /* The draft-only room was created as accepted: composing is own intent. */
  g_assert_cmpint(sql_int(f.store, "SELECT request_state FROM conversations WHERE backend_key = '%s'",
                          as), ==, GH_STORE_REQUEST_ACCEPTED);

  /* The restored read markers and request states keep working. */
  Rumor ap5 = { .author = PEER_P, .to = { ACCOUNT_A }, .created_at = T0 - 20, .content = "one more" };
  Rumor ar2 = { .author = PEER_R, .to = { ACCOUNT_A }, .created_at = T0 - 15, .content = "again" };
  Rumor aq3 = { .author = PEER_Q, .to = { ACCOUNT_A }, .created_at = T0 - 12, .content = "thanks" };
  g_assert_cmpint(deliver(f.model, &ap5, "ap5"), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(deliver(f.model, &ar2, "ar2"), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(deliver(f.model, &aq3, "aq3"), ==, GH_CONVERSATION_ADD_NEW);
  assert_room(&f, ap, 5, 2, FALSE);
  assert_room(&f, ar, 2, 1, TRUE);
  assert_room(&f, aq, 3, 3, FALSE);
  /* Replying in the group (local echo) accepts it and reads what came before. */
  Rumor apq2 = { .author = ACCOUNT_A, .to = { PEER_P, PEER_Q }, .created_at = T0 - 5,
                 .content = "reply" };
  g_assert_cmpint(deliver(f.model, &apq2, NULL), ==, GH_CONVERSATION_ADD_NEW);
  assert_room(&f, apq, 2, 0, FALSE);
  gh_conversation_mark_read(room(&f, aq));
  assert_room(&f, aq, 3, 0, FALSE);
  g_autofree gchar *later = model_snapshot(f.model);

  fixture_restart(&f);
  g_autofree gchar *restored = model_snapshot(f.model);
  g_assert_cmpstr(restored, ==, later);
  g_autofree gchar *order2 = room_order(f.model);
  g_autofree gchar *expected_order2 = g_strjoin("|", apq, aq, ar, ap, NULL);
  g_assert_cmpstr(order2, ==, expected_order2);
  assert_store_consistent(f.store);
  fixture_clear(&f);
}

/* ---- Paged history --------------------------------------------------------------------------- */

static void
test_paging(void)
{
  Fixture f;
  fixture_init(&f, ACCOUNT_A, 10);
  g_autofree gchar *ap = room_of(ACCOUNT_A, PEER_P, NULL);
  for (guint i = 0; i < 60; i++) {
    g_autofree gchar *text = g_strdup_printf("message %u", i);
    g_autofree gchar *wrap = g_strdup_printf("paging/%u", i);
    /* Even seconds, so the own message below never ties with one. */
    Rumor r = { .author = PEER_P, .to = { ACCOUNT_A }, .created_at = T0 - 1000 + 2 * i,
                .content = text, .subject = i == 0 ? "Old name" : NULL };
    g_assert_cmpint(deliver(f.model, &r, wrap), ==, GH_CONVERSATION_ADD_NEW);
  }
  assert_room(&f, ap, 60, 60, TRUE);

  /* The newest page is listed; unread and the name cover the unloaded rest. */
  fixture_restart(&f);
  GhConversation *c = room(&f, ap);
  assert_room(&f, ap, 10, 60, TRUE);
  g_assert_true(gh_conversation_get_has_older(c));
  /* A request: the stored name is its subject, never its title (§7.9). */
  g_assert_cmpstr(gh_conversation_get_subject(c), ==, "Old name");
  g_assert_true(g_str_has_prefix(gh_conversation_get_title(c), "npub1"));
  g_assert_cmpstr(gh_conversation_get_preview(c), ==, "message 59");
  guint loaded = 0;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_conversations_load_older(f.conversations, c, 20, &loaded, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(loaded, ==, 20);
  assert_room(&f, ap, 30, 60, TRUE);
  g_autoptr(GhMessage) oldest = g_list_model_get_item(G_LIST_MODEL(c), 0);
  g_assert_cmpstr(gh_message_get_content(oldest), ==, "message 30");

  /* An own message older than the listed history (a late self-copy) is
   * committed into it: it reads everything before it, and is listed once
   * that history is loaded. It sorts between messages 19 and 20. */
  Rumor own = { .author = ACCOUNT_A, .to = { PEER_P }, .created_at = T0 - 961, .content = "old reply" };
  g_autofree gchar *own_id = rumor_id(ACCOUNT_A, &own);
  g_assert_cmpint(deliver(f.model, &own, "paging/own"), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_false(gh_conversation_store_has_message(f.model, own_id));
  assert_room(&f, ap, 30, 40, FALSE);
  /* An older incoming one before the read marker stays read. */
  Rumor early = { .author = PEER_P, .to = { ACCOUNT_A }, .created_at = T0 - 2000, .content = "early" };
  g_assert_cmpint(deliver(f.model, &early, "paging/early"), ==, GH_CONVERSATION_ADD_NEW);
  assert_room(&f, ap, 30, 40, FALSE);

  g_assert_true(gh_store_conversations_load_older(f.conversations, c, 1000, &loaded, &error));
  g_assert_cmpuint(loaded, ==, 32);
  g_assert_false(gh_conversation_get_has_older(c));
  assert_room(&f, ap, 62, 40, FALSE);
  g_assert_true(gh_conversation_store_has_message(f.model, own_id));
  g_assert_true(gh_store_conversations_load_older(f.conversations, c, 5, &loaded, &error));
  g_assert_cmpuint(loaded, ==, 0);

  gh_conversation_mark_read(c);
  assert_room(&f, ap, 62, 0, FALSE);
  fixture_restart(&f);
  c = room(&f, ap);
  assert_room(&f, ap, 10, 0, FALSE);
  g_assert_true(gh_conversation_get_has_older(c));
  Rumor next = { .author = PEER_P, .to = { ACCOUNT_A }, .created_at = T0, .content = "newest" };
  g_assert_cmpint(deliver(f.model, &next, "paging/next"), ==, GH_CONVERSATION_ADD_NEW);
  assert_room(&f, ap, 11, 1, FALSE);

  /* Bounds: page size, and only rooms of the attached model. */
  g_assert_false(gh_store_conversations_load_older(f.conversations, c, 0, &loaded, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
  g_clear_error(&error);
  g_autoptr(GhConversationStore) other = gh_conversation_store_new();
  gh_conversation_store_set_account(other, ACCOUNT_A, NULL, NULL, NULL);
  Rumor stray = { .author = PEER_P, .to = { ACCOUNT_A }, .created_at = T0 - 1, .content = "x" };
  g_assert_cmpint(deliver(other, &stray, NULL), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_false(gh_store_conversations_load_older(f.conversations,
                                                   gh_conversation_store_lookup(other, ap),
                                                   5, &loaded, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
  g_clear_error(&error);
  assert_store_consistent(f.store);
  fixture_clear(&f);
}

/* Restored messages are verified again from the stored rumor JSON: a row
 * that does not match its id is skipped (and logged), never shown. */
static void
test_restore_verifies(void)
{
  Fixture f;
  fixture_init(&f, ACCOUNT_A, 0);
  g_autofree gchar *ap = room_of(ACCOUNT_A, PEER_P, NULL);
  Rumor r1 = { .author = PEER_P, .to = { ACCOUNT_A }, .created_at = T0 - 20, .content = "genuine" };
  Rumor r2 = { .author = PEER_P, .to = { ACCOUNT_A }, .created_at = T0 - 10, .content = "tampered" };
  g_autofree gchar *id2 = rumor_id(ACCOUNT_A, &r2);
  g_assert_cmpint(deliver(f.model, &r1, "verify/1"), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(deliver(f.model, &r2, "verify/2"), ==, GH_CONVERSATION_ADD_NEW);
  g_autofree gchar *sql = g_strdup_printf(
    "UPDATE messages SET raw_json = replace(raw_json, 'tampered', 'rewritten') "
    "WHERE backend_msg_id = '%s'", id2);
  sql_exec(f.store, sql);
  fixture_close(&f);
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*failed verification*");
  fixture_open(&f);
  g_test_assert_expected_messages();
  GhConversation *c = room(&f, ap);
  g_assert_nonnull(c);
  g_assert_cmpuint(n_items(c), ==, 1);
  g_assert_cmpstr(gh_conversation_get_preview(c), ==, "genuine");
  fixture_clear(&f);
}

/* ---- ST-9: forget conversation ------------------------------------------------------------------ */

static void
test_st9_forget(void)
{
  Fixture f;
  fixture_init(&f, ACCOUNT_A, 0);
  g_autofree gchar *ap = room_of(ACCOUNT_A, PEER_P, NULL);
  Rumor one = { .author = PEER_P, .to = { ACCOUNT_A }, .created_at = T0 - 100, .content = "one" };
  Rumor two = { .author = PEER_P, .to = { ACCOUNT_A }, .created_at = T0 - 90, .content = "two" };
  g_assert_cmpint(deliver(f.model, &one, "st9/1"), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(deliver(f.model, &two, "st9/2"), ==, GH_CONVERSATION_ADD_NEW);
  set_draft(&f, ap, "unsent");
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_conversations_forget(f.conversations, ap, &error));
  g_assert_no_error(error);
  g_assert_null(room(&f, ap));
  g_assert_cmpuint(n_items(f.model), ==, 0);
  g_assert_cmpint(sql_int(f.store, "SELECT count(*) FROM messages"), ==, 0);
  g_autofree gchar *draft = draft_of(&f, ap);
  g_assert_null(draft);

  /* Backfill of old wraps (a re-wrap of a known rumor, and an unseen rumor
   * older than the tombstone) does not recreate it. */
  g_assert_cmpint(deliver(f.model, &one, "st9/1-again"), ==, GH_CONVERSATION_ADD_DUPLICATE);
  Rumor old = { .author = PEER_P, .to = { ACCOUNT_A }, .created_at = T0 - 95, .content = "unseen" };
  g_autofree gchar *old_id = rumor_id(ACCOUNT_A, &old);
  g_assert_cmpint(deliver(f.model, &old, "st9/old"), ==, GH_CONVERSATION_ADD_HIDDEN);
  g_assert_true(is_seen(f.store, GH_STORE_SEEN_RUMOR, old_id));
  g_assert_cmpint(stored_messages(f.store, old_id), ==, 0);
  g_assert_null(room(&f, ap));
  fixture_restart(&f);
  g_assert_null(room(&f, ap));

  /* A new message starts it again with only that message. */
  Rumor fresh = { .author = PEER_P, .to = { ACCOUNT_A }, .created_at = T0 + 10, .content = "fresh" };
  g_assert_cmpint(deliver(f.model, &fresh, "st9/fresh"), ==, GH_CONVERSATION_ADD_NEW);
  assert_room(&f, ap, 1, 1, TRUE);
  fixture_restart(&f);
  assert_room(&f, ap, 1, 1, TRUE);
  g_assert_cmpstr(gh_conversation_get_preview(room(&f, ap)), ==, "fresh");

  g_autofree gchar *unknown = room_of(ACCOUNT_A, PEER_S, NULL);
  g_assert_false(gh_store_conversations_forget(f.conversations, unknown, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND);
  assert_store_consistent(f.store);
  fixture_clear(&f);
}

/* ---- EX-4 / EX-6: expiry at admission, no resurrection ------------------------------------------- */

static void
test_expiry(void)
{
  Fixture f;
  fixture_init(&f, ACCOUNT_A, 0);
  g_autofree gchar *ap = room_of(ACCOUNT_A, PEER_P, NULL);

  /* Expired on arrival (rumor tag, and exactly now): seen only, never shown,
   * no room created. */
  Rumor gone = { .author = PEER_P, .to = { ACCOUNT_A }, .created_at = T0 - 10,
                 .content = "gone", .expiration = T0 - 1 };
  Rumor edge = { .author = PEER_P, .to = { ACCOUNT_A }, .created_at = T0 - 9,
                 .content = "edge", .expiration = T0 };
  g_autofree gchar *gone_id = rumor_id(ACCOUNT_A, &gone);
  g_autofree gchar *gone_wrap = hex_of("ex/gone");
  g_assert_cmpint(deliver(f.model, &gone, "ex/gone"), ==, GH_CONVERSATION_ADD_HIDDEN);
  g_assert_cmpint(deliver(f.model, &edge, "ex/edge"), ==, GH_CONVERSATION_ADD_HIDDEN);
  g_assert_true(is_seen(f.store, GH_STORE_SEEN_RUMOR, gone_id));
  g_assert_true(gh_conversation_store_has_wrap(f.model, gone_wrap));
  g_assert_cmpint(sql_int(f.store, "SELECT count(*) FROM messages"), ==, 0);
  g_assert_cmpint(sql_int(f.store, "SELECT count(*) FROM conversations"), ==, 0);
  g_assert_cmpuint(n_items(f.model), ==, 0);
  /* The seal's or wrap's expiration (set from the unwrap) counts the same. */
  Rumor sealed = { .author = PEER_P, .to = { ACCOUNT_A }, .created_at = T0 - 8, .content = "sealed" };
  g_autoptr(GhMessage) sealed_message = message_new(ACCOUNT_A, &sealed);
  gh_message_set_expires_at(sealed_message, T0 - 5);
  g_assert_cmpint(deliver_message(f.model, sealed_message, "ex/sealed"), ==,
                  GH_CONVERSATION_ADD_HIDDEN);
  /* Backfill of another wrap of it does not bring it back. */
  g_assert_cmpint(deliver(f.model, &gone, "ex/gone-again"), ==, GH_CONVERSATION_ADD_DUPLICATE);
  g_assert_cmpuint(n_items(f.model), ==, 0);

  /* Not yet expired: stored with its expiry (rumor tag, or seal/wrap). */
  Rumor minute = { .author = PEER_P, .to = { ACCOUNT_A }, .created_at = T0 - 5,
                   .content = "for a minute", .expiration = T0 + 60 };
  g_autofree gchar *minute_id = rumor_id(ACCOUNT_A, &minute);
  g_assert_cmpint(deliver(f.model, &minute, "ex/minute"), ==, GH_CONVERSATION_ADD_NEW);
  Rumor longer = { .author = PEER_P, .to = { ACCOUNT_A }, .created_at = T0 - 4, .content = "longer" };
  g_autofree gchar *longer_id = rumor_id(ACCOUNT_A, &longer);
  g_autoptr(GhMessage) longer_message = message_new(ACCOUNT_A, &longer);
  gh_message_set_expires_at(longer_message, T0 + 120);
  g_assert_cmpint(deliver_message(f.model, longer_message, "ex/longer"), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(sql_int(f.store, "SELECT expires_at FROM messages WHERE backend_msg_id = '%s'",
                          longer_id), ==, T0 + 120);
  fixture_restart(&f);
  assert_room(&f, ap, 2, 2, TRUE);
  GhMessage *restored = gh_conversation_store_lookup_message(f.model, longer_id);
  g_assert_nonnull(restored);
  g_assert_cmpint(gh_message_get_expires_at(restored), ==, T0 + 120);

  /* Expired while stored: not restored, and gone for good once purged. */
  gh_clock_fake_advance(f.clock, 90 * G_USEC_PER_SEC);
  fixture_restart(&f);
  g_assert_null(gh_conversation_store_lookup_message(f.model, minute_id));
  g_assert_cmpuint(n_items(room(&f, ap)), ==, 1);
  GhStorePurgeStats stats;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_purge(f.store, 0, &stats, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(stats.n_expired, ==, 1);
  g_assert_cmpint(deliver(f.model, &minute, "ex/minute-again"), ==, GH_CONVERSATION_ADD_DUPLICATE);
  g_assert_cmpint(stored_messages(f.store, minute_id), ==, 0);
  fixture_restart(&f);
  g_assert_null(gh_conversation_store_lookup_message(f.model, minute_id));
  g_assert_nonnull(gh_conversation_store_lookup_message(f.model, longer_id));
  fixture_clear(&f);
}

/* ---- ST-12: legacy seen import -------------------------------------------------------------------- */

static gchar *
seen_file(const gchar *account, const gchar *const *lines, const gchar *tail)
{
  GString *out = g_string_new("groundhog-nip17-seen 1 ");
  g_string_append_printf(out, "%s\n", account);
  for (guint i = 0; lines[i]; i++)
    g_string_append_printf(out, "%s\n", lines[i]);
  if (tail)
    g_string_append(out, tail);
  return g_string_free(out, FALSE);
}

static void
write_private(const gchar *path, const gchar *contents)
{
  g_autofree gchar *dir = g_path_get_dirname(path);
  g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
  g_autoptr(GError) error = NULL;
  g_assert_true(g_file_set_contents_full(path, contents, -1, G_FILE_SET_CONTENTS_CONSISTENT,
                                         0600, &error));
  g_assert_no_error(error);
}

static void
import_ok(Fixture *f, const gchar *path, guint rejected, guint dropped)
{
  GhStoreSeenImport stats = { 99, 99 };
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_conversations_import_seen_file(f->conversations, path, &stats, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(stats.rejected, ==, rejected);
  g_assert_cmpuint(stats.dropped, ==, dropped);
  g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
}

static void
import_refused(Fixture *f, const gchar *path, gint code)
{
  g_autoptr(GError) error = NULL;
  GhStoreSeenImport stats;
  g_assert_false(gh_store_conversations_import_seen_file(f->conversations, path, &stats, &error));
  g_assert_error(error, GH_STORE_ERROR, code);
  g_assert_cmpuint(stats.rejected + stats.dropped, ==, 0);
  g_assert_true(g_file_test(path, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_SYMLINK));
}

static void
test_st12_legacy_seen(void)
{
  Fixture f;
  fixture_init(&f, ACCOUNT_A, 0);
  g_autofree gchar *path = gh_store_conversations_legacy_seen_path(NULL, ACCOUNT_A);
  g_autofree gchar *name = g_strconcat(ACCOUNT_A, ".seen", NULL);
  g_autofree gchar *expected = g_build_filename(g_get_user_state_dir(), "groundhog", "nip17",
                                                name, NULL);
  g_assert_cmpstr(path, ==, expected);

  Rumor known = { .author = PEER_P, .to = { ACCOUNT_A }, .created_at = T0 - 50, .content = "seen" };
  g_autofree gchar *r1 = rumor_id(ACCOUNT_A, &known);
  g_autofree gchar *w1 = hex_of("legacy/w1");
  g_autofree gchar *w2 = hex_of("legacy/w2");
  g_autofree gchar *x1 = hex_of("legacy/x1");
  g_autofree gchar *lw1 = g_strconcat("w ", w1, NULL);
  g_autofree gchar *lw2 = g_strconcat("w ", w2, NULL);
  g_autofree gchar *lr1 = g_strconcat("r ", r1, NULL);
  g_autofree gchar *lx1 = g_strconcat("x ", x1, NULL);
  const gchar *const lines[] = { lw1, lr1, lx1, lw2, lw1, NULL };
  /* The torn final append of a crash is ignored, as GhNip17Seen did. */
  g_autofree gchar *contents = seen_file(ACCOUNT_A, lines, "w 12ab");
  write_private(path, contents);
  /* Only the rejected id is imported (W13 review B1): the file's inbox kept
   * its messages in memory only, so its "w"/"r" keys would hide messages no
   * store holds although the relays still have them. */
  import_ok(&f, path, 1, 4);
  g_assert_true(gh_conversation_store_has_rejected(f.model, x1));
  g_assert_false(gh_conversation_store_has_wrap(f.model, x1));
  g_assert_false(gh_conversation_store_has_wrap(f.model, w1));
  g_assert_false(gh_conversation_store_has_wrap(f.model, w2));
  g_assert_false(gh_conversation_store_has_rejected(f.model, w1));
  g_assert_cmpint(sql_int(f.store, "SELECT count(*) FROM seen WHERE ns IN (1, 2)"), ==, 0);
  /* So the relays' copy of that very message, fetched again, is stored and
   * listed; the rejected wrap stays skipped before any signer call. */
  g_assert_cmpint(deliver(f.model, &known, "legacy/w1"), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpuint(n_items(f.model), ==, 1);
  g_assert_nonnull(gh_conversation_store_lookup_message(f.model, r1));
  g_assert_true(gh_conversation_store_has_wrap(f.model, w1));

  /* Idempotent: nothing left to import, and a re-created file adds nothing. */
  import_ok(&f, path, 0, 0);
  write_private(path, contents);
  import_ok(&f, path, 0, 4);
  g_assert_cmpuint(n_items(f.model), ==, 1);

  /* Refused and kept: another account's file, a malformed one, a symlink. */
  g_autofree gchar *x2 = hex_of("legacy/x2");
  g_autofree gchar *lx2 = g_strconcat("x ", x2, NULL);
  const gchar *const foreign_lines[] = { lx2, NULL };
  g_autofree gchar *foreign = seen_file(ACCOUNT_B, foreign_lines, NULL);
  write_private(path, foreign);
  import_refused(&f, path, GH_STORE_ERROR_FOREIGN);
  g_autofree gchar *upper = g_ascii_strup(w1, -1);
  g_autofree gchar *bad_line = g_strconcat("w ", upper, NULL);
  const gchar *const malformed_lines[] = { lx2, bad_line, NULL };
  g_autofree gchar *malformed = seen_file(ACCOUNT_A, malformed_lines, NULL);
  write_private(path, malformed);
  import_refused(&f, path, GH_STORE_ERROR_INVALID);
  g_assert_false(gh_conversation_store_has_rejected(f.model, x2));
  g_assert_cmpint(g_unlink(path), ==, 0);
  g_autofree gchar *elsewhere = g_build_filename(g_get_user_state_dir(), "planted.seen", NULL);
  write_private(elsewhere, contents);
  g_assert_cmpint(symlink(elsewhere, path), ==, 0);
  import_refused(&f, path, GH_STORE_ERROR_PERMISSIONS);
  g_assert_true(g_file_test(elsewhere, G_FILE_TEST_IS_REGULAR));
  g_assert_cmpint(g_unlink(path), ==, 0);

  /* Durable from here on: the stored message comes back, w2 still not seen. */
  fixture_restart(&f);
  g_assert_true(gh_conversation_store_has_wrap(f.model, w1));
  g_assert_false(gh_conversation_store_has_wrap(f.model, w2));
  g_assert_true(gh_conversation_store_has_rejected(f.model, x1));
  g_assert_nonnull(gh_conversation_store_lookup_message(f.model, r1));
  fixture_clear(&f);
}

/* ---- The rejected-wrap namespace ------------------------------------------------------------------- */

static void
test_rejected_namespace(void)
{
  Fixture f;
  fixture_init(&f, ACCOUNT_A, 0);
  g_autofree gchar *x = hex_of("rejected/1");
  g_autoptr(GError) error = NULL;
  g_assert_false(gh_conversation_store_has_rejected(f.model, x));
  g_assert_true(gh_conversation_store_record_rejected(f.model, x, &error));
  g_assert_no_error(error);
  g_assert_true(gh_conversation_store_record_rejected(f.model, x, &error));
  g_assert_true(gh_conversation_store_has_rejected(f.model, x));
  g_assert_false(gh_conversation_store_has_wrap(f.model, x));
  g_assert_cmpint(sql_int(f.store, "SELECT count(*) FROM seen WHERE ns = %d",
                          GH_STORE_SEEN_REJECTED_WRAP), ==, 1);
  g_assert_false(gh_conversation_store_record_rejected(f.model, "not-a-wrap-id", &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
  g_clear_error(&error);
  fixture_restart(&f);
  g_assert_true(gh_conversation_store_has_rejected(f.model, x));

  /* A memory-only model keeps no rejected namespace. */
  g_autoptr(GhConversationStore) memory = gh_conversation_store_new();
  gh_conversation_store_set_account(memory, ACCOUNT_A, NULL, NULL, NULL);
  g_assert_true(gh_conversation_store_record_rejected(memory, x, &error));
  g_assert_false(gh_conversation_store_has_rejected(memory, x));
  fixture_clear(&f);
}

/* ---- Per-account isolation --------------------------------------------------------------------- */

static void
test_account_isolation(void)
{
  Fixture a, b;
  fixture_init(&a, ACCOUNT_A, 0);
  fixture_init(&b, ACCOUNT_B, 0);
  g_autofree gchar *ap = room_of(ACCOUNT_A, PEER_P, NULL);
  g_autofree gchar *bp = room_of(ACCOUNT_B, PEER_P, NULL);
  Rumor to_a = { .author = PEER_P, .to = { ACCOUNT_A }, .created_at = T0 - 10, .content = "for A" };
  Rumor to_b = { .author = PEER_P, .to = { ACCOUNT_B }, .created_at = T0 - 5, .content = "for B" };
  g_assert_cmpint(deliver(a.model, &to_a, "iso/a"), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(deliver(b.model, &to_b, "iso/b"), ==, GH_CONVERSATION_ADD_NEW);
  g_autofree gchar *wrap_a = hex_of("iso/a");

  /* Switching A's model to B drops A's rooms before B's appear. */
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_conversations_attach(b.conversations, a.model, 0, &error));
  g_assert_no_error(error);
  g_assert_cmpstr(gh_conversation_store_get_account(a.model), ==, ACCOUNT_B);
  g_assert_null(gh_conversation_store_lookup(a.model, ap));
  g_assert_nonnull(gh_conversation_store_lookup(a.model, bp));
  g_assert_cmpuint(n_items(a.model), ==, 1);
  g_assert_false(gh_conversation_store_has_wrap(a.model, wrap_a));
  /* A's message is refused by B, and B's store never saw A's keys. */
  g_autoptr(GhMessage) for_a = message_new(ACCOUNT_A, &to_a);
  g_assert_cmpint(gh_conversation_store_admit(a.model, for_a, wrap_a, &error), ==,
                  GH_CONVERSATION_ADD_REJECTED);
  g_clear_error(&error);
  g_assert_cmpint(sql_int(b.store, "SELECT count(*) FROM seen WHERE ns = 1"), ==, 1);
  g_assert_cmpint(sql_int(b.store, "SELECT count(*) FROM messages"), ==, 1);
  g_assert_false(gh_store_conversations_set_draft(b.conversations, ap, "no", &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
  g_clear_error(&error);
  g_autofree gchar *legacy_a = gh_store_conversations_legacy_seen_path(NULL, ACCOUNT_A);
  g_autofree gchar *lw = g_strconcat("w ", wrap_a, NULL);
  const gchar *const lines[] = { lw, NULL };
  g_autofree gchar *contents = seen_file(ACCOUNT_A, lines, NULL);
  write_private(legacy_a, contents);
  import_refused(&b, legacy_a, GH_STORE_ERROR_FOREIGN);

  /* Back to A: its room returns from A's store. */
  g_assert_true(gh_store_conversations_attach(a.conversations, a.model, 0, &error));
  g_assert_no_error(error);
  g_assert_null(gh_conversation_store_lookup(a.model, bp));
  assert_room(&a, ap, 1, 1, TRUE);
  g_assert_true(gh_conversation_store_has_wrap(a.model, wrap_a));
  assert_room(&b, bp, 1, 1, TRUE);
  fixture_clear(&b);
  fixture_clear(&a);
}

/* ---- Local echo of a message the outbox stored (T-enqueue) ---------------------------------------- */

static void
test_local_echo_after_enqueue(void)
{
  Fixture f;
  fixture_init(&f, ACCOUNT_A, 0);
  g_autofree gchar *ap = room_of(ACCOUNT_A, PEER_P, NULL);
  Rumor in = { .author = PEER_P, .to = { ACCOUNT_A }, .created_at = T0 - 20, .content = "question" };
  g_assert_cmpint(deliver(f.model, &in, "echo/in"), ==, GH_CONVERSATION_ADD_NEW);
  assert_room(&f, ap, 1, 1, TRUE);

  Rumor out = { .author = ACCOUNT_A, .to = { PEER_P }, .created_at = T0 - 10, .content = "answer" };
  g_autofree gchar *json = rumor_json(&out);
  g_autofree gchar *id = rumor_id(ACCOUNT_A, &out);
  g_autofree gchar *op_id = gh_store_new_op_id();
  g_autoptr(GError) error = NULL;
  gint64 conversation_id = 0, outbox_id = 0, message_row = 0;
  g_assert_true(gh_store_find_conversation(f.store, GH_STORE_BACKEND_NIP17, ap, &conversation_id,
                                           &error));
  GhStoreOutgoing outgoing = {
    .conversation_id = conversation_id, .op_id = op_id, .backend_msg_id = id,
    .sender_pubkey = ACCOUNT_A, .kind = 14, .created_at = T0 - 10, .body = "answer",
    .rumor_json = json,
  };
  g_assert_true(gh_store_enqueue(f.store, &outgoing, &outbox_id, &message_row, &error));
  g_assert_no_error(error);
  /* The echo of the enqueued message is listed, once, and reads the room. */
  g_assert_cmpint(deliver(f.model, &out, NULL), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(stored_messages(f.store, id), ==, 1);
  assert_room(&f, ap, 2, 0, FALSE);
  g_assert_cmpint(sql_int(f.store, "SELECT last_read_msg FROM conversations WHERE id = %"
                          G_GINT64_FORMAT, conversation_id), ==, message_row);
  /* Its self-copy coming back is a duplicate. */
  g_assert_cmpint(deliver(f.model, &out, "echo/self-copy"), ==, GH_CONVERSATION_ADD_DUPLICATE);
  fixture_restart(&f);
  assert_room(&f, ap, 2, 0, FALSE);
  assert_store_consistent(f.store);
  fixture_clear(&f);
}

/* ---- After close ------------------------------------------------------------------------------------ */

static void
test_closed(void)
{
  Fixture f;
  fixture_init(&f, ACCOUNT_A, 0);
  g_autofree gchar *ap = room_of(ACCOUNT_A, PEER_P, NULL);
  Rumor first = { .author = PEER_P, .to = { ACCOUNT_A }, .created_at = T0 - 20, .content = "first" };
  g_assert_cmpint(deliver(f.model, &first, "closed/1"), ==, GH_CONVERSATION_ADD_NEW);
  g_autofree gchar *wrap = hex_of("closed/1");
  gh_store_conversations_close(f.conversations);
  gh_store_conversations_close(f.conversations);

  /* Nothing is committed or marked seen; admissions fail and are retried. */
  Rumor second = { .author = PEER_P, .to = { ACCOUNT_A }, .created_at = T0 - 10, .content = "second" };
  g_autoptr(GhMessage) message = message_new(ACCOUNT_A, &second);
  g_autofree gchar *wrap2 = hex_of("closed/2");
  g_autoptr(GError) error = NULL;
  g_assert_cmpint(gh_conversation_store_admit(f.model, message, wrap2, &error), ==,
                  GH_CONVERSATION_ADD_FAILED);
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_STATE);
  g_clear_error(&error);
  g_assert_false(gh_conversation_store_has_wrap(f.model, wrap));
  g_assert_false(gh_conversation_store_record_rejected(f.model, wrap2, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_STATE);
  g_clear_error(&error);
  gchar *draft = NULL;
  g_assert_false(gh_store_conversations_get_draft(f.conversations, ap, &draft, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_STATE);
  g_clear_error(&error);
  g_assert_false(gh_store_conversations_attach(f.conversations, f.model, 0, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_STATE);
  g_clear_error(&error);
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*read position*");
  gh_conversation_mark_read(room(&f, ap));
  g_test_assert_expected_messages();
  g_assert_cmpint(stored_unread(f.store, ap), ==, 1);
  g_assert_cmpint(sql_int(f.store, "SELECT count(*) FROM seen WHERE ns = 1"), ==, 1);
  fixture_clear(&f);
}

/* ---- Canary scan (H7) --------------------------------------------------------------------------------- */

static void
scan_tree(const gchar *dir, const gchar *const *needles, guint *n_files)
{
  GDir *handle = g_dir_open(dir, 0, NULL);
  if (!handle)
    return;
  const gchar *name;
  while ((name = g_dir_read_name(handle))) {
    g_autofree gchar *path = g_build_filename(dir, name, NULL);
    GStatBuf st;
    g_assert_cmpint(g_lstat(path, &st), ==, 0);
    if (S_ISDIR(st.st_mode)) {
      scan_tree(path, needles, n_files);
      continue;
    }
    if (!S_ISREG(st.st_mode))
      continue;
    g_autofree gchar *contents = NULL;
    gsize length = 0;
    g_assert_true(g_file_get_contents(path, &contents, &length, NULL));
    for (guint i = 0; needles[i]; i++)
      if (memmem(contents, length, needles[i], strlen(needles[i])))
        g_error("%s holds plaintext \"%s\"", path, needles[i]);
    if (length >= 16 && memcmp(contents, "SQLite format 3", 16) == 0)
      g_error("%s is an unencrypted SQLite database", path);
    (*n_files)++;
  }
  g_dir_close(handle);
}

static guint
scan_user_dirs(const gchar *const *needles)
{
  guint n_files = 0;
  scan_tree(g_get_user_data_dir(), needles, &n_files);
  scan_tree(g_get_user_state_dir(), needles, &n_files);
  scan_tree(g_get_user_cache_dir(), needles, &n_files);
  scan_tree(g_get_user_config_dir(), needles, &n_files);
  return n_files;
}

static void
test_canary_scan(void)
{
  Fixture f;
  fixture_init(&f, ACCOUNT_A, 0);
  g_autofree gchar *nonce = gh_store_new_op_id();
  g_autofree gchar *canary = g_strdup_printf("GROUNDHOG-CANARY-%s", nonce);
  g_autofree gchar *ap = room_of(ACCOUNT_A, PEER_P, NULL);
  g_autoptr(GPtrArray) needles = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(needles, g_strdup(canary));
  g_ptr_array_add(needles, g_strdup(ACCOUNT_A));
  g_ptr_array_add(needles, g_strdup(PEER_P));
  for (guint i = 0; i < 30; i++) {
    g_autofree gchar *body = g_strdup_printf("%s message %u", canary, i);
    g_autofree gchar *wrap_seed = g_strdup_printf("canary/%u", i);
    Rumor r = { .author = i % 3 ? PEER_P : ACCOUNT_A, .to = { i % 3 ? ACCOUNT_A : PEER_P },
                .created_at = T0 - 100 + i, .content = body, .subject = canary };
    g_assert_cmpint(deliver(f.model, &r, wrap_seed), ==, GH_CONVERSATION_ADD_NEW);
    g_ptr_array_add(needles, hex_of(wrap_seed));
    g_ptr_array_add(needles, rumor_id(ACCOUNT_A, &r));
  }
  g_autofree gchar *draft = g_strdup_printf("draft %s", canary);
  set_draft(&f, ap, draft);
  g_autofree gchar *rejected = hex_of("canary/rejected");
  g_assert_true(gh_conversation_store_record_rejected(f.model, rejected, NULL));
  g_ptr_array_add(needles, g_strdup(rejected));
  /* The legacy plaintext seen file's rejected ids are imported, and the file
   * is removed. */
  g_autofree gchar *legacy_wrap = hex_of("canary/legacy");
  g_autofree gchar *line = g_strconcat("x ", legacy_wrap, NULL);
  const gchar *const lines[] = { line, NULL };
  g_autofree gchar *contents = seen_file(ACCOUNT_A, lines, NULL);
  g_autofree gchar *legacy = gh_store_conversations_legacy_seen_path(NULL, ACCOUNT_A);
  write_private(legacy, contents);
  import_ok(&f, legacy, 1, 0);
  g_ptr_array_add(needles, g_strdup(legacy_wrap));
  g_ptr_array_add(needles, NULL);

  /* While open the new pages sit in the WAL; after close in store.db. */
  g_autofree gchar *wal = g_strconcat(gh_store_get_path(f.store), "-wal", NULL);
  g_assert_true(g_file_test(wal, G_FILE_TEST_IS_REGULAR));
  g_assert_cmpuint(scan_user_dirs((const gchar *const *)needles->pdata), >=, 2);
  fixture_close(&f);
  g_assert_cmpuint(scan_user_dirs((const gchar *const *)needles->pdata), >=, 1);
  fixture_open(&f);
  /* The last own message is i = 27; i = 28 and 29 are unread. */
  assert_room(&f, ap, 30, 2, FALSE);
  fixture_clear(&f);
}

int
main(int argc, char **argv)
{
  umask(022);
  g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
  if (sodium_init() < 0)
    g_error("libsodium failed to initialize");
  ACCOUNT_A = hex_of("account-a");
  ACCOUNT_B = hex_of("account-b");
  PEER_P = hex_of("peer-p");
  PEER_Q = hex_of("peer-q");
  PEER_R = hex_of("peer-r");
  PEER_S = hex_of("peer-s");

  g_test_add_func("/groundhog/store-conversations/st6/crash-admit", test_st6_crash_admit);
  g_test_add_func("/groundhog/store-conversations/st7/idempotent", test_st7_idempotent);
  g_test_add_func("/groundhog/store-conversations/restart/restores", test_restart_restores);
  g_test_add_func("/groundhog/store-conversations/restart/paging", test_paging);
  g_test_add_func("/groundhog/store-conversations/restart/verifies", test_restore_verifies);
  g_test_add_func("/groundhog/store-conversations/st9/forget", test_st9_forget);
  g_test_add_func("/groundhog/store-conversations/ex4-ex6/expiry", test_expiry);
  g_test_add_func("/groundhog/store-conversations/st12/legacy-seen", test_st12_legacy_seen);
  g_test_add_func("/groundhog/store-conversations/rejected-namespace", test_rejected_namespace);
  g_test_add_func("/groundhog/store-conversations/account-isolation", test_account_isolation);
  g_test_add_func("/groundhog/store-conversations/local-echo", test_local_echo_after_enqueue);
  g_test_add_func("/groundhog/store-conversations/closed", test_closed);
  g_test_add_func("/groundhog/store-conversations/canary-scan", test_canary_scan);
  int status = g_test_run();
  g_free(ACCOUNT_A);
  g_free(ACCOUNT_B);
  g_free(PEER_P);
  g_free(PEER_Q);
  g_free(PEER_R);
  g_free(PEER_S);
  return status;
}
