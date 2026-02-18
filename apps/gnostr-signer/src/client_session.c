/* client_session.c - NIP-46 client session management implementation
 *
 * Implements secure session management for NIP-46 remote signing clients:
 * - Per-client session tracking with activity timestamps
 * - Configurable session timeout (default 15 minutes)
 * - Persistence via libsecret (JSON-serialized session data)
 * - Automatic expiration and cleanup
 *
 * SPDX-License-Identifier: MIT
 */
#include "client_session.h"
#include "settings_manager.h"

#include <string.h>
#include <time.h>
#include <json-glib/json-glib.h>

#ifdef GNOSTR_HAVE_LIBSECRET
#include <libsecret/secret.h>
#endif

/* Default session timeout: 15 minutes (900 seconds) */
#define DEFAULT_SESSION_TIMEOUT_SEC 900

/* GSettings key for client session timeout */
#define GSETTINGS_CLIENT_SESSION_TIMEOUT_KEY "client-session-timeout-sec"

/* Secret store schema for persistent sessions */
#define CLIENT_SESSION_SCHEMA_NAME "org.gnostr.Signer.ClientSessions"
#define CLIENT_SESSION_KEY_ID "client-sessions-data"

/* ============================================================================
 * GnClientSession Implementation
 * ============================================================================ */

struct _GnClientSession {
  GObject parent_instance;

  /* Client identification */
  gchar *client_pubkey;       /* Client's public key (hex) */
  gchar *app_name;            /* Application name (optional) */
  gchar *identity;            /* Associated identity npub */

  /* Session state */
  GnClientSessionState state;
  guint permissions;          /* Bitmask of GnClientSessionPermission */

  /* Timestamps */
  gint64 created_at;          /* When session was created */
  gint64 last_activity;       /* Last activity timestamp */
  gint64 expires_at;          /* When session expires (0 = use timeout) */

  /* Statistics */
  guint request_count;        /* Total requests in this session */

  /* Configuration */
  gboolean persistent;        /* Whether to persist across restarts */
  guint timeout_seconds;      /* Session-specific timeout (0 = use default) */
};

enum {
  SESSION_PROP_0,
  SESSION_PROP_CLIENT_PUBKEY,
  SESSION_PROP_APP_NAME,
  SESSION_PROP_IDENTITY,
  SESSION_PROP_STATE,
  SESSION_PROP_PERMISSIONS,
  SESSION_PROP_CREATED_AT,
  SESSION_PROP_LAST_ACTIVITY,
  SESSION_PROP_EXPIRES_AT,
  SESSION_PROP_REQUEST_COUNT,
  SESSION_PROP_PERSISTENT,
  SESSION_N_PROPS
};

static GParamSpec *session_props[SESSION_N_PROPS] = { NULL };

G_DEFINE_FINAL_TYPE(GnClientSession, gn_client_session, G_TYPE_OBJECT)

static void
gn_client_session_get_property(GObject *object,
                               guint prop_id,
                               GValue *value,
                               GParamSpec *pspec)
{
  GnClientSession *self = GN_CLIENT_SESSION(object);

  switch (prop_id) {
    case SESSION_PROP_CLIENT_PUBKEY:
      g_value_set_string(value, self->client_pubkey);
      break;
    case SESSION_PROP_APP_NAME:
      g_value_set_string(value, self->app_name);
      break;
    case SESSION_PROP_IDENTITY:
      g_value_set_string(value, self->identity);
      break;
    case SESSION_PROP_STATE:
      g_value_set_int(value, self->state);
      break;
    case SESSION_PROP_PERMISSIONS:
      g_value_set_uint(value, self->permissions);
      break;
    case SESSION_PROP_CREATED_AT:
      g_value_set_int64(value, self->created_at);
      break;
    case SESSION_PROP_LAST_ACTIVITY:
      g_value_set_int64(value, self->last_activity);
      break;
    case SESSION_PROP_EXPIRES_AT:
      g_value_set_int64(value, self->expires_at);
      break;
    case SESSION_PROP_REQUEST_COUNT:
      g_value_set_uint(value, self->request_count);
      break;
    case SESSION_PROP_PERSISTENT:
      g_value_set_boolean(value, self->persistent);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void
gn_client_session_finalize(GObject *object)
{
  GnClientSession *self = GN_CLIENT_SESSION(object);

  g_free(self->client_pubkey);
  g_free(self->app_name);
  g_free(self->identity);

  G_OBJECT_CLASS(gn_client_session_parent_class)->finalize(object);
}

static void
gn_client_session_class_init(GnClientSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->get_property = gn_client_session_get_property;
  object_class->finalize = gn_client_session_finalize;

  session_props[SESSION_PROP_CLIENT_PUBKEY] =
    g_param_spec_string("client-pubkey", "Client Public Key",
                        "Client's public key in hex format",
                        NULL,
                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  session_props[SESSION_PROP_APP_NAME] =
    g_param_spec_string("app-name", "Application Name",
                        "Name of the remote application",
                        NULL,
                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  session_props[SESSION_PROP_IDENTITY] =
    g_param_spec_string("identity", "Identity",
                        "Associated identity npub",
                        NULL,
                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  session_props[SESSION_PROP_STATE] =
    g_param_spec_int("state", "State",
                     "Current session state",
                     GN_CLIENT_SESSION_PENDING,
                     GN_CLIENT_SESSION_REVOKED,
                     GN_CLIENT_SESSION_PENDING,
                     G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  session_props[SESSION_PROP_PERMISSIONS] =
    g_param_spec_uint("permissions", "Permissions",
                      "Granted permissions bitmask",
                      0, G_MAXUINT, 0,
                      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  session_props[SESSION_PROP_CREATED_AT] =
    g_param_spec_int64("created-at", "Created At",
                       "Session creation timestamp",
                       0, G_MAXINT64, 0,
                       G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  session_props[SESSION_PROP_LAST_ACTIVITY] =
    g_param_spec_int64("last-activity", "Last Activity",
                       "Last activity timestamp",
                       0, G_MAXINT64, 0,
                       G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  session_props[SESSION_PROP_EXPIRES_AT] =
    g_param_spec_int64("expires-at", "Expires At",
                       "Session expiration timestamp",
                       0, G_MAXINT64, 0,
                       G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  session_props[SESSION_PROP_REQUEST_COUNT] =
    g_param_spec_uint("request-count", "Request Count",
                      "Total requests in session",
                      0, G_MAXUINT, 0,
                      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  session_props[SESSION_PROP_PERSISTENT] =
    g_param_spec_boolean("persistent", "Persistent",
                         "Whether session is persisted",
                         FALSE,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(object_class, SESSION_N_PROPS, session_props);
}

static void
gn_client_session_init(GnClientSession *self)
{
  self->state = GN_CLIENT_SESSION_PENDING;
  self->permissions = GN_PERM_NONE;
  self->created_at = (gint64)time(NULL);
  self->last_activity = self->created_at;
  self->expires_at = 0;
  self->request_count = 0;
  self->persistent = FALSE;
  self->timeout_seconds = 0;
}

/* GnClientSession public API */

const gchar *
gn_client_session_get_client_pubkey(GnClientSession *self)
{
  g_return_val_if_fail(GN_IS_CLIENT_SESSION(self), NULL);
  return self->client_pubkey;
}

const gchar *
gn_client_session_get_app_name(GnClientSession *self)
{
  g_return_val_if_fail(GN_IS_CLIENT_SESSION(self), NULL);
  return self->app_name;
}

const gchar *
gn_client_session_get_identity(GnClientSession *self)
{
  g_return_val_if_fail(GN_IS_CLIENT_SESSION(self), NULL);
  return self->identity;
}

GnClientSessionState
gn_client_session_get_state(GnClientSession *self)
{
  g_return_val_if_fail(GN_IS_CLIENT_SESSION(self), GN_CLIENT_SESSION_REVOKED);
  return self->state;
}

guint
gn_client_session_get_permissions(GnClientSession *self)
{
  g_return_val_if_fail(GN_IS_CLIENT_SESSION(self), GN_PERM_NONE);
  return self->permissions;
}

gboolean
gn_client_session_has_permission(GnClientSession *self,
                                 GnClientSessionPermission perm)
{
  g_return_val_if_fail(GN_IS_CLIENT_SESSION(self), FALSE);
  return (self->permissions & perm) == perm;
}

gint64
gn_client_session_get_created_at(GnClientSession *self)
{
  g_return_val_if_fail(GN_IS_CLIENT_SESSION(self), 0);
  return self->created_at;
}

gint64
gn_client_session_get_last_activity(GnClientSession *self)
{
  g_return_val_if_fail(GN_IS_CLIENT_SESSION(self), 0);
  return self->last_activity;
}

guint
gn_client_session_get_request_count(GnClientSession *self)
{
  g_return_val_if_fail(GN_IS_CLIENT_SESSION(self), 0);
  return self->request_count;
}

gint64
gn_client_session_get_expires_at(GnClientSession *self)
{
  g_return_val_if_fail(GN_IS_CLIENT_SESSION(self), 0);
  return self->expires_at;
}

gboolean
gn_client_session_is_persistent(GnClientSession *self)
{
  g_return_val_if_fail(GN_IS_CLIENT_SESSION(self), FALSE);
  return self->persistent;
}

guint
gn_client_session_get_remaining_time(GnClientSession *self)
{
  g_return_val_if_fail(GN_IS_CLIENT_SESSION(self), 0);

  if (self->state != GN_CLIENT_SESSION_ACTIVE)
    return 0;

  gint64 now = (gint64)time(NULL);

  /* If explicit expiration time is set */
  if (self->expires_at > 0) {
    gint64 remaining = self->expires_at - now;
    return remaining > 0 ? (guint)remaining : 0;
  }

  /* Calculate based on inactivity timeout */
  guint timeout = self->timeout_seconds > 0 ?
                  self->timeout_seconds : DEFAULT_SESSION_TIMEOUT_SEC;

  if (timeout == 0)
    return G_MAXUINT;  /* No timeout */

  gint64 elapsed = now - self->last_activity;
  gint64 remaining = (gint64)timeout - elapsed;

  return remaining > 0 ? (guint)remaining : 0;
}

/* ============================================================================
 * GnClientSessionManager Implementation
 * ============================================================================ */

struct _GnClientSessionManager {
  GObject parent_instance;

  /* Sessions indexed by "client_pubkey:identity" */
  GHashTable *sessions;

  /* Configuration */
  guint default_timeout_seconds;

  /* Settings */
  GSettings *settings;

  /* Timer for checking expirations */
  guint expiration_timer_id;
};

enum {
  MANAGER_SIGNAL_SESSION_CREATED,
  MANAGER_SIGNAL_SESSION_EXPIRED,
  MANAGER_SIGNAL_SESSION_REVOKED,
  MANAGER_SIGNAL_SESSION_ACTIVITY,
  MANAGER_N_SIGNALS
};

static guint manager_signals[MANAGER_N_SIGNALS] = { 0 };

enum {
  MANAGER_PROP_0,
  MANAGER_PROP_TIMEOUT,
  MANAGER_PROP_SESSION_COUNT,
  MANAGER_PROP_ACTIVE_COUNT,
  MANAGER_N_PROPS
};

static GParamSpec *manager_props[MANAGER_N_PROPS] = { NULL };

G_DEFINE_FINAL_TYPE(GnClientSessionManager, gn_client_session_manager, G_TYPE_OBJECT)

/* Singleton instance */
static GnClientSessionManager *default_manager = NULL;

/* Forward declarations */
static gboolean check_session_expirations(gpointer user_data);
static gchar *make_session_key(const gchar *client_pubkey, const gchar *identity);

static void
gn_client_session_manager_get_property(GObject *object,
                                       guint prop_id,
                                       GValue *value,
                                       GParamSpec *pspec)
{
  GnClientSessionManager *self = GN_CLIENT_SESSION_MANAGER(object);

  switch (prop_id) {
    case MANAGER_PROP_TIMEOUT:
      g_value_set_uint(value, self->default_timeout_seconds);
      break;
    case MANAGER_PROP_SESSION_COUNT:
      g_value_set_uint(value, g_hash_table_size(self->sessions));
      break;
    case MANAGER_PROP_ACTIVE_COUNT:
      g_value_set_uint(value, gn_client_session_manager_get_active_count(self));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void
gn_client_session_manager_set_property(GObject *object,
                                       guint prop_id,
                                       const GValue *value,
                                       GParamSpec *pspec)
{
  GnClientSessionManager *self = GN_CLIENT_SESSION_MANAGER(object);

  switch (prop_id) {
    case MANAGER_PROP_TIMEOUT:
      gn_client_session_manager_set_timeout(self, g_value_get_uint(value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void
gn_client_session_manager_finalize(GObject *object)
{
  GnClientSessionManager *self = GN_CLIENT_SESSION_MANAGER(object);

  if (self->expiration_timer_id != 0) {
    g_source_remove(self->expiration_timer_id);
    self->expiration_timer_id = 0;
  }

  /* Save persistent sessions before shutdown */
  gn_client_session_manager_save_persistent(self);

  g_clear_pointer(&self->sessions, g_hash_table_destroy);
  g_clear_object(&self->settings);

  if (default_manager == self) {
    default_manager = NULL;
  }

  G_OBJECT_CLASS(gn_client_session_manager_parent_class)->finalize(object);
}

static void
gn_client_session_manager_class_init(GnClientSessionManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->get_property = gn_client_session_manager_get_property;
  object_class->set_property = gn_client_session_manager_set_property;
  object_class->finalize = gn_client_session_manager_finalize;

  manager_props[MANAGER_PROP_TIMEOUT] =
    g_param_spec_uint("timeout", "Timeout",
                      "Default session timeout in seconds",
                      0, G_MAXUINT, DEFAULT_SESSION_TIMEOUT_SEC,
                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  manager_props[MANAGER_PROP_SESSION_COUNT] =
    g_param_spec_uint("session-count", "Session Count",
                      "Total number of sessions",
                      0, G_MAXUINT, 0,
                      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  manager_props[MANAGER_PROP_ACTIVE_COUNT] =
    g_param_spec_uint("active-count", "Active Count",
                      "Number of active sessions",
                      0, G_MAXUINT, 0,
                      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(object_class, MANAGER_N_PROPS, manager_props);

  /**
   * GnClientSessionManager::session-created:
   * @manager: The session manager
   * @session: The new session
   *
   * Emitted when a new client session is created.
   */
  manager_signals[MANAGER_SIGNAL_SESSION_CREATED] =
    g_signal_new("session-created",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1,
                 GN_TYPE_CLIENT_SESSION);

  /**
   * GnClientSessionManager::session-expired:
   * @manager: The session manager
   * @session: The expired session
   *
   * Emitted when a client session expires due to timeout.
   */
  manager_signals[MANAGER_SIGNAL_SESSION_EXPIRED] =
    g_signal_new("session-expired",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1,
                 GN_TYPE_CLIENT_SESSION);

  /**
   * GnClientSessionManager::session-revoked:
   * @manager: The session manager
   * @session: The revoked session
   *
   * Emitted when a client session is manually revoked.
   */
  manager_signals[MANAGER_SIGNAL_SESSION_REVOKED] =
    g_signal_new("session-revoked",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1,
                 GN_TYPE_CLIENT_SESSION);

  /**
   * GnClientSessionManager::session-activity:
   * @manager: The session manager
   * @session: The session with activity
   *
   * Emitted when session activity is recorded.
   */
  manager_signals[MANAGER_SIGNAL_SESSION_ACTIVITY] =
    g_signal_new("session-activity",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1,
                 GN_TYPE_CLIENT_SESSION);
}

static void
gn_client_session_manager_init(GnClientSessionManager *self)
{
  self->sessions = g_hash_table_new_full(g_str_hash, g_str_equal,
                                         g_free, g_object_unref);
  self->default_timeout_seconds = DEFAULT_SESSION_TIMEOUT_SEC;
  self->expiration_timer_id = 0;

  /* Load settings */
  self->settings = g_settings_new("org.gnostr.Signer");
  if (self->settings) {
    GVariant *timeout_var = g_settings_get_value(self->settings,
                                                  "client-session-timeout-sec");
    if (timeout_var) {
      self->default_timeout_seconds = (guint)g_variant_get_int32(timeout_var);
      g_variant_unref(timeout_var);
    }
  }

  /* Load persistent sessions */
  gn_client_session_manager_load_persistent(self);

  /* Start expiration timer (checks every 30 seconds) */
  self->expiration_timer_id = g_timeout_add_seconds(30,
                                                    check_session_expirations,
                                                    self);
}

/* Utility: Create session hash key */
static gchar *
make_session_key(const gchar *client_pubkey, const gchar *identity)
{
  if (identity && *identity) {
    return g_strdup_printf("%s:%s", client_pubkey, identity);
  }
  return g_strdup(client_pubkey);
}

/* Timer callback: Check for expired sessions */
static gboolean
check_session_expirations(gpointer user_data)
{
  GnClientSessionManager *self = GN_CLIENT_SESSION_MANAGER(user_data);

  GHashTableIter iter;
  gpointer key, value;
  GPtrArray *expired = g_ptr_array_new();

  g_hash_table_iter_init(&iter, self->sessions);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    GnClientSession *session = GN_CLIENT_SESSION(value);

    if (session->state != GN_CLIENT_SESSION_ACTIVE)
      continue;

    /* Check if session has expired */
    if (gn_client_session_get_remaining_time(session) == 0) {
      session->state = GN_CLIENT_SESSION_EXPIRED;
      g_ptr_array_add(expired, session);
    }
  }

  /* Emit signals for expired sessions */
  for (guint i = 0; i < expired->len; i++) {
    GnClientSession *session = g_ptr_array_index(expired, i);
    g_debug("client-session: Session expired for %s (%s)",
            session->client_pubkey,
            session->app_name ? session->app_name : "unknown");

    g_signal_emit(self, manager_signals[MANAGER_SIGNAL_SESSION_EXPIRED], 0, session);
    g_object_notify_by_pspec(G_OBJECT(session), session_props[SESSION_PROP_STATE]);
  }

  g_ptr_array_free(expired, TRUE);

  /* Update active count property */
  g_object_notify_by_pspec(G_OBJECT(self), manager_props[MANAGER_PROP_ACTIVE_COUNT]);

  return G_SOURCE_CONTINUE;
}

/* Public API */

GnClientSessionManager *
gn_client_session_manager_new(void)
{
  return g_object_new(GN_TYPE_CLIENT_SESSION_MANAGER, NULL);
}

GnClientSessionManager *
gn_client_session_manager_get_default(void)
{
  if (!default_manager) {
    default_manager = gn_client_session_manager_new();
  }
  return default_manager;
}

guint
gn_client_session_manager_get_timeout(GnClientSessionManager *self)
{
  g_return_val_if_fail(GN_IS_CLIENT_SESSION_MANAGER(self), DEFAULT_SESSION_TIMEOUT_SEC);
  return self->default_timeout_seconds;
}

void
gn_client_session_manager_set_timeout(GnClientSessionManager *self,
                                      guint seconds)
{
  g_return_if_fail(GN_IS_CLIENT_SESSION_MANAGER(self));

  if (self->default_timeout_seconds == seconds)
    return;

  self->default_timeout_seconds = seconds;

  /* Persist to settings */
  if (self->settings) {
    g_settings_set_int(self->settings, "client-session-timeout-sec", (gint)seconds);
  }

  g_object_notify_by_pspec(G_OBJECT(self), manager_props[MANAGER_PROP_TIMEOUT]);
}

GnClientSession *
gn_client_session_manager_create_session(GnClientSessionManager *self,
                                         const gchar *client_pubkey,
                                         const gchar *identity,
                                         const gchar *app_name,
                                         guint permissions,
                                         gboolean persistent,
                                         gint64 ttl_seconds)
{
  g_return_val_if_fail(GN_IS_CLIENT_SESSION_MANAGER(self), NULL);
  g_return_val_if_fail(client_pubkey && *client_pubkey, NULL);
  g_return_val_if_fail(identity && *identity, NULL);

  gchar *key = make_session_key(client_pubkey, identity);

  /* Check for existing session */
  GnClientSession *existing = g_hash_table_lookup(self->sessions, key);
  if (existing && existing->state == GN_CLIENT_SESSION_ACTIVE) {
    g_debug("client-session: Reactivating existing session for %s", client_pubkey);
    existing->last_activity = (gint64)time(NULL);
    g_free(key);
    return existing;
  }

  /* Create new session */
  GnClientSession *session = g_object_new(GN_TYPE_CLIENT_SESSION, NULL);
  session->client_pubkey = g_strdup(client_pubkey);
  session->identity = g_strdup(identity);
  session->app_name = g_strdup(app_name);
  session->permissions = permissions;
  session->persistent = persistent;
  session->state = GN_CLIENT_SESSION_ACTIVE;
  session->timeout_seconds = self->default_timeout_seconds;

  /* Set expiration if explicit TTL provided */
  if (ttl_seconds > 0) {
    session->expires_at = session->created_at + ttl_seconds;
  } else if (ttl_seconds == -1) {
    /* Never expires (persistent session) */
    session->expires_at = 0;
    session->timeout_seconds = 0;
  }

  g_hash_table_replace(self->sessions, key, session);

  g_debug("client-session: Created session for %s (%s) with permissions 0x%x",
          client_pubkey,
          app_name ? app_name : "unknown",
          permissions);

  g_signal_emit(self, manager_signals[MANAGER_SIGNAL_SESSION_CREATED], 0, session);
  g_object_notify_by_pspec(G_OBJECT(self), manager_props[MANAGER_PROP_SESSION_COUNT]);
  g_object_notify_by_pspec(G_OBJECT(self), manager_props[MANAGER_PROP_ACTIVE_COUNT]);

  /* Save if persistent */
  if (persistent) {
    gn_client_session_manager_save_persistent(self);
  }

  return session;
}

GnClientSession *
gn_client_session_manager_get_session(GnClientSessionManager *self,
                                      const gchar *client_pubkey,
                                      const gchar *identity)
{
  g_return_val_if_fail(GN_IS_CLIENT_SESSION_MANAGER(self), NULL);
  g_return_val_if_fail(client_pubkey && *client_pubkey, NULL);

  gchar *key = make_session_key(client_pubkey, identity);
  GnClientSession *session = g_hash_table_lookup(self->sessions, key);
  g_free(key);

  return session;
}

gboolean
gn_client_session_manager_has_active_session(GnClientSessionManager *self,
                                             const gchar *client_pubkey,
                                             const gchar *identity)
{
  GnClientSession *session = gn_client_session_manager_get_session(self,
                                                                   client_pubkey,
                                                                   identity);
  if (!session)
    return FALSE;

  /* Check if active and not expired */
  if (session->state != GN_CLIENT_SESSION_ACTIVE)
    return FALSE;

  return gn_client_session_get_remaining_time(session) > 0;
}

gboolean
gn_client_session_manager_touch_session(GnClientSessionManager *self,
                                        const gchar *client_pubkey,
                                        const gchar *identity)
{
  g_return_val_if_fail(GN_IS_CLIENT_SESSION_MANAGER(self), FALSE);
  g_return_val_if_fail(client_pubkey && *client_pubkey, FALSE);

  GnClientSession *session = gn_client_session_manager_get_session(self,
                                                                   client_pubkey,
                                                                   identity);
  if (!session || session->state != GN_CLIENT_SESSION_ACTIVE)
    return FALSE;

  session->last_activity = (gint64)time(NULL);
  session->request_count++;

  g_object_notify_by_pspec(G_OBJECT(session), session_props[SESSION_PROP_LAST_ACTIVITY]);
  g_object_notify_by_pspec(G_OBJECT(session), session_props[SESSION_PROP_REQUEST_COUNT]);

  g_signal_emit(self, manager_signals[MANAGER_SIGNAL_SESSION_ACTIVITY], 0, session);

  return TRUE;
}

gboolean
gn_client_session_manager_revoke_session(GnClientSessionManager *self,
                                         const gchar *client_pubkey,
                                         const gchar *identity)
{
  g_return_val_if_fail(GN_IS_CLIENT_SESSION_MANAGER(self), FALSE);
  g_return_val_if_fail(client_pubkey && *client_pubkey, FALSE);

  GnClientSession *session = gn_client_session_manager_get_session(self,
                                                                   client_pubkey,
                                                                   identity);
  if (!session)
    return FALSE;

  if (session->state == GN_CLIENT_SESSION_REVOKED)
    return TRUE;  /* Already revoked */

  session->state = GN_CLIENT_SESSION_REVOKED;

  g_debug("client-session: Revoked session for %s (%s)",
          client_pubkey,
          session->app_name ? session->app_name : "unknown");

  g_signal_emit(self, manager_signals[MANAGER_SIGNAL_SESSION_REVOKED], 0, session);
  g_object_notify_by_pspec(G_OBJECT(session), session_props[SESSION_PROP_STATE]);
  g_object_notify_by_pspec(G_OBJECT(self), manager_props[MANAGER_PROP_ACTIVE_COUNT]);

  /* Update persistence */
  if (session->persistent) {
    gn_client_session_manager_save_persistent(self);
  }

  return TRUE;
}

guint
gn_client_session_manager_revoke_all_for_client(GnClientSessionManager *self,
                                                const gchar *client_pubkey)
{
  g_return_val_if_fail(GN_IS_CLIENT_SESSION_MANAGER(self), 0);
  g_return_val_if_fail(client_pubkey && *client_pubkey, 0);

  guint count = 0;
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init(&iter, self->sessions);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    GnClientSession *session = GN_CLIENT_SESSION(value);

    if (g_strcmp0(session->client_pubkey, client_pubkey) == 0 &&
        session->state == GN_CLIENT_SESSION_ACTIVE) {
      session->state = GN_CLIENT_SESSION_REVOKED;
      g_signal_emit(self, manager_signals[MANAGER_SIGNAL_SESSION_REVOKED], 0, session);
      g_object_notify_by_pspec(G_OBJECT(session), session_props[SESSION_PROP_STATE]);
      count++;
    }
  }

  if (count > 0) {
    g_object_notify_by_pspec(G_OBJECT(self), manager_props[MANAGER_PROP_ACTIVE_COUNT]);
    gn_client_session_manager_save_persistent(self);
  }

  return count;
}

guint
gn_client_session_manager_revoke_all(GnClientSessionManager *self)
{
  g_return_val_if_fail(GN_IS_CLIENT_SESSION_MANAGER(self), 0);

  guint count = 0;
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init(&iter, self->sessions);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    GnClientSession *session = GN_CLIENT_SESSION(value);

    if (session->state == GN_CLIENT_SESSION_ACTIVE) {
      session->state = GN_CLIENT_SESSION_REVOKED;
      g_signal_emit(self, manager_signals[MANAGER_SIGNAL_SESSION_REVOKED], 0, session);
      g_object_notify_by_pspec(G_OBJECT(session), session_props[SESSION_PROP_STATE]);
      count++;
    }
  }

  if (count > 0) {
    g_object_notify_by_pspec(G_OBJECT(self), manager_props[MANAGER_PROP_ACTIVE_COUNT]);
    gn_client_session_manager_save_persistent(self);
  }

  return count;
}

GPtrArray *
gn_client_session_manager_list_sessions(GnClientSessionManager *self)
{
  g_return_val_if_fail(GN_IS_CLIENT_SESSION_MANAGER(self), NULL);

  GPtrArray *arr = g_ptr_array_new_with_free_func(g_object_unref);

  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init(&iter, self->sessions);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    g_ptr_array_add(arr, g_object_ref(value));
  }

  return arr;
}

GPtrArray *
gn_client_session_manager_list_active_sessions(GnClientSessionManager *self)
{
  g_return_val_if_fail(GN_IS_CLIENT_SESSION_MANAGER(self), NULL);

  GPtrArray *arr = g_ptr_array_new_with_free_func(g_object_unref);

  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init(&iter, self->sessions);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    GnClientSession *session = GN_CLIENT_SESSION(value);
    if (session->state == GN_CLIENT_SESSION_ACTIVE &&
        gn_client_session_get_remaining_time(session) > 0) {
      g_ptr_array_add(arr, g_object_ref(session));
    }
  }

  return arr;
}

guint
gn_client_session_manager_cleanup_expired(GnClientSessionManager *self)
{
  g_return_val_if_fail(GN_IS_CLIENT_SESSION_MANAGER(self), 0);

  GPtrArray *to_remove = g_ptr_array_new();
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init(&iter, self->sessions);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    GnClientSession *session = GN_CLIENT_SESSION(value);

    /* Remove expired/revoked non-persistent sessions */
    if ((session->state == GN_CLIENT_SESSION_EXPIRED ||
         session->state == GN_CLIENT_SESSION_REVOKED) &&
        !session->persistent) {
      g_ptr_array_add(to_remove, g_strdup(key));
    }
  }

  for (guint i = 0; i < to_remove->len; i++) {
    g_hash_table_remove(self->sessions, g_ptr_array_index(to_remove, i));
  }

  guint count = to_remove->len;
  g_ptr_array_free(to_remove, TRUE);

  if (count > 0) {
    g_object_notify_by_pspec(G_OBJECT(self), manager_props[MANAGER_PROP_SESSION_COUNT]);
  }

  return count;
}

guint
gn_client_session_manager_get_session_count(GnClientSessionManager *self)
{
  g_return_val_if_fail(GN_IS_CLIENT_SESSION_MANAGER(self), 0);
  return g_hash_table_size(self->sessions);
}

guint
gn_client_session_manager_get_active_count(GnClientSessionManager *self)
{
  g_return_val_if_fail(GN_IS_CLIENT_SESSION_MANAGER(self), 0);

  guint count = 0;
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init(&iter, self->sessions);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    GnClientSession *session = GN_CLIENT_SESSION(value);
    if (session->state == GN_CLIENT_SESSION_ACTIVE &&
        gn_client_session_get_remaining_time(session) > 0) {
      count++;
    }
  }

  return count;
}

/* ============================================================================
 * Persistence via libsecret
 * ============================================================================ */

#ifdef GNOSTR_HAVE_LIBSECRET

static const SecretSchema CLIENT_SESSION_SCHEMA = {
  CLIENT_SESSION_SCHEMA_NAME,
  SECRET_SCHEMA_NONE,
  {
    { "key_id", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { NULL, 0 }
  }
};

/* Serialize sessions to JSON */
static gchar *
serialize_sessions_to_json(GnClientSessionManager *self)
{
  JsonBuilder *builder = json_builder_new();

  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "version");
  json_builder_add_int_value(builder, 1);

  json_builder_set_member_name(builder, "sessions");
  json_builder_begin_array(builder);

  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init(&iter, self->sessions);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    GnClientSession *session = GN_CLIENT_SESSION(value);

    /* Only persist active, persistent sessions */
    if (!session->persistent)
      continue;
    if (session->state != GN_CLIENT_SESSION_ACTIVE)
      continue;

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "client_pubkey");
    json_builder_add_string_value(builder, session->client_pubkey);

    json_builder_set_member_name(builder, "identity");
    json_builder_add_string_value(builder, session->identity);

    if (session->app_name) {
      json_builder_set_member_name(builder, "app_name");
      json_builder_add_string_value(builder, session->app_name);
    }

    json_builder_set_member_name(builder, "permissions");
    json_builder_add_int_value(builder, session->permissions);

    json_builder_set_member_name(builder, "created_at");
    json_builder_add_int_value(builder, session->created_at);

    json_builder_set_member_name(builder, "expires_at");
    json_builder_add_int_value(builder, session->expires_at);

    json_builder_set_member_name(builder, "timeout_seconds");
    json_builder_add_int_value(builder, session->timeout_seconds);

    json_builder_end_object(builder);
  }

  json_builder_end_array(builder);
  json_builder_end_object(builder);

  JsonNode *root = json_builder_get_root(builder);
  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, root);

  gchar *json_str = json_generator_to_data(gen, NULL);

  json_node_free(root);
  g_object_unref(gen);
  g_object_unref(builder);

  return json_str;
}

/* Deserialize sessions from JSON */
static void
deserialize_sessions_from_json(GnClientSessionManager *self, const gchar *json_str)
{
  if (!json_str || !*json_str)
    return;

  JsonParser *parser = json_parser_new();
  GError *err = NULL;

  if (!json_parser_load_from_data(parser, json_str, -1, &err)) {
    g_warning("client-session: Failed to parse session JSON: %s", err->message);
    g_clear_error(&err);
    g_object_unref(parser);
    return;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root)) {
    g_object_unref(parser);
    return;
  }

  JsonObject *obj = json_node_get_object(root);

  /* Check version */
  gint64 version = json_object_get_int_member(obj, "version");
  if (version != 1) {
    g_warning("client-session: Unknown session data version: %ld", (long)version);
    g_object_unref(parser);
    return;
  }

  JsonArray *sessions = json_object_get_array_member(obj, "sessions");
  if (!sessions) {
    g_object_unref(parser);
    return;
  }

  gint64 now = (gint64)time(NULL);
  guint count = json_array_get_length(sessions);

  for (guint i = 0; i < count; i++) {
    JsonObject *sess_obj = json_array_get_object_element(sessions, i);
    if (!sess_obj)
      continue;

    const gchar *client_pubkey = json_object_get_string_member(sess_obj, "client_pubkey");
    const gchar *identity = json_object_get_string_member(sess_obj, "identity");
    const gchar *app_name = NULL;

    if (json_object_has_member(sess_obj, "app_name")) {
      app_name = json_object_get_string_member(sess_obj, "app_name");
    }

    guint permissions = (guint)json_object_get_int_member(sess_obj, "permissions");
    gint64 created_at = json_object_get_int_member(sess_obj, "created_at");
    gint64 expires_at = json_object_get_int_member(sess_obj, "expires_at");
    guint timeout_seconds = (guint)json_object_get_int_member(sess_obj, "timeout_seconds");

    /* Skip if already expired based on explicit expiration */
    if (expires_at > 0 && expires_at <= now) {
      g_debug("client-session: Skipping expired persistent session for %s", client_pubkey);
      continue;
    }

    /* Create session */
    GnClientSession *session = g_object_new(GN_TYPE_CLIENT_SESSION, NULL);
    session->client_pubkey = g_strdup(client_pubkey);
    session->identity = g_strdup(identity);
    session->app_name = g_strdup(app_name);
    session->permissions = permissions;
    session->created_at = created_at;
    session->last_activity = now;  /* Reset activity on load */
    session->expires_at = expires_at;
    session->timeout_seconds = timeout_seconds;
    session->persistent = TRUE;
    session->state = GN_CLIENT_SESSION_ACTIVE;

    gchar *key = make_session_key(client_pubkey, identity);
    g_hash_table_replace(self->sessions, key, session);

    g_debug("client-session: Loaded persistent session for %s (%s)",
            client_pubkey, app_name ? app_name : "unknown");
  }

  g_object_unref(parser);
}

#endif /* GNOSTR_HAVE_LIBSECRET */

guint
gn_client_session_manager_load_persistent(GnClientSessionManager *self)
{
  g_return_val_if_fail(GN_IS_CLIENT_SESSION_MANAGER(self), 0);

#ifdef GNOSTR_HAVE_LIBSECRET
  GError *err = NULL;

  gchar *json_str = secret_password_lookup_sync(
    &CLIENT_SESSION_SCHEMA,
    NULL,
    &err,
    "key_id", CLIENT_SESSION_KEY_ID,
    NULL
  );

  if (err) {
    g_debug("client-session: No persistent sessions: %s", err->message);
    g_clear_error(&err);
    return 0;
  }

  if (!json_str || !*json_str) {
    g_free(json_str);
    return 0;
  }

  guint prev_count = g_hash_table_size(self->sessions);
  deserialize_sessions_from_json(self, json_str);
  guint loaded = g_hash_table_size(self->sessions) - prev_count;

  g_free(json_str);

  g_debug("client-session: Loaded %u persistent sessions", loaded);
  return loaded;
#else
  return 0;
#endif
}

gboolean
gn_client_session_manager_save_persistent(GnClientSessionManager *self)
{
  g_return_val_if_fail(GN_IS_CLIENT_SESSION_MANAGER(self), FALSE);

#ifdef GNOSTR_HAVE_LIBSECRET
  gchar *json_str = serialize_sessions_to_json(self);
  GError *err = NULL;

  gboolean ok = secret_password_store_sync(
    &CLIENT_SESSION_SCHEMA,
    SECRET_COLLECTION_DEFAULT,
    "GNostr Signer Client Sessions",
    json_str,
    NULL,
    &err,
    "key_id", CLIENT_SESSION_KEY_ID,
    NULL
  );

  g_free(json_str);

  if (err) {
    g_warning("client-session: Failed to save sessions: %s", err->message);
    g_clear_error(&err);
    return FALSE;
  }

  return ok;
#else
  return TRUE;
#endif
}
