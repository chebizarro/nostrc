#ifndef CRYPTO_UTILS_GOBJECT_H
#define CRYPTO_UTILS_GOBJECT_H

#include <glib-object.h>

/* Utility functions */
gchar *nostr_key_generate_private_g(void);
gchar *nostr_key_get_public_g(const gchar *sk);
gboolean nostr_key_is_valid_public_hex_g(const gchar *pk);
gboolean nostr_key_is_valid_public_g(const gchar *pk);

#endif // CRYPTO_UTILS_GOBJECT_H