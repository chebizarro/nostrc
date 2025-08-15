#pragma once
#include <glib.h>
#include <libsecret/secret.h>

#define GNOSTR_SECRET_SCHEMA_NAME "org.gnostr.Key"

extern const SecretSchema gnostr_secret_schema;

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif
