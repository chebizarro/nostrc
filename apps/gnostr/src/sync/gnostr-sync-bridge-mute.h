#ifndef GNOSTR_SYNC_BRIDGE_MUTE_H
#define GNOSTR_SYNC_BRIDGE_MUTE_H

#include <glib.h>

G_BEGIN_DECLS

gboolean gnostr_sync_bridge_reload_mute_list(const char *user_pubkey_hex);

G_END_DECLS

#endif /* GNOSTR_SYNC_BRIDGE_MUTE_H */
