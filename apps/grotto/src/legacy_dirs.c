/* legacy_dirs.c — see legacy_dirs.h. */
#include "legacy_dirs.h"

#include <errno.h>
#include <glib/gstdio.h>

/* The directory name before the rename (W31). The only place it may appear:
 * scripts/check-grotto-names.sh allows it here. */
#define GROTTO_LEGACY_DIR_NAME "gnostr" "-signer"
#define GROTTO_DIR_NAME "grotto"

GrottoLegacyDirResult
grotto_legacy_dir_migrate_in(const gchar *base)
{
  g_return_val_if_fail(base != NULL, GROTTO_LEGACY_DIR_NOTHING);
  g_autofree gchar *old_dir = g_build_filename(base, GROTTO_LEGACY_DIR_NAME, NULL);
  g_autofree gchar *new_dir = g_build_filename(base, GROTTO_DIR_NAME, NULL);

  /* A symlink is not followed: only a real directory is ours to move. */
  if (!g_file_test(old_dir, G_FILE_TEST_IS_DIR) || g_file_test(old_dir, G_FILE_TEST_IS_SYMLINK))
    return GROTTO_LEGACY_DIR_NOTHING;
  if (g_file_test(new_dir, G_FILE_TEST_EXISTS))
    return GROTTO_LEGACY_DIR_KEPT;

  if (g_rename(old_dir, new_dir) == 0) {
    g_message("grotto: moved %s to %s", old_dir, new_dir);
    return GROTTO_LEGACY_DIR_MOVED;
  }
  int saved = errno;
  /* Another Grotto process won the race: the new directory is there now. */
  if (g_file_test(new_dir, G_FILE_TEST_IS_DIR) && !g_file_test(old_dir, G_FILE_TEST_EXISTS))
    return GROTTO_LEGACY_DIR_MOVED;
  g_warning("grotto: could not move %s to %s: %s", old_dir, new_dir, g_strerror(saved));
  return GROTTO_LEGACY_DIR_FAILED;
}

void
grotto_legacy_dirs_migrate(void)
{
  grotto_legacy_dir_migrate_in(g_get_user_config_dir());
  grotto_legacy_dir_migrate_in(g_get_user_data_dir());
  grotto_legacy_dir_migrate_in(g_get_user_cache_dir());
}
