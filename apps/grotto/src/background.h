#pragma once
#include <gio/gio.h>

G_BEGIN_DECLS
#define GROTTO_TYPE_BACKGROUND (grotto_background_get_type())
G_DECLARE_FINAL_TYPE(GrottoBackground, grotto_background, GROTTO, BACKGROUND, GObject)
typedef enum {
  GROTTO_BACKGROUND_DISABLED,
  GROTTO_BACKGROUND_REQUESTING,
  GROTTO_BACKGROUND_REGISTERED,
  GROTTO_BACKGROUND_UNAVAILABLE,
  GROTTO_BACKGROUND_DENIED
} GrottoBackgroundState;
/* GUI context only. The application owns this object and stops it at shutdown.
 * Portal absence permits native background operation, not sandbox operation.
 * No permission request precedes background-disclosure-accepted. */
GrottoBackground *grotto_background_new(GApplication *app, GDBusConnection *bus,
                                        GSettings *settings, gboolean native);
void grotto_background_stop(GrottoBackground *self);
GrottoBackgroundState grotto_background_get_state(GrottoBackground *self);
guint grotto_background_get_holds(GrottoBackground *self);
const char *grotto_background_get_description(GrottoBackground *self);
void grotto_background_set_status(GrottoBackground *self, gboolean waiting, gboolean paused);
void grotto_background_migrate_settings(GSettings *settings);
/* Writes only Grotto's entry in the caller's XDG autostart directory. */
gboolean grotto_background_sync_autostart(GSettings *settings, gboolean native, GError **error);
G_END_DECLS
