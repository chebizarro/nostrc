/* SPDX-License-Identifier: GPL-3.0-or-later
 * Review B1/R2: a Commit is merged only after a relay accepted it, and
 * cleared only when every relay definitely refused it. */

#include "gn-mls-commit-publish.h"
#include <string.h>

/* Each relay URL names its answer: ".../ok", ".../refuse" (NIP-01 OK false)
 * or ".../timeout" (no OK: it may have been stored). */
typedef struct {
  GPtrArray *tried;
} FakeRelays;

static void
fake_publish(gpointer target, const char *event_json, const char *relay_url,
             GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  FakeRelays *f = target;
  (void)event_json;
  g_ptr_array_add(f->tried, g_strdup(relay_url));
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  if (g_str_has_suffix(relay_url, "/ok"))
    g_task_return_boolean(task, TRUE);
  else if (g_str_has_suffix(relay_url, "/refuse"))
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "refused by %s", relay_url);
  else
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                            "no OK from %s", relay_url);
  g_object_unref(task);
}

static gboolean
fake_finish(gpointer target, GAsyncResult *result, GError **error)
{
  (void)target;
  return g_task_propagate_boolean(G_TASK(result), error);
}

static gboolean
fake_is_rejection(const GError *error)
{
  return g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
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

static guint
run(const char * const *relays, Outcome *o)
{
  FakeRelays f = { g_ptr_array_new_with_free_func(g_free) };
  o->loop = g_main_loop_new(NULL, FALSE);
  gn_mls_publish_until_ack_async(fake_publish, fake_finish, fake_is_rejection, &f, "{}",
                                 relays, NULL, on_done, o);
  g_main_loop_run(o->loop);
  g_main_loop_unref(o->loop);
  guint tried = f.tried->len;
  g_ptr_array_unref(f.tried);
  return tried;
}

static void
test_first_accepting_relay_wins(void)
{
  const char *relays[] = { "wss://a/refuse", "wss://b/timeout", "wss://c/ok", "wss://d/ok", NULL };
  Outcome o = {0};
  g_assert_cmpuint(run(relays, &o), ==, 3);   /* stops at the first OK */
  g_assert_no_error(o.error);
  g_assert_cmpstr(o.relay, ==, "wss://c/ok");
  g_free(o.relay);
}

static void
test_all_refused_is_rejected(void)
{
  const char *relays[] = { "wss://a/refuse", "wss://b/refuse", NULL };
  Outcome o = {0};
  g_assert_cmpuint(run(relays, &o), ==, 2);
  g_assert_null(o.relay);
  g_assert_error(o.error, GN_MLS_PUBLISH_ERROR, GN_MLS_PUBLISH_REJECTED);
  g_assert_nonnull(strstr(o.error->message, "wss://b/refuse"));
  g_clear_error(&o.error);
}

/* One relay that may have stored it makes the outcome uncertain, even if
 * every other relay refused: the Commit must stay pending. */
static void
test_lost_ok_is_uncertain(void)
{
  const char *orders[2][3] = {
    { "wss://a/timeout", "wss://b/refuse", NULL },
    { "wss://a/refuse", "wss://b/timeout", NULL },
  };
  for (int i = 0; i < 2; i++)
    {
      Outcome o = {0};
      run(orders[i], &o);
      g_assert_null(o.relay);
      g_assert_error(o.error, GN_MLS_PUBLISH_ERROR, GN_MLS_PUBLISH_UNCERTAIN);
      g_clear_error(&o.error);
    }
}

static void
test_no_relays(void)
{
  const char *none[] = { NULL };
  for (int i = 0; i < 2; i++)
    {
      Outcome o = {0};
      g_assert_cmpuint(run(i == 0 ? none : NULL, &o), ==, 0);
      g_assert_null(o.relay);
      g_assert_error(o.error, GN_MLS_PUBLISH_ERROR, GN_MLS_PUBLISH_NO_RELAYS);
      g_clear_error(&o.error);
    }
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/mls-groups/commit-publish/first-accepting-relay", test_first_accepting_relay_wins);
  g_test_add_func("/mls-groups/commit-publish/all-refused", test_all_refused_is_rejected);
  g_test_add_func("/mls-groups/commit-publish/lost-ok-is-uncertain", test_lost_ok_is_uncertain);
  g_test_add_func("/mls-groups/commit-publish/no-relays", test_no_relays);
  return g_test_run();
}
