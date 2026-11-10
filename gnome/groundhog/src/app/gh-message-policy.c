#include "gh-message-policy.h"
#include "gh-store.h"
#include "gh-conversation-store.h"
#include "gh-conversation-private.h"
#include <jansson.h>
#include <sqlite3.h>
#include <string.h>

/* A sparse Aho-Corasick automaton over NFC/casefolded UTF-8 bytes. */
typedef struct {
  guint8 byte;
  guint next;
} Edge;
typedef struct {
  GArray *edges;
  guint fail;
  gboolean match;
} Node;
typedef struct {
  guint refs;
  GHashTable *authors, *events, *hashtags;
  GArray *words;
  gchar *revision, *content_revision;
} Snapshot;

struct _GhMessagePolicy {
  GObject parent_instance;
  gchar *account, *error;
  GhStore *store;
  GWeakRef model;
  Snapshot *snapshot;
  Snapshot *projected_snapshot; /* last successfully published projection */
  /* room + id -> explicit root/reply parents; never q tags. */
  GHashTable *parents, *threads;
  guint source;
  guint64 epoch;
  gint64 cursor;
  gboolean loaded, updating, closed;
};
G_DEFINE_TYPE(GhMessagePolicy, gh_message_policy, G_TYPE_OBJECT)
enum { CHANGED, PROJECTED, FAILED, N_SIGNALS };
static guint signals[N_SIGNALS];

static GHashTable *
set_new(void)
{
  return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}
static gchar *
fold(const gchar *s)
{
  g_autofree gchar *nfc = g_utf8_normalize(s ? s : "", -1, G_NORMALIZE_NFC);
  g_autofree gchar *cf = g_utf8_casefold(nfc ? nfc : "", -1);
  return g_utf8_normalize(cf, -1, G_NORMALIZE_NFC);
}
static gboolean
hex_id(const gchar *s)
{
  if (!s || strlen(s) != 64)
    return FALSE;
  for (guint i = 0; i < 64; i++)
    if (!g_ascii_isxdigit(s[i]))
      return FALSE;
  return TRUE;
}
static void
node_clear(gpointer data)
{
  g_array_unref(((Node *)data)->edges);
}
static guint
node_add(GArray *nodes)
{
  Node n = { .edges = g_array_new(FALSE, FALSE, sizeof(Edge)) };
  g_array_append_val(nodes, n);
  return nodes->len - 1;
}
static guint
transition(GArray *nodes, guint at, guint8 byte)
{
  GArray *edges = g_array_index(nodes, Node, at).edges;
  for (guint i = 0; i < edges->len; i++) {
    Edge e = g_array_index(edges, Edge, i);
    if (e.byte == byte)
      return e.next;
  }
  return 0;
}
static void
word_add(GArray *nodes, const gchar *s)
{
  guint at = 0;
  for (const guint8 *p = (const guint8 *)s; *p; p++) {
    guint next = transition(nodes, at, *p);
    if (!next) {
      next = node_add(nodes);
      Edge edge = { *p, next };
      g_array_append_val(g_array_index(nodes, Node, at).edges, edge);
    }
    at = next;
  }
  if (*s)
    g_array_index(nodes, Node, at).match = TRUE;
}
static void
words_compile(GArray *nodes)
{
  GQueue queue = G_QUEUE_INIT;
  GArray *root = g_array_index(nodes, Node, 0).edges;
  for (guint i = 0; i < root->len; i++)
    g_queue_push_tail(&queue, GUINT_TO_POINTER(g_array_index(root, Edge, i).next));
  while (!g_queue_is_empty(&queue)) {
    guint at = GPOINTER_TO_UINT(g_queue_pop_head(&queue));
    Node *node = &g_array_index(nodes, Node, at);
    for (guint i = 0; i < node->edges->len; i++) {
      Edge e = g_array_index(node->edges, Edge, i);
      guint f = node->fail;
      while (f && !transition(nodes, f, e.byte))
        f = g_array_index(nodes, Node, f).fail;
      Node *child = &g_array_index(nodes, Node, e.next);
      child->fail = transition(nodes, f, e.byte);
      child->match |= g_array_index(nodes, Node, child->fail).match;
      g_queue_push_tail(&queue, GUINT_TO_POINTER(e.next));
    }
  }
}
static gboolean
words_match(GArray *nodes, const gchar *s)
{
  guint at = 0;
  for (const guint8 *p = (const guint8 *)s; *p; p++) {
    while (at && !transition(nodes, at, *p))
      at = g_array_index(nodes, Node, at).fail;
    at = transition(nodes, at, *p);
    if (g_array_index(nodes, Node, at).match)
      return TRUE;
  }
  return FALSE;
}
static Snapshot *
snapshot_ref(Snapshot *s)
{
  if (s)
    s->refs++;
  return s;
}
static void
snapshot_free(Snapshot *s)
{
  if (!s || --s->refs)
    return;
  g_hash_table_unref(s->authors);
  g_hash_table_unref(s->events);
  g_hash_table_unref(s->hashtags);
  g_array_unref(s->words);
  g_free(s->revision);
  g_free(s->content_revision);
  g_free(s);
}
static gboolean
sql_done(GhMessagePolicy *self, sqlite3_stmt *stmt, GError **error)
{
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_DONE)
    return TRUE;
  gh_store_set_sqlite_error(self->store, rc, "Updating hidden history", error);
  return FALSE;
}
static sqlite3_stmt *
prepare(GhMessagePolicy *self, const gchar *sql, GError **error)
{
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(gh_store_get_db(self->store), sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK)
    gh_store_set_sqlite_error(self->store, rc, "Reading hide policy", error);
  return stmt;
}
static Snapshot *
snapshot_load(GhMessagePolicy *self, GError **error)
{
  sqlite3_stmt *stmt
      = prepare(self, "SELECT tag_name,value FROM social_mutes ORDER BY tag_name,value", error);
  if (!stmt)
    return NULL;
  Snapshot *s = g_new0(Snapshot, 1);
  s->refs = 1;
  s->authors = set_new();
  s->events = set_new();
  s->hashtags = set_new();
  s->words = g_array_new(FALSE, TRUE, sizeof(Node));
  g_array_set_clear_func(s->words, node_clear);
  node_add(s->words);
  GChecksum *all = g_checksum_new(G_CHECKSUM_SHA256), *content = g_checksum_new(G_CHECKSUM_SHA256);
  int rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const gchar *name = (const gchar *)sqlite3_column_text(stmt, 0);
    const gchar *value = (const gchar *)sqlite3_column_text(stmt, 1);
    if (!name || !value || !g_utf8_validate(value, -1, NULL))
      continue;
    g_checksum_update(all, (const guchar *)name, strlen(name) + 1);
    g_checksum_update(all, (const guchar *)value, strlen(value) + 1);
    if (strcmp(name, "p")) {
      g_checksum_update(content, (const guchar *)name, strlen(name) + 1);
      g_checksum_update(content, (const guchar *)value, strlen(value) + 1);
    }
    if (!strcmp(name, "p") && hex_id(value))
      g_hash_table_add(s->authors, g_ascii_strdown(value, -1));
    else if (!strcmp(name, "e") && hex_id(value))
      g_hash_table_add(s->events, g_ascii_strdown(value, -1));
    else if (!strcmp(name, "word")) {
      g_autofree gchar *v = fold(value);
      word_add(s->words, v);
    } else if (!strcmp(name, "t")) {
      gchar *v = fold(*value == '#' ? value + 1 : value);
      if (*v)
        g_hash_table_add(s->hashtags, v);
      else
        g_free(v);
    }
  }
  sqlite3_finalize(stmt);
  s->revision = g_strdup(g_checksum_get_string(all));
  s->content_revision = g_strdup(g_checksum_get_string(content));
  g_checksum_free(all);
  g_checksum_free(content);
  if (rc != SQLITE_DONE) {
    gh_store_set_sqlite_error(self->store, rc, "Reading hide policy", error);
    snapshot_free(s);
    return NULL;
  }
  words_compile(s->words);
  return s;
}
static gchar *
thread_key(const gchar *room, const gchar *id)
{
  return g_strdup_printf("%s\036%s", room, id);
}
static gboolean
hashtag_char(gunichar c)
{
  return g_unichar_isalnum(c) || g_unichar_ismark(c) || c == '_';
}
static gboolean
hashtag_valid(const gchar *s)
{
  if (!s || !*s)
    return FALSE;
  for (const gchar *p = s; *p; p = g_utf8_next_char(p))
    if (!hashtag_char(g_utf8_get_char(p)))
      return FALSE;
  return TRUE;
}
static gboolean
tag_matches(Snapshot *s, const gchar *value)
{
  if (!hashtag_valid(value))
    return FALSE;
  g_autofree gchar *v = fold(value);
  return g_hash_table_contains(s->hashtags, v);
}
/* URLs, encrypted file bodies and imeta/key metadata are not application text.
 * A file caption (when present) remains text; no attachment is opened. */
static gchar *
application_text(json_t *event)
{
  GString *text = g_string_new(NULL);
  gint kind = (gint)json_integer_value(json_object_get(event, "kind"));
  const gchar *body = json_string_value(json_object_get(event, "content"));
  if (kind != GH_NIP17_FILE_KIND && body)
    g_string_append(text, body);
  json_t *tags = json_object_get(event, "tags"), *tag;
  size_t i;
  json_array_foreach(tags, i, tag)
  {
    const gchar *name = json_string_value(json_array_get(tag, 0));
    const gchar *value = json_string_value(json_array_get(tag, 1));
    if (name && value
        && (!strcmp(name, "caption") || (kind == GH_NIP17_FILE_KIND && !strcmp(name, "alt")))) {
      g_string_append_c(text, ' ');
      g_string_append(text, value);
    }
  }
  GString *clean = g_string_new(NULL);
  for (const gchar *p = text->str; *p;) {
    if (g_str_has_prefix(p, "https://") || g_str_has_prefix(p, "http://")
        || g_str_has_prefix(p, "file://")) {
      while (*p && !g_unichar_isspace(g_utf8_get_char(p)))
        p = g_utf8_next_char(p);
      g_string_append_c(clean, ' ');
    } else {
      g_string_append_unichar(clean, g_utf8_get_char(p));
      p = g_utf8_next_char(p);
    }
  }
  g_string_free(text, TRUE);
  return g_string_free(clean, FALSE);
}
static gboolean
content_matches(GhMessagePolicy *self, json_t *event)
{
  Snapshot *s = self->snapshot;
  json_t *tags = json_object_get(event, "tags"), *tag;
  size_t i;
  json_array_foreach(tags, i, tag)
  {
    const gchar *name = json_string_value(json_array_get(tag, 0));
    const gchar *value = json_string_value(json_array_get(tag, 1));
    if (name && !strcmp(name, "t") && tag_matches(s, value))
      return TRUE;
  }
  g_autofree gchar *text = application_text(event);
  g_autofree gchar *normalized = fold(text);
  if (words_match(s->words, normalized))
    return TRUE;
  for (const gchar *p = text; *p; p = g_utf8_next_char(p)) {
    if (*p != '#' || (p > text && hashtag_char(g_utf8_get_char(g_utf8_find_prev_char(text, p)))))
      continue;
    const gchar *end = p + 1;
    while (*end && hashtag_char(g_utf8_get_char(end)))
      end = g_utf8_next_char(end);
    g_autofree gchar *value = g_strndup(p + 1, end - p - 1);
    if (tag_matches(s, value))
      return TRUE;
  }
  return FALSE;
}
static void
remember_edges(GhMessagePolicy *self, const gchar *room, const gchar *id, json_t *event)
{
  g_autofree gchar *key = thread_key(room, id);
  GPtrArray *parents = g_ptr_array_new_with_free_func(g_free);
  json_t *tags = json_object_get(event, "tags"), *tag;
  size_t i;
  json_array_foreach(tags, i, tag)
  {
    const gchar *name = json_string_value(json_array_get(tag, 0));
    const gchar *value = json_string_value(json_array_get(tag, 1));
    const gchar *marker = json_string_value(json_array_get(tag, 3));
    if (name && !strcmp(name, "e") && hex_id(value) && marker
        && (!strcmp(marker, "root") || !strcmp(marker, "reply"))) {
      g_autofree gchar *lower = g_ascii_strdown(value, -1);
      g_ptr_array_add(parents, thread_key(room, lower));
    }
  }
  g_hash_table_replace(self->parents, g_steal_pointer(&key), parents);
}
/* Iterative closure, one visit per stored message; cycles are harmless. */
static void
thread_closure(GhMessagePolicy *self)
{
  g_hash_table_remove_all(self->threads);
  GHashTable *children
      = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
  GQueue queue = G_QUEUE_INIT;
  GHashTableIter it;
  gpointer key, value;
  g_hash_table_iter_init(&it, self->parents);
  while (g_hash_table_iter_next(&it, &key, &value)) {
    const gchar *id = strrchr(key, '\036');
    if (id && g_hash_table_contains(self->snapshot->events, id + 1)) {
      g_hash_table_add(self->threads, g_strdup(key));
      g_queue_push_tail(&queue, key);
    }
    GPtrArray *parents = value;
    for (guint i = 0; i < parents->len; i++) {
      const gchar *parent = parents->pdata[i];
      GPtrArray *list = g_hash_table_lookup(children, parent);
      if (!list) {
        list = g_ptr_array_new();
        g_hash_table_insert(children, g_strdup(parent), list);
      }
      g_ptr_array_add(list, key);
      const gchar *parent_id = strrchr(parent, '\036');
      if (parent_id && g_hash_table_contains(self->snapshot->events, parent_id + 1)
          && g_hash_table_add(self->threads, g_strdup(parent)))
        g_queue_push_tail(&queue, (gpointer)parent);
    }
  }
  while (!g_queue_is_empty(&queue)) {
    GPtrArray *list = g_hash_table_lookup(children, g_queue_pop_head(&queue));
    for (guint i = 0; list && i < list->len; i++)
      if (g_hash_table_add(self->threads, g_strdup(list->pdata[i])))
        g_queue_push_tail(&queue, list->pdata[i]);
  }
  g_hash_table_unref(children);
}
static gboolean
thread_matches(GhMessagePolicy *self, const gchar *room, const gchar *id, json_t *event)
{
  if (g_hash_table_contains(self->snapshot->events, id))
    return TRUE;
  g_autofree gchar *key = thread_key(room, id);
  if (g_hash_table_contains(self->threads, key))
    return TRUE;
  json_t *tag, *tags = json_object_get(event, "tags");
  size_t i;
  json_array_foreach(tags, i, tag)
  {
    const gchar *name = json_string_value(json_array_get(tag, 0));
    const gchar *value = json_string_value(json_array_get(tag, 1));
    const gchar *marker = json_string_value(json_array_get(tag, 3));
    if (name && !strcmp(name, "e") && hex_id(value) && marker
        && (!strcmp(marker, "root") || !strcmp(marker, "reply"))) {
      g_autofree gchar *lower = g_ascii_strdown(value, -1);
      g_autofree gchar *parent = thread_key(room, lower);
      if (g_hash_table_contains(self->snapshot->events, lower)
          || g_hash_table_contains(self->threads, parent))
        return TRUE;
    }
  }
  return FALSE;
}

gboolean
gh_message_policy_classify(GhMessagePolicy *self, GhMessage *message, GhMessageDisposition *out,
                           GError **error)
{
  *out = GH_MESSAGE_INCLUDE;
  if (self->closed) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_STATE, "Account message storage is closed");
    return FALSE;
  }
  if (!self->loaded || g_strcmp0(self->account, gh_message_get_account(message))) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                        "Account hide policy is not loaded");
    return FALSE;
  }
  if (gh_message_is_self(message))
    return TRUE;
  if (gh_message_policy_hides_author(self, gh_message_get_sender(message))) {
    *out = GH_MESSAGE_DROP;
    return TRUE;
  }
  json_t *event = json_loads(gh_message_get_rumor_json(message), JSON_REJECT_DUPLICATES, NULL);
  if (!event) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Cannot classify application content");
    return FALSE;
  }
  if (content_matches(self, event)
      || thread_matches(self, gh_message_get_room_id(message), gh_message_get_rumor_id(message),
                        event))
    *out = GH_MESSAGE_HIDE_RETAIN;
  json_decref(event);
  return TRUE;
}
gboolean
gh_message_policy_hides_author(GhMessagePolicy *self, const gchar *pubkey)
{
  if (!self || !self->loaded || !pubkey || !strcmp(pubkey, self->account))
    return FALSE;
  g_autofree gchar *lower = g_ascii_strdown(pubkey, -1);
  return g_hash_table_contains(self->snapshot->authors, lower);
}

static gboolean
recount(GhMessagePolicy *self, GError **error)
{
  sqlite3_stmt *stmt = prepare(self, "SELECT id,backend FROM conversations", error);
  if (!stmt)
    return FALSE;
  GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
  int rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    gint64 id = sqlite3_column_int64(stmt, 0);
    g_array_append_val(ids, id);
  }
  sqlite3_finalize(stmt);
  gboolean ok = rc == SQLITE_DONE;
  if (!ok)
    gh_store_set_sqlite_error(self->store, rc, "Recounting visible history", error);
  for (guint i = 0; ok && i < ids->len; i++)
    ok = gh_store_read_state_update(self->store, g_array_index(ids, gint64, i),
                                    GH_STORE_READ_RECOUNT, 0, 0, NULL, error);
  g_array_unref(ids);
  if (!ok
      || !gh_store_exec(self->store,
                        "UPDATE conversations SET last_activity = COALESCE((SELECT "
                        "max(m.created_at) FROM messages m "
                        "WHERE m.conversation_id = conversations.id AND m.social_hidden = 0 AND "
                        "m.kind NOT IN (5,7,444) "
                        "AND NOT (conversations.backend=3 AND m.kind BETWEEN 1200 AND 1202)),0)",
                        error))
    return FALSE;
  /* Cached subject metadata avoids a plaintext scan on the indexed p path.
   * A present empty subject clears the title. Group names are control state. */
  return gh_store_exec(self->store,
                       "UPDATE conversations SET title=(SELECT m.social_subject FROM messages m "
                       "WHERE m.conversation_id=conversations.id AND m.social_hidden=0 AND "
                       "m.social_subject IS NOT NULL "
                       "ORDER BY m.created_at DESC,m.backend_msg_id DESC LIMIT 1) WHERE backend=1",
                       error);
}
static gboolean
save_revision(GhMessagePolicy *self, GError **error)
{
  sqlite3_stmt *stmt
      = prepare(self, "UPDATE social_preferences SET visibility_revision=?1 WHERE id=1", error);
  if (!stmt)
    return FALSE;
  sqlite3_bind_text(stmt, 1, self->snapshot->revision, -1, SQLITE_STATIC);
  gboolean ok = sql_done(self, stmt, error);
  sqlite3_finalize(stmt);
  return ok;
}
static gboolean
refresh_model(GhMessagePolicy *self, GError **error)
{
  g_autoptr(GhConversationStore) model = g_weak_ref_get(&self->model);
  return !model || gh_conversation_store_refresh_visibility(model, error);
}
static void
projected(GhMessagePolicy *self)
{
  snapshot_free(self->projected_snapshot);
  self->projected_snapshot = snapshot_ref(self->snapshot);
  self->updating = FALSE;
  gh_store_set_visibility_updating(self->store, FALSE);
  g_autoptr(GhConversationStore) model = g_weak_ref_get(&self->model);
  if (model)
    gh_conversation_store_set_visibility_updating(model, FALSE);
  g_signal_emit(self, signals[PROJECTED], 0);
}
static gboolean
author_update(GhMessagePolicy *self, const gchar *pubkey, gboolean hidden, GError **error)
{
  sqlite3_stmt *stmt
      = prepare(self,
                "UPDATE messages INDEXED BY messages_sender SET social_hidden=CASE WHEN "
                "direction=0 AND ?2 THEN 1 ELSE social_content_hidden END WHERE sender_pubkey=?1",
                error);
  if (!stmt)
    return FALSE;
  sqlite3_bind_text(stmt, 1, pubkey, -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, hidden && strcmp(pubkey, self->account));
  gboolean ok = sql_done(self, stmt, error);
  sqlite3_finalize(stmt);
  return ok;
}
static gboolean
indexed_projection(GhMessagePolicy *self, Snapshot *old, GError **error)
{
  if (!gh_store_begin(self->store, error))
    return FALSE;
  gboolean ok = TRUE;
  GHashTableIter it;
  gpointer key;
  if (!old)
    ok = gh_store_exec(self->store, "UPDATE messages SET social_hidden=0,social_content_hidden=0",
                       error);
  if (old) {
    g_hash_table_iter_init(&it, old->authors);
    while (ok && g_hash_table_iter_next(&it, &key, NULL))
      if (!g_hash_table_contains(self->snapshot->authors, key))
        ok = author_update(self, key, FALSE, error);
  }
  g_hash_table_iter_init(&it, self->snapshot->authors);
  while (ok && g_hash_table_iter_next(&it, &key, NULL))
    if (!old || !g_hash_table_contains(old->authors, key))
      ok = author_update(self, key, TRUE, error);
  ok = ok && recount(self, error) && save_revision(self, error);
  if (!ok) {
    gh_store_rollback(self->store);
    return FALSE;
  }
  return gh_store_commit(self->store, error);
}
static gboolean
projection_finish(GhMessagePolicy *self, GError **error)
{
  thread_closure(self);
  if (!gh_store_begin(self->store, error))
    return FALSE;
  /* Mark thread descendants after all edges are known, including forward
   * references. Each graph key is visited at most once. */
  sqlite3_stmt *stmt = prepare(self,
                               "SELECT m.id,m.backend_msg_id,c.backend,c.backend_key,m.direction "
                               "FROM messages m JOIN conversations c ON c.id=m.conversation_id",
                               error);
  gboolean ok = stmt != NULL;
  int rc = SQLITE_DONE;
  sqlite3_stmt *mark
      = ok ? prepare(self,
                     "UPDATE social_visibility_work SET hidden=1 WHERE epoch=?1 AND message_id=?2",
                     error)
           : NULL;
  ok = ok && mark;
  while (ok && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    if (sqlite3_column_int(stmt, 4) != GH_STORE_DIRECTION_IN)
      continue;
    const gchar *id = (const gchar *)sqlite3_column_text(stmt, 1),
                *backend_key = (const gchar *)sqlite3_column_text(stmt, 3);
    g_autofree gchar *room = sqlite3_column_int(stmt, 2) == GH_STORE_BACKEND_MLS
                                 ? gh_message_mls_room_id(backend_key)
                                 : g_strdup(backend_key);
    g_autofree gchar *key = thread_key(room, id);
    if (!g_hash_table_contains(self->threads, key)
        && !g_hash_table_contains(self->snapshot->events, id))
      continue;
    sqlite3_reset(mark);
    sqlite3_bind_int64(mark, 1, self->epoch);
    sqlite3_bind_int64(mark, 2, sqlite3_column_int64(stmt, 0));
    ok = sql_done(self, mark, error);
  }
  sqlite3_finalize(stmt);
  sqlite3_finalize(mark);
  if (ok && rc != SQLITE_DONE) {
    gh_store_set_sqlite_error(self->store, rc, "Projecting thread visibility", error);
    ok = FALSE;
  }
  if (ok) {
    stmt = prepare(self,
                   "UPDATE messages SET social_content_hidden=COALESCE((SELECT hidden FROM "
                   "social_visibility_work w WHERE w.epoch=?1 AND "
                   "w.message_id=messages.id),social_content_hidden)",
                   error);
    ok = stmt != NULL;
    if (stmt) {
      sqlite3_bind_int64(stmt, 1, self->epoch);
      ok = sql_done(self, stmt, error);
      sqlite3_finalize(stmt);
    }
  }
  ok = ok
       && gh_store_exec(self->store, "UPDATE messages SET social_hidden=social_content_hidden",
                        error);
  GHashTableIter it;
  gpointer key;
  g_hash_table_iter_init(&it, self->snapshot->authors);
  while (ok && g_hash_table_iter_next(&it, &key, NULL))
    ok = author_update(self, key, TRUE, error);
  ok = ok && recount(self, error) && save_revision(self, error)
       && gh_store_exec(self->store, "DELETE FROM social_visibility_work", error);
  if (!ok) {
    gh_store_rollback(self->store);
    return FALSE;
  }
  return gh_store_commit(self->store, error);
}
/* Returns MORE, DONE or ERROR. A turn scans at most 256 retained rows. */
static gint
projection_batch(GhMessagePolicy *self, GError **error)
{
  sqlite3_stmt *stmt = prepare(
      self,
      "SELECT m.id,m.backend_msg_id,m.raw_json,m.direction,c.backend,c.backend_key FROM messages m "
      "JOIN conversations c ON c.id=m.conversation_id WHERE m.id>?1 ORDER BY m.id LIMIT 256",
      error);
  if (!stmt)
    return -1;
  sqlite3_bind_int64(stmt, 1, self->cursor);
  if (!gh_store_begin(self->store, error)) {
    sqlite3_finalize(stmt);
    return -1;
  }
  sqlite3_stmt *write = prepare(
      self,
      "INSERT OR REPLACE INTO social_visibility_work(epoch,message_id,hidden) VALUES(?1,?2,?3)",
      error);
  gboolean ok = write != NULL;
  int rc = SQLITE_DONE;
  guint count = 0;
  while (ok && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    self->cursor = sqlite3_column_int64(stmt, 0);
    count++;
    const gchar *id = (const gchar *)sqlite3_column_text(stmt, 1),
                *wire = (const gchar *)sqlite3_column_text(stmt, 2),
                *backend_key = (const gchar *)sqlite3_column_text(stmt, 5);
    g_autofree gchar *room = sqlite3_column_int(stmt, 4) == GH_STORE_BACKEND_MLS
                                 ? gh_message_mls_room_id(backend_key)
                                 : g_strdup(backend_key);
    json_t *event = json_loads(wire, 0, NULL);
    if (!event) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                          "Cannot project stored application content");
      ok = FALSE;
      break;
    }
    remember_edges(self, room, id, event);
    gboolean hidden
        = sqlite3_column_int(stmt, 3) == GH_STORE_DIRECTION_IN && content_matches(self, event);
    json_decref(event);
    sqlite3_reset(write);
    sqlite3_bind_int64(write, 1, self->epoch);
    sqlite3_bind_int64(write, 2, self->cursor);
    sqlite3_bind_int(write, 3, hidden);
    ok = sql_done(self, write, error);
  }
  sqlite3_finalize(stmt);
  sqlite3_finalize(write);
  if (ok && rc != SQLITE_DONE) {
    gh_store_set_sqlite_error(self->store, rc, "Scanning retained history", error);
    ok = FALSE;
  }
  if (!ok) {
    gh_store_rollback(self->store);
    return -1;
  }
  if (!gh_store_commit(self->store, error))
    return -1;
  if (count == 256)
    return 1;
  return projection_finish(self, error) ? 0 : -1;
}
static void
projection_failed(GhMessagePolicy *self)
{
  g_free(self->error);
  self->error = g_strdup("Hidden messages could not be updated");
  g_signal_emit(self, signals[FAILED], 0);
}
static gboolean
projection_idle(gpointer data)
{
  GhMessagePolicy *self = data;
  g_autoptr(GError) error = NULL;
  gint result = projection_batch(self, &error);
  if (result > 0)
    return G_SOURCE_CONTINUE;
  self->source = 0;
  if (result < 0 || !refresh_model(self, &error))
    projection_failed(self);
  else
    projected(self);
  return G_SOURCE_REMOVE;
}

gboolean
gh_message_policy_refresh(GhMessagePolicy *self, GError **error)
{
  if (!self->store) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED,
                        "Account hide policy storage is closed");
    return FALSE;
  }
  Snapshot *next = snapshot_load(self, error);
  if (!next)
    return FALSE;
  Snapshot *old = self->snapshot;
  gboolean content_changed
      = !self->projected_snapshot
        || strcmp(self->projected_snapshot->content_revision, next->content_revision);
  if (old && self->loaded && !strcmp(old->revision, next->revision) && !self->updating) {
    snapshot_free(next);
    return TRUE;
  }
  if (self->source) {
    g_source_remove(self->source);
    self->source = 0;
  }
  gboolean was_updating = self->updating;
  self->snapshot = next;
  self->loaded = TRUE;
  self->epoch++;
  self->cursor = 0;
  g_clear_pointer(&self->error, g_free);
  thread_closure(self);
  g_signal_emit(self, signals[CHANGED], 0);
  gboolean has_content = g_hash_table_size(next->events) || g_hash_table_size(next->hashtags)
                         || next->words->len > 1;
  if ((!content_changed && !was_updating) || (!self->projected_snapshot && !has_content)) {
    gboolean ok = indexed_projection(self, self->projected_snapshot, error);
    if (ok)
      ok = refresh_model(self, error);
    snapshot_free(old);
    if (ok)
      projected(self);
    else {
      self->loaded = FALSE;
      projection_failed(self);
    }
    return ok;
  }
  snapshot_free(old);
  self->updating = TRUE;
  gh_store_set_visibility_updating(self->store, TRUE);
  g_autoptr(GhConversationStore) model = g_weak_ref_get(&self->model);
  if (model)
    gh_conversation_store_set_visibility_updating(model, TRUE);
  if (!gh_store_exec(self->store, "DELETE FROM social_visibility_work", error)) {
    projection_failed(self);
    return FALSE;
  }
  g_hash_table_remove_all(self->parents);
  self->source = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, projection_idle, g_object_ref(self),
                                 g_object_unref);
  return TRUE;
}

gboolean
gh_message_policy_load(GhMessagePolicy *self, GhStore *store, GError **error)
{
  self->store = store;
  self->closed = FALSE;
  Snapshot *snapshot = snapshot_load(self, error);
  if (!snapshot)
    return FALSE;
  sqlite3_stmt *stmt = prepare(self, "SELECT visibility_revision FROM social_preferences WHERE id=1", error);
  if (!stmt) {
    snapshot_free(snapshot);
    return FALSE;
  }
  int rc = sqlite3_step(stmt);
  gboolean matches = rc == SQLITE_ROW &&
    !g_strcmp0((const gchar *)sqlite3_column_text(stmt, 0), snapshot->revision);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_ROW) {
    gh_store_set_sqlite_error(store, rc, "Reading hidden-history revision", error);
    snapshot_free(snapshot);
    return FALSE;
  }
  gboolean empty = !g_hash_table_size(snapshot->authors) && !g_hash_table_size(snapshot->events) &&
    !g_hash_table_size(snapshot->hashtags) && snapshot->words->len == 1;
  gboolean read_only = gh_store_is_read_only(store);
  if (read_only && !matches && !empty) {
    snapshot_free(snapshot);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_READ_ONLY,
                        "Hidden history needs an update in writable account storage");
    return FALSE;
  }
  /* A successfully published cache needs no startup transaction. Thread rules
   * additionally need their retained-edge graph rebuilt before admission. */
  if (read_only || (matches && !g_hash_table_size(snapshot->events))) {
    snapshot_free(self->snapshot);
    self->snapshot = snapshot;
    snapshot_free(self->projected_snapshot);
    self->projected_snapshot = snapshot_ref(snapshot);
    self->loaded = TRUE;
    g_clear_pointer(&self->error, g_free);
    return TRUE;
  }
  snapshot_free(snapshot);
  if (!gh_message_policy_refresh(self, error))
    return FALSE;
  /* No model is exposed yet on bind. Finish the initial projection before
   * a delegate, restored content or a live subscription can become usable. */
  if (self->source) {
    g_source_remove(self->source);
    self->source = 0;
  }
  while (self->updating) {
    gint result = projection_batch(self, error);
    if (result < 0) {
      self->loaded = FALSE;
      projection_failed(self);
      return FALSE;
    }
    if (!result)
      projected(self);
  }
  return TRUE;
}
void
gh_message_policy_set_model(GhMessagePolicy *self, GhConversationStore *model)
{
  g_weak_ref_set(&self->model, model);
}
void
gh_message_policy_committed(GhMessagePolicy *self, GhMessage *message)
{
  if (!g_hash_table_size(self->snapshot->events))
    return;
  json_t *event = json_loads(gh_message_get_rumor_json(message), 0, NULL);
  if (!event)
    return;
  guint before = g_hash_table_size(self->threads);
  remember_edges(self, gh_message_get_room_id(message), gh_message_get_rumor_id(message), event);
  json_decref(event);
  thread_closure(self);
  /* A late parent can hide already-retained descendants. Reuse the normal
   * bounded epoch, not recursion or a timer-based catch-up heuristic. */
  if (g_hash_table_size(self->threads) > before && !self->updating && self->store) {
    self->updating = TRUE;
    self->cursor = 0;
    self->epoch++;
    gh_store_set_visibility_updating(self->store, TRUE);
    g_autoptr(GhConversationStore) model = g_weak_ref_get(&self->model);
    if (model)
      gh_conversation_store_set_visibility_updating(model, TRUE);
    g_signal_emit(self, signals[CHANGED], 0);
    self->source = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, projection_idle, g_object_ref(self),
                                   g_object_unref);
  }
}
void
gh_message_policy_close(GhMessagePolicy *self)
{
  if (self->source) {
    g_source_remove(self->source);
    self->source = 0;
  }
  if (self->store)
    gh_store_set_visibility_updating(self->store, FALSE);
  self->epoch++;
  self->loaded = FALSE;
  self->updating = FALSE;
  self->closed = TRUE;
  self->store = NULL;
  snapshot_free(self->snapshot);
  self->snapshot = NULL;
  snapshot_free(self->projected_snapshot);
  self->projected_snapshot = NULL;
  g_hash_table_remove_all(self->parents);
  g_hash_table_remove_all(self->threads);
  g_weak_ref_set(&self->model, NULL);
}
static void
gh_message_policy_dispose(GObject *object)
{
  gh_message_policy_close(GH_MESSAGE_POLICY(object));
  G_OBJECT_CLASS(gh_message_policy_parent_class)->dispose(object);
}
static void
gh_message_policy_finalize(GObject *object)
{
  GhMessagePolicy *self = GH_MESSAGE_POLICY(object);
  snapshot_free(self->snapshot);
  snapshot_free(self->projected_snapshot);
  g_free(self->account);
  g_free(self->error);
  g_hash_table_unref(self->parents);
  g_hash_table_unref(self->threads);
  g_weak_ref_clear(&self->model);
  G_OBJECT_CLASS(gh_message_policy_parent_class)->finalize(object);
}
static void
gh_message_policy_class_init(GhMessagePolicyClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gh_message_policy_dispose;
  G_OBJECT_CLASS(klass)->finalize = gh_message_policy_finalize;
  signals[CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                  NULL, NULL, G_TYPE_NONE, 0);
  signals[PROJECTED] = g_signal_new("projected", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                                    NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[FAILED] = g_signal_new("failed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                 NULL, NULL, G_TYPE_NONE, 0);
}
static void
gh_message_policy_init(GhMessagePolicy *self)
{
  g_weak_ref_init(&self->model, NULL);
  self->parents
      = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
  self->threads = set_new();
}
GhMessagePolicy *
gh_message_policy_new(const gchar *account)
{
  GhMessagePolicy *self = g_object_new(GH_TYPE_MESSAGE_POLICY, NULL);
  self->account = g_strdup(account);
  return self;
}
gboolean
gh_message_policy_is_loaded(GhMessagePolicy *self)
{
  return self && self->loaded;
}
gboolean
gh_message_policy_is_updating(GhMessagePolicy *self)
{
  return self && self->updating;
}
const gchar *
gh_message_policy_get_error(GhMessagePolicy *self)
{
  return self->error;
}
