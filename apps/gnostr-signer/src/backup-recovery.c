/* backup-recovery.c - Backup and recovery implementation for gnostr-signer
 *
 * Implements NIP-49 encrypted backup and BIP-39 mnemonic import/export.
 * Uses secure memory functions for handling private keys and passwords.
 */
#include "backup-recovery.h"
#include "secure-mem.h"

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
