#ifndef GH_MLS_IMETA_H
#define GH_MLS_IMETA_H

/* An encrypted group message's attachments as display data (W25,
 * nostrc-q3a6). GTK-free and network-free: libmarmot's strict MIP-04
 * encrypted-media-v2 imeta parser (marmot_media_imeta_parse(), the default
 * policy) over the inner event's tags, in tag order; a tag it rejects is
 * counted and skipped alone, never invalidating the message. The MLS service
 * calls it wherever an MLS GhMessage is made (received, sent, listed from the
 * store), so the conversation UI shows a card per file without libmarmot,
 * and the store binds each file's decrypted copy to its message
 * (GhStoreMessage.media_ids). Opening a file is gh-mls-media.h's. */

#include "gh-message.h"

#include <marmot/marmot.h>

G_BEGIN_DECLS

/* At most this many attachments are read from one message (MDK sends one
 * imeta per file; GH_STORE_MAX_MLS_MEDIA). Further tags count as rejected. */
#define GH_MLS_IMETA_MAX_ATTACHMENTS 16

/* Sets message's source epoch (has_epoch: libmarmot authenticated one) and
 * its attachments from its inner event (gh_message_set_attachments()). Each
 * attachment's file_id is gh_store_mls_media_file_id() for group_id_hex and
 * that epoch (NULL without an epoch). */
void gh_mls_imeta_describe(GhMessage *message, const gchar *group_id_hex, gboolean has_epoch,
                           guint64 epoch);

/* The cache identity of one parsed reference (transfer full; NULL for
 * malformed input). */
gchar *gh_mls_imeta_file_id(const MarmotMediaReference *reference, const gchar *group_id_hex,
                            guint64 epoch);

/* The file_ids of message's attachments, NULL-terminated (transfer full), or
 * NULL when it has none with an identity. */
GStrv gh_mls_imeta_dup_file_ids(GhMessage *message);

G_END_DECLS
#endif
