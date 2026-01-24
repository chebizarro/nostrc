#ifndef GNOSTR_MAIN_WINDOW_H
#define GNOSTR_MAIN_WINDOW_H

#include <adwaita.h>

/* Forward declaration for GnostrNoteCardRow (avoid circular dependency) */
typedef struct _GnostrNoteCardRow GnostrNoteCardRow;

G_BEGIN_DECLS

#define GNOSTR_TYPE_MAIN_WINDOW (gnostr_main_window_get_type())

G_DECLARE_FINAL_TYPE(GnostrMainWindow, gnostr_main_window, GNOSTR, MAIN_WINDOW, AdwApplicationWindow)

GnostrMainWindow *gnostr_main_window_new(AdwApplication *app);

/* Demand-driven profile prefetch: enqueue author(s) by 64-hex pubkey */
void gnostr_main_window_enqueue_profile_author(GnostrMainWindow *self, const char *pubkey_hex);
void gnostr_main_window_enqueue_profile_authors(GnostrMainWindow *self, const char **pubkey_hexes, size_t count);

/* Public: Show a toast message in the main window (auto-dismisses after 2s) */
void gnostr_main_window_show_toast(GtkWidget *window, const char *message);

/* Public: Request a like/reaction (kind 7) for an event - NIP-25 */
void gnostr_main_window_request_like(GtkWidget *window, const char *id_hex, const char *pubkey_hex, gint event_kind, const char *reaction_content, GnostrNoteCardRow *row);

/* Public: Request deletion of a note (kind 5) - NIP-09 */
void gnostr_main_window_request_delete_note(GtkWidget *window, const char *id_hex, const char *pubkey_hex);

/* Public: Request a NIP-22 comment (kind 1111) on an event */
void gnostr_main_window_request_comment(GtkWidget *window, const char *id_hex, int kind, const char *pubkey_hex);

G_END_DECLS

#endif /* GNOSTR_MAIN_WINDOW_H */
