/*
 * badge_manager.c - Notification badge system implementation
 *
 * Tracks unread notifications and updates system tray badges.
 * Uses GSettings for persistence and integrates with nostrdb subscriptions.
 */

#define G_LOG_DOMAIN "badge-manager"

#include "badge_manager.h"
#include "../model/gn-ndb-sub-dispatcher.h"
#include "../storage_ndb.h"
#include "../util/zap.h"

#include <stdio.h>
#include <string.h>

/* Nostr event kinds for notifications */
#define KIND_LEGACY_DM        4
#define KIND_TEXT_NOTE        1
#define KIND_REPOST           6
#define KIND_REACTION         7
#define KIND_NIP17_RUMOR      14
#define KIND_ZAP_RECEIPT      9735
#define KIND_GIFT_WRAP        1059

/* NIP-51 list event kinds */
#define KIND_MUTE_LIST        10000
#define KIND_PIN_LIST         10001
#define KIND_PEOPLE_LIST      30000
#define KIND_BOOKMARK_LIST    30001

/* GSettings schema IDs */
#define GSETTINGS_NOTIFICATIONS_SCHEMA "org.gnostr.Notifications"
#define GSETTINGS_NOTIFICATIONS_PATH   "/org/gnostr/notifications/"

struct _GnostrBadgeManager {
  GObject parent_instance;

  /* Configuration */
  gchar *user_pubkey;
  gboolean enabled[GNOSTR_NOTIFICATION_TYPE_COUNT];
  GnostrBadgeDisplayMode display_mode;

  /* Counts (in-memory) */
  guint counts[GNOSTR_NOTIFICATION_TYPE_COUNT];

  /* Last-read timestamps */
  gint64 last_read[GNOSTR_NOTIFICATION_TYPE_COUNT];

  /* Change callback */
  GnostrBadgeChangedCallback callback;
  gpointer callback_data;
  GDestroyNotify callback_destroy;

  /* Event callback (for desktop notifications) */
  GnostrNotificationEventCallback event_callback;
  gpointer event_callback_data;
  GDestroyNotify event_callback_destroy;

  /* Subscription IDs */
  uint64_t sub_dm;
  uint64_t sub_mentions;
  uint64_t sub_zaps;
  uint64_t sub_reposts;
  uint64_t sub_reactions;
  uint64_t sub_lists;

  /* GSettings for persistence */
  GSettings *settings;
};

G_DEFINE_TYPE(GnostrBadgeManager, gnostr_badge_manager, G_TYPE_OBJECT)

static GnostrBadgeManager *g_default_manager = NULL;

/* Forward declarations */
static void on_dm_events(uint64_t subid, const uint64_t *note_keys,
                         guint n_keys, gpointer user_data);
static void on_mention_events(uint64_t subid, const uint64_t *note_keys,
                              guint n_keys, gpointer user_data);
static void on_zap_events(uint64_t subid, const uint64_t *note_keys,
                          guint n_keys, gpointer user_data);
static void on_repost_events(uint64_t subid, const uint64_t *note_keys,
                             guint n_keys, gpointer user_data);
static void on_reaction_events(uint64_t subid, const uint64_t *note_keys,
                               guint n_keys, gpointer user_data);
static void on_list_events(uint64_t subid, const uint64_t *note_keys,
                           guint n_keys, gpointer user_data);
static void emit_changed(GnostrBadgeManager *self);
static void load_settings(GnostrBadgeManager *self);
static void save_settings(GnostrBadgeManager *self);

/* ============== GObject Implementation ============== */

static void
gnostr_badge_manager_dispose(GObject *object)
{
  GnostrBadgeManager *self = GNOSTR_BADGE_MANAGER(object);

  gnostr_badge_manager_stop_subscriptions(self);

  if (self->callback_destroy && self->callback_data) {
    self->callback_destroy(self->callback_data);
  }
  self->callback = NULL;
  self->callback_data = NULL;
  self->callback_destroy = NULL;

  if (self->event_callback_destroy && self->event_callback_data) {
    self->event_callback_destroy(self->event_callback_data);
  }
  self->event_callback = NULL;
  self->event_callback_data = NULL;
  self->event_callback_destroy = NULL;

  g_clear_object(&self->settings);

  G_OBJECT_CLASS(gnostr_badge_manager_parent_class)->dispose(object);
}

static void
gnostr_badge_manager_finalize(GObject *object)
{
  GnostrBadgeManager *self = GNOSTR_BADGE_MANAGER(object);

  g_free(self->user_pubkey);

  G_OBJECT_CLASS(gnostr_badge_manager_parent_class)->finalize(object);
}

static void
gnostr_badge_manager_class_init(GnostrBadgeManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = gnostr_badge_manager_dispose;
  object_class->finalize = gnostr_badge_manager_finalize;
}

static void
gnostr_badge_manager_init(GnostrBadgeManager *self)
{
  /* Default: all notification types enabled */
  for (int i = 0; i < GNOSTR_NOTIFICATION_TYPE_COUNT; i++) {
    self->enabled[i] = TRUE;
    self->counts[i] = 0;
    self->last_read[i] = 0;
  }

  self->display_mode = GNOSTR_BADGE_DISPLAY_COUNT;
  self->user_pubkey = NULL;
  self->callback = NULL;
  self->callback_data = NULL;
  self->callback_destroy = NULL;
  self->event_callback = NULL;
  self->event_callback_data = NULL;
  self->event_callback_destroy = NULL;
  self->sub_dm = 0;
  self->sub_mentions = 0;
  self->sub_zaps = 0;
  self->sub_reposts = 0;
  self->settings = NULL;

  /* Try to load settings */
  load_settings(self);
}

/* ============== Settings Persistence ============== */

static void
load_settings(GnostrBadgeManager *self)
{
  GSettingsSchemaSource *source = g_settings_schema_source_get_default();
  if (!source) return;

  GSettingsSchema *schema = g_settings_schema_source_lookup(
    source, GSETTINGS_NOTIFICATIONS_SCHEMA, TRUE);
  if (!schema) {
    g_debug("Notifications schema not found, using defaults");
    return;
  }
  g_settings_schema_unref(schema);

  self->settings = g_settings_new(GSETTINGS_NOTIFICATIONS_SCHEMA);
  if (!self->settings) return;

  /* Load enabled states */
  self->enabled[GNOSTR_NOTIFICATION_DM] =
    g_settings_get_boolean(self->settings, "badge-dm-enabled");
  self->enabled[GNOSTR_NOTIFICATION_MENTION] =
    g_settings_get_boolean(self->settings, "badge-mention-enabled");
  self->enabled[GNOSTR_NOTIFICATION_REPLY] =
    g_settings_get_boolean(self->settings, "badge-reply-enabled");
  self->enabled[GNOSTR_NOTIFICATION_ZAP] =
    g_settings_get_boolean(self->settings, "badge-zap-enabled");
  self->enabled[GNOSTR_NOTIFICATION_REPOST] =
    g_settings_get_boolean(self->settings, "badge-repost-enabled");
  self->enabled[GNOSTR_NOTIFICATION_REACTION] =
    g_settings_get_boolean(self->settings, "badge-reaction-enabled");
  /* nostrc-51a.6: List notifications - try to load if key exists, else default to enabled */
  self->enabled[GNOSTR_NOTIFICATION_LIST] = TRUE; /* Default until schema is updated */

  /* Load display mode */
  gchar *mode_str = g_settings_get_string(self->settings, "badge-display-mode");
  if (g_strcmp0(mode_str, "dot") == 0) {
    self->display_mode = GNOSTR_BADGE_DISPLAY_DOT;
  } else if (g_strcmp0(mode_str, "none") == 0) {
    self->display_mode = GNOSTR_BADGE_DISPLAY_NONE;
  } else {
    self->display_mode = GNOSTR_BADGE_DISPLAY_COUNT;
  }
  g_free(mode_str);

  /* Load last-read timestamps */
  self->last_read[GNOSTR_NOTIFICATION_DM] =
    g_settings_get_int64(self->settings, "last-read-dm");
  self->last_read[GNOSTR_NOTIFICATION_MENTION] =
    g_settings_get_int64(self->settings, "last-read-mention");
  self->last_read[GNOSTR_NOTIFICATION_REPLY] =
    g_settings_get_int64(self->settings, "last-read-reply");
  self->last_read[GNOSTR_NOTIFICATION_ZAP] =
    g_settings_get_int64(self->settings, "last-read-zap");
  self->last_read[GNOSTR_NOTIFICATION_REPOST] =
    g_settings_get_int64(self->settings, "last-read-repost");
  self->last_read[GNOSTR_NOTIFICATION_REACTION] =
    g_settings_get_int64(self->settings, "last-read-reaction");
  self->last_read[GNOSTR_NOTIFICATION_LIST] = 0; /* Default until schema is updated */

  g_debug("Loaded settings: dm=%d mention=%d reply=%d zap=%d repost=%d reaction=%d list=%d mode=%d",
          self->enabled[0], self->enabled[1], self->enabled[2], self->enabled[3],
          self->enabled[4], self->enabled[5], self->enabled[6], self->display_mode);
}

static void
save_settings(GnostrBadgeManager *self)
{
  if (!self->settings) return;

  g_settings_set_boolean(self->settings, "badge-dm-enabled",
                          self->enabled[GNOSTR_NOTIFICATION_DM]);
  g_settings_set_boolean(self->settings, "badge-mention-enabled",
                          self->enabled[GNOSTR_NOTIFICATION_MENTION]);
  g_settings_set_boolean(self->settings, "badge-reply-enabled",
                          self->enabled[GNOSTR_NOTIFICATION_REPLY]);
  g_settings_set_boolean(self->settings, "badge-zap-enabled",
                          self->enabled[GNOSTR_NOTIFICATION_ZAP]);
  g_settings_set_boolean(self->settings, "badge-repost-enabled",
                          self->enabled[GNOSTR_NOTIFICATION_REPOST]);
  g_settings_set_boolean(self->settings, "badge-reaction-enabled",
                          self->enabled[GNOSTR_NOTIFICATION_REACTION]);

  const char *mode_str = "count";
  switch (self->display_mode) {
    case GNOSTR_BADGE_DISPLAY_DOT: mode_str = "dot"; break;
    case GNOSTR_BADGE_DISPLAY_NONE: mode_str = "none"; break;
    default: break;
  }
  g_settings_set_string(self->settings, "badge-display-mode", mode_str);

  g_settings_set_int64(self->settings, "last-read-dm",
                        self->last_read[GNOSTR_NOTIFICATION_DM]);
  g_settings_set_int64(self->settings, "last-read-mention",
                        self->last_read[GNOSTR_NOTIFICATION_MENTION]);
  g_settings_set_int64(self->settings, "last-read-reply",
                        self->last_read[GNOSTR_NOTIFICATION_REPLY]);
  g_settings_set_int64(self->settings, "last-read-zap",
                        self->last_read[GNOSTR_NOTIFICATION_ZAP]);
  g_settings_set_int64(self->settings, "last-read-repost",
                        self->last_read[GNOSTR_NOTIFICATION_REPOST]);
  g_settings_set_int64(self->settings, "last-read-reaction",
                        self->last_read[GNOSTR_NOTIFICATION_REACTION]);
}

/* ============== Lifecycle ============== */

GnostrBadgeManager *
gnostr_badge_manager_get_default(void)
{
  if (!g_default_manager) {
    g_default_manager = gnostr_badge_manager_new();
  }
  return g_default_manager;
}

GnostrBadgeManager *
gnostr_badge_manager_new(void)
{
  return g_object_new(GNOSTR_TYPE_BADGE_MANAGER, NULL);
}

/* ============== Configuration ============== */

void
gnostr_badge_manager_set_user_pubkey(GnostrBadgeManager *self,
                                      const char *pubkey_hex)
{
  g_return_if_fail(GNOSTR_IS_BADGE_MANAGER(self));

  g_free(self->user_pubkey);
  self->user_pubkey = g_strdup(pubkey_hex);

  g_debug("User pubkey set: %s", pubkey_hex ? pubkey_hex : "(null)");
}

void
gnostr_badge_manager_set_notification_enabled(GnostrBadgeManager *self,
                                               GnostrNotificationType type,
                                               gboolean enabled)
{
  g_return_if_fail(GNOSTR_IS_BADGE_MANAGER(self));
  g_return_if_fail(type < GNOSTR_NOTIFICATION_TYPE_COUNT);

  if (self->enabled[type] == enabled) return;

  self->enabled[type] = enabled;
  save_settings(self);
  emit_changed(self);
}

gboolean
gnostr_badge_manager_get_notification_enabled(GnostrBadgeManager *self,
                                               GnostrNotificationType type)
{
  g_return_val_if_fail(GNOSTR_IS_BADGE_MANAGER(self), FALSE);
  g_return_val_if_fail(type < GNOSTR_NOTIFICATION_TYPE_COUNT, FALSE);

  return self->enabled[type];
}

void
gnostr_badge_manager_set_display_mode(GnostrBadgeManager *self,
                                       GnostrBadgeDisplayMode mode)
{
  g_return_if_fail(GNOSTR_IS_BADGE_MANAGER(self));

  if (self->display_mode == mode) return;

  self->display_mode = mode;
  save_settings(self);
  emit_changed(self);
}

GnostrBadgeDisplayMode
gnostr_badge_manager_get_display_mode(GnostrBadgeManager *self)
{
  g_return_val_if_fail(GNOSTR_IS_BADGE_MANAGER(self), GNOSTR_BADGE_DISPLAY_NONE);
  return self->display_mode;
}

/* ============== Count Management ============== */

void
gnostr_badge_manager_increment(GnostrBadgeManager *self,
                                GnostrNotificationType type,
                                guint count)
{
  g_return_if_fail(GNOSTR_IS_BADGE_MANAGER(self));
  g_return_if_fail(type < GNOSTR_NOTIFICATION_TYPE_COUNT);

  if (count == 0) return;

  self->counts[type] += count;
  g_debug("Incremented %d by %u, now %u", type, count, self->counts[type]);
  emit_changed(self);
}

void
gnostr_badge_manager_clear(GnostrBadgeManager *self,
                            GnostrNotificationType type)
{
  g_return_if_fail(GNOSTR_IS_BADGE_MANAGER(self));
  g_return_if_fail(type < GNOSTR_NOTIFICATION_TYPE_COUNT);

  if (self->counts[type] == 0) return;

  self->counts[type] = 0;
  /* Update last-read to now */
  self->last_read[type] = g_get_real_time() / G_USEC_PER_SEC;
  save_settings(self);
  emit_changed(self);
}

void
gnostr_badge_manager_clear_all(GnostrBadgeManager *self)
{
  g_return_if_fail(GNOSTR_IS_BADGE_MANAGER(self));

  gboolean changed = FALSE;
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;

  for (int i = 0; i < GNOSTR_NOTIFICATION_TYPE_COUNT; i++) {
    if (self->counts[i] > 0) {
      self->counts[i] = 0;
      self->last_read[i] = now;
      changed = TRUE;
    }
  }

  if (changed) {
    save_settings(self);
    emit_changed(self);
  }
}

guint
gnostr_badge_manager_get_count(GnostrBadgeManager *self,
                                GnostrNotificationType type)
{
  g_return_val_if_fail(GNOSTR_IS_BADGE_MANAGER(self), 0);
  g_return_val_if_fail(type < GNOSTR_NOTIFICATION_TYPE_COUNT, 0);

  return self->counts[type];
}

guint
gnostr_badge_manager_get_total_count(GnostrBadgeManager *self)
{
  g_return_val_if_fail(GNOSTR_IS_BADGE_MANAGER(self), 0);

  guint total = 0;
  for (int i = 0; i < GNOSTR_NOTIFICATION_TYPE_COUNT; i++) {
    if (self->enabled[i]) {
      total += self->counts[i];
    }
  }
  return total;
}

/* ============== Timestamp Tracking ============== */

void
gnostr_badge_manager_set_last_read(GnostrBadgeManager *self,
                                    GnostrNotificationType type,
                                    gint64 timestamp)
{
  g_return_if_fail(GNOSTR_IS_BADGE_MANAGER(self));
  g_return_if_fail(type < GNOSTR_NOTIFICATION_TYPE_COUNT);

  self->last_read[type] = timestamp;
  save_settings(self);
}

gint64
gnostr_badge_manager_get_last_read(GnostrBadgeManager *self,
                                    GnostrNotificationType type)
{
  g_return_val_if_fail(GNOSTR_IS_BADGE_MANAGER(self), 0);
  g_return_val_if_fail(type < GNOSTR_NOTIFICATION_TYPE_COUNT, 0);

  return self->last_read[type];
}

/* ============== Callbacks ============== */

void
gnostr_badge_manager_set_changed_callback(GnostrBadgeManager *self,
                                           GnostrBadgeChangedCallback callback,
                                           gpointer user_data,
                                           GDestroyNotify destroy)
{
  g_return_if_fail(GNOSTR_IS_BADGE_MANAGER(self));

  if (self->callback_destroy && self->callback_data) {
    self->callback_destroy(self->callback_data);
  }

  self->callback = callback;
  self->callback_data = user_data;
  self->callback_destroy = destroy;
}

static void
emit_changed(GnostrBadgeManager *self)
{
  if (self->callback) {
    guint total = gnostr_badge_manager_get_total_count(self);
    self->callback(self, total, self->callback_data);
  }
}

void
gnostr_badge_manager_set_event_callback(GnostrBadgeManager *self,
                                         GnostrNotificationEventCallback callback,
                                         gpointer user_data,
                                         GDestroyNotify destroy)
{
  g_return_if_fail(GNOSTR_IS_BADGE_MANAGER(self));

  if (self->event_callback_destroy && self->event_callback_data) {
    self->event_callback_destroy(self->event_callback_data);
  }

  self->event_callback = callback;
  self->event_callback_data = user_data;
  self->event_callback_destroy = destroy;
}

static void
emit_event(GnostrBadgeManager *self,
           GnostrNotificationType type,
           const char *sender_pubkey,
           const char *sender_name,
           const char *content,
           const char *event_id,
           guint64 amount_sats)
{
  if (self->event_callback) {
    self->event_callback(self, type, sender_pubkey, sender_name,
                         content, event_id, amount_sats, self->event_callback_data);
  }
}

/* ============== Subscription Callbacks ============== */

static void
on_dm_events(uint64_t subid, const uint64_t *note_keys,
             guint n_keys, gpointer user_data)
{
  (void)subid;
  GnostrBadgeManager *self = GNOSTR_BADGE_MANAGER(user_data);

  if (!self->enabled[GNOSTR_NOTIFICATION_DM]) return;

  /* Get transaction to access notes */
  void *txn = NULL;
  if (storage_ndb_begin_query_retry(&txn, 3, 10) != 0 || !txn) {
    g_warning("Failed to begin query for DM notifications");
    return;
  }

  guint new_count = 0;
  gint64 last_read = self->last_read[GNOSTR_NOTIFICATION_DM];

  for (guint i = 0; i < n_keys; i++) {
    storage_ndb_note *note = storage_ndb_get_note_ptr(txn, note_keys[i]);
    if (!note) continue;

    gint64 created_at = (gint64)storage_ndb_note_created_at(note);
    if (created_at > last_read) {
      new_count++;

      /* Emit event for desktop notification (only for first new DM to avoid spam) */
      if (new_count == 1 && self->event_callback) {
        const unsigned char *pubkey_bin = storage_ndb_note_pubkey(note);
        const unsigned char *id_bin = storage_ndb_note_id(note);
        const char *content = storage_ndb_note_content(note);

        char pubkey_hex[65], id_hex[65];
        storage_ndb_hex_encode(pubkey_bin, pubkey_hex);
        storage_ndb_hex_encode(id_bin, id_hex);

        /* Note: sender_name would require profile lookup, pass NULL for now */
        emit_event(self, GNOSTR_NOTIFICATION_DM, pubkey_hex, NULL, content, id_hex, 0);
      }
    }
  }

  storage_ndb_end_query(txn);

  if (new_count > 0) {
    gnostr_badge_manager_increment(self, GNOSTR_NOTIFICATION_DM, new_count);
    g_debug("DM notification: +%u new", new_count);
  }
}

/* Check if a note is a reply to a note authored by the given user (NIP-10).
 * Returns TRUE if this note replies to one of user's notes.
 * Also returns the target note ID via target_id_out (caller must g_free).
 * Uses storage_ndb_note_get_nip10_thread for proper NIP-10 parsing. */
static gboolean
note_is_reply_to_user(void *txn, storage_ndb_note *note, const char *user_pubkey,
                      char **target_id_out)
{
  if (!note || !user_pubkey || strlen(user_pubkey) != 64) return FALSE;

  if (target_id_out) *target_id_out = NULL;

  /* Extract NIP-10 thread context - gets the note ID this note replies to */
  char *root_id = NULL;
  char *reply_id = NULL;
  storage_ndb_note_get_nip10_thread(note, &root_id, &reply_id);

  /* If no reply_id, check root_id (some clients only set root for direct replies) */
  char *target_id = reply_id ? reply_id : root_id;

  if (!target_id) {
    g_free(root_id);
    g_free(reply_id);
    return FALSE;
  }

  /* Convert target_id hex to binary for lookup */
  unsigned char target_id_bin[32];
  gboolean valid_hex = TRUE;
  for (int i = 0; i < 32 && valid_hex; i++) {
    unsigned int byte;
    if (sscanf(target_id + (i * 2), "%2x", &byte) != 1) {
      valid_hex = FALSE;
    } else {
      target_id_bin[i] = (unsigned char)byte;
    }
  }

  if (!valid_hex) {
    g_free(root_id);
    g_free(reply_id);
    return FALSE;
  }

  /* Look up the target note to check its author */
  storage_ndb_note *target_note = NULL;
  uint64_t target_key = storage_ndb_get_note_key_by_id(txn, target_id_bin, &target_note);

  gboolean is_reply_to_user = FALSE;
  if (target_key != 0 && target_note) {
    const unsigned char *target_author = storage_ndb_note_pubkey(target_note);
    if (target_author) {
      char target_author_hex[65];
      storage_ndb_hex_encode(target_author, target_author_hex);
      is_reply_to_user = (g_strcmp0(target_author_hex, user_pubkey) == 0);
    }
  }

  if (is_reply_to_user && target_id_out) {
    *target_id_out = g_strdup(target_id);
  }

  g_free(root_id);
  g_free(reply_id);
  return is_reply_to_user;
}

static void
on_mention_events(uint64_t subid, const uint64_t *note_keys,
                  guint n_keys, gpointer user_data)
{
  (void)subid;
  GnostrBadgeManager *self = GNOSTR_BADGE_MANAGER(user_data);

  /* Need user pubkey to distinguish replies to user's notes from general mentions */
  if (!self->user_pubkey || strlen(self->user_pubkey) != 64) {
    g_debug("No user pubkey set, cannot process mention/reply events");
    return;
  }

  /* Get transaction to access notes */
  void *txn = NULL;
  if (storage_ndb_begin_query_retry(&txn, 3, 10) != 0 || !txn) {
    g_warning("Failed to begin query for mention notifications");
    return;
  }

  guint mention_count = 0;
  guint reply_count = 0;
  gint64 mention_last = self->last_read[GNOSTR_NOTIFICATION_MENTION];
  gint64 reply_last = self->last_read[GNOSTR_NOTIFICATION_REPLY];
  gboolean mention_event_emitted = FALSE;
  gboolean reply_event_emitted = FALSE;

  for (guint i = 0; i < n_keys; i++) {
    storage_ndb_note *note = storage_ndb_get_note_ptr(txn, note_keys[i]);
    if (!note) continue;

    gint64 created_at = (gint64)storage_ndb_note_created_at(note);

    /* Check if this note is a reply to one of user's notes using NIP-10 thread context.
     * This properly handles both marker style and positional e-tags. */
    char *target_note_id = NULL;
    gboolean is_reply_to_user = note_is_reply_to_user(txn, note, self->user_pubkey, &target_note_id);

    if (is_reply_to_user && self->enabled[GNOSTR_NOTIFICATION_REPLY]) {
      if (created_at > reply_last) {
        reply_count++;

        /* Emit event for first reply only */
        if (!reply_event_emitted && self->event_callback) {
          const unsigned char *pubkey_bin = storage_ndb_note_pubkey(note);
          const unsigned char *id_bin = storage_ndb_note_id(note);
          const char *content = storage_ndb_note_content(note);

          char pubkey_hex[65], id_hex[65];
          storage_ndb_hex_encode(pubkey_bin, pubkey_hex);
          storage_ndb_hex_encode(id_bin, id_hex);

          emit_event(self, GNOSTR_NOTIFICATION_REPLY, pubkey_hex, NULL, content, id_hex, 0);
          reply_event_emitted = TRUE;

          g_debug("Reply to user's note detected: %.16s... replied to %.16s...",
                  pubkey_hex, target_note_id ? target_note_id : "unknown");
        }
      }
      g_free(target_note_id);
    } else if (self->enabled[GNOSTR_NOTIFICATION_MENTION]) {
      /* Not a reply to user's note, but user is p-tagged = mention */
      if (created_at > mention_last) {
        mention_count++;

        /* Emit event for first mention only */
        if (!mention_event_emitted && self->event_callback) {
          const unsigned char *pubkey_bin = storage_ndb_note_pubkey(note);
          const unsigned char *id_bin = storage_ndb_note_id(note);
          const char *content = storage_ndb_note_content(note);

          char pubkey_hex[65], id_hex[65];
          storage_ndb_hex_encode(pubkey_bin, pubkey_hex);
          storage_ndb_hex_encode(id_bin, id_hex);

          emit_event(self, GNOSTR_NOTIFICATION_MENTION, pubkey_hex, NULL, content, id_hex, 0);
          mention_event_emitted = TRUE;
        }
      }
    }
  }

  storage_ndb_end_query(txn);

  if (mention_count > 0) {
    gnostr_badge_manager_increment(self, GNOSTR_NOTIFICATION_MENTION, mention_count);
    g_debug("Mention notification: +%u new", mention_count);
  }

  if (reply_count > 0) {
    gnostr_badge_manager_increment(self, GNOSTR_NOTIFICATION_REPLY, reply_count);
    g_debug("Reply notification: +%u new", reply_count);
  }
}

static void
on_zap_events(uint64_t subid, const uint64_t *note_keys,
              guint n_keys, gpointer user_data)
{
  (void)subid;
  GnostrBadgeManager *self = GNOSTR_BADGE_MANAGER(user_data);

  if (!self->enabled[GNOSTR_NOTIFICATION_ZAP]) return;

  /* Get transaction to access notes */
  void *txn = NULL;
  if (storage_ndb_begin_query_retry(&txn, 3, 10) != 0 || !txn) {
    g_warning("Failed to begin query for zap notifications");
    return;
  }

  guint new_count = 0;
  gint64 last_read = self->last_read[GNOSTR_NOTIFICATION_ZAP];

  for (guint i = 0; i < n_keys; i++) {
    storage_ndb_note *note = storage_ndb_get_note_ptr(txn, note_keys[i]);
    if (!note) continue;

    gint64 created_at = (gint64)storage_ndb_note_created_at(note);
    if (created_at > last_read) {
      new_count++;

      /* Emit event for first zap only to avoid notification spam */
      if (new_count == 1 && self->event_callback) {
        const unsigned char *id_bin = storage_ndb_note_id(note);

        /* Get the note JSON to parse zap receipt details */
        char *note_json = NULL;
        int json_len = 0;
        int rc = storage_ndb_get_note_by_id(txn, id_bin, &note_json, &json_len);

        if (rc == 0 && note_json && json_len > 0) {
          /* Make a null-terminated copy for parsing */
          char *json_copy = g_strndup(note_json, json_len);
          GnostrZapReceipt *receipt = gnostr_zap_parse_receipt(json_copy);

          if (receipt) {
            /* Extract amount in sats from the parsed receipt */
            guint64 amount_sats = 0;
            if (receipt->amount_msat > 0) {
              amount_sats = (guint64)(receipt->amount_msat / 1000);
            }

            /* Use sender pubkey from receipt (P tag), fallback to receipt event pubkey */
            const char *sender = receipt->sender_pubkey ? receipt->sender_pubkey : receipt->event_pubkey;

            char id_hex[65];
            storage_ndb_hex_encode(id_bin, id_hex);

            /* Emit with actual amount and sender */
            emit_event(self, GNOSTR_NOTIFICATION_ZAP, sender, NULL, NULL, id_hex, amount_sats);

            g_debug("Zap notification: %llu sats from %.16s...",
                    (unsigned long long)amount_sats, sender ? sender : "unknown");

            gnostr_zap_receipt_free(receipt);
          } else {
            /* Fallback if parsing fails */
            char pubkey_hex[65], id_hex[65];
            storage_ndb_hex_encode(storage_ndb_note_pubkey(note), pubkey_hex);
            storage_ndb_hex_encode(id_bin, id_hex);
            emit_event(self, GNOSTR_NOTIFICATION_ZAP, pubkey_hex, NULL, NULL, id_hex, 0);
          }

          g_free(json_copy);
        } else {
          /* Fallback if JSON fetch fails */
          char pubkey_hex[65], id_hex[65];
          storage_ndb_hex_encode(storage_ndb_note_pubkey(note), pubkey_hex);
          storage_ndb_hex_encode(id_bin, id_hex);
          emit_event(self, GNOSTR_NOTIFICATION_ZAP, pubkey_hex, NULL, NULL, id_hex, 0);
        }
      }
    }
  }

  storage_ndb_end_query(txn);

  if (new_count > 0) {
    gnostr_badge_manager_increment(self, GNOSTR_NOTIFICATION_ZAP, new_count);
    g_debug("Zap notification: +%u new", new_count);
  }
}

static void
on_repost_events(uint64_t subid, const uint64_t *note_keys,
                 guint n_keys, gpointer user_data)
{
  (void)subid;
  GnostrBadgeManager *self = GNOSTR_BADGE_MANAGER(user_data);

  if (!self->enabled[GNOSTR_NOTIFICATION_REPOST]) return;

  /* Get transaction to access notes */
  void *txn = NULL;
  if (storage_ndb_begin_query_retry(&txn, 3, 10) != 0 || !txn) {
    g_warning("Failed to begin query for repost notifications");
    return;
  }

  guint new_count = 0;
  gint64 last_read = self->last_read[GNOSTR_NOTIFICATION_REPOST];

  for (guint i = 0; i < n_keys; i++) {
    storage_ndb_note *note = storage_ndb_get_note_ptr(txn, note_keys[i]);
    if (!note) continue;

    gint64 created_at = (gint64)storage_ndb_note_created_at(note);
    if (created_at > last_read) {
      new_count++;

      /* Emit event for first repost only to avoid notification spam */
      if (new_count == 1 && self->event_callback) {
        const unsigned char *pubkey_bin = storage_ndb_note_pubkey(note);
        const unsigned char *id_bin = storage_ndb_note_id(note);

        char pubkey_hex[65], id_hex[65];
        storage_ndb_hex_encode(pubkey_bin, pubkey_hex);
        storage_ndb_hex_encode(id_bin, id_hex);

        /* Emit repost notification - amount_sats is 0 for reposts */
        emit_event(self, GNOSTR_NOTIFICATION_REPOST, pubkey_hex, NULL, NULL, id_hex, 0);

        g_debug("Repost notification from %.16s...", pubkey_hex);
      }
    }
  }

  storage_ndb_end_query(txn);

  if (new_count > 0) {
    gnostr_badge_manager_increment(self, GNOSTR_NOTIFICATION_REPOST, new_count);
    g_debug("Repost notification: +%u new", new_count);
  }
}

static void
on_reaction_events(uint64_t subid, const uint64_t *note_keys,
                   guint n_keys, gpointer user_data)
{
  (void)subid;
  GnostrBadgeManager *self = GNOSTR_BADGE_MANAGER(user_data);

  if (!self->enabled[GNOSTR_NOTIFICATION_REACTION]) return;

  /* Get transaction to access notes */
  void *txn = NULL;
  if (storage_ndb_begin_query_retry(&txn, 3, 10) != 0 || !txn) {
    g_warning("Failed to begin query for reaction notifications");
    return;
  }

  guint new_count = 0;
  gint64 last_read = self->last_read[GNOSTR_NOTIFICATION_REACTION];

  for (guint i = 0; i < n_keys; i++) {
    storage_ndb_note *note = storage_ndb_get_note_ptr(txn, note_keys[i]);
    if (!note) continue;

    gint64 created_at = (gint64)storage_ndb_note_created_at(note);
    if (created_at > last_read) {
      new_count++;

      /* Emit event for first reaction only to avoid notification spam */
      if (new_count == 1 && self->event_callback) {
        const unsigned char *pubkey_bin = storage_ndb_note_pubkey(note);
        const unsigned char *id_bin = storage_ndb_note_id(note);
        const char *content = storage_ndb_note_content(note);

        char pubkey_hex[65], id_hex[65];
        storage_ndb_hex_encode(pubkey_bin, pubkey_hex);
        storage_ndb_hex_encode(id_bin, id_hex);

        /* Content is typically the reaction emoji ("+", "-", or custom emoji) */
        emit_event(self, GNOSTR_NOTIFICATION_REACTION, pubkey_hex, NULL, content, id_hex, 0);

        g_debug("Reaction notification from %.16s... content=%s", pubkey_hex, content ? content : "+");
      }
    }
  }

  storage_ndb_end_query(txn);

  if (new_count > 0) {
    gnostr_badge_manager_increment(self, GNOSTR_NOTIFICATION_REACTION, new_count);
    g_debug("Reaction notification: +%u new", new_count);
  }
}

/* nostrc-51a.6: Handle NIP-51 list events (mute, pin, people, bookmark lists) */
static void
on_list_events(uint64_t subid, const uint64_t *note_keys,
               guint n_keys, gpointer user_data)
{
  (void)subid;
  GnostrBadgeManager *self = GNOSTR_BADGE_MANAGER(user_data);

  if (!self->enabled[GNOSTR_NOTIFICATION_LIST]) return;

  /* Get transaction to access notes */
  void *txn = NULL;
  if (storage_ndb_begin_query_retry(&txn, 3, 10) != 0 || !txn) {
    g_warning("Failed to begin query for list notifications");
    return;
  }

  guint new_count = 0;
  gint64 last_read = self->last_read[GNOSTR_NOTIFICATION_LIST];

  for (guint i = 0; i < n_keys; i++) {
    storage_ndb_note *note = storage_ndb_get_note_ptr(txn, note_keys[i]);
    if (!note) continue;

    gint64 created_at = (gint64)storage_ndb_note_created_at(note);
    if (created_at > last_read) {
      new_count++;

      /* Emit event for first list addition only to avoid notification spam */
      if (new_count == 1 && self->event_callback) {
        const unsigned char *pubkey_bin = storage_ndb_note_pubkey(note);
        const unsigned char *id_bin = storage_ndb_note_id(note);
        uint32_t kind = storage_ndb_note_kind(note);

        char pubkey_hex[65], id_hex[65];
        storage_ndb_hex_encode(pubkey_bin, pubkey_hex);
        storage_ndb_hex_encode(id_bin, id_hex);

        /* Determine list type from kind for notification content */
        const char *list_type = "a list";
        switch (kind) {
          case KIND_MUTE_LIST:   list_type = "their mute list"; break;
          case KIND_PIN_LIST:    list_type = "their pinned list"; break;
          case KIND_PEOPLE_LIST: list_type = "a people list"; break;
          case KIND_BOOKMARK_LIST: list_type = "a bookmark list"; break;
        }

        /* Emit list notification - use content field for list type description */
        emit_event(self, GNOSTR_NOTIFICATION_LIST, pubkey_hex, NULL, list_type, id_hex, 0);

        g_debug("List notification: added to %s by %.16s...", list_type, pubkey_hex);
      }
    }
  }

  storage_ndb_end_query(txn);

  if (new_count > 0) {
    gnostr_badge_manager_increment(self, GNOSTR_NOTIFICATION_LIST, new_count);
    g_debug("List notification: +%u new", new_count);
  }
}

/* ============== Relay Subscription Integration ============== */

void
gnostr_badge_manager_start_subscriptions(GnostrBadgeManager *self)
{
  g_return_if_fail(GNOSTR_IS_BADGE_MANAGER(self));

  if (!self->user_pubkey || strlen(self->user_pubkey) != 64) {
    g_debug("Cannot start subscriptions: no valid user pubkey");
    return;
  }

  /* Stop existing subscriptions first */
  gnostr_badge_manager_stop_subscriptions(self);

  /* Subscribe to DMs (gift wraps addressed to user)
   * Kind 1059 (gift wrap) with #p tag matching user pubkey */
  gchar *dm_filter = g_strdup_printf(
    "[{\"kinds\":[%d],\"#p\":[\"%s\"]}]",
    KIND_GIFT_WRAP, self->user_pubkey);
  self->sub_dm = gn_ndb_subscribe(dm_filter, on_dm_events, self, NULL);
  g_free(dm_filter);

  /* Subscribe to mentions (text notes with #p tag matching user) */
  gchar *mention_filter = g_strdup_printf(
    "[{\"kinds\":[%d],\"#p\":[\"%s\"]}]",
    KIND_TEXT_NOTE, self->user_pubkey);
  self->sub_mentions = gn_ndb_subscribe(mention_filter, on_mention_events, self, NULL);
  g_free(mention_filter);

  /* Subscribe to zaps (zap receipts with #p tag matching user) */
  gchar *zap_filter = g_strdup_printf(
    "[{\"kinds\":[%d],\"#p\":[\"%s\"]}]",
    KIND_ZAP_RECEIPT, self->user_pubkey);
  self->sub_zaps = gn_ndb_subscribe(zap_filter, on_zap_events, self, NULL);
  g_free(zap_filter);

  /* Subscribe to reposts (kind 6 with #p tag matching user per NIP-18) */
  gchar *repost_filter = g_strdup_printf(
    "[{\"kinds\":[%d],\"#p\":[\"%s\"]}]",
    KIND_REPOST, self->user_pubkey);
  self->sub_reposts = gn_ndb_subscribe(repost_filter, on_repost_events, self, NULL);
  g_free(repost_filter);

  /* Subscribe to reactions (kind 7 with #p tag matching user per NIP-25) */
  gchar *reaction_filter = g_strdup_printf(
    "[{\"kinds\":[%d],\"#p\":[\"%s\"]}]",
    KIND_REACTION, self->user_pubkey);
  self->sub_reactions = gn_ndb_subscribe(reaction_filter, on_reaction_events, self, NULL);
  g_free(reaction_filter);

  /* nostrc-51a.6: Subscribe to NIP-51 lists that include user (mute, pin, people, bookmark) */
  gchar *list_filter = g_strdup_printf(
    "[{\"kinds\":[%d,%d,%d,%d],\"#p\":[\"%s\"]}]",
    KIND_MUTE_LIST, KIND_PIN_LIST, KIND_PEOPLE_LIST, KIND_BOOKMARK_LIST,
    self->user_pubkey);
  self->sub_lists = gn_ndb_subscribe(list_filter, on_list_events, self, NULL);
  g_free(list_filter);

  g_debug("Started notification subscriptions for %s (dm=%lu, mentions=%lu, zaps=%lu, reposts=%lu, reactions=%lu, lists=%lu)",
          self->user_pubkey, (unsigned long)self->sub_dm,
          (unsigned long)self->sub_mentions, (unsigned long)self->sub_zaps,
          (unsigned long)self->sub_reposts, (unsigned long)self->sub_reactions,
          (unsigned long)self->sub_lists);
}

void
gnostr_badge_manager_stop_subscriptions(GnostrBadgeManager *self)
{
  g_return_if_fail(GNOSTR_IS_BADGE_MANAGER(self));

  if (self->sub_dm) {
    gn_ndb_unsubscribe(self->sub_dm);
    self->sub_dm = 0;
  }

  if (self->sub_mentions) {
    gn_ndb_unsubscribe(self->sub_mentions);
    self->sub_mentions = 0;
  }

  if (self->sub_zaps) {
    gn_ndb_unsubscribe(self->sub_zaps);
    self->sub_zaps = 0;
  }

  if (self->sub_reposts) {
    gn_ndb_unsubscribe(self->sub_reposts);
    self->sub_reposts = 0;
  }

  if (self->sub_reactions) {
    gn_ndb_unsubscribe(self->sub_reactions);
    self->sub_reactions = 0;
  }

  if (self->sub_lists) {
    gn_ndb_unsubscribe(self->sub_lists);
    self->sub_lists = 0;
  }

  g_debug("Stopped notification subscriptions");
}

/* ============== Badge Formatting ============== */

gchar *
gnostr_badge_manager_format_count(guint count)
{
  if (count == 0) {
    return g_strdup("");
  } else if (count > 99) {
    return g_strdup("99+");
  } else {
    return g_strdup_printf("%u", count);
  }
}
