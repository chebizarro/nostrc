#ifndef GH_NIP46_CREDENTIALS_PRIVATE_H
#define GH_NIP46_CREDENTIALS_PRIVATE_H

#include "gh-nip46-credentials.h"

typedef struct {
  gchar *account;
  gchar *version;
  gchar *label;
  gboolean locked;
  gboolean attributes_valid;
  GBytes *secret;
} GhNip46CredentialItem;

static inline void gh_nip46_credential_item_free(GhNip46CredentialItem *item)
{
  if (!item) return;
  g_free(item->account);
  g_free(item->version);
  g_free(item->label);
  g_clear_pointer(&item->secret, g_bytes_unref);
  g_free(item);
}

typedef struct _GhNip46CredentialBackend GhNip46CredentialBackend;
/* search() with load_secrets FALSE must only read item attributes: it may not
 * request secret data or unlock anything, so it can never raise a Keychain
 * access prompt or a keyring unlock prompt (nostrc-p15n5.3). Listing accounts
 * uses it that way; only lookup/store/delete load secrets. */
struct _GhNip46CredentialBackend {
  GPtrArray *(*search)(GhNip46CredentialBackend *self, const gchar *account,
                       gboolean interactive, gboolean load_secrets,
                       GCancellable *cancellable, GError **error);
  gboolean (*write)(GhNip46CredentialBackend *self, const gchar *account,
                    GBytes *secret, gboolean interactive, GError **error);
  gboolean (*remove)(GhNip46CredentialBackend *self, const gchar *account,
                     gboolean interactive, GError **error);
  void (*free)(GhNip46CredentialBackend *self);
};

GhNip46CredentialBackend *gh_nip46_credentials_secret_service_new(void);
/* Test seam: a store over an injected backend (takes ownership). */
GhNip46CredentialStore *gh_nip46_credential_store_new_with_backend(GhNip46CredentialBackend *backend);
#ifdef __APPLE__
#include <Security/Security.h>
GhNip46CredentialBackend *gh_nip46_credentials_keychain_new(SecKeychainRef keychain);
/* The temporary keychain must never be installed as the default/search list. */
GhNip46CredentialStore *gh_nip46_credential_store_new_keychain(SecKeychainRef keychain);
#endif

#endif
