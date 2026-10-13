#ifndef GROTTO_CLIENTS_H
#define GROTTO_CLIENTS_H
#include "signer_permissions.h"
G_BEGIN_DECLS
/* Daemon owner-thread API. No record is created by a failed/unconsented call.
 * Admission and signing permissions are separate; paired never grants signing. */
typedef struct _GrottoClients GrottoClients;
GrottoClients *grotto_clients_new(const gchar *config_home, GError **error);
void grotto_clients_free(GrottoClients *clients);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GrottoClients, grotto_clients_free)
GVariant *grotto_clients_list(GrottoClients *clients);
gboolean grotto_clients_principal_valid(const gchar *principal);
SignerTrustTier grotto_clients_trust(const gchar *principal, gboolean *consented, gpointer clients);
/* NULL metadata preserves observed values (permission-only edits). */
gboolean grotto_clients_record(GrottoClients *clients, const gchar *principal,
    const gchar *transport, const gchar *claimed_name, const gchar *identity,
    gboolean consented, gboolean used, GError **error);
gboolean grotto_clients_set_label(GrottoClients *clients, const gchar *principal,
    const gchar *label, GError **error);
gboolean grotto_clients_set_tier(GrottoClients *clients, const gchar *principal,
    SignerTrustTier tier, GError **error);
/* G06 persists signature-authenticated pairing admission, including relays.
 * Requires an existing explicitly consented record; never creates admission. */
gboolean grotto_clients_set_pairing(GrottoClients *clients, const gchar *principal,
    gboolean paired, const gchar *const *relays, GError **error);
gboolean grotto_clients_remove(GrottoClients *clients, const gchar *principal, GError **error);
guint64 grotto_clients_revision(GrottoClients *clients);
G_END_DECLS
#endif
