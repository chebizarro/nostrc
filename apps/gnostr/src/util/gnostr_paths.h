#pragma once

#include <glib.h>

/* Returns DB directory path. Uses GNOSTR_DB_DIR env if set, else ~/.cache/gnostr/ndb. Caller frees. */
gchar *gnostr_get_db_dir(void);