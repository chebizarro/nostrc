/**
 * Secure Key Storage - macOS Keychain Implementation
 *
 * Uses Security.framework Keychain Services to store keys securely.
 * This file is only compiled on macOS (HAVE_MACOS_KEYCHAIN defined).
 */

#ifdef HAVE_MACOS_KEYCHAIN

#include "keystore.h"
#include <Security/Security.h>
#include <CoreFoundation/CoreFoundation.h>
#include <string.h>

#define GNOSTR_SERVICE_NAME "org.gnostr.Client"
#define GNOSTR_ACCESS_GROUP NULL /* Default access group */

/* Error quark */
G_DEFINE_QUARK(gnostr-keystore-error-quark, gnostr_keystore_error)

/* Key info helpers */
void gnostr_key_info_free(GnostrKeyInfo *info) {
  if (!info) return;
  g_free(info->npub);
  g_free(info->label);
  g_free(info);
}

GnostrKeyInfo *gnostr_key_info_copy(const GnostrKeyInfo *info) {
  if (!info) return NULL;
  GnostrKeyInfo *copy = g_new0(GnostrKeyInfo, 1);
  copy->npub = g_strdup(info->npub);
  copy->label = g_strdup(info->label);
  copy->created_at = info->created_at;
  return copy;
}

/* Convert OSStatus to GError */
static void set_error_from_osstatus(OSStatus status, GError **error) {
  if (!error) return;

  GnostrKeystoreError code;
  const char *message;

  switch (status) {
    case errSecItemNotFound:
      code = GNOSTR_KEYSTORE_ERROR_NOT_FOUND;
      message = "Key not found in Keychain";
      break;
    case errSecAuthFailed:
    case errSecUserCanceled:
      code = GNOSTR_KEYSTORE_ERROR_ACCESS_DENIED;
      message = "Access denied (user cancelled or authentication failed)";
      break;
    case errSecDuplicateItem:
      code = GNOSTR_KEYSTORE_ERROR_FAILED;
      message = "Key already exists in Keychain";
      break;
    case errSecNotAvailable:
      code = GNOSTR_KEYSTORE_ERROR_NOT_AVAILABLE;
      message = "Keychain is not available";
      break;
    default:
      code = GNOSTR_KEYSTORE_ERROR_FAILED;
      message = "Keychain operation failed";
      break;
  }

  CFStringRef desc = SecCopyErrorMessageString(status, NULL);
  if (desc) {
    char buf[256];
    CFStringGetCString(desc, buf, sizeof(buf), kCFStringEncodingUTF8);
    g_set_error(error, GNOSTR_KEYSTORE_ERROR, code,
                "%s: %s (OSStatus %d)", message, buf, (int)status);
    CFRelease(desc);
  } else {
    g_set_error(error, GNOSTR_KEYSTORE_ERROR, code,
                "%s (OSStatus %d)", message, (int)status);
  }
}

gboolean gnostr_keystore_available(void) {
  /* Keychain is always available on macOS */
  return TRUE;
}

/* Validate npub format (basic check) */
static gboolean validate_npub(const char *npub, GError **error) {
  if (!npub || !g_str_has_prefix(npub, "npub1") || strlen(npub) != 63) {
    g_set_error(error,
                GNOSTR_KEYSTORE_ERROR,
                GNOSTR_KEYSTORE_ERROR_INVALID_KEY,
                "Invalid npub format: expected npub1... with 63 characters");
    return FALSE;
  }
  return TRUE;
}

/* Validate nsec format (basic check) */
static gboolean validate_nsec(const char *nsec, GError **error) {
  if (!nsec || !g_str_has_prefix(nsec, "nsec1") || strlen(nsec) != 63) {
    g_set_error(error,
                GNOSTR_KEYSTORE_ERROR,
                GNOSTR_KEYSTORE_ERROR_INVALID_KEY,
                "Invalid nsec format: expected nsec1... with 63 characters");
    return FALSE;
  }
  return TRUE;
}

gboolean gnostr_keystore_store_key(const char *npub,
                                    const char *nsec,
                                    const char *label,
                                    GError **error) {
  if (!validate_npub(npub, error)) return FALSE;
  if (!validate_nsec(nsec, error)) return FALSE;

  CFStringRef service = CFStringCreateWithCString(NULL, GNOSTR_SERVICE_NAME,
                                                   kCFStringEncodingUTF8);
  CFStringRef account = CFStringCreateWithCString(NULL, npub,
                                                   kCFStringEncodingUTF8);
  CFDataRef secret_data = CFDataCreate(NULL, (const UInt8 *)nsec, strlen(nsec));

  /* Build the label string */
  char *display_label = g_strdup_printf("GNostr: %s", label ? label : npub);
  CFStringRef cf_label = CFStringCreateWithCString(NULL, display_label,
                                                    kCFStringEncodingUTF8);
  g_free(display_label);

  /* First, try to delete any existing item */
  CFMutableDictionaryRef delete_query = CFDictionaryCreateMutable(
      NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(delete_query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(delete_query, kSecAttrService, service);
  CFDictionarySetValue(delete_query, kSecAttrAccount, account);
  SecItemDelete(delete_query);
  CFRelease(delete_query);

  /* Build the add query */
  CFMutableDictionaryRef query = CFDictionaryCreateMutable(
      NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, service);
  CFDictionarySetValue(query, kSecAttrAccount, account);
  CFDictionarySetValue(query, kSecAttrLabel, cf_label);
  CFDictionarySetValue(query, kSecValueData, secret_data);

  /* Set accessibility - available when device is unlocked */
  CFDictionarySetValue(query, kSecAttrAccessible,
                       kSecAttrAccessibleWhenUnlocked);

  OSStatus status = SecItemAdd(query, NULL);

  CFRelease(query);
  CFRelease(service);
  CFRelease(account);
  CFRelease(secret_data);
  CFRelease(cf_label);

  if (status != errSecSuccess) {
    set_error_from_osstatus(status, error);
    return FALSE;
  }

  return TRUE;
}

/* Async store implementation */
typedef struct {
  char *npub;
  char *nsec;
  char *label;
} StoreAsyncData;

static void store_async_data_free(gpointer data) {
  StoreAsyncData *d = data;
  if (d->nsec) {
    /* Securely clear nsec from memory */
    memset(d->nsec, 0, strlen(d->nsec));
    g_free(d->nsec);
  }
  g_free(d->npub);
  g_free(d->label);
  g_free(d);
}

static void store_key_thread(GTask *task,
                              gpointer source_object,
                              gpointer task_data,
                              GCancellable *cancellable) {
  (void)source_object;
  (void)cancellable;

  StoreAsyncData *data = task_data;
  GError *error = NULL;

  gboolean result = gnostr_keystore_store_key(data->npub,
                                               data->nsec,
                                               data->label,
                                               &error);

  if (result) {
    g_task_return_boolean(task, TRUE);
  } else {
    g_task_return_error(task, error);
  }
}

void gnostr_keystore_store_key_async(const char *npub,
                                      const char *nsec,
                                      const char *label,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data) {
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);

  StoreAsyncData *data = g_new0(StoreAsyncData, 1);
  data->npub = g_strdup(npub);
  data->nsec = g_strdup(nsec);
  data->label = g_strdup(label);

  g_task_set_task_data(task, data, store_async_data_free);
  g_task_run_in_thread(task, store_key_thread);
  g_object_unref(task);
}

gboolean gnostr_keystore_store_key_finish(GAsyncResult *result,
                                           GError **error) {
  return g_task_propagate_boolean(G_TASK(result), error);
}

char *gnostr_keystore_retrieve_key(const char *npub, GError **error) {
  if (!validate_npub(npub, error)) return NULL;

  CFStringRef service = CFStringCreateWithCString(NULL, GNOSTR_SERVICE_NAME,
                                                   kCFStringEncodingUTF8);
  CFStringRef account = CFStringCreateWithCString(NULL, npub,
                                                   kCFStringEncodingUTF8);

  CFMutableDictionaryRef query = CFDictionaryCreateMutable(
      NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, service);
  CFDictionarySetValue(query, kSecAttrAccount, account);
  CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
  CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);

  CFDataRef result_data = NULL;
  OSStatus status = SecItemCopyMatching(query, (CFTypeRef *)&result_data);

  CFRelease(query);
  CFRelease(service);
  CFRelease(account);

  if (status != errSecSuccess) {
    set_error_from_osstatus(status, error);
    return NULL;
  }

  if (!result_data) {
    g_set_error(error, GNOSTR_KEYSTORE_ERROR,
                GNOSTR_KEYSTORE_ERROR_NOT_FOUND,
                "Key not found for npub: %s", npub);
    return NULL;
  }

  CFIndex length = CFDataGetLength(result_data);
  char *nsec = g_malloc(length + 1);
  memcpy(nsec, CFDataGetBytePtr(result_data), length);
  nsec[length] = '\0';

  CFRelease(result_data);

  return nsec;
}

/* Async retrieve implementation */
typedef struct {
  char *npub;
} RetrieveAsyncData;

static void retrieve_async_data_free(gpointer data) {
  RetrieveAsyncData *d = data;
  g_free(d->npub);
  g_free(d);
}

static void retrieve_key_thread(GTask *task,
                                 gpointer source_object,
                                 gpointer task_data,
                                 GCancellable *cancellable) {
  (void)source_object;
  (void)cancellable;

  RetrieveAsyncData *data = task_data;
  GError *error = NULL;

  char *nsec = gnostr_keystore_retrieve_key(data->npub, &error);

  if (nsec) {
    g_task_return_pointer(task, nsec, g_free);
  } else {
    g_task_return_error(task, error);
  }
}

void gnostr_keystore_retrieve_key_async(const char *npub,
                                         GCancellable *cancellable,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data) {
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);

  RetrieveAsyncData *data = g_new0(RetrieveAsyncData, 1);
  data->npub = g_strdup(npub);

  g_task_set_task_data(task, data, retrieve_async_data_free);
  g_task_run_in_thread(task, retrieve_key_thread);
  g_object_unref(task);
}

char *gnostr_keystore_retrieve_key_finish(GAsyncResult *result,
                                           GError **error) {
  return g_task_propagate_pointer(G_TASK(result), error);
}

gboolean gnostr_keystore_delete_key(const char *npub, GError **error) {
  if (!validate_npub(npub, error)) return FALSE;

  CFStringRef service = CFStringCreateWithCString(NULL, GNOSTR_SERVICE_NAME,
                                                   kCFStringEncodingUTF8);
  CFStringRef account = CFStringCreateWithCString(NULL, npub,
                                                   kCFStringEncodingUTF8);

  CFMutableDictionaryRef query = CFDictionaryCreateMutable(
      NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, service);
  CFDictionarySetValue(query, kSecAttrAccount, account);

  OSStatus status = SecItemDelete(query);

  CFRelease(query);
  CFRelease(service);
  CFRelease(account);

  if (status != errSecSuccess) {
    set_error_from_osstatus(status, error);
    return FALSE;
  }

  return TRUE;
}

/* Async delete implementation */
static void delete_key_thread(GTask *task,
                               gpointer source_object,
                               gpointer task_data,
                               GCancellable *cancellable) {
  (void)source_object;
  (void)cancellable;

  RetrieveAsyncData *data = task_data;
  GError *error = NULL;

  gboolean result = gnostr_keystore_delete_key(data->npub, &error);

  if (result) {
    g_task_return_boolean(task, TRUE);
  } else {
    g_task_return_error(task, error);
  }
}

void gnostr_keystore_delete_key_async(const char *npub,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data) {
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);

  RetrieveAsyncData *data = g_new0(RetrieveAsyncData, 1);
  data->npub = g_strdup(npub);

  g_task_set_task_data(task, data, retrieve_async_data_free);
  g_task_run_in_thread(task, delete_key_thread);
  g_object_unref(task);
}

gboolean gnostr_keystore_delete_key_finish(GAsyncResult *result,
                                            GError **error) {
  return g_task_propagate_boolean(G_TASK(result), error);
}

GList *gnostr_keystore_list_keys(GError **error) {
  GList *result = NULL;

  CFStringRef service = CFStringCreateWithCString(NULL, GNOSTR_SERVICE_NAME,
                                                   kCFStringEncodingUTF8);

  CFMutableDictionaryRef query = CFDictionaryCreateMutable(
      NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, service);
  CFDictionarySetValue(query, kSecReturnAttributes, kCFBooleanTrue);
  CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitAll);

  CFArrayRef items = NULL;
  OSStatus status = SecItemCopyMatching(query, (CFTypeRef *)&items);

  CFRelease(query);
  CFRelease(service);

  if (status == errSecItemNotFound) {
    /* No items is not an error */
    return NULL;
  }

  if (status != errSecSuccess) {
    set_error_from_osstatus(status, error);
    return NULL;
  }

  if (!items) {
    return NULL;
  }

  CFIndex count = CFArrayGetCount(items);
  for (CFIndex i = 0; i < count; i++) {
    CFDictionaryRef item = CFArrayGetValueAtIndex(items, i);

    CFStringRef account = CFDictionaryGetValue(item, kSecAttrAccount);
    CFStringRef label = CFDictionaryGetValue(item, kSecAttrLabel);
    CFDateRef creation_date = CFDictionaryGetValue(item, kSecAttrCreationDate);

    if (account) {
      char account_buf[128];
      if (CFStringGetCString(account, account_buf, sizeof(account_buf),
                             kCFStringEncodingUTF8)) {
        /* Only include items that look like npubs */
        if (g_str_has_prefix(account_buf, "npub1")) {
          GnostrKeyInfo *info = g_new0(GnostrKeyInfo, 1);
          info->npub = g_strdup(account_buf);

          if (label) {
            char label_buf[256];
            if (CFStringGetCString(label, label_buf, sizeof(label_buf),
                                   kCFStringEncodingUTF8)) {
              info->label = g_strdup(label_buf);
            }
          }

          if (creation_date) {
            CFAbsoluteTime abs_time = CFDateGetAbsoluteTime(creation_date);
            /* Convert from CFAbsoluteTime (seconds since Jan 1 2001)
             * to Unix timestamp (seconds since Jan 1 1970) */
            info->created_at = (gint64)(abs_time + 978307200.0);
          }

          result = g_list_prepend(result, info);
        }
      }
    }
  }

  CFRelease(items);

  return g_list_reverse(result);
}

/* Async list implementation */
static void list_keys_thread(GTask *task,
                              gpointer source_object,
                              gpointer task_data,
                              GCancellable *cancellable) {
  (void)source_object;
  (void)task_data;
  (void)cancellable;

  GError *error = NULL;
  GList *keys = gnostr_keystore_list_keys(&error);

  if (error) {
    g_task_return_error(task, error);
  } else {
    g_task_return_pointer(task, keys, NULL);
  }
}

void gnostr_keystore_list_keys_async(GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data) {
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_run_in_thread(task, list_keys_thread);
  g_object_unref(task);
}

GList *gnostr_keystore_list_keys_finish(GAsyncResult *result,
                                         GError **error) {
  return g_task_propagate_pointer(G_TASK(result), error);
}

gboolean gnostr_keystore_has_key(const char *npub) {
  if (!npub || !g_str_has_prefix(npub, "npub1")) return FALSE;

  CFStringRef service = CFStringCreateWithCString(NULL, GNOSTR_SERVICE_NAME,
                                                   kCFStringEncodingUTF8);
  CFStringRef account = CFStringCreateWithCString(NULL, npub,
                                                   kCFStringEncodingUTF8);

  CFMutableDictionaryRef query = CFDictionaryCreateMutable(
      NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, service);
  CFDictionarySetValue(query, kSecAttrAccount, account);
  CFDictionarySetValue(query, kSecReturnAttributes, kCFBooleanTrue);
  CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);

  CFDictionaryRef result = NULL;
  OSStatus status = SecItemCopyMatching(query, (CFTypeRef *)&result);

  CFRelease(query);
  CFRelease(service);
  CFRelease(account);

  if (result) {
    CFRelease(result);
  }

  return status == errSecSuccess;
}

#endif /* HAVE_MACOS_KEYCHAIN */
