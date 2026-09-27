/* nostr-publish-session-relay.c - Upstream status of the per-user session relay
 *
 * SPDX-License-Identifier: MIT
 *
 * See nostr-publish-session-relay.h. Everything is asynchronous on the
 * construction-time thread-default main context. A generation counter
 * guards FederationState reads: a reply that was requested before the
 * owner vanished (or before a newer read) is ignored, so a stale "active"
 * can never overwrite "not-running".
 */

#include "nostr-publish-session-relay.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define NP_SR_CALL_TIMEOUT_MS 5000

struct _NostrPublishSessionRelay {
  GDBusConnection           *bus;
  NostrPublishFederationFunc federation_cb;
  NostrPublishForwardFunc    forward_cb;
  gpointer                   user_data;
  NostrPublishFederation     federation;
  gboolean                   has_owner;
  guint                      generation;
  guint                      watch_id;
  guint                      signal_id;
  GCancellable              *cancellable;
};

/* ---- State names ---- */

static const struct {
  NostrPublishFederation state;
  const gchar           *name;
} FEDERATION_NAMES[] = {
  { NOSTR_PUBLISH_FEDERATION_ACTIVE,              "active" },
  { NOSTR_PUBLISH_FEDERATION_WAITING_FOR_ACCOUNT, "waiting-for-account" },
  { NOSTR_PUBLISH_FEDERATION_DISABLED,            "disabled" },
  { NOSTR_PUBLISH_FEDERATION_UNAVAILABLE,         "unavailable" },
  { NOSTR_PUBLISH_FEDERATION_UNKNOWN,             "unknown" },
  { NOSTR_PUBLISH_FEDERATION_NOT_RUNNING,         "not-running" },
  { NOSTR_PUBLISH_FEDERATION_UNSUPPORTED,         "unsupported" },
};

NostrPublishFederation
nostr_publish_federation_from_string(const gchar *state)
{
  /* Only the four D-Bus values parse; the client-side words never come
   * from the daemon, so they (and anything new) are UNSUPPORTED. */
  for (gsize i = 0; i < 4; i++)
    if (g_strcmp0(state, FEDERATION_NAMES[i].name) == 0)
      return FEDERATION_NAMES[i].state;
  return NOSTR_PUBLISH_FEDERATION_UNSUPPORTED;
}

const gchar *
nostr_publish_federation_to_string(NostrPublishFederation state)
{
  for (gsize i = 0; i < G_N_ELEMENTS(FEDERATION_NAMES); i++)
    if (FEDERATION_NAMES[i].state == state)
      return FEDERATION_NAMES[i].name;
  return "unknown";
}

gboolean
nostr_publish_federation_forwards(NostrPublishFederation state)
{
  return state == NOSTR_PUBLISH_FEDERATION_ACTIVE ||
         state == NOSTR_PUBLISH_FEDERATION_WAITING_FOR_ACCOUNT;
}

static const struct {
  NostrPublishForwardState state;
  const gchar             *name;
} FORWARD_NAMES[] = {
  { NOSTR_PUBLISH_FORWARD_UNKNOWN,    "unknown" },
  { NOSTR_PUBLISH_FORWARD_NEW,        "new" },
  { NOSTR_PUBLISH_FORWARD_UNROUTABLE, "unroutable" },
  { NOSTR_PUBLISH_FORWARD_PENDING,    "pending" },
  { NOSTR_PUBLISH_FORWARD_FORWARDED,  "forwarded" },
  { NOSTR_PUBLISH_FORWARD_PARTIAL,    "partial" },
  { NOSTR_PUBLISH_FORWARD_FAILED,     "failed" },
  { NOSTR_PUBLISH_FORWARD_SKIPPED,    "skipped" },
  { NOSTR_PUBLISH_FORWARD_SUPERSEDED, "superseded" },
  { NOSTR_PUBLISH_FORWARD_CANCELLED,  "cancelled" },
};

NostrPublishForwardState
nostr_publish_forward_state_from_string(const gchar *state)
{
  for (gsize i = 0; i < G_N_ELEMENTS(FORWARD_NAMES); i++)
    if (g_strcmp0(state, FORWARD_NAMES[i].name) == 0)
      return FORWARD_NAMES[i].state;
  return NOSTR_PUBLISH_FORWARD_UNKNOWN;
}

const gchar *
nostr_publish_forward_state_to_string(NostrPublishForwardState state)
{
  for (gsize i = 0; i < G_N_ELEMENTS(FORWARD_NAMES); i++)
    if (FORWARD_NAMES[i].state == state)
      return FORWARD_NAMES[i].name;
  return "unknown";
}

gboolean
nostr_publish_forward_state_delivered(NostrPublishForwardState state)
{
  return state == NOSTR_PUBLISH_FORWARD_FORWARDED ||
         state == NOSTR_PUBLISH_FORWARD_PARTIAL;
}

gboolean
nostr_publish_forward_state_is_final(NostrPublishForwardState state)
{
  switch (state) {
  case NOSTR_PUBLISH_FORWARD_NEW:
  case NOSTR_PUBLISH_FORWARD_UNROUTABLE:
  case NOSTR_PUBLISH_FORWARD_PENDING:
    return FALSE;
  case NOSTR_PUBLISH_FORWARD_UNKNOWN:
  case NOSTR_PUBLISH_FORWARD_FORWARDED:
  case NOSTR_PUBLISH_FORWARD_PARTIAL:
  case NOSTR_PUBLISH_FORWARD_FAILED:
  case NOSTR_PUBLISH_FORWARD_SKIPPED:
  case NOSTR_PUBLISH_FORWARD_SUPERSEDED:
  case NOSTR_PUBLISH_FORWARD_CANCELLED:
    break;
  }
  return TRUE;
}

/* ---- Federation state ---- */

static void
set_federation(NostrPublishSessionRelay *self, NostrPublishFederation state)
{
  if (self->federation == state)
    return;
  self->federation = state;
  if (self->federation_cb != NULL)
    self->federation_cb(self, state, self->user_data);
}

typedef struct {
  NostrPublishSessionRelay *self;
  guint                     generation;
} FederationRead;

static gboolean
error_means_no_owner(const GError *error)
{
  return g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER) ||
         g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN);
}

static void
on_federation_reply(GObject *source, GAsyncResult *res, gpointer user_data)
{
  FederationRead *read = user_data;
  GError *error = NULL;
  GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    /* Freed (or superseded by free): @read->self may be gone. */
    g_error_free(error);
    g_free(read);
    return;
  }
  NostrPublishSessionRelay *self = read->self;
  gboolean current = read->generation == self->generation;
  g_free(read);

  NostrPublishFederation state;
  if (reply != NULL) {
    GVariant *inner = NULL;
    g_variant_get(reply, "(v)", &inner);
    state = g_variant_is_of_type(inner, G_VARIANT_TYPE_STRING)
              ? nostr_publish_federation_from_string(g_variant_get_string(inner, NULL))
              : NOSTR_PUBLISH_FEDERATION_UNSUPPORTED;
    g_variant_unref(inner);
    g_variant_unref(reply);
  } else {
    /* No property (a pre-7d96 daemon answers UnknownProperty /
     * InvalidArgs), no answer, or anything else: not a forwarding relay. */
    g_debug("nostr-publish: SessionRelay1.FederationState: %s", error->message);
    state = error_means_no_owner(error) ? NOSTR_PUBLISH_FEDERATION_NOT_RUNNING
                                        : NOSTR_PUBLISH_FEDERATION_UNSUPPORTED;
    g_error_free(error);
  }
  if (current)
    set_federation(self, state);
}

static void
read_federation(NostrPublishSessionRelay *self)
{
  FederationRead *read = g_new0(FederationRead, 1);
  read->self = self;
  read->generation = ++self->generation;
  g_dbus_connection_call(self->bus, NOSTR_PUBLISH_SESSION_RELAY_BUS_NAME,
                         NOSTR_PUBLISH_SESSION_RELAY_PATH,
                         "org.freedesktop.DBus.Properties", "Get",
                         g_variant_new("(ss)", NOSTR_PUBLISH_SESSION_RELAY_IFACE,
                                       "FederationState"),
                         G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
                         NP_SR_CALL_TIMEOUT_MS, self->cancellable,
                         on_federation_reply, read);
}

static void
on_name_appeared(GDBusConnection *bus, const gchar *name, const gchar *owner,
                 gpointer user_data)
{
  (void)bus; (void)name; (void)owner;
  NostrPublishSessionRelay *self = user_data;
  self->has_owner = TRUE;
  read_federation(self);
}

static void
on_name_vanished(GDBusConnection *bus, const gchar *name, gpointer user_data)
{
  (void)bus; (void)name;
  NostrPublishSessionRelay *self = user_data;
  self->has_owner = FALSE;
  self->generation++;          /* drop any read still in flight */
  set_federation(self, NOSTR_PUBLISH_FEDERATION_NOT_RUNNING);
}

/* ---- Event upstream state ---- */

static void
on_upstream_signal(GDBusConnection *bus, const gchar *sender, const gchar *path,
                   const gchar *iface, const gchar *signal, GVariant *params,
                   gpointer user_data)
{
  (void)bus; (void)sender; (void)path; (void)iface; (void)signal;
  NostrPublishSessionRelay *self = user_data;
  if (self->forward_cb == NULL || !g_variant_is_of_type(params, G_VARIANT_TYPE("(sssss)")))
    return;
  const gchar *event_id, *relay_url, *relay_state, *reason, *event_state;
  g_variant_get(params, "(&s&s&s&s&s)", &event_id, &relay_url, &relay_state, &reason,
                &event_state);
  gboolean event_level = *relay_url == '\0';
  NostrPublishForwardUpdate u = {
    .event_id     = event_id,
    .state        = nostr_publish_forward_state_from_string(event_state),
    .detail       = event_level ? reason : "",
    .relay_url    = event_level ? NULL : relay_url,
    .relay_state  = event_level ? NULL : relay_state,
    .relay_reason = event_level ? NULL : reason,
    .relays       = NULL,
  };
  self->forward_cb(self, &u, self->user_data);
}

typedef struct {
  NostrPublishSessionRelay *self;
  gchar                    *event_id;
} QueryCall;

static void
on_query_reply(GObject *source, GAsyncResult *res, gpointer user_data)
{
  QueryCall *call = user_data;
  GError *error = NULL;
  GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
  if (reply == NULL) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_debug("nostr-publish: GetEventUpstream(%s): %s", call->event_id, error->message);
    g_error_free(error);
    g_free(call->event_id);
    g_free(call);
    return;
  }
  NostrPublishSessionRelay *self = call->self;
  const gchar *state = NULL, *detail = NULL;
  GVariantIter *iter = NULL;
  g_variant_get(reply, "(&s&sa(sssuxx))", &state, &detail, &iter);
  GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);
  const gchar *url, *relay_state, *reason;
  guint32 attempts;
  gint64 updated_at, acked_at;
  while (g_variant_iter_next(iter, "(&s&s&suxx)", &url, &relay_state, &reason, &attempts,
                             &updated_at, &acked_at))
    g_ptr_array_add(lines, *reason != '\0'
                             ? g_strdup_printf("%s %s: %s", url, relay_state, reason)
                             : g_strdup_printf("%s %s", url, relay_state));
  g_ptr_array_add(lines, NULL);
  g_variant_iter_free(iter);

  if (self->forward_cb != NULL) {
    NostrPublishForwardUpdate u = {
      .event_id = call->event_id,
      .state    = nostr_publish_forward_state_from_string(state),
      .detail   = detail,
      .relays   = (const gchar *const *)lines->pdata,
    };
    self->forward_cb(self, &u, self->user_data);
  }
  g_ptr_array_unref(lines);
  g_variant_unref(reply);
  g_free(call->event_id);
  g_free(call);
}

void
nostr_publish_session_relay_query(NostrPublishSessionRelay *self, const gchar *event_id)
{
  g_return_if_fail(self != NULL);
  g_return_if_fail(event_id != NULL);
  QueryCall *call = g_new0(QueryCall, 1);
  call->self = self;
  call->event_id = g_strdup(event_id);
  g_dbus_connection_call(self->bus, NOSTR_PUBLISH_SESSION_RELAY_BUS_NAME,
                         NOSTR_PUBLISH_SESSION_RELAY_PATH,
                         NOSTR_PUBLISH_SESSION_RELAY_IFACE, "GetEventUpstream",
                         g_variant_new("(s)", event_id),
                         G_VARIANT_TYPE("(ssa(sssuxx))"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
                         NP_SR_CALL_TIMEOUT_MS, self->cancellable, on_query_reply, call);
}

/* ---- Lifecycle ---- */

NostrPublishSessionRelay *
nostr_publish_session_relay_new(GDBusConnection           *bus,
                                NostrPublishFederationFunc federation_cb,
                                NostrPublishForwardFunc    forward_cb,
                                gpointer                   user_data)
{
  g_return_val_if_fail(G_IS_DBUS_CONNECTION(bus), NULL);
  NostrPublishSessionRelay *self = g_new0(NostrPublishSessionRelay, 1);
  self->bus = g_object_ref(bus);
  self->federation_cb = federation_cb;
  self->forward_cb = forward_cb;
  self->user_data = user_data;
  self->cancellable = g_cancellable_new();
  /* Subscribe before anything can be published, so no transition is
   * missed between a publisher's OK and its first query. */
  self->signal_id = g_dbus_connection_signal_subscribe(
    bus, NOSTR_PUBLISH_SESSION_RELAY_BUS_NAME, NOSTR_PUBLISH_SESSION_RELAY_IFACE,
    "UpstreamStatusChanged", NOSTR_PUBLISH_SESSION_RELAY_PATH, NULL,
    G_DBUS_SIGNAL_FLAGS_NONE, on_upstream_signal, self, NULL);
  self->watch_id = g_bus_watch_name_on_connection(bus, NOSTR_PUBLISH_SESSION_RELAY_BUS_NAME,
                                                  G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                  on_name_appeared, on_name_vanished,
                                                  self, NULL);
  return self;
}

void
nostr_publish_session_relay_free(NostrPublishSessionRelay *self)
{
  if (self == NULL)
    return;
  g_cancellable_cancel(self->cancellable);
  if (self->signal_id != 0)
    g_dbus_connection_signal_unsubscribe(self->bus, self->signal_id);
  if (self->watch_id != 0)
    g_bus_unwatch_name(self->watch_id);
  g_clear_object(&self->cancellable);
  g_clear_object(&self->bus);
  g_free(self);
}

NostrPublishFederation
nostr_publish_session_relay_get_federation(NostrPublishSessionRelay *self)
{
  g_return_val_if_fail(self != NULL, NOSTR_PUBLISH_FEDERATION_UNKNOWN);
  return self->federation;
}

void
nostr_publish_session_relay_refresh(NostrPublishSessionRelay *self)
{
  g_return_if_fail(self != NULL);
  if (self->has_owner)
    read_federation(self);
}

gboolean
nostr_publish_session_relay_nudge(const gchar *socket_path)
{
  struct sockaddr_un sa;
  if (socket_path == NULL || strlen(socket_path) >= sizeof(sa.sun_path))
    return FALSE;
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return FALSE;
  (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
  memset(&sa, 0, sizeof(sa));
  sa.sun_family = AF_UNIX;
  strcpy(sa.sun_path, socket_path);
  int rc;
  do {
    rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
  } while (rc != 0 && errno == EINTR);
  close(fd);
  return rc == 0;
}
