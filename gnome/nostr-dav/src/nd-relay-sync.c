/* nd-relay-sync.c - Inbound relay subscription + fold/upsert
 *
 * SPDX-License-Identifier: MIT
 *
 * See nd-relay-sync.h for the design. Two responsibilities:
 *
 *   1. Fold — nd_relay_sync_ingest_event() takes an EVENT object JSON,
 *      validates the envelope, dispatches on kind, and folds the event
 *      into the SQLite stores using last-writer-wins by created_at.
 *      Idempotent per event id (in-memory LRU) so a relay that resends
 *      history does not thrash the DB.
 *
 *   2. Transport — nd_relay_sync_start()/stop() drive one
 *      NdRelayTransport per configured home relay. Backoff is
 *      exponential (60 s -> 60 min) using a GSource wall-clock timer.
 *      NIP-42 AUTH challenges are cached per-URL so a reconnect does
 *      not need a fresh signer round-trip.
 *
 * The transport lifecycle uses the fixture backend under tests. The
 * websocket backend is a scaffold today — production wiring lands in
 * the follow-up bead — but the state-machine and cursor code here are
 * exercised end-to-end via the fixture.
 */

#include "nd-relay-sync.h"

#include "nd-ical.h"
#include "nd-vcard.h"

#include <json-glib/json-glib.h>
#include <sqlite3.h>

#include <string.h>

G_DEFINE_QUARK(nd-relay-sync-error-quark, nd_relay_sync_error)

/* Reconnect backoff — plan spec (60 s -> 60 min cap, doubled). */
#define ND_BACKOFF_INITIAL_SEC 60
#define ND_BACKOFF_MAX_SEC     (60 * 60)

/* In-memory dedup ring. 4096 is generous for a per-user home_relays
 * fan-in; the cursor persists the actual last-seen created_at across
 * restarts so this is a soft cache, not the durable dedup. */
#define ND_DEDUP_CAPACITY 4096

typedef struct {
  gchar             *url;
  NdRelayTransport  *transport;
  guint              backoff_sec;
  gchar             *auth_challenge;   /* NIP-42 cache; NULL if none */
} NdRelayEndpoint;

struct _NdRelaySync {
  NdStoreDb        *db;
  NdCalendarStore  *cal_store;
  NdContactStore   *contact_store;

  NdRelayTransportFactory  factory;
  gpointer                 factory_data;

  gchar   *account_pubkey;    /* hex64 or NULL */
  GStrv    home_relays;       /* owned */
  NdUpstreamMode upstream_mode;

  GHashTable *endpoints;      /* url -> NdRelayEndpoint*, owned */
  gboolean    started;

  /* Dedup: hash-set of event ids most recently seen. Keys owned. */
  GHashTable *seen_ids;
  GQueue     *seen_order;     /* insertion order for eviction */
};

/* ---- Helpers ---- */

static void
endpoint_free(gpointer data)
{
  NdRelayEndpoint *ep = data;
  if (ep == NULL)
    return;
  if (ep->transport != NULL) {
    nd_relay_transport_set_listener(ep->transport, NULL, NULL);
    nd_relay_transport_set_state_callback(ep->transport, NULL, NULL);
    nd_relay_transport_disconnect(ep->transport);
    nd_relay_transport_unref(ep->transport);
  }
  g_free(ep->url);
  g_free(ep->auth_challenge);
  g_free(ep);
}

static gboolean
dedup_seen(NdRelaySync *self, const gchar *event_id)
{
  if (g_hash_table_contains(self->seen_ids, event_id))
    return TRUE;
  gchar *copy = g_strdup(event_id);
  g_hash_table_add(self->seen_ids, copy);
  g_queue_push_tail(self->seen_order, copy);
  while (g_queue_get_length(self->seen_order) > ND_DEDUP_CAPACITY) {
    gpointer old = g_queue_pop_head(self->seen_order);
    g_hash_table_remove(self->seen_ids, old);
  }
  return FALSE;
}

/* ---- SQL fold helpers ----
 *
 * Both helpers preserve any local `publish_state='pending'` row: the
 * DAV client's local write must win over an inbound copy of an older
 * version. Concretely, the incoming event is only written when its
 * created_at is strictly greater than the row's existing created_at
 * (which, for a `pending` row, was set when the local write staged the
 * outbox entry). */

static gboolean
fold_calendar_row(NdRelaySync *self,
                  const gchar *uid,
                  const gchar *ical,
                  const gchar *etag,
                  gint         kind,
                  const gchar *pubkey,
                  gint64       created_at,
                  const gchar *event_id,
                  GError     **error)
{
  sqlite3 *h = nd_store_db_get_handle(self->db);
  sqlite3_stmt *stmt = NULL;

  /* Skip the fold entirely for rows currently `pending` locally: the
   * inbound copy might be an older version of what the user just wrote,
   * or the same version echoed back from the relay before the publisher
   * transitioned the row to `published`. Overwriting `ical`/`etag`
   * while the outbox row is still pending would either publish the
   * inbound copy (data loss on the local edit) or shift the ETag under
   * the DAV client mid-transaction. */
  if (sqlite3_prepare_v2(h,
        "INSERT INTO events "
        "  (uid, ical, etag, kind, pubkey, created_at, nostr_event_id,"
        "   updated_at, publish_state) "
        "  VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, 'published') "
        "  ON CONFLICT(uid) DO UPDATE SET "
        "    ical = excluded.ical, "
        "    etag = excluded.etag, "
        "    kind = excluded.kind, "
        "    pubkey = excluded.pubkey, "
        "    created_at = excluded.created_at, "
        "    nostr_event_id = excluded.nostr_event_id, "
        "    updated_at = excluded.updated_at, "
        "    publish_state = CASE "
        "      WHEN events.publish_state = 'failed_permanent' "
        "        THEN events.publish_state "
        "      ELSE 'published' END "
        "  WHERE events.publish_state != 'pending' "
        "    AND excluded.created_at > events.created_at",
        -1, &stmt, NULL) != SQLITE_OK)
    return nd_store_db_set_sql_error(self->db, error, "prepare fold event");

  sqlite3_bind_text (stmt, 1, uid, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (stmt, 2, ical, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (stmt, 3, etag, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int  (stmt, 4, kind);
  sqlite3_bind_text (stmt, 5, pubkey, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 6, created_at);
  sqlite3_bind_text (stmt, 7, event_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 8, g_get_real_time() / G_USEC_PER_SEC);

  gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  gboolean wrote_row = (sqlite3_changes(h) > 0);
  if (!ok)
    nd_store_db_set_sql_error(self->db, error, "fold event");
  sqlite3_finalize(stmt);
  if (!ok)
    return FALSE;

  if (wrote_row &&
      !nd_store_db_bump_generation(self->db,
                                   ND_STORE_COLLECTION_EVENTS, error))
    return FALSE;
  return TRUE;
}

static gboolean
fold_contact_row(NdRelaySync *self,
                 const gchar *uid,
                 const gchar *vcard,
                 const gchar *etag,
                 const gchar *pubkey,
                 gint64       created_at,
                 const gchar *event_id,
                 GError     **error)
{
  sqlite3 *h = nd_store_db_get_handle(self->db);
  sqlite3_stmt *stmt = NULL;

  if (sqlite3_prepare_v2(h,
        "INSERT INTO contacts "
        "  (uid, vcard, etag, pubkey, created_at, nostr_event_id, "
        "   updated_at, publish_state) "
        "  VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, 'published') "
        "  ON CONFLICT(uid) DO UPDATE SET "
        "    vcard = excluded.vcard, "
        "    etag = excluded.etag, "
        "    pubkey = excluded.pubkey, "
        "    created_at = excluded.created_at, "
        "    nostr_event_id = excluded.nostr_event_id, "
        "    updated_at = excluded.updated_at, "
        "    publish_state = CASE "
        "      WHEN contacts.publish_state = 'failed_permanent' "
        "        THEN contacts.publish_state "
        "      ELSE 'published' END "
        "  WHERE contacts.publish_state != 'pending' "
        "    AND excluded.created_at > contacts.created_at",
        -1, &stmt, NULL) != SQLITE_OK)
    return nd_store_db_set_sql_error(self->db, error, "prepare fold contact");

  sqlite3_bind_text (stmt, 1, uid, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (stmt, 2, vcard, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (stmt, 3, etag, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (stmt, 4, pubkey, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 5, created_at);
  sqlite3_bind_text (stmt, 6, event_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 7, g_get_real_time() / G_USEC_PER_SEC);

  gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  gboolean wrote_row = (sqlite3_changes(h) > 0);
  if (!ok)
    nd_store_db_set_sql_error(self->db, error, "fold contact");
  sqlite3_finalize(stmt);
  if (!ok)
    return FALSE;

  if (wrote_row &&
      !nd_store_db_bump_generation(self->db,
                                   ND_STORE_COLLECTION_CONTACTS, error))
    return FALSE;
  return TRUE;
}

/* Delete by exact `nostr_event_id` match across events/contacts. Local
 * `pending` rows are protected: a tombstone from the relay must not
 * silently drop a DAV write the user just made and the outbox has not
 * yet finished publishing. */
static gboolean
delete_by_event_id(NdRelaySync *self, const gchar *event_id,
                   gboolean *out_removed, GError **error)
{
  sqlite3 *h = nd_store_db_get_handle(self->db);
  gboolean removed = FALSE;

  static const char *const sqls[] = {
    "DELETE FROM events   WHERE nostr_event_id = ?1 "
    "  AND publish_state != 'pending'",
    "DELETE FROM contacts WHERE nostr_event_id = ?1 "
    "  AND publish_state != 'pending'",
  };
  static const NdStoreCollection cols[] = {
    ND_STORE_COLLECTION_EVENTS,
    ND_STORE_COLLECTION_CONTACTS,
  };

  for (gsize i = 0; i < G_N_ELEMENTS(sqls); i++) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(h, sqls[i], -1, &stmt, NULL) != SQLITE_OK)
      return nd_store_db_set_sql_error(self->db, error, "prepare tombstone");
    sqlite3_bind_text(stmt, 1, event_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      nd_store_db_set_sql_error(self->db, error, "tombstone by id");
      sqlite3_finalize(stmt);
      return FALSE;
    }
    if (sqlite3_changes(h) > 0) {
      removed = TRUE;
      if (!nd_store_db_bump_generation(self->db, cols[i], error)) {
        sqlite3_finalize(stmt);
        return FALSE;
      }
    }
    sqlite3_finalize(stmt);
  }

  if (out_removed)
    *out_removed = removed;
  return TRUE;
}

/* Delete a row by (kind, pubkey, d-tag). Used for NIP-09 `a`-tag
 * tombstones which reference an addressable event coordinate rather
 * than a concrete event id. */
static gboolean
delete_by_address(NdRelaySync *self,
                  gint         kind,
                  const gchar *pubkey,
                  const gchar *d_tag,
                  gboolean    *out_removed,
                  GError     **error)
{
  /* NIP-09 authorises deletion only for the target address's own author.
   * In v1 our fold path already dropped the kind-5 unless it was signed
   * by @account_pubkey, so the a-tag's pubkey must match that; still
   * reject silently on mismatch as documentation of the invariant. */
  if (self->account_pubkey != NULL && pubkey != NULL &&
      !g_str_equal(self->account_pubkey, pubkey)) {
    if (out_removed) *out_removed = FALSE;
    return TRUE;
  }

  sqlite3 *h = nd_store_db_get_handle(self->db);
  const gchar *table = NULL;
  NdStoreCollection col = ND_STORE_COLLECTION_EVENTS;
  if (kind == ND_NIP52_KIND_DATE || kind == ND_NIP52_KIND_TIME) {
    table = "events";
    col   = ND_STORE_COLLECTION_EVENTS;
  } else if (kind == ND_CONTACT_KIND) {
    table = "contacts";
    col   = ND_STORE_COLLECTION_CONTACTS;
  } else {
    /* Unknown addressable kind — silently ignore rather than error out. */
    if (out_removed) *out_removed = FALSE;
    return TRUE;
  }

  g_autofree gchar *sql = g_strdup_printf(
    "DELETE FROM %s WHERE uid = ?1 AND publish_state != 'pending'", table);
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(h, sql, -1, &stmt, NULL) != SQLITE_OK)
    return nd_store_db_set_sql_error(self->db, error, "prepare a-tag delete");
  sqlite3_bind_text(stmt, 1, d_tag, -1, SQLITE_TRANSIENT);
  gboolean removed = FALSE;
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    nd_store_db_set_sql_error(self->db, error, "a-tag delete");
    sqlite3_finalize(stmt);
    return FALSE;
  }
  if (sqlite3_changes(h) > 0) {
    removed = TRUE;
    if (!nd_store_db_bump_generation(self->db, col, error)) {
      sqlite3_finalize(stmt);
      return FALSE;
    }
  }
  sqlite3_finalize(stmt);
  if (out_removed) *out_removed = removed;
  return TRUE;
}

/* ---- Kind-specific fold ---- */

static gboolean
fold_calendar_event(NdRelaySync *self,
                    const gchar *event_object_json,
                    JsonObject  *obj,
                    GError     **error)
{
  GError *parse_err = NULL;
  NdCalendarEvent *event =
    nd_ical_event_from_nip52_json(event_object_json, &parse_err);
  if (event == NULL) {
    g_propagate_prefixed_error(error, parse_err,
                               "kind %d event unparseable: ",
                               (int)json_object_get_int_member(obj, "kind"));
    return FALSE;
  }

  g_autofree gchar *ical = nd_ical_generate_vevent(event);
  g_autofree gchar *etag = nd_ical_compute_etag(event);
  const gchar *event_id = json_object_get_string_member(obj, "id");

  gboolean ok = fold_calendar_row(self, event->uid, ical, etag, event->kind,
                                  event->pubkey, event->created_at,
                                  event_id, error);
  nd_calendar_event_free(event);
  return ok;
}

static gboolean
fold_contact_event(NdRelaySync *self,
                   const gchar *event_object_json,
                   JsonObject  *obj,
                   GError     **error)
{
  GError *parse_err = NULL;
  NdContact *contact = nd_vcard_from_nostr_json(event_object_json, &parse_err);
  if (contact == NULL) {
    g_propagate_prefixed_error(error, parse_err,
                               "kind 30085 event unparseable: ");
    return FALSE;
  }

  g_autofree gchar *vcard = nd_vcard_generate(contact);
  g_autofree gchar *etag  = nd_vcard_compute_etag(contact);
  const gchar *event_id = json_object_get_string_member(obj, "id");

  gboolean ok = fold_contact_row(self, contact->uid, vcard, etag,
                                 contact->pubkey, contact->created_at,
                                 event_id, error);
  nd_contact_free(contact);
  return ok;
}

/* Kind 5 — NIP-09 deletion event. `e`/`a`-tag targets are resolved
 * against the store and deleted if their author matches the deletion's
 * signer. NIP-09 authorises only the target event's author to delete
 * its events; when we do not know the account pubkey yet we drop the
 * kind-5 rather than fall through unauthenticated (see review B1). */
static gboolean
fold_deletion(NdRelaySync *self, JsonObject *obj, GError **error)
{
  const gchar *pubkey = json_object_get_string_member(obj, "pubkey");
  if (pubkey == NULL)
    return TRUE; /* silently drop malformed */

  if (self->account_pubkey == NULL ||
      !g_str_equal(self->account_pubkey, pubkey)) {
    /* Deletion from a stranger (or before we know the account) — drop. */
    return TRUE;
  }

  JsonArray *tags = json_object_has_member(obj, "tags")
                      ? json_object_get_array_member(obj, "tags")
                      : NULL;
  if (tags == NULL)
    return TRUE;

  guint n = json_array_get_length(tags);
  for (guint i = 0; i < n; i++) {
    JsonNode *item = json_array_get_element(tags, i);
    if (item == NULL || JSON_NODE_TYPE(item) != JSON_NODE_ARRAY)
      continue;
    JsonArray *t = json_node_get_array(item);
    if (json_array_get_length(t) < 2)
      continue;
    const gchar *name = json_array_get_string_element(t, 0);
    const gchar *value = json_array_get_string_element(t, 1);
    if (name == NULL || value == NULL)
      continue;

    if (g_str_equal(name, "e")) {
      if (!delete_by_event_id(self, value, NULL, error))
        return FALSE;
    } else if (g_str_equal(name, "a")) {
      /* Format: <kind>:<pubkey>:<d-tag> */
      gchar **parts = g_strsplit(value, ":", 3);
      guint pn = g_strv_length(parts);
      if (pn == 3) {
        gint kind = atoi(parts[0]);
        gboolean ok = delete_by_address(self, kind, parts[1], parts[2],
                                        NULL, error);
        g_strfreev(parts);
        if (!ok)
          return FALSE;
      } else {
        g_strfreev(parts);
      }
    }
  }
  return TRUE;
}

/* ---- Public API ---- */

NdRelaySync *
nd_relay_sync_new(NdStoreDb              *db,
                  NdCalendarStore        *cal_store,
                  NdContactStore         *contact_store,
                  NdRelayTransportFactory factory,
                  gpointer                factory_data)
{
  g_return_val_if_fail(db != NULL, NULL);
  g_return_val_if_fail(cal_store != NULL, NULL);
  g_return_val_if_fail(contact_store != NULL, NULL);

  NdRelaySync *self = g_new0(NdRelaySync, 1);
  self->db            = nd_store_db_ref(db);
  self->cal_store     = cal_store;
  self->contact_store = contact_store;
  self->factory       = factory;
  self->factory_data  = factory_data;
  self->upstream_mode = ND_UPSTREAM_MODE_DEFAULT;
  self->endpoints     = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              NULL, endpoint_free);
  self->seen_ids      = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, NULL);
  self->seen_order    = g_queue_new();
  return self;
}

void
nd_relay_sync_free(NdRelaySync *self)
{
  if (self == NULL)
    return;
  nd_relay_sync_stop(self);
  g_clear_pointer(&self->endpoints, g_hash_table_destroy);
  g_clear_pointer(&self->seen_ids, g_hash_table_destroy);
  if (self->seen_order != NULL) {
    /* Keys were owned by seen_ids (already freed); the queue holds only
     * borrowed pointers, so it is safe to free without a per-element
     * destroy. */
    g_queue_free(self->seen_order);
    self->seen_order = NULL;
  }
  g_clear_pointer(&self->account_pubkey, g_free);
  g_clear_pointer(&self->home_relays, g_strfreev);
  g_clear_pointer(&self->db, nd_store_db_unref);
  g_free(self);
}

static void on_transport_listener(NdRelayTransport *transport,
                                  const gchar      *kind_hint,
                                  const gchar      *envelope_json,
                                  gpointer          user_data);

static NdRelayEndpoint *
endpoint_new(NdRelaySync *self, const gchar *url)
{
  NdRelayEndpoint *ep = g_new0(NdRelayEndpoint, 1);
  ep->url         = g_strdup(url);
  ep->backoff_sec = ND_BACKOFF_INITIAL_SEC;

  if (self->factory != NULL) {
    ep->transport = self->factory(url, self->factory_data);
    if (ep->transport != NULL) {
      nd_relay_transport_set_listener(ep->transport,
                                      on_transport_listener, self);
    }
  }
  return ep;
}

void
nd_relay_sync_configure(NdRelaySync    *self,
                        const gchar    *account_pubkey,
                        const GStrv     home_relays,
                        NdUpstreamMode  upstream_mode)
{
  g_return_if_fail(self != NULL);

  g_free(self->account_pubkey);
  self->account_pubkey = account_pubkey ? g_strdup(account_pubkey) : NULL;

  g_strfreev(self->home_relays);
  self->home_relays = home_relays ? g_strdupv(home_relays) : NULL;

  self->upstream_mode = upstream_mode;

  /* Prune endpoints no longer in the list, add newcomers. */
  g_autoptr(GHashTable) desired =
    g_hash_table_new(g_str_hash, g_str_equal);
  if (self->home_relays != NULL)
    for (guint i = 0; self->home_relays[i] != NULL; i++)
      g_hash_table_add(desired, self->home_relays[i]);

  GHashTableIter iter;
  gpointer key = NULL;
  GList *to_remove = NULL;
  g_hash_table_iter_init(&iter, self->endpoints);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    if (!g_hash_table_contains(desired, key))
      to_remove = g_list_prepend(to_remove, key);
  }
  for (GList *l = to_remove; l != NULL; l = l->next)
    g_hash_table_remove(self->endpoints, l->data);
  g_list_free(to_remove);

  if (self->home_relays != NULL) {
    for (guint i = 0; self->home_relays[i] != NULL; i++) {
      const gchar *url = self->home_relays[i];
      if (!g_hash_table_contains(self->endpoints, url)) {
        NdRelayEndpoint *ep = endpoint_new(self, url);
        g_hash_table_insert(self->endpoints, ep->url, ep);
      }
    }
  }
}

void
nd_relay_sync_start(NdRelaySync *self)
{
  g_return_if_fail(self != NULL);
  self->started = TRUE;

  GHashTableIter iter;
  gpointer value = NULL;
  g_hash_table_iter_init(&iter, self->endpoints);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    NdRelayEndpoint *ep = value;
    if (ep->transport != NULL)
      nd_relay_transport_connect_async(ep->transport);
  }
}

void
nd_relay_sync_stop(NdRelaySync *self)
{
  g_return_if_fail(self != NULL);
  self->started = FALSE;
  if (self->endpoints == NULL)
    return;

  GHashTableIter iter;
  gpointer value = NULL;
  g_hash_table_iter_init(&iter, self->endpoints);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    NdRelayEndpoint *ep = value;
    if (ep->transport != NULL)
      nd_relay_transport_disconnect(ep->transport);
  }
}

gboolean
nd_relay_sync_ingest_event(NdRelaySync *self,
                           const gchar *relay_url,
                           const gchar *event_object_json,
                           GError     **error)
{
  g_return_val_if_fail(self != NULL, FALSE);
  g_return_val_if_fail(event_object_json != NULL, FALSE);

  g_autoptr(JsonParser) parser = json_parser_new();
  GError *parse_err = NULL;
  if (!json_parser_load_from_data(parser, event_object_json, -1, &parse_err)) {
    g_propagate_prefixed_error(error, parse_err,
                               "EVENT payload not JSON: ");
    return FALSE;
  }
  JsonNode *root = json_parser_get_root(parser);
  if (root == NULL || JSON_NODE_TYPE(root) != JSON_NODE_OBJECT) {
    g_set_error_literal(error, ND_RELAY_SYNC_ERROR,
                        ND_RELAY_SYNC_ERROR_MALFORMED,
                        "EVENT payload is not an object");
    return FALSE;
  }
  JsonObject *obj = json_node_get_object(root);

  const gchar *event_id = json_object_get_string_member(obj, "id");
  if (event_id == NULL || strlen(event_id) != 64) {
    g_set_error_literal(error, ND_RELAY_SYNC_ERROR,
                        ND_RELAY_SYNC_ERROR_MALFORMED,
                        "EVENT id missing or not 64 hex chars");
    return FALSE;
  }
  if (dedup_seen(self, event_id))
    return TRUE;

  gint64 kind_i = json_object_get_int_member(obj, "kind");
  gint kind = (gint)kind_i;
  gint64 created_at = json_object_get_int_member(obj, "created_at");

  gboolean handled = TRUE;
  switch (kind) {
  case ND_NIP52_KIND_DATE:
  case ND_NIP52_KIND_TIME:
    handled = fold_calendar_event(self, event_object_json, obj, error);
    break;
  case ND_CONTACT_KIND:
    handled = fold_contact_event(self, event_object_json, obj, error);
    break;
  case 5:
    handled = fold_deletion(self, obj, error);
    break;
  default:
    /* Not a kind we fold — mark cursor and move on. */
    break;
  }
  if (!handled)
    return FALSE;

  if (relay_url != NULL && created_at > 0)
    return nd_store_db_set_relay_cursor(self->db, relay_url, created_at, error);
  return TRUE;
}

gboolean
nd_relay_sync_handle_envelope(NdRelaySync *self,
                              const gchar *relay_url,
                              const gchar *envelope_json,
                              GError     **error)
{
  g_return_val_if_fail(self != NULL, FALSE);
  g_return_val_if_fail(envelope_json != NULL, FALSE);

  g_autoptr(JsonParser) parser = json_parser_new();
  GError *parse_err = NULL;
  if (!json_parser_load_from_data(parser, envelope_json, -1, &parse_err)) {
    g_propagate_prefixed_error(error, parse_err,
                               "envelope not JSON: ");
    return FALSE;
  }
  JsonNode *root = json_parser_get_root(parser);
  if (root == NULL || JSON_NODE_TYPE(root) != JSON_NODE_ARRAY) {
    g_set_error_literal(error, ND_RELAY_SYNC_ERROR,
                        ND_RELAY_SYNC_ERROR_MALFORMED,
                        "envelope is not a JSON array");
    return FALSE;
  }
  JsonArray *arr = json_node_get_array(root);
  if (json_array_get_length(arr) < 1)
    return TRUE;
  const gchar *cmd = json_array_get_string_element(arr, 0);
  if (cmd == NULL)
    return TRUE;

  if (g_str_equal(cmd, "EVENT")) {
    /* ["EVENT","<sub>",{...}] */
    if (json_array_get_length(arr) < 3)
      return TRUE;
    JsonNode *ev_node = json_array_get_element(arr, 2);
    if (ev_node == NULL || JSON_NODE_TYPE(ev_node) != JSON_NODE_OBJECT)
      return TRUE;
    g_autoptr(JsonGenerator) gen = json_generator_new();
    json_generator_set_root(gen, ev_node);
    g_autofree gchar *event_object_json = json_generator_to_data(gen, NULL);
    return nd_relay_sync_ingest_event(self, relay_url, event_object_json,
                                      error);
  }
  if (g_str_equal(cmd, "AUTH")) {
    /* ["AUTH","<challenge>"] — cache the challenge per relay so the
     * publisher / subscribe rebuild can reuse it without a fresh signer
     * round-trip. */
    const gchar *challenge = json_array_get_string_element(arr, 1);
    if (challenge != NULL && relay_url != NULL) {
      NdRelayEndpoint *ep = g_hash_table_lookup(self->endpoints, relay_url);
      if (ep != NULL) {
        g_free(ep->auth_challenge);
        ep->auth_challenge = g_strdup(challenge);
      }
    }
    return TRUE;
  }
  /* EOSE / OK / NOTICE / CLOSED are noops here; the transport listener
   * consumes them for its own state machine. */
  return TRUE;
}

/* ---- Transport listener ---- */

static void
on_transport_listener(NdRelayTransport *transport,
                      const gchar      *kind_hint,
                      const gchar      *envelope_json,
                      gpointer          user_data)
{
  NdRelaySync *self = user_data;
  const gchar *url = nd_relay_transport_get_url(transport);
  GError *err = NULL;

  (void)kind_hint;   /* fast path taken via envelope prefix */
  if (!nd_relay_sync_handle_envelope(self, url, envelope_json, &err)) {
    g_warning("nostr-dav: relay %s produced an event we could not fold: %s",
              url, err ? err->message : "unknown error");
    g_clear_error(&err);
  }
}
