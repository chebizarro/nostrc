/* test_session_relay.c - org.nostr.SessionRelay1 client (nostrc-t24q)
 *
 * SPDX-License-Identifier: MIT
 *
 * The D-Bus cases run against np-fake-session-relay on a private bus
 * (GTestDBus); without dbus-daemon they are skipped with a reason, and
 * the process never touches the caller's session bus.
 */
#include <nostr-publish/nostr-publish.h>

#include "np-fake-session-relay.h"

#include <glib/gstdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static GTestDBus *s_bus;

typedef struct {
  NostrPublishFederation federation;
  guint                  federation_changes;
  gboolean               federation_seen;
  GPtrArray             *updates;    /* "state|relay_url|relay_state|relays…" */
  gboolean               update_seen;
} Rec;

static void
on_federation(NostrPublishSessionRelay *relay, NostrPublishFederation state, gpointer ud)
{
  (void)relay;
  Rec *r = ud;
  r->federation = state;
  r->federation_changes++;
  r->federation_seen = TRUE;
}

static void
on_forward(NostrPublishSessionRelay *relay, const NostrPublishForwardUpdate *u, gpointer ud)
{
  (void)relay;
  Rec *r = ud;
  g_autofree gchar *relays = u->relays ? g_strjoinv(",", (gchar **)u->relays) : g_strdup("-");
  g_ptr_array_add(r->updates,
                  g_strdup_printf("%s|%s|%s|%s|%s|%s", u->event_id,
                                  nostr_publish_forward_state_to_string(u->state),
                                  u->relay_url ? u->relay_url : "-",
                                  u->relay_state ? u->relay_state : "-", u->detail, relays));
  r->update_seen = TRUE;
}

static GDBusConnection *
client_bus(void)
{
  GError *err = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  g_assert_no_error(err);
  return bus;
}

static NostrPublishFederation
wait_federation(Rec *r, NostrPublishFederation want)
{
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  while (r->federation != want && g_get_monotonic_time() < deadline) {
    r->federation_seen = FALSE;
    np_wait_for(NULL, &r->federation_seen, 250);
  }
  return r->federation;
}

static void
test_states(void)
{
  g_assert_cmpint(nostr_publish_federation_from_string("active"), ==,
                  NOSTR_PUBLISH_FEDERATION_ACTIVE);
  g_assert_cmpint(nostr_publish_federation_from_string("waiting-for-account"), ==,
                  NOSTR_PUBLISH_FEDERATION_WAITING_FOR_ACCOUNT);
  g_assert_cmpint(nostr_publish_federation_from_string("disabled"), ==,
                  NOSTR_PUBLISH_FEDERATION_DISABLED);
  g_assert_cmpint(nostr_publish_federation_from_string("unavailable"), ==,
                  NOSTR_PUBLISH_FEDERATION_UNAVAILABLE);
  /* Client-side words and future values never parse as forwarding. */
  g_assert_cmpint(nostr_publish_federation_from_string("not-running"), ==,
                  NOSTR_PUBLISH_FEDERATION_UNSUPPORTED);
  g_assert_cmpint(nostr_publish_federation_from_string("sometimes"), ==,
                  NOSTR_PUBLISH_FEDERATION_UNSUPPORTED);
  g_assert_cmpint(nostr_publish_federation_from_string(NULL), ==,
                  NOSTR_PUBLISH_FEDERATION_UNSUPPORTED);
  g_assert_true(nostr_publish_federation_forwards(NOSTR_PUBLISH_FEDERATION_ACTIVE));
  g_assert_true(nostr_publish_federation_forwards(NOSTR_PUBLISH_FEDERATION_WAITING_FOR_ACCOUNT));
  for (NostrPublishFederation f = NOSTR_PUBLISH_FEDERATION_UNKNOWN;
       f <= NOSTR_PUBLISH_FEDERATION_UNAVAILABLE; f++)
    if (f != NOSTR_PUBLISH_FEDERATION_ACTIVE &&
        f != NOSTR_PUBLISH_FEDERATION_WAITING_FOR_ACCOUNT)
      g_assert_false(nostr_publish_federation_forwards(f));

  static const struct { const gchar *s; gboolean final, delivered; } T[] = {
    { "new", FALSE, FALSE },       { "unroutable", FALSE, FALSE },
    { "pending", FALSE, FALSE },   { "forwarded", TRUE, TRUE },
    { "partial", TRUE, TRUE },     { "failed", TRUE, FALSE },
    { "skipped", TRUE, FALSE },    { "superseded", TRUE, FALSE },
    { "cancelled", TRUE, FALSE },  { "unknown", TRUE, FALSE },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(T); i++) {
    NostrPublishForwardState s = nostr_publish_forward_state_from_string(T[i].s);
    g_assert_cmpstr(nostr_publish_forward_state_to_string(s), ==, T[i].s);
    g_assert_cmpint(nostr_publish_forward_state_is_final(s), ==, T[i].final);
    g_assert_cmpint(nostr_publish_forward_state_delivered(s), ==, T[i].delivered);
  }
  g_assert_cmpint(nostr_publish_forward_state_from_string("queued-on-mars"), ==,
                  NOSTR_PUBLISH_FORWARD_UNKNOWN);
}

static void
test_federation_tracking(void)
{
  if (s_bus == NULL) {
    g_test_skip("dbus-daemon not installed: no private session bus for the fake relay");
    return;
  }
  g_autoptr(GDBusConnection) bus = client_bus();
  Rec r = { 0 };
  r.updates = g_ptr_array_new_with_free_func(g_free);
  NostrPublishSessionRelay *sr = nostr_publish_session_relay_new(bus, on_federation,
                                                                 on_forward, &r);
  /* Nothing reported synchronously; with no owner: not running. */
  g_assert_cmpuint(r.federation_changes, ==, 0);
  g_assert_cmpint(wait_federation(&r, NOSTR_PUBLISH_FEDERATION_NOT_RUNNING), ==,
                  NOSTR_PUBLISH_FEDERATION_NOT_RUNNING);

  NpFakeSessionRelay *fake = np_fake_session_relay_start("active");
  g_assert_cmpint(wait_federation(&r, NOSTR_PUBLISH_FEDERATION_ACTIVE), ==,
                  NOSTR_PUBLISH_FEDERATION_ACTIVE);
  g_assert_cmpint(nostr_publish_session_relay_get_federation(sr), ==,
                  NOSTR_PUBLISH_FEDERATION_ACTIVE);
  np_fake_session_relay_stop(fake);
  g_assert_cmpint(wait_federation(&r, NOSTR_PUBLISH_FEDERATION_NOT_RUNNING), ==,
                  NOSTR_PUBLISH_FEDERATION_NOT_RUNNING);

  /* A daemon from before nostrc-7d96 has no FederationState: fail closed. */
  fake = np_fake_session_relay_start(NULL);
  g_assert_cmpint(wait_federation(&r, NOSTR_PUBLISH_FEDERATION_UNSUPPORTED), ==,
                  NOSTR_PUBLISH_FEDERATION_UNSUPPORTED);
  np_fake_session_relay_stop(fake);
  g_assert_cmpint(wait_federation(&r, NOSTR_PUBLISH_FEDERATION_NOT_RUNNING), ==,
                  NOSTR_PUBLISH_FEDERATION_NOT_RUNNING);

  fake = np_fake_session_relay_start("disabled");
  g_assert_cmpint(wait_federation(&r, NOSTR_PUBLISH_FEDERATION_DISABLED), ==,
                  NOSTR_PUBLISH_FEDERATION_DISABLED);
  np_fake_session_relay_stop(fake);
  g_assert_cmpint(wait_federation(&r, NOSTR_PUBLISH_FEDERATION_NOT_RUNNING), ==,
                  NOSTR_PUBLISH_FEDERATION_NOT_RUNNING);

  nostr_publish_session_relay_free(sr);
  g_ptr_array_unref(r.updates);
}

static void
test_event_status(void)
{
  if (s_bus == NULL) {
    g_test_skip("dbus-daemon not installed: no private session bus for the fake relay");
    return;
  }
  g_autoptr(GDBusConnection) bus = client_bus();
  Rec r = { 0 };
  r.updates = g_ptr_array_new_with_free_func(g_free);
  NpFakeSessionRelay *fake = np_fake_session_relay_start("active");
  NostrPublishSessionRelay *sr = nostr_publish_session_relay_new(bus, on_federation,
                                                                 on_forward, &r);
  g_assert_cmpint(wait_federation(&r, NOSTR_PUBLISH_FEDERATION_ACTIVE), ==,
                  NOSTR_PUBLISH_FEDERATION_ACTIVE);

  /* Query → snapshot (with per-relay lines), then the relay's follow-up
   * transition arrives as a signal. */
  const gchar *relays[] = { "wss://a.test pending", "wss://b.test failed blocked: no", NULL };
  np_fake_session_relay_set_reply(fake, "pending", "", relays);
  np_fake_session_relay_set_followup(fake, "wss://a.test", "acked", "", "partial");
  nostr_publish_session_relay_query(sr, "ab01");
  while (r.updates->len < 2) {
    r.update_seen = FALSE;
    g_assert_true(np_wait_for(NULL, &r.update_seen, 5000));
  }
  g_assert_cmpstr(g_ptr_array_index(r.updates, 0), ==,
                  "ab01|pending|-|-||wss://a.test pending,wss://b.test failed: blocked: no");
  g_assert_cmpstr(g_ptr_array_index(r.updates, 1), ==,
                  "ab01|partial|wss://a.test|acked||-");
  g_autofree gchar *asked = np_fake_session_relay_last_query(fake);
  g_assert_cmpstr(asked, ==, "ab01");

  /* Event-level signal: relay_url "" → NULL, reason becomes the detail. */
  np_fake_session_relay_emit(fake, "cd02", "", "", "no relay list yet", "unroutable");
  r.update_seen = FALSE;
  g_assert_true(np_wait_for(NULL, &r.update_seen, 5000));
  g_assert_cmpstr(g_ptr_array_index(r.updates, 2), ==,
                  "cd02|unroutable|-|-|no relay list yet|-");

  /* No callbacks after free, even with a reply in flight. */
  nostr_publish_session_relay_query(sr, "ef03");
  nostr_publish_session_relay_free(sr);
  guint before = r.updates->len;
  gboolean never = FALSE;
  np_wait_for(NULL, &never, 300);
  g_assert_cmpuint(r.updates->len, ==, before);

  np_fake_session_relay_stop(fake);
  g_ptr_array_unref(r.updates);
}

static void
test_nudge(void)
{
  g_autofree gchar *dir = g_dir_make_tmp("np-nudge-XXXXXX", NULL);
  g_autofree gchar *path = g_build_filename(dir, "relay.sock", NULL);
  g_assert_false(nostr_publish_session_relay_nudge(path));   /* nothing there */

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  g_assert_cmpint(fd, >=, 0);
  struct sockaddr_un sa = { 0 };
  sa.sun_family = AF_UNIX;
  g_strlcpy(sa.sun_path, path, sizeof(sa.sun_path));
  g_assert_cmpint(bind(fd, (struct sockaddr *)&sa, sizeof(sa)), ==, 0);
  g_assert_cmpint(listen(fd, 1), ==, 0);
  g_assert_true(nostr_publish_session_relay_nudge(path));
  int c = accept(fd, NULL, NULL);
  g_assert_cmpint(c, >=, 0);   /* the activation-triggering connection */
  close(c);
  close(fd);
  g_unlink(path);
  g_rmdir(dir);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  if (np_fake_session_relay_bus_available()) {
    s_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(s_bus);
  } else {
    /* Never fall through to the real session bus. */
    g_setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/np-test-bus", TRUE);
  }
  g_test_add_func("/nostr-publish/session-relay/states", test_states);
  g_test_add_func("/nostr-publish/session-relay/federation", test_federation_tracking);
  g_test_add_func("/nostr-publish/session-relay/event-status", test_event_status);
  g_test_add_func("/nostr-publish/session-relay/nudge", test_nudge);
  int rc = g_test_run();
  if (s_bus != NULL) {
    g_test_dbus_down(s_bus);
    g_object_unref(s_bus);
  }
  return rc;
}
