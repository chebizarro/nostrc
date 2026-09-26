/* nn-launch.c - start a helper from inside Nautilus
 *
 * SPDX-License-Identifier: MIT
 */
#include "nn-launch.h"
#include "nn-core.h"

#ifdef NN_HAVE_GDK
#include <gdk/gdk.h>
#endif

static NnLaunchHook launch_hook;
static gpointer     launch_hook_data;

void
nn_launch_set_hook(NnLaunchHook hook, gpointer user_data)
{
  launch_hook = hook;
  launch_hook_data = user_data;
}

GAppInfo *
nn_launch_app_info(const gchar *const *argv, GError **error)
{
  g_return_val_if_fail(argv != NULL && argv[0] != NULL, NULL);
  g_autofree gchar *line = nn_exec_line(argv);
  g_autofree gchar *name = g_path_get_basename(argv[0]);
  /* create_from_commandline appends " %f"; launched with no files it
   * expands to nothing. */
  return g_app_info_create_from_commandline(line, name,
                                            G_APP_INFO_CREATE_SUPPORTS_STARTUP_NOTIFICATION,
                                            error);
}

gboolean
nn_launch_argv(const gchar *const *argv, GError **error)
{
  if (launch_hook != NULL)
    return launch_hook(argv, launch_hook_data);

  g_autoptr(GAppInfo) app = nn_launch_app_info(argv, error);
  if (app == NULL)
    return FALSE;

  g_autoptr(GAppLaunchContext) ctx = NULL;
#ifdef NN_HAVE_GDK
  GdkDisplay *display = gdk_display_get_default();
  if (display != NULL)
    ctx = G_APP_LAUNCH_CONTEXT(gdk_display_get_app_launch_context(display));
#endif
  return g_app_info_launch(app, NULL, ctx, error);
}
