/* key_provider_ed25519.c - Ed25519 key provider implementation
 *
 * Provides Ed25519 key operations for future NIP compatibility.
 * Uses libsodium if available, otherwise falls back to a stub
 * implementation that reports unsupported.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "key_provider_ed25519.h"
#include "secure-mem.h"
#include "secure-memory.h"
#include <string.h>

#ifdef HAVE_SODIUM
#include <sodium.h>
#define ED25519_AVAILABLE 1
#else
#define ED25519_AVAILABLE 0
#endif

/* ============================================================================
 * Private structure
 * ============================================================================ */

struct _GnKeyProviderEd25519 {
  GObject parent_instance;
  gboolean initialized;
};

/* Forward declarations for interface implementation */
static void gn_key_provider_ed25519_iface_init(GnKeyProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GnKeyProviderEd25519,
                        gn_key_provider_ed25519,
                        G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GN_TYPE_KEY_PROVIDER,
                                              gn_key_provider_ed25519_iface_init))

/* Singleton instance */
static GnKeyProviderEd25519 *default_instance = NULL;

/* ============================================================================
 * Helper functions
 * ============================================================================ */

static gboolean
hex_to_bin(const gchar *hex, guint8 *out, gsize expected_len)
{
  if (!hex || !out) return FALSE;

  gsize hex_len = strlen(hex);
  if (hex_len != expected_len * 2) return FALSE;

  for (gsize i = 0; i < expected_len; i++) {
    guint8 hi = 0, lo = 0;
    gchar c;

    c = hex[i * 2];
    if (c >= '0' && c <= '9') hi = c - '0';
    else if (c >= 'a' && c <= 'f') hi = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') hi = c - 'A' + 10;
    else return FALSE;

    c = hex[i * 2 + 1];
    if (c >= '0' && c <= '9') lo = c - '0';
    else if (c >= 'a' && c <= 'f') lo = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') lo = c - 'A' + 10;
    else return FALSE;

    out[i] = (hi << 4) | lo;
  }

  return TRUE;
}

static gchar *
bin_to_hex(const guint8 *bin, gsize len)
{
  static const gchar hexd[] = "0123456789abcdef";
  gchar *out = g_malloc(len * 2 + 1);

  for (gsize i = 0; i < len; i++) {
    out[i * 2] = hexd[(bin[i] >> 4) & 0x0F];
    out[i * 2 + 1] = hexd[bin[i] & 0x0F];
  }
  out[len * 2] = '\0';

  return out;
}

/* ============================================================================
 * GnKeyProvider interface implementation
 * ============================================================================ */

static GnKeyType
ed25519_get_key_type(GnKeyProvider *self)
{
  (void)self;
  return GN_KEY_TYPE_ED25519;
}

static const gchar *
ed25519_get_key_type_name(GnKeyProvider *self)
{
  (void)self;
  return "ed25519";
}

static gsize
ed25519_get_private_key_size(GnKeyProvider *self)
{
  (void)self;
  return GN_ED25519_PRIVATE_KEY_SIZE;
}

static gsize
ed25519_get_public_key_size(GnKeyProvider *self)
{
  (void)self;
  return GN_ED25519_PUBLIC_KEY_SIZE;
}

static gsize
ed25519_get_signature_size(GnKeyProvider *self)
{
  (void)self;
  return GN_ED25519_SIGNATURE_SIZE;
}

static gboolean
ed25519_derive_public_key(GnKeyProvider  *self,
                          const guint8   *private_key,
                          gsize           private_key_len,
                          guint8         *public_key_out,
                          gsize          *public_key_len_out,
                          GError        **error)
{
  (void)self;

  if (private_key_len != GN_ED25519_PRIVATE_KEY_SIZE) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INVALID_KEY,
                "Invalid private key length: expected %d, got %zu",
                GN_ED25519_PRIVATE_KEY_SIZE, private_key_len);
    return FALSE;
  }

#if ED25519_AVAILABLE
  /* In libsodium, the "seed" is the 32-byte private key
   * We derive the full 64-byte secret key internally */
  guint8 sk[crypto_sign_SECRETKEYBYTES];  /* 64 bytes */
  guint8 pk[crypto_sign_PUBLICKEYBYTES];  /* 32 bytes */

  /* seed_keypair derives both from the 32-byte seed */
  if (crypto_sign_seed_keypair(pk, sk, private_key) != 0) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INTERNAL,
                "Failed to derive Ed25519 keypair");
    return FALSE;
  }

  memcpy(public_key_out, pk, GN_ED25519_PUBLIC_KEY_SIZE);

  /* Securely clear the full secret key */
  sodium_memzero(sk, sizeof(sk));

  if (public_key_len_out) {
    *public_key_len_out = GN_ED25519_PUBLIC_KEY_SIZE;
  }

  return TRUE;

#else
  (void)private_key;
  (void)public_key_out;
  (void)public_key_len_out;
  g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_UNSUPPORTED,
              "Ed25519 not available (requires libsodium)");
  return FALSE;
#endif
}

static gboolean
ed25519_sign(GnKeyProvider  *self,
             const guint8   *private_key,
             gsize           private_key_len,
             const guint8   *message_hash,
             gsize           hash_len,
             guint8         *signature_out,
             gsize          *signature_len_out,
             GError        **error)
{
  (void)self;

  if (private_key_len != GN_ED25519_PRIVATE_KEY_SIZE) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INVALID_KEY,
                "Invalid private key length");
    return FALSE;
  }

  if (hash_len != 32) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INVALID_KEY,
                "Invalid hash length: expected 32, got %zu", hash_len);
    return FALSE;
  }

#if ED25519_AVAILABLE
  /* Derive full secret key from seed */
  guint8 sk[crypto_sign_SECRETKEYBYTES];
  guint8 pk[crypto_sign_PUBLICKEYBYTES];

  if (crypto_sign_seed_keypair(pk, sk, private_key) != 0) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INTERNAL,
                "Failed to derive keypair for signing");
    return FALSE;
  }

  /* Ed25519 signs the message directly; we sign the hash for API consistency */
  unsigned long long sig_len = 0;
  guint8 sig_with_msg[crypto_sign_BYTES + hash_len];

  if (crypto_sign(sig_with_msg, &sig_len, message_hash, hash_len, sk) != 0) {
    sodium_memzero(sk, sizeof(sk));
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_SIGNING_FAILED,
                "Ed25519 signing failed");
    return FALSE;
  }

  sodium_memzero(sk, sizeof(sk));

  /* Extract just the signature (first 64 bytes) */
  memcpy(signature_out, sig_with_msg, GN_ED25519_SIGNATURE_SIZE);

  if (signature_len_out) {
    *signature_len_out = GN_ED25519_SIGNATURE_SIZE;
  }

  return TRUE;

#else
  (void)private_key;
  (void)message_hash;
  (void)signature_out;
  (void)signature_len_out;
  g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_UNSUPPORTED,
              "Ed25519 signing not available (requires libsodium)");
  return FALSE;
#endif
}

static gboolean
ed25519_verify(GnKeyProvider  *self,
               const guint8   *public_key,
               gsize           public_key_len,
               const guint8   *message_hash,
               gsize           hash_len,
               const guint8   *signature,
               gsize           signature_len,
               GError        **error)
{
  (void)self;

  if (public_key_len != GN_ED25519_PUBLIC_KEY_SIZE) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INVALID_KEY,
                "Invalid public key length");
    return FALSE;
  }

  if (hash_len != 32) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INVALID_KEY,
                "Invalid hash length");
    return FALSE;
  }

  if (signature_len != GN_ED25519_SIGNATURE_SIZE) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INVALID_KEY,
                "Invalid signature length");
    return FALSE;
  }

#if ED25519_AVAILABLE
  /* Reconstruct signed message format (signature || message) */
  guint8 signed_msg[GN_ED25519_SIGNATURE_SIZE + hash_len];
  memcpy(signed_msg, signature, GN_ED25519_SIGNATURE_SIZE);
  memcpy(signed_msg + GN_ED25519_SIGNATURE_SIZE, message_hash, hash_len);

  guint8 msg_out[hash_len];
  unsigned long long msg_len = 0;

  if (crypto_sign_open(msg_out, &msg_len, signed_msg, sizeof(signed_msg), public_key) != 0) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_VERIFICATION_FAILED,
                "Ed25519 signature verification failed");
    return FALSE;
  }

  return TRUE;

#else
  (void)public_key;
  (void)message_hash;
  (void)signature;
  g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_UNSUPPORTED,
              "Ed25519 verification not available (requires libsodium)");
  return FALSE;
#endif
}

static gboolean
ed25519_generate_private_key(GnKeyProvider  *self,
                             guint8         *private_key_out,
                             gsize          *private_key_len_out,
                             GError        **error)
{
  (void)self;

#if ED25519_AVAILABLE
  /* Generate a random 32-byte seed as the private key */
  randombytes_buf(private_key_out, GN_ED25519_PRIVATE_KEY_SIZE);

  if (private_key_len_out) {
    *private_key_len_out = GN_ED25519_PRIVATE_KEY_SIZE;
  }

  return TRUE;

#else
  (void)private_key_out;
  (void)private_key_len_out;
  g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_UNSUPPORTED,
              "Ed25519 key generation not available (requires libsodium)");
  return FALSE;
#endif
}

static gboolean
ed25519_is_valid_private_key(GnKeyProvider *self,
                             const guint8  *private_key,
                             gsize          private_key_len)
{
  (void)self;

  if (private_key_len != GN_ED25519_PRIVATE_KEY_SIZE) {
    return FALSE;
  }

  /* Any 32-byte value is a valid Ed25519 seed */
  /* Check it's not all zeros */
  gboolean all_zero = TRUE;
  for (gsize i = 0; i < private_key_len; i++) {
    if (private_key[i] != 0x00) {
      all_zero = FALSE;
      break;
    }
  }

  return !all_zero;
}

static gboolean
ed25519_is_valid_public_key(GnKeyProvider *self,
                            const guint8  *public_key,
                            gsize          public_key_len)
{
  (void)self;

  if (public_key_len != GN_ED25519_PUBLIC_KEY_SIZE) {
    return FALSE;
  }

#if ED25519_AVAILABLE
  /* Check if the public key is a valid point on the curve
   * libsodium provides a validation function */
  return crypto_core_ed25519_is_valid_point(public_key) == 1;
#else
  /* Basic check: not all zeros */
  gboolean all_zero = TRUE;
  for (gsize i = 0; i < public_key_len; i++) {
    if (public_key[i] != 0x00) {
      all_zero = FALSE;
      break;
    }
  }
  return !all_zero;
#endif
}

/* ============================================================================
 * GObject lifecycle
 * ============================================================================ */

static void
gn_key_provider_ed25519_finalize(GObject *object)
{
  G_OBJECT_CLASS(gn_key_provider_ed25519_parent_class)->finalize(object);
}

static void
gn_key_provider_ed25519_class_init(GnKeyProviderEd25519Class *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = gn_key_provider_ed25519_finalize;
}

static void
gn_key_provider_ed25519_iface_init(GnKeyProviderInterface *iface)
{
  iface->get_key_type = ed25519_get_key_type;
  iface->get_key_type_name = ed25519_get_key_type_name;
  iface->get_private_key_size = ed25519_get_private_key_size;
  iface->get_public_key_size = ed25519_get_public_key_size;
  iface->get_signature_size = ed25519_get_signature_size;
  iface->derive_public_key = ed25519_derive_public_key;
  iface->sign = ed25519_sign;
  iface->verify = ed25519_verify;
  iface->generate_private_key = ed25519_generate_private_key;
  iface->is_valid_private_key = ed25519_is_valid_private_key;
  iface->is_valid_public_key = ed25519_is_valid_public_key;
}

static void
gn_key_provider_ed25519_init(GnKeyProviderEd25519 *self)
{
#if ED25519_AVAILABLE
  /* Initialize libsodium if not already done */
  if (sodium_init() < 0) {
    g_warning("Failed to initialize libsodium");
    self->initialized = FALSE;
  } else {
    self->initialized = TRUE;
  }
#else
  self->initialized = FALSE;
#endif
}

/* ============================================================================
 * Public API
 * ============================================================================ */

GnKeyProviderEd25519 *
gn_key_provider_ed25519_new(void)
{
  return g_object_new(GN_TYPE_KEY_PROVIDER_ED25519, NULL);
}

GnKeyProviderEd25519 *
gn_key_provider_ed25519_get_default(void)
{
  if (!default_instance) {
    default_instance = gn_key_provider_ed25519_new();
  }
  return default_instance;
}

void
gn_key_provider_ed25519_register(void)
{
  GnKeyProviderEd25519 *provider = gn_key_provider_ed25519_get_default();
  gn_key_provider_register(GN_KEY_TYPE_ED25519, GN_KEY_PROVIDER(provider));
}

gboolean
gn_key_provider_ed25519_is_available(void)
{
#if ED25519_AVAILABLE
  GnKeyProviderEd25519 *provider = gn_key_provider_ed25519_get_default();
  return provider->initialized;
#else
  return FALSE;
#endif
}

/* ============================================================================
 * Hex utility functions
 * ============================================================================ */

gboolean
gn_ed25519_derive_pubkey_hex(const gchar  *private_key_hex,
                             gchar       **public_key_hex_out,
                             GError      **error)
{
  g_return_val_if_fail(private_key_hex != NULL, FALSE);
  g_return_val_if_fail(public_key_hex_out != NULL, FALSE);

  guint8 sk[GN_ED25519_PRIVATE_KEY_SIZE];
  guint8 pk[GN_ED25519_PUBLIC_KEY_SIZE];
  gsize pk_len;

  if (!hex_to_bin(private_key_hex, sk, GN_ED25519_PRIVATE_KEY_SIZE)) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INVALID_KEY,
                "Invalid private key hex");
    return FALSE;
  }

  GnKeyProvider *provider = GN_KEY_PROVIDER(gn_key_provider_ed25519_get_default());
  gboolean result = gn_key_provider_derive_public_key(provider, sk, sizeof(sk),
                                                      pk, &pk_len, error);

  gn_secure_clear_buffer(sk);

  if (!result) {
    return FALSE;
  }

  *public_key_hex_out = bin_to_hex(pk, pk_len);
  return TRUE;
}

gboolean
gn_ed25519_sign_hash_hex(const gchar  *private_key_hex,
                         const gchar  *hash_hex,
                         gchar       **signature_hex_out,
                         GError      **error)
{
  g_return_val_if_fail(private_key_hex != NULL, FALSE);
  g_return_val_if_fail(hash_hex != NULL, FALSE);
  g_return_val_if_fail(signature_hex_out != NULL, FALSE);

  guint8 sk[GN_ED25519_PRIVATE_KEY_SIZE];
  guint8 hash[32];
  guint8 sig[GN_ED25519_SIGNATURE_SIZE];
  gsize sig_len;

  if (!hex_to_bin(private_key_hex, sk, GN_ED25519_PRIVATE_KEY_SIZE)) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INVALID_KEY,
                "Invalid private key hex");
    return FALSE;
  }

  if (!hex_to_bin(hash_hex, hash, 32)) {
    gn_secure_clear_buffer(sk);
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INVALID_KEY,
                "Invalid hash hex");
    return FALSE;
  }

  GnKeyProvider *provider = GN_KEY_PROVIDER(gn_key_provider_ed25519_get_default());
  gboolean result = gn_key_provider_sign(provider, sk, sizeof(sk),
                                         hash, sizeof(hash),
                                         sig, &sig_len, error);

  gn_secure_clear_buffer(sk);

  if (!result) {
    return FALSE;
  }

  *signature_hex_out = bin_to_hex(sig, sig_len);
  return TRUE;
}

gboolean
gn_ed25519_verify_hex(const gchar  *public_key_hex,
                      const gchar  *hash_hex,
                      const gchar  *signature_hex,
                      GError      **error)
{
  g_return_val_if_fail(public_key_hex != NULL, FALSE);
  g_return_val_if_fail(hash_hex != NULL, FALSE);
  g_return_val_if_fail(signature_hex != NULL, FALSE);

  guint8 pk[GN_ED25519_PUBLIC_KEY_SIZE];
  guint8 hash[32];
  guint8 sig[GN_ED25519_SIGNATURE_SIZE];

  if (!hex_to_bin(public_key_hex, pk, GN_ED25519_PUBLIC_KEY_SIZE)) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INVALID_KEY,
                "Invalid public key hex");
    return FALSE;
  }

  if (!hex_to_bin(hash_hex, hash, 32)) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INVALID_KEY,
                "Invalid hash hex");
    return FALSE;
  }

  if (!hex_to_bin(signature_hex, sig, GN_ED25519_SIGNATURE_SIZE)) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INVALID_KEY,
                "Invalid signature hex");
    return FALSE;
  }

  GnKeyProvider *provider = GN_KEY_PROVIDER(gn_key_provider_ed25519_get_default());
  return gn_key_provider_verify(provider, pk, sizeof(pk),
                                hash, sizeof(hash),
                                sig, sizeof(sig), error);
}
