/* accounts_store.c - Multi-account management implementation
 *
 * Persists account metadata to:
 * - ~/.config/gnostr-signer/accounts.ini (primary storage)
 * - GSettings for default-identity and account-order (integration)
 * Integrates with secret_store for secure key operations.
 * Supports multiple key types via key_provider (nostrc-bq0).
 *
 * Error Handling:
 * - Uses GN_SIGNER_ERROR domain for general errors
 * - Uses g_set_error_literal() for static messages
 * - Uses g_propagate_error() to pass errors up from underlying stores
 * - Uses g_prefix_error() to add context to propagated errors
 */
#include "accounts_store.h"
#include "gn-signer-error.h"
#include "secret_store.h"
#include "settings_manager.h"
#include "secure-delete.h"
#include "secure-mem.h"
#include "secure-memory.h"
#include "key_provider.h"
#include "key_provider_secp256k1.h"
#include <nostr_nip19.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

/* Change callback entry */
typedef struct {
  guint id;
  AccountsChangedCb cb;
  gpointer user_data;
} ChangeHandler;

struct _AccountsStore {
  GHashTable *map;   /* id -> label */
  GHashTable *watch_only_set;  /* Set of watch-only account IDs */
  GHashTable *key_types;       /* id -> GnKeyType (nostrc-bq0) */
  gchar *active;
  gchar *path;
  SettingsManager *settings;  /* GSettings integration */
  GPtrArray *handlers;        /* Change notification handlers */
  guint next_handler_id;
};

/* Singleton instance */
static AccountsStore *default_instance = NULL;

/* Emit change notification */
static void emit_change(AccountsStore *as, AccountsChangeType change, const gchar *id) {
  if (!as || !as->handlers) return;
  for (guint i = 0; i < as->handlers->len; i++) {
    ChangeHandler *h = g_ptr_array_index(as->handlers, i);
    if (h && h->cb) {
      h->cb(as, change, id, h->user_data);
    }
  }
}

static const char *config_path(void) {
  static gchar *p = NULL;
  if (!p) {
    const char *conf = g_get_user_config_dir();
    gchar *dir = g_build_filename(conf, "gnostr-signer", NULL);
    g_mkdir_with_parents(dir, 0700);
    p = g_build_filename(dir, "accounts.ini", NULL);
    g_free(dir);
  }
  return p;
}

static void change_handler_free(gpointer data) {
  g_free(data);
}

AccountsStore *accounts_store_new(void) {
  AccountsStore *as = g_new0(AccountsStore, 1);
  as->map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  as->watch_only_set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  as->key_types = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  as->path = g_strdup(config_path());
  as->settings = settings_manager_get_default();
  as->handlers = g_ptr_array_new_with_free_func(change_handler_free);
  as->next_handler_id = 1;
  return as;
}

AccountsStore *accounts_store_get_default(void) {
  if (!default_instance) {
    default_instance = accounts_store_new();
    /* Note: Errors during load are logged but not fatal for singleton */
    GError *error = NULL;
    if (!accounts_store_load(default_instance, &error)) {
      if (error) {
        g_warning("accounts_store_get_default: load failed: %s", error->message);
        g_clear_error(&error);
      }
    }
    accounts_store_sync_with_secrets(default_instance);
  }
  return default_instance;
}

void accounts_store_free(AccountsStore *as) {
  if (!as) return;
  if (as->map) g_hash_table_destroy(as->map);
  if (as->watch_only_set) g_hash_table_destroy(as->watch_only_set);
  if (as->key_types) g_hash_table_destroy(as->key_types);
  if (as->handlers) g_ptr_array_unref(as->handlers);
  g_free(as->active);
  g_free(as->path);
  /* settings is singleton, don't free */
  if (as == default_instance) default_instance = NULL;
  g_free(as);
}

gboolean accounts_store_load(AccountsStore *as, GError **error) {
  if (!as) {
    g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_INVALID_INPUT,
                        "AccountsStore is NULL");
    return FALSE;
  }

  GKeyFile *kf = g_key_file_new();
  GError *local_error = NULL;

  if (!g_key_file_load_from_file(kf, as->path, G_KEY_FILE_NONE, &local_error)) {
    /* File not existing is not an error - try GSettings fallback */
    if (g_error_matches(local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
      g_clear_error(&local_error);
      g_key_file_unref(kf);

      /* Try loading from GSettings if INI file doesn't exist */
      if (as->settings) {
        gchar **order = settings_manager_get_account_order(as->settings);
        if (order) {
          for (gsize i = 0; order[i]; i++) {
            const gchar *npub = order[i];
            if (npub && *npub && !g_hash_table_contains(as->map, npub)) {
              gchar *label = settings_manager_get_identity_label(as->settings, npub);
              g_hash_table_replace(as->map, g_strdup(npub), label ? label : g_strdup(""));
            }
          }
          g_strfreev(order);
        }

        /* Load active from GSettings */
        const gchar *default_id = settings_manager_get_default_identity(as->settings);
        if (default_id && *default_id && !as->active) {
          as->active = g_strdup(default_id);
        }
      }
      return TRUE;
    }

    /* Other file errors are propagated */
    g_propagate_error(error, local_error);
    g_prefix_error(error, "Failed to load accounts file: ");
    g_key_file_unref(kf);
    return FALSE;
  }

  /* Load accounts group */
  gsize nkeys = 0;
  gchar **keys = g_key_file_get_keys(kf, "accounts", &nkeys, NULL);
  for (gsize i = 0; i < nkeys; i++) {
    const gchar *id = keys[i];
    gchar *label = g_key_file_get_string(kf, "accounts", id, NULL);
    g_hash_table_replace(as->map, g_strdup(id), label ? label : g_strdup(""));
  }
  g_strfreev(keys);

  /* Load watch-only accounts group */
  nkeys = 0;
  keys = g_key_file_get_keys(kf, "watch_only", &nkeys, NULL);
  for (gsize i = 0; i < nkeys; i++) {
    const gchar *id = keys[i];
    g_hash_table_add(as->watch_only_set, g_strdup(id));
    /* Also add to main map if not already there */
    if (!g_hash_table_contains(as->map, id)) {
      gchar *label = g_key_file_get_string(kf, "watch_only", id, NULL);
      g_hash_table_replace(as->map, g_strdup(id), label ? label : g_strdup(""));
    }
  }
  g_strfreev(keys);

  /* Load key types group (nostrc-bq0) */
  nkeys = 0;
  keys = g_key_file_get_keys(kf, "key_types", &nkeys, NULL);
  for (gsize i = 0; i < nkeys; i++) {
    const gchar *id = keys[i];
    gchar *type_str = g_key_file_get_string(kf, "key_types", id, NULL);
    if (type_str) {
      GnKeyType type = gn_key_type_from_string(type_str);
      if (type != GN_KEY_TYPE_UNKNOWN) {
        g_hash_table_replace(as->key_types, g_strdup(id), GINT_TO_POINTER(type));
      }
      g_free(type_str);
    }
  }
  g_strfreev(keys);

  /* Load active state */
  gchar *active = g_key_file_get_string(kf, "state", "active", NULL);
  if (active) {
    g_free(as->active);
    as->active = active;
  }

  g_key_file_unref(kf);

  /* Sync labels from GSettings (GSettings may have newer labels) */
  if (as->settings) {
    GHashTableIter it;
    gpointer key;
    g_hash_table_iter_init(&it, as->map);
    while (g_hash_table_iter_next(&it, &key, NULL)) {
      const gchar *npub = (const gchar*)key;
      gchar *gs_label = settings_manager_get_identity_label(as->settings, npub);
      if (gs_label && *gs_label) {
        g_hash_table_replace(as->map, g_strdup(npub), gs_label);
      } else {
        g_free(gs_label);
      }
    }
  }

  return TRUE;
}

gboolean accounts_store_save(AccountsStore *as, GError **error) {
  if (!as) {
    g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_INVALID_INPUT,
                        "AccountsStore is NULL");
    return FALSE;
  }

  GKeyFile *kf = g_key_file_new();

  /* Build account order array for GSettings */
  GPtrArray *order_arr = g_ptr_array_new();

  /* Write accounts */
  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init(&it, as->map);
  while (g_hash_table_iter_next(&it, &key, &val)) {
    const gchar *id = (const gchar*)key;
    const gchar *label = (const gchar*)val;
    /* Write to appropriate group based on watch-only status */
    if (g_hash_table_contains(as->watch_only_set, id)) {
      g_key_file_set_string(kf, "watch_only", id, label);
    } else {
      g_key_file_set_string(kf, "accounts", id, label);
    }
    g_ptr_array_add(order_arr, (gpointer)key);
  }

  /* Write key types (nostrc-bq0) */
  g_hash_table_iter_init(&it, as->key_types);
  while (g_hash_table_iter_next(&it, &key, &val)) {
    const gchar *id = (const gchar*)key;
    GnKeyType type = GPOINTER_TO_INT(val);
    if (type != GN_KEY_TYPE_UNKNOWN) {
      g_key_file_set_string(kf, "key_types", id, gn_key_type_to_string(type));
    }
  }

  /* Write state */
  if (as->active) {
    g_key_file_set_string(kf, "state", "active", as->active);
  }

  /* Save to file */
  gsize len = 0;
  gchar *data = g_key_file_to_data(kf, &len, NULL);
  gboolean success = TRUE;

  if (data) {
    GError *local_error = NULL;
    if (!g_file_set_contents(as->path, data, len, &local_error)) {
      g_propagate_error(error, local_error);
      g_prefix_error(error, "Failed to save accounts file: ");
      success = FALSE;
    }
    g_free(data);
  } else {
    g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_INTERNAL,
                        "Failed to serialize accounts data");
    success = FALSE;
  }

  g_key_file_unref(kf);

  /* Sync to GSettings even if file save failed */
  if (as->settings) {
    /* Update account order */
    g_ptr_array_add(order_arr, NULL);  /* NULL terminate */
    settings_manager_set_account_order(as->settings, (const gchar *const *)order_arr->pdata);

    /* Update default identity */
    if (as->active) {
      settings_manager_set_default_identity(as->settings, as->active);
    }

    /* Update identity labels */
    g_hash_table_iter_init(&it, as->map);
    while (g_hash_table_iter_next(&it, &key, &val)) {
      const gchar *npub = (const gchar*)key;
      const gchar *label = (const gchar*)val;
      if (label && *label) {
        settings_manager_set_identity_label(as->settings, npub, label);
      }
    }
  }

  g_ptr_array_free(order_arr, TRUE);
  return success;
}

gboolean accounts_store_add(AccountsStore *as, const gchar *id,
                            const gchar *label, GError **error) {
  if (!as) {
    g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_INVALID_INPUT,
                        "AccountsStore is NULL");
    return FALSE;
  }

  if (!id || !*id) {
    g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_INVALID_INPUT,
                        "Account ID cannot be empty");
    return FALSE;
  }

  if (g_hash_table_contains(as->map, id)) {
    g_set_error(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_ALREADY_EXISTS,
                "Account '%s' already exists", id);
    return FALSE;
  }

  g_hash_table_insert(as->map, g_strdup(id), g_strdup(label ? label : ""));

  /* Set as active if this is the first account */
  gboolean was_first = !as->active;
  if (was_first) {
    as->active = g_strdup(id);
  }

  /* Emit change notification */
  emit_change(as, ACCOUNTS_CHANGE_ADDED, id);
  if (was_first) {
    emit_change(as, ACCOUNTS_CHANGE_ACTIVE, id);
  }

  return TRUE;
}

gboolean accounts_store_remove(AccountsStore *as, const gchar *id, GError **error) {
  if (!as) {
    g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_INVALID_INPUT,
                        "AccountsStore is NULL");
    return FALSE;
  }

  if (!id || !*id) {
    g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_INVALID_INPUT,
                        "Account ID cannot be empty");
    return FALSE;
  }

  if (!g_hash_table_contains(as->map, id)) {
    g_set_error(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_NOT_FOUND,
                "Account '%s' not found", id);
    return FALSE;
  }

  g_hash_table_remove(as->map, id);
  g_hash_table_remove(as->watch_only_set, id);
  g_hash_table_remove(as->key_types, id);

  /* Securely delete any local files associated with this identity
   * Note: This does NOT delete from secure storage (Keychain/libsecret)
   * That must be done separately via secret_store_remove()
   */
  GnDeleteResult del_result = gn_secure_delete_identity_files(id);
  if (del_result != GN_DELETE_OK) {
    /* Log warning but don't fail the remove operation */
    g_warning("accounts_store_remove: secure delete of identity files failed for %s: %s",
              id, gn_delete_result_to_string(del_result));
  }

  emit_change(as, ACCOUNTS_CHANGE_REMOVED, id);

  if (as->active && g_strcmp0(as->active, id) == 0) {
    /* Pick a new active if any remain */
    g_clear_pointer(&as->active, g_free);

    GHashTableIter it;
    gpointer key = NULL, val = NULL;
    g_hash_table_iter_init(&it, as->map);
    if (g_hash_table_iter_next(&it, &key, &val)) {
      as->active = g_strdup((const gchar*)key);
      emit_change(as, ACCOUNTS_CHANGE_ACTIVE, as->active);
    }
  }

  return TRUE;
}

void accounts_store_entry_free(AccountEntry *entry) {
  if (!entry) return;
  g_free(entry->id);
  g_free(entry->label);
  g_free(entry);
}

GPtrArray *accounts_store_list(AccountsStore *as) {
  if (!as) return NULL;

  GPtrArray *arr = g_ptr_array_new_with_free_func((GDestroyNotify)accounts_store_entry_free);

  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init(&it, as->map);
  while (g_hash_table_iter_next(&it, &key, &val)) {
    AccountEntry *e = g_new0(AccountEntry, 1);
    e->id = g_strdup((const gchar*)key);
    e->label = g_strdup((const gchar*)val);

    /* Check if this is a watch-only account */
    e->watch_only = g_hash_table_contains(as->watch_only_set, e->id);

    /* Get key type (default to secp256k1 for Nostr) */
    gpointer key_type_ptr = g_hash_table_lookup(as->key_types, e->id);
    e->key_type = key_type_ptr ? GPOINTER_TO_INT(key_type_ptr) : GN_KEY_TYPE_SECP256K1;

    /* Check if secret exists - nsec is in secure memory, must free securely */
    gchar *nsec = NULL;
    e->has_secret = (secret_store_get_secret(e->id, &nsec) == SECRET_STORE_OK);
    if (nsec) {
      gnostr_secure_strfree(nsec);
    }

    g_ptr_array_add(arr, e);
  }

  return arr;
}

gboolean accounts_store_set_active(AccountsStore *as, const gchar *id, GError **error) {
  if (!as) {
    g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_INVALID_INPUT,
                        "AccountsStore is NULL");
    return FALSE;
  }

  if (id && *id && !g_hash_table_contains(as->map, id)) {
    g_set_error(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_NOT_FOUND,
                "Account '%s' not found", id);
    return FALSE;
  }

  /* Check if actually changing */
  gboolean changed = (g_strcmp0(as->active, id) != 0);

  g_free(as->active);
  as->active = id ? g_strdup(id) : NULL;

  /* Also update GSettings immediately */
  if (as->settings && id) {
    settings_manager_set_default_identity(as->settings, id);
  }

  /* Emit change notification */
  if (changed) {
    emit_change(as, ACCOUNTS_CHANGE_ACTIVE, id);
  }

  return TRUE;
}

gboolean accounts_store_get_active(AccountsStore *as, gchar **out_id, GError **error) {
  if (!as) {
    g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_INVALID_INPUT,
                        "AccountsStore is NULL");
    return FALSE;
  }

  if (!as->active) {
    g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_NOT_FOUND,
                        "No active account set");
    return FALSE;
  }

  if (out_id) *out_id = g_strdup(as->active);
  return TRUE;
}

gboolean accounts_store_set_label(AccountsStore *as, const gchar *id,
                                  const gchar *label, GError **error) {
  if (!as) {
    g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_INVALID_INPUT,
                        "AccountsStore is NULL");
    return FALSE;
  }

  if (!id || !*id) {
    g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_INVALID_INPUT,
                        "Account ID cannot be empty");
    return FALSE;
  }

  if (!g_hash_table_contains(as->map, id)) {
    g_set_error(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_NOT_FOUND,
                "Account '%s' not found", id);
    return FALSE;
  }

  g_hash_table_replace(as->map, g_strdup(id), g_strdup(label ? label : ""));

  /* Also update in secret store */
  secret_store_set_label(id, label);

  /* Also update in GSettings */
  if (as->settings) {
    settings_manager_set_identity_label(as->settings, id, label);
  }

  /* Emit change notification */
  emit_change(as, ACCOUNTS_CHANGE_LABEL, id);

  return TRUE;
}

guint accounts_store_count(AccountsStore *as) {
  if (!as) return 0;
  return g_hash_table_size(as->map);
}

gboolean accounts_store_exists(AccountsStore *as, const gchar *id) {
  if (!as || !id) return FALSE;
  return g_hash_table_contains(as->map, id);
}

AccountEntry *accounts_store_find(AccountsStore *as, const gchar *query) {
  if (!as || !query || !*query) return NULL;

  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init(&it, as->map);
  while (g_hash_table_iter_next(&it, &key, &val)) {
    const gchar *id = (const gchar*)key;
    const gchar *label = (const gchar*)val;

    /* Match by exact id or partial match */
    if (g_strcmp0(id, query) == 0 ||
        (label && g_str_has_prefix(label, query)) ||
        g_str_has_prefix(id, query)) {
      AccountEntry *e = g_new0(AccountEntry, 1);
      e->id = g_strdup(id);
      e->label = g_strdup(label);

      /* Check if this is a watch-only account */
      e->watch_only = g_hash_table_contains(as->watch_only_set, id);

      /* Get key type (default to secp256k1 for Nostr) */
      gpointer key_type_ptr = g_hash_table_lookup(as->key_types, id);
      e->key_type = key_type_ptr ? GPOINTER_TO_INT(key_type_ptr) : GN_KEY_TYPE_SECP256K1;

      /* nsec is in secure memory, must free securely */
      gchar *nsec = NULL;
      e->has_secret = (secret_store_get_secret(id, &nsec) == SECRET_STORE_OK);
      if (nsec) {
        gnostr_secure_strfree(nsec);
      }

      return e;
    }
  }

  return NULL;
}

void accounts_store_sync_with_secrets(AccountsStore *as) {
  if (!as) return;

  /* Get all identities from secret storage */
  GPtrArray *secrets = secret_store_list();
  if (!secrets) return;

  for (guint i = 0; i < secrets->len; i++) {
    SecretStoreEntry *se = g_ptr_array_index(secrets, i);
    if (!se || !se->npub) continue;

    /* Add if not already tracked */
    if (!g_hash_table_contains(as->map, se->npub)) {
      g_hash_table_insert(as->map, g_strdup(se->npub),
                          g_strdup(se->label ? se->label : ""));

      /* Set as active if first */
      if (!as->active) {
        as->active = g_strdup(se->npub);
      }
    }
  }

  g_ptr_array_unref(secrets);
}

gboolean accounts_store_import_key(AccountsStore *as, const gchar *key,
                                   const gchar *label, gchar **out_npub,
                                   GError **error) {
  if (!as) {
    g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_INVALID_INPUT,
                        "AccountsStore is NULL");
    return FALSE;
  }

  if (!key || !*key) {
    g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_INVALID_INPUT,
                        "Private key cannot be empty");
    return FALSE;
  }

  if (out_npub) *out_npub = NULL;

  /* Store in secure storage */
  SecretStoreResult rc = secret_store_add(key, label, TRUE);
  if (rc != SECRET_STORE_OK) {
    g_set_error(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_BACKEND_FAILED,
                "Failed to store key in secure storage: %s",
                secret_store_result_to_string(rc));
    return FALSE;
  }

  /* Get the npub for this key */
  gchar *npub = NULL;
  rc = secret_store_get_public_key(NULL, &npub);
  if (rc != SECRET_STORE_OK || !npub) {
    g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_CRYPTO_FAILED,
                        "Failed to derive public key from imported key");
    return FALSE;
  }

  /* Add to our tracking */
  GError *add_error = NULL;
  if (!accounts_store_add(as, npub, label, &add_error)) {
    /* If already exists, that's okay - just log and continue */
    if (!g_error_matches(add_error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_ALREADY_EXISTS)) {
      g_propagate_error(error, add_error);
      g_prefix_error(error, "Failed to track imported key: ");
      g_free(npub);
      return FALSE;
    }
    g_clear_error(&add_error);
  }

  if (out_npub) {
    *out_npub = npub;
  } else {
    g_free(npub);
  }

  return TRUE;
}

gboolean accounts_store_generate_key(AccountsStore *as, const gchar *label,
                                     gchar **out_npub, GError **error) {
  if (!as) {
    g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_INVALID_INPUT,
                        "AccountsStore is NULL");
    return FALSE;
  }

  if (out_npub) *out_npub = NULL;

  gchar *npub = NULL;
  SecretStoreResult rc = secret_store_generate(label, TRUE, &npub);
  if (rc != SECRET_STORE_OK || !npub) {
    g_set_error(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_CRYPTO_FAILED,
                "Failed to generate keypair: %s",
                secret_store_result_to_string(rc));
    return FALSE;
  }

  /* Add to our tracking */
  GError *add_error = NULL;
  if (!accounts_store_add(as, npub, label, &add_error)) {
    g_propagate_error(error, add_error);
    g_prefix_error(error, "Failed to track generated key: ");
    g_free(npub);
    return FALSE;
  }

  if (out_npub) {
    *out_npub = npub;
  } else {
    g_free(npub);
  }

  return TRUE;
}

gchar *accounts_store_get_display_name(AccountsStore *as, const gchar *id) {
  if (!as || !id) return NULL;

  gpointer label = g_hash_table_lookup(as->map, id);
  if (label && *(const gchar*)label) {
    return g_strdup((const gchar*)label);
  }

  /* Truncate npub for display */
  gsize len = strlen(id);
  if (len > 16) {
    return g_strdup_printf("%.8s...%.4s", id, id + len - 4);
  }

  return g_strdup(id);
}

/* Helper to check if a string is 64-character hex */
static gboolean is_hex64(const char *s) {
  if (!s) return FALSE;
  size_t n = strlen(s);
  if (n != 64) return FALSE;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (!isxdigit((unsigned char)c)) return FALSE;
  }
  return TRUE;
}

/* Helper to convert hex to bytes */
static gboolean hex_to_bytes(const char *hex, uint8_t *out, size_t out_len) {
  size_t hex_len = strlen(hex);
  if (hex_len != out_len * 2) return FALSE;
  for (size_t i = 0; i < out_len; i++) {
    unsigned int byte;
    if (sscanf(hex + 2*i, "%2x", &byte) != 1) return FALSE;
    out[i] = (uint8_t)byte;
  }
  return TRUE;
}

gboolean accounts_store_import_pubkey(AccountsStore *as, const gchar *pubkey,
                                      const gchar *label, gchar **out_npub,
                                      GError **error) {
  if (!as) {
    g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_INVALID_INPUT,
                        "AccountsStore is NULL");
    return FALSE;
  }

  if (!pubkey || !*pubkey) {
    g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_INVALID_INPUT,
                        "Public key cannot be empty");
    return FALSE;
  }

  if (out_npub) *out_npub = NULL;

  gchar *npub = NULL;

  /* Parse input: can be npub or 64-char hex */
  if (g_str_has_prefix(pubkey, "npub1")) {
    /* Validate npub by decoding, then re-encode to normalize */
    GNostrNip19 *nip19 = gnostr_nip19_decode(pubkey, NULL);
    if (!nip19 || gnostr_nip19_get_entity_type(nip19) != GNOSTR_BECH32_NPUB) {
      if (nip19) g_object_unref(nip19);
      g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_INVALID_INPUT,
                          "Invalid npub format");
      return FALSE;
    }
    /* Re-encode from decoded hex pubkey to normalize */
    const gchar *pk_hex = gnostr_nip19_get_pubkey(nip19);
    GNostrNip19 *encoded = gnostr_nip19_encode_npub(pk_hex, NULL);
    g_object_unref(nip19);
    if (!encoded) {
      g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_CRYPTO_FAILED,
                          "Failed to encode npub");
      return FALSE;
    }
    npub = g_strdup(gnostr_nip19_get_bech32(encoded));
    g_object_unref(encoded);
  } else if (is_hex64(pubkey)) {
    /* Encode hex pubkey as npub via GNostrNip19 */
    GNostrNip19 *nip19 = gnostr_nip19_encode_npub(pubkey, NULL);
    if (!nip19) {
      g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_CRYPTO_FAILED,
                          "Failed to encode npub from hex");
      return FALSE;
    }
    npub = g_strdup(gnostr_nip19_get_bech32(nip19));
    g_object_unref(nip19);
  } else {
    g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_INVALID_INPUT,
                        "Unrecognized format: expected npub1... or 64-character hex");
    return FALSE;
  }

  /* Check if already exists */
  if (g_hash_table_contains(as->map, npub)) {
    g_set_error(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_ALREADY_EXISTS,
                "Account '%s' already exists", npub);
    g_free(npub);
    return FALSE;
  }

  /* Add to our tracking as watch-only */
  g_hash_table_insert(as->map, g_strdup(npub), g_strdup(label ? label : ""));
  g_hash_table_add(as->watch_only_set, g_strdup(npub));

  /* Set as active if this is the first account */
  gboolean was_first = !as->active;
  if (was_first) {
    as->active = g_strdup(npub);
  }

  /* Emit change notification */
  emit_change(as, ACCOUNTS_CHANGE_ADDED, npub);
  if (was_first) {
    emit_change(as, ACCOUNTS_CHANGE_ACTIVE, npub);
  }

  if (out_npub) {
    *out_npub = npub;
  } else {
    g_free(npub);
  }

  return TRUE;
}

gboolean accounts_store_is_watch_only(AccountsStore *as, const gchar *id) {
  if (!as || !id) return FALSE;
  return g_hash_table_contains(as->watch_only_set, id);
}

guint accounts_store_connect_changed(AccountsStore *as, AccountsChangedCb cb,
                                     gpointer user_data) {
  if (!as || !cb) return 0;

  ChangeHandler *h = g_new0(ChangeHandler, 1);
  h->id = as->next_handler_id++;
  h->cb = cb;
  h->user_data = user_data;

  g_ptr_array_add(as->handlers, h);
  return h->id;
}

void accounts_store_disconnect_changed(AccountsStore *as, guint handler_id) {
  if (!as || handler_id == 0) return;

  for (guint i = 0; i < as->handlers->len; i++) {
    ChangeHandler *h = g_ptr_array_index(as->handlers, i);
    if (h && h->id == handler_id) {
      g_ptr_array_remove_index(as->handlers, i);
      return;
    }
  }
}

/* ======== Async API implementation ======== */

typedef struct {
  AccountsStore *as;
  AccountsStoreSyncCallback callback;
  gpointer user_data;
  GPtrArray *secrets;  /* Result from async secret store list */
} SyncAsyncData;

/* Called in main thread after secret list completes */
static void sync_secrets_list_cb(GPtrArray *entries, gpointer user_data) {
  SyncAsyncData *data = user_data;
  AccountsStore *as = data->as;

  if (entries && as) {
    for (guint i = 0; i < entries->len; i++) {
      SecretStoreEntry *se = g_ptr_array_index(entries, i);
      if (!se || !se->npub) continue;

      /* Add if not already tracked */
      if (!g_hash_table_contains(as->map, se->npub)) {
        g_hash_table_insert(as->map, g_strdup(se->npub),
                            g_strdup(se->label ? se->label : ""));

        /* Set as active if first */
        if (!as->active) {
          as->active = g_strdup(se->npub);
        }
      }
    }
    g_ptr_array_unref(entries);
  }

  /* Invoke callback */
  if (data->callback) {
    data->callback(as, data->user_data);
  }

  g_free(data);
}

void accounts_store_sync_with_secrets_async(AccountsStore *as,
                                            AccountsStoreSyncCallback callback,
                                            gpointer user_data) {
  if (!as) {
    if (callback) callback(NULL, user_data);
    return;
  }

  SyncAsyncData *data = g_new0(SyncAsyncData, 1);
  data->as = as;
  data->callback = callback;
  data->user_data = user_data;

  /* Use async secret store list */
  secret_store_list_async(sync_secrets_list_cb, data);
}

/* ======== Key type API (nostrc-bq0) ======== */

GnKeyType accounts_store_get_key_type(AccountsStore *as, const gchar *id) {
  if (!as || !id) return GN_KEY_TYPE_SECP256K1;

  gpointer key_type_ptr = g_hash_table_lookup(as->key_types, id);
  if (key_type_ptr) {
    return GPOINTER_TO_INT(key_type_ptr);
  }

  /* Default to secp256k1 for Nostr compatibility */
  return GN_KEY_TYPE_SECP256K1;
}

gboolean accounts_store_set_key_type(AccountsStore *as,
                                     const gchar *id,
                                     GnKeyType key_type,
                                     GError **error) {
  if (!as) {
    g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_INVALID_INPUT,
                        "AccountsStore is NULL");
    return FALSE;
  }

  if (!id || !*id) {
    g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_INVALID_INPUT,
                        "Account ID cannot be empty");
    return FALSE;
  }

  /* Verify account exists */
  if (!g_hash_table_contains(as->map, id)) {
    g_set_error(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_NOT_FOUND,
                "Account '%s' not found", id);
    return FALSE;
  }

  g_hash_table_replace(as->key_types, g_strdup(id), GINT_TO_POINTER(key_type));
  return TRUE;
}

gboolean accounts_store_generate_key_with_type(AccountsStore *as,
                                               const gchar *label,
                                               GnKeyType key_type,
                                               gchar **out_npub,
                                               GError **error) {
  if (!as) {
    g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_INVALID_INPUT,
                        "AccountsStore is NULL");
    return FALSE;
  }

  if (out_npub) *out_npub = NULL;

  /* For secp256k1, use existing secret_store_generate */
  if (key_type == GN_KEY_TYPE_SECP256K1 || key_type == GN_KEY_TYPE_UNKNOWN) {
    gchar *npub = NULL;
    SecretStoreResult rc = secret_store_generate(label, TRUE, &npub);
    if (rc != SECRET_STORE_OK || !npub) {
      g_set_error(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_CRYPTO_FAILED,
                  "Failed to generate secp256k1 keypair: %s",
                  secret_store_result_to_string(rc));
      return FALSE;
    }

    /* Add to our tracking */
    GError *add_error = NULL;
    if (!accounts_store_add(as, npub, label, &add_error)) {
      g_propagate_error(error, add_error);
      g_prefix_error(error, "Failed to track generated key: ");
      g_free(npub);
      return FALSE;
    }

    /* Store key type */
    g_hash_table_replace(as->key_types, g_strdup(npub), GINT_TO_POINTER(GN_KEY_TYPE_SECP256K1));

    if (out_npub) {
      *out_npub = npub;
    } else {
      g_free(npub);
    }

    return TRUE;
  }

  /* For other key types, use the key provider interface */
  GnKeyProvider *provider = gn_key_provider_get_for_type(key_type);
  if (!provider) {
    g_set_error(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_NOT_SUPPORTED,
                "No provider available for key type '%s'",
                gn_key_type_to_string(key_type));
    return FALSE;
  }

  /* Generate private key via provider */
  gsize sk_size = gn_key_provider_get_private_key_size(provider);
  guint8 *sk = g_malloc(sk_size);
  gsize sk_len = 0;
  GError *gen_error = NULL;

  if (!gn_key_provider_generate_private_key(provider, sk, &sk_len, &gen_error)) {
    g_propagate_error(error, gen_error);
    g_prefix_error(error, "Key generation failed: ");
    gn_secure_clear_buffer(sk);
    g_free(sk);
    return FALSE;
  }

  /* For now, only secp256k1 is fully supported for storage.
   * Other key types would need additional NIP definitions for encoding.
   * This is a placeholder for future expansion. */
  g_set_error(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_NOT_SUPPORTED,
              "Key type '%s' is not yet supported for storage",
              gn_key_type_to_string(key_type));

  /* Securely clear the key */
  gn_secure_clear_buffer(sk);
  g_free(sk);

  return FALSE;
}

gboolean accounts_store_import_key_with_type(AccountsStore *as,
                                             const gchar *key,
                                             const gchar *label,
                                             GnKeyType key_type,
                                             gchar **out_npub,
                                             GError **error) {
  if (!as) {
    g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_INVALID_INPUT,
                        "AccountsStore is NULL");
    return FALSE;
  }

  if (!key || !*key) {
    g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_INVALID_INPUT,
                        "Private key cannot be empty");
    return FALSE;
  }

  if (out_npub) *out_npub = NULL;

  /* Auto-detect if unknown */
  GnKeyType detected_type = key_type;
  if (detected_type == GN_KEY_TYPE_UNKNOWN) {
    /* Default to secp256k1 for nsec and hex keys */
    if (g_str_has_prefix(key, "nsec1") || is_hex64(key)) {
      detected_type = GN_KEY_TYPE_SECP256K1;
    }
    /* Future: add detection for other key formats */
  }

  /* For secp256k1, use existing import function */
  if (detected_type == GN_KEY_TYPE_SECP256K1) {
    gchar *npub = NULL;
    GError *import_error = NULL;
    if (!accounts_store_import_key(as, key, label, &npub, &import_error)) {
      g_propagate_error(error, import_error);
      return FALSE;
    }

    /* Store key type */
    g_hash_table_replace(as->key_types, g_strdup(npub), GINT_TO_POINTER(GN_KEY_TYPE_SECP256K1));

    if (out_npub) {
      *out_npub = npub;
    } else {
      g_free(npub);
    }

    return TRUE;
  }

  /* Other key types not yet supported for import */
  g_set_error(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_NOT_SUPPORTED,
              "Key type '%s' is not yet supported for import",
              gn_key_type_to_string(detected_type));
  return FALSE;
}
