/*
 * session_outbox.c — see session_outbox.h.
 */
#include "session_outbox.h"

#include <sqlite3.h>
#include <string.h>

struct NsrOutbox {
  GMutex lock;
  sqlite3 *db;
};

#define UNSETTLED "('new','unroutable','pending')"
/* Delivery records are kept this long (well past any NIP-09 deletion a
 * user is likely to issue); the outbox rows themselves go much sooner. */
#define DELIVERED_KEEP_S (400LL * 24 * 3600)

static const char k_schema[] =
    "CREATE TABLE IF NOT EXISTS events("
    "  id TEXT PRIMARY KEY,"
    "  pubkey TEXT NOT NULL,"
    "  kind INTEGER NOT NULL,"
    "  created_at INTEGER NOT NULL,"
    "  replace_key TEXT,"
    "  json TEXT NOT NULL,"
    "  state TEXT NOT NULL,"
    "  detail TEXT NOT NULL DEFAULT '',"
    "  lane INTEGER NOT NULL DEFAULT 0,"
    "  enqueued_at INTEGER NOT NULL,"
    "  next_route_at INTEGER NOT NULL DEFAULT 0,"
    "  route_attempts INTEGER NOT NULL DEFAULT 0,"
    "  hold INTEGER NOT NULL DEFAULT 0,"
    "  settled_at INTEGER);"
    "CREATE INDEX IF NOT EXISTS events_state ON events(state, next_route_at);"
    "CREATE INDEX IF NOT EXISTS events_replace ON events(replace_key);"
    "CREATE INDEX IF NOT EXISTS events_settled ON events(settled_at);"
    "CREATE TABLE IF NOT EXISTS targets("
    "  event_id TEXT NOT NULL REFERENCES events(id) ON DELETE CASCADE,"
    "  relay TEXT NOT NULL,"
    "  state TEXT NOT NULL,"
    "  attempts INTEGER NOT NULL DEFAULT 0,"
    "  next_attempt_at INTEGER NOT NULL DEFAULT 0,"
    "  last_reason TEXT NOT NULL DEFAULT '',"
    "  updated_at INTEGER NOT NULL,"
    "  acked_at INTEGER,"
    "  PRIMARY KEY(event_id, relay));"
    "CREATE INDEX IF NOT EXISTS targets_due ON targets(state, next_attempt_at);"
    "CREATE TABLE IF NOT EXISTS hints("
    "  pubkey TEXT NOT NULL,"
    "  kind INTEGER NOT NULL,"
    "  created_at INTEGER NOT NULL,"
    "  json TEXT NOT NULL,"
    "  PRIMARY KEY(pubkey, kind));"
    /* Where each event was acknowledged: outlives the pruned event rows so
     * a much later NIP-09 deletion still reaches every relay that holds
     * the original. */
    "CREATE TABLE IF NOT EXISTS delivered("
    "  event_id TEXT NOT NULL,"
    "  relay TEXT NOT NULL,"
    "  replace_key TEXT,"
    "  acked_at INTEGER NOT NULL,"
    "  PRIMARY KEY(event_id, relay));"
    "CREATE INDEX IF NOT EXISTS delivered_replace ON delivered(replace_key);"
    "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value);"
    "PRAGMA user_version = 1;";

/* ── sqlite helpers (caller holds the lock) ───────────────────────────── */

static int exec(NsrOutbox *ob, const char *sql) {
  char *msg = NULL;
  int rc = sqlite3_exec(ob->db, sql, NULL, NULL, &msg);
  if (rc != SQLITE_OK) {
    g_warning("nsr-outbox: %s: %s", sql, msg ? msg : sqlite3_errmsg(ob->db));
    sqlite3_free(msg);
    return -1;
  }
  return 0;
}

static sqlite3_stmt *prep(NsrOutbox *ob, const char *sql) {
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(ob->db, sql, -1, &st, NULL) != SQLITE_OK) {
    g_warning("nsr-outbox: prepare %s: %s", sql, sqlite3_errmsg(ob->db));
    return NULL;
  }
  return st;
}

static void bind_text(sqlite3_stmt *st, int i, const char *v) {
  if (v) sqlite3_bind_text(st, i, v, -1, SQLITE_TRANSIENT);
  else sqlite3_bind_null(st, i);
}

static char *col_dup(sqlite3_stmt *st, int i) {
  const unsigned char *t = sqlite3_column_text(st, i);
  return g_strdup(t ? (const char *)t : "");
}

/* Run a statement with no result rows; returns sqlite3_changes() or -1. */
static int run(NsrOutbox *ob, sqlite3_stmt *st) {
  if (!st) return -1;
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) {
    g_warning("nsr-outbox: step: %s", sqlite3_errmsg(ob->db));
    return -1;
  }
  return sqlite3_changes(ob->db);
}

static void meta_incr(NsrOutbox *ob, const char *key) {
  sqlite3_stmt *st = prep(ob,
      "INSERT INTO meta(key, value) VALUES(?, 1) "
      "ON CONFLICT(key) DO UPDATE SET value = CAST(value AS INTEGER) + 1");
  if (!st) return;
  bind_text(st, 1, key);
  (void)run(ob, st);
}

static void meta_set(NsrOutbox *ob, const char *key, const char *value) {
  sqlite3_stmt *st = prep(ob,
      "INSERT INTO meta(key, value) VALUES(?, ?) "
      "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
  if (!st) return;
  bind_text(st, 1, key);
  bind_text(st, 2, value);
  (void)run(ob, st);
}

static guint64 meta_u64(NsrOutbox *ob, const char *key) {
  sqlite3_stmt *st = prep(ob, "SELECT CAST(value AS INTEGER) FROM meta WHERE key = ?");
  if (!st) return 0;
  bind_text(st, 1, key);
  guint64 v = 0;
  if (sqlite3_step(st) == SQLITE_ROW) v = (guint64)sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  return v;
}

static char *meta_str(NsrOutbox *ob, const char *key) {
  sqlite3_stmt *st = prep(ob, "SELECT value FROM meta WHERE key = ?");
  if (!st) return NULL;
  bind_text(st, 1, key);
  char *v = NULL;
  if (sqlite3_step(st) == SQLITE_ROW) v = col_dup(st, 0);
  sqlite3_finalize(st);
  return v;
}

/* Settle @id into @state if it is still unsettled; bumps the lifetime
 * counter for forwarded/partial/failed/skipped. */
static void settle_event(NsrOutbox *ob, const char *id, const char *state,
                         const char *detail, int64_t now) {
  sqlite3_stmt *st = prep(ob,
      "UPDATE events SET state = ?, detail = COALESCE(?, detail), settled_at = ? "
      "WHERE id = ? AND state IN " UNSETTLED);
  if (!st) return;
  bind_text(st, 1, state);
  bind_text(st, 2, detail);
  sqlite3_bind_int64(st, 3, now);
  bind_text(st, 4, id);
  if (run(ob, st) > 0) {
    char key[32];
    g_snprintf(key, sizeof key, "count_%s", state);
    if (!strcmp(state, "forwarded") || !strcmp(state, "partial") ||
        !strcmp(state, "failed") || !strcmp(state, "skipped"))
      meta_incr(ob, key);
  }
}

/* Recompute a pending event's state from its targets. Returns the event
 * state (static string). */
static const char *recompute(NsrOutbox *ob, const char *id, int64_t now) {
  sqlite3_stmt *st = prep(ob,
      "SELECT COALESCE(SUM(state = 'pending'), 0), COALESCE(SUM(state = 'acked'), 0),"
      "       COALESCE(SUM(state = 'failed'), 0) FROM targets WHERE event_id = ?");
  if (!st) return "pending";
  bind_text(st, 1, id);
  gint64 pending = 0, acked = 0, failed = 0;
  if (sqlite3_step(st) == SQLITE_ROW) {
    pending = sqlite3_column_int64(st, 0);
    acked = sqlite3_column_int64(st, 1);
    failed = sqlite3_column_int64(st, 2);
  }
  sqlite3_finalize(st);
  if (pending > 0) return "pending";
  const char *state = acked > 0 ? (failed > 0 ? "partial" : "forwarded") : "failed";
  settle_event(ob, id, state, NULL, now);
  return state;
}

/* ── Open / close ─────────────────────────────────────────────────────── */

NsrOutbox *nsr_outbox_open(const char *path, GError **error) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open_v2(path, &db,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                               SQLITE_OPEN_FULLMUTEX,
                           NULL);
  if (rc != SQLITE_OK) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "sqlite open %s: %s", path,
                db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    if (db) sqlite3_close(db);
    return NULL;
  }
  NsrOutbox *ob = g_new0(NsrOutbox, 1);
  g_mutex_init(&ob->lock);
  ob->db = db;
  sqlite3_busy_timeout(db, 5000);
  if (exec(ob, "PRAGMA journal_mode = WAL;") != 0 ||
      exec(ob, "PRAGMA synchronous = FULL;") != 0 ||
      exec(ob, "PRAGMA foreign_keys = ON;") != 0 || exec(ob, k_schema) != 0) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "sqlite schema %s: %s", path,
                sqlite3_errmsg(db));
    nsr_outbox_close(ob);
    return NULL;
  }
  return ob;
}

void nsr_outbox_close(NsrOutbox *ob) {
  if (!ob) return;
  sqlite3_close_v2(ob->db);
  g_mutex_clear(&ob->lock);
  g_free(ob);
}

/* ── Ingest ───────────────────────────────────────────────────────────── */

int nsr_outbox_ingest(NsrOutbox *ob, const NsrOutboxIngest *in, int64_t now, GError **error) {
  if (!in->enqueue && !in->hint) return 0;
  g_mutex_lock(&ob->lock);
  int rc = exec(ob, "BEGIN IMMEDIATE");
  if (rc == 0 && in->enqueue) {
    sqlite3_stmt *st = prep(ob,
        "INSERT OR IGNORE INTO events(id, pubkey, kind, created_at, replace_key, json,"
        " state, enqueued_at) VALUES(?, ?, ?, ?, ?, ?, 'new', ?)");
    if (st) {
      bind_text(st, 1, in->id);
      bind_text(st, 2, in->pubkey);
      sqlite3_bind_int(st, 3, in->kind);
      sqlite3_bind_int64(st, 4, in->created_at);
      bind_text(st, 5, in->replace_key);
      bind_text(st, 6, in->json);
      sqlite3_bind_int64(st, 7, now);
    }
    if (run(ob, st) < 0) rc = -1;
  }
  if (rc == 0 && in->hint) {
    sqlite3_stmt *st = prep(ob,
        "INSERT INTO hints(pubkey, kind, created_at, json) VALUES(?, ?, ?, ?) "
        "ON CONFLICT(pubkey, kind) DO UPDATE SET created_at = excluded.created_at,"
        " json = excluded.json WHERE excluded.created_at > hints.created_at");
    if (st) {
      bind_text(st, 1, in->pubkey);
      sqlite3_bind_int(st, 2, in->kind);
      sqlite3_bind_int64(st, 3, in->created_at);
      bind_text(st, 4, in->json);
    }
    int ch = run(ob, st);
    if (ch < 0) rc = -1;
    else if (ch > 0 &&
             exec(ob, "UPDATE events SET next_route_at = 0 WHERE state = 'unroutable'") != 0)
      rc = -1;
  }
  if (rc == 0) rc = exec(ob, "COMMIT");
  if (rc != 0) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "outbox ingest: %s",
                sqlite3_errmsg(ob->db));
    (void)sqlite3_exec(ob->db, "ROLLBACK", NULL, NULL, NULL);
  }
  g_mutex_unlock(&ob->lock);
  return rc;
}

/* ── Routing ──────────────────────────────────────────────────────────── */

void nsr_outbox_event_free(gpointer p) {
  NsrOutboxEvent *e = p;
  if (!e) return;
  g_free(e->id);
  g_free(e->pubkey);
  g_free(e->json);
  g_free(e->replace_key);
  g_free(e);
}

GPtrArray *nsr_outbox_take_routable(NsrOutbox *ob, int64_t now, guint limit) {
  GPtrArray *out = g_ptr_array_new_with_free_func(nsr_outbox_event_free);
  g_mutex_lock(&ob->lock);
  sqlite3_stmt *st = prep(ob,
      "SELECT id, pubkey, json, replace_key, kind, enqueued_at, route_attempts FROM events"
      " WHERE state = 'new' OR (state = 'unroutable' AND next_route_at <= ?)"
      " ORDER BY enqueued_at, rowid LIMIT ?");
  if (st) {
    sqlite3_bind_int64(st, 1, now);
    sqlite3_bind_int(st, 2, (int)limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
      NsrOutboxEvent *e = g_new0(NsrOutboxEvent, 1);
      e->id = col_dup(st, 0);
      e->pubkey = col_dup(st, 1);
      e->json = col_dup(st, 2);
      e->replace_key = sqlite3_column_type(st, 3) == SQLITE_NULL ? NULL : col_dup(st, 3);
      e->kind = sqlite3_column_int(st, 4);
      e->enqueued_at = sqlite3_column_int64(st, 5);
      e->route_attempts = (guint)sqlite3_column_int(st, 6);
      g_ptr_array_add(out, e);
    }
    sqlite3_finalize(st);
  }
  g_mutex_unlock(&ob->lock);
  return out;
}

int nsr_outbox_set_routed(NsrOutbox *ob, const char *id, const char *const *relays,
                          NsrFedLane lane, int64_t now) {
  if (!relays || !relays[0]) return -1;
  g_mutex_lock(&ob->lock);
  int rc = exec(ob, "BEGIN IMMEDIATE");
  if (rc == 0) {
    sqlite3_stmt *st = prep(ob,
        "UPDATE events SET state = 'pending', lane = ?, detail = '', hold = 0 "
        "WHERE id = ? AND state IN ('new', 'unroutable')");
    if (st) {
      sqlite3_bind_int(st, 1, (int)lane);
      bind_text(st, 2, id);
    }
    if (run(ob, st) != 1) rc = -1;
  }
  for (size_t i = 0; rc == 0 && relays[i]; i++) {
    sqlite3_stmt *st = prep(ob,
        "INSERT OR IGNORE INTO targets(event_id, relay, state, updated_at)"
        " VALUES(?, ?, 'pending', ?)");
    if (st) {
      bind_text(st, 1, id);
      bind_text(st, 2, relays[i]);
      sqlite3_bind_int64(st, 3, now);
    }
    if (run(ob, st) < 0) rc = -1;
  }
  if (rc == 0) rc = exec(ob, "COMMIT");
  else (void)sqlite3_exec(ob->db, "ROLLBACK", NULL, NULL, NULL);
  g_mutex_unlock(&ob->lock);
  return rc;
}

int nsr_outbox_set_unroutable(NsrOutbox *ob, const char *id, const char *reason,
                              int64_t next_route_at, gboolean waiting_account, int64_t now) {
  (void)now;
  g_mutex_lock(&ob->lock);
  sqlite3_stmt *st = prep(ob,
      "UPDATE events SET state = 'unroutable', detail = ?, next_route_at = ?, hold = ?,"
      " route_attempts = route_attempts + 1 WHERE id = ? AND state IN ('new', 'unroutable')");
  if (st) {
    bind_text(st, 1, reason ? reason : "");
    sqlite3_bind_int64(st, 2, next_route_at);
    sqlite3_bind_int(st, 3, waiting_account ? 1 : 0);
    bind_text(st, 4, id);
  }
  int rc = run(ob, st) == 1 ? 0 : -1;
  g_mutex_unlock(&ob->lock);
  return rc;
}

int nsr_outbox_set_final(NsrOutbox *ob, const char *id, const char *state,
                         const char *reason, int64_t now) {
  g_mutex_lock(&ob->lock);
  int rc = exec(ob, "BEGIN IMMEDIATE");
  if (rc == 0) {
    sqlite3_stmt *st = prep(ob,
        "UPDATE targets SET state = 'cancelled', updated_at = ? "
        "WHERE event_id = ? AND state = 'pending'");
    if (st) {
      sqlite3_bind_int64(st, 1, now);
      bind_text(st, 2, id);
    }
    if (run(ob, st) < 0) rc = -1;
  }
  if (rc == 0) settle_event(ob, id, state, reason ? reason : "", now);
  if (rc == 0) rc = exec(ob, "COMMIT");
  else (void)sqlite3_exec(ob->db, "ROLLBACK", NULL, NULL, NULL);
  g_mutex_unlock(&ob->lock);
  return rc;
}

/* Settle every id in @ids as @state (targets cancelled). Lock held. */
static int settle_ids(NsrOutbox *ob, GPtrArray *ids, const char *state,
                      const char *detail, int64_t now) {
  for (guint i = 0; i < ids->len; i++) {
    const char *id = g_ptr_array_index(ids, i);
    sqlite3_stmt *st = prep(ob,
        "UPDATE targets SET state = 'cancelled', updated_at = ? "
        "WHERE event_id = ? AND state = 'pending'");
    if (st) {
      sqlite3_bind_int64(st, 1, now);
      bind_text(st, 2, id);
    }
    (void)run(ob, st);
    settle_event(ob, id, state, detail, now);
  }
  return (int)ids->len;
}

static GPtrArray *select_ids(sqlite3_stmt *st) {
  GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
  if (!st) return ids;
  while (sqlite3_step(st) == SQLITE_ROW) g_ptr_array_add(ids, col_dup(st, 0));
  sqlite3_finalize(st);
  return ids;
}

int nsr_outbox_supersede(NsrOutbox *ob, const char *replace_key, const char *keep_id,
                         int64_t created_at, int64_t now) {
  if (!replace_key) return 0;
  g_mutex_lock(&ob->lock);
  sqlite3_stmt *st = prep(ob,
      "SELECT id FROM events WHERE replace_key = ? AND id != ? AND state IN " UNSETTLED
      " AND (created_at < ? OR (created_at = ? AND id < ?))");
  if (st) {
    bind_text(st, 1, replace_key);
    bind_text(st, 2, keep_id);
    sqlite3_bind_int64(st, 3, created_at);
    sqlite3_bind_int64(st, 4, created_at);
    bind_text(st, 5, keep_id);
  }
  GPtrArray *ids = select_ids(st);
  char *detail = g_strdup_printf("superseded by %s", keep_id);
  int n = 0;
  if (ids->len && exec(ob, "BEGIN IMMEDIATE") == 0) {
    n = settle_ids(ob, ids, "superseded", detail, now);
    if (exec(ob, "COMMIT") != 0) n = -1;
  }
  g_free(detail);
  g_ptr_array_unref(ids);
  g_mutex_unlock(&ob->lock);
  return n;
}

int nsr_outbox_cancel_ref(NsrOutbox *ob, const char *pubkey, const char *ref,
                          gboolean is_coordinate, int64_t now) {
  if (!pubkey || !ref) return 0;
  g_mutex_lock(&ob->lock);
  sqlite3_stmt *st = prep(ob, is_coordinate
      ? "SELECT id FROM events WHERE replace_key = ? AND pubkey = ? AND state IN " UNSETTLED
      : "SELECT id FROM events WHERE id = ? AND pubkey = ? AND state IN " UNSETTLED);
  if (st) {
    bind_text(st, 1, ref);
    bind_text(st, 2, pubkey);
  }
  GPtrArray *ids = select_ids(st);
  int n = 0;
  if (ids->len && exec(ob, "BEGIN IMMEDIATE") == 0) {
    n = settle_ids(ob, ids, "cancelled", "deleted by the author (NIP-09) before forwarding", now);
    if (exec(ob, "COMMIT") != 0) n = -1;
  }
  g_ptr_array_unref(ids);
  g_mutex_unlock(&ob->lock);
  return n;
}

GStrv nsr_outbox_acked_relays(NsrOutbox *ob, const char *ref, gboolean is_coordinate) {
  if (!ref) return NULL;
  g_mutex_lock(&ob->lock);
  sqlite3_stmt *st = prep(ob, is_coordinate
      ? "SELECT DISTINCT relay FROM delivered WHERE replace_key = ? ORDER BY relay"
      : "SELECT DISTINCT relay FROM delivered WHERE event_id = ? ORDER BY relay");
  if (st) bind_text(st, 1, ref);
  GPtrArray *r = select_ids(st);
  g_mutex_unlock(&ob->lock);
  g_ptr_array_add(r, NULL);
  g_ptr_array_set_free_func(r, NULL);
  return (GStrv)g_ptr_array_free(r, FALSE);
}

char *nsr_outbox_hint_json(NsrOutbox *ob, const char *pubkey, int kind) {
  if (!pubkey) return NULL;
  g_mutex_lock(&ob->lock);
  sqlite3_stmt *st = prep(ob, "SELECT json FROM hints WHERE pubkey = ? AND kind = ?");
  char *json = NULL;
  if (st) {
    bind_text(st, 1, pubkey);
    sqlite3_bind_int(st, 2, kind);
    if (sqlite3_step(st) == SQLITE_ROW) json = col_dup(st, 0);
    sqlite3_finalize(st);
  }
  g_mutex_unlock(&ob->lock);
  return json;
}

/* ── Delivery ─────────────────────────────────────────────────────────── */

void nsr_outbox_due_free(gpointer p) {
  NsrOutboxDue *d = p;
  if (!d) return;
  g_free(d->event_id);
  g_free(d->relay);
  g_free(d->json);
  g_free(d);
}

GPtrArray *nsr_outbox_take_due(NsrOutbox *ob, int64_t now, int lease_seconds, guint limit) {
  GPtrArray *out = g_ptr_array_new_with_free_func(nsr_outbox_due_free);
  g_mutex_lock(&ob->lock);
  sqlite3_stmt *st = prep(ob,
      "SELECT t.event_id, t.relay, e.json, e.lane, t.attempts FROM targets t"
      " JOIN events e ON e.id = t.event_id"
      " WHERE t.state = 'pending' AND t.next_attempt_at <= ?"
      " ORDER BY e.enqueued_at, t.rowid LIMIT ?");
  if (st) {
    sqlite3_bind_int64(st, 1, now);
    sqlite3_bind_int(st, 2, (int)limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
      NsrOutboxDue *d = g_new0(NsrOutboxDue, 1);
      d->event_id = col_dup(st, 0);
      d->relay = col_dup(st, 1);
      d->json = col_dup(st, 2);
      d->lane = (NsrFedLane)sqlite3_column_int(st, 3);
      d->attempts = (guint)sqlite3_column_int(st, 4);
      g_ptr_array_add(out, d);
    }
    sqlite3_finalize(st);
  }
  if (out->len && exec(ob, "BEGIN IMMEDIATE") == 0) {
    for (guint i = 0; i < out->len; i++) {
      NsrOutboxDue *d = g_ptr_array_index(out, i);
      sqlite3_stmt *u = prep(ob,
          "UPDATE targets SET next_attempt_at = ? WHERE event_id = ? AND relay = ?");
      if (u) {
        sqlite3_bind_int64(u, 1, now + lease_seconds);
        bind_text(u, 2, d->event_id);
        bind_text(u, 3, d->relay);
      }
      (void)run(ob, u);
    }
    (void)exec(ob, "COMMIT");
  }
  g_mutex_unlock(&ob->lock);
  return out;
}

int nsr_outbox_record(NsrOutbox *ob, const NsrFedConfig *cfg, const char *event_id,
                      const char *relay, NsrOutboxResult result, const char *reason,
                      double jitter01, int64_t now, const char **out_target_state,
                      const char **out_event_state) {
  if (out_target_state) *out_target_state = NULL;
  if (out_event_state) *out_event_state = NULL;
  g_mutex_lock(&ob->lock);
  sqlite3_stmt *st = prep(ob,
      "SELECT t.attempts, e.enqueued_at FROM targets t JOIN events e ON e.id = t.event_id"
      " WHERE t.event_id = ? AND t.relay = ? AND t.state = 'pending'");
  gboolean found = FALSE;
  guint attempts = 0;
  int64_t enqueued_at = now;
  if (st) {
    bind_text(st, 1, event_id);
    bind_text(st, 2, relay);
    if (sqlite3_step(st) == SQLITE_ROW) {
      found = TRUE;
      attempts = (guint)sqlite3_column_int(st, 0);
      enqueued_at = sqlite3_column_int64(st, 1);
    }
    sqlite3_finalize(st);
  }
  if (!found) {
    g_mutex_unlock(&ob->lock);
    return -1;
  }
  const char *why = reason ? reason : "";
  const char *tstate = "pending";
  char *stored_reason = g_strdup(why);
  int64_t next = 0;
  if (result == NSR_OUTBOX_ACKED) {
    tstate = "acked";
  } else if (result == NSR_OUTBOX_PERMANENT) {
    tstate = "failed";
  } else {
    attempts++;
    if (now - enqueued_at >= (int64_t)cfg->max_age_seconds) {
      tstate = "failed";
      g_free(stored_reason);
      stored_reason = g_strdup_printf("expired: %s", why);
    } else {
      next = now + nsr_fed_backoff_delay(cfg, attempts, jitter01);
    }
  }
  int rc = exec(ob, "BEGIN IMMEDIATE");
  if (rc == 0) {
    sqlite3_stmt *u = prep(ob,
        "UPDATE targets SET state = ?, attempts = ?, next_attempt_at = ?, last_reason = ?,"
        " updated_at = ?, acked_at = CASE WHEN ? = 'acked' THEN ? ELSE acked_at END"
        " WHERE event_id = ? AND relay = ?");
    if (u) {
      bind_text(u, 1, tstate);
      sqlite3_bind_int(u, 2, (int)attempts);
      sqlite3_bind_int64(u, 3, next);
      bind_text(u, 4, stored_reason);
      sqlite3_bind_int64(u, 5, now);
      bind_text(u, 6, tstate);
      sqlite3_bind_int64(u, 7, now);
      bind_text(u, 8, event_id);
      bind_text(u, 9, relay);
    }
    if (run(ob, u) != 1) rc = -1;
  }
  if (rc == 0 && strcmp(tstate, "acked") == 0) {
    sqlite3_stmt *d = prep(ob,
        "INSERT OR IGNORE INTO delivered(event_id, relay, replace_key, acked_at)"
        " SELECT id, ?, replace_key, ? FROM events WHERE id = ?");
    if (d) {
      bind_text(d, 1, relay);
      sqlite3_bind_int64(d, 2, now);
      bind_text(d, 3, event_id);
    }
    if (run(ob, d) < 0) rc = -1;
  }
  const char *estate = "pending";
  if (rc == 0) {
    if (result != NSR_OUTBOX_ACKED) {
      char *msg = g_strdup_printf("%s: %s", relay, stored_reason);
      meta_set(ob, "last_error", msg);
      g_free(msg);
    }
    estate = recompute(ob, event_id, now);
    rc = exec(ob, "COMMIT");
  }
  if (rc != 0) (void)sqlite3_exec(ob->db, "ROLLBACK", NULL, NULL, NULL);
  g_free(stored_reason);
  g_mutex_unlock(&ob->lock);
  if (rc == 0) {
    if (out_target_state) *out_target_state = tstate;
    if (out_event_state) *out_event_state = estate;
  }
  return rc;
}

int nsr_outbox_expire(NsrOutbox *ob, int64_t now, int64_t max_age) {
  int64_t cutoff = now - max_age;
  g_mutex_lock(&ob->lock);
  int n = 0;
  /* hold = 1: waiting for the local account to become known, which is not
   * the event's fault; it is forwarded once the account is learned. */
  sqlite3_stmt *st = prep(ob,
      "SELECT id, state FROM events WHERE state IN " UNSETTLED " AND enqueued_at < ?"
      " AND hold = 0");
  GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
  GPtrArray *states = g_ptr_array_new_with_free_func(g_free);
  if (st) {
    sqlite3_bind_int64(st, 1, cutoff);
    while (sqlite3_step(st) == SQLITE_ROW) {
      g_ptr_array_add(ids, col_dup(st, 0));
      g_ptr_array_add(states, col_dup(st, 1));
    }
    sqlite3_finalize(st);
  }
  if (ids->len && exec(ob, "BEGIN IMMEDIATE") == 0) {
    for (guint i = 0; i < ids->len; i++) {
      const char *id = g_ptr_array_index(ids, i);
      if (strcmp(g_ptr_array_index(states, i), "pending") == 0) {
        sqlite3_stmt *u = prep(ob,
            "UPDATE targets SET state = 'failed', last_reason = 'expired: ' || last_reason,"
            " updated_at = ? WHERE event_id = ? AND state = 'pending'");
        if (u) {
          sqlite3_bind_int64(u, 1, now);
          bind_text(u, 2, id);
        }
        (void)run(ob, u);
        (void)recompute(ob, id, now);
      } else {
        sqlite3_stmt *u = prep(ob, "SELECT detail FROM events WHERE id = ?");
        char *detail = NULL;
        if (u) {
          bind_text(u, 1, id);
          if (sqlite3_step(u) == SQLITE_ROW) detail = col_dup(u, 0);
          sqlite3_finalize(u);
        }
        char *d2 = g_strdup_printf("expired: %s", detail ? detail : "never routed");
        settle_event(ob, id, "failed", d2, now);
        g_free(d2);
        g_free(detail);
      }
      n++;
    }
    (void)exec(ob, "COMMIT");
  }
  g_ptr_array_unref(ids);
  g_ptr_array_unref(states);
  g_mutex_unlock(&ob->lock);
  return n;
}

int nsr_outbox_prune(NsrOutbox *ob, int64_t now, int64_t keep) {
  g_mutex_lock(&ob->lock);
  sqlite3_stmt *st = prep(ob, "DELETE FROM events WHERE settled_at IS NOT NULL AND settled_at < ?");
  if (st) sqlite3_bind_int64(st, 1, now - keep);
  int n = run(ob, st);
  st = prep(ob, "DELETE FROM delivered WHERE acked_at < ?");
  if (st) sqlite3_bind_int64(st, 1, now - DELIVERED_KEEP_S);
  (void)run(ob, st);
  g_mutex_unlock(&ob->lock);
  return n;
}

void nsr_outbox_reroute_all(NsrOutbox *ob) {
  g_mutex_lock(&ob->lock);
  (void)exec(ob, "UPDATE events SET next_route_at = 0 WHERE state = 'unroutable'");
  g_mutex_unlock(&ob->lock);
}

void nsr_outbox_release_lease(NsrOutbox *ob, const char *event_id, const char *relay,
                              int64_t now) {
  g_mutex_lock(&ob->lock);
  sqlite3_stmt *st = prep(ob,
      "UPDATE targets SET next_attempt_at = ? "
      "WHERE event_id = ? AND relay = ? AND state = 'pending' AND next_attempt_at > ?");
  if (st) {
    sqlite3_bind_int64(st, 1, now);
    bind_text(st, 2, event_id);
    bind_text(st, 3, relay);
    sqlite3_bind_int64(st, 4, now);
    (void)run(ob, st);
  }
  g_mutex_unlock(&ob->lock);
}

int64_t nsr_outbox_next_due(NsrOutbox *ob) {
  g_mutex_lock(&ob->lock);
  int64_t best = INT64_MAX;
  static const char *const qs[] = {
      "SELECT MIN(next_attempt_at) FROM targets WHERE state = 'pending'",
      "SELECT MIN(next_route_at) FROM events WHERE state = 'unroutable'",
      "SELECT 0 FROM events WHERE state = 'new' LIMIT 1",
  };
  for (size_t i = 0; i < G_N_ELEMENTS(qs); i++) {
    sqlite3_stmt *st = prep(ob, qs[i]);
    if (!st) continue;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) {
      int64_t v = sqlite3_column_int64(st, 0);
      if (v < best) best = v;
    }
    sqlite3_finalize(st);
  }
  g_mutex_unlock(&ob->lock);
  return best;
}

/* ── Observability ────────────────────────────────────────────────────── */

void nsr_outbox_stats(NsrOutbox *ob, NsrOutboxStats *out) {
  memset(out, 0, sizeof *out);
  g_mutex_lock(&ob->lock);
  sqlite3_stmt *st = prep(ob,
      "SELECT COALESCE(SUM(state IN " UNSETTLED "), 0), COALESCE(SUM(state = 'unroutable'), 0)"
      " FROM events");
  if (st) {
    if (sqlite3_step(st) == SQLITE_ROW) {
      out->queued = (guint64)sqlite3_column_int64(st, 0);
      out->unroutable = (guint64)sqlite3_column_int64(st, 1);
    }
    sqlite3_finalize(st);
  }
  st = prep(ob, "SELECT COUNT(*) FROM targets WHERE state = 'pending'");
  if (st) {
    if (sqlite3_step(st) == SQLITE_ROW) out->pending_targets = (guint64)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
  }
  out->forwarded = meta_u64(ob, "count_forwarded");
  out->partial = meta_u64(ob, "count_partial");
  out->failed = meta_u64(ob, "count_failed");
  out->skipped = meta_u64(ob, "count_skipped");
  out->last_error = meta_str(ob, "last_error");
  if (!out->last_error) out->last_error = g_strdup("");
  g_mutex_unlock(&ob->lock);
}

void nsr_outbox_stats_clear(NsrOutboxStats *s) {
  g_clear_pointer(&s->last_error, g_free);
}

void nsr_outbox_set_last_error(NsrOutbox *ob, const char *msg) {
  g_mutex_lock(&ob->lock);
  meta_set(ob, "last_error", msg ? msg : "");
  g_mutex_unlock(&ob->lock);
}

void nsr_outbox_target_info_free(gpointer p) {
  NsrOutboxTargetInfo *t = p;
  if (!t) return;
  g_free(t->relay);
  g_free(t->state);
  g_free(t->reason);
  g_free(t);
}

int nsr_outbox_event_status(NsrOutbox *ob, const char *id, char **state, char **detail,
                            GPtrArray **targets) {
  g_mutex_lock(&ob->lock);
  sqlite3_stmt *st = prep(ob, "SELECT state, detail FROM events WHERE id = ?");
  int rc = -1;
  if (st) {
    bind_text(st, 1, id);
    if (sqlite3_step(st) == SQLITE_ROW) {
      rc = 0;
      if (state) *state = col_dup(st, 0);
      if (detail) *detail = col_dup(st, 1);
    }
    sqlite3_finalize(st);
  }
  if (rc == 0 && targets) {
    *targets = g_ptr_array_new_with_free_func(nsr_outbox_target_info_free);
    st = prep(ob,
        "SELECT relay, state, last_reason, attempts, updated_at, COALESCE(acked_at, 0)"
        " FROM targets WHERE event_id = ? ORDER BY rowid");
    if (st) {
      bind_text(st, 1, id);
      while (sqlite3_step(st) == SQLITE_ROW) {
        NsrOutboxTargetInfo *t = g_new0(NsrOutboxTargetInfo, 1);
        t->relay = col_dup(st, 0);
        t->state = col_dup(st, 1);
        t->reason = col_dup(st, 2);
        t->attempts = (guint)sqlite3_column_int(st, 3);
        t->updated_at = sqlite3_column_int64(st, 4);
        t->acked_at = sqlite3_column_int64(st, 5);
        g_ptr_array_add(*targets, t);
      }
      sqlite3_finalize(st);
    }
  }
  g_mutex_unlock(&ob->lock);
  return rc;
}

void nsr_outbox_relay_counts_free(gpointer p) {
  NsrOutboxRelayCounts *c = p;
  if (!c) return;
  g_free(c->relay);
  g_free(c);
}

GPtrArray *nsr_outbox_relay_counts(NsrOutbox *ob) {
  GPtrArray *out = g_ptr_array_new_with_free_func(nsr_outbox_relay_counts_free);
  g_mutex_lock(&ob->lock);
  sqlite3_stmt *st = prep(ob,
      "SELECT relay, SUM(state = 'pending'), SUM(state = 'acked'), SUM(state = 'failed')"
      " FROM targets GROUP BY relay ORDER BY relay");
  if (st) {
    while (sqlite3_step(st) == SQLITE_ROW) {
      NsrOutboxRelayCounts *c = g_new0(NsrOutboxRelayCounts, 1);
      c->relay = col_dup(st, 0);
      c->pending = (guint)sqlite3_column_int64(st, 1);
      c->acked = (guint64)sqlite3_column_int64(st, 2);
      c->failed = (guint64)sqlite3_column_int64(st, 3);
      g_ptr_array_add(out, c);
    }
    sqlite3_finalize(st);
  }
  g_mutex_unlock(&ob->lock);
  return out;
}

gboolean nsr_outbox_eviction_eligible(NsrOutbox *ob, const char *id) {
  g_mutex_lock(&ob->lock);
  sqlite3_stmt *st = prep(ob, "SELECT state IN " UNSETTLED " FROM events WHERE id = ?");
  gboolean eligible = TRUE;
  if (st) {
    bind_text(st, 1, id);
    if (sqlite3_step(st) == SQLITE_ROW) eligible = sqlite3_column_int(st, 0) == 0;
    sqlite3_finalize(st);
  } else {
    eligible = FALSE; /* cannot tell: keep it */
  }
  g_mutex_unlock(&ob->lock);
  return eligible;
}

GStrv nsr_outbox_get_accounts(NsrOutbox *ob) {
  g_mutex_lock(&ob->lock);
  char *v = meta_str(ob, "accounts");
  g_mutex_unlock(&ob->lock);
  GStrv out = g_strsplit(v ? v : "", ",", -1);
  g_free(v);
  /* drop empty entries */
  GPtrArray *a = g_ptr_array_new();
  for (guint i = 0; out[i]; i++)
    if (*out[i]) g_ptr_array_add(a, g_strdup(out[i]));
  g_strfreev(out);
  g_ptr_array_add(a, NULL);
  return (GStrv)g_ptr_array_free(a, FALSE);
}

void nsr_outbox_add_account(NsrOutbox *ob, const char *pubkey_hex) {
  if (!pubkey_hex || !*pubkey_hex) return;
  GStrv cur = nsr_outbox_get_accounts(ob);
  gboolean have = g_strv_contains((const gchar *const *)cur, pubkey_hex);
  if (!have) {
    GString *s = g_string_new(NULL);
    for (guint i = 0; cur[i]; i++) g_string_append_printf(s, "%s,", cur[i]);
    g_string_append(s, pubkey_hex);
    g_mutex_lock(&ob->lock);
    meta_set(ob, "accounts", s->str);
    g_mutex_unlock(&ob->lock);
    g_string_free(s, TRUE);
  }
  g_strfreev(cur);
}
