/* rate-limiter.c - Rate limiting implementation for authentication attempts
 *
 * Implements rate limiting to prevent brute force attacks on password/passphrase
 * entry. Uses in-memory storage that is cleared on app restart.
 *
 * Features:
 * - Tracks failed authentication attempts within a sliding time window
 * - Enforces lockout period after max attempts exceeded
 * - Implements exponential backoff on repeated lockouts (30s, 60s, 120s, etc.)
 * - Emits signal when rate limit is exceeded
 *
 * Related to issue: nostrc-1g1
 */
#include "rate-limiter.h"
#include <string.h>

/* Signal IDs */
enum {
  SIGNAL_RATE_LIMIT_EXCEEDED,
  SIGNAL_LOCKOUT_EXPIRED,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

/* Private structure */
struct _GnRateLimiter {
  GObject parent_instance;

  /* Configuration */
  guint max_attempts;
  guint window_seconds;
  guint base_lockout_seconds;

  /* State */
  GQueue *attempt_times;      /* Queue of gint64 timestamps (microseconds) */
  gint64 lockout_until;       /* Timestamp when lockout expires (microseconds), 0 if not locked */
  guint lockout_multiplier;   /* Exponential backoff multiplier (1, 2, 4, etc.) */
  guint lockout_timer_id;     /* GSource ID for lockout expiration timer */
};

/* Singleton instance */
static GnRateLimiter *default_instance = NULL;

G_DEFINE_TYPE(GnRateLimiter, gn_rate_limiter, G_TYPE_OBJECT)

/* Forward declarations */
static void prune_old_attempts(GnRateLimiter *self);
static gboolean on_lockout_expired(gpointer user_data);

static void
gn_rate_limiter_finalize(GObject *object) {
  GnRateLimiter *self = GN_RATE_LIMITER(object);

  /* Cancel any pending timer */
  if (self->lockout_timer_id > 0) {
    g_source_remove(self->lockout_timer_id);
    self->lockout_timer_id = 0;
  }

  /* Free attempt times queue */
  if (self->attempt_times) {
    g_queue_free_full(self->attempt_times, g_free);
    self->attempt_times = NULL;
  }

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
   * Emitted when the rate limit is exceeded and a lockout begins.
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
   * Emitted when a lockout period has expired and attempts are allowed again.
   */
  signals[SIGNAL_LOCKOUT_EXPIRED] =
    g_signal_new("lockout-expired",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE, 0);
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
 * Callback when lockout timer expires.
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
    if (self->lockout_multiplier < 64) {  /* Cap multiplier at 64x */
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
