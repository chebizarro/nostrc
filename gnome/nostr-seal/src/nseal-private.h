/* nseal-private.h — internals shared by the CLI, GUI and tests.
 * SPDX-License-Identifier: MIT */
#ifndef NSEAL_PRIVATE_H
#define NSEAL_PRIVATE_H

#include "nostr-seal.h"

G_BEGIN_DECLS

gboolean nseal_write_all(int fd, const void *buf, gsize len, GError **error);

/* Open one NIP-44 key stanza payload with an already-derived conversation
 * key (the signer lane derives it via NIP44DeriveConversationKey). */
gboolean nseal_unwrap_with_convkey(const uint8_t convkey[32], const char *payload,
                                   uint8_t out_file_key[NSEAL_FILE_KEY_LEN],
                                   GError **error);

/* Output file written to a 0600 temp in the destination directory and
 * renamed into place only on success (removed otherwise). path "-" is
 * stdout. private_mode keeps 0600; otherwise 0666 & ~umask. */
typedef struct {
  int fd;
  char *final_path;   /* NULL → stdout */
  char *tmp_path;
} NsealOutput;

gboolean nseal_output_open(NsealOutput *o, const char *path, gboolean force,
                           gboolean private_mode, GError **error);
gboolean nseal_output_close(NsealOutput *o, gboolean success, GError **error);

G_END_DECLS

#endif
