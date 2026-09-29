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

/* Schema v2 (G23, charter §3.9, D5 = vtable): the rest of libmarmot's
 * MarmotStorage, beside v1's mls_kv (mls_store/load/delete) and mls_snapshots
 * (one header row per snapshot). gh-store-marmot.c is the only writer; value
 * encodings are documented in gh-store-marmot.h. v1's mls_groups remains the
 * caller's conversation link and is not written by GhStoreMarmot.
 *
 * - Natural keys are the libmarmot ids (MLS group id, event ids, KeyPackageRef).
 *   Tables whose rows can be large (text, JSON, relay lists) keep a rowid.
 * - admin_pubkeys columns hold n x 32 bytes; relay lists in welcomes and key
 *   packages are length-prefixed blobs, so any URL round-trips exactly.
 * - mls_snapshot_rows holds a snapshot's exact row copies (tbl says which
 *   table). c0..c12 are declared without a type (BLOB affinity), so SQLite
 *   stores every copied value unchanged: storage class and bytes. Deleting the
 *   header in mls_snapshots deletes its rows. */
static const gchar schema_v2[] =
  "CREATE TABLE mls_group_info ("
  "  mls_group_id              BLOB NOT NULL PRIMARY KEY,"
  "  nostr_group_id            BLOB NOT NULL,"
  "  name                      TEXT,"
  "  description               TEXT,"
  "  image_hash                BLOB,"
  "  image_key                 BLOB,"
  "  image_nonce               BLOB,"
  "  admin_pubkeys             BLOB,"
  "  last_message_id           TEXT,"
  "  last_message_at           INTEGER NOT NULL,"
  "  last_message_processed_at INTEGER NOT NULL,"
  "  epoch                     INTEGER NOT NULL,"
  "  state                     INTEGER NOT NULL);"
  "CREATE INDEX mls_group_info_by_nostr_id ON mls_group_info (nostr_group_id);"

  "CREATE TABLE mls_group_relays ("
  "  mls_group_id BLOB NOT NULL,"
  "  relay_url    TEXT NOT NULL,"
  "  position     INTEGER NOT NULL,"
  "  PRIMARY KEY (mls_group_id, relay_url)) WITHOUT ROWID;"

  "CREATE TABLE mls_exporter_secrets ("
  "  mls_group_id BLOB NOT NULL,"
  "  epoch        INTEGER NOT NULL,"
  "  secret       BLOB NOT NULL,"
  "  PRIMARY KEY (mls_group_id, epoch)) WITHOUT ROWID;"

  "CREATE TABLE mls_messages ("
  "  id               BLOB NOT NULL UNIQUE,"
  "  mls_group_id     BLOB NOT NULL,"
  "  pubkey           BLOB NOT NULL,"
  "  kind             INTEGER NOT NULL,"
  "  created_at       INTEGER NOT NULL,"
  "  processed_at     INTEGER NOT NULL,"
  "  content          TEXT,"
  "  tags_json        TEXT,"
  "  event_json       TEXT,"
  "  wrapper_event_id BLOB NOT NULL,"
  "  epoch            INTEGER NOT NULL,"
  "  state            INTEGER NOT NULL);"
  "CREATE INDEX mls_messages_by_created ON mls_messages "
  "  (mls_group_id, created_at, processed_at, id);"
  "CREATE INDEX mls_messages_by_processed ON mls_messages "
  "  (mls_group_id, processed_at, created_at, id);"

  "CREATE TABLE mls_processed_messages ("
  "  wrapper_event_id BLOB NOT NULL PRIMARY KEY,"
  "  message_event_id BLOB,"
  "  processed_at     INTEGER NOT NULL,"
  "  epoch            INTEGER NOT NULL,"
  "  mls_group_id     BLOB,"
  "  state            INTEGER NOT NULL,"
  "  failure_reason   TEXT) WITHOUT ROWID;"

  "CREATE TABLE mls_welcomes ("
  "  id                  BLOB NOT NULL UNIQUE,"
  "  wrapper_event_id    BLOB NOT NULL UNIQUE,"
  "  event_json          TEXT,"
  "  mls_group_id        BLOB,"
  "  nostr_group_id      BLOB NOT NULL,"
  "  group_name          TEXT,"
  "  group_description   TEXT,"
  "  group_image_hash    BLOB,"
  "  group_admin_pubkeys BLOB,"
  "  group_relays        BLOB,"
  "  welcomer            BLOB NOT NULL,"
  "  member_count        INTEGER NOT NULL,"
  "  state               INTEGER NOT NULL);"
  "CREATE INDEX mls_welcomes_by_state ON mls_welcomes (state);"

  "CREATE TABLE mls_processed_welcomes ("
  "  wrapper_event_id BLOB NOT NULL PRIMARY KEY,"
  "  welcome_event_id BLOB,"
  "  processed_at     INTEGER NOT NULL,"
  "  state            INTEGER NOT NULL,"
  "  failure_reason   TEXT) WITHOUT ROWID;"

  "CREATE TABLE mls_key_packages ("
  "  ref          BLOB NOT NULL PRIMARY KEY,"
  "  owner_pubkey BLOB NOT NULL,"
  "  relay_urls   BLOB,"
  "  created_at   INTEGER NOT NULL,"
  "  active       INTEGER NOT NULL);"
  "CREATE INDEX mls_key_packages_by_owner ON mls_key_packages (owner_pubkey);"

  "CREATE TABLE mls_snapshot_rows ("
  "  group_id BLOB NOT NULL,"
  "  name     TEXT NOT NULL,"
  "  tbl      INTEGER NOT NULL,"
  "  c0, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11, c12,"
  "  FOREIGN KEY (group_id, name) REFERENCES mls_snapshots (group_id, name) "
  "    ON DELETE CASCADE);"
  "CREATE INDEX mls_snapshot_rows_by_snapshot ON mls_snapshot_rows (group_id, name, tbl);";

/* Schema v3 (G19, charter §3.3 `contacts`, P8): when the account marked a
 * contact's key verified after comparing it out of band (unix seconds; 0 =
 * not marked). Local only: never published, never shared. gh-store-contacts.c
 * is the only writer. */
static const gchar schema_v3[] =
  "ALTER TABLE contacts ADD COLUMN verified_at INTEGER NOT NULL DEFAULT 0;";

/* Schema v4 (W18; local only, never published, P8):
 *
 * - Read state by admission (nostrc-qp24.75). messages.seq is the message's
 *   place in its room's arrival order, from the room's own counter
 *   conversations.admit_seq, which never goes back, so a sequence is never
 *   reused (unlike a rowid after its row is deleted). The trigger numbers
 *   every insert, whichever write stores it. conversations.read_seq is how
 *   far that order the read marker (last_read_msg) was set: a message is read
 *   when it sorts at or before the marker AND arrived by then, so one that
 *   arrives later (a same-second rumor with a lower id, a clock-skewed or
 *   delayed one) is unread wherever it sorts. reply_read_at/_id is the
 *   newest own message another device wrote (a relay-delivered self-copy):
 *   everything sorting at or before it is read, as "replying implies having
 *   read what came before" holds there. Existing rows keep seq 0 and
 *   read_seq 0, which reads exactly as before.
 * - A read marker never dangles: before a message row is deleted (purge,
 *   cancel, moderation, forget), a marker on it moves back to the newest
 *   remaining message of its room at or before it.
 * - conversations.timer_changed_at: when the disappearing timer last changed
 *   (unix seconds; 0 = never), for the local timeline row (nostrc-qp24.83).
 * - outbox_events.no_inbox: the recipient had no usable kind-10050 when last
 *   looked up, so a targetless wrap stays "hasn't set up private messaging"
 *   across restarts (nostrc-9cho). */
static const gchar schema_v4[] =
  "ALTER TABLE messages ADD COLUMN seq INTEGER NOT NULL DEFAULT 0;"
  "ALTER TABLE conversations ADD COLUMN admit_seq INTEGER NOT NULL DEFAULT 0;"
  "ALTER TABLE conversations ADD COLUMN read_seq INTEGER NOT NULL DEFAULT 0;"
  "ALTER TABLE conversations ADD COLUMN reply_read_at INTEGER;"
  "ALTER TABLE conversations ADD COLUMN reply_read_id TEXT;"
  "ALTER TABLE conversations ADD COLUMN timer_changed_at INTEGER NOT NULL DEFAULT 0;"
  "ALTER TABLE outbox_events ADD COLUMN no_inbox INTEGER NOT NULL DEFAULT 0;"

  "CREATE TRIGGER messages_admit_seq AFTER INSERT ON messages BEGIN "
  "  UPDATE conversations SET admit_seq = admit_seq + 1 WHERE id = NEW.conversation_id; "
  "  UPDATE messages SET seq = (SELECT admit_seq FROM conversations "
  "    WHERE id = NEW.conversation_id) WHERE id = NEW.id; "
  "END;"

  "CREATE TRIGGER messages_keep_read_marker BEFORE DELETE ON messages BEGIN "
  "  UPDATE conversations SET last_read_msg = (SELECT k.id FROM messages k "
  "    WHERE k.conversation_id = OLD.conversation_id AND k.id <> OLD.id AND "
  "    (k.created_at < OLD.created_at OR (k.created_at = OLD.created_at AND "
  "    k.backend_msg_id < OLD.backend_msg_id)) "
  "    ORDER BY k.created_at DESC, k.backend_msg_id DESC LIMIT 1) "
  "  WHERE id = OLD.conversation_id AND last_read_msg = OLD.id; "
  "END;";

static const GhStoreMigration migrations[] = {
  { 1, "Groundhog store schema v1 (privacy charter §3.3)", schema_v1 },
  { 2, "MLS state for libmarmot's MarmotStorage (charter §3.9, G23)", schema_v2 },
  { 3, "Local verification marks on contacts (charter §3.3, G19)", schema_v3 },
  { 4, "Read state by arrival, timer changes, recipients without an inbox (W18)", schema_v4 },
};

G_STATIC_ASSERT(G_N_ELEMENTS(migrations) == GH_STORE_SCHEMA_VERSION);

const GhStoreMigration *
gh_store_schema_get_migrations(gsize *n_migrations)
{
  if (n_migrations)
    *n_migrations = G_N_ELEMENTS(migrations);
  return migrations;
}
