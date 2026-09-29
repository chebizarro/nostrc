#ifndef GH_EXPIRY_H
#define GH_EXPIRY_H

#include "gh-nip17-envelope.h"
#include "gh-store.h"

G_BEGIN_DECLS

/* gh-store-conversations.h, not included here: it brings the conversation
 * model's GhMessageStatus, which gh-outbox.h's cannot meet in one
 * translation unit (see gh-app-outbox.h), and the outbox uses this file. */
struct _GhStoreConversations;

/*
 * Disappearing messages and retention (privacy charter §3.7, D10, item G07).
 * GTK-free; main context only.
 *
 * Outgoing. A conversation's timer (off, 1 day, 1 week or 4 weeks, kept in
 * the encrypted store) makes each of the account's messages there expire at
 * send + timer. That exact time travels only inside the encryption: the
 * rumor carries it (NIP-40) and the store keeps it for the local purge. Each
 * seal and gift wrap around it carries its own later value,
 *   ceil_hour(expiration + U(0, min(timer, 24 h))),
 * drawn independently per layer from the GhClock's CSPRNG. An exact outer
 * value would give away the send time (expiration - timer) that the random
 * created_at hides, and equal values would tie a message's layers together
 * (PT-7: each is in [send + D, send + D + min(D, 24 h) + 3600] and a whole
 * hour). The self-copy is still sent (multi-device), with values of its own.
 * NIP-17 only asks for the wrap's expiration and the seal's "in case it
 * leaks"; neither needs to be exact, and receivers take the rumor's.
 *
 * Incoming. The unwrap's expiration (rumor, else seal, else wrap) is stored
 * with the message; one already expired on arrival is recorded as seen only
 * and never shown (T-admit, EX-4).
 *
 * Purge. A GhExpiry runs the store's T-purge (with the attached conversation
 * model kept in step, gh_store_conversations_purge()) when asked at open,
 * at the earliest stored expiry (one GhClock timer, moved earlier whenever a
 * sooner-expiring message is stored), and every day while a retention
 * period is set: messages received more than that many days ago go too
 * (D10: keep forever by default; 30 days or a year). Every wait is capped
 * so a wall-clock jump or a suspend is noticed within minutes. "purged"
 * names the NIP-17 messages that went, so notifications can be withdrawn
 * and the outbox can let go of their entries. This is the one expiry timer
 * of the application: the conversation model (and so every view of it)
 * never holds an expired message.
 */

/* The disappearing timers a conversation can have, in seconds. */
#define GH_EXPIRY_TIMER_OFF        G_GINT64_CONSTANT(0)
#define GH_EXPIRY_TIMER_DAY        G_GINT64_CONSTANT(86400)
#define GH_EXPIRY_TIMER_WEEK       (7 * GH_EXPIRY_TIMER_DAY)
#define GH_EXPIRY_TIMER_FOUR_WEEKS (28 * GH_EXPIRY_TIMER_DAY)
/* The widest spread of an outer expiration past the message's own. */
#define GH_EXPIRY_MAX_JITTER       GH_EXPIRY_TIMER_DAY
/* The longest retention period honoured (a larger setting is capped). */
#define GH_EXPIRY_MAX_RETENTION_DAYS 36500

/* Whether @seconds is one of the timers above. */
gboolean gh_expiry_timer_is_valid(gint64 seconds);

/* The exact expiration of a message sent at @sent_at (unix seconds, > 0)
 * with @timer (>= 0): sent_at + timer, or 0 when the timer is off. */
gint64 gh_expiry_message_expiration(gint64 sent_at, gint64 timer);
/* min(expires_at - sent_at, GH_EXPIRY_MAX_JITTER); expires_at > sent_at. */
gint64 gh_expiry_outer_jitter_max(gint64 sent_at, gint64 expires_at);
/* ceil_hour(expires_at + jitter), with jitter clamped to
 * [0, gh_expiry_outer_jitter_max()]: never before @expires_at, a whole
 * hour, at most expires_at + min(D, 24 h) + 3599. */
gint64 gh_expiry_outer_expiration(gint64 sent_at, gint64 expires_at, gint64 jitter);
/* Draws every seal's and wrap's expiration of a message sent at @sent_at
 * that expires at @expires_at, each with its own jitter from @clock. FALSE
 * (nothing drawn) unless 0 < sent_at < expires_at. */
gboolean gh_expiry_draw_outer(GhClock *clock, gint64 sent_at, gint64 expires_at,
                              GhNip17OuterExpiration *out);
/* The same for a message to @n_recipients people (W17; 0: a note to self,
 * at most GH_NIP17_MAX_SEND_RECIPIENTS): every recipient's seal and wrap and
 * the self-copy's, each drawn independently. */
gboolean gh_expiry_draw_room(GhClock *clock, gint64 sent_at, gint64 expires_at,
                             guint n_recipients, GhNip17RoomExpiration *out);

/* Retention: messages received before the returned cutoff are purged; 0
 * (keep everything) when @days is 0 or less. */
gint64 gh_expiry_retention_cutoff(gint64 now, gint days);

#define GH_TYPE_EXPIRY (gh_expiry_get_type())
G_DECLARE_FINAL_TYPE(GhExpiry, gh_expiry, GH, EXPIRY, GObject)

typedef struct {
  /* Required; borrowed. A read-only (damaged) store deletes nothing: its
   * expired messages only leave the model. Dispose the GhExpiry when the store
   * closes, before its account's next store opens; it never uses the store
   * after its dispose, so that may come right after the close. */
  GhStore *store;
  /* Nullable: the GhStoreConversations whose attached model follows every
   * purge (gh_store_conversations_purge()). */
  struct _GhStoreConversations *conversations;
  gint retention_days;   /* 0 keeps messages (default); capped */
  gint64 default_timer;  /* timer of conversations the account starts from now on */
} GhExpiryConfig;

/* Takes over the store's expiry notification and its default timer, and
 * schedules the purge at open on @store's clock. Signals:
 *  - "purged" (GStrv rumor_ids, guint n_outbox) after a purge that deleted
 *    anything: the purged NIP-17 rumor ids (maybe empty) and how many outbox
 *    entries went with them;
 *  - "timer-changed" (const gchar *room_id, gint64 seconds). */
GhExpiry *gh_expiry_new(const GhExpiryConfig *config);

/* Purges now (e.g. at open, after connecting to "purged") and schedules the
 * next run. On failure the next attempt comes a minute later. */
gboolean gh_expiry_purge(GhExpiry *self, GError **error);
/* Unix seconds of the next scheduled purge or check, 0 when none. */
gint64 gh_expiry_get_next_run(GhExpiry *self);

/* Retention period in days (0: keep). A new or shorter period purges now. */
void gh_expiry_set_retention_days(GhExpiry *self, gint days);
gint gh_expiry_get_retention_days(GhExpiry *self);
/* The timer conversations the account starts from now on begin with (a
 * valid timer; anything else turns it off). One someone else starts (a
 * message request) begins with the timer off (gh_store_set_default_
 * disappearing(); nostrc-qp24.83). */
void gh_expiry_set_default_timer(GhExpiry *self, gint64 seconds);

/* The timer of the NIP-17 room @room_id (a canonical room id of the store's
 * account, gh_message_get_room_id()); a room not stored yet reports the
 * default it would get when the account starts it. */
gboolean gh_expiry_get_timer(GhExpiry *self, const gchar *room_id, gint64 *out_seconds,
                             GError **error);
/* Sets it (gh_expiry_timer_is_valid(), else G_IO_ERROR_INVALID_ARGUMENT);
 * a room not stored yet is created, as accepted. It applies to messages the
 * account sends from now on; received ones keep their sender's expiration.
 * A change is recorded with its time and shown as the local timeline row
 * "You set messages to disappear after 1 day" (the attached model's
 * gh_conversation_get_timer_change(), charter §3.7; never published). */
gboolean gh_expiry_set_timer(GhExpiry *self, const gchar *room_id, gint64 seconds,
                             GError **error);

G_END_DECLS
#endif
