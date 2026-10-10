#include "gh-nip46-credentials-private.h"
#include "nostr/nip19/nip19.h"
#include <json-glib/json-glib.h>
#include <sodium.h>
#include <string.h>

G_DEFINE_QUARK(gh-nip46-credential-error-quark, gh_nip46_credential_error)

struct _GhNip46Credential {
  gchar *user;
  gchar *signer;
  gchar *client_secret; /* wiped before free */
  gchar **relays;
};

static gboolean
valid_hex(const gchar *s)
{
  if (!s || strlen(s) != 64) return FALSE;
  for (guint i = 0; i < 64; i++)
    if (!g_ascii_isdigit(s[i]) && !(s[i] >= 'a' && s[i] <= 'f')) return FALSE;
  return TRUE;
}

static gchar *
normalize_relay(const gchar *url)
{
  if (!url || strlen(url) > 2048) return NULL;
  g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
  if (!uri || g_uri_get_userinfo(uri) || g_uri_get_query(uri) || g_uri_get_fragment(uri))
    return NULL;
  const gchar *scheme = g_uri_get_scheme(uri), *host = g_uri_get_host(uri);
  if (!host || !*host || !scheme) return NULL;
  g_autofree gchar *lower_host = g_ascii_strdown(host, -1);
  g_autofree gchar *lower_scheme = g_ascii_strdown(scheme, -1);
  gboolean onion = g_str_has_suffix(lower_host, ".onion");
  g_autoptr(GInetAddress) address = g_inet_address_new_from_string(host);
  gboolean loopback = g_ascii_strcasecmp(host, "localhost") == 0 ||
                      (address && g_inet_address_get_is_loopback(address));
  if (!g_str_equal(lower_scheme, "wss") &&
      !(g_str_equal(lower_scheme, "ws") && (loopback || onion))) return NULL;
  gint port = g_uri_get_port(uri);
  if ((g_str_equal(lower_scheme, "wss") && port == 443) ||
      (g_str_equal(lower_scheme, "ws") && port == 80)) port = -1;
  const gchar *path = g_uri_get_path(uri);
  if (!path || g_str_equal(path, "/")) path = "";
  g_autoptr(GUri) normalized = g_uri_build(G_URI_FLAGS_NONE, lower_scheme, NULL,
                                           lower_host, port, path, NULL, NULL);
  return g_uri_to_string(normalized);
}

GhNip46Credential *
gh_nip46_credential_new(const gchar *user, const gchar *signer,
                         const gchar *client_secret, const gchar * const *relays,
                         GError **error)
{
  if (!valid_hex(user) || !valid_hex(signer) || !valid_hex(client_secret) || !relays) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid remote signer credential");
    return NULL;
  }
  gchar *normalized[5] = {0};
  guint count = 0;
  for (; relays[count] && count < 4; count++) {
    normalized[count] = normalize_relay(relays[count]);
    if (!normalized[count]) goto invalid_relays;
    for (guint j = 0; j < count; j++)
      if (g_str_equal(normalized[j], normalized[count])) goto invalid_relays;
  }
  if (count == 0 || relays[count]) goto invalid_relays;
  GhNip46Credential *credential = g_new0(GhNip46Credential, 1);
  credential->user = g_strdup(user);
  credential->signer = g_strdup(signer);
  credential->client_secret = g_strdup(client_secret);
  credential->relays = g_new0(gchar *, count + 1);
  for (guint i = 0; i < count; i++) credential->relays[i] = normalized[i];
  return credential;
invalid_relays:
  for (guint i = 0; i < G_N_ELEMENTS(normalized); i++) g_free(normalized[i]);
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                      "Invalid remote signer relays");
  return NULL;
}

void
gh_nip46_credential_free(GhNip46Credential *credential)
{
  if (!credential) return;
  g_free(credential->user);
  g_free(credential->signer);
  if (credential->client_secret) {
    sodium_memzero(credential->client_secret, strlen(credential->client_secret));
    g_free(credential->client_secret);
  }
  g_strfreev(credential->relays);
  g_free(credential);
}

const gchar *gh_nip46_credential_get_user_pubkey_hex(const GhNip46Credential *c) { return c->user; }
const gchar *gh_nip46_credential_get_remote_signer_pubkey_hex(const GhNip46Credential *c) { return c->signer; }
const gchar *gh_nip46_credential_get_client_secret_hex(const GhNip46Credential *c) { return c->client_secret; }
const gchar * const *gh_nip46_credential_get_relays(const GhNip46Credential *c) { return (const gchar * const *)c->relays; }

static void
wipe_bytes(gpointer data)
{
  gsize *block = data;
  sodium_memzero(block + 1, *block);
  g_free(block);
}

/* A GBytes whose copy is wiped on final release. */
GBytes *
gh_nip46_credentials_secret_bytes_new(const void *data, gsize len)
{
  gsize *block = g_malloc(sizeof(gsize) + (len ? len : 1));
  *block = len;
  if (len) memcpy(block + 1, data, len);
  return g_bytes_new_with_free_func(block + 1, len, wipe_bytes, block);
}

static GBytes *
serialize(const GhNip46Credential *c)
{
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "version"); json_builder_add_int_value(builder, 1);
  json_builder_set_member_name(builder, "client_secret_hex"); json_builder_add_string_value(builder, c->client_secret);
  json_builder_set_member_name(builder, "remote_signer_pubkey_hex"); json_builder_add_string_value(builder, c->signer);
  json_builder_set_member_name(builder, "user_pubkey_hex"); json_builder_add_string_value(builder, c->user);
  json_builder_set_member_name(builder, "transport"); json_builder_add_string_value(builder, "nip44-v2");
  json_builder_set_member_name(builder, "relays"); json_builder_begin_array(builder);
  for (guint i = 0; c->relays[i]; i++) json_builder_add_string_value(builder, c->relays[i]);
  json_builder_end_array(builder); json_builder_end_object(builder);
  g_autoptr(JsonNode) root = json_builder_get_root(builder);
  g_autoptr(JsonGenerator) generator = json_generator_new();
  json_generator_set_root(generator, root);
  gsize len = 0;
  gchar *json = json_generator_to_data(generator, &len);
  GBytes *bytes = gh_nip46_credentials_secret_bytes_new(json, len);
  sodium_memzero(json, len);
  g_free(json);
  return bytes;
}

static guint64
version_number(const gchar *version)
{
  if (!version || !*version) return 0;
  for (const gchar *p = version; *p; p++)
    if (!g_ascii_isdigit(*p)) return 0;
  return g_ascii_strtoull(version, NULL, 10);
}

static GError *
credential_error(GhNip46CredentialError code)
{
  const gchar *message = code == GH_NIP46_CREDENTIAL_ERROR_NEWER_VERSION ?
    "Remote signer credential requires a newer Groundhog" :
    "Remote signer credential needs repair: remove and pair again";
  return g_error_new_literal(GH_NIP46_CREDENTIAL_ERROR, code, message);
}

static GhNip46Credential *
parse_item(GhNip46CredentialItem *item, GError **error)
{
  g_autoptr(JsonParser) parser = NULL;
  if (!valid_hex(item->account) || !item->version || !item->attributes_valid) goto invalid;
  guint64 attribute_version = version_number(item->version);
  if (attribute_version == 0) goto invalid;
  if (attribute_version > 1) {
    *error = credential_error(GH_NIP46_CREDENTIAL_ERROR_NEWER_VERSION);
    return NULL;
  }
  if (g_strcmp0(item->version, GH_NIP46_CREDENTIAL_VERSION) != 0) goto invalid;
  if (item->locked) {
    *error = g_error_new_literal(GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_LOCKED,
                                 "The keyring is locked");
    return NULL;
  }
  if (!item->secret || g_bytes_get_size(item->secret) > 8192) goto invalid;
  gsize len = 0;
  const gchar *data = g_bytes_get_data(item->secret, &len);
  if (!g_utf8_validate(data, len, NULL)) goto invalid;
  parser = json_parser_new();
  if (!json_parser_load_from_data(parser, data, (gssize)len, NULL)) goto invalid;
  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root)) goto invalid;
  JsonObject *obj = json_node_get_object(root);
  if (!json_object_has_member(obj, "version") ||
      !JSON_NODE_HOLDS_VALUE(json_object_get_member(obj, "version"))) goto invalid;
  if (json_node_get_value_type(json_object_get_member(obj, "version")) != G_TYPE_INT64) goto invalid;
  gint64 version = json_object_get_int_member(obj, "version");
  if (version > 1) {
    *error = credential_error(GH_NIP46_CREDENTIAL_ERROR_NEWER_VERSION);
    return NULL;
  }
  if (version != 1 || !json_object_has_member(obj, "client_secret_hex") ||
      !json_object_has_member(obj, "remote_signer_pubkey_hex") ||
      !json_object_has_member(obj, "user_pubkey_hex") ||
      !json_object_has_member(obj, "transport") ||
      !json_object_has_member(obj, "relays")) goto invalid;
  const gchar *string_names[] = { "client_secret_hex", "remote_signer_pubkey_hex",
                                  "user_pubkey_hex", "transport" };
  for (guint i = 0; i < G_N_ELEMENTS(string_names); i++) {
    JsonNode *node = json_object_get_member(obj, string_names[i]);
    if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
      goto invalid;
  }
  const gchar *secret = json_object_get_string_member(obj, "client_secret_hex");
  const gchar *signer = json_object_get_string_member(obj, "remote_signer_pubkey_hex");
  const gchar *user = json_object_get_string_member(obj, "user_pubkey_hex");
  const gchar *transport = json_object_get_string_member(obj, "transport");
  if (!valid_hex(secret) || !valid_hex(signer) || !valid_hex(user) ||
      !g_str_equal(user, item->account) || g_strcmp0(transport, "nip44-v2") != 0) goto invalid;
  JsonNode *relay_node = json_object_get_member(obj, "relays");
  if (!JSON_NODE_HOLDS_ARRAY(relay_node)) goto invalid;
  JsonArray *array = json_node_get_array(relay_node);
  if (json_array_get_length(array) < 1 || json_array_get_length(array) > 4) goto invalid;
  const gchar *relays[5] = {0};
  for (guint i = 0; i < json_array_get_length(array); i++) {
    JsonNode *element = json_array_get_element(array, i);
    if (!JSON_NODE_HOLDS_VALUE(element) || json_node_get_value_type(element) != G_TYPE_STRING)
      goto invalid;
    relays[i] = json_node_get_string(element);
  }
  GhNip46Credential *credential = gh_nip46_credential_new(user, signer, secret, relays, NULL);
  if (credential) {
    for (guint i = 0; relays[i]; i++) {
      if (!g_str_equal(relays[i], credential->relays[i])) {
        gh_nip46_credential_free(credential);
        goto invalid;
      }
    }
    return credential;
  }
invalid:
  *error = credential_error(GH_NIP46_CREDENTIAL_ERROR_INVALID);
  return NULL;
}

struct _GhNip46CredentialStore {
  GObject parent_instance;
  GhNip46CredentialBackend *backend;
  GMutex mutex; /* one store's operations are serialized across worker threads */
};
G_DEFINE_TYPE(GhNip46CredentialStore, gh_nip46_credential_store, G_TYPE_OBJECT)

static void
store_finalize(GObject *object)
{
  GhNip46CredentialStore *self = GH_NIP46_CREDENTIAL_STORE(object);
  if (self->backend) self->backend->free(self->backend);
  g_mutex_clear(&self->mutex);
  G_OBJECT_CLASS(gh_nip46_credential_store_parent_class)->finalize(object);
}
static void
gh_nip46_credential_store_class_init(GhNip46CredentialStoreClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = store_finalize;
}
static void
gh_nip46_credential_store_init(GhNip46CredentialStore *self)
{
  g_mutex_init(&self->mutex);
}
static GhNip46CredentialStore *
store_with_backend(GhNip46CredentialBackend *backend)
{
  GhNip46CredentialStore *self = g_object_new(GH_TYPE_NIP46_CREDENTIAL_STORE, NULL);
  self->backend = backend;
  return self;
}
GhNip46CredentialStore *
gh_nip46_credential_store_new_secret_service(void)
{
  return store_with_backend(gh_nip46_credentials_secret_service_new());
}
GhNip46CredentialStore *
gh_nip46_credential_store_new(void)
{
#if defined(__APPLE__) && !defined(GH_NIP46_CREDENTIAL_DEFAULT_SECRET_SERVICE)
  return store_with_backend(gh_nip46_credentials_keychain_new(NULL));
#else
  return gh_nip46_credential_store_new_secret_service();
#endif
}
GhNip46CredentialStore *
gh_nip46_credential_store_new_with_backend(GhNip46CredentialBackend *backend)
{
  g_return_val_if_fail(backend && backend->search && backend->free, NULL);
  return store_with_backend(backend);
}
#ifdef __APPLE__
GhNip46CredentialStore *
gh_nip46_credential_store_new_keychain(SecKeychainRef keychain)
{
  return store_with_backend(gh_nip46_credentials_keychain_new(keychain));
}
#endif

typedef enum { OP_LIST, OP_LOOKUP, OP_STORE, OP_DELETE } OpKind;
typedef struct {
  OpKind kind;
  gchar *account;
  GhNip46Credential *credential;
  gboolean interactive;
} Operation;
static void
operation_free(Operation *op)
{
  g_free(op->account);
  gh_nip46_credential_free(op->credential);
  g_free(op);
}

static gboolean
cancelled(GCancellable *cancellable, GError **error)
{
  if (!cancellable || !g_cancellable_is_cancelled(cancellable)) return FALSE;
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Operation cancelled");
  return TRUE;
}

static gint
compare_identity(gconstpointer a, gconstpointer b)
{
  const GhIdentityInfo *ia = *(GhIdentityInfo * const *)a;
  const GhIdentityInfo *ib = *(GhIdentityInfo * const *)b;
  return g_strcmp0(ia->npub, ib->npub);
}

static void
run_operation(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  GhNip46CredentialStore *self = source;
  Operation *op = task_data;
  GError *error = NULL;
  if (cancelled(cancellable, &error)) goto fail;
  if (op->kind != OP_LIST && !valid_hex(op->account)) {
    error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                "Invalid user pubkey");
    goto fail;
  }
  if (op->kind == OP_LIST) {
    /* Attribute-only and lock-free: a lookup or write blocked on a Keychain
     * or keyring prompt holds the mutex, and must not stall listing. */
    GPtrArray *items = self->backend->search(self->backend, NULL, FALSE, FALSE,
                                              cancellable, &error);
    if (!items) goto fail;
    GPtrArray *list = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
    for (guint i = 0; i < items->len; i++) {
      GhNip46CredentialItem *item = g_ptr_array_index(items, i);
      if (!valid_hex(item->account)) { error = credential_error(GH_NIP46_CREDENTIAL_ERROR_INVALID); break; }
      guint8 pubkey[32];
      for (guint j = 0; j < 32; j++) {
        gchar pair[3] = {item->account[j*2], item->account[j*2+1], 0};
        pubkey[j] = (guint8)strtoul(pair, NULL, 16);
      }
      char *npub = NULL;
      if (nostr_nip19_encode_npub(pubkey, &npub) != 0) {
        error = credential_error(GH_NIP46_CREDENTIAL_ERROR_INVALID); break;
      }
      GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
      info->npub = npub;
      info->label = g_strdup(GH_NIP46_CREDENTIAL_LABEL);
      info->backend = GH_SIGNER_BACKEND_NIP46;
      g_ptr_array_add(list, info);
    }
    g_ptr_array_sort(list, compare_identity);
    g_ptr_array_unref(items);
    if (!error) cancelled(cancellable, &error);
    if (error) { g_ptr_array_unref(list); goto fail; }
    g_task_return_pointer(task, list, (GDestroyNotify)g_ptr_array_unref);
    return;
  }
  g_mutex_lock(&self->mutex);
  GPtrArray *items = self->backend->search(self->backend, op->account,
                                            op->interactive, TRUE, cancellable, &error);
  if (!items) goto unlock_fail;
  if (cancelled(cancellable, &error)) goto items_fail;
  for (guint i = 0; i < items->len; i++) {
    GhNip46CredentialItem *item = g_ptr_array_index(items, i);
    if (version_number(item->version) > 1) {
      error = credential_error(GH_NIP46_CREDENTIAL_ERROR_NEWER_VERSION);
      goto items_fail;
    }
    if (item->locked) {
      error = g_error_new_literal(GH_NIP46_CREDENTIAL_ERROR,
                                  GH_NIP46_CREDENTIAL_ERROR_LOCKED, "The keyring is locked");
      goto items_fail;
    }
  }
  if (items->len > 1 && op->kind != OP_DELETE) {
    error = credential_error(GH_NIP46_CREDENTIAL_ERROR_INVALID);
    goto items_fail;
  }
  if (op->kind == OP_DELETE) {
    for (guint i = 0; i < items->len; i++) {
      GError *parse_error = NULL;
      GhNip46Credential *existing = parse_item(g_ptr_array_index(items, i), &parse_error);
      gh_nip46_credential_free(existing);
      if (g_error_matches(parse_error, GH_NIP46_CREDENTIAL_ERROR,
                          GH_NIP46_CREDENTIAL_ERROR_NEWER_VERSION)) {
        error = parse_error;
        goto items_fail;
      }
      g_clear_error(&parse_error); /* malformed v1 is removable for repair */
    }
    goto mutate;
  }
  if (items->len == 0 && op->kind == OP_LOOKUP) {
    error = credential_error(GH_NIP46_CREDENTIAL_ERROR_NOT_FOUND);
    goto items_fail;
  }
  if (items->len == 1) {
    GhNip46CredentialItem *item = g_ptr_array_index(items, 0);
    GhNip46Credential *existing = parse_item(item, &error);
    if (!existing) goto items_fail;
    if (op->kind == OP_LOOKUP) {
      g_ptr_array_unref(items);
      g_mutex_unlock(&self->mutex);
      g_task_return_pointer(task, existing, (GDestroyNotify)gh_nip46_credential_free);
      return;
    }
    gh_nip46_credential_free(existing);
  }
mutate:
  g_ptr_array_unref(items);
  if (cancelled(cancellable, &error)) goto unlock_fail;
  /* Once a mutating call begins it is not cancelled: the caller sees the
   * confirmed outcome rather than a false cancellation after a committed write. */
  gboolean ok;
  if (op->kind == OP_STORE) {
    GBytes *secret = serialize(op->credential);
    ok = self->backend->write(self->backend, op->account, secret, op->interactive, &error);
    g_bytes_unref(secret);
  } else {
    ok = self->backend->remove(self->backend, op->account, op->interactive, &error);
  }
  g_mutex_unlock(&self->mutex);
  if (!ok) goto fail;
  g_task_return_boolean(task, TRUE);
  return;
items_fail:
  g_ptr_array_unref(items);
unlock_fail:
  g_mutex_unlock(&self->mutex);
fail:
  g_task_return_error(task, error);
}

static void
start(GhNip46CredentialStore *self, Operation *op, GCancellable *cancellable,
      GAsyncReadyCallback callback, gpointer user_data, gpointer tag)
{
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  /* Reads complete as soon as they are cancelled even if the backend thread
   * is still parked on a prompt; mutations report their confirmed outcome. */
  if (op->kind == OP_LIST || op->kind == OP_LOOKUP)
    g_task_set_return_on_cancel(task, TRUE);
  else
    g_task_set_check_cancellable(task, FALSE);
  g_task_set_source_tag(task, tag);
  g_task_set_task_data(task, op, (GDestroyNotify)operation_free);
  g_task_run_in_thread(task, run_operation);
  g_object_unref(task);
}
void
gh_nip46_credential_store_list_async(GhNip46CredentialStore *self, GCancellable *c,
                                          GAsyncReadyCallback callback, gpointer data)
{
  g_return_if_fail(GH_IS_NIP46_CREDENTIAL_STORE(self));
  start(self, g_new0(Operation, 1), c, callback, data, gh_nip46_credential_store_list_async);
}
GPtrArray *
gh_nip46_credential_store_list_finish(GhNip46CredentialStore *self, GAsyncResult *r, GError **e)
{
  g_return_val_if_fail(g_task_is_valid(r, self), NULL);
  return g_task_propagate_pointer(G_TASK(r), e);
}
void
gh_nip46_credential_store_lookup_async(GhNip46CredentialStore *self, const gchar *account,
                                              GCancellable *c, GAsyncReadyCallback callback, gpointer data)
{
  g_return_if_fail(GH_IS_NIP46_CREDENTIAL_STORE(self));
  Operation *op = g_new0(Operation, 1);
  op->kind = OP_LOOKUP; op->account = g_strdup(account);
  start(self, op, c, callback, data, gh_nip46_credential_store_lookup_async);
}
GhNip46Credential *
gh_nip46_credential_store_lookup_finish(GhNip46CredentialStore *self, GAsyncResult *r, GError **e)
{
  g_return_val_if_fail(g_task_is_valid(r, self), NULL);
  return g_task_propagate_pointer(G_TASK(r), e);
}
void
gh_nip46_credential_store_store_async(GhNip46CredentialStore *self, const GhNip46Credential *credential,
                                             gboolean interactive, GCancellable *c,
                                             GAsyncReadyCallback callback, gpointer data)
{
  g_return_if_fail(GH_IS_NIP46_CREDENTIAL_STORE(self));
  g_return_if_fail(credential != NULL);
  Operation *op = g_new0(Operation, 1);
  op->kind = OP_STORE; op->account = g_strdup(credential->user);
  op->credential = gh_nip46_credential_new(credential->user, credential->signer,
    credential->client_secret, (const gchar * const *)credential->relays, NULL);
  op->interactive = interactive;
  start(self, op, c, callback, data, gh_nip46_credential_store_store_async);
}
gboolean
gh_nip46_credential_store_store_finish(GhNip46CredentialStore *self, GAsyncResult *r, GError **e)
{
  g_return_val_if_fail(g_task_is_valid(r, self), FALSE);
  return g_task_propagate_boolean(G_TASK(r), e);
}
void
gh_nip46_credential_store_delete_async(GhNip46CredentialStore *self, const gchar *account,
                                              gboolean interactive, GCancellable *c,
                                              GAsyncReadyCallback callback, gpointer data)
{
  g_return_if_fail(GH_IS_NIP46_CREDENTIAL_STORE(self));
  Operation *op = g_new0(Operation, 1);
  op->kind = OP_DELETE; op->account = g_strdup(account); op->interactive = interactive;
  start(self, op, c, callback, data, gh_nip46_credential_store_delete_async);
}
gboolean
gh_nip46_credential_store_delete_finish(GhNip46CredentialStore *self, GAsyncResult *r, GError **e)
{
  g_return_val_if_fail(g_task_is_valid(r, self), FALSE);
  return g_task_propagate_boolean(G_TASK(r), e);
}
