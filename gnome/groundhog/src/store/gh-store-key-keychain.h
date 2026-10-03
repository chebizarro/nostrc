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

G_BEGIN_DECLS

#define GH_TYPE_STORE_KEY_KEYCHAIN (gh_store_key_keychain_get_type())
G_DECLARE_FINAL_TYPE(GhStoreKeyKeychain, gh_store_key_keychain,
                     GH, STORE_KEY_KEYCHAIN, GObject)

G_END_DECLS

#endif /* __APPLE__ */
#endif /* GH_STORE_KEY_KEYCHAIN_H */
