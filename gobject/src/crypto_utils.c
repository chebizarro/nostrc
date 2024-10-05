#include "crypto_utils_gobject.h"
#include "crypto_utils.h"
#include <glib.h>

/* Utility functions */
gchar *generate_private_key() {
    char *key = generate_private_key();
    return g_strdup(key);
}

gchar *get_public_key(const gchar *sk) {
    char *key = get_public_key(sk);
    return g_strdup(key);
}

gboolean is_valid_public_key_hex(const gchar *pk) {
    return is_valid_public_key_hex(pk);
}

gboolean is_valid_public_key(const gchar *pk) {
    return is_valid_public_key(pk);
}