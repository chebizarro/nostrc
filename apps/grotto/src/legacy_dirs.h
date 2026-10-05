/* legacy_dirs.h — one-time move of the pre-rename per-user directories.
 *
 * Until W31 (nostrc-8otj) Grotto had another name and kept its files in
 * directories of that name (GROTTO_LEGACY_DIR_NAME in legacy_dirs.c) under
 * the XDG config, data and cache homes. On first start under the new name each of those is renamed to
 * "grotto", so accounts, delegations, event history and multisig state carry
 * over. Stored keys are not involved: their Secret Service and Keychain
 * identifiers did not change.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef GROTTO_LEGACY_DIRS_H
#define GROTTO_LEGACY_DIRS_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  GROTTO_LEGACY_DIR_NOTHING, /* no old directory: nothing to do */
  GROTTO_LEGACY_DIR_MOVED,   /* old renamed to new */
  GROTTO_LEGACY_DIR_KEPT,    /* both exist: the new one wins, the old is left alone */
  GROTTO_LEGACY_DIR_FAILED,  /* the rename failed; the old directory is untouched */
} GrottoLegacyDirResult;

/* Moves base/<old name> to base/grotto when only the old one exists. */
GrottoLegacyDirResult grotto_legacy_dir_migrate_in(const gchar *base);

/* The same for the user's config, data and cache homes. Call first thing in
 * every Grotto process, before anything reads those directories. Safe to call
 * from several processes at once: the rename is atomic and the loser sees
 * the new directory. */
void grotto_legacy_dirs_migrate(void);

G_END_DECLS

#endif /* GROTTO_LEGACY_DIRS_H */
