/* key-migration-event.h - unsigned key-migration announcement builder.
 *
 * Custody (which key signs it) and publication are the caller's policy. */
#ifndef APPS_GROTTO_KEY_MIGRATION_EVENT_H
#define APPS_GROTTO_KEY_MIGRATION_EVENT_H

#include <glib.h>

G_BEGIN_DECLS

/* Migration event kind per the NIP-41 draft. */
#define KEY_MIGRATION_EVENT_KIND 1776

/* Build an unsigned migration event authored by @old_pubkey_hex that names
 * @new_pubkey_hex as its successor. @created_at 0 means now; NULL or empty
 * @content uses a default message. Returns (transfer full) JSON, or NULL. */
gchar *key_rotation_build_migration_event(const gchar *old_pubkey_hex,
                                          const gchar *new_pubkey_hex,
                                          gint64 created_at,
                                          const gchar *content);

G_END_DECLS
#endif /* APPS_GROTTO_KEY_MIGRATION_EVENT_H */
