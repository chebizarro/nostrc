#include "gnostr_paths.h"

#define G_SETTINGS_ENABLE_BACKEND
#include <gio/gsettingsbackend.h>
#include <nostrc-profile-guard.h>

gchar *gnostr_get_db_dir(void) {
  const char *override = g_getenv("GNOSTR_DB_DIR");
  gchar *dir = override && *override
                 ? g_strdup(override)
                 : g_build_filename(g_get_user_cache_dir(), "gnostr", "ndb", NULL);
  /* nostrc-wqn0u: under test, never the owner's note database. */
  if (nostrc_profile_guard_refuses_path(dir))
    g_error("gnostr: the NDB directory %s is the real profile; refusing it under test "
            "(NOSTRC_TEST_ISOLATION)", dir);
  return dir;
}

void gnostr_assert_private_profile(void) {
  nostrc_profile_guard_assert_user_dirs("gnostr");
  GSettingsBackend *backend = g_settings_backend_get_default();
  const gchar *type = G_OBJECT_TYPE_NAME(backend);
  gboolean allowed = nostrc_profile_guard_settings_backend_allowed_for(
      nostrc_profile_guard_under_test(), type);
  g_object_unref(backend);
  if (!allowed)
    g_error("gnostr: the GSettings backend %s persists to the real profile; refusing it "
            "under test (NOSTRC_TEST_ISOLATION)", type);
}
