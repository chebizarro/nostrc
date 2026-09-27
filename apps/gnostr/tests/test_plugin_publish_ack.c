/*
 * test_plugin_publish_ack — gnostr_plugin_context_publish_event_to_relay_ack
 * against a real WebSocket relay (nostrc-4gf4).
 *
 * The mock relay (testing/, libwebsockets) verifies signatures and answers
 * every EVENT with NIP-01 OK. The call must complete on that OK:
 *   - accepted -> TRUE, and the relay stored the event;
 *   - OK false (tampered signature) -> GNOSTR_PLUGIN_ERROR_RELAY_REJECTED
 *     carrying the relay's reason;
 *   - nobody listening -> GNOSTR_PLUGIN_ERROR_NETWORK;
 *   - cancelled -> G_IO_ERROR_CANCELLED;
 *   - not an event -> GNOSTR_PLUGIN_ERROR_INVALID_DATA.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gnostr-plugin-api.h"

#include <glib.h>
#include <gio/gio.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <string.h>
#include "nostr/testing/mock_relay_server.h"
#include <nostr-gobject-1.0/nostr_relay.h>

#define SK "7f7ff03d123792d6ac594bfa67bf6d0c0ab55b6b1fdb6249303fe861f1ccba9a"

typedef struct {
  GMainLoop *loop;
  gboolean ok;
  GError *error;
} Wait;

static void
on_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void)source;
  Wait *w = user_data;
  w->ok = gnostr_plugin_context_publish_event_to_relay_ack_finish(NULL, res, &w->error);
  g_main_loop_quit(w->loop);
}

static gboolean
run(GnostrPluginContext *ctx, const char *json, const char *url, GCancellable *c, GError **error)
{
  Wait w = { .loop = g_main_loop_new(NULL, FALSE) };
  gnostr_plugin_context_publish_event_to_relay_ack_async(ctx, json, url, c, on_done, &w);
  g_main_loop_run(w.loop);
  g_main_loop_unref(w.loop);
  if (w.error)
    g_propagate_error(error, w.error);
  return w.ok;
}

static char *
signed_event(const char *content)
{
  NostrEvent *ev = nostr_event_new();
  nostr_event_set_kind(ev, 9007);
  nostr_event_set_created_at(ev, 1700000000);
  nostr_event_set_content(ev, content);
  g_assert_cmpint(nostr_event_sign(ev, SK), ==, 0);
  char *json = nostr_event_serialize_compact(ev);
  nostr_event_free(ev);
  return json;
}

static void
test_publish_ack(void)
{
  g_autofree char *good = signed_event("create");
  g_autofree char *other = signed_event("original");

  NostrMockRelayServerConfig cfg = nostr_mock_server_config_default();
  cfg.validate_signatures = true;
  NostrMockRelayServer *server = nostr_mock_server_new(&cfg);
  g_assert_nonnull(server);
  g_assert_cmpint(nostr_mock_server_start(server), ==, 0);
  const char *url = nostr_mock_server_get_url(server);
  GnostrPluginContext *ctx = gnostr_plugin_context_new(NULL, "test");
  GError *error = NULL;
  /* gnostr_relay_new() hands out one shared relay per URL; in the app the
   * pool keeps it alive. Do the same, so rounds do not race the teardown
   * of the previous round's connection (see nostrc-jc2o). */
  g_autoptr(GNostrRelay) shared = gnostr_relay_new(url);



  /* Accepted. */
  g_assert_true(run(ctx, good, url, NULL, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(nostr_mock_server_get_published_count(server), ==, 1);

  /* OK false: the relay's reason comes back. */
  char *p = strstr(other, "\"original\"");
  g_assert_nonnull(p);
  memcpy(p, "\"tampered\"", 10);
  g_assert_false(run(ctx, other, url, NULL, &error));
  g_assert_error(error, GNOSTR_PLUGIN_ERROR, GNOSTR_PLUGIN_ERROR_RELAY_REJECTED);
  g_assert_nonnull(strstr(error->message, "signature"));
  g_clear_error(&error);

  /* Nobody listening. (Today this ends at the 15 s OK bound rather than at
   * connect time: nostrc-oz77.) */
  g_assert_false(run(ctx, good, "ws://127.0.0.1:1", NULL, &error));
  g_assert_error(error, GNOSTR_PLUGIN_ERROR, GNOSTR_PLUGIN_ERROR_NETWORK);
  g_clear_error(&error);

  /* Cancelled before an answer. */
  g_autoptr(GCancellable) c = g_cancellable_new();
  g_cancellable_cancel(c);
  g_assert_false(run(ctx, good, url, c, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error(&error);

  /* Not a signed event. */
  g_assert_false(run(ctx, "{\"kind\":1}", url, NULL, &error));
  g_assert_error(error, GNOSTR_PLUGIN_ERROR, GNOSTR_PLUGIN_ERROR_INVALID_DATA);
  g_clear_error(&error);

  gnostr_plugin_context_free(ctx);
  nostr_mock_server_stop(server);
  nostr_mock_server_free(server);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/plugin-api/publish-ack", test_publish_ack);
  return g_test_run();
}
