/* nd-publisher.c - Outbox worker for nostr-dav DAV writes
 *
 * SPDX-License-Identifier: MIT
 *
 * See nd-publisher.h for the design. Since bead nostrc-tmsc the split is:
 *
 *   * nostr-dav (this file) owns "what to publish and where state lives":
 *     the SQLite outbox columns on events/contacts/files, the kind-5
 *     `tombstones` table, the `publish_log`, the DAV -> unsigned-event
 *     builders, signing-before-persisting (so a retry never re-prompts
 *     the signer), retry scheduling, and the failed_permanent
 *     notification.
 *   * libnostr-publish's NostrPublisher owns "how to publish": EVENT
 *     fan-out to the target set, per-relay OK classification
 *     (duplicate: = accept; invalid:/blocked:/banned:/restricted:/
 *     auth-required: = permanent; everything else transient), the
 *     all-ACK / numeric-quorum verdict, and the 120 s OK-wait deadline.
 *
 * Row lifecycle is unchanged: rows stay `pending` until every relay in
 * their target set ACKs (`published`), a relay rejects permanently or the
 * signer denies (`failed_permanent` + one notification for collection
 * rows), or the attempt settles short of quorum (retry after
 * `min(60 * 2^attempts, 3600)` s).
 */

#include "nd-publisher.h"

#include "nd-ical.h"
#include "nd-vcard.h"
#include "nd-calendar-store.h"
#include "nd-contact-store.h"

#include <nostr-publish/nostr-publisher.h>

#include <json-glib/json-glib.h>
#include <sqlite3.h>

#include <string.h>

G_DEFINE_QUARK(nd-publisher-error-quark, nd_publisher_error)

/* Discriminator: primary outbox rows (events/contacts/files) live in
 * their own tables; kind-5 tombstones (nostrc-ls2c) live in the
 * `tombstones` table and are keyed by an AUTOINCREMENT `id`. Both
 * pathways share the same in-flight bookkeeping and settle through
 * on_publish_done(); the enum lets the publisher pick the right SQL. */
typedef enum {
  ND_OUTBOX_COLLECTION = 0,  /* events / contacts / files */
  ND_OUTBOX_TOMBSTONE  = 1
} NdOutboxKind;

/* One outbox row currently being published by the engine. Owned by the
 * engine request (released through in_flight_destroy); indexed by
 * `key` in NdPublisher.in_flight so tick() does not re-dispatch it. */
typedef struct {
  NdPublisher       *publisher;
  NdOutboxKind       outbox;
  NdStoreCollection  collection;  /* meaningful only for ND_OUTBOX_COLLECTION */
  gchar             *row_id;      /* uid for collections, decimal id for tombstones */
  gchar             *key;         /* "<outbox>:<collection>:<row_id>" */
} NdInFlight;

struct _NdPublisher {
  NdStoreDb      *db;
  NdSigner       *signer;
  NostrPublisher *engine;

  gchar             *account_pubkey;
  GStrv              home_relays;   /* configured home relays */
  NdPublishQuorum    quorum;        /* as configured, before clamping */
  NdUpstreamMode     upstream_mode;
  gchar             *session_relay_url;  /* NULL: no session relay */
  GStrv              targets;       /* upstream-filtered default target set */
  gboolean           held;          /* session_relay_only, no session relay */
  NostrPublishPolicy policy;        /* mapped from NdPublishQuorum */

  GHashTable    *in_flight;       /* key(borrowed) -> NdInFlight*(borrowed) */

  NdPublisherNotifyCallback notify_cb;
  gpointer                  notify_data;
};

/* ---- Small helpers ---- */

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
  g_autofree gchar *targets_json = targets_to_json(self->targets);
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
  g_autofree gchar *targets_json = targets_to_json(self->targets);
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

  sqlite3 *h = nd_store_db_get_handle(self->db);
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(h,
        "SELECT ical, created_at FROM events WHERE uid = ?1",
        -1, &stmt, NULL) != SQLITE_OK) {
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
  gint64 row_created_at = sqlite3_column_int64(stmt, 1);
  sqlite3_finalize(stmt);
  if (ev == NULL) {
    g_propagate_prefixed_error(error, err,
                               "cannot parse stored ICS for signing: ");
    return NULL;
  }
  /* Force UID so the NIP-52 `d`-tag matches the row key. */
  g_free(ev->uid);
  ev->uid = g_strdup(uid);
  /* Sign with the created_at stamped at DAV PUT (nostrc-ir7c); the signer
   * keeps a non-zero created_at and fills in pubkey/id/sig. */
  ev->created_at = row_created_at;
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
        "SELECT vcard, created_at FROM contacts WHERE uid = ?1",
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
  gint64 row_created_at = sqlite3_column_int64(stmt, 1);
  sqlite3_finalize(stmt);
  if (contact == NULL) {
    g_propagate_prefixed_error(error, err,
                               "cannot parse stored vCard for signing: ");
    return NULL;
  }
  g_free(contact->uid);
  contact->uid = g_strdup(uid);
  contact->created_at = row_created_at; /* nostrc-ir7c */
  return nd_vcard_to_nostr_json(contact);
}

/* ---- In-flight bookkeeping ---- */

static gchar *
in_flight_key(NdOutboxKind outbox, NdStoreCollection col, const gchar *row_id)
{
  return g_strdup_printf("%d:%d:%s", (int)outbox, (int)col, row_id);
}

static void
in_flight_destroy(gpointer data)
{
  NdInFlight *ifl = data;
  if (ifl->publisher->in_flight != NULL)
    g_hash_table_remove(ifl->publisher->in_flight, ifl->key);
  g_free(ifl->row_id);
  g_free(ifl->key);
  g_free(ifl);
}

static int
read_attempts(NdPublisher *self, NdOutboxKind outbox, NdStoreCollection col,
              const gchar *row_id)
{
  sqlite3 *h = nd_store_db_get_handle(self->db);
  g_autofree gchar *sql = g_strdup_printf(
    "SELECT publish_attempts FROM %s WHERE %s = ?1",
    outbox_table(outbox, col), outbox_key(outbox, col));
  int attempts = 0;
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(h, sql, -1, &stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, row_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW)
      attempts = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }
  return attempts;
}

static void
notify_failed_permanent(NdPublisher       *self,
                        NdOutboxKind       outbox,
                        NdStoreCollection  col,
                        const gchar       *row_id,
                        const gchar       *reason)
{
  /* Primary outbox rows fire an operator notification on permanent
   * failure so the DAV write does not silently vanish; tombstones are
   * best-effort cleanup and would drown the operator in noise for keys
   * the signer no longer trusts, so we log-only there. */
  if (outbox == ND_OUTBOX_COLLECTION && self->notify_cb != NULL)
    self->notify_cb(self, ND_PUBLISHER_NOTIFICATION_FAILED_PERMANENT,
                    col, row_id, reason, self->notify_data);
}

/* Engine per-relay progress -> publish_log. Timeouts were never logged
 * per relay; the retry itself is visible through publish_attempts. */
static void
on_publish_relay(NostrPublisher          *engine,
                 const gchar             *event_id,
                 const gchar             *relay_url,
                 NostrPublishRelayStatus  status,
                 const gchar             *reason,
                 gpointer                 user_data)
{
  (void)engine;
  (void)event_id;
  NdInFlight *ifl = user_data;
  int http_status;
  switch (status) {
  case NOSTR_PUBLISH_RELAY_ACCEPTED:
    http_status = 200;
    break;
  case NOSTR_PUBLISH_RELAY_REJECTED_TRANSIENT:
  case NOSTR_PUBLISH_RELAY_REJECTED_PERMANENT:
    http_status = 400;
    break;
  case NOSTR_PUBLISH_RELAY_UNREACHABLE:
    http_status = 0;
    break;
  case NOSTR_PUBLISH_RELAY_TIMED_OUT:
  case NOSTR_PUBLISH_RELAY_PENDING:
  default:
    return;
  }
  log_attempt(ifl->publisher, ifl->outbox, ifl->collection, ifl->row_id,
              relay_url, http_status, reason);
}

/* Engine verdict -> outbox row state. */
static void
on_publish_done(NostrPublisher           *engine,
                const NostrPublishResult *result,
                gpointer                  user_data)
{
  (void)engine;
  NdInFlight *ifl = user_data;
  NdPublisher *self = ifl->publisher;
  GError *err = NULL;
  gboolean ok = TRUE;

  switch (nostr_publish_result_get_verdict(result)) {
  case NOSTR_PUBLISH_VERDICT_PUBLISHED:
    ok = mark_published(self, ifl->outbox, ifl->collection, ifl->row_id, &err);
    break;

  case NOSTR_PUBLISH_VERDICT_FAILED_PERMANENT: {
    const gchar *reason = nostr_publish_result_get_reason(result);
    ok = mark_failed_permanent(self, ifl->outbox, ifl->collection,
                               ifl->row_id, &err);
    notify_failed_permanent(self, ifl->outbox, ifl->collection, ifl->row_id,
                            reason ? reason : "relay rejected event");
    break;
  }

  case NOSTR_PUBLISH_VERDICT_RETRY:
  default: {
    /* Re-read attempts from the DB so backoff scaling stays accurate
     * under out-of-order events. */
    int attempts = read_attempts(self, ifl->outbox, ifl->collection,
                                 ifl->row_id);
    gint64 next = nostr_publish_result_get_completed_at(result) +
                  nostr_publish_policy_backoff_delay(&self->policy,
                                                     (guint)attempts);
    ok = schedule_retry(self, ifl->outbox, ifl->collection, ifl->row_id,
                        next, &err);
    break;
  }
  }

  if (!ok) {
    g_warning("nostr-dav publisher: %s", err ? err->message : "finalize failed");
    g_clear_error(&err);
  }
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

/* Attempt to publish a single pending row: sign if needed (persisting
 * the signed JSON first so retries never re-prompt the signer), resolve
 * the target set, and hand the row to the engine. Returns TRUE if the
 * row is now in flight (waiting for OKs); FALSE if it was settled during
 * this attempt (signer denied, no relays, or no relay reachable) —
 * either terminally or scheduled for a later retry — or on error. */
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
      const gchar *msg = sign_err ? sign_err->message : NULL;
      if (nostr_publish_signer_error_is_permanent(sign_err)) {
        log_attempt(self, outbox, col, row_id, NULL, 0,
                    msg ? msg : "signer denied");
        if (mark_failed_permanent(self, outbox, col, row_id, error))
          notify_failed_permanent(self, outbox, col, row_id,
                                  msg ? msg : "signer denied");
        g_clear_error(&sign_err);
        return FALSE;
      }
      /* Transient signer failure — retry later. */
      gint64 next = now_ts +
                    nostr_publish_policy_backoff_delay(&self->policy,
                                                       (guint)attempts);
      log_attempt(self, outbox, col, row_id, NULL, 0,
                  msg ? msg : "signer transient");
      g_clear_error(&sign_err);
      schedule_retry(self, outbox, col, row_id, next, error);
      return FALSE;
    }
    if (!save_signed_json(self, outbox, col, row_id, signed_json, error))
      return FALSE;
  }

  g_auto(GStrv) targets = targets_from_json(ctx->targets_json);
  if (self->upstream_mode == ND_UPSTREAM_MODE_SESSION_RELAY_ONLY) {
    /* nostrc-862u: a row staged under a wider mode (or before the mode
     * was changed) must not leak to home relays now. */
    g_strfreev(targets);
    targets = NULL;
  }
  if (targets == NULL || targets[0] == NULL) {
    /* No target set — fall back to the current upstream-filtered set.
     * Without any relays configured, the row cannot proceed; mark it
     * permanent so the operator gets notified rather than looping
     * silently. (A held session_relay_only publisher never gets here:
     * nd_publisher_tick() does not dispatch while held.) */
    g_strfreev(targets);
    targets = self->targets != NULL ? g_strdupv(self->targets) : NULL;
    if (targets == NULL || targets[0] == NULL) {
      log_attempt(self, outbox, col, row_id, NULL, 0, "no relays configured");
      if (!mark_failed_permanent(self, outbox, col, row_id, error))
        return FALSE;
      notify_failed_permanent(self, outbox, col, row_id,
                              "no relays configured");
      return FALSE;
    }
  }

  NdInFlight *ifl = g_new0(NdInFlight, 1);
  ifl->publisher  = self;
  ifl->outbox     = outbox;
  ifl->collection = col;
  ifl->row_id     = g_strdup(row_id);
  ifl->key        = in_flight_key(outbox, col, row_id);
  g_autofree gchar *key = g_strdup(ifl->key);
  g_hash_table_insert(self->in_flight, ifl->key, ifl);

  /* When no relay is reachable the engine settles (RETRY) before
   * returning; on_publish_done has then already scheduled the retry and
   * released @ifl. */
  if (!nostr_publisher_publish_signed(self->engine, signed_json,
                                      (const gchar *const *)targets,
                                      &self->policy, now_ts,
                                      on_publish_relay, on_publish_done,
                                      ifl, in_flight_destroy, error))
    return FALSE;
  return g_hash_table_contains(self->in_flight, key);
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

  /* @factory is accepted for API compatibility but deliberately not
   * handed to the engine: transports are owned by the sync layer (or the
   * test harness) and shared through nd_publisher_bind_transport(), so an
   * unbound URL stays "no transport" exactly as before. */
  (void)factory;
  (void)factory_data;

  NdPublisher *self = g_new0(NdPublisher, 1);
  self->db        = nd_store_db_ref(db);
  self->signer    = nd_signer_ref(signer);
  self->engine    = nostr_publisher_new(signer);
  /* Explicit: the enum's zero value is SESSION_RELAY_ONLY. */
  self->upstream_mode = ND_UPSTREAM_MODE_DEFAULT;
  nd_publisher_configure(self, NULL, NULL, ND_PUBLISH_QUORUM_DEFAULT);
  self->in_flight = g_hash_table_new(g_str_hash, g_str_equal);
  return self;
}

void
nd_publisher_free(NdPublisher *self)
{
  if (self == NULL) return;
  /* Freeing the engine releases every in-flight NdInFlight through
   * in_flight_destroy(), which unindexes it from self->in_flight. */
  g_clear_pointer(&self->engine, nostr_publisher_free);
  g_clear_pointer(&self->in_flight, g_hash_table_destroy);
  g_clear_pointer(&self->home_relays, g_strfreev);
  g_clear_pointer(&self->targets, g_strfreev);
  g_clear_pointer(&self->session_relay_url, g_free);
  g_clear_pointer(&self->account_pubkey, g_free);
  g_clear_pointer(&self->signer, nd_signer_unref);
  g_clear_pointer(&self->db, nd_store_db_unref);
  g_free(self);
}

/* Recompute the default target set from home_relays + upstream mode, then
 * the engine policy (quorum is clamped to the resulting set size). */
static void
recompute_targets(NdPublisher *self)
{
  NostrPublishPolicy route;
  nostr_publish_policy_init(&route);
  route.upstream = nd_upstream_mode_to_publish(self->upstream_mode);
  GError *err = NULL;
  g_strfreev(self->targets);
  self->targets = nostr_publish_policy_select_targets(
    &route, (const gchar *const *)self->home_relays, self->session_relay_url,
    &err);
  g_clear_error(&err);

  gboolean was_held = self->held;
  /* nostrc-862u: session_relay_only with no session relay holds the outbox
   * (rows stay pending and local) instead of widening to home relays or
   * failing rows as "no relays configured". */
  self->held = self->targets == NULL &&
               self->upstream_mode == ND_UPSTREAM_MODE_SESSION_RELAY_ONLY;
  if (self->held && !was_held)
    g_message("nostr-dav: publishing held: upstream_mode=session_relay_only "
              "and no session relay socket; DAV edits stay local (pending)");
  else if (was_held && !self->held)
    g_message("nostr-dav: publishing resumed via the session relay");

  /* Clamp numeric quorum to the target set size so a misconfiguration
   * ("quorum = 5, home_relays = 2") does not silently retry forever.
   * `all` never needs clamping — it always waits for every target. */
  NdPublishQuorum quorum = self->quorum;
  if (!quorum.all) {
    guint n = self->targets ? g_strv_length(self->targets) : 0;
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

  /* nostr_dav_publish_quorum -> engine policy: `all` is quorum 0. The
   * 120 s OK wait and 60 s..60 min backoff are the policy defaults. */
  nostr_publish_policy_init(&self->policy);
  self->policy.quorum = quorum.all ? 0 : quorum.count;
}

void
nd_publisher_configure(NdPublisher      *self,
                       const gchar      *account_pubkey,
                       const GStrv       home_relays,
                       NdPublishQuorum   quorum)
{
  g_return_if_fail(self != NULL);
  gchar *pubkey = account_pubkey ? g_strdup(account_pubkey) : NULL;
  g_free(self->account_pubkey);
  self->account_pubkey = pubkey;
  GStrv relays = home_relays ? g_strdupv(home_relays) : NULL;
  g_strfreev(self->home_relays);
  self->home_relays = relays;
  self->quorum = quorum;
  recompute_targets(self);
}

void
nd_publisher_set_upstream(NdPublisher    *self,
                          NdUpstreamMode  mode,
                          const gchar    *session_relay_url)
{
  g_return_if_fail(self != NULL);
  self->upstream_mode = mode;
  g_free(self->session_relay_url);
  self->session_relay_url =
    (session_relay_url && *session_relay_url) ? g_strdup(session_relay_url)
                                              : NULL;
  recompute_targets(self);
}

gboolean
nd_publisher_is_held(NdPublisher *self)
{
  g_return_val_if_fail(self != NULL, FALSE);
  return self->held;
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
  nostr_publisher_bind_transport(self->engine, relay_url, transport);
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

gboolean
nd_publisher_tick(NdPublisher *self, gint64 now_ts)
{
  g_return_val_if_fail(self != NULL, FALSE);

  /* Settle in-flight rows whose OK-wait deadline has passed (silent
   * relays count as transient failures) before loading due rows. */
  nostr_publisher_tick(self->engine, now_ts);

  /* Held (session_relay_only, no session relay): leave pending rows alone;
   * they publish once nd_publisher_set_upstream() supplies the relay. */
  if (self->held)
    return FALSE;

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
      /* Skip rows already in flight from a previous tick (they settle
       * via record_ok / the deadline sweep). The key includes (outbox,
       * collection) so a tombstone and a collection row that happen to
       * share an id string never alias each other. */
      g_autofree gchar *key = in_flight_key(row->outbox, row->col,
                                            row->row_id);
      if (g_hash_table_contains(self->in_flight, key)) {
        more = TRUE;
        continue;
      }

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
  (void)nostr_publisher_record_ok(self->engine, relay_url, event_id, ok,
                                  reason);
}
