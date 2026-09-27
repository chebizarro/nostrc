#ifndef GH_ACCOUNT_CONTROLLER_H
#define GH_ACCOUNT_CONTROLLER_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum {
  GH_ACCOUNT_STATE_DISCOVERING,       /* first identity listing in progress */
  GH_ACCOUNT_STATE_STORE_UNAVAILABLE, /* signer-owned metadata could not be read */
  GH_ACCOUNT_STATE_NO_IDENTITIES,
  GH_ACCOUNT_STATE_UNSELECTED,
  GH_ACCOUNT_STATE_SELECTED_MISSING,  /* current-npub is not in the listed store */
  GH_ACCOUNT_STATE_ACTIVE
} GhAccountState;

typedef enum {
  GH_SIGNER_AVAILABILITY_UNKNOWN,
  GH_SIGNER_AVAILABILITY_NO_BUS,
  GH_SIGNER_AVAILABILITY_ABSENT,
  GH_SIGNER_AVAILABILITY_ACTIVATABLE, /* not running; the bus can start it */
  GH_SIGNER_AVAILABILITY_RUNNING
} GhSignerAvailability;

/* Runs in a worker thread and returns GhIdentityInfo items (gh-identity.h). */
typedef GPtrArray *(*GhAccountListFunc)(gpointer user_data, GError **error);

#define GH_TYPE_ACCOUNT_CONTROLLER (gh_account_controller_get_type())
G_DECLARE_FINAL_TYPE(GhAccountController, gh_account_controller, GH,
                     ACCOUNT_CONTROLLER, GObject)

/* Groundhog's active account. It reads only signer-owned public metadata,
 * writes only the org.nostr.Groundhog current-npub key, and watches whether
 * org.nostr.Signer is reachable; it never loads secrets. bus may be NULL.
 * Emits "changed" on the main context after every state update. */
GhAccountController *gh_account_controller_new(GSettings *settings,
                                               GDBusConnection *bus);
GhAccountController *gh_account_controller_new_full(GSettings *settings,
                                                    GDBusConnection *bus,
                                                    GhAccountListFunc list,
                                                    gpointer list_data);
/* Re-lists identities; a result from an older listing is discarded. */
void gh_account_controller_refresh(GhAccountController *self);
gboolean gh_account_controller_select(GhAccountController *self,
                                      const gchar *npub, GError **error);

GhAccountState gh_account_controller_get_state(GhAccountController *self);
GhSignerAvailability gh_account_controller_get_signer_availability(GhAccountController *self);
/* NULL unless the state is ACTIVE. */
const gchar *gh_account_controller_get_active_npub(GhAccountController *self);
/* Borrowed; NULL until a listing succeeds. */
GPtrArray *gh_account_controller_get_identities(GhAccountController *self);

/* The generation changes whenever the active account (or its absence)
 * changes. It is revoked before its cancellable is cancelled, so work bound
 * to either sees itself as stale; callbacks must check is_current.
 * This is local discard only: cancelling abandons a client reply, but it does
 * not revoke a request already pending approval in the signer service, which
 * may still be approved and executed there. */
guint64 gh_account_controller_get_generation(GhAccountController *self);
GCancellable *gh_account_controller_get_cancellable(GhAccountController *self);
gboolean gh_account_controller_is_current(GhAccountController *self,
                                          guint64 generation);

/* Why sending is unavailable, for the always-shown banner. No send path
 * exists in this build, so every result is a read-only or offline state. */
gchar *gh_account_describe_limits(GhAccountState state,
                                  GhSignerAvailability availability,
                                  const gchar *requested_method,
                                  gboolean network_available);

G_END_DECLS
#endif
