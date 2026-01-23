/* accounts_store.c - Multi-account management implementation
 *
 * Persists account metadata to:
 * - ~/.config/gnostr-signer/accounts.ini (primary storage)
 * - GSettings for default-identity and account-order (integration)
 * Integrates with secret_store for secure key operations.
 */
#include "accounts_store.h"
#include "secret_store.h"
#include "settings_manager.h"
#include "secure-delete.h"
#include "secure-mem.h"
#include <string.h>

/* Change callback entry */
typedef struct {
  guint id;
  AccountsChangedCb cb;
  gpointer user_data;
} ChangeHandler;

struct _AccountsStore {
  GHashTable *map;   /* id -> label */
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
  as->path = g_strdup(config_path());
  as->settings = settings_manager_get_default();
  as->handlers = g_ptr_array_new_with_free_func(change_handler_free);
  as->next_handler_id = 1;
  return as;
}

AccountsStore *accounts_store_get_default(void) {
  if (!default_instance) {
    default_instance = accounts_store_new();
    accounts_store_load(default_instance);
    accounts_store_sync_with_secrets(default_instance);
  }
  return default_instance;
}

void accounts_store_free(AccountsStore *as) {
  if (!as) return;
  if (as->map) g_hash_table_destroy(as->map);
  if (as->handlers) g_ptr_array_unref(as->handlers);
  g_free(as->active);
  g_free(as->path);
  /* settings is singleton, don't free */
  if (as == default_instance) default_instance = NULL;
  g_free(as);
}

void accounts_store_load(AccountsStore *as) {
  if (!as) return;

  GKeyFile *kf = g_key_file_new();
  GError *err = NULL;

  if (!g_key_file_load_from_file(kf, as->path, G_KEY_FILE_NONE, &err)) {
    if (err) g_clear_error(&err);
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
    return;
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
}

void accounts_store_save(AccountsStore *as) {
  if (!as) return;

  GKeyFile *kf = g_key_file_new();

  /* Build account order array for GSettings */
  GPtrArray *order_arr = g_ptr_array_new();

  /* Write accounts */
  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init(&it, as->map);
  while (g_hash_table_iter_next(&it, &key, &val)) {
    g_key_file_set_string(kf, "accounts", (const gchar*)key, (const gchar*)val);
    g_ptr_array_add(order_arr, (gpointer)key);
  }

  /* Write state */
  if (as->active) {
    g_key_file_set_string(kf, "state", "active", as->active);
  }

  /* Save to file */
  gsize len = 0;
  gchar *data = g_key_file_to_data(kf, &len, NULL);
  if (data) {
    GError *err = NULL;
    if (!g_file_set_contents(as->path, data, len, &err)) {
      if (err) {
        g_warning("accounts_store: save failed: %s", err->message);
        g_clear_error(&err);
      }
    }
    g_free(data);
  }

  g_key_file_unref(kf);

  /* Sync to GSettings */
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
}

gboolean accounts_store_add(AccountsStore *as, const gchar *id, const gchar *label) {
  if (!as || !id || !*id) return FALSE;
  if (g_hash_table_contains(as->map, id)) return FALSE;

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

gboolean accounts_store_remove(AccountsStore *as, const gchar *id) {
  if (!as || !id) return FALSE;

  gboolean removed = g_hash_table_remove(as->map, id);

  if (removed) {
    /* Securely delete any local files associated with this identity
     * Note: This does NOT delete from secure storage (Keychain/libsecret)
     * That must be done separately via secret_store_remove()
     */
    GnDeleteResult del_result = gn_secure_delete_identity_files(id);
    if (del_result != GN_DELETE_OK) {
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
  }

  return removed;
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

void accounts_store_set_active(AccountsStore *as, const gchar *id) {
  if (!as) return;

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
}

gboolean accounts_store_get_active(AccountsStore *as, gchar **out_id) {
  if (!as || !as->active) return FALSE;
  if (out_id) *out_id = g_strdup(as->active);
  return TRUE;
}

gboolean accounts_store_set_label(AccountsStore *as, const gchar *id, const gchar *label) {
  if (!as || !id || !*id) return FALSE;

  if (!g_hash_table_contains(as->map, id)) return FALSE;

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
                                   const gchar *label, gchar **out_npub) {
  if (!as || !key || !*key) return FALSE;
  if (out_npub) *out_npub = NULL;

  /* Store in secure storage */
  SecretStoreResult rc = secret_store_add(key, label, TRUE);
  if (rc != SECRET_STORE_OK) {
    g_warning("accounts_store_import_key: secret_store_add failed: %d", rc);
    return FALSE;
  }

  /* Get the npub for this key */
  gchar *npub = NULL;
  rc = secret_store_get_public_key(NULL, &npub);
  if (rc != SECRET_STORE_OK || !npub) {
    g_warning("accounts_store_import_key: could not get npub");
    return FALSE;
  }

  /* Add to our tracking */
  accounts_store_add(as, npub, label);

  if (out_npub) {
    *out_npub = npub;
  } else {
    g_free(npub);
  }

  return TRUE;
}

gboolean accounts_store_generate_key(AccountsStore *as, const gchar *label,
                                     gchar **out_npub) {
  if (!as) return FALSE;
  if (out_npub) *out_npub = NULL;

  gchar *npub = NULL;
  SecretStoreResult rc = secret_store_generate(label, TRUE, &npub);
  if (rc != SECRET_STORE_OK || !npub) {
    g_warning("accounts_store_generate_key: secret_store_generate failed: %d", rc);
    return FALSE;
  }

  /* Add to our tracking */
  accounts_store_add(as, npub, label);

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
