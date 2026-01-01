#ifndef GNOSTR_PROFILE_PANE_H
#define GNOSTR_PROFILE_PANE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_PROFILE_PANE (gnostr_profile_pane_get_type())
G_DECLARE_FINAL_TYPE(GnostrProfilePane, gnostr_profile_pane, GNOSTR, PROFILE_PANE, GtkWidget)

GnostrProfilePane *gnostr_profile_pane_new(void);
void gnostr_profile_pane_set_pubkey(GnostrProfilePane *self, const char *pubkey_hex);
void gnostr_profile_pane_clear(GnostrProfilePane *self);
void gnostr_profile_pane_update_from_json(GnostrProfilePane *self, const char *profile_json_str);

G_END_DECLS

#endif /* GNOSTR_PROFILE_PANE_H */
