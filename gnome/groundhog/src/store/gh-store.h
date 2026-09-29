#ifndef GH_STORE_H
#define GH_STORE_H

#include <gio/gio.h>

#include "gh-clock.h"

G_BEGIN_DECLS

/* GhStore: Groundhog's encrypted per-account store (privacy charter §3,
 * decision D1). GTK-free; one SQLCipher 4 database per account, keyed with a
 * raw 256-bit key that only the key provider (the Secret Service in the app,
 * G03) holds. There is no plaintext fallback: without SQLCipher, a key or a
 * safe directory, nothing is written.
 *
 * Layout (§3.2), with <data> = realpath($XDG_DATA_HOME) so a legitimately
 * symlinked data home still works while every component below it must be a
 * real, private file or directory:
 *
 *   <data>/groundhog/                 0700
 *     accounts/                       0700
 *       <acct>/                       0700  gh_store_account_dir_name()
 *         store.db, -wal, -shm        0600  (SQLite copies store.db's mode)
 *
 * Existing components that are symbolic links, owned by another user, or
 * accessible to group/other are refused with GH_STORE_ERROR_PERMISSIONS and
 * never chmod-ed: they may have been planted. store.db is pre-created with
 * O_CREAT|O_EXCL|O_NOFOLLOW (0600) and opened with SQLITE_OPEN_NOFOLLOW.
 *
 * Open sequence (§3.5): the single-SQLite guard (ST-4), sqlite3_key_v2 with
 * the raw key spec "x'<64 hex>'" before any statement (exactly what
 * PRAGMA key = "x'...'" executes, without passing the key through SQL text),
 * a non-empty cipher_version, a read that verifies the key, then
 * journal_mode=WAL, synchronous=FULL, foreign_keys, secure_delete,
 * temp_store=MEMORY, busy timeout and quick_check. The key buffer lives in
 * sodium_malloc() memory and is wiped right after keying.
 *
 * Why this leaves no plaintext on disk: SQLCipher encrypts and authenticates
 * (HMAC-SHA512) every page written to store.db, the WAL and any rollback
 * journal; the first 16 bytes of store.db are a random salt, not the SQLite
 * header. Frame headers in -wal and the whole -shm wal-index hold only page
 * numbers, salts, checksums and read marks. temp_store=MEMORY (refused if the
 * library was built with SQLITE_TEMP_STORE=0) keeps temp tables, sorter spills
 * and statement journals in memory, and ATTACH is disabled so no second
 * (possibly unencrypted) database can be created through this connection.
 * Deleted content is zeroed (secure_delete) and leaves the WAL at the next
 * TRUNCATE checkpoint (purge, forget, close).
 *
 * Threading: a GhStore is used from one thread at a time (open it in a GTask
 * worker, then hand it over). The key provider is called on the opening
 * thread and may block. */

struct sqlite3;

#define GH_STORE_ERROR gh_store_error_quark()
GQuark gh_store_error_quark(void);

typedef enum {
  GH_STORE_ERROR_FAILED,        /* I/O or SQLite failure */
  GH_STORE_ERROR_INVALID,       /* malformed or out-of-bounds argument */
  GH_STORE_ERROR_PERMISSIONS,   /* unsafe mode, owner or symlink (ST-3) */
  GH_STORE_ERROR_KEY,           /* wrong key, or not a Groundhog store (ST-2) */
  GH_STORE_ERROR_KEY_MISSING,   /* key item missing; store untouched (KC-3) */
  GH_STORE_ERROR_LOCKED,        /* keyring locked and prompting not allowed (KC-2) */
  GH_STORE_ERROR_UNAVAILABLE,   /* no key store, e.g. no Secret Service (KC-4) */
  GH_STORE_ERROR_FOREIGN,       /* meta does not match account/store id (KC-5) */
  GH_STORE_ERROR_NEWER_SCHEMA,  /* created by a newer Groundhog (ST-11) */
  GH_STORE_ERROR_CORRUPT,       /* integrity check failed / store is read-only */
  GH_STORE_ERROR_FULL,          /* disk or page limit full (ST-10) */
  GH_STORE_ERROR_BUSY,          /* locked elsewhere, or already open in-process */
  GH_STORE_ERROR_NO_CIPHER,     /* not exactly one SQLite, SQLCipher (ST-4) */
  GH_STORE_ERROR_NOT_FOUND,     /* no such store or row */
  GH_STORE_ERROR_STATE          /* not valid in the current state (e.g. re-seal) */
} GhStoreError;

/* ---- Bounds ------------------------------------------------------------------
 * Arguments beyond these are rejected with GH_STORE_ERROR_INVALID before any
 * SQL runs. Callers bound untrusted input first (e.g. GH_NIP17_MAX_*). */
#ifndef GH_STORE_KEY_SIZE /* also defined, identically, by gh-store-key.h */
#define GH_STORE_KEY_SIZE               32
#endif
#define GH_STORE_MAX_ID                 128          /* lowercase hex ids */
#define GH_STORE_MAX_BACKEND_KEY        (16 * 1024)
#define GH_STORE_MAX_TITLE              4096
#define GH_STORE_MAX_BODY               (256 * 1024)
#define GH_STORE_MAX_DRAFT              (256 * 1024)
#define GH_STORE_MAX_EVENT_JSON         (512 * 1024) /* raw/rumor/event JSON */
#define GH_STORE_MAX_URL                2048
#define GH_STORE_MAX_OK_MESSAGE         1024         /* longer relay text is cut */
#define GH_STORE_MAX_PARTICIPANTS       256
#define GH_STORE_MAX_SEALED_EVENTS      256
#define GH_STORE_MAX_TARGETS_PER_EVENT  64
/* SQLITE_LIMIT_LENGTH: the largest single value or row (media cache items). */
#define GH_STORE_MAX_VALUE_SIZE         (32 * 1024 * 1024)
/* The longest disappearing timer a conversation can keep, in seconds. */
#define GH_STORE_MAX_DISAPPEARING       (366 * 24 * 60 * 60)

/* ---- Key custody seam --------------------------------------------------------
 * GhStore never persists the key. Two ways to supply it:
 *
 * - Asynchronous key custody on the main context, such as G03's GhStoreKey
 *   (gh-store-key.h): gh_store_exists() tells whether to call
 *   gh_store_key_lookup_async() (store on disk) or
 *   gh_store_key_lookup_or_create_async() (none yet); the blocking
 *   gh_store_open_with_key() then runs with the key it returned; forget is
 *   gh_store_key_destroy_async() followed by gh_store_delete_files().
 * - A synchronous provider (below), which gh_store_open() asks on every
 *   open, feeds a key it generated with the OS CSPRNG on first open, and
 *   gh_store_forget() uses to destroy the key (crypto-shred).
 *
 * Provider callbacks are all required, run synchronously on the calling
 * thread, may block and should honour @cancellable. Report these conditions
 * with GH_STORE_ERROR:
 *   GH_STORE_ERROR_KEY_MISSING   no item for the account
 *   GH_STORE_ERROR_LOCKED        locked, and the flags did not allow a prompt
 *   GH_STORE_ERROR_UNAVAILABLE   no key store at all
 * Any other error is propagated unchanged. */
typedef enum {
  GH_STORE_KEY_LOOKUP_NONE = 0,
  /* Interactive (window shown): may show the keyring unlock prompt. */
  GH_STORE_KEY_LOOKUP_ALLOW_PROMPT = 1 << 0
} GhStoreKeyLookupFlags;

typedef struct {
  /* Copies the account's key into @key (GH_STORE_KEY_SIZE bytes of locked
   * memory owned by GhStore) and returns the item's store id (a UUID). */
  gboolean (*lookup)(const gchar *account_pubkey, GhStoreKeyLookupFlags flags,
                     guint8 *key, gchar **store_id, gpointer user_data,
                     GCancellable *cancellable, GError **error);
  /* Durably stores a new item; returns only after the key store confirmed. */
  gboolean (*store)(const gchar *account_pubkey, const gchar *store_id,
                    const guint8 *key, gpointer user_data,
                    GCancellable *cancellable, GError **error);
  /* Deletes every item for the account. A missing item is success. */
  gboolean (*destroy)(const gchar *account_pubkey, gpointer user_data,
                      GCancellable *cancellable, GError **error);
} GhStoreKeyProvider;

/* ---- Layout ------------------------------------------------------------------ */

/* hex(SHA-256("groundhog/v1/account-dir" || pubkey))[0:32], where pubkey is
 * the 32-byte x-only key decoded from @account_pubkey (64 lowercase hex).
 * Pseudonymous: hides the pubkey from a casual listing only. */
gchar *gh_store_account_dir_name(const gchar *account_pubkey);
/* <realpath(data_dir)>/groundhog/accounts/<name>; data_dir must exist. NULL
 * data_dir means g_get_user_data_dir(). */
gchar *gh_store_account_dir_path(const gchar *data_dir, const gchar *account_pubkey,
                                 GError **error);

/* ---- Single-SQLite guard (ST-4) -------------------------------------------------
 * Verifies, once per process, that every sqlite3_* entry point this code uses
 * resolves into one loaded object, that the object is SQLCipher (exports
 * sqlite3_key and reports a cipher_version), that it is thread-safe and not
 * built with SQLITE_TEMP_STORE=0. On ELF the entry points are resolved through
 * the global scope (dlsym(RTLD_DEFAULT)), the same lookup that binds every
 * other library's sqlite3_* references, so a plain libsqlite3 that precedes
 * SQLCipher in the link order (or is preloaded) fails the check. On Mach-O,
 * where each image binds to the library it linked (two-level namespace), the
 * guard checks this code's own bindings and refuses a forced flat namespace.
 * Every open runs it first and fails with GH_STORE_ERROR_NO_CIPHER. */
gboolean gh_store_check_sqlite(GError **error);
/* "SQLCipher <cipher_version> (SQLite <version>) from <object path>". */
gchar *gh_store_describe_sqlite(void);

/* ---- Open, close, forget --------------------------------------------------------- */

typedef enum {
  GH_STORE_OPEN_NONE = 0,
  /* Create the store, and its key first, when neither exists (§3.4 first open:
   * the key item is stored and confirmed before any file is created). */
  GH_STORE_OPEN_CREATE = 1 << 0,
  /* A window is shown: the key lookup may prompt to unlock the keyring. */
  GH_STORE_OPEN_INTERACTIVE = 1 << 1,
  /* STORE_CORRUPT, after an open failed with GH_STORE_ERROR_CORRUPT: skip
   * the integrity scan and open read-only (every write fails with
   * GH_STORE_ERROR_CORRUPT) so what is still readable can be shown before
   * "Reset storage". Version and identity (FOREIGN) are still checked. A
   * read that reaches a damaged page fails, and SQLCipher then fails every
   * later read on that handle too. */
  GH_STORE_OPEN_ALLOW_CORRUPT = 1 << 2
} GhStoreOpenFlags;

typedef struct {
  const gchar *data_dir;                  /* absolute; NULL = g_get_user_data_dir() */
  const gchar *account_pubkey;            /* 64 lowercase hex */
  /* Copied. Required by gh_store_open() and gh_store_forget_account();
   * optional for gh_store_open_with_key(). */
  const GhStoreKeyProvider *key_provider;
  gpointer key_provider_data;             /* must outlive the store */
  GhClock *clock;                         /* NULL = system clock; ref'd */
} GhStoreConfig;

typedef struct _GhStore GhStore;

/* Opens (or with GH_STORE_OPEN_CREATE creates) the account's store. Errors:
 * NO_CIPHER, INVALID, PERMISSIONS, KEY_MISSING (store present, key gone:
 * nothing touched), LOCKED, UNAVAILABLE, NOT_FOUND (nothing to open and no
 * CREATE), KEY, FOREIGN, NEWER_SCHEMA, CORRUPT, BUSY (already open in this
 * process), G_IO_ERROR_CANCELLED. A store interrupted during creation is
 * completed on the next open. */
GhStore *gh_store_open(const GhStoreConfig *config, GhStoreOpenFlags flags,
                       GCancellable *cancellable, GError **error);
/* Whether the account's store exists on disk, after the same safety checks
 * as an open (an unsafe layout is GH_STORE_ERROR_PERMISSIONS). Nothing is
 * created. */
gboolean gh_store_exists(const gchar *data_dir, const gchar *account_pubkey,
                         gboolean *out_exists, GError **error);
/* Opens, or with GH_STORE_OPEN_CREATE creates, the store with a key the
 * caller already holds (e.g. gh_store_key_lookup_finish()), without calling a
 * provider. @key is GH_STORE_KEY_SIZE raw bytes; it is only read, so
 * read-only guarded memory is fine. @store_id is the key item's store id:
 * written into a new store, compared with an existing one (FOREIGN).
 * GH_STORE_OPEN_INTERACTIVE is ignored. Errors as gh_store_open(). */
GhStore *gh_store_open_with_key(const GhStoreConfig *config, GBytes *key,
                                const gchar *store_id, GhStoreOpenFlags flags,
                                GError **error);
/* In-memory store with the same schema and API for "Continue Without Saving
 * Messages" (KC-4): no file, no key, nothing survives gh_store_close(). */
GhStore *gh_store_open_ephemeral(const gchar *account_pubkey, GhClock *clock,
                                 GError **error);
/* Rolls back an open transaction (a bug; logged), truncates the WAL, closes. */
void gh_store_close(GhStore *store);
/* Forget account at the store level (§3.8 steps 2-3): closes @store, destroys
 * the key item through the provider, then unlinks the account directory (never
 * following symlinks). The unlink runs even if key destruction fails, and that
 * failure is then returned. Without a provider (gh_store_open_with_key()) it
 * only closes and unlinks: destroy the key item first. Consumes @store. */
gboolean gh_store_forget(GhStore *store, GCancellable *cancellable, GError **error);
/* The same for a store that is not open, e.g. "Start Fresh" after
 * KEY_MISSING. Fails with BUSY if the store is open in this process. */
gboolean gh_store_forget_account(const GhStoreConfig *config,
                                 GCancellable *cancellable, GError **error);
/* Only the unlink half, for callers that destroyed the key item themselves
 * (gh_store_key_destroy_async()). BUSY if the store is open in this process;
 * a missing directory is success. */
gboolean gh_store_delete_files(const gchar *data_dir, const gchar *account_pubkey,
                               GError **error);

const gchar *gh_store_get_account_pubkey(GhStore *store);
const gchar *gh_store_get_store_id(GhStore *store);
/* Account directory and store.db path; NULL for an ephemeral store. */
const gchar *gh_store_get_dir(GhStore *store);
const gchar *gh_store_get_path(GhStore *store);
const gchar *gh_store_get_cipher_version(GhStore *store);
GhClock *gh_store_get_clock(GhStore *store);
gboolean gh_store_is_read_only(GhStore *store);
gboolean gh_store_is_ephemeral(GhStore *store);

/* Raw connection for store-layer modules (G05 conversations, G23 MarmotStorage)
 * that keep their SQL beside the schema. Use it only on the store's thread and
 * never close it, change its key, pragmas or limits, or ATTACH. Map SQLite
 * failures with gh_store_set_sqlite_error() so disk-full stays
 * GH_STORE_ERROR_FULL. */
struct sqlite3 *gh_store_get_db(GhStore *store);
/* Sets @error from SQLite result code @rc (e.g. SQLITE_FULL -> FULL) and the
 * connection's message, prefixed by @what. Always returns FALSE. */
gboolean gh_store_set_sqlite_error(GhStore *store, gint rc, const gchar *what,
                                   GError **error);
/* Runs SQL without result rows (e.g. from a store-layer module). */
gboolean gh_store_exec(GhStore *store, const gchar *sql, GError **error);

/* ---- Transactions (§3.5) ---------------------------------------------------------
 * The outermost level is BEGIN IMMEDIATE (the write lock is taken up front);
 * nested levels are savepoints. A failed commit rolls back and returns the
 * error (e.g. GH_STORE_ERROR_FULL). If SQLite aborts the whole transaction
 * (disk full, I/O error), every later statement in it fails until the
 * outermost level is rolled back, so no partial work is ever committed. The
 * T-* operations below run inside a caller's transaction as a savepoint, so
 * e.g. MLS state and outbox events (T-mls) can commit together. */
gboolean gh_store_begin(GhStore *store, GError **error);
gboolean gh_store_commit(GhStore *store, GError **error);
void gh_store_rollback(GhStore *store);
guint gh_store_get_transaction_depth(GhStore *store);
typedef gboolean (*GhStoreTransactionFunc)(GhStore *store, gpointer user_data,
                                           GError **error);
/* begin, func, commit; rolls back if func or the commit fails. */
gboolean gh_store_transaction(GhStore *store, GhStoreTransactionFunc func,
                              gpointer user_data, GError **error);

/* ---- Integer encodings of the schema (§3.3) ---------------------------------------- */

typedef enum {
  GH_STORE_BACKEND_NIP17 = 1,
  GH_STORE_BACKEND_NIP29 = 2,
  GH_STORE_BACKEND_MLS = 3
} GhStoreBackend;

typedef enum {
  GH_STORE_SEEN_WRAP = 1,         /* NIP-17 gift-wrap id */
  GH_STORE_SEEN_RUMOR = 2,        /* NIP-17 rumor id */
  GH_STORE_SEEN_NIP29_EVENT = 3,
  GH_STORE_SEEN_MLS_MESSAGE = 4,
  /* NIP-17 gift-wrap id finally rejected after a signer call: skipped before
   * any signer call, never a seen message (G05, legacy .seen "x" lines). */
  GH_STORE_SEEN_REJECTED_WRAP = 5
} GhStoreSeenNs;

typedef enum {
  GH_STORE_DIRECTION_IN = 0,
  GH_STORE_DIRECTION_OUT = 1
} GhStoreDirection;

typedef enum {
  GH_STORE_REQUEST_ACCEPTED = 0,
  GH_STORE_REQUEST_PENDING = 1,   /* Message Requests (PD-8) */
  GH_STORE_REQUEST_BLOCKED = 2
} GhStoreRequestState;

/* §3.6. The transitions after SEALED belong to the outbox engine (G06). */
typedef enum {
  GH_STORE_OUTBOX_QUEUED = 0,
  GH_STORE_OUTBOX_SEALING = 1,
  GH_STORE_OUTBOX_SEALED = 2,
  GH_STORE_OUTBOX_PUBLISHING = 3,
  GH_STORE_OUTBOX_WAITING_RETRY = 4,
  GH_STORE_OUTBOX_SETTLED = 5,
  GH_STORE_OUTBOX_NEEDS_ATTENTION = 6,
  GH_STORE_OUTBOX_CANCELLED = 7
} GhStoreOutboxState;

typedef enum {
  GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP = 0,
  GH_STORE_OUTBOX_ROLE_SELF_WRAP = 1,
  GH_STORE_OUTBOX_ROLE_NIP29_EVENT = 2,
  GH_STORE_OUTBOX_ROLE_MLS_MESSAGE = 3,  /* kind 445 */
  GH_STORE_OUTBOX_ROLE_WELCOME_WRAP = 4
} GhStoreOutboxRole;

/* ---- Conversations ------------------------------------------------------------------ */

/* backend_key (§3.3): NIP-17 the sorted lowercase hex participant set incl.
 * self, ','-joined; NIP-29 the normalized relay URL, 0x1f, group id; MLS the
 * hex MLS group id (never the routing h). */
gboolean gh_store_ensure_conversation(GhStore *store, GhStoreBackend backend,
                                      const gchar *backend_key,
                                      GhStoreRequestState request_state_if_new,
                                      gint64 *out_conversation_id, GError **error);
/* NOT_FOUND if absent. */
gboolean gh_store_find_conversation(GhStore *store, GhStoreBackend backend,
                                    const gchar *backend_key,
                                    gint64 *out_conversation_id, GError **error);
/* NULL or "" clears the draft. */
gboolean gh_store_set_draft(GhStore *store, gint64 conversation_id,
                            const gchar *draft, GError **error);
gboolean gh_store_get_draft(GhStore *store, gint64 conversation_id,
                            gchar **out_draft, GError **error);
/* §3.8 forget conversation (ST-9): deletes its outbox rows (cancelling
 * unsettled sends), messages and participants, clears title, draft, unread,
 * read and pin state, and sets forgotten_before = now so relay backfill of
 * older messages cannot resurrect it; a newer message starts it fresh. The
 * row stays as that tombstone and keeps request_state (blocks survive),
 * muted_until and disappearing_s. Seen keys are kept. The WAL is truncated
 * afterwards. Backend state (nip29_groups, mls_groups) is the caller's. */
gboolean gh_store_forget_conversation(GhStore *store, gint64 conversation_id,
                                      GError **error);

/* Disappearing messages (charter §3.7, G07): conversations.disappearing_s,
 * the timer (seconds, 0 = off) given to the account's own messages in the
 * conversation. Bounded by GH_STORE_MAX_DISAPPEARING; which values a user
 * may pick is gh-expiry.h's policy. NOT_FOUND if the conversation is absent. */
gboolean gh_store_get_disappearing(GhStore *store, gint64 conversation_id,
                                   gint64 *out_seconds, GError **error);
gboolean gh_store_set_disappearing(GhStore *store, gint64 conversation_id,
                                   gint64 seconds, GError **error);
/* The timer every conversation created from now on starts with, however it
 * is created (sent to, received from, a draft); 0 (the default) is off.
 * Kept in memory only: the owner sets it again after each open. */
void gh_store_set_default_disappearing(GhStore *store, gint64 seconds);

/* Called on the store's thread right after T-admit or T-enqueue stored a
 * message that expires (at @expires_at, unix seconds), so a purge scheduler
 * can move its next wake-up earlier. Inside a caller's transaction it runs
 * before that commits (a rollback then only costs an early wake-up). The
 * callback must not use the store. */
typedef void (*GhStoreExpiryFunc)(gint64 expires_at, gpointer user_data);
/* Replaces the callback (NULL removes it); @destroy releases @user_data when
 * it is replaced or the store closes. */
void gh_store_set_expiry_notify(GhStore *store, GhStoreExpiryFunc func,
                                gpointer user_data, GDestroyNotify destroy);

/* ---- Cursors (§3.3 `cursors`) -------------------------------------------------------
 * Sync checkpoints kept inside the encrypted store, e.g. the NIP-17 inbox's
 * "everything before this time was received" mark (G04), so no plaintext
 * state file names the account. @scope is a caller-chosen name (1 to
 * GH_STORE_MAX_CURSOR_SCOPE bytes); @relay_url is a relay URL or "" for a
 * scope-wide cursor. */
#define GH_STORE_MAX_CURSOR_SCOPE 64

/* *out_since is the stored value, or 0 when there is none. */
gboolean gh_store_get_cursor(GhStore *store, const gchar *scope, const gchar *relay_url,
                             gint64 *out_since, GError **error);
/* Inserts or replaces the cursor; @since must be >= 0 (0 deletes it). */
gboolean gh_store_set_cursor(GhStore *store, const gchar *scope, const gchar *relay_url,
                             gint64 since, GError **error);

/* ---- Seen set ----------------------------------------------------------------------- */

/* The wrap pre-check before any signer prompt (T-admit). */
gboolean gh_store_seen_contains(GhStore *store, GhStoreSeenNs ns, const gchar *id,
                                gboolean *out_seen, GError **error);
/* Records an id with no message (e.g. legacy .seen import, ST-12). */
gboolean gh_store_seen_add(GhStore *store, GhStoreSeenNs ns, const gchar *id,
                           GError **error);

/* ---- T-admit ------------------------------------------------------------------------ */

typedef struct {
  GhStoreBackend backend;
  const gchar *backend_key;
  const gchar *backend_msg_id;       /* NIP-17/29: 64 lowercase hex; MLS: bounded hex */
  const gchar *wrap_id;              /* NIP-17: verified wrap id, or NULL */
  const gchar *sender_pubkey;        /* 64 lowercase hex */
  gint kind;
  gint64 created_at;                 /* sender-claimed time */
  gint64 received_at;                /* 0 = now */
  GhStoreDirection direction;        /* OUT for our own self-copies */
  const gchar *body;                 /* may be NULL */
  const gchar *raw_json;             /* canonical rumor/event */
  const gchar *reply_to;             /* lowercase hex or NULL */
  gint64 expires_at;                 /* NIP-40; 0 = none */
  const gchar *title;                /* NULL keeps the current title */
  const gchar *const *participants;  /* NULL-terminated pubkeys, or NULL */
  gboolean unread;                   /* counts towards unread_count (IN only) */
  GhStoreRequestState request_state; /* only when this creates the conversation */
} GhStoreMessage;

typedef enum {
  GH_STORE_ADMIT_STORED,     /* new message stored, seen keys recorded */
  GH_STORE_ADMIT_DUPLICATE,  /* message id already seen; new wrap id recorded */
  GH_STORE_ADMIT_FORGOTTEN,  /* older than the conversation's forgotten_before */
  GH_STORE_ADMIT_EXPIRED     /* expired on arrival (EX-4) */
} GhStoreAdmitResult;

/* T-admit: records the wrap id and message id in `seen`, and stores the
 * message (creating or updating its conversation: last activity, unread count,
 * title, participants) in one transaction, so dedup and storage can never
 * disagree. A message whose id is already seen is never stored again, so
 * purged or forgotten messages do not return from backfill (EX-6); FORGOTTEN
 * and EXPIRED messages are seen-recorded only. On failure (e.g.
 * GH_STORE_ERROR_FULL) nothing is recorded and the wrap is re-fetched later.
 * @out_message_id is set for STORED only (otherwise 0). */
gboolean gh_store_admit(GhStore *store, const GhStoreMessage *message,
                        GhStoreAdmitResult *out_result, gint64 *out_message_id,
                        GError **error);

/* ---- T-enqueue ---------------------------------------------------------------------- */

typedef struct {
  gint64 conversation_id;
  const gchar *op_id;          /* 32 lowercase hex (128-bit idempotency key) */
  const gchar *backend_msg_id; /* id of the unsigned rumor / event */
  const gchar *sender_pubkey;  /* the account */
  gint kind;
  gint64 created_at;           /* the real send time (rumor created_at) */
  const gchar *body;
  const gchar *rumor_json;     /* canonical unsigned rumor/event */
  const gchar *reply_to;
  gint64 expires_at;           /* 0 = none */
} GhStoreOutgoing;

/* 128 random bits as 32 lowercase hex characters. */
gchar *gh_store_new_op_id(void);

/* T-enqueue, before the first signer call: the outgoing message row
 * (direction OUT), its QUEUED outbox row, the message id in `seen` (so the
 * self-copy coming back is a duplicate) and the cleared draft, in one
 * transaction. On failure the draft is kept. Idempotent on op_id: a repeat
 * returns the existing ids and changes nothing. */
gboolean gh_store_enqueue(GhStore *store, const GhStoreOutgoing *outgoing,
                          gint64 *out_outbox_id, gint64 *out_message_id,
                          GError **error);

/* ---- T-seal ------------------------------------------------------------------------- */

typedef struct {
  GhStoreOutboxRole role;
  const gchar *target_pubkey;       /* recipient of a wrap, else NULL */
  const gchar *event_id;            /* 64 lowercase hex */
  const gchar *event_json;          /* signed; republished byte-identically */
  gint64 not_before;                /* unix seconds; 0 = immediately (D8) */
  const gchar *const *relay_urls;   /* NULL-terminated target snapshot */
} GhStoreSealedEvent;

/* T-seal: every signed event, its target URL snapshot and state SEALED, in
 * one transaction: either all events of the message are durable or none.
 * Only a QUEUED or SEALING outbox can be sealed; anything later fails with
 * GH_STORE_ERROR_STATE, because stored events are republished and never
 * re-sealed. */
gboolean gh_store_seal(GhStore *store, gint64 outbox_id,
                       const GhStoreSealedEvent *events, gsize n_events,
                       GError **error);

/* ---- T-outcome ---------------------------------------------------------------------- */

typedef struct {
  const gchar *relay_url;
  gint outcome;              /* a GhRelayPublishOutcome value */
  gint ok_prefix;            /* a GhRelayOkPrefix value, or -1 for none */
  const gchar *ok_message;   /* relay text or NULL; cut to GH_STORE_MAX_OK_MESSAGE */
  gboolean count_attempt;    /* FALSE for CANCELLED (switch/quit) */
} GhStoreTargetOutcome;

/* T-outcome: records one target's latest outcome and attempt; a URL that is
 * not yet a target of the event (10050 changed after sealing) is added. */
gboolean gh_store_record_outcome(GhStore *store, gint64 outbox_event_id,
                                 const GhStoreTargetOutcome *outcome,
                                 GError **error);

/* ---- Outbox engine (G06) ---------------------------------------------------------------
 * What the durable outbox engine (src/app/gh-outbox.c) resumes from after a
 * restart, and the §3.6 transitions it records after SEALED. Each write is one
 * small transaction (cut points "outbox:*"). */

typedef struct {
  gchar *relay_url;
  gint outcome;              /* a GhRelayPublishOutcome value; 0 = not reported yet */
  gint ok_prefix;            /* a GhRelayOkPrefix value, or -1 for none */
  gchar *ok_message;
  guint attempts;            /* counted attempts (T-outcome count_attempt) */
  gint64 last_attempt_at;    /* unix seconds; 0 = never */
} GhStoreOutboxTarget;

typedef struct {
  gint64 id;                 /* outbox_events.id: the T-outcome key */
  GhStoreOutboxRole role;
  gchar *target_pubkey;      /* recipient of a wrap, else NULL */
  gchar *event_id;
  gchar *event_json;         /* signed, byte-identical to T-seal; NULL once pruned */
  gint64 not_before;         /* unix seconds; 0 = immediately (D8) */
  GPtrArray *targets;        /* GhStoreOutboxTarget, ordered by URL */
} GhStoreOutboxEvent;

typedef struct {
  gint64 id;
  gint64 conversation_id;
  gint64 message_id;         /* the outgoing message row; 0 if it is gone */
  gchar *op_id;
  GhStoreBackend backend;
  GhStoreOutboxState state;
  gchar *rumor_json;
  gint64 created_at;         /* unix seconds at T-enqueue (store clock) */
  gint64 next_attempt_at;    /* 0 = none */
  guint attempts;            /* rounds counted by gh_store_outbox_update() */
  gchar *last_error;         /* the engine's reason code, or NULL */
  GPtrArray *events;         /* GhStoreOutboxEvent in sealing order; empty until sealed */
} GhStoreOutboxEntry;

void gh_store_outbox_entry_free(GhStoreOutboxEntry *entry);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhStoreOutboxEntry, gh_store_outbox_entry_free)

/* The ids (gint64) of @backend's entries that are neither SETTLED nor
 * CANCELLED, oldest first. Each backend's engine resumes only its own. */
GArray *gh_store_outbox_list_unfinished(GhStore *store, GhStoreBackend backend,
                                        GError **error);
/* One entry with its events and their targets; NOT_FOUND if absent. */
GhStoreOutboxEntry *gh_store_outbox_load(GhStore *store, gint64 outbox_id, GError **error);
/* The entry an outgoing message was queued with; NOT_FOUND if none. */
gboolean gh_store_outbox_find_by_message(GhStore *store, gint64 message_id,
                                         gint64 *out_outbox_id, GError **error);
/* The entry the outgoing message @backend_msg_id (the rumor id) of
 * @conversation_id was queued with; NOT_FOUND if none (e.g. a self-copy of a
 * message sent from another device). */
gboolean gh_store_outbox_find_by_rumor(GhStore *store, gint64 conversation_id,
                                       const gchar *backend_msg_id, gint64 *out_outbox_id,
                                       GError **error);

typedef struct {
  GhStoreOutboxState state;
  gint64 next_attempt_at;    /* unix seconds; 0 = none */
  gboolean count_attempt;    /* attempts + 1 (one publish or seal round) */
  const gchar *last_error;   /* reason code; NULL clears; at most GH_STORE_MAX_OK_MESSAGE */
} GhStoreOutboxUpdate;

/* Records one transition of the outbox engine. Refused with STATE: leaving
 * CANCELLED; SEALED (only gh_store_seal() seals); QUEUED or SEALING once the
 * entry has stored events (they are republished, never re-sealed); and
 * PUBLISHING, WAITING_RETRY or SETTLED while it has none. NOT_FOUND if
 * absent. */
gboolean gh_store_outbox_update(GhStore *store, gint64 outbox_id,
                                const GhStoreOutboxUpdate *update, GError **error);
/* The user deleted a message before it settled: deletes the entry (its events
 * and targets cascade) and its outgoing message in one transaction, then
 * truncates the WAL. The seen keys stay, so a self-copy that comes back from
 * a relay is not admitted again. NOT_FOUND if absent. */
gboolean gh_store_outbox_delete(GhStore *store, gint64 outbox_id, GError **error);

/* ---- T-purge ------------------------------------------------------------------------ */

typedef struct {
  guint n_expired;           /* messages whose expires_at passed */
  guint n_retention;         /* messages received before the cutoff */
  guint n_outbox;            /* their outbox entries (rumor and signed events) */
  gint64 next_expires_at;    /* earliest remaining expires_at; 0 = none */
  gboolean checkpointed;     /* the WAL was truncated */
  gboolean checkpoint_deferred; /* rate-limited: purge again within a minute */
} GhStorePurgeStats;

/* T-purge (§3.7): deletes messages with expires_at <= now and, if
 * retention_cutoff > 0, those received before it, together with the outbox
 * entries of outgoing ones (their rumor text and signed events), in one
 * transaction (seen keys are kept so they cannot return). The read state
 * stays exact: a read marker on a deleted message moves back to the newest
 * remaining one at or before it (or none), and each touched conversation's
 * unread count becomes its remaining messages from others after the marker.
 * Then the WAL is truncated at most once a minute (GhClock) so expired
 * content leaves it too. Inside a caller's transaction the truncation is
 * reported as deferred. */
gboolean gh_store_purge(GhStore *store, gint64 retention_cutoff,
                        GhStorePurgeStats *out_stats, GError **error);

/* One message a purge deleted. */
typedef struct {
  GhStoreBackend backend;
  gchar *backend_key;        /* its conversation */
  gchar *backend_msg_id;     /* NIP-17 rumor id, NIP-29 event id, MLS message id */
} GhStorePurgedMessage;

void gh_store_purged_message_free(GhStorePurgedMessage *message);
/* gh_store_purge() that also lists what it deleted: *out_purged (nullable)
 * receives a GPtrArray of GhStorePurgedMessage, possibly empty. */
gboolean gh_store_purge_full(GhStore *store, gint64 retention_cutoff,
                             GPtrArray **out_purged, GhStorePurgeStats *out_stats,
                             GError **error);

/* ---- Maintenance --------------------------------------------------------------------- */

/* PRAGMA integrity_check (full) or quick_check; CORRUPT with the first
 * problem otherwise. */
gboolean gh_store_check_integrity(GhStore *store, gboolean full, GError **error);
/* wal_checkpoint(TRUNCATE); BUSY if a reader blocked it. */
gboolean gh_store_checkpoint(GhStore *store, GError **error);

/* ---- Schema (gh-store-schema.c) --------------------------------------------------------
 * Ordered, append-only migrations; each runs in one transaction that also
 * records it in schema_migrations and sets PRAGMA user_version. A store with
 * a higher user_version is refused (NEWER_SCHEMA). */
#define GH_STORE_SCHEMA_VERSION 3

typedef struct {
  gint version;
  const gchar *description;
  const gchar *sql;
} GhStoreMigration;

const GhStoreMigration *gh_store_schema_get_migrations(gsize *n_migrations);

#ifdef GH_STORE_TEST_HOOKS
/* ---- Test builds only (charter H8) ------------------------------------------------------
 * Named cut points inside every T-* transaction, store creation and commits.
 * When the armed cut point is reached for the @nth time (1-based; 0 means 1),
 * the process SIGKILLs itself: no unwinding, no close, like a crash. NULL
 * disarms. Unknown names abort, so a typo cannot pass silently. */
void gh_store_test_crash_at(const gchar *cut_point, guint nth);
/* NULL-terminated list of every cut point, optionally filtered by prefix. */
GStrv gh_store_test_list_cut_points(const gchar *prefix);
/* Pretend the process runs as @uid for ownership checks (-1 restores). */
void gh_store_test_set_uid(gint64 uid);
#endif

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhStore, gh_store_close)

G_END_DECLS
#endif
