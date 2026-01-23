/* multisig_store.c - Partial signature storage implementation
 *
 * Stores partial signatures with secure memory handling.
 * Storage: ~/.config/gnostr-signer/multisig_partials.enc
 *
 * Note: In production, this should encrypt the stored data.
 * For now, we use a simple JSON format but handle signatures
 * in secure memory during runtime.
 *
 * Issue: nostrc-orz
 */
#include "multisig_store.h"
#include "secure-memory.h"
#include "secure-mem.h"
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <time.h>

/* Default expiry: 1 hour */
#define DEFAULT_EXPIRY_SECONDS (60 * 60)

struct _MultisigStore {
  GHashTable *partials;   /* "session_id:signer_npub" -> MultisigPartialSig* */
  gchar *storage_path;
  gboolean dirty;
};

static MultisigStore *default_store = NULL;

/* ======== Memory Management ======== */

static gchar *make_key(const gchar *session_id, const gchar *signer_npub) {
  return g_strdup_printf("%s:%s", session_id, signer_npub);
}

void multisig_partial_sig_free(MultisigPartialSig *partial) {
  if (!partial) return;
  g_free(partial->session_id);
  g_free(partial->signer_npub);
  /* Use secure free for the signature */
  if (partial->partial_sig) {
    gn_secure_strfree(partial->partial_sig);
  }
  g_free(partial);
}

static MultisigPartialSig *partial_copy(const MultisigPartialSig *src) {
  if (!src) return NULL;

  MultisigPartialSig *copy = g_new0(MultisigPartialSig, 1);
  copy->session_id = g_strdup(src->session_id);
  copy->signer_npub = g_strdup(src->signer_npub);
  copy->partial_sig = gn_secure_strdup(src->partial_sig);
  copy->received_at = src->received_at;
  copy->verified = src->verified;

  return copy;
}

/* ======== Storage Path ======== */

static const gchar *get_storage_path(void) {
  static gchar *path = NULL;
  if (!path) {
    const gchar *conf = g_get_user_config_dir();
    gchar *dir = g_build_filename(conf, "gnostr-signer", NULL);
    g_mkdir_with_parents(dir, 0700);
    path = g_build_filename(dir, "multisig_partials.json", NULL);
    g_free(dir);
  }
  return path;
}

/* ======== Store Implementation ======== */

MultisigStore *multisig_store_get_default(void) {
  if (!default_store) {
    default_store = g_new0(MultisigStore, 1);
    default_store->partials = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                      g_free, (GDestroyNotify)multisig_partial_sig_free);
    default_store->storage_path = g_strdup(get_storage_path());
    default_store->dirty = FALSE;
    multisig_store_load(default_store);
  }
  return default_store;
}

void multisig_store_free(MultisigStore *store) {
  if (!store) return;

  if (store->dirty) {
    multisig_store_save(store);
  }

  if (store->partials) {
    g_hash_table_destroy(store->partials);
  }
  g_free(store->storage_path);

  if (store == default_store) {
    default_store = NULL;
  }
  g_free(store);
}

gboolean multisig_store_add_partial(MultisigStore *store,
                                    const gchar *session_id,
                                    const gchar *signer_npub,
                                    const gchar *partial_sig,
                                    GError **error) {
  if (!store || !session_id || !signer_npub || !partial_sig) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_INVALID_SIGNER,
                "Invalid parameters");
    return FALSE;
  }

  gchar *key = make_key(session_id, signer_npub);

  /* Check for duplicate */
  if (g_hash_table_contains(store->partials, key)) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_DUPLICATE,
                "Partial signature already exists for %s", signer_npub);
    g_free(key);
    return FALSE;
  }

  MultisigPartialSig *partial = g_new0(MultisigPartialSig, 1);
  partial->session_id = g_strdup(session_id);
  partial->signer_npub = g_strdup(signer_npub);
  partial->partial_sig = gn_secure_strdup(partial_sig);
  partial->received_at = (gint64)time(NULL);
  partial->verified = FALSE;

  g_hash_table_insert(store->partials, key, partial);
  store->dirty = TRUE;

  g_debug("multisig_store: added partial signature for %s in session %s",
          signer_npub, session_id);

  return TRUE;
}

gboolean multisig_store_get_partial(MultisigStore *store,
                                    const gchar *session_id,
                                    const gchar *signer_npub,
                                    gchar **out_partial_sig) {
  if (!store || !session_id || !signer_npub || !out_partial_sig) {
    return FALSE;
  }

  gchar *key = make_key(session_id, signer_npub);
  MultisigPartialSig *partial = g_hash_table_lookup(store->partials, key);
  g_free(key);

  if (!partial) {
    return FALSE;
  }

  /* Return a secure copy */
  *out_partial_sig = gn_secure_strdup(partial->partial_sig);
  return TRUE;
}

GPtrArray *multisig_store_list_partials(MultisigStore *store,
                                        const gchar *session_id) {
  GPtrArray *arr = g_ptr_array_new_with_free_func((GDestroyNotify)multisig_partial_sig_free);

  if (!store || !session_id) {
    return arr;
  }

  gchar *prefix = g_strdup_printf("%s:", session_id);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, store->partials);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    const gchar *k = key;
    if (g_str_has_prefix(k, prefix)) {
      MultisigPartialSig *partial = value;
      g_ptr_array_add(arr, partial_copy(partial));
    }
  }

  g_free(prefix);
  return arr;
}

guint multisig_store_count_partials(MultisigStore *store,
                                    const gchar *session_id) {
  if (!store || !session_id) {
    return 0;
  }

  gchar *prefix = g_strdup_printf("%s:", session_id);
  guint count = 0;

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, store->partials);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    const gchar *k = key;
    if (g_str_has_prefix(k, prefix)) {
      count++;
    }
  }

  g_free(prefix);
  return count;
}

gboolean multisig_store_remove_partial(MultisigStore *store,
                                       const gchar *session_id,
                                       const gchar *signer_npub) {
  if (!store || !session_id || !signer_npub) {
    return FALSE;
  }

  gchar *key = make_key(session_id, signer_npub);
  gboolean removed = g_hash_table_remove(store->partials, key);
  g_free(key);

  if (removed) {
    store->dirty = TRUE;
    g_debug("multisig_store: removed partial signature for %s in session %s",
            signer_npub, session_id);
  }

  return removed;
}

guint multisig_store_clear_session(MultisigStore *store,
                                   const gchar *session_id) {
  if (!store || !session_id) {
    return 0;
  }

  gchar *prefix = g_strdup_printf("%s:", session_id);
  GPtrArray *keys_to_remove = g_ptr_array_new_with_free_func(g_free);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, store->partials);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    const gchar *k = key;
    if (g_str_has_prefix(k, prefix)) {
      g_ptr_array_add(keys_to_remove, g_strdup(k));
    }
  }

  guint removed = 0;
  for (guint i = 0; i < keys_to_remove->len; i++) {
    gchar *k = g_ptr_array_index(keys_to_remove, i);
    if (g_hash_table_remove(store->partials, k)) {
      removed++;
    }
  }

  g_ptr_array_unref(keys_to_remove);
  g_free(prefix);

  if (removed > 0) {
    store->dirty = TRUE;
    g_debug("multisig_store: cleared %u partial signatures for session %s",
            removed, session_id);
  }

  return removed;
}

guint multisig_store_expire_old(MultisigStore *store,
                                gint64 max_age_seconds) {
  if (!store) {
    return 0;
  }

  gint64 cutoff = (gint64)time(NULL) - max_age_seconds;
  GPtrArray *keys_to_remove = g_ptr_array_new_with_free_func(g_free);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, store->partials);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    MultisigPartialSig *partial = value;
    if (partial->received_at < cutoff) {
      g_ptr_array_add(keys_to_remove, g_strdup(key));
    }
  }

  guint removed = 0;
  for (guint i = 0; i < keys_to_remove->len; i++) {
    gchar *k = g_ptr_array_index(keys_to_remove, i);
    if (g_hash_table_remove(store->partials, k)) {
      removed++;
    }
  }

  g_ptr_array_unref(keys_to_remove);

  if (removed > 0) {
    store->dirty = TRUE;
    g_message("multisig_store: expired %u old partial signatures", removed);
  }

  return removed;
}

void multisig_store_save(MultisigStore *store) {
  if (!store || !store->storage_path) return;

  JsonBuilder *builder = json_builder_new();
  json_builder_begin_object(builder);

  json_builder_set_member_name(builder, "version");
  json_builder_add_int_value(builder, 1);

  json_builder_set_member_name(builder, "partials");
  json_builder_begin_array(builder);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, store->partials);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    MultisigPartialSig *partial = value;

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "session_id");
    json_builder_add_string_value(builder, partial->session_id);

    json_builder_set_member_name(builder, "signer_npub");
    json_builder_add_string_value(builder, partial->signer_npub);

    /* Note: In production, this should be encrypted!
     * For now, we store it as-is but this is a security concern. */
    json_builder_set_member_name(builder, "partial_sig");
    json_builder_add_string_value(builder, partial->partial_sig);

    json_builder_set_member_name(builder, "received_at");
    json_builder_add_int_value(builder, partial->received_at);

    json_builder_set_member_name(builder, "verified");
    json_builder_add_boolean_value(builder, partial->verified);

    json_builder_end_object(builder);
  }

  json_builder_end_array(builder);
  json_builder_end_object(builder);

  JsonGenerator *gen = json_generator_new();
  json_generator_set_pretty(gen, TRUE);
  json_generator_set_root(gen, json_builder_get_root(builder));

  GError *error = NULL;
  if (!json_generator_to_file(gen, store->storage_path, &error)) {
    g_warning("multisig_store: failed to save: %s", error ? error->message : "unknown");
    g_clear_error(&error);
  } else {
    /* Set restrictive permissions */
    g_chmod(store->storage_path, 0600);
    g_debug("multisig_store: saved %u partial signatures",
            g_hash_table_size(store->partials));
  }

  g_object_unref(gen);
  g_object_unref(builder);
  store->dirty = FALSE;
}

void multisig_store_load(MultisigStore *store) {
  if (!store || !store->storage_path) return;

  gchar *contents = NULL;
  gsize length = 0;
  GError *error = NULL;

  if (!g_file_get_contents(store->storage_path, &contents, &length, &error)) {
    if (error) {
      if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
        g_warning("multisig_store: failed to load: %s", error->message);
      }
      g_clear_error(&error);
    }
    return;
  }

  JsonParser *parser = json_parser_new();
  if (!json_parser_load_from_data(parser, contents, length, &error)) {
    g_warning("multisig_store: failed to parse: %s", error ? error->message : "unknown");
    g_clear_error(&error);
    g_free(contents);
    g_object_unref(parser);
    return;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_warning("multisig_store: invalid format");
    g_free(contents);
    g_object_unref(parser);
    return;
  }

  JsonObject *root_obj = json_node_get_object(root);
  JsonArray *partials_arr = json_object_get_array_member(root_obj, "partials");

  if (partials_arr) {
    guint n = json_array_get_length(partials_arr);
    for (guint i = 0; i < n; i++) {
      JsonObject *p_obj = json_array_get_object_element(partials_arr, i);
      if (!p_obj) continue;

      MultisigPartialSig *partial = g_new0(MultisigPartialSig, 1);
      partial->session_id = g_strdup(json_object_get_string_member(p_obj, "session_id"));
      partial->signer_npub = g_strdup(json_object_get_string_member(p_obj, "signer_npub"));

      /* Load into secure memory */
      const gchar *sig_str = json_object_get_string_member(p_obj, "partial_sig");
      partial->partial_sig = sig_str ? gn_secure_strdup(sig_str) : NULL;

      partial->received_at = json_object_get_int_member(p_obj, "received_at");
      partial->verified = json_object_get_boolean_member(p_obj, "verified");

      if (partial->session_id && partial->signer_npub) {
        gchar *key = make_key(partial->session_id, partial->signer_npub);
        g_hash_table_insert(store->partials, key, partial);
      } else {
        multisig_partial_sig_free(partial);
      }
    }
  }

  g_free(contents);
  g_object_unref(parser);

  /* Expire old entries on load */
  multisig_store_expire_old(store, DEFAULT_EXPIRY_SECONDS);

  g_debug("multisig_store: loaded %u partial signatures",
          g_hash_table_size(store->partials));
}
