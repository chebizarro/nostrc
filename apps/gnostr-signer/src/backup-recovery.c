/* backup-recovery.c - Backup and recovery implementation for gnostr-signer
 *
 * Implements NIP-49 encrypted backup and BIP-39 mnemonic import/export.
 * Uses secure memory functions for handling private keys and passwords.
 */
#include "backup-recovery.h"
#include "secure-mem.h"

#include <json-glib/json-glib.h>
#include <nostr/nip49/nip49.h>
#include <nostr/nip49/nip49_g.h>
#include <nostr/nip19/nip19.h>
#include <nostr/crypto/bip39.h>
#include <nip06.h>
#include <keys.h>
#include <nostr-utils.h>

#include <string.h>
#include <stdlib.h>
#include <gio/gio.h>

G_DEFINE_QUARK(gn-backup-error-quark, gn_backup_error)

/* Helper: Check if string is 64-character hex */
static gboolean is_hex_64(const gchar *s) {
  if (!s) return FALSE;
  gsize len = strlen(s);
  if (len != 64) return FALSE;
  for (gsize i = 0; i < len; i++) {
    gchar c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
      return FALSE;
  }
  return TRUE;
}

/* Helper: Convert nsec/hex to raw 32-byte key */
static gboolean parse_private_key(const gchar *input, guint8 out_key[32], GError **error) {
  if (!input || !*input) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_INVALID_KEY,
                "Private key is required");
    return FALSE;
  }

  if (g_str_has_prefix(input, "nsec1")) {
    /* Decode bech32 nsec */
    if (nostr_nip19_decode_nsec(input, out_key) != 0) {
      g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_INVALID_KEY,
                  "Invalid nsec format");
      return FALSE;
    }
    return TRUE;
  } else if (is_hex_64(input)) {
    /* Decode hex */
    if (!nostr_hex2bin(out_key, input, 32)) {
      g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_INVALID_KEY,
                  "Invalid hex key format");
      return FALSE;
    }
    return TRUE;
  } else {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_INVALID_KEY,
                "Key must be nsec1... or 64-character hex");
    return FALSE;
  }
}

/* Helper: Convert 32-byte key to nsec string */
static gchar *key_to_nsec(const guint8 key[32]) {
  gchar *nsec = NULL;
  if (nostr_nip19_encode_nsec(key, &nsec) != 0) {
    return NULL;
  }
  return nsec;
}

/* Helper: Convert 32-byte public key to npub string */
static gchar *pubkey_to_npub(const guint8 pk[32]) {
  gchar *npub = NULL;
  if (nostr_nip19_encode_npub(pk, &npub) != 0) {
    return NULL;
  }
  return npub;
}

/* Helper: Securely clear memory - wraps gnostr_secure_clear */
static void secure_clear(void *ptr, gsize len) {
  gnostr_secure_clear(ptr, len);
}

gboolean gn_backup_export_nip49(const gchar *nsec,
                                 const gchar *password,
                                 GnBackupSecurityLevel security,
                                 gchar **out_ncryptsec,
                                 GError **error) {
  g_return_val_if_fail(out_ncryptsec != NULL, FALSE);
  *out_ncryptsec = NULL;

  if (!password || !*password) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_INVALID_PASSWORD,
                "Password is required for encryption");
    return FALSE;
  }

  guint8 privkey[32];
  if (!parse_private_key(nsec, privkey, error)) {
    return FALSE;
  }

  gchar *ncryptsec = NULL;
  GError *nip49_error = NULL;

  /* Use the GLib wrapper for NIP-49 encryption */
  gboolean ok = nostr_nip49_encrypt_g(privkey,
                                       NOSTR_NIP49_SECURITY_SECURE,
                                       password,
                                       (guint8)security,
                                       &ncryptsec,
                                       &nip49_error);

  /* Securely clear the private key */
  secure_clear(privkey, sizeof(privkey));

  if (!ok) {
    if (nip49_error) {
      g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_ENCRYPT_FAILED,
                  "Encryption failed: %s", nip49_error->message);
      g_error_free(nip49_error);
    } else {
      g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_ENCRYPT_FAILED,
                  "Encryption failed");
    }
    return FALSE;
  }

  *out_ncryptsec = ncryptsec;
  return TRUE;
}

gboolean gn_backup_import_nip49(const gchar *encrypted,
                                 const gchar *password,
                                 gchar **out_nsec,
                                 GError **error) {
  g_return_val_if_fail(out_nsec != NULL, FALSE);
  *out_nsec = NULL;

  if (!encrypted || !*encrypted) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_INVALID_ENCRYPTED,
                "Encrypted key string is required");
    return FALSE;
  }

  if (!g_str_has_prefix(encrypted, "ncryptsec1")) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_INVALID_ENCRYPTED,
                "Invalid format: must start with 'ncryptsec1'");
    return FALSE;
  }

  if (!password || !*password) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_INVALID_PASSWORD,
                "Password is required for decryption");
    return FALSE;
  }

  guint8 privkey[32];
  guint8 security_byte = 0;
  guint8 log_n = 0;
  GError *nip49_error = NULL;

  gboolean ok = nostr_nip49_decrypt_g(encrypted,
                                       password,
                                       privkey,
                                       &security_byte,
                                       &log_n,
                                       &nip49_error);

  if (!ok) {
    if (nip49_error) {
      g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_DECRYPT_FAILED,
                  "Decryption failed: %s (wrong password or corrupted data)",
                  nip49_error->message);
      g_error_free(nip49_error);
    } else {
      g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_DECRYPT_FAILED,
                  "Decryption failed: wrong password or corrupted data");
    }
    return FALSE;
  }

  gchar *nsec = key_to_nsec(privkey);
  secure_clear(privkey, sizeof(privkey));

  if (!nsec) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_DECRYPT_FAILED,
                "Failed to encode decrypted key as nsec");
    return FALSE;
  }

  *out_nsec = nsec;
  return TRUE;
}

gboolean gn_backup_export_mnemonic(const gchar *nsec,
                                    gchar **out_mnemonic,
                                    GError **error) {
  (void)nsec;
  g_return_val_if_fail(out_mnemonic != NULL, FALSE);
  *out_mnemonic = NULL;

  /* Cannot recover mnemonic from derived key - this is mathematically impossible
   * as the mnemonic->seed->key derivation is a one-way function.
   *
   * The mnemonic must be stored separately if the user wants to export it later.
   */
  g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_NOT_AVAILABLE,
              "Cannot recover mnemonic from derived key. "
              "Please use NIP-49 encrypted backup instead, or store your mnemonic separately.");
  return FALSE;
}

gboolean gn_backup_import_mnemonic(const gchar *mnemonic,
                                    const gchar *passphrase,
                                    guint account,
                                    gchar **out_nsec,
                                    GError **error) {
  g_return_val_if_fail(out_nsec != NULL, FALSE);
  *out_nsec = NULL;

  if (!mnemonic || !*mnemonic) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_INVALID_MNEMONIC,
                "Mnemonic is required");
    return FALSE;
  }

  /* Normalize the mnemonic: lowercase, single spaces */
  g_autofree gchar *normalized = g_strdup(mnemonic);
  g_strstrip(normalized);

  /* Convert to lowercase */
  for (gchar *p = normalized; *p; p++) {
    *p = g_ascii_tolower(*p);
  }

  /* Validate the mnemonic */
  if (!nostr_bip39_validate(normalized)) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_INVALID_MNEMONIC,
                "Invalid mnemonic: check word count (12/15/18/21/24) and checksum");
    return FALSE;
  }

  /* Derive seed from mnemonic using BIP-39 with optional passphrase */
  guint8 seed[64];
  if (!nostr_bip39_seed(normalized, passphrase ? passphrase : "", seed)) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_DERIVATION_FAILED,
                "Failed to derive seed from mnemonic");
    return FALSE;
  }

  /* Derive private key from seed using NIP-06 path m/44'/1237'/account'/0/0 */
  gchar *sk_hex = nostr_nip06_private_key_from_seed_account(seed, account);
  secure_clear(seed, sizeof(seed));

  if (!sk_hex) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_DERIVATION_FAILED,
                "Failed to derive private key from seed");
    return FALSE;
  }

  /* Convert hex to binary and then to nsec */
  guint8 privkey[32];
  if (!nostr_hex2bin(privkey, sk_hex, 32)) {
    secure_clear(sk_hex, strlen(sk_hex));
    free(sk_hex);
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_DERIVATION_FAILED,
                "Invalid derived key format");
    return FALSE;
  }
  secure_clear(sk_hex, strlen(sk_hex));
  free(sk_hex);

  gchar *nsec = key_to_nsec(privkey);
  secure_clear(privkey, sizeof(privkey));

  if (!nsec) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_DERIVATION_FAILED,
                "Failed to encode derived key as nsec");
    return FALSE;
  }

  *out_nsec = nsec;
  return TRUE;
}

gboolean gn_backup_generate_mnemonic(gint word_count,
                                      const gchar *passphrase,
                                      gchar **out_mnemonic,
                                      gchar **out_nsec,
                                      GError **error) {
  g_return_val_if_fail(out_mnemonic != NULL, FALSE);
  g_return_val_if_fail(out_nsec != NULL, FALSE);
  *out_mnemonic = NULL;
  *out_nsec = NULL;

  /* Validate word count */
  if (word_count != 12 && word_count != 15 && word_count != 18 &&
      word_count != 21 && word_count != 24) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_INVALID_MNEMONIC,
                "Word count must be 12, 15, 18, 21, or 24");
    return FALSE;
  }

  /* Generate mnemonic */
  gchar *mnemonic = nostr_bip39_generate(word_count);
  if (!mnemonic) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_DERIVATION_FAILED,
                "Failed to generate mnemonic");
    return FALSE;
  }

  /* Derive key from mnemonic */
  gchar *nsec = NULL;
  GError *derive_error = NULL;
  if (!gn_backup_import_mnemonic(mnemonic, passphrase, 0, &nsec, &derive_error)) {
    free(mnemonic);
    g_propagate_error(error, derive_error);
    return FALSE;
  }

  *out_mnemonic = mnemonic;
  *out_nsec = nsec;
  return TRUE;
}

gboolean gn_backup_export_to_file(const gchar *nsec,
                                   const gchar *password,
                                   GnBackupSecurityLevel security,
                                   const gchar *filepath,
                                   GError **error) {
  if (!filepath || !*filepath) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_FILE_IO,
                "File path is required");
    return FALSE;
  }

  gchar *ncryptsec = NULL;
  if (!gn_backup_export_nip49(nsec, password, security, &ncryptsec, error)) {
    return FALSE;
  }

  /* Write to file */
  GError *file_error = NULL;
  gboolean ok = g_file_set_contents(filepath, ncryptsec, -1, &file_error);
  g_free(ncryptsec);

  if (!ok) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_FILE_IO,
                "Failed to write file: %s", file_error->message);
    g_error_free(file_error);
    return FALSE;
  }

  return TRUE;
}

gboolean gn_backup_import_from_file(const gchar *filepath,
                                     const gchar *password,
                                     gchar **out_nsec,
                                     GError **error) {
  g_return_val_if_fail(out_nsec != NULL, FALSE);
  *out_nsec = NULL;

  if (!filepath || !*filepath) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_FILE_IO,
                "File path is required");
    return FALSE;
  }

  /* Read file contents */
  gchar *contents = NULL;
  gsize length = 0;
  GError *file_error = NULL;

  if (!g_file_get_contents(filepath, &contents, &length, &file_error)) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_FILE_IO,
                "Failed to read file: %s", file_error->message);
    g_error_free(file_error);
    return FALSE;
  }

  /* Trim whitespace */
  g_strstrip(contents);

  /* Import from ncryptsec */
  gboolean ok = gn_backup_import_nip49(contents, password, out_nsec, error);
  g_free(contents);

  return ok;
}

gboolean gn_backup_validate_ncryptsec(const gchar *encrypted) {
  if (!encrypted || !*encrypted) return FALSE;

  /* Must start with ncryptsec1 */
  if (!g_str_has_prefix(encrypted, "ncryptsec1")) return FALSE;

  /* Basic length check - ncryptsec should be reasonably long */
  gsize len = strlen(encrypted);
  if (len < 50 || len > 500) return FALSE;

  /* Check for valid bech32 characters after prefix */
  for (gsize i = 10; i < len; i++) {
    gchar c = encrypted[i];
    /* bech32 character set (no 1, b, i, o after HRP) */
    if (!((c >= '0' && c <= '9') ||
          (c >= 'a' && c <= 'z' && c != 'b' && c != 'i' && c != 'o') ||
          (c >= 'A' && c <= 'Z' && c != 'B' && c != 'I' && c != 'O'))) {
      return FALSE;
    }
  }

  return TRUE;
}

gboolean gn_backup_validate_mnemonic(const gchar *mnemonic) {
  if (!mnemonic || !*mnemonic) return FALSE;

  /* Normalize: trim and lowercase */
  g_autofree gchar *normalized = g_strdup(mnemonic);
  g_strstrip(normalized);
  for (gchar *p = normalized; *p; p++) {
    *p = g_ascii_tolower(*p);
  }

  return nostr_bip39_validate(normalized);
}

gboolean gn_backup_get_npub(const gchar *nsec,
                             gchar **out_npub,
                             GError **error) {
  g_return_val_if_fail(out_npub != NULL, FALSE);
  *out_npub = NULL;

  guint8 privkey[32];
  if (!parse_private_key(nsec, privkey, error)) {
    return FALSE;
  }

  /* Get hex representation of private key */
  gchar *sk_hex = nostr_bin2hex(privkey, 32);
  secure_clear(privkey, sizeof(privkey));

  if (!sk_hex) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_INVALID_KEY,
                "Failed to convert key to hex");
    return FALSE;
  }

  /* Derive public key */
  gchar *pk_hex = nostr_key_get_public(sk_hex);
  secure_clear(sk_hex, strlen(sk_hex));
  free(sk_hex);

  if (!pk_hex) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_INVALID_KEY,
                "Failed to derive public key");
    return FALSE;
  }

  /* Convert to binary */
  guint8 pubkey[32];
  if (!nostr_hex2bin(pubkey, pk_hex, 32)) {
    free(pk_hex);
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_INVALID_KEY,
                "Invalid public key format");
    return FALSE;
  }
  free(pk_hex);

  /* Encode as npub */
  gchar *npub = pubkey_to_npub(pubkey);
  if (!npub) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_INVALID_KEY,
                "Failed to encode public key as npub");
    return FALSE;
  }

  *out_npub = npub;
  return TRUE;
}

/* ============================================================
 * Backup File Format with Metadata
 * ============================================================ */

void gn_backup_metadata_free(GnBackupMetadata *meta) {
  if (!meta) return;

  /* Securely clear ncryptsec before freeing */
  if (meta->ncryptsec) {
    gnostr_secure_clear(meta->ncryptsec, strlen(meta->ncryptsec));
    g_free(meta->ncryptsec);
  }

  g_free(meta->identity_name);
  g_free(meta->npub);
  g_free(meta->created_at);
  g_free(meta);
}

/* Helper: Get current ISO 8601 timestamp */
static gchar *get_iso8601_timestamp(void) {
  GDateTime *now = g_date_time_new_now_utc();
  gchar *timestamp = g_date_time_format_iso8601(now);
  g_date_time_unref(now);
  return timestamp;
}

/* Helper: Convert security level to string */
static const gchar *security_level_to_string(GnBackupSecurityLevel level) {
  switch (level) {
    case GN_BACKUP_SECURITY_FAST: return "fast";
    case GN_BACKUP_SECURITY_NORMAL: return "normal";
    case GN_BACKUP_SECURITY_HIGH: return "high";
    case GN_BACKUP_SECURITY_PARANOID: return "paranoid";
    default: return "normal";
  }
}

/* Helper: Parse security level from string */
static GnBackupSecurityLevel security_level_from_string(const gchar *str) {
  if (!str) return GN_BACKUP_SECURITY_NORMAL;
  if (g_strcmp0(str, "fast") == 0) return GN_BACKUP_SECURITY_FAST;
  if (g_strcmp0(str, "high") == 0) return GN_BACKUP_SECURITY_HIGH;
  if (g_strcmp0(str, "paranoid") == 0) return GN_BACKUP_SECURITY_PARANOID;
  return GN_BACKUP_SECURITY_NORMAL;
}

gboolean gn_backup_create_metadata_json(const gchar *nsec,
                                          const gchar *password,
                                          GnBackupSecurityLevel security,
                                          const gchar *identity_name,
                                          gchar **out_json,
                                          GError **error) {
  g_return_val_if_fail(out_json != NULL, FALSE);
  *out_json = NULL;

  /* Create encrypted backup */
  gchar *ncryptsec = NULL;
  if (!gn_backup_export_nip49(nsec, password, security, &ncryptsec, error)) {
    return FALSE;
  }

  /* Get npub */
  gchar *npub = NULL;
  if (!gn_backup_get_npub(nsec, &npub, error)) {
    g_free(ncryptsec);
    return FALSE;
  }

  /* Get timestamp */
  gchar *created_at = get_iso8601_timestamp();

  /* Build JSON object */
  JsonBuilder *builder = json_builder_new();
  json_builder_begin_object(builder);

  json_builder_set_member_name(builder, "version");
  json_builder_add_int_value(builder, 1);

  json_builder_set_member_name(builder, "format");
  json_builder_add_string_value(builder, "gnostr-backup");

  json_builder_set_member_name(builder, "created_at");
  json_builder_add_string_value(builder, created_at);

  if (identity_name && *identity_name) {
    json_builder_set_member_name(builder, "identity_name");
    json_builder_add_string_value(builder, identity_name);
  }

  json_builder_set_member_name(builder, "npub");
  json_builder_add_string_value(builder, npub);

  json_builder_set_member_name(builder, "ncryptsec");
  json_builder_add_string_value(builder, ncryptsec);

  json_builder_set_member_name(builder, "security_level");
  json_builder_add_string_value(builder, security_level_to_string(security));

  json_builder_end_object(builder);

  /* Generate JSON string */
  JsonGenerator *gen = json_generator_new();
  json_generator_set_pretty(gen, TRUE);
  json_generator_set_indent(gen, 2);
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(gen, root);

  *out_json = json_generator_to_data(gen, NULL);

  /* Cleanup */
  json_node_unref(root);
  g_object_unref(gen);
  g_object_unref(builder);
  g_free(created_at);
  g_free(npub);

  /* Securely clear ncryptsec */
  gnostr_secure_clear(ncryptsec, strlen(ncryptsec));
  g_free(ncryptsec);

  return TRUE;
}

gboolean gn_backup_parse_metadata_json(const gchar *json,
                                         GnBackupMetadata **out_metadata,
                                         GError **error) {
  g_return_val_if_fail(out_metadata != NULL, FALSE);
  *out_metadata = NULL;

  if (!json || !*json) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_INVALID_ENCRYPTED,
                "Empty JSON input");
    return FALSE;
  }

  /* Parse JSON */
  JsonParser *parser = json_parser_new();
  GError *parse_error = NULL;

  if (!json_parser_load_from_data(parser, json, -1, &parse_error)) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_INVALID_ENCRYPTED,
                "Invalid JSON: %s", parse_error->message);
    g_error_free(parse_error);
    g_object_unref(parser);
    return FALSE;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root)) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_INVALID_ENCRYPTED,
                "Invalid backup format: expected JSON object");
    g_object_unref(parser);
    return FALSE;
  }

  JsonObject *obj = json_node_get_object(root);

  /* Validate format */
  const gchar *format = NULL;
  if (json_object_has_member(obj, "format")) {
    format = json_object_get_string_member(obj, "format");
  }
  if (!format || g_strcmp0(format, "gnostr-backup") != 0) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_INVALID_ENCRYPTED,
                "Invalid backup format: expected 'gnostr-backup'");
    g_object_unref(parser);
    return FALSE;
  }

  /* Get ncryptsec (required) */
  if (!json_object_has_member(obj, "ncryptsec")) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_INVALID_ENCRYPTED,
                "Invalid backup: missing 'ncryptsec' field");
    g_object_unref(parser);
    return FALSE;
  }

  const gchar *ncryptsec = json_object_get_string_member(obj, "ncryptsec");
  if (!ncryptsec || !gn_backup_validate_ncryptsec(ncryptsec)) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_INVALID_ENCRYPTED,
                "Invalid backup: invalid 'ncryptsec' format");
    g_object_unref(parser);
    return FALSE;
  }

  /* Create metadata structure */
  GnBackupMetadata *meta = g_new0(GnBackupMetadata, 1);
  meta->ncryptsec = g_strdup(ncryptsec);

  /* Optional fields */
  if (json_object_has_member(obj, "version")) {
    meta->version = (gint)json_object_get_int_member(obj, "version");
  } else {
    meta->version = 1;
  }

  if (json_object_has_member(obj, "identity_name")) {
    meta->identity_name = g_strdup(json_object_get_string_member(obj, "identity_name"));
  }

  if (json_object_has_member(obj, "npub")) {
    meta->npub = g_strdup(json_object_get_string_member(obj, "npub"));
  }

  if (json_object_has_member(obj, "created_at")) {
    meta->created_at = g_strdup(json_object_get_string_member(obj, "created_at"));
  }

  if (json_object_has_member(obj, "security_level")) {
    const gchar *level_str = json_object_get_string_member(obj, "security_level");
    meta->security_level = security_level_from_string(level_str);
  } else {
    meta->security_level = GN_BACKUP_SECURITY_NORMAL;
  }

  g_object_unref(parser);
  *out_metadata = meta;
  return TRUE;
}

gboolean gn_backup_export_to_file_with_metadata(const gchar *nsec,
                                                  const gchar *password,
                                                  GnBackupSecurityLevel security,
                                                  const gchar *identity_name,
                                                  const gchar *filepath,
                                                  GError **error) {
  if (!filepath || !*filepath) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_FILE_IO,
                "File path is required");
    return FALSE;
  }

  /* Create JSON with metadata */
  gchar *json = NULL;
  if (!gn_backup_create_metadata_json(nsec, password, security, identity_name, &json, error)) {
    return FALSE;
  }

  /* Write to file */
  GError *file_error = NULL;
  gboolean ok = g_file_set_contents(filepath, json, -1, &file_error);

  /* Securely clear and free JSON */
  gnostr_secure_clear(json, strlen(json));
  g_free(json);

  if (!ok) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_FILE_IO,
                "Failed to write file: %s", file_error->message);
    g_error_free(file_error);
    return FALSE;
  }

  return TRUE;
}

gboolean gn_backup_import_from_file_with_metadata(const gchar *filepath,
                                                    const gchar *password,
                                                    gchar **out_nsec,
                                                    GnBackupMetadata **out_metadata,
                                                    GError **error) {
  g_return_val_if_fail(out_nsec != NULL, FALSE);
  *out_nsec = NULL;
  if (out_metadata) *out_metadata = NULL;

  if (!filepath || !*filepath) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_FILE_IO,
                "File path is required");
    return FALSE;
  }

  /* Read file contents */
  gchar *contents = NULL;
  gsize length = 0;
  GError *file_error = NULL;

  if (!g_file_get_contents(filepath, &contents, &length, &file_error)) {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_FILE_IO,
                "Failed to read file: %s", file_error->message);
    g_error_free(file_error);
    return FALSE;
  }

  /* Trim whitespace */
  g_strstrip(contents);

  gboolean success = FALSE;
  gchar *ncryptsec_to_decrypt = NULL;

  /* Check if this is JSON format or plain ncryptsec */
  if (contents[0] == '{') {
    /* JSON format with metadata */
    GnBackupMetadata *meta = NULL;
    GError *parse_error = NULL;

    if (!gn_backup_parse_metadata_json(contents, &meta, &parse_error)) {
      /* Securely clear contents before freeing */
      gnostr_secure_clear(contents, length);
      g_free(contents);
      g_propagate_error(error, parse_error);
      return FALSE;
    }

    ncryptsec_to_decrypt = g_strdup(meta->ncryptsec);

    if (out_metadata) {
      *out_metadata = meta;
    } else {
      gn_backup_metadata_free(meta);
    }
  } else if (g_str_has_prefix(contents, "ncryptsec1")) {
    /* Legacy plain ncryptsec format */
    ncryptsec_to_decrypt = g_strdup(contents);
  } else {
    g_set_error(error, GN_BACKUP_ERROR, GN_BACKUP_ERROR_INVALID_ENCRYPTED,
                "Unrecognized backup format");
    gnostr_secure_clear(contents, length);
    g_free(contents);
    return FALSE;
  }

  /* Securely clear file contents */
  gnostr_secure_clear(contents, length);
  g_free(contents);

  /* Decrypt the ncryptsec */
  success = gn_backup_import_nip49(ncryptsec_to_decrypt, password, out_nsec, error);

  /* Securely clear ncryptsec */
  if (ncryptsec_to_decrypt) {
    gnostr_secure_clear(ncryptsec_to_decrypt, strlen(ncryptsec_to_decrypt));
    g_free(ncryptsec_to_decrypt);
  }

  /* If decryption failed and we have metadata, free it */
  if (!success && out_metadata && *out_metadata) {
    gn_backup_metadata_free(*out_metadata);
    *out_metadata = NULL;
  }

  return success;
}
