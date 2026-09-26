#ifndef GNOME_SEAHORSE_SECRET_STORE_H
#define GNOME_SEAHORSE_SECRET_STORE_H
#include <glib.h>
#ifdef GNOSTR_HAVE_LIBSECRET
#include <libsecret/secret.h>
#endif

#define GNOSTR_SECRET_SCHEMA_NAME "org.gnostr.Key"

#ifdef GNOSTR_HAVE_LIBSECRET
extern const SecretSchema gnostr_secret_schema;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Build the Seahorse-visible item label documented in
 * org.gnostr.secret.schema.txt: "Nostr key: <uid> (<npub prefix>…)", or
 * "Nostr key: <npub prefix>…" when uid is NULL/empty. The full npub is kept
 * in the item attributes; the label abbreviates it so Seahorse's list stays
 * readable. Returns NULL when npub is NULL/empty. Free with g_free(). */
gchar *gnostr_secret_store_build_label(const gchar *uid, const gchar *npub);

#ifdef GNOSTR_HAVE_LIBSECRET
gboolean gnostr_secret_store_save_software_key(const gchar *npub,
                                                const gchar *uid,
                                                const gchar *secret, /* hex or nsec */
                                                GError **error);

GHashTable *gnostr_secret_store_find_all(GError **error);

/* Delete secrets matching provided identity attributes. If npub or uid are NULL/empty,
 * they are ignored in the match. Returns TRUE if all matched items were deleted,
 * FALSE on any error. */
gboolean gnostr_secret_store_delete_by_identity(const gchar *npub,
                                                const gchar *uid,
                                                GError **error);
#endif

#ifdef __cplusplus
}
#endif
#endif /* GNOME_SEAHORSE_SECRET_STORE_H */
