#pragma once
#include <gtk/gtk.h>

G_BEGIN_DECLS

/* An app indicator shown only when a StatusNotifierWatcher exists. */
#define GN_TYPE_STATUS_NOTIFIER (gn_status_notifier_get_type())
G_DECLARE_FINAL_TYPE(GnStatusNotifier, gn_status_notifier, GN, STATUS_NOTIFIER, GObject)

typedef struct {
  gint id;                  /* positive, stable, never reused for another action */
  const gchar *label;       /* localized; NULL for a separator */
  gboolean enabled;
  gboolean visible;
  gboolean separator;
  gint checked;             /* -1: ordinary item; 0/1: checkmark */
} GnStatusNotifierItem;

/* Constructor and legacy activate/quit signals remain available to Gnostr.
 * The default menu contains Open (id 1) and Quit (id 2). */
GnStatusNotifier *gn_status_notifier_new(GApplication *app, const gchar *icon_name,
                                         const gchar *title);
gboolean gn_status_notifier_is_registered(GnStatusNotifier *self);

/* Replace the flat menu atomically. The notifier copies all items and emits
 * LayoutUpdated after the new snapshot is in place. "item-activated" carries
 * the clicked stable ID; hidden/disabled items cannot activate. */
void gn_status_notifier_set_menu_items(GnStatusNotifier *self,
                                       const GnStatusNotifierItem *items, gsize n_items);

G_END_DECLS
