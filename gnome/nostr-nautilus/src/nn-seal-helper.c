/* nn-seal-helper.c - "Encrypt for Nostr Contact…" dialog (nostrc-xlf3)
 *
 * SPDX-License-Identifier: MIT
 *
 *   nostr-nautilus-seal FILE…
 *
 * nostr-seal-gtk only opens sealed files, so this is the minimal recipient
 * entry for sealing: one row for npubs (space/comma separated), an "also
 * for me" switch, then `nostr-seal encrypt --to … [--to-self] FILE` per file.
 * nostr-seal does the cryptography and writes FILE.nsealed next to FILE;
 * its error text is shown verbatim.
 */
#include "nn-core.h"

#include <adwaita.h>

typedef struct {
  AdwApplicationWindow *win;
  GtkStack             *stack;
  AdwToastOverlay      *toasts;
  AdwEntryRow          *to_row;
  AdwSwitchRow         *self_row;
  GtkButton            *go;
  AdwStatusPage        *result;
  GPtrArray            *paths;   /* gchar* */
  gchar                *seal_exe;
  GStrv                 recipients;
  gboolean              to_self;
  guint                 next;
  guint                 ok;
  GString              *errors;
  GCancellable         *cancel;
} Win;

static void
win_free(gpointer p)
{
  Win *w = p;
  g_cancellable_cancel(w->cancel);
  g_clear_object(&w->cancel);
  g_ptr_array_unref(w->paths);
  g_free(w->seal_exe);
  g_strfreev(w->recipients);
  g_string_free(w->errors, TRUE);
  g_free(w);
}

static void run_next(Win *w);

static void
on_show_clicked(GtkButton *b, gpointer data)
{
  (void)b;
  Win *w = data;
  g_autofree gchar *first = g_strconcat(g_ptr_array_index(w->paths, 0), NN_SEALED_SUFFIX, NULL);
  g_autoptr(GFile) f = g_file_new_for_path(first);
  g_autoptr(GtkFileLauncher) l = gtk_file_launcher_new(f);
  gtk_file_launcher_open_containing_folder(l, GTK_WINDOW(w->win), NULL, NULL, NULL);
}

static void
on_close_clicked(GtkButton *b, gpointer data)
{
  (void)b;
  gtk_window_close(GTK_WINDOW(((Win *)data)->win));
}

static void
finish(Win *w)
{
  guint n = w->paths->len;
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_halign(box, GTK_ALIGN_CENTER);

  if (w->errors->len == 0) {
    adw_status_page_set_icon_name(w->result, "channel-secure-symbolic");
    adw_status_page_set_title(w->result, "Encrypted");
    g_autofree gchar *desc = n == 1
        ? g_strdup("Sealed as .nsealed next to the original")
        : g_strdup_printf("%u files sealed as .nsealed next to the originals", n);
    adw_status_page_set_description(w->result, desc);
  } else {
    adw_status_page_set_icon_name(w->result, "dialog-error-symbolic");
    adw_status_page_set_title(w->result, w->ok ? "Some Files Were Not Encrypted"
                                               : "Could Not Encrypt");
    g_autofree gchar *esc = g_markup_escape_text(w->errors->str, -1);
    adw_status_page_set_description(w->result, esc);
  }
  if (w->ok > 0) {
    GtkWidget *show = gtk_button_new_with_mnemonic("_Show in Files");
    gtk_widget_add_css_class(show, "pill");
    g_signal_connect(show, "clicked", G_CALLBACK(on_show_clicked), w);
    gtk_box_append(GTK_BOX(box), show);
  }
  GtkWidget *close = gtk_button_new_with_mnemonic("_Close");
  gtk_widget_add_css_class(close, "pill");
  gtk_widget_add_css_class(close, "suggested-action");
  g_signal_connect(close, "clicked", G_CALLBACK(on_close_clicked), w);
  gtk_box_append(GTK_BOX(box), close);
  adw_status_page_set_child(w->result, box);
  gtk_stack_set_visible_child_name(w->stack, "result");
}

static void
on_seal_done(GObject *src, GAsyncResult *res, gpointer data)
{
  Win *w = data;
  GSubprocess *proc = G_SUBPROCESS(src);
  g_autofree gchar *err_out = NULL;
  g_autoptr(GError) e = NULL;
  gboolean done = g_subprocess_communicate_utf8_finish(proc, res, NULL, &err_out, &e);
  if (!done && g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return; /* window gone: @w is freed */

  g_autofree gchar *base = g_path_get_basename(g_ptr_array_index(w->paths, w->next));
  if (!done) {
    g_string_append_printf(w->errors, "%s: %s\n", base, e->message);
  } else if (!g_subprocess_get_successful(proc)) {
    g_strstrip(err_out);
    g_string_append_printf(w->errors, "%s: %s\n", base,
                           err_out && *err_out ? err_out : "nostr-seal failed");
  } else {
    w->ok++;
  }
  w->next++;
  run_next(w);
}

static void
run_next(Win *w)
{
  if (w->next >= w->paths->len) {
    finish(w);
    return;
  }
  const gchar *path = g_ptr_array_index(w->paths, w->next);
  g_auto(GStrv) argv = nn_seal_encrypt_argv(w->seal_exe, (const gchar *const *)w->recipients,
                                            w->to_self, path);
  g_autoptr(GError) e = NULL;
  g_autoptr(GSubprocess) proc = g_subprocess_newv((const gchar *const *)argv,
                                                  G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                                  G_SUBPROCESS_FLAGS_STDERR_PIPE, &e);
  if (proc == NULL) {
    g_autofree gchar *base = g_path_get_basename(path);
    g_string_append_printf(w->errors, "%s: %s\n", base, e->message);
    w->next++;
    run_next(w);
    return;
  }
  g_subprocess_communicate_utf8_async(proc, NULL, w->cancel, on_seal_done, w);
}

static gboolean
busy(const Win *w)
{
  return w->recipients != NULL && w->next < w->paths->len;
}

static gboolean
on_close_request(GtkWindow *win, gpointer data)
{
  (void)win;
  Win *w = data;
  if (!busy(w))
    return FALSE;
  adw_toast_overlay_add_toast(w->toasts,
                              adw_toast_new("Still encrypting — answer the signer prompt first"));
  return TRUE;
}

static void
on_encrypt(GtkWidget *widget, gpointer data)
{
  (void)widget;
  Win *w = data;
  if (w->recipients != NULL)
    return; /* already running or done */
  g_autoptr(GError) e = NULL;
  g_auto(GStrv) keys = NULL;
  if (!nn_parse_recipients(gtk_editable_get_text(GTK_EDITABLE(w->to_row)), &keys, &e)) {
    adw_toast_overlay_add_toast(w->toasts, adw_toast_new(e->message));
    return;
  }
  gboolean to_self = adw_switch_row_get_active(w->self_row);
  if (g_strv_length(keys) == 0 && !to_self) {
    adw_toast_overlay_add_toast(w->toasts, adw_toast_new("Add at least one npub"));
    return;
  }
  w->recipients = g_steal_pointer(&keys);
  w->to_self = to_self;
  gtk_widget_set_sensitive(GTK_WIDGET(w->go), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(w->to_row), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(w->self_row), FALSE);
  if (to_self)
    adw_toast_overlay_add_toast(w->toasts,
                                adw_toast_new("Your signer may ask to share your public key…"));
  run_next(w);
}

static void
on_to_changed(GtkEditable *editable, gpointer data)
{
  (void)data;
  g_auto(GStrv) keys = NULL;
  gboolean ok = nn_parse_recipients(gtk_editable_get_text(editable), &keys, NULL);
  if (ok)
    gtk_widget_remove_css_class(GTK_WIDGET(editable), "error");
  else
    gtk_widget_add_css_class(GTK_WIDGET(editable), "error");
}

static gchar *
files_summary(GPtrArray *paths)
{
  GString *s = g_string_new(NULL);
  guint shown = MIN(paths->len, 3u);
  for (guint i = 0; i < shown; i++) {
    g_autofree gchar *b = g_path_get_basename(g_ptr_array_index(paths, i));
    g_string_append_printf(s, "%s%s", i ? ", " : "", b);
  }
  if (paths->len > shown)
    g_string_append_printf(s, " and %u more", paths->len - shown);
  gchar *esc = g_markup_escape_text(s->str, -1);
  g_string_free(s, TRUE);
  return esc;
}

static void
open_window(AdwApplication *app, GPtrArray *paths)
{
  Win *w = g_new0(Win, 1);
  w->paths = paths;
  w->errors = g_string_new(NULL);
  w->cancel = g_cancellable_new();
  w->seal_exe = g_find_program_in_path("nostr-seal");

  w->win = ADW_APPLICATION_WINDOW(adw_application_window_new(GTK_APPLICATION(app)));
  g_object_set_data_full(G_OBJECT(w->win), "nn-win", w, win_free);
  gtk_window_set_title(GTK_WINDOW(w->win), "Encrypt for Nostr Contact");
  gtk_window_set_default_size(GTK_WINDOW(w->win), 520, 460);
  g_signal_connect(w->win, "close-request", G_CALLBACK(on_close_request), w);

  AdwToolbarView *tv = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
  AdwHeaderBar *hb = ADW_HEADER_BAR(adw_header_bar_new());
  w->go = GTK_BUTTON(gtk_button_new_with_mnemonic("_Encrypt"));
  gtk_widget_add_css_class(GTK_WIDGET(w->go), "suggested-action");
  g_signal_connect(w->go, "clicked", G_CALLBACK(on_encrypt), w);
  adw_header_bar_pack_end(hb, GTK_WIDGET(w->go));
  adw_toolbar_view_add_top_bar(tv, GTK_WIDGET(hb));

  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
  AdwPreferencesGroup *files = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(files, paths->len == 1 ? "File" : "Files");
  g_autofree gchar *summary = files_summary(paths);
  adw_preferences_group_set_description(files, summary);
  adw_preferences_page_add(page, files);

  AdwPreferencesGroup *grp = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(grp, "Recipients");
  adw_preferences_group_set_description(grp,
      "Only these keys' holders can open the result (FILE.nsealed, next to each file). "
      "The sender stays anonymous.");
  w->to_row = ADW_ENTRY_ROW(adw_entry_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(w->to_row), "npub1… (separate several with spaces)");
  g_signal_connect(w->to_row, "changed", G_CALLBACK(on_to_changed), NULL);
  g_signal_connect(w->to_row, "entry-activated", G_CALLBACK(on_encrypt), w);
  adw_preferences_group_add(grp, GTK_WIDGET(w->to_row));
  w->self_row = ADW_SWITCH_ROW(adw_switch_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(w->self_row), "Also Encrypt for Me");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(w->self_row),
                              "Your signer's active identity can open it too");
  adw_switch_row_set_active(w->self_row, TRUE);
  adw_preferences_group_add(grp, GTK_WIDGET(w->self_row));
  adw_preferences_page_add(page, grp);

  w->result = ADW_STATUS_PAGE(adw_status_page_new());
  w->stack = GTK_STACK(gtk_stack_new());
  gtk_stack_add_named(w->stack, GTK_WIDGET(page), "form");
  gtk_stack_add_named(w->stack, GTK_WIDGET(w->result), "result");
  w->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
  adw_toast_overlay_set_child(w->toasts, GTK_WIDGET(w->stack));
  adw_toolbar_view_set_content(tv, GTK_WIDGET(w->toasts));
  adw_application_window_set_content(w->win, GTK_WIDGET(tv));

  if (w->seal_exe == NULL) {
    adw_status_page_set_icon_name(w->result, "dialog-error-symbolic");
    adw_status_page_set_title(w->result, "nostr-seal Is Not Installed");
    adw_status_page_set_description(w->result, "Install the nostr-seal package to encrypt files.");
    gtk_stack_set_visible_child_name(w->stack, "result");
    gtk_widget_set_sensitive(GTK_WIDGET(w->go), FALSE);
  }
  gtk_window_present(GTK_WINDOW(w->win));
}

static void
on_open(GApplication *app, GFile **files, gint n, const gchar *hint, gpointer data)
{
  (void)hint; (void)data;
  GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
  for (gint i = 0; i < n; i++) {
    gchar *p = g_file_get_path(files[i]);
    if (p != NULL)
      g_ptr_array_add(paths, p);
    else {
      g_autofree gchar *uri = g_file_get_uri(files[i]);
      g_printerr("nostr-nautilus-seal: %s is not a local file; skipped\n", uri);
    }
  }
  if (paths->len == 0) {
    g_ptr_array_unref(paths);
    return;
  }
  open_window(ADW_APPLICATION(app), paths);
}

static void
on_activate(GApplication *app, gpointer data)
{
  (void)app; (void)data;
  g_printerr("usage: nostr-nautilus-seal FILE…\n");
}

int
main(int argc, char **argv)
{
  g_autoptr(AdwApplication) app = adw_application_new("org.nostr.Nautilus.Seal",
                                                      G_APPLICATION_HANDLES_OPEN |
                                                      G_APPLICATION_NON_UNIQUE);
  g_signal_connect(app, "open", G_CALLBACK(on_open), NULL);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
  return g_application_run(G_APPLICATION(app), argc, argv);
}
