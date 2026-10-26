#ifndef CRYPTO_UTILS_GOBJECT_H
#define CRYPTO_UTILS_GOBJECT_H

#include "nostr-gobject-export.h"
#include <glib-object.h>

/* Utility functions */
NOSTR_GOBJECT_API gchar *nostr_key_generate_private_g(void);
NOSTR_GOBJECT_API gchar *nostr_key_get_public_g(const gchar *sk);
NOSTR_GOBJECT_API gboolean nostr_key_is_valid_public_hex_g(const gchar *pk);
NOSTR_GOBJECT_API gboolean nostr_key_is_valid_public_g(const gchar *pk);

#endif // CRYPTO_UTILS_GOBJECT_H