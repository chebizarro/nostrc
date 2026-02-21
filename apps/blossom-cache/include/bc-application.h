/*
 * bc-application.h - BcApplication: GApplication subclass for the Blossom Cache daemon
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef BC_APPLICATION_H
#define BC_APPLICATION_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define BC_TYPE_APPLICATION (bc_application_get_type())
G_DECLARE_FINAL_TYPE(BcApplication, bc_application, BC, APPLICATION, GApplication)

/**
 * bc_application_new:
 *
 * Creates a new BcApplication instance.
 *
 * Returns: (transfer full): a new #BcApplication
 */
BcApplication *bc_application_new(void);

G_END_DECLS

#endif /* BC_APPLICATION_H */
