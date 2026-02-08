/*
 * desktop_notify.c - Desktop notification implementation for Linux
 *
 * Uses GNotification (GLib/GIO) for native desktop notifications.
 * Integrates with GNOME, KDE, and other FreeDesktop-compliant desktops.
 *
 * This file is compiled on Linux. macOS uses desktop_notify_macos.m instead.
 */

#ifndef __APPLE__

#define G_LOG_DOMAIN "desktop-notify"

#include "desktop_notify.h"

#include <string.h>

/* GSettings schema IDs */
#define GSETTINGS_NOTIFICATIONS_SCHEMA "org.gnostr.Notifications"

/* Notification ID prefixes for grouping */
#define NOTIFY_ID_DM      "gnostr-dm-"
#define NOTIFY_ID_MENTION "gnostr-mention-"
#define NOTIFY_ID_REPLY   "gnostr-reply-"
#define NOTIFY_ID_ZAP     "gnostr-zap-"
#define NOTIFY_ID_REPOST  "gnostr-repost-"

/* Maximum preview length for notification body */
#define MAX_PREVIEW_LENGTH 100

struct _GnostrDesktopNotify {
  GObject parent_instance;

  /* GApplication for sending notifications */
  GApplication *app;  /* weak ref */

  /* Configuration */
  gboolean enabled[GNOSTR_NOTIFICATION_TYPE_COUNT];
  gboolean sound_enabled;
  GnostrDesktopNotifyPrivacy privacy;

  /* Action callback */
  GnostrNotifyActionCallback callback;
  gpointer callback_data;
  GDestroyNotify callback_destroy;

  /* GSettings for persistence */
  GSettings *settings;

  /* Permission status */
  gboolean has_permission;
};

G_DEFINE_TYPE(GnostrDesktopNotify, gnostr_desktop_notify, G_TYPE_OBJECT)

static GnostrDesktopNotify *g_default_notify = NULL;

/* Forward declarations */
static void load_settings(GnostrDesktopNotify *self);
static void save_settings(GnostrDesktopNotify *self);
static gchar *truncate_preview(const char *text, gsize max_len);
static gchar *get_notification_id(GnostrNotificationType type, const char *event_id);
static void on_notification_action(GSimpleAction *action, GVariant *parameter, gpointer user_data);

/* ============== GObject Implementation ============== */

static void
gnostr_desktop_notify_dispose(GObject *object)
{
  GnostrDesktopNotify *self = GNOSTR_DESKTOP_NOTIFY(object);

  if (self->callback_destroy && self->callback_data) {
    self->callback_destroy(self->callback_data);
  }
  self->callback = NULL;
  self->callback_data = NULL;
  self->callback_destroy = NULL;

  g_clear_object(&self->settings);

  G_OBJECT_CLASS(gnostr_desktop_notify_parent_class)->dispose(object);
}

static void
gnostr_desktop_notify_class_init(GnostrDesktopNotifyClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gnostr_desktop_notify_dispose;
}

static void
gnostr_desktop_notify_init(GnostrDesktopNotify *self)
{
  /* Default: all notification types enabled */
  for (int i = 0; i < GNOSTR_NOTIFICATION_TYPE_COUNT; i++) {
    self->enabled[i] = TRUE;
  }

  self->sound_enabled = TRUE;
  self->privacy = GNOSTR_NOTIFY_PRIVACY_FULL;
  self->app = NULL;
  self->callback = NULL;
  self->callback_data = NULL;
  self->callback_destroy = NULL;
  self->settings = NULL;
  self->has_permission = TRUE;  /* Linux: always granted */

  /* Try to load settings */
  load_settings(self);
}

/* ============== Settings Persistence ============== */

static void
load_settings(GnostrDesktopNotify *self)
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

  /* Load master enable and popup enable */
  gboolean master_enabled = g_settings_get_boolean(self->settings, "enabled");
  gboolean popup_enabled = g_settings_get_boolean(self->settings, "desktop-popup-enabled");

  /* Per-type notification toggles */
  self->enabled[GNOSTR_NOTIFICATION_DM] =
    master_enabled && popup_enabled &&
    g_settings_get_boolean(self->settings, "notify-dm-enabled");
  self->enabled[GNOSTR_NOTIFICATION_MENTION] =
    master_enabled && popup_enabled &&
    g_settings_get_boolean(self->settings, "notify-mention-enabled");
  self->enabled[GNOSTR_NOTIFICATION_REPLY] =
    master_enabled && popup_enabled &&
    g_settings_get_boolean(self->settings, "notify-reply-enabled");
  self->enabled[GNOSTR_NOTIFICATION_ZAP] =
    master_enabled && popup_enabled &&
    g_settings_get_boolean(self->settings, "notify-zap-enabled");

  /* Sound setting */
  self->sound_enabled = g_settings_get_boolean(self->settings, "sound-enabled");

  g_debug("Loaded notification settings: dm=%d mention=%d reply=%d zap=%d sound=%d",
          self->enabled[0], self->enabled[1], self->enabled[2], self->enabled[3],
          self->sound_enabled);
}

static void
save_settings(GnostrDesktopNotify *self)
{
  if (!self->settings) return;

  g_settings_set_boolean(self->settings, "notify-dm-enabled",
                          self->enabled[GNOSTR_NOTIFICATION_DM]);
  g_settings_set_boolean(self->settings, "notify-mention-enabled",
                          self->enabled[GNOSTR_NOTIFICATION_MENTION]);
  g_settings_set_boolean(self->settings, "notify-reply-enabled",
                          self->enabled[GNOSTR_NOTIFICATION_REPLY]);
  g_settings_set_boolean(self->settings, "notify-zap-enabled",
                          self->enabled[GNOSTR_NOTIFICATION_ZAP]);
  g_settings_set_boolean(self->settings, "sound-enabled",
                          self->sound_enabled);
}

/* ============== Lifecycle ============== */

GnostrDesktopNotify *
gnostr_desktop_notify_get_default(void)
{
  if (!g_default_notify) {
    g_default_notify = gnostr_desktop_notify_new(NULL);
  }
  return g_default_notify;
}

GnostrDesktopNotify *
gnostr_desktop_notify_new(GApplication *app)
{
  GnostrDesktopNotify *self = g_object_new(GNOSTR_TYPE_DESKTOP_NOTIFY, NULL);

  if (app) {
    gnostr_desktop_notify_set_app(self, app);
  }

  return self;
}

/* ============== Initialization ============== */

void
gnostr_desktop_notify_set_app(GnostrDesktopNotify *self, GApplication *app)
{
  g_return_if_fail(GNOSTR_IS_DESKTOP_NOTIFY(self));

  self->app = app;  /* weak reference */

  if (app) {
    /* Register notification actions with the application */
    static const GActionEntry notify_actions[] = {
      { "notify-open", on_notification_action, "s", NULL, NULL },
      { "notify-mark-read", on_notification_action, "s", NULL, NULL },
      { "notify-reply", on_notification_action, "s", NULL, NULL },
    };

    g_action_map_add_action_entries(G_ACTION_MAP(app), notify_actions,
                                     G_N_ELEMENTS(notify_actions), self);

    g_debug("Registered notification actions with app");
  }
}

void
gnostr_desktop_notify_request_permission(GnostrDesktopNotify *self)
{
  g_return_if_fail(GNOSTR_IS_DESKTOP_NOTIFY(self));

  /* On Linux, notification permissions are always granted.
   * Just mark as having permission. */
  self->has_permission = TRUE;
  g_debug("Linux: Notification permissions always granted");
}

gboolean
gnostr_desktop_notify_is_available(void)
{
  /* GNotification is always available through GLib/GIO */
  return TRUE;
}

gboolean
gnostr_desktop_notify_has_permission(GnostrDesktopNotify *self)
{
  g_return_val_if_fail(GNOSTR_IS_DESKTOP_NOTIFY(self), FALSE);
  return self->has_permission;
}

/* ============== Configuration ============== */

void
gnostr_desktop_notify_set_enabled(GnostrDesktopNotify *self,
                                   GnostrNotificationType type,
                                   gboolean enabled)
{
  g_return_if_fail(GNOSTR_IS_DESKTOP_NOTIFY(self));
  g_return_if_fail(type < GNOSTR_NOTIFICATION_TYPE_COUNT);

  if (self->enabled[type] == enabled) return;

  self->enabled[type] = enabled;
  save_settings(self);
}

gboolean
gnostr_desktop_notify_get_enabled(GnostrDesktopNotify *self,
                                   GnostrNotificationType type)
{
  g_return_val_if_fail(GNOSTR_IS_DESKTOP_NOTIFY(self), FALSE);
  g_return_val_if_fail(type < GNOSTR_NOTIFICATION_TYPE_COUNT, FALSE);

  return self->enabled[type];
}

void
gnostr_desktop_notify_set_privacy(GnostrDesktopNotify *self,
                                   GnostrDesktopNotifyPrivacy privacy)
{
  g_return_if_fail(GNOSTR_IS_DESKTOP_NOTIFY(self));
  self->privacy = privacy;
}

GnostrDesktopNotifyPrivacy
gnostr_desktop_notify_get_privacy(GnostrDesktopNotify *self)
{
  g_return_val_if_fail(GNOSTR_IS_DESKTOP_NOTIFY(self), GNOSTR_NOTIFY_PRIVACY_FULL);
  return self->privacy;
}

void
gnostr_desktop_notify_set_sound_enabled(GnostrDesktopNotify *self, gboolean enabled)
{
  g_return_if_fail(GNOSTR_IS_DESKTOP_NOTIFY(self));

  if (self->sound_enabled == enabled) return;

  self->sound_enabled = enabled;
  save_settings(self);
}

gboolean
gnostr_desktop_notify_get_sound_enabled(GnostrDesktopNotify *self)
{
  g_return_val_if_fail(GNOSTR_IS_DESKTOP_NOTIFY(self), TRUE);
  return self->sound_enabled;
}

/* ============== Helper Functions ============== */

static gchar *
truncate_preview(const char *text, gsize max_len)
{
  if (!text) return NULL;

  gsize len = strlen(text);
  if (len <= max_len) {
    return g_strdup(text);
  }

  /* Truncate at word boundary if possible */
  gchar *result = g_strndup(text, max_len - 3);

  /* Find last space for cleaner truncation */
  gchar *last_space = g_strrstr(result, " ");
  if (last_space && (last_space - result) > (glong)(max_len / 2)) {
    *last_space = '\0';
  }

  gchar *truncated = g_strdup_printf("%s...", result);
  g_free(result);

  return truncated;
}

static gchar *
get_notification_id(GnostrNotificationType type, const char *event_id)
{
  const char *prefix;
  switch (type) {
    case GNOSTR_NOTIFICATION_DM:
      prefix = NOTIFY_ID_DM;
      break;
    case GNOSTR_NOTIFICATION_MENTION:
      prefix = NOTIFY_ID_MENTION;
      break;
    case GNOSTR_NOTIFICATION_REPLY:
      prefix = NOTIFY_ID_REPLY;
      break;
    case GNOSTR_NOTIFICATION_ZAP:
      prefix = NOTIFY_ID_ZAP;
      break;
    case GNOSTR_NOTIFICATION_REPOST:
      prefix = NOTIFY_ID_REPOST;
      break;
    default:
      prefix = "gnostr-";
      break;
  }

  if (event_id) {
    return g_strdup_printf("%s%s", prefix, event_id);
  } else {
    return g_strdup_printf("%s%" G_GINT64_FORMAT, prefix, g_get_monotonic_time());
  }
}

static void
on_notification_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  GnostrDesktopNotify *self = GNOSTR_DESKTOP_NOTIFY(user_data);

  if (!self->callback) return;

  const char *action_name = g_action_get_name(G_ACTION(action));
  const char *event_id = g_variant_get_string(parameter, NULL);

  /* Extract action type from name */
  const char *action_type = "open";  /* default */
  if (g_str_has_suffix(action_name, "-mark-read")) {
    action_type = "mark-read";
  } else if (g_str_has_suffix(action_name, "-reply")) {
    action_type = "reply";
  }

  g_debug("Notification action: %s for event %s", action_type, event_id);

  self->callback(self, action_type, event_id, self->callback_data);
}

/* ============== Send Notifications ============== */

static void
send_notification_internal(GnostrDesktopNotify *self,
                            GnostrNotificationType type,
                            const char *title,
                            const char *body,
                            const char *event_id)
{
  if (!self->app) {
    g_warning("Cannot send notification: no app set");
    return;
  }

  if (!self->enabled[type]) {
    g_debug("Notification type %d disabled, not sending", type);
    return;
  }

  if (!self->has_permission) {
    g_debug("No notification permission");
    return;
  }

  GNotification *notification = g_notification_new(title);

  /* Set application icon so notifications show the app icon
   * instead of a generic fallback (hq-h0kvq) */
  g_autoptr(GIcon) icon = g_themed_icon_new("org.gnostr.gnostr");
  g_notification_set_icon(notification, icon);

  if (body) {
    g_notification_set_body(notification, body);
  }

  /* Set notification priority based on type */
  GNotificationPriority priority;
  switch (type) {
    case GNOSTR_NOTIFICATION_DM:
      priority = G_NOTIFICATION_PRIORITY_HIGH;
      break;
    case GNOSTR_NOTIFICATION_ZAP:
      priority = G_NOTIFICATION_PRIORITY_HIGH;
      break;
    default:
      priority = G_NOTIFICATION_PRIORITY_NORMAL;
      break;
  }
  g_notification_set_priority(notification, priority);

  /* Set default action (clicking the notification) */
  if (event_id) {
    g_notification_set_default_action_and_target(notification,
                                                  "app.notify-open",
                                                  "s", event_id);

    /* Add additional actions */
    g_notification_add_button_with_target(notification,
                                           "Mark Read",
                                           "app.notify-mark-read",
                                           "s", event_id);

    if (type == GNOSTR_NOTIFICATION_DM || type == GNOSTR_NOTIFICATION_REPLY) {
      g_notification_add_button_with_target(notification,
                                             "Reply",
                                             "app.notify-reply",
                                             "s", event_id);
    }
  }

  /* Generate notification ID */
  gchar *notify_id = get_notification_id(type, event_id);

  /* Send the notification */
  g_application_send_notification(self->app, notify_id, notification);

  g_debug("Sent notification: id=%s title='%s'", notify_id, title);

  g_free(notify_id);
  g_object_unref(notification);
}

void
gnostr_desktop_notify_send_dm(GnostrDesktopNotify *self,
                               const char *sender_name,
                               const char *sender_pubkey,
                               const char *message_preview,
                               const char *event_id)
{
  g_return_if_fail(GNOSTR_IS_DESKTOP_NOTIFY(self));
  g_return_if_fail(sender_name != NULL);
  (void)sender_pubkey;  /* For future use with avatars */

  gchar *title;
  gchar *body = NULL;

  switch (self->privacy) {
    case GNOSTR_NOTIFY_PRIVACY_HIDDEN:
      title = g_strdup("New direct message");
      body = NULL;
      break;

    case GNOSTR_NOTIFY_PRIVACY_SENDER_ONLY:
      title = g_strdup_printf("New message from %s", sender_name);
      body = NULL;
      break;

    case GNOSTR_NOTIFY_PRIVACY_FULL:
    default:
      title = g_strdup_printf("Message from %s", sender_name);
      body = truncate_preview(message_preview, MAX_PREVIEW_LENGTH);
      break;
  }

  send_notification_internal(self, GNOSTR_NOTIFICATION_DM, title, body, event_id);

  g_free(title);
  g_free(body);
}

void
gnostr_desktop_notify_send_mention(GnostrDesktopNotify *self,
                                    const char *sender_name,
                                    const char *sender_pubkey,
                                    const char *note_preview,
                                    const char *event_id)
{
  g_return_if_fail(GNOSTR_IS_DESKTOP_NOTIFY(self));
  g_return_if_fail(sender_name != NULL);
  (void)sender_pubkey;

  gchar *title;
  gchar *body = NULL;

  switch (self->privacy) {
    case GNOSTR_NOTIFY_PRIVACY_HIDDEN:
      title = g_strdup("You were mentioned");
      break;

    case GNOSTR_NOTIFY_PRIVACY_SENDER_ONLY:
    case GNOSTR_NOTIFY_PRIVACY_FULL:
    default:
      title = g_strdup_printf("%s mentioned you", sender_name);
      if (self->privacy == GNOSTR_NOTIFY_PRIVACY_FULL) {
        body = truncate_preview(note_preview, MAX_PREVIEW_LENGTH);
      }
      break;
  }

  send_notification_internal(self, GNOSTR_NOTIFICATION_MENTION, title, body, event_id);

  g_free(title);
  g_free(body);
}

void
gnostr_desktop_notify_send_reply(GnostrDesktopNotify *self,
                                  const char *sender_name,
                                  const char *sender_pubkey,
                                  const char *reply_preview,
                                  const char *event_id)
{
  g_return_if_fail(GNOSTR_IS_DESKTOP_NOTIFY(self));
  g_return_if_fail(sender_name != NULL);
  (void)sender_pubkey;

  gchar *title;
  gchar *body = NULL;

  switch (self->privacy) {
    case GNOSTR_NOTIFY_PRIVACY_HIDDEN:
      title = g_strdup("New reply to your note");
      break;

    case GNOSTR_NOTIFY_PRIVACY_SENDER_ONLY:
    case GNOSTR_NOTIFY_PRIVACY_FULL:
    default:
      title = g_strdup_printf("%s replied to your note", sender_name);
      if (self->privacy == GNOSTR_NOTIFY_PRIVACY_FULL) {
        body = truncate_preview(reply_preview, MAX_PREVIEW_LENGTH);
      }
      break;
  }

  send_notification_internal(self, GNOSTR_NOTIFICATION_REPLY, title, body, event_id);

  g_free(title);
  g_free(body);
}

void
gnostr_desktop_notify_send_zap(GnostrDesktopNotify *self,
                                const char *sender_name,
                                const char *sender_pubkey,
                                guint64 amount_sats,
                                const char *message,
                                const char *event_id)
{
  g_return_if_fail(GNOSTR_IS_DESKTOP_NOTIFY(self));
  g_return_if_fail(sender_name != NULL);
  (void)sender_pubkey;

  gchar *title;
  gchar *body = NULL;

  switch (self->privacy) {
    case GNOSTR_NOTIFY_PRIVACY_HIDDEN:
      title = g_strdup("You received a zap!");
      break;

    case GNOSTR_NOTIFY_PRIVACY_SENDER_ONLY:
      title = g_strdup_printf("%s zapped you", sender_name);
      break;

    case GNOSTR_NOTIFY_PRIVACY_FULL:
    default:
      /* Format amount with proper units */
      if (amount_sats >= 1000000) {
        title = g_strdup_printf("%s zapped you %.2f M sats",
                                sender_name, (double)amount_sats / 1000000.0);
      } else if (amount_sats >= 1000) {
        title = g_strdup_printf("%s zapped you %.1f K sats",
                                sender_name, (double)amount_sats / 1000.0);
      } else {
        title = g_strdup_printf("%s zapped you %" G_GUINT64_FORMAT " sats",
                                sender_name, amount_sats);
      }
      body = truncate_preview(message, MAX_PREVIEW_LENGTH);
      break;
  }

  send_notification_internal(self, GNOSTR_NOTIFICATION_ZAP, title, body, event_id);

  g_free(title);
  g_free(body);
}

void
gnostr_desktop_notify_send_repost(GnostrDesktopNotify *self,
                                   const char *reposter_name,
                                   const char *reposter_pubkey,
                                   const char *event_id)
{
  g_return_if_fail(GNOSTR_IS_DESKTOP_NOTIFY(self));
  g_return_if_fail(reposter_name != NULL);
  (void)reposter_pubkey;

  gchar *title;

  switch (self->privacy) {
    case GNOSTR_NOTIFY_PRIVACY_HIDDEN:
      title = g_strdup("Your note was reposted");
      break;

    case GNOSTR_NOTIFY_PRIVACY_SENDER_ONLY:
    case GNOSTR_NOTIFY_PRIVACY_FULL:
    default:
      title = g_strdup_printf("%s reposted your note", reposter_name);
      break;
  }

  send_notification_internal(self, GNOSTR_NOTIFICATION_REPOST, title, NULL, event_id);

  g_free(title);
}

void
gnostr_desktop_notify_send(GnostrDesktopNotify *self,
                            GnostrNotificationType type,
                            const char *title,
                            const char *body,
                            const char *event_id)
{
  g_return_if_fail(GNOSTR_IS_DESKTOP_NOTIFY(self));
  g_return_if_fail(title != NULL);

  send_notification_internal(self, type, title, body, event_id);
}

/* ============== Action Callback ============== */

void
gnostr_desktop_notify_set_action_callback(GnostrDesktopNotify *self,
                                           GnostrNotifyActionCallback callback,
                                           gpointer user_data,
                                           GDestroyNotify destroy)
{
  g_return_if_fail(GNOSTR_IS_DESKTOP_NOTIFY(self));

  if (self->callback_destroy && self->callback_data) {
    self->callback_destroy(self->callback_data);
  }

  self->callback = callback;
  self->callback_data = user_data;
  self->callback_destroy = destroy;
}

/* ============== Withdraw Notifications ============== */

void
gnostr_desktop_notify_withdraw(GnostrDesktopNotify *self, const char *event_id)
{
  g_return_if_fail(GNOSTR_IS_DESKTOP_NOTIFY(self));

  if (!self->app || !event_id) return;

  /* Try to withdraw for all notification types */
  for (int i = 0; i < GNOSTR_NOTIFICATION_TYPE_COUNT; i++) {
    gchar *notify_id = get_notification_id((GnostrNotificationType)i, event_id);
    g_application_withdraw_notification(self->app, notify_id);
    g_free(notify_id);
  }
}

void
gnostr_desktop_notify_withdraw_type(GnostrDesktopNotify *self,
                                     GnostrNotificationType type)
{
  g_return_if_fail(GNOSTR_IS_DESKTOP_NOTIFY(self));
  g_return_if_fail(type < GNOSTR_NOTIFICATION_TYPE_COUNT);

  if (!self->app) return;

  /* Note: GNotification doesn't support withdrawing by prefix/group.
   * We can only withdraw specific notification IDs.
   * For now, this is a no-op. A future implementation could track
   * sent notification IDs to enable this functionality. */
  g_debug("Withdraw type %d - not implemented for GNotification", type);
}

void
gnostr_desktop_notify_withdraw_all(GnostrDesktopNotify *self)
{
  g_return_if_fail(GNOSTR_IS_DESKTOP_NOTIFY(self));

  if (!self->app) return;

  /* Same limitation as withdraw_type - GNotification doesn't support this.
   * A full implementation would need to track all sent notification IDs. */
  g_debug("Withdraw all - not implemented for GNotification");
}

#endif /* !__APPLE__ */
