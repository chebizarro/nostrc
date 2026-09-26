/* nseal-config.h — nostr-seal user defaults (~/.config/nostr/seal.conf).
 * SPDX-License-Identifier: MIT
 *
 * $NOSTR_SEAL_CONFIG, else $XDG_CONFIG_HOME/nostr/seal.conf. GKeyFile,
 * group [seal]; every key optional (data/seal.conf.example):
 *
 *   default_recipients  npub1…/64-hex list (';' or ',' separated) used by
 *                       `encrypt` when the command line names no
 *                       --to / --to-self / --passphrase
 *   include_self        also seal for the signer's identity in that case
 *   work_factor         scrypt log2(N) for --passphrase without
 *                       --work-factor (16..20; default 16)
 *
 * Written by org.nostr.Settings (Files page) or by hand. A malformed file
 * or recipient is an error, never silently skipped: sealing for the wrong
 * set of people is worse than refusing.
 */
#ifndef NSEAL_CONFIG_H
#define NSEAL_CONFIG_H

#include <glib.h>

G_BEGIN_DECLS

#define NSEAL_CONFIG_GROUP "seal"

typedef struct {
  gchar   *path;                /* file consulted (may not exist) */
  gboolean loaded;              /* file existed and parsed */
  gchar  **default_recipients;  /* never NULL; validated pubkeys as given */
  gboolean include_self;
  gint     work_factor;         /* 0 = built-in default */
} NsealConfig;

/* Missing file → defaults, no error. */
NsealConfig *nseal_config_load(GError **error);
void         nseal_config_free(NsealConfig *cfg);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(NsealConfig, nseal_config_free)

/* Default location (ignores $NOSTR_SEAL_CONFIG). */
gchar *nseal_config_default_path(void);

G_END_DECLS

#endif /* NSEAL_CONFIG_H */
