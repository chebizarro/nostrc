#include "gnostr_paths.h"

gchar *gnostr_get_db_dir(void) {
  const char *override = g_getenv("GNOSTR_DB_DIR");
  if (override && *override) {
    return g_strdup(override);
  }

  return g_build_filename(g_get_user_cache_dir(), "gnostr", "ndb", NULL);
}