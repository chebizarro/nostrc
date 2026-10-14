#ifndef NIP55L_SIGNER_PERMISSIONS_H
#define NIP55L_SIGNER_PERMISSIONS_H
#include <gio/gio.h>
G_BEGIN_DECLS

typedef struct _SignerPermissions SignerPermissions;
typedef enum { SIGNER_TRUST_LOW, SIGNER_TRUST_MEDIUM, SIGNER_TRUST_FULL } SignerTrustTier;
typedef enum { SIGNER_PERMISSION_ASK, SIGNER_PERMISSION_ALLOW, SIGNER_PERMISSION_DENY,
               SIGNER_PERMISSION_ERROR } SignerPermissionResult;
/* Called only on the owning thread. Unknown clients must return Low with
 * first_consented=FALSE. Method/transport admission remains the adapter's ceiling. */
typedef SignerTrustTier (*SignerTrustLookup)(const gchar *principal,
                                            gboolean *first_consented, gpointer data);
typedef void (*SignerPermissionsChanged)(gpointer data);

/* All mutation, reload, list and evaluation calls on the owning thread.
 * revision() is thread-safe for worker admission; mutation changes it before
 * invoking the service invalidation callback. Paths are always profile-guarded. */
/* NULL config_home uses the process XDG config directory; an explicit root
 * supports independent profiles/fixtures without mutating GLib's cached paths. */
SignerPermissions *signer_permissions_new(const gchar *config_home);
void signer_permissions_free(SignerPermissions *permissions);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SignerPermissions, signer_permissions_free)
gboolean signer_permissions_reload(SignerPermissions *permissions, GError **error);
guint64 signer_permissions_revision(SignerPermissions *permissions);
/* Worker freshness fence: no callbacks/I/O. A changed revision or elapsed
 * absolute expiry requires an owner-thread recheck, not blanket cancellation. */
gboolean signer_permissions_revision_is_current(SignerPermissions *permissions, guint64 revision);
void signer_permissions_set_changed(SignerPermissions *permissions,
                                     SignerPermissionsChanged fn, gpointer data);
void signer_permissions_set_trust_lookup(SignerPermissions *permissions,
                                         SignerTrustLookup fn, gpointer data);
/* G04 calls this after a live tier/client-admission change. */
void signer_permissions_policy_changed(SignerPermissions *permissions);
gboolean signer_permissions_set_required_confirmation(SignerPermissions *permissions,
    const guint32 *kinds, gsize count, GError **error);
SignerPermissionResult signer_permissions_evaluate(SignerPermissions *permissions,
    const gchar *principal, const gchar *identity, const gchar *operation,
    gint event_kind, gboolean first_party_default, GError **error);
/* Ephemeral flow consent. The opaque request ID reserves one slot per scope;
 * repeated admission/result checks reuse that reservation. No disk/list rows. */
#ifdef NIP55L_TEST_TRUST_ENV
/* Initialize before admitting work; the fixture clock must be thread-safe. */
void signer_permissions_set_flow_clock_for_test(SignerPermissions *permissions,
    gint64 (*clock)(gpointer), gpointer data);
#endif
gboolean signer_permissions_kind_requires_confirmation(SignerPermissions *permissions, gint kind);
/* Atomically persist selected non-required scopes and install optional seal flow consent. */
gboolean signer_permissions_consent_room(SignerPermissions *permissions,
    const gchar *principal, const gchar *identity, guint transport,
    const gchar *flow_id, guint flow_size, gboolean encrypt, gboolean seals,
    guint64 ttl_seconds, GError **error);
SignerPermissionResult signer_permissions_evaluate_flow(SignerPermissions *permissions,
    const gchar *principal, const gchar *identity, guint transport,
    const gchar *operation, gint event_kind, const gchar *flow_id, guint flow_size,
    const gchar *request_id, gboolean first_party_default, GError **error);
/* Rows are aa{sv}: principal, identity, operation, event-kind:i (-1 means
 * operation-wide), decision, expires-at:t, state (explicit|needs-review),
 * legacy-scope for review rows. These are explicit rows; effective tier policy
 * is available through evaluate(), not expanded into persisted grants. */
GVariant *signer_permissions_list(SignerPermissions *permissions,
                                  const gchar *principal, GError **error);
gboolean signer_permissions_needs_review(SignerPermissions *permissions,
                                         const gchar *principal, GError **error);
/* Complete replacement for one principal/identity pair, using list row shape.
 * Expected revision must match; failure leaves in-memory authority unchanged.
 * Error codes use G_IO_ERROR (BUSY = revision Conflict). */
gboolean signer_permissions_replace(SignerPermissions *permissions,
    const gchar *principal, const gchar *identity, GVariant *rows,
    guint64 expected_revision, GError **error);
gboolean signer_permissions_remember(SignerPermissions *permissions,
    const gchar *principal, const gchar *identity, const gchar *operation,
    gint event_kind, gboolean allow, guint64 ttl_seconds, GError **error);
/* NULL identity/operation clears all matching rows for the principal.
 * operation="event" clears every event-kind plus the needs-review row. */
gboolean signer_permissions_clear(SignerPermissions *permissions,
    const gchar *principal, const gchar *identity, const gchar *operation,
    gboolean *removed, GError **error);
GVariant *signer_permissions_list_legacy(SignerPermissions *permissions, GError **error);
G_END_DECLS
#endif
