/* secret-storage.c - Secure key storage implementation for gnostr-signer
 *
 * Platform-independent secure storage using:
 *   - Linux: libsecret (Secret Service D-Bus API)
 *   - macOS: Security.framework Keychain
 *
 * SPDX-License-Identifier: MIT
 */
#include "secret-storage.h"
#include <nostr/nip19/nip19.h>
#include <keys.h>
#include <nostr-utils.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#ifdef GNOSTR_HAVE_LIBSECRET
#include <libsecret/secret.h>
#endif

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#define GNOSTR_HAVE_KEYCHAIN 1
#endif

/* Application identifier for stored keys */
#define GN_APP_NAME "gnostr-signer"
#define GN_KEY_TYPE "nostr"

/* Schema name for libsecret */
#define GN_SECRET_SCHEMA_NAME "org.gnostr.Signer/key"

/* Keychain service name for macOS */
#define GN_KEYCHAIN_SERVICE "Gnostr Signer Keys"

G_DEFINE_QUARK(gn-secret-storage-error-quark, gn_secret_storage_error)

/* Module state */
static gboolean g_initialized = FALSE;

#ifdef GNOSTR_HAVE_LIBSECRET
static SecretService *g_secret_service = NULL;

/* Secret schema definition */
static const SecretSchema GN_KEY_SCHEMA = {
  GN_SECRET_SCHEMA_NAME,
  SECRET_SCHEMA_NONE,
  {
    { "application",  SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "label",        SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "npub",         SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "key_type",     SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "created_at",   SECRET_SCHEMA_ATTRIBUTE_STRING },
    { NULL, 0 }
  }
};
#endif

/* Helper: Check if string is 64-char lowercase hex */
static gboolean
is_hex_64(const gchar *s)
{
  if (!s) return FALSE;
  gsize n = strlen(s);
  if (n != 64) return FALSE;
  for (gsize i = 0; i < n; i++) {
    gchar c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
      return FALSE;
  }
  return TRUE;
}

/* Helper: Convert binary to hex string */
static gchar *
bin_to_hex(const guint8 *buf, gsize len)
{
  static const gchar hexd[16] = "0123456789abcdef";
  gchar *out = g_malloc(len * 2 + 1);
  for (gsize i = 0; i < len; i++) {
    out[2 * i] = hexd[(buf[i] >> 4) & 0xF];
    out[2 * i + 1] = hexd[buf[i] & 0xF];
  }
  out[len * 2] = '\0';
  return out;
}

/* Helper: Get current ISO 8601 timestamp */
static gchar *
get_iso8601_timestamp(void)
{
  GDateTime *now = g_date_time_new_now_utc();
  gchar *timestamp = g_date_time_format_iso8601(now);
  g_date_time_unref(now);
  return timestamp;
}

/* Helper: Normalize key to hex, derive npub */
static gboolean
normalize_key_and_derive_npub(const gchar *input_key,
                              gchar **out_sk_hex,
                              gchar **out_npub,
                              GError **error)
{
  g_return_val_if_fail(input_key != NULL, FALSE);
  g_return_val_if_fail(out_sk_hex != NULL, FALSE);
  g_return_val_if_fail(out_npub != NULL, FALSE);

  *out_sk_hex = NULL;
  *out_npub = NULL;

  gchar *sk_hex = NULL;

  /* Normalize to hex */
  if (is_hex_64(input_key)) {
    sk_hex = g_ascii_strdown(input_key, -1);
  } else if (g_str_has_prefix(input_key, "nsec1")) {
    guint8 sk[32];
    if (nostr_nip19_decode_nsec(input_key, sk) != 0) {
      g_set_error(error,
                  GN_SECRET_STORAGE_ERROR,
                  GN_SECRET_STORAGE_ERROR_INVALID_DATA,
                  "Invalid nsec format");
      return FALSE;
    }
    sk_hex = bin_to_hex(sk, 32);
    memset(sk, 0, sizeof(sk));
  } else {
    g_set_error(error,
                GN_SECRET_STORAGE_ERROR,
                GN_SECRET_STORAGE_ERROR_INVALID_DATA,
                "Key must be nsec1 or 64-char hex");
    return FALSE;
  }

  /* Derive public key */
  gchar *pk_hex = nostr_key_get_public(sk_hex);
  if (!pk_hex) {
    memset(sk_hex, 0, strlen(sk_hex));
    g_free(sk_hex);
    g_set_error(error,
                GN_SECRET_STORAGE_ERROR,
                GN_SECRET_STORAGE_ERROR_FAILED,
                "Failed to derive public key");
    return FALSE;
  }

  /* Convert to npub */
  guint8 pk[32];
  if (!nostr_hex2bin(pk, pk_hex, 32)) {
    g_free(pk_hex);
    memset(sk_hex, 0, strlen(sk_hex));
    g_free(sk_hex);
    g_set_error(error,
                GN_SECRET_STORAGE_ERROR,
                GN_SECRET_STORAGE_ERROR_FAILED,
                "Invalid public key format");
    return FALSE;
  }
  g_free(pk_hex);

  gchar *npub = NULL;
  if (nostr_nip19_encode_npub(pk, &npub) != 0 || !npub) {
    memset(sk_hex, 0, strlen(sk_hex));
    g_free(sk_hex);
    g_set_error(error,
                GN_SECRET_STORAGE_ERROR,
                GN_SECRET_STORAGE_ERROR_FAILED,
                "Failed to encode npub");
    return FALSE;
  }

  *out_sk_hex = sk_hex;
  *out_npub = g_strdup(npub);
  free(npub);
  return TRUE;
}

void
gn_secret_storage_key_info_free(GnSecretStorageKeyInfo *info)
{
  if (!info) return;
  g_free(info->label);
  g_free(info->npub);
  g_free(info->key_type);
  g_free(info->created_at);
  g_free(info->application);
  g_free(info);
}

gboolean
gn_secret_storage_init(GError **error)
{
  if (g_initialized) {
    return TRUE;
  }

#ifdef GNOSTR_HAVE_LIBSECRET
  GError *local_error = NULL;
  g_secret_service = secret_service_get_sync(SECRET_SERVICE_OPEN_SESSION,
                                              NULL,
                                              &local_error);
  if (local_error) {
    g_propagate_prefixed_error(error, local_error,
                               "Failed to connect to secret service: ");
    return FALSE;
  }

  if (!g_secret_service) {
    g_set_error(error,
                GN_SECRET_STORAGE_ERROR,
                GN_SECRET_STORAGE_ERROR_NOT_AVAILABLE,
                "Secret service not available");
    return FALSE;
  }

  g_initialized = TRUE;
  return TRUE;

#elif defined(GNOSTR_HAVE_KEYCHAIN)
  /* macOS Keychain is always available */
  g_initialized = TRUE;
  return TRUE;

#else
  g_set_error(error,
              GN_SECRET_STORAGE_ERROR,
              GN_SECRET_STORAGE_ERROR_NOT_AVAILABLE,
              "No secret storage backend available");
  return FALSE;
#endif
}

void
gn_secret_storage_shutdown(void)
{
  if (!g_initialized) return;

#ifdef GNOSTR_HAVE_LIBSECRET
  if (g_secret_service) {
    g_object_unref(g_secret_service);
    g_secret_service = NULL;
  }
#endif

  g_initialized = FALSE;
}

gboolean
gn_secret_storage_is_available(void)
{
#ifdef GNOSTR_HAVE_LIBSECRET
  if (!g_initialized) {
    GError *err = NULL;
    SecretService *svc = secret_service_get_sync(SECRET_SERVICE_NONE, NULL, &err);
    if (err) {
      g_clear_error(&err);
      return FALSE;
    }
    if (svc) {
      g_object_unref(svc);
      return TRUE;
    }
    return FALSE;
  }
  return g_secret_service != NULL;

#elif defined(GNOSTR_HAVE_KEYCHAIN)
  return TRUE;

#else
  return FALSE;
#endif
}

const gchar *
gn_secret_storage_get_backend_name(void)
{
#ifdef GNOSTR_HAVE_LIBSECRET
  return "libsecret";
#elif defined(GNOSTR_HAVE_KEYCHAIN)
  return "Keychain";
#else
  return "none";
#endif
}

gboolean
gn_secret_storage_store_key(const gchar *label,
                            const gchar *nsec,
                            GError **error)
{
  g_return_val_if_fail(label != NULL && *label != '\0', FALSE);
  g_return_val_if_fail(nsec != NULL && *nsec != '\0', FALSE);

  if (!g_initialized) {
    g_set_error(error,
                GN_SECRET_STORAGE_ERROR,
                GN_SECRET_STORAGE_ERROR_NOT_AVAILABLE,
                "Secret storage not initialized");
    return FALSE;
  }

  /* Normalize key and derive npub */
  gchar *sk_hex = NULL;
  gchar *npub = NULL;
  if (!normalize_key_and_derive_npub(nsec, &sk_hex, &npub, error)) {
    return FALSE;
  }

  gchar *created_at = get_iso8601_timestamp();
  gboolean result = FALSE;

#ifdef GNOSTR_HAVE_LIBSECRET
  GError *local_error = NULL;

  /* Check if label already exists */
  gchar *existing = secret_password_lookup_sync(&GN_KEY_SCHEMA,
                                                 NULL,
                                                 &local_error,
                                                 "application", GN_APP_NAME,
                                                 "label", label,
                                                 NULL);
  if (existing) {
    secret_password_free(existing);
    memset(sk_hex, 0, strlen(sk_hex));
    g_free(sk_hex);
    g_free(npub);
    g_free(created_at);
    g_set_error(error,
                GN_SECRET_STORAGE_ERROR,
                GN_SECRET_STORAGE_ERROR_ALREADY_EXISTS,
                "Key with label '%s' already exists", label);
    return FALSE;
  }
  if (local_error) {
    g_clear_error(&local_error);
  }

  /* Store the key */
  gchar *display_name = g_strdup_printf("Nostr Key: %s", label);

  result = secret_password_store_sync(&GN_KEY_SCHEMA,
                                      SECRET_COLLECTION_DEFAULT,
                                      display_name,
                                      sk_hex,
                                      NULL,
                                      &local_error,
                                      "application", GN_APP_NAME,
                                      "label", label,
                                      "npub", npub,
                                      "key_type", GN_KEY_TYPE,
                                      "created_at", created_at,
                                      NULL);

  g_free(display_name);

  if (local_error) {
    g_propagate_prefixed_error(error, local_error,
                               "Failed to store key: ");
    result = FALSE;
  }

#elif defined(GNOSTR_HAVE_KEYCHAIN)
  /* macOS Keychain implementation */
  guint8 skb[32];
  if (!nostr_hex2bin(skb, sk_hex, 32)) {
    memset(sk_hex, 0, strlen(sk_hex));
    g_free(sk_hex);
    g_free(npub);
    g_free(created_at);
    g_set_error(error,
                GN_SECRET_STORAGE_ERROR,
                GN_SECRET_STORAGE_ERROR_INVALID_DATA,
                "Invalid hex key format");
    return FALSE;
  }

  CFMutableDictionaryRef query = CFDictionaryCreateMutable(
    kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks,
    &kCFTypeDictionaryValueCallBacks);

  CFStringRef serviceCF = CFStringCreateWithCString(NULL, GN_KEYCHAIN_SERVICE, kCFStringEncodingUTF8);
  CFStringRef accountCF = CFStringCreateWithCString(NULL, label, kCFStringEncodingUTF8);
  CFStringRef labelCF = CFStringCreateWithCString(NULL, label, kCFStringEncodingUTF8);
  CFDataRef secretData = CFDataCreate(NULL, skb, 32);

  memset(skb, 0, sizeof(skb));

  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, serviceCF);
  CFDictionarySetValue(query, kSecAttrAccount, accountCF);

  /* Check if already exists */
  CFDictionarySetValue(query, kSecReturnData, kCFBooleanFalse);
  OSStatus checkStatus = SecItemCopyMatching(query, NULL);
  if (checkStatus == errSecSuccess) {
    CFRelease(serviceCF);
    CFRelease(accountCF);
    CFRelease(labelCF);
    CFRelease(secretData);
    CFRelease(query);
    memset(sk_hex, 0, strlen(sk_hex));
    g_free(sk_hex);
    g_free(npub);
    g_free(created_at);
    g_set_error(error,
                GN_SECRET_STORAGE_ERROR,
                GN_SECRET_STORAGE_ERROR_ALREADY_EXISTS,
                "Key with label '%s' already exists", label);
    return FALSE;
  }

  /* Remove the return data flag and set the actual data */
  CFDictionaryRemoveValue(query, kSecReturnData);
  CFDictionarySetValue(query, kSecAttrLabel, labelCF);
  CFDictionarySetValue(query, kSecValueData, secretData);
  CFDictionarySetValue(query, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlock);

  /* Store comment with npub and creation time */
  gchar *comment = g_strdup_printf("npub:%s;created:%s;type:%s", npub, created_at, GN_KEY_TYPE);
  CFStringRef commentCF = CFStringCreateWithCString(NULL, comment, kCFStringEncodingUTF8);
  CFDictionarySetValue(query, kSecAttrComment, commentCF);
  g_free(comment);

  OSStatus status = SecItemAdd(query, NULL);

  CFRelease(serviceCF);
  CFRelease(accountCF);
  CFRelease(labelCF);
  CFRelease(secretData);
  CFRelease(commentCF);
  CFRelease(query);

  if (status == errSecSuccess) {
    result = TRUE;
  } else if (status == errSecDuplicateItem) {
    g_set_error(error,
                GN_SECRET_STORAGE_ERROR,
                GN_SECRET_STORAGE_ERROR_ALREADY_EXISTS,
                "Key with label '%s' already exists", label);
  } else {
    g_set_error(error,
                GN_SECRET_STORAGE_ERROR,
                GN_SECRET_STORAGE_ERROR_FAILED,
                "Keychain error: %d", (int)status);
  }

#else
  g_set_error(error,
              GN_SECRET_STORAGE_ERROR,
              GN_SECRET_STORAGE_ERROR_NOT_AVAILABLE,
              "No secret storage backend available");
#endif

  /* Securely clear the secret key */
  memset(sk_hex, 0, strlen(sk_hex));
  g_free(sk_hex);
  g_free(npub);
  g_free(created_at);

  return result;
}

gchar *
gn_secret_storage_retrieve_key(const gchar *label, GError **error)
{
  g_return_val_if_fail(label != NULL && *label != '\0', NULL);

  if (!g_initialized) {
    g_set_error(error,
                GN_SECRET_STORAGE_ERROR,
                GN_SECRET_STORAGE_ERROR_NOT_AVAILABLE,
                "Secret storage not initialized");
    return NULL;
  }

  gchar *nsec = NULL;

#ifdef GNOSTR_HAVE_LIBSECRET
  GError *local_error = NULL;

  gchar *secret = secret_password_lookup_sync(&GN_KEY_SCHEMA,
                                               NULL,
                                               &local_error,
                                               "application", GN_APP_NAME,
                                               "label", label,
                                               NULL);

  if (local_error) {
    g_propagate_prefixed_error(error, local_error,
                               "Failed to retrieve key: ");
    return NULL;
  }

  if (!secret) {
    g_set_error(error,
                GN_SECRET_STORAGE_ERROR,
                GN_SECRET_STORAGE_ERROR_NOT_FOUND,
                "Key with label '%s' not found", label);
    return NULL;
  }

  /* Convert hex to nsec */
  if (is_hex_64(secret)) {
    guint8 sk[32];
    if (nostr_hex2bin(sk, secret, 32)) {
      gchar *encoded = NULL;
      if (nostr_nip19_encode_nsec(sk, &encoded) == 0 && encoded) {
        nsec = g_strdup(encoded);
        free(encoded);
      }
      memset(sk, 0, sizeof(sk));
    }
  }

  secret_password_free(secret);

  if (!nsec) {
    g_set_error(error,
                GN_SECRET_STORAGE_ERROR,
                GN_SECRET_STORAGE_ERROR_FAILED,
                "Failed to convert stored key to nsec format");
    return NULL;
  }

#elif defined(GNOSTR_HAVE_KEYCHAIN)
  CFMutableDictionaryRef query = CFDictionaryCreateMutable(
    kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks,
    &kCFTypeDictionaryValueCallBacks);

  CFStringRef serviceCF = CFStringCreateWithCString(NULL, GN_KEYCHAIN_SERVICE, kCFStringEncodingUTF8);
  CFStringRef accountCF = CFStringCreateWithCString(NULL, label, kCFStringEncodingUTF8);

  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, serviceCF);
  CFDictionarySetValue(query, kSecAttrAccount, accountCF);
  CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
  CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);

  CFTypeRef result = NULL;
  OSStatus status = SecItemCopyMatching(query, &result);

  CFRelease(serviceCF);
  CFRelease(accountCF);
  CFRelease(query);

  if (status == errSecItemNotFound) {
    g_set_error(error,
                GN_SECRET_STORAGE_ERROR,
                GN_SECRET_STORAGE_ERROR_NOT_FOUND,
                "Key with label '%s' not found", label);
    return NULL;
  }

  if (status != errSecSuccess) {
    g_set_error(error,
                GN_SECRET_STORAGE_ERROR,
                GN_SECRET_STORAGE_ERROR_FAILED,
                "Keychain error: %d", (int)status);
    return NULL;
  }

  if (result) {
    CFDataRef data = (CFDataRef)result;
    CFIndex len = CFDataGetLength(data);
    const UInt8 *bytes = CFDataGetBytePtr(data);

    if (len == 32 && bytes) {
      gchar *encoded = NULL;
      if (nostr_nip19_encode_nsec(bytes, &encoded) == 0 && encoded) {
        nsec = g_strdup(encoded);
        free(encoded);
      }
    }
    CFRelease(result);
  }

  if (!nsec) {
    g_set_error(error,
                GN_SECRET_STORAGE_ERROR,
                GN_SECRET_STORAGE_ERROR_FAILED,
                "Failed to retrieve key data");
    return NULL;
  }

#else
  g_set_error(error,
              GN_SECRET_STORAGE_ERROR,
              GN_SECRET_STORAGE_ERROR_NOT_AVAILABLE,
              "No secret storage backend available");
#endif

  return nsec;
}

gboolean
gn_secret_storage_delete_key(const gchar *label, GError **error)
{
  g_return_val_if_fail(label != NULL && *label != '\0', FALSE);

  if (!g_initialized) {
    g_set_error(error,
                GN_SECRET_STORAGE_ERROR,
                GN_SECRET_STORAGE_ERROR_NOT_AVAILABLE,
                "Secret storage not initialized");
    return FALSE;
  }

#ifdef GNOSTR_HAVE_LIBSECRET
  GError *local_error = NULL;

  gboolean result = secret_password_clear_sync(&GN_KEY_SCHEMA,
                                                NULL,
                                                &local_error,
                                                "application", GN_APP_NAME,
                                                "label", label,
                                                NULL);

  if (local_error) {
    g_propagate_prefixed_error(error, local_error,
                               "Failed to delete key: ");
    return FALSE;
  }

  if (!result) {
    g_set_error(error,
                GN_SECRET_STORAGE_ERROR,
                GN_SECRET_STORAGE_ERROR_NOT_FOUND,
                "Key with label '%s' not found", label);
    return FALSE;
  }

  return TRUE;

#elif defined(GNOSTR_HAVE_KEYCHAIN)
  CFMutableDictionaryRef query = CFDictionaryCreateMutable(
    kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks,
    &kCFTypeDictionaryValueCallBacks);

  CFStringRef serviceCF = CFStringCreateWithCString(NULL, GN_KEYCHAIN_SERVICE, kCFStringEncodingUTF8);
  CFStringRef accountCF = CFStringCreateWithCString(NULL, label, kCFStringEncodingUTF8);

  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, serviceCF);
  CFDictionarySetValue(query, kSecAttrAccount, accountCF);

  OSStatus status = SecItemDelete(query);

  CFRelease(serviceCF);
  CFRelease(accountCF);
  CFRelease(query);

  if (status == errSecItemNotFound) {
    g_set_error(error,
                GN_SECRET_STORAGE_ERROR,
                GN_SECRET_STORAGE_ERROR_NOT_FOUND,
                "Key with label '%s' not found", label);
    return FALSE;
  }

  if (status != errSecSuccess) {
    g_set_error(error,
                GN_SECRET_STORAGE_ERROR,
                GN_SECRET_STORAGE_ERROR_FAILED,
                "Keychain error: %d", (int)status);
    return FALSE;
  }

  return TRUE;

#else
  g_set_error(error,
              GN_SECRET_STORAGE_ERROR,
              GN_SECRET_STORAGE_ERROR_NOT_AVAILABLE,
              "No secret storage backend available");
  return FALSE;
#endif
}

GPtrArray *
gn_secret_storage_list_keys(GError **error)
{
  GPtrArray *arr = g_ptr_array_new_with_free_func(
    (GDestroyNotify)gn_secret_storage_key_info_free);

  if (!g_initialized) {
    g_set_error(error,
                GN_SECRET_STORAGE_ERROR,
                GN_SECRET_STORAGE_ERROR_NOT_AVAILABLE,
                "Secret storage not initialized");
    return arr;
  }

#ifdef GNOSTR_HAVE_LIBSECRET
  GError *local_error = NULL;

  GHashTable *attrs = g_hash_table_new(g_str_hash, g_str_equal);
  g_hash_table_insert(attrs, (gpointer)"application", (gpointer)GN_APP_NAME);

  GList *items = secret_service_search_sync(g_secret_service,
                                            &GN_KEY_SCHEMA,
                                            attrs,
                                            SECRET_SEARCH_ALL | SECRET_SEARCH_UNLOCK,
                                            NULL,
                                            &local_error);
  g_hash_table_unref(attrs);

  if (local_error) {
    g_propagate_prefixed_error(error, local_error,
                               "Failed to list keys: ");
    return arr;
  }

  for (GList *it = items; it != NULL; it = it->next) {
    SecretItem *item = SECRET_ITEM(it->data);
    GHashTable *item_attrs = secret_item_get_attributes(item);

    GnSecretStorageKeyInfo *info = g_new0(GnSecretStorageKeyInfo, 1);

    const gchar *label_val = g_hash_table_lookup(item_attrs, "label");
    const gchar *npub_val = g_hash_table_lookup(item_attrs, "npub");
    const gchar *key_type_val = g_hash_table_lookup(item_attrs, "key_type");
    const gchar *created_at_val = g_hash_table_lookup(item_attrs, "created_at");
    const gchar *app_val = g_hash_table_lookup(item_attrs, "application");

    info->label = label_val ? g_strdup(label_val) : NULL;
    info->npub = npub_val ? g_strdup(npub_val) : NULL;
    info->key_type = key_type_val ? g_strdup(key_type_val) : g_strdup(GN_KEY_TYPE);
    info->created_at = created_at_val ? g_strdup(created_at_val) : NULL;
    info->application = app_val ? g_strdup(app_val) : g_strdup(GN_APP_NAME);

    g_hash_table_unref(item_attrs);
    g_ptr_array_add(arr, info);
  }

  g_list_free_full(items, g_object_unref);

#elif defined(GNOSTR_HAVE_KEYCHAIN)
  CFMutableDictionaryRef query = CFDictionaryCreateMutable(
    kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks,
    &kCFTypeDictionaryValueCallBacks);

  CFStringRef serviceCF = CFStringCreateWithCString(NULL, GN_KEYCHAIN_SERVICE, kCFStringEncodingUTF8);

  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, serviceCF);
  CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitAll);
  CFDictionarySetValue(query, kSecReturnAttributes, kCFBooleanTrue);

  CFTypeRef result = NULL;
  OSStatus status = SecItemCopyMatching(query, &result);

  CFRelease(serviceCF);
  CFRelease(query);

  if (status == errSecSuccess && result) {
    CFArrayRef items = (CFArrayRef)result;
    CFIndex count = CFArrayGetCount(items);

    for (CFIndex i = 0; i < count; i++) {
      CFDictionaryRef item = CFArrayGetValueAtIndex(items, i);
      GnSecretStorageKeyInfo *info = g_new0(GnSecretStorageKeyInfo, 1);

      /* Get account (label) */
      CFStringRef account = CFDictionaryGetValue(item, kSecAttrAccount);
      if (account) {
        CFIndex len = CFStringGetLength(account);
        CFIndex maxSize = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
        gchar *buf = g_malloc(maxSize);
        if (CFStringGetCString(account, buf, maxSize, kCFStringEncodingUTF8)) {
          info->label = buf;
        } else {
          g_free(buf);
        }
      }

      /* Get comment (contains npub and created_at) */
      CFStringRef commentRef = CFDictionaryGetValue(item, kSecAttrComment);
      if (commentRef) {
        CFIndex len = CFStringGetLength(commentRef);
        CFIndex maxSize = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
        gchar *comment = g_malloc(maxSize);
        if (CFStringGetCString(commentRef, comment, maxSize, kCFStringEncodingUTF8)) {
          /* Parse comment: "npub:xxx;created:yyy;type:zzz" */
          gchar **parts = g_strsplit(comment, ";", -1);
          for (int j = 0; parts && parts[j]; j++) {
            if (g_str_has_prefix(parts[j], "npub:")) {
              info->npub = g_strdup(parts[j] + 5);
            } else if (g_str_has_prefix(parts[j], "created:")) {
              info->created_at = g_strdup(parts[j] + 8);
            } else if (g_str_has_prefix(parts[j], "type:")) {
              info->key_type = g_strdup(parts[j] + 5);
            }
          }
          g_strfreev(parts);
        }
        g_free(comment);
      }

      /* Set defaults */
      if (!info->key_type) info->key_type = g_strdup(GN_KEY_TYPE);
      info->application = g_strdup(GN_APP_NAME);

      g_ptr_array_add(arr, info);
    }
    CFRelease(result);
  } else if (status != errSecItemNotFound) {
    g_set_error(error,
                GN_SECRET_STORAGE_ERROR,
                GN_SECRET_STORAGE_ERROR_FAILED,
                "Keychain error: %d", (int)status);
  }

#else
  g_set_error(error,
              GN_SECRET_STORAGE_ERROR,
              GN_SECRET_STORAGE_ERROR_NOT_AVAILABLE,
              "No secret storage backend available");
#endif

  return arr;
}

gboolean
gn_secret_storage_key_exists(const gchar *label)
{
  if (!label || !*label || !g_initialized) {
    return FALSE;
  }

#ifdef GNOSTR_HAVE_LIBSECRET
  GError *err = NULL;
  gchar *secret = secret_password_lookup_sync(&GN_KEY_SCHEMA,
                                               NULL,
                                               &err,
                                               "application", GN_APP_NAME,
                                               "label", label,
                                               NULL);
  if (err) {
    g_clear_error(&err);
    return FALSE;
  }
  if (secret) {
    secret_password_free(secret);
    return TRUE;
  }
  return FALSE;

#elif defined(GNOSTR_HAVE_KEYCHAIN)
  CFMutableDictionaryRef query = CFDictionaryCreateMutable(
    kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks,
    &kCFTypeDictionaryValueCallBacks);

  CFStringRef serviceCF = CFStringCreateWithCString(NULL, GN_KEYCHAIN_SERVICE, kCFStringEncodingUTF8);
  CFStringRef accountCF = CFStringCreateWithCString(NULL, label, kCFStringEncodingUTF8);

  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, serviceCF);
  CFDictionarySetValue(query, kSecAttrAccount, accountCF);
  CFDictionarySetValue(query, kSecReturnData, kCFBooleanFalse);

  OSStatus status = SecItemCopyMatching(query, NULL);

  CFRelease(serviceCF);
  CFRelease(accountCF);
  CFRelease(query);

  return (status == errSecSuccess);

#else
  return FALSE;
#endif
}

GnSecretStorageKeyInfo *
gn_secret_storage_get_key_info(const gchar *label, GError **error)
{
  g_return_val_if_fail(label != NULL && *label != '\0', NULL);

  if (!g_initialized) {
    g_set_error(error,
                GN_SECRET_STORAGE_ERROR,
                GN_SECRET_STORAGE_ERROR_NOT_AVAILABLE,
                "Secret storage not initialized");
    return NULL;
  }

#ifdef GNOSTR_HAVE_LIBSECRET
  GError *local_error = NULL;

  GHashTable *attrs = g_hash_table_new(g_str_hash, g_str_equal);
  g_hash_table_insert(attrs, (gpointer)"application", (gpointer)GN_APP_NAME);
  g_hash_table_insert(attrs, (gpointer)"label", (gpointer)label);

  GList *items = secret_service_search_sync(g_secret_service,
                                            &GN_KEY_SCHEMA,
                                            attrs,
                                            SECRET_SEARCH_UNLOCK,
                                            NULL,
                                            &local_error);
  g_hash_table_unref(attrs);

  if (local_error) {
    g_propagate_prefixed_error(error, local_error,
                               "Failed to get key info: ");
    return NULL;
  }

  if (!items) {
    g_set_error(error,
                GN_SECRET_STORAGE_ERROR,
                GN_SECRET_STORAGE_ERROR_NOT_FOUND,
                "Key with label '%s' not found", label);
    return NULL;
  }

  SecretItem *item = SECRET_ITEM(items->data);
  GHashTable *item_attrs = secret_item_get_attributes(item);

  GnSecretStorageKeyInfo *info = g_new0(GnSecretStorageKeyInfo, 1);

  const gchar *label_val = g_hash_table_lookup(item_attrs, "label");
  const gchar *npub_val = g_hash_table_lookup(item_attrs, "npub");
  const gchar *key_type_val = g_hash_table_lookup(item_attrs, "key_type");
  const gchar *created_at_val = g_hash_table_lookup(item_attrs, "created_at");
  const gchar *app_val = g_hash_table_lookup(item_attrs, "application");

  info->label = label_val ? g_strdup(label_val) : g_strdup(label);
  info->npub = npub_val ? g_strdup(npub_val) : NULL;
  info->key_type = key_type_val ? g_strdup(key_type_val) : g_strdup(GN_KEY_TYPE);
  info->created_at = created_at_val ? g_strdup(created_at_val) : NULL;
  info->application = app_val ? g_strdup(app_val) : g_strdup(GN_APP_NAME);

  g_hash_table_unref(item_attrs);
  g_list_free_full(items, g_object_unref);

  return info;

#elif defined(GNOSTR_HAVE_KEYCHAIN)
  CFMutableDictionaryRef query = CFDictionaryCreateMutable(
    kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks,
    &kCFTypeDictionaryValueCallBacks);

  CFStringRef serviceCF = CFStringCreateWithCString(NULL, GN_KEYCHAIN_SERVICE, kCFStringEncodingUTF8);
  CFStringRef accountCF = CFStringCreateWithCString(NULL, label, kCFStringEncodingUTF8);

  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, serviceCF);
  CFDictionarySetValue(query, kSecAttrAccount, accountCF);
  CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
  CFDictionarySetValue(query, kSecReturnAttributes, kCFBooleanTrue);

  CFTypeRef result = NULL;
  OSStatus status = SecItemCopyMatching(query, &result);

  CFRelease(serviceCF);
  CFRelease(accountCF);
  CFRelease(query);

  if (status == errSecItemNotFound) {
    g_set_error(error,
                GN_SECRET_STORAGE_ERROR,
                GN_SECRET_STORAGE_ERROR_NOT_FOUND,
                "Key with label '%s' not found", label);
    return NULL;
  }

  if (status != errSecSuccess) {
    g_set_error(error,
                GN_SECRET_STORAGE_ERROR,
                GN_SECRET_STORAGE_ERROR_FAILED,
                "Keychain error: %d", (int)status);
    return NULL;
  }

  GnSecretStorageKeyInfo *info = g_new0(GnSecretStorageKeyInfo, 1);
  info->label = g_strdup(label);
  info->application = g_strdup(GN_APP_NAME);
  info->key_type = g_strdup(GN_KEY_TYPE);

  if (result) {
    CFDictionaryRef item = (CFDictionaryRef)result;

    /* Get comment (contains npub and created_at) */
    CFStringRef commentRef = CFDictionaryGetValue(item, kSecAttrComment);
    if (commentRef) {
      CFIndex len = CFStringGetLength(commentRef);
      CFIndex maxSize = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
      gchar *comment = g_malloc(maxSize);
      if (CFStringGetCString(commentRef, comment, maxSize, kCFStringEncodingUTF8)) {
        /* Parse comment: "npub:xxx;created:yyy;type:zzz" */
        gchar **parts = g_strsplit(comment, ";", -1);
        for (int j = 0; parts && parts[j]; j++) {
          if (g_str_has_prefix(parts[j], "npub:")) {
            info->npub = g_strdup(parts[j] + 5);
          } else if (g_str_has_prefix(parts[j], "created:")) {
            info->created_at = g_strdup(parts[j] + 8);
          } else if (g_str_has_prefix(parts[j], "type:")) {
            g_free(info->key_type);
            info->key_type = g_strdup(parts[j] + 5);
          }
        }
        g_strfreev(parts);
      }
      g_free(comment);
    }
    CFRelease(result);
  }

  return info;

#else
  g_set_error(error,
              GN_SECRET_STORAGE_ERROR,
              GN_SECRET_STORAGE_ERROR_NOT_AVAILABLE,
              "No secret storage backend available");
  return NULL;
#endif
}
