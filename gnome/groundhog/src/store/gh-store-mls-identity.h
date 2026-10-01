#ifndef GH_STORE_MLS_IDENTITY_H
#define GH_STORE_MLS_IDENTITY_H

#include "gh-store.h"

G_BEGIN_DECLS

/* What Groundhog learned about an encrypted-group member device whose leaf
 * carries no account proof (nostrc-6ukh): whether a KeyPackage the account
 * signed matches the device, and who added it. Kept in the account's
 * encrypted store (the `meta` table, one row per group member device under
 * GH_STORE_MLS_MEMBER_PREFIX + a SHA-256 of group, account and device key,
 * so the key names nobody), never in GSettings: who is in which group is
 * private (charter §2.2). Goes with the store. Store thread only. */

#define GH_STORE_MLS_MEMBER_PREFIX "mls-member:"

typedef enum {
  GH_STORE_MLS_MEMBER_UNCHECKED = 0, /* not looked up yet (added_by may be known) */
  GH_STORE_MLS_MEMBER_VERIFIED = 1,  /* a KeyPackage the account signed matches */
  GH_STORE_MLS_MEMBER_NOT_FOUND = 2  /* looked up, no matching KeyPackage */
} GhStoreMlsMemberCheck;

typedef struct {
  GhStoreMlsMemberCheck check;
  gint64 checked_at;   /* unix seconds of the check; 0: none */
  gchar *added_by;     /* lowercase hex of the admin who added the device, or NULL */
} GhStoreMlsMember;

void gh_store_mls_member_clear(GhStoreMlsMember *member);

/* The record of the device (lowercase hex: the MLS group id, the account,
 * the leaf's signature key). *found FALSE (and *out zeroed) without one. */
gboolean gh_store_mls_member_load(GhStore *store, const gchar *group_hex, const gchar *account,
                                  const gchar *signature_key, GhStoreMlsMember *out,
                                  gboolean *found, GError **error);
/* Records it, replacing any earlier record. */
gboolean gh_store_mls_member_save(GhStore *store, const gchar *group_hex, const gchar *account,
                                  const gchar *signature_key, const GhStoreMlsMember *member,
                                  GError **error);

G_END_DECLS
#endif
