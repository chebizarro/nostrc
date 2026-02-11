#ifndef GNOSTR_LABEL_GUARD_H
#define GNOSTR_LABEL_GUARD_H

#include <gtk/gtk.h>

/* nostrc-pgo3/pgo5: Guard against Pango SEGV when label's native surface is
 * gone. Use before gtk_label_set_text/gtk_label_set_markup in timer callbacks,
 * async completion handlers, dispose, and factory unbind. */
#define GNOSTR_LABEL_SAFE(lbl) \
  ((lbl) != NULL && GTK_IS_LABEL(lbl) && \
   gtk_widget_get_native(GTK_WIDGET(lbl)) != NULL)

#endif /* GNOSTR_LABEL_GUARD_H */
