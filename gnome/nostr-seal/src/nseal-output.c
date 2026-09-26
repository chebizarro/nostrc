/* nseal-output.c — atomic output file (temp in the target dir + rename).
 * SPDX-License-Identifier: MIT */

#include "nseal-private.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <glib/gstdio.h>

gboolean nseal_output_open(NsealOutput *o, const char *path, gboolean force, gboolean private_mode,
                            GError **error) {
  memset(o, 0, sizeof *o);
  if (g_str_equal(path, "-")) { o->fd = STDOUT_FILENO; return TRUE; }
  if (!force && g_file_test(path, G_FILE_TEST_EXISTS)) {
    g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_ARG, "%s exists (use --force to overwrite)", path);
    return FALSE;
  }
  g_autofree char *dir = g_path_get_dirname(path);
  g_autofree char *base = g_path_get_basename(path);
  o->tmp_path = g_strdup_printf("%s/.%s.XXXXXX", dir, base);
  o->fd = g_mkstemp_full(o->tmp_path, O_RDWR | O_CLOEXEC, 0600);
  if (o->fd < 0) {
    int e = errno;
    g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_IO, "cannot create %s: %s", o->tmp_path, g_strerror(e));
    g_clear_pointer(&o->tmp_path, g_free);
    return FALSE;
  }
  if (!private_mode) {
    mode_t um = umask(0); umask(um);
    if (fchmod(o->fd, 0666 & ~um) != 0) { /* stays 0600: stricter, harmless */ }
  }
  o->final_path = g_strdup(path);
  return TRUE;
}

gboolean nseal_output_close(NsealOutput *o, gboolean success, GError **error) {
  gboolean ok = success;
  if (o->final_path) {
    if (ok && fsync(o->fd) != 0) {
      int e = errno;
      g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_IO, "fsync: %s", g_strerror(e));
      ok = FALSE;
    }
    close(o->fd);
    if (ok && g_rename(o->tmp_path, o->final_path) != 0) {
      int e = errno;
      g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_IO, "rename to %s: %s", o->final_path, g_strerror(e));
      ok = FALSE;
    }
    if (!ok) (void)g_unlink(o->tmp_path);
  }
  g_free(o->final_path);
  g_free(o->tmp_path);
  return ok;
}

