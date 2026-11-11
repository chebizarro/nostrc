#include "gh-call-settings.h"
#include "gh-call-settings-private.h"
#include <libsecret/secret.h>
#include <string.h>
#ifdef __APPLE__
#include "../store/gh-profile-guard.h"
#endif

#define TURN_SERVICE "org.nostr.Groundhog.CallTurn"
static const SecretSchema turn_schema = {
  .name = TURN_SERVICE,
  .flags = SECRET_SCHEMA_NONE,
  .attributes = {
    { "url", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "username", SECRET_SCHEMA_ATTRIBUTE_STRING }, { NULL, 0 }
  }
};

gboolean
gh_call_ice_url_validate(const gchar *url, gboolean turn, GError **error)
{
  g_autoptr(GUri) uri = url ? g_uri_parse(url, G_URI_FLAGS_NONE, NULL) : NULL;
  const gchar *scheme = uri ? g_uri_get_scheme(uri) : NULL;
  if (!scheme || (turn ? (g_strcmp0(scheme, "turn") && g_strcmp0(scheme, "turns"))
                       : g_strcmp0(scheme, "stun")) ||
      !g_uri_get_host(uri) || !*g_uri_get_host(uri) ||
      g_uri_get_userinfo(uri) || g_uri_get_fragment(uri) ||
      (g_uri_get_path(uri) && *g_uri_get_path(uri)) ||
      (g_uri_get_query(uri) && g_strcmp0(g_uri_get_query(uri), "transport=udp") &&
       g_strcmp0(g_uri_get_query(uri), "transport=tcp"))) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid ICE server URL (use stun://, turn:// or turns:// without credentials)");
    return FALSE;
  }
  return TRUE;
}

typedef enum { TURN_LOOKUP, TURN_STORE, TURN_DELETE } TurnOperation;
typedef struct {
  TurnOperation operation;
  gchar *url;
  gchar *username;
  gchar *password;
#ifdef __APPLE__
  SecKeychainRef keychain;
  gboolean native;
#endif
} TurnRequest;

void
gh_call_turn_password_free(gchar *password)
{
  if (password) {
    secret_password_wipe(password);
    g_free(password);
  }
}

static void
turn_request_free(gpointer data)
{
  TurnRequest *request = data;
  g_free(request->url);
  g_free(request->username);
  gh_call_turn_password_free(request->password);
#ifdef __APPLE__
  if (request->keychain) CFRelease(request->keychain);
#endif
  g_free(request);
}

#ifdef __APPLE__
static GMutex test_keychain_mutex;
static SecKeychainRef test_keychain;

void
gh_call_turn_password_set_test_keychain(SecKeychainRef keychain)
{
  g_mutex_lock(&test_keychain_mutex);
  if (test_keychain) CFRelease(test_keychain);
  test_keychain = keychain ? (SecKeychainRef)CFRetain(keychain) : NULL;
  g_mutex_unlock(&test_keychain_mutex);
}

/* Legacy file keychains use the current application's default ACL. Do not
 * install an all-applications ACL or synchronize TURN credentials to iCloud.
 * Account is a length-prefixed pair: exact URL + username, without delimiter
 * collisions. Service is distinct from StoreKey and signer credentials. */
static CFMutableDictionaryRef
turn_query(TurnRequest *request, gboolean add)
{
  CFMutableDictionaryRef query = CFDictionaryCreateMutable(NULL, 0,
    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  g_autofree gchar *account = g_strdup_printf("%" G_GSIZE_FORMAT ":%s%s",
                                            strlen(request->url), request->url, request->username);
  CFStringRef cf_account = CFStringCreateWithCString(NULL, account, kCFStringEncodingUTF8);
  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, CFSTR(TURN_SERVICE));
  CFDictionarySetValue(query, kSecAttrAccount, cf_account);
  CFDictionarySetValue(query, kSecAttrSynchronizable, kCFBooleanFalse);
  CFRelease(cf_account);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  CFDictionarySetValue(query, kSecUseAuthenticationUI, kSecUseAuthenticationUIFail);
  if (request->keychain) {
    if (add) CFDictionarySetValue(query, kSecUseKeychain, request->keychain);
    else {
      CFArrayRef list = CFArrayCreate(NULL, (const void *[]){request->keychain}, 1,
                                     &kCFTypeArrayCallBacks);
      CFDictionarySetValue(query, kSecMatchSearchList, list);
      CFRelease(list);
    }
  }
#pragma clang diagnostic pop
  return query;
}

static void
turn_keychain_error(GError **error, OSStatus status)
{
  GIOErrorEnum code = status == errSecItemNotFound ? G_IO_ERROR_NOT_FOUND :
    status == errSecNotAvailable ? G_IO_ERROR_NOT_SUPPORTED :
    status == errSecInteractionNotAllowed || status == errSecAuthFailed ||
    status == errSecUserCanceled || status == errAuthorizationCanceled ? G_IO_ERROR_PERMISSION_DENIED :
    G_IO_ERROR_FAILED;
  g_set_error(error, G_IO_ERROR, code, "TURN Keychain operation failed (%d)%s", (int)status,
              code == G_IO_ERROR_PERMISSION_DENIED ? ": unlock Keychain or allow application access" : "");
}

static gchar *
turn_keychain_run(TurnRequest *request, GError **error)
{
  /* Prevent even an implicit unlock prompt on the default Keychain. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  SecKeychainRef keychain = request->keychain;
  OSStatus status = keychain ? errSecSuccess : SecKeychainCopyDefault(&keychain);
  SecKeychainStatus state = 0;
  if (status == errSecSuccess) status = SecKeychainGetStatus(keychain, &state);
  if (keychain && !request->keychain) CFRelease(keychain);
#pragma clang diagnostic pop
  if (status == errSecSuccess && !(state & kSecUnlockStateStatus))
    status = errSecInteractionNotAllowed;
  if (status != errSecSuccess) {
    turn_keychain_error(error, status);
    return NULL;
  }
  CFMutableDictionaryRef query = turn_query(request, FALSE);
  gchar *password = NULL;
  if (request->operation == TURN_LOOKUP) {
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFTypeRef result = NULL;
    status = SecItemCopyMatching(query, &result);
    if (status == errSecSuccess && result && CFGetTypeID(result) == CFDataGetTypeID()) {
      CFDataRef bytes = (CFDataRef)result;
      CFIndex length = CFDataGetLength(bytes);
      const UInt8 *data = CFDataGetBytePtr(bytes);
      if (length > 0 && !memchr(data, 0, (gsize)length) &&
          g_utf8_validate((const gchar *)data, length, NULL))
        password = g_strndup((const gchar *)data, (gsize)length);
      else
        status = errSecDecode;
    } else if (status == errSecSuccess) {
      status = errSecDecode;
    }
    if (result) CFRelease(result);
  } else if (request->operation == TURN_DELETE) {
    status = SecItemDelete(query);
    if (status == errSecItemNotFound) status = errSecSuccess;
  } else {
    CFDataRef bytes = CFDataCreate(NULL, (const UInt8 *)request->password,
                                  (CFIndex)strlen(request->password));
    CFDictionaryRef changes = CFDictionaryCreate(NULL, (const void *[]){kSecValueData},
      (const void *[]){bytes}, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    status = SecItemUpdate(query, changes);
    if (status == errSecItemNotFound) {
      CFRelease(query);
      query = turn_query(request, TRUE);
      CFDictionarySetValue(query, kSecAttrLabel, CFSTR("Groundhog call TURN credential"));
      CFDictionarySetValue(query, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlock);
      CFDictionarySetValue(query, kSecValueData, bytes);
      status = SecItemAdd(query, NULL);
      if (status == errSecDuplicateItem) {
        CFRelease(query);
        query = turn_query(request, FALSE);
        status = SecItemUpdate(query, changes);
      }
    }
    CFRelease(changes);
    CFRelease(bytes);
  }
  CFRelease(query);
  if (status != errSecSuccess) turn_keychain_error(error, status);
  return password;
}
#endif

static void
turn_return_result(GTask *task, gchar *password, GError *error)
{
  TurnRequest *request = g_task_get_task_data(task);
  if (error) {
    gh_call_turn_password_free(password);
    g_task_return_error(task, error);
  } else if (request->operation == TURN_LOOKUP) {
    g_task_return_pointer(task, password, (GDestroyNotify)gh_call_turn_password_free);
  } else {
    g_task_return_boolean(task, TRUE);
  }
}

#ifdef __APPLE__
static void
turn_keychain_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
  (void)source; (void)cancel;
  if (g_task_return_error_if_cancelled(task)) return;
  GError *error = NULL;
  gchar *password = turn_keychain_run(data, &error);
  turn_return_result(task, password, error);
}
#endif

static void
turn_secret_done(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  g_autoptr(GTask) task = data;
  TurnRequest *request = g_task_get_task_data(task);
  GError *error = NULL;
  gchar *password = NULL;
  if (request->operation == TURN_LOOKUP) {
    gchar *secret = secret_password_lookup_finish(result, &error);
    if (secret) {
      password = g_strdup(secret);
      secret_password_free(secret);
    } else if (!error) {
      g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                          "No TURN credential saved for this server and username");
    }
  } else if (request->operation == TURN_STORE) {
    gboolean saved = secret_password_store_finish(result, &error);
    /* A dismissed Secret Service prompt may return FALSE without an error. */
    if (!saved && !error)
      g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                          "The TURN credential was not saved");
  } else {
    secret_password_clear_finish(result, &error); /* missing delete is successful */
  }
  turn_return_result(task, password, error);
}

static void
turn_start(TurnOperation operation, const gchar *url, const gchar *username,
           const gchar *password, GCancellable *cancel, GAsyncReadyCallback callback, gpointer data)
{
  g_autoptr(GTask) task = g_task_new(NULL, cancel, callback, data);
  if (!url || !g_utf8_validate(url, -1, NULL) ||
      !gh_call_ice_url_validate(url, TRUE, NULL) || !username || !*username ||
      !g_utf8_validate(username, -1, NULL) ||
      (operation == TURN_STORE && (!password || !*password || !g_utf8_validate(password, -1, NULL)))) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "TURN storage requires a valid server, username and nonempty password");
    return;
  }
  TurnRequest *request = g_new0(TurnRequest, 1);
  request->operation = operation;
  request->url = g_strdup(url);
  request->username = g_strdup(username);
  request->password = g_strdup(password);
  g_task_set_task_data(task, request, turn_request_free);
#ifdef __APPLE__
  g_mutex_lock(&test_keychain_mutex);
  request->keychain = test_keychain ? (SecKeychainRef)CFRetain(test_keychain) : NULL;
  g_mutex_unlock(&test_keychain_mutex);
#ifdef GH_CALL_TURN_DEFAULT_SECRET_SERVICE
  request->native = request->keychain != NULL;
#else
  request->native = TRUE;
#endif
  if (request->native && !request->keychain && gh_profile_guard_refuses_default_keychain(username)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "Refusing to use the login Keychain under test or for a test account");
    return;
  }
  if (request->native) {
    g_task_run_in_thread(task, turn_keychain_thread);
    return;
  }
#endif
  /* libsecret already provides nonblocking operations on the caller's context. */
  if (operation == TURN_LOOKUP)
    secret_password_lookup(&turn_schema, cancel, turn_secret_done, g_object_ref(task),
                           "url", url, "username", username, NULL);
  else if (operation == TURN_STORE)
    secret_password_store(&turn_schema, SECRET_COLLECTION_DEFAULT,
      "Groundhog call TURN credential", password, cancel, turn_secret_done, g_object_ref(task),
      "url", url, "username", username, NULL);
  else
    secret_password_clear(&turn_schema, cancel, turn_secret_done, g_object_ref(task),
                          "url", url, "username", username, NULL);
}

void
gh_call_turn_password_store_async(const gchar *url, const gchar *username,
                                  const gchar *password, GCancellable *cancel,
                                  GAsyncReadyCallback callback, gpointer data)
{
  turn_start(TURN_STORE, url, username, password, cancel, callback, data);
}

gboolean
gh_call_turn_password_store_finish(GAsyncResult *result, GError **error)
{
  return g_task_propagate_boolean(G_TASK(result), error);
}

void
gh_call_turn_password_lookup_async(const gchar *url, const gchar *username,
                                   GCancellable *cancel,
                                   GAsyncReadyCallback callback, gpointer data)
{
  turn_start(TURN_LOOKUP, url, username, NULL, cancel, callback, data);
}

gchar *
gh_call_turn_password_lookup_finish(GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer(G_TASK(result), error);
}

void
gh_call_turn_password_delete_async(const gchar *url, const gchar *username, GCancellable *cancel,
                                   GAsyncReadyCallback callback, gpointer data)
{
  turn_start(TURN_DELETE, url, username, NULL, cancel, callback, data);
}

gboolean
gh_call_turn_password_delete_finish(GAsyncResult *result, GError **error)
{
  return g_task_propagate_boolean(G_TASK(result), error);
}
