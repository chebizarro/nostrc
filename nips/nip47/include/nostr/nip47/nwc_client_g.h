#pragma once

#include <glib.h>
#include "nwc_client.h"

G_BEGIN_DECLS

gpointer nostr_nwc_client_session_init_g(const gchar *wallet_pub_hex,
                                         const gchar **client_supported, gsize client_n,
                                         const gchar **wallet_supported, gsize wallet_n,
                                         GError **error);

void nostr_nwc_client_session_free_g(gpointer session);

gboolean nostr_nwc_client_build_request_g(gpointer session,
                                          const gchar *method,
                                          const gchar *params_json,
                                          gchar **out_event_json,
                                          GError **error);

G_END_DECLS
