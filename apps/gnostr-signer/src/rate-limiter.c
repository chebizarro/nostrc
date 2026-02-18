/* rate-limiter.c - Rate limiting implementation for authentication attempts
 *
 * Implements rate limiting to prevent brute force attacks on authentication
 * in gnostr-signer. Supports both global (UI password entry) and per-client
 * (NIP-46 bunker) rate limiting.
 *
 * Features:
 * - Per-client pubkey tracking with exponential backoff
 * - Configurable max attempts before lockout (default 5)
 * - Exponential backoff: 1s, 2s, 4s, 8s... up to 5 min max
 * - Persistent storage across app restarts
 * - Admin functions to clear rate limits
 * - GObject signals for lockout events
 *
 * Related to issue: nostrc-1g1
 */
#include "rate-limiter.h"
#include <json-glib/json-glib.h>
#include <string.h>
#include <time.h>

/* Signal IDs */
enum {
  SIGNAL_RATE_LIMIT_EXCEEDED,
  SIGNAL_LOCKOUT_EXPIRED,
  SIGNAL_CLIENT_RATE_LIMIT_EXCEEDED,
  SIGNAL_CLIENT_LOCKOUT_EXPIRED,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

/* Internal client state structure */
typedef struct {
  gchar *client_pubkey;
  guint failed_attempts;
  gint64 lockout_until;        /* Real time (Unix timestamp) when lockout expires */
  guint backoff_multiplier;    /* Current exponential backoff: 1, 2, 4, 8, etc. */
  gint64 last_attempt;         /* Real time (Unix timestamp) of last attempt */
} ClientState;

/* Private structure */
struct _GnRateLimiter {
  GObject parent_instance;

  /* Configuration */
  guint max_attempts;
  guint window_seconds;
  guint base_lockout_seconds;

  /* Global state (for UI password entry) */
  GQueue *attempt_times;      /* Queue of gint64 monotonic timestamps */
  gint64 lockout_until;       /* Monotonic timestamp when lockout expires, 0 if not locked */
  guint lockout_multiplier;   /* Exponential backoff multiplier (1, 2, 4, etc.) */
  guint lockout_timer_id;     /* GSource ID for lockout expiration timer */

  /* Per-client state (for NIP-46 bunker auth) */
  GHashTable *client_states;  /* client_pubkey -> ClientState* */

  /* Persistence */
  gchar *state_file_path;
  gboolean dirty;             /* State needs saving */
  guint save_timer_id;        /* Debounced save timer */
};

/* Singleton instance */
static GnRateLimiter *default_instance = NULL;

G_DEFINE_TYPE(GnRateLimiter, gn_rate_limiter, G_TYPE_OBJECT)

/* Forward declarations */
static void prune_old_attempts(GnRateLimiter *self);
static gboolean on_lockout_expired(gpointer user_data);
static void client_state_free(ClientState *cs);
static ClientState *client_state_new(const gchar *pubkey);
static ClientState *get_or_create_client_state(GnRateLimiter *self, const gchar *pubkey);
static void schedule_save(GnRateLimiter *self);
static gchar *get_state_file_path(void);

/* Free a client state structure */
static void
client_state_free(ClientState *cs) {
  if (!cs) return;
  g_free(cs->client_pubkey);
  g_free(cs);
}

/* Create a new client state */
static ClientState *
client_state_new(const gchar *pubkey) {
  ClientState *cs = g_new0(ClientState, 1);
  cs->client_pubkey = g_strdup(pubkey);
  cs->failed_attempts = 0;
  cs->lockout_until = 0;
  cs->backoff_multiplier = 1;
  cs->last_attempt = 0;
  return cs;
}

/* Get or create client state for a pubkey */
static ClientState *
get_or_create_client_state(GnRateLimiter *self, const gchar *pubkey) {
  if (!self || !pubkey || !*pubkey) return NULL;

  ClientState *cs = g_hash_table_lookup(self->client_states, pubkey);
  if (!cs) {
    cs = client_state_new(pubkey);
    g_hash_table_insert(self->client_states, g_strdup(pubkey), cs);
  }
  return cs;
}

/* Get the path for the rate limit state file */
static gchar *
get_state_file_path(void) {
  const gchar *config_dir = g_get_user_config_dir();
  gchar *app_dir = g_build_filename(config_dir, "gnostr-signer", NULL);

  /* Ensure directory exists */
  if (!g_file_test(app_dir, G_FILE_TEST_IS_DIR)) {
    g_mkdir_with_parents(app_dir, 0700);
  }

  gchar *path = g_build_filename(app_dir, "rate-limits.json", NULL);
  g_free(app_dir);
  return path;
}

/* Public function to free a GnClientRateLimitInfo */
void
gn_client_rate_limit_info_free(GnClientRateLimitInfo *info) {
  if (!info) return;
  g_free(info->client_pubkey);
  g_free(info);
}

static void
gn_rate_limiter_finalize(GObject *object) {
  GnRateLimiter *self = GN_RATE_LIMITER(object);

  /* Save state before cleanup */
  if (self->dirty) {
    gn_rate_limiter_save(self);
  }

  /* Cancel any pending timers */
  if (self->lockout_timer_id > 0) {
    g_source_remove(self->lockout_timer_id);
    self->lockout_timer_id = 0;
  }

  if (self->save_timer_id > 0) {
    g_source_remove(self->save_timer_id);
    self->save_timer_id = 0;
  }

  /* Free attempt times queue */
  if (self->attempt_times) {
    g_queue_free_full(self->attempt_times, g_free);
    self->attempt_times = NULL;
  }

  /* Free client states */
  g_clear_pointer(&self->client_states, g_hash_table_destroy);

  /* Free state file path */
  g_clear_pointer(&self->state_file_path, g_free);

  if (self == default_instance) {
    default_instance = NULL;
  }

  G_OBJECT_CLASS(gn_rate_limiter_parent_class)->finalize(object);
}

static void
gn_rate_limiter_class_init(GnRateLimiterClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->finalize = gn_rate_limiter_finalize;

  /**
   * GnRateLimiter::rate-limit-exceeded:
   * @self: The #GnRateLimiter that emitted the signal
   * @lockout_seconds: The lockout duration in seconds
   *
   * Emitted when the global rate limit is exceeded and a lockout begins.
   */
  signals[SIGNAL_RATE_LIMIT_EXCEEDED] =
    g_signal_new("rate-limit-exceeded",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE, 1, G_TYPE_UINT);

  /**
   * GnRateLimiter::lockout-expired:
   * @self: The #GnRateLimiter that emitted the signal
   *
   * Emitted when a global lockout period has expired.
   */
  signals[SIGNAL_LOCKOUT_EXPIRED] =
    g_signal_new("lockout-expired",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE, 0);

  /**
   * GnRateLimiter::client-rate-limit-exceeded:
   * @self: The #GnRateLimiter that emitted the signal
   * @client_pubkey: The client's public key
   * @lockout_seconds: The lockout duration in seconds
   *
   * Emitted when a client's rate limit is exceeded.
   */
  signals[SIGNAL_CLIENT_RATE_LIMIT_EXCEEDED] =
    g_signal_new("client-rate-limit-exceeded",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_UINT);

  /**
   * GnRateLimiter::client-lockout-expired:
   * @self: The #GnRateLimiter that emitted the signal
   * @client_pubkey: The client's public key
   *
   * Emitted when a client's lockout period has expired.
   */
  signals[SIGNAL_CLIENT_LOCKOUT_EXPIRED] =
    g_signal_new("client-lockout-expired",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gn_rate_limiter_init(GnRateLimiter *self) {
  self->max_attempts = GN_RATE_LIMITER_DEFAULT_MAX_ATTEMPTS;
  self->window_seconds = GN_RATE_LIMITER_DEFAULT_WINDOW_SECONDS;
  self->base_lockout_seconds = GN_RATE_LIMITER_DEFAULT_LOCKOUT_SECONDS;
  self->attempt_times = g_queue_new();
  self->lockout_until = 0;
  self->lockout_multiplier = 1;
  self->lockout_timer_id = 0;
  self->client_states = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, (GDestroyNotify)client_state_free);
  self->state_file_path = get_state_file_path();
  self->dirty = FALSE;
  self->save_timer_id = 0;

  /* Load persisted state */
  gn_rate_limiter_load(self);
}

GnRateLimiter *
gn_rate_limiter_new(guint max_attempts,
                    guint window_seconds,
                    guint lockout_seconds) {
  GnRateLimiter *self = g_object_new(GN_TYPE_RATE_LIMITER, NULL);

  self->max_attempts = max_attempts > 0 ? max_attempts : GN_RATE_LIMITER_DEFAULT_MAX_ATTEMPTS;
  self->window_seconds = window_seconds > 0 ? window_seconds : GN_RATE_LIMITER_DEFAULT_WINDOW_SECONDS;
  self->base_lockout_seconds = lockout_seconds > 0 ? lockout_seconds : GN_RATE_LIMITER_DEFAULT_LOCKOUT_SECONDS;

  return self;
}

GnRateLimiter *
gn_rate_limiter_new_default(void) {
  return gn_rate_limiter_new(GN_RATE_LIMITER_DEFAULT_MAX_ATTEMPTS,
                              GN_RATE_LIMITER_DEFAULT_WINDOW_SECONDS,
                              GN_RATE_LIMITER_DEFAULT_LOCKOUT_SECONDS);
}

GnRateLimiter *
gn_rate_limiter_get_default(void) {
  if (!default_instance) {
    default_instance = gn_rate_limiter_new_default();
  }
  return default_instance;
}

/**
 * prune_old_attempts:
 * @self: The rate limiter
 *
 * Removes attempt timestamps that are outside the current time window.
 */
static void
prune_old_attempts(GnRateLimiter *self) {
  if (!self || !self->attempt_times) return;

  gint64 now = g_get_monotonic_time();
  gint64 window_start = now - (self->window_seconds * G_USEC_PER_SEC);

  /* Remove attempts older than the window */
  while (!g_queue_is_empty(self->attempt_times)) {
    gint64 *oldest = g_queue_peek_head(self->attempt_times);
    if (oldest && *oldest < window_start) {
      g_free(g_queue_pop_head(self->attempt_times));
    } else {
      break;
    }
  }
}

/**
 * on_lockout_expired:
 * @user_data: The rate limiter
 *
 * Callback when global lockout timer expires.
 *
 * Returns: %G_SOURCE_REMOVE to stop the timer
 */
static gboolean
on_lockout_expired(gpointer user_data) {
  GnRateLimiter *self = GN_RATE_LIMITER(user_data);

  if (!self) return G_SOURCE_REMOVE;

  self->lockout_timer_id = 0;
  self->lockout_until = 0;

  /* Clear attempts so user gets fresh start */
  if (self->attempt_times) {
    g_queue_free_full(self->attempt_times, g_free);
    self->attempt_times = g_queue_new();
  }

  g_signal_emit(self, signals[SIGNAL_LOCKOUT_EXPIRED], 0);

  return G_SOURCE_REMOVE;
}

/* ============================================================================
 * Global rate limiting (for UI password entry)
 * ============================================================================ */

gboolean
gn_rate_limiter_check_allowed(GnRateLimiter *self) {
  if (!self) return TRUE;

  /* Check if currently locked out */
  if (self->lockout_until > 0) {
    gint64 now = g_get_monotonic_time();
    if (now < self->lockout_until) {
      return FALSE;
    }
    /* Lockout expired */
    self->lockout_until = 0;
  }

  /* Prune old attempts and check count */
  prune_old_attempts(self);

  guint recent_attempts = g_queue_get_length(self->attempt_times);
  return recent_attempts < self->max_attempts;
}

void
gn_rate_limiter_record_attempt(GnRateLimiter *self, gboolean success) {
  if (!self) return;

  if (success) {
    /* Reset on successful authentication */
    gn_rate_limiter_reset(self);
    return;
  }

  /* Check if already locked out */
  if (self->lockout_until > 0) {
    gint64 now = g_get_monotonic_time();
    if (now < self->lockout_until) {
      /* Still locked out, don't record */
      return;
    }
  }

  /* Prune old attempts */
  prune_old_attempts(self);

  /* Record this failed attempt */
  gint64 *timestamp = g_new(gint64, 1);
  *timestamp = g_get_monotonic_time();
  g_queue_push_tail(self->attempt_times, timestamp);

  /* Check if we've exceeded the limit */
  guint recent_attempts = g_queue_get_length(self->attempt_times);
  if (recent_attempts >= self->max_attempts) {
    /* Calculate lockout duration with exponential backoff */
    guint lockout_duration = self->base_lockout_seconds * self->lockout_multiplier;

    /* Cap at maximum lockout */
    if (lockout_duration > GN_RATE_LIMITER_MAX_LOCKOUT_SECONDS) {
      lockout_duration = GN_RATE_LIMITER_MAX_LOCKOUT_SECONDS;
    }

    /* Set lockout end time */
    gint64 now = g_get_monotonic_time();
    self->lockout_until = now + (lockout_duration * G_USEC_PER_SEC);

    /* Increase multiplier for next time (exponential backoff) */
    if (self->lockout_multiplier < GN_RATE_LIMITER_MAX_BACKOFF_MULTIPLIER) {
      self->lockout_multiplier *= 2;
    }

    /* Cancel any existing timer */
    if (self->lockout_timer_id > 0) {
      g_source_remove(self->lockout_timer_id);
    }

    /* Start timer for lockout expiration */
    self->lockout_timer_id = g_timeout_add_seconds(lockout_duration,
                                                    on_lockout_expired,
                                                    self);

    /* Emit signal */
    g_signal_emit(self, signals[SIGNAL_RATE_LIMIT_EXCEEDED], 0, lockout_duration);

    g_debug("Rate limit exceeded: %u attempts in window, locking out for %u seconds (multiplier: %u)",
            recent_attempts, lockout_duration, self->lockout_multiplier / 2);
  }
}

guint
gn_rate_limiter_get_remaining_lockout(GnRateLimiter *self) {
  if (!self) return 0;

  if (self->lockout_until <= 0) return 0;

  gint64 now = g_get_monotonic_time();
  if (now >= self->lockout_until) {
    self->lockout_until = 0;
    return 0;
  }

  gint64 remaining_usec = self->lockout_until - now;
  guint remaining_sec = (guint)((remaining_usec + G_USEC_PER_SEC - 1) / G_USEC_PER_SEC);

  return remaining_sec;
}

guint
gn_rate_limiter_get_attempts_remaining(GnRateLimiter *self) {
  if (!self) return 0;

  /* If locked out, no attempts remaining */
  if (gn_rate_limiter_is_locked_out(self)) {
    return 0;
  }

  prune_old_attempts(self);
  guint recent_attempts = g_queue_get_length(self->attempt_times);

  if (recent_attempts >= self->max_attempts) {
    return 0;
  }

  return self->max_attempts - recent_attempts;
}

void
gn_rate_limiter_reset(GnRateLimiter *self) {
  if (!self) return;

  /* Cancel any pending timer */
  if (self->lockout_timer_id > 0) {
    g_source_remove(self->lockout_timer_id);
    self->lockout_timer_id = 0;
  }

  /* Clear lockout state */
  self->lockout_until = 0;
  self->lockout_multiplier = 1;

  /* Clear attempt history */
  if (self->attempt_times) {
    g_queue_free_full(self->attempt_times, g_free);
    self->attempt_times = g_queue_new();
  }

  g_debug("Rate limiter reset");
}

guint
gn_rate_limiter_get_lockout_multiplier(GnRateLimiter *self) {
  if (!self) return 1;
  return self->lockout_multiplier;
}

gboolean
gn_rate_limiter_is_locked_out(GnRateLimiter *self) {
  if (!self) return FALSE;

  if (self->lockout_until <= 0) return FALSE;

  gint64 now = g_get_monotonic_time();
  if (now >= self->lockout_until) {
    self->lockout_until = 0;
    return FALSE;
  }

  return TRUE;
}

/* ============================================================================
 * Per-client rate limiting (for NIP-46 bunker authentication)
 * ============================================================================ */

GnRateLimitStatus
gn_rate_limiter_check_client(GnRateLimiter *self,
                              const gchar *client_pubkey,
                              guint *remaining_seconds) {
  if (!self || !client_pubkey || !*client_pubkey) {
    if (remaining_seconds) *remaining_seconds = 0;
    return GN_RATE_LIMIT_ALLOWED;
  }

  ClientState *cs = g_hash_table_lookup(self->client_states, client_pubkey);
  if (!cs) {
    /* No state for this client - allowed */
    if (remaining_seconds) *remaining_seconds = 0;
    return GN_RATE_LIMIT_ALLOWED;
  }

  gint64 now = (gint64)time(NULL);

  /* Check if currently locked out */
  if (cs->lockout_until > 0) {
    if (now < cs->lockout_until) {
      guint remaining = (guint)(cs->lockout_until - now);
      if (remaining_seconds) *remaining_seconds = remaining;
      return GN_RATE_LIMIT_LOCKED_OUT;
    }
    /* Lockout expired - emit signal and reset lockout */
    cs->lockout_until = 0;
    /* Keep failed_attempts and backoff_multiplier for continued tracking */
    self->dirty = TRUE;
    schedule_save(self);
    g_signal_emit(self, signals[SIGNAL_CLIENT_LOCKOUT_EXPIRED], 0, client_pubkey);
  }

  /* Check if in backoff period (rate limited but not fully locked out) */
  if (cs->failed_attempts > 0 && cs->failed_attempts < self->max_attempts) {
    /* Calculate backoff delay based on attempts */
    guint backoff_seconds = self->base_lockout_seconds * cs->backoff_multiplier;
    if (backoff_seconds > GN_RATE_LIMITER_MAX_LOCKOUT_SECONDS) {
      backoff_seconds = GN_RATE_LIMITER_MAX_LOCKOUT_SECONDS;
    }

    gint64 next_allowed = cs->last_attempt + backoff_seconds;
    if (now < next_allowed) {
      guint remaining = (guint)(next_allowed - now);
      if (remaining_seconds) *remaining_seconds = remaining;
      return GN_RATE_LIMIT_BACKOFF;
    }
  }

  if (remaining_seconds) *remaining_seconds = 0;
  return GN_RATE_LIMIT_ALLOWED;
}

void
gn_rate_limiter_record_client_attempt(GnRateLimiter *self,
                                       const gchar *client_pubkey,
                                       gboolean success) {
  if (!self || !client_pubkey || !*client_pubkey) return;

  if (success) {
    /* Reset on successful authentication */
    gn_rate_limiter_reset_client(self, client_pubkey);
    return;
  }

  ClientState *cs = get_or_create_client_state(self, client_pubkey);
  if (!cs) return;

  gint64 now = (gint64)time(NULL);

  /* Check if still in lockout */
  if (cs->lockout_until > 0 && now < cs->lockout_until) {
    /* Still locked out, ignore attempt */
    return;
  }

  /* Record this failed attempt */
  cs->failed_attempts++;
  cs->last_attempt = now;

  /* Increase backoff multiplier (exponential: 1, 2, 4, 8, ...) */
  if (cs->backoff_multiplier < GN_RATE_LIMITER_MAX_BACKOFF_MULTIPLIER) {
    cs->backoff_multiplier *= 2;
  }

  /* Check if we've exceeded the limit */
  if (cs->failed_attempts >= self->max_attempts) {
    /* Calculate lockout duration with exponential backoff */
    guint lockout_duration = self->base_lockout_seconds * cs->backoff_multiplier;

    /* Cap at maximum lockout */
    if (lockout_duration > GN_RATE_LIMITER_MAX_LOCKOUT_SECONDS) {
      lockout_duration = GN_RATE_LIMITER_MAX_LOCKOUT_SECONDS;
    }

    /* Set lockout end time */
    cs->lockout_until = now + lockout_duration;

    g_info("Client %s rate limit exceeded: %u attempts, locking out for %u seconds",
           client_pubkey, cs->failed_attempts, lockout_duration);

    /* Emit signal */
    g_signal_emit(self, signals[SIGNAL_CLIENT_RATE_LIMIT_EXCEEDED], 0,
                  client_pubkey, lockout_duration);
  } else {
    /* Not locked out yet, but in backoff */
    guint backoff = self->base_lockout_seconds * cs->backoff_multiplier;
    if (backoff > GN_RATE_LIMITER_MAX_LOCKOUT_SECONDS) {
      backoff = GN_RATE_LIMITER_MAX_LOCKOUT_SECONDS;
    }
    g_debug("Client %s failed attempt %u/%u, backoff %u seconds",
            client_pubkey, cs->failed_attempts, self->max_attempts, backoff);
  }

  self->dirty = TRUE;
  schedule_save(self);
}

guint
gn_rate_limiter_get_client_remaining_lockout(GnRateLimiter *self,
                                              const gchar *client_pubkey) {
  if (!self || !client_pubkey || !*client_pubkey) return 0;

  ClientState *cs = g_hash_table_lookup(self->client_states, client_pubkey);
  if (!cs) return 0;

  if (cs->lockout_until <= 0) return 0;

  gint64 now = (gint64)time(NULL);
  if (now >= cs->lockout_until) {
    cs->lockout_until = 0;
    self->dirty = TRUE;
    schedule_save(self);
    return 0;
  }

  return (guint)(cs->lockout_until - now);
}

guint
gn_rate_limiter_get_client_attempts_remaining(GnRateLimiter *self,
                                               const gchar *client_pubkey) {
  if (!self || !client_pubkey || !*client_pubkey) return self->max_attempts;

  ClientState *cs = g_hash_table_lookup(self->client_states, client_pubkey);
  if (!cs) return self->max_attempts;

  /* If locked out, no attempts remaining */
  if (gn_rate_limiter_is_client_locked_out(self, client_pubkey)) {
    return 0;
  }

  if (cs->failed_attempts >= self->max_attempts) {
    return 0;
  }

  return self->max_attempts - cs->failed_attempts;
}

void
gn_rate_limiter_reset_client(GnRateLimiter *self,
                              const gchar *client_pubkey) {
  if (!self || !client_pubkey || !*client_pubkey) return;

  /* Remove the client state entirely on successful auth */
  if (g_hash_table_remove(self->client_states, client_pubkey)) {
    g_debug("Rate limit state cleared for client %s", client_pubkey);
    self->dirty = TRUE;
    schedule_save(self);
  }
}

void
gn_rate_limiter_clear_all_clients(GnRateLimiter *self) {
  if (!self) return;

  guint count = g_hash_table_size(self->client_states);
  g_hash_table_remove_all(self->client_states);

  g_info("Admin: Cleared rate limit state for %u clients", count);

  self->dirty = TRUE;
  schedule_save(self);
}

gboolean
gn_rate_limiter_is_client_locked_out(GnRateLimiter *self,
                                      const gchar *client_pubkey) {
  if (!self || !client_pubkey || !*client_pubkey) return FALSE;

  ClientState *cs = g_hash_table_lookup(self->client_states, client_pubkey);
  if (!cs) return FALSE;

  if (cs->lockout_until <= 0) return FALSE;

  gint64 now = (gint64)time(NULL);
  if (now >= cs->lockout_until) {
    cs->lockout_until = 0;
    self->dirty = TRUE;
    schedule_save(self);
    return FALSE;
  }

  return TRUE;
}

GnClientRateLimitInfo *
gn_rate_limiter_get_client_info(GnRateLimiter *self,
                                 const gchar *client_pubkey) {
  if (!self || !client_pubkey || !*client_pubkey) return NULL;

  ClientState *cs = g_hash_table_lookup(self->client_states, client_pubkey);
  if (!cs) return NULL;

  GnClientRateLimitInfo *info = g_new0(GnClientRateLimitInfo, 1);
  info->client_pubkey = g_strdup(cs->client_pubkey);
  info->failed_attempts = cs->failed_attempts;
  info->lockout_until = cs->lockout_until;
  info->backoff_multiplier = cs->backoff_multiplier;
  info->last_attempt = cs->last_attempt;

  return info;
}

GPtrArray *
gn_rate_limiter_list_clients(GnRateLimiter *self) {
  GPtrArray *arr = g_ptr_array_new_with_free_func((GDestroyNotify)gn_client_rate_limit_info_free);

  if (!self) return arr;

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->client_states);

  while (g_hash_table_iter_next(&iter, &key, &value)) {
    ClientState *cs = (ClientState *)value;
    GnClientRateLimitInfo *info = g_new0(GnClientRateLimitInfo, 1);
    info->client_pubkey = g_strdup(cs->client_pubkey);
    info->failed_attempts = cs->failed_attempts;
    info->lockout_until = cs->lockout_until;
    info->backoff_multiplier = cs->backoff_multiplier;
    info->last_attempt = cs->last_attempt;
    g_ptr_array_add(arr, info);
  }

  return arr;
}

/* ============================================================================
 * Persistence
 * ============================================================================ */

static gboolean
do_save(gpointer user_data) {
  GnRateLimiter *self = GN_RATE_LIMITER(user_data);
  if (!self) return G_SOURCE_REMOVE;

  self->save_timer_id = 0;
  gn_rate_limiter_save(self);

  return G_SOURCE_REMOVE;
}

static void
schedule_save(GnRateLimiter *self) {
  if (!self) return;

  /* Debounce saves - wait 1 second before actually saving */
  if (self->save_timer_id > 0) {
    g_source_remove(self->save_timer_id);
  }
  self->save_timer_id = g_timeout_add_seconds(1, do_save, self);
}

gboolean
gn_rate_limiter_save(GnRateLimiter *self) {
  if (!self || !self->state_file_path) return FALSE;

  JsonBuilder *builder = json_builder_new();
  json_builder_begin_object(builder);

  /* Version for future compatibility */
  json_builder_set_member_name(builder, "version");
  json_builder_add_int_value(builder, 1);

  /* Save time for expiration calculations */
  json_builder_set_member_name(builder, "saved_at");
  json_builder_add_int_value(builder, (gint64)time(NULL));

  /* Client states */
  json_builder_set_member_name(builder, "clients");
  json_builder_begin_array(builder);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->client_states);

  while (g_hash_table_iter_next(&iter, &key, &value)) {
    ClientState *cs = (ClientState *)value;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "pubkey");
    json_builder_add_string_value(builder, cs->client_pubkey);
    json_builder_set_member_name(builder, "failed_attempts");
    json_builder_add_int_value(builder, cs->failed_attempts);
    json_builder_set_member_name(builder, "lockout_until");
    json_builder_add_int_value(builder, cs->lockout_until);
    json_builder_set_member_name(builder, "backoff_multiplier");
    json_builder_add_int_value(builder, cs->backoff_multiplier);
    json_builder_set_member_name(builder, "last_attempt");
    json_builder_add_int_value(builder, cs->last_attempt);
    json_builder_end_object(builder);
  }

  json_builder_end_array(builder);
  json_builder_end_object(builder);

  JsonNode *root = json_builder_get_root(builder);
  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, root);
  json_generator_set_pretty(gen, TRUE);

  GError *error = NULL;
  gboolean success = json_generator_to_file(gen, self->state_file_path, &error);

  if (!success) {
    g_warning("Failed to save rate limit state: %s", error->message);
    g_error_free(error);
  } else {
    g_debug("Rate limit state saved to %s", self->state_file_path);
    self->dirty = FALSE;
  }

  json_node_unref(root);
  g_object_unref(gen);
  g_object_unref(builder);

  return success;
}

gboolean
gn_rate_limiter_load(GnRateLimiter *self) {
  if (!self || !self->state_file_path) return FALSE;

  if (!g_file_test(self->state_file_path, G_FILE_TEST_EXISTS)) {
    return FALSE;
  }

  JsonParser *parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_file(parser, self->state_file_path, &error)) {
    g_warning("Failed to load rate limit state: %s", error->message);
    g_error_free(error);
    g_object_unref(parser);
    return FALSE;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root)) {
    g_warning("Invalid rate limit state file format");
    g_object_unref(parser);
    return FALSE;
  }

  JsonObject *obj = json_node_get_object(root);

  /* Check version */
  gint64 version = json_object_get_int_member_with_default(obj, "version", 0);
  if (version != 1) {
    g_warning("Unknown rate limit state version: %" G_GINT64_FORMAT, version);
    g_object_unref(parser);
    return FALSE;
  }

  /* Load client states */
  if (json_object_has_member(obj, "clients")) {
    JsonArray *clients = json_object_get_array_member(obj, "clients");
    guint len = json_array_get_length(clients);
    gint64 now = (gint64)time(NULL);

    for (guint i = 0; i < len; i++) {
      JsonObject *client_obj = json_array_get_object_element(clients, i);

      const gchar *pubkey = json_object_get_string_member(client_obj, "pubkey");
      if (!pubkey || !*pubkey) continue;

      gint64 lockout_until = json_object_get_int_member_with_default(client_obj, "lockout_until", 0);

      /* Skip expired lockouts that have no meaningful state */
      gint64 last_attempt = json_object_get_int_member_with_default(client_obj, "last_attempt", 0);
      gint64 age = now - last_attempt;

      /* If last attempt was more than window_seconds ago and not locked out, skip */
      if (lockout_until <= now && age > (gint64)self->window_seconds) {
        continue;
      }

      ClientState *cs = client_state_new(pubkey);
      cs->failed_attempts = (guint)json_object_get_int_member_with_default(client_obj, "failed_attempts", 0);
      cs->lockout_until = lockout_until;
      cs->backoff_multiplier = (guint)json_object_get_int_member_with_default(client_obj, "backoff_multiplier", 1);
      cs->last_attempt = last_attempt;

      /* Ensure backoff_multiplier is at least 1 */
      if (cs->backoff_multiplier < 1) cs->backoff_multiplier = 1;

      g_hash_table_insert(self->client_states, g_strdup(pubkey), cs);
    }

    g_debug("Loaded rate limit state for %u clients from %s",
            g_hash_table_size(self->client_states), self->state_file_path);
  }

  g_object_unref(parser);
  self->dirty = FALSE;
  return TRUE;
}

/* ============================================================================
 * User-friendly error messages
 * ============================================================================ */

gchar *
gn_rate_limiter_format_error_message(GnRateLimitStatus status,
                                      guint remaining_seconds) {
  switch (status) {
    case GN_RATE_LIMIT_ALLOWED:
      return g_strdup("Authentication allowed");

    case GN_RATE_LIMIT_BACKOFF: {
      if (remaining_seconds < 60) {
        return g_strdup_printf(
          "Too many failed attempts. Please wait %u second%s before trying again.",
          remaining_seconds, remaining_seconds == 1 ? "" : "s");
      } else if (remaining_seconds < 3600) {
        guint minutes = remaining_seconds / 60;
        guint seconds = remaining_seconds % 60;
        if (seconds > 0) {
          return g_strdup_printf(
            "Too many failed attempts. Please wait %u minute%s and %u second%s before trying again.",
            minutes, minutes == 1 ? "" : "s",
            seconds, seconds == 1 ? "" : "s");
        } else {
          return g_strdup_printf(
            "Too many failed attempts. Please wait %u minute%s before trying again.",
            minutes, minutes == 1 ? "" : "s");
        }
      } else {
        guint hours = remaining_seconds / 3600;
        return g_strdup_printf(
          "Too many failed attempts. Please wait %u hour%s before trying again.",
          hours, hours == 1 ? "" : "s");
      }
    }

    case GN_RATE_LIMIT_LOCKED_OUT: {
      if (remaining_seconds < 60) {
        return g_strdup_printf(
          "This client has been temporarily locked out due to too many failed authentication attempts. "
          "Please wait %u second%s before trying again.",
          remaining_seconds, remaining_seconds == 1 ? "" : "s");
      } else if (remaining_seconds < 3600) {
        guint minutes = (remaining_seconds + 30) / 60;  /* Round to nearest minute */
        return g_strdup_printf(
          "This client has been temporarily locked out due to too many failed authentication attempts. "
          "Please wait approximately %u minute%s before trying again.",
          minutes, minutes == 1 ? "" : "s");
      } else {
        guint hours = remaining_seconds / 3600;
        return g_strdup_printf(
          "This client has been temporarily locked out due to too many failed authentication attempts. "
          "Please wait approximately %u hour%s before trying again.",
          hours, hours == 1 ? "" : "s");
      }
    }

    default:
      return g_strdup("Unknown rate limit status");
  }
}
