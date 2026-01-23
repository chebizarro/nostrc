/* secret_store.c - Secure key storage implementation
 *
 * Wraps the nip55l signer_ops for UI-friendly operations with additional
 * features like listing all identities and generating new keys.
 *
 * Uses secure memory for handling private keys to prevent:
 * - Keys being swapped to disk
 * - Keys remaining in memory after use
 * - Timing attacks via constant-time comparison
 */
#include "secret_store.h"
#include "secure-memory.h"
#include "secure-mem.h"
#include "secure-delete.h"
#include <nostr/nip55l/signer_ops.h>
#include <nostr/nip55l/error.h>
#include <nostr/nip19/nip19.h>
#include <keys.h>
#include <nostr-utils.h>
#include <gio/gio.h>  /* For GTask async API */
#include <string.h>
#include <stdlib.h>

#ifdef GNOSTR_HAVE_LIBSECRET
#include <libsecret/secret.h>
#endif

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#define GNOSTR_HAVE_KEYCHAIN 1
#endif

/* Internal helpers */
static gboolean is_hex_64(const gchar *s) {
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

/* Convert binary to hex string in secure memory */
static gchar *bin_to_hex_secure(const guint8 *buf, gsize len) {
  static const gchar hexd[16] = "0123456789abcdef";
  gchar *out = (gchar*)gn_secure_alloc(len * 2 + 1);
  if (!out) return NULL;
  for (gsize i = 0; i < len; i++) {
    out[2*i] = hexd[(buf[i] >> 4) & 0xF];
    out[2*i+1] = hexd[buf[i] & 0xF];
  }
  out[len*2] = '\0';
  return out;
}

/* Legacy bin_to_hex for non-sensitive data */
static gchar *bin_to_hex(const guint8 *buf, gsize len) {
  static const gchar hexd[16] = "0123456789abcdef";
  gchar *out = g_malloc(len * 2 + 1);
  for (gsize i = 0; i < len; i++) {
    out[2*i] = hexd[(buf[i] >> 4) & 0xF];
    out[2*i+1] = hexd[buf[i] & 0xF];
  }
  out[len*2] = '\0';
  return out;
}

#ifdef GNOSTR_HAVE_LIBSECRET
static const SecretSchema IDENTITY_SCHEMA = {
  "org.gnostr.Signer/identity",
  SECRET_SCHEMA_NONE,
  {
    { "key_id",   SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "npub",     SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "label",    SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "hardware", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "owner_uid",      SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "owner_username", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { NULL, 0 }
  }
};
#endif

gboolean secret_store_is_available(void) {
#ifdef GNOSTR_HAVE_LIBSECRET
  /* Check if secret service is reachable */
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
#elif defined(GNOSTR_HAVE_KEYCHAIN)
  return TRUE;
#else
  return FALSE;
#endif
}

const gchar *secret_store_backend_name(void) {
#ifdef GNOSTR_HAVE_LIBSECRET
  return "libsecret";
#elif defined(GNOSTR_HAVE_KEYCHAIN)
  return "Keychain";
#else
  return "none";
#endif
}

SecretStoreResult secret_store_add(const gchar *key,
                                   const gchar *label,
                                   gboolean link_to_user) {
  if (!key || !*key) return SECRET_STORE_ERR_INVALID_KEY;

  /* Normalize key to hex using secure memory */
  gchar *sk_hex = NULL;
  if (is_hex_64(key)) {
    /* Use secure memory for secret key hex */
    sk_hex = gn_secure_strdup(key);
    if (!sk_hex) return SECRET_STORE_ERR_BACKEND;
    /* Convert to lowercase */
    for (gsize i = 0; sk_hex[i]; i++) {
      sk_hex[i] = g_ascii_tolower(sk_hex[i]);
    }
  } else if (g_str_has_prefix(key, "nsec1")) {
    guint8 sk[32];
    if (nostr_nip19_decode_nsec(key, sk) != 0) {
      return SECRET_STORE_ERR_INVALID_KEY;
    }
    sk_hex = bin_to_hex_secure(sk, 32);
    gn_secure_clear_buffer(sk);  /* Securely zero the buffer */
    if (!sk_hex) return SECRET_STORE_ERR_BACKEND;
  } else if (g_str_has_prefix(key, "ncrypt")) {
    /* ncrypt keys need special handling - for now pass through */
    sk_hex = gn_secure_strdup(key);
    if (!sk_hex) return SECRET_STORE_ERR_BACKEND;
  } else {
    return SECRET_STORE_ERR_INVALID_KEY;
  }

  /* Derive public key */
  gchar *pk_hex = nostr_key_get_public(sk_hex);
  if (!pk_hex) {
    gn_secure_strfree(sk_hex);
    return SECRET_STORE_ERR_BACKEND;
  }

  guint8 pk[32];
  if (!nostr_hex2bin(pk, pk_hex, 32)) {
    g_free(pk_hex);
    gn_secure_strfree(sk_hex);
    return SECRET_STORE_ERR_INVALID_KEY;
  }
  g_free(pk_hex);

  gchar *npub = NULL;
  if (nostr_nip19_encode_npub(pk, &npub) != 0 || !npub) {
    gn_secure_strfree(sk_hex);
    return SECRET_STORE_ERR_BACKEND;
  }

#ifdef GNOSTR_HAVE_LIBSECRET
  GError *err = NULL;
  gchar uid_buf[32];
  g_snprintf(uid_buf, sizeof(uid_buf), "%u", (unsigned)getuid());

  gboolean ok = secret_password_store_sync(&IDENTITY_SCHEMA,
                                           SECRET_COLLECTION_DEFAULT,
                                           label && *label ? label : "Gnostr Identity Key",
                                           sk_hex,
                                           NULL,
                                           &err,
                                           "key_id", npub,
                                           "npub", npub,
                                           "label", label ? label : "",
                                           "hardware", "false",
                                           "owner_uid", link_to_user ? uid_buf : "",
                                           "owner_username", "",
                                           NULL);

  /* Securely zero and free the secret key hex */
  gn_secure_strfree(sk_hex);

  if (err) {
    g_warning("secret_store_add: %s", err->message);
    g_clear_error(&err);
  }

  g_free(npub);
  return ok ? SECRET_STORE_OK : SECRET_STORE_ERR_BACKEND;

#elif defined(GNOSTR_HAVE_KEYCHAIN)
  /* macOS Keychain implementation */
  guint8 skb[32];
  if (!nostr_hex2bin(skb, sk_hex, 32)) {
    gn_secure_strfree(sk_hex);
    g_free(npub);
    return SECRET_STORE_ERR_INVALID_KEY;
  }

  CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFStringRef service = CFStringCreateWithCString(NULL, "Gnostr Identity Key", kCFStringEncodingUTF8);
  CFStringRef account = CFStringCreateWithCString(NULL, npub, kCFStringEncodingUTF8);
  CFStringRef labelCF = label ? CFStringCreateWithCString(NULL, label, kCFStringEncodingUTF8) : NULL;
  CFDataRef secretData = CFDataCreate(NULL, skb, 32);

  /* Securely zero the secret key buffers */
  gn_secure_clear_buffer(skb);
  gn_secure_strfree(sk_hex);

  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, service);
  CFDictionarySetValue(query, kSecAttrAccount, account);
  if (labelCF) CFDictionarySetValue(query, kSecAttrLabel, labelCF);

  /* Delete existing if present */
  SecItemDelete(query);

  CFDictionarySetValue(query, kSecValueData, secretData);
  CFDictionarySetValue(query, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlock);

  OSStatus st = SecItemAdd(query, NULL);

  if (service) CFRelease(service);
  if (account) CFRelease(account);
  if (labelCF) CFRelease(labelCF);
  if (secretData) CFRelease(secretData);
  CFRelease(query);
  g_free(npub);

  return (st == errSecSuccess) ? SECRET_STORE_OK : SECRET_STORE_ERR_BACKEND;
#else
  gn_secure_strfree(sk_hex);
  g_free(npub);
  return SECRET_STORE_ERR_BACKEND;
#endif
}

SecretStoreResult secret_store_remove(const gchar *selector) {
  if (!selector || !*selector) return SECRET_STORE_ERR_INVALID_KEY;

  int rc = nostr_nip55l_clear_key(selector);
  if (rc == 0) return SECRET_STORE_OK;
  if (rc == NOSTR_SIGNER_ERROR_NOT_FOUND) return SECRET_STORE_ERR_NOT_FOUND;
  return SECRET_STORE_ERR_BACKEND;
}

void secret_store_entry_free(SecretStoreEntry *entry) {
  if (!entry) return;
  g_free(entry->npub);
  g_free(entry->key_id);
  g_free(entry->label);
  g_free(entry->owner_username);
  g_free(entry);
}

GPtrArray *secret_store_list(void) {
  GPtrArray *arr = g_ptr_array_new_with_free_func((GDestroyNotify)secret_store_entry_free);

#ifdef GNOSTR_HAVE_LIBSECRET
  GError *err = NULL;
  SecretService *service = secret_service_get_sync(SECRET_SERVICE_NONE, NULL, &err);
  if (err || !service) {
    if (err) g_clear_error(&err);
    return arr;
  }

  GHashTable *attrs = g_hash_table_new(g_str_hash, g_str_equal);
  GList *items = secret_service_search_sync(service, &IDENTITY_SCHEMA, attrs,
                                            SECRET_SEARCH_ALL | SECRET_SEARCH_UNLOCK,
                                            NULL, &err);
  g_hash_table_unref(attrs);

  if (err) {
    g_warning("secret_store_list: %s", err->message);
    g_clear_error(&err);
  }

  for (GList *it = items; it; it = it->next) {
    SecretItem *item = SECRET_ITEM(it->data);
    GHashTable *item_attrs = secret_item_get_attributes(item);

    SecretStoreEntry *entry = g_new0(SecretStoreEntry, 1);

    const gchar *npub = g_hash_table_lookup(item_attrs, "npub");
    const gchar *key_id = g_hash_table_lookup(item_attrs, "key_id");
    const gchar *label = g_hash_table_lookup(item_attrs, "label");
    const gchar *owner_uid = g_hash_table_lookup(item_attrs, "owner_uid");
    const gchar *owner_username = g_hash_table_lookup(item_attrs, "owner_username");

    entry->npub = npub ? g_strdup(npub) : NULL;
    entry->key_id = key_id ? g_strdup(key_id) : NULL;
    entry->label = label ? g_strdup(label) : NULL;

    if (owner_uid && *owner_uid) {
      entry->has_owner = TRUE;
      entry->owner_uid = (uid_t)g_ascii_strtoull(owner_uid, NULL, 10);
      entry->owner_username = owner_username ? g_strdup(owner_username) : NULL;
    }

    g_hash_table_unref(item_attrs);
    g_ptr_array_add(arr, entry);
  }

  g_list_free_full(items, g_object_unref);
  g_object_unref(service);

#elif defined(GNOSTR_HAVE_KEYCHAIN)
  /* macOS: Query all items with our service name */
  CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFStringRef service = CFStringCreateWithCString(NULL, "Gnostr Identity Key", kCFStringEncodingUTF8);

  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, service);
  CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitAll);
  CFDictionarySetValue(query, kSecReturnAttributes, kCFBooleanTrue);

  CFTypeRef result = NULL;
  OSStatus st = SecItemCopyMatching(query, &result);

  if (st == errSecSuccess && result) {
    CFArrayRef items = (CFArrayRef)result;
    CFIndex count = CFArrayGetCount(items);

    for (CFIndex i = 0; i < count; i++) {
      CFDictionaryRef item = CFArrayGetValueAtIndex(items, i);

      SecretStoreEntry *entry = g_new0(SecretStoreEntry, 1);

      CFStringRef account = CFDictionaryGetValue(item, kSecAttrAccount);
      CFStringRef label = CFDictionaryGetValue(item, kSecAttrLabel);

      if (account) {
        CFIndex len = CFStringGetLength(account);
        CFIndex maxSize = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
        gchar *buf = g_malloc(maxSize);
        if (CFStringGetCString(account, buf, maxSize, kCFStringEncodingUTF8)) {
          entry->npub = buf;
          entry->key_id = g_strdup(buf);
        } else {
          g_free(buf);
        }
      }

      if (label) {
        CFIndex len = CFStringGetLength(label);
        CFIndex maxSize = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
        gchar *buf = g_malloc(maxSize);
        if (CFStringGetCString(label, buf, maxSize, kCFStringEncodingUTF8)) {
          entry->label = buf;
        } else {
          g_free(buf);
        }
      }

      g_ptr_array_add(arr, entry);
    }
    CFRelease(result);
  }

  if (service) CFRelease(service);
  CFRelease(query);
#endif

  return arr;
}

SecretStoreResult secret_store_get_public_key(const gchar *selector,
                                              gchar **out_npub) {
  if (!out_npub) return SECRET_STORE_ERR_INVALID_KEY;
  *out_npub = NULL;

  gchar *npub = NULL;
  int rc = nostr_nip55l_get_public_key(&npub);

  if (rc == 0 && npub) {
    *out_npub = g_strdup(npub);
    free(npub);
    return SECRET_STORE_OK;
  }

  if (rc == NOSTR_SIGNER_ERROR_NOT_FOUND) return SECRET_STORE_ERR_NOT_FOUND;
  return SECRET_STORE_ERR_BACKEND;
}

SecretStoreResult secret_store_sign_event(const gchar *event_json,
                                          const gchar *selector,
                                          gchar **out_signature) {
  if (!event_json || !out_signature) return SECRET_STORE_ERR_INVALID_KEY;
  *out_signature = NULL;

  gchar *sig = NULL;
  int rc = nostr_nip55l_sign_event(event_json, selector, NULL, &sig);

  if (rc == 0 && sig) {
    *out_signature = g_strdup(sig);
    free(sig);
    return SECRET_STORE_OK;
  }

  if (rc == NOSTR_SIGNER_ERROR_NOT_FOUND) return SECRET_STORE_ERR_NOT_FOUND;
  if (rc == NOSTR_SIGNER_ERROR_INVALID_KEY) return SECRET_STORE_ERR_INVALID_KEY;
  return SECRET_STORE_ERR_BACKEND;
}

SecretStoreResult secret_store_generate(const gchar *label,
                                        gboolean link_to_user,
                                        gchar **out_npub) {
  if (!out_npub) return SECRET_STORE_ERR_INVALID_KEY;
  *out_npub = NULL;

  /* Generate new keypair using libnostr */
  gchar *sk_hex_raw = nostr_key_generate_private();
  if (!sk_hex_raw) {
    return SECRET_STORE_ERR_BACKEND;
  }

  /* Copy to secure memory immediately and clear original */
  gchar *sk_hex = gn_secure_strdup(sk_hex_raw);
  gn_secure_clear_string(sk_hex_raw);  /* Zero and free the original */

  if (!sk_hex) {
    return SECRET_STORE_ERR_BACKEND;
  }

  /* Store it */
  SecretStoreResult rc = secret_store_add(sk_hex, label, link_to_user);
  if (rc != SECRET_STORE_OK) {
    gn_secure_strfree(sk_hex);
    return rc;
  }

  /* Derive npub to return */
  gchar *pk_hex = nostr_key_get_public(sk_hex);
  gn_secure_strfree(sk_hex);

  if (!pk_hex) {
    return SECRET_STORE_ERR_BACKEND;
  }

  guint8 pk[32];
  if (!nostr_hex2bin(pk, pk_hex, 32)) {
    free(pk_hex);
    return SECRET_STORE_ERR_BACKEND;
  }
  free(pk_hex);

  gchar *npub = NULL;
  if (nostr_nip19_encode_npub(pk, &npub) != 0 || !npub) {
    return SECRET_STORE_ERR_BACKEND;
  }

  *out_npub = g_strdup(npub);
  free(npub);
  return SECRET_STORE_OK;
}

SecretStoreResult secret_store_set_label(const gchar *selector,
                                         const gchar *new_label) {
  if (!selector || !*selector) return SECRET_STORE_ERR_INVALID_KEY;

#ifdef GNOSTR_HAVE_LIBSECRET
  GError *err = NULL;
  SecretService *service = secret_service_get_sync(SECRET_SERVICE_NONE, NULL, &err);
  if (err || !service) {
    if (err) g_clear_error(&err);
    return SECRET_STORE_ERR_BACKEND;
  }

  /* Find the item */
  GHashTable *attrs = g_hash_table_new(g_str_hash, g_str_equal);
  g_hash_table_insert(attrs, (gpointer)"npub", (gpointer)selector);

  GList *items = secret_service_search_sync(service, &IDENTITY_SCHEMA, attrs,
                                            SECRET_SEARCH_ALL | SECRET_SEARCH_UNLOCK,
                                            NULL, &err);
  g_hash_table_unref(attrs);

  if (err) {
    g_clear_error(&err);
    g_object_unref(service);
    return SECRET_STORE_ERR_BACKEND;
  }

  if (!items) {
    /* Try by key_id */
    attrs = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(attrs, (gpointer)"key_id", (gpointer)selector);
    items = secret_service_search_sync(service, &IDENTITY_SCHEMA, attrs,
                                       SECRET_SEARCH_ALL | SECRET_SEARCH_UNLOCK,
                                       NULL, &err);
    g_hash_table_unref(attrs);
    if (err) g_clear_error(&err);
  }

  if (!items) {
    g_object_unref(service);
    return SECRET_STORE_ERR_NOT_FOUND;
  }

  SecretItem *item = SECRET_ITEM(items->data);
  GHashTable *item_attrs = secret_item_get_attributes(item);

  /* Update label attribute */
  g_hash_table_replace(item_attrs, g_strdup("label"), g_strdup(new_label ? new_label : ""));

  gboolean ok = secret_item_set_attributes_sync(item, &IDENTITY_SCHEMA, item_attrs, NULL, &err);

  g_hash_table_unref(item_attrs);
  g_list_free_full(items, g_object_unref);
  g_object_unref(service);

  if (err) {
    g_warning("secret_store_set_label: %s", err->message);
    g_clear_error(&err);
    return SECRET_STORE_ERR_BACKEND;
  }

  return ok ? SECRET_STORE_OK : SECRET_STORE_ERR_BACKEND;

#elif defined(GNOSTR_HAVE_KEYCHAIN)
  /* macOS: Update label by deleting and re-adding */
  /* First, get the existing data */
  CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFStringRef service = CFStringCreateWithCString(NULL, "Gnostr Identity Key", kCFStringEncodingUTF8);
  CFStringRef account = CFStringCreateWithCString(NULL, selector, kCFStringEncodingUTF8);

  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, service);
  CFDictionarySetValue(query, kSecAttrAccount, account);

  CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

  CFStringRef labelCF = new_label ? CFStringCreateWithCString(NULL, new_label, kCFStringEncodingUTF8) : NULL;
  if (labelCF) {
    CFDictionarySetValue(attrs, kSecAttrLabel, labelCF);
  }

  OSStatus st = SecItemUpdate(query, attrs);

  if (service) CFRelease(service);
  if (account) CFRelease(account);
  if (labelCF) CFRelease(labelCF);
  CFRelease(query);
  CFRelease(attrs);

  return (st == errSecSuccess) ? SECRET_STORE_OK : SECRET_STORE_ERR_NOT_FOUND;
#else
  (void)new_label;
  return SECRET_STORE_ERR_BACKEND;
#endif
}

SecretStoreResult secret_store_get_secret(const gchar *selector,
                                          gchar **out_nsec) {
  if (!out_nsec) return SECRET_STORE_ERR_INVALID_KEY;
  *out_nsec = NULL;

#ifdef GNOSTR_HAVE_LIBSECRET
  GError *err = NULL;
  gchar *secret = NULL;

  /* Try by npub */
  secret = secret_password_lookup_sync(&IDENTITY_SCHEMA, NULL, &err,
                                       "npub", selector ? selector : "",
                                       NULL);
  if (err) {
    g_clear_error(&err);
  }

  if (!secret && selector) {
    /* Try by key_id */
    secret = secret_password_lookup_sync(&IDENTITY_SCHEMA, NULL, &err,
                                         "key_id", selector,
                                         NULL);
    if (err) g_clear_error(&err);
  }

  if (!secret) {
    return SECRET_STORE_ERR_NOT_FOUND;
  }

  /* Convert hex to nsec - use secure memory for the secret key */
  if (is_hex_64(secret)) {
    guint8 sk[32];
    if (nostr_hex2bin(sk, secret, 32)) {
      gchar *nsec = NULL;
      if (nostr_nip19_encode_nsec(sk, &nsec) == 0 && nsec) {
        /* Return nsec in secure memory */
        *out_nsec = gn_secure_strdup(nsec);
        gn_secure_clear_string(nsec);  /* Zero and free the original */
      }
      gn_secure_clear_buffer(sk);  /* Securely zero the buffer */
    }
  } else if (g_str_has_prefix(secret, "nsec1")) {
    /* Return nsec in secure memory */
    *out_nsec = gn_secure_strdup(secret);
  }

  /* Securely clear the secret before freeing */
  gn_secure_zero(secret, strlen(secret));
  secret_password_free(secret);
  return *out_nsec ? SECRET_STORE_OK : SECRET_STORE_ERR_BACKEND;

#elif defined(GNOSTR_HAVE_KEYCHAIN)
  CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFStringRef service = CFStringCreateWithCString(NULL, "Gnostr Identity Key", kCFStringEncodingUTF8);
  CFStringRef account = selector ? CFStringCreateWithCString(NULL, selector, kCFStringEncodingUTF8) : NULL;

  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, service);
  if (account) CFDictionarySetValue(query, kSecAttrAccount, account);
  CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
  CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);

  CFTypeRef result = NULL;
  OSStatus st = SecItemCopyMatching(query, &result);

  if (service) CFRelease(service);
  if (account) CFRelease(account);
  CFRelease(query);

  if (st == errSecSuccess && result) {
    CFDataRef data = (CFDataRef)result;
    CFIndex len = CFDataGetLength(data);
    const UInt8 *bytes = CFDataGetBytePtr(data);

    if (len == 32 && bytes) {
      gchar *nsec = NULL;
      if (nostr_nip19_encode_nsec(bytes, &nsec) == 0 && nsec) {
        *out_nsec = g_strdup(nsec);
        free(nsec);
      }
    }
    CFRelease(result);
    return *out_nsec ? SECRET_STORE_OK : SECRET_STORE_ERR_BACKEND;
  }

  return SECRET_STORE_ERR_NOT_FOUND;
#else
  (void)selector;
  return SECRET_STORE_ERR_BACKEND;
#endif
}

/* ======== Async API implementation ======== */

typedef struct {
  SecretStoreListCallback callback;
  gpointer user_data;
} ListAsyncData;

static void list_async_thread_func(GTask *task, gpointer source_object,
                                   gpointer task_data, GCancellable *cancellable) {
  (void)source_object;
  (void)task_data;
  (void)cancellable;

  /* Run the synchronous list in thread pool */
  GPtrArray *entries = secret_store_list();
  g_task_return_pointer(task, entries, (GDestroyNotify)g_ptr_array_unref);
}

static void list_async_ready_cb(GObject *source_object, GAsyncResult *res,
                                gpointer user_data) {
  (void)source_object;
  ListAsyncData *data = user_data;

  GError *error = NULL;
  GPtrArray *entries = g_task_propagate_pointer(G_TASK(res), &error);

  if (error) {
    g_warning("secret_store_list_async failed: %s", error->message);
    g_clear_error(&error);
    entries = NULL;
  }

  if (data->callback) {
    data->callback(entries, data->user_data);
  } else if (entries) {
    g_ptr_array_unref(entries);
  }

  g_free(data);
}

void secret_store_list_async(SecretStoreListCallback callback, gpointer user_data) {
  ListAsyncData *data = g_new0(ListAsyncData, 1);
  data->callback = callback;
  data->user_data = user_data;

  GTask *task = g_task_new(NULL, NULL, list_async_ready_cb, data);
  g_task_set_name(task, "secret_store_list_async");
  g_task_run_in_thread(task, list_async_thread_func);
  g_object_unref(task);
}

typedef struct {
  SecretStoreAvailableCallback callback;
  gpointer user_data;
} AvailableAsyncData;

static void available_async_thread_func(GTask *task, gpointer source_object,
                                        gpointer task_data, GCancellable *cancellable) {
  (void)source_object;
  (void)task_data;
  (void)cancellable;

  gboolean available = secret_store_is_available();
  g_task_return_boolean(task, available);
}

static void available_async_ready_cb(GObject *source_object, GAsyncResult *res,
                                     gpointer user_data) {
  (void)source_object;
  AvailableAsyncData *data = user_data;

  GError *error = NULL;
  gboolean available = g_task_propagate_boolean(G_TASK(res), &error);

  if (error) {
    g_warning("secret_store_check_available_async failed: %s", error->message);
    g_clear_error(&error);
    available = FALSE;
  }

  if (data->callback) {
    data->callback(available, data->user_data);
  }

  g_free(data);
}

void secret_store_check_available_async(SecretStoreAvailableCallback callback,
                                        gpointer user_data) {
  AvailableAsyncData *data = g_new0(AvailableAsyncData, 1);
  data->callback = callback;
  data->user_data = user_data;

  GTask *task = g_task_new(NULL, NULL, available_async_ready_cb, data);
  g_task_set_name(task, "secret_store_check_available_async");
  g_task_run_in_thread(task, available_async_thread_func);
  g_object_unref(task);
}
