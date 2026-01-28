/*
 * desktop_notify_macos.m - Desktop notification implementation for macOS
 *
 * Uses UNUserNotificationCenter (UserNotifications.framework) for native
 * macOS notification center integration.
 *
 * This file is only compiled on macOS.
 */

#ifdef __APPLE__

#import <UserNotifications/UserNotifications.h>
#import <Cocoa/Cocoa.h>

#include "desktop_notify.h"

#include <string.h>

/* GSettings schema IDs */
#define GSETTINGS_NOTIFICATIONS_SCHEMA "org.gnostr.Notifications"

/* Notification category identifiers */
#define CATEGORY_DM      "gnostr.dm"
#define CATEGORY_MENTION "gnostr.mention"
#define CATEGORY_REPLY   "gnostr.reply"
#define CATEGORY_ZAP     "gnostr.zap"

/* Action identifiers */
#define ACTION_OPEN      "open"
#define ACTION_MARK_READ "mark-read"
#define ACTION_REPLY     "reply"

/* Maximum preview length for notification body */
#define MAX_PREVIEW_LENGTH 100

/* User info keys */
#define USERINFO_EVENT_ID   "event_id"
#define USERINFO_TYPE       "type"
#define USERINFO_PUBKEY     "pubkey"

/* ============================================================
 * Forward declarations
 * ============================================================ */

/* Internal function to handle action callbacks - defined later in C implementation */
void gnostr_desktop_notify_handle_action(GnostrDesktopNotify *self,
                                          const char *action,
                                          const char *event_id);

/* Callback data for g_idle_add - structs defined here, functions defined after _GnostrDesktopNotify */
typedef struct {
    GnostrDesktopNotify *owner;
    char *action;
    char *event_id;
} NotifyActionData;

typedef struct {
    GnostrDesktopNotify *owner;
    gboolean granted;
} PermissionData;

/* Forward declarations for idle callbacks - defined after struct _GnostrDesktopNotify */
static gboolean notify_action_idle_cb(gpointer user_data);
static gboolean permission_idle_cb(gpointer user_data);

/* ============================================================
 * Objective-C Delegate for notification handling
 * ============================================================ */

@interface GnostrNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@property (nonatomic, assign) GnostrDesktopNotify *owner;
@end

@implementation GnostrNotificationDelegate

- (void)userNotificationCenter:(UNUserNotificationCenter *)center
        willPresentNotification:(UNNotification *)notification
          withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler
{
    (void)center;
    (void)notification;

    /* Show notification even when app is in foreground */
    if (@available(macOS 11.0, *)) {
        completionHandler(UNNotificationPresentationOptionBanner |
                          UNNotificationPresentationOptionSound |
                          UNNotificationPresentationOptionBadge);
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        completionHandler(UNNotificationPresentationOptionAlert |
                          UNNotificationPresentationOptionSound |
                          UNNotificationPresentationOptionBadge);
#pragma clang diagnostic pop
    }
}

- (void)userNotificationCenter:(UNUserNotificationCenter *)center
        didReceiveNotificationResponse:(UNNotificationResponse *)response
                 withCompletionHandler:(void (^)(void))completionHandler
{
    (void)center;

    NSDictionary *userInfo = response.notification.request.content.userInfo;
    NSString *eventId = userInfo[@ USERINFO_EVENT_ID];
    NSString *actionId = response.actionIdentifier;

    /* Determine action type */
    const char *action = "open";  /* default for tapping the notification */
    if ([actionId isEqualToString:@ACTION_MARK_READ]) {
        action = "mark-read";
    } else if ([actionId isEqualToString:@ACTION_REPLY]) {
        action = "reply";
    } else if ([actionId isEqualToString:UNNotificationDefaultActionIdentifier]) {
        action = "open";
    }

    /* Dispatch callback to GLib main loop */
    if (self.owner) {
        NotifyActionData *data = g_new0(NotifyActionData, 1);
        data->owner = self.owner;
        data->action = g_strdup(action);
        data->event_id = eventId ? g_strdup([eventId UTF8String]) : NULL;
        g_idle_add(notify_action_idle_cb, data);
    }

    completionHandler();
}

@end

/* ============================================================
 * C Implementation
 * ============================================================ */

struct _GnostrDesktopNotify {
    GObject parent_instance;

    /* GApplication for integration (weak ref) */
    GApplication *app;

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
    gboolean permission_requested;

    /* Objective-C delegate */
    void *delegate;  /* GnostrNotificationDelegate* */
};

G_DEFINE_TYPE(GnostrDesktopNotify, gnostr_desktop_notify, G_TYPE_OBJECT)

static GnostrDesktopNotify *g_default_notify = NULL;

/* Idle callback implementations - now that struct is defined */
static gboolean notify_action_idle_cb(gpointer user_data) {
    NotifyActionData *data = user_data;
    if (data->owner && GNOSTR_IS_DESKTOP_NOTIFY(data->owner)) {
        gnostr_desktop_notify_handle_action(data->owner, data->action, data->event_id);
    }
    g_free(data->action);
    g_free(data->event_id);
    g_free(data);
    return G_SOURCE_REMOVE;
}

static gboolean permission_idle_cb(gpointer user_data) {
    PermissionData *data = user_data;
    if (data->owner && GNOSTR_IS_DESKTOP_NOTIFY(data->owner)) {
        data->owner->has_permission = data->granted;
    }
    g_free(data);
    return G_SOURCE_REMOVE;
}

/* Forward declarations */
static void load_settings(GnostrDesktopNotify *self);
static void save_settings(GnostrDesktopNotify *self);
static NSString *truncate_preview(const char *text, NSUInteger max_len);
static void register_notification_categories(void);

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

    if (self->delegate) {
        GnostrNotificationDelegate *delegate =
            (__bridge_transfer GnostrNotificationDelegate *)self->delegate;
        delegate.owner = NULL;
        self->delegate = NULL;
    }

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
    self->has_permission = FALSE;
    self->permission_requested = FALSE;
    self->delegate = NULL;

    /* Try to load settings */
    load_settings(self);

    /* Create delegate and register with notification center */
    @autoreleasepool {
        GnostrNotificationDelegate *delegate = [[GnostrNotificationDelegate alloc] init];
        delegate.owner = self;
        self->delegate = (__bridge_retained void *)delegate;

        UNUserNotificationCenter *center = [UNUserNotificationCenter currentNotificationCenter];
        center.delegate = delegate;

        /* Register notification categories */
        register_notification_categories();
    }
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

/* ============== Register Notification Categories ============== */

static void
register_notification_categories(void)
{
    @autoreleasepool {
        /* Create actions */
        UNNotificationAction *openAction =
            [UNNotificationAction actionWithIdentifier:@ACTION_OPEN
                                                 title:@"Open"
                                               options:UNNotificationActionOptionForeground];

        UNNotificationAction *markReadAction =
            [UNNotificationAction actionWithIdentifier:@ACTION_MARK_READ
                                                 title:@"Mark Read"
                                               options:UNNotificationActionOptionNone];

        UNNotificationAction *replyAction =
            [UNNotificationAction actionWithIdentifier:@ACTION_REPLY
                                                 title:@"Reply"
                                               options:UNNotificationActionOptionForeground];

        /* DM category: open, mark read, reply */
        UNNotificationCategory *dmCategory =
            [UNNotificationCategory categoryWithIdentifier:@CATEGORY_DM
                                                   actions:@[openAction, markReadAction, replyAction]
                                         intentIdentifiers:@[]
                                                   options:UNNotificationCategoryOptionNone];

        /* Mention category: open, mark read */
        UNNotificationCategory *mentionCategory =
            [UNNotificationCategory categoryWithIdentifier:@CATEGORY_MENTION
                                                   actions:@[openAction, markReadAction]
                                         intentIdentifiers:@[]
                                                   options:UNNotificationCategoryOptionNone];

        /* Reply category: open, mark read, reply */
        UNNotificationCategory *replyCategory =
            [UNNotificationCategory categoryWithIdentifier:@CATEGORY_REPLY
                                                   actions:@[openAction, markReadAction, replyAction]
                                         intentIdentifiers:@[]
                                                   options:UNNotificationCategoryOptionNone];

        /* Zap category: open, mark read */
        UNNotificationCategory *zapCategory =
            [UNNotificationCategory categoryWithIdentifier:@CATEGORY_ZAP
                                                   actions:@[openAction, markReadAction]
                                         intentIdentifiers:@[]
                                                   options:UNNotificationCategoryOptionNone];

        /* Register all categories */
        NSSet *categories = [NSSet setWithObjects:dmCategory, mentionCategory,
                                                  replyCategory, zapCategory, nil];

        UNUserNotificationCenter *center = [UNUserNotificationCenter currentNotificationCenter];
        [center setNotificationCategories:categories];

        NSLog(@"desktop-notify-macos: Registered notification categories");
    }
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
}

void
gnostr_desktop_notify_request_permission(GnostrDesktopNotify *self)
{
    g_return_if_fail(GNOSTR_IS_DESKTOP_NOTIFY(self));

    if (self->permission_requested) {
        return;
    }

    self->permission_requested = TRUE;

    @autoreleasepool {
        UNUserNotificationCenter *center = [UNUserNotificationCenter currentNotificationCenter];

        UNAuthorizationOptions options = UNAuthorizationOptionAlert |
                                         UNAuthorizationOptionSound |
                                         UNAuthorizationOptionBadge;

        GnostrDesktopNotify *weak_self = self;
        [center requestAuthorizationWithOptions:options
                              completionHandler:^(BOOL granted, NSError * _Nullable error) {
            if (error) {
                NSLog(@"desktop-notify-macos: Permission request error: %@", error);
            } else {
                NSLog(@"desktop-notify-macos: Permission %@", granted ? @"granted" : @"denied");
            }
            /* Use g_idle_add to update state on GLib main loop */
            PermissionData *data = g_new0(PermissionData, 1);
            data->owner = weak_self;
            data->granted = granted;
            g_idle_add(permission_idle_cb, data);
        }];
    }
}

gboolean
gnostr_desktop_notify_is_available(void)
{
    /* UNUserNotificationCenter is available on macOS 10.14+ */
    if (@available(macOS 10.14, *)) {
        return TRUE;
    }
    return FALSE;
}

gboolean
gnostr_desktop_notify_has_permission(GnostrDesktopNotify *self)
{
    g_return_val_if_fail(GNOSTR_IS_DESKTOP_NOTIFY(self), FALSE);

    /* Check current authorization status */
    @autoreleasepool {
        __block BOOL permitted = NO;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);

        UNUserNotificationCenter *center = [UNUserNotificationCenter currentNotificationCenter];
        [center getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings *settings) {
            permitted = (settings.authorizationStatus == UNAuthorizationStatusAuthorized);
            dispatch_semaphore_signal(sem);
        }];

        /* Wait with timeout */
        dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 1 * NSEC_PER_SEC));

        self->has_permission = permitted;
        return permitted;
    }
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

static NSString *
truncate_preview(const char *text, NSUInteger max_len)
{
    if (!text) return nil;

    NSString *str = [NSString stringWithUTF8String:text];
    if (!str) return nil;

    if ([str length] <= max_len) {
        return str;
    }

    /* Truncate at word boundary if possible */
    NSRange range = NSMakeRange(0, max_len - 3);
    NSRange spaceRange = [str rangeOfString:@" "
                                    options:NSBackwardsSearch
                                      range:range];

    if (spaceRange.location != NSNotFound && spaceRange.location > max_len / 2) {
        range.length = spaceRange.location;
    }

    return [NSString stringWithFormat:@"%@...", [str substringWithRange:range]];
}

static NSString *
get_category_id(GnostrNotificationType type)
{
    switch (type) {
        case GNOSTR_NOTIFICATION_DM:
            return @CATEGORY_DM;
        case GNOSTR_NOTIFICATION_MENTION:
            return @CATEGORY_MENTION;
        case GNOSTR_NOTIFICATION_REPLY:
            return @CATEGORY_REPLY;
        case GNOSTR_NOTIFICATION_ZAP:
            return @CATEGORY_ZAP;
        default:
            return @CATEGORY_MENTION;
    }
}

/* ============== Send Notifications ============== */

static void
send_notification_internal(GnostrDesktopNotify *self,
                            GnostrNotificationType type,
                            const char *title,
                            const char *body,
                            const char *event_id,
                            const char *pubkey)
{
    if (!self->enabled[type]) {
        g_debug("Notification type %d disabled, not sending", type);
        return;
    }

    if (!self->has_permission) {
        g_debug("No notification permission");
        return;
    }

    @autoreleasepool {
        UNMutableNotificationContent *content = [[UNMutableNotificationContent alloc] init];

        content.title = title ? [NSString stringWithUTF8String:title] : @"GNostr";
        if (body) {
            content.body = [NSString stringWithUTF8String:body];
        }

        /* Set category for actions */
        content.categoryIdentifier = get_category_id(type);

        /* Set sound if enabled */
        if (self->sound_enabled) {
            content.sound = [UNNotificationSound defaultSound];
        }

        /* Store event info for action handling */
        NSMutableDictionary *userInfo = [NSMutableDictionary dictionary];
        if (event_id) {
            userInfo[@ USERINFO_EVENT_ID] = [NSString stringWithUTF8String:event_id];
        }
        userInfo[@ USERINFO_TYPE] = @(type);
        if (pubkey) {
            userInfo[@ USERINFO_PUBKEY] = [NSString stringWithUTF8String:pubkey];
        }
        content.userInfo = userInfo;

        /* Create unique identifier */
        NSString *identifier;
        if (event_id) {
            identifier = [NSString stringWithFormat:@"gnostr-%d-%s", type, event_id];
        } else {
            identifier = [NSString stringWithFormat:@"gnostr-%d-%lld", type,
                         (long long)[[NSDate date] timeIntervalSince1970]];
        }

        /* Create request and add to notification center */
        UNNotificationRequest *request =
            [UNNotificationRequest requestWithIdentifier:identifier
                                                 content:content
                                                 trigger:nil];

        UNUserNotificationCenter *center = [UNUserNotificationCenter currentNotificationCenter];
        [center addNotificationRequest:request
                 withCompletionHandler:^(NSError * _Nullable error) {
            if (error) {
                NSLog(@"desktop-notify-macos: Failed to send notification: %@", error);
            } else {
                NSLog(@"desktop-notify-macos: Sent notification: %@", identifier);
            }
        }];
    }
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

    const char *title = NULL;
    const char *body = NULL;
    g_autofree gchar *title_str = NULL;
    g_autofree gchar *body_str = NULL;

    switch (self->privacy) {
        case GNOSTR_NOTIFY_PRIVACY_HIDDEN:
            title = "New direct message";
            break;

        case GNOSTR_NOTIFY_PRIVACY_SENDER_ONLY:
            title_str = g_strdup_printf("New message from %s", sender_name);
            title = title_str;
            break;

        case GNOSTR_NOTIFY_PRIVACY_FULL:
        default:
            title_str = g_strdup_printf("Message from %s", sender_name);
            title = title_str;
            @autoreleasepool {
                NSString *preview = truncate_preview(message_preview, MAX_PREVIEW_LENGTH);
                if (preview) {
                    body_str = g_strdup([preview UTF8String]);
                    body = body_str;
                }
            }
            break;
    }

    send_notification_internal(self, GNOSTR_NOTIFICATION_DM, title, body, event_id, sender_pubkey);
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

    const char *title = NULL;
    const char *body = NULL;
    g_autofree gchar *title_str = NULL;
    g_autofree gchar *body_str = NULL;

    switch (self->privacy) {
        case GNOSTR_NOTIFY_PRIVACY_HIDDEN:
            title = "You were mentioned";
            break;

        case GNOSTR_NOTIFY_PRIVACY_SENDER_ONLY:
        case GNOSTR_NOTIFY_PRIVACY_FULL:
        default:
            title_str = g_strdup_printf("%s mentioned you", sender_name);
            title = title_str;
            if (self->privacy == GNOSTR_NOTIFY_PRIVACY_FULL) {
                @autoreleasepool {
                    NSString *preview = truncate_preview(note_preview, MAX_PREVIEW_LENGTH);
                    if (preview) {
                        body_str = g_strdup([preview UTF8String]);
                        body = body_str;
                    }
                }
            }
            break;
    }

    send_notification_internal(self, GNOSTR_NOTIFICATION_MENTION, title, body, event_id, sender_pubkey);
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

    const char *title = NULL;
    const char *body = NULL;
    g_autofree gchar *title_str = NULL;
    g_autofree gchar *body_str = NULL;

    switch (self->privacy) {
        case GNOSTR_NOTIFY_PRIVACY_HIDDEN:
            title = "New reply to your note";
            break;

        case GNOSTR_NOTIFY_PRIVACY_SENDER_ONLY:
        case GNOSTR_NOTIFY_PRIVACY_FULL:
        default:
            title_str = g_strdup_printf("%s replied to your note", sender_name);
            title = title_str;
            if (self->privacy == GNOSTR_NOTIFY_PRIVACY_FULL) {
                @autoreleasepool {
                    NSString *preview = truncate_preview(reply_preview, MAX_PREVIEW_LENGTH);
                    if (preview) {
                        body_str = g_strdup([preview UTF8String]);
                        body = body_str;
                    }
                }
            }
            break;
    }

    send_notification_internal(self, GNOSTR_NOTIFICATION_REPLY, title, body, event_id, sender_pubkey);
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

    const char *title = NULL;
    const char *body = NULL;
    g_autofree gchar *title_str = NULL;
    g_autofree gchar *body_str = NULL;

    switch (self->privacy) {
        case GNOSTR_NOTIFY_PRIVACY_HIDDEN:
            title = "You received a zap!";
            break;

        case GNOSTR_NOTIFY_PRIVACY_SENDER_ONLY:
            title_str = g_strdup_printf("%s zapped you", sender_name);
            title = title_str;
            break;

        case GNOSTR_NOTIFY_PRIVACY_FULL:
        default:
            /* Format amount with proper units */
            if (amount_sats >= 1000000) {
                title_str = g_strdup_printf("%s zapped you %.2f M sats",
                                            sender_name, (double)amount_sats / 1000000.0);
            } else if (amount_sats >= 1000) {
                title_str = g_strdup_printf("%s zapped you %.1f K sats",
                                            sender_name, (double)amount_sats / 1000.0);
            } else {
                title_str = g_strdup_printf("%s zapped you %" G_GUINT64_FORMAT " sats",
                                            sender_name, amount_sats);
            }
            title = title_str;
            @autoreleasepool {
                NSString *preview = truncate_preview(message, MAX_PREVIEW_LENGTH);
                if (preview) {
                    body_str = g_strdup([preview UTF8String]);
                    body = body_str;
                }
            }
            break;
    }

    send_notification_internal(self, GNOSTR_NOTIFICATION_ZAP, title, body, event_id, sender_pubkey);
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

    send_notification_internal(self, type, title, body, event_id, NULL);
}

/* ============== Action Handling ============== */

void
gnostr_desktop_notify_handle_action(GnostrDesktopNotify *self,
                                     const char *action,
                                     const char *event_id)
{
    if (!self || !self->callback) return;

    self->callback(self, action, event_id, self->callback_data);
}

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

    if (!event_id) return;

    @autoreleasepool {
        /* Build identifiers for all types */
        NSMutableArray *identifiers = [NSMutableArray array];
        for (int i = 0; i < GNOSTR_NOTIFICATION_TYPE_COUNT; i++) {
            [identifiers addObject:[NSString stringWithFormat:@"gnostr-%d-%s", i, event_id]];
        }

        UNUserNotificationCenter *center = [UNUserNotificationCenter currentNotificationCenter];
        [center removeDeliveredNotificationsWithIdentifiers:identifiers];
        [center removePendingNotificationRequestsWithIdentifiers:identifiers];
    }
}

void
gnostr_desktop_notify_withdraw_type(GnostrDesktopNotify *self,
                                     GnostrNotificationType type)
{
    g_return_if_fail(GNOSTR_IS_DESKTOP_NOTIFY(self));
    g_return_if_fail(type < GNOSTR_NOTIFICATION_TYPE_COUNT);

    @autoreleasepool {
        UNUserNotificationCenter *center = [UNUserNotificationCenter currentNotificationCenter];
        NSString *prefix = [NSString stringWithFormat:@"gnostr-%d-", type];

        [center getDeliveredNotificationsWithCompletionHandler:^(NSArray<UNNotification *> *notifications) {
            NSMutableArray *identifiers = [NSMutableArray array];
            for (UNNotification *notif in notifications) {
                if ([notif.request.identifier hasPrefix:prefix]) {
                    [identifiers addObject:notif.request.identifier];
                }
            }
            if ([identifiers count] > 0) {
                [center removeDeliveredNotificationsWithIdentifiers:identifiers];
            }
        }];
    }
}

void
gnostr_desktop_notify_withdraw_all(GnostrDesktopNotify *self)
{
    g_return_if_fail(GNOSTR_IS_DESKTOP_NOTIFY(self));

    @autoreleasepool {
        UNUserNotificationCenter *center = [UNUserNotificationCenter currentNotificationCenter];
        [center removeAllDeliveredNotifications];
        [center removeAllPendingNotificationRequests];
    }
}

#endif /* __APPLE__ */
