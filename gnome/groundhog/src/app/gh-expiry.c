#include "gh-expiry.h"
#include "gh-store-conversations.h"

#include <string.h>

#define HOUR_S              3600
#define DAY_S               86400
#define RETRY_S             60   /* after a failed purge */
/* A deferred WAL truncation waits out the store's once-a-minute limit. */
#define CHECKPOINT_DELAY_S  61
/* The longest wait: wall-clock jumps and suspends are noticed within it. */
#define RECHECK_S           (10 * 60)

struct _GhExpiry {
  GObject parent_instance;
  GhStore *store;                      /* borrowed; NULL once disposed */
  GhStoreConversations *conversations; /* nullable */
  GhClock *clock;                      /* the store's */
  gchar *account;
  gint retention_days;
  gint64 default_timer;
  /* Wake-ups, unix seconds (0: none); the timer waits for the earliest. */
  gint64 purge_at;       /* at open, or a retry after a failure */
  gint64 expires_at;     /* the earliest stored expiry */
  gint64 retention_at;   /* the next daily retention run */
  gint64 checkpoint_at;  /* a deferred WAL truncation */
  guint timer;
  GWeakRef *notify_ref;  /* the store's expiry notification data (store-owned) */
};

enum { SIGNAL_PURGED, SIGNAL_TIMER_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhExpiry, gh_expiry, G_TYPE_OBJECT)

/* ---- policy ------------------------------------------------------------------ */

gboolean
gh_expiry_timer_is_valid(gint64 seconds)
{
  return seconds == GH_EXPIRY_TIMER_OFF || seconds == GH_EXPIRY_TIMER_DAY ||
         seconds == GH_EXPIRY_TIMER_WEEK || seconds == GH_EXPIRY_TIMER_FOUR_WEEKS;
}

gint64
gh_expiry_message_expiration(gint64 sent_at, gint64 timer)
{
  g_return_val_if_fail(sent_at > 0 && timer >= 0, 0);
  return timer > 0 ? sent_at + timer : 0;
}

gint64
gh_expiry_outer_jitter_max(gint64 sent_at, gint64 expires_at)
{
  g_return_val_if_fail(expires_at > sent_at, 0);
  return MIN(expires_at - sent_at, GH_EXPIRY_MAX_JITTER);
}

gint64
gh_expiry_outer_expiration(gint64 sent_at, gint64 expires_at, gint64 jitter)
{
  g_return_val_if_fail(sent_at > 0 && expires_at > sent_at, expires_at);
  gint64 later = expires_at + CLAMP(jitter, 0, gh_expiry_outer_jitter_max(sent_at, expires_at));
  return (later + HOUR_S - 1) / HOUR_S * HOUR_S;
}

gboolean
gh_expiry_draw_outer(GhClock *clock, gint64 sent_at, gint64 expires_at,
                     GhNip17OuterExpiration *out)
{
  g_return_val_if_fail(clock != NULL && out != NULL, FALSE);
  if (sent_at <= 0 || expires_at <= sent_at)
    return FALSE;
  const gint64 jitter_max = gh_expiry_outer_jitter_max(sent_at, expires_at);
  gint64 *layers[] = { &out->recipient.seal, &out->recipient.wrap,
                       &out->self_copy.seal, &out->self_copy.wrap };
  for (guint i = 0; i < G_N_ELEMENTS(layers); i++)
    *layers[i] = gh_expiry_outer_expiration(sent_at, expires_at,
                                            gh_clock_random_range(clock, 0, jitter_max));
  return TRUE;
}

gboolean
gh_expiry_draw_room(GhClock *clock, gint64 sent_at, gint64 expires_at, guint n_recipients,
                    GhNip17RoomExpiration *out)
{
  g_return_val_if_fail(clock != NULL && out != NULL, FALSE);
  if (sent_at <= 0 || expires_at <= sent_at || n_recipients > GH_NIP17_MAX_SEND_RECIPIENTS)
    return FALSE;
  const gint64 jitter_max = gh_expiry_outer_jitter_max(sent_at, expires_at);
  memset(out, 0, sizeof *out);
  out->n_recipients = n_recipients;
  for (guint i = 0; i <= n_recipients; i++) {
    GhNip17LayerExpiration *layer = i < n_recipients ? &out->recipients[i] : &out->self_copy;
    layer->seal = gh_expiry_outer_expiration(sent_at, expires_at,
                                             gh_clock_random_range(clock, 0, jitter_max));
    layer->wrap = gh_expiry_outer_expiration(sent_at, expires_at,
                                             gh_clock_random_range(clock, 0, jitter_max));
  }
  return TRUE;
}

gint64
gh_expiry_retention_cutoff(gint64 now, gint days)
{
  if (days <= 0)
    return 0;
  return MAX(now - (gint64)MIN(days, GH_EXPIRY_MAX_RETENTION_DAYS) * DAY_S, 1);
}

/* ---- scheduling ---------------------------------------------------------------- */

static gint64
now_unix(GhExpiry *self)
{
  return gh_clock_get_unix(self->clock);
}

static gint64
earliest(gint64 a, gint64 b)
{
  return !a ? b : !b ? a : MIN(a, b);
}

static gint64
next_run(GhExpiry *self)
{
  return earliest(earliest(self->purge_at, self->expires_at),
                  earliest(self->retention_at, self->checkpoint_at));
}

static gboolean on_timer(gpointer data);

static void
schedule(GhExpiry *self)
{
  if (self->timer) {
    gh_clock_source_remove(self->clock, self->timer);
    self->timer = 0;
  }
  gint64 at = next_run(self);
  if (!self->store || !at)
    return;
  gint64 now_ms = gh_clock_get_real_time(self->clock) / 1000;
  gint64 delay_ms = CLAMP(at * 1000 - now_ms, 0, (gint64)RECHECK_S * 1000);
  /* The timer holds no reference: dispose removes it. */
  self->timer = gh_clock_timeout_add(self->clock, (guint64)delay_ms, on_timer, self, NULL);
}

static gboolean
run_purge(GhExpiry *self, GError **error)
{
  const gint64 now = now_unix(self);
  const gint64 cutoff = gh_expiry_retention_cutoff(now, self->retention_days);
  GhStorePurgeStats stats = { 0 };
  g_auto(GStrv) rumor_ids = NULL;
  gboolean ok;
  if (self->conversations) {
    ok = gh_store_conversations_purge(self->conversations, cutoff, &stats, &rumor_ids, error);
  } else {
    g_autoptr(GPtrArray) purged = NULL;
    ok = gh_store_purge_full(self->store, cutoff, &purged, &stats, error);
    g_autoptr(GStrvBuilder) ids = g_strv_builder_new();
    for (guint i = 0; ok && i < purged->len; i++) {
      GhStorePurgedMessage *message = g_ptr_array_index(purged, i);
      if (message->backend == GH_STORE_BACKEND_NIP17)
        g_strv_builder_add(ids, message->backend_msg_id);
    }
    rumor_ids = g_strv_builder_end(ids);
  }
  if (!ok) {
    /* Everything that was due waits for the retry (no spinning on a store
     * that keeps failing); a success recomputes it all. */
    self->purge_at = now + RETRY_S;
    if (self->expires_at && self->expires_at < self->purge_at)
      self->expires_at = self->purge_at;
    if (self->retention_at && self->retention_at < self->purge_at)
      self->retention_at = self->purge_at;
    schedule(self);
    return FALSE;
  }
  self->purge_at = 0;
  self->expires_at = stats.next_expires_at;
  self->retention_at = self->retention_days > 0 ? now + DAY_S : 0;
  if (stats.checkpointed)
    self->checkpoint_at = 0;
  else if (stats.checkpoint_deferred && !self->checkpoint_at)
    self->checkpoint_at = now + CHECKPOINT_DELAY_S;
  schedule(self);
  if (stats.n_expired + stats.n_retention > 0)
    g_signal_emit(self, signals[SIGNAL_PURGED], 0, rumor_ids, stats.n_outbox);
  return TRUE;
}

static gboolean
on_timer(gpointer data)
{
  GhExpiry *self = data;
  self->timer = 0;
  const gint64 now = now_unix(self);
  g_object_ref(self); /* a "purged" handler may drop the last other reference */
  if ((self->purge_at && self->purge_at <= now) ||
      (self->expires_at && self->expires_at <= now) ||
      (self->retention_at && self->retention_at <= now)) {
    g_autoptr(GError) error = NULL;
    if (!run_purge(self, &error))
      g_warning("Groundhog could not delete expired messages: %s", error->message);
  } else if (self->checkpoint_at && self->checkpoint_at <= now) {
    g_autoptr(GError) error = NULL;
    self->checkpoint_at = 0;
    /* BUSY (a reader) or a transaction in progress: try again shortly. */
    if (!gh_store_checkpoint(self->store, &error))
      self->checkpoint_at = now + CHECKPOINT_DELAY_S;
    schedule(self);
  } else {
    schedule(self); /* a capped wait, or the wall clock moved back */
  }
  g_object_unref(self);
  return G_SOURCE_REMOVE;
}

/* The store stored a message expiring at expires_at (T-admit, T-enqueue). */
static void
on_stored_expiry(gint64 expires_at, gpointer data)
{
  g_autoptr(GhExpiry) self = g_weak_ref_get(data);
  if (!self || !self->store || (self->expires_at && self->expires_at <= expires_at))
    return;
  self->expires_at = expires_at;
  schedule(self);
}

static void
notify_ref_free(gpointer data)
{
  g_weak_ref_clear(data);
  g_free(data);
}

/* ---- public ---------------------------------------------------------------------- */

GhExpiry *
gh_expiry_new(const GhExpiryConfig *config)
{
  g_return_val_if_fail(config != NULL && config->store != NULL, NULL);
  g_return_val_if_fail(!config->conversations ||
                       GH_IS_STORE_CONVERSATIONS(config->conversations), NULL);
  GhExpiry *self = g_object_new(GH_TYPE_EXPIRY, NULL);
  self->store = config->store;
  self->clock = gh_clock_ref(gh_store_get_clock(config->store));
  self->account = g_strdup(gh_store_get_account_pubkey(config->store));
  self->conversations = config->conversations ? g_object_ref(config->conversations) : NULL;
  self->retention_days = CLAMP(config->retention_days, 0, GH_EXPIRY_MAX_RETENTION_DAYS);
  gh_expiry_set_default_timer(self, config->default_timer);
  self->notify_ref = g_new0(GWeakRef, 1);
  g_weak_ref_init(self->notify_ref, self);
  gh_store_set_expiry_notify(self->store, on_stored_expiry, self->notify_ref, notify_ref_free);
  /* At open: expired messages were not restored, but the store still holds
   * them, and retention was not applied at all. */
  self->purge_at = now_unix(self);
  schedule(self);
  return self;
}

/* After dispose the store may be closed: nothing reaches it any more. */
static gboolean
check_live(GhExpiry *self, GError **error)
{
  if (self->store)
    return TRUE;
  g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_STATE, "The store is closed");
  return FALSE;
}

gboolean
gh_expiry_purge(GhExpiry *self, GError **error)
{
  g_return_val_if_fail(GH_IS_EXPIRY(self), FALSE);
  if (!check_live(self, error))
    return FALSE;
  g_autoptr(GhExpiry) hold = g_object_ref(self);
  return run_purge(self, error);
}

gint64
gh_expiry_get_next_run(GhExpiry *self)
{
  g_return_val_if_fail(GH_IS_EXPIRY(self), 0);
  return self->store ? next_run(self) : 0;
}

void
gh_expiry_set_retention_days(GhExpiry *self, gint days)
{
  g_return_if_fail(GH_IS_EXPIRY(self));
  days = CLAMP(days, 0, GH_EXPIRY_MAX_RETENTION_DAYS);
  if (days == self->retention_days)
    return;
  gboolean sooner = days > 0 && (self->retention_days == 0 || days < self->retention_days);
  self->retention_days = days;
  if (!self->store)
    return;
  if (sooner) {
    self->purge_at = now_unix(self);
  } else if (days == 0) {
    self->retention_at = 0;
  }
  schedule(self);
}

gint
gh_expiry_get_retention_days(GhExpiry *self)
{
  g_return_val_if_fail(GH_IS_EXPIRY(self), 0);
  return self->retention_days;
}

void
gh_expiry_set_default_timer(GhExpiry *self, gint64 seconds)
{
  g_return_if_fail(GH_IS_EXPIRY(self));
  self->default_timer = gh_expiry_timer_is_valid(seconds) ? seconds : GH_EXPIRY_TIMER_OFF;
  if (self->store)
    gh_store_set_default_disappearing(self->store, self->default_timer);
}

/* A canonical NIP-17 room id of the account: sorted, unique lowercase hex
 * pubkeys joined by ',', one of them the account. */
static gboolean
check_room(GhExpiry *self, const gchar *room_id, GError **error)
{
  g_auto(GStrv) members = room_id ? g_strsplit(room_id, ",", GH_STORE_MAX_PARTICIPANTS + 1)
                                  : NULL;
  guint n = members ? g_strv_length(members) : 0;
  gboolean ok = n > 0 && n <= GH_STORE_MAX_PARTICIPANTS;
  gboolean has_account = FALSE;
  for (guint i = 0; ok && i < n; i++) {
    ok = strlen(members[i]) == 64 && (i == 0 || strcmp(members[i - 1], members[i]) < 0);
    for (const gchar *c = members[i]; ok && *c; c++)
      ok = g_ascii_isdigit(*c) || (*c >= 'a' && *c <= 'f');
    has_account = has_account || g_strcmp0(members[i], self->account) == 0;
  }
  if (ok && has_account)
    return TRUE;
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                      "Not a NIP-17 conversation of this account");
  return FALSE;
}

gboolean
gh_expiry_get_timer(GhExpiry *self, const gchar *room_id, gint64 *out_seconds, GError **error)
{
  g_return_val_if_fail(GH_IS_EXPIRY(self), FALSE);
  g_return_val_if_fail(out_seconds != NULL, FALSE);
  *out_seconds = 0;
  if (!check_live(self, error) || !check_room(self, room_id, error))
    return FALSE;
  g_autoptr(GError) local = NULL;
  gint64 id = 0;
  if (!gh_store_find_conversation(self->store, GH_STORE_BACKEND_NIP17, room_id, &id, &local)) {
    if (!g_error_matches(local, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND)) {
      g_propagate_error(error, g_steal_pointer(&local));
      return FALSE;
    }
    *out_seconds = self->default_timer;
    return TRUE;
  }
  return gh_store_get_disappearing(self->store, id, out_seconds, error);
}

gboolean
gh_expiry_set_timer(GhExpiry *self, const gchar *room_id, gint64 seconds, GError **error)
{
  g_return_val_if_fail(GH_IS_EXPIRY(self), FALSE);
  if (!check_live(self, error) || !check_room(self, room_id, error))
    return FALSE;
  if (!gh_expiry_timer_is_valid(seconds)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Messages disappear after a day, a week or four weeks, or never");
    return FALSE;
  }
  gint64 id = 0, current = -1;
  if (!gh_store_begin(self->store, error))
    return FALSE;
  /* Choosing a timer for someone is the account's own intent, like a draft. */
  if (!gh_store_ensure_conversation(self->store, GH_STORE_BACKEND_NIP17, room_id,
                                    GH_STORE_REQUEST_ACCEPTED, &id, error) ||
      !gh_store_get_disappearing(self->store, id, &current, error) ||
      !gh_store_set_disappearing(self->store, id, seconds, error)) {
    gh_store_rollback(self->store);
    return FALSE;
  }
  if (!gh_store_commit(self->store, error))
    return FALSE;
  if (current != seconds)
    g_signal_emit(self, signals[SIGNAL_TIMER_CHANGED], 0, room_id, seconds);
  return TRUE;
}

/* ---- GObject ------------------------------------------------------------------- */

static void
gh_expiry_dispose(GObject *object)
{
  GhExpiry *self = GH_EXPIRY(object);
  /* The store may already be closed: only the clock is touched here. The
   * store's notification data is a weak reference that now reads NULL. */
  if (self->timer) {
    gh_clock_source_remove(self->clock, self->timer);
    self->timer = 0;
  }
  self->store = NULL;
  self->notify_ref = NULL;
  g_clear_object(&self->conversations);
  G_OBJECT_CLASS(gh_expiry_parent_class)->dispose(object);
}

static void
gh_expiry_finalize(GObject *object)
{
  GhExpiry *self = GH_EXPIRY(object);
  g_clear_pointer(&self->clock, gh_clock_unref);
  g_free(self->account);
  G_OBJECT_CLASS(gh_expiry_parent_class)->finalize(object);
}

static void
gh_expiry_class_init(GhExpiryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gh_expiry_dispose;
  object_class->finalize = gh_expiry_finalize;
  signals[SIGNAL_PURGED] = g_signal_new("purged", G_TYPE_FROM_CLASS(klass),
                                        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                        G_TYPE_NONE, 2, G_TYPE_STRV, G_TYPE_UINT);
  signals[SIGNAL_TIMER_CHANGED] = g_signal_new("timer-changed", G_TYPE_FROM_CLASS(klass),
                                               G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                               G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_INT64);
}

static void
gh_expiry_init(GhExpiry *self)
{
  (void)self;
}
