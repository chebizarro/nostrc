/* SPDX-License-Identifier: GPL-3.0-or-later
 * Review B1: a Commit is merged only after a relay accepted it. */

#include "gn-mls-commit-publish.h"
#include <string.h>

typedef struct {
  const char *accepting;   /* relay that answers OK; others refuse */
  GPtrArray  *tried;
} FakeRelays;

static void
fake_publish(gpointer target, const char *event_json, const char *relay_url,
             GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  FakeRelays *f = target;
  (void)event_json;
  g_ptr_array_add(f->tried, g_strdup(relay_url));
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  if (g_strcmp0(relay_url, f->accepting) == 0)
    g_task_return_boolean(task, TRUE);
  else
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "refused by %s", relay_url);
  g_object_unref(task);
}

static gboolean
fake_finish(gpointer target, GAsyncResult *result, GError **error)
{
  (void)target;
  return g_task_propagate_boolean(G_TASK(result), error);
}

typedef struct {
  GMainLoop *loop;
  gchar     *relay;
  GError    *error;
} Outcome;

static void
on_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  Outcome *o = user_data;
  o->relay = gn_mls_publish_until_ack_finish(result, &o->error);
  g_main_loop_quit(o->loop);
}

static void
run(FakeRelays *f, const char * const *relays, Outcome *o)
{
  o->loop = g_main_loop_new(NULL, FALSE);
  gn_mls_publish_until_ack_async(fake_publish, fake_finish, f, "{}", relays, NULL,
                                 on_done, o);
  g_main_loop_run(o->loop);
  g_main_loop_unref(o->loop);
}

static void
test_first_accepting_relay_wins(void)
{
  const char *relays[] = { "wss://a", "wss://b", "wss://c", NULL };
  FakeRelays f = { "wss://b", g_ptr_array_new_with_free_func(g_free) };
  Outcome o = {0};
  run(&f, relays, &o);
  g_assert_no_error(o.error);
  g_assert_cmpstr(o.relay, ==, "wss://b");
  /* It stops at the first OK. */
  g_assert_cmpuint(f.tried->len, ==, 2);
  g_free(o.relay);
  g_ptr_array_unref(f.tried);
}

static void
test_no_relay_accepts(void)
{
  const char *relays[] = { "wss://a", "wss://b", NULL };
  FakeRelays f = { NULL, g_ptr_array_new_with_free_func(g_free) };
  Outcome o = {0};
  run(&f, relays, &o);
  g_assert_null(o.relay);
  g_assert_error(o.error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_nonnull(strstr(o.error->message, "wss://b"));
  g_assert_cmpuint(f.tried->len, ==, 2);
  g_clear_error(&o.error);
  g_ptr_array_unref(f.tried);
}

static void
test_no_relays(void)
{
  const char *none[] = { NULL };
  FakeRelays f = { "wss://a", g_ptr_array_new_with_free_func(g_free) };
  for (int i = 0; i < 2; i++) {
    Outcome o = {0};
    run(&f, i == 0 ? none : NULL, &o);
    g_assert_null(o.relay);
    g_assert_error(o.error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&o.error);
  }
  g_assert_cmpuint(f.tried->len, ==, 0);
  g_ptr_array_unref(f.tried);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/mls-groups/commit-publish/first-accepting-relay", test_first_accepting_relay_wins);
  g_test_add_func("/mls-groups/commit-publish/no-relay-accepts", test_no_relay_accepts);
  g_test_add_func("/mls-groups/commit-publish/no-relays", test_no_relays);
  return g_test_run();
}
