#ifndef NOSTR_GOBJECT_NOSTR_UTILS_H
#define NOSTR_GOBJECT_NOSTR_UTILS_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * gnostr_ensure_hex_pubkey:
 * @input: a pubkey in any format: 64-char hex, npub1..., or nprofile1...
 *
 * Defensive normalizer for pubkey strings. Accepts hex passthrough,
 * or decodes NIP-19 bech32 (npub1/nprofile1) to 64-char hex.
 *
 * Returns: (transfer full) (nullable): newly-allocated 64-char hex string,
 *   or %NULL if @input is invalid. Caller must g_free().
 */
gchar *gnostr_ensure_hex_pubkey(const char *input);

G_END_DECLS
#endif /* NOSTR_GOBJECT_NOSTR_UTILS_H */
