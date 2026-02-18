/*
 * badge_manager.c - Notification badge system implementation
 *
 * Tracks unread notifications and updates system tray badges.
 * Uses GSettings for persistence and integrates with nostrdb subscriptions.
 */

#define G_LOG_DOMAIN "badge-manager"

#include "badge_manager.h"
#include <nostr-gobject-1.0/gn-ndb-sub-dispatcher.h>
#include <nostr-gobject-1.0/storage_ndb.h>
#include "../util/zap.h"
#include "../ui/gnostr-notifications-view.h"
#include <nostr-gobject-1.0/nostr_json.h>

#include <stdio.h>
#include <string.h>

/* Nostr event kinds for notifications */
#define KIND_LEGACY_DM        4
#define KIND_TEXT_NOTE        1
#define KIND_COMMENT          1111  /* NIP-22 comments */
#define KIND_CONTACT_LIST     3     /* NIP-02 contact list (followers) */
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
  uint64_t sub_followers;

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
static void on_follower_events(uint64_t subid, const uint64_t *note_keys,
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
  /* nostrc-51a.5: Follower notifications - default to enabled */
  self->enabled[GNOSTR_NOTIFICATION_FOLLOWER] = TRUE; /* Default until schema is updated */

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
  self->last_read[GNOSTR_NOTIFICATION_FOLLOWER] = 0; /* Default until schema is updated */

  g_debug("Loaded settings: dm=%d mention=%d reply=%d zap=%d repost=%d reaction=%d list=%d follower=%d mode=%d",
          self->enabled[0], self->enabled[1], self->enabled[2], self->enabled[3],
          self->enabled[4], self->enabled[5], self->enabled[6], self->enabled[7], self->display_mode);
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
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) {
    g_debug("Failed to begin query for DM notifications");
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
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) {
    g_debug("Failed to begin query for mention notifications");
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
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) {
    g_debug("Failed to begin query for zap notifications");
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
        int rc = storage_ndb_get_note_by_id(txn, id_bin, &note_json, &json_len, NULL);

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
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) {
    g_debug("Failed to begin query for repost notifications");
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
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) {
    g_debug("Failed to begin query for reaction notifications");
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
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) {
    g_debug("Failed to begin query for list notifications");
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

/* nostrc-51a.5: Handle new follower events (kind 3 contact lists with user in p-tag).
 * When someone adds the user to their contact list, we detect it as a new follower. */
static void
on_follower_events(uint64_t subid, const uint64_t *note_keys,
                   guint n_keys, gpointer user_data)
{
  (void)subid;
  GnostrBadgeManager *self = GNOSTR_BADGE_MANAGER(user_data);

  if (!self->enabled[GNOSTR_NOTIFICATION_FOLLOWER]) return;

  /* Need user pubkey to verify we're in their contact list */
  if (!self->user_pubkey || strlen(self->user_pubkey) != 64) {
    g_debug("No user pubkey set, cannot process follower events");
    return;
  }

  /* Get transaction to access notes */
  void *txn = NULL;
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) {
    g_debug("Failed to begin query for follower notifications");
    return;
  }

  guint new_count = 0;
  gint64 last_read = self->last_read[GNOSTR_NOTIFICATION_FOLLOWER];
  gboolean event_emitted = FALSE;

  for (guint i = 0; i < n_keys; i++) {
    storage_ndb_note *note = storage_ndb_get_note_ptr(txn, note_keys[i]);
    if (!note) continue;

    /* Check if this is a kind 3 contact list */
    int kind = storage_ndb_note_kind(note);
    if (kind != KIND_CONTACT_LIST) continue;

    /* Check timestamp - only count new followers */
    gint64 created_at = storage_ndb_note_created_at(note);
    if (created_at <= last_read) continue;

    /* The fact that we received this event means the user is in the p-tags
     * (our subscription filter already filters for #p:[user_pubkey]).
     * This is a new follower! */
    new_count++;

    /* Emit event for first new follower only (to show desktop notification) */
    if (!event_emitted && self->event_callback) {
      const unsigned char *pubkey_bin = storage_ndb_note_pubkey(note);
      const unsigned char *id_bin = storage_ndb_note_id(note);

      char pubkey_hex[65], id_hex[65];
      storage_ndb_hex_encode(pubkey_bin, pubkey_hex);
      storage_ndb_hex_encode(id_bin, id_hex);

      emit_event(self, GNOSTR_NOTIFICATION_FOLLOWER, pubkey_hex, NULL,
                 "started following you", id_hex, 0);
      event_emitted = TRUE;

      g_debug("New follower: %.16s...", pubkey_hex);
    }
  }

  storage_ndb_end_query(txn);

  if (new_count > 0) {
    gnostr_badge_manager_increment(self, GNOSTR_NOTIFICATION_FOLLOWER, new_count);
    g_debug("Follower notification: +%u new", new_count);
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

  /* Subscribe to mentions (text notes and comments with #p tag matching user)
   * Kind 1 = text notes, kind 1111 = NIP-22 comments */
  gchar *mention_filter = g_strdup_printf(
    "[{\"kinds\":[%d,%d],\"#p\":[\"%s\"]}]",
    KIND_TEXT_NOTE, KIND_COMMENT, self->user_pubkey);
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

  /* nostrc-51a.5: Subscribe to new followers (kind 3 contact lists with user in p-tag) */
  gchar *follower_filter = g_strdup_printf(
    "[{\"kinds\":[%d],\"#p\":[\"%s\"]}]",
    KIND_CONTACT_LIST, self->user_pubkey);
  self->sub_followers = gn_ndb_subscribe(follower_filter, on_follower_events, self, NULL);
  g_free(follower_filter);

  g_debug("Started notification subscriptions for %s (dm=%lu, mentions=%lu, zaps=%lu, reposts=%lu, reactions=%lu, lists=%lu, followers=%lu)",
          self->user_pubkey, (unsigned long)self->sub_dm,
          (unsigned long)self->sub_mentions, (unsigned long)self->sub_zaps,
          (unsigned long)self->sub_reposts, (unsigned long)self->sub_reactions,
          (unsigned long)self->sub_lists, (unsigned long)self->sub_followers);
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

  if (self->sub_followers) {
    gn_ndb_unsubscribe(self->sub_followers);
    self->sub_followers = 0;
  }

  g_debug("Stopped notification subscriptions");
}

/* ============== History Loading (nostrc-27) ============== */

/* Data for async history loading GTask */
typedef struct {
  char *user_pubkey;
  gint64 last_read[GNOSTR_NOTIFICATION_TYPE_COUNT];
  GPtrArray *notifs;  /* result: array of GnostrNotification* */
  GnostrNotificationsView *view;
} HistoryLoadData;

static void
history_load_data_free(HistoryLoadData *data)
{
  if (!data) return;
  g_free(data->user_pubkey);
  if (data->notifs)
    g_ptr_array_unref(data->notifs);
  g_slice_free(HistoryLoadData, data);
}

/* Context for extracting last "e" tag value from tags array */
typedef struct {
  char *last_e_value;
} HistoryETagCtx;

static gboolean
history_extract_e_tag_cb(gsize idx, const gchar *element_json, gpointer user_data)
{
  (void)idx;
  HistoryETagCtx *ctx = user_data;
  if (!gnostr_json_is_array_str(element_json)) return TRUE;

  char *name = gnostr_json_get_array_string(element_json, NULL, 0, NULL);
  if (!name) return TRUE;

  if (g_strcmp0(name, "e") == 0) {
    g_free(ctx->last_e_value);
    ctx->last_e_value = gnostr_json_get_array_string(element_json, NULL, 1, NULL);
  }
  g_free(name);
  return TRUE;
}

/* Extract the last "e" tag value from event JSON. Returns newly allocated hex or NULL. */
static char *
history_extract_target_event_id(const char *event_json)
{
  char *tags_json = gnostr_json_get_raw(event_json, "tags", NULL);
  if (!tags_json) return NULL;

  HistoryETagCtx ctx = { .last_e_value = NULL };
  gnostr_json_array_foreach_root(tags_json, history_extract_e_tag_cb, &ctx);

  g_free(tags_json);
  return ctx.last_e_value;
}

/* Convert hex string to 32-byte binary. Returns TRUE on success. */
static gboolean
hex_to_bin32(const char *hex, unsigned char out[32])
{
  if (!hex || strlen(hex) != 64) return FALSE;
  for (int i = 0; i < 32; i++) {
    unsigned int byte;
    if (sscanf(hex + (i * 2), "%2x", &byte) != 1) return FALSE;
    out[i] = (unsigned char)byte;
  }
  return TRUE;
}

/* Sort comparator: ascending by created_at (oldest first for prepend ordering) */
static gint
notif_cmp_created_at_asc(gconstpointer a, gconstpointer b)
{
  const GnostrNotification *na = *(const GnostrNotification *const *)a;
  const GnostrNotification *nb = *(const GnostrNotification *const *)b;
  if (na->created_at < nb->created_at) return -1;
  if (na->created_at > nb->created_at) return 1;
  return 0;
}

/* Process mentions/replies (kinds 1, 1111) from query results */
static void
history_process_mentions(HistoryLoadData *data, void *txn,
                         char **results, int count)
{
  for (int i = 0; i < count; i++) {
    if (!results[i] || !gnostr_json_is_valid(results[i])) continue;

    char *pubkey = gnostr_json_get_string(results[i], "pubkey", NULL);
    if (!pubkey) continue;

    /* Skip self-notifications */
    if (g_strcmp0(pubkey, data->user_pubkey) == 0) {
      g_free(pubkey);
      continue;
    }

    char *id = gnostr_json_get_string(results[i], "id", NULL);
    if (!id) { g_free(pubkey); continue; }

    gint64 created_at = gnostr_json_get_int64(results[i], "created_at", NULL);
    char *content = gnostr_json_get_string(results[i], "content", NULL);

    /* Classify as reply vs mention using note_is_reply_to_user */
    GnostrNotificationType type = GNOSTR_NOTIFICATION_MENTION;
    char *target_id = NULL;

    unsigned char id_bin[32];
    if (hex_to_bin32(id, id_bin)) {
      storage_ndb_note *note = NULL;
      uint64_t nkey = storage_ndb_get_note_key_by_id(txn, id_bin, &note);
      if (nkey != 0 && note) {
        char *reply_target = NULL;
        if (note_is_reply_to_user(txn, note, data->user_pubkey, &reply_target)) {
          type = GNOSTR_NOTIFICATION_REPLY;
          target_id = reply_target;
        }
      }
    }

    gint64 last_read = data->last_read[type];

    GnostrNotification *notif = g_new0(GnostrNotification, 1);
    notif->id = id;
    notif->type = type;
    notif->actor_pubkey = pubkey;
    notif->content_preview = content;
    notif->target_note_id = target_id ? target_id : g_strdup(id);
    notif->created_at = created_at;
    notif->is_read = (created_at <= last_read);
    notif->zap_amount_msats = 0;

    g_ptr_array_add(data->notifs, notif);
  }
}

/* Process reactions (kind 7) from query results */
static void
history_process_reactions(HistoryLoadData *data, char **results, int count)
{
  gint64 last_read = data->last_read[GNOSTR_NOTIFICATION_REACTION];

  for (int i = 0; i < count; i++) {
    if (!results[i] || !gnostr_json_is_valid(results[i])) continue;

    char *pubkey = gnostr_json_get_string(results[i], "pubkey", NULL);
    if (!pubkey) continue;

    if (g_strcmp0(pubkey, data->user_pubkey) == 0) {
      g_free(pubkey);
      continue;
    }

    char *id = gnostr_json_get_string(results[i], "id", NULL);
    if (!id) { g_free(pubkey); continue; }

    gint64 created_at = gnostr_json_get_int64(results[i], "created_at", NULL);
    char *content = gnostr_json_get_string(results[i], "content", NULL);
    char *target_id = history_extract_target_event_id(results[i]);

    GnostrNotification *notif = g_new0(GnostrNotification, 1);
    notif->id = id;
    notif->type = GNOSTR_NOTIFICATION_REACTION;
    notif->actor_pubkey = pubkey;
    notif->content_preview = content;  /* emoji like "+", heart, etc. */
    notif->target_note_id = target_id;
    notif->created_at = created_at;
    notif->is_read = (created_at <= last_read);

    g_ptr_array_add(data->notifs, notif);
  }
}

/* Process reposts (kind 6) from query results */
static void
history_process_reposts(HistoryLoadData *data, char **results, int count)
{
  gint64 last_read = data->last_read[GNOSTR_NOTIFICATION_REPOST];

  for (int i = 0; i < count; i++) {
    if (!results[i] || !gnostr_json_is_valid(results[i])) continue;

    char *pubkey = gnostr_json_get_string(results[i], "pubkey", NULL);
    if (!pubkey) continue;

    if (g_strcmp0(pubkey, data->user_pubkey) == 0) {
      g_free(pubkey);
      continue;
    }

    char *id = gnostr_json_get_string(results[i], "id", NULL);
    if (!id) { g_free(pubkey); continue; }

    gint64 created_at = gnostr_json_get_int64(results[i], "created_at", NULL);
    char *target_id = history_extract_target_event_id(results[i]);

    GnostrNotification *notif = g_new0(GnostrNotification, 1);
    notif->id = id;
    notif->type = GNOSTR_NOTIFICATION_REPOST;
    notif->actor_pubkey = pubkey;
    notif->target_note_id = target_id;
    notif->created_at = created_at;
    notif->is_read = (created_at <= last_read);

    g_ptr_array_add(data->notifs, notif);
  }
}

/* Process zaps (kind 9735) from query results */
static void
history_process_zaps(HistoryLoadData *data, char **results, int count)
{
  gint64 last_read = data->last_read[GNOSTR_NOTIFICATION_ZAP];

  for (int i = 0; i < count; i++) {
    if (!results[i] || !gnostr_json_is_valid(results[i])) continue;

    char *id = gnostr_json_get_string(results[i], "id", NULL);
    if (!id) continue;

    gint64 created_at = gnostr_json_get_int64(results[i], "created_at", NULL);

    /* Parse zap receipt for sender and amount */
    GnostrZapReceipt *receipt = gnostr_zap_parse_receipt(results[i]);
    char *sender = NULL;
    guint64 amount_msats = 0;

    if (receipt) {
      sender = g_strdup(receipt->sender_pubkey ? receipt->sender_pubkey
                                               : receipt->event_pubkey);
      if (receipt->amount_msat > 0)
        amount_msats = (guint64)receipt->amount_msat;
      gnostr_zap_receipt_free(receipt);
    } else {
      /* Fallback: use event pubkey */
      sender = gnostr_json_get_string(results[i], "pubkey", NULL);
    }

    if (!sender || g_strcmp0(sender, data->user_pubkey) == 0) {
      g_free(sender);
      g_free(id);
      continue;
    }

    char *target_id = history_extract_target_event_id(results[i]);

    GnostrNotification *notif = g_new0(GnostrNotification, 1);
    notif->id = id;
    notif->type = GNOSTR_NOTIFICATION_ZAP;
    notif->actor_pubkey = sender;
    notif->target_note_id = target_id;
    notif->created_at = created_at;
    notif->is_read = (created_at <= last_read);
    notif->zap_amount_msats = amount_msats;

    g_ptr_array_add(data->notifs, notif);
  }
}

/* Process followers (kind 3) from query results */
static void
history_process_followers(HistoryLoadData *data, char **results, int count)
{
  gint64 last_read = data->last_read[GNOSTR_NOTIFICATION_FOLLOWER];

  for (int i = 0; i < count; i++) {
    if (!results[i] || !gnostr_json_is_valid(results[i])) continue;

    char *pubkey = gnostr_json_get_string(results[i], "pubkey", NULL);
    if (!pubkey) continue;

    if (g_strcmp0(pubkey, data->user_pubkey) == 0) {
      g_free(pubkey);
      continue;
    }

    char *id = gnostr_json_get_string(results[i], "id", NULL);
    if (!id) { g_free(pubkey); continue; }

    gint64 created_at = gnostr_json_get_int64(results[i], "created_at", NULL);

    GnostrNotification *notif = g_new0(GnostrNotification, 1);
    notif->id = id;
    notif->type = GNOSTR_NOTIFICATION_FOLLOWER;
    notif->actor_pubkey = pubkey;
    notif->created_at = created_at;
    notif->is_read = (created_at <= last_read);

    g_ptr_array_add(data->notifs, notif);
  }
}

#define HISTORY_LIMIT_PER_TYPE 100

static void
history_load_thread_func(GTask *task, gpointer source_object,
                         gpointer task_data, GCancellable *cancellable)
{
  (void)source_object;
  (void)cancellable;
  HistoryLoadData *data = task_data;
  data->notifs = g_ptr_array_new_with_free_func(
      (GDestroyNotify)gnostr_notification_free);

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) {
    g_debug("[HISTORY] Failed to begin NDB query for history loading");
    g_task_return_pointer(task, data, NULL);
    return;
  }

  /* 1. Mentions/replies (kinds 1, 1111) */
  {
    gchar *filter = g_strdup_printf(
        "{\"kinds\":[%d,%d],\"#p\":[\"%s\"],\"limit\":%d}",
        KIND_TEXT_NOTE, KIND_COMMENT, data->user_pubkey, HISTORY_LIMIT_PER_TYPE);
    char **results = NULL;
    int count = 0;
    if (storage_ndb_query(txn, filter, &results, &count, NULL) == 0 && count > 0) {
      history_process_mentions(data, txn, results, count);
    }
    if (results) storage_ndb_free_results(results, count);
    g_free(filter);
  }

  /* 2. Reactions (kind 7) */
  {
    gchar *filter = g_strdup_printf(
        "{\"kinds\":[%d],\"#p\":[\"%s\"],\"limit\":%d}",
        KIND_REACTION, data->user_pubkey, HISTORY_LIMIT_PER_TYPE);
    char **results = NULL;
    int count = 0;
    if (storage_ndb_query(txn, filter, &results, &count, NULL) == 0 && count > 0) {
      history_process_reactions(data, results, count);
    }
    if (results) storage_ndb_free_results(results, count);
    g_free(filter);
  }

  /* 3. Reposts (kind 6) */
  {
    gchar *filter = g_strdup_printf(
        "{\"kinds\":[%d],\"#p\":[\"%s\"],\"limit\":%d}",
        KIND_REPOST, data->user_pubkey, HISTORY_LIMIT_PER_TYPE);
    char **results = NULL;
    int count = 0;
    if (storage_ndb_query(txn, filter, &results, &count, NULL) == 0 && count > 0) {
      history_process_reposts(data, results, count);
    }
    if (results) storage_ndb_free_results(results, count);
    g_free(filter);
  }

  /* 4. Zaps (kind 9735) */
  {
    gchar *filter = g_strdup_printf(
        "{\"kinds\":[%d],\"#p\":[\"%s\"],\"limit\":%d}",
        KIND_ZAP_RECEIPT, data->user_pubkey, HISTORY_LIMIT_PER_TYPE);
    char **results = NULL;
    int count = 0;
    if (storage_ndb_query(txn, filter, &results, &count, NULL) == 0 && count > 0) {
      history_process_zaps(data, results, count);
    }
    if (results) storage_ndb_free_results(results, count);
    g_free(filter);
  }

  /* 5. Followers (kind 3) */
  {
    gchar *filter = g_strdup_printf(
        "{\"kinds\":[%d],\"#p\":[\"%s\"],\"limit\":%d}",
        KIND_CONTACT_LIST, data->user_pubkey, HISTORY_LIMIT_PER_TYPE);
    char **results = NULL;
    int count = 0;
    if (storage_ndb_query(txn, filter, &results, &count, NULL) == 0 && count > 0) {
      history_process_followers(data, results, count);
    }
    if (results) storage_ndb_free_results(results, count);
    g_free(filter);
  }

  storage_ndb_end_query(txn);

  /* Sort by created_at ascending (oldest first → prepend gives newest on top) */
  g_ptr_array_sort(data->notifs, notif_cmp_created_at_asc);

  g_debug("[HISTORY] Loaded %u historical notifications", data->notifs->len);
  g_task_return_pointer(task, data, NULL);
}

static void
history_load_done(GObject *source_object, GAsyncResult *result, gpointer user_data)
{
  (void)source_object;
  (void)user_data;
  GTask *task = G_TASK(result);
  HistoryLoadData *data = g_task_propagate_pointer(task, NULL);
  if (!data) return;

  if (data->view && GNOSTR_IS_NOTIFICATIONS_VIEW(data->view)) {
    for (guint i = 0; i < data->notifs->len; i++) {
      GnostrNotification *notif = g_ptr_array_index(data->notifs, i);
      gnostr_notifications_view_add_notification(data->view, notif);
    }
    gnostr_notifications_view_set_loading(data->view, FALSE);
    if (data->notifs->len == 0)
      gnostr_notifications_view_set_empty(data->view, TRUE);

    g_debug("[HISTORY] Added %u notifications to view", data->notifs->len);
  }

  history_load_data_free(data);
}

void
gnostr_badge_manager_load_history(GnostrBadgeManager *self,
                                   GnostrNotificationsView *view)
{
  g_return_if_fail(GNOSTR_IS_BADGE_MANAGER(self));
  if (!self->user_pubkey || strlen(self->user_pubkey) != 64 || !view) return;

  HistoryLoadData *data = g_slice_new0(HistoryLoadData);
  data->user_pubkey = g_strdup(self->user_pubkey);
  memcpy(data->last_read, self->last_read, sizeof(data->last_read));
  data->view = view;

  GTask *task = g_task_new(self, NULL, history_load_done, NULL);
  g_task_set_task_data(task, data, NULL);
  g_task_run_in_thread(task, history_load_thread_func);
  g_object_unref(task);
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
