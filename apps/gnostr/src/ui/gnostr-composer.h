#ifndef GNOSTR_COMPOSER_H
#define GNOSTR_COMPOSER_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_COMPOSER (gnostr_composer_get_type())

G_DECLARE_FINAL_TYPE(GnostrComposer, gnostr_composer, GNOSTR, COMPOSER, GtkWidget)

GtkWidget *gnostr_composer_new(void);

G_END_DECLS

#endif /* GNOSTR_COMPOSER_H */
