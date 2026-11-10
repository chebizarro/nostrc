/* Schema v13 draft-sync state (plan §3.4, §3.7; nostrc-9lwl7.8): revisions
 * and tombstones move with conversations.draft in the setter's, the
 * enqueue's and forget's own transactions; an injected ROLLBACK/ABORT leaves
 * draft, revision and tombstone as they were; kinds 5 and 7 keep the draft;
 * identifiers converge on the lowest; remote revisions never dirty a room.
 * Ephemeral SQLCipher stores on a fake clock; no network, no signer. */
#include "gh-store-conversations.h"
#include "gh-store-drafts.h"
#include <sqlite3.h>
#include <string.h>

#define ACCOUNT "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define PEER    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define ROOM    ACCOUNT "," PEER
#define SELF    ACCOUNT
#define T0      1700000000

typedef struct {
  GhClock *clock;
  GhStore *store;
  GhStoreConversations *drafts;
  guint signals;
  gchar *signalled;
} Fixture;

static gint64
scalar(GhStore *store, const gchar *sql)
{
  sqlite3_stmt *stmt = NULL;
  g_assert_cmpint(sqlite3_prepare_v2(gh_store_get_db(store), sql, -1, &stmt, NULL), ==, SQLITE_OK);
  g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_ROW);
  gint64 value = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return value;
}

static void
on_draft_changed(GhStoreConversations *drafts, const gchar *room, Fixture *f)
{
  (void) drafts;
  f->signals++;
  g_free(f->signalled);
  f->signalled = g_strdup(room);
}

static void
setup(Fixture *f)
{
  memset(f, 0, sizeof *f);
  f->clock = gh_clock_new_fake((gint64) T0 * G_USEC_PER_SEC);
  g_autoptr(GError) error = NULL;
  f->store = gh_store_open_ephemeral(ACCOUNT, f->clock, &error);
  g_assert_no_error(error);
  f->drafts = gh_store_conversations_new(f->store);
  g_signal_connect(f->drafts, "draft-changed", G_CALLBACK(on_draft_changed), f);
}

static void
teardown(Fixture *f)
{
  gh_store_conversations_close(f->drafts);
  g_object_unref(f->drafts);
  gh_store_close(f->store);
  gh_clock_unref(f->clock);
  g_free(f->signalled);
}

static void
set_draft(Fixture *f, const gchar *room, const gchar *text)
{
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_conversations_set_draft(f->drafts, room, text, &error));
  g_assert_no_error(error);
}

static gchar *
get_draft(Fixture *f, const gchar *room)
{
  g_autoptr(GError) error = NULL;
  gchar *text = NULL;
  g_assert_true(gh_store_conversations_get_draft(f->drafts, room, &text, &error));
  g_assert_no_error(error);
  return text;
}

static GhStoreDraftState *
state(Fixture *f, const gchar *room)
{
  g_autoptr(GError) error = NULL;
  GhStoreDraftState *s = gh_store_drafts_get(f->store, room, &error);
  g_assert_no_error(error);
  return s;
}

static gint64
conversation_id(Fixture *f, const gchar *room)
{
  g_autoptr(GError) error = NULL;
  gint64 id = 0;
  g_assert_true(gh_store_find_conversation(f->store, GH_STORE_BACKEND_NIP17, room, &id, &error));
  g_assert_no_error(error);
  return id;
}

static gboolean
enqueue(Fixture *f, const gchar *room, gint kind, const gchar *op, GError **error)
{
  g_autofree gchar *rumor_id = g_strdup_printf("%s%s", op, op); /* distinct per op */
  GhStoreOutgoing outgoing = {
    .conversation_id = conversation_id(f, room),
    .op_id = op,
    .backend_msg_id = rumor_id,
    .sender_pubkey = ACCOUNT,
    .kind = kind,
    .created_at = T0 + 100,
    .body = "sent",
    .rumor_json = "{\"kind\":14}",
  };
  return gh_store_enqueue(f->store, &outgoing, NULL, NULL, error);
}

static void
test_schema(void)
{
  Fixture f; setup(&f);
  g_assert_cmpint(scalar(f.store, "PRAGMA user_version"), ==, GH_STORE_SCHEMA_VERSION);
  g_assert_cmpint(scalar(f.store, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN "
                                  "('draft_sync_state','draft_publications','draft_conflicts')"), ==, 3);
  g_assert_cmpint(scalar(f.store, "SELECT count(*) FROM schema_migrations WHERE version = 13"), ==, 1);
  teardown(&f);
}

/* The setter bumps the revision and marks the room dirty in its own
 * transaction, and emptying the draft leaves a tombstone. */
static void
test_setter_revisions(void)
{
  Fixture f; setup(&f);
  g_assert_null(state(&f, ROOM));
  set_draft(&f, ROOM, "hello");
  g_assert_cmpuint(f.signals, ==, 1); g_assert_cmpstr(f.signalled, ==, ROOM);
  g_autoptr(GhStoreDraftState) s1 = state(&f, ROOM);
  g_assert_nonnull(s1);
  g_assert_cmpint(s1->local_revision, ==, 1); g_assert_true(s1->dirty);
  g_assert_null(s1->draft_d); g_assert_cmpint(s1->cleared_at, ==, 0);
  set_draft(&f, ROOM, "hello again");
  g_autoptr(GhStoreDraftState) s2 = state(&f, ROOM);
  g_assert_cmpint(s2->local_revision, ==, 2); g_assert_true(s2->dirty);
  gh_clock_fake_advance(f.clock, 5 * G_USEC_PER_SEC);
  set_draft(&f, ROOM, "");
  g_autoptr(GhStoreDraftState) s3 = state(&f, ROOM);
  g_assert_cmpint(s3->local_revision, ==, 3);
  g_assert_false(s3->dirty); /* never published: nothing to blank */
  g_assert_cmpint(s3->cleared_at, ==, T0 + 5);
  g_assert_cmpint(scalar(f.store, "SELECT count(*) FROM draft_publications"), ==, 0);
  g_autofree gchar *text = get_draft(&f, ROOM);
  g_assert_null(text);
  g_assert_cmpuint(f.signals, ==, 3);
  /* Another backend's room is not touched by the hooks. */
  gint64 group = 0;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_ensure_conversation(f.store, GH_STORE_BACKEND_NIP29, "wss://g.example\x1fg",
                                             GH_STORE_REQUEST_ACCEPTED, &group, &error));
  g_assert_true(gh_store_begin(f.store, &error));
  g_assert_true(gh_store_drafts_touch(f.store, group, &error));
  g_assert_true(gh_store_drafts_clear(f.store, group, T0, &error));
  g_assert_true(gh_store_commit(f.store, &error));
  g_assert_cmpint(scalar(f.store, "SELECT count(*) FROM draft_sync_state"), ==, 1);
  teardown(&f);
}

/* A chat enqueue clears the draft, records the tombstone and queues a blank
 * for every known identifier, all in the enqueue's transaction; an injected
 * failure there keeps the draft and queues nothing. Kinds 5 and 7 keep it. */
static void
test_enqueue_clear(void)
{
  Fixture f; setup(&f);
  g_autoptr(GError) error = NULL;
  set_draft(&f, ROOM, "unsent words");
  g_autofree gchar *d = NULL;
  g_assert_true(gh_store_drafts_assign_identifier(f.store, ROOM, &d, &error));
  g_assert_cmpuint(strlen(d), ==, 64);
  g_assert_true(gh_store_drafts_add_identifier(f.store, ROOM,
    "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee", &error));
  g_autoptr(GhStoreDraftState) before = state(&f, ROOM);
  g_assert_true(before->dirty);
  gint64 revision = before->local_revision;

  /* Reactions and deletion requests are not writing in the room. */
  g_assert_true(enqueue(&f, ROOM, 7, "00000000000000000000000000000001", &error));
  g_assert_true(enqueue(&f, ROOM, 5, "00000000000000000000000000000002", &error));
  g_autofree gchar *kept = get_draft(&f, ROOM);
  g_assert_cmpstr(kept, ==, "unsent words");
  g_autoptr(GhStoreDraftState) same = state(&f, ROOM);
  g_assert_cmpint(same->local_revision, ==, revision); g_assert_cmpint(same->cleared_at, ==, 0);
  g_assert_cmpint(scalar(f.store, "SELECT count(*) FROM draft_publications"), ==, 0);

  /* Rollback: the blank cannot be queued, so nothing of the enqueue lands. */
  g_assert_true(gh_store_exec(f.store, "CREATE TRIGGER reject BEFORE INSERT ON draft_publications "
                                        "BEGIN SELECT RAISE(ABORT,'injected'); END", &error));
  g_assert_false(enqueue(&f, ROOM, 14, "00000000000000000000000000000003", &error));
  g_assert_nonnull(error); g_clear_error(&error);
  g_autofree gchar *still = get_draft(&f, ROOM);
  g_assert_cmpstr(still, ==, "unsent words");
  g_autoptr(GhStoreDraftState) untouched = state(&f, ROOM);
  g_assert_cmpint(untouched->local_revision, ==, revision); g_assert_cmpint(untouched->cleared_at, ==, 0);
  g_assert_cmpint(scalar(f.store, "SELECT count(*) FROM outbox"), ==, 2);
  g_assert_cmpint(scalar(f.store, "SELECT count(*) FROM draft_publications"), ==, 0);
  g_assert_true(gh_store_exec(f.store, "DROP TRIGGER reject", &error));

  gh_clock_fake_advance(f.clock, 7 * G_USEC_PER_SEC);
  g_assert_true(enqueue(&f, ROOM, 14, "00000000000000000000000000000003", &error));
  g_assert_no_error(error);
  g_autofree gchar *cleared = get_draft(&f, ROOM);
  g_assert_null(cleared);
  g_autoptr(GhStoreDraftState) after = state(&f, ROOM);
  g_assert_cmpint(after->local_revision, ==, revision + 1);
  g_assert_true(after->dirty); g_assert_cmpint(after->cleared_at, ==, T0 + 7);
  g_autoptr(GPtrArray) queued = gh_store_drafts_list_publications(f.store, &error);
  g_assert_cmpuint(queued->len, ==, 2); /* the identifier and its alias */
  for (guint i = 0; i < queued->len; i++) {
    GhStoreDraftPublication *p = queued->pdata[i];
    g_assert_cmpint(p->state, ==, GH_STORE_DRAFT_PUBLICATION_QUEUED);
    g_assert_cmpint(p->local_revision, ==, revision + 1);
    g_assert_cmpstr(p->event_json, ==, "");
  }
  /* Sync switched off deletes signed events only: a queued blank is the
   * tombstone's intent and survives for a later enable. */
  const gchar *targets[] = { "wss://private.example", NULL };
  g_assert_true(gh_store_drafts_save_publication(f.store, ROOM, d, revision + 1, "{\"id\":\"x\"}", targets, &error));
  g_assert_true(gh_store_drafts_delete_unsent(f.store, &error));
  g_assert_cmpint(scalar(f.store, "SELECT count(*) FROM draft_publications"), ==, 1);
  teardown(&f);
}

/* Forget sets the tombstone and forgotten_before and queues the blanks. */
static void
test_forget(void)
{
  Fixture f; setup(&f);
  g_autoptr(GError) error = NULL;
  set_draft(&f, ROOM, "to forget");
  g_autofree gchar *d = NULL;
  g_assert_true(gh_store_drafts_assign_identifier(f.store, ROOM, &d, &error));
  gh_clock_fake_advance(f.clock, 11 * G_USEC_PER_SEC);
  f.signals = 0;
  g_assert_true(gh_store_conversations_forget(f.drafts, ROOM, &error));
  g_assert_cmpuint(f.signals, ==, 1);
  g_autoptr(GhStoreDraftState) s = state(&f, ROOM);
  g_assert_true(s->dirty); g_assert_cmpint(s->cleared_at, ==, T0 + 11);
  g_assert_cmpstr(s->draft_d, ==, d);
  gboolean blocked = TRUE; gint64 forgotten = 0; g_autofree gchar *text = NULL;
  g_assert_true(gh_store_drafts_get_room(f.store, ROOM, &blocked, &forgotten, &text, &error));
  g_assert_false(blocked); g_assert_cmpint(forgotten, ==, T0 + 11); g_assert_null(text);
  g_assert_cmpint(scalar(f.store, "SELECT count(*) FROM draft_publications WHERE state = 0"), ==, 1);
  teardown(&f);
}

/* Identifiers: a room gets one random identifier, a second one learned from
 * a remote revision is kept as an alias with the lowest as head. */
static void
test_identifiers(void)
{
  Fixture f; setup(&f);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *first = NULL, *again = NULL;
  g_assert_true(gh_store_drafts_assign_identifier(f.store, SELF, &first, &error));
  g_assert_true(gh_store_drafts_assign_identifier(f.store, SELF, &again, &error));
  g_assert_cmpstr(first, ==, again);
  for (guint i = 0; i < 64; i++)
    g_assert_true(g_ascii_isxdigit(first[i]) && !g_ascii_isupper(first[i]));
  g_autofree gchar *other = NULL;
  g_assert_true(gh_store_drafts_assign_identifier(f.store, ROOM, &other, &error));
  g_assert_cmpstr(other, !=, first);
  const gchar *low = "0000000000000000000000000000000000000000000000000000000000000000";
  const gchar *high = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
  g_assert_true(gh_store_drafts_add_identifier(f.store, SELF, high, &error));
  g_autoptr(GhStoreDraftState) s1 = state(&f, SELF);
  g_assert_cmpstr(s1->draft_d, ==, first); g_assert_cmpstr(s1->aliases[0], ==, high); g_assert_null(s1->aliases[1]);
  g_assert_true(gh_store_drafts_add_identifier(f.store, SELF, low, &error));
  g_autoptr(GhStoreDraftState) s2 = state(&f, SELF);
  g_assert_cmpstr(s2->draft_d, ==, low);
  g_assert_true(g_strv_contains((const gchar *const *) s2->aliases, first));
  g_assert_true(g_strv_contains((const gchar *const *) s2->aliases, high));
  g_assert_true(gh_store_drafts_add_identifier(f.store, SELF, high, &error)); /* known: no change */
  g_autoptr(GhStoreDraftState) s3 = state(&f, SELF);
  g_assert_cmpuint(g_strv_length(s3->aliases), ==, 2);
  g_autofree gchar *room = gh_store_drafts_find_room(f.store, high, &error);
  g_assert_cmpstr(room, ==, SELF);
  g_autofree gchar *room2 = gh_store_drafts_find_room(f.store, other, &error);
  g_assert_cmpstr(room2, ==, ROOM);
  g_autofree gchar *none = gh_store_drafts_find_room(f.store, "deadbeef", &error);
  g_assert_null(none); g_assert_no_error(error);
  g_assert_true(gh_store_drafts_remove_alias(f.store, SELF, high, &error));
  g_autoptr(GhStoreDraftState) s4 = state(&f, SELF);
  g_assert_cmpuint(g_strv_length(s4->aliases), ==, 1); g_assert_cmpstr(s4->aliases[0], ==, first);
  teardown(&f);
}

/* A remote revision sets the text (creating the room as accepted) without
 * dirtying it; a remote clear leaves a tombstone; acceptance of the current
 * local revision cleans the room, of a stale one does not. */
static void
test_remote_and_accept(void)
{
  Fixture f; setup(&f);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_drafts_apply_remote(f.store, ROOM, "from elsewhere", "e1", T0 + 1, T0 + 100, TRUE, &error));
  g_assert_no_error(error);
  g_autofree gchar *text = get_draft(&f, ROOM);
  g_assert_cmpstr(text, ==, "from elsewhere");
  g_assert_cmpint(scalar(f.store, "SELECT request_state FROM conversations"), ==, GH_STORE_REQUEST_ACCEPTED);
  g_autoptr(GhStoreDraftState) s1 = state(&f, ROOM);
  g_assert_false(s1->dirty); g_assert_cmpint(s1->local_revision, ==, 0);
  g_assert_cmpstr(s1->last_event_id, ==, "e1"); g_assert_cmpint(s1->last_created_at, ==, T0 + 1);
  g_assert_cmpint(s1->expires_at, ==, T0 + 100);
  g_assert_cmpuint(f.signals, ==, 0); /* not a local edit */
  /* Record-only (same text, or a conflict kept locally): no text change. */
  g_assert_true(gh_store_drafts_apply_remote(f.store, ROOM, "ignored", "e2", T0 + 2, T0 + 101, FALSE, &error));
  g_autofree gchar *same = get_draft(&f, ROOM);
  g_assert_cmpstr(same, ==, "from elsewhere");
  g_autoptr(GhStoreDraftState) s2 = state(&f, ROOM);
  g_assert_cmpstr(s2->last_event_id, ==, "e2");
  /* A remote clear. */
  g_assert_true(gh_store_drafts_apply_remote(f.store, ROOM, NULL, "e3", T0 + 3, T0 + 102, TRUE, &error));
  g_autofree gchar *gone = get_draft(&f, ROOM);
  g_assert_null(gone);
  g_autoptr(GhStoreDraftState) s3 = state(&f, ROOM);
  g_assert_cmpstr(s3->last_event_id, ==, "e3"); g_assert_cmpint(s3->last_created_at, ==, T0 + 3);
  g_assert_cmpint(s3->cleared_at, ==, 0); /* ordered like any revision, not a local cutoff */
  /* A local edit, then acceptance of its revision and of a stale one. */
  set_draft(&f, ROOM, "local");
  g_autofree gchar *d = NULL;
  g_assert_true(gh_store_drafts_assign_identifier(f.store, ROOM, &d, &error));
  g_autoptr(GhStoreDraftState) s4 = state(&f, ROOM);
  g_assert_true(s4->dirty); g_assert_cmpint(s4->local_revision, ==, 1);
  const gchar *targets[] = { "wss://private.example", NULL };
  g_assert_true(gh_store_drafts_save_publication(f.store, ROOM, d, 1, "{\"id\":\"e4\"}", targets, &error));
  g_assert_true(gh_store_drafts_set_outcomes(f.store, ROOM, d, "{\"wss://private.example\":\"accepted\"}", &error));
  g_autoptr(GPtrArray) pubs = gh_store_drafts_list_publications(f.store, &error);
  GhStoreDraftPublication *p = pubs->pdata[0];
  g_assert_cmpint(p->state, ==, GH_STORE_DRAFT_PUBLICATION_SIGNED);
  g_assert_cmpstr(p->targets[0], ==, targets[0]); g_assert_null(p->targets[1]);
  g_assert_cmpstr(p->outcomes_json, ==, "{\"wss://private.example\":\"accepted\"}");
  set_draft(&f, ROOM, "local, newer"); /* revision 2 supersedes the signed revision 1 */
  gboolean current = TRUE;
  g_assert_true(gh_store_drafts_accept(f.store, ROOM, d, 1, "e4", T0 + 10, T0 + 110, &current, &error));
  g_assert_false(current);
  g_autoptr(GhStoreDraftState) s5 = state(&f, ROOM);
  g_assert_true(s5->dirty); g_assert_cmpint(s5->local_revision, ==, 2);
  g_assert_cmpint(scalar(f.store, "SELECT count(*) FROM draft_publications"), ==, 0);
  g_assert_true(gh_store_drafts_accept(f.store, ROOM, d, 2, "e5", T0 + 11, T0 + 111, &current, &error));
  g_assert_true(current);
  g_autoptr(GhStoreDraftState) s6 = state(&f, ROOM);
  g_assert_false(s6->dirty); g_assert_cmpstr(s6->last_event_id, ==, "e5");
  g_assert_cmpint(s6->last_created_at, ==, T0 + 11); g_assert_cmpint(s6->expires_at, ==, T0 + 111);
  /* Pruning: a clean, expired room without pending work goes; a dirty one stays. */
  g_assert_true(gh_store_drafts_prune(f.store, T0 + 112, &error));
  g_assert_null(state(&f, ROOM));
  teardown(&f);
}

static void
test_conflicts(void)
{
  Fixture f; setup(&f);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_drafts_add_conflict(f.store, ROOM, "theirs", "e1", T0 + 1, &error));
  g_assert_true(gh_store_drafts_add_conflict(f.store, SELF, "mine", NULL, T0 + 2, &error));
  g_assert_true(gh_store_drafts_add_conflict(f.store, ROOM, "theirs, later", "e2", T0 + 3, &error));
  g_autoptr(GPtrArray) all = gh_store_drafts_list_conflicts(f.store, NULL, &error);
  g_assert_cmpuint(all->len, ==, 3);
  g_assert_cmpstr(((GhStoreDraftConflict *) all->pdata[0])->text, ==, "theirs, later");
  g_autoptr(GPtrArray) room = gh_store_drafts_list_conflicts(f.store, ROOM, &error);
  g_assert_cmpuint(room->len, ==, 2);
  GhStoreDraftConflict *c = room->pdata[1];
  g_assert_cmpstr(c->text, ==, "theirs"); g_assert_cmpstr(c->source_event_id, ==, "e1");
  g_assert_cmpint(c->saved_at, ==, T0 + 1);
  g_assert_true(gh_store_drafts_delete_conflict(f.store, c->id, &error));
  g_assert_cmpint(scalar(f.store, "SELECT count(*) FROM draft_conflicts"), ==, 2);
  /* The JSON helpers round-trip awkward relay URLs. */
  const gchar *targets[] = { "wss://a.example/\"q\"\\", "wss://b.example", NULL };
  g_assert_true(gh_store_drafts_save_publication(f.store, ROOM, "d", 1, "{}", targets, &error));
  g_autoptr(GPtrArray) pubs = gh_store_drafts_list_publications(f.store, &error);
  GhStoreDraftPublication *p = pubs->pdata[0];
  g_assert_cmpstr(p->targets[0], ==, targets[0]); g_assert_cmpstr(p->targets[1], ==, targets[1]);
  g_assert_null(p->targets[2]);
  teardown(&f);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/store-drafts/schema-v13", test_schema);
  g_test_add_func("/groundhog/store-drafts/setter-revisions", test_setter_revisions);
  g_test_add_func("/groundhog/store-drafts/enqueue-clear-atomic", test_enqueue_clear);
  g_test_add_func("/groundhog/store-drafts/forget-tombstone", test_forget);
  g_test_add_func("/groundhog/store-drafts/identifiers-converge", test_identifiers);
  g_test_add_func("/groundhog/store-drafts/remote-and-accept", test_remote_and_accept);
  g_test_add_func("/groundhog/store-drafts/conflicts", test_conflicts);
  return g_test_run();
}
