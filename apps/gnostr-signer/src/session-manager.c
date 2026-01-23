/* session-manager.c - Session management implementation
 *
 * Implements secure session management with:
 * - Password hashing using GChecksum (SHA-256)
 * - Secure credential storage via secret store
 * - GLib timeout-based auto-lock
 * - GSettings integration for persistence
 * - Rate limiting to prevent brute force attacks (nostrc-1g1)
 * - Secure memory handling for sensitive data (nostrc-ycd)
 */
#include "session-manager.h"
#include "settings_manager.h"
#include "rate-limiter.h"
#include "secure-mem.h"

#include <string.h>

#ifdef GNOSTR_HAVE_LIBSECRET
#include <libsecret/secret.h>
#endif

/* GSettings key for session timeout */
#define GSETTINGS_SESSION_TIMEOUT_KEY "session-timeout-sec"
#define GSETTINGS_SESSION_LOCK_ON_IDLE_KEY "session-lock-on-idle"

/* Default timeout: 5 minutes */
#define DEFAULT_SESSION_TIMEOUT_SEC 300

/* Secret store schema for session password */
#define SESSION_PASSWORD_SCHEMA_NAME "org.gnostr.Signer.Session"
#define SESSION_PASSWORD_KEY_ID "session-master-password"

/* GObject private structure */
struct _GnSessionManager {
  GObject parent_instance;

  /* Session state */
  GnSessionState state;
  gint64 last_activity;        /* Timestamp of last activity */
  gint64 session_started;      /* Timestamp when session was authenticated */

  /* Configuration */
  guint timeout_seconds;       /* Auto-lock timeout (0 = disabled) */
  gboolean lock_on_idle;       /* Lock when system goes idle */

  /* Timer */
  guint timeout_source_id;     /* GSource ID for auto-lock timer */

  /* Settings */
  GSettings *settings;

  /* Cached password hash for session verification */
  gchar *password_hash;        /* SHA-256 hash of password */
  gboolean password_configured; /* Whether a password is set */
};

/* Signals */
enum {
  SIGNAL_SESSION_LOCKED,
  SIGNAL_SESSION_UNLOCKED,
  SIGNAL_SESSION_EXPIRED,
  SIGNAL_TIMEOUT_WARNING,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

/* Properties */
enum {
  PROP_0,
  PROP_STATE,
  PROP_TIMEOUT,
  PROP_REMAINING_TIME,
  N_PROPS
};

static GParamSpec *properties[N_PROPS] = { NULL };

G_DEFINE_FINAL_TYPE(GnSessionManager, gn_session_manager, G_TYPE_OBJECT)

/* Singleton instance */
static GnSessionManager *default_instance = NULL;

/* Forward declarations */
static void gn_session_manager_start_timer(GnSessionManager *self);
static void gn_session_manager_stop_timer(GnSessionManager *self);
static gboolean on_timeout_expired(gpointer user_data);

/* Utility: Compute SHA-256 hash of password */
static gchar *
compute_password_hash(const gchar *password)
{
  if (!password || !*password)
    return NULL;

  GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
  g_checksum_update(checksum, (const guchar *)password, strlen(password));

  const gchar *hash = g_checksum_get_string(checksum);
  gchar *result = g_strdup(hash);

  g_checksum_free(checksum);
  return result;
}

/* Utility: Secure memory wipe using secure-mem.h API
 * This uses explicit_bzero/sodium_memzero to ensure the compiler
 * doesn't optimize away the zeroing operation.
 */
static void
secure_wipe(gchar *str)
{
  if (!str)
    return;

  gnostr_secure_clear(str, strlen(str));
}

/* Load password hash from secret store */
static gboolean
load_password_from_store(GnSessionManager *self)
{
#ifdef GNOSTR_HAVE_LIBSECRET
  GError *err = NULL;

  /* Define schema inline for session password */
  static const SecretSchema SESSION_SCHEMA = {
    SESSION_PASSWORD_SCHEMA_NAME,
    SECRET_SCHEMA_NONE,
    {
      { "key_id", SECRET_SCHEMA_ATTRIBUTE_STRING },
      { NULL, 0 }
    }
  };

  gchar *stored_hash = secret_password_lookup_sync(
    &SESSION_SCHEMA,
    NULL,
    &err,
    "key_id", SESSION_PASSWORD_KEY_ID,
    NULL
  );

  if (err) {
    g_debug("session-manager: No stored password hash: %s", err->message);
    g_clear_error(&err);
    return FALSE;
  }

  if (stored_hash && *stored_hash) {
    g_free(self->password_hash);
    self->password_hash = stored_hash;
    self->password_configured = TRUE;
    return TRUE;
  }

  g_free(stored_hash);
#endif

  return FALSE;
}

/* Save password hash to secret store */
static gboolean
save_password_to_store(GnSessionManager *self, const gchar *hash)
{
#ifdef GNOSTR_HAVE_LIBSECRET
  GError *err = NULL;

  static const SecretSchema SESSION_SCHEMA = {
    SESSION_PASSWORD_SCHEMA_NAME,
    SECRET_SCHEMA_NONE,
    {
      { "key_id", SECRET_SCHEMA_ATTRIBUTE_STRING },
      { NULL, 0 }
    }
  };

  gboolean ok = secret_password_store_sync(
    &SESSION_SCHEMA,
    SECRET_COLLECTION_DEFAULT,
    "GNostr Signer Session Password",
    hash,
    NULL,
    &err,
    "key_id", SESSION_PASSWORD_KEY_ID,
    NULL
  );

  if (err) {
    g_warning("session-manager: Failed to store password: %s", err->message);
    g_clear_error(&err);
    return FALSE;
  }

  return ok;
#else
  /* No secret store available - store hash in memory only */
  (void)self;
  (void)hash;
  return TRUE;
#endif
}

/* Clear password from secret store */
static gboolean
clear_password_from_store(void)
{
#ifdef GNOSTR_HAVE_LIBSECRET
  GError *err = NULL;

  static const SecretSchema SESSION_SCHEMA = {
    SESSION_PASSWORD_SCHEMA_NAME,
    SECRET_SCHEMA_NONE,
    {
      { "key_id", SECRET_SCHEMA_ATTRIBUTE_STRING },
      { NULL, 0 }
    }
  };

  gboolean ok = secret_password_clear_sync(
    &SESSION_SCHEMA,
    NULL,
    &err,
    "key_id", SESSION_PASSWORD_KEY_ID,
    NULL
  );

  if (err) {
    g_debug("session-manager: Clear password: %s", err->message);
    g_clear_error(&err);
    return FALSE;
  }

  return ok;
#else
  return TRUE;
#endif
}

/* GObject property getters */
static void
gn_session_manager_get_property(GObject *object,
                                guint prop_id,
                                GValue *value,
                                GParamSpec *pspec)
{
  GnSessionManager *self = GN_SESSION_MANAGER(object);

  switch (prop_id) {
    case PROP_STATE:
      g_value_set_int(value, self->state);
      break;
    case PROP_TIMEOUT:
      g_value_set_uint(value, self->timeout_seconds);
      break;
    case PROP_REMAINING_TIME:
      g_value_set_uint(value, gn_session_manager_get_remaining_time(self));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

/* GObject property setters */
static void
gn_session_manager_set_property(GObject *object,
                                guint prop_id,
                                const GValue *value,
                                GParamSpec *pspec)
{
  GnSessionManager *self = GN_SESSION_MANAGER(object);

  switch (prop_id) {
    case PROP_TIMEOUT:
      gn_session_manager_set_timeout(self, g_value_get_uint(value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

/* GObject finalize */
static void
gn_session_manager_finalize(GObject *object)
{
  GnSessionManager *self = GN_SESSION_MANAGER(object);

  gn_session_manager_stop_timer(self);

  if (self->password_hash) {
    secure_wipe(self->password_hash);
    g_free(self->password_hash);
  }

  g_clear_object(&self->settings);

  if (default_instance == self) {
    default_instance = NULL;
  }

  G_OBJECT_CLASS(gn_session_manager_parent_class)->finalize(object);
}

/* GObject class init */
static void
gn_session_manager_class_init(GnSessionManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->get_property = gn_session_manager_get_property;
  object_class->set_property = gn_session_manager_set_property;
  object_class->finalize = gn_session_manager_finalize;

  /**
   * GnSessionManager:state:
   *
   * The current session state.
   */
  properties[PROP_STATE] =
    g_param_spec_int("state",
                     "State",
                     "Current session state",
                     GN_SESSION_STATE_LOCKED,
                     GN_SESSION_STATE_EXPIRED,
                     GN_SESSION_STATE_LOCKED,
                     G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * GnSessionManager:timeout:
   *
   * The auto-lock timeout in seconds. 0 means disabled.
   */
  properties[PROP_TIMEOUT] =
    g_param_spec_uint("timeout",
                      "Timeout",
                      "Auto-lock timeout in seconds",
                      0, G_MAXUINT, DEFAULT_SESSION_TIMEOUT_SEC,
                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  /**
   * GnSessionManager:remaining-time:
   *
   * The remaining time before auto-lock in seconds.
   */
  properties[PROP_REMAINING_TIME] =
    g_param_spec_uint("remaining-time",
                      "Remaining Time",
                      "Seconds until auto-lock",
                      0, G_MAXUINT, 0,
                      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(object_class, N_PROPS, properties);

  /**
   * GnSessionManager::session-locked:
   * @self: The session manager
   * @reason: The reason for locking (GnLockReason)
   *
   * Emitted when the session is locked, either manually or due to timeout.
   */
  signals[SIGNAL_SESSION_LOCKED] =
    g_signal_new("session-locked",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE, 1,
                 G_TYPE_INT);

  /**
   * GnSessionManager::session-unlocked:
   * @self: The session manager
   *
   * Emitted when the session is successfully authenticated.
   */
  signals[SIGNAL_SESSION_UNLOCKED] =
    g_signal_new("session-unlocked",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE, 0);

  /**
   * GnSessionManager::session-expired:
   * @self: The session manager
   *
   * Emitted when the session expires due to inactivity timeout.
   */
  signals[SIGNAL_SESSION_EXPIRED] =
    g_signal_new("session-expired",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE, 0);

  /**
   * GnSessionManager::timeout-warning:
   * @self: The session manager
   * @seconds_remaining: Seconds until lock
   *
   * Emitted as a warning before the session locks.
   * Default warning is at 60 seconds remaining.
   */
  signals[SIGNAL_TIMEOUT_WARNING] =
    g_signal_new("timeout-warning",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE, 1,
                 G_TYPE_UINT);
}

/* GObject instance init */
static void
gn_session_manager_init(GnSessionManager *self)
{
  self->state = GN_SESSION_STATE_LOCKED;
  self->timeout_seconds = DEFAULT_SESSION_TIMEOUT_SEC;
  self->lock_on_idle = TRUE;
  self->last_activity = 0;
  self->session_started = 0;
  self->timeout_source_id = 0;
  self->password_hash = NULL;
  self->password_configured = FALSE;

  /* Load settings */
  self->settings = g_settings_new("org.gnostr.Signer");

  if (self->settings) {
    /* Load timeout from settings */
    GVariant *timeout_var = g_settings_get_value(self->settings, "lock-timeout-sec");
    if (timeout_var) {
      self->timeout_seconds = (guint)g_variant_get_int32(timeout_var);
      g_variant_unref(timeout_var);
    }
  }

  /* Try to load existing password from secret store */
  load_password_from_store(self);
}

/* Timer callback - check if session should lock */
static gboolean
on_timeout_expired(gpointer user_data)
{
  GnSessionManager *self = GN_SESSION_MANAGER(user_data);

  if (self->state != GN_SESSION_STATE_AUTHENTICATED) {
    self->timeout_source_id = 0;
    return G_SOURCE_REMOVE;
  }

  gint64 now = g_get_monotonic_time() / G_USEC_PER_SEC;
  gint64 elapsed = now - self->last_activity;
  gint64 remaining = (gint64)self->timeout_seconds - elapsed;

  if (remaining <= 0) {
    /* Session expired */
    g_debug("session-manager: Session expired after %u seconds", self->timeout_seconds);

    self->state = GN_SESSION_STATE_EXPIRED;
    self->timeout_source_id = 0;

    g_signal_emit(self, signals[SIGNAL_SESSION_EXPIRED], 0);
    g_signal_emit(self, signals[SIGNAL_SESSION_LOCKED], 0, (gint)GN_LOCK_REASON_TIMEOUT);

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_STATE]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_REMAINING_TIME]);

    return G_SOURCE_REMOVE;
  }

  /* Emit warning at 60 seconds, 30 seconds, and 10 seconds */
  if (remaining == 60 || remaining == 30 || remaining == 10) {
    g_signal_emit(self, signals[SIGNAL_TIMEOUT_WARNING], 0, (guint)remaining);
  }

  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_REMAINING_TIME]);

  return G_SOURCE_CONTINUE;
}

/* Start the auto-lock timer */
static void
gn_session_manager_start_timer(GnSessionManager *self)
{
  gn_session_manager_stop_timer(self);

  if (self->timeout_seconds == 0) {
    /* Auto-lock disabled */
    return;
  }

  self->last_activity = g_get_monotonic_time() / G_USEC_PER_SEC;

  /* Check every second for timeout */
  self->timeout_source_id = g_timeout_add_seconds(1, on_timeout_expired, self);
}

/* Stop the auto-lock timer */
static void
gn_session_manager_stop_timer(GnSessionManager *self)
{
  if (self->timeout_source_id != 0) {
    g_source_remove(self->timeout_source_id);
    self->timeout_source_id = 0;
  }
}

/* Public API */

GnSessionManager *
gn_session_manager_new(void)
{
  return g_object_new(GN_TYPE_SESSION_MANAGER, NULL);
}

GnSessionManager *
gn_session_manager_get_default(void)
{
  if (!default_instance) {
    default_instance = gn_session_manager_new();
  }
  return default_instance;
}

gboolean
gn_session_manager_authenticate(GnSessionManager *self,
                                const gchar *password,
                                GError **error)
{
  g_return_val_if_fail(GN_IS_SESSION_MANAGER(self), FALSE);

  /* Check rate limiting first (nostrc-1g1) */
  GnRateLimiter *limiter = gn_rate_limiter_get_default();
  if (!gn_rate_limiter_check_allowed(limiter)) {
    guint remaining = gn_rate_limiter_get_remaining_lockout(limiter);
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_PERMISSION_DENIED,
                "Too many failed attempts. Please wait %u seconds before trying again.",
                remaining);
    g_debug("session-manager: Rate limited, %u seconds remaining", remaining);
    return FALSE;
  }

  /* If no password is configured, any password is accepted
   * (or we auto-unlock if password is NULL) */
  if (!self->password_configured) {
    g_debug("session-manager: No password configured, auto-authenticating");

    /* Reset rate limiter on success */
    gn_rate_limiter_record_attempt(limiter, TRUE);

    self->state = GN_SESSION_STATE_AUTHENTICATED;
    self->session_started = g_get_monotonic_time() / G_USEC_PER_SEC;
    self->last_activity = self->session_started;

    gn_session_manager_start_timer(self);

    g_signal_emit(self, signals[SIGNAL_SESSION_UNLOCKED], 0);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_STATE]);

    return TRUE;
  }

  /* Password is required */
  if (!password || !*password) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_PERMISSION_DENIED,
                "Password is required");
    return FALSE;
  }

  /* Compute hash and compare */
  gchar *hash = compute_password_hash(password);
  if (!hash) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "Failed to hash password");
    return FALSE;
  }

  gboolean match = (g_strcmp0(hash, self->password_hash) == 0);
  secure_wipe(hash);
  g_free(hash);

  if (!match) {
    /* Record failed attempt for rate limiting (nostrc-1g1) */
    gn_rate_limiter_record_attempt(limiter, FALSE);

    /* Check if now rate limited after this failure */
    if (gn_rate_limiter_is_locked_out(limiter)) {
      guint remaining = gn_rate_limiter_get_remaining_lockout(limiter);
      g_set_error(error,
                  G_IO_ERROR,
                  G_IO_ERROR_PERMISSION_DENIED,
                  "Too many failed attempts. Please wait %u seconds before trying again.",
                  remaining);
    } else {
      guint attempts_left = gn_rate_limiter_get_attempts_remaining(limiter);
      g_set_error(error,
                  G_IO_ERROR,
                  G_IO_ERROR_PERMISSION_DENIED,
                  "Invalid password. %u attempt%s remaining.",
                  attempts_left, attempts_left == 1 ? "" : "s");
    }
    return FALSE;
  }

  /* Authentication successful - reset rate limiter (nostrc-1g1) */
  gn_rate_limiter_record_attempt(limiter, TRUE);

  g_debug("session-manager: Authentication successful");

  self->state = GN_SESSION_STATE_AUTHENTICATED;
  self->session_started = g_get_monotonic_time() / G_USEC_PER_SEC;
  self->last_activity = self->session_started;

  gn_session_manager_start_timer(self);

  g_signal_emit(self, signals[SIGNAL_SESSION_UNLOCKED], 0);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_STATE]);

  return TRUE;
}

gboolean
gn_session_manager_is_authenticated(GnSessionManager *self)
{
  g_return_val_if_fail(GN_IS_SESSION_MANAGER(self), FALSE);

  return self->state == GN_SESSION_STATE_AUTHENTICATED;
}

GnSessionState
gn_session_manager_get_state(GnSessionManager *self)
{
  g_return_val_if_fail(GN_IS_SESSION_MANAGER(self), GN_SESSION_STATE_LOCKED);

  return self->state;
}

guint
gn_session_manager_get_timeout(GnSessionManager *self)
{
  g_return_val_if_fail(GN_IS_SESSION_MANAGER(self), DEFAULT_SESSION_TIMEOUT_SEC);

  return self->timeout_seconds;
}

void
gn_session_manager_set_timeout(GnSessionManager *self, guint seconds)
{
  g_return_if_fail(GN_IS_SESSION_MANAGER(self));

  if (self->timeout_seconds == seconds)
    return;

  self->timeout_seconds = seconds;

  /* Persist to GSettings */
  if (self->settings) {
    g_settings_set_int(self->settings, "lock-timeout-sec", (gint)seconds);
  }

  /* Restart timer if session is active */
  if (self->state == GN_SESSION_STATE_AUTHENTICATED) {
    gn_session_manager_start_timer(self);
  }

  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_TIMEOUT]);
}

void
gn_session_manager_lock(GnSessionManager *self, GnLockReason reason)
{
  g_return_if_fail(GN_IS_SESSION_MANAGER(self));

  if (self->state == GN_SESSION_STATE_LOCKED)
    return;

  g_debug("session-manager: Locking session (reason=%d)", reason);

  gn_session_manager_stop_timer(self);

  self->state = GN_SESSION_STATE_LOCKED;
  self->session_started = 0;
  self->last_activity = 0;

  g_signal_emit(self, signals[SIGNAL_SESSION_LOCKED], 0, (gint)reason);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_STATE]);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_REMAINING_TIME]);
}

gboolean
gn_session_manager_is_locked(GnSessionManager *self)
{
  g_return_val_if_fail(GN_IS_SESSION_MANAGER(self), TRUE);

  return self->state != GN_SESSION_STATE_AUTHENTICATED;
}

void
gn_session_manager_extend(GnSessionManager *self)
{
  g_return_if_fail(GN_IS_SESSION_MANAGER(self));

  if (self->state != GN_SESSION_STATE_AUTHENTICATED)
    return;

  self->last_activity = g_get_monotonic_time() / G_USEC_PER_SEC;

  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_REMAINING_TIME]);
}

guint
gn_session_manager_get_remaining_time(GnSessionManager *self)
{
  g_return_val_if_fail(GN_IS_SESSION_MANAGER(self), 0);

  if (self->state != GN_SESSION_STATE_AUTHENTICATED)
    return 0;

  if (self->timeout_seconds == 0)
    return G_MAXUINT;  /* No timeout */

  gint64 now = g_get_monotonic_time() / G_USEC_PER_SEC;
  gint64 elapsed = now - self->last_activity;
  gint64 remaining = (gint64)self->timeout_seconds - elapsed;

  return remaining > 0 ? (guint)remaining : 0;
}

gboolean
gn_session_manager_set_password(GnSessionManager *self,
                                const gchar *current_password,
                                const gchar *new_password,
                                GError **error)
{
  g_return_val_if_fail(GN_IS_SESSION_MANAGER(self), FALSE);

  /* If password is configured, verify current password */
  if (self->password_configured) {
    if (!current_password || !*current_password) {
      g_set_error(error,
                  G_IO_ERROR,
                  G_IO_ERROR_PERMISSION_DENIED,
                  "Current password is required");
      return FALSE;
    }

    gchar *current_hash = compute_password_hash(current_password);
    gboolean match = (g_strcmp0(current_hash, self->password_hash) == 0);
    secure_wipe(current_hash);
    g_free(current_hash);

    if (!match) {
      g_set_error(error,
                  G_IO_ERROR,
                  G_IO_ERROR_PERMISSION_DENIED,
                  "Current password is incorrect");
      return FALSE;
    }
  }

  /* Set new password */
  if (!new_password || !*new_password) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_ARGUMENT,
                "New password cannot be empty");
    return FALSE;
  }

  gchar *new_hash = compute_password_hash(new_password);
  if (!new_hash) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "Failed to hash new password");
    return FALSE;
  }

  /* Store in secret store */
  if (!save_password_to_store(self, new_hash)) {
    secure_wipe(new_hash);
    g_free(new_hash);
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "Failed to store password");
    return FALSE;
  }

  /* Update in-memory state */
  if (self->password_hash) {
    secure_wipe(self->password_hash);
    g_free(self->password_hash);
  }
  self->password_hash = new_hash;
  self->password_configured = TRUE;

  g_debug("session-manager: Password set successfully");
  return TRUE;
}

gboolean
gn_session_manager_has_password(GnSessionManager *self)
{
  g_return_val_if_fail(GN_IS_SESSION_MANAGER(self), FALSE);

  return self->password_configured;
}

gboolean
gn_session_manager_clear_password(GnSessionManager *self,
                                  const gchar *current_password,
                                  GError **error)
{
  g_return_val_if_fail(GN_IS_SESSION_MANAGER(self), FALSE);

  if (!self->password_configured) {
    /* Already no password */
    return TRUE;
  }

  /* Verify current password */
  if (!current_password || !*current_password) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_PERMISSION_DENIED,
                "Current password is required");
    return FALSE;
  }

  gchar *current_hash = compute_password_hash(current_password);
  gboolean match = (g_strcmp0(current_hash, self->password_hash) == 0);
  secure_wipe(current_hash);
  g_free(current_hash);

  if (!match) {
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_PERMISSION_DENIED,
                "Current password is incorrect");
    return FALSE;
  }

  /* Clear from secret store */
  clear_password_from_store();

  /* Clear in-memory state */
  if (self->password_hash) {
    secure_wipe(self->password_hash);
    g_free(self->password_hash);
    self->password_hash = NULL;
  }
  self->password_configured = FALSE;

  g_debug("session-manager: Password cleared");
  return TRUE;
}
