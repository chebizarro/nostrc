/*
 * gnostr-menubar-macos.m - Menu bar icon support for macOS
 *
 * Implementation using NSStatusItem from AppKit for native macOS menu bar integration.
 * This file is only compiled on macOS (APPLE defined).
 *
 * Note: This file is Objective-C to access AppKit's NSStatusItem API.
 * The C API is exposed through gnostr-tray-icon.h for consistency with Linux.
 */

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#include "gnostr-tray-icon.h"

/* Objective-C class to handle menu bar and delegate methods */
@interface GnostrMenuBarHelper : NSObject
@property (nonatomic, assign) GtkApplication *app;
@property (nonatomic, assign) GtkWindow *window;
@property (nonatomic, strong) NSStatusItem *statusItem;
@property (nonatomic, assign) int unreadCount;

- (instancetype)initWithApp:(GtkApplication *)app;
- (void)setWindow:(GtkWindow *)window;
- (void)setUnreadCount:(int)count;
- (void)cleanup;

/* Menu actions */
- (void)showHideWindow:(id)sender;
- (void)newNote:(id)sender;
- (void)checkDMs:(id)sender;
- (void)openPreferences:(id)sender;
- (void)quitApp:(id)sender;
@end

@implementation GnostrMenuBarHelper

- (instancetype)initWithApp:(GtkApplication *)app {
    self = [super init];
    if (self) {
        _app = app;
        _window = NULL;
        _unreadCount = 0;

        /* Create status item in the system menu bar */
        _statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];

        /* Use a template image for proper dark/light mode support
         *
         * Try in order:
         * 1. SF Symbols "note.text" (macOS 11+)
         * 2. App bundle icon (scaled down)
         * 3. Simple drawn icon as final fallback
         */
        NSImage *image = nil;

        /* Try SF Symbols first (macOS 11+) */
        if (@available(macOS 11.0, *)) {
            image = [NSImage imageWithSystemSymbolName:@"note.text"
                                accessibilityDescription:@"GNostr"];
        }

        /* Fallback: try to use app icon (scaled to menu bar size) */
        if (!image) {
            NSImage *appIcon = [NSApp applicationIconImage];
            if (appIcon) {
                /* Scale app icon to menu bar size (18x18) */
                image = [[NSImage alloc] initWithSize:NSMakeSize(18, 18)];
                [image lockFocus];
                [appIcon drawInRect:NSMakeRect(0, 0, 18, 18)
                           fromRect:NSZeroRect
                          operation:NSCompositingOperationSourceOver
                           fraction:1.0];
                [image unlockFocus];
            }
        }

        /* Final fallback: draw a simple icon */
        if (!image) {
            image = [[NSImage alloc] initWithSize:NSMakeSize(18, 18)];
            [image lockFocus];
            [[NSColor labelColor] setFill];
            /* Draw a simple "N" shape for Nostr */
            NSBezierPath *path = [NSBezierPath bezierPath];
            [path moveToPoint:NSMakePoint(3, 3)];
            [path lineToPoint:NSMakePoint(3, 15)];
            [path lineToPoint:NSMakePoint(15, 3)];
            [path lineToPoint:NSMakePoint(15, 15)];
            [path setLineWidth:2.5];
            [[NSColor labelColor] setStroke];
            [path stroke];
            [image unlockFocus];
        }

        [image setTemplate:YES]; /* Makes it adapt to menu bar appearance */
        _statusItem.button.image = image;
        _statusItem.button.toolTip = @"GNostr - Nostr Client";

        /* Create the dropdown menu */
        [self createMenu];

        NSLog(@"menubar-macos: Status item created successfully");
    }
    return self;
}

- (void)createMenu {
    NSMenu *menu = [[NSMenu alloc] init];

    /* Show/Hide Window */
    NSMenuItem *showHideItem = [[NSMenuItem alloc] initWithTitle:@"Show Window"
                                                          action:@selector(showHideWindow:)
                                                   keyEquivalent:@""];
    showHideItem.target = self;
    showHideItem.tag = 1; /* Tag for identification when updating */
    [menu addItem:showHideItem];

    [menu addItem:[NSMenuItem separatorItem]];

    /* New Note */
    NSMenuItem *newNoteItem = [[NSMenuItem alloc] initWithTitle:@"New Note"
                                                         action:@selector(newNote:)
                                                  keyEquivalent:@"n"];
    newNoteItem.target = self;
    newNoteItem.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [menu addItem:newNoteItem];

    /* Check DMs */
    NSMenuItem *dmsItem = [[NSMenuItem alloc] initWithTitle:@"Check DMs"
                                                     action:@selector(checkDMs:)
                                              keyEquivalent:@"d"];
    dmsItem.target = self;
    dmsItem.tag = 2; /* Tag for updating with unread count */
    dmsItem.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [menu addItem:dmsItem];

    [menu addItem:[NSMenuItem separatorItem]];

    /* Preferences */
    NSMenuItem *prefsItem = [[NSMenuItem alloc] initWithTitle:@"Settings..."
                                                       action:@selector(openPreferences:)
                                                keyEquivalent:@","];
    prefsItem.target = self;
    prefsItem.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    [menu addItem:prefsItem];

    [menu addItem:[NSMenuItem separatorItem]];

    /* Quit */
    NSMenuItem *quitItem = [[NSMenuItem alloc] initWithTitle:@"Quit GNostr"
                                                      action:@selector(quitApp:)
                                               keyEquivalent:@"q"];
    quitItem.target = self;
    quitItem.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    [menu addItem:quitItem];

    _statusItem.menu = menu;
}

- (void)setWindow:(GtkWindow *)window {
    _window = window;
    [self updateShowHideLabel];
}

- (void)setUnreadCount:(int)count {
    _unreadCount = count;
    [self updateBadge];
    [self updateDMsLabel];
}

- (void)updateShowHideLabel {
    if (!_statusItem.menu) return;

    NSMenuItem *item = [_statusItem.menu itemWithTag:1];
    if (item && _window) {
        gboolean visible = gtk_widget_get_visible(GTK_WIDGET(_window));
        item.title = visible ? @"Hide Window" : @"Show Window";
    }
}

- (void)updateDMsLabel {
    if (!_statusItem.menu) return;

    NSMenuItem *item = [_statusItem.menu itemWithTag:2];
    if (item) {
        if (_unreadCount > 0) {
            item.title = [NSString stringWithFormat:@"Check DMs (%d)", _unreadCount];
        } else {
            item.title = @"Check DMs";
        }
    }
}

- (NSImage *)createBadgedImageWithCount:(int)count baseImage:(NSImage *)baseImage {
    NSSize size = NSMakeSize(22, 22); /* Standard menu bar icon size */
    NSImage *badgedImage = [[NSImage alloc] initWithSize:size];

    [badgedImage lockFocus];

    /* Draw the base image */
    NSRect baseRect = NSMakeRect(0, 0, 18, 18);
    [baseImage drawInRect:baseRect fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1.0];

    /* Draw badge circle */
    NSRect badgeRect;
    if (count > 9) {
        badgeRect = NSMakeRect(8, 10, 14, 12);  /* Wider for "99+" */
    } else {
        badgeRect = NSMakeRect(10, 10, 12, 12);  /* Circle for single digit */
    }

    /* Red badge background */
    [[NSColor systemRedColor] setFill];
    NSBezierPath *badgePath = [NSBezierPath bezierPathWithRoundedRect:badgeRect
                                                              xRadius:6
                                                              yRadius:6];
    [badgePath fill];

    /* Badge text */
    NSString *badgeText;
    if (count > 99) {
        badgeText = @"99+";
    } else {
        badgeText = [NSString stringWithFormat:@"%d", count];
    }

    NSDictionary *textAttrs = @{
        NSFontAttributeName: [NSFont boldSystemFontOfSize:8],
        NSForegroundColorAttributeName: [NSColor whiteColor]
    };
    NSSize textSize = [badgeText sizeWithAttributes:textAttrs];
    NSPoint textPoint = NSMakePoint(
        badgeRect.origin.x + (badgeRect.size.width - textSize.width) / 2,
        badgeRect.origin.y + (badgeRect.size.height - textSize.height) / 2
    );
    [badgeText drawAtPoint:textPoint withAttributes:textAttrs];

    [badgedImage unlockFocus];
    [badgedImage setTemplate:NO];  /* Badge has color, so not a template */

    return badgedImage;
}

- (void)updateBadge {
    if (!_statusItem.button) return;

    if (_unreadCount > 0) {
        /* Create a base image - try SF Symbols first, then fallback */
        NSImage *baseImage = nil;
        if (@available(macOS 11.0, *)) {
            baseImage = [NSImage imageWithSystemSymbolName:@"note.text"
                                   accessibilityDescription:@"GNostr"];
        }
        if (!baseImage) {
            baseImage = [[NSImage alloc] initWithSize:NSMakeSize(18, 18)];
            [baseImage lockFocus];
            [[NSColor labelColor] setFill];
            NSBezierPath *path = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(2, 2, 14, 14)];
            [path fill];
            [baseImage unlockFocus];
        }
        [baseImage setTemplate:YES];

        /* Create badged image */
        NSImage *badgedImage = [self createBadgedImageWithCount:_unreadCount baseImage:baseImage];
        _statusItem.button.image = badgedImage;

        /* Update tooltip */
        _statusItem.button.toolTip = [NSString stringWithFormat:@"GNostr - %d unread", _unreadCount];
    } else {
        /* Restore original template image - try SF Symbols first, then fallback */
        NSImage *image = nil;
        if (@available(macOS 11.0, *)) {
            image = [NSImage imageWithSystemSymbolName:@"note.text"
                                accessibilityDescription:@"GNostr"];
        }
        if (!image) {
            image = [[NSImage alloc] initWithSize:NSMakeSize(18, 18)];
            [image lockFocus];
            [[NSColor labelColor] setFill];
            NSBezierPath *path = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(2, 2, 14, 14)];
            [path fill];
            [image unlockFocus];
        }
        [image setTemplate:YES];
        _statusItem.button.image = image;
        _statusItem.button.toolTip = @"GNostr - Nostr Client";
    }
}

- (void)cleanup {
    if (_statusItem) {
        [[NSStatusBar systemStatusBar] removeStatusItem:_statusItem];
        _statusItem = nil;
    }
}

#pragma mark - Menu Actions

- (void)showHideWindow:(id)sender {
    (void)sender;
    if (!_window) return;

    /* Execute on GLib main loop to be thread-safe with GTK */
    dispatch_async(dispatch_get_main_queue(), ^{
        if (gtk_widget_get_visible(GTK_WIDGET(self->_window))) {
            gtk_widget_set_visible(GTK_WIDGET(self->_window), FALSE);
        } else {
            gtk_widget_set_visible(GTK_WIDGET(self->_window), TRUE);
            gtk_window_present(self->_window);
        }
        [self updateShowHideLabel];
    });
}

- (void)newNote:(id)sender {
    (void)sender;
    if (!_window) return;

    dispatch_async(dispatch_get_main_queue(), ^{
        /* Show window if hidden, then activate the new note action */
        if (!gtk_widget_get_visible(GTK_WIDGET(self->_window))) {
            gtk_widget_set_visible(GTK_WIDGET(self->_window), TRUE);
        }
        gtk_window_present(self->_window);

        /* Activate the app action for new note if it exists */
        if (self->_app) {
            GAction *action = g_action_map_lookup_action(G_ACTION_MAP(self->_app), "new-note");
            if (action) {
                g_action_activate(action, NULL);
            } else {
                /* Try window action */
                action = g_action_map_lookup_action(G_ACTION_MAP(self->_window), "new-note");
                if (action) {
                    g_action_activate(action, NULL);
                }
            }
        }
        [self updateShowHideLabel];
    });
}

- (void)checkDMs:(id)sender {
    (void)sender;
    if (!_window) return;

    dispatch_async(dispatch_get_main_queue(), ^{
        /* Show window if hidden */
        if (!gtk_widget_get_visible(GTK_WIDGET(self->_window))) {
            gtk_widget_set_visible(GTK_WIDGET(self->_window), TRUE);
        }
        gtk_window_present(self->_window);

        /* Activate the DM view action if it exists */
        if (self->_app) {
            GAction *action = g_action_map_lookup_action(G_ACTION_MAP(self->_app), "show-dms");
            if (action) {
                g_action_activate(action, NULL);
            } else {
                action = g_action_map_lookup_action(G_ACTION_MAP(self->_window), "show-dms");
                if (action) {
                    g_action_activate(action, NULL);
                }
            }
        }
        [self updateShowHideLabel];
    });
}

- (void)openPreferences:(id)sender {
    (void)sender;
    if (!_window) return;

    dispatch_async(dispatch_get_main_queue(), ^{
        /* Show window if hidden */
        if (!gtk_widget_get_visible(GTK_WIDGET(self->_window))) {
            gtk_widget_set_visible(GTK_WIDGET(self->_window), TRUE);
        }
        gtk_window_present(self->_window);

        /* Activate the preferences action */
        if (self->_app) {
            GAction *action = g_action_map_lookup_action(G_ACTION_MAP(self->_app), "preferences");
            if (action) {
                g_action_activate(action, NULL);
            } else {
                action = g_action_map_lookup_action(G_ACTION_MAP(self->_window), "preferences");
                if (action) {
                    g_action_activate(action, NULL);
                }
            }
        }
        [self updateShowHideLabel];
    });
}

- (void)quitApp:(id)sender {
    (void)sender;

    dispatch_async(dispatch_get_main_queue(), ^{
        if (self->_app) {
            g_application_quit(G_APPLICATION(self->_app));
        }
    });
}

@end

/* ============================================================
 * C API Implementation - Bridges to the Objective-C class
 * ============================================================ */

struct _GnostrTrayIcon {
    GObject parent_instance;

    GtkApplication *app;     /* weak ref */
    GtkWindow *window;       /* weak ref */

    /* Objective-C helper object */
    void *helper;  /* GnostrMenuBarHelper* */
};

G_DEFINE_TYPE(GnostrTrayIcon, gnostr_tray_icon, G_TYPE_OBJECT)

static void
gnostr_tray_icon_dispose(GObject *object)
{
    GnostrTrayIcon *self = GNOSTR_TRAY_ICON(object);

    if (self->helper) {
        GnostrMenuBarHelper *helper = (__bridge_transfer GnostrMenuBarHelper *)self->helper;
        [helper cleanup];
        self->helper = NULL;
    }

    self->app = NULL;
    self->window = NULL;

    G_OBJECT_CLASS(gnostr_tray_icon_parent_class)->dispose(object);
}

static void
gnostr_tray_icon_class_init(GnostrTrayIconClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = gnostr_tray_icon_dispose;
}

static void
gnostr_tray_icon_init(GnostrTrayIcon *self)
{
    self->helper = NULL;
    self->app = NULL;
    self->window = NULL;
}

gboolean
gnostr_tray_icon_is_available(void)
{
    /* Menu bar is always available on macOS */
    return TRUE;
}

GnostrTrayIcon *
gnostr_tray_icon_new(GtkApplication *app)
{
    g_return_val_if_fail(GTK_IS_APPLICATION(app), NULL);

    GnostrTrayIcon *self = g_object_new(GNOSTR_TYPE_TRAY_ICON, NULL);
    self->app = app;  /* weak reference, app outlives tray icon */

    @autoreleasepool {
        GnostrMenuBarHelper *helper = [[GnostrMenuBarHelper alloc] initWithApp:app];
        if (!helper) {
            g_warning("menubar-macos: Failed to create menu bar helper");
            g_object_unref(self);
            return NULL;
        }

        /* Transfer ownership to the C struct */
        self->helper = (__bridge_retained void *)helper;
    }

    g_message("menubar-macos: Menu bar icon created successfully");

    return self;
}

void
gnostr_tray_icon_set_window(GnostrTrayIcon *self, GtkWindow *window)
{
    g_return_if_fail(GNOSTR_IS_TRAY_ICON(self));
    g_return_if_fail(window == NULL || GTK_IS_WINDOW(window));

    self->window = window;  /* weak reference */

    if (self->helper) {
        @autoreleasepool {
            GnostrMenuBarHelper *helper = (__bridge GnostrMenuBarHelper *)self->helper;
            [helper setWindow:window];
        }
    }
}

void
gnostr_tray_icon_set_unread_count(GnostrTrayIcon *self, int count)
{
    g_return_if_fail(GNOSTR_IS_TRAY_ICON(self));

    if (self->helper) {
        @autoreleasepool {
            GnostrMenuBarHelper *helper = (__bridge GnostrMenuBarHelper *)self->helper;
            [helper setUnreadCount:count];
        }
    }
}

#endif /* __APPLE__ */
