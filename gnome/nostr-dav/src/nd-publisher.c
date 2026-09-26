/* nd-publisher.c - Outbox worker for nostr-dav DAV writes
 *
 * SPDX-License-Identifier: MIT
 *
 * See nd-publisher.h for the design. Implementation notes:
 *
 *   * Row lifecycle mirrors the schema comment: rows stay `pending`
 *     while at least one relay in their target set has not yet
 *     acknowledged. `published` only when EVERY target relay OK'd —
 *     that is the NIP-51/65 outbox commit contract per plan Q4.
 *   * Backoff is exponential with a 60 s floor and 60 min cap; the
 *     effective delay is `min(60 * 2^attempts, 3600)` seconds.
 *   * Failure taxonomy uses the NIP-01 OK reason prefix:
 *       - "duplicate:"                        -> accepted (already there)
 *       - "invalid:" | "blocked:" | "banned:" -> permanent
 *       - everything else, incl. no OK at all -> transient
 *   * Notification dedup uses a per-row bit stored via `notified` in
 *     the in-memory PendingRow, plus the row's `publish_state` check
 *     when the tick loop re-encounters an already-failed row.
 */

#include "nd-publisher.h"

#include "nd-ical.h"
#include "nd-vcard.h"
#include "nd-calendar-store.h"
#include "nd-contact-store.h"

#include <json-glib/json-glib.h>
#include <sqlite3.h>

#include <string.h>

G_DEFINE_QUARK(nd-publisher-error-quark, nd_publisher_error)

#define ND_PUBLISH_BACKOFF_INITIAL_SEC 60
#define ND_PUBLISH_BACKOFF_MAX_SEC     (60 * 60)

/* Wall-clock budget for an in-flight publish attempt. A relay that
 * accepts an EVENT frame but never sends OK would otherwise pin the
 * pending row in memory forever; when @deadline is passed the tick
 * loop treats every still-waiting relay as a transient failure and
 * schedules the retry. 120 s balances "give a slow relay a chance"
 * with "do not deadlock the outbox". */
#define ND_PUBLISH_OK_WAIT_SEC         120

typedef enum {
  ND_OK_TRANSIENT = 0,
  ND_OK_ACCEPT,          /* OK true, or duplicate: */
  ND_OK_PERMANENT_FAIL   /* invalid:/blocked:/banned: */
} NdOkClass;

/* Discriminator: primary outbox rows (events/contacts/files) live in
 * their own tables; kind-5 tombstones (nostrc-ls2c) live in the
 * `tombstones` table and are keyed by an AUTOINCREMENT `id`. Both
 * pathways share the same pending-row bookkeeping and settle through
 * finalize_row(); the enum lets the publisher pick the right SQL. */
typedef enum {
  ND_OUTBOX_COLLECTION = 0,  /* events / contacts / files */
  ND_OUTBOX_TOMBSTONE  = 1
} NdOutboxKind;

typedef struct {
  NdOutboxKind       outbox;
  NdStoreCollection  collection;  /* meaningful only for ND_OUTBOX_COLLECTION */
  gchar             *row_id;      /* uid for collections, decimal id for tombstones */
  gchar             *event_id;
  GHashTable        *waiting;     /* url -> bool (still waiting) */
  GHashTable        *acked;
  GHashTable        *failed;      /* transient failures this attempt */
  gboolean           notified;
  gint64             deadline;    /* wall clock; row escalates past it */
} NdPendingRow;

struct _NdPublisher {
  NdStoreDb  *db;
  NdSigner   *signer;

  NdRelayTransportFactory  factory;
  gpointer                 factory_data;

  gchar          *account_pubkey;
  GStrv           home_relays;    /* default target set */
  NdPublishQuorum quorum;

  /* Bound transports (URL -> NdRelayTransport*, unowned by publisher).
   * Populated by nd_publisher_bind_transport(); tick() also asks the
   * factory when the URL is not bound. */
  GHashTable    *bound;           /* char*(borrowed) -> NdRelayTransport*(unowned) */

  /* In-flight publish state, keyed by event_id (so record_ok() O(1)). */
  GHashTable    *in_flight;       /* char*(owned) -> NdPendingRow* */

  NdPublisherNotifyCallback notify_cb;
  gpointer                  notify_data;
};

/* ---- Small helpers ---- */

static void
pending_row_free(gpointer data)
{
  NdPendingRow *row = data;
  if (row == NULL) return;
  g_free(row->row_id);
  g_free(row->event_id);
  g_clear_pointer(&row->waiting, g_hash_table_destroy);
  g_clear_pointer(&row->acked,   g_hash_table_destroy);
  g_clear_pointer(&row->failed,  g_hash_table_destroy);
  g_free(row);
}

static NdOkClass
classify_ok(gboolean ok, const gchar *reason)
{
  if (ok)
    return ND_OK_ACCEPT;
  if (reason == NULL)
    return ND_OK_TRANSIENT;
  if (g_str_has_prefix(reason, "duplicate:"))
    return ND_OK_ACCEPT;
  if (g_str_has_prefix(reason, "invalid:") ||
      g_str_has_prefix(reason, "blocked:") ||
      g_str_has_prefix(reason, "banned:") ||
      /* NIP-42 auth handshake is not implemented in v1; a relay that
       * refuses without a signed AUTH frame is effectively permanent
       * for us until the follow-up bead adds AUTH. Classifying as
       * transient would spin the outbox forever. */
      g_str_has_prefix(reason, "restricted:") ||
      g_str_has_prefix(reason, "auth-required:"))
    return ND_OK_PERMANENT_FAIL;
  return ND_OK_TRANSIENT;
}

static gint64
backoff_delay(int attempts)
{
  gint64 delay = ND_PUBLISH_BACKOFF_INITIAL_SEC;
  for (int i = 0; i < attempts && delay < ND_PUBLISH_BACKOFF_MAX_SEC; i++)
    delay *= 2;
  if (delay > ND_PUBLISH_BACKOFF_MAX_SEC)
    delay = ND_PUBLISH_BACKOFF_MAX_SEC;
  return delay;
}

static const gchar *
collection_table(NdStoreCollection col)
{
  switch (col) {
  case ND_STORE_COLLECTION_EVENTS:   return "events";
  case ND_STORE_COLLECTION_CONTACTS: return "contacts";
  case ND_STORE_COLLECTION_FILES:    return "files";
  }
  g_return_val_if_reached("events");
}

static const gchar *
collection_key(NdStoreCollection col)
{
  return col == ND_STORE_COLLECTION_FILES ? "path" : "uid";
}

/* Tombstones share the outbox column names but live in their own table
 * keyed by AUTOINCREMENT `id` (stringified so the same bookkeeping
 * plumbing that keys collections by uid works unmodified). The two
 * helpers keep the SQL string builders symmetric with the collection
 * versions above, and outbox_table/outbox_key give the SQL builders a
 * single (outbox, col) → (table, key) lookup that closes both worlds. */
static const gchar *tombstone_table(void) { return "tombstones"; }
static const gchar *tombstone_key(void)   { return "id"; }

static const gchar *
outbox_table(NdOutboxKind outbox, NdStoreCollection col)
{
  return outbox == ND_OUTBOX_TOMBSTONE ? tombstone_table() : collection_table(col);
}

static const gchar *
outbox_key(NdOutboxKind outbox, NdStoreCollection col)
{
  return outbox == ND_OUTBOX_TOMBSTONE ? tombstone_key() : collection_key(col);
}

/* Log a per-relay attempt uses target_kind to disambiguate rows across
 * the different outboxes. Primary outbox rows reuse NdStoreCollection
 * values (0-2). Tombstones live on their own so pick a value outside
 * that range — 100 is far enough to survive any future collections. */
#define ND_LOG_KIND_TOMBSTONE 100

static int
outbox_log_kind(NdOutboxKind outbox, NdStoreCollection col)
{
  return outbox == ND_OUTBOX_TOMBSTONE ? ND_LOG_KIND_TOMBSTONE : (int)col;
}

/* ---- Target-set serialisation ---- */

static gchar *
targets_to_json(const GStrv urls)
{
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_array(b);
  if (urls != NULL)
    for (guint i = 0; urls[i] != NULL; i++)
      json_builder_add_string_value(b, urls[i]);
  json_builder_end_array(b);
  g_autoptr(JsonGenerator) gen = json_generator_new();
  g_autoptr(JsonNode) root = json_builder_get_root(b);
  json_generator_set_root(gen, root);
  return json_generator_to_data(gen, NULL);
}

static GStrv
targets_from_json(const gchar *json_str)
{
  if (json_str == NULL || *json_str == '\0')
    return NULL;
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, json_str, -1, NULL))
    return NULL;
  JsonNode *root = json_parser_get_root(parser);
  if (root == NULL || JSON_NODE_TYPE(root) != JSON_NODE_ARRAY)
    return NULL;
  JsonArray *arr = json_node_get_array(root);
  guint n = json_array_get_length(arr);
  GStrv out = g_new0(gchar *, n + 1);
  guint k = 0;
  for (guint i = 0; i < n; i++) {
    const gchar *s = json_array_get_string_element(arr, i);
    if (s != NULL)
      out[k++] = g_strdup(s);
  }
  out[k] = NULL;
  return out;
}

/* ---- SQL: staging + row lookup ---- */

static gboolean
stage_row(NdPublisher       *self,
          NdStoreCollection  col,
          const gchar       *row_id,
          gint64             now_ts,
          GError           **error)
{
  g_autofree gchar *targets_json = targets_to_json(self->home_relays);
  sqlite3 *h = nd_store_db_get_handle(self->db);
  g_autofree gchar *sql = g_strdup_printf(
    "UPDATE %s SET publish_state = 'pending', "
    "  publish_attempts = 0, publish_next_ts = ?1, "
    "  publish_targets = ?2 "
    "WHERE %s = ?3",
    collection_table(col), collection_key(col));

  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(h, sql, -1, &stmt, NULL) != SQLITE_OK)
    return nd_store_db_set_sql_error(self->db, error, "prepare stage");

  sqlite3_bind_int64(stmt, 1, now_ts);
  sqlite3_bind_text (stmt, 2, targets_json, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (stmt, 3, row_id, -1, SQLITE_TRANSIENT);

  gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  if (!ok)
    nd_store_db_set_sql_error(self->db, error, "stage row");
  sqlite3_finalize(stmt);
  return ok;
}

/* Insert a tombstone row (nostrc-ls2c). Copies the publisher's configured
 * home relays into publish_targets so a later NIP-65 change does not
 * silently retarget the retry — same rationale as the primary outbox. */
static gboolean
stage_tombstone_row(NdPublisher *self,
                    int          target_kind,
                    const gchar *target_pubkey_hex,
                    const gchar *target_uid,
                    gint64       now_ts,
                    GError     **error)
{
  g_autofree gchar *targets_json = targets_to_json(self->home_relays);
  sqlite3 *h = nd_store_db_get_handle(self->db);

  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(h,
        "INSERT INTO tombstones"
        "  (target_kind, target_pubkey, target_uid, publish_state,"
        "   publish_attempts, publish_next_ts, publish_targets, created_at)"
        "  VALUES (?1, ?2, ?3, 'pending', 0, ?4, ?5, ?6)",
        -1, &stmt, NULL) != SQLITE_OK)
    return nd_store_db_set_sql_error(self->db, error, "prepare stage tombstone");

  sqlite3_bind_int  (stmt, 1, target_kind);
  sqlite3_bind_text (stmt, 2, target_pubkey_hex, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (stmt, 3, target_uid, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 4, now_ts);
  if (targets_json != NULL)
    sqlite3_bind_text(stmt, 5, targets_json, -1, SQLITE_TRANSIENT);
  else
    sqlite3_bind_null(stmt, 5);
  sqlite3_bind_int64(stmt, 6, now_ts);

  gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  if (!ok)
    nd_store_db_set_sql_error(self->db, error, "stage tombstone");
  sqlite3_finalize(stmt);
  return ok;
}

/* Log a per-relay publish attempt. Errors are non-fatal — the outbox
 * itself is authoritative and the log is for operator debugging. */
static void
log_attempt(NdPublisher       *self,
            NdOutboxKind       outbox,
            NdStoreCollection  col,
            const gchar       *row_id,
            const gchar       *relay_url,
            int                http_status,
            const gchar       *error_msg)
{
  sqlite3 *h = nd_store_db_get_handle(self->db);
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(h,
        "INSERT INTO publish_log "
        "  (target_kind, target_uid, relay_url, attempted_at, "
        "   http_status, error) "
        "  VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
        -1, &stmt, NULL) != SQLITE_OK)
    return;

  sqlite3_bind_int  (stmt, 1, outbox_log_kind(outbox, col));
  sqlite3_bind_text (stmt, 2, row_id, -1, SQLITE_TRANSIENT);
  if (relay_url != NULL)
    sqlite3_bind_text(stmt, 3, relay_url, -1, SQLITE_TRANSIENT);
  else
    sqlite3_bind_null(stmt, 3);
  sqlite3_bind_int64(stmt, 4, g_get_real_time() / G_USEC_PER_SEC);
  sqlite3_bind_int  (stmt, 5, http_status);
  if (error_msg != NULL)
    sqlite3_bind_text(stmt, 6, error_msg, -1, SQLITE_TRANSIENT);
  else
    sqlite3_bind_null(stmt, 6);

  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

static gboolean
mark_published(NdPublisher *self, NdOutboxKind outbox,
               NdStoreCollection col, const gchar *row_id, GError **error)
{
  sqlite3 *h = nd_store_db_get_handle(self->db);
  g_autofree gchar *sql = g_strdup_printf(
    "UPDATE %s SET publish_state = 'published' WHERE %s = ?1",
    outbox_table(outbox, col), outbox_key(outbox, col));
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(h, sql, -1, &stmt, NULL) != SQLITE_OK)
    return nd_store_db_set_sql_error(self->db, error, "prepare mark published");
  sqlite3_bind_text(stmt, 1, row_id, -1, SQLITE_TRANSIENT);
  gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  if (!ok)
    nd_store_db_set_sql_error(self->db, error, "mark published");
  sqlite3_finalize(stmt);
  return ok;
}

static gboolean
mark_failed_permanent(NdPublisher *self, NdOutboxKind outbox,
                      NdStoreCollection col, const gchar *row_id,
                      GError **error)
{
  sqlite3 *h = nd_store_db_get_handle(self->db);
  g_autofree gchar *sql = g_strdup_printf(
    "UPDATE %s SET publish_state = 'failed_permanent' WHERE %s = ?1",
    outbox_table(outbox, col), outbox_key(outbox, col));
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(h, sql, -1, &stmt, NULL) != SQLITE_OK)
    return nd_store_db_set_sql_error(self->db, error,
                                     "prepare mark failed_permanent");
  sqlite3_bind_text(stmt, 1, row_id, -1, SQLITE_TRANSIENT);
  gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  if (!ok)
    nd_store_db_set_sql_error(self->db, error, "mark failed_permanent");
  sqlite3_finalize(stmt);
  return ok;
}

static gboolean
schedule_retry(NdPublisher *self, NdOutboxKind outbox,
               NdStoreCollection col, const gchar *row_id,
               gint64 next_ts, GError **error)
{
  sqlite3 *h = nd_store_db_get_handle(self->db);
  g_autofree gchar *sql = g_strdup_printf(
    "UPDATE %s SET publish_attempts = publish_attempts + 1, "
    "  publish_next_ts = ?1 WHERE %s = ?2",
    outbox_table(outbox, col), outbox_key(outbox, col));
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(h, sql, -1, &stmt, NULL) != SQLITE_OK)
    return nd_store_db_set_sql_error(self->db, error,
                                     "prepare schedule retry");
  sqlite3_bind_int64(stmt, 1, next_ts);
  sqlite3_bind_text (stmt, 2, row_id, -1, SQLITE_TRANSIENT);
  gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  if (!ok)
    nd_store_db_set_sql_error(self->db, error, "schedule retry");
  sqlite3_finalize(stmt);
  return ok;
}

static gboolean
save_signed_json(NdPublisher *self, NdOutboxKind outbox,
                 NdStoreCollection col, const gchar *row_id,
                 const gchar *signed_json, GError **error)
{
  sqlite3 *h = nd_store_db_get_handle(self->db);
  g_autofree gchar *sql = g_strdup_printf(
    "UPDATE %s SET signed_event_json = ?1 WHERE %s = ?2",
    outbox_table(outbox, col), outbox_key(outbox, col));
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(h, sql, -1, &stmt, NULL) != SQLITE_OK)
    return nd_store_db_set_sql_error(self->db, error,
                                     "prepare save signed json");
  sqlite3_bind_text(stmt, 1, signed_json, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, row_id, -1, SQLITE_TRANSIENT);
  gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  if (!ok)
    nd_store_db_set_sql_error(self->db, error, "save signed json");
  sqlite3_finalize(stmt);
  return ok;
}

/* ---- Unsigned-event construction ---- */

static gchar *
build_unsigned_for_calendar(NdPublisher *self, const gchar *uid, GError **error)
{
  GError *err = NULL;
  g_autoptr(NdCalendarEvent) ev = NULL;
  {
    NdCalendarStore *store = NULL;
    /* We do not need the store wrapper here — hit SQLite directly to
     * avoid coupling; but sharing the store keeps parsing quirks in one
     * place. For v1, we materialise via a fresh temporary wrapper. */
    (void)store;
  }

  sqlite3 *h = nd_store_db_get_handle(self->db);
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(h,
        "SELECT ical FROM events WHERE uid = ?1", -1, &stmt, NULL) != SQLITE_OK) {
    nd_store_db_set_sql_error(self->db, error, "load event for signing");
    return NULL;
  }
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    g_set_error(error, ND_PUBLISHER_ERROR, ND_PUBLISHER_ERROR_STORE,
                "event %s vanished from store before publish", uid);
    return NULL;
  }
  const gchar *ical = (const gchar *)sqlite3_column_text(stmt, 0);
  ev = nd_ical_parse_vevent(ical ? ical : "", &err);
  sqlite3_finalize(stmt);
  if (ev == NULL) {
    g_propagate_prefixed_error(error, err,
                               "cannot parse stored ICS for signing: ");
    return NULL;
  }
  /* Force UID so the NIP-52 `d`-tag matches the row key. */
  g_free(ev->uid);
  ev->uid = g_strdup(uid);
  /* Pubkey / created_at are stamped in by the signer; the unsigned JSON
   * omits them for those fields where the signer sets them. */
  return nd_ical_event_to_nip52_json(ev);
}

/* Build the unsigned NIP-09 kind-5 tombstone for the given addressable
 * pointer. `a` tags on parameterized-replaceable events take the form
 * `<kind>:<pubkey>:<d-tag>`; NIP-09 requires exactly one such tag per
 * addressable pointer being deleted. The signer stamps `id`, `pubkey`,
 * and `sig`; `created_at` is set to now so a slow signer does not stamp
 * a value predating the deleted event. */
static gchar *
build_unsigned_for_tombstone(int          target_kind,
                             const gchar *target_pubkey_hex,
                             const gchar *target_uid,
                             gint64       now_ts)
{
  g_autofree gchar *a_value =
    g_strdup_printf("%d:%s:%s", target_kind,
                    target_pubkey_hex ? target_pubkey_hex : "",
                    target_uid ? target_uid : "");

  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);

  json_builder_set_member_name(b, "kind");
  json_builder_add_int_value(b, 5);

  json_builder_set_member_name(b, "created_at");
  json_builder_add_int_value(b, now_ts);

  json_builder_set_member_name(b, "content");
  json_builder_add_string_value(b, "");

  json_builder_set_member_name(b, "tags");
  json_builder_begin_array(b);
  json_builder_begin_array(b);
  json_builder_add_string_value(b, "a");
  json_builder_add_string_value(b, a_value);
  json_builder_end_array(b);
  json_builder_end_array(b);

  json_builder_end_object(b);

  g_autoptr(JsonGenerator) gen = json_generator_new();
  g_autoptr(JsonNode) root = json_builder_get_root(b);
  json_generator_set_root(gen, root);
  return json_generator_to_data(gen, NULL);
}

static gchar *
build_unsigned_for_contact(NdPublisher *self, const gchar *uid, GError **error)
{
  sqlite3 *h = nd_store_db_get_handle(self->db);
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(h,
        "SELECT vcard FROM contacts WHERE uid = ?1",
        -1, &stmt, NULL) != SQLITE_OK) {
    nd_store_db_set_sql_error(self->db, error, "load contact for signing");
    return NULL;
  }
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    g_set_error(error, ND_PUBLISHER_ERROR, ND_PUBLISHER_ERROR_STORE,
                "contact %s vanished from store before publish", uid);
    return NULL;
  }
  const gchar *vcard = (const gchar *)sqlite3_column_text(stmt, 0);
  GError *err = NULL;
  g_autoptr(NdContact) contact = nd_vcard_parse(vcard ? vcard : "", &err);
  sqlite3_finalize(stmt);
  if (contact == NULL) {
    g_propagate_prefixed_error(error, err,
                               "cannot parse stored vCard for signing: ");
    return NULL;
  }
  g_free(contact->uid);
  contact->uid = g_strdup(uid);
  return nd_vcard_to_nostr_json(contact);
}

/* Extract the `id` string from a signed event JSON. Robust against the
 * signer inserting fields in any order. */
static gchar *
extract_event_id(const gchar *signed_json, GError **error)
{
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, signed_json, -1, error))
    return NULL;
  JsonNode *root = json_parser_get_root(parser);
  if (root == NULL || JSON_NODE_TYPE(root) != JSON_NODE_OBJECT) {
    g_set_error_literal(error, ND_PUBLISHER_ERROR, ND_PUBLISHER_ERROR_SIGNER,
                        "signed event is not a JSON object");
    return NULL;
  }
  JsonObject *obj = json_node_get_object(root);
  if (!json_object_has_member(obj, "id")) {
    g_set_error_literal(error, ND_PUBLISHER_ERROR, ND_PUBLISHER_ERROR_SIGNER,
                        "signed event has no id field");
    return NULL;
  }
  return g_strdup(json_object_get_string_member(obj, "id"));
}

/* ---- In-flight bookkeeping ---- */

static NdPendingRow *
pending_new_row(NdOutboxKind outbox, NdStoreCollection col, const gchar *row_id,
                const gchar *event_id, gint64 deadline)
{
  NdPendingRow *row = g_new0(NdPendingRow, 1);
  row->outbox     = outbox;
  row->collection = col;
  row->row_id     = g_strdup(row_id);
  row->event_id   = g_strdup(event_id);
  row->waiting    = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  row->acked      = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  row->failed     = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  row->deadline   = deadline;
  return row;
}

static NdRelayTransport *
resolve_transport(NdPublisher *self, const gchar *url)
{
  /* v1: transports are owned by the sync layer (or the test harness)
   * and shared with the publisher via nd_publisher_bind_transport().
   * Falling through to the factory would leak transports because we
   * have nowhere to store the transfer-full result. The follow-up bead
   * folds the sync + publisher transport caches into one. */
  return g_hash_table_lookup(self->bound, url);
}

/* Descriptor for one dispatchable outbox row. `outbox` picks which
 * table the SQL helpers touch; `col` remains meaningful only when the
 * outbox is a primary collection. Tombstones fill in target_kind /
 * target_pubkey / target_uid instead of relying on an unsigned-event
 * builder that reads back from the collection tables. */
typedef struct {
  NdOutboxKind        outbox;
  NdStoreCollection   col;
  const gchar        *row_id;
  const gchar        *signed_json_existing;
  const gchar        *targets_json;
  int                 attempts;
  /* Tombstone-only. */
  int                 target_kind;
  const gchar        *target_pubkey_hex;
  const gchar        *target_uid;
} NdDispatchCtx;

/* Attempt to publish a single pending row: sign if needed, iterate the
 * target list, send an EVENT frame per relay. Returns TRUE if the row
 * transitioned to in-flight (waiting for OKs); FALSE if it was already
 * settled during this attempt (e.g. signer denied) — either terminally
 * or scheduled for a later retry. */
static gboolean
dispatch_row(NdPublisher         *self,
             const NdDispatchCtx *ctx,
             gint64               now_ts,
             GError             **error)
{
  NdOutboxKind      outbox = ctx->outbox;
  NdStoreCollection col    = ctx->col;
  const gchar      *row_id = ctx->row_id;
  int               attempts = ctx->attempts;
  g_autofree gchar *signed_json = g_strdup(ctx->signed_json_existing);

  if (signed_json == NULL) {
    g_autofree gchar *unsigned_json = NULL;
    if (outbox == ND_OUTBOX_TOMBSTONE) {
      unsigned_json = build_unsigned_for_tombstone(ctx->target_kind,
                                                   ctx->target_pubkey_hex,
                                                   ctx->target_uid, now_ts);
    } else if (col == ND_STORE_COLLECTION_EVENTS) {
      unsigned_json = build_unsigned_for_calendar(self, row_id, error);
    } else if (col == ND_STORE_COLLECTION_CONTACTS) {
      unsigned_json = build_unsigned_for_contact(self, row_id, error);
    } else {
      g_set_error_literal(error, ND_PUBLISHER_ERROR, ND_PUBLISHER_ERROR_STORE,
                          "kind not yet publishable");
      return FALSE;
    }
    if (unsigned_json == NULL)
      return FALSE;

    GError *sign_err = NULL;
    signed_json = nd_signer_sign_event_json(self->signer, unsigned_json,
                                            NULL, &sign_err);
    if (signed_json == NULL) {
      if (g_error_matches(sign_err, ND_SIGNER_ERROR, ND_SIGNER_ERROR_DENIED) ||
          g_error_matches(sign_err, ND_SIGNER_ERROR, ND_SIGNER_ERROR_MALFORMED)) {
        log_attempt(self, outbox, col, row_id, NULL, 0,
                    sign_err ? sign_err->message : "signer denied");
        if (!mark_failed_permanent(self, outbox, col, row_id, error)) {
          g_clear_error(&sign_err);
          return FALSE;
        }
        /* Primary outbox rows fire an operator notification on permanent
         * failure so the DAV write does not silently vanish; tombstones
         * are best-effort cleanup and would drown the operator in noise
         * for keys the signer no longer trusts, so we log-only there. */
        if (outbox == ND_OUTBOX_COLLECTION && self->notify_cb != NULL)
          self->notify_cb(self, ND_PUBLISHER_NOTIFICATION_FAILED_PERMANENT,
                          col, row_id,
                          sign_err ? sign_err->message : "signer denied",
                          self->notify_data);
        g_clear_error(&sign_err);
        return FALSE;
      }
      /* Transient signer failure — retry later. */
      gint64 next = now_ts + backoff_delay(attempts);
      log_attempt(self, outbox, col, row_id, NULL, 0,
                  sign_err ? sign_err->message : "signer transient");
      g_clear_error(&sign_err);
      return schedule_retry(self, outbox, col, row_id, next, error) ? FALSE : FALSE;
    }
    if (!save_signed_json(self, outbox, col, row_id, signed_json, error))
      return FALSE;
  }

  g_autofree gchar *event_id = extract_event_id(signed_json, error);
  if (event_id == NULL)
    return FALSE;

  GStrv targets = targets_from_json(ctx->targets_json);
  if (targets == NULL || targets[0] == NULL) {
    /* No target set — fall back to configured home_relays. Without any
     * relays configured, the row cannot proceed; mark it permanent so
     * the operator gets notified rather than looping silently. */
    g_strfreev(targets);
    targets = self->home_relays != NULL ? g_strdupv(self->home_relays) : NULL;
    if (targets == NULL || targets[0] == NULL) {
      g_strfreev(targets);
      log_attempt(self, outbox, col, row_id, NULL, 0, "no relays configured");
      if (!mark_failed_permanent(self, outbox, col, row_id, error))
        return FALSE;
      if (outbox == ND_OUTBOX_COLLECTION && self->notify_cb != NULL)
        self->notify_cb(self, ND_PUBLISHER_NOTIFICATION_FAILED_PERMANENT,
                        col, row_id, "no relays configured",
                        self->notify_data);
      return FALSE;
    }
  }

  NdPendingRow *pending = pending_new_row(outbox, col, row_id, event_id,
                                          now_ts + ND_PUBLISH_OK_WAIT_SEC);
  gboolean any_sent = FALSE;
  for (guint i = 0; targets[i] != NULL; i++) {
    const gchar *url = targets[i];
    NdRelayTransport *t = resolve_transport(self, url);
    if (t == NULL) {
      log_attempt(self, outbox, col, row_id, url, 0, "no transport");
      g_hash_table_add(pending->failed, g_strdup(url));
      continue;
    }
    g_autofree gchar *frame =
      g_strdup_printf("[\"EVENT\",%s]", signed_json);
    GError *send_err = NULL;
    if (!nd_relay_transport_send_frame(t, frame, &send_err)) {
      log_attempt(self, outbox, col, row_id, url, 0,
                  send_err ? send_err->message : "send failed");
      g_clear_error(&send_err);
      g_hash_table_add(pending->failed, g_strdup(url));
      continue;
    }
    g_hash_table_add(pending->waiting, g_strdup(url));
    any_sent = TRUE;
  }
  g_strfreev(targets);

  if (!any_sent) {
    /* Every target unreachable — schedule a retry. */
    pending_row_free(pending);
    gint64 next = now_ts + backoff_delay(attempts);
    return schedule_retry(self, outbox, col, row_id, next, error) ? FALSE : FALSE;
  }

  g_hash_table_replace(self->in_flight,
                       g_strdup(pending->event_id), pending);
  return TRUE;
}

/* Finalize a row after every relay has answered. Returns FALSE if a
 * DB update failed (error set).
 *
 * NB: @row is freed as a side-effect of removing it from @self->in_flight,
 * so all fields must be captured before that removal happens. */
static gboolean
finalize_row(NdPublisher *self, NdPendingRow *row, gint64 now_ts,
             int prior_attempts, GError **error)
{
  gsize acks   = g_hash_table_size(row->acked);
  gsize fails  = g_hash_table_size(row->failed);

  gboolean quorum_all = self->quorum.all;
  guint    quorum_n   = self->quorum.count;

  gboolean met = FALSE;
  if (quorum_all)
    met = (fails == 0 && acks > 0);
  else
    met = (acks >= quorum_n);

  NdOutboxKind      outbox     = row->outbox;
  NdStoreCollection collection = row->collection;
  g_autofree gchar *row_id = g_strdup(row->row_id);

  gboolean removed_from_flight =
    g_hash_table_remove(self->in_flight, row->event_id);
  (void)removed_from_flight;
  /* @row is now dangling. */

  if (met)
    return mark_published(self, outbox, collection, row_id, error);

  /* Transient failure — schedule a retry. Total > 0 because we only
   * finalise once every target has answered. */
  gint64 next = now_ts + backoff_delay(prior_attempts);
  return schedule_retry(self, outbox, collection, row_id, next, error);
}

/* ---- Public API ---- */

NdPublisher *
nd_publisher_new(NdStoreDb              *db,
                 NdSigner               *signer,
                 NdRelayTransportFactory factory,
                 gpointer                factory_data)
{
  g_return_val_if_fail(db != NULL, NULL);
  g_return_val_if_fail(signer != NULL, NULL);

  NdPublisher *self = g_new0(NdPublisher, 1);
  self->db            = nd_store_db_ref(db);
  self->signer        = nd_signer_ref(signer);
  self->factory       = factory;
  self->factory_data  = factory_data;
  self->quorum        = ND_PUBLISH_QUORUM_DEFAULT;
  self->bound     = g_hash_table_new_full(g_str_hash, g_str_equal,
                                          g_free, NULL);
  self->in_flight = g_hash_table_new_full(g_str_hash, g_str_equal,
                                          g_free, pending_row_free);
  return self;
}

void
nd_publisher_free(NdPublisher *self)
{
  if (self == NULL) return;
  g_clear_pointer(&self->bound, g_hash_table_destroy);
  g_clear_pointer(&self->in_flight, g_hash_table_destroy);
  g_clear_pointer(&self->home_relays, g_strfreev);
  g_clear_pointer(&self->account_pubkey, g_free);
  g_clear_pointer(&self->signer, nd_signer_unref);
  g_clear_pointer(&self->db, nd_store_db_unref);
  g_free(self);
}

void
nd_publisher_configure(NdPublisher      *self,
                       const gchar      *account_pubkey,
                       const GStrv       home_relays,
                       NdPublishQuorum   quorum)
{
  g_return_if_fail(self != NULL);
  g_free(self->account_pubkey);
  self->account_pubkey = account_pubkey ? g_strdup(account_pubkey) : NULL;
  g_strfreev(self->home_relays);
  self->home_relays = home_relays ? g_strdupv(home_relays) : NULL;

  /* Clamp numeric quorum to the target set size so a misconfiguration
   * ("quorum = 5, home_relays = 2") does not silently retry forever.
   * `all` never needs clamping — it always waits for every target. */
  if (!quorum.all) {
    guint n = self->home_relays ? g_strv_length(self->home_relays) : 0;
    if (n == 0)
      quorum.count = 1;
    else if (quorum.count > n) {
      g_warning("nostr-dav: publish quorum %u > relay count %u; clamping",
                quorum.count, n);
      quorum.count = n;
    }
    if (quorum.count == 0)
      quorum.count = 1;
  }
  self->quorum = quorum;
}

void
nd_publisher_set_notify_callback(NdPublisher              *self,
                                 NdPublisherNotifyCallback cb,
                                 gpointer                  user_data)
{
  g_return_if_fail(self != NULL);
  self->notify_cb   = cb;
  self->notify_data = user_data;
}

gboolean
nd_publisher_stage_calendar_put(NdPublisher *self, const gchar *uid,
                                GError **error)
{
  g_return_val_if_fail(self != NULL, FALSE);
  g_return_val_if_fail(uid != NULL, FALSE);
  return stage_row(self, ND_STORE_COLLECTION_EVENTS, uid,
                   g_get_real_time() / G_USEC_PER_SEC, error);
}

gboolean
nd_publisher_stage_contact_put(NdPublisher *self, const gchar *uid,
                               GError **error)
{
  g_return_val_if_fail(self != NULL, FALSE);
  g_return_val_if_fail(uid != NULL, FALSE);
  return stage_row(self, ND_STORE_COLLECTION_CONTACTS, uid,
                   g_get_real_time() / G_USEC_PER_SEC, error);
}

gboolean
nd_publisher_stage_tombstone(NdPublisher  *self,
                             int           target_kind,
                             const gchar  *target_pubkey_hex,
                             const gchar  *target_uid,
                             GError      **error)
{
  g_return_val_if_fail(self != NULL, FALSE);
  g_return_val_if_fail(target_uid != NULL && *target_uid, FALSE);

  /* Fall back to the configured account when the caller does not know
   * the row's pubkey (e.g. a file DELETE where the store never captured
   * the author). Without either, we cannot build a well-formed `a`
   * tag — refuse rather than publishing an event that would be silently
   * dropped by the relay. */
  const gchar *pubkey_hex = target_pubkey_hex;
  if (pubkey_hex == NULL || *pubkey_hex == '\0')
    pubkey_hex = self->account_pubkey;
  if (pubkey_hex == NULL || *pubkey_hex == '\0') {
    g_set_error_literal(error, ND_PUBLISHER_ERROR, ND_PUBLISHER_ERROR_STORE,
                        "stage_tombstone: no target pubkey available");
    return FALSE;
  }

  return stage_tombstone_row(self, target_kind, pubkey_hex, target_uid,
                             g_get_real_time() / G_USEC_PER_SEC, error);
}

void
nd_publisher_bind_transport(NdPublisher      *self,
                            const gchar      *relay_url,
                            NdRelayTransport *transport)
{
  g_return_if_fail(self != NULL);
  g_return_if_fail(relay_url != NULL);
  if (transport == NULL)
    g_hash_table_remove(self->bound, relay_url);
  else
    g_hash_table_insert(self->bound, g_strdup(relay_url), transport);
}

typedef struct {
  NdOutboxKind      outbox;
  NdStoreCollection col;
  gchar   *row_id;
  gchar   *signed_json;
  gchar   *targets_json;
  int      attempts;
  /* Tombstone-only. */
  int      target_kind;
  gchar   *target_pubkey_hex;
  gchar   *target_uid;
} NdOutboxRow;

static void
outbox_row_clear(NdOutboxRow *row)
{
  g_clear_pointer(&row->row_id, g_free);
  g_clear_pointer(&row->signed_json, g_free);
  g_clear_pointer(&row->targets_json, g_free);
  g_clear_pointer(&row->target_pubkey_hex, g_free);
  g_clear_pointer(&row->target_uid, g_free);
}

/* Free-func for the GPtrArray of NdOutboxRow*: frees the inner strings
 * and the struct itself. */
static void
outbox_row_free(gpointer data)
{
  NdOutboxRow *row = data;
  if (row == NULL) return;
  outbox_row_clear(row);
  g_free(row);
}

/* Query outbox rows for a collection whose publish_next_ts <= now. */
static GPtrArray *
load_pending(NdPublisher *self, NdStoreCollection col, gint64 now_ts)
{
  sqlite3 *h = nd_store_db_get_handle(self->db);
  const gchar *table = collection_table(col);
  const gchar *key   = collection_key(col);
  g_autofree gchar *sql = g_strdup_printf(
    "SELECT %s, signed_event_json, publish_targets, publish_attempts "
    "FROM %s "
    "WHERE publish_state = 'pending' "
    "  AND (publish_next_ts IS NULL OR publish_next_ts <= ?1) "
    "ORDER BY publish_next_ts ASC", key, table);

  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(h, sql, -1, &stmt, NULL) != SQLITE_OK)
    return NULL;
  sqlite3_bind_int64(stmt, 1, now_ts);

  GPtrArray *rows = g_ptr_array_new_with_free_func(outbox_row_free);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    NdOutboxRow *row = g_new0(NdOutboxRow, 1);
    row->outbox = ND_OUTBOX_COLLECTION;
    row->col    = col;
    row->row_id = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
    const unsigned char *sj = sqlite3_column_text(stmt, 1);
    row->signed_json = sj ? g_strdup((const gchar *)sj) : NULL;
    const unsigned char *tj = sqlite3_column_text(stmt, 2);
    row->targets_json = tj ? g_strdup((const gchar *)tj) : NULL;
    row->attempts = sqlite3_column_int(stmt, 3);
    g_ptr_array_add(rows, row);
  }
  sqlite3_finalize(stmt);
  return rows;
}

/* Sibling loader for the tombstones table. The tombstone-specific
 * target_kind / target_pubkey / target_uid ride along so dispatch_row
 * can build the unsigned NIP-09 event without another SQL round-trip. */
static GPtrArray *
load_pending_tombstones(NdPublisher *self, gint64 now_ts)
{
  sqlite3 *h = nd_store_db_get_handle(self->db);
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(h,
        "SELECT id, signed_event_json, publish_targets, publish_attempts,"
        "       target_kind, target_pubkey, target_uid "
        "FROM tombstones "
        "WHERE publish_state = 'pending' "
        "  AND (publish_next_ts IS NULL OR publish_next_ts <= ?1) "
        "ORDER BY publish_next_ts ASC",
        -1, &stmt, NULL) != SQLITE_OK)
    return NULL;
  sqlite3_bind_int64(stmt, 1, now_ts);

  GPtrArray *rows = g_ptr_array_new_with_free_func(outbox_row_free);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    NdOutboxRow *row = g_new0(NdOutboxRow, 1);
    row->outbox = ND_OUTBOX_TOMBSTONE;
    row->col    = ND_STORE_COLLECTION_EVENTS; /* unused for tombstones */
    /* Stringify the AUTOINCREMENT id so it fits the row_id contract.
     * Cast sqlite3_int64 -> gint64 because on some 64-bit platforms
     * (e.g. aarch64 Linux) sqlite3_int64 is `long long int` while gint64
     * is `long int`, and G_GINT64_FORMAT ("li") triggers -Werror=format
     * without the cast. Both are guaranteed 64-bit signed. */
    row->row_id = g_strdup_printf("%" G_GINT64_FORMAT,
                                  (gint64)sqlite3_column_int64(stmt, 0));
    const unsigned char *sj = sqlite3_column_text(stmt, 1);
    row->signed_json = sj ? g_strdup((const gchar *)sj) : NULL;
    const unsigned char *tj = sqlite3_column_text(stmt, 2);
    row->targets_json = tj ? g_strdup((const gchar *)tj) : NULL;
    row->attempts = sqlite3_column_int(stmt, 3);
    row->target_kind = sqlite3_column_int(stmt, 4);
    const unsigned char *pk = sqlite3_column_text(stmt, 5);
    row->target_pubkey_hex = pk ? g_strdup((const gchar *)pk) : NULL;
    const unsigned char *tu = sqlite3_column_text(stmt, 6);
    row->target_uid = tu ? g_strdup((const gchar *)tu) : NULL;
    g_ptr_array_add(rows, row);
  }
  sqlite3_finalize(stmt);
  return rows;
}

/* Sweep every in-flight row whose OK-wait deadline has passed and turn
 * its still-waiting relays into transient failures. Prevents a silent
 * relay from pinning the row in memory forever. Called at the top of
 * every tick so the outbox eventually settles even in the absence of
 * fresh incoming OK/fail frames. */
static void
sweep_expired(NdPublisher *self, gint64 now_ts)
{
  GHashTableIter it;
  gpointer key = NULL, value = NULL;
  GList *expired = NULL;

  g_hash_table_iter_init(&it, self->in_flight);
  while (g_hash_table_iter_next(&it, &key, &value)) {
    NdPendingRow *row = value;
    if (now_ts >= row->deadline && g_hash_table_size(row->waiting) > 0)
      expired = g_list_prepend(expired, row);
  }

  for (GList *l = expired; l != NULL; l = l->next) {
    NdPendingRow *row = l->data;
    /* Move every still-waiting relay into `failed` and finalize as a
     * transient outcome (row stays pending, backoff scheduled). */
    g_hash_table_iter_init(&it, row->waiting);
    while (g_hash_table_iter_next(&it, &key, NULL))
      g_hash_table_add(row->failed, g_strdup(key));
    g_hash_table_remove_all(row->waiting);

    sqlite3 *h = nd_store_db_get_handle(self->db);
    g_autofree gchar *sql = g_strdup_printf(
      "SELECT publish_attempts FROM %s WHERE %s = ?1",
      outbox_table(row->outbox, row->collection),
      outbox_key(row->outbox, row->collection));
    int attempts = 0;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, row->row_id, -1, SQLITE_TRANSIENT);
      if (sqlite3_step(stmt) == SQLITE_ROW)
        attempts = sqlite3_column_int(stmt, 0);
      sqlite3_finalize(stmt);
    }
    GError *err = NULL;
    if (!finalize_row(self, row, now_ts, attempts, &err)) {
      g_warning("nostr-dav publisher: sweep finalize: %s",
                err ? err->message : "unknown");
      g_clear_error(&err);
    }
  }
  g_list_free(expired);
}

gboolean
nd_publisher_tick(NdPublisher *self, gint64 now_ts)
{
  g_return_val_if_fail(self != NULL, FALSE);

  sweep_expired(self, now_ts);

  static const NdStoreCollection cols[] = {
    ND_STORE_COLLECTION_EVENTS,
    ND_STORE_COLLECTION_CONTACTS,
  };
  gboolean more = FALSE;

  /* Drain the primary outboxes, then the tombstone outbox. Ordering is
   * incidental — the two share no rows — but running collections first
   * keeps the DAV write path visible in the log ahead of any deletion
   * artefacts staged in the same tick. */
  GPtrArray *batches[G_N_ELEMENTS(cols) + 1] = {0};
  gsize n_batches = 0;
  for (gsize c = 0; c < G_N_ELEMENTS(cols); c++)
    batches[n_batches++] = load_pending(self, cols[c], now_ts);
  batches[n_batches++] = load_pending_tombstones(self, now_ts);

  for (gsize b = 0; b < n_batches; b++) {
    GPtrArray *rows = batches[b];
    if (rows == NULL) continue;
    for (guint i = 0; i < rows->len; i++) {
      NdOutboxRow *row = g_ptr_array_index(rows, i);
      /* Skip rows already in-flight from a previous tick (they'll settle
       * via record_ok). Match on (outbox, collection, row_id) so a
       * tombstone and a collection row that happen to share an id string
       * never alias each other. */
      gboolean already_flying = FALSE;
      GHashTableIter it;
      gpointer v = NULL;
      g_hash_table_iter_init(&it, self->in_flight);
      while (g_hash_table_iter_next(&it, NULL, &v)) {
        NdPendingRow *pr = v;
        if (pr->outbox == row->outbox &&
            pr->collection == row->col &&
            g_str_equal(pr->row_id, row->row_id)) {
          already_flying = TRUE;
          more = TRUE;
          break;
        }
      }
      if (already_flying) continue;

      NdDispatchCtx ctx = {
        .outbox               = row->outbox,
        .col                  = row->col,
        .row_id               = row->row_id,
        .signed_json_existing = row->signed_json,
        .targets_json         = row->targets_json,
        .attempts             = row->attempts,
        .target_kind          = row->target_kind,
        .target_pubkey_hex    = row->target_pubkey_hex,
        .target_uid           = row->target_uid,
      };
      GError *err = NULL;
      if (!dispatch_row(self, &ctx, now_ts, &err)) {
        if (err != NULL) {
          g_warning("nostr-dav publisher: %s", err->message);
          g_clear_error(&err);
        }
        /* dispatch_row already marked terminal or scheduled retry. */
      } else {
        more = TRUE;
      }
    }
    /* outbox_row_clear() runs via the free-func registered on load. */
    g_ptr_array_free(rows, TRUE);
  }
  return more || g_hash_table_size(self->in_flight) > 0;
}

void
nd_publisher_record_ok(NdPublisher *self,
                       const gchar *relay_url,
                       const gchar *event_id,
                       gboolean     ok,
                       const gchar *reason)
{
  g_return_if_fail(self != NULL);
  g_return_if_fail(event_id != NULL);
  g_return_if_fail(relay_url != NULL);

  NdPendingRow *row = g_hash_table_lookup(self->in_flight, event_id);
  if (row == NULL)
    return;

  NdOkClass cls = classify_ok(ok, reason);
  log_attempt(self, row->outbox, row->collection, row->row_id, relay_url,
              ok ? 200 : 400, reason);

  g_hash_table_remove(row->waiting, relay_url);

  if (cls == ND_OK_PERMANENT_FAIL) {
    GError *err = NULL;
    if (!mark_failed_permanent(self, row->outbox, row->collection,
                               row->row_id, &err)) {
      g_warning("nostr-dav publisher: %s",
                err ? err->message : "mark permanent failed");
      g_clear_error(&err);
    }
    if (row->outbox == ND_OUTBOX_COLLECTION &&
        !row->notified && self->notify_cb != NULL) {
      row->notified = TRUE;
      self->notify_cb(self, ND_PUBLISHER_NOTIFICATION_FAILED_PERMANENT,
                      row->collection, row->row_id,
                      reason ? reason : "relay rejected event",
                      self->notify_data);
    }
    g_hash_table_remove(self->in_flight, event_id);
    return;
  }

  if (cls == ND_OK_ACCEPT)
    g_hash_table_add(row->acked, g_strdup(relay_url));
  else
    g_hash_table_add(row->failed, g_strdup(relay_url));

  if (g_hash_table_size(row->waiting) == 0) {
    /* Every target has answered. Re-load the attempts count from the
     * DB so backoff scaling stays accurate under out-of-order events. */
    sqlite3 *h = nd_store_db_get_handle(self->db);
    g_autofree gchar *sql = g_strdup_printf(
      "SELECT publish_attempts FROM %s WHERE %s = ?1",
      outbox_table(row->outbox, row->collection),
      outbox_key(row->outbox, row->collection));
    int attempts = 0;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, row->row_id, -1, SQLITE_TRANSIENT);
      if (sqlite3_step(stmt) == SQLITE_ROW)
        attempts = sqlite3_column_int(stmt, 0);
      sqlite3_finalize(stmt);
    }
    GError *err = NULL;
    if (!finalize_row(self, row, g_get_real_time() / G_USEC_PER_SEC,
                      attempts, &err)) {
      g_warning("nostr-dav publisher: %s",
                err ? err->message : "finalize failed");
      g_clear_error(&err);
    }
  }
}
