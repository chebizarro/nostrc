#ifndef GH_STORE_KEY_H
#define GH_STORE_KEY_H

/* GhStoreKey: custody of the per-account store key (charter §3.4, D2).
 *
 * Each account's encrypted store is keyed with 32 uniformly random bytes held
 * in one Groundhog-owned Secret Service item. Groundhog never derives the key
 * from, or wraps it with, the account's signer (v1 has no recovery blob).
 *
 * Guarantees:
 * - Key material only ever lives in libsodium guarded memory (sodium_malloc:
 *   mlocked where the rlimit allows, guard pages, read-only once filled) and
 *   is wiped when the last GBytes reference is dropped. This module writes
 *   nothing to disk.
 * - Fail closed: a missing, locked or unreachable Secret Service is reported
 *   as an explicit GH_STORE_KEY_ERROR; there is no plaintext or file fallback.
 * - GH_STORE_KEY_FLAGS_NONE never shows an unlock (or keyring-creation)
 *   prompt, so it is safe for background and windowless use.
 * - Operations on one account run strictly in call order within one
 *   GhStoreKey, so a lookup-or-create cannot race a destroy or a second
 *   create. Use one instance per process (the app's service container owns
 *   it); separate instances or processes can still both create an item,
 *   which later lookups report as GH_STORE_KEY_ERROR_INVALID.
 * - The item label and error messages never contain the account pubkey.
 *
 * Limitation: NOT_FOUND trusts the service to list items of locked keyrings
 * (gnome-keyring does) or to lock the default keyring; a service that hides
 * a locked non-default keyring can make a present key look missing, so a
 * destructive "start fresh" must stay behind user confirmation.
 *
 * Use from a single thread (the thread-default GMainContext it was created
 * on). */

#include <gio/gio.h>

G_BEGIN_DECLS

#define GH_STORE_KEY_SCHEMA_NAME   "org.nostr.Groundhog.StoreKey"
#define GH_STORE_KEY_ATTR_ACCOUNT  "account"   /* lowercase 64-hex x-only pubkey */
#define GH_STORE_KEY_ATTR_STORE_ID "store-id"  /* RFC 4122 UUID, = meta.store_id */
#define GH_STORE_KEY_ATTR_VERSION  "version"   /* key format; this build writes "1" */
#define GH_STORE_KEY_VERSION       "1"
#define GH_STORE_KEY_LABEL         "Groundhog message storage key"
#define GH_STORE_KEY_SIZE          32

#define GH_STORE_KEY_ERROR (gh_store_key_error_quark())
GQuark gh_store_key_error_quark(void);

/* Each code maps onto one store state (charter §3.4). Invalid arguments are
 * G_IO_ERROR_INVALID_ARGUMENT; cancellation is G_IO_ERROR_CANCELLED. */
typedef enum {
  /* No Secret Service on the session bus (or no bus): STORE_UNAVAILABLE. */
  GH_STORE_KEY_ERROR_UNAVAILABLE,
  /* The item or the default keyring is locked and was not unlocked (always
   * the case without GH_STORE_KEY_FLAGS_INTERACTIVE), or a background store
   * found no default keyring to create the item in: STORE_LOCKED. */
  GH_STORE_KEY_ERROR_LOCKED,
  /* No item for this account. From gh_store_key_lookup_async() while a store
   * exists on disk this is STORE_KEY_MISSING. */
  GH_STORE_KEY_ERROR_NOT_FOUND,
  /* An item exists but cannot be used: wrong secret size, missing or
   * malformed store-id or version, a foreign account, or several items
   * (none of them newer than this build). */
  GH_STORE_KEY_ERROR_INVALID,
  /* An item of the account was written by a newer Groundhog (version > 1),
   * alone or beside others. Never offer to destroy it from this build. */
  GH_STORE_KEY_ERROR_NEWER_VERSION,
  /* The Secret Service refused or failed the operation. */
  GH_STORE_KEY_ERROR_FAILED,
} GhStoreKeyError;

typedef enum {
  GH_STORE_KEY_FLAGS_NONE = 0,
  /* A user is present: the Secret Service may show its unlock prompt (and,
   * when storing, its keyring-creation prompt). */
  GH_STORE_KEY_FLAGS_INTERACTIVE = 1 << 0,
} GhStoreKeyFlags;

/* A read-only GBytes whose bytes live in sodium_malloc memory and are wiped
 * (sodium_free) when the last reference is dropped. Never copy its contents
 * into ordinary memory. */
GBytes *gh_store_key_secret_new(gconstpointer data, gsize size);

/* The SQLCipher raw-key literal x'<64 lowercase hex>' for a
 * GH_STORE_KEY_SIZE key, in the same guarded memory. The GBytes size is the
 * literal length (67); the byte after it is NUL, so the data is also a C
 * string. Use it for PRAGMA key / sqlite3_key(), then drop the reference.
 * Returns NULL if key is not GH_STORE_KEY_SIZE bytes. */
GBytes *gh_store_key_dup_sqlcipher_key(GBytes *key);

/* ---- Secret backend seam (charter §9.1 H5) --------------------------------
 *
 * GhStoreKey applies all policy (validation, per-account ordering, error
 * states); a backend only moves items. The production backend talks to the
 * Secret Service through libsecret; tests use an in-process fake. */

typedef struct {
  GHashTable *attributes; /* owned: string -> string */
  gboolean locked;
  /* owned, from gh_store_key_secret_new(); NULL when locked. A secret that
   * is not GH_STORE_KEY_SIZE bytes may be reported as empty. */
  GBytes *secret;
} GhStoreKeyItem;

/* Takes ownership of attributes and secret. */
GhStoreKeyItem *gh_store_key_item_new(GHashTable *attributes, gboolean locked, GBytes *secret);
void gh_store_key_item_free(GhStoreKeyItem *item);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhStoreKeyItem, gh_store_key_item_free)

#define GH_TYPE_STORE_KEY_BACKEND (gh_store_key_backend_get_type())
G_DECLARE_INTERFACE(GhStoreKeyBackend, gh_store_key_backend, GH, STORE_KEY_BACKEND, GObject)

/* Every method works only on items of the GH_STORE_KEY_SCHEMA_NAME schema.
 * Without GH_STORE_KEY_FLAGS_INTERACTIVE no method may prompt. */
struct _GhStoreKeyBackendInterface {
  GTypeInterface parent_iface;

  /* Items whose attributes include every pair in attributes, with secrets
   * loaded for unlocked items. An empty result while the default keyring is
   * locked (and stays locked) is GH_STORE_KEY_ERROR_LOCKED, never "none":
   * a service may hide a locked keyring's items. */
  void (*search_async)(GhStoreKeyBackend *self, GHashTable *attributes,
                       GhStoreKeyFlags flags, GCancellable *cancellable,
                       GAsyncReadyCallback callback, gpointer user_data);
  /* Returns a GPtrArray of GhStoreKeyItem (with a free func). */
  GPtrArray *(*search_finish)(GhStoreKeyBackend *self, GAsyncResult *result,
                              GError **error);

  /* Create an item in the default keyring. GhStoreKey always passes a fresh
   * store-id, so no existing item can be replaced. */
  void (*store_async)(GhStoreKeyBackend *self, GHashTable *attributes,
                      const gchar *label, GBytes *secret, GhStoreKeyFlags flags,
                      GCancellable *cancellable, GAsyncReadyCallback callback,
                      gpointer user_data);
  gboolean (*store_finish)(GhStoreKeyBackend *self, GAsyncResult *result,
                           GError **error);

  /* Delete every matching item. Succeeds when none remain (including when
   * none existed); GH_STORE_KEY_ERROR_LOCKED when a match could not be
   * deleted because it, or the default keyring, is locked. */
  void (*clear_async)(GhStoreKeyBackend *self, GHashTable *attributes,
                      GhStoreKeyFlags flags, GCancellable *cancellable,
                      GAsyncReadyCallback callback, gpointer user_data);
  gboolean (*clear_finish)(GhStoreKeyBackend *self, GAsyncResult *result,
                           GError **error);
};

void gh_store_key_backend_search_async(GhStoreKeyBackend *self, GHashTable *attributes,
                                       GhStoreKeyFlags flags, GCancellable *cancellable,
                                       GAsyncReadyCallback callback, gpointer user_data);
GPtrArray *gh_store_key_backend_search_finish(GhStoreKeyBackend *self, GAsyncResult *result,
                                              GError **error);
void gh_store_key_backend_store_async(GhStoreKeyBackend *self, GHashTable *attributes,
                                      const gchar *label, GBytes *secret,
                                      GhStoreKeyFlags flags, GCancellable *cancellable,
                                      GAsyncReadyCallback callback, gpointer user_data);
gboolean gh_store_key_backend_store_finish(GhStoreKeyBackend *self, GAsyncResult *result,
                                           GError **error);
void gh_store_key_backend_clear_async(GhStoreKeyBackend *self, GHashTable *attributes,
                                      GhStoreKeyFlags flags, GCancellable *cancellable,
                                      GAsyncReadyCallback callback, gpointer user_data);
gboolean gh_store_key_backend_clear_finish(GhStoreKeyBackend *self, GAsyncResult *result,
                                           GError **error);

/* ---- GhStoreKey ------------------------------------------------------------ */

#define GH_TYPE_STORE_KEY (gh_store_key_get_type())
G_DECLARE_FINAL_TYPE(GhStoreKey, gh_store_key, GH, STORE_KEY, GObject)

/* backend NULL selects the session Secret Service (libsecret). */
GhStoreKey *gh_store_key_new(GhStoreKeyBackend *backend);

/* account_pubkey_hex is a 64-character hex x-only pubkey (any case; it is
 * stored lowercase), so each account has its own item and key. */

/* The existing key, or GH_STORE_KEY_ERROR_NOT_FOUND. Never creates an item:
 * use this whenever the account's store already exists on disk. */
void gh_store_key_lookup_async(GhStoreKey *self, const gchar *account_pubkey_hex,
                               GhStoreKeyFlags flags, GCancellable *cancellable,
                               GAsyncReadyCallback callback, gpointer user_data);
/* Returns the GH_STORE_KEY_SIZE key (see gh_store_key_secret_new) and the
 * item's store-id in *out_store_id (optional, free with g_free), or NULL. */
GBytes *gh_store_key_lookup_finish(GhStoreKey *self, GAsyncResult *result,
                                   gchar **out_store_id, GError **error);

/* First open: the existing key, or else a new random key stored under a new
 * store-id. The key is returned only after the Secret Service confirmed the
 * item, so a failed store leaves nothing to create a database with (once
 * stored, it is returned even if cancellable fired meanwhile). Call it only
 * when no store exists on disk yet; otherwise a lost item would be silently
 * replaced by a key that cannot open the store.
 *
 * Cancelling an operation that is still queued behind another one for the
 * same account completes it at once with G_IO_ERROR_CANCELLED. */
void gh_store_key_lookup_or_create_async(GhStoreKey *self, const gchar *account_pubkey_hex,
                                         GhStoreKeyFlags flags, GCancellable *cancellable,
                                         GAsyncReadyCallback callback, gpointer user_data);
/* As gh_store_key_lookup_finish(); *out_created (optional) is TRUE when this
 * call stored a new item. */
GBytes *gh_store_key_lookup_or_create_finish(GhStoreKey *self, GAsyncResult *result,
                                             gchar **out_store_id, gboolean *out_created,
                                             GError **error);

/* Forget (charter §3.8): delete every store-key item of the account, of any
 * store-id or version. Succeeds when none is left, including when none
 * existed. Run it before unlinking the store directory. */
void gh_store_key_destroy_async(GhStoreKey *self, const gchar *account_pubkey_hex,
                                GhStoreKeyFlags flags, GCancellable *cancellable,
                                GAsyncReadyCallback callback, gpointer user_data);
gboolean gh_store_key_destroy_finish(GhStoreKey *self, GAsyncResult *result, GError **error);

G_END_DECLS
#endif
