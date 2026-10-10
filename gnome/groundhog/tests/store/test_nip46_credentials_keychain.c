#ifdef __APPLE__
#include "gh-nip46-credentials-private.h"
#include "../../../../tests/common/nostrc-test-keychain-guard.h"
#include <Security/Security.h>
#include <glib.h>
#include <unistd.h>

static SecKeychainRef keychain;
static char directory[256], path[512];
static GMainLoop *loop;
static const gchar *account = "1111111111111111111111111111111111111111111111111111111111111111";
static const gchar *signer = "2222222222222222222222222222222222222222222222222222222222222222";
static const gchar *first_key = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const gchar *second_key = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

typedef struct { GhNip46Credential *credential; GPtrArray *list; gboolean ok; GError *error; } Result;
static void
on_store(GObject *o, GAsyncResult *r, gpointer p)
{
  Result *out = p;
  out->ok = gh_nip46_credential_store_store_finish(GH_NIP46_CREDENTIAL_STORE(o), r, &out->error);
  g_main_loop_quit(loop);
}
static void
on_lookup(GObject *o, GAsyncResult *r, gpointer p)
{
  Result *out = p;
  out->credential = gh_nip46_credential_store_lookup_finish(GH_NIP46_CREDENTIAL_STORE(o), r, &out->error);
  g_main_loop_quit(loop);
}
static void
on_delete(GObject *o, GAsyncResult *r, gpointer p)
{
  Result *out = p;
  out->ok = gh_nip46_credential_store_delete_finish(GH_NIP46_CREDENTIAL_STORE(o), r, &out->error);
  g_main_loop_quit(loop);
}
static void
on_list(GObject *o, GAsyncResult *r, gpointer p)
{
  Result *out = p;
  out->list = gh_nip46_credential_store_list_finish(GH_NIP46_CREDENTIAL_STORE(o), r, &out->error);
  g_main_loop_quit(loop);
}
static Result
save(GhNip46CredentialStore *store, GhNip46Credential *credential, GCancellable *cancel)
{
  Result out = {0};
  gh_nip46_credential_store_store_async(store, credential, FALSE, cancel, on_store, &out);
  g_main_loop_run(loop); return out;
}
static Result
lookup(GhNip46CredentialStore *store)
{
  Result out = {0};
  gh_nip46_credential_store_lookup_async(store, account, NULL, on_lookup, &out);
  g_main_loop_run(loop); return out;
}
static Result
remove_credential(GhNip46CredentialStore *store, GCancellable *cancel)
{
  Result out = {0};
  gh_nip46_credential_store_delete_async(store, account, FALSE, cancel, on_delete, &out);
  g_main_loop_run(loop); return out;
}
static Result
list(GhNip46CredentialStore *store)
{
  Result out = {0};
  gh_nip46_credential_store_list_async(store, NULL, on_list, &out);
  g_main_loop_run(loop); return out;
}
static void
clear(Result *out)
{
  g_clear_error(&out->error);
  gh_nip46_credential_free(out->credential);
  if (out->list) g_ptr_array_unref(out->list);
}
static GhNip46Credential *
make_credential(const gchar *secret)
{
  const gchar *relays[] = { "wss://nos.lol", NULL };
  GError *error = NULL;
  GhNip46Credential *credential = gh_nip46_credential_new(account, signer, secret, relays, &error);
  g_assert_no_error(error);
  return credential;
}
static CFMutableDictionaryRef
raw_query(void)
{
  CFMutableDictionaryRef query = CFDictionaryCreateMutable(NULL, 0,
    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFStringRef service = CFSTR("org.nostr.Groundhog.Nip46Credential");
  CFStringRef acct = CFStringCreateWithCString(NULL, account, kCFStringEncodingUTF8);
  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, service);
  CFDictionarySetValue(query, kSecAttrAccount, acct);
  CFDictionarySetValue(query, kSecAttrSynchronizable, kCFBooleanFalse);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  CFArrayRef search = CFArrayCreate(NULL, (const void *[]){keychain}, 1, &kCFTypeArrayCallBacks);
  CFDictionarySetValue(query, kSecMatchSearchList, search);
  CFRelease(search);
#pragma clang diagnostic pop
  CFRelease(acct);
  return query;
}
static void
test_keychain(void)
{
  GhNip46CredentialStore *store = gh_nip46_credential_store_new_keychain(keychain);
  g_autoptr(GhNip46Credential) first = make_credential(first_key);
  g_autoptr(GhNip46Credential) second = make_credential(second_key);
  Result found = lookup(store);
  g_assert_error(found.error, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_NOT_FOUND);
  clear(&found);
  Result saved = save(store, first, NULL);
  g_assert_no_error(saved.error); g_assert_true(saved.ok); clear(&saved);
  found = lookup(store);
  g_assert_no_error(found.error);
  g_assert_cmpstr(gh_nip46_credential_get_client_secret_hex(found.credential), ==, first_key);
  clear(&found);
  Result listed = list(store);
  g_assert_no_error(listed.error); g_assert_cmpuint(listed.list->len, ==, 1);
  GhIdentityInfo *identity = g_ptr_array_index(listed.list, 0);
  g_assert_cmpint(identity->backend, ==, GH_SIGNER_BACKEND_NIP46);
  clear(&listed);
  CFMutableDictionaryRef query = raw_query();
  CFDictionarySetValue(query, kSecReturnAttributes, kCFBooleanTrue);
  CFTypeRef raw = NULL;
  g_assert_cmpint(SecItemCopyMatching(query, &raw), ==, errSecSuccess);
  CFDictionaryRef attrs = (CFDictionaryRef)raw;
  g_assert_true(CFEqual(CFDictionaryGetValue(attrs, kSecAttrLabel), CFSTR("Groundhog remote signer")));
  g_assert_true(CFEqual(CFDictionaryGetValue(attrs, kSecAttrComment), CFSTR("1")));
  CFStringRef forbidden = CFStringCreateWithCString(NULL, first_key, kCFStringEncodingUTF8);
  CFIndex count = CFDictionaryGetCount(attrs);
  const void **values = g_new(const void *, (gsize)count);
  CFDictionaryGetKeysAndValues(attrs, NULL, values);
  for (CFIndex i = 0; i < count; i++) {
    if (CFGetTypeID(values[i]) == CFStringGetTypeID())
      g_assert_cmpint(CFStringFind(values[i], forbidden, 0).location, ==, kCFNotFound);
  }
  g_free(values);
  CFRelease(forbidden);
  CFRelease(raw); CFRelease(query);
  saved = save(store, second, NULL);
  g_assert_no_error(saved.error); g_assert_true(saved.ok); clear(&saved);
  found = lookup(store);
  g_assert_no_error(found.error);
  g_assert_cmpstr(gh_nip46_credential_get_client_secret_hex(found.credential), ==, second_key);
  clear(&found);
  GCancellable *cancel = g_cancellable_new();
  g_cancellable_cancel(cancel);
  saved = save(store, first, cancel);
  g_assert_error(saved.error, G_IO_ERROR, G_IO_ERROR_CANCELLED); clear(&saved);
  found = lookup(store);
  g_assert_no_error(found.error);
  g_assert_cmpstr(gh_nip46_credential_get_client_secret_hex(found.credential), ==, second_key);
  clear(&found);
  Result deleted = remove_credential(store, cancel);
  g_assert_error(deleted.error, G_IO_ERROR, G_IO_ERROR_CANCELLED); clear(&deleted);
  found = lookup(store); g_assert_no_error(found.error); clear(&found);
  g_object_unref(cancel);
  deleted = remove_credential(store, NULL);
  g_assert_no_error(deleted.error); g_assert_true(deleted.ok); clear(&deleted);
  found = lookup(store);
  g_assert_error(found.error, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_NOT_FOUND);
  clear(&found);
  g_object_unref(store);
}
static void
test_newer_and_locked(void)
{
  GhNip46CredentialStore *store = gh_nip46_credential_store_new_keychain(keychain);
  CFMutableDictionaryRef add = CFDictionaryCreateMutable(NULL, 0,
    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFStringRef acct = CFStringCreateWithCString(NULL, account, kCFStringEncodingUTF8);
  CFDictionarySetValue(add, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(add, kSecAttrService, CFSTR("org.nostr.Groundhog.Nip46Credential"));
  CFDictionarySetValue(add, kSecAttrAccount, acct);
  CFDictionarySetValue(add, kSecAttrComment, CFSTR("2"));
  CFDictionarySetValue(add, kSecAttrLabel, CFSTR("Groundhog remote signer"));
  CFDataRef value = CFDataCreate(NULL, (const UInt8 *)"{\"version\":2}", 13);
  CFDictionarySetValue(add, kSecValueData, value);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  CFDictionarySetValue(add, kSecUseKeychain, keychain);
#pragma clang diagnostic pop
  g_assert_cmpint(SecItemAdd(add, NULL), ==, errSecSuccess);
  CFRelease(value); CFRelease(acct); CFRelease(add);
  g_autoptr(GhNip46Credential) c = make_credential(first_key);
  Result found = lookup(store);
  g_assert_error(found.error, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_NEWER_VERSION);
  clear(&found);
  Result saved = save(store, c, NULL);
  g_assert_error(saved.error, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_NEWER_VERSION);
  clear(&saved);
  Result deleted = remove_credential(store, NULL);
  g_assert_error(deleted.error, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_NEWER_VERSION);
  clear(&deleted);
  CFMutableDictionaryRef q = raw_query();
  g_assert_cmpint(SecItemDelete(q), ==, errSecSuccess);
  CFRelease(q);
  saved = save(store, c, NULL);
  g_assert_no_error(saved.error); clear(&saved);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  g_assert_cmpint(SecKeychainLock(keychain), ==, errSecSuccess);
#pragma clang diagnostic pop
  Result listed = list(store);
  if (listed.list) {
    g_assert_no_error(listed.error);
    g_assert_cmpuint(listed.list->len, ==, 1);
  } else {
    g_assert_error(listed.error, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_LOCKED);
  }
  clear(&listed);
  found = lookup(store);
  g_assert_error(found.error, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_LOCKED);
  clear(&found);
  g_object_unref(store);
}
/* nostrc-p15n5.3: an item whose ACL trusts no application (as an unsigned or
 * rebuilt binary sees its own items) makes any data read raise a macOS
 * "allow access" prompt. Listing must only read attributes, so it returns at
 * once without a prompt. A regression would block here until ctest's timeout. */
static void
test_list_without_secret_access(void)
{
  GhNip46CredentialStore *store = gh_nip46_credential_store_new_keychain(keychain);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  CFArrayRef nobody = CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks);
  SecAccessRef access = NULL;
  g_assert_cmpint(SecAccessCreate(CFSTR("Groundhog remote signer"), nobody, &access), ==, errSecSuccess);
  CFRelease(nobody);
  CFMutableDictionaryRef add = CFDictionaryCreateMutable(NULL, 0,
    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFStringRef acct = CFStringCreateWithCString(NULL, account, kCFStringEncodingUTF8);
  CFDictionarySetValue(add, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(add, kSecAttrService, CFSTR("org.nostr.Groundhog.Nip46Credential"));
  CFDictionarySetValue(add, kSecAttrAccount, acct);
  CFDictionarySetValue(add, kSecAttrComment, CFSTR("1"));
  CFDictionarySetValue(add, kSecAttrLabel, CFSTR("Groundhog remote signer"));
  CFDictionarySetValue(add, kSecAttrAccess, access);
  CFDataRef value = CFDataCreate(NULL, (const UInt8 *)"{}", 2);
  CFDictionarySetValue(add, kSecValueData, value);
  CFDictionarySetValue(add, kSecUseKeychain, keychain);
#pragma clang diagnostic pop
  g_assert_cmpint(SecItemAdd(add, NULL), ==, errSecSuccess);
  CFRelease(value); CFRelease(acct); CFRelease(add); CFRelease(access);
  gint64 started = g_get_monotonic_time();
  Result listed = list(store);
  g_assert_no_error(listed.error);
  g_assert_cmpuint(listed.list->len, ==, 1);
  g_assert_cmpint(((GhIdentityInfo *)g_ptr_array_index(listed.list, 0))->backend, ==,
                  GH_SIGNER_BACKEND_NIP46);
  g_assert_cmpint(g_get_monotonic_time() - started, <, 5 * G_USEC_PER_SEC);
  clear(&listed);
  CFMutableDictionaryRef q = raw_query();
  g_assert_cmpint(SecItemDelete(q), ==, errSecSuccess);
  CFRelease(q);
  g_object_unref(store);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static gboolean
setup_keychain(void)
{
  g_snprintf(directory, sizeof directory, "%s/nip46-kc-XXXXXX", g_get_tmp_dir());
  if (!mkdtemp(directory)) return FALSE;
  g_snprintf(path, sizeof path, "%s/test.keychain", directory);
  if (SecKeychainCreate(path, 4, "test", FALSE, NULL, &keychain) != errSecSuccess) return FALSE;
  return SecKeychainUnlock(keychain, 4, "test", TRUE) == errSecSuccess;
}
static void
teardown_keychain(void)
{
  if (keychain) { SecKeychainDelete(keychain); CFRelease(keychain); }
  unlink(path);
  char db[600]; g_snprintf(db, sizeof db, "%s-db", path); unlink(db);
  rmdir(directory);
}
#pragma clang diagnostic pop
int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  nostrc_test_keychain_guard_begin();
  if (!setup_keychain()) { g_print("SKIP: temporary Keychain unavailable\n"); return 77; }
  loop = g_main_loop_new(NULL, FALSE);
  g_test_add_func("/nip46-credentials/keychain", test_keychain);
  g_test_add_func("/nip46-credentials/list-without-secret-access",
                  test_list_without_secret_access);
  g_test_add_func("/nip46-credentials/zz-newer-and-locked", test_newer_and_locked);
  int result = g_test_run();
  g_main_loop_unref(loop);
  teardown_keychain();
  nostrc_test_keychain_guard_end();
  return result;
}
#endif
