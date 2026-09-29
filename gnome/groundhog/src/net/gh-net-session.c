#include "gh-net-session.h"

#include <string.h>
#include <sys/random.h>

#define PROBE_TIMEOUT_S 10

G_DEFINE_ENUM_TYPE(GhNetMode, gh_net_mode,
  G_DEFINE_ENUM_VALUE(GH_NET_MODE_SYSTEM, "system"),
  G_DEFINE_ENUM_VALUE(GH_NET_MODE_NONE, "none"),
  G_DEFINE_ENUM_VALUE(GH_NET_MODE_TOR, "tor"))

G_DEFINE_ENUM_TYPE(GhNetTorState, gh_net_tor_state,
  G_DEFINE_ENUM_VALUE(GH_NET_TOR_OFF, "off"),
  G_DEFINE_ENUM_VALUE(GH_NET_TOR_CHECKING, "checking"),
  G_DEFINE_ENUM_VALUE(GH_NET_TOR_READY, "ready"),
  G_DEFINE_ENUM_VALUE(GH_NET_TOR_UNREACHABLE, "unreachable"))

/* ---- pure helpers ---------------------------------------------------------------- */

GhNetMode
gh_net_mode_from_string(const gchar *mode)
{
  if (g_strcmp0(mode, "system") == 0)
    return GH_NET_MODE_SYSTEM;
  if (g_strcmp0(mode, "none") == 0)
    return GH_NET_MODE_NONE;
  return GH_NET_MODE_TOR;
}

gboolean
gh_net_host_is_onion(const gchar *host)
{
  gsize length = host ? strlen(host) : 0;
  /* A trailing dot is the same name. */
  if (length > 0 && host[length - 1] == '.')
    length--;
  return length > strlen(".onion") &&
         g_ascii_strncasecmp(host + length - strlen(".onion"), ".onion", strlen(".onion")) == 0;
}

gboolean
gh_net_host_is_loopback(const gchar *host)
{
  if (!host)
    return FALSE;
  if (g_ascii_strcasecmp(host, "localhost") == 0)
    return TRUE;
  g_autoptr(GInetAddress) address = g_inet_address_new_from_string(host);
  return address && g_inet_address_get_is_loopback(address);
}

gboolean
gh_net_relay_url_allowed(GhNetMode mode, const gchar *url, GError **error)
{
  g_autoptr(GUri) uri = url ? g_uri_parse(url, G_URI_FLAGS_NONE, NULL) : NULL;
  const gchar *scheme = uri ? g_uri_get_scheme(uri) : NULL;
  const gchar *host = uri ? g_uri_get_host(uri) : NULL;
  if (!host || !*host ||
      (g_strcmp0(scheme, "ws") != 0 && g_strcmp0(scheme, "wss") != 0)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Not a relay address");
    return FALSE;
  }
  gboolean onion = gh_net_host_is_onion(host);
  if (onion && mode != GH_NET_MODE_TOR) {
    /* Never resolved or dialled directly: that would ask the local DNS. */
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        ".onion relays can only be reached through Tor");
    return FALSE;
  }
  if (g_str_equal(scheme, "ws") && !gh_net_host_is_loopback(host) && !onion) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        "Insecure relay skipped: ws:// is only used for this computer "
                        "and .onion relays");
    return FALSE;
  }
  return TRUE;
}

static guint8 salt[32];

static void
init_salt(void)
{
  static gsize once = 0;
  if (!g_once_init_enter(&once))
    return;
  if (getentropy(salt, sizeof salt) != 0) {
    /* No kernel randomness: never a fixed salt. */
    for (gsize i = 0; i < sizeof salt; i += sizeof(guint32)) {
      guint32 value = g_random_int();
      memcpy(salt + i, &value, sizeof value);
    }
  }
  g_once_init_leave(&once, 1);
}

/* 32 hex digits: the username (first 16) and password (last 16). */
static gchar *
isolation_digest(const gchar *isolation)
{
  init_salt();
  g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA256);
  g_checksum_update(checksum, (const guchar *)isolation, strlen(isolation));
  g_checksum_update(checksum, (const guchar *)"\0", 1);
  g_checksum_update(checksum, salt, sizeof salt);
  return g_strndup(g_checksum_get_string(checksum), 32);
}

gchar *
gh_net_isolation_username(const gchar *isolation)
{
  g_return_val_if_fail(isolation != NULL, NULL);
  g_autofree gchar *digest = isolation_digest(isolation);
  return g_strndup(digest, 16);
}

/* host:port, bracketing an IPv6 host; NULL if address is not host:port. */
static gchar *
normalized_proxy_address(const gchar *address)
{
  g_autofree gchar *trimmed = g_strstrip(g_strdup(address ? address : ""));
  if (!*trimmed || strstr(trimmed, "://"))
    return NULL;
  g_autoptr(GSocketConnectable) parsed =
    g_network_address_parse(trimmed, GH_NET_TOR_DEFAULT_PORT, NULL);
  if (!parsed)
    return NULL;
  const gchar *host = g_network_address_get_hostname(G_NETWORK_ADDRESS(parsed));
  guint16 port = g_network_address_get_port(G_NETWORK_ADDRESS(parsed));
  if (!host || !*host || port == 0 || strpbrk(host, "@/?#[] "))
    return NULL;
  return strchr(host, ':') ? g_strdup_printf("[%s]:%u", host, port)
                           : g_strdup_printf("%s:%u", host, port);
}

GProxyResolver *
gh_net_proxy_resolver_new(GhNetMode mode, const gchar *tor_address, const gchar *isolation,
                          GError **error)
{
  switch (mode) {
  case GH_NET_MODE_SYSTEM:
    return g_object_ref(g_proxy_resolver_get_default());
  case GH_NET_MODE_NONE:
    return g_simple_proxy_resolver_new(NULL, NULL);
  case GH_NET_MODE_TOR:
    break;
  }
  g_autofree gchar *address = normalized_proxy_address(tor_address);
  if (!address) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "The Tor proxy address is not host:port");
    return NULL;
  }
  g_autofree gchar *random = isolation ? NULL : g_uuid_string_random();
  g_autofree gchar *digest = isolation_digest(isolation ? isolation : random);
  g_autofree gchar *proxy = g_strdup_printf("socks5://%.16s:%.16s@%s", digest, digest + 16,
                                            address);
  /* The only proxy for every URI, with no ignore list: no host is ever
   * reached directly (P5). */
  return g_simple_proxy_resolver_new(proxy, NULL);
}

/* ---- the session ----------------------------------------------------------------- */

struct _GhNetSession {
  GObject parent_instance;
  GSettings *settings;
  GMainContext *context;     /* the owning (main) context */
  GMutex lock;               /* mode, tor_address, serial */
  GhNetMode mode;
  gchar *tor_address;
  guint64 serial;
  GhNetTorState tor_state;   /* owning context */
  GCancellable *probe;       /* the SOCKS check in flight */
};

G_DEFINE_FINAL_TYPE(GhNetSession, gh_net_session, G_TYPE_OBJECT)

enum { PROP_0, PROP_MODE, PROP_TOR_STATE, N_PROPS };
static GParamSpec *props[N_PROPS];
enum { SIGNAL_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static GMutex default_lock;
static GhNetSession *default_session;

GhNetSession *
gh_net_session_dup_default(void)
{
  g_mutex_lock(&default_lock);
  GhNetSession *session = default_session ? g_object_ref(default_session) : NULL;
  g_mutex_unlock(&default_lock);
  return session;
}

void
gh_net_session_set_default(GhNetSession *session)
{
  g_return_if_fail(!session || GH_IS_NET_SESSION(session));
  g_mutex_lock(&default_lock);
  GhNetSession *old = default_session;
  default_session = session ? g_object_ref(session) : NULL;
  g_mutex_unlock(&default_lock);
  g_clear_object(&old);
}

static void
set_tor_state(GhNetSession *self, GhNetTorState state)
{
  if (self->tor_state == state)
    return;
  self->tor_state = state;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_TOR_STATE]);
}

typedef struct {
  GhNetSession *session;
  GCancellable *cancellable;
  guint64 serial;
  GSocketConnection *connection;
  guint8 reply[2];
} Probe;

static void
probe_free(Probe *probe)
{
  if (probe->connection)
    (void)g_io_stream_close(G_IO_STREAM(probe->connection), NULL, NULL);
  g_clear_object(&probe->connection);
  g_clear_object(&probe->cancellable);
  g_object_unref(probe->session);
  g_free(probe);
}

/* The check's verdict, if it is still the current one. */
static void
probe_done(Probe *probe, gboolean reachable)
{
  GhNetSession *self = probe->session;
  if (!g_cancellable_is_cancelled(probe->cancellable) && self->probe == probe->cancellable) {
    g_clear_object(&self->probe);
    set_tor_state(self, reachable ? GH_NET_TOR_READY : GH_NET_TOR_UNREACHABLE);
  }
  probe_free(probe);
}

static void
on_probe_read(GObject *source, GAsyncResult *result, gpointer data)
{
  Probe *probe = data;
  gsize read = 0;
  gboolean ok = g_input_stream_read_all_finish(G_INPUT_STREAM(source), result, &read, NULL);
  /* A SOCKS5 server answers the greeting with version 5 and a method it
   * accepts (none or username/password), never 0xff for these two. */
  probe_done(probe, ok && read == 2 && probe->reply[0] == 0x05 &&
                      (probe->reply[1] == 0x00 || probe->reply[1] == 0x02));
}

static void
on_probe_written(GObject *source, GAsyncResult *result, gpointer data)
{
  Probe *probe = data;
  if (!g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result, NULL, NULL)) {
    probe_done(probe, FALSE);
    return;
  }
  GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(probe->connection));
  g_input_stream_read_all_async(input, probe->reply, sizeof probe->reply, G_PRIORITY_DEFAULT,
                                probe->cancellable, on_probe_read, probe);
}

static void
on_probe_connected(GObject *source, GAsyncResult *result, gpointer data)
{
  Probe *probe = data;
  probe->connection = g_socket_client_connect_finish(G_SOCKET_CLIENT(source), result, NULL);
  if (!probe->connection) {
    probe_done(probe, FALSE);
    return;
  }
  /* Greeting only (RFC 1928 §3): version 5, methods "none" and
   * username/password. Nothing is asked to connect anywhere. */
  static const guint8 greeting[] = { 0x05, 0x02, 0x00, 0x02 };
  GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(probe->connection));
  g_output_stream_write_all_async(output, greeting, sizeof greeting, G_PRIORITY_DEFAULT,
                                  probe->cancellable, on_probe_written, probe);
}

void
gh_net_session_check_tor(GhNetSession *self)
{
  g_return_if_fail(GH_IS_NET_SESSION(self));
  g_mutex_lock(&self->lock);
  gboolean tor = self->mode == GH_NET_MODE_TOR;
  g_autofree gchar *address = g_strdup(self->tor_address);
  guint64 serial = self->serial;
  g_mutex_unlock(&self->lock);
  if (!tor || self->probe)
    return;
  g_autofree gchar *normalized = normalized_proxy_address(address);
  g_autoptr(GSocketConnectable) connectable =
    normalized ? g_network_address_parse(normalized, GH_NET_TOR_DEFAULT_PORT, NULL) : NULL;
  if (!connectable) {
    set_tor_state(self, GH_NET_TOR_UNREACHABLE);
    return;
  }
  set_tor_state(self, GH_NET_TOR_CHECKING);
  Probe *probe = g_new0(Probe, 1);
  probe->session = g_object_ref(self);
  probe->serial = serial;
  probe->cancellable = g_cancellable_new();
  self->probe = g_object_ref(probe->cancellable);
  /* The proxy itself is reached directly, never through another proxy. */
  g_autoptr(GSocketClient) client = g_socket_client_new();
  g_socket_client_set_enable_proxy(client, FALSE);
  g_socket_client_set_timeout(client, PROBE_TIMEOUT_S);
  g_socket_client_connect_async(client, connectable, probe->cancellable, on_probe_connected,
                                probe);
}

static void
cancel_probe(GhNetSession *self)
{
  if (self->probe)
    g_cancellable_cancel(self->probe);
  g_clear_object(&self->probe);
}

static void
apply(GhNetSession *self, GhNetMode mode, const gchar *tor_address)
{
  g_mutex_lock(&self->lock);
  gboolean same = self->mode == mode &&
                  (mode != GH_NET_MODE_TOR || g_strcmp0(self->tor_address, tor_address) == 0);
  if (!same) {
    self->mode = mode;
    g_free(self->tor_address);
    self->tor_address = g_strdup(tor_address);
    self->serial++;
  }
  g_mutex_unlock(&self->lock);
  if (same)
    return;
  cancel_probe(self);
  if (mode == GH_NET_MODE_TOR)
    gh_net_session_check_tor(self);
  else
    set_tor_state(self, GH_NET_TOR_OFF);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_MODE]);
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
}

static gboolean
settings_have(GSettings *settings, const gchar *key)
{
  g_autoptr(GSettingsSchema) schema = NULL;
  g_object_get(settings, "settings-schema", &schema, NULL);
  return schema && g_settings_schema_has_key(schema, key);
}

static void
load_settings(GhNetSession *self)
{
  g_autofree gchar *mode = settings_have(self->settings, "network-mode")
                             ? g_settings_get_string(self->settings, "network-mode")
                             : g_strdup("system");
  g_autofree gchar *address = settings_have(self->settings, "tor-socks-address")
                                ? g_settings_get_string(self->settings, "tor-socks-address")
                                : NULL;
  apply(self, gh_net_mode_from_string(mode), address);
}

static void
on_settings_changed(GSettings *settings, const gchar *key, gpointer data)
{
  (void)settings;
  if (g_strcmp0(key, "network-mode") == 0 || g_strcmp0(key, "tor-socks-address") == 0)
    load_settings(GH_NET_SESSION(data));
}

GhNetSession *
gh_net_session_new(GSettings *settings)
{
  g_return_val_if_fail(!settings || G_IS_SETTINGS(settings), NULL);
  GhNetSession *self = g_object_new(GH_TYPE_NET_SESSION, NULL);
  if (settings) {
    self->settings = g_object_ref(settings);
    g_signal_connect_object(settings, "changed", G_CALLBACK(on_settings_changed), self, 0);
    load_settings(self);
  }
  return self;
}

void
gh_net_session_set_mode(GhNetSession *self, GhNetMode mode, const gchar *tor_address)
{
  g_return_if_fail(GH_IS_NET_SESSION(self));
  g_return_if_fail(self->settings == NULL);
  apply(self, mode, tor_address);
}

GhNetMode
gh_net_session_get_mode(GhNetSession *self)
{
  g_return_val_if_fail(GH_IS_NET_SESSION(self), GH_NET_MODE_TOR);
  g_mutex_lock(&self->lock);
  GhNetMode mode = self->mode;
  g_mutex_unlock(&self->lock);
  return mode;
}

gchar *
gh_net_session_dup_tor_address(GhNetSession *self)
{
  g_return_val_if_fail(GH_IS_NET_SESSION(self), NULL);
  g_mutex_lock(&self->lock);
  gchar *address = g_strdup(self->tor_address);
  g_mutex_unlock(&self->lock);
  return address;
}

guint64
gh_net_session_get_serial(GhNetSession *self)
{
  g_return_val_if_fail(GH_IS_NET_SESSION(self), 0);
  g_mutex_lock(&self->lock);
  guint64 serial = self->serial;
  g_mutex_unlock(&self->lock);
  return serial;
}

GProxyResolver *
gh_net_session_dup_resolver(GhNetSession *self, const gchar *isolation, GError **error)
{
  g_return_val_if_fail(GH_IS_NET_SESSION(self), NULL);
  g_mutex_lock(&self->lock);
  GhNetMode mode = self->mode;
  g_autofree gchar *address = g_strdup(self->tor_address);
  g_mutex_unlock(&self->lock);
  return gh_net_proxy_resolver_new(mode, address, isolation, error);
}

GhNetTorState
gh_net_session_get_tor_state(GhNetSession *self)
{
  g_return_val_if_fail(GH_IS_NET_SESSION(self), GH_NET_TOR_OFF);
  return self->tor_state;
}

typedef struct {
  GhNetSession *session;
  gboolean connected;
  guint64 serial;
} Report;

static gboolean
deliver_report(gpointer data)
{
  Report *report = data;
  GhNetSession *self = report->session;
  if (gh_net_session_get_mode(self) == GH_NET_MODE_TOR &&
      gh_net_session_get_serial(self) == report->serial) {
    if (report->connected) {
      cancel_probe(self);
      set_tor_state(self, GH_NET_TOR_READY);
    } else {
      gh_net_session_check_tor(self);
    }
  }
  return G_SOURCE_REMOVE;
}

static void
report_free(gpointer data)
{
  Report *report = data;
  g_object_unref(report->session);
  g_free(report);
}

void
gh_net_session_report(GhNetSession *self, gboolean connected)
{
  g_return_if_fail(GH_IS_NET_SESSION(self));
  Report *report = g_new0(Report, 1);
  report->session = g_object_ref(self);
  report->connected = connected;
  report->serial = gh_net_session_get_serial(self);
  /* Always queued on the owning context, never run inside a transport's
   * callback. */
  GSource *source = g_idle_source_new();
  g_source_set_callback(source, deliver_report, report, report_free);
  g_source_attach(source, self->context);
  g_source_unref(source);
}

static void
gh_net_session_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  GhNetSession *self = GH_NET_SESSION(object);
  switch (id) {
  case PROP_MODE:
    g_value_set_enum(value, gh_net_session_get_mode(self));
    break;
  case PROP_TOR_STATE:
    g_value_set_enum(value, self->tor_state);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gh_net_session_dispose(GObject *object)
{
  GhNetSession *self = GH_NET_SESSION(object);
  cancel_probe(self);
  if (self->settings)
    g_signal_handlers_disconnect_by_data(self->settings, self);
  g_clear_object(&self->settings);
  G_OBJECT_CLASS(gh_net_session_parent_class)->dispose(object);
}

static void
gh_net_session_finalize(GObject *object)
{
  GhNetSession *self = GH_NET_SESSION(object);
  g_free(self->tor_address);
  g_main_context_unref(self->context);
  g_mutex_clear(&self->lock);
  G_OBJECT_CLASS(gh_net_session_parent_class)->finalize(object);
}

static void
gh_net_session_class_init(GhNetSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->get_property = gh_net_session_get_property;
  object_class->dispose = gh_net_session_dispose;
  object_class->finalize = gh_net_session_finalize;
  props[PROP_MODE] = g_param_spec_enum("mode", NULL, NULL, GH_TYPE_NET_MODE,
                                       GH_NET_MODE_SYSTEM,
                                       G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  props[PROP_TOR_STATE] = g_param_spec_enum("tor-state", NULL, NULL, GH_TYPE_NET_TOR_STATE,
                                            GH_NET_TOR_OFF,
                                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(object_class, N_PROPS, props);
  signals[SIGNAL_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                                         0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
gh_net_session_init(GhNetSession *self)
{
  g_mutex_init(&self->lock);
  self->mode = GH_NET_MODE_SYSTEM;
  self->tor_state = GH_NET_TOR_OFF;
  self->context = g_main_context_ref_thread_default();
}
