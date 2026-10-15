/* Standalone, not installed; no Groundhog data, signer, or keychain access.
 * Build: ninja -C build groundhog-drop-probe
 * Run: build/gnome/groundhog/groundhog-drop-probe [--preload]
 * Default matches the composer. --preload reproduces the old read-on-enter
 * timing; GTK 4.24's macOS private drop context can strand that read.
 */
#include <gtk/gtk.h>
#include <string.h>

static void
log_value(const gchar *stage, const GValue *value)
{
  if (!value) {
    g_print("drop-probe: %s value=(pending or cleared)\n", stage);
    return;
  }
  g_print("drop-probe: %s type=%s", stage, G_VALUE_TYPE_NAME(value));
  if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
    GdkFileList *list = g_value_get_boxed(value);
    GSList *files = list ? gdk_file_list_get_files(list) : NULL;
    g_print(" files=%u", g_slist_length(files));
    g_slist_free(files);
  } else if (G_VALUE_HOLDS(value, G_TYPE_FILE)) {
    g_print(" files=%u", g_value_get_object(value) ? 1 : 0);
  } else if (G_VALUE_HOLDS(value, G_TYPE_STRING)) {
    const gchar *text = g_value_get_string(value);
    g_auto(GStrv) uris = text ? g_uri_list_extract_uris(text) : NULL;
    g_print(" bytes=%zu uri-list-entries=%u", text ? strlen(text) : 0,
            uris ? g_strv_length(uris) : 0);
  }
  g_print("\n");
}

static GdkDragAction
on_enter(GtkDropTarget *target, gdouble x, gdouble y, gpointer data)
{
  (void)x;
  (void)y;
  (void)data;
  GdkDrop *drop = gtk_drop_target_get_current_drop(target);
  g_autofree gchar *formats = drop ?
    gdk_content_formats_to_string(gdk_drop_get_formats(drop)) : NULL;
  g_print("drop-probe: enter formats=%s preload=%d\n", formats ? formats : "(none)",
          gtk_drop_target_get_preload(target));
  log_value("enter", gtk_drop_target_get_value(target));
  return GDK_ACTION_COPY;
}

static void
on_value(GtkDropTarget *target, GParamSpec *pspec, gpointer data)
{
  (void)pspec;
  (void)data;
  log_value("read-result", gtk_drop_target_get_value(target));
}

static gboolean
on_drop(GtkDropTarget *target, const GValue *value, gdouble x, gdouble y, gpointer data)
{
  (void)target;
  (void)x;
  (void)y;
  (void)data;
  log_value("drop", value);
  return TRUE;
}

static void
on_leave(GtkDropTarget *target, gpointer data)
{
  (void)target;
  (void)data;
  g_print("drop-probe: leave\n");
}

static gboolean
on_close(GtkWindow *window, GMainLoop *loop)
{
  (void)window;
  g_main_loop_quit(loop);
  return TRUE;
}

int
main(int argc, char **argv)
{
  gboolean preload = argc == 2 && g_str_equal(argv[1], "--preload");
  if (argc > 1 && !preload) {
    g_printerr("Usage: %s [--preload]\n", argv[0]);
    return 2;
  }
  gtk_init();
  g_print("drop-probe: GTK %u.%u.%u backend=%s preload=%d\n",
          gtk_get_major_version(), gtk_get_minor_version(), gtk_get_micro_version(),
          G_OBJECT_TYPE_NAME(gdk_display_get_default()), preload);
  g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
  GtkWidget *window = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(window), "Groundhog file drop probe");
  gtk_window_set_default_size(GTK_WINDOW(window), 480, 240);
  gtk_window_set_child(GTK_WINDOW(window), gtk_label_new(
    "Drop a Finder/Files file here.\nRead results are printed in the terminal."));
  GtkDropTarget *target = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_COPY);
  GType types[] = { GDK_TYPE_FILE_LIST, G_TYPE_FILE, GDK_TYPE_TEXTURE, G_TYPE_STRING };
  gtk_drop_target_set_gtypes(target, types, G_N_ELEMENTS(types));
  gtk_drop_target_set_preload(target, preload);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(target), GTK_PHASE_CAPTURE);
  g_signal_connect(target, "enter", G_CALLBACK(on_enter), NULL);
  g_signal_connect(target, "notify::value", G_CALLBACK(on_value), NULL);
  g_signal_connect(target, "drop", G_CALLBACK(on_drop), NULL);
  g_signal_connect(target, "leave", G_CALLBACK(on_leave), NULL);
  gtk_widget_add_controller(window, GTK_EVENT_CONTROLLER(target));
  g_signal_connect(window, "close-request", G_CALLBACK(on_close), loop);
  gtk_window_present(GTK_WINDOW(window));
  g_main_loop_run(loop);
  gtk_window_destroy(GTK_WINDOW(window));
  return 0;
}
