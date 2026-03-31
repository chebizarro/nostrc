#pragma once

#include <glib.h>

G_BEGIN_DECLS

gboolean gnostr_profile_event_extract_for_apply(const gchar *event_json,
                                                gchar      **out_pubkey_hex,
                                                gchar      **out_content_json,
                                                gint64      *out_created_at,
                                                gchar      **out_reason);

G_END_DECLS
