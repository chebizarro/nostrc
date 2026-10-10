/* test_keychain_migration - macOS Keychain import of pre-e5nz GNostr client
 * keys (nostrc-de9h).
 *
 * Compiles signer_ops.c with NIP55L_KEYCHAIN_TEST_HOOKS, which pins every
 * Keychain query and write of the migration to a throwaway keychain file
 * created here (the hooks abort if none is set), so the user's login
 * keychain is never read or written.
 *
 * Seeds three "org.gnostr.Client" items (account = npub, data = nsec text,
 * as GNostr wrote them before nostrc-e5nz) and asserts:
 *   - a software key is re-stored in the daemon's format (service
 *     "Gnostr Identity Key", account = comment = npub, 32 raw bytes) and the
 *     client item is deleted;
 *   - an item whose secret is not a key is left in place (skipped);
 *   - a key the signer already holds (under another label) only retires
 *     the client copy; the signer's item is untouched;
 *   - the marker makes a second pass a no-op.
 */
#include "nostr/nip55l/signer_ops.h"
#include "nostr/nip55l/error.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <nostr-keys.h>
#include <nostr-utils.h>
#include <nostr/nip19/nip19.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../../../tests/common/nostrc-test-keychain-guard.h"

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

void nostr_nip55l_test_use_keychain(SecKeychainRef kc);
int nostr_nip55l_test_keychain_lookup_status(OSStatus status);

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); \
  } } while (0)

static SecKeychainRef kc;

typedef struct { char *sk_hex, *npub, *nsec; } Key;

static void key_new(Key *k) {
  k->sk_hex = nostr_key_generate_private();
  CHECK(k->sk_hex);
  char *pk = nostr_key_get_public(k->sk_hex);
  uint8_t b[32];
  CHECK(pk && nostr_hex2bin(b, pk, 32));
  CHECK(nostr_nip19_encode_npub(b, &k->npub) == 0);
  CHECK(nostr_hex2bin(b, k->sk_hex, 32));
  CHECK(nostr_nip19_encode_nsec(b, &k->nsec) == 0);
  free(pk);
}

static CFMutableDictionaryRef q_new(const char *service, CFStringRef attr, const char *value) {
  CFMutableDictionaryRef q = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                       &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
  CFStringRef s = CFStringCreateWithCString(NULL, service, kCFStringEncodingUTF8);
  CFDictionarySetValue(q, kSecAttrService, s);
  CFRelease(s);
  if (attr) {
    CFStringRef v = CFStringCreateWithCString(NULL, value, kCFStringEncodingUTF8);
    CFDictionarySetValue(q, attr, v);
    CFRelease(v);
  }
  return q;
}

static void add_item(const char *service, const char *account, const char *comment,
                     const void *data, size_t len) {
  CFMutableDictionaryRef q = q_new(service, kSecAttrAccount, account);
  CFDictionarySetValue(q, kSecUseKeychain, kc);
  if (comment) {
    CFStringRef c = CFStringCreateWithCString(NULL, comment, kCFStringEncodingUTF8);
    CFDictionarySetValue(q, kSecAttrComment, c);
    CFRelease(c);
  }
  CFDataRef d = CFDataCreate(NULL, data, (CFIndex)len);
  CFDictionarySetValue(q, kSecValueData, d);
  OSStatus st = SecItemAdd(q, NULL);
  if (st != errSecSuccess) fprintf(stderr, "SecItemAdd: %d\n", (int)st);
  CHECK(st == errSecSuccess);
  CFRelease(d);
  CFRelease(q);
}

/* Data of the item (service, attr=value) in the test keychain, or NULL. */
static CFDataRef get_item(const char *service, CFStringRef attr, const char *value) {
  CFMutableDictionaryRef q = q_new(service, attr, value);
  const void *kcs[1] = { kc };
  CFArrayRef list = CFArrayCreate(NULL, kcs, 1, &kCFTypeArrayCallBacks);
  CFDictionarySetValue(q, kSecMatchSearchList, list);
  CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
  CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
  CFTypeRef res = NULL;
  OSStatus st = SecItemCopyMatching(q, &res);
  CFRelease(list);
  CFRelease(q);
  return st == errSecSuccess ? (CFDataRef)res : NULL;
}

static int holds_raw_key(CFDataRef d, const char *sk_hex) {
  uint8_t sk[32];
  CHECK(nostr_hex2bin(sk, sk_hex, 32));
  return d && CFDataGetLength(d) == 32 && memcmp(CFDataGetBytePtr(d), sk, 32) == 0;
}

int main(void) {
  nostrc_test_keychain_guard_begin();
  /* Pure status mapping: no query against the developer's login keychain. */
  CHECK(nostr_nip55l_test_keychain_lookup_status(errSecSuccess) == 0);
  CHECK(nostr_nip55l_test_keychain_lookup_status(errSecItemNotFound) == NOSTR_SIGNER_ERROR_NOT_FOUND);
  CHECK(nostr_nip55l_test_keychain_lookup_status(errSecInteractionNotAllowed) == NOSTR_SIGNER_ERROR_BACKEND);
  CHECK(nostr_nip55l_test_keychain_lookup_status(errSecAuthFailed) == NOSTR_SIGNER_ERROR_BACKEND);
  char tmpl[] = "/tmp/nip55l-kc-XXXXXX";
  int fd = mkstemp(tmpl);
  CHECK(fd >= 0);
  close(fd);
  unlink(tmpl);
  char path[64];
  snprintf(path, sizeof path, "%s.keychain", tmpl);
  OSStatus st = SecKeychainCreate(path, 4, "test", FALSE, NULL, &kc);
  if (st != errSecSuccess) fprintf(stderr, "SecKeychainCreate: %d\n", (int)st);
  CHECK(st == errSecSuccess);
  CHECK(SecKeychainUnlock(kc, 4, "test", TRUE) == errSecSuccess);
  nostr_nip55l_test_use_keychain(kc);

  Key a, c;
  key_new(&a);
  key_new(&c);
  Key b; key_new(&b); /* only its npub is used, with a non-key secret */
  add_item("org.gnostr.Client", a.npub, NULL, a.nsec, strlen(a.nsec));
  add_item("org.gnostr.Client", b.npub, NULL, "not a key", 9);
  add_item("org.gnostr.Client", c.npub, NULL, c.nsec, strlen(c.nsec));
  /* The signer already holds c under the user's own label. */
  uint8_t skc[32];
  CHECK(nostr_hex2bin(skc, c.sk_hex, 32));
  add_item("Gnostr Identity Key", "Alice", c.npub, skc, 32);

  nostr_nip55l_keyring_migration r;
  int rc = nostr_nip55l_migrate_legacy_keys(&r);
  fprintf(stderr, "rc=%d found=%u migrated=%u skipped=%u failed=%u marker=%d\n",
          rc, r.found, r.migrated, r.skipped, r.failed, r.marker_written);
  CHECK(rc == 0);
  CHECK(r.found == 3 && r.migrated == 2 && r.skipped == 1 && r.failed == 0 && r.marker_written);

  /* a: moved into the daemon's format, client copy gone. */
  CFDataRef d = get_item("Gnostr Identity Key", kSecAttrAccount, a.npub);
  CHECK(holds_raw_key(d, a.sk_hex));
  CFRelease(d);
  d = get_item("Gnostr Identity Key", kSecAttrComment, a.npub);
  CHECK(holds_raw_key(d, a.sk_hex));
  CFRelease(d);
  CHECK(get_item("org.gnostr.Client", kSecAttrAccount, a.npub) == NULL);
  /* b: not a key, left in place, nothing stored. */
  d = get_item("org.gnostr.Client", kSecAttrAccount, b.npub);
  CHECK(d != NULL);
  CFRelease(d);
  CHECK(get_item("Gnostr Identity Key", kSecAttrComment, b.npub) == NULL);
  /* c: client copy retired, the signer's own item untouched. */
  CHECK(get_item("org.gnostr.Client", kSecAttrAccount, c.npub) == NULL);
  d = get_item("Gnostr Identity Key", kSecAttrAccount, "Alice");
  CHECK(holds_raw_key(d, c.sk_hex));
  CFRelease(d);
  CHECK(get_item("Gnostr Identity Key", kSecAttrAccount, c.npub) == NULL);

  /* Marker: a later pass is a no-op even with a new client item present. */
  Key late; key_new(&late);
  add_item("org.gnostr.Client", late.npub, NULL, late.nsec, strlen(late.nsec));
  rc = nostr_nip55l_migrate_legacy_keys(&r);
  CHECK(rc == 0 && r.already_done == 1 && r.found == 0);
  d = get_item("org.gnostr.Client", kSecAttrAccount, late.npub);
  CHECK(d != NULL);
  CFRelease(d);

  SecKeychainDelete(kc);
  CFRelease(kc);
  nostrc_test_keychain_guard_end();
  printf("test_nip55l_keychain_migration: PASS\n");
  return 0;
}
