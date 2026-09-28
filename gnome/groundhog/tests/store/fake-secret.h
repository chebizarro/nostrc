#ifndef GH_TEST_FAKE_SECRET_H
#define GH_TEST_FAKE_SECRET_H

/* In-process Secret Service fake for GhStoreKey (charter §9.1 H5), plus
 * synchronous drivers shared by the store-key tests.
 *
 * FakeSecret implements GhStoreKeyBackend with the production backend's
 * semantics: one default keyring that is locked or unlocked as a whole, an
 * unlock "prompt" only for GH_STORE_KEY_FLAGS_INTERACTIVE calls, and LOCKED
 * (never "no items") while the keyring stays locked. Operations complete from
 * the main loop, like D-Bus replies, or are parked until released. Every
 * call is recorded with its flags, so a test can prove nothing prompted. */

#include "gh-store-key.h"

G_BEGIN_DECLS

#define FAKE_TYPE_SECRET (fake_secret_get_type())
G_DECLARE_FINAL_TYPE(FakeSecret, fake_secret, FAKE, SECRET, GObject)

typedef enum { FAKE_SECRET_SEARCH, FAKE_SECRET_STORE, FAKE_SECRET_CLEAR } FakeSecretOp;

typedef struct {
  FakeSecretOp op;
  GhStoreKeyFlags flags;
  GHashTable *attributes; /* the request's attributes */
  gchar *label;           /* FAKE_SECRET_STORE only */
} FakeSecretCall;

typedef struct {
  GHashTable *attributes;
  gchar *label;
  GBytes *secret;
} FakeSecretItem;

FakeSecret *fake_secret_new(void);

/* FALSE: every call fails GH_STORE_KEY_ERROR_UNAVAILABLE. Default TRUE. */
void fake_secret_set_available(FakeSecret *self, gboolean available);
/* Lock or unlock the (single, default) keyring. Default unlocked. */
void fake_secret_set_locked(FakeSecret *self, gboolean locked);
gboolean fake_secret_get_locked(FakeSecret *self);
/* The user's answer to an unlock prompt. Default TRUE. */
void fake_secret_set_unlock_accepted(FakeSecret *self, gboolean accepted);
/* TRUE: every store fails GH_STORE_KEY_ERROR_FAILED. */
void fake_secret_set_store_fails(FakeSecret *self, gboolean fails);
/* TRUE: a misbehaving service whose search returns every item. */
void fake_secret_set_ignore_query(FakeSecret *self, gboolean ignore);
/* TRUE: operations complete even if cancelled, like a D-Bus call the
 * service already received. */
void fake_secret_set_ignore_cancel(FakeSecret *self, gboolean ignore);
/* TRUE: park operations until fake_secret_release(). */
void fake_secret_set_hold(FakeSecret *self, gboolean hold);
guint fake_secret_pending(FakeSecret *self);
/* Parked operations whose request names account. */
guint fake_secret_pending_for(FakeSecret *self, const gchar *account);
/* Complete the oldest parked operation; FALSE when none is parked. */
gboolean fake_secret_release(FakeSecret *self);

/* Seed an item; the varargs are attribute name/value pairs, NULL-terminated. */
void fake_secret_add_item(FakeSecret *self, const gchar *label, gconstpointer secret,
                          gsize secret_len, ...) G_GNUC_NULL_TERMINATED;
/* Borrowed array of FakeSecretItem. */
GPtrArray *fake_secret_items(FakeSecret *self);
/* Items whose "account" attribute is account. */
guint fake_secret_count(FakeSecret *self, const gchar *account);
/* Borrowed array of FakeSecretCall, oldest first. */
GPtrArray *fake_secret_calls(FakeSecret *self);
void fake_secret_clear_calls(FakeSecret *self);
/* Unlock prompts shown so far. */
guint fake_secret_prompts(FakeSecret *self);

/* ---- Synchronous drivers (iterate the default main context) ------------ */

typedef struct {
  GBytes *key;
  gchar *store_id;
  gboolean created;
  GError *error;
} GhTestKeyResult;

void gh_test_key_result_clear(GhTestKeyResult *result);
GhTestKeyResult gh_test_lookup(GhStoreKey *store_key, const gchar *account,
                               GhStoreKeyFlags flags, GCancellable *cancellable);
GhTestKeyResult gh_test_lookup_or_create(GhStoreKey *store_key, const gchar *account,
                                         GhStoreKeyFlags flags, GCancellable *cancellable);
gboolean gh_test_destroy(GhStoreKey *store_key, const gchar *account, GhStoreKeyFlags flags,
                         GError **error);
/* Wait until *slot is set by gh_test_store_result(). */
GAsyncResult *gh_test_wait(GAsyncResult **slot);
/* GAsyncReadyCallback storing a reference to the result in user_data
 * (a GAsyncResult **). */
void gh_test_store_result(GObject *source, GAsyncResult *result, gpointer user_data);
/* Iterate the default main context until nothing is ready. */
void gh_test_run_until_idle(void);

/* Entries below path that are not directories (each is printed); 0 means no
 * file was written anywhere in the tree. Symlinks count and are not followed. */
guint gh_test_count_files(const gchar *path);
/* rm -rf path (symlinks are removed, not followed). */
void gh_test_remove_tree(const gchar *path);

G_END_DECLS
#endif
