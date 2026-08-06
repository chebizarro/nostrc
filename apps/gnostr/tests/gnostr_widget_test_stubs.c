/* App shell callbacks that widget tests cannot exercise. */

#include <gtk/gtk.h>

void
gnostr_main_window_show_toast(GtkWidget *window, const char *message)
{
  (void)window;
  (void)message;
}
