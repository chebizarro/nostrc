#include "crypto_utils_gobject.h"
#include "keys.h"
#include <glib.h>

/* Utility functions */
gchar *nostr_key_generate_private_g() {
    char *key = nostr_key_generate_private();
    return g_strdup(key);
}

gchar *nostr_key_get_public_g(const gchar *sk) {
    char *key = nostr_key_get_public(sk);
    return g_strdup(key);
}

gboolean nostr_key_is_valid_public_hex_g(const gchar *pk) {
    return nostr_key_is_valid_public_hex(pk);
}

gboolean nostr_key_is_valid_public_g(const gchar *pk) {
    return nostr_key_is_valid_public(pk);
}