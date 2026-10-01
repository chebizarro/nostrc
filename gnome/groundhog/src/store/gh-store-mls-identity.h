#ifndef GH_STORE_MLS_IDENTITY_H
#define GH_STORE_MLS_IDENTITY_H

#include "gh-store.h"

G_BEGIN_DECLS

/* What Groundhog learned about an encrypted-group member device whose leaf
 * carries no account proof (nostrc-6ukh): whether a KeyPackage the account
 * signed matches the device, and who added it; and an admin's Commit the
 * group was refused at (nostrc-prrl). Kept in the account's encrypted store
 * (the `meta` table), never in GSettings: who is in which group is private
 * (charter §2.2). Keys name nobody: GH_STORE_MLS_MEMBER_PREFIX + SHA-256 of
 * the group + "/" + SHA-256 of group, account and device key, and
 * GH_STORE_MLS_REFUSED_PREFIX + SHA-256 of the group. A device's row goes
 * when it leaves the group, all of a group's when the group ends
 * (gh_store_mls_member_forget_group(); W24 review N3). Store thread only. */

#define GH_STORE_MLS_MEMBER_PREFIX "mls-member:"
#define GH_STORE_MLS_REFUSED_PREFIX "mls-refused:"
#define GH_STORE_MLS_ROUTING_PREFIX "mls-routing:"

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
/* Forgets one device (it left the group). */
gboolean gh_store_mls_member_delete(GhStore *store, const gchar *group_hex, const gchar *account,
                                    const gchar *signature_key, GError **error);
/* Forgets every device record, the refused Commit and the routing record of
 * a group. */
gboolean gh_store_mls_member_forget_group(GhStore *store, const gchar *group_hex,
                                          GError **error);

/* The admin's Commit (signed kind 445 JSON) the group was refused at for
 * good, and why (nostrc-prrl, W24 review L2): `cause` 0 a proof that does
 * not verify, 1 a member without a proof while the account required proofs,
 * 2 a change libmarmot cannot follow (W24b slice H review L2,
 * MARMOT_ERR_COMMIT_REFUSED).  Kept until a later Commit moves the group
 * on.  json NULL deletes it.  load: *json NULL without one; a record of
 * another cause is not read. */
gboolean gh_store_mls_refused_save(GhStore *store, const gchar *group_hex, const gchar *json,
                                   guint cause, GError **error);
gboolean gh_store_mls_refused_load(GhStore *store, const gchar *group_hex, gchar **json,
                                   guint *cause, GError **error);

/* The routing addresses an adopted group left and that Groundhog still reads
 * for backfill (nostrc-ms4d): an opaque record of GhMlsService's, at most
 * 64 KiB of UTF-8, under GH_STORE_MLS_ROUTING_PREFIX + SHA-256 of the group.
 * Saving NULL forgets it; loading gives NULL without one. The group's
 * forget_group() forgets it too. */
gboolean gh_store_mls_routing_save(GhStore *store, const gchar *group_hex, const gchar *record,
                                   GError **error);
gboolean gh_store_mls_routing_load(GhStore *store, const gchar *group_hex, gchar **record,
                                   GError **error);

G_END_DECLS
#endif
