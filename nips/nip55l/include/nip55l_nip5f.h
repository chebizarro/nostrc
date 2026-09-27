/* nip55l_nip5f.h - the NIP-5F Unix socket, served through the same access
 * control as org.nostr.Signer (nostrc-q23h).
 *
 * Each connection's principal comes from the kernel (SO_PEERCRED /
 * SO_PEERPIDFD on Linux, getpeereid + LOCAL_PEERPID on macOS) and has the
 * same shape as the D-Bus principal of the same process, so grants in
 * $XDG_CONFIG_HOME/gnostr/signer-grants.ini apply to both transports, and a
 * call without a grant raises the same ApprovalRequested on the bus. Only
 * peers of the daemon's own uid are accepted.
 *
 * Opt-in: daemons start it only when NOSTR_SIGNER_ENDPOINT names a socket.
 * Start after signer_export(): prompts are raised through the exported
 * service. The GLib main context must be running (requests are decided on
 * it). */
#ifndef NIP55L_NIP5F_H
#define NIP55L_NIP5F_H

#include <glib.h>

G_BEGIN_DECLS

gboolean nip55l_nip5f_start(const gchar *socket_path, GError **error);
void     nip55l_nip5f_stop(void);

/* Serve a connection a transport accepted and authenticated itself (the
 * gnostr-signer-daemon TCP lane, after its token check, handshake done).
 * No kernel credentials: the caller is unidentified, so it is prompted every
 * time and nothing is remembered for it. Takes ownership of @fd when it
 * returns TRUE. */
gboolean nip55l_nip5f_serve_unidentified(int fd);

G_END_DECLS

#endif /* NIP55L_NIP5F_H */
