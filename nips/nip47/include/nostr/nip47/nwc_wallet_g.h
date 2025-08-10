#pragma once

#include <glib.h>
#include "nwc_wallet.h"

G_BEGIN_DECLS

gpointer nostr_nwc_wallet_session_init_g(const gchar *client_pub_hex,
                                         const gchar **wallet_supported, gsize wallet_n,
                                         const gchar **client_supported, gsize client_n,
                                         GError **error);

void nostr_nwc_wallet_session_free_g(gpointer session);

gboolean nostr_nwc_wallet_build_response_g(gpointer session,
                                           const gchar *req_event_id,
                                           const gchar *result_type,
                                           const gchar *result_json,
                                           gchar **out_event_json,
                                           GError **error);

G_END_DECLS
