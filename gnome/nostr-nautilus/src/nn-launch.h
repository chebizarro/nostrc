/* nn-launch.h - start a helper from inside Nautilus
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NN_LAUNCH_H
#define NN_LAUNCH_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* A GAppInfo whose launch runs exactly @argv (see nn_exec_line()). */
GAppInfo *nn_launch_app_info (const gchar *const *argv, GError **error);

/* Launch @argv detached. Inside Nautilus the GDK launch context supplies
 * the startup-notification / xdg-activation token, so the helper's window
 * is raised instead of opening behind Files. */
gboolean  nn_launch_argv (const gchar *const *argv, GError **error);

/* Tests: intercept nn_launch_argv(). NULL restores real launching. */
typedef gboolean (*NnLaunchHook) (const gchar *const *argv, gpointer user_data);
void      nn_launch_set_hook (NnLaunchHook hook, gpointer user_data);

G_END_DECLS

#endif /* NN_LAUNCH_H */
