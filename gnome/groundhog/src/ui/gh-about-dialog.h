#ifndef GH_ABOUT_DIALOG_H
#define GH_ABOUT_DIALOG_H

#include <adwaita.h>

G_BEGIN_DECLS

/* The About dialog (app.about in the primary menu): the app icon, the
 * version, the licence and the project links. It reads nothing but build-time
 * constants and contacts no network; links open in the browser only when the
 * user activates them. @parent may be NULL (the dialog is then free-standing). */
AdwDialog *gh_about_dialog_new(void);
void gh_about_dialog_present(GtkWidget *parent);

/* The resource path of the app icon's hicolor layout, registered with the
 * icon theme at startup (main.c) so "org.nostr.Groundhog" resolves without an
 * installed icon theme. */
#define GH_ICON_RESOURCE_PATH "/org/nostr/Groundhog/icons"
void gh_about_dialog_register_icons(void);

G_END_DECLS

#endif
