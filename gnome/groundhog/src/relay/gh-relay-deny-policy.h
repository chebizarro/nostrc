#ifndef GH_RELAY_DENY_POLICY_H
#define GH_RELAY_DENY_POLICY_H

#include <gio/gio.h>

G_BEGIN_DECLS
#define GH_TYPE_RELAY_DENY_POLICY (gh_relay_deny_policy_get_type())
G_DECLARE_FINAL_TYPE(GhRelayDenyPolicy, gh_relay_deny_policy, GH, RELAY_DENY_POLICY, GObject)
#define GH_RELAY_DENY_ERROR (gh_relay_deny_error_quark())
#define GH_RELAY_DENY_MESSAGE "Blocked by this account's local relay policy"
GQuark gh_relay_deny_error_quark(void);
/* Process-wide boundary, owned by the account list service. Disabled until the
 * first account binding; afterwards a revoked generation fails closed. Signal:
 * changed (), after installing a new immutable URL snapshot. */
GhRelayDenyPolicy *gh_relay_deny_policy_get_default(void); /* borrowed */
void gh_relay_deny_policy_bind(GhRelayDenyPolicy *self, guint64 generation,
                                const gchar *const *urls);
void gh_relay_deny_policy_revoke(GhRelayDenyPolicy *self);
gboolean gh_relay_deny_policy_check(guint64 generation, const gchar *url,
                                     gboolean signer_control, GError **error);
/* Canonical endpoint identity, retaining path/query distinctions. */
gchar *gh_relay_deny_policy_normalize(const gchar *url);
G_END_DECLS
#endif
