#include "gh-store.h"

/* Schema v1: privacy charter §3.3, with these deliberate differences:
 *
 * - schema_migrations records every applied migration next to
 *   PRAGMA user_version (the version that is checked on open).
 * - messages.outbox_id is ON DELETE SET NULL, so pruning a settled outbox row
 *   never fails a foreign-key check; indexes back the foreign keys that cascade
 *   or are nulled (messages.outbox_id, outbox.conversation_id).
 * - The optional messages_fts table is not created: Ubuntu 24.04's SQLCipher
 *   (4.5.6) is built without FTS5. Search adds it (or FTS4) in a later
 *   migration once it is designed.
 *
 * Column meanings are documented in the charter; the integer encodings are
 * the GhStore* enums in gh-store.h. Migrations are append-only: never edit a
 * released step, add the next version instead. */
static const gchar schema_v1[] =
  "CREATE TABLE schema_migrations ("
  "  version     INTEGER PRIMARY KEY,"
  "  applied_at  INTEGER NOT NULL,"
  "  description TEXT NOT NULL);"

  "CREATE TABLE meta (key TEXT PRIMARY KEY, value BLOB NOT NULL) WITHOUT ROWID;"

  "CREATE TABLE conversations ("
  "  id               INTEGER PRIMARY KEY,"
  "  backend          INTEGER NOT NULL CHECK (backend IN (1,2,3)),"
  "  backend_key      TEXT NOT NULL,"
  "  title            TEXT,"
  "  created_at       INTEGER NOT NULL,"
  "  last_activity    INTEGER NOT NULL,"
  "  unread_count     INTEGER NOT NULL DEFAULT 0,"
  "  last_read_msg    INTEGER,"
  "  pinned_rank      INTEGER,"
  "  muted_until      INTEGER NOT NULL DEFAULT 0,"
  "  disappearing_s   INTEGER NOT NULL DEFAULT 0,"
  "  request_state    INTEGER NOT NULL DEFAULT 0,"
  "  draft            TEXT,"
  "  forgotten_before INTEGER NOT NULL DEFAULT 0,"
  "  UNIQUE (backend, backend_key));"

  "CREATE TABLE participants ("
  "  conversation_id INTEGER NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,"
  "  pubkey TEXT NOT NULL,"
  "  role   TEXT,"
  "  PRIMARY KEY (conversation_id, pubkey)) WITHOUT ROWID;"

  "CREATE TABLE messages ("
  "  id              INTEGER PRIMARY KEY,"
  "  conversation_id INTEGER NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,"
  "  backend_msg_id  TEXT NOT NULL,"
  "  sender_pubkey   TEXT NOT NULL,"
  "  kind            INTEGER NOT NULL,"
  "  created_at      INTEGER NOT NULL,"
  "  received_at     INTEGER NOT NULL,"
  "  direction       INTEGER NOT NULL,"
  "  body            TEXT,"
  "  raw_json        TEXT NOT NULL,"
  "  reply_to        TEXT,"
  "  expires_at      INTEGER,"
  "  outbox_id       INTEGER REFERENCES outbox(id) ON DELETE SET NULL,"
  "  UNIQUE (conversation_id, backend_msg_id));"
  "CREATE INDEX messages_by_time ON messages (conversation_id, created_at, backend_msg_id);"
  "CREATE INDEX messages_by_expiry ON messages (expires_at) WHERE expires_at IS NOT NULL;"
  "CREATE INDEX messages_by_outbox ON messages (outbox_id) WHERE outbox_id IS NOT NULL;"

  "CREATE TABLE seen ("
  "  ns         INTEGER NOT NULL,"
  "  id         TEXT NOT NULL,"
  "  first_seen INTEGER NOT NULL,"
  "  PRIMARY KEY (ns, id)) WITHOUT ROWID;"

  "CREATE TABLE outbox ("
  "  id              INTEGER PRIMARY KEY,"
  "  conversation_id INTEGER NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,"
  "  op_id           TEXT NOT NULL UNIQUE,"
  "  backend         INTEGER NOT NULL,"
  "  state           INTEGER NOT NULL,"
  "  rumor_json      TEXT,"
  "  created_at      INTEGER NOT NULL,"
  "  next_attempt_at INTEGER,"
  "  attempts        INTEGER NOT NULL DEFAULT 0,"
  "  last_error      TEXT);"
  "CREATE INDEX outbox_by_conversation ON outbox (conversation_id);"

  "CREATE TABLE outbox_events ("
  "  id            INTEGER PRIMARY KEY,"
  "  outbox_id     INTEGER NOT NULL REFERENCES outbox(id) ON DELETE CASCADE,"
  "  role          INTEGER NOT NULL,"
  "  target_pubkey TEXT,"
  "  event_id      TEXT NOT NULL,"
  "  event_json    TEXT,"
  "  not_before    INTEGER NOT NULL DEFAULT 0,"
  "  UNIQUE (outbox_id, role, target_pubkey));"

  "CREATE TABLE outbox_targets ("
  "  outbox_event_id INTEGER NOT NULL REFERENCES outbox_events(id) ON DELETE CASCADE,"
  "  relay_url       TEXT NOT NULL,"
  "  outcome         INTEGER NOT NULL DEFAULT 0,"
  "  ok_prefix       INTEGER,"
  "  ok_message      TEXT,"
  "  attempts        INTEGER NOT NULL DEFAULT 0,"
  "  last_attempt_at INTEGER,"
  "  PRIMARY KEY (outbox_event_id, relay_url)) WITHOUT ROWID;"

  "CREATE TABLE directory ("
  "  pubkey     TEXT NOT NULL,"
  "  kind       INTEGER NOT NULL,"
  "  event_id   TEXT NOT NULL,"
  "  created_at INTEGER NOT NULL,"
  "  event_json TEXT NOT NULL,"
  "  fetched_at INTEGER NOT NULL,"
  "  PRIMARY KEY (pubkey, kind)) WITHOUT ROWID;"

  "CREATE TABLE contacts ("
  "  pubkey      TEXT PRIMARY KEY,"
  "  petname     TEXT,"
  "  accepted_at INTEGER,"
  "  blocked     INTEGER NOT NULL DEFAULT 0) WITHOUT ROWID;"

  "CREATE TABLE cursors ("
  "  scope     TEXT NOT NULL,"
  "  relay_url TEXT NOT NULL,"
  "  since     INTEGER NOT NULL,"
  "  PRIMARY KEY (scope, relay_url)) WITHOUT ROWID;"

  "CREATE TABLE nip29_groups ("
  "  conversation_id INTEGER PRIMARY KEY REFERENCES conversations(id) ON DELETE CASCADE,"
  "  relay_url     TEXT NOT NULL,"
  "  group_id      TEXT NOT NULL,"
  "  relay_pubkey  TEXT NOT NULL,"
  "  snapshot_json TEXT);"

  /* Decrypted attachment / preview cache, LRU-capped at 200 MiB (G21). */
  "CREATE TABLE media ("
  "  sha256    TEXT PRIMARY KEY,"
  "  mime      TEXT,"
  "  bytes     BLOB NOT NULL,"
  "  last_used INTEGER NOT NULL) WITHOUT ROWID;"

  /* MLS state for GhStoreMarmot (D5 = MarmotStorage vtable, G23). */
  "CREATE TABLE mls_groups ("
  "  conversation_id INTEGER PRIMARY KEY REFERENCES conversations(id) ON DELETE CASCADE,"
  "  mls_group_id   BLOB NOT NULL UNIQUE,"
  "  nostr_group_id BLOB,"
  "  epoch          INTEGER,"
  "  state          INTEGER NOT NULL);"
  "CREATE TABLE mls_kv ("
  "  label TEXT NOT NULL,"
  "  key   BLOB NOT NULL,"
  "  value BLOB NOT NULL,"
  "  PRIMARY KEY (label, key)) WITHOUT ROWID;"
  "CREATE TABLE mls_snapshots ("
  "  group_id   BLOB NOT NULL,"
  "  name       TEXT NOT NULL,"
  "  created_at INTEGER NOT NULL,"
  "  data       BLOB NOT NULL,"
  "  PRIMARY KEY (group_id, name)) WITHOUT ROWID;";

static const GhStoreMigration migrations[] = {
  { 1, "Groundhog store schema v1 (privacy charter §3.3)", schema_v1 },
};

G_STATIC_ASSERT(G_N_ELEMENTS(migrations) == GH_STORE_SCHEMA_VERSION);

const GhStoreMigration *
gh_store_schema_get_migrations(gsize *n_migrations)
{
  if (n_migrations)
    *n_migrations = G_N_ELEMENTS(migrations);
  return migrations;
}
