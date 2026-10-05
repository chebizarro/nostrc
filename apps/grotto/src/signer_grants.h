/* signer_grants.h - remembered approval decisions, as the signer daemon
 * keeps them (nostrc-yjky).
 *
 * The daemon owns $XDG_CONFIG_HOME/gnostr/signer-grants.ini: it writes an
 * entry when ApproveRequest is sent with remember=TRUE and reads it on every
 * call, for D-Bus and NIP-5F callers alike. This UI never parses the file;
 * it lists and revokes entries through org.nostr.Signer ListGrants /
 * RevokeGrant, which only the approval UI may call. */
#ifndef APPS_GNOSTR_SIGNER_SIGNER_GRANTS_H
#define APPS_GNOSTR_SIGNER_SIGNER_GRANTS_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Human-readable name for a signer principal (ApprovalRequested app_id /
 * a grant's principal): flatpak:<id> | app:<id>;exe:<path> |
 * snap:<name>;exe:<path> | exe:<path> | https://<site> | claimed:<app_id>. */
gchar *signer_principal_display_name(const gchar *principal);

/* Fill @list (a GtkListBox) with one row per grant, each with a revoke
 * button; asynchronous, and again after each revoke. Rows show an error
 * when the signer cannot be asked (not running, or too old). */
void signer_grants_list_box_refresh(GtkListBox *list);

G_END_DECLS

#endif /* APPS_GNOSTR_SIGNER_SIGNER_GRANTS_H */
