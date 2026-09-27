#ifndef GH_IDENTITY_H
#define GH_IDENTITY_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct {
  gchar *npub;
  gchar *label;
} GhIdentityInfo;

void gh_identity_info_free(GhIdentityInfo *info);
/* Signer-owned Secret Service attributes only. Never loads or unlocks secrets. */
GPtrArray *gh_identity_list(GError **error);
/* Selects only a listed identity in org.nostr.Groundhog, never Gnostr settings. */
gboolean gh_identity_select(GSettings *settings, const gchar *npub, GError **error);
gboolean gh_identity_select_from_list(GSettings *settings, GPtrArray *identities,
                                       const gchar *npub, GError **error);
/* A valid npub maps to its lowercase x-only public key. */
gchar *gh_identity_pubkey_hex(const gchar *npub);

G_END_DECLS
#endif
