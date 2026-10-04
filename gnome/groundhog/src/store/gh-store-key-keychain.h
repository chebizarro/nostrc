#ifndef GH_STORE_KEY_KEYCHAIN_H
#define GH_STORE_KEY_KEYCHAIN_H
/* GhStoreKeyKeychain: macOS Keychain backend for GhStoreKeyBackend.
 *
 * Uses Security.framework SecItem APIs instead of libsecret.
 * Keychain items are stored with:
 *   kSecAttrService  = GH_STORE_KEY_SCHEMA_NAME
 *   kSecAttrAccount  = account pubkey hex
 *   kSecAttrDescription = store-id UUID
 *   kSecAttrComment  = version string
 *   kSecAttrLabel    = label
 *   kSecValueData    = 32-byte key material
 */

#ifdef __APPLE__

#include "gh-store-key.h"
#include <Security/Security.h>

G_BEGIN_DECLS

#define GH_TYPE_STORE_KEY_KEYCHAIN (gh_store_key_keychain_get_type())
G_DECLARE_FINAL_TYPE(GhStoreKeyKeychain, gh_store_key_keychain,
                     GH, STORE_KEY_KEYCHAIN, GObject)

/* Set the keychain to scope all SecItem operations to.
 * NULL (the default) uses the user's default keychain.
 * The object takes ownership of the ref (retains it). */
void gh_store_key_keychain_set_keychain(GhStoreKeyKeychain *self,
                                        SecKeychainRef      keychain);

/* Internal status mapping, exposed here for deterministic tests. */
GhStoreKeyError gh_store_key_keychain_error_from_status(OSStatus status);

G_END_DECLS

#endif /* __APPLE__ */
#endif /* GH_STORE_KEY_KEYCHAIN_H */
