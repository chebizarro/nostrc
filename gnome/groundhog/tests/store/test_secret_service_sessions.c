/* Groundhog's Secret Service backends against a provider that offers only
 * "plain" sessions, and against one that refuses every session
 * (nostrc-ep54q, the Groundhog side of nostrc-poc10).
 *
 * libsecret 0.21.8 opens a missing session itself inside
 * secret_service_search_sync(..., SECRET_SEARCH_LOAD_SECRETS, ...) with a
 * NULL GError, and _secret_session_open_sync()'s fallback from the
 * dh-ietf1024-sha256-aes128-cbc-pkcs7 algorithm to "plain" dereferences that
 * NULL once the first OpenSession fails: a plain-only or session-refusing
 * provider crashes the process. Groundhog never lets libsecret open the
 * session there: GhStoreKey and the NIP-46 credential backend get the
 * service with SECRET_SERVICE_OPEN_SESSION and an error to report into, so
 * the fallback works and a refused session is the backend's UNAVAILABLE
 * error. This test pins that, both for a fresh libsecret proxy and for a
 * shared one another component already created without a session.
 *
 * The fake org.freedesktop.secrets runs in this process, on its own thread
 * and main context (the NIP-46 backend's calls are synchronous), on a
 * private bus (nostrc-test-bus.h). It holds one test item per schema and
 * implements only what these lookups call. Test data only; never a real
 * session bus or keyring. */
#include "fake-secret.h"
#include "gh-nip46-credentials-private.h"
#include "nostrc-test-bus.h"

#include <libsecret/secret.h>
#include <string.h>

#define FSS_NAME       "org.freedesktop.secrets"
#define FSS_PATH       "/org/freedesktop/secrets"
#define FSS_COLLECTION FSS_PATH "/collection/fake"
#define AES_ALGORITHM  "dh-ietf1024-sha256-aes128-cbc-pkcs7"

#define STORE_ACCOUNT  "5555555555555555555555555555555555555555555555555555555555555555"
#define STORE_ID       "1b4e28ba-2fa1-41d2-883f-0016d3cca427"
#define NIP46_ACCOUNT  "6666666666666666666666666666666666666666666666666666666666666666"
#define NIP46_SECRET   "{\"test\":\"plain session\"}"

typedef enum { MODE_PLAIN, MODE_NONE } SessionMode;

static const gchar fss_xml[] =
  "<node>"
  " <interface name='org.freedesktop.Secret.Service'>"
  "  <method name='OpenSession'><arg type='s' direction='in'/><arg type='v' direction='in'/>"
  "   <arg type='v' direction='out'/><arg type='o' direction='out'/></method>"
  "  <method name='SearchItems'><arg type='a{ss}' direction='in'/>"
  "   <arg type='ao' direction='out'/><arg type='ao' direction='out'/></method>"
  "  <method name='GetSecrets'><arg type='ao' direction='in'/><arg type='o' direction='in'/>"
  "   <arg type='a{o(oayays)}' direction='out'/></method>"
  "  <property name='Collections' type='ao' access='read'/>"
  " </interface>"
  " <interface name='org.freedesktop.Secret.Item'>"
  "  <property name='Locked' type='b' access='read'/>"
  "  <property name='Attributes' type='a{ss}' access='read'/>"
  "  <property name='Label' type='s' access='read'/>"
  "  <property name='Created' type='t' access='read'/>"
  "  <property name='Modified' type='t' access='read'/>"
  " </interface>"
  " <interface name='org.freedesktop.Secret.Session'><method name='Close'/></interface>"
  "</node>";

typedef struct {
  const gchar *path;
  const gchar *label;
  const gchar *attributes[9]; /* name/value pairs, NULL-terminated */
  const guint8 *secret;
  gsize secret_len;
} FssItem;

static const guint8 store_key_bytes[GH_STORE_KEY_SIZE] = {
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
  0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};

static const FssItem fss_items[] = {
  { FSS_COLLECTION "/store_key", GH_STORE_KEY_LABEL,
    { "xdg:schema", GH_STORE_KEY_SCHEMA_NAME, GH_STORE_KEY_ATTR_ACCOUNT, STORE_ACCOUNT,
      GH_STORE_KEY_ATTR_STORE_ID, STORE_ID, GH_STORE_KEY_ATTR_VERSION, GH_STORE_KEY_VERSION,
      NULL },
    store_key_bytes, sizeof store_key_bytes },
  { FSS_COLLECTION "/nip46", "NIP-46 test credential",
    { "xdg:schema", GH_NIP46_CREDENTIAL_SCHEMA, "account", NIP46_ACCOUNT,
      "version", GH_NIP46_CREDENTIAL_VERSION, NULL },
    (const guint8 *)NIP46_SECRET, sizeof NIP46_SECRET - 1 },
};

/* The fake's state. lock guards mode and the counters, which the test reads
 * from the main thread; everything else belongs to the fake's thread. */
static struct {
  GMutex lock;
  GCond cond;
  gboolean ready;
  SessionMode mode;
  guint aes_refused;   /* OpenSession calls refused for the AES algorithm */
  guint plain_refused; /* ... and for "plain" */
  guint plain_opened;  /* plain sessions opened */
  GHashTable *sessions; /* open session paths */
  guint next_session;
  GDBusNodeInfo *node;
  GMainContext *context;
  GMainLoop *loop;
  GThread *thread;
} fss;

static NostrcTestBus *bus;

static const gchar *
item_attribute(const FssItem *item, const gchar *name)
{
  for (guint i = 0; item->attributes[i]; i += 2)
    if (g_str_equal(item->attributes[i], name))
      return item->attributes[i + 1];
  return NULL;
}

static const FssItem *
find_item(const gchar *path)
{
  for (guint i = 0; i < G_N_ELEMENTS(fss_items); i++)
    if (g_str_equal(fss_items[i].path, path))
      return &fss_items[i];
  return NULL;
}

static void
session_call(GDBusConnection *connection, const gchar *sender, const gchar *path,
             const gchar *iface, const gchar *method, GVariant *params,
             GDBusMethodInvocation *invocation, gpointer user_data)
{
  (void)connection; (void)sender; (void)iface; (void)method; (void)params; (void)user_data;
  g_hash_table_remove(fss.sessions, path);
  g_dbus_method_invocation_return_value(invocation, NULL);
}

static void
open_session(GDBusConnection *connection, GVariant *params, GDBusMethodInvocation *invocation)
{
  const gchar *algorithm = NULL;
  g_variant_get(params, "(&sv)", &algorithm, NULL);
  gboolean plain = g_str_equal(algorithm, "plain");
  g_mutex_lock(&fss.lock);
  gboolean accept = plain && fss.mode == MODE_PLAIN;
  if (accept)
    fss.plain_opened++;
  else if (plain)
    fss.plain_refused++;
  else if (g_str_equal(algorithm, AES_ALGORITHM))
    fss.aes_refused++;
  g_mutex_unlock(&fss.lock);
  if (!accept) {
    g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.NotSupported",
                                               "session algorithm not supported");
    return;
  }
  gchar *path = g_strdup_printf(FSS_PATH "/session/s%u", ++fss.next_session);
  static const GDBusInterfaceVTable vtable = { session_call, NULL, NULL, { 0 } };
  GError *error = NULL;
  g_dbus_connection_register_object(connection, path,
    g_dbus_node_info_lookup_interface(fss.node, "org.freedesktop.Secret.Session"),
    &vtable, NULL, NULL, &error);
  g_assert_no_error(error);
  g_hash_table_add(fss.sessions, g_strdup(path));
  g_dbus_method_invocation_return_value(invocation,
    g_variant_new("(@vo)", g_variant_new_variant(g_variant_new_string("")), path));
  g_free(path);
}

static void
search_items(GVariant *params, GDBusMethodInvocation *invocation)
{
  GVariant *wanted = g_variant_get_child_value(params, 0);
  GVariantBuilder unlocked, locked;
  g_variant_builder_init(&unlocked, G_VARIANT_TYPE("ao"));
  g_variant_builder_init(&locked, G_VARIANT_TYPE("ao"));
  for (guint i = 0; i < G_N_ELEMENTS(fss_items); i++) {
    gboolean match = TRUE;
    GVariantIter iter;
    const gchar *name, *value;
    g_variant_iter_init(&iter, wanted);
    while (match && g_variant_iter_next(&iter, "{&s&s}", &name, &value))
      match = g_strcmp0(item_attribute(&fss_items[i], name), value) == 0;
    if (match)
      g_variant_builder_add(&unlocked, "o", fss_items[i].path);
  }
  g_variant_unref(wanted);
  g_dbus_method_invocation_return_value(invocation, g_variant_new("(aoao)", &unlocked, &locked));
}

static void
get_secrets(GVariant *params, GDBusMethodInvocation *invocation)
{
  GVariantIter *paths = NULL;
  const gchar *session = NULL, *path = NULL;
  g_variant_get(params, "(ao&o)", &paths, &session);
  if (!g_hash_table_contains(fss.sessions, session)) {
    g_variant_iter_free(paths);
    g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.Secret.Error.NoSession",
                                               "no such session");
    return;
  }
  GVariantBuilder out;
  g_variant_builder_init(&out, G_VARIANT_TYPE("a{o(oayays)}"));
  while (g_variant_iter_next(paths, "&o", &path)) {
    const FssItem *item = find_item(path);
    if (!item)
      continue;
    g_variant_builder_add(&out, "{o@(oayays)}", item->path,
      g_variant_new("(o@ay@ays)", session, g_variant_new_array(G_VARIANT_TYPE_BYTE, NULL, 0),
                    g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, item->secret,
                                              item->secret_len, 1),
                    "application/octet-stream"));
  }
  g_variant_iter_free(paths);
  g_dbus_method_invocation_return_value(invocation, g_variant_new("(a{o(oayays)})", &out));
}

static void
service_call(GDBusConnection *connection, const gchar *sender, const gchar *path,
             const gchar *iface, const gchar *method, GVariant *params,
             GDBusMethodInvocation *invocation, gpointer user_data)
{
  (void)sender; (void)path; (void)iface; (void)user_data;
  if (g_str_equal(method, "OpenSession"))
    open_session(connection, params, invocation);
  else if (g_str_equal(method, "SearchItems"))
    search_items(params, invocation);
  else if (g_str_equal(method, "GetSecrets"))
    get_secrets(params, invocation);
  else
    g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.UnknownMethod",
                                               method);
}

static GVariant *
service_property(GDBusConnection *connection, const gchar *sender, const gchar *path,
                 const gchar *iface, const gchar *property, GError **error, gpointer user_data)
{
  (void)connection; (void)sender; (void)path; (void)iface; (void)error; (void)user_data;
  if (g_str_equal(property, "Collections")) {
    const gchar *collections[] = { FSS_COLLECTION, NULL };
    return g_variant_new_objv(collections, -1);
  }
  return NULL;
}

static GVariant *
item_property(GDBusConnection *connection, const gchar *sender, const gchar *path,
              const gchar *iface, const gchar *property, GError **error, gpointer user_data)
{
  (void)connection; (void)sender; (void)path; (void)iface; (void)error;
  const FssItem *item = user_data;
  if (g_str_equal(property, "Locked"))
    return g_variant_new_boolean(FALSE);
  if (g_str_equal(property, "Label"))
    return g_variant_new_string(item->label);
  if (g_str_equal(property, "Created") || g_str_equal(property, "Modified"))
    return g_variant_new_uint64(1700000000);
  if (g_str_equal(property, "Attributes")) {
    GVariantBuilder attributes;
    g_variant_builder_init(&attributes, G_VARIANT_TYPE("a{ss}"));
    for (guint i = 0; item->attributes[i]; i += 2)
      g_variant_builder_add(&attributes, "{ss}", item->attributes[i], item->attributes[i + 1]);
    return g_variant_builder_end(&attributes);
  }
  return NULL;
}

/* Registers the objects, then takes the name, and serves until the main
 * thread quits the loop (after nostrc_test_bus_down()). */
static gpointer
fss_thread(gpointer user_data)
{
  (void)user_data;
  g_main_context_push_thread_default(fss.context);
  /* Dispatches in fss.context (this thread's), owned by the bus. */
  GDBusConnection *connection = nostrc_test_bus_connect(bus);
  static const GDBusInterfaceVTable service_vtable = { service_call, service_property, NULL, { 0 } };
  static const GDBusInterfaceVTable item_vtable = { NULL, item_property, NULL, { 0 } };
  GError *error = NULL;
  g_dbus_connection_register_object(connection, FSS_PATH,
    g_dbus_node_info_lookup_interface(fss.node, "org.freedesktop.Secret.Service"),
    &service_vtable, NULL, NULL, &error);
  g_assert_no_error(error);
  for (guint i = 0; i < G_N_ELEMENTS(fss_items); i++) {
    g_dbus_connection_register_object(connection, fss_items[i].path,
      g_dbus_node_info_lookup_interface(fss.node, "org.freedesktop.Secret.Item"),
      &item_vtable, (gpointer)&fss_items[i], NULL, &error);
    g_assert_no_error(error);
  }
  GVariant *reply = g_dbus_connection_call_sync(connection, "org.freedesktop.DBus",
    "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
    g_variant_new("(su)", FSS_NAME, 4u /* DO_NOT_QUEUE */), G_VARIANT_TYPE("(u)"),
    G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
  g_assert_no_error(error);
  guint32 result = 0;
  g_variant_get(reply, "(u)", &result);
  g_variant_unref(reply);
  g_assert_cmpuint(result, ==, 1); /* primary owner */

  g_mutex_lock(&fss.lock);
  fss.ready = TRUE;
  g_cond_signal(&fss.cond);
  g_mutex_unlock(&fss.lock);
  g_main_loop_run(fss.loop);
  g_main_context_pop_thread_default(fss.context);
  return NULL;
}

static void
fss_start(void)
{
  GError *error = NULL;
  fss.node = g_dbus_node_info_new_for_xml(fss_xml, &error);
  g_assert_no_error(error);
  fss.sessions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  fss.context = g_main_context_new();
  fss.loop = g_main_loop_new(fss.context, FALSE);
  fss.thread = g_thread_new("fake-secret-service", fss_thread, NULL);
  g_mutex_lock(&fss.lock);
  while (!fss.ready)
    g_cond_wait(&fss.cond, &fss.lock);
  g_mutex_unlock(&fss.lock);
}

/* After nostrc_test_bus_down(): the connection has seen the bus go. */
static void
fss_stop(void)
{
  g_main_loop_quit(fss.loop);
  g_thread_join(fss.thread);
  g_main_loop_unref(fss.loop);
  g_main_context_unref(fss.context);
  g_hash_table_unref(fss.sessions);
  g_dbus_node_info_unref(fss.node);
}

/* Switch the fake's mode and drop libsecret's shared proxy and its session,
 * so the next lookup opens a session against the new mode. */
static void
reset(SessionMode mode)
{
  secret_service_disconnect();
  g_mutex_lock(&fss.lock);
  fss.mode = mode;
  fss.aes_refused = fss.plain_refused = fss.plain_opened = 0;
  g_mutex_unlock(&fss.lock);
}

/* The libsecret proxy another component of the process may have created
 * first, with no session open. (transfer full) */
static SecretService *
shared_service_without_session(void)
{
  GError *error = NULL;
  SecretService *service = secret_service_get_sync(SECRET_SERVICE_NONE, NULL, &error);
  g_assert_no_error(error);
  g_assert_null(secret_service_get_session_algorithms(service));
  return service;
}

static void
assert_counts(guint aes_refused, guint plain_refused, guint plain_opened)
{
  g_mutex_lock(&fss.lock);
  guint aes = fss.aes_refused, refused = fss.plain_refused, opened = fss.plain_opened;
  g_mutex_unlock(&fss.lock);
  g_assert_cmpuint(aes, ==, aes_refused);
  g_assert_cmpuint(refused, ==, plain_refused);
  g_assert_cmpuint(opened, ==, plain_opened);
}

GType gh_store_key_secret_service_get_type(void); /* private implementation */

static GhTestKeyResult
store_key_lookup(void)
{
  GhStoreKeyBackend *backend = g_object_new(gh_store_key_secret_service_get_type(), NULL);
  GhStoreKey *store_key = gh_store_key_new(backend);
  g_object_unref(backend);
  GhTestKeyResult result = gh_test_lookup(store_key, STORE_ACCOUNT, GH_STORE_KEY_FLAGS_NONE, NULL);
  g_object_unref(store_key);
  return result;
}

/* The NIP-46 backend's secret-loading search (a lookup's first step). */
static GPtrArray *
nip46_search(GError **error)
{
  GhNip46CredentialBackend *backend = gh_nip46_credentials_secret_service_new();
  GPtrArray *items = backend->search(backend, NIP46_ACCOUNT, FALSE, TRUE, NULL, error);
  backend->free(backend);
  return items;
}

static void
check_store_key_found(void)
{
  GhTestKeyResult result = store_key_lookup();
  g_assert_no_error(result.error);
  g_assert_nonnull(result.key);
  g_assert_cmpmem(g_bytes_get_data(result.key, NULL), g_bytes_get_size(result.key),
                  store_key_bytes, sizeof store_key_bytes);
  g_assert_cmpstr(result.store_id, ==, STORE_ID);
  gh_test_key_result_clear(&result);
}

static void
check_nip46_found(void)
{
  GError *error = NULL;
  GPtrArray *items = nip46_search(&error);
  g_assert_no_error(error);
  g_assert_nonnull(items);
  g_assert_cmpuint(items->len, ==, 1);
  const GhNip46CredentialItem *item = g_ptr_array_index(items, 0);
  g_assert_cmpstr(item->account, ==, NIP46_ACCOUNT);
  g_assert_true(item->attributes_valid);
  g_assert_false(item->locked);
  g_assert_nonnull(item->secret);
  g_assert_cmpmem(g_bytes_get_data(item->secret, NULL), g_bytes_get_size(item->secret),
                  NIP46_SECRET, strlen(NIP46_SECRET));
  g_ptr_array_unref(items);
}

static void
check_store_key_unavailable(void)
{
  GhTestKeyResult result = store_key_lookup();
  g_assert_error(result.error, GH_STORE_KEY_ERROR, GH_STORE_KEY_ERROR_UNAVAILABLE);
  g_assert_null(result.key);
  gh_test_key_result_clear(&result);
}

static void
check_nip46_unavailable(void)
{
  GError *error = NULL;
  GPtrArray *items = nip46_search(&error);
  g_assert_error(error, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_UNAVAILABLE);
  g_assert_null(items);
  g_clear_error(&error);
}

/* A plain-only provider: each fresh proxy is refused AES once, then opens
 * one plain session, and both lookups read their secret through it. */
static void
test_plain_fresh(void)
{
  reset(MODE_PLAIN);
  check_store_key_found();
  assert_counts(1, 0, 1);
  reset(MODE_PLAIN);
  check_nip46_found();
  assert_counts(1, 0, 1);
}

/* The same with the shared proxy already created without a session: the
 * session is still opened before anything loads a secret. */
static void
test_plain_cached(void)
{
  reset(MODE_PLAIN);
  SecretService *service = shared_service_without_session();
  check_store_key_found();
  check_nip46_found();
  assert_counts(1, 0, 1); /* one session, shared by both */
  g_assert_cmpstr(secret_service_get_session_algorithms(service), ==, "plain");
  g_object_unref(service);
}

/* A provider refusing every session: both backends report UNAVAILABLE, the
 * existing "no Secret Service" state, and read nothing. */
static void
test_no_session_fresh(void)
{
  reset(MODE_NONE);
  check_store_key_unavailable();
  assert_counts(1, 1, 0);
  reset(MODE_NONE);
  check_nip46_unavailable();
  assert_counts(1, 1, 0);
}

static void
test_no_session_cached(void)
{
  reset(MODE_NONE);
  SecretService *service = shared_service_without_session();
  check_store_key_unavailable();
  check_nip46_unavailable();
  assert_counts(2, 2, 0);
  g_assert_null(secret_service_get_session_algorithms(service));
  g_object_unref(service);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  if (!nostrc_test_bus_available()) {
    g_print("SKIP: dbus-daemon is not installed\n");
    return 77;
  }
  bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
  nostrc_test_bus_up(bus);
  fss_start();
  nostrc_test_bus_add_func("/secret-service-sessions/plain/fresh", test_plain_fresh);
  nostrc_test_bus_add_func("/secret-service-sessions/plain/cached-without-session", test_plain_cached);
  nostrc_test_bus_add_func("/secret-service-sessions/none/fresh", test_no_session_fresh);
  nostrc_test_bus_add_func("/secret-service-sessions/none/cached-without-session",
                           test_no_session_cached);
  int status = g_test_run();
  secret_service_disconnect();
  nostrc_test_bus_down(bus);
  fss_stop();
  return status;
}
