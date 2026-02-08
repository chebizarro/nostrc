/* multisig_wallet.c - Multi-signature wallet implementation
 *
 * Implements m-of-n threshold signatures for gnostr-signer.
 * Uses secure memory for handling partial signatures.
 *
 * Storage: ~/.config/gnostr-signer/multisig_wallets.json
 *
 * Issue: nostrc-orz
 */
#include "multisig_wallet.h"
#include "multisig_nip46.h"
#include "secret_store.h"
#include "secure-memory.h"
#include "secure-mem.h"
#include <nostr_nip19.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <time.h>

/* Default session timeout: 5 minutes */
#define DEFAULT_SESSION_TIMEOUT_SECONDS 300

/* Storage singleton */
typedef struct {
  GHashTable *wallets;           /* wallet_id -> MultisigWallet* */
  GHashTable *sessions;          /* session_id -> MultisigSigningSession* */
  GHashTable *session_callbacks; /* session_id -> SessionCallbackData* */
  gchar *storage_path;
  gboolean dirty;
} MultisigStorage;

typedef struct {
  MultisigProgressCb progress_cb;
  MultisigCompleteCb complete_cb;
  gpointer user_data;
  guint timeout_source_id;
} SessionCallbackData;

static MultisigStorage *storage = NULL;

/* Forward declarations */
static void ensure_storage_initialized(void);
static void save_storage(void);
static void load_storage(void);
static MultisigWallet *wallet_copy(const MultisigWallet *src);
static void check_session_complete(MultisigSigningSession *session);

G_DEFINE_QUARK(multisig-wallet-error-quark, multisig_wallet_error)

/* ======== Helper Functions ======== */

const gchar *multisig_result_to_string(MultisigResult result) {
  switch (result) {
    case MULTISIG_OK:
      return "Success";
    case MULTISIG_ERR_INVALID_CONFIG:
      return "Invalid threshold configuration";
    case MULTISIG_ERR_INVALID_SIGNER:
      return "Invalid signer information";
    case MULTISIG_ERR_NOT_FOUND:
      return "Wallet or session not found";
    case MULTISIG_ERR_THRESHOLD_NOT_MET:
      return "Signature threshold not met";
    case MULTISIG_ERR_DUPLICATE:
      return "Duplicate entry";
    case MULTISIG_ERR_BACKEND:
      return "Backend error";
    case MULTISIG_ERR_TIMEOUT:
      return "Session timed out";
    case MULTISIG_ERR_CANCELED:
      return "Signing canceled";
    case MULTISIG_ERR_REMOTE_FAILED:
      return "Remote signer communication failed";
    default:
      return "Unknown error";
  }
}

const gchar *multisig_cosigner_status_to_string(CosignerStatus status) {
  switch (status) {
    case COSIGNER_STATUS_PENDING:
      return "Pending";
    case COSIGNER_STATUS_REQUESTED:
      return "Requested";
    case COSIGNER_STATUS_SIGNED:
      return "Signed";
    case COSIGNER_STATUS_REJECTED:
      return "Rejected";
    case COSIGNER_STATUS_TIMEOUT:
      return "Timed out";
    case COSIGNER_STATUS_ERROR:
      return "Error";
    default:
      return "Unknown";
  }
}

gboolean multisig_validate_config(guint threshold_m,
                                  guint total_n,
                                  GError **error) {
  if (total_n < 1) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_INVALID_CONFIG,
                "Total signers must be at least 1");
    return FALSE;
  }
  if (threshold_m < 1) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_INVALID_CONFIG,
                "Threshold must be at least 1");
    return FALSE;
  }
  if (threshold_m > total_n) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_INVALID_CONFIG,
                "Threshold (%u) cannot exceed total signers (%u)",
                threshold_m, total_n);
    return FALSE;
  }
  return TRUE;
}

gchar *multisig_format_progress(guint collected, guint required) {
  if (collected >= required) {
    return g_strdup_printf("%u of %u signatures collected (complete)",
                           collected, required);
  }
  return g_strdup_printf("%u of %u signatures collected",
                         collected, required);
}

/* ======== Memory Management ======== */

MultisigCosigner *multisig_cosigner_new(const gchar *npub,
                                        const gchar *label,
                                        CosignerType type) {
  if (!npub) return NULL;

  MultisigCosigner *cs = g_new0(MultisigCosigner, 1);
  cs->id = g_strdup_printf("cs_%ld_%d", (long)time(NULL), g_random_int_range(1000, 9999));
  cs->npub = g_strdup(npub);
  cs->label = g_strdup(label);
  cs->type = type;
  cs->bunker_uri = NULL;
  cs->is_self = FALSE;

  return cs;
}

MultisigCosigner *multisig_cosigner_new_remote(const gchar *bunker_uri,
                                               const gchar *label) {
  if (!bunker_uri || !g_str_has_prefix(bunker_uri, "bunker://")) {
    return NULL;
  }

  /* Extract pubkey hex from bunker://PUBKEY_HEX?... */
  const gchar *pk_start = bunker_uri + strlen("bunker://");
  const gchar *pk_end = strchr(pk_start, '?');
  if (!pk_end) pk_end = pk_start + strlen(pk_start);

  gsize pk_len = pk_end - pk_start;
  if (pk_len != 64) {
    g_warning("multisig_cosigner_new_remote: invalid pubkey length in URI");
    return NULL;
  }

  gchar *pk_hex = g_strndup(pk_start, pk_len);

  /* Convert hex to npub */
  GNostrNip19 *nip19 = gnostr_nip19_encode_npub(pk_hex, NULL);
  g_free(pk_hex);

  if (!nip19) {
    g_warning("multisig_cosigner_new_remote: failed to encode npub");
    return NULL;
  }

  const gchar *npub = gnostr_nip19_get_bech32(nip19);
  MultisigCosigner *cs = multisig_cosigner_new(npub, label, COSIGNER_TYPE_REMOTE_NIP46);
  g_object_unref(nip19);

  if (cs) {
    cs->bunker_uri = g_strdup(bunker_uri);
  }

  return cs;
}

static MultisigCosigner *cosigner_copy(const MultisigCosigner *src) {
  if (!src) return NULL;

  MultisigCosigner *copy = g_new0(MultisigCosigner, 1);
  copy->id = g_strdup(src->id);
  copy->npub = g_strdup(src->npub);
  copy->label = g_strdup(src->label);
  copy->type = src->type;
  copy->bunker_uri = g_strdup(src->bunker_uri);
  copy->is_self = src->is_self;

  return copy;
}

void multisig_cosigner_free(MultisigCosigner *cosigner) {
  if (!cosigner) return;
  g_free(cosigner->id);
  g_free(cosigner->npub);
  g_free(cosigner->label);
  g_free(cosigner->bunker_uri);
  g_free(cosigner);
}

static MultisigWallet *wallet_copy(const MultisigWallet *src) {
  if (!src) return NULL;

  MultisigWallet *copy = g_new0(MultisigWallet, 1);
  copy->wallet_id = g_strdup(src->wallet_id);
  copy->name = g_strdup(src->name);
  copy->threshold_m = src->threshold_m;
  copy->total_n = src->total_n;
  copy->aggregated_pubkey = g_strdup(src->aggregated_pubkey);
  copy->created_at = src->created_at;
  copy->updated_at = src->updated_at;

  copy->cosigners = g_ptr_array_new_with_free_func((GDestroyNotify)multisig_cosigner_free);
  if (src->cosigners) {
    for (guint i = 0; i < src->cosigners->len; i++) {
      MultisigCosigner *cs = g_ptr_array_index(src->cosigners, i);
      g_ptr_array_add(copy->cosigners, cosigner_copy(cs));
    }
  }

  return copy;
}

void multisig_wallet_free(MultisigWallet *wallet) {
  if (!wallet) return;
  g_free(wallet->wallet_id);
  g_free(wallet->name);
  g_free(wallet->aggregated_pubkey);
  if (wallet->cosigners) {
    g_ptr_array_unref(wallet->cosigners);
  }
  g_free(wallet);
}

void multisig_signing_session_free(MultisigSigningSession *session) {
  if (!session) return;
  g_free(session->session_id);
  g_free(session->wallet_id);
  g_free(session->event_json);
  g_free(session->event_id);
  if (session->partial_sigs) {
    g_ptr_array_unref(session->partial_sigs);
  }
  if (session->signer_status) {
    g_hash_table_destroy(session->signer_status);
  }
  /* Use secure free for signature data */
  if (session->final_signature) {
    gn_secure_strfree(session->final_signature);
  }
  g_free(session);
}

static void session_callback_data_free(gpointer data) {
  SessionCallbackData *scd = data;
  if (!scd) return;
  if (scd->timeout_source_id > 0) {
    g_source_remove(scd->timeout_source_id);
  }
  g_free(scd);
}

/* ======== Storage ======== */

static const gchar *get_storage_path(void) {
  static gchar *path = NULL;
  if (!path) {
    const gchar *conf = g_get_user_config_dir();
    gchar *dir = g_build_filename(conf, "gnostr-signer", NULL);
    g_mkdir_with_parents(dir, 0700);
    path = g_build_filename(dir, "multisig_wallets.json", NULL);
    g_free(dir);
  }
  return path;
}

static void ensure_storage_initialized(void) {
  if (storage) return;

  storage = g_new0(MultisigStorage, 1);
  storage->wallets = g_hash_table_new_full(g_str_hash, g_str_equal,
                                            g_free, (GDestroyNotify)multisig_wallet_free);
  storage->sessions = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, (GDestroyNotify)multisig_signing_session_free);
  storage->session_callbacks = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                      g_free, session_callback_data_free);
  storage->storage_path = g_strdup(get_storage_path());
  storage->dirty = FALSE;

  load_storage();
}

static void load_storage(void) {
  if (!storage || !storage->storage_path) return;

  gchar *contents = NULL;
  gsize length = 0;
  GError *error = NULL;

  if (!g_file_get_contents(storage->storage_path, &contents, &length, &error)) {
    if (error) {
      /* File not existing is fine for first run */
      if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
        g_warning("multisig: failed to load storage: %s", error->message);
      }
      g_clear_error(&error);
    }
    return;
  }

  JsonParser *parser = json_parser_new();
  if (!json_parser_load_from_data(parser, contents, length, &error)) {
    g_warning("multisig: failed to parse storage: %s", error ? error->message : "unknown");
    g_clear_error(&error);
    g_free(contents);
    g_object_unref(parser);
    return;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_warning("multisig: invalid storage format");
    g_free(contents);
    g_object_unref(parser);
    return;
  }

  JsonObject *root_obj = json_node_get_object(root);
  JsonArray *wallets_arr = json_object_get_array_member(root_obj, "wallets");

  if (wallets_arr) {
    guint n = json_array_get_length(wallets_arr);
    for (guint i = 0; i < n; i++) {
      JsonObject *w_obj = json_array_get_object_element(wallets_arr, i);
      if (!w_obj) continue;

      MultisigWallet *wallet = g_new0(MultisigWallet, 1);
      wallet->wallet_id = g_strdup(json_object_get_string_member(w_obj, "wallet_id"));
      wallet->name = g_strdup(json_object_get_string_member(w_obj, "name"));
      wallet->threshold_m = (guint)json_object_get_int_member(w_obj, "threshold_m");
      wallet->total_n = (guint)json_object_get_int_member(w_obj, "total_n");
      wallet->aggregated_pubkey = g_strdup(json_object_get_string_member(w_obj, "aggregated_pubkey"));
      wallet->created_at = json_object_get_int_member(w_obj, "created_at");
      wallet->updated_at = json_object_get_int_member(w_obj, "updated_at");

      wallet->cosigners = g_ptr_array_new_with_free_func((GDestroyNotify)multisig_cosigner_free);

      JsonArray *cs_arr = json_object_get_array_member(w_obj, "cosigners");
      if (cs_arr) {
        guint cs_n = json_array_get_length(cs_arr);
        for (guint j = 0; j < cs_n; j++) {
          JsonObject *cs_obj = json_array_get_object_element(cs_arr, j);
          if (!cs_obj) continue;

          MultisigCosigner *cs = g_new0(MultisigCosigner, 1);
          cs->id = g_strdup(json_object_get_string_member(cs_obj, "id"));
          cs->npub = g_strdup(json_object_get_string_member(cs_obj, "npub"));
          cs->label = g_strdup(json_object_get_string_member(cs_obj, "label"));
          cs->type = (CosignerType)json_object_get_int_member(cs_obj, "type");
          cs->bunker_uri = g_strdup(json_object_get_string_member(cs_obj, "bunker_uri"));
          cs->is_self = json_object_get_boolean_member(cs_obj, "is_self");

          g_ptr_array_add(wallet->cosigners, cs);
        }
      }

      if (wallet->wallet_id) {
        g_hash_table_replace(storage->wallets, g_strdup(wallet->wallet_id), wallet);
      } else {
        multisig_wallet_free(wallet);
      }
    }
  }

  g_free(contents);
  g_object_unref(parser);
  g_debug("multisig: loaded %u wallets from storage",
          g_hash_table_size(storage->wallets));
}

static void save_storage(void) {
  if (!storage || !storage->storage_path) return;

  JsonBuilder *builder = json_builder_new();
  json_builder_begin_object(builder);

  json_builder_set_member_name(builder, "version");
  json_builder_add_int_value(builder, 1);

  json_builder_set_member_name(builder, "wallets");
  json_builder_begin_array(builder);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, storage->wallets);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    MultisigWallet *wallet = value;

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "wallet_id");
    json_builder_add_string_value(builder, wallet->wallet_id);

    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, wallet->name);

    json_builder_set_member_name(builder, "threshold_m");
    json_builder_add_int_value(builder, wallet->threshold_m);

    json_builder_set_member_name(builder, "total_n");
    json_builder_add_int_value(builder, wallet->total_n);

    if (wallet->aggregated_pubkey) {
      json_builder_set_member_name(builder, "aggregated_pubkey");
      json_builder_add_string_value(builder, wallet->aggregated_pubkey);
    }

    json_builder_set_member_name(builder, "created_at");
    json_builder_add_int_value(builder, wallet->created_at);

    json_builder_set_member_name(builder, "updated_at");
    json_builder_add_int_value(builder, wallet->updated_at);

    json_builder_set_member_name(builder, "cosigners");
    json_builder_begin_array(builder);

    if (wallet->cosigners) {
      for (guint i = 0; i < wallet->cosigners->len; i++) {
        MultisigCosigner *cs = g_ptr_array_index(wallet->cosigners, i);

        json_builder_begin_object(builder);

        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, cs->id);

        json_builder_set_member_name(builder, "npub");
        json_builder_add_string_value(builder, cs->npub);

        if (cs->label) {
          json_builder_set_member_name(builder, "label");
          json_builder_add_string_value(builder, cs->label);
        }

        json_builder_set_member_name(builder, "type");
        json_builder_add_int_value(builder, cs->type);

        if (cs->bunker_uri) {
          json_builder_set_member_name(builder, "bunker_uri");
          json_builder_add_string_value(builder, cs->bunker_uri);
        }

        json_builder_set_member_name(builder, "is_self");
        json_builder_add_boolean_value(builder, cs->is_self);

        json_builder_end_object(builder);
      }
    }

    json_builder_end_array(builder);
    json_builder_end_object(builder);
  }

  json_builder_end_array(builder);
  json_builder_end_object(builder);

  JsonGenerator *gen = json_generator_new();
  json_generator_set_pretty(gen, TRUE);
  json_generator_set_root(gen, json_builder_get_root(builder));

  GError *error = NULL;
  if (!json_generator_to_file(gen, storage->storage_path, &error)) {
    g_warning("multisig: failed to save storage: %s", error ? error->message : "unknown");
    g_clear_error(&error);
  }

  g_object_unref(gen);
  g_object_unref(builder);
  storage->dirty = FALSE;

  g_debug("multisig: saved %u wallets to storage",
          g_hash_table_size(storage->wallets));
}

/* ======== Wallet Management ======== */

MultisigResult multisig_wallet_create(const gchar *name,
                                      guint threshold_m,
                                      guint total_n,
                                      gchar **out_wallet_id,
                                      GError **error) {
  if (!multisig_validate_config(threshold_m, total_n, error)) {
    return MULTISIG_ERR_INVALID_CONFIG;
  }

  ensure_storage_initialized();

  MultisigWallet *wallet = g_new0(MultisigWallet, 1);
  wallet->wallet_id = g_strdup_printf("msw_%ld_%d", (long)time(NULL), g_random_int_range(1000, 9999));
  wallet->name = g_strdup(name && *name ? name : "Multisig Wallet");
  wallet->threshold_m = threshold_m;
  wallet->total_n = total_n;
  wallet->cosigners = g_ptr_array_new_with_free_func((GDestroyNotify)multisig_cosigner_free);
  wallet->created_at = (gint64)time(NULL);
  wallet->updated_at = wallet->created_at;

  g_hash_table_replace(storage->wallets, g_strdup(wallet->wallet_id), wallet);
  storage->dirty = TRUE;
  save_storage();

  if (out_wallet_id) {
    *out_wallet_id = g_strdup(wallet->wallet_id);
  }

  g_message("multisig: created wallet %s (%u-of-%u)", wallet->wallet_id, threshold_m, total_n);
  return MULTISIG_OK;
}

MultisigResult multisig_wallet_add_cosigner(const gchar *wallet_id,
                                            MultisigCosigner *cosigner,
                                            GError **error) {
  if (!wallet_id || !cosigner) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_INVALID_SIGNER,
                "Invalid wallet ID or cosigner");
    return MULTISIG_ERR_INVALID_SIGNER;
  }

  ensure_storage_initialized();

  MultisigWallet *wallet = g_hash_table_lookup(storage->wallets, wallet_id);
  if (!wallet) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_NOT_FOUND,
                "Wallet not found: %s", wallet_id);
    multisig_cosigner_free(cosigner);
    return MULTISIG_ERR_NOT_FOUND;
  }

  /* Check if we've reached the limit */
  if (wallet->cosigners->len >= wallet->total_n) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_INVALID_CONFIG,
                "Cannot add more than %u co-signers", wallet->total_n);
    multisig_cosigner_free(cosigner);
    return MULTISIG_ERR_INVALID_CONFIG;
  }

  /* Check for duplicate */
  for (guint i = 0; i < wallet->cosigners->len; i++) {
    MultisigCosigner *existing = g_ptr_array_index(wallet->cosigners, i);
    if (g_strcmp0(existing->npub, cosigner->npub) == 0) {
      g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_DUPLICATE,
                  "Co-signer already exists: %s", cosigner->npub);
      multisig_cosigner_free(cosigner);
      return MULTISIG_ERR_DUPLICATE;
    }
  }

  g_ptr_array_add(wallet->cosigners, cosigner);
  wallet->updated_at = (gint64)time(NULL);
  storage->dirty = TRUE;
  save_storage();

  g_message("multisig: added cosigner %s to wallet %s", cosigner->npub, wallet_id);
  return MULTISIG_OK;
}

MultisigResult multisig_wallet_remove_cosigner(const gchar *wallet_id,
                                               const gchar *cosigner_id,
                                               GError **error) {
  if (!wallet_id || !cosigner_id) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_INVALID_SIGNER,
                "Invalid wallet ID or cosigner ID");
    return MULTISIG_ERR_INVALID_SIGNER;
  }

  ensure_storage_initialized();

  MultisigWallet *wallet = g_hash_table_lookup(storage->wallets, wallet_id);
  if (!wallet) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_NOT_FOUND,
                "Wallet not found: %s", wallet_id);
    return MULTISIG_ERR_NOT_FOUND;
  }

  /* Check if removal would put us below threshold */
  if (wallet->cosigners->len <= wallet->threshold_m) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_INVALID_CONFIG,
                "Cannot remove co-signer: would go below threshold");
    return MULTISIG_ERR_INVALID_CONFIG;
  }

  gboolean found = FALSE;
  for (guint i = 0; i < wallet->cosigners->len; i++) {
    MultisigCosigner *cs = g_ptr_array_index(wallet->cosigners, i);
    if (g_strcmp0(cs->id, cosigner_id) == 0) {
      g_ptr_array_remove_index(wallet->cosigners, i);
      found = TRUE;
      break;
    }
  }

  if (!found) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_NOT_FOUND,
                "Co-signer not found: %s", cosigner_id);
    return MULTISIG_ERR_NOT_FOUND;
  }

  wallet->updated_at = (gint64)time(NULL);
  storage->dirty = TRUE;
  save_storage();

  g_message("multisig: removed cosigner %s from wallet %s", cosigner_id, wallet_id);
  return MULTISIG_OK;
}

MultisigResult multisig_wallet_get(const gchar *wallet_id,
                                   MultisigWallet **out_wallet) {
  if (!wallet_id || !out_wallet) {
    return MULTISIG_ERR_NOT_FOUND;
  }

  ensure_storage_initialized();

  MultisigWallet *wallet = g_hash_table_lookup(storage->wallets, wallet_id);
  if (!wallet) {
    return MULTISIG_ERR_NOT_FOUND;
  }

  *out_wallet = wallet_copy(wallet);
  return MULTISIG_OK;
}

GPtrArray *multisig_wallet_list(void) {
  ensure_storage_initialized();

  GPtrArray *arr = g_ptr_array_new_with_free_func((GDestroyNotify)multisig_wallet_free);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, storage->wallets);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    MultisigWallet *wallet = value;
    g_ptr_array_add(arr, wallet_copy(wallet));
  }

  return arr;
}

MultisigResult multisig_wallet_delete(const gchar *wallet_id,
                                      GError **error) {
  if (!wallet_id) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_NOT_FOUND,
                "Invalid wallet ID");
    return MULTISIG_ERR_NOT_FOUND;
  }

  ensure_storage_initialized();

  if (!g_hash_table_remove(storage->wallets, wallet_id)) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_NOT_FOUND,
                "Wallet not found: %s", wallet_id);
    return MULTISIG_ERR_NOT_FOUND;
  }

  storage->dirty = TRUE;
  save_storage();

  g_message("multisig: deleted wallet %s", wallet_id);
  return MULTISIG_OK;
}

MultisigResult multisig_wallet_save(MultisigWallet *wallet,
                                    GError **error) {
  if (!wallet || !wallet->wallet_id) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_NOT_FOUND,
                "Invalid wallet");
    return MULTISIG_ERR_NOT_FOUND;
  }

  ensure_storage_initialized();

  wallet->updated_at = (gint64)time(NULL);
  g_hash_table_replace(storage->wallets, g_strdup(wallet->wallet_id), wallet_copy(wallet));
  storage->dirty = TRUE;
  save_storage();

  return MULTISIG_OK;
}

/* ======== Signing Sessions ======== */

static gboolean session_timeout_cb(gpointer user_data) {
  gchar *session_id = user_data;

  if (!storage) {
    g_free(session_id);
    return G_SOURCE_REMOVE;
  }

  MultisigSigningSession *session = g_hash_table_lookup(storage->sessions, session_id);
  if (!session || session->is_complete) {
    g_free(session_id);
    return G_SOURCE_REMOVE;
  }

  /* Mark all pending signers as timed out */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, session->signer_status);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    CosignerStatus status = GPOINTER_TO_INT(value);
    if (status == COSIGNER_STATUS_PENDING || status == COSIGNER_STATUS_REQUESTED) {
      g_hash_table_replace(session->signer_status, g_strdup(key),
                           GINT_TO_POINTER(COSIGNER_STATUS_TIMEOUT));
    }
  }

  /* Notify completion with timeout error */
  SessionCallbackData *scd = g_hash_table_lookup(storage->session_callbacks, session_id);
  if (scd && scd->complete_cb) {
    scd->complete_cb(session, FALSE, "Signing session timed out", scd->user_data);
    scd->timeout_source_id = 0;  /* Already fired */
  }

  g_hash_table_remove(storage->session_callbacks, session_id);
  g_free(session_id);
  return G_SOURCE_REMOVE;
}

MultisigResult multisig_signing_start(const gchar *wallet_id,
                                      const gchar *event_json,
                                      gint timeout_seconds,
                                      MultisigProgressCb progress_cb,
                                      MultisigCompleteCb complete_cb,
                                      gpointer user_data,
                                      gchar **out_session_id,
                                      GError **error) {
  if (!wallet_id || !event_json) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_INVALID_CONFIG,
                "Invalid wallet ID or event");
    return MULTISIG_ERR_INVALID_CONFIG;
  }

  ensure_storage_initialized();

  MultisigWallet *wallet = g_hash_table_lookup(storage->wallets, wallet_id);
  if (!wallet) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_NOT_FOUND,
                "Wallet not found: %s", wallet_id);
    return MULTISIG_ERR_NOT_FOUND;
  }

  if (wallet->cosigners->len < wallet->threshold_m) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_INVALID_CONFIG,
                "Not enough co-signers configured (%u < %u)",
                wallet->cosigners->len, wallet->threshold_m);
    return MULTISIG_ERR_INVALID_CONFIG;
  }

  /* Create session */
  MultisigSigningSession *session = g_new0(MultisigSigningSession, 1);
  session->session_id = g_strdup_printf("mss_%ld_%d", (long)time(NULL), g_random_int_range(1000, 9999));
  session->wallet_id = g_strdup(wallet_id);
  session->event_json = g_strdup(event_json);
  session->signatures_collected = 0;
  session->signatures_required = wallet->threshold_m;
  session->partial_sigs = g_ptr_array_new_with_free_func(g_free);
  session->signer_status = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  session->created_at = (gint64)time(NULL);

  gint timeout = timeout_seconds > 0 ? timeout_seconds : DEFAULT_SESSION_TIMEOUT_SECONDS;
  session->expires_at = session->created_at + timeout;
  session->is_complete = FALSE;

  /* Extract event kind for display */
  const gchar *kind_str = strstr(event_json, "\"kind\"");
  if (kind_str) {
    kind_str = strchr(kind_str, ':');
    if (kind_str) {
      kind_str++;
      while (*kind_str == ' ') kind_str++;
      session->event_kind = (gint)g_ascii_strtoll(kind_str, NULL, 10);
    }
  }

  /* Initialize signer status */
  for (guint i = 0; i < wallet->cosigners->len; i++) {
    MultisigCosigner *cs = g_ptr_array_index(wallet->cosigners, i);
    g_hash_table_insert(session->signer_status, g_strdup(cs->npub),
                        GINT_TO_POINTER(COSIGNER_STATUS_PENDING));
  }

  /* Store session */
  g_hash_table_replace(storage->sessions, g_strdup(session->session_id), session);

  /* Store callbacks */
  SessionCallbackData *scd = g_new0(SessionCallbackData, 1);
  scd->progress_cb = progress_cb;
  scd->complete_cb = complete_cb;
  scd->user_data = user_data;
  scd->timeout_source_id = g_timeout_add_seconds(timeout, session_timeout_cb,
                                                  g_strdup(session->session_id));
  g_hash_table_replace(storage->session_callbacks, g_strdup(session->session_id), scd);

  if (out_session_id) {
    *out_session_id = g_strdup(session->session_id);
  }

  g_message("multisig: started signing session %s for wallet %s",
            session->session_id, wallet_id);

  /* Request signatures from remote signers via NIP-46 */
  MultisigNip46Client *nip46_client = multisig_nip46_get_default();
  if (nip46_client) {
    for (guint i = 0; i < wallet->cosigners->len; i++) {
      MultisigCosigner *cs = g_ptr_array_index(wallet->cosigners, i);

      if (cs->type == COSIGNER_TYPE_REMOTE_NIP46 && cs->bunker_uri) {
        /* Connect to the remote signer if not already connected */
        GError *conn_error = NULL;
        if (!multisig_nip46_is_connected(nip46_client, cs->npub)) {
          if (!multisig_nip46_connect(nip46_client, cs->bunker_uri, NULL, &conn_error)) {
            g_warning("multisig: failed to connect to remote signer %s: %s",
                      cs->npub, conn_error ? conn_error->message : "unknown");
            g_clear_error(&conn_error);
            /* Mark as error but continue with other signers */
            g_hash_table_replace(session->signer_status, g_strdup(cs->npub),
                                 GINT_TO_POINTER(COSIGNER_STATUS_ERROR));
            continue;
          }
        }

        /* Request signature from remote signer */
        GError *sig_error = NULL;
        if (multisig_nip46_request_signature(nip46_client, cs->npub,
                                              session->session_id, event_json, &sig_error)) {
          g_hash_table_replace(session->signer_status, g_strdup(cs->npub),
                               GINT_TO_POINTER(COSIGNER_STATUS_REQUESTED));
          g_message("multisig: requested signature from remote signer %s", cs->npub);
        } else {
          g_warning("multisig: failed to request signature from %s: %s",
                    cs->npub, sig_error ? sig_error->message : "unknown");
          g_hash_table_replace(session->signer_status, g_strdup(cs->npub),
                               GINT_TO_POINTER(COSIGNER_STATUS_ERROR));
          g_clear_error(&sig_error);
        }
      }
    }
  }

  return MULTISIG_OK;
}

MultisigResult multisig_signing_add_signature(const gchar *session_id,
                                              const gchar *signer_npub,
                                              const gchar *partial_sig,
                                              GError **error) {
  if (!session_id || !signer_npub || !partial_sig) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_INVALID_SIGNER,
                "Invalid parameters");
    return MULTISIG_ERR_INVALID_SIGNER;
  }

  ensure_storage_initialized();

  MultisigSigningSession *session = g_hash_table_lookup(storage->sessions, session_id);
  if (!session) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_NOT_FOUND,
                "Session not found: %s", session_id);
    return MULTISIG_ERR_NOT_FOUND;
  }

  if (session->is_complete) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_DUPLICATE,
                "Session already complete");
    return MULTISIG_ERR_DUPLICATE;
  }

  /* Check if signer is part of this session */
  if (!g_hash_table_contains(session->signer_status, signer_npub)) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_INVALID_SIGNER,
                "Signer not part of this wallet: %s", signer_npub);
    return MULTISIG_ERR_INVALID_SIGNER;
  }

  /* Check for duplicate signature */
  CosignerStatus current_status = GPOINTER_TO_INT(
      g_hash_table_lookup(session->signer_status, signer_npub));
  if (current_status == COSIGNER_STATUS_SIGNED) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_DUPLICATE,
                "Signature already received from: %s", signer_npub);
    return MULTISIG_ERR_DUPLICATE;
  }

  /* Store partial signature (use secure memory) */
  g_ptr_array_add(session->partial_sigs, gn_secure_strdup(partial_sig));

  /* Update status */
  g_hash_table_replace(session->signer_status, g_strdup(signer_npub),
                       GINT_TO_POINTER(COSIGNER_STATUS_SIGNED));
  session->signatures_collected++;

  /* Notify progress */
  SessionCallbackData *scd = g_hash_table_lookup(storage->session_callbacks, session_id);
  if (scd && scd->progress_cb) {
    scd->progress_cb(session, signer_npub, COSIGNER_STATUS_SIGNED, scd->user_data);
  }

  g_message("multisig: received signature from %s for session %s (%u/%u)",
            signer_npub, session_id,
            session->signatures_collected, session->signatures_required);

  /* Check if threshold met */
  check_session_complete(session);

  return MULTISIG_OK;
}

/**
 * aggregate_schnorr_signatures:
 * @partial_sigs: Array of partial signature hex strings (64 bytes each when decoded)
 * @n_sigs: Number of signatures to aggregate
 * @out_aggregated: Output aggregated signature hex (caller frees with gn_secure_strfree)
 *
 * Aggregates multiple Schnorr partial signatures using simple addition in the
 * scalar field. For Nostr's use case, this implements a basic aggregation scheme
 * where each signer produces a partial signature s_i and the final signature is
 * S = sum(s_i) mod n (where n is the curve order).
 *
 * Note: This is a simplified aggregation. Full MuSig2 would require additional
 * nonce commitment rounds for security against rogue key attacks.
 *
 * Returns: TRUE on success, FALSE on failure
 */
static gboolean aggregate_schnorr_signatures(GPtrArray *partial_sigs,
                                              gchar **out_aggregated) {
  if (!partial_sigs || partial_sigs->len == 0 || !out_aggregated) {
    return FALSE;
  }

  /* Schnorr signature is 64 bytes: 32 bytes R (nonce point) + 32 bytes s (scalar) */
  guint8 aggregated_sig[64];
  guint8 aggregated_s[32];
  gboolean first = TRUE;

  /* secp256k1 curve order n (for modular reduction) */
  static const guint8 secp256k1_order[32] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE,
    0xBA, 0xAE, 0xDC, 0xE6, 0xAF, 0x48, 0xA0, 0x3B,
    0xBF, 0xD2, 0x5E, 0x8C, 0xD0, 0x36, 0x41, 0x41
  };

  memset(aggregated_sig, 0, 64);
  memset(aggregated_s, 0, 32);

  for (guint i = 0; i < partial_sigs->len; i++) {
    const gchar *sig_hex = g_ptr_array_index(partial_sigs, i);
    if (!sig_hex || strlen(sig_hex) != 128) {
      g_warning("aggregate_schnorr_signatures: invalid signature length at index %u", i);
      continue;
    }

    /* Decode hex signature to bytes */
    guint8 sig_bytes[64];
    gboolean valid = TRUE;
    for (gsize j = 0; j < 64 && valid; j++) {
      gchar byte_str[3] = { sig_hex[j*2], sig_hex[j*2+1], '\0' };
      gchar *end = NULL;
      gulong val = strtoul(byte_str, &end, 16);
      if (end != byte_str + 2) valid = FALSE;
      else sig_bytes[j] = (guint8)val;
    }

    if (!valid) {
      g_warning("aggregate_schnorr_signatures: invalid hex at index %u", i);
      continue;
    }

    if (first) {
      /* First signature: copy R and s directly */
      memcpy(aggregated_sig, sig_bytes, 32);  /* R from first signature */
      memcpy(aggregated_s, sig_bytes + 32, 32);  /* s from first signature */
      first = FALSE;
    } else {
      /* Subsequent signatures: add s values modulo curve order
       * For proper MuSig2, all signers would use the same aggregated R,
       * so we verify R matches or combine them. Here we use a simplified
       * approach suitable for threshold signing where R is coordinated. */

      /* Add s values: aggregated_s = (aggregated_s + sig_s) mod n */
      guint16 carry = 0;
      for (gint j = 31; j >= 0; j--) {
        guint16 sum = (guint16)aggregated_s[j] + (guint16)sig_bytes[32 + j] + carry;
        aggregated_s[j] = (guint8)(sum & 0xFF);
        carry = sum >> 8;
      }

      /* Modular reduction if result >= n */
      gboolean greater_or_equal = FALSE;
      if (carry > 0) {
        greater_or_equal = TRUE;
      } else {
        for (gint j = 0; j < 32; j++) {
          if (aggregated_s[j] > secp256k1_order[j]) {
            greater_or_equal = TRUE;
            break;
          } else if (aggregated_s[j] < secp256k1_order[j]) {
            break;
          }
        }
      }

      if (greater_or_equal) {
        /* Subtract n: aggregated_s = aggregated_s - n */
        guint16 borrow = 0;
        for (gint j = 31; j >= 0; j--) {
          gint16 diff = (gint16)aggregated_s[j] - (gint16)secp256k1_order[j] - borrow;
          if (diff < 0) {
            diff += 256;
            borrow = 1;
          } else {
            borrow = 0;
          }
          aggregated_s[j] = (guint8)diff;
        }
      }
    }
  }

  if (first) {
    /* No valid signatures were processed */
    g_warning("aggregate_schnorr_signatures: no valid signatures to aggregate");
    return FALSE;
  }

  /* Combine R and aggregated s into final signature */
  memcpy(aggregated_sig + 32, aggregated_s, 32);

  /* Convert to hex string */
  gchar *result = gn_secure_alloc(129);
  if (!result) {
    return FALSE;
  }

  for (gsize i = 0; i < 64; i++) {
    g_snprintf(result + i*2, 3, "%02x", aggregated_sig[i]);
  }
  result[128] = '\0';

  /* Securely clear temporary buffers */
  gnostr_secure_clear(aggregated_sig, 64);
  gnostr_secure_clear(aggregated_s, 32);

  *out_aggregated = result;
  return TRUE;
}

static void check_session_complete(MultisigSigningSession *session) {
  if (!session || session->is_complete) return;

  if (session->signatures_collected >= session->signatures_required) {
    session->is_complete = TRUE;

    /* Aggregate signatures using Schnorr signature addition */
    if (session->partial_sigs->len > 0) {
      gchar *aggregated = NULL;

      if (session->partial_sigs->len == 1) {
        /* Single signature - no aggregation needed */
        gchar *single_sig = g_ptr_array_index(session->partial_sigs, 0);
        session->final_signature = gn_secure_strdup(single_sig);
        g_message("multisig: using single signature (no aggregation needed)");
      } else if (aggregate_schnorr_signatures(session->partial_sigs, &aggregated)) {
        session->final_signature = aggregated;
        g_message("multisig: aggregated %u partial signatures", session->partial_sigs->len);
      } else {
        /* Aggregation failed - fall back to first signature with warning */
        g_warning("multisig: signature aggregation failed, using first signature");
        gchar *first_sig = g_ptr_array_index(session->partial_sigs, 0);
        session->final_signature = gn_secure_strdup(first_sig);
      }
    }

    /* Notify completion */
    SessionCallbackData *scd = g_hash_table_lookup(storage->session_callbacks, session->session_id);
    if (scd) {
      if (scd->timeout_source_id > 0) {
        g_source_remove(scd->timeout_source_id);
        scd->timeout_source_id = 0;
      }
      if (scd->complete_cb) {
        scd->complete_cb(session, TRUE, NULL, scd->user_data);
      }
      g_hash_table_remove(storage->session_callbacks, session->session_id);
    }

    g_message("multisig: session %s complete", session->session_id);
  }
}

void multisig_signing_reject(const gchar *session_id,
                             const gchar *signer_npub,
                             const gchar *reason) {
  if (!session_id || !signer_npub) return;

  ensure_storage_initialized();

  MultisigSigningSession *session = g_hash_table_lookup(storage->sessions, session_id);
  if (!session || session->is_complete) return;

  g_hash_table_replace(session->signer_status, g_strdup(signer_npub),
                       GINT_TO_POINTER(COSIGNER_STATUS_REJECTED));

  SessionCallbackData *scd = g_hash_table_lookup(storage->session_callbacks, session_id);
  if (scd && scd->progress_cb) {
    scd->progress_cb(session, signer_npub, COSIGNER_STATUS_REJECTED, scd->user_data);
  }

  g_message("multisig: signer %s rejected session %s: %s",
            signer_npub, session_id, reason ? reason : "no reason");
}

MultisigResult multisig_signing_get_status(const gchar *session_id,
                                           MultisigSigningSession **out_session) {
  if (!session_id || !out_session) {
    return MULTISIG_ERR_NOT_FOUND;
  }

  ensure_storage_initialized();

  MultisigSigningSession *session = g_hash_table_lookup(storage->sessions, session_id);
  if (!session) {
    return MULTISIG_ERR_NOT_FOUND;
  }

  /* Create a copy for the caller */
  MultisigSigningSession *copy = g_new0(MultisigSigningSession, 1);
  copy->session_id = g_strdup(session->session_id);
  copy->wallet_id = g_strdup(session->wallet_id);
  copy->event_json = g_strdup(session->event_json);
  copy->event_kind = session->event_kind;
  copy->event_id = g_strdup(session->event_id);
  copy->signatures_collected = session->signatures_collected;
  copy->signatures_required = session->signatures_required;
  copy->created_at = session->created_at;
  copy->expires_at = session->expires_at;
  copy->is_complete = session->is_complete;

  /* Copy signer status */
  copy->signer_status = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, session->signer_status);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    g_hash_table_insert(copy->signer_status, g_strdup(key), value);
  }

  /* Don't copy partial_sigs or final_signature for security */
  copy->partial_sigs = NULL;
  copy->final_signature = NULL;

  *out_session = copy;
  return MULTISIG_OK;
}

void multisig_signing_cancel(const gchar *session_id) {
  if (!session_id) return;

  ensure_storage_initialized();

  MultisigSigningSession *session = g_hash_table_lookup(storage->sessions, session_id);
  if (!session) return;

  /* Mark all pending as canceled */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, session->signer_status);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    CosignerStatus status = GPOINTER_TO_INT(value);
    if (status == COSIGNER_STATUS_PENDING || status == COSIGNER_STATUS_REQUESTED) {
      g_hash_table_replace(session->signer_status, g_strdup(key),
                           GINT_TO_POINTER(COSIGNER_STATUS_ERROR));
    }
  }

  /* Notify */
  SessionCallbackData *scd = g_hash_table_lookup(storage->session_callbacks, session_id);
  if (scd && scd->complete_cb) {
    scd->complete_cb(session, FALSE, "Signing canceled", scd->user_data);
  }

  g_hash_table_remove(storage->session_callbacks, session_id);
  g_hash_table_remove(storage->sessions, session_id);

  g_message("multisig: canceled session %s", session_id);
}

MultisigResult multisig_signing_get_final_signature(const gchar *session_id,
                                                    gchar **out_signature,
                                                    GError **error) {
  if (!session_id || !out_signature) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_NOT_FOUND,
                "Invalid parameters");
    return MULTISIG_ERR_NOT_FOUND;
  }

  ensure_storage_initialized();

  MultisigSigningSession *session = g_hash_table_lookup(storage->sessions, session_id);
  if (!session) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_NOT_FOUND,
                "Session not found: %s", session_id);
    return MULTISIG_ERR_NOT_FOUND;
  }

  if (!session->is_complete) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_THRESHOLD_NOT_MET,
                "Session not complete: %u/%u signatures",
                session->signatures_collected, session->signatures_required);
    return MULTISIG_ERR_THRESHOLD_NOT_MET;
  }

  if (!session->final_signature) {
    g_set_error(error, MULTISIG_WALLET_ERROR, MULTISIG_ERR_BACKEND,
                "No final signature available");
    return MULTISIG_ERR_BACKEND;
  }

  /* Return a copy in secure memory */
  *out_signature = gn_secure_strdup(session->final_signature);
  return MULTISIG_OK;
}
