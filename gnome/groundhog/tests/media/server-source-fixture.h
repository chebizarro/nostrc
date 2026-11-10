#ifndef GH_TEST_SERVER_SOURCE_FIXTURE_H
#define GH_TEST_SERVER_SOURCE_FIXTURE_H
#include "gh-blossom-client.h"

/* Explicit per-fixture configuration for transport/media tests. The production
 * client never reads the deprecated key; real account storage/import is tested
 * separately against GhSocialLists and SQLCipher. */
static inline GStrv gh_test_server_source_dup(GObject *source)
{
  return g_settings_get_strv(G_SETTINGS(source), "blossom-servers");
}
static inline gboolean gh_test_server_source_save(GObject *source,
  const gchar *const *servers, GError **error)
{
  if (g_settings_set_strv(G_SETTINGS(source), "blossom-servers", servers)) return TRUE;
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Fixture destinations could not be saved");
  return FALSE;
}
static inline void gh_test_client_server_source(GhBlossomClient *client, GSettings *settings)
{
  gh_blossom_client_set_server_source(client, G_OBJECT(settings),
                                    gh_test_server_source_dup, gh_test_server_source_save);
}
#endif
