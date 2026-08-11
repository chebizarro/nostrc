#ifndef GN_CONCORD_COMPOSER_H
#define GN_CONCORD_COMPOSER_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GN_TYPE_CONCORD_COMPOSER (gn_concord_composer_get_type())
G_DECLARE_FINAL_TYPE(GnConcordComposer, gn_concord_composer,
                     GN, CONCORD_COMPOSER, GtkBox)

GnConcordComposer *gn_concord_composer_new(void);
gchar *gn_concord_composer_get_text(GnConcordComposer *self);
void gn_concord_composer_set_text(GnConcordComposer *self, const char *text);
void gn_concord_composer_clear(GnConcordComposer *self);
void gn_concord_composer_set_send_sensitive(GnConcordComposer *self,
                                            gboolean sensitive);

G_END_DECLS
#endif
