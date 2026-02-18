/* hsm_provider_tpm.c - TPM/Secure Enclave HSM provider implementation
 *
 * Platform-specific implementations for hardware-backed key storage:
 *   - Linux: TPM 2.0 via tpm2-tss
 *   - macOS: Secure Enclave via Security.framework
 *   - Windows: TPM via Windows CNG
 *
 * SPDX-License-Identifier: MIT
 */
#include "hsm_provider_tpm.h"
#include "secure-memory.h"
#include <string.h>
#include <time.h>

/* Platform-specific includes */
#ifdef __linux__
  #ifdef GNOSTR_HAVE_TPM2
    #include <tss2/tss2_esys.h>
    #include <tss2/tss2_tctildr.h>
    #include <tss2/tss2_rc.h>
  #endif
#endif

#ifdef __APPLE__
  #include <CoreFoundation/CoreFoundation.h>
  #include <Security/Security.h>
  /* Note: LocalAuthentication requires Objective-C compilation.
   * If biometric auth is needed, implement in a separate .m file. */
#endif

#ifdef _WIN32
  #include <windows.h>
  #include <bcrypt.h>
  #include <ncrypt.h>
  #pragma comment(lib, "bcrypt.lib")
  #pragma comment(lib, "ncrypt.lib")
#endif

/* libnostr for crypto operations */
#include <nostr-gobject-1.0/nostr_keys.h>
#include <keys.h>       /* nostr_key_generate_private() - no GObject equivalent */
#include <nostr-event.h>

/* OpenSSL for HKDF (if available) */
#ifdef GNOSTR_HAVE_OPENSSL
  #include <openssl/kdf.h>
  #include <openssl/evp.h>
  #include <openssl/sha.h>
#else
  /* Fallback: use GLib checksum for SHA256 */
  #include <glib.h>
#endif

/* Master key identifier in keystore */
#define MASTER_KEY_LABEL "gnostr-master-key"
#define MASTER_KEY_SERVICE "org.gnostr.Signer.HardwareKeystore"
#define MASTER_KEY_ACCOUNT "master-key"

/* Key derivation info string */
#define KEY_DERIVATION_INFO "nostr-signing-key-v1"

/* ============================================================================
 * Private Types
 * ============================================================================ */

struct _GnHsmProviderTpm {
  GObject parent_instance;

  gboolean initialized;
  GnHwKeystoreBackend backend;
  GnHwKeystoreStatus status;
  gboolean fallback_enabled;
  gboolean using_fallback;

  /* Master key (cached when unlocked) */
  guint8 *master_key;
  gsize master_key_len;
  gboolean master_key_cached;

  /* Platform-specific handles */
#ifdef __linux__
  #ifdef GNOSTR_HAVE_TPM2
  ESYS_CONTEXT *esys_ctx;
  TSS2_TCTI_CONTEXT *tcti_ctx;
  ESYS_TR primary_handle;
  #endif
#endif

#ifdef __APPLE__
  SecKeyRef enclave_key;
#endif

#ifdef _WIN32
  NCRYPT_PROV_HANDLE cng_provider;
  NCRYPT_KEY_HANDLE cng_key;
#endif

  GMutex lock;
};

static void gn_hsm_provider_tpm_iface_init(GnHsmProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GnHsmProviderTpm, gn_hsm_provider_tpm, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GN_TYPE_HSM_PROVIDER,
                                              gn_hsm_provider_tpm_iface_init))

/* Signals */
enum {
  SIGNAL_STATUS_CHANGED,
  SIGNAL_MASTER_KEY_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = {0};

/* ============================================================================
 * Keystore Info Management
 * ============================================================================ */

GnHwKeystoreInfo *
gn_hw_keystore_info_copy(const GnHwKeystoreInfo *info)
{
  if (!info)
    return NULL;

  GnHwKeystoreInfo *copy = g_new0(GnHwKeystoreInfo, 1);
  copy->backend = info->backend;
  copy->status = info->status;
  copy->backend_name = g_strdup(info->backend_name);
  copy->backend_version = g_strdup(info->backend_version);
  copy->has_master_key = info->has_master_key;
  copy->master_key_id = g_strdup(info->master_key_id);
  copy->tpm_manufacturer = g_strdup(info->tpm_manufacturer);
  copy->tpm_version = g_strdup(info->tpm_version);
  copy->enclave_supported = info->enclave_supported;
  return copy;
}

void
gn_hw_keystore_info_free(GnHwKeystoreInfo *info)
{
  if (!info)
    return;
  g_free(info->backend_name);
  g_free(info->backend_version);
  g_free(info->master_key_id);
  g_free(info->tpm_manufacturer);
  g_free(info->tpm_version);
  g_free(info);
}

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static gchar *
bytes_to_hex(const guint8 *bytes, gsize len)
{
  static const gchar hex_chars[] = "0123456789abcdef";
  gchar *result = g_malloc(len * 2 + 1);
  for (gsize i = 0; i < len; i++) {
    result[i * 2] = hex_chars[(bytes[i] >> 4) & 0xF];
    result[i * 2 + 1] = hex_chars[bytes[i] & 0xF];
  }
  result[len * 2] = '\0';
  return result;
}

static gboolean
hex_to_bytes(const gchar *hex, guint8 *out, gsize expected_len)
{
  gsize hex_len = strlen(hex);
  if (hex_len != expected_len * 2)
    return FALSE;

  for (gsize i = 0; i < expected_len; i++) {
    gchar high = hex[i * 2];
    gchar low = hex[i * 2 + 1];
    guint8 val = 0;

    if (high >= '0' && high <= '9')
      val = (high - '0') << 4;
    else if (high >= 'a' && high <= 'f')
      val = (high - 'a' + 10) << 4;
    else if (high >= 'A' && high <= 'F')
      val = (high - 'A' + 10) << 4;
    else
      return FALSE;

    if (low >= '0' && low <= '9')
      val |= low - '0';
    else if (low >= 'a' && low <= 'f')
      val |= low - 'a' + 10;
    else if (low >= 'A' && low <= 'F')
      val |= low - 'A' + 10;
    else
      return FALSE;

    out[i] = val;
  }
  return TRUE;
}

/* HKDF-SHA256 implementation */
static gboolean
hkdf_sha256(const guint8 *ikm, gsize ikm_len,
            const guint8 *salt, gsize salt_len,
            const guint8 *info, gsize info_len,
            guint8 *okm, gsize okm_len)
{
#ifdef GNOSTR_HAVE_OPENSSL
  EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
  if (!pctx)
    return FALSE;

  if (EVP_PKEY_derive_init(pctx) <= 0 ||
      EVP_PKEY_CTX_set_hkdf_md(pctx, EVP_sha256()) <= 0 ||
      EVP_PKEY_CTX_set1_hkdf_salt(pctx, salt, salt_len) <= 0 ||
      EVP_PKEY_CTX_set1_hkdf_key(pctx, ikm, ikm_len) <= 0 ||
      EVP_PKEY_CTX_add1_hkdf_info(pctx, info, info_len) <= 0) {
    EVP_PKEY_CTX_free(pctx);
    return FALSE;
  }

  size_t out_len = okm_len;
  if (EVP_PKEY_derive(pctx, okm, &out_len) <= 0) {
    EVP_PKEY_CTX_free(pctx);
    return FALSE;
  }

  EVP_PKEY_CTX_free(pctx);
  return TRUE;
#else
  /* Simple HKDF implementation using HMAC-SHA256 */
  /* Extract phase */
  guint8 prk[32];

  GHmac *extract = g_hmac_new(G_CHECKSUM_SHA256, salt, salt_len);
  g_hmac_update(extract, ikm, ikm_len);
  gsize prk_len = 32;
  g_hmac_get_digest(extract, prk, &prk_len);
  g_hmac_unref(extract);

  /* Expand phase */
  if (okm_len > 255 * 32)
    return FALSE;

  guint8 t[32] = {0};
  gsize t_len = 0;
  guint8 counter = 1;
  gsize offset = 0;

  while (offset < okm_len) {
    GHmac *expand = g_hmac_new(G_CHECKSUM_SHA256, prk, 32);
    if (t_len > 0)
      g_hmac_update(expand, t, t_len);
    g_hmac_update(expand, info, info_len);
    g_hmac_update(expand, &counter, 1);

    gsize digest_len = 32;
    g_hmac_get_digest(expand, t, &digest_len);
    g_hmac_unref(expand);
    t_len = 32;

    gsize to_copy = MIN(32, okm_len - offset);
    memcpy(okm + offset, t, to_copy);
    offset += to_copy;
    counter++;
  }

  /* Clear sensitive data */
  memset(prk, 0, sizeof(prk));
  memset(t, 0, sizeof(t));
  return TRUE;
#endif
}

/* ============================================================================
 * Platform Detection
 * ============================================================================ */

GnHwKeystoreBackend
gn_hw_keystore_detect_backend(void)
{
#ifdef __APPLE__
  /* Check for Secure Enclave support */
  /* Secure Enclave is available on:
   * - Mac with T1/T2 chip or Apple Silicon
   * - iOS devices with A7 or later
   */
  #if TARGET_OS_OSX
    /* Check if Secure Enclave is available */
    SecAccessControlRef access = SecAccessControlCreateWithFlags(
      kCFAllocatorDefault,
      kSecAttrAccessibleWhenUnlockedThisDeviceOnly,
      kSecAccessControlPrivateKeyUsage,
      NULL);
    if (access) {
      CFRelease(access);
      return GN_HW_KEYSTORE_SECURE_ENCLAVE;
    }
  #endif
  return GN_HW_KEYSTORE_SOFTWARE;
#endif

#ifdef __linux__
  #ifdef GNOSTR_HAVE_TPM2
  /* Try to detect TPM 2.0 */
  TSS2_TCTI_CONTEXT *tcti = NULL;
  TSS2_RC rc = Tss2_TctiLdr_Initialize(NULL, &tcti);
  if (rc == TSS2_RC_SUCCESS && tcti) {
    Tss2_TctiLdr_Finalize(&tcti);
    return GN_HW_KEYSTORE_TPM;
  }
  #endif
  return GN_HW_KEYSTORE_SOFTWARE;
#endif

#ifdef _WIN32
  /* Check for TPM via CNG */
  NCRYPT_PROV_HANDLE prov = 0;
  SECURITY_STATUS status = NCryptOpenStorageProvider(
    &prov,
    MS_PLATFORM_CRYPTO_PROVIDER,
    0);
  if (status == ERROR_SUCCESS) {
    NCryptFreeObject(prov);
    return GN_HW_KEYSTORE_CNG;
  }
  return GN_HW_KEYSTORE_SOFTWARE;
#endif

  return GN_HW_KEYSTORE_NONE;
}

const gchar *
gn_hw_keystore_backend_to_string(GnHwKeystoreBackend backend)
{
  switch (backend) {
    case GN_HW_KEYSTORE_NONE:
      return "None";
    case GN_HW_KEYSTORE_TPM:
      return "TPM 2.0";
    case GN_HW_KEYSTORE_SECURE_ENCLAVE:
      return "Secure Enclave";
    case GN_HW_KEYSTORE_CNG:
      return "Windows TPM (CNG)";
    case GN_HW_KEYSTORE_SOFTWARE:
      return "Software Keystore";
    default:
      return "Unknown";
  }
}

const gchar *
gn_hw_keystore_status_to_string(GnHwKeystoreStatus status)
{
  switch (status) {
    case GN_HW_KEYSTORE_STATUS_UNKNOWN:
      return "Unknown";
    case GN_HW_KEYSTORE_STATUS_AVAILABLE:
      return "Available";
    case GN_HW_KEYSTORE_STATUS_UNAVAILABLE:
      return "Unavailable";
    case GN_HW_KEYSTORE_STATUS_DISABLED:
      return "Disabled";
    case GN_HW_KEYSTORE_STATUS_ERROR:
      return "Error";
    case GN_HW_KEYSTORE_STATUS_FALLBACK:
      return "Using Software Fallback";
    default:
      return "Unknown";
  }
}

gboolean
gn_hw_keystore_is_supported(void)
{
  GnHwKeystoreBackend backend = gn_hw_keystore_detect_backend();
  return backend != GN_HW_KEYSTORE_NONE;
}

/* ============================================================================
 * macOS Secure Enclave Implementation
 * ============================================================================ */

#ifdef __APPLE__

static SecKeyRef
create_secure_enclave_key(CFStringRef label, CFErrorRef *error)
{
  CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(
    kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks,
    &kCFTypeDictionaryValueCallBacks);

  /* Create access control for Secure Enclave */
  SecAccessControlRef access = SecAccessControlCreateWithFlags(
    kCFAllocatorDefault,
    kSecAttrAccessibleWhenUnlockedThisDeviceOnly,
    kSecAccessControlPrivateKeyUsage,
    error);
  if (!access) {
    CFRelease(attrs);
    return NULL;
  }

  /* Key attributes */
  CFMutableDictionaryRef private_attrs = CFDictionaryCreateMutable(
    kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks,
    &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(private_attrs, kSecAttrIsPermanent, kCFBooleanTrue);
  CFDictionarySetValue(private_attrs, kSecAttrAccessControl, access);
  CFDictionarySetValue(private_attrs, kSecAttrLabel, label);

  /* Use ECC P-256 (secp256r1) for Secure Enclave */
  /* We'll use this to derive a master secret, then use software
   * secp256k1 for actual Nostr signing */
  CFDictionarySetValue(attrs, kSecAttrKeyType, kSecAttrKeyTypeECSECPrimeRandom);
  int key_size = 256;
  CFNumberRef key_size_ref = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &key_size);
  CFDictionarySetValue(attrs, kSecAttrKeySizeInBits, key_size_ref);
  CFRelease(key_size_ref);
  CFDictionarySetValue(attrs, kSecAttrTokenID, kSecAttrTokenIDSecureEnclave);
  CFDictionarySetValue(attrs, kSecPrivateKeyAttrs, private_attrs);

  SecKeyRef key = SecKeyCreateRandomKey(attrs, error);

  CFRelease(access);
  CFRelease(private_attrs);
  CFRelease(attrs);

  return key;
}

static SecKeyRef
load_secure_enclave_key(CFStringRef label)
{
  CFMutableDictionaryRef query = CFDictionaryCreateMutable(
    kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks,
    &kCFTypeDictionaryValueCallBacks);

  CFDictionarySetValue(query, kSecClass, kSecClassKey);
  CFDictionarySetValue(query, kSecAttrLabel, label);
  CFDictionarySetValue(query, kSecAttrKeyType, kSecAttrKeyTypeECSECPrimeRandom);
  CFDictionarySetValue(query, kSecAttrTokenID, kSecAttrTokenIDSecureEnclave);
  CFDictionarySetValue(query, kSecReturnRef, kCFBooleanTrue);

  SecKeyRef key = NULL;
  OSStatus status = SecItemCopyMatching(query, (CFTypeRef *)&key);

  CFRelease(query);

  if (status == errSecSuccess)
    return key;
  return NULL;
}

static gboolean
delete_secure_enclave_key(CFStringRef label)
{
  CFMutableDictionaryRef query = CFDictionaryCreateMutable(
    kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks,
    &kCFTypeDictionaryValueCallBacks);

  CFDictionarySetValue(query, kSecClass, kSecClassKey);
  CFDictionarySetValue(query, kSecAttrLabel, label);
  CFDictionarySetValue(query, kSecAttrKeyType, kSecAttrKeyTypeECSECPrimeRandom);
  CFDictionarySetValue(query, kSecAttrTokenID, kSecAttrTokenIDSecureEnclave);

  OSStatus status = SecItemDelete(query);
  CFRelease(query);

  return status == errSecSuccess || status == errSecItemNotFound;
}

static gboolean
derive_master_secret_from_enclave(SecKeyRef key, guint8 *secret_out, gsize *len_out)
{
  /* Get public key and use its raw bytes as part of master secret derivation */
  SecKeyRef public_key = SecKeyCopyPublicKey(key);
  if (!public_key)
    return FALSE;

  CFErrorRef error = NULL;
  CFDataRef key_data = SecKeyCopyExternalRepresentation(public_key, &error);
  CFRelease(public_key);

  if (!key_data) {
    if (error) CFRelease(error);
    return FALSE;
  }

  /* Use HKDF to derive a 32-byte master secret from the public key */
  CFIndex data_len = CFDataGetLength(key_data);
  const UInt8 *data_ptr = CFDataGetBytePtr(key_data);

  guint8 salt[32] = {0}; /* Fixed salt for reproducibility */
  const guint8 *info = (const guint8 *)"gnostr-master-secret-v1";
  gsize info_len = strlen((const char *)info);

  gboolean result = hkdf_sha256(data_ptr, data_len,
                                 salt, sizeof(salt),
                                 info, info_len,
                                 secret_out, 32);

  CFRelease(key_data);
  if (result && len_out)
    *len_out = 32;

  return result;
}

#endif /* __APPLE__ */

/* ============================================================================
 * Linux TPM 2.0 Implementation
 * ============================================================================ */

#ifdef __linux__
#ifdef GNOSTR_HAVE_TPM2

static gboolean
tpm_init_context(GnHsmProviderTpm *self, GError **error)
{
  TSS2_RC rc;

  /* Initialize TCTI */
  rc = Tss2_TctiLdr_Initialize(NULL, &self->tcti_ctx);
  if (rc != TSS2_RC_SUCCESS) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_DEVICE_ERROR,
                "Failed to initialize TCTI: %s", Tss2_RC_Decode(rc));
    return FALSE;
  }

  /* Initialize ESYS context */
  rc = Esys_Initialize(&self->esys_ctx, self->tcti_ctx, NULL);
  if (rc != TSS2_RC_SUCCESS) {
    Tss2_TctiLdr_Finalize(&self->tcti_ctx);
    self->tcti_ctx = NULL;
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_DEVICE_ERROR,
                "Failed to initialize ESYS: %s", Tss2_RC_Decode(rc));
    return FALSE;
  }

  return TRUE;
}

static void
tpm_cleanup_context(GnHsmProviderTpm *self)
{
  if (self->esys_ctx) {
    Esys_Finalize(&self->esys_ctx);
    self->esys_ctx = NULL;
  }
  if (self->tcti_ctx) {
    Tss2_TctiLdr_Finalize(&self->tcti_ctx);
    self->tcti_ctx = NULL;
  }
}

static gboolean
tpm_create_primary(GnHsmProviderTpm *self, GError **error)
{
  TSS2_RC rc;

  /* Primary key template (RSA 2048) */
  TPM2B_SENSITIVE_CREATE in_sensitive = {0};
  TPM2B_PUBLIC in_public = {
    .size = 0,
    .publicArea = {
      .type = TPM2_ALG_RSA,
      .nameAlg = TPM2_ALG_SHA256,
      .objectAttributes = (TPMA_OBJECT_USERWITHAUTH |
                           TPMA_OBJECT_RESTRICTED |
                           TPMA_OBJECT_DECRYPT |
                           TPMA_OBJECT_FIXEDTPM |
                           TPMA_OBJECT_FIXEDPARENT |
                           TPMA_OBJECT_SENSITIVEDATAORIGIN),
      .authPolicy = {0},
      .parameters.rsaDetail = {
        .symmetric = {
          .algorithm = TPM2_ALG_AES,
          .keyBits.aes = 128,
          .mode.aes = TPM2_ALG_CFB
        },
        .scheme = {.scheme = TPM2_ALG_NULL},
        .keyBits = 2048,
        .exponent = 0
      },
      .unique.rsa = {0}
    }
  };

  TPM2B_DATA outside_info = {0};
  TPML_PCR_SELECTION creation_pcr = {0};
  TPM2B_PUBLIC *out_public = NULL;
  TPM2B_CREATION_DATA *creation_data = NULL;
  TPM2B_DIGEST *creation_hash = NULL;
  TPMT_TK_CREATION *creation_ticket = NULL;

  rc = Esys_CreatePrimary(
    self->esys_ctx,
    ESYS_TR_RH_OWNER,
    ESYS_TR_PASSWORD,
    ESYS_TR_NONE,
    ESYS_TR_NONE,
    &in_sensitive,
    &in_public,
    &outside_info,
    &creation_pcr,
    &self->primary_handle,
    &out_public,
    &creation_data,
    &creation_hash,
    &creation_ticket);

  Esys_Free(out_public);
  Esys_Free(creation_data);
  Esys_Free(creation_hash);
  Esys_Free(creation_ticket);

  if (rc != TSS2_RC_SUCCESS) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_DEVICE_ERROR,
                "Failed to create primary key: %s", Tss2_RC_Decode(rc));
    return FALSE;
  }

  return TRUE;
}

static gboolean
tpm_get_random_bytes(GnHsmProviderTpm *self, guint8 *buffer, gsize len, GError **error)
{
  TSS2_RC rc;
  TPM2B_DIGEST *random_bytes = NULL;

  /* TPM2_GetRandom has a max of 64 bytes per call */
  gsize offset = 0;
  while (offset < len) {
    gsize chunk = MIN(64, len - offset);

    rc = Esys_GetRandom(
      self->esys_ctx,
      ESYS_TR_NONE,
      ESYS_TR_NONE,
      ESYS_TR_NONE,
      chunk,
      &random_bytes);

    if (rc != TSS2_RC_SUCCESS) {
      g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_DEVICE_ERROR,
                  "Failed to get random bytes: %s", Tss2_RC_Decode(rc));
      return FALSE;
    }

    memcpy(buffer + offset, random_bytes->buffer, random_bytes->size);
    offset += random_bytes->size;
    Esys_Free(random_bytes);
    random_bytes = NULL;
  }

  return TRUE;
}

#endif /* GNOSTR_HAVE_TPM2 */
#endif /* __linux__ */

/* ============================================================================
 * Windows CNG/TPM Implementation
 * ============================================================================ */

#ifdef _WIN32

static gboolean
cng_init_provider(GnHsmProviderTpm *self, GError **error)
{
  SECURITY_STATUS status = NCryptOpenStorageProvider(
    &self->cng_provider,
    MS_PLATFORM_CRYPTO_PROVIDER,
    0);

  if (status != ERROR_SUCCESS) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_DEVICE_ERROR,
                "Failed to open CNG provider: 0x%lx", status);
    return FALSE;
  }

  return TRUE;
}

static void
cng_cleanup_provider(GnHsmProviderTpm *self)
{
  if (self->cng_key) {
    NCryptFreeObject(self->cng_key);
    self->cng_key = 0;
  }
  if (self->cng_provider) {
    NCryptFreeObject(self->cng_provider);
    self->cng_provider = 0;
  }
}

static gboolean
cng_create_master_key(GnHsmProviderTpm *self, GError **error)
{
  SECURITY_STATUS status;

  /* Create a persisted key in the TPM */
  status = NCryptCreatePersistedKey(
    self->cng_provider,
    &self->cng_key,
    BCRYPT_RSA_ALGORITHM,
    L"GnostrMasterKey",
    0,
    NCRYPT_OVERWRITE_KEY_FLAG);

  if (status != ERROR_SUCCESS) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_KEY_GENERATION_FAILED,
                "Failed to create key: 0x%lx", status);
    return FALSE;
  }

  /* Set key length */
  DWORD key_length = 2048;
  status = NCryptSetProperty(
    self->cng_key,
    NCRYPT_LENGTH_PROPERTY,
    (PBYTE)&key_length,
    sizeof(key_length),
    0);

  if (status != ERROR_SUCCESS) {
    NCryptDeleteKey(self->cng_key, 0);
    self->cng_key = 0;
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_KEY_GENERATION_FAILED,
                "Failed to set key length: 0x%lx", status);
    return FALSE;
  }

  /* Finalize the key */
  status = NCryptFinalizeKey(self->cng_key, 0);

  if (status != ERROR_SUCCESS) {
    NCryptDeleteKey(self->cng_key, 0);
    self->cng_key = 0;
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_KEY_GENERATION_FAILED,
                "Failed to finalize key: 0x%lx", status);
    return FALSE;
  }

  return TRUE;
}

static gboolean
cng_load_master_key(GnHsmProviderTpm *self)
{
  SECURITY_STATUS status = NCryptOpenKey(
    self->cng_provider,
    &self->cng_key,
    L"GnostrMasterKey",
    0,
    0);

  return status == ERROR_SUCCESS;
}

static gboolean
cng_delete_master_key(GnHsmProviderTpm *self, GError **error)
{
  if (!self->cng_key) {
    if (!cng_load_master_key(self)) {
      return TRUE; /* Key doesn't exist */
    }
  }

  SECURITY_STATUS status = NCryptDeleteKey(self->cng_key, 0);
  self->cng_key = 0;

  if (status != ERROR_SUCCESS) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_FAILED,
                "Failed to delete key: 0x%lx", status);
    return FALSE;
  }

  return TRUE;
}

#endif /* _WIN32 */

/* ============================================================================
 * Software Fallback Implementation
 * ============================================================================ */

/* Store/retrieve master key using libsecret or Keychain */

#ifdef __APPLE__

static gboolean
software_store_master_key(const guint8 *key, gsize len)
{
  CFMutableDictionaryRef query = CFDictionaryCreateMutable(
    kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks,
    &kCFTypeDictionaryValueCallBacks);

  CFStringRef service = CFStringCreateWithCString(NULL, MASTER_KEY_SERVICE, kCFStringEncodingUTF8);
  CFStringRef account = CFStringCreateWithCString(NULL, MASTER_KEY_ACCOUNT, kCFStringEncodingUTF8);
  CFDataRef key_data = CFDataCreate(NULL, key, len);

  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, service);
  CFDictionarySetValue(query, kSecAttrAccount, account);

  /* Delete existing */
  SecItemDelete(query);

  CFDictionarySetValue(query, kSecValueData, key_data);
  CFDictionarySetValue(query, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlock);

  OSStatus status = SecItemAdd(query, NULL);

  CFRelease(service);
  CFRelease(account);
  CFRelease(key_data);
  CFRelease(query);

  return status == errSecSuccess;
}

static gboolean
software_load_master_key(guint8 *key_out, gsize *len_out)
{
  CFMutableDictionaryRef query = CFDictionaryCreateMutable(
    kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks,
    &kCFTypeDictionaryValueCallBacks);

  CFStringRef service = CFStringCreateWithCString(NULL, MASTER_KEY_SERVICE, kCFStringEncodingUTF8);
  CFStringRef account = CFStringCreateWithCString(NULL, MASTER_KEY_ACCOUNT, kCFStringEncodingUTF8);

  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, service);
  CFDictionarySetValue(query, kSecAttrAccount, account);
  CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);

  CFDataRef key_data = NULL;
  OSStatus status = SecItemCopyMatching(query, (CFTypeRef *)&key_data);

  CFRelease(service);
  CFRelease(account);
  CFRelease(query);

  if (status == errSecSuccess && key_data) {
    CFIndex len = CFDataGetLength(key_data);
    if (len <= 32) {
      memcpy(key_out, CFDataGetBytePtr(key_data), len);
      *len_out = len;
      CFRelease(key_data);
      return TRUE;
    }
    CFRelease(key_data);
  }

  return FALSE;
}

static gboolean
software_delete_master_key(void)
{
  CFMutableDictionaryRef query = CFDictionaryCreateMutable(
    kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks,
    &kCFTypeDictionaryValueCallBacks);

  CFStringRef service = CFStringCreateWithCString(NULL, MASTER_KEY_SERVICE, kCFStringEncodingUTF8);
  CFStringRef account = CFStringCreateWithCString(NULL, MASTER_KEY_ACCOUNT, kCFStringEncodingUTF8);

  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, service);
  CFDictionarySetValue(query, kSecAttrAccount, account);

  OSStatus status = SecItemDelete(query);

  CFRelease(service);
  CFRelease(account);
  CFRelease(query);

  return status == errSecSuccess || status == errSecItemNotFound;
}

static gboolean
software_has_master_key(void)
{
  guint8 key[32];
  gsize len;
  return software_load_master_key(key, &len);
}

#else /* Non-Apple platforms use libsecret */

#ifdef GNOSTR_HAVE_LIBSECRET
#include <libsecret/secret.h>

static const SecretSchema MASTER_KEY_SCHEMA = {
  MASTER_KEY_SERVICE,
  SECRET_SCHEMA_NONE,
  {
    { "purpose", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { NULL, 0 }
  }
};

static gboolean
software_store_master_key(const guint8 *key, gsize len)
{
  gchar *hex = bytes_to_hex(key, len);
  GError *err = NULL;

  gboolean ok = secret_password_store_sync(
    &MASTER_KEY_SCHEMA,
    SECRET_COLLECTION_DEFAULT,
    "Gnostr Hardware Keystore Master Key",
    hex,
    NULL,
    &err,
    "purpose", "master-key",
    NULL);

  /* Securely zero and free */
  memset(hex, 0, strlen(hex));
  g_free(hex);

  if (err) {
    g_warning("Failed to store master key: %s", err->message);
    g_error_free(err);
    return FALSE;
  }

  return ok;
}

static gboolean
software_load_master_key(guint8 *key_out, gsize *len_out)
{
  GError *err = NULL;

  gchar *hex = secret_password_lookup_sync(
    &MASTER_KEY_SCHEMA,
    NULL,
    &err,
    "purpose", "master-key",
    NULL);

  if (err) {
    g_warning("Failed to load master key: %s", err->message);
    g_error_free(err);
    return FALSE;
  }

  if (!hex)
    return FALSE;

  gboolean ok = hex_to_bytes(hex, key_out, 32);
  if (ok)
    *len_out = 32;

  /* Securely zero and free */
  memset(hex, 0, strlen(hex));
  secret_password_free(hex);

  return ok;
}

static gboolean
software_delete_master_key(void)
{
  GError *err = NULL;

  gboolean ok = secret_password_clear_sync(
    &MASTER_KEY_SCHEMA,
    NULL,
    &err,
    "purpose", "master-key",
    NULL);

  if (err) {
    g_warning("Failed to delete master key: %s", err->message);
    g_error_free(err);
    return FALSE;
  }

  return ok;
}

static gboolean
software_has_master_key(void)
{
  guint8 key[32];
  gsize len;
  return software_load_master_key(key, &len);
}

#else

/* No libsecret - minimal implementation */
static gboolean software_store_master_key(const guint8 *key, gsize len)
{
  (void)key; (void)len;
  g_warning("libsecret not available for software keystore");
  return FALSE;
}

static gboolean software_load_master_key(guint8 *key_out, gsize *len_out)
{
  (void)key_out; (void)len_out;
  return FALSE;
}

static gboolean software_delete_master_key(void)
{
  return TRUE;
}

static gboolean software_has_master_key(void)
{
  return FALSE;
}

#endif /* GNOSTR_HAVE_LIBSECRET */
#endif /* __APPLE__ */

/* ============================================================================
 * Provider Interface Implementation
 * ============================================================================ */

static const gchar *
tpm_get_name(GnHsmProvider *provider)
{
  GnHsmProviderTpm *self = GN_HSM_PROVIDER_TPM(provider);
  return gn_hw_keystore_backend_to_string(self->backend);
}

static gboolean
tpm_is_available(GnHsmProvider *provider)
{
  GnHsmProviderTpm *self = GN_HSM_PROVIDER_TPM(provider);
  return self->status == GN_HW_KEYSTORE_STATUS_AVAILABLE ||
         self->status == GN_HW_KEYSTORE_STATUS_FALLBACK;
}

static gboolean
tpm_init_provider(GnHsmProvider *provider, GError **error)
{
  GnHsmProviderTpm *self = GN_HSM_PROVIDER_TPM(provider);

  g_mutex_lock(&self->lock);

  if (self->initialized) {
    g_mutex_unlock(&self->lock);
    return TRUE;
  }

  /* Detect and initialize backend */
  self->backend = gn_hw_keystore_detect_backend();

  switch (self->backend) {
#ifdef __APPLE__
    case GN_HW_KEYSTORE_SECURE_ENCLAVE: {
      CFStringRef label = CFStringCreateWithCString(NULL, MASTER_KEY_LABEL, kCFStringEncodingUTF8);
      self->enclave_key = load_secure_enclave_key(label);
      CFRelease(label);

      if (self->enclave_key || self->fallback_enabled) {
        self->status = GN_HW_KEYSTORE_STATUS_AVAILABLE;
        self->initialized = TRUE;
      } else {
        self->status = GN_HW_KEYSTORE_STATUS_UNAVAILABLE;
      }
      break;
    }
#endif

#ifdef __linux__
#ifdef GNOSTR_HAVE_TPM2
    case GN_HW_KEYSTORE_TPM: {
      if (!tpm_init_context(self, error)) {
        if (self->fallback_enabled) {
          self->status = GN_HW_KEYSTORE_STATUS_FALLBACK;
          self->using_fallback = TRUE;
          self->initialized = TRUE;
        } else {
          self->status = GN_HW_KEYSTORE_STATUS_ERROR;
        }
      } else {
        self->status = GN_HW_KEYSTORE_STATUS_AVAILABLE;
        self->initialized = TRUE;
      }
      break;
    }
#endif
#endif

#ifdef _WIN32
    case GN_HW_KEYSTORE_CNG: {
      if (!cng_init_provider(self, error)) {
        if (self->fallback_enabled) {
          self->status = GN_HW_KEYSTORE_STATUS_FALLBACK;
          self->using_fallback = TRUE;
          self->initialized = TRUE;
        } else {
          self->status = GN_HW_KEYSTORE_STATUS_ERROR;
        }
      } else {
        /* Try to load existing key */
        cng_load_master_key(self);
        self->status = GN_HW_KEYSTORE_STATUS_AVAILABLE;
        self->initialized = TRUE;
      }
      break;
    }
#endif

    case GN_HW_KEYSTORE_SOFTWARE:
    default:
      if (self->fallback_enabled) {
        self->status = GN_HW_KEYSTORE_STATUS_FALLBACK;
        self->using_fallback = TRUE;
        self->initialized = TRUE;
      } else {
        self->status = GN_HW_KEYSTORE_STATUS_UNAVAILABLE;
        g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
                    "No hardware keystore available and fallback disabled");
      }
      break;
  }

  g_mutex_unlock(&self->lock);

  if (self->initialized) {
    g_message("TPM/Secure Enclave provider initialized: %s (%s)",
              gn_hw_keystore_backend_to_string(self->backend),
              gn_hw_keystore_status_to_string(self->status));
  }

  return self->initialized;
}

static void
tpm_shutdown_provider(GnHsmProvider *provider)
{
  GnHsmProviderTpm *self = GN_HSM_PROVIDER_TPM(provider);

  g_mutex_lock(&self->lock);

  /* Clear cached master key */
  if (self->master_key) {
    memset(self->master_key, 0, self->master_key_len);
    g_free(self->master_key);
    self->master_key = NULL;
    self->master_key_len = 0;
    self->master_key_cached = FALSE;
  }

#ifdef __APPLE__
  if (self->enclave_key) {
    CFRelease(self->enclave_key);
    self->enclave_key = NULL;
  }
#endif

#ifdef __linux__
#ifdef GNOSTR_HAVE_TPM2
  tpm_cleanup_context(self);
#endif
#endif

#ifdef _WIN32
  cng_cleanup_provider(self);
#endif

  self->initialized = FALSE;
  self->status = GN_HW_KEYSTORE_STATUS_UNKNOWN;

  g_mutex_unlock(&self->lock);

  g_message("TPM/Secure Enclave provider shut down");
}

static GPtrArray *
tpm_detect_devices(GnHsmProvider *provider, GError **error)
{
  GnHsmProviderTpm *self = GN_HSM_PROVIDER_TPM(provider);

  GPtrArray *devices = g_ptr_array_new_with_free_func(
    (GDestroyNotify)gn_hsm_device_info_free);

  g_mutex_lock(&self->lock);

  /* Return a single virtual device representing the hardware keystore */
  GnHsmDeviceInfo *info = g_new0(GnHsmDeviceInfo, 1);
  info->slot_id = 0;
  info->label = g_strdup(gn_hw_keystore_backend_to_string(self->backend));
  info->manufacturer = g_strdup("Platform Hardware Keystore");
  info->model = g_strdup(self->using_fallback ? "Software Fallback" : "Hardware Enclave");
  info->serial = g_strdup("0");
  info->flags = 0;
  info->is_token_present = TRUE;
  info->is_initialized = self->master_key_cached || gn_hsm_provider_tpm_has_master_key(self);
  info->needs_pin = FALSE;

  g_ptr_array_add(devices, info);

  g_mutex_unlock(&self->lock);
  (void)error;
  return devices;
}

static GPtrArray *
tpm_list_keys(GnHsmProvider *provider, guint64 slot_id, GError **error)
{
  (void)provider;
  (void)slot_id;
  (void)error;

  /* Keys are derived on-demand, not stored */
  return g_ptr_array_new_with_free_func((GDestroyNotify)gn_hsm_key_info_free);
}

static GnHsmKeyInfo *
tpm_get_public_key(GnHsmProvider *provider,
                   guint64 slot_id,
                   const gchar *key_id,
                   GError **error)
{
  (void)provider;
  (void)slot_id;
  (void)key_id;
  (void)error;

  /* Key info is derived on-demand via derive_signing_key */
  g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
              "Use derive_signing_key to get keys from hardware keystore");
  return NULL;
}

static gboolean
tpm_sign_hash(GnHsmProvider *provider,
              guint64 slot_id,
              const gchar *key_id,
              const guint8 *hash,
              gsize hash_len,
              guint8 *signature,
              gsize *signature_len,
              GError **error)
{
  GnHsmProviderTpm *self = GN_HSM_PROVIDER_TPM(provider);
  (void)slot_id;

  if (hash_len != 32) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_FAILED,
                "Hash must be 32 bytes");
    return FALSE;
  }

  if (*signature_len < 64) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_FAILED,
                "Signature buffer too small");
    return FALSE;
  }

  /* Derive signing key for this key_id (which should be an npub) */
  guint8 private_key[32];
  if (!gn_hsm_provider_tpm_derive_signing_key(self, key_id, private_key, error)) {
    return FALSE;
  }

  /* Sign using libnostr/secp256k1 */
  gchar *sk_hex = bytes_to_hex(private_key, 32);
  memset(private_key, 0, sizeof(private_key));

  g_autoptr(GNostrKeys) gkeys = gnostr_keys_new_from_hex(sk_hex, NULL);
  if (!gkeys) {
    memset(sk_hex, 0, strlen(sk_hex));
    g_free(sk_hex);
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_SIGNING_FAILED,
                "Failed to derive public key");
    return FALSE;
  }

  const gchar *pk_hex = gnostr_keys_get_pubkey(gkeys);

  /* Create a temporary event to sign the hash */
  NostrEvent *event = nostr_event_new();
  nostr_event_set_pubkey(event, pk_hex);
  nostr_event_set_kind(event, 1);
  nostr_event_set_created_at(event, time(NULL));
  nostr_event_set_content(event, "");


  /* Set the event id to our hash */
  gchar *hash_hex = bytes_to_hex(hash, 32);
  event->id = g_strdup(hash_hex);
  g_free(hash_hex);

  int sign_result = nostr_event_sign(event, sk_hex);

  memset(sk_hex, 0, strlen(sk_hex));
  g_free(sk_hex);

  if (sign_result != 0) {
    nostr_event_free(event);
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_SIGNING_FAILED,
                "Signing failed");
    return FALSE;
  }

  const char *sig_hex = nostr_event_get_sig(event);
  if (!sig_hex || !hex_to_bytes(sig_hex, signature, 64)) {
    nostr_event_free(event);
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_SIGNING_FAILED,
                "Failed to decode signature");
    return FALSE;
  }

  nostr_event_free(event);
  *signature_len = 64;
  return TRUE;
}

static gchar *
tpm_sign_event(GnHsmProvider *provider,
               guint64 slot_id,
               const gchar *key_id,
               const gchar *event_json,
               GError **error)
{
  GnHsmProviderTpm *self = GN_HSM_PROVIDER_TPM(provider);
  (void)slot_id;

  /* Derive signing key */
  guint8 private_key[32];
  if (!gn_hsm_provider_tpm_derive_signing_key(self, key_id, private_key, error)) {
    return NULL;
  }

  gchar *sk_hex = bytes_to_hex(private_key, 32);
  memset(private_key, 0, sizeof(private_key));

  /* Parse and sign the event */
  NostrEvent *event = nostr_event_new();
  if (!nostr_event_deserialize_compact(event, event_json, NULL)) {
    memset(sk_hex, 0, strlen(sk_hex));
    g_free(sk_hex);
    nostr_event_free(event);
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_SIGNING_FAILED,
                "Failed to parse event JSON");
    return NULL;
  }

  int sign_result = nostr_event_sign(event, sk_hex);

  memset(sk_hex, 0, strlen(sk_hex));
  g_free(sk_hex);

  if (sign_result != 0) {
    nostr_event_free(event);
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_SIGNING_FAILED,
                "Event signing failed");
    return NULL;
  }

  gchar *signed_json = nostr_event_serialize_compact(event);
  nostr_event_free(event);

  return signed_json;
}

static GnHsmKeyInfo *
tpm_generate_key(GnHsmProvider *provider,
                 guint64 slot_id,
                 const gchar *label,
                 GnHsmKeyType key_type,
                 GError **error)
{
  (void)provider;
  (void)slot_id;
  (void)label;
  (void)key_type;

  g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
              "Hardware keystore derives keys from master key - use create_master_key first");
  return NULL;
}

static gboolean
tpm_import_key(GnHsmProvider *provider,
               guint64 slot_id,
               const gchar *label,
               const guint8 *private_key,
               gsize key_len,
               GnHsmKeyInfo **out_info,
               GError **error)
{
  (void)provider;
  (void)slot_id;
  (void)label;
  (void)private_key;
  (void)key_len;
  (void)out_info;

  g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
              "Hardware keystore does not support key import - keys are derived from master");
  return FALSE;
}

static gboolean
tpm_delete_key(GnHsmProvider *provider,
               guint64 slot_id,
               const gchar *key_id,
               GError **error)
{
  (void)provider;
  (void)slot_id;
  (void)key_id;

  g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
              "Derived keys cannot be individually deleted - delete the master key to remove all");
  return FALSE;
}

static gboolean
tpm_login(GnHsmProvider *provider, guint64 slot_id, const gchar *pin, GError **error)
{
  (void)provider;
  (void)slot_id;
  (void)pin;
  (void)error;
  /* Hardware keystore doesn't use PIN login */
  return TRUE;
}

static void
tpm_logout(GnHsmProvider *provider, guint64 slot_id)
{
  (void)provider;
  (void)slot_id;
  /* No-op */
}

/* ============================================================================
 * Interface Setup
 * ============================================================================ */

static void
gn_hsm_provider_tpm_iface_init(GnHsmProviderInterface *iface)
{
  iface->get_name = tpm_get_name;
  iface->is_available = tpm_is_available;
  iface->init_provider = tpm_init_provider;
  iface->shutdown_provider = tpm_shutdown_provider;
  iface->detect_devices = tpm_detect_devices;
  iface->list_keys = tpm_list_keys;
  iface->get_public_key = tpm_get_public_key;
  iface->sign_hash = tpm_sign_hash;
  iface->sign_event = tpm_sign_event;
  iface->generate_key = tpm_generate_key;
  iface->import_key = tpm_import_key;
  iface->delete_key = tpm_delete_key;
  iface->login = tpm_login;
  iface->logout = tpm_logout;
}

/* ============================================================================
 * GObject Implementation
 * ============================================================================ */

static void
gn_hsm_provider_tpm_finalize(GObject *object)
{
  GnHsmProviderTpm *self = GN_HSM_PROVIDER_TPM(object);

  if (self->initialized) {
    tpm_shutdown_provider(GN_HSM_PROVIDER(self));
  }

  g_mutex_clear(&self->lock);

  G_OBJECT_CLASS(gn_hsm_provider_tpm_parent_class)->finalize(object);
}

static void
gn_hsm_provider_tpm_class_init(GnHsmProviderTpmClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = gn_hsm_provider_tpm_finalize;

  /**
   * GnHsmProviderTpm::status-changed:
   * @provider: The provider
   * @status: The new status
   */
  signals[SIGNAL_STATUS_CHANGED] =
    g_signal_new("status-changed",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE, 1,
                 G_TYPE_INT);

  /**
   * GnHsmProviderTpm::master-key-changed:
   * @provider: The provider
   * @has_master_key: Whether master key is present
   */
  signals[SIGNAL_MASTER_KEY_CHANGED] =
    g_signal_new("master-key-changed",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE, 1,
                 G_TYPE_BOOLEAN);
}

static void
gn_hsm_provider_tpm_init(GnHsmProviderTpm *self)
{
  g_mutex_init(&self->lock);
  self->initialized = FALSE;
  self->backend = GN_HW_KEYSTORE_NONE;
  self->status = GN_HW_KEYSTORE_STATUS_UNKNOWN;
  self->fallback_enabled = TRUE;
  self->using_fallback = FALSE;
  self->master_key = NULL;
  self->master_key_len = 0;
  self->master_key_cached = FALSE;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

GnHsmProviderTpm *
gn_hsm_provider_tpm_new(void)
{
  return g_object_new(GN_TYPE_HSM_PROVIDER_TPM, NULL);
}

GnHwKeystoreInfo *
gn_hsm_provider_tpm_get_keystore_info(GnHsmProviderTpm *self)
{
  g_return_val_if_fail(GN_IS_HSM_PROVIDER_TPM(self), NULL);

  g_mutex_lock(&self->lock);

  GnHwKeystoreInfo *info = g_new0(GnHwKeystoreInfo, 1);
  info->backend = self->backend;
  info->status = self->status;
  info->backend_name = g_strdup(gn_hw_keystore_backend_to_string(self->backend));
  info->backend_version = g_strdup("1.0");
  info->has_master_key = self->master_key_cached || software_has_master_key();
  info->master_key_id = g_strdup(MASTER_KEY_LABEL);

#ifdef __APPLE__
  info->enclave_supported = (self->backend == GN_HW_KEYSTORE_SECURE_ENCLAVE);
#endif

  g_mutex_unlock(&self->lock);
  return info;
}

GnHwKeystoreBackend
gn_hsm_provider_tpm_get_backend(GnHsmProviderTpm *self)
{
  g_return_val_if_fail(GN_IS_HSM_PROVIDER_TPM(self), GN_HW_KEYSTORE_NONE);
  return self->backend;
}

GnHwKeystoreStatus
gn_hsm_provider_tpm_get_status(GnHsmProviderTpm *self)
{
  g_return_val_if_fail(GN_IS_HSM_PROVIDER_TPM(self), GN_HW_KEYSTORE_STATUS_UNKNOWN);
  return self->status;
}

gboolean
gn_hsm_provider_tpm_has_master_key(GnHsmProviderTpm *self)
{
  g_return_val_if_fail(GN_IS_HSM_PROVIDER_TPM(self), FALSE);

  if (self->master_key_cached)
    return TRUE;

#ifdef __APPLE__
  if (self->backend == GN_HW_KEYSTORE_SECURE_ENCLAVE) {
    CFStringRef label = CFStringCreateWithCString(NULL, MASTER_KEY_LABEL, kCFStringEncodingUTF8);
    SecKeyRef key = load_secure_enclave_key(label);
    CFRelease(label);
    if (key) {
      CFRelease(key);
      return TRUE;
    }
  }
#endif

#ifdef _WIN32
  if (self->backend == GN_HW_KEYSTORE_CNG) {
    return self->cng_key != 0;
  }
#endif

  /* Check software fallback */
  return software_has_master_key();
}

gboolean
gn_hsm_provider_tpm_create_master_key(GnHsmProviderTpm *self, GError **error)
{
  g_return_val_if_fail(GN_IS_HSM_PROVIDER_TPM(self), FALSE);

  g_mutex_lock(&self->lock);

  gboolean result = FALSE;

#ifdef __APPLE__
  if (self->backend == GN_HW_KEYSTORE_SECURE_ENCLAVE && !self->using_fallback) {
    CFStringRef label = CFStringCreateWithCString(NULL, MASTER_KEY_LABEL, kCFStringEncodingUTF8);
    CFErrorRef cf_error = NULL;

    /* Delete existing key first */
    delete_secure_enclave_key(label);

    self->enclave_key = create_secure_enclave_key(label, &cf_error);
    CFRelease(label);

    if (!self->enclave_key) {
      if (cf_error) {
        CFStringRef desc = CFErrorCopyDescription(cf_error);
        char buf[256];
        CFStringGetCString(desc, buf, sizeof(buf), kCFStringEncodingUTF8);
        g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_KEY_GENERATION_FAILED,
                    "Failed to create Secure Enclave key: %s", buf);
        CFRelease(desc);
        CFRelease(cf_error);
      }
      g_mutex_unlock(&self->lock);
      return FALSE;
    }

    /* Derive and cache master secret */
    self->master_key = g_malloc(32);
    if (!derive_master_secret_from_enclave(self->enclave_key, self->master_key, &self->master_key_len)) {
      g_free(self->master_key);
      self->master_key = NULL;
      g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_KEY_GENERATION_FAILED,
                  "Failed to derive master secret from Secure Enclave");
      g_mutex_unlock(&self->lock);
      return FALSE;
    }
    self->master_key_cached = TRUE;
    result = TRUE;
  } else
#endif

#ifdef _WIN32
  if (self->backend == GN_HW_KEYSTORE_CNG && !self->using_fallback) {
    result = cng_create_master_key(self, error);
  } else
#endif

#ifdef __linux__
#ifdef GNOSTR_HAVE_TPM2
  if (self->backend == GN_HW_KEYSTORE_TPM && !self->using_fallback) {
    /* Generate random master key using TPM */
    self->master_key = g_malloc(32);
    if (!tpm_get_random_bytes(self, self->master_key, 32, error)) {
      g_free(self->master_key);
      self->master_key = NULL;
      g_mutex_unlock(&self->lock);
      return FALSE;
    }
    self->master_key_len = 32;
    self->master_key_cached = TRUE;

    /* Store in software keystore as backup */
    software_store_master_key(self->master_key, 32);
    result = TRUE;
  } else
#endif
#endif

  /* Software fallback */
  {
    /* Generate random master key */
    guint8 key[32];
    for (int i = 0; i < 32; i++) {
      key[i] = g_random_int_range(0, 256);
    }

    if (!software_store_master_key(key, 32)) {
      memset(key, 0, sizeof(key));
      g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_KEY_GENERATION_FAILED,
                  "Failed to store master key");
      g_mutex_unlock(&self->lock);
      return FALSE;
    }

    /* Cache the key */
    self->master_key = g_malloc(32);
    memcpy(self->master_key, key, 32);
    self->master_key_len = 32;
    self->master_key_cached = TRUE;
    memset(key, 0, sizeof(key));
    result = TRUE;
  }

  g_mutex_unlock(&self->lock);

  if (result) {
    g_signal_emit(self, signals[SIGNAL_MASTER_KEY_CHANGED], 0, TRUE);
  }

  return result;
}

gboolean
gn_hsm_provider_tpm_delete_master_key(GnHsmProviderTpm *self, GError **error)
{
  g_return_val_if_fail(GN_IS_HSM_PROVIDER_TPM(self), FALSE);

  g_mutex_lock(&self->lock);

  /* Clear cached key */
  if (self->master_key) {
    memset(self->master_key, 0, self->master_key_len);
    g_free(self->master_key);
    self->master_key = NULL;
    self->master_key_len = 0;
    self->master_key_cached = FALSE;
  }

  gboolean result = FALSE;

#ifdef __APPLE__
  if (self->backend == GN_HW_KEYSTORE_SECURE_ENCLAVE) {
    CFStringRef label = CFStringCreateWithCString(NULL, MASTER_KEY_LABEL, kCFStringEncodingUTF8);
    result = delete_secure_enclave_key(label);
    CFRelease(label);

    if (self->enclave_key) {
      CFRelease(self->enclave_key);
      self->enclave_key = NULL;
    }
  }
#endif

#ifdef _WIN32
  if (self->backend == GN_HW_KEYSTORE_CNG) {
    result = cng_delete_master_key(self, error);
  }
#endif

  /* Always try to delete from software fallback too */
  software_delete_master_key();

  if (!result && self->using_fallback) {
    result = TRUE; /* Software deletion succeeded */
  }

  g_mutex_unlock(&self->lock);

  if (result) {
    g_signal_emit(self, signals[SIGNAL_MASTER_KEY_CHANGED], 0, FALSE);
  } else if (!error || !*error) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_FAILED,
                "Failed to delete master key");
  }

  return result;
}

gboolean
gn_hsm_provider_tpm_derive_signing_key(GnHsmProviderTpm *self,
                                        const gchar *npub,
                                        guint8 *private_key_out,
                                        GError **error)
{
  g_return_val_if_fail(GN_IS_HSM_PROVIDER_TPM(self), FALSE);
  g_return_val_if_fail(npub != NULL, FALSE);
  g_return_val_if_fail(private_key_out != NULL, FALSE);

  g_mutex_lock(&self->lock);

  /* Ensure master key is loaded */
  if (!self->master_key_cached) {
#ifdef __APPLE__
    if (self->backend == GN_HW_KEYSTORE_SECURE_ENCLAVE && !self->using_fallback) {
      if (!self->enclave_key) {
        CFStringRef label = CFStringCreateWithCString(NULL, MASTER_KEY_LABEL, kCFStringEncodingUTF8);
        self->enclave_key = load_secure_enclave_key(label);
        CFRelease(label);
      }
      if (self->enclave_key) {
        self->master_key = g_malloc(32);
        if (derive_master_secret_from_enclave(self->enclave_key, self->master_key, &self->master_key_len)) {
          self->master_key_cached = TRUE;
        } else {
          g_free(self->master_key);
          self->master_key = NULL;
        }
      }
    }
#endif

    if (!self->master_key_cached) {
      /* Try software fallback */
      self->master_key = g_malloc(32);
      if (software_load_master_key(self->master_key, &self->master_key_len)) {
        self->master_key_cached = TRUE;
      } else {
        g_free(self->master_key);
        self->master_key = NULL;
        g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_FOUND,
                    "Master key not found - create one first");
        g_mutex_unlock(&self->lock);
        return FALSE;
      }
    }
  }

  /* Derive signing key using HKDF:
   * IKM = master_key
   * salt = npub (as bytes)
   * info = KEY_DERIVATION_INFO
   * OKM = 32-byte private key
   */
  guint8 salt[32] = {0};
  gsize salt_len = strlen(npub);
  if (salt_len > 32) salt_len = 32;
  memcpy(salt, npub, salt_len);

  const guint8 *info = (const guint8 *)KEY_DERIVATION_INFO;
  gsize info_len = strlen(KEY_DERIVATION_INFO);

  gboolean result = hkdf_sha256(self->master_key, self->master_key_len,
                                 salt, sizeof(salt),
                                 info, info_len,
                                 private_key_out, 32);

  g_mutex_unlock(&self->lock);

  if (!result) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_FAILED,
                "Key derivation failed");
  }

  return result;
}

void
gn_hsm_provider_tpm_set_fallback_enabled(GnHsmProviderTpm *self, gboolean enabled)
{
  g_return_if_fail(GN_IS_HSM_PROVIDER_TPM(self));
  self->fallback_enabled = enabled;
}

gboolean
gn_hsm_provider_tpm_get_fallback_enabled(GnHsmProviderTpm *self)
{
  g_return_val_if_fail(GN_IS_HSM_PROVIDER_TPM(self), FALSE);
  return self->fallback_enabled;
}

gboolean
gn_hsm_provider_tpm_is_using_fallback(GnHsmProviderTpm *self)
{
  g_return_val_if_fail(GN_IS_HSM_PROVIDER_TPM(self), FALSE);
  return self->using_fallback;
}
