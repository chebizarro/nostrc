/* nd-application.h - NostrDav application lifecycle
 *
 * SPDX-License-Identifier: MIT
 *
 * GApplication subclass that owns the DAV server, token store,
 * and D-Bus activation lifecycle. Runs as a systemd --user daemon.
 */
#ifndef ND_APPLICATION_H
#define ND_APPLICATION_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define ND_TYPE_APPLICATION (nd_application_get_type())
G_DECLARE_FINAL_TYPE(NdApplication, nd_application, ND, APPLICATION, GApplication)

NdApplication *nd_application_new(void);

G_END_DECLS
#endif /* ND_APPLICATION_H */
