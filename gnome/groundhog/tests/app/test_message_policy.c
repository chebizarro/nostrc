#include "gh-message-policy.h"
#include "gh-conversation-private.h"
#include "gh-store-conversations.h"
#include "gh-store-nip29.h"
#include "gh-store-mls.h"
#include "gh-store-reactions.h"
#include "gh-reaction-store.h"
#include <jansson.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <nostr-event.h>
#include <string.h>

#define ALICE "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
#define BOB "c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5"
#define BOB_SK "0000000000000000000000000000000000000000000000000000000000000002"
#define WRAP "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"

typedef struct {
  GhStore *store;
  GhConversationStore *model;
  GhStoreConversations *dm;
  GhStoreNip29 *nip29;
  GhStoreMls *mls;
  GhMessagePolicy *policy;
} Fixture;

static void
setup(Fixture *f, gconstpointer unused)
{
  (void)unused;
  GError *error = NULL;
  f->store = gh_store_open_ephemeral(ALICE, NULL, &error);
  g_assert_no_error(error);
  g_assert_nonnull(f->store);
  f->model = gh_conversation_store_new();
  f->dm = gh_store_conversations_new(f->store);
  g_assert_true(gh_store_conversations_attach(f->dm, f->model, 0, &error));
  f->nip29 = gh_store_nip29_new(f->store);
  f->mls = gh_store_mls_new(f->store);
  g_assert_true(gh_store_nip29_attach(f->nip29, f->model, 0, &error));
  g_assert_true(gh_store_mls_attach(f->mls, f->model, 0, &error));
  g_assert_no_error(error);
  f->policy = gh_conversation_store_get_policy(f->model);
  g_assert_true(gh_message_policy_is_loaded(f->policy));
}
static void
teardown(Fixture *f, gconstpointer unused)
{
  (void)unused;
  gh_conversation_store_set_account(f->model, NULL, NULL, NULL, NULL);
  gh_store_conversations_close(f->dm);
  gh_store_nip29_close(f->nip29);
  gh_store_mls_close(f->mls);
  g_object_unref(f->dm);
  g_object_unref(f->nip29);
  g_object_unref(f->mls);
  g_object_unref(f->model);
  gh_store_close(f->store);
  while (g_main_context_iteration(NULL, FALSE))
    ;
}
static gint64
scalar(Fixture *f, const gchar *sql)
{
  sqlite3_stmt *stmt = NULL;
  g_assert_cmpint(sqlite3_prepare_v2(gh_store_get_db(f->store), sql, -1, &stmt, NULL), ==,
                  SQLITE_OK);
  g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_ROW);
  gint64 n = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return n;
}
static void
drain_projection(Fixture *f)
{
  while (gh_message_policy_is_updating(f->policy)) {
    g_assert_null(gh_message_policy_get_error(f->policy));
    g_main_context_iteration(NULL, TRUE);
  }
  g_assert_null(gh_message_policy_get_error(f->policy));
}
static void
policy(Fixture *f, const gchar *name, const gchar *value, gboolean wait)
{
  GError *error = NULL;
  g_assert_true(gh_store_exec(f->store, "DELETE FROM social_mutes", &error));
  if (name) {
    sqlite3_stmt *stmt = NULL;
    g_assert_cmpint(
        sqlite3_prepare_v2(gh_store_get_db(f->store),
                           "INSERT INTO social_mutes(entry_key,tag_name,value,position,is_private) "
                           "VALUES(?1,?1,?2,0,1)",
                           -1, &stmt, NULL),
        ==, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, value, -1, SQLITE_STATIC);
    g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_DONE);
    sqlite3_finalize(stmt);
  }
  g_assert_true(gh_message_policy_refresh(f->policy, &error));
  g_assert_no_error(error);
  if (wait)
    drain_projection(f);
}
static GhMessage *
message(guint backend, const gchar *body, gint64 at, const gchar *extra)
{
  GError *error = NULL;
  g_autofree gchar *tags
      = g_strdup_printf("[%s%s%s]",
                        backend == 1   ? "[\"p\",\"" ALICE "\"]"
                        : backend == 2 ? "[\"h\",\"hide-test\"]"
                                       : "",
                        extra && *extra && backend != 3 ? "," : "", extra ? extra : "");
  json_t *event
      = json_pack("{s:s,s:i,s:I,s:s,s:o}", "pubkey", BOB, "kind", backend == 1 ? 14 : 9,
                  "created_at", (json_int_t)at, "content", body, "tags", json_loads(tags, 0, NULL));
  g_autofree gchar *wire = json_dumps(event, JSON_COMPACT);
  json_decref(event);
  GhMessage *m = NULL;
  if (backend == 1)
    m = gh_message_new_from_rumor(ALICE, wire, &error);
  else if (backend == 3)
    m = gh_message_new_from_mls(ALICE, "aabb", wire, &error);
  else {
    NostrEvent *signed_event = nostr_event_new();
    g_assert_cmpint(nostr_event_deserialize_unsigned(signed_event, wire, NULL), ==,
                    NOSTR_EVENT_VALIDATION_OK);
    g_assert_cmpint(nostr_event_sign(signed_event, BOB_SK), ==, 0);
    gchar *signed_wire = nostr_event_serialize_compact(signed_event);
    m = gh_message_new_from_nip29_event(ALICE, "wss://nos.lol", signed_wire, &error);
    free(signed_wire);
    nostr_event_free(signed_event);
  }
  g_assert_no_error(error);
  g_assert_nonnull(m);
  return m;
}
static void
test_dispositions(Fixture *f, gconstpointer unused)
{
  (void)unused;
  GError *error = NULL;
  for (guint backend = 1; backend <= 3; backend++) {
    policy(f, "p", BOB, TRUE);
    g_assert_false(gh_message_policy_is_updating(f->policy)); /* indexed, no mask */
    g_autoptr(GhMessage) dropped = message(backend, "not retained", 100 + backend, NULL);
    g_assert_cmpint(gh_conversation_store_admit(f->model, dropped, WRAP, &error), ==,
                    GH_CONVERSATION_ADD_HIDDEN);
    g_assert_no_error(error);
    g_assert_cmpint(scalar(f, "SELECT count(*) FROM messages WHERE body='not retained'"), ==, 0);
    policy(f, NULL, NULL, TRUE);
    g_assert_cmpint(gh_conversation_store_admit(f->model, dropped, WRAP, &error), ==,
                    GH_CONVERSATION_ADD_DUPLICATE);
    g_assert_no_error(error);
    g_assert_cmpint(scalar(f, "SELECT count(*) FROM messages WHERE body='not retained'"), ==, 0);
    const gchar *names[] = { "word", "t", "e" };
    for (guint which = 0; which < 3; which++) {
      g_autoptr(GhMessage) hidden = message(backend, "Café #Quiet", 200 + backend * 10 + which,
                                            which == 1 ? "[\"t\",\"Quiet\"]" : NULL);
      const gchar *value = which == 0   ? "CAFE\314\201"
                           : which == 1 ? "#quiet"
                                        : gh_message_get_rumor_id(hidden);
      policy(f, names[which], value, TRUE);
      g_assert_cmpint(gh_conversation_store_admit(f->model, hidden, WRAP, &error), ==,
                      GH_CONVERSATION_ADD_HIDDEN);
      g_assert_no_error(error);
      g_assert_cmpint(scalar(f, "SELECT count(*) FROM messages WHERE social_hidden=1"), >, 0);
      policy(f, NULL, NULL, TRUE);
      g_assert_nonnull(
          gh_conversation_store_lookup_message(f->model, gh_message_get_rumor_id(hidden)));
      g_assert_cmpint(scalar(f, "SELECT count(*) FROM messages WHERE social_hidden=1"), ==, 0);
    }
  }
}
static void
test_matching(Fixture *f, gconstpointer unused)
{
  (void)unused;
  GError *error = NULL;
  g_autoptr(GhMessage) root = message(1, "root", 500, NULL);
  g_autofree gchar *reply
      = g_strdup_printf("[\"e\",\"%s\",\"\",\"reply\"]", gh_message_get_rumor_id(root));
  g_autofree gchar *quote = g_strdup_printf("[\"q\",\"%s\"]", gh_message_get_rumor_id(root));
  g_autoptr(GhMessage) child = message(1, "child", 501, reply);
  g_autoptr(GhMessage) quoted = message(1, "quote", 502, quote);
  policy(f, "e", gh_message_get_rumor_id(root), TRUE);
  GhMessageDisposition d;
  g_assert_true(gh_message_policy_classify(f->policy, child, &d, &error));
  g_assert_cmpint(d, ==, GH_MESSAGE_HIDE_RETAIN);
  g_assert_true(gh_message_policy_classify(f->policy, quoted, &d, &error));
  g_assert_cmpint(d, ==, GH_MESSAGE_INCLUDE);
  policy(f, "word", "secret", TRUE);
  g_autoptr(GhMessage) url = message(1, "https://example.com/secret caption", 503, NULL);
  g_assert_true(gh_message_policy_classify(f->policy, url, &d, &error));
  g_assert_cmpint(d, ==, GH_MESSAGE_INCLUDE);
  policy(f, "t", "quiet", TRUE);
  g_autoptr(GhMessage) boundary = message(1, "#quietly", 504, NULL);
  g_assert_true(gh_message_policy_classify(f->policy, boundary, &d, &error));
  g_assert_cmpint(d, ==, GH_MESSAGE_INCLUDE);
  g_assert_no_error(error);
}
static void
test_cancel_and_recount(Fixture *f, gconstpointer unused)
{
  (void)unused;
  GError *error = NULL;
  for (guint i = 0; i < 270; i++) {
    g_autoptr(GhMessage) m = message(1, i % 2 ? "red" : "blue", 1000 + i, NULL);
    g_assert_cmpint(gh_conversation_store_admit(f->model, m, WRAP, &error), ==,
                    GH_CONVERSATION_ADD_NEW);
    g_assert_no_error(error);
  }
  policy(f, "word", "red", FALSE);
  g_assert_true(gh_conversation_store_get_visibility_updating(f->model));
  g_autoptr(GhConversation) room = g_list_model_get_item(G_LIST_MODEL(f->model), 0);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(room)), ==, 0);
  g_assert_cmpstr(gh_conversation_get_preview(room), ==, "Updating Hidden Messages…");
  g_main_context_iteration(NULL, FALSE); /* at most one 256-row turn */
  policy(f, "word", "blue", FALSE);      /* invalidates the unfinished red epoch */
  drain_projection(f);
  g_assert_false(gh_conversation_store_get_visibility_updating(f->model));
  g_assert_cmpint(scalar(f, "SELECT count(*) FROM messages WHERE social_hidden=1 AND body='blue'"),
                  ==, 135);
  g_assert_cmpint(scalar(f, "SELECT count(*) FROM messages WHERE social_hidden=1 AND body='red'"),
                  ==, 0);
  g_assert_cmpint(scalar(f, "SELECT unread_count FROM conversations WHERE backend=1"), ==, 135);
  GHashTable *rooms = NULL;
  g_assert_true(gh_store_search_message_rooms(f->store, "blue", &rooms, &error));
  g_assert_cmpuint(g_hash_table_size(rooms), ==, 0);
  g_hash_table_unref(rooms);
  g_assert_true(gh_store_read_state_update(f->store,
                                           scalar(f, "SELECT id FROM conversations LIMIT 1"),
                                           GH_STORE_READ_RECOUNT, 0, 0, NULL, &error));
  g_assert_cmpint(scalar(f, "SELECT unread_count FROM conversations WHERE backend=1"), ==, 135);
  g_assert_no_error(error);
}
static void
count_arrival(GhConversationStore *model, GhConversation *room, GhMessage *message, guint *count)
{
  (void)model;
  (void)room;
  (void)message;
  (*count)++;
}
static void
test_epoch_arrivals(Fixture *f, gconstpointer unused)
{
  (void)unused;
  GError *error = NULL;
  g_autoptr(GhMessage) old = message(1, "secret old", 2000, NULL);
  g_assert_cmpint(gh_conversation_store_admit(f->model, old, WRAP, &error), ==,
                  GH_CONVERSATION_ADD_NEW);
  guint arrivals = 0;
  g_signal_connect(f->model, "message-added", G_CALLBACK(count_arrival), &arrivals);
  policy(f, "word", "secret", FALSE);
  GHashTable *rooms = NULL;
  g_assert_true(gh_store_search_message_rooms(f->store, "secret", &rooms, &error));
  g_assert_cmpuint(g_hash_table_size(rooms), ==, 0);
  g_hash_table_unref(rooms);
  g_autoptr(GhMessage) fresh = message(1, "fresh visible", 2001, NULL);
  g_autoptr(GhMessage) hidden = message(1, "secret new", 2002, NULL);
  g_assert_cmpint(gh_conversation_store_admit(f->model, fresh, WRAP, &error), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(gh_conversation_store_admit(f->model, hidden, WRAP, &error), ==,
                  GH_CONVERSATION_ADD_HIDDEN);
  g_assert_cmpuint(arrivals, ==, 0);
  drain_projection(f);
  g_assert_cmpuint(arrivals, ==, 1);
  policy(f, NULL, NULL, TRUE);
  g_assert_cmpuint(arrivals, ==, 1); /* unhidden history never becomes an arrival */
  g_signal_handlers_disconnect_by_data(f->model, &arrivals);
  g_assert_no_error(error);
}
static void
test_projection_retry(Fixture *f, gconstpointer unused)
{
  (void)unused;
  GError *error = NULL;
  g_autoptr(GhMessage) m = message(1, "retry history", 2100, NULL);
  g_assert_cmpint(gh_conversation_store_admit(f->model, m, WRAP, &error), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_assert_true(gh_store_exec(
      f->store,
      "INSERT INTO social_mutes(entry_key,tag_name,value,position,is_private) VALUES('p','p','" BOB
      "',0,1);"
      "CREATE TRIGGER fail_visibility BEFORE UPDATE OF social_hidden ON messages BEGIN SELECT "
      "RAISE(ABORT,'fixture failure'); END",
      &error));
  g_assert_false(gh_message_policy_refresh(f->policy, &error));
  g_assert_nonnull(error);
  g_clear_error(&error);
  g_assert_false(gh_message_policy_is_loaded(f->policy));
  g_assert_cmpint(scalar(f, "SELECT social_hidden FROM messages"), ==, 0);
  g_assert_true(gh_store_exec(f->store, "DROP TRIGGER fail_visibility", &error));
  g_assert_true(gh_message_policy_refresh(f->policy, &error));
  g_assert_false(gh_message_policy_is_updating(f->policy));
  g_assert_cmpint(scalar(f, "SELECT social_hidden FROM messages"), ==, 1);
  g_assert_true(
      gh_store_exec(f->store,
                    "DELETE FROM social_mutes; CREATE TRIGGER fail_visibility BEFORE UPDATE OF "
                    "social_hidden ON messages BEGIN SELECT RAISE(ABORT,'fixture failure'); END",
                    &error));
  g_assert_false(gh_message_policy_refresh(f->policy, &error));
  g_assert_nonnull(error);
  g_clear_error(&error);
  g_assert_cmpint(scalar(f, "SELECT social_hidden FROM messages"), ==, 1);
  g_assert_true(gh_store_exec(f->store, "DROP TRIGGER fail_visibility", &error));
  g_assert_true(gh_message_policy_refresh(f->policy, &error));
  g_assert_cmpint(scalar(f, "SELECT social_hidden FROM messages"), ==, 0);

  g_assert_true(gh_store_exec(f->store,
                              "CREATE TRIGGER fail_work BEFORE INSERT ON social_visibility_work "
                              "BEGIN SELECT RAISE(ABORT,'fixture failure'); END",
                              &error));
  policy(f, "word", "retry", FALSE);
  while (!gh_message_policy_get_error(f->policy))
    g_main_context_iteration(NULL, TRUE);
  g_assert_true(gh_message_policy_is_updating(f->policy));
  GHashTable *rooms = NULL;
  g_assert_true(gh_store_search_message_rooms(f->store, "retry", &rooms, &error));
  g_assert_cmpuint(g_hash_table_size(rooms), ==, 0);
  g_hash_table_unref(rooms);
  g_assert_true(gh_store_exec(f->store, "DROP TRIGGER fail_work", &error));
  g_assert_true(gh_message_policy_refresh(f->policy, &error));
  drain_projection(f);
  g_assert_cmpint(scalar(f, "SELECT social_hidden FROM messages"), ==, 1);
  g_assert_no_error(error);
}
static void
test_indexed_author_cache(Fixture *f, gconstpointer unused)
{
  (void)unused;
  GError *error = NULL;
  g_autoptr(GhMessage) m = message(1, "retained secret", 2200, NULL);
  g_assert_cmpint(gh_conversation_store_admit(f->model, m, WRAP, &error), ==,
                  GH_CONVERSATION_ADD_NEW);
  policy(f, "word", "secret", TRUE);
  /* A p-only edit must not parse this hidden payload, even when p is removed
   * while a non-p reason still hides it. Visible-window reloads cannot read it. */
  g_assert_true(gh_store_exec(
      f->store,
      "UPDATE messages SET raw_json='not JSON'; INSERT INTO "
      "social_mutes(entry_key,tag_name,value,position,is_private) VALUES('p','p','" BOB "',0,1)",
      &error));
  g_assert_true(gh_message_policy_refresh(f->policy, &error));
  g_assert_false(gh_message_policy_is_updating(f->policy));
  g_assert_true(gh_store_exec(f->store, "DELETE FROM social_mutes WHERE tag_name='p'", &error));
  g_assert_true(gh_message_policy_refresh(f->policy, &error));
  g_assert_false(gh_message_policy_is_updating(f->policy));
  g_assert_cmpint(scalar(f, "SELECT social_hidden FROM messages"), ==, 1);
  g_assert_no_error(error);
}
static gchar *
state_text(Fixture *f, guint backend)
{
  g_autofree gchar *sql = g_strdup_printf(
      "SELECT quote(unread_count)||'|'||quote(title)||'|'||quote(last_activity)||'|'||"
      "quote(last_read_msg)||'|'||quote(read_seq)||'|'||quote(reply_read_at)||'|'||quote(reply_"
      "read_id)||'|'||"
      "quote(request_state)||'|'||quote(draft)||'|'||quote(disappearing_s)||'|'||quote(timer_"
      "changed_at) "
      "FROM conversations WHERE backend=%u LIMIT 1",
      backend);
  sqlite3_stmt *stmt = NULL;
  g_assert_cmpint(sqlite3_prepare_v2(gh_store_get_db(f->store), sql, -1, &stmt, NULL), ==,
                  SQLITE_OK);
  g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_ROW);
  gchar *result = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);
  return result;
}
static void
test_retained_state(Fixture *f, gconstpointer unused)
{
  (void)unused;
  GError *error = NULL;
  for (guint backend = 1; backend <= 3; backend++) {
    policy(f, NULL, NULL, TRUE);
    g_autoptr(GhMessage) visible
        = message(backend, "visible", 3000 + backend, "[\"subject\",\"Visible title\"]");
    g_assert_cmpint(gh_conversation_store_admit(f->model, visible, WRAP, &error), ==,
                    GH_CONVERSATION_ADD_NEW);
    GhConversation *room = gh_conversation_store_lookup(f->model, gh_message_get_room_id(visible));
    gh_conversation_mark_read(room);
    policy(f, "word", "quiet", TRUE);
    g_autofree gchar *before = state_text(f, backend);
    g_autoptr(GhMessage) hidden
        = message(backend, "quiet", 4000 + backend, "[\"subject\",\"Hidden title\"]");
    g_assert_cmpint(gh_conversation_store_admit(f->model, hidden, WRAP, &error), ==,
                    GH_CONVERSATION_ADD_HIDDEN);
    g_assert_no_error(error);
    g_autofree gchar *after = state_text(f, backend);
    g_assert_cmpstr(before, ==, after);
    /* The read boundary survives the later unhide too. */
    g_autofree gchar *read_sql
        = g_strdup_printf("SELECT read_seq FROM conversations WHERE backend=%u", backend);
    gint64 read_seq = scalar(f, read_sql);
    policy(f, NULL, NULL, TRUE);
    g_assert_cmpint(scalar(f, read_sql), ==, read_seq);
    g_assert_nonnull(
        gh_conversation_store_lookup_message(f->model, gh_message_get_rumor_id(hidden)));
  }
  policy(f, "word", "quiet", TRUE);
  g_autoptr(GhMessage) own = gh_message_new_from_mls(
      ALICE, "ccdd",
      "{\"pubkey\":\"" ALICE "\",\"kind\":9,\"created_at\":5000,\"content\":\"quiet\",\"tags\":[]}",
      &error);
  GhMessageDisposition d;
  g_assert_true(gh_message_policy_classify(f->policy, own, &d, &error));
  g_assert_cmpint(d, ==, GH_MESSAGE_INCLUDE);
  g_assert_no_error(error);
}
static void
test_hidden_paging(Fixture *f, gconstpointer unused)
{
  (void)unused;
  GError *error = NULL;
  policy(f, "word", "hidden", TRUE);
  for (guint backend = 1; backend <= 3; backend++) {
    for (guint i = 0; i < 44; i++) {
      g_autoptr(GhMessage) m = message(
          backend, (i == 0 || i == 1 || i == 42 || i == 43) ? "visible page" : "hidden page",
          6000 + i, NULL);
      GhConversationAddResult expected = (i == 0 || i == 1 || i == 42 || i == 43)
                                             ? GH_CONVERSATION_ADD_NEW
                                             : GH_CONVERSATION_ADD_HIDDEN;
      g_assert_cmpint(gh_conversation_store_admit(f->model, m, WRAP, &error), ==, expected);
    }
    gboolean ok = backend == 1   ? gh_store_conversations_attach(f->dm, f->model, 2, &error)
                  : backend == 2 ? gh_store_nip29_attach(f->nip29, f->model, 2, &error)
                                 : gh_store_mls_attach(f->mls, f->model, 2, &error);
    g_assert_true(ok);
    g_assert_no_error(error);
    g_autoptr(GhMessage) newest = message(backend, "visible page", 6043, NULL);
    GhConversation *room = gh_conversation_store_lookup(f->model, gh_message_get_room_id(newest));
    g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(room)), ==, 2);
    guint loaded = 0;
    ok = backend == 1   ? gh_store_conversations_load_older(f->dm, room, 2, &loaded, &error)
         : backend == 2 ? gh_store_nip29_load_older(f->nip29, room, 2, &loaded, &error)
                        : gh_store_mls_load_older(f->mls, room, 2, &loaded, &error);
    g_assert_true(ok);
    g_assert_no_error(error);
    g_assert_cmpuint(loaded, ==, 2);
    g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(room)), ==, 4);
    g_assert_false(gh_conversation_get_has_older(room));
  }
}
static void
test_recount_and_deletion(Fixture *f, gconstpointer unused)
{
  (void)unused;
  GError *error = NULL;
  g_autoptr(GhMessage) first = message(1, "visible first", 7000, NULL);
  g_autoptr(GhMessage) second = message(1, "visible second", 7001, NULL);
  g_autoptr(GhMessage) hidden = message(1, "secret retained", 7002, NULL);
  g_assert_cmpint(gh_conversation_store_admit(f->model, first, WRAP, &error), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(gh_conversation_store_admit(f->model, second, WRAP, &error), ==,
                  GH_CONVERSATION_ADD_NEW);
  policy(f, "word", "secret", TRUE);
  g_assert_cmpint(gh_conversation_store_admit(f->model, hidden, WRAP, &error), ==,
                  GH_CONVERSATION_ADD_HIDDEN);
  gint64 room = scalar(f, "SELECT id FROM conversations LIMIT 1"),
         seq = scalar(f, "SELECT seq FROM messages WHERE body='visible first'");
  gint64 row = scalar(f, "SELECT id FROM messages WHERE body='visible first'");
  g_assert_true(
      gh_store_read_state_update(f->store, room, GH_STORE_READ_LISTED, row, seq, NULL, &error));
  g_assert_cmpint(scalar(f, "SELECT unread_count FROM conversations LIMIT 1"), ==, 1);
  g_assert_true(gh_store_exec(
      f->store, "UPDATE messages SET expires_at=1 WHERE body='visible first'", &error));
  GhStorePurgeStats stats;
  g_assert_true(gh_store_purge(f->store, 0, &stats, &error));
  g_assert_cmpint(stats.n_expired, ==, 1);
  g_assert_cmpint(scalar(f, "SELECT unread_count FROM conversations LIMIT 1"), ==, 1);
  gboolean deleted = FALSE;
  g_assert_true(gh_store_delete_message(f->store, GH_STORE_BACKEND_NIP17,
                                        gh_message_get_room_id(second),
                                        gh_message_get_rumor_id(second), ALICE, &deleted, &error));
  g_assert_false(deleted); /* still ownership checked */
  /* A hidden author can still delete legitimate prior content. */
  policy(f, "p", BOB, TRUE);
  g_assert_true(gh_store_delete_message(f->store, GH_STORE_BACKEND_NIP17,
                                        gh_message_get_room_id(second),
                                        gh_message_get_rumor_id(second), BOB, &deleted, &error));
  g_assert_true(deleted);
  g_assert_cmpint(scalar(f, "SELECT unread_count FROM conversations LIMIT 1"), ==, 0);
  g_assert_no_error(error);
}

static void
test_reaction_projection(Fixture *f, gconstpointer unused)
{
  (void)unused;
  const gchar *carol = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
  GError *error = NULL;
  g_autoptr(GhMessage) first = message(3, "target-one", 8000, NULL);
  g_autoptr(GhMessage) future = message(3, "target-two", 8001, NULL);
  g_assert_cmpint(gh_conversation_store_admit(f->model, first, WRAP, &error), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_autoptr(GhStoreReactions) durable = gh_store_reactions_new(f->store);
  g_autoptr(GhReactionStore) reactions = gh_reaction_store_new();
  g_assert_true(gh_store_reactions_attach(durable, reactions, &error));
  gh_reaction_store_set_policy(reactions, f->policy);
  g_autoptr(GhReaction) r1
      = gh_reaction_new(gh_message_get_rumor_id(first),
                        "1111111111111111111111111111111111111111111111111111111111111111", carol,
                        "+", 8002, gh_message_get_room_id(first));
  g_assert_true(gh_reaction_store_admit(reactions, r1, &error));
  GhReactionSummary *summary = gh_reaction_store_lookup(reactions, gh_message_get_rumor_id(first));
  g_assert_cmpuint(gh_reaction_summary_get_total_count(summary), ==, 1);
  policy(f, "p", carol, TRUE);
  g_assert_cmpuint(gh_reaction_summary_get_total_count(summary), ==, 0);
  g_autoptr(GhReaction) r2
      = gh_reaction_new(gh_message_get_rumor_id(future),
                        "2222222222222222222222222222222222222222222222222222222222222222", carol,
                        "+", 8003, gh_message_get_room_id(future));
  g_assert_false(gh_reaction_store_admit(reactions, r2, &error));
  g_assert_cmpint(scalar(f, "SELECT count(*) FROM pending_reactions"), ==, 1);
  g_assert_cmpint(gh_conversation_store_admit(f->model, future, WRAP, &error), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_assert_true(gh_store_reactions_reconcile(durable, reactions, NULL, NULL, &error));
  g_assert_cmpint(scalar(f, "SELECT count(*) FROM pending_reactions"), ==, 0);
  g_assert_cmpint(scalar(f, "SELECT count(*) FROM reactions"), ==, 2);
  GhReactionSummary *other = gh_reaction_store_lookup(reactions, gh_message_get_rumor_id(future));
  g_assert_cmpuint(gh_reaction_summary_get_total_count(other), ==, 0);
  policy(f, NULL, NULL, TRUE);
  g_assert_cmpuint(gh_reaction_summary_get_total_count(summary), ==, 1);
  g_assert_cmpuint(gh_reaction_summary_get_total_count(other), ==, 1);
  policy(f, "word", "target-one", TRUE);
  g_assert_cmpuint(gh_reaction_summary_get_total_count(summary), ==, 0);
  g_assert_cmpuint(gh_reaction_summary_get_total_count(other), ==, 1);
  policy(f, NULL, NULL, TRUE);
  g_assert_cmpuint(gh_reaction_summary_get_total_count(summary), ==, 1);
  g_assert_no_error(error);
  gh_reaction_store_set_account(reactions, NULL, NULL, NULL, NULL);
  gh_store_reactions_close(durable);
}

static gboolean
never_seen(gpointer data, const gchar *id)
{
  (void)data;
  (void)id;
  return FALSE;
}
static gboolean
must_not_commit(gpointer data, GhMessage *message, const gchar *wrap,
                GhMessageDisposition disposition, GhConversationCommit *commit, GError **error)
{
  (void)message;
  (void)wrap;
  (void)disposition;
  (void)commit;
  (void)error;
  (*(guint *)data)++;
  return TRUE;
}
static void
test_unloaded(void)
{
  g_autoptr(GhConversationStore) model = gh_conversation_store_new();
  g_autoptr(GhMessagePolicy) p = gh_message_policy_new(ALICE);
  /* A delegate carrying an unloaded policy cannot commit/record anything. */
  guint commits = 0;
  GhConversationDelegate delegate
      = { .has_wrap = never_seen, .has_rumor = never_seen, .admit = must_not_commit, .policy = p };
  gh_conversation_store_set_account(model, ALICE, &delegate, &commits, NULL);
  g_autoptr(GhMessage) m = message(1, "not admitted", 1, NULL);
  GhMessageDisposition d;
  GError *error = NULL;
  g_assert_false(gh_message_policy_classify(p, m, &d, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED);
  g_clear_error(&error);
  g_assert_cmpint(gh_conversation_store_admit(model, m, WRAP, &error), ==,
                  GH_CONVERSATION_ADD_FAILED);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED);
  g_clear_error(&error);
  g_assert_cmpuint(commits, ==, 0);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(model)), ==, 0);
  g_assert_false(gh_conversation_store_has_wrap(model, WRAP));
}
int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add("/groundhog/message-policy/dispositions-all-backends", Fixture, NULL, setup,
             test_dispositions, teardown);
  g_test_add("/groundhog/message-policy/unicode-tags-threads", Fixture, NULL, setup, test_matching,
             teardown);
  g_test_add("/groundhog/message-policy/cancel-restart-search-recount", Fixture, NULL, setup,
             test_cancel_and_recount, teardown);
  g_test_add("/groundhog/message-policy/retain-room-state", Fixture, NULL, setup,
             test_retained_state, teardown);
  g_test_add("/groundhog/message-policy/hidden-only-paging", Fixture, NULL, setup,
             test_hidden_paging, teardown);
  g_test_add("/groundhog/message-policy/recounts-and-valid-deletions", Fixture, NULL, setup,
             test_recount_and_deletion, teardown);
  g_test_add("/groundhog/message-policy/reactions-pending-and-unhide", Fixture, NULL, setup,
             test_reaction_projection, teardown);
  g_test_add("/groundhog/message-policy/epoch-fresh-arrivals", Fixture, NULL, setup,
             test_epoch_arrivals, teardown);
  g_test_add("/groundhog/message-policy/projection-failure-retry", Fixture, NULL, setup,
             test_projection_retry, teardown);
  g_test_add("/groundhog/message-policy/indexed-author-content-cache", Fixture, NULL, setup,
             test_indexed_author_cache, teardown);
  g_test_add_func("/groundhog/message-policy/unloaded", test_unloaded);
  return g_test_run();
}
