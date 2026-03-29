#ifndef GNOSTR_MACOS_ACTIVATION_H
#define GNOSTR_MACOS_ACTIVATION_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

void gnostr_macos_activation_prepare_regular_policy(void);
void gnostr_macos_activation_request_foreground(GtkWindow *window);

G_END_DECLS

#endif /* GNOSTR_MACOS_ACTIVATION_H */
