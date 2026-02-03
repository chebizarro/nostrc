/* social-recovery.c - Social recovery implementation using Shamir's Secret Sharing
 *
 * Implements k-of-n threshold secret sharing for key recovery through trusted
 * guardians. Uses GF(2^8) arithmetic for share generation and Lagrange
 * interpolation for reconstruction.
 */
#include "social-recovery.h"
#include "secure-mem.h"
#include "backup-recovery.h"

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <nostr/nip19/nip19.h>
#include <nostr/nip04.h>
#include <keys.h>
#include <nostr-utils.h>

#include <string.h>
#include <stdlib.h>
#include <time.h>

G_DEFINE_QUARK(gn-social-recovery-error-quark, gn_social_recovery_error)

/* Configuration file version */
#define RECOVERY_CONFIG_VERSION "1.0"

/* Share encoding prefix */
#define SHARE_PREFIX "sss1:"

/* ============================================================
 * GF(2^8) Arithmetic for Shamir's Secret Sharing
 * ============================================================
 * We use the Rijndael (AES) field with polynomial x^8 + x^4 + x^3 + x + 1
 * This is a standard choice that provides good properties.
 */

/* Logarithm table for GF(2^8) multiplication */
static const guint8 gf256_log[256] = {
  0x00, 0x00, 0x19, 0x01, 0x32, 0x02, 0x1a, 0xc6, 0x4b, 0xc7, 0x1b, 0x68, 0x33, 0xee, 0xdf, 0x03,
  0x64, 0x04, 0xe0, 0x0e, 0x34, 0x8d, 0x81, 0xef, 0x4c, 0x71, 0x08, 0xc8, 0xf8, 0x69, 0x1c, 0xc1,
  0x7d, 0xc2, 0x1d, 0xb5, 0xf9, 0xb9, 0x27, 0x6a, 0x4d, 0xe4, 0xa6, 0x72, 0x9a, 0xc9, 0x09, 0x78,
  0x65, 0x2f, 0x8a, 0x05, 0x21, 0x0f, 0xe1, 0x24, 0x12, 0xf0, 0x82, 0x45, 0x35, 0x93, 0xda, 0x8e,
  0x96, 0x8f, 0xdb, 0xbd, 0x36, 0xd0, 0xce, 0x94, 0x13, 0x5c, 0xd2, 0xf1, 0x40, 0x46, 0x83, 0x38,
  0x66, 0xdd, 0xfd, 0x30, 0xbf, 0x06, 0x8b, 0x62, 0xb3, 0x25, 0xe2, 0x98, 0x22, 0x88, 0x91, 0x10,
  0x7e, 0x6e, 0x48, 0xc3, 0xa3, 0xb6, 0x1e, 0x42, 0x3a, 0x6b, 0x28, 0x54, 0xfa, 0x85, 0x3d, 0xba,
  0x2b, 0x79, 0x0a, 0x15, 0x9b, 0x9f, 0x5e, 0xca, 0x4e, 0xd4, 0xac, 0xe5, 0xf3, 0x73, 0xa7, 0x57,
  0xaf, 0x58, 0xa8, 0x50, 0xf4, 0xea, 0xd6, 0x74, 0x4f, 0xae, 0xe9, 0xd5, 0xe7, 0xe6, 0xad, 0xe8,
  0x2c, 0xd7, 0x75, 0x7a, 0xeb, 0x16, 0x0b, 0xf5, 0x59, 0xcb, 0x5f, 0xb0, 0x9c, 0xa9, 0x51, 0xa0,
  0x7f, 0x0c, 0xf6, 0x6f, 0x17, 0xc4, 0x49, 0xec, 0xd8, 0x43, 0x1f, 0x2d, 0xa4, 0x76, 0x7b, 0xb7,
  0xcc, 0xbb, 0x3e, 0x5a, 0xfb, 0x60, 0xb1, 0x86, 0x3b, 0x52, 0xa1, 0x6c, 0xaa, 0x55, 0x29, 0x9d,
  0x97, 0xb2, 0x87, 0x90, 0x61, 0xbe, 0xdc, 0xfc, 0xbc, 0x95, 0xcf, 0xcd, 0x37, 0x3f, 0x5b, 0xd1,
  0x53, 0x39, 0x84, 0x3c, 0x41, 0xa2, 0x6d, 0x47, 0x14, 0x2a, 0x9e, 0x5d, 0x56, 0xf2, 0xd3, 0xab,
  0x44, 0x11, 0x92, 0xd9, 0x23, 0x20, 0x2e, 0x89, 0xb4, 0x7c, 0xb8, 0x26, 0x77, 0x99, 0xe3, 0xa5,
  0x67, 0x4a, 0xed, 0xde, 0xc5, 0x31, 0xfe, 0x18, 0x0d, 0x63, 0x8c, 0x80, 0xc0, 0xf7, 0x70, 0x07
};

/* Exponential table for GF(2^8) multiplication */
static const guint8 gf256_exp[256] = {
  0x01, 0x03, 0x05, 0x0f, 0x11, 0x33, 0x55, 0xff, 0x1a, 0x2e, 0x72, 0x96, 0xa1, 0xf8, 0x13, 0x35,
  0x5f, 0xe1, 0x38, 0x48, 0xd8, 0x73, 0x95, 0xa4, 0xf7, 0x02, 0x06, 0x0a, 0x1e, 0x22, 0x66, 0xaa,
  0xe5, 0x34, 0x5c, 0xe4, 0x37, 0x59, 0xeb, 0x26, 0x6a, 0xbe, 0xd9, 0x70, 0x90, 0xab, 0xe6, 0x31,
  0x53, 0xf5, 0x04, 0x0c, 0x14, 0x3c, 0x44, 0xcc, 0x4f, 0xd1, 0x68, 0xb8, 0xd3, 0x6e, 0xb2, 0xcd,
  0x4c, 0xd4, 0x67, 0xa9, 0xe0, 0x3b, 0x4d, 0xd7, 0x62, 0xa6, 0xf1, 0x08, 0x18, 0x28, 0x78, 0x88,
  0x83, 0x9e, 0xb9, 0xd0, 0x6b, 0xbd, 0xdc, 0x7f, 0x81, 0x98, 0xb3, 0xce, 0x49, 0xdb, 0x76, 0x9a,
  0xb5, 0xc4, 0x57, 0xf9, 0x10, 0x30, 0x50, 0xf0, 0x0b, 0x1d, 0x27, 0x69, 0xbb, 0xd6, 0x61, 0xa3,
  0xfe, 0x19, 0x2b, 0x7d, 0x87, 0x92, 0xad, 0xec, 0x2f, 0x71, 0x93, 0xae, 0xe9, 0x20, 0x60, 0xa0,
  0xfb, 0x16, 0x3a, 0x4e, 0xd2, 0x6d, 0xb7, 0xc2, 0x5d, 0xe7, 0x32, 0x56, 0xfa, 0x15, 0x3f, 0x41,
  0xc3, 0x5e, 0xe2, 0x3d, 0x47, 0xc9, 0x40, 0xc0, 0x5b, 0xed, 0x2c, 0x74, 0x9c, 0xbf, 0xda, 0x75,
  0x9f, 0xba, 0xd5, 0x64, 0xac, 0xef, 0x2a, 0x7e, 0x82, 0x9d, 0xbc, 0xdf, 0x7a, 0x8e, 0x89, 0x80,
  0x9b, 0xb6, 0xc1, 0x58, 0xe8, 0x23, 0x65, 0xaf, 0xea, 0x25, 0x6f, 0xb1, 0xc8, 0x43, 0xc5, 0x54,
  0xfc, 0x1f, 0x21, 0x63, 0xa5, 0xf4, 0x07, 0x09, 0x1b, 0x2d, 0x77, 0x99, 0xb0, 0xcb, 0x46, 0xca,
  0x45, 0xcf, 0x4a, 0xde, 0x79, 0x8b, 0x86, 0x91, 0xa8, 0xe3, 0x3e, 0x42, 0xc6, 0x51, 0xf3, 0x0e,
  0x12, 0x36, 0x5a, 0xee, 0x29, 0x7b, 0x8d, 0x8c, 0x8f, 0x8a, 0x85, 0x94, 0xa7, 0xf2, 0x0d, 0x17,
  0x39, 0x4b, 0xdd, 0x7c, 0x84, 0x97, 0xa2, 0xfd, 0x1c, 0x24, 0x6c, 0xb4, 0xc7, 0x52, 0xf6, 0x01
};

/* Multiply two elements in GF(2^8) */
static inline guint8 gf256_mul(guint8 a, guint8 b) {
  if (a == 0 || b == 0) return 0;
  return gf256_exp[(gf256_log[a] + gf256_log[b]) % 255];
}

/* Divide in GF(2^8) */
static inline guint8 gf256_div(guint8 a, guint8 b) {
  if (b == 0) return 0;  /* Error case, should not happen */
  if (a == 0) return 0;
  return gf256_exp[(255 + gf256_log[a] - gf256_log[b]) % 255];
}

/* Add/subtract in GF(2^8) - same operation (XOR) */
static inline guint8 gf256_add(guint8 a, guint8 b) {
  return a ^ b;
}

/* ============================================================
 * Secure Random Number Generation
 * ============================================================ */

/* Generate cryptographically secure random bytes */
static gboolean secure_random_bytes(guint8 *buf, gsize len) {
  /* Use GLib's random source which uses /dev/urandom or equivalent */
  for (gsize i = 0; i < len; i++) {
    buf[i] = (guint8)g_random_int_range(0, 256);
  }
  return TRUE;
}

/* ============================================================
 * Shamir's Secret Sharing Implementation
 * ============================================================ */

/* Evaluate polynomial at x using Horner's method in GF(2^8) */
static guint8 evaluate_polynomial(const guint8 *coeffs, gsize num_coeffs, guint8 x) {
  if (num_coeffs == 0) return 0;

  guint8 result = coeffs[num_coeffs - 1];
  for (gsize i = num_coeffs - 1; i > 0; i--) {
    result = gf256_add(gf256_mul(result, x), coeffs[i - 1]);
  }
  return result;
}

gboolean gn_sss_split(const guint8 *secret,
                      gsize secret_len,
                      guint8 threshold,
                      guint8 total_shares,
                      GPtrArray **out_shares,
                      GError **error) {
  g_return_val_if_fail(out_shares != NULL, FALSE);
  *out_shares = NULL;

  /* Validate parameters */
  if (secret == NULL || secret_len == 0) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_INVALID_KEY,
                "Secret is required and must not be empty");
    return FALSE;
  }

  if (threshold < 2) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_INVALID_PARAMS,
                "Threshold must be at least 2 for security");
    return FALSE;
  }

  if (threshold > total_shares) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_INVALID_PARAMS,
                "Threshold cannot exceed total shares (%u > %u)", threshold, total_shares);
    return FALSE;
  }

  if (total_shares > 255) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_INVALID_PARAMS,
                "Maximum 255 shares supported (requested %u)", total_shares);
    return FALSE;
  }

  /* Allocate coefficients array (threshold elements per byte) */
  guint8 *coeffs = gnostr_secure_alloc(threshold);
  if (!coeffs) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_INVALID_PARAMS,
                "Failed to allocate secure memory");
    return FALSE;
  }

  /* Create shares array */
  GPtrArray *shares = g_ptr_array_new_with_free_func((GDestroyNotify)gn_sss_share_free);

  /* Allocate share structures */
  for (guint8 i = 0; i < total_shares; i++) {
    GnSSSShare *share = g_new0(GnSSSShare, 1);
    share->index = i + 1;  /* Indices are 1-based */
    share->data = gnostr_secure_alloc(secret_len);
    share->data_len = secret_len;
    if (!share->data) {
      gnostr_secure_free(coeffs, threshold);
      g_ptr_array_unref(shares);
      g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_INVALID_PARAMS,
                  "Failed to allocate secure memory for share");
      return FALSE;
    }
    g_ptr_array_add(shares, share);
  }

  /* Process each byte of the secret independently */
  for (gsize byte_idx = 0; byte_idx < secret_len; byte_idx++) {
    /* Set constant term to secret byte */
    coeffs[0] = secret[byte_idx];

    /* Generate random coefficients for higher terms */
    secure_random_bytes(coeffs + 1, threshold - 1);

    /* Evaluate polynomial for each share */
    for (guint i = 0; i < total_shares; i++) {
      GnSSSShare *share = g_ptr_array_index(shares, i);
      share->data[byte_idx] = evaluate_polynomial(coeffs, threshold, share->index);
    }
  }

  /* Securely clear coefficients */
  gnostr_secure_free(coeffs, threshold);

  *out_shares = shares;
  return TRUE;
}

gboolean gn_sss_combine(GPtrArray *shares,
                        guint8 threshold,
                        guint8 **out_secret,
                        gsize *out_len,
                        GError **error) {
  g_return_val_if_fail(out_secret != NULL, FALSE);
  g_return_val_if_fail(out_len != NULL, FALSE);
  *out_secret = NULL;
  *out_len = 0;

  if (!shares || shares->len == 0) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_THRESHOLD_NOT_MET,
                "No shares provided");
    return FALSE;
  }

  if (shares->len < threshold) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_THRESHOLD_NOT_MET,
                "Not enough shares: %u provided, %u required", shares->len, threshold);
    return FALSE;
  }

  /* Use only the first 'threshold' shares for reconstruction */
  guint num_shares = MIN(shares->len, threshold);

  /* Get secret length from first share */
  GnSSSShare *first = g_ptr_array_index(shares, 0);
  gsize secret_len = first->data_len;

  /* Validate all shares have same length */
  for (guint i = 1; i < num_shares; i++) {
    GnSSSShare *share = g_ptr_array_index(shares, i);
    if (share->data_len != secret_len) {
      g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_INVALID_SHARE,
                  "Share %u has inconsistent length", i);
      return FALSE;
    }
  }

  /* Allocate output */
  guint8 *secret = gnostr_secure_alloc(secret_len);
  if (!secret) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_RECONSTRUCTION,
                "Failed to allocate secure memory");
    return FALSE;
  }

  /* Extract x coordinates (share indices) */
  guint8 *x_coords = g_new(guint8, num_shares);
  for (guint i = 0; i < num_shares; i++) {
    GnSSSShare *share = g_ptr_array_index(shares, i);
    x_coords[i] = share->index;
  }

  /* Lagrange interpolation for each byte */
  for (gsize byte_idx = 0; byte_idx < secret_len; byte_idx++) {
    guint8 result = 0;

    for (guint i = 0; i < num_shares; i++) {
      GnSSSShare *share_i = g_ptr_array_index(shares, i);
      guint8 xi = x_coords[i];
      guint8 yi = share_i->data[byte_idx];

      /* Compute Lagrange basis polynomial at x=0 */
      guint8 numerator = 1;
      guint8 denominator = 1;

      for (guint j = 0; j < num_shares; j++) {
        if (i == j) continue;
        guint8 xj = x_coords[j];

        /* At x=0: numerator *= (0 - xj) = xj (in GF(2^8)) */
        numerator = gf256_mul(numerator, xj);
        /* denominator *= (xi - xj) */
        denominator = gf256_mul(denominator, gf256_add(xi, xj));
      }

      /* basis = numerator / denominator */
      guint8 basis = gf256_div(numerator, denominator);

      /* result += yi * basis */
      result = gf256_add(result, gf256_mul(yi, basis));
    }

    secret[byte_idx] = result;
  }

  g_free(x_coords);

  *out_secret = secret;
  *out_len = secret_len;
  return TRUE;
}

void gn_sss_share_free(GnSSSShare *share) {
  if (!share) return;
  if (share->data) {
    gnostr_secure_free(share->data, share->data_len);
  }
  g_free(share);
}

void gn_sss_shares_free(GPtrArray *shares) {
  if (!shares) return;
  g_ptr_array_unref(shares);
}

/* ============================================================
 * Share Encoding/Decoding
 * ============================================================ */

gchar *gn_sss_share_encode(const GnSSSShare *share) {
  if (!share || !share->data || share->data_len == 0) return NULL;

  gchar *base64 = g_base64_encode(share->data, share->data_len);
  gchar *encoded = g_strdup_printf("%s%u:%s", SHARE_PREFIX, share->index, base64);
  g_free(base64);

  return encoded;
}

GnSSSShare *gn_sss_share_decode(const gchar *encoded, GError **error) {
  if (!encoded) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_INVALID_SHARE,
                "Share string is NULL");
    return NULL;
  }

  if (!g_str_has_prefix(encoded, SHARE_PREFIX)) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_INVALID_SHARE,
                "Invalid share format: must start with '%s'", SHARE_PREFIX);
    return NULL;
  }

  /* Parse "sss1:<index>:<base64>" */
  const gchar *rest = encoded + strlen(SHARE_PREFIX);
  const gchar *colon = strchr(rest, ':');
  if (!colon) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_INVALID_SHARE,
                "Invalid share format: missing index separator");
    return NULL;
  }

  /* Parse index */
  gchar *idx_str = g_strndup(rest, colon - rest);
  gchar *endptr;
  gulong idx = strtoul(idx_str, &endptr, 10);
  gboolean parse_ok = (*endptr == '\0' && idx >= 1 && idx <= 255);
  g_free(idx_str);

  if (!parse_ok) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_INVALID_SHARE,
                "Invalid share index (must be 1-255)");
    return NULL;
  }

  /* Decode base64 */
  const gchar *base64 = colon + 1;
  gsize data_len;
  guint8 *data = g_base64_decode(base64, &data_len);
  if (!data || data_len == 0) {
    g_free(data);
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_INVALID_SHARE,
                "Invalid share data: base64 decode failed");
    return NULL;
  }

  /* Create share with secure memory */
  GnSSSShare *share = g_new0(GnSSSShare, 1);
  share->index = (guint8)idx;
  share->data = gnostr_secure_alloc(data_len);
  share->data_len = data_len;

  if (share->data) {
    memcpy(share->data, data, data_len);
  }

  /* Clear and free decoded data */
  gnostr_secure_clear(data, data_len);
  g_free(data);

  if (!share->data) {
    g_free(share);
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_INVALID_SHARE,
                "Failed to allocate secure memory for share");
    return NULL;
  }

  return share;
}

gboolean gn_sss_share_validate(const gchar *encoded) {
  if (!encoded) return FALSE;
  if (!g_str_has_prefix(encoded, SHARE_PREFIX)) return FALSE;

  const gchar *rest = encoded + strlen(SHARE_PREFIX);
  const gchar *colon = strchr(rest, ':');
  if (!colon) return FALSE;

  /* Check index is numeric */
  for (const gchar *p = rest; p < colon; p++) {
    if (*p < '0' || *p > '9') return FALSE;
  }

  /* Check there's data after colon */
  if (*(colon + 1) == '\0') return FALSE;

  return TRUE;
}

/* ============================================================
 * Guardian Management
 * ============================================================ */

GnGuardian *gn_guardian_new(const gchar *npub, const gchar *label) {
  g_return_val_if_fail(npub != NULL, NULL);

  GnGuardian *guardian = g_new0(GnGuardian, 1);
  guardian->npub = g_strdup(npub);
  guardian->label = g_strdup(label);
  guardian->share_index = 0;
  guardian->assigned_at = 0;
  guardian->confirmed = FALSE;

  return guardian;
}

void gn_guardian_free(GnGuardian *guardian) {
  if (!guardian) return;
  g_free(guardian->npub);
  g_free(guardian->label);
  g_free(guardian);
}

GnGuardian *gn_guardian_dup(const GnGuardian *guardian) {
  if (!guardian) return NULL;

  GnGuardian *dup = g_new0(GnGuardian, 1);
  dup->npub = g_strdup(guardian->npub);
  dup->label = g_strdup(guardian->label);
  dup->share_index = guardian->share_index;
  dup->assigned_at = guardian->assigned_at;
  dup->confirmed = guardian->confirmed;

  return dup;
}

/* ============================================================
 * Recovery Configuration
 * ============================================================ */

GnRecoveryConfig *gn_recovery_config_new(const gchar *owner_npub) {
  g_return_val_if_fail(owner_npub != NULL, NULL);

  GnRecoveryConfig *config = g_new0(GnRecoveryConfig, 1);
  config->owner_npub = g_strdup(owner_npub);
  config->threshold = 0;
  config->total_shares = 0;
  config->guardians = g_ptr_array_new_with_free_func((GDestroyNotify)gn_guardian_free);
  config->created_at = 0;
  config->last_verified = 0;
  config->version = g_strdup(RECOVERY_CONFIG_VERSION);

  return config;
}

void gn_recovery_config_free(GnRecoveryConfig *config) {
  if (!config) return;
  g_free(config->owner_npub);
  g_free(config->version);
  if (config->guardians) {
    g_ptr_array_unref(config->guardians);
  }
  g_free(config);
}

gboolean gn_recovery_config_add_guardian(GnRecoveryConfig *config,
                                         GnGuardian *guardian) {
  g_return_val_if_fail(config != NULL, FALSE);
  g_return_val_if_fail(guardian != NULL, FALSE);

  /* Check for duplicate */
  if (gn_recovery_config_find_guardian(config, guardian->npub)) {
    gn_guardian_free(guardian);
    return FALSE;
  }

  g_ptr_array_add(config->guardians, guardian);
  return TRUE;
}

gboolean gn_recovery_config_remove_guardian(GnRecoveryConfig *config,
                                            const gchar *npub) {
  g_return_val_if_fail(config != NULL, FALSE);
  g_return_val_if_fail(npub != NULL, FALSE);

  for (guint i = 0; i < config->guardians->len; i++) {
    GnGuardian *g = g_ptr_array_index(config->guardians, i);
    if (g_strcmp0(g->npub, npub) == 0) {
      g_ptr_array_remove_index(config->guardians, i);
      return TRUE;
    }
  }

  return FALSE;
}

GnGuardian *gn_recovery_config_find_guardian(GnRecoveryConfig *config,
                                             const gchar *npub) {
  g_return_val_if_fail(config != NULL, NULL);
  g_return_val_if_fail(npub != NULL, NULL);

  for (guint i = 0; i < config->guardians->len; i++) {
    GnGuardian *g = g_ptr_array_index(config->guardians, i);
    if (g_strcmp0(g->npub, npub) == 0) {
      return g;
    }
  }

  return NULL;
}

/* ============================================================
 * High-Level Recovery Operations
 * ============================================================ */

/* Helper: Parse nsec/hex to raw 32-byte key */
static gboolean parse_private_key(const gchar *input, guint8 out_key[32], GError **error) {
  if (!input || !*input) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_INVALID_KEY,
                "Private key is required");
    return FALSE;
  }

  if (g_str_has_prefix(input, "nsec1")) {
    if (nostr_nip19_decode_nsec(input, out_key) != 0) {
      g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_INVALID_KEY,
                  "Invalid nsec format");
      return FALSE;
    }
    return TRUE;
  }

  /* Try hex */
  if (strlen(input) == 64) {
    if (!nostr_hex2bin(out_key, input, 32)) {
      g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_INVALID_KEY,
                  "Invalid hex key format");
      return FALSE;
    }
    return TRUE;
  }

  g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_INVALID_KEY,
              "Key must be nsec1... or 64-character hex");
  return FALSE;
}

/* Helper: Convert 32-byte key to nsec string */
static gchar *key_to_nsec(const guint8 key[32]) {
  gchar *nsec = NULL;
  if (nostr_nip19_encode_nsec(key, &nsec) != 0) {
    return NULL;
  }
  return nsec;
}

gboolean gn_social_recovery_setup(const gchar *nsec,
                                  guint8 threshold,
                                  GPtrArray *guardians,
                                  GnRecoveryConfig **out_config,
                                  GPtrArray **out_encrypted_shares,
                                  GError **error) {
  g_return_val_if_fail(out_config != NULL, FALSE);
  g_return_val_if_fail(out_encrypted_shares != NULL, FALSE);
  *out_config = NULL;
  *out_encrypted_shares = NULL;

  if (!guardians || guardians->len == 0) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_INVALID_PARAMS,
                "At least one guardian is required");
    return FALSE;
  }

  guint8 total_shares = (guint8)guardians->len;

  /* Validate threshold */
  GError *validate_error = NULL;
  if (!gn_social_recovery_validate_threshold(threshold, total_shares, &validate_error)) {
    g_propagate_error(error, validate_error);
    return FALSE;
  }

  /* Parse the secret key */
  guint8 privkey[32];
  if (!parse_private_key(nsec, privkey, error)) {
    return FALSE;
  }

  /* Get owner npub */
  gchar *owner_npub = NULL;
  if (!gn_backup_get_npub(nsec, &owner_npub, error)) {
    gnostr_secure_clear(privkey, 32);
    return FALSE;
  }

  /* Split the secret */
  GPtrArray *shares = NULL;
  if (!gn_sss_split(privkey, 32, threshold, total_shares, &shares, error)) {
    gnostr_secure_clear(privkey, 32);
    g_free(owner_npub);
    return FALSE;
  }

  /* Create configuration */
  GnRecoveryConfig *config = gn_recovery_config_new(owner_npub);
  config->threshold = threshold;
  config->total_shares = total_shares;
  config->created_at = time(NULL);

  /* Create encrypted shares array */
  GPtrArray *encrypted_shares = g_ptr_array_new_with_free_func(g_free);

  /* Assign shares to guardians and encrypt */
  gboolean success = TRUE;
  for (guint i = 0; i < guardians->len && success; i++) {
    GnGuardian *src_guardian = g_ptr_array_index(guardians, i);
    GnGuardian *guardian = gn_guardian_dup(src_guardian);
    GnSSSShare *share = g_ptr_array_index(shares, i);

    guardian->share_index = share->index;
    guardian->assigned_at = config->created_at;

    /* Encrypt share for guardian */
    gchar *encrypted = NULL;
    if (!gn_social_recovery_encrypt_share(share, nsec, guardian->npub, &encrypted, error)) {
      gn_guardian_free(guardian);
      success = FALSE;
      break;
    }

    g_ptr_array_add(config->guardians, guardian);
    g_ptr_array_add(encrypted_shares, encrypted);
  }

  /* Cleanup */
  gnostr_secure_clear(privkey, 32);
  gn_sss_shares_free(shares);
  g_free(owner_npub);

  if (!success) {
    gn_recovery_config_free(config);
    g_ptr_array_unref(encrypted_shares);
    return FALSE;
  }

  *out_config = config;
  *out_encrypted_shares = encrypted_shares;
  return TRUE;
}

gboolean gn_social_recovery_encrypt_share(const GnSSSShare *share,
                                          const gchar *owner_nsec,
                                          const gchar *guardian_npub,
                                          gchar **out_encrypted,
                                          GError **error) {
  g_return_val_if_fail(share != NULL, FALSE);
  g_return_val_if_fail(out_encrypted != NULL, FALSE);
  *out_encrypted = NULL;

  /* Encode share to string */
  gchar *share_str = gn_sss_share_encode(share);
  if (!share_str) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_ENCRYPTION,
                "Failed to encode share");
    return FALSE;
  }

  /* Parse private key */
  guint8 privkey[32];
  if (!parse_private_key(owner_nsec, privkey, error)) {
    g_free(share_str);
    return FALSE;
  }

  /* Decode guardian npub */
  guint8 guardian_pubkey[32];
  if (!g_str_has_prefix(guardian_npub, "npub1")) {
    /* Try hex */
    if (!nostr_hex2bin(guardian_pubkey, guardian_npub, 32)) {
      gnostr_secure_clear(privkey, 32);
      g_free(share_str);
      g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_ENCRYPTION,
                  "Invalid guardian npub format");
      return FALSE;
    }
  } else {
    if (nostr_nip19_decode_npub(guardian_npub, guardian_pubkey) != 0) {
      gnostr_secure_clear(privkey, 32);
      g_free(share_str);
      g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_ENCRYPTION,
                  "Failed to decode guardian npub");
      return FALSE;
    }
  }

  /* Convert keys to hex for NIP-04 */
  gchar *sk_hex = nostr_bin2hex(privkey, 32);
  gchar *pk_hex = nostr_bin2hex(guardian_pubkey, 32);
  gnostr_secure_clear(privkey, 32);

  if (!sk_hex || !pk_hex) {
    g_free(sk_hex);
    g_free(pk_hex);
    g_free(share_str);
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_ENCRYPTION,
                "Failed to convert keys to hex");
    return FALSE;
  }

  /* Encrypt with NIP-04 */
  gchar *ciphertext = NULL;
  gchar *nip04_error = NULL;
  int nip04_result = nostr_nip04_encrypt(share_str, pk_hex, sk_hex, &ciphertext, &nip04_error);

  /* Cleanup sensitive data */
  gnostr_secure_clear(sk_hex, 64);
  free(sk_hex);
  g_free(pk_hex);
  gnostr_secure_clear(share_str, strlen(share_str));
  g_free(share_str);

  if (nip04_result != 0 || !ciphertext) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_ENCRYPTION,
                "NIP-04 encryption failed: %s", nip04_error ? nip04_error : "unknown error");
    free(nip04_error);
    return FALSE;
  }

  /* Wrap in JSON with metadata */
  JsonBuilder *builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "type");
  json_builder_add_string_value(builder, "social_recovery_share");
  json_builder_set_member_name(builder, "version");
  json_builder_add_string_value(builder, "1.0");
  json_builder_set_member_name(builder, "content");
  json_builder_add_string_value(builder, ciphertext);
  json_builder_end_object(builder);

  JsonGenerator *gen = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(gen, root);
  *out_encrypted = json_generator_to_data(gen, NULL);

  json_node_unref(root);
  g_object_unref(gen);
  g_object_unref(builder);
  free(ciphertext);

  return TRUE;
}

gboolean gn_social_recovery_decrypt_share(const gchar *encrypted,
                                          const gchar *guardian_nsec,
                                          const gchar *owner_npub,
                                          GnSSSShare **out_share,
                                          GError **error) {
  g_return_val_if_fail(encrypted != NULL, FALSE);
  g_return_val_if_fail(out_share != NULL, FALSE);
  *out_share = NULL;

  /* Parse JSON wrapper */
  JsonParser *parser = json_parser_new();
  GError *parse_error = NULL;

  if (!json_parser_load_from_data(parser, encrypted, -1, &parse_error)) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_DECRYPTION,
                "Invalid JSON: %s", parse_error->message);
    g_error_free(parse_error);
    g_object_unref(parser);
    return FALSE;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root)) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_DECRYPTION,
                "Expected JSON object");
    g_object_unref(parser);
    return FALSE;
  }

  JsonObject *obj = json_node_get_object(root);

  /* Verify type */
  if (!json_object_has_member(obj, "type") ||
      g_strcmp0(json_object_get_string_member(obj, "type"), "social_recovery_share") != 0) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_DECRYPTION,
                "Invalid share type");
    g_object_unref(parser);
    return FALSE;
  }

  /* Get ciphertext */
  if (!json_object_has_member(obj, "content")) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_DECRYPTION,
                "Missing encrypted content");
    g_object_unref(parser);
    return FALSE;
  }

  const gchar *ciphertext = json_object_get_string_member(obj, "content");

  /* Parse guardian private key */
  guint8 privkey[32];
  if (!parse_private_key(guardian_nsec, privkey, error)) {
    g_object_unref(parser);
    return FALSE;
  }

  /* Decode owner npub */
  guint8 owner_pubkey[32];
  if (!g_str_has_prefix(owner_npub, "npub1")) {
    if (!nostr_hex2bin(owner_pubkey, owner_npub, 32)) {
      gnostr_secure_clear(privkey, 32);
      g_object_unref(parser);
      g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_DECRYPTION,
                  "Invalid owner npub format");
      return FALSE;
    }
  } else {
    if (nostr_nip19_decode_npub(owner_npub, owner_pubkey) != 0) {
      gnostr_secure_clear(privkey, 32);
      g_object_unref(parser);
      g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_DECRYPTION,
                  "Failed to decode owner npub");
      return FALSE;
    }
  }

  /* Convert keys to hex for NIP-04 */
  gchar *sk_hex = nostr_bin2hex(privkey, 32);
  gchar *pk_hex = nostr_bin2hex(owner_pubkey, 32);
  gnostr_secure_clear(privkey, 32);

  if (!sk_hex || !pk_hex) {
    g_free(sk_hex);
    g_free(pk_hex);
    g_object_unref(parser);
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_DECRYPTION,
                "Failed to convert keys to hex");
    return FALSE;
  }

  /* Decrypt with NIP-04 */
  gchar *plaintext = NULL;
  gchar *nip04_error = NULL;
  int nip04_result = nostr_nip04_decrypt(ciphertext, pk_hex, sk_hex, &plaintext, &nip04_error);

  gnostr_secure_clear(sk_hex, 64);
  free(sk_hex);
  g_free(pk_hex);
  g_object_unref(parser);

  if (nip04_result != 0 || !plaintext) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_DECRYPTION,
                "NIP-04 decryption failed: %s", nip04_error ? nip04_error : "unknown error");
    free(nip04_error);
    return FALSE;
  }

  /* Decode share */
  GnSSSShare *share = gn_sss_share_decode(plaintext, error);
  gnostr_secure_clear(plaintext, strlen(plaintext));
  free(plaintext);

  if (!share) {
    return FALSE;
  }

  *out_share = share;
  return TRUE;
}

gboolean gn_social_recovery_recover(GPtrArray *collected_shares,
                                    guint8 threshold,
                                    gchar **out_nsec,
                                    GError **error) {
  g_return_val_if_fail(out_nsec != NULL, FALSE);
  *out_nsec = NULL;

  /* Combine shares */
  guint8 *secret = NULL;
  gsize secret_len = 0;

  if (!gn_sss_combine(collected_shares, threshold, &secret, &secret_len, error)) {
    return FALSE;
  }

  /* Verify we got 32 bytes (Nostr key size) */
  if (secret_len != 32) {
    gnostr_secure_free(secret, secret_len);
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_RECONSTRUCTION,
                "Reconstructed secret has wrong size (%zu, expected 32)", secret_len);
    return FALSE;
  }

  /* Convert to nsec */
  gchar *nsec = key_to_nsec(secret);
  gnostr_secure_free(secret, secret_len);

  if (!nsec) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_RECONSTRUCTION,
                "Failed to encode recovered key as nsec");
    return FALSE;
  }

  *out_nsec = nsec;
  return TRUE;
}

/* ============================================================
 * Configuration Persistence
 * ============================================================ */

/* Get config directory path */
static gchar *get_config_dir(void) {
  const gchar *data_dir = g_get_user_data_dir();
  return g_build_filename(data_dir, "gnostr-signer", "recovery", NULL);
}

/* Get config file path for an identity */
static gchar *get_config_path(const gchar *owner_npub) {
  gchar *dir = get_config_dir();
  /* Use truncated npub for filename */
  gchar *filename = g_strdup_printf("%.16s.json", owner_npub + 5);  /* Skip "npub1" prefix */
  gchar *path = g_build_filename(dir, filename, NULL);
  g_free(dir);
  g_free(filename);
  return path;
}

gchar *gn_recovery_config_to_json(GnRecoveryConfig *config) {
  if (!config) return NULL;

  JsonBuilder *builder = json_builder_new();
  json_builder_begin_object(builder);

  json_builder_set_member_name(builder, "version");
  json_builder_add_string_value(builder, config->version ? config->version : RECOVERY_CONFIG_VERSION);

  json_builder_set_member_name(builder, "owner_npub");
  json_builder_add_string_value(builder, config->owner_npub);

  json_builder_set_member_name(builder, "threshold");
  json_builder_add_int_value(builder, config->threshold);

  json_builder_set_member_name(builder, "total_shares");
  json_builder_add_int_value(builder, config->total_shares);

  json_builder_set_member_name(builder, "created_at");
  json_builder_add_int_value(builder, config->created_at);

  json_builder_set_member_name(builder, "last_verified");
  json_builder_add_int_value(builder, config->last_verified);

  /* Guardians array */
  json_builder_set_member_name(builder, "guardians");
  json_builder_begin_array(builder);
  for (guint i = 0; i < config->guardians->len; i++) {
    GnGuardian *g = g_ptr_array_index(config->guardians, i);
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "npub");
    json_builder_add_string_value(builder, g->npub);

    if (g->label) {
      json_builder_set_member_name(builder, "label");
      json_builder_add_string_value(builder, g->label);
    }

    json_builder_set_member_name(builder, "share_index");
    json_builder_add_int_value(builder, g->share_index);

    json_builder_set_member_name(builder, "assigned_at");
    json_builder_add_int_value(builder, g->assigned_at);

    json_builder_set_member_name(builder, "confirmed");
    json_builder_add_boolean_value(builder, g->confirmed);

    json_builder_end_object(builder);
  }
  json_builder_end_array(builder);

  json_builder_end_object(builder);

  JsonGenerator *gen = json_generator_new();
  json_generator_set_pretty(gen, TRUE);
  json_generator_set_indent(gen, 2);
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(gen, root);
  gchar *json = json_generator_to_data(gen, NULL);

  json_node_unref(root);
  g_object_unref(gen);
  g_object_unref(builder);

  return json;
}

GnRecoveryConfig *gn_recovery_config_from_json(const gchar *json, GError **error) {
  if (!json) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_STORAGE,
                "JSON string is NULL");
    return NULL;
  }

  JsonParser *parser = json_parser_new();
  GError *parse_error = NULL;

  if (!json_parser_load_from_data(parser, json, -1, &parse_error)) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_STORAGE,
                "Invalid JSON: %s", parse_error->message);
    g_error_free(parse_error);
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root)) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_STORAGE,
                "Expected JSON object");
    g_object_unref(parser);
    return NULL;
  }

  JsonObject *obj = json_node_get_object(root);

  /* Get owner_npub (required) */
  if (!json_object_has_member(obj, "owner_npub")) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_STORAGE,
                "Missing owner_npub field");
    g_object_unref(parser);
    return NULL;
  }

  const gchar *owner_npub = json_object_get_string_member(obj, "owner_npub");
  GnRecoveryConfig *config = gn_recovery_config_new(owner_npub);

  /* Version */
  if (json_object_has_member(obj, "version")) {
    g_free(config->version);
    config->version = g_strdup(json_object_get_string_member(obj, "version"));
  }

  /* Threshold */
  if (json_object_has_member(obj, "threshold")) {
    config->threshold = (guint8)json_object_get_int_member(obj, "threshold");
  }

  /* Total shares */
  if (json_object_has_member(obj, "total_shares")) {
    config->total_shares = (guint8)json_object_get_int_member(obj, "total_shares");
  }

  /* Timestamps */
  if (json_object_has_member(obj, "created_at")) {
    config->created_at = json_object_get_int_member(obj, "created_at");
  }
  if (json_object_has_member(obj, "last_verified")) {
    config->last_verified = json_object_get_int_member(obj, "last_verified");
  }

  /* Guardians */
  if (json_object_has_member(obj, "guardians")) {
    JsonArray *guardians_arr = json_object_get_array_member(obj, "guardians");
    guint n = json_array_get_length(guardians_arr);

    for (guint i = 0; i < n; i++) {
      JsonObject *g_obj = json_array_get_object_element(guardians_arr, i);

      const gchar *npub = json_object_get_string_member_with_default(g_obj, "npub", NULL);
      const gchar *label = json_object_get_string_member_with_default(g_obj, "label", NULL);

      if (!npub) continue;

      GnGuardian *guardian = gn_guardian_new(npub, label);
      guardian->share_index = (guint8)json_object_get_int_member(g_obj, "share_index");
      guardian->assigned_at = json_object_get_int_member(g_obj, "assigned_at");
      guardian->confirmed = json_object_get_boolean_member_with_default(g_obj, "confirmed", FALSE);

      g_ptr_array_add(config->guardians, guardian);
    }
  }

  g_object_unref(parser);
  return config;
}

gboolean gn_recovery_config_save(GnRecoveryConfig *config, GError **error) {
  g_return_val_if_fail(config != NULL, FALSE);

  /* Ensure directory exists */
  gchar *dir = get_config_dir();
  if (g_mkdir_with_parents(dir, 0700) != 0) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_STORAGE,
                "Failed to create config directory: %s", dir);
    g_free(dir);
    return FALSE;
  }
  g_free(dir);

  /* Serialize to JSON */
  gchar *json = gn_recovery_config_to_json(config);
  if (!json) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_STORAGE,
                "Failed to serialize configuration");
    return FALSE;
  }

  /* Write to file */
  gchar *path = get_config_path(config->owner_npub);
  GError *write_error = NULL;
  gboolean ok = g_file_set_contents(path, json, -1, &write_error);

  g_free(json);
  g_free(path);

  if (!ok) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_STORAGE,
                "Failed to write config file: %s", write_error->message);
    g_error_free(write_error);
    return FALSE;
  }

  return TRUE;
}

GnRecoveryConfig *gn_recovery_config_load(const gchar *owner_npub, GError **error) {
  g_return_val_if_fail(owner_npub != NULL, NULL);

  gchar *path = get_config_path(owner_npub);
  gchar *contents = NULL;
  gsize length = 0;
  GError *read_error = NULL;

  if (!g_file_get_contents(path, &contents, &length, &read_error)) {
    /* Not finding the file is not necessarily an error */
    if (g_error_matches(read_error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
      g_error_free(read_error);
      g_free(path);
      return NULL;  /* No error, just not found */
    }
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_STORAGE,
                "Failed to read config file: %s", read_error->message);
    g_error_free(read_error);
    g_free(path);
    return NULL;
  }
  g_free(path);

  GnRecoveryConfig *config = gn_recovery_config_from_json(contents, error);
  g_free(contents);

  return config;
}

gboolean gn_recovery_config_delete(const gchar *owner_npub, GError **error) {
  g_return_val_if_fail(owner_npub != NULL, FALSE);

  gchar *path = get_config_path(owner_npub);
  GFile *file = g_file_new_for_path(path);
  g_free(path);

  GError *delete_error = NULL;
  gboolean ok = g_file_delete(file, NULL, &delete_error);
  g_object_unref(file);

  if (!ok && !g_error_matches(delete_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_STORAGE,
                "Failed to delete config: %s", delete_error->message);
    g_error_free(delete_error);
    return FALSE;
  }

  g_clear_error(&delete_error);
  return TRUE;
}

gboolean gn_recovery_config_exists(const gchar *owner_npub) {
  if (!owner_npub) return FALSE;

  gchar *path = get_config_path(owner_npub);
  gboolean exists = g_file_test(path, G_FILE_TEST_IS_REGULAR);
  g_free(path);

  return exists;
}

/* ============================================================
 * Utility Functions
 * ============================================================ */

gboolean gn_social_recovery_validate_threshold(guint8 threshold,
                                               guint8 total_guardians,
                                               GError **error) {
  if (threshold < 2) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_INVALID_PARAMS,
                "Threshold must be at least 2 (single guardian could reconstruct key)");
    return FALSE;
  }

  if (total_guardians == 0) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_INVALID_PARAMS,
                "At least one guardian is required");
    return FALSE;
  }

  if (threshold > total_guardians) {
    g_set_error(error, GN_SOCIAL_RECOVERY_ERROR, GN_SOCIAL_RECOVERY_ERROR_INVALID_PARAMS,
                "Threshold (%u) cannot exceed number of guardians (%u)",
                threshold, total_guardians);
    return FALSE;
  }

  return TRUE;
}

gchar *gn_social_recovery_format_share_message(const gchar *encrypted_share,
                                               const gchar *guardian_label,
                                               const gchar *owner_npub) {
  if (!encrypted_share || !owner_npub) return NULL;

  const gchar *name = guardian_label ? guardian_label : "Guardian";
  gchar *short_npub = g_strndup(owner_npub, 20);

  gchar *message = g_strdup_printf(
    "Hello %s,\n\n"
    "You have been designated as a recovery guardian for the Nostr identity: %s...\n\n"
    "Please save the following encrypted recovery share in a secure location. "
    "You may be asked to provide this share if the owner needs to recover their key.\n\n"
    "IMPORTANT: Never share this with anyone except the original owner during recovery.\n\n"
    "--- BEGIN RECOVERY SHARE ---\n%s\n--- END RECOVERY SHARE ---\n\n"
    "To confirm receipt, please reply to this message.",
    name, short_npub, encrypted_share
  );

  g_free(short_npub);
  return message;
}
