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
 * GnKeyProvider interface implementation
 * ============================================================================ */

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
  iface->derive_public_key = ed25519_derive_public_key;
  iface->sign = ed25519_sign;
  iface->verify = ed25519_verify;
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

/* ============================================================================
 * Hex utility functions
 * ============================================================================ */
