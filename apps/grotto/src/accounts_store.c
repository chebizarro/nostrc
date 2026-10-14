/* accounts_store.c - Multi-account management implementation
 *
 * Persists account metadata to:
 * - ~/.config/grotto/accounts.ini (primary storage)
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
#include <nostrc-profile-guard.h>
#include "key_provider.h"
#include "key_provider_secp256k1.h"
#include <nostr-gobject-1.0/nostr_nip19.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdint.h>

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
  GHashTable *signable;        /* last successful daemon ListIdentities snapshot */
  GHashTable *setup;           /* id -> non-secret setup state */
  gchar *active;
  gchar *selected; /* GUI-only, never serialized or used as a signer default */
  gchar *path;
  SettingsManager *settings;  /* GSettings integration */
  GPtrArray *handlers;        /* Change notification handlers */
  guint next_handler_id;
  guint refs, inventory_generation;
  GFileMonitor *monitor;
  GDBusConnection *inventory_bus;
  guint inventory_signal, owner_watch;
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
    gchar *dir = g_build_filename(conf, "grotto", NULL);
    /* nostrc-wqn0u: never the owner's identity list under test. */
    if (nostrc_profile_guard_refuses_path(dir))
      g_error("accounts_store: %s is the real profile; refusing it under test "
              "(NOSTRC_TEST_ISOLATION)", dir);
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
  as->signable = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  as->setup = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  as->path = g_strdup(config_path());
  as->settings = settings_manager_get_default();
  as->handlers = g_ptr_array_new_with_free_func(change_handler_free);
  as->next_handler_id = 1;
  as->refs = 1;
  return as;
}

static void accounts_file_changed(GFileMonitor *monitor, GFile *file, GFile *other,
    GFileMonitorEvent event, gpointer data) {
  (void)monitor; (void)event;
  AccountsStore *as = data;
  g_autofree gchar *path = g_file_get_path(file);
  g_autofree gchar *other_path = other ? g_file_get_path(other) : NULL;
  if (g_strcmp0(path, as->path) && g_strcmp0(other_path, as->path)) return;
  if (accounts_store_load(as, NULL)) {
    emit_change(as, ACCOUNTS_CHANGE_ADDED, NULL);
    accounts_store_sync_with_secrets_async(as, NULL, NULL);
  }
}
static void identities_changed(GDBusConnection *bus, const gchar *sender, const gchar *path,
    const gchar *interface, const gchar *signal, GVariant *args, gpointer data) {
  (void)bus; (void)sender; (void)path; (void)interface; (void)signal; (void)args;
  accounts_store_sync_with_secrets_async(data, NULL, NULL);
}
static void signer_appeared(GDBusConnection *bus, const gchar *name, const gchar *owner, gpointer data) {
  (void)bus; (void)name; (void)owner; accounts_store_sync_with_secrets_async(data, NULL, NULL);
}
static void signer_vanished(GDBusConnection *bus, const gchar *name, gpointer data) {
  (void)bus; (void)name; AccountsStore *as = data;
  as->inventory_generation++; g_hash_table_remove_all(as->signable);
  emit_change(as, ACCOUNTS_CHANGE_ADDED, NULL);
}
static void inventory_bus_ready(GObject *source, GAsyncResult *result, gpointer data) {
  (void)source; AccountsStore *as = data;
  as->inventory_bus = g_bus_get_finish(result, NULL);
  if (as->inventory_bus) {
    as->inventory_signal = g_dbus_connection_signal_subscribe(as->inventory_bus, "org.nostr.Signer", "org.nostr.Signer",
        "IdentitiesChanged", "/org/nostr/signer", NULL, G_DBUS_SIGNAL_FLAGS_NONE, identities_changed, as, NULL);
    as->owner_watch = g_bus_watch_name_on_connection(as->inventory_bus, "org.nostr.Signer", G_BUS_NAME_WATCHER_FLAGS_AUTO_START,
        signer_appeared, signer_vanished, as, NULL);
    /* Subscribe before the initial snapshot: no lost store/remove gap. */
    accounts_store_sync_with_secrets_async(as, NULL, NULL);
  }
  accounts_store_free(as);
}
AccountsStore *accounts_store_get_default(void) {
  if (!default_instance) {
    default_instance = accounts_store_new();
    g_autofree gchar *dir = g_path_get_dirname(default_instance->path);
    g_autoptr(GFile) directory = g_file_new_for_path(dir);
    default_instance->monitor = g_file_monitor_directory(directory, G_FILE_MONITOR_NONE, NULL, NULL);
    if (default_instance->monitor) g_signal_connect(default_instance->monitor, "changed", G_CALLBACK(accounts_file_changed), default_instance);
    /* Observe the INI before the initial transactional read. */
    GError *error = NULL;
    if (!accounts_store_load(default_instance, &error)) {
      if (error) {
        g_warning("accounts_store_get_default: load failed: %s", error->message);
        g_clear_error(&error);
      }
    }
    if (!nostrc_profile_guard_refuses_session_keyring()) {
      default_instance->refs++;
      g_bus_get(G_BUS_TYPE_SESSION, NULL, inventory_bus_ready, default_instance);
    }
  }
  return default_instance;
}

void accounts_store_free(AccountsStore *as) {
  if (!as || --as->refs) return;
  if (as->monitor) { g_file_monitor_cancel(as->monitor); g_clear_object(&as->monitor); }
  if (as->owner_watch) g_bus_unwatch_name(as->owner_watch);
  if (as->inventory_signal) g_dbus_connection_signal_unsubscribe(as->inventory_bus, as->inventory_signal);
  g_clear_object(&as->inventory_bus);
  if (as->map) g_hash_table_destroy(as->map);
  if (as->watch_only_set) g_hash_table_destroy(as->watch_only_set);
  if (as->key_types) g_hash_table_destroy(as->key_types);
  if (as->signable) g_hash_table_destroy(as->signable);
  if (as->setup) g_hash_table_destroy(as->setup);
  if (as->handlers) g_ptr_array_unref(as->handlers);
  g_free(as->active); g_free(as->selected);
  g_free(as->path);
  /* settings is singleton, don't free */
  if (as == default_instance) default_instance = NULL;
  g_free(as);
}

static gboolean accounts_store_load_into(AccountsStore *as, GError **error);
gboolean accounts_store_load(AccountsStore *as, GError **error) {
  if (!as) return FALSE;
  AccountsStore *parsed = accounts_store_new();
  gboolean ok = accounts_store_load_into(parsed, error);
  if (ok) {
    GHashTable *map = as->map, *watch = as->watch_only_set, *types = as->key_types;
    as->map = parsed->map; as->watch_only_set = parsed->watch_only_set; as->key_types = parsed->key_types;
    parsed->map = map; parsed->watch_only_set = watch; parsed->key_types = types;
    GHashTable *setup = as->setup; as->setup = parsed->setup; parsed->setup = setup;
    g_free(as->active); as->active = g_steal_pointer(&parsed->active);
    if (as->active && !g_hash_table_contains(as->map, as->active)) g_clear_pointer(&as->active, g_free);
    if (as->selected && !g_hash_table_contains(as->map, as->selected)) g_clear_pointer(&as->selected, g_free);
  }
  accounts_store_free(parsed); return ok;
}
static gboolean accounts_store_load_into(AccountsStore *as, GError **error) {
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

      /* A removed INI file means an empty metadata snapshot, not stale
       * GSettings resurrection. Signable keys are rediscovered from daemon. */
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

  nkeys = 0;
  keys = g_key_file_get_keys(kf, "setup", &nkeys, NULL);
  for (gsize i = 0; i < nkeys; i++) {
    gchar *state = g_key_file_get_string(kf, "setup", keys[i], NULL);
    if (state && (!strcmp(state, "ready") || !strcmp(state, "backup-pending") || !strcmp(state, "verification-pending")))
      g_hash_table_insert(as->setup, g_strdup(keys[i]), state);
    else g_free(state);
  }
  g_strfreev(keys);

  /* Load active state */
  gchar *active = g_key_file_get_string(kf, "state", "active", NULL);
  if (active) {
    g_free(as->active);
    as->active = active;
  }

  g_key_file_unref(kf);

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

  g_hash_table_iter_init(&it, as->setup);
  while (g_hash_table_iter_next(&it, &key, &val))
    if (g_hash_table_contains(as->map, key)) g_key_file_set_string(kf, "setup", key, val);

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
    if (!g_file_set_contents_full(as->path, data, len,
          G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, &local_error)) {
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

  /* Mirror metadata only after the authoritative INI commit succeeded. */
  if (success && as->settings) {
    /* Update account order */
    g_ptr_array_add(order_arr, NULL);  /* NULL terminate */
    settings_manager_set_account_order(as->settings, (const gchar *const *)order_arr->pdata);

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

  if (!accounts_store_save(as, error)) { g_hash_table_remove(as->map, id); return FALSE; }
  emit_change(as, ACCOUNTS_CHANGE_ADDED, id);

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

  g_autofree gchar *old_label = g_strdup(g_hash_table_lookup(as->map, id));
  g_autofree gchar *old_active = g_strdup(as->active);
  gboolean was_watch = g_hash_table_contains(as->watch_only_set, id);
  gpointer old_type = g_hash_table_lookup(as->key_types, id);
  g_hash_table_remove(as->map, id);
  g_hash_table_remove(as->watch_only_set, id);
  g_hash_table_remove(as->key_types, id);
  if (!g_strcmp0(as->active, id)) g_clear_pointer(&as->active, g_free);
  if (!accounts_store_save(as, error)) {
    g_hash_table_insert(as->map, g_strdup(id), g_strdup(old_label));
    if (was_watch) g_hash_table_add(as->watch_only_set, g_strdup(id));
    if (old_type) g_hash_table_insert(as->key_types, g_strdup(id), old_type);
    g_free(as->active); as->active = g_strdup(old_active); return FALSE;
  }
  if (as->settings && !g_strcmp0(settings_manager_get_default_identity(as->settings), id))
    settings_manager_set_default_identity(as->settings, "");

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

  g_hash_table_remove(as->setup, id);
  if (!g_strcmp0(as->selected, id)) g_clear_pointer(&as->selected, g_free);
  emit_change(as, ACCOUNTS_CHANGE_REMOVED, id);

  if (g_strcmp0(old_active, as->active)) emit_change(as, ACCOUNTS_CHANGE_ACTIVE, as->active);

  return TRUE;
}

void accounts_store_entry_free(AccountEntry *entry) {
  if (!entry) return;
  g_free(entry->id);
  g_free(entry->label); g_free(entry->setup_state);
  g_free(entry);
}

static AccountEntry *account_entry(AccountsStore *as, const gchar *id, const gchar *label) {
  AccountEntry *entry = g_new0(AccountEntry, 1);
  entry->id = g_strdup(id); entry->label = g_strdup(label ? label : "");
  const gchar *setup = g_hash_table_lookup(as->setup, id);
  entry->setup_state = g_strdup(setup ? setup : "verification-pending");
  entry->watch_only = accounts_store_is_watch_only(as, id);
  gpointer type = g_hash_table_lookup(as->key_types, id);
  entry->key_type = type ? GPOINTER_TO_INT(type) : GN_KEY_TYPE_SECP256K1;
  entry->has_secret = g_hash_table_contains(as->signable, id);
  return entry;
}
GPtrArray *accounts_store_list(AccountsStore *as) {
  if (!as) return NULL;

  GPtrArray *arr = g_ptr_array_new_with_free_func((GDestroyNotify)accounts_store_entry_free);

  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init(&it, as->map);
  while (g_hash_table_iter_next(&it, &key, &val)) {
    g_ptr_array_add(arr, account_entry(as, key, val));
  }

  /* A failed metadata save must not hide a durably stored daemon identity. */
  g_hash_table_iter_init(&it, as->signable);
  while (g_hash_table_iter_next(&it, &key, NULL))
    if (!g_hash_table_contains(as->map, key)) g_ptr_array_add(arr, account_entry(as, key, ""));
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

  gchar *previous = as->active;
  as->active = id && *id ? g_strdup(id) : NULL;
  if (!accounts_store_save(as, error)) { g_free(as->active); as->active = previous; return FALSE; }
  g_free(previous);
  if (as->settings) settings_manager_set_default_identity(as->settings, as->active ? as->active : "");

  /* Emit change notification */
  if (changed) {
    emit_change(as, ACCOUNTS_CHANGE_ACTIVE, id);
  }

  return TRUE;
}

gboolean accounts_store_set_setup_state(AccountsStore *as, const gchar *id, const gchar *state, GError **error) {
  if (!as || !accounts_store_exists(as, id) || !state ||
      (strcmp(state, "ready") && strcmp(state, "backup-pending") && strcmp(state, "verification-pending"))) return FALSE;
  g_autofree gchar *previous = g_strdup(g_hash_table_lookup(as->setup, id));
  g_hash_table_replace(as->setup, g_strdup(id), g_strdup(state));
  if (!accounts_store_save(as, error)) {
    if (previous) g_hash_table_replace(as->setup, g_strdup(id), g_strdup(previous));
    else g_hash_table_remove(as->setup, id);
    return FALSE;
  }
  emit_change(as, ACCOUNTS_CHANGE_LABEL, id); return TRUE;
}
gboolean accounts_store_set_selected(AccountsStore *as, const gchar *id) {
  if (!as || (id && *id && !accounts_store_exists(as, id) && !g_hash_table_contains(as->signable, id))) return FALSE;
  if (!g_strcmp0(as->selected, id)) return TRUE;
  g_free(as->selected); as->selected = id && *id ? g_strdup(id) : NULL;
  emit_change(as, ACCOUNTS_CHANGE_ACTIVE, as->selected); return TRUE;
}
const gchar *accounts_store_get_selected(AccountsStore *as) { return as ? as->selected : NULL; }

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

  g_autofree gchar *previous = g_strdup(g_hash_table_lookup(as->map, id));
  g_hash_table_replace(as->map, g_strdup(id), g_strdup(label ? label : ""));
  if (!accounts_store_save(as, error)) {
    g_hash_table_replace(as->map, g_strdup(id), g_strdup(previous)); return FALSE;
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
  g_autoptr(GPtrArray) entries = accounts_store_list(as);
  for (guint i = 0; i < entries->len; i++) {
    AccountEntry *entry = entries->pdata[i];
    if (!g_strcmp0(entry->id, query) || g_str_has_prefix(entry->id, query) ||
        (*entry->label && g_str_has_prefix(entry->label, query))) return g_ptr_array_steal_index(entries, i);
  }
  return NULL;
}

static void reconcile_default(AccountsStore *as) {
  /* A successful complete daemon snapshot, not metadata removal or owner
   * disappearance, is evidence that a legacy default no longer exists. */
  GSettings *settings = settings_manager_get_gsettings(settings_manager_get_default());
  g_autofree gchar *identity = g_settings_get_string(settings, "default-identity");
  if (*identity && !g_hash_table_contains(as->signable, identity))
    g_settings_set_string(settings, "default-identity", "");
}
void accounts_store_sync_with_secrets(AccountsStore *as) {
  if (!as) return;
  as->inventory_generation++;

  /* Get all identities from secret storage */
  GPtrArray *secrets = secret_store_list();
  if (!secrets) return;

  g_hash_table_remove_all(as->signable);
  for (guint i = 0; i < secrets->len; i++) {
    SecretStoreEntry *se = g_ptr_array_index(secrets, i);
    if (!se || !se->npub) continue;
    g_hash_table_add(as->signable, g_strdup(se->npub));

    /* Add if not already tracked */
    if (!g_hash_table_contains(as->map, se->npub)) {
      if (!accounts_store_add(as, se->npub, se->label, NULL))
        g_warning("Could not persist daemon identity metadata");
    }
  }

  reconcile_default(as);
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
    g_autoptr(GNostrNip19) nip19 = gnostr_nip19_decode(pubkey, NULL);
    if (!nip19 || gnostr_nip19_get_entity_type(nip19) != GNOSTR_BECH32_NPUB) {
      g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_INVALID_INPUT,
                          "Invalid npub format");
      return FALSE;
    }
    /* Re-encode from decoded hex pubkey to normalize */
    const gchar *pk_hex = gnostr_nip19_get_pubkey(nip19);
    g_autoptr(GNostrNip19) encoded = gnostr_nip19_encode_npub(pk_hex, NULL);
    if (!encoded) {
      g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_CRYPTO_FAILED,
                          "Failed to encode npub");
      return FALSE;
    }
    npub = g_strdup(gnostr_nip19_get_bech32(encoded));
  } else if (is_hex64(pubkey)) {
    /* Encode hex pubkey as npub via GNostrNip19 */
    g_autoptr(GNostrNip19) nip19 = gnostr_nip19_encode_npub(pubkey, NULL);
    if (!nip19) {
      g_set_error_literal(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_CRYPTO_FAILED,
                          "Failed to encode npub from hex");
      return FALSE;
    }
    npub = g_strdup(gnostr_nip19_get_bech32(nip19));
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

  if (!accounts_store_save(as, error)) {
    g_hash_table_remove(as->map, npub); g_hash_table_remove(as->watch_only_set, npub);
    g_free(npub); return FALSE;
  }
  emit_change(as, ACCOUNTS_CHANGE_ADDED, npub);

  if (out_npub) {
    *out_npub = npub;
  } else {
    g_free(npub);
  }

  return TRUE;
}

gboolean accounts_store_is_watch_only(AccountsStore *as, const gchar *id) {
  if (!as || !id) return FALSE;
  return g_hash_table_contains(as->watch_only_set, id) && !g_hash_table_contains(as->signable, id);
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
  guint generation;
} SyncAsyncData;

/* Called in main thread after secret list completes */
static void sync_secrets_list_cb(GPtrArray *entries, gpointer user_data) {
  SyncAsyncData *data = user_data;
  AccountsStore *as = data->as;

  if (entries && as && data->generation == as->inventory_generation) {
    g_hash_table_remove_all(as->signable);
    for (guint i = 0; i < entries->len; i++) {
      SecretStoreEntry *se = g_ptr_array_index(entries, i);
      if (!se || !se->npub) continue;
      g_hash_table_add(as->signable, g_strdup(se->npub));

      /* Add if not already tracked */
      if (!g_hash_table_contains(as->map, se->npub)) {
        if (!accounts_store_add(as, se->npub, se->label, NULL))
          g_warning("Could not persist daemon identity metadata");
      }
    }
    reconcile_default(as);
    g_ptr_array_unref(entries);
    emit_change(as, ACCOUNTS_CHANGE_ADDED, NULL);
  } else if (entries) g_ptr_array_unref(entries);

  /* Invoke callback */
  if (data->callback) {
    data->callback(as, data->user_data);
  }

  accounts_store_free(as);
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
  data->as = as; as->refs++; data->generation = ++as->inventory_generation;
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

  gpointer previous = g_hash_table_lookup(as->key_types, id);
  g_hash_table_replace(as->key_types, g_strdup(id), GINT_TO_POINTER(key_type));
  if (!accounts_store_save(as, error)) {
    if (previous) g_hash_table_replace(as->key_types, g_strdup(id), previous);
    else g_hash_table_remove(as->key_types, id);
    return FALSE;
  }
  emit_change(as, ACCOUNTS_CHANGE_LABEL, id); return TRUE;
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

  /* Other key types are not yet supported for account storage.
   * secp256k1 keys are stored as nsec1 (NIP-19); other key types would
   * need additional NIP definitions for encoding and storage format.
   * Return early before generating any key material to avoid wasting
   * crypto operations and creating sensitive data that's immediately
   * discarded. */
  g_set_error(error, GN_SIGNER_ERROR, GN_SIGNER_ERROR_NOT_SUPPORTED,
              "Key type '%s' is not yet supported for account storage. "
              "Only secp256k1 keys can be generated and stored.",
              gn_key_type_to_string(key_type));
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
