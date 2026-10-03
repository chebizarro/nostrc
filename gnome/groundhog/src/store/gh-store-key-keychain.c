/* gh-store-key-keychain.c — macOS Keychain backend for GhStoreKeyBackend.
 *
 * Maps Groundhog's schema attributes to Keychain item fields:
 *   kSecAttrService     ← GH_STORE_KEY_SCHEMA_NAME
 *   kSecAttrAccount     ← "account" attribute (64-hex pubkey)
 *   kSecAttrDescription ← "store-id" attribute (UUID)
 *   kSecAttrComment     ← "version" attribute
 *   kSecAttrLabel       ← item label
 *   kSecValueData       ← 32-byte secret key
 *   kSecAttrAccessible  ← kSecAttrAccessibleAfterFirstUnlock
 *                         (chosen over the more restrictive
 *                          kSecAttrAccessibleWhenUnlocked so that the
 *                          background daemon can access keys after the
 *                          first login without requiring the screen to
 *                          be unlocked)
 *
 * All operations are synchronous SecItem calls wrapped in GTask so the
 * GhStoreKey policy layer sees the same async API as with libsecret.
 */

#ifdef __APPLE__

#include "gh-store-key-keychain.h"
#include <Security/Security.h>
#include <sodium.h>

struct _GhStoreKeyKeychain {
  GObject parent_instance;
  SecKeychainRef keychain; /* NULL = default; test-only: a temporary keychain */
};

static void keychain_backend_init(GhStoreKeyBackendInterface *iface);
G_DEFINE_TYPE_WITH_CODE(GhStoreKeyKeychain, gh_store_key_keychain, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GH_TYPE_STORE_KEY_BACKEND, keychain_backend_init))

static void
gh_store_key_keychain_dispose(GObject *object)
{
  GhStoreKeyKeychain *self = GH_STORE_KEY_KEYCHAIN(object);
  if (self->keychain) { CFRelease(self->keychain); self->keychain = NULL; }
  G_OBJECT_CLASS(gh_store_key_keychain_parent_class)->dispose(object);
}

static void
gh_store_key_keychain_class_init(GhStoreKeyKeychainClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gh_store_key_keychain_dispose;
}

static void
gh_store_key_keychain_init(GhStoreKeyKeychain *self) { self->keychain = NULL; }

/* ---- public API ------------------------------------------------------- */

void
gh_store_key_keychain_set_keychain(GhStoreKeyKeychain *self,
                                   SecKeychainRef      keychain)
{
  g_return_if_fail(GH_IS_STORE_KEY_KEYCHAIN(self));
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  if (self->keychain) CFRelease(self->keychain);
  self->keychain = keychain ? (SecKeychainRef)CFRetain(keychain) : NULL;
#pragma clang diagnostic pop
}

/* Helper: copy a CFString to a g_strdup'd C string, or NULL. */
static gchar *
cfstring_to_gchar(CFStringRef s)
{
  if (!s) return NULL;
  CFIndex len = CFStringGetLength(s);
  CFIndex max = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
  gchar *buf = g_malloc((gsize)max);
  if (!CFStringGetCString(s, buf, max, kCFStringEncodingUTF8)) {
    g_free(buf);
    return NULL;
  }
  return buf;
}

/* Apply the test keychain to a SecItem query: kSecUseKeychain for add,
 * kSecMatchSearchList for search/delete. */
static void
kc_scope_query(GhStoreKeyKeychain *self, CFMutableDictionaryRef q, gboolean for_add)
{
  if (!self->keychain) return;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  if (for_add)
    CFDictionarySetValue(q, kSecUseKeychain, self->keychain);
  else {
    CFArrayRef list = CFArrayCreate(NULL, (const void *[]){self->keychain}, 1,
                                    &kCFTypeArrayCallBacks);
    CFDictionarySetValue(q, kSecMatchSearchList, list);
    CFRelease(list);
  }
#pragma clang diagnostic pop
}

/* ---- search ----------------------------------------------------------- */

/* Fetch the secret data for a single item identified by service + account +
 * store-id.  On macOS 26+ kSecReturnData with kSecMatchLimitAll returns
 * errSecParam, so we fetch data per-item with kSecMatchLimitOne. */
static GBytes *
kc_fetch_secret(GhStoreKeyKeychain *self, CFStringRef service,
                CFStringRef cf_acct, CFStringRef cf_sid)
{
  CFMutableDictionaryRef q = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(q, kSecAttrSynchronizable, kCFBooleanFalse);
  CFDictionarySetValue(q, kSecAttrService, service);
  if (cf_acct) CFDictionarySetValue(q, kSecAttrAccount, cf_acct);
  if (cf_sid) CFDictionarySetValue(q, kSecAttrDescription, cf_sid);
  CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
  kc_scope_query(self, q, FALSE);

  CFTypeRef result = NULL;
  OSStatus st = SecItemCopyMatching(q, &result);
  CFRelease(q);

  if (st != errSecSuccess || !result)
    return NULL;

  GBytes *secret = NULL;
  CFDataRef data = (CFDataRef)result;
  const UInt8 *bytes = CFDataGetBytePtr(data);
  CFIndex blen = CFDataGetLength(data);
  if (bytes && blen > 0)
    secret = gh_store_key_secret_new(bytes, (gsize)blen);
  /* Wipe key material from the CF-owned buffer before releasing.
   * The cast is intentional: we own the sole reference and are about
   * to release it, so zeroing prevents the secret lingering on the heap. */
  if (bytes && blen > 0)
    memset((void *)bytes, 0, (size_t)blen);
  CFRelease(result);
  return secret;
}

static void
kc_search_in_thread(GTask *task, gpointer source, gpointer task_data,
                    GCancellable *cancel)
{
  (void)cancel;
  GhStoreKeyKeychain *self = GH_STORE_KEY_KEYCHAIN(source);
  GHashTable *attrs = task_data;

  CFStringRef service = CFStringCreateWithCString(NULL, GH_STORE_KEY_SCHEMA_NAME,
                                                  kCFStringEncodingUTF8);

  /* Step 1: search for attributes only (no kSecReturnData).
   * macOS 26 returns errSecParam for kSecMatchLimitAll + kSecReturnData. */
  CFMutableDictionaryRef q = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(q, kSecAttrSynchronizable, kCFBooleanFalse);
  CFDictionarySetValue(q, kSecAttrService, service);
  CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitAll);
  CFDictionarySetValue(q, kSecReturnAttributes, kCFBooleanTrue);
  kc_scope_query(self, q, FALSE);

  /* Filter by account if requested. */
  const gchar *acct = attrs ? g_hash_table_lookup(attrs, GH_STORE_KEY_ATTR_ACCOUNT) : NULL;
  CFStringRef cf_acct = NULL;
  if (acct && *acct) {
    cf_acct = CFStringCreateWithCString(NULL, acct, kCFStringEncodingUTF8);
    CFDictionarySetValue(q, kSecAttrAccount, cf_acct);
  }

  CFTypeRef result = NULL;
  OSStatus st = SecItemCopyMatching(q, &result);
  CFRelease(q);

  GPtrArray *out = g_ptr_array_new_with_free_func(
      (GDestroyNotify)gh_store_key_item_free);

  if (st == errSecItemNotFound || !result) {
    CFRelease(service);
    if (cf_acct) CFRelease(cf_acct);
    g_task_return_pointer(task, out, (GDestroyNotify)g_ptr_array_unref);
    return;
  }
  if (st != errSecSuccess) {
    CFRelease(service);
    if (cf_acct) CFRelease(cf_acct);
    g_ptr_array_unref(out);
    g_task_return_new_error(task, GH_STORE_KEY_ERROR,
                            GH_STORE_KEY_ERROR_UNAVAILABLE,
                            "Keychain search failed: %d", (int)st);
    if (result) CFRelease(result);
    return;
  }

  /* Step 2: for each item, extract attributes and fetch data separately. */
  CFArrayRef items = (CFArrayRef)result;
  CFIndex count = CFArrayGetCount(items);

  for (CFIndex i = 0; i < count; i++) {
    CFDictionaryRef d = CFArrayGetValueAtIndex(items, i);

    GHashTable *ia = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    gchar *a = cfstring_to_gchar(CFDictionaryGetValue(d, kSecAttrAccount));
    gchar *sid = cfstring_to_gchar(CFDictionaryGetValue(d, kSecAttrDescription));
    gchar *ver = cfstring_to_gchar(CFDictionaryGetValue(d, kSecAttrComment));

    if (a) g_hash_table_insert(ia, g_strdup(GH_STORE_KEY_ATTR_ACCOUNT), a);
    if (sid) g_hash_table_insert(ia, g_strdup(GH_STORE_KEY_ATTR_STORE_ID), sid);
    if (ver) g_hash_table_insert(ia, g_strdup(GH_STORE_KEY_ATTR_VERSION), ver);

    /* Fetch secret data per-item (kSecMatchLimitOne + kSecReturnData). */
    CFStringRef cf_item_acct = CFDictionaryGetValue(d, kSecAttrAccount);
    CFStringRef cf_item_sid = CFDictionaryGetValue(d, kSecAttrDescription);
    GBytes *secret = kc_fetch_secret(self, service, cf_item_acct, cf_item_sid);

    GhStoreKeyItem *item = gh_store_key_item_new(ia, /* locked= */ FALSE, secret);
    g_ptr_array_add(out, item);
  }

  CFRelease(result);
  CFRelease(service);
  if (cf_acct) CFRelease(cf_acct);
  g_task_return_pointer(task, out, (GDestroyNotify)g_ptr_array_unref);
}

static void
kc_search_async(GhStoreKeyBackend *self, GHashTable *attributes,
                GhStoreKeyFlags flags, GCancellable *cancellable,
                GAsyncReadyCallback callback, gpointer user_data)
{
  (void)flags;
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_task_data(task, attributes ? g_hash_table_ref(attributes) : NULL,
                       attributes ? (GDestroyNotify)g_hash_table_unref : NULL);
  g_task_run_in_thread(task, kc_search_in_thread);
  g_object_unref(task);
}

static GPtrArray *
kc_search_finish(GhStoreKeyBackend *self, GAsyncResult *result, GError **error)
{
  (void)self;
  return g_task_propagate_pointer(G_TASK(result), error);
}

/* ---- store ------------------------------------------------------------ */

typedef struct {
  GHashTable *attributes;
  gchar *label;
  GBytes *secret;
} StoreData;

static void store_data_free(gpointer p) {
  StoreData *sd = p;
  if (!sd) return;
  g_clear_pointer(&sd->attributes, g_hash_table_unref);
  g_free(sd->label);
  g_clear_pointer(&sd->secret, g_bytes_unref);
  g_free(sd);
}

static void
kc_store_in_thread(GTask *task, gpointer source, gpointer task_data,
                   GCancellable *cancel)
{
  (void)cancel;
  GhStoreKeyKeychain *self = GH_STORE_KEY_KEYCHAIN(source);
  StoreData *sd = task_data;

  const gchar *acct = g_hash_table_lookup(sd->attributes, GH_STORE_KEY_ATTR_ACCOUNT);
  const gchar *sid = g_hash_table_lookup(sd->attributes, GH_STORE_KEY_ATTR_STORE_ID);
  const gchar *ver = g_hash_table_lookup(sd->attributes, GH_STORE_KEY_ATTR_VERSION);

  CFMutableDictionaryRef q = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

  CFStringRef cf_service = CFStringCreateWithCString(NULL, GH_STORE_KEY_SCHEMA_NAME,
                                                     kCFStringEncodingUTF8);
  CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(q, kSecAttrSynchronizable, kCFBooleanFalse);
  CFDictionarySetValue(q, kSecAttrService, cf_service);

  CFStringRef cf_acct = acct ? CFStringCreateWithCString(NULL, acct, kCFStringEncodingUTF8) : NULL;
  CFStringRef cf_sid = sid ? CFStringCreateWithCString(NULL, sid, kCFStringEncodingUTF8) : NULL;
  CFStringRef cf_ver = ver ? CFStringCreateWithCString(NULL, ver, kCFStringEncodingUTF8) : NULL;
  CFStringRef cf_label = sd->label ? CFStringCreateWithCString(NULL, sd->label, kCFStringEncodingUTF8) : NULL;

  if (cf_acct) CFDictionarySetValue(q, kSecAttrAccount, cf_acct);
  if (cf_sid) CFDictionarySetValue(q, kSecAttrDescription, cf_sid);
  if (cf_ver) CFDictionarySetValue(q, kSecAttrComment, cf_ver);
  if (cf_label) CFDictionarySetValue(q, kSecAttrLabel, cf_label);

  gsize slen = 0;
  gconstpointer sdata = g_bytes_get_data(sd->secret, &slen);
  CFDataRef cf_data = CFDataCreate(NULL, sdata, (CFIndex)slen);
  CFDictionarySetValue(q, kSecValueData, cf_data);
  CFDictionarySetValue(q, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlock);
  kc_scope_query(self, q, TRUE);

  OSStatus st = SecItemAdd(q, NULL);

  CFRelease(cf_service);
  if (cf_acct) CFRelease(cf_acct);
  if (cf_sid) CFRelease(cf_sid);
  if (cf_ver) CFRelease(cf_ver);
  if (cf_label) CFRelease(cf_label);
  CFRelease(cf_data);
  CFRelease(q);

  if (st == errSecSuccess) {
    g_task_return_boolean(task, TRUE);
  } else {
    g_task_return_new_error(task, GH_STORE_KEY_ERROR,
                            GH_STORE_KEY_ERROR_FAILED,
                            "Keychain store failed: %d", (int)st);
  }
}

static void
kc_store_async(GhStoreKeyBackend *self, GHashTable *attributes,
               const gchar *label, GBytes *secret, GhStoreKeyFlags flags,
               GCancellable *cancellable, GAsyncReadyCallback callback,
               gpointer user_data)
{
  (void)flags;
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  StoreData *sd = g_new0(StoreData, 1);
  sd->attributes = g_hash_table_ref(attributes);
  sd->label = g_strdup(label);
  sd->secret = g_bytes_ref(secret);
  g_task_set_task_data(task, sd, store_data_free);
  g_task_run_in_thread(task, kc_store_in_thread);
  g_object_unref(task);
}

static gboolean
kc_store_finish(GhStoreKeyBackend *self, GAsyncResult *result, GError **error)
{
  (void)self;
  return g_task_propagate_boolean(G_TASK(result), error);
}

/* ---- clear ------------------------------------------------------------ */

static void
kc_clear_in_thread(GTask *task, gpointer source, gpointer task_data,
                   GCancellable *cancel)
{
  (void)cancel;
  GhStoreKeyKeychain *self = GH_STORE_KEY_KEYCHAIN(source);
  GHashTable *attrs = task_data;

  CFMutableDictionaryRef q = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

  CFStringRef cf_service = CFStringCreateWithCString(NULL, GH_STORE_KEY_SCHEMA_NAME,
                                                     kCFStringEncodingUTF8);
  CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(q, kSecAttrService, cf_service);

  const gchar *acct = attrs ? g_hash_table_lookup(attrs, GH_STORE_KEY_ATTR_ACCOUNT) : NULL;
  CFStringRef cf_acct = NULL;
  if (acct && *acct) {
    cf_acct = CFStringCreateWithCString(NULL, acct, kCFStringEncodingUTF8);
    CFDictionarySetValue(q, kSecAttrAccount, cf_acct);
  }

  kc_scope_query(self, q, FALSE);
  OSStatus st = SecItemDelete(q);
  CFRelease(cf_service);
  if (cf_acct) CFRelease(cf_acct);
  CFRelease(q);

  /* Success or not-found both mean "none remain". */
  if (st == errSecSuccess || st == errSecItemNotFound) {
    g_task_return_boolean(task, TRUE);
  } else {
    g_task_return_new_error(task, GH_STORE_KEY_ERROR,
                            GH_STORE_KEY_ERROR_FAILED,
                            "Keychain clear failed: %d", (int)st);
  }
}

static void
kc_clear_async(GhStoreKeyBackend *self, GHashTable *attributes,
               GhStoreKeyFlags flags, GCancellable *cancellable,
               GAsyncReadyCallback callback, gpointer user_data)
{
  (void)flags;
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_task_data(task, attributes ? g_hash_table_ref(attributes) : NULL,
                       attributes ? (GDestroyNotify)g_hash_table_unref : NULL);
  g_task_run_in_thread(task, kc_clear_in_thread);
  g_object_unref(task);
}

static gboolean
kc_clear_finish(GhStoreKeyBackend *self, GAsyncResult *result, GError **error)
{
  (void)self;
  return g_task_propagate_boolean(G_TASK(result), error);
}

/* ---- interface init --------------------------------------------------- */

static void
keychain_backend_init(GhStoreKeyBackendInterface *iface)
{
  iface->search_async  = kc_search_async;
  iface->search_finish = kc_search_finish;
  iface->store_async   = kc_store_async;
  iface->store_finish  = kc_store_finish;
  iface->clear_async   = kc_clear_async;
  iface->clear_finish  = kc_clear_finish;
}

#endif /* __APPLE__ */
