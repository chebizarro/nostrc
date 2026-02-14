#ifndef NIPS_NIP49_NOSTR_NIP49_NIP49_G_H
#define NIPS_NIP49_NOSTR_NIP49_NIP49_G_H

#include <glib.h>

G_BEGIN_DECLS

gboolean nostr_nip49_encrypt_g(const guint8 privkey32[32],
                               guint8 security_byte,
                               const gchar *password_utf8,
                               guint8 log_n,
                               gchar **out_ncryptsec,
                               GError **error);

gboolean nostr_nip49_decrypt_g(const gchar *ncryptsec_bech32,
                               const gchar *password_utf8,
                               guint8 out_privkey32[32],
                               guint8 *out_security_byte,
                               guint8 *out_log_n,
                               GError **error);

G_END_DECLS
#endif /* NIPS_NIP49_NOSTR_NIP49_NIP49_G_H */
