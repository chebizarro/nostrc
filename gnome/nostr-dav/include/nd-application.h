/* nd-application.h - NostrDav application lifecycle
 *
 * SPDX-License-Identifier: MIT
 *
 * GApplication service that owns the DAV server, token store, and
 * store database. Runs under the nostr-dav.service systemd user unit.
 */
#ifndef ND_APPLICATION_H
#define ND_APPLICATION_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define ND_TYPE_APPLICATION (nd_application_get_type())
G_DECLARE_FINAL_TYPE(NdApplication, nd_application, ND, APPLICATION, GApplication)

NdApplication *nd_application_new(void);

/**
 * nd_application_get_exit_status:
 *
 * Returns: non-zero if startup was refused (g_application_run() itself
 *   returns 0 in that case).
 */
int nd_application_get_exit_status(NdApplication *self);

G_END_DECLS
#endif /* ND_APPLICATION_H */
