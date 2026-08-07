#ifndef GN_COMMUNIKEYS_COMPOSER_H
#define GN_COMMUNIKEYS_COMPOSER_H
#include <gtk/gtk.h>
G_BEGIN_DECLS
#define GN_TYPE_COMMUNIKEYS_COMPOSER (gn_communikeys_composer_get_type())
G_DECLARE_FINAL_TYPE(GnCommunikeysComposer, gn_communikeys_composer,
                     GN, COMMUNIKEYS_COMPOSER, GtkBox)
GnCommunikeysComposer *gn_communikeys_composer_new(void);
gchar *gn_communikeys_composer_get_text(GnCommunikeysComposer *self);
void gn_communikeys_composer_set_text(GnCommunikeysComposer *self,
                                      const char *text);
void gn_communikeys_composer_clear(GnCommunikeysComposer *self);
void gn_communikeys_composer_set_allowed_kinds(
    GnCommunikeysComposer *self, gboolean allow_kind_9,
    gboolean allow_kind_11);
void gn_communikeys_composer_set_send_sensitive(
    GnCommunikeysComposer *self, gboolean sensitive);
G_END_DECLS
#endif
