#ifndef CRYPTO_UTILS_GOBJECT_H
#define CRYPTO_UTILS_GOBJECT_H

#include <glib-object.h>

/* Utility functions */
gchar *generate_private_key();
gchar *get_public_key(const gchar *sk);
gboolean is_valid_public_key_hex(const gchar *pk);
gboolean is_valid_public_key(const gchar *pk);

#endif // CRYPTO_UTILS_GOBJECT_H