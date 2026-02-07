/**
 * SPDX-License-Identifier: MIT
 *
 * GNostrKeys: GObject wrapper for Nostr key operations
 *
 * Provides a modern GObject implementation with:
 * - Key generation and import (hex/nsec)
 * - Public key derivation
 * - Schnorr signing (BIP-340)
 * - NIP-04 encryption/decryption (legacy)
 * - NIP-44 encryption/decryption (recommended)
 * - Secure memory handling for private keys
 */

#include "nostr_keys.h"
#include <glib.h>
#include <string.h>

/* Core libnostr headers */
#include "keys.h"
#include "nostr-keys.h"
#include "secure_buf.h"

/* NIP headers - include nostr subdirectory versions for full API */
#include "nostr/nip04.h"
#include "nostr/nip44/nip44.h"

/* Property IDs */
enum {
    PROP_0,
    PROP_PUBKEY,
    PROP_HAS_PRIVATE_KEY,
    N_PROPERTIES
};

/* Signal indices */
enum {
    SIGNAL_KEY_GENERATED,
    SIGNAL_KEY_IMPORTED,
    SIGNAL_SIGNED,
    SIGNAL_ENCRYPTED,
    SIGNAL_DECRYPTED,
    N_SIGNALS
};

static GParamSpec *obj_properties[N_PROPERTIES] = { NULL, };
static guint gnostr_keys_signals[N_SIGNALS] = { 0 };

struct _GNostrKeys {
    GObject parent_instance;
    gchar *pubkey;                   /* Public key in hex (64 chars) */
    nostr_secure_buf privkey;        /* Private key in secure buffer (32 bytes raw) */
    gchar *privkey_hex;              /* Private key in hex for API compat (64 chars) */
};

G_DEFINE_TYPE(GNostrKeys, gnostr_keys, G_TYPE_OBJECT)

/* Forward declarations for internal helpers */
static gboolean hex_to_bytes(const gchar *hex, guint8 *out, gsize out_len);
static gchar *bytes_to_hex(const guint8 *bytes, gsize len) G_GNUC_UNUSED;

static void
gnostr_keys_get_property(GObject    *object,
                         guint       property_id,
                         GValue     *value,
                         GParamSpec *pspec)
{
    GNostrKeys *self = GNOSTR_KEYS(object);

    switch (property_id) {
    case PROP_PUBKEY:
        g_value_set_string(value, self->pubkey);
        break;
    case PROP_HAS_PRIVATE_KEY:
        g_value_set_boolean(value, self->privkey.ptr != NULL);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
gnostr_keys_finalize(GObject *object)
{
    GNostrKeys *self = GNOSTR_KEYS(object);

    /* Securely wipe and free private key */
    if (self->privkey.ptr != NULL) {
        secure_free(&self->privkey);
    }

    /* Wipe hex representation too */
    if (self->privkey_hex != NULL) {
        secure_wipe(self->privkey_hex, strlen(self->privkey_hex));
        g_free(self->privkey_hex);
        self->privkey_hex = NULL;
    }

    g_free(self->pubkey);
    self->pubkey = NULL;

    G_OBJECT_CLASS(gnostr_keys_parent_class)->finalize(object);
}

static void
gnostr_keys_class_init(GNostrKeysClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->get_property = gnostr_keys_get_property;
    object_class->finalize = gnostr_keys_finalize;

    /**
     * GNostrKeys:pubkey:
     *
     * The public key as a 64-character hex string.
     */
    obj_properties[PROP_PUBKEY] =
        g_param_spec_string("pubkey",
                            "Public Key",
                            "Public key in hex format",
                            NULL,
                            G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

    /**
     * GNostrKeys:has-private-key:
     *
     * Whether a private key is loaded.
     */
    obj_properties[PROP_HAS_PRIVATE_KEY] =
        g_param_spec_boolean("has-private-key",
                             "Has Private Key",
                             "Whether a private key is available",
                             FALSE,
                             G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPERTIES, obj_properties);

    /**
     * GNostrKeys::key-generated:
     * @self: the keys object
     *
     * Emitted after a new keypair has been generated.
     */
    gnostr_keys_signals[SIGNAL_KEY_GENERATED] =
        g_signal_new("key-generated",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 0);

    /**
     * GNostrKeys::key-imported:
     * @self: the keys object
     *
     * Emitted after a key has been successfully imported.
     */
    gnostr_keys_signals[SIGNAL_KEY_IMPORTED] =
        g_signal_new("key-imported",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 0);

    /**
     * GNostrKeys::signed:
     * @self: the keys object
     * @signature: the generated signature
     *
     * Emitted after a signing operation completes.
     */
    gnostr_keys_signals[SIGNAL_SIGNED] =
        g_signal_new("signed",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 1, G_TYPE_STRING);

    /**
     * GNostrKeys::encrypted:
     * @self: the keys object
     *
     * Emitted after an encryption operation completes.
     */
    gnostr_keys_signals[SIGNAL_ENCRYPTED] =
        g_signal_new("encrypted",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 0);

    /**
     * GNostrKeys::decrypted:
     * @self: the keys object
     *
     * Emitted after a decryption operation completes.
     */
    gnostr_keys_signals[SIGNAL_DECRYPTED] =
        g_signal_new("decrypted",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 0);
}

static void
gnostr_keys_init(GNostrKeys *self)
{
    self->pubkey = NULL;
    self->privkey.ptr = NULL;
    self->privkey.len = 0;
    self->privkey.locked = FALSE;
    self->privkey_hex = NULL;
}

/* Internal: load a private key from hex string */
static gboolean
gnostr_keys_load_privkey_hex(GNostrKeys *self, const gchar *privkey_hex, GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_KEYS(self), FALSE);
    g_return_val_if_fail(privkey_hex != NULL, FALSE);

    /* Validate hex length */
    gsize len = strlen(privkey_hex);
    if (len != 64) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_INVALID_KEY,
                    "Invalid private key: expected 64 hex characters, got %zu", len);
        return FALSE;
    }

    /* Allocate secure buffer for raw key */
    nostr_secure_buf new_privkey = secure_alloc(32);
    if (new_privkey.ptr == NULL) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_INVALID_KEY,
                    "Failed to allocate secure memory for private key");
        return FALSE;
    }

    /* Convert hex to bytes */
    if (!hex_to_bytes(privkey_hex, new_privkey.ptr, 32)) {
        secure_free(&new_privkey);
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_INVALID_KEY,
                    "Invalid hex encoding in private key");
        return FALSE;
    }

    /* Clear any existing key */
    if (self->privkey.ptr != NULL) {
        secure_free(&self->privkey);
    }
    if (self->privkey_hex != NULL) {
        secure_wipe(self->privkey_hex, strlen(self->privkey_hex));
        g_free(self->privkey_hex);
    }

    /* Store new key */
    self->privkey = new_privkey;
    self->privkey_hex = g_strdup(privkey_hex);

    /* Derive public key */
    char *derived_pubkey = nostr_key_get_public(privkey_hex);
    if (derived_pubkey == NULL) {
        secure_free(&self->privkey);
        secure_wipe(self->privkey_hex, 64);
        g_free(self->privkey_hex);
        self->privkey_hex = NULL;
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_INVALID_KEY,
                    "Failed to derive public key from private key");
        return FALSE;
    }

    g_free(self->pubkey);
    self->pubkey = g_strdup(derived_pubkey);
    free(derived_pubkey);

    /* Notify property changes */
    g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_PUBKEY]);
    g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_HAS_PRIVATE_KEY]);

    return TRUE;
}

/* Public API */

GNostrKeys *
gnostr_keys_new(void)
{
    GNostrKeys *self = g_object_new(GNOSTR_TYPE_KEYS, NULL);

    /* Generate a new keypair */
    char *privkey_hex = nostr_key_generate_private();
    if (privkey_hex == NULL) {
        g_warning("gnostr_keys_new: Failed to generate private key");
        return self;
    }

    GError *error = NULL;
    if (gnostr_keys_load_privkey_hex(self, privkey_hex, &error)) {
        g_signal_emit(self, gnostr_keys_signals[SIGNAL_KEY_GENERATED], 0);
    } else {
        g_warning("gnostr_keys_new: %s", error->message);
        g_error_free(error);
    }

    /* Securely wipe the temporary */
    secure_wipe(privkey_hex, strlen(privkey_hex));
    free(privkey_hex);

    return self;
}

GNostrKeys *
gnostr_keys_new_from_hex(const gchar *privkey_hex, GError **error)
{
    g_return_val_if_fail(privkey_hex != NULL, NULL);

    GNostrKeys *self = g_object_new(GNOSTR_TYPE_KEYS, NULL);

    if (!gnostr_keys_load_privkey_hex(self, privkey_hex, error)) {
        g_object_unref(self);
        return NULL;
    }

    g_signal_emit(self, gnostr_keys_signals[SIGNAL_KEY_IMPORTED], 0);

    return self;
}

GNostrKeys *
gnostr_keys_new_from_nsec(const gchar *nsec, GError **error)
{
    g_return_val_if_fail(nsec != NULL, NULL);

    /* Check for nsec prefix */
    if (!g_str_has_prefix(nsec, "nsec1")) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_INVALID_KEY,
                    "Invalid nsec format: must start with 'nsec1'");
        return NULL;
    }

    /* Decode bech32 nsec to hex - this requires NIP-19 decoding */
    /* For now, we rely on the nip19 library if available */
    /* TODO: Add proper NIP-19 bech32 decoding support */
    g_set_error(error,
                NOSTR_ERROR,
                NOSTR_ERROR_INVALID_KEY,
                "nsec import not yet implemented - use hex format");
    return NULL;
}

GNostrKeys *
gnostr_keys_new_pubkey_only(const gchar *pubkey_hex, GError **error)
{
    g_return_val_if_fail(pubkey_hex != NULL, NULL);

    /* Validate public key */
    if (!nostr_key_is_valid_public_hex(pubkey_hex)) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_INVALID_KEY,
                    "Invalid public key format");
        return NULL;
    }

    GNostrKeys *self = g_object_new(GNOSTR_TYPE_KEYS, NULL);
    self->pubkey = g_strdup(pubkey_hex);

    g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_PUBKEY]);

    return self;
}

const gchar *
gnostr_keys_get_pubkey(GNostrKeys *self)
{
    g_return_val_if_fail(GNOSTR_IS_KEYS(self), NULL);
    return self->pubkey;
}

gchar *
gnostr_keys_get_npub(GNostrKeys *self)
{
    g_return_val_if_fail(GNOSTR_IS_KEYS(self), NULL);

    if (self->pubkey == NULL)
        return NULL;

    /* TODO: Add proper NIP-19 bech32 encoding */
    /* For now, return NULL - full implementation requires bech32 library */
    return NULL;
}

gboolean
gnostr_keys_has_private_key(GNostrKeys *self)
{
    g_return_val_if_fail(GNOSTR_IS_KEYS(self), FALSE);
    return self->privkey.ptr != NULL;
}

gchar *
gnostr_keys_sign(GNostrKeys *self, const gchar *message, GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_KEYS(self), NULL);
    g_return_val_if_fail(message != NULL, NULL);

    if (self->privkey.ptr == NULL) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_SIGNATURE_FAILED,
                    "No private key available for signing");
        return NULL;
    }

    /* Signing requires the nostr_event_sign or lower-level secp256k1 API */
    /* For now, we expose only through GNostrEvent which handles this */
    g_set_error(error,
                NOSTR_ERROR,
                NOSTR_ERROR_SIGNATURE_FAILED,
                "Direct signing not yet implemented - use GNostrEvent::sign");
    return NULL;
}

gboolean
gnostr_keys_verify(GNostrKeys *self, const gchar *message, const gchar *signature, GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_KEYS(self), FALSE);
    g_return_val_if_fail(message != NULL, FALSE);
    g_return_val_if_fail(signature != NULL, FALSE);

    if (self->pubkey == NULL) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_SIGNATURE_INVALID,
                    "No public key available for verification");
        return FALSE;
    }

    /* Verification requires secp256k1 schnorr verify */
    /* For now, exposed only through GNostrEvent::verify */
    g_set_error(error,
                NOSTR_ERROR,
                NOSTR_ERROR_SIGNATURE_INVALID,
                "Direct verification not yet implemented - use GNostrEvent::verify");
    return FALSE;
}

gchar *
gnostr_keys_nip04_encrypt(GNostrKeys *self,
                          const gchar *plaintext,
                          const gchar *recipient_pubkey,
                          GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_KEYS(self), NULL);
    g_return_val_if_fail(plaintext != NULL, NULL);
    g_return_val_if_fail(recipient_pubkey != NULL, NULL);

    if (self->privkey_hex == NULL) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_ENCRYPTION_FAILED,
                    "No private key available for encryption");
        return NULL;
    }

    /* Create secure buffer from hex for the encrypt call */
    nostr_secure_buf sender_seckey = secure_alloc(32);
    if (sender_seckey.ptr == NULL) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_ENCRYPTION_FAILED,
                    "Failed to allocate secure memory");
        return NULL;
    }
    hex_to_bytes(self->privkey_hex, sender_seckey.ptr, 32);

    char *out_content = NULL;
    char *out_error = NULL;

    int result = nostr_nip04_encrypt_secure(
        plaintext,
        recipient_pubkey,
        &sender_seckey,
        &out_content,
        &out_error);

    secure_free(&sender_seckey);

    if (result != 0) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_ENCRYPTION_FAILED,
                    "NIP-04 encryption failed: %s",
                    out_error ? out_error : "unknown error");
        if (out_error) free(out_error);
        return NULL;
    }

    g_signal_emit(self, gnostr_keys_signals[SIGNAL_ENCRYPTED], 0);

    gchar *result_str = g_strdup(out_content);
    free(out_content);
    return result_str;
}

gchar *
gnostr_keys_nip04_decrypt(GNostrKeys *self,
                          const gchar *ciphertext,
                          const gchar *sender_pubkey,
                          GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_KEYS(self), NULL);
    g_return_val_if_fail(ciphertext != NULL, NULL);
    g_return_val_if_fail(sender_pubkey != NULL, NULL);

    if (self->privkey_hex == NULL) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_DECRYPTION_FAILED,
                    "No private key available for decryption");
        return NULL;
    }

    /* Create secure buffer from hex */
    nostr_secure_buf receiver_seckey = secure_alloc(32);
    if (receiver_seckey.ptr == NULL) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_DECRYPTION_FAILED,
                    "Failed to allocate secure memory");
        return NULL;
    }
    hex_to_bytes(self->privkey_hex, receiver_seckey.ptr, 32);

    char *out_plaintext = NULL;
    char *out_error = NULL;

    int result = nostr_nip04_decrypt_secure(
        ciphertext,
        sender_pubkey,
        &receiver_seckey,
        &out_plaintext,
        &out_error);

    secure_free(&receiver_seckey);

    if (result != 0) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_DECRYPTION_FAILED,
                    "NIP-04 decryption failed: %s",
                    out_error ? out_error : "unknown error");
        if (out_error) free(out_error);
        return NULL;
    }

    g_signal_emit(self, gnostr_keys_signals[SIGNAL_DECRYPTED], 0);

    gchar *result_str = g_strdup(out_plaintext);
    free(out_plaintext);
    return result_str;
}

gchar *
gnostr_keys_nip44_encrypt(GNostrKeys *self,
                          const gchar *plaintext,
                          const gchar *recipient_pubkey,
                          GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_KEYS(self), NULL);
    g_return_val_if_fail(plaintext != NULL, NULL);
    g_return_val_if_fail(recipient_pubkey != NULL, NULL);

    if (self->privkey.ptr == NULL) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_ENCRYPTION_FAILED,
                    "No private key available for encryption");
        return NULL;
    }

    /* Validate recipient pubkey */
    gsize pk_len = strlen(recipient_pubkey);
    if (pk_len != 64) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_INVALID_KEY,
                    "Invalid recipient public key length");
        return NULL;
    }

    /* Convert recipient pubkey from hex to bytes */
    guint8 recipient_pk_bytes[32];
    if (!hex_to_bytes(recipient_pubkey, recipient_pk_bytes, 32)) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_INVALID_KEY,
                    "Invalid hex encoding in recipient public key");
        return NULL;
    }

    char *out_base64 = NULL;

    int result = nostr_nip44_encrypt_v2(
        self->privkey.ptr,
        recipient_pk_bytes,
        (const guint8 *)plaintext,
        strlen(plaintext),
        &out_base64);

    if (result != 0 || out_base64 == NULL) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_ENCRYPTION_FAILED,
                    "NIP-44 encryption failed");
        return NULL;
    }

    g_signal_emit(self, gnostr_keys_signals[SIGNAL_ENCRYPTED], 0);

    gchar *result_str = g_strdup(out_base64);
    free(out_base64);
    return result_str;
}

gchar *
gnostr_keys_nip44_decrypt(GNostrKeys *self,
                          const gchar *ciphertext,
                          const gchar *sender_pubkey,
                          GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_KEYS(self), NULL);
    g_return_val_if_fail(ciphertext != NULL, NULL);
    g_return_val_if_fail(sender_pubkey != NULL, NULL);

    if (self->privkey.ptr == NULL) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_DECRYPTION_FAILED,
                    "No private key available for decryption");
        return NULL;
    }

    /* Validate sender pubkey */
    gsize pk_len = strlen(sender_pubkey);
    if (pk_len != 64) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_INVALID_KEY,
                    "Invalid sender public key length");
        return NULL;
    }

    /* Convert sender pubkey from hex to bytes */
    guint8 sender_pk_bytes[32];
    if (!hex_to_bytes(sender_pubkey, sender_pk_bytes, 32)) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_INVALID_KEY,
                    "Invalid hex encoding in sender public key");
        return NULL;
    }

    guint8 *out_plaintext = NULL;
    gsize out_len = 0;

    int result = nostr_nip44_decrypt_v2(
        self->privkey.ptr,
        sender_pk_bytes,
        ciphertext,
        &out_plaintext,
        &out_len);

    if (result != 0 || out_plaintext == NULL) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_DECRYPTION_FAILED,
                    "NIP-44 decryption failed");
        return NULL;
    }

    g_signal_emit(self, gnostr_keys_signals[SIGNAL_DECRYPTED], 0);

    /* Convert to null-terminated string */
    gchar *result_str = g_strndup((gchar *)out_plaintext, out_len);
    free(out_plaintext);
    return result_str;
}

gboolean
gnostr_keys_is_valid_pubkey(const gchar *pubkey_hex)
{
    if (pubkey_hex == NULL)
        return FALSE;

    return nostr_key_is_valid_public_hex(pubkey_hex);
}

gboolean
gnostr_keys_generate_new(GNostrKeys *self, GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_KEYS(self), FALSE);

    char *privkey_hex = nostr_key_generate_private();
    if (privkey_hex == NULL) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_INVALID_KEY,
                    "Failed to generate private key");
        return FALSE;
    }

    gboolean result = gnostr_keys_load_privkey_hex(self, privkey_hex, error);

    /* Securely wipe temporary */
    secure_wipe(privkey_hex, strlen(privkey_hex));
    free(privkey_hex);

    if (result) {
        g_signal_emit(self, gnostr_keys_signals[SIGNAL_KEY_GENERATED], 0);
    }

    return result;
}

/* Internal helpers */

static gboolean
hex_to_bytes(const gchar *hex, guint8 *out, gsize out_len)
{
    gsize hex_len = strlen(hex);
    if (hex_len != out_len * 2)
        return FALSE;

    for (gsize i = 0; i < out_len; i++) {
        gchar byte_str[3] = { hex[i*2], hex[i*2+1], '\0' };
        gchar *endptr;
        gulong val = strtoul(byte_str, &endptr, 16);
        if (*endptr != '\0')
            return FALSE;
        out[i] = (guint8)val;
    }

    return TRUE;
}

static gchar *
bytes_to_hex(const guint8 *bytes, gsize len)
{
    gchar *hex = g_malloc(len * 2 + 1);
    for (gsize i = 0; i < len; i++) {
        g_snprintf(hex + i * 2, 3, "%02x", bytes[i]);
    }
    hex[len * 2] = '\0';
    return hex;
}
