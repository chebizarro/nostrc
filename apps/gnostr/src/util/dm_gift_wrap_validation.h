#pragma once

#include <glib.h>

typedef struct _NostrEvent NostrEvent;

G_BEGIN_DECLS

gboolean gnostr_dm_gift_wrap_parse_for_processing(const gchar *gift_wrap_json,
                                                  NostrEvent **out_gift_wrap,
                                                  gchar      **out_reason);

G_END_DECLS
