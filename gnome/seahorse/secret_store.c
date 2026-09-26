/* gnostr-secret — see secret_store.h and org.gnostr.secret.schema.txt. */
#include "secret_store.h"

#include <gio/gio.h>
#include <string.h>

const SecretSchema gnostr_secret_schema = {
  .name = GNOSTR_SECRET_SCHEMA_NAME,
  .flags = SECRET_SCHEMA_NONE,
  .attributes = {
    { "key_id",         SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "npub",           SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "label",          SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "hardware",       SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "owner_uid",      SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "owner_username", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "curve",          SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "origin",         SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "hardware_slot",  SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "fingerprint",    SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "created_at",     SECRET_SCHEMA_ATTRIBUTE_STRING },
    { NULL, 0 }
  }
};

const SecretSchema gnostr_secret_legacy_signer_key_schema = {
  .name = GNOSTR_SECRET_LEGACY_SIGNER_KEY_SCHEMA_NAME,
  .flags = SECRET_SCHEMA_NONE,
  .attributes = {
    { "application", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "label",       SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "npub",        SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "key_type",    SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "created_at",  SECRET_SCHEMA_ATTRIBUTE_STRING },
    { NULL, 0 }
  }
};

const SecretSchema gnostr_secret_legacy_helper_schema = {
  .name = GNOSTR_SECRET_LEGACY_HELPER_SCHEMA_NAME,
  .flags = SECRET_SCHEMA_NONE,
  .attributes = {
    { "type",          SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "npub",          SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "uid",           SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "curve",         SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "origin",        SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "hardware_slot", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { NULL, 0 }
  }
};

const SecretSchema gnostr_secret_legacy_client_schema = {
  .name = GNOSTR_SECRET_LEGACY_CLIENT_SCHEMA_NAME,
  .flags = SECRET_SCHEMA_NONE,
  .attributes = {
    { "npub",        SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "application", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { NULL, 0 }
  }
};

const SecretSchema gnostr_secret_migration_schema = {
  .name = GNOSTR_SECRET_MIGRATION_SCHEMA_NAME,
  .flags = SECRET_SCHEMA_NONE,
  .attributes = {
    { "name", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { NULL, 0 }
  }
};

static gboolean is_set(const gchar *s) { return s && *s; }

/* "npub1" + 8 data chars: enough to tell keys apart at a glance. */
#define GNOSTR_SECRET_LABEL_NPUB_PREFIX 13

gchar *gnostr_secret_store_build_label(const gchar *uid, const gchar *npub){
  if (!is_set(npub)) return NULL;
  gchar *short_npub = strlen(npub) > GNOSTR_SECRET_LABEL_NPUB_PREFIX
    ? g_strdup_printf("%.*s…", GNOSTR_SECRET_LABEL_NPUB_PREFIX, npub)
    : g_strdup(npub);
  gchar *label = is_set(uid)
    ? g_strdup_printf("Nostr key: %s (%s)", uid, short_npub)
    : g_strdup_printf("Nostr key: %s", short_npub);
  g_free(short_npub);
  return label;
}

static void put(GHashTable *attrs, const gchar *key, const gchar *value){
  if (is_set(value)) g_hash_table_replace(attrs, (gpointer)key, g_strdup(value));
}

GHashTable *gnostr_secret_identity_to_attributes(const GnostrSecretIdentity *id){
  if (!id || !is_set(id->npub)) return NULL;
  const gchar *origin = is_set(id->origin) ? id->origin : GNOSTR_SECRET_ORIGIN_SOFTWARE;
  GHashTable *attrs = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
  put(attrs, "key_id", is_set(id->key_id) ? id->key_id : id->npub);
  put(attrs, "npub", id->npub);
  put(attrs, "label", id->label);
  put(attrs, "hardware",
      g_strcmp0(origin, GNOSTR_SECRET_ORIGIN_HARDWARE) == 0 ? "true" : "false");
  put(attrs, "owner_uid", id->owner_uid);
  put(attrs, "owner_username", id->owner_username);
  put(attrs, "curve", GNOSTR_SECRET_CURVE);
  put(attrs, "origin", origin);
  put(attrs, "hardware_slot", id->hardware_slot);
  put(attrs, "fingerprint", id->fingerprint);
  put(attrs, "created_at", id->created_at);
  return attrs;
}

const gchar *gnostr_secret_origin_from_key_type(const gchar *key_type){
  static const gchar *const hardware_types[] = { "hardware", "hsm", "tpm", "pkcs11", NULL };
  if (is_set(key_type)) {
    for (guint i = 0; hardware_types[i]; i++)
      if (g_ascii_strcasecmp(key_type, hardware_types[i]) == 0)
        return GNOSTR_SECRET_ORIGIN_HARDWARE;
  }
  /* "nostr" (the only value secret-storage.c ever wrote), empty, or unknown:
   * the item secret is a software private key. */
  return GNOSTR_SECRET_ORIGIN_SOFTWARE;
}

gboolean gnostr_secret_legacy_to_identity(GnostrSecretLegacyKind kind,
                                          GHashTable *legacy_attrs,
                                          GnostrSecretIdentity *out,
                                          const gchar **why_not){
  g_return_val_if_fail(out != NULL, FALSE);
  memset(out, 0, sizeof *out);
  if (why_not) *why_not = NULL;
  if (!legacy_attrs) {
    if (why_not) *why_not = "no attributes";
    return FALSE;
  }
  const gchar *npub = g_hash_table_lookup(legacy_attrs, "npub");
  out->npub = npub;
  out->key_id = npub;
  switch (kind) {
    case GNOSTR_SECRET_LEGACY_SIGNER_KEY:
      out->label = g_hash_table_lookup(legacy_attrs, "label");
      out->origin = gnostr_secret_origin_from_key_type(
          g_hash_table_lookup(legacy_attrs, "key_type"));
      out->created_at = g_hash_table_lookup(legacy_attrs, "created_at");
      break;
    case GNOSTR_SECRET_LEGACY_HELPER_KEY: {
      const gchar *origin = g_hash_table_lookup(legacy_attrs, "origin");
      out->label = g_hash_table_lookup(legacy_attrs, "uid");
      out->origin = (g_strcmp0(origin, GNOSTR_SECRET_ORIGIN_HARDWARE) == 0)
          ? GNOSTR_SECRET_ORIGIN_HARDWARE : GNOSTR_SECRET_ORIGIN_SOFTWARE;
      out->hardware_slot = g_hash_table_lookup(legacy_attrs, "hardware_slot");
      break;
    }
    case GNOSTR_SECRET_LEGACY_CLIENT_KEY: {
      const gchar *app = g_hash_table_lookup(legacy_attrs, "application");
      if (is_set(app) && strcmp(app, GNOSTR_SECRET_LEGACY_CLIENT_APPLICATION) != 0) {
        if (why_not) *why_not = "application attribute is not " GNOSTR_SECRET_LEGACY_CLIENT_APPLICATION;
        return FALSE;
      }
      /* The client keystore only ever held software nsec keys. */
      out->label = GNOSTR_SECRET_LEGACY_CLIENT_IMPORT_LABEL;
      out->origin = GNOSTR_SECRET_ORIGIN_SOFTWARE;
      break;
    }
    default:
      if (why_not) *why_not = "unknown legacy schema";
      return FALSE;
  }
  if (g_strcmp0(out->origin, GNOSTR_SECRET_ORIGIN_HARDWARE) == 0) {
    if (why_not) *why_not = "hardware key reference (no private key material)";
    return FALSE;
  }
  return TRUE;
}

gboolean gnostr_secret_legacy_label_is_selector(GnostrSecretLegacyKind kind){
  return kind != GNOSTR_SECRET_LEGACY_CLIENT_KEY;
}

/* TRUE when item_attrs carries exactly want's attributes. Namespaced keys
 * ("xdg:schema", any "<ns>:<key>" a Secret Service adds) are ignored; the
 * unified schema has none. */
static gboolean same_attribute_set(GHashTable *item_attrs, GHashTable *want){
  guint own = 0;
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, item_attrs);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    if (strchr((const gchar *)k, ':')) continue;
    own++;
    if (g_strcmp0(g_hash_table_lookup(want, k), v) != 0) return FALSE;
  }
  return own == g_hash_table_size(want);
}

/* Best-effort: after a store, drop other items for the same identity. A
 * store with an identical attribute set replaced its predecessor in place,
 * so only items with a *different* set are stale duplicates.
 *
 * Invariant this relies on: items sharing {key_id, npub} hold the same key
 * (npub is derived from the secret by every writer), so whichever copy a
 * concurrent writer (e.g. the daemon's migration thread) leaves behind is
 * equivalent. Hardware references never reach this path (the migration
 * refuses them and StoreKey only takes private keys).
 *
 * secret_service_get_sync(SECRET_SERVICE_NONE) returns the process-wide
 * shared SecretService, not a new connection. */
static void prune_stale_duplicates(GHashTable *stored){
  SecretService *service = secret_service_get_sync(SECRET_SERVICE_NONE, NULL, NULL);
  if (!service) return;
  GHashTable *match = g_hash_table_new(g_str_hash, g_str_equal);
  g_hash_table_insert(match, (gpointer)"key_id", g_hash_table_lookup(stored, "key_id"));
  g_hash_table_insert(match, (gpointer)"npub", g_hash_table_lookup(stored, "npub"));
  GList *items = secret_service_search_sync(service, &gnostr_secret_schema, match,
                                            SECRET_SEARCH_ALL, NULL, NULL);
  g_hash_table_unref(match);
  for (GList *l = items; l; l = l->next) {
    SecretItem *item = l->data;
    GHashTable *ia = secret_item_get_attributes(item);
    gboolean stale = ia && !same_attribute_set(ia, stored);
    if (ia) g_hash_table_unref(ia);
    if (!stale) continue;
    GError *err = NULL;
    if (!secret_item_delete_sync(item, NULL, &err)) {
      g_debug("gnostr-secret: could not prune stale duplicate: %s",
              err ? err->message : "unknown");
      g_clear_error(&err);
    }
  }
  g_list_free_full(items, g_object_unref);
  g_object_unref(service);
}

gboolean gnostr_secret_store_save(const GnostrSecretIdentity *id,
                                  const gchar *secret,
                                  GError **error){
  if (!is_set(secret)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "secret required");
    return FALSE;
  }
  GHashTable *attrs = gnostr_secret_identity_to_attributes(id);
  if (!attrs) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "npub required");
    return FALSE;
  }
  gchar *label = gnostr_secret_store_build_label(id->label, id->npub);
  gboolean ok = secret_password_storev_sync(&gnostr_secret_schema, attrs,
                                            SECRET_COLLECTION_DEFAULT, label,
                                            secret, NULL, error);
  if (ok) prune_stale_duplicates(attrs);
  g_free(label);
  g_hash_table_unref(attrs);
  return ok;
}

gboolean gnostr_secret_store_save_software_key(const gchar *npub,
                                                const gchar *uid,
                                                const gchar *secret,
                                                GError **error){
  const GnostrSecretIdentity id = {
    .npub = npub,
    .label = uid,
    .origin = GNOSTR_SECRET_ORIGIN_SOFTWARE,
  };
  return gnostr_secret_store_save(&id, secret, error);
}

GHashTable *gnostr_secret_store_find_all(GError **error){
  GHashTable *result = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                             (GDestroyNotify)g_hash_table_unref);
  SecretService *service = secret_service_get_sync(SECRET_SERVICE_NONE, NULL, error);
  if (!service) return result;
  GHashTable *attrs = g_hash_table_new(g_str_hash, g_str_equal);
  GList *items = secret_service_search_sync(service, &gnostr_secret_schema, attrs,
                                            SECRET_SEARCH_ALL, NULL, error);
  g_hash_table_unref(attrs);
  for (GList *l = items; l; l = l->next) {
    GHashTable *ia = secret_item_get_attributes(l->data);
    if (!ia) continue;
    const gchar *npub = g_hash_table_lookup(ia, "npub");
    const gchar *label = g_hash_table_lookup(ia, "label");
    g_hash_table_replace(result,
                         g_strdup_printf("%s|%s", npub ? npub : "", label ? label : ""),
                         ia);
  }
  g_list_free_full(items, g_object_unref);
  g_object_unref(service);
  return result;
}

gboolean gnostr_secret_store_delete_by_identity(const gchar *npub,
                                                const gchar *label,
                                                GError **error){
  if (!is_set(npub) && !is_set(label)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "npub or label required");
    return FALSE;
  }
  SecretService *service = secret_service_get_sync(SECRET_SERVICE_NONE, NULL, error);
  if (!service) return FALSE;

  GHashTable *attrs = g_hash_table_new(g_str_hash, g_str_equal);
  if (is_set(npub)) g_hash_table_insert(attrs, (gpointer)"npub", (gpointer)npub);
  if (is_set(label)) g_hash_table_insert(attrs, (gpointer)"label", (gpointer)label);

  GError *local_err = NULL;
  GList *items = secret_service_search_sync(service, &gnostr_secret_schema, attrs,
                                            SECRET_SEARCH_ALL | SECRET_SEARCH_UNLOCK,
                                            NULL, &local_err);
  g_hash_table_unref(attrs);
  if (local_err) {
    g_propagate_error(error, local_err);
    g_object_unref(service);
    return FALSE;
  }

  gboolean all_ok = TRUE;
  for (GList *l = items; l; l = l->next) {
    if (!secret_item_delete_sync(l->data, NULL, &local_err)) {
      all_ok = FALSE;
      if (error && !*error) g_propagate_error(error, local_err);
      else g_clear_error(&local_err);
      local_err = NULL;
    }
  }
  g_list_free_full(items, g_object_unref);
  g_object_unref(service);
  return all_ok;
}

/* ---- Nostr Wallet Connect pairing (nostrc-yka8) ---- */

const SecretSchema gnostr_secret_wallet_schema = {
  .name = GNOSTR_SECRET_WALLET_SCHEMA_NAME,
  .flags = SECRET_SCHEMA_NONE,
  .attributes = {
    { "wallet_pubkey", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "client_pubkey", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "relay",         SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "lud16",         SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "created_at",    SECRET_SCHEMA_ATTRIBUTE_STRING },
    { NULL, 0 }
  }
};

/* 8 hex chars, like the identity schema's "fingerprint". */
#define GNOSTR_SECRET_LABEL_HEX_PREFIX 8

gchar *gnostr_secret_wallet_build_label(const gchar *lud16,
                                        const gchar *wallet_pubkey){
  if (!is_set(wallet_pubkey)) return NULL;
  gchar *short_pk = strlen(wallet_pubkey) > GNOSTR_SECRET_LABEL_HEX_PREFIX
    ? g_strdup_printf("%.*s…", GNOSTR_SECRET_LABEL_HEX_PREFIX, wallet_pubkey)
    : g_strdup(wallet_pubkey);
  gchar *label = is_set(lud16)
    ? g_strdup_printf("Nostr Wallet Connect: %s (%s)", lud16, short_pk)
    : g_strdup_printf("Nostr Wallet Connect: %s", short_pk);
  g_free(short_pk);
  return label;
}

/* Delete wallet items whose attribute set differs from keep (NULL = all). */
static gboolean wallet_delete_others(GHashTable *keep, GError **error){
  SecretService *service = secret_service_get_sync(SECRET_SERVICE_NONE, NULL, error);
  if (!service) return FALSE;
  GHashTable *match = g_hash_table_new(g_str_hash, g_str_equal);
  GError *local_err = NULL;
  GList *items = secret_service_search_sync(service, &gnostr_secret_wallet_schema, match,
                                            SECRET_SEARCH_ALL | SECRET_SEARCH_UNLOCK,
                                            NULL, &local_err);
  g_hash_table_unref(match);
  if (local_err) {
    g_propagate_error(error, local_err);
    g_object_unref(service);
    return FALSE;
  }
  gboolean all_ok = TRUE;
  for (GList *l = items; l; l = l->next) {
    SecretItem *item = l->data;
    if (keep) {
      GHashTable *ia = secret_item_get_attributes(item);
      gboolean same = ia && same_attribute_set(ia, keep);
      if (ia) g_hash_table_unref(ia);
      if (same) continue;
    }
    if (!secret_item_delete_sync(item, NULL, &local_err)) {
      all_ok = FALSE;
      if (error && !*error) g_propagate_error(error, local_err);
      else g_clear_error(&local_err);
      local_err = NULL;
    }
  }
  g_list_free_full(items, g_object_unref);
  g_object_unref(service);
  return all_ok;
}

gboolean gnostr_secret_wallet_save(const GnostrSecretWallet *wallet,
                                   const gchar *nwc_uri,
                                   GError **error){
  if (!is_set(nwc_uri)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "pairing URI required");
    return FALSE;
  }
  if (!wallet || !is_set(wallet->wallet_pubkey)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "wallet_pubkey required");
    return FALSE;
  }
  GHashTable *attrs = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
  put(attrs, "wallet_pubkey", wallet->wallet_pubkey);
  put(attrs, "client_pubkey", wallet->client_pubkey);
  put(attrs, "relay", wallet->relay);
  put(attrs, "lud16", wallet->lud16);
  put(attrs, "created_at", wallet->created_at);
  gchar *label = gnostr_secret_wallet_build_label(wallet->lud16, wallet->wallet_pubkey);
  gboolean ok = secret_password_storev_sync(&gnostr_secret_wallet_schema, attrs,
                                            SECRET_COLLECTION_DEFAULT, label,
                                            nwc_uri, NULL, error);
  if (ok) {
    GError *prune_err = NULL;
    if (!wallet_delete_others(attrs, &prune_err)) {
      g_debug("gnostr-secret: could not drop previous wallet pairing: %s",
              prune_err ? prune_err->message : "unknown");
      g_clear_error(&prune_err);
    }
  }
  g_free(label);
  g_hash_table_unref(attrs);
  return ok;
}

gchar *gnostr_secret_wallet_lookup(GError **error){
  GHashTable *attrs = g_hash_table_new(g_str_hash, g_str_equal);
  gchar *uri = secret_password_lookupv_sync(&gnostr_secret_wallet_schema, attrs, NULL, error);
  g_hash_table_unref(attrs);
  return uri;
}

gboolean gnostr_secret_wallet_delete_all(GError **error){
  return wallet_delete_others(NULL, error);
}
