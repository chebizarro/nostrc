#include "gh-store-key.h"
#ifdef __APPLE__
#include "gh-store-key-keychain.h"
#endif

#include <libsecret/secret.h>
#include <sodium.h>
#include <string.h>

G_DEFINE_QUARK(gh-store-key-error-quark, gh_store_key_error)

/* ---- Guarded key memory ---------------------------------------------------- */

static void
ensure_sodium(void)
{
  static gsize initialized = 0;
  if (g_once_init_enter(&initialized)) {
    /* Without libsodium there is no guarded memory and no CSPRNG to key a
     * store with; stop like any other allocator failure would. */
    if (sodium_init() < 0)
      g_error("libsodium failed to initialize; refusing to handle store keys");
    g_once_init_leave(&initialized, 1);
  }
}

static guint8 *
secret_alloc(gsize size)
{
  ensure_sodium();
  guint8 *buf = sodium_malloc(size > 0 ? size : 1);
  if (!buf)
    g_error("sodium_malloc of %" G_GSIZE_FORMAT " bytes failed", size);
  return buf;
}

/* Freeze a filled buffer and hand it to a GBytes that wipes it on release
 * (sodium_free restores write access, zeroes, unlocks and unmaps). */
static GBytes *
secret_seal(guint8 *buf, gsize size)
{
  sodium_mprotect_readonly(buf);
  return g_bytes_new_with_free_func(buf, size, sodium_free, buf);
}

GBytes *
gh_store_key_secret_new(gconstpointer data, gsize size)
{
  g_return_val_if_fail(data != NULL || size == 0, NULL);
  guint8 *buf = secret_alloc(size);
  if (size > 0)
    memcpy(buf, data, size);
  return secret_seal(buf, size);
}

static GBytes *
secret_new_random(gsize size)
{
  guint8 *buf = secret_alloc(size);
  randombytes_buf(buf, size); /* OS CSPRNG */
  return secret_seal(buf, size);
}

GBytes *
gh_store_key_dup_sqlcipher_key(GBytes *key)
{
  g_return_val_if_fail(key != NULL, NULL);
  gsize size = 0;
  const guint8 *raw = g_bytes_get_data(key, &size);
  if (size != GH_STORE_KEY_SIZE)
    return NULL;
  const gsize hex_len = 2 * GH_STORE_KEY_SIZE;
  const gsize len = 2 + hex_len + 1; /* x' <hex> ' */
  guint8 *buf = secret_alloc(len + 1);
  buf[0] = 'x';
  buf[1] = '\'';
  sodium_bin2hex((char *)buf + 2, hex_len + 1, raw, GH_STORE_KEY_SIZE); /* constant time */
  buf[len - 1] = '\'';
  buf[len] = '\0';
  return secret_seal(buf, len);
}

/* ---- Backend seam ------------------------------------------------------------ */

GhStoreKeyItem *
gh_store_key_item_new(GHashTable *attributes, gboolean locked, GBytes *secret)
{
  g_return_val_if_fail(attributes != NULL, NULL);
  GhStoreKeyItem *item = g_new0(GhStoreKeyItem, 1);
  item->attributes = attributes;
  item->locked = locked;
  item->secret = secret;
  return item;
}

void
gh_store_key_item_free(GhStoreKeyItem *item)
{
  if (!item)
    return;
  g_clear_pointer(&item->attributes, g_hash_table_unref);
  g_clear_pointer(&item->secret, g_bytes_unref);
  g_free(item);
}

G_DEFINE_INTERFACE(GhStoreKeyBackend, gh_store_key_backend, G_TYPE_OBJECT)

static void
gh_store_key_backend_default_init(GhStoreKeyBackendInterface *iface)
{
  (void)iface;
}

void
gh_store_key_backend_search_async(GhStoreKeyBackend *self, GHashTable *attributes,
                                  GhStoreKeyFlags flags, GCancellable *cancellable,
                                  GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_STORE_KEY_BACKEND(self));
  GH_STORE_KEY_BACKEND_GET_IFACE(self)->search_async(self, attributes, flags, cancellable,
                                                     callback, user_data);
}

GPtrArray *
gh_store_key_backend_search_finish(GhStoreKeyBackend *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_KEY_BACKEND(self), NULL);
  return GH_STORE_KEY_BACKEND_GET_IFACE(self)->search_finish(self, result, error);
}

void
gh_store_key_backend_store_async(GhStoreKeyBackend *self, GHashTable *attributes,
                                 const gchar *label, GBytes *secret, GhStoreKeyFlags flags,
                                 GCancellable *cancellable, GAsyncReadyCallback callback,
                                 gpointer user_data)
{
  g_return_if_fail(GH_IS_STORE_KEY_BACKEND(self));
  GH_STORE_KEY_BACKEND_GET_IFACE(self)->store_async(self, attributes, label, secret, flags,
                                                    cancellable, callback, user_data);
}

gboolean
gh_store_key_backend_store_finish(GhStoreKeyBackend *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_KEY_BACKEND(self), FALSE);
  return GH_STORE_KEY_BACKEND_GET_IFACE(self)->store_finish(self, result, error);
}

void
gh_store_key_backend_clear_async(GhStoreKeyBackend *self, GHashTable *attributes,
                                 GhStoreKeyFlags flags, GCancellable *cancellable,
                                 GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_STORE_KEY_BACKEND(self));
  GH_STORE_KEY_BACKEND_GET_IFACE(self)->clear_async(self, attributes, flags, cancellable,
                                                    callback, user_data);
}

gboolean
gh_store_key_backend_clear_finish(GhStoreKeyBackend *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_KEY_BACKEND(self), FALSE);
  return GH_STORE_KEY_BACKEND_GET_IFACE(self)->clear_finish(self, result, error);
}

/* ---- Secret Service backend (libsecret) ------------------------------------ */

static const SecretSchema store_key_schema = {
  .name = GH_STORE_KEY_SCHEMA_NAME,
  .flags = SECRET_SCHEMA_NONE, /* match xdg:schema: only Groundhog's own items */
  .attributes = {
    { GH_STORE_KEY_ATTR_ACCOUNT, SECRET_SCHEMA_ATTRIBUTE_STRING },
    { GH_STORE_KEY_ATTR_STORE_ID, SECRET_SCHEMA_ATTRIBUTE_STRING },
    { GH_STORE_KEY_ATTR_VERSION, SECRET_SCHEMA_ATTRIBUTE_STRING },
    { NULL, 0 },
  },
};

#define GH_TYPE_STORE_KEY_SECRET_SERVICE (gh_store_key_secret_service_get_type())
G_DECLARE_FINAL_TYPE(GhStoreKeySecretService, gh_store_key_secret_service,
                     GH, STORE_KEY_SECRET_SERVICE, GObject)

struct _GhStoreKeySecretService {
  GObject parent_instance;
};

static void secret_service_backend_init(GhStoreKeyBackendInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GhStoreKeySecretService, gh_store_key_secret_service, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GH_TYPE_STORE_KEY_BACKEND,
                                              secret_service_backend_init))

static void
gh_store_key_secret_service_class_init(GhStoreKeySecretServiceClass *klass)
{
  (void)klass;
}

static void
gh_store_key_secret_service_init(GhStoreKeySecretService *self)
{
  (void)self;
}

typedef enum { SS_SEARCH, SS_STORE, SS_CLEAR } SsKind;

typedef struct {
  SsKind kind;
  GHashTable *attributes;
  GhStoreKeyFlags flags;
  gchar *label;           /* SS_STORE */
  GBytes *secret;         /* SS_STORE */
  SecretService *service;
  gboolean tried_unlock;  /* the default keyring unlock was already offered */
  GList *pending;         /* SS_CLEAR: SecretItem refs still to delete */
  guint locked_left;      /* SS_CLEAR */
  GError *first_error;    /* SS_CLEAR */
} SsOp;

static void
ss_op_free(SsOp *op)
{
  g_clear_pointer(&op->attributes, g_hash_table_unref);
  g_free(op->label);
  g_clear_pointer(&op->secret, g_bytes_unref);
  g_clear_object(&op->service);
  g_list_free_full(op->pending, g_object_unref);
  g_clear_error(&op->first_error);
  g_free(op);
}

static gboolean
ss_interactive(const SsOp *op)
{
  return (op->flags & GH_STORE_KEY_FLAGS_INTERACTIVE) != 0;
}

static gboolean
error_means_unavailable(const GError *error)
{
  if (error->domain == G_DBUS_ERROR)
    return error->code == G_DBUS_ERROR_SERVICE_UNKNOWN ||
           error->code == G_DBUS_ERROR_NAME_HAS_NO_OWNER ||
           error->code == G_DBUS_ERROR_NO_REPLY ||
           error->code == G_DBUS_ERROR_TIMEOUT ||
           error->code == G_DBUS_ERROR_TIMED_OUT ||
           error->code == G_DBUS_ERROR_DISCONNECTED ||
           error->code == G_DBUS_ERROR_NO_SERVER ||
           error->code == G_DBUS_ERROR_SPAWN_SERVICE_NOT_FOUND ||
           error->code == G_DBUS_ERROR_SPAWN_EXEC_FAILED ||
           error->code == G_DBUS_ERROR_SPAWN_CHILD_EXITED ||
           error->code == G_DBUS_ERROR_SPAWN_FAILED;
  if (error->domain == G_IO_ERROR)
    return error->code == G_IO_ERROR_CLOSED ||
           error->code == G_IO_ERROR_CONNECTION_CLOSED ||
           error->code == G_IO_ERROR_BROKEN_PIPE;
  return FALSE;
}

static GError *
locked_error(void)
{
  return g_error_new_literal(GH_STORE_KEY_ERROR, GH_STORE_KEY_ERROR_LOCKED,
                             "The keyring is locked");
}

/* Takes ownership of error. connecting: failures to reach the service at
 * all (no session bus, nothing owning org.freedesktop.secrets). */
static GError *
ss_map_error(GError *error, gboolean connecting)
{
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return error;
  GError *mapped;
  if (connecting || error_means_unavailable(error))
    mapped = g_error_new(GH_STORE_KEY_ERROR, GH_STORE_KEY_ERROR_UNAVAILABLE,
                         "The Secret Service is not available: %s", error->message);
  else if (g_error_matches(error, SECRET_ERROR, SECRET_ERROR_IS_LOCKED))
    mapped = locked_error();
  else
    mapped = g_error_new(GH_STORE_KEY_ERROR, GH_STORE_KEY_ERROR_FAILED,
                         "The Secret Service failed: %s", error->message);
  g_error_free(error);
  return mapped;
}

static void
ss_fail(GTask *task, GError *error)
{
  g_task_return_error(task, error);
  g_object_unref(task);
}

static void ss_search(GTask *task);
static void ss_delete_next(GTask *task);

static void
ss_return_items(GTask *task, GList *items)
{
  GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)gh_store_key_item_free);
  for (GList *l = items; l; l = l->next) {
    SecretItem *item = l->data;
    gboolean locked = secret_item_get_locked(item);
    GBytes *secret = NULL;
    if (!locked) {
      SecretValue *value = secret_item_get_secret(item);
      if (value) {
        gsize len = 0;
        const gchar *data = secret_value_get(value, &len);
        /* Only a well-formed key is copied; anything else (of any size a
         * bus client chose) becomes an empty, unusable secret. */
        gboolean usable = len == GH_STORE_KEY_SIZE;
        secret = gh_store_key_secret_new(usable ? data : NULL, usable ? len : 0);
        secret_value_unref(value); /* libsecret's copy is in its own secure memory */
      }
    }
    g_ptr_array_add(out, gh_store_key_item_new(secret_item_get_attributes(item), locked, secret));
  }
  g_list_free_full(items, g_object_unref);
  g_task_return_pointer(task, out, (GDestroyNotify)g_ptr_array_unref);
  g_object_unref(task);
}

/* items: transfer full, possibly NULL. */
static void
ss_found(GTask *task, GList *items)
{
  SsOp *op = g_task_get_task_data(task);
  if (op->kind == SS_SEARCH) {
    ss_return_items(task, items);
    return;
  }
  /* SS_CLEAR. Deleting a locked item may make the service prompt, so locked
   * matches are never touched: an interactive search already offered to
   * unlock them. */
  for (GList *l = items; l; l = l->next) {
    SecretItem *item = l->data;
    if (secret_item_get_locked(item)) {
      op->locked_left++;
      g_object_unref(item);
    } else {
      op->pending = g_list_prepend(op->pending, item);
    }
  }
  g_list_free(items);
  ss_delete_next(task);
}

static void
ss_on_item_deleted(GObject *source, GAsyncResult *result, gpointer user_data)
{
  GTask *task = user_data;
  SsOp *op = g_task_get_task_data(task);
  GError *error = NULL;
  if (!secret_item_delete_finish(SECRET_ITEM(source), result, &error)) {
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      ss_fail(task, error);
      return;
    }
    if (g_error_matches(error, SECRET_ERROR, SECRET_ERROR_IS_LOCKED)) {
      op->locked_left++;
      g_error_free(error);
    } else if (!op->first_error) {
      op->first_error = ss_map_error(error, FALSE);
    } else {
      g_error_free(error);
    }
  }
  ss_delete_next(task);
}

static void
ss_delete_next(GTask *task)
{
  SsOp *op = g_task_get_task_data(task);
  if (op->pending) {
    SecretItem *item = op->pending->data;
    op->pending = g_list_delete_link(op->pending, op->pending);
    secret_item_delete(item, g_task_get_cancellable(task), ss_on_item_deleted, task);
    g_object_unref(item); /* the delete call holds its own reference */
    return;
  }
  if (op->first_error)
    ss_fail(task, g_steal_pointer(&op->first_error));
  else if (op->locked_left > 0)
    ss_fail(task, locked_error());
  else {
    g_task_return_boolean(task, TRUE);
    g_object_unref(task);
  }
}

static void
ss_on_default_unlocked(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  GTask *task = user_data;
  SsOp *op = g_task_get_task_data(task);
  GError *error = NULL;
  GList *unlocked = NULL;
  secret_service_unlock_finish(op->service, result, &unlocked, &error);
  g_list_free_full(unlocked, g_object_unref);
  if (error) {
    ss_fail(task, ss_map_error(error, FALSE));
    return;
  }
  /* Search again: if the prompt was dismissed the keyring is still locked
   * and the second empty result reports that. */
  ss_search(task);
}

/* Nothing matched. That only means "no item" if the default keyring is
 * unlocked: a service may not expose a locked keyring's items, and reporting
 * NOT_FOUND there would offer to wipe a store whose key merely sits behind
 * the lock. */
static void
ss_on_default_for_search(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  GTask *task = user_data;
  SsOp *op = g_task_get_task_data(task);
  GError *error = NULL;
  SecretCollection *collection = secret_collection_for_alias_finish(result, &error);
  if (error) {
    ss_fail(task, ss_map_error(error, FALSE));
    return;
  }
  if (!collection || !secret_collection_get_locked(collection)) {
    g_clear_object(&collection);
    ss_found(task, NULL);
    return;
  }
  if (!ss_interactive(op) || op->tried_unlock) {
    g_object_unref(collection);
    ss_fail(task, locked_error());
    return;
  }
  op->tried_unlock = TRUE;
  GList *objects = g_list_append(NULL, collection);
  secret_service_unlock(op->service, objects, g_task_get_cancellable(task),
                        ss_on_default_unlocked, task);
  g_list_free(objects);
  g_object_unref(collection);
}

static void
ss_on_search(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  GTask *task = user_data;
  SsOp *op = g_task_get_task_data(task);
  GError *error = NULL;
  GList *items = secret_service_search_finish(op->service, result, &error);
  if (error) {
    ss_fail(task, ss_map_error(error, FALSE));
    return;
  }
  if (items) {
    ss_found(task, items);
    return;
  }
  secret_collection_for_alias(op->service, SECRET_COLLECTION_DEFAULT, SECRET_COLLECTION_NONE,
                              g_task_get_cancellable(task), ss_on_default_for_search, task);
}

static void
ss_search(GTask *task)
{
  SsOp *op = g_task_get_task_data(task);
  SecretSearchFlags flags = SECRET_SEARCH_ALL;
  if (op->kind == SS_SEARCH)
    flags |= SECRET_SEARCH_LOAD_SECRETS; /* clear never reads the secret */
  if (ss_interactive(op))
    flags |= SECRET_SEARCH_UNLOCK;
  secret_service_search(op->service, &store_key_schema, op->attributes, flags,
                        g_task_get_cancellable(task), ss_on_search, task);
}

static void
ss_on_stored(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  GTask *task = user_data;
  SsOp *op = g_task_get_task_data(task);
  GError *error = NULL;
  gboolean ok;
  if (ss_interactive(op)) {
    ok = secret_service_store_finish(op->service, result, &error);
  } else {
    SecretItem *item = secret_item_create_finish(result, &error);
    ok = item != NULL;
    g_clear_object(&item);
  }
  if (!ok) {
    ss_fail(task, ss_map_error(error, FALSE));
    return;
  }
  g_task_return_boolean(task, TRUE);
  g_object_unref(task);
}

static SecretValue *
ss_secret_value(const SsOp *op)
{
  gsize len = 0;
  const gchar *data = g_bytes_get_data(op->secret, &len);
  /* secret_value_new copies into libsecret's secure memory. */
  return secret_value_new(data, (gssize)len, "application/octet-stream");
}

/* Background store: only into an existing, already unlocked default keyring,
 * through a call that fails (instead of prompting) if it locks meanwhile. */
static void
ss_on_default_for_store(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  GTask *task = user_data;
  SsOp *op = g_task_get_task_data(task);
  GError *error = NULL;
  SecretCollection *collection = secret_collection_for_alias_finish(result, &error);
  if (error) {
    ss_fail(task, ss_map_error(error, FALSE));
    return;
  }
  if (!collection || secret_collection_get_locked(collection)) {
    /* No default keyring yet also needs the user (a creation prompt). */
    g_clear_object(&collection);
    ss_fail(task, locked_error());
    return;
  }
  SecretValue *value = ss_secret_value(op);
  secret_item_create(collection, &store_key_schema, op->attributes, op->label, value,
                     SECRET_ITEM_CREATE_NONE, g_task_get_cancellable(task), ss_on_stored, task);
  secret_value_unref(value);
  g_object_unref(collection);
}

static void
ss_store(GTask *task)
{
  SsOp *op = g_task_get_task_data(task);
  if (ss_interactive(op)) {
    /* The service may prompt to unlock, or to create, the default keyring.
     * secret_service_store replaces an item with identical attributes;
     * GhStoreKey always passes a fresh store-id, so none can match. */
    SecretValue *value = ss_secret_value(op);
    secret_service_store(op->service, &store_key_schema, op->attributes,
                         SECRET_COLLECTION_DEFAULT, op->label, value,
                         g_task_get_cancellable(task), ss_on_stored, task);
    secret_value_unref(value);
    return;
  }
  secret_collection_for_alias(op->service, SECRET_COLLECTION_DEFAULT, SECRET_COLLECTION_NONE,
                              g_task_get_cancellable(task), ss_on_default_for_store, task);
}

static void
ss_on_service(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  GTask *task = user_data;
  SsOp *op = g_task_get_task_data(task);
  GError *error = NULL;
  op->service = secret_service_get_finish(result, &error);
  if (!op->service) {
    ss_fail(task, ss_map_error(error, TRUE));
    return;
  }
  if (op->kind == SS_STORE)
    ss_store(task);
  else
    ss_search(task);
}

static void
ss_start(GhStoreKeyBackend *backend, SsOp *op, GCancellable *cancellable,
         GAsyncReadyCallback callback, gpointer user_data, gpointer source_tag)
{
  GTask *task = g_task_new(backend, cancellable, callback, user_data);
  g_task_set_source_tag(task, source_tag);
  g_task_set_task_data(task, op, (GDestroyNotify)ss_op_free);
  /* OPEN_SESSION makes "nothing owns org.freedesktop.secrets" fail here,
   * before any item operation. */
  secret_service_get(SECRET_SERVICE_OPEN_SESSION, cancellable, ss_on_service, task);
}

static SsOp *
ss_op_new(SsKind kind, GHashTable *attributes, GhStoreKeyFlags flags)
{
  SsOp *op = g_new0(SsOp, 1);
  op->kind = kind;
  op->attributes = g_hash_table_ref(attributes);
  op->flags = flags;
  return op;
}

static void
ss_search_async(GhStoreKeyBackend *self, GHashTable *attributes, GhStoreKeyFlags flags,
                GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  ss_start(self, ss_op_new(SS_SEARCH, attributes, flags), cancellable, callback, user_data,
           ss_search_async);
}

static GPtrArray *
ss_search_finish(GhStoreKeyBackend *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
ss_store_async(GhStoreKeyBackend *self, GHashTable *attributes, const gchar *label,
               GBytes *secret, GhStoreKeyFlags flags, GCancellable *cancellable,
               GAsyncReadyCallback callback, gpointer user_data)
{
  SsOp *op = ss_op_new(SS_STORE, attributes, flags);
  op->label = g_strdup(label);
  op->secret = g_bytes_ref(secret);
  ss_start(self, op, cancellable, callback, user_data, ss_store_async);
}

static gboolean
ss_boolean_finish(GhStoreKeyBackend *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

static void
ss_clear_async(GhStoreKeyBackend *self, GHashTable *attributes, GhStoreKeyFlags flags,
               GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  ss_start(self, ss_op_new(SS_CLEAR, attributes, flags), cancellable, callback, user_data,
           ss_clear_async);
}

static void
secret_service_backend_init(GhStoreKeyBackendInterface *iface)
{
  iface->search_async = ss_search_async;
  iface->search_finish = ss_search_finish;
  iface->store_async = ss_store_async;
  iface->store_finish = ss_boolean_finish;
  iface->clear_async = ss_clear_async;
  iface->clear_finish = ss_boolean_finish;
}

/* ---- GhStoreKey ------------------------------------------------------------ */

struct _GhStoreKey {
  GObject parent_instance;
  GhStoreKeyBackend *backend;
  /* account -> GQueue of GTask (one reference each); the head is running.
   * Every queued task references self, so the table is empty at finalize. */
  GHashTable *queues;
};

G_DEFINE_TYPE(GhStoreKey, gh_store_key, G_TYPE_OBJECT)

typedef enum { OP_LOOKUP, OP_LOOKUP_OR_CREATE, OP_DESTROY } OpKind;

typedef struct {
  OpKind kind;
  gchar *account;
  GhStoreKeyFlags flags;
  GBytes *new_key;         /* OP_LOOKUP_OR_CREATE, while the item is being stored */
  gchar *new_store_id;
  gulong queued_cancel_id; /* while waiting behind another operation */
} Op;

typedef struct {
  GBytes *key;
  gchar *store_id;
  gboolean created;
} KeyResult;

static void
op_free(Op *op)
{
  g_free(op->account);
  g_clear_pointer(&op->new_key, g_bytes_unref);
  g_free(op->new_store_id);
  g_free(op);
}

static void
key_result_free(KeyResult *result)
{
  g_clear_pointer(&result->key, g_bytes_unref);
  g_free(result->store_id);
  g_free(result);
}

static void
gh_store_key_finalize(GObject *object)
{
  GhStoreKey *self = GH_STORE_KEY(object);
  g_clear_object(&self->backend);
  g_clear_pointer(&self->queues, g_hash_table_unref);
  G_OBJECT_CLASS(gh_store_key_parent_class)->finalize(object);
}

static void
gh_store_key_class_init(GhStoreKeyClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = gh_store_key_finalize;
}

static void
gh_store_key_init(GhStoreKey *self)
{
  self->queues = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                       (GDestroyNotify)g_queue_free);
}

GhStoreKey *
gh_store_key_new(GhStoreKeyBackend *backend)
{
  g_return_val_if_fail(backend == NULL || GH_IS_STORE_KEY_BACKEND(backend), NULL);
  GhStoreKey *self = g_object_new(GH_TYPE_STORE_KEY, NULL);
#ifdef __APPLE__
  self->backend = backend ? g_object_ref(backend)
                          : g_object_new(GH_TYPE_STORE_KEY_KEYCHAIN, NULL);
#else
  self->backend = backend ? g_object_ref(backend)
                          : g_object_new(GH_TYPE_STORE_KEY_SECRET_SERVICE, NULL);
#endif
  return self;
}

GhStoreKey *
gh_store_key_new_secret_service(void)
{
  g_autoptr(GhStoreKeyBackend) backend = g_object_new(GH_TYPE_STORE_KEY_SECRET_SERVICE, NULL);
  return gh_store_key_new(backend);
}

static gchar *
normalize_account(const gchar *hex)
{
  if (!hex || strlen(hex) != 64)
    return NULL;
  for (guint i = 0; i < 64; i++)
    if (!g_ascii_isxdigit(hex[i]))
      return NULL;
  return g_ascii_strdown(hex, 64);
}

static GHashTable *
account_attributes(const gchar *account)
{
  GHashTable *attributes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  g_hash_table_insert(attributes, g_strdup(GH_STORE_KEY_ATTR_ACCOUNT), g_strdup(account));
  return attributes;
}

static void run_op(GTask *task);

static void
disconnect_queued_cancel(GTask *task)
{
  Op *op = g_task_get_task_data(task);
  if (op->queued_cancel_id) {
    g_cancellable_disconnect(g_task_get_cancellable(task), op->queued_cancel_id);
    op->queued_cancel_id = 0;
  }
}

/* A queued (not yet running) operation was cancelled: complete it now
 * rather than after the operation ahead of it, which may be waiting on an
 * unlock prompt. A running one is left to its own cancellation. */
static gboolean
drop_cancelled_queued(gpointer user_data)
{
  GTask *task = user_data;
  GhStoreKey *self = g_task_get_source_object(task);
  Op *op = g_task_get_task_data(task);
  GQueue *queue = g_hash_table_lookup(self->queues, op->account);
  if (!queue || g_queue_peek_head(queue) == task || !g_queue_find(queue, task))
    return G_SOURCE_REMOVE;
  g_queue_remove(queue, task); /* never the head, so the queue stays non-empty */
  disconnect_queued_cancel(task);
  g_task_return_error_if_cancelled(task);
  g_object_unref(task); /* the queue's reference */
  return G_SOURCE_REMOVE;
}

/* May run in the cancelling thread, under the cancellable's lock: only
 * schedule the work on the task's context. */
static void
on_queued_cancelled(GCancellable *cancellable, gpointer user_data)
{
  (void)cancellable;
  GTask *task = user_data;
  GSource *source = g_idle_source_new();
  g_source_set_callback(source, drop_cancelled_queued, g_object_ref(task), g_object_unref);
  g_source_attach(source, g_task_get_context(task));
  g_source_unref(source);
}

/* Called after the task has been returned: release it and start the next
 * operation queued for the same account. */
static void
op_done(GTask *task)
{
  GhStoreKey *self = g_task_get_source_object(task);
  Op *op = g_task_get_task_data(task);
  GQueue *queue = g_hash_table_lookup(self->queues, op->account);
  g_assert(queue != NULL && g_queue_peek_head(queue) == task);
  g_queue_pop_head(queue);
  GTask *next = g_queue_peek_head(queue);
  if (!next)
    g_hash_table_remove(self->queues, op->account);
  g_object_unref(task);
  if (next)
    run_op(next);
}

static void
op_return_error(GTask *task, GError *error)
{
  g_task_return_error(task, error);
  op_done(task);
}

static const gchar *
item_attribute(const GhStoreKeyItem *item, const gchar *name)
{
  return g_hash_table_lookup(item->attributes, name);
}

/* A version this build does not know but a newer one wrote ("2", "10"). */
static gboolean
version_is_newer(const gchar *version)
{
  guint64 number = 0;
  return version && g_ascii_string_to_unsigned(version, 10, 2, G_MAXUINT64, &number, NULL);
}

/* The one usable item of account in items, or an error naming the store
 * state. Never includes the account in a message. */
static KeyResult *
classify(const gchar *account, GPtrArray *items, GError **error)
{
  /* A newer Groundhog's item must never look destroyable, even beside
   * another item (e.g. an interrupted migration). */
  for (guint i = 0; i < items->len; i++) {
    const GhStoreKeyItem *item = g_ptr_array_index(items, i);
    if (g_strcmp0(item_attribute(item, GH_STORE_KEY_ATTR_ACCOUNT), account) == 0 &&
        version_is_newer(item_attribute(item, GH_STORE_KEY_ATTR_VERSION))) {
      g_set_error_literal(error, GH_STORE_KEY_ERROR, GH_STORE_KEY_ERROR_NEWER_VERSION,
                          "The message storage key was created by a newer Groundhog");
      return NULL;
    }
  }
  if (items->len == 0) {
    g_set_error_literal(error, GH_STORE_KEY_ERROR, GH_STORE_KEY_ERROR_NOT_FOUND,
                        "No message storage key exists for this account");
    return NULL;
  }
  if (items->len > 1) {
    g_set_error_literal(error, GH_STORE_KEY_ERROR, GH_STORE_KEY_ERROR_INVALID,
                        "More than one message storage key exists for this account");
    return NULL;
  }
  const GhStoreKeyItem *item = g_ptr_array_index(items, 0);
  if (g_strcmp0(item_attribute(item, GH_STORE_KEY_ATTR_ACCOUNT), account) != 0) {
    g_set_error_literal(error, GH_STORE_KEY_ERROR, GH_STORE_KEY_ERROR_INVALID,
                        "The message storage key belongs to another account");
    return NULL;
  }
  if (g_strcmp0(item_attribute(item, GH_STORE_KEY_ATTR_VERSION), GH_STORE_KEY_VERSION) != 0) {
    g_set_error_literal(error, GH_STORE_KEY_ERROR, GH_STORE_KEY_ERROR_INVALID,
                        "The message storage key has no valid version");
    return NULL;
  }
  const gchar *store_id = item_attribute(item, GH_STORE_KEY_ATTR_STORE_ID);
  if (!store_id || !g_uuid_string_is_valid(store_id)) {
    g_set_error_literal(error, GH_STORE_KEY_ERROR, GH_STORE_KEY_ERROR_INVALID,
                        "The message storage key has no valid store id");
    return NULL;
  }
  if (item->locked) {
    g_propagate_error(error, locked_error());
    return NULL;
  }
  if (!item->secret) {
    g_set_error_literal(error, GH_STORE_KEY_ERROR, GH_STORE_KEY_ERROR_FAILED,
                        "The Secret Service returned no secret for the message storage key");
    return NULL;
  }
  if (g_bytes_get_size(item->secret) != GH_STORE_KEY_SIZE) {
    g_set_error_literal(error, GH_STORE_KEY_ERROR, GH_STORE_KEY_ERROR_INVALID,
                        "The message storage key has the wrong size");
    return NULL;
  }
  KeyResult *result = g_new0(KeyResult, 1);
  result->key = g_bytes_ref(item->secret);
  result->store_id = g_strdup(store_id);
  return result;
}

static void
on_stored(GObject *source, GAsyncResult *res, gpointer user_data)
{
  GTask *task = user_data;
  Op *op = g_task_get_task_data(task);
  GError *error = NULL;
  if (!gh_store_key_backend_store_finish(GH_STORE_KEY_BACKEND(source), res, &error)) {
    op_return_error(task, error); /* op_free wipes the unused key */
    return;
  }
  KeyResult *result = g_new0(KeyResult, 1);
  result->key = g_steal_pointer(&op->new_key);
  result->store_id = g_steal_pointer(&op->new_store_id);
  result->created = TRUE;
  /* The item exists now: report it even if the caller cancelled meanwhile,
   * rather than wiping a key the keyring keeps. */
  g_task_set_check_cancellable(task, FALSE);
  g_task_return_pointer(task, result, (GDestroyNotify)key_result_free);
  op_done(task);
}

static void
create_item(GTask *task)
{
  GhStoreKey *self = g_task_get_source_object(task);
  Op *op = g_task_get_task_data(task);
  op->new_key = secret_new_random(GH_STORE_KEY_SIZE);
  op->new_store_id = g_uuid_string_random();
  GHashTable *attributes = account_attributes(op->account);
  g_hash_table_insert(attributes, g_strdup(GH_STORE_KEY_ATTR_STORE_ID),
                      g_strdup(op->new_store_id));
  g_hash_table_insert(attributes, g_strdup(GH_STORE_KEY_ATTR_VERSION),
                      g_strdup(GH_STORE_KEY_VERSION));
  gh_store_key_backend_store_async(self->backend, attributes, GH_STORE_KEY_LABEL, op->new_key,
                                   op->flags, g_task_get_cancellable(task), on_stored, task);
  g_hash_table_unref(attributes);
}

static void
on_searched(GObject *source, GAsyncResult *res, gpointer user_data)
{
  GTask *task = user_data;
  Op *op = g_task_get_task_data(task);
  GError *error = NULL;
  GPtrArray *items = gh_store_key_backend_search_finish(GH_STORE_KEY_BACKEND(source), res, &error);
  if (!items) {
    op_return_error(task, error);
    return;
  }
  KeyResult *result = classify(op->account, items, &error);
  g_ptr_array_unref(items);
  if (!result) {
    if (op->kind == OP_LOOKUP_OR_CREATE &&
        g_error_matches(error, GH_STORE_KEY_ERROR, GH_STORE_KEY_ERROR_NOT_FOUND)) {
      g_error_free(error);
      create_item(task);
      return;
    }
    op_return_error(task, error);
    return;
  }
  g_task_return_pointer(task, result, (GDestroyNotify)key_result_free);
  op_done(task);
}

static void
on_cleared(GObject *source, GAsyncResult *res, gpointer user_data)
{
  GTask *task = user_data;
  GError *error = NULL;
  if (!gh_store_key_backend_clear_finish(GH_STORE_KEY_BACKEND(source), res, &error)) {
    op_return_error(task, error);
    return;
  }
  g_task_return_boolean(task, TRUE);
  op_done(task);
}

static void
run_op(GTask *task)
{
  GhStoreKey *self = g_task_get_source_object(task);
  Op *op = g_task_get_task_data(task);
  disconnect_queued_cancel(task);
  if (g_task_return_error_if_cancelled(task)) {
    op_done(task);
    return;
  }
  GHashTable *attributes = account_attributes(op->account);
  if (op->kind == OP_DESTROY)
    gh_store_key_backend_clear_async(self->backend, attributes, op->flags,
                                     g_task_get_cancellable(task), on_cleared, task);
  else
    gh_store_key_backend_search_async(self->backend, attributes, op->flags,
                                      g_task_get_cancellable(task), on_searched, task);
  g_hash_table_unref(attributes);
}

static void
queue_op(GhStoreKey *self, OpKind kind, const gchar *account_pubkey_hex, GhStoreKeyFlags flags,
         GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data,
         gpointer source_tag)
{
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, source_tag);
  gchar *account = normalize_account(account_pubkey_hex);
  if (!account) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "The account is not a 64-character hex public key");
    g_object_unref(task);
    return;
  }
  Op *op = g_new0(Op, 1);
  op->kind = kind;
  op->account = account;
  op->flags = flags;
  g_task_set_task_data(task, op, (GDestroyNotify)op_free);

  GQueue *queue = g_hash_table_lookup(self->queues, account);
  if (!queue) {
    queue = g_queue_new();
    g_hash_table_insert(self->queues, g_strdup(account), queue);
  }
  g_queue_push_tail(queue, task); /* the queue owns the reference */
  if (queue->length == 1)
    run_op(task);
  else if (cancellable)
    op->queued_cancel_id = g_cancellable_connect(cancellable, G_CALLBACK(on_queued_cancelled),
                                                 task, NULL);
}

void
gh_store_key_lookup_async(GhStoreKey *self, const gchar *account_pubkey_hex,
                          GhStoreKeyFlags flags, GCancellable *cancellable,
                          GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_STORE_KEY(self));
  queue_op(self, OP_LOOKUP, account_pubkey_hex, flags, cancellable, callback, user_data,
           gh_store_key_lookup_async);
}

void
gh_store_key_lookup_or_create_async(GhStoreKey *self, const gchar *account_pubkey_hex,
                                    GhStoreKeyFlags flags, GCancellable *cancellable,
                                    GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_STORE_KEY(self));
  queue_op(self, OP_LOOKUP_OR_CREATE, account_pubkey_hex, flags, cancellable, callback,
           user_data, gh_store_key_lookup_or_create_async);
}

void
gh_store_key_destroy_async(GhStoreKey *self, const gchar *account_pubkey_hex,
                           GhStoreKeyFlags flags, GCancellable *cancellable,
                           GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_STORE_KEY(self));
  queue_op(self, OP_DESTROY, account_pubkey_hex, flags, cancellable, callback, user_data,
           gh_store_key_destroy_async);
}

static GBytes *
key_finish(GAsyncResult *result, gchar **out_store_id, gboolean *out_created, GError **error)
{
  if (out_store_id)
    *out_store_id = NULL;
  if (out_created)
    *out_created = FALSE;
  KeyResult *key_result = g_task_propagate_pointer(G_TASK(result), error);
  if (!key_result)
    return NULL;
  GBytes *key = g_steal_pointer(&key_result->key);
  if (out_store_id)
    *out_store_id = g_steal_pointer(&key_result->store_id);
  if (out_created)
    *out_created = key_result->created;
  key_result_free(key_result);
  return key;
}

GBytes *
gh_store_key_lookup_finish(GhStoreKey *self, GAsyncResult *result, gchar **out_store_id,
                           GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) == gh_store_key_lookup_async, NULL);
  return key_finish(result, out_store_id, NULL, error);
}

GBytes *
gh_store_key_lookup_or_create_finish(GhStoreKey *self, GAsyncResult *result,
                                     gchar **out_store_id, gboolean *out_created, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), NULL);
  g_return_val_if_fail(
    g_task_get_source_tag(G_TASK(result)) == gh_store_key_lookup_or_create_async, NULL);
  return key_finish(result, out_store_id, out_created, error);
}

gboolean
gh_store_key_destroy_finish(GhStoreKey *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) == gh_store_key_destroy_async, FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}
