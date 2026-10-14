#ifndef GROTTO_CONTROL_H
#define GROTTO_CONTROL_H
#include "signer_approval_service.h"
#include "bunker_service.h"
G_BEGIN_DECLS
#define GROTTO_CONTROL_INTERFACE "org.nostr.Grotto.Control1"
#define GROTTO_CONTROL_PATH "/org/nostr/signer/grotto"
#define GROTTO_CONTROL_ERROR "org.nostr.Grotto.Control1.Error."
/* Private daemon API; same owner thread/context as SignerApprovalService. */
typedef struct _GrottoControl GrottoControl;
GrottoControl *grotto_control_new(GDBusConnection *connection,
    SignerApprovalService *approval, const gchar *config_home, GError **error);
void grotto_control_free(GrottoControl *control);
gboolean grotto_control_start_bunker(GrottoControl *control); /* after owning signer name */
/* Borrowed bunker state source. Unset before freeing the BunkerService. */
void grotto_control_set_bunker(GrottoControl *control, BunkerService *bunker, gboolean desired_enabled);
G_END_DECLS
#endif
