#ifdef __APPLE__
#include "gh-nip46-credentials-private.h"
#include <Security/Security.h>
#include <Security/Authorization.h>
#include <sodium.h>

GBytes *gh_nip46_credentials_secret_bytes_new(const void *data, gsize len);

typedef struct {
  GhNip46CredentialBackend base;
  SecKeychainRef keychain; /* NULL means the user's normal keychain */
} KeychainBackend;

static CFStringRef
cf_string(const gchar *value)
{
  return value ? CFStringCreateWithCString(NULL, value, kCFStringEncodingUTF8) : NULL;
}
static gchar *
utf8(CFStringRef value)
{
  if (!value) return NULL;
  CFIndex size = CFStringGetMaximumSizeForEncoding(CFStringGetLength(value), kCFStringEncodingUTF8) + 1;
  gchar *text = g_malloc((gsize)size);
  if (!CFStringGetCString(value, text, size, kCFStringEncodingUTF8)) { g_free(text); return NULL; }
  return text;
}

static CFMutableDictionaryRef
query(KeychainBackend *self, const gchar *account, gboolean add, gboolean interactive)
{
  CFMutableDictionaryRef q = CFDictionaryCreateMutable(NULL, 0,
    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(q, kSecAttrSynchronizable, kCFBooleanFalse);
  CFStringRef service = cf_string(GH_NIP46_CREDENTIAL_SCHEMA);
  CFDictionarySetValue(q, kSecAttrService, service);
  CFRelease(service);
  if (account) {
    CFStringRef acct = cf_string(account);
    CFDictionarySetValue(q, kSecAttrAccount, acct);
    CFRelease(acct);
  }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  if (self->keychain) {
    if (add) CFDictionarySetValue(q, kSecUseKeychain, self->keychain);
    else {
      CFArrayRef list = CFArrayCreate(NULL, (const void *[]){self->keychain}, 1,
                                       &kCFTypeArrayCallBacks);
      CFDictionarySetValue(q, kSecMatchSearchList, list);
      CFRelease(list);
    }
  }
#pragma clang diagnostic pop
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  if (!interactive) CFDictionarySetValue(q, kSecUseAuthenticationUI, kSecUseAuthenticationUIFail);
#pragma clang diagnostic pop
  return q;
}

static GError *
status_error(OSStatus status)
{
  GhNip46CredentialError code = status == errSecNotAvailable ? GH_NIP46_CREDENTIAL_ERROR_UNAVAILABLE :
    status == errSecInteractionNotAllowed || status == errSecAuthFailed ||
    status == errSecUserCanceled || status == errAuthorizationCanceled ?
      GH_NIP46_CREDENTIAL_ERROR_LOCKED : GH_NIP46_CREDENTIAL_ERROR_FAILED;
  return g_error_new_literal(GH_NIP46_CREDENTIAL_ERROR, code,
    code == GH_NIP46_CREDENTIAL_ERROR_LOCKED ? "The keyring is locked" :
    code == GH_NIP46_CREDENTIAL_ERROR_UNAVAILABLE ? "Keychain unavailable" :
    "Keychain operation failed");
}

static gboolean
keychain_locked(KeychainBackend *self, gboolean *locked, GError **error)
{
  SecKeychainRef keychain = self->keychain;
  if (!keychain) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    OSStatus status = SecKeychainCopyDefault(&keychain);
#pragma clang diagnostic pop
    if (status != errSecSuccess) { *error = status_error(status); return FALSE; }
  }
  SecKeychainStatus state = 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  OSStatus status = SecKeychainGetStatus(keychain, &state);
#pragma clang diagnostic pop
  if (!self->keychain) CFRelease(keychain);
  if (status != errSecSuccess) { *error = status_error(status); return FALSE; }
  *locked = (state & kSecUnlockStateStatus) == 0;
  return TRUE;
}

static GPtrArray *
kc_search(GhNip46CredentialBackend *backend, const gchar *account,
          gboolean interactive, GCancellable *cancellable, GError **error)
{
  (void)cancellable;
  KeychainBackend *self = (KeychainBackend *)backend;
  gboolean locked = FALSE;
  if (!interactive && !keychain_locked(self, &locked, error)) return NULL;
  if (locked && account) {
    *error = status_error(errSecInteractionNotAllowed);
    return NULL;
  }
  CFMutableDictionaryRef q = query(self, account, FALSE, interactive);
  CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitAll);
  CFDictionarySetValue(q, kSecReturnAttributes, kCFBooleanTrue);
  CFTypeRef result = NULL;
  OSStatus status = SecItemCopyMatching(q, &result);
  CFRelease(q);
  GPtrArray *items = g_ptr_array_new_with_free_func((GDestroyNotify)gh_nip46_credential_item_free);
  if (status == errSecItemNotFound) return items;
  if (status != errSecSuccess) {
    g_ptr_array_unref(items);
    if (result) CFRelease(result);
    *error = status_error(status);
    return NULL;
  }
  CFArrayRef array = (CFArrayRef)result;
  for (CFIndex i = 0; i < CFArrayGetCount(array); i++) {
    CFDictionaryRef raw = CFArrayGetValueAtIndex(array, i);
    GhNip46CredentialItem *item = g_new0(GhNip46CredentialItem, 1);
    item->account = utf8(CFDictionaryGetValue(raw, kSecAttrAccount));
    item->version = utf8(CFDictionaryGetValue(raw, kSecAttrComment));
    item->label = utf8(CFDictionaryGetValue(raw, kSecAttrLabel));
    item->attributes_valid = TRUE;
    if (locked) {
      item->locked = TRUE;
      g_ptr_array_add(items, item);
      continue;
    }
    CFMutableDictionaryRef data_q = query(self, item->account, FALSE, interactive);
    CFDictionarySetValue(data_q, kSecMatchLimit, kSecMatchLimitOne);
    CFDictionarySetValue(data_q, kSecReturnData, kCFBooleanTrue);
    CFTypeRef data = NULL;
    OSStatus data_status = SecItemCopyMatching(data_q, &data);
    CFRelease(data_q);
    if (data_status == errSecSuccess && data) {
      CFDataRef bytes = (CFDataRef)data;
      CFIndex len = CFDataGetLength(bytes);
      if (len >= 0 && len <= 8192)
        item->secret = gh_nip46_credentials_secret_bytes_new(CFDataGetBytePtr(bytes), (gsize)len);
      if (len > 0) sodium_memzero((void *)CFDataGetBytePtr(bytes), (size_t)len);
      CFRelease(data);
    } else if (data_status == errSecInteractionNotAllowed || data_status == errSecAuthFailed ||
               data_status == errSecUserCanceled || data_status == errAuthorizationCanceled) {
      item->locked = TRUE;
    } else if (data_status != errSecItemNotFound) {
      gh_nip46_credential_item_free(item);
      CFRelease(array);
      g_ptr_array_unref(items);
      *error = status_error(data_status);
      return NULL;
    }
    g_ptr_array_add(items, item);
  }
  CFRelease(array);
  return items;
}

static gboolean
kc_write(GhNip46CredentialBackend *backend, const gchar *account, GBytes *secret,
         gboolean interactive, GError **error)
{
  KeychainBackend *self = (KeychainBackend *)backend;
  gboolean locked = FALSE;
  if (!interactive && (!keychain_locked(self, &locked, error) || locked)) {
    if (locked) *error = status_error(errSecInteractionNotAllowed);
    return FALSE;
  }
  CFMutableDictionaryRef q = query(self, account, TRUE, interactive);
  CFStringRef label = cf_string(GH_NIP46_CREDENTIAL_LABEL);
  CFStringRef version = cf_string(GH_NIP46_CREDENTIAL_VERSION);
  CFDictionarySetValue(q, kSecAttrLabel, label);
  CFDictionarySetValue(q, kSecAttrComment, version);
  CFDictionarySetValue(q, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlock);
  gsize len = 0;
  const guint8 *raw = g_bytes_get_data(secret, &len);
  CFDataRef data = CFDataCreate(NULL, raw, (CFIndex)len);
  CFDictionarySetValue(q, kSecValueData, data);
  OSStatus status = SecItemAdd(q, NULL);
  if (status == errSecDuplicateItem) {
    /* SecItemUpdate changes the value in place; no delete/create gap. */
    CFMutableDictionaryRef match = query(self, account, FALSE, interactive);
    CFMutableDictionaryRef update = CFDictionaryCreateMutable(NULL, 0,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(update, kSecValueData, data);
    CFDictionarySetValue(update, kSecAttrComment, version);
    CFDictionarySetValue(update, kSecAttrLabel, label);
    status = SecItemUpdate(match, update);
    CFRelease(update);
    CFRelease(match);
  }
  if (len) sodium_memzero((void *)CFDataGetBytePtr(data), len);
  CFRelease(data); CFRelease(version); CFRelease(label); CFRelease(q);
  if (status != errSecSuccess) { *error = status_error(status); return FALSE; }
  return TRUE;
}

static gboolean
kc_remove(GhNip46CredentialBackend *backend, const gchar *account,
          gboolean interactive, GError **error)
{
  KeychainBackend *self = (KeychainBackend *)backend;
  gboolean locked = FALSE;
  if (!interactive && (!keychain_locked(self, &locked, error) || locked)) {
    if (locked) *error = status_error(errSecInteractionNotAllowed);
    return FALSE;
  }
  CFMutableDictionaryRef q = query(self, account, FALSE, interactive);
  OSStatus status = SecItemDelete(q);
  CFRelease(q);
  if (status != errSecSuccess && status != errSecItemNotFound) {
    *error = status_error(status); return FALSE;
  }
  return TRUE;
}

static void
kc_free(GhNip46CredentialBackend *backend)
{
  KeychainBackend *self = (KeychainBackend *)backend;
  if (self->keychain) CFRelease(self->keychain);
  g_free(self);
}

GhNip46CredentialBackend *
gh_nip46_credentials_keychain_new(SecKeychainRef keychain)
{
  KeychainBackend *self = g_new0(KeychainBackend, 1);
  self->base.search = kc_search;
  self->base.write = kc_write;
  self->base.remove = kc_remove;
  self->base.free = kc_free;
  self->keychain = keychain ? (SecKeychainRef)CFRetain(keychain) : NULL;
  return &self->base;
}
#endif
