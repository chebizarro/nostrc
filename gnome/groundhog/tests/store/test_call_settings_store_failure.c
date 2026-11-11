/* Fault injection at libsecret's documented boolean result boundary. This
 * complements, rather than replaces, the real isolated-backend roundtrip. */
#include "gh-call-settings.h"
#include <libsecret/secret.h>

/* Resolve the production library's libsecret call to a dismissed prompt:
 * FALSE with no GError. Never contacts a Secret Service or a Keychain. */
void
secret_password_store(const SecretSchema *schema, const gchar *collection,
                      const gchar *label, const gchar *password,
                      GCancellable *cancellable, GAsyncReadyCallback callback,
                      gpointer user_data, ...)
{
  (void)schema; (void)collection; (void)label; (void)password;
  g_autoptr(GTask) task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_return_boolean(task, FALSE);
}

gboolean
secret_password_store_finish(GAsyncResult *result, GError **error)
{
  return g_task_propagate_boolean(G_TASK(result), error);
}

static void
done(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  g_autoptr(GError) error = NULL;
  g_assert_false(gh_call_turn_password_store_finish(result, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_main_loop_quit(data);
}

static gboolean
expired(gpointer data)
{
  (void)data;
  g_error("TURN failed-store callback timed out");
  return G_SOURCE_REMOVE;
}

int
main(int argc, char **argv)
{
  g_setenv("NOSTRC_TEST_ISOLATION", "1", TRUE);
  g_test_init(&argc, &argv, NULL);
  g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
  guint timeout = g_timeout_add_seconds(10, expired, NULL);
  gh_call_turn_password_store_async("turn://turn.example.net:3478", "alice", "not-saved",
                                    NULL, done, loop);
  g_main_loop_run(loop);
  g_source_remove(timeout);
  return 0;
}
