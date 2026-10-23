/* Private runtime-fixture approval agent. Never linked into a shipped target.
 * Build with: cc nip5f_approver.c $(pkg-config --cflags --libs gio-2.0)
 * Consent is explicit, one-shot, and bound to the real pending request metadata.
 */
#include <gio/gio.h>
#include <glib-unix.h>
#include <stdio.h>
#include <string.h>

#define BUS "org.nostr.Signer"
#define PATH "/org/nostr/signer"
#define IFACE "org.nostr.Signer"
static GDBusConnection *bus;
static const gchar *identity, *decision;
static guint requests, decisions;

static void bus_fence(void) {
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus",
      "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetId", NULL,
      G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &error);
  if (!reply) g_error("approval subscription fence failed");
}

static GVariant *call(const gchar *path, const gchar *interface, const gchar *method,
                      GVariant *args, const GVariantType *type) {
  g_autoptr(GError) error = NULL;
  GVariant *reply = g_dbus_connection_call_sync(bus, BUS, path, interface, method,
      args, type, G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &error);
  /* Do not print remote error text: key-bearing calls must not leak arguments. */
  if (!reply) g_error("fixture API failed: %s", method);
  return reply;
}
static gboolean field(GVariant *info, const gchar *key, const gchar *expected) {
  const gchar *value = NULL;
  return g_variant_lookup(info, key, "&s", &value) && !g_strcmp0(value, expected);
}
static void requested(GDBusConnection *connection, const gchar *sender,
    const gchar *path, const gchar *interface, const gchar *signal,
    GVariant *parameters, gpointer data) {
  (void)connection; (void)sender; (void)path; (void)interface; (void)signal; (void)data;
  const gchar *app, *npub, *kind, *preview, *id;
  g_variant_get(parameters, "(&s&s&s&s&s)", &app, &npub, &kind, &preview, &id);
  (void)app; (void)preview;
  if (++requests != 1) g_error("fixture refuses more than one request");
  g_autoptr(GVariant) reply = call(PATH, IFACE, "GetApprovalInfo",
      g_variant_new("(s)", id), G_VARIANT_TYPE("(a{sv})"));
  g_autoptr(GVariant) info = g_variant_get_child_value(reply, 0);
  gboolean attested = TRUE, rememberable = TRUE;
  if (strcmp(npub, identity) || strcmp(kind, "get_public_key") ||
      !field(info, "request_id", id) || !field(info, "identity", identity) ||
      !field(info, "principal_kind", "unidentified") ||
      !field(info, "transport", "NIP5F") ||
      !field(info, "operation", "get_public_key") || !field(info, "state", "PENDING") ||
      !g_variant_lookup(info, "attested", "b", &attested) || attested ||
      !g_variant_lookup(info, "rememberable", "b", &rememberable) || rememberable)
    g_error("fixture refuses request outside explicit unidentified-public-key consent");
  puts("pending:unidentified:NIP5F:get_public_key:nonrememberable"); fflush(stdout);
  if (!strcmp(decision, "hold")) return;
  g_autoptr(GVariant) approved = call(PATH, IFACE, "ApproveRequest",
      g_variant_new("(sbbt)", id, !strcmp(decision, "allow"), FALSE, (guint64)0),
      G_VARIANT_TYPE("(b)"));
  gboolean ok = FALSE; g_variant_get(approved, "(b)", &ok);
  if (!ok) g_error("explicit approval decision was not accepted");
  decisions++;
  g_print("decision:%s:remember=false\n", decision); fflush(stdout);
}

typedef struct { GDBusMethodInvocation *invocation; guint pending; } Snapshot;
static gboolean snapshot_ready(gpointer data) {
  Snapshot *s = data;
  g_dbus_method_invocation_return_value(s->invocation,
      g_variant_new("(uuu)", requests, decisions, s->pending));
  g_object_unref(s->invocation); g_free(s);
  return G_SOURCE_REMOVE;
}
static void snapshot(GDBusConnection *connection, const gchar *sender,
    const gchar *path, const gchar *interface, const gchar *method,
    GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data) {
  (void)connection; (void)sender; (void)path; (void)interface;
  (void)method; (void)parameters; (void)data;
  g_autoptr(GVariant) reply = call(PATH, IFACE, "ListPendingRequests", NULL,
      G_VARIANT_TYPE("(a(sssss))"));
  g_autoptr(GVariant) pending = g_variant_get_child_value(reply, 0);
  bus_fence();
  Snapshot *s = g_new0(Snapshot, 1);
  s->invocation = g_object_ref(invocation); s->pending = g_variant_n_children(pending);
  /* Daemon reply + bus fence precede this idle. Already delivered approval
   * signal callbacks run at default priority before the counter is sampled. */
  g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, snapshot_ready, s, NULL);
}
static const GDBusInterfaceVTable status_vtable = {.method_call = snapshot};

/* Owned fixture shutdown is a normal exit, so sanitizer exit checks run. */
static gboolean quit_on_signal(gpointer loop) {
  g_main_loop_quit(loop);
  return G_SOURCE_CONTINUE;  /* removed explicitly after the loop */
}

int main(int argc, char **argv) {
  if (argc != 4 || strcmp(argv[1], "--approve-unidentified-get-public-key") ||
      (strcmp(argv[2], "allow") && strcmp(argv[2], "deny") && strcmp(argv[2], "hold"))) {
    fputs("explicit consent flag, allow|deny|hold and private key-file required\n", stderr); return 2;
  }
  decision = argv[2];
  g_autoptr(GError) error = NULL;
  bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
  if (!bus) g_error("private session bus unavailable");
  g_autoptr(GVariant) owner = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus",
      "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
      g_variant_new("(su)", "org.nostr.Grotto", 4u), G_VARIANT_TYPE("(u)"),
      G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &error);
  guint result = 0; if (owner) g_variant_get(owner, "(u)", &result);
  if (result != 1) g_error("fixture approval name is not exclusively owned");
  gchar *key = NULL; gsize length = 0;
  if (!g_file_get_contents(argv[3], &key, &length, NULL) || length != 64)
    g_error("invalid private fixture key file");
  g_autoptr(GVariant) stored = call(PATH, IFACE, "StoreKey",
      g_variant_new("(ss)", key, ""), G_VARIANT_TYPE("(bs)"));
  /* No environment-key lane, key-mutation override, or preseeded grants. */
  memset(key, 0, length); g_free(key);
  gboolean ok = FALSE; g_variant_get(stored, "(b&s)", &ok, &identity);
  if (!ok || !g_str_has_prefix(identity, "npub1")) g_error("identity storage failed");
  g_autoptr(GVariant) selected = call("/org/nostr/signer/grotto", "org.nostr.Grotto.Control1",
      "SetDefaultIdentityForLegacy", g_variant_new("(s)", identity), G_VARIANT_TYPE_UNIT);
  guint subscription = g_dbus_connection_signal_subscribe(bus, BUS, IFACE, "ApprovalRequested",
      PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, requested, NULL, NULL);
  g_autoptr(GDBusNodeInfo) status = g_dbus_node_info_new_for_xml(
      "<node><interface name='org.nostr.Test.Nip5fApprover1'><method name='Snapshot'>"
      "<arg type='u' direction='out'/><arg type='u' direction='out'/>"
      "<arg type='u' direction='out'/></method></interface></node>", &error);
  if (!status || !g_dbus_connection_register_object(bus, "/org/nostr/test/nip5f_approver",
      status->interfaces[0], &status_vtable, NULL, NULL, &error))
    g_error("fixture status export failed");
  /* Graceful shutdown handlers exist before readiness is announced. */
  g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
  guint term = g_unix_signal_add(SIGTERM, quit_on_signal, loop);
  guint interrupt = g_unix_signal_add(SIGINT, quit_on_signal, loop);
  /* Roundtrip on the same connection fences AddMatch before client launch. */
  bus_fence();
  g_print("ready:%s\n", identity); fflush(stdout);
  g_main_loop_run(loop);
  g_source_remove(term); g_source_remove(interrupt);
  g_dbus_connection_signal_unsubscribe(bus, subscription); g_object_unref(bus);
  return 0;
}
