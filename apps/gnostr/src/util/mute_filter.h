#ifndef GNOSTR_MUTE_FILTER_H
#define GNOSTR_MUTE_FILTER_H

#include <glib.h>
#include <nostr-gobject-1.0/gnostr-mute-list.h>

G_BEGIN_DECLS

gboolean gnostr_mute_filter_should_hide_fields(GNostrMuteList *mute_list,
                                               const char *pubkey_hex,
                                               const char *content,
                                               char * const *hashtags);

G_END_DECLS

#endif /* GNOSTR_MUTE_FILTER_H */
