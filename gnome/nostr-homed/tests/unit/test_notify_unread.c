/*
 * test_notify_unread — org.nostr.NotifyDaemon1 (prqu.18) exported on a
 * private GTestDBus bus and read by a second connection, as the Shell
 * extension would: property values, PropertiesChanged on every change,
 * MarkRead, no signal for a no-op reset, and DM-vs-group deep links.
 */
#include "notify_unread.h"

#include "nostr/nip19/nip19.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static guint changes;
static guint32 seen_unread;

static void on_changed(GDBusConnection *c, const gchar *s, const gchar *p, const gchar *i, const gchar *sig,
                       GVariant *params, gpointer ud) {
  (void)c; (void)s; (void)p; (void)i; (void)sig; (void)ud;
  const gchar *iface = NULL;
  g_autoptr(GVariant) changed = NULL;
  g_variant_get(params, "(&s@a{sv}as)", &iface, &changed, NULL);
  if (g_strcmp0(iface, "org.nostr.NotifyDaemon1") != 0) return;
  CHECK(g_variant_lookup(changed, "UnreadDirectMessages", "u", &seen_unread));
  changes++;
}

static void spin(guint want) {
  gint64 end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  while (changes < want && g_get_monotonic_time() < end) g_main_context_iteration(NULL, TRUE);
  CHECK(changes >= want);
}

/* Async + spin: the exporting connection is served by this same thread. */
typedef struct { gboolean done; GVariant *value; } Reply;

static void on_reply(GObject *src, GAsyncResult *res, gpointer ud) {
  Reply *r = ud;
  GError *e = NULL;
  r->value = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
  if (!r->value) {
    fprintf(stderr, "call failed: %s\n", e->message);
    g_error_free(e);
  }
  r->done = TRUE;
}

static GVariant *call(GDBusConnection *client, const gchar *name, const gchar *iface, const gchar *method,
                      GVariant *args) {
  Reply r = { 0 };
  g_dbus_connection_call(client, name, "/org/nostr/NotifyDaemon1", iface, method, args, NULL,
                         G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, NULL, on_reply, &r);
  while (!r.done) g_main_context_iteration(NULL, TRUE);
  CHECK(r.value);
  return r.value;
}

static guint32 get_unread(GDBusConnection *client, const gchar *name) {
  g_autoptr(GVariant) r = call(client, name, "org.freedesktop.DBus.Properties", "Get",
                               g_variant_new("(ss)", "org.nostr.NotifyDaemon1", "UnreadDirectMessages"));
  g_autoptr(GVariant) v = NULL;
  g_variant_get(r, "(v)", &v);
  return g_variant_get_uint32(v);
}

static char *nevent(int kind) {
  NostrEventPointer ptr = { .id = (char *)"cafebabecafebabecafebabecafebabecafebabecafebabecafebabecafebabe",
                            .kind = kind };
  char *b = NULL;
  CHECK(nostr_nip19_encode_nevent(&ptr, &b) == 0);
  char *uri = g_strconcat("nostr:", b, NULL);
  free(b);
  return uri;
}

int main(void) {
  GTestDBus *tbus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(tbus);
  GDBusConnection *daemon = g_dbus_connection_new_for_address_sync(g_test_dbus_get_bus_address(tbus),
      G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, NULL);
  GDBusConnection *client = g_dbus_connection_new_for_address_sync(g_test_dbus_get_bus_address(tbus),
      G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, NULL);
  CHECK(daemon && client);
  const gchar *name = g_dbus_connection_get_unique_name(daemon);
  GError *err = NULL;
  CHECK(nostr_notify_unread_export(daemon, &err));
  CHECK(nostr_notify_unread_export(daemon, &err)); /* idempotent */
  g_dbus_connection_signal_subscribe(client, name, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                     "/org/nostr/NotifyDaemon1", NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_changed, NULL, NULL);

  CHECK(get_unread(client, name) == 0);
  nostr_notify_unread_add(1700000000);
  spin(1);
  CHECK(seen_unread == 1);
  nostr_notify_unread_add(1700000100);
  spin(2);
  CHECK(seen_unread == 2 && get_unread(client, name) == 2);
  CHECK(nostr_notify_unread_last() == 1700000100);

  g_autoptr(GVariant) mr = call(client, name, "org.nostr.NotifyDaemon1", "MarkRead", NULL);
  spin(3);
  CHECK(seen_unread == 0 && get_unread(client, name) == 0);
  nostr_notify_unread_reset(); /* already 0: no signal */
  for (int i = 0; i < 20; i++) g_main_context_iteration(NULL, FALSE);
  CHECK(changes == 3);

  /* only a gift-wrap (kind 1059) deep link counts as opening a DM */
  g_autofree char *dm = nevent(1059);
  g_autofree char *grp = nevent(9);
  CHECK(nostr_notify_uri_is_dm(dm));
  CHECK(!nostr_notify_uri_is_dm(grp));
  CHECK(!nostr_notify_uri_is_dm("nostr:npub1xyz"));
  CHECK(!nostr_notify_uri_is_dm(NULL));

  nostr_notify_unread_unexport();
  g_object_unref(client);
  g_object_unref(daemon);
  g_test_dbus_down(tbus);
  g_object_unref(tbus);
  fprintf(stderr, "test_notify_unread: OK\n");
  return 0;
}
