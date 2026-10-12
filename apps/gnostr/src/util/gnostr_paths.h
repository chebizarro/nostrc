#ifndef APPS_GNOSTR_UTIL_GNOSTR_PATHS_H
#define APPS_GNOSTR_UTIL_GNOSTR_PATHS_H

#include <glib.h>

/* Returns DB directory path. Uses GNOSTR_DB_DIR env if set, else
 * $XDG_CACHE_HOME/gnostr/ndb. Caller frees. Under test
 * (NOSTRC_TEST_ISOLATION) aborts rather than return the real profile's. */
gchar *gnostr_get_db_dir(void);

/* Start-up check: under test (NOSTRC_TEST_ISOLATION) abort when the XDG
 * directories are the real profile or the default GSettings backend writes
 * outside them (nostrc-wqn0u). No-op otherwise. */
void gnostr_assert_private_profile(void);
#endif /* APPS_GNOSTR_UTIL_GNOSTR_PATHS_H */
