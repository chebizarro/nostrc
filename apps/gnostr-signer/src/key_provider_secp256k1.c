/* key_provider_secp256k1.c - secp256k1 key provider implementation
 *
 * Uses libsecp256k1 for cryptographic operations, specifically:
 * - Schnorr signatures (BIP-340) for Nostr event signing
 * - X-only public keys as used in Nostr
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "key_provider_secp256k1.h"
#include "secure-mem.h"
#include "secure-memory.h"
#include <keys.h>
#include <nostr-utils.h>
#include <string.h>

#ifdef HAVE_SECP256K1
#include <secp256k1.h>
#include <secp256k1_schnorrsig.h>
#include <secp256k1_extrakeys.h>
#endif

/* ============================================================================
 * Private structure
 * ============================================================================ */

struct _GnKeyProviderSecp256k1 {
  GObject parent_instance;

#ifdef HAVE_SECP256K1
  secp256k1_context *ctx;
#endif
};

/* Forward declarations for interface implementation */
static void gn_key_provider_secp256k1_iface_init(GnKeyProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GnKeyProviderSecp256k1,
                        gn_key_provider_secp256k1,
                        G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GN_TYPE_KEY_PROVIDER,
                                              gn_key_provider_secp256k1_iface_init))

/* Singleton instance */
static GnKeyProviderSecp256k1 *default_instance = NULL;

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

/* Secure version that uses secure memory */
static gchar *
bin_to_hex_secure(const guint8 *bin, gsize len)
{
  static const gchar hexd[] = "0123456789abcdef";
  gchar *out = (gchar *)gn_secure_alloc(len * 2 + 1);
  if (!out) return NULL;

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
secp256k1_get_key_type(GnKeyProvider *self)
{
  (void)self;
  return GN_KEY_TYPE_SECP256K1;
}

static const gchar *
secp256k1_get_key_type_name(GnKeyProvider *self)
{
  (void)self;
  return "secp256k1";
}

static gsize
secp256k1_get_private_key_size(GnKeyProvider *self)
{
  (void)self;
  return GN_SECP256K1_PRIVATE_KEY_SIZE;
}

static gsize
secp256k1_get_public_key_size(GnKeyProvider *self)
{
  (void)self;
  return GN_SECP256K1_PUBLIC_KEY_SIZE;
}

static gsize
secp256k1_get_signature_size(GnKeyProvider *self)
{
  (void)self;
  return GN_SECP256K1_SIGNATURE_SIZE;
}

static gboolean
secp256k1_derive_public_key(GnKeyProvider  *self,
                            const guint8   *private_key,
                            gsize           private_key_len,
                            guint8         *public_key_out,
                            gsize          *public_key_len_out,
                            GError        **error)
{
  GnKeyProviderSecp256k1 *provider = GN_KEY_PROVIDER_SECP256K1(self);

  if (private_key_len != GN_SECP256K1_PRIVATE_KEY_SIZE) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INVALID_KEY,
                "Invalid private key length: expected %d, got %zu",
                GN_SECP256K1_PRIVATE_KEY_SIZE, private_key_len);
    return FALSE;
  }

#ifdef HAVE_SECP256K1
  secp256k1_keypair keypair;
  secp256k1_xonly_pubkey xonly_pk;

  if (!secp256k1_keypair_create(provider->ctx, &keypair, private_key)) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INVALID_KEY,
                "Failed to create keypair from private key");
    return FALSE;
  }

  if (!secp256k1_keypair_xonly_pub(provider->ctx, &xonly_pk, NULL, &keypair)) {
    gn_secure_clear_buffer(&keypair);
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INTERNAL,
                "Failed to extract x-only public key");
    return FALSE;
  }

  /* Serialize x-only public key (32 bytes) */
  secp256k1_xonly_pubkey_serialize(provider->ctx, public_key_out, &xonly_pk);

  if (public_key_len_out) {
    *public_key_len_out = GN_SECP256K1_PUBLIC_KEY_SIZE;
  }

  gn_secure_clear_buffer(&keypair);
  return TRUE;

#else
  /* Fallback using libnostr */
  gchar *sk_hex = bin_to_hex_secure(private_key, private_key_len);
  if (!sk_hex) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INTERNAL,
                "Failed to allocate secure memory");
    return FALSE;
  }

  gchar *pk_hex = nostr_key_get_public(sk_hex);
  gn_secure_strfree(sk_hex);

  if (!pk_hex) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INVALID_KEY,
                "Failed to derive public key");
    return FALSE;
  }

  if (!hex_to_bin(pk_hex, public_key_out, GN_SECP256K1_PUBLIC_KEY_SIZE)) {
    g_free(pk_hex);
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INTERNAL,
                "Failed to decode public key");
    return FALSE;
  }

  g_free(pk_hex);

  if (public_key_len_out) {
    *public_key_len_out = GN_SECP256K1_PUBLIC_KEY_SIZE;
  }

  return TRUE;
#endif
}

static gboolean
secp256k1_sign(GnKeyProvider  *self,
               const guint8   *private_key,
               gsize           private_key_len,
               const guint8   *message_hash,
               gsize           hash_len,
               guint8         *signature_out,
               gsize          *signature_len_out,
               GError        **error)
{
  GnKeyProviderSecp256k1 *provider = GN_KEY_PROVIDER_SECP256K1(self);

  if (private_key_len != GN_SECP256K1_PRIVATE_KEY_SIZE) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INVALID_KEY,
                "Invalid private key length");
    return FALSE;
  }

  if (hash_len != 32) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INVALID_KEY,
                "Invalid hash length: expected 32, got %zu", hash_len);
    return FALSE;
  }

#ifdef HAVE_SECP256K1
  secp256k1_keypair keypair;

  if (!secp256k1_keypair_create(provider->ctx, &keypair, private_key)) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INVALID_KEY,
                "Failed to create keypair");
    return FALSE;
  }

  /* Use Schnorr signature (BIP-340) */
  if (!secp256k1_schnorrsig_sign32(provider->ctx, signature_out,
                                   message_hash, &keypair, NULL)) {
    gn_secure_clear_buffer(&keypair);
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_SIGNING_FAILED,
                "Schnorr signing failed");
    return FALSE;
  }

  gn_secure_clear_buffer(&keypair);

  if (signature_len_out) {
    *signature_len_out = GN_SECP256K1_SIGNATURE_SIZE;
  }

  return TRUE;

#else
  /* Fallback: not implemented without secp256k1 */
  (void)provider;
  (void)message_hash;
  (void)signature_out;
  (void)signature_len_out;
  g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_UNSUPPORTED,
              "Signing requires libsecp256k1");
  return FALSE;
#endif
}

static gboolean
secp256k1_verify(GnKeyProvider  *self,
                 const guint8   *public_key,
                 gsize           public_key_len,
                 const guint8   *message_hash,
                 gsize           hash_len,
                 const guint8   *signature,
                 gsize           signature_len,
                 GError        **error)
{
  GnKeyProviderSecp256k1 *provider = GN_KEY_PROVIDER_SECP256K1(self);

  if (public_key_len != GN_SECP256K1_PUBLIC_KEY_SIZE) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INVALID_KEY,
                "Invalid public key length");
    return FALSE;
  }

  if (hash_len != 32) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INVALID_KEY,
                "Invalid hash length");
    return FALSE;
  }

  if (signature_len != GN_SECP256K1_SIGNATURE_SIZE) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INVALID_KEY,
                "Invalid signature length");
    return FALSE;
  }

#ifdef HAVE_SECP256K1
  secp256k1_xonly_pubkey xonly_pk;

  if (!secp256k1_xonly_pubkey_parse(provider->ctx, &xonly_pk, public_key)) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INVALID_KEY,
                "Failed to parse public key");
    return FALSE;
  }

  int valid = secp256k1_schnorrsig_verify(provider->ctx, signature,
                                          message_hash, 32, &xonly_pk);

  if (!valid) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_VERIFICATION_FAILED,
                "Signature verification failed");
    return FALSE;
  }

  return TRUE;

#else
  (void)provider;
  (void)signature;
  g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_UNSUPPORTED,
              "Verification requires libsecp256k1");
  return FALSE;
#endif
}

static gboolean
secp256k1_generate_private_key(GnKeyProvider  *self,
                               guint8         *private_key_out,
                               gsize          *private_key_len_out,
                               GError        **error)
{
  GnKeyProviderSecp256k1 *provider = GN_KEY_PROVIDER_SECP256K1(self);

#ifdef HAVE_SECP256K1
  /* Generate random bytes */
  GRand *rand = g_rand_new();
  for (gsize i = 0; i < GN_SECP256K1_PRIVATE_KEY_SIZE; i++) {
    private_key_out[i] = (guint8)g_rand_int_range(rand, 0, 256);
  }
  g_rand_free(rand);

  /* Verify it's a valid secret key */
  if (!secp256k1_ec_seckey_verify(provider->ctx, private_key_out)) {
    /* Very rare - retry with different random bytes */
    gn_secure_clear_buffer(private_key_out);
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INTERNAL,
                "Generated key failed validation");
    return FALSE;
  }

  if (private_key_len_out) {
    *private_key_len_out = GN_SECP256K1_PRIVATE_KEY_SIZE;
  }

  return TRUE;

#else
  /* Fallback using libnostr */
  (void)provider;

  gchar *sk_hex = nostr_key_generate_private();
  if (!sk_hex) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INTERNAL,
                "Key generation failed");
    return FALSE;
  }

  if (!hex_to_bin(sk_hex, private_key_out, GN_SECP256K1_PRIVATE_KEY_SIZE)) {
    /* Securely clear before free */
    gn_secure_clear_string(sk_hex);
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INTERNAL,
                "Failed to decode generated key");
    return FALSE;
  }

  gn_secure_clear_string(sk_hex);

  if (private_key_len_out) {
    *private_key_len_out = GN_SECP256K1_PRIVATE_KEY_SIZE;
  }

  return TRUE;
#endif
}

static gboolean
secp256k1_is_valid_private_key(GnKeyProvider *self,
                               const guint8  *private_key,
                               gsize          private_key_len)
{
  GnKeyProviderSecp256k1 *provider = GN_KEY_PROVIDER_SECP256K1(self);

  if (private_key_len != GN_SECP256K1_PRIVATE_KEY_SIZE) {
    return FALSE;
  }

#ifdef HAVE_SECP256K1
  return secp256k1_ec_seckey_verify(provider->ctx, private_key) == 1;
#else
  (void)provider;
  /* Basic check: not all zeros or all ones */
  gboolean all_zero = TRUE, all_one = TRUE;
  for (gsize i = 0; i < private_key_len; i++) {
    if (private_key[i] != 0x00) all_zero = FALSE;
    if (private_key[i] != 0xFF) all_one = FALSE;
  }
  return !all_zero && !all_one;
#endif
}

static gboolean
secp256k1_is_valid_public_key(GnKeyProvider *self,
                              const guint8  *public_key,
                              gsize          public_key_len)
{
  GnKeyProviderSecp256k1 *provider = GN_KEY_PROVIDER_SECP256K1(self);

  if (public_key_len != GN_SECP256K1_PUBLIC_KEY_SIZE) {
    return FALSE;
  }

#ifdef HAVE_SECP256K1
  secp256k1_xonly_pubkey xonly_pk;
  return secp256k1_xonly_pubkey_parse(provider->ctx, &xonly_pk, public_key) == 1;
#else
  (void)provider;
  /* Basic check: not all zeros */
  gboolean all_zero = TRUE;
  for (gsize i = 0; i < public_key_len; i++) {
    if (public_key[i] != 0x00) all_zero = FALSE;
  }
  return !all_zero;
#endif
}

/* ============================================================================
 * GObject lifecycle
 * ============================================================================ */

static void
gn_key_provider_secp256k1_finalize(GObject *object)
{
  GnKeyProviderSecp256k1 *self = GN_KEY_PROVIDER_SECP256K1(object);

#ifdef HAVE_SECP256K1
  if (self->ctx) {
    secp256k1_context_destroy(self->ctx);
    self->ctx = NULL;
  }
#else
  (void)self;
#endif

  G_OBJECT_CLASS(gn_key_provider_secp256k1_parent_class)->finalize(object);
}

static void
gn_key_provider_secp256k1_class_init(GnKeyProviderSecp256k1Class *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = gn_key_provider_secp256k1_finalize;
}

static void
gn_key_provider_secp256k1_iface_init(GnKeyProviderInterface *iface)
{
  iface->get_key_type = secp256k1_get_key_type;
  iface->get_key_type_name = secp256k1_get_key_type_name;
  iface->get_private_key_size = secp256k1_get_private_key_size;
  iface->get_public_key_size = secp256k1_get_public_key_size;
  iface->get_signature_size = secp256k1_get_signature_size;
  iface->derive_public_key = secp256k1_derive_public_key;
  iface->sign = secp256k1_sign;
  iface->verify = secp256k1_verify;
  iface->generate_private_key = secp256k1_generate_private_key;
  iface->is_valid_private_key = secp256k1_is_valid_private_key;
  iface->is_valid_public_key = secp256k1_is_valid_public_key;
}

static void
gn_key_provider_secp256k1_init(GnKeyProviderSecp256k1 *self)
{
#ifdef HAVE_SECP256K1
  /* Create secp256k1 context with all capabilities */
  self->ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN |
                                       SECP256K1_CONTEXT_VERIFY);
#else
  (void)self;
#endif
}

/* ============================================================================
 * Public API
 * ============================================================================ */

GnKeyProviderSecp256k1 *
gn_key_provider_secp256k1_new(void)
{
  return g_object_new(GN_TYPE_KEY_PROVIDER_SECP256K1, NULL);
}

GnKeyProviderSecp256k1 *
gn_key_provider_secp256k1_get_default(void)
{
  if (!default_instance) {
    default_instance = gn_key_provider_secp256k1_new();
  }
  return default_instance;
}

void
gn_key_provider_secp256k1_register(void)
{
  GnKeyProviderSecp256k1 *provider = gn_key_provider_secp256k1_get_default();
  gn_key_provider_register(GN_KEY_TYPE_SECP256K1, GN_KEY_PROVIDER(provider));
}

/* ============================================================================
 * Hex utility functions
 * ============================================================================ */

gboolean
gn_secp256k1_derive_pubkey_hex(const gchar  *private_key_hex,
                               gchar       **public_key_hex_out,
                               GError      **error)
{
  g_return_val_if_fail(private_key_hex != NULL, FALSE);
  g_return_val_if_fail(public_key_hex_out != NULL, FALSE);

  guint8 sk[GN_SECP256K1_PRIVATE_KEY_SIZE];
  guint8 pk[GN_SECP256K1_PUBLIC_KEY_SIZE];
  gsize pk_len;

  if (!hex_to_bin(private_key_hex, sk, GN_SECP256K1_PRIVATE_KEY_SIZE)) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INVALID_KEY,
                "Invalid private key hex");
    return FALSE;
  }

  GnKeyProvider *provider = GN_KEY_PROVIDER(gn_key_provider_secp256k1_get_default());
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
gn_secp256k1_sign_hash_hex(const gchar  *private_key_hex,
                           const gchar  *hash_hex,
                           gchar       **signature_hex_out,
                           GError      **error)
{
  g_return_val_if_fail(private_key_hex != NULL, FALSE);
  g_return_val_if_fail(hash_hex != NULL, FALSE);
  g_return_val_if_fail(signature_hex_out != NULL, FALSE);

  guint8 sk[GN_SECP256K1_PRIVATE_KEY_SIZE];
  guint8 hash[32];
  guint8 sig[GN_SECP256K1_SIGNATURE_SIZE];
  gsize sig_len;

  if (!hex_to_bin(private_key_hex, sk, GN_SECP256K1_PRIVATE_KEY_SIZE)) {
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

  GnKeyProvider *provider = GN_KEY_PROVIDER(gn_key_provider_secp256k1_get_default());
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
gn_secp256k1_verify_hex(const gchar  *public_key_hex,
                        const gchar  *hash_hex,
                        const gchar  *signature_hex,
                        GError      **error)
{
  g_return_val_if_fail(public_key_hex != NULL, FALSE);
  g_return_val_if_fail(hash_hex != NULL, FALSE);
  g_return_val_if_fail(signature_hex != NULL, FALSE);

  guint8 pk[GN_SECP256K1_PUBLIC_KEY_SIZE];
  guint8 hash[32];
  guint8 sig[GN_SECP256K1_SIGNATURE_SIZE];

  if (!hex_to_bin(public_key_hex, pk, GN_SECP256K1_PUBLIC_KEY_SIZE)) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INVALID_KEY,
                "Invalid public key hex");
    return FALSE;
  }

  if (!hex_to_bin(hash_hex, hash, 32)) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INVALID_KEY,
                "Invalid hash hex");
    return FALSE;
  }

  if (!hex_to_bin(signature_hex, sig, GN_SECP256K1_SIGNATURE_SIZE)) {
    g_set_error(error, GN_KEY_PROVIDER_ERROR, GN_KEY_PROVIDER_ERROR_INVALID_KEY,
                "Invalid signature hex");
    return FALSE;
  }

  GnKeyProvider *provider = GN_KEY_PROVIDER(gn_key_provider_secp256k1_get_default());
  return gn_key_provider_verify(provider, pk, sizeof(pk),
                                hash, sizeof(hash),
                                sig, sizeof(sig), error);
}
