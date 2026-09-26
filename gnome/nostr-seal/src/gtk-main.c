/*
 * nostr-seal-gtk — double-click handler for application/vnd.nostr.sealed.
 * SPDX-License-Identifier: MIT
 *
 * Shows who a .nsealed file is sealed for, preselects the signer's active
 * identity (org.nostr.Signer.GetPublicKey) and opens the file through the
 * signer (NIP44DeriveConversationKey — the signer shows its own approval
 * prompt), or asks for the passphrase of a NIP-49 sealed file. The
 * plaintext lands next to the sealed file. Bead nostrc-da9c.
 */

#include <adwaita.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <openssl/crypto.h>

#include "nostr-seal.h"
#include "nseal-private.h"
#include "nseal-signer.h"

typedef struct {
  GtkWindow *win;
  AdwToastOverlay *toasts;
  AdwStatusPage *status;
  GtkWidget *form;          /* recipients / passphrase group */
  GtkWidget *decrypt_btn;
  AdwPasswordEntryRow *pass_row;
  GPtrArray *checks;        /* GtkCheckButton* per recipient stanza */
  GFile *file;
  NsealHeader *header;
  uint8_t active[32];
  gboolean have_active;
  gboolean busy;            /* a decrypt is running: keep the window */
  char *out_path;
} Win;

static void win_free(gpointer p) {
  Win *w = p;
  g_clear_object(&w->file);
  g_clear_pointer(&w->header, nseal_header_free);
  g_clear_pointer(&w->checks, g_ptr_array_unref);
  g_free(w->out_path);
  g_free(w);
}

static char *unique_output_path(const char *sealed) {
  g_autofree char *base = g_str_has_suffix(sealed, NSEAL_SUFFIX)
                            ? g_strndup(sealed, strlen(sealed) - strlen(NSEAL_SUFFIX))
                            : g_strconcat(sealed, ".opened", NULL);
  if (!g_file_test(base, G_FILE_TEST_EXISTS)) return g_steal_pointer(&base);
  g_autofree char *dir = g_path_get_dirname(base);
  g_autofree char *name = g_path_get_basename(base);
  const char *dot = strrchr(name, '.');
  g_autofree char *stem = dot && dot != name ? g_strndup(name, (gsize)(dot - name)) : g_strdup(name);
  const char *ext = dot && dot != name ? dot : "";
  for (int i = 1; i < 1000; i++) {
    g_autofree char *cand_name = g_strdup_printf("%s (%d)%s", stem, i, ext);
    char *cand = g_build_filename(dir, cand_name, NULL);
    if (!g_file_test(cand, G_FILE_TEST_EXISTS)) return cand;
    g_free(cand);
  }
  return NULL;
}

/* ─── Worker ─────────────────────────────────────────────────────────── */

typedef struct {
  char *in_path, *out_path, *identity, *passphrase;
  uint8_t who[32];
  gboolean use_signer;
} Job;

static void job_free(gpointer p) {
  Job *j = p;
  g_free(j->in_path); g_free(j->out_path); g_free(j->identity);
  if (j->passphrase) { OPENSSL_cleanse(j->passphrase, strlen(j->passphrase)); g_free(j->passphrase); }
  g_free(j);
}

static void decrypt_thread(GTask *task, gpointer src, gpointer data, GCancellable *c) {
  (void)src; (void)c;
  Job *j = data;
  GError *e = NULL;
  g_autoptr(NsealSigner) signer = NULL;
  NsealDecryptOptions opts = {0};
  if (j->use_signer) {
    if (!(signer = nseal_signer_new(j->identity, &e))) { g_task_return_error(task, e); return; }
    opts.identity_pubkey = j->who;
    opts.unwrap = nseal_signer_unwrap;
    opts.unwrap_data = signer;
  } else {
    opts.passphrase = j->passphrase;
  }
  int fd = open(j->in_path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    g_task_return_new_error(task, NSEAL_ERROR, NSEAL_ERROR_IO, "cannot open %s", j->in_path);
    return;
  }
  NsealOutput o;
  if (!nseal_output_open(&o, j->out_path, FALSE, TRUE, &e)) { close(fd); g_task_return_error(task, e); return; }
  gboolean ok = nseal_decrypt_fd(fd, o.fd, &opts, &e);
  close(fd);
  ok = nseal_output_close(&o, ok, ok ? &e : NULL) && ok;
  if (ok) g_task_return_boolean(task, TRUE);
  else g_task_return_error(task, e);
}

static void on_open_clicked(GtkButton *b, gpointer data) {
  (void)b;
  Win *w = data;
  g_autoptr(GFile) f = g_file_new_for_path(w->out_path);
  g_autoptr(GtkFileLauncher) l = gtk_file_launcher_new(f);
  gtk_file_launcher_launch(l, w->win, NULL, NULL, NULL);
}

static void on_show_clicked(GtkButton *b, gpointer data) {
  (void)b;
  Win *w = data;
  g_autoptr(GFile) f = g_file_new_for_path(w->out_path);
  g_autoptr(GtkFileLauncher) l = gtk_file_launcher_new(f);
  gtk_file_launcher_open_containing_folder(l, w->win, NULL, NULL, NULL);
}

static void on_decrypt_done(GObject *src, GAsyncResult *res, gpointer data) {
  (void)src;
  Win *w = data;
  GError *e = NULL;
  w->busy = FALSE;
  gtk_widget_set_sensitive(w->decrypt_btn, TRUE);
  gtk_widget_set_sensitive(w->form, TRUE);
  if (!g_task_propagate_boolean(G_TASK(res), &e)) {
    adw_toast_overlay_add_toast(w->toasts, adw_toast_new(e->message));
    adw_status_page_set_icon_name(w->status, "channel-secure-symbolic");
    g_error_free(e);
    return;
  }
  g_autofree char *name = g_path_get_basename(w->out_path);
  adw_status_page_set_icon_name(w->status, "emblem-ok-symbolic");
  adw_status_page_set_title(w->status, "Opened");
  adw_status_page_set_description(w->status, name);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
  GtkWidget *open = gtk_button_new_with_label("Open");
  gtk_widget_add_css_class(open, "suggested-action");
  gtk_widget_add_css_class(open, "pill");
  GtkWidget *show = gtk_button_new_with_label("Show in Files");
  gtk_widget_add_css_class(show, "pill");
  g_signal_connect(open, "clicked", G_CALLBACK(on_open_clicked), w);
  g_signal_connect(show, "clicked", G_CALLBACK(on_show_clicked), w);
  gtk_box_append(GTK_BOX(box), open);
  gtk_box_append(GTK_BOX(box), show);
  adw_status_page_set_child(w->status, box);
}

static void on_decrypt_clicked(GtkButton *b, gpointer data) {
  (void)b;
  Win *w = data;
  Job *j = g_new0(Job, 1);
  j->in_path = g_file_get_path(w->file);
  g_free(w->out_path);
  w->out_path = unique_output_path(j->in_path);
  j->out_path = g_strdup(w->out_path);
  if (!j->out_path) { job_free(j); return; }
  if (nseal_header_is_passphrase(w->header)) {
    const char *pw = gtk_editable_get_text(GTK_EDITABLE(w->pass_row));
    if (!pw || !*pw) { job_free(j); return; }
    j->passphrase = g_strdup(pw);
    gtk_editable_set_text(GTK_EDITABLE(w->pass_row), "");
  } else {
    gint sel = -1;
    for (guint i = 0; i < w->checks->len; i++)
      if (gtk_check_button_get_active(g_ptr_array_index(w->checks, i))) sel = (gint)i;
    if (sel < 0) { job_free(j); return; }
    memcpy(j->who, nseal_header_stanza_recipient(w->header, (gsize)sel), 32);
    j->use_signer = TRUE;
    /* The active identity goes through the signer's default selector; any
     * other recipient is named explicitly. */
    j->identity = (w->have_active && memcmp(j->who, w->active, 32) == 0)
                    ? g_strdup("") : nseal_pubkey_to_npub(j->who);
    adw_status_page_set_description(w->status, "Approve the request in your Nostr signer…");
  }
  gtk_widget_set_sensitive(w->decrypt_btn, FALSE);
  gtk_widget_set_sensitive(w->form, FALSE);
  w->busy = TRUE;
  GTask *t = g_task_new(w->win, NULL, on_decrypt_done, w);
  g_task_set_task_data(t, j, job_free);
  g_task_run_in_thread(t, decrypt_thread);
  g_object_unref(t);
}

/* ─── Window ─────────────────────────────────────────────────────────── */

static gboolean on_close_request(GtkWindow *win, gpointer data) {
  (void)win;
  Win *w = data;
  if (w->busy)
    adw_toast_overlay_add_toast(w->toasts, adw_toast_new("Still opening — answer the signer prompt first"));
  return w->busy;
}

static void open_window(AdwApplication *app, GFile *file) {
  Win *w = g_new0(Win, 1);
  w->file = g_object_ref(file);
  AdwApplicationWindow *win = ADW_APPLICATION_WINDOW(adw_application_window_new(GTK_APPLICATION(app)));
  w->win = GTK_WINDOW(win);
  g_object_set_data_full(G_OBJECT(win), "nseal-win", w, win_free);
  gtk_window_set_default_size(w->win, 520, 560);
  gtk_window_set_title(w->win, "Nostr Seal");
  g_signal_connect(w->win, "close-request", G_CALLBACK(on_close_request), w);

  AdwToolbarView *tv = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
  adw_toolbar_view_add_top_bar(tv, adw_header_bar_new());
  w->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
  w->status = ADW_STATUS_PAGE(adw_status_page_new());
  adw_status_page_set_icon_name(w->status, "channel-secure-symbolic");
  g_autofree char *base = g_file_get_basename(file);
  adw_status_page_set_title(w->status, base);
  adw_toast_overlay_set_child(w->toasts, GTK_WIDGET(w->status));
  adw_toolbar_view_set_content(tv, GTK_WIDGET(w->toasts));
  adw_application_window_set_content(win, GTK_WIDGET(tv));

  GError *e = NULL;
  g_autofree char *path = g_file_get_path(file);
  int fd = path ? open(path, O_RDONLY | O_CLOEXEC) : -1;
  if (fd >= 0) { w->header = nseal_header_read_fd(fd, &e); close(fd); }
  if (!w->header) {
    adw_status_page_set_icon_name(w->status, "dialog-error-symbolic");
    adw_status_page_set_description(w->status, e ? e->message : "cannot open the file");
    g_clear_error(&e);
    gtk_window_present(w->win);
    return;
  }

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  AdwPreferencesGroup *grp = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  w->form = GTK_WIDGET(grp);
  if (nseal_header_is_passphrase(w->header)) {
    adw_status_page_set_description(w->status, "Sealed with a passphrase");
    w->pass_row = ADW_PASSWORD_ENTRY_ROW(adw_password_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(w->pass_row), "Passphrase");
    adw_preferences_group_add(grp, GTK_WIDGET(w->pass_row));
  } else {
    g_autoptr(NsealSigner) s = nseal_signer_new(NULL, NULL);
    w->have_active = s && nseal_signer_public_key(s, w->active, NULL);
    gboolean active_listed = w->have_active && nseal_header_has_recipient(w->header, w->active);
    adw_preferences_group_set_title(grp, "Open as");
    adw_status_page_set_description(w->status, active_listed
      ? "Sealed for your Nostr identity"
      : "Your signer's active identity is not a recipient — pick the identity to open it as");
    w->checks = g_ptr_array_new();
    GtkCheckButton *first = NULL;
    for (gsize i = 0; i < nseal_header_n_stanzas(w->header); i++) {
      const uint8_t *pk = nseal_header_stanza_recipient(w->header, i);
      g_autofree char *npub = nseal_pubkey_to_npub(pk);
      AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), npub ? npub : "?");
      adw_action_row_set_title_lines(row, 1);
      gboolean is_active = w->have_active && memcmp(pk, w->active, 32) == 0;
      if (is_active) adw_action_row_set_subtitle(row, "active signer identity");
      GtkWidget *check = gtk_check_button_new();
      if (first) gtk_check_button_set_group(GTK_CHECK_BUTTON(check), first); else first = GTK_CHECK_BUTTON(check);
      gtk_check_button_set_active(GTK_CHECK_BUTTON(check), is_active || (!active_listed && i == 0));
      adw_action_row_add_prefix(row, check);
      adw_action_row_set_activatable_widget(row, check);
      adw_preferences_group_add(grp, GTK_WIDGET(row));
      g_ptr_array_add(w->checks, check);
    }
  }
  w->decrypt_btn = gtk_button_new_with_label("Decrypt");
  gtk_widget_add_css_class(w->decrypt_btn, "suggested-action");
  gtk_widget_add_css_class(w->decrypt_btn, "pill");
  gtk_widget_set_halign(w->decrypt_btn, GTK_ALIGN_CENTER);
  g_signal_connect(w->decrypt_btn, "clicked", G_CALLBACK(on_decrypt_clicked), w);
  if (w->pass_row) g_signal_connect_swapped(w->pass_row, "entry-activated", G_CALLBACK(gtk_widget_activate), w->decrypt_btn);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(grp));
  gtk_box_append(GTK_BOX(box), w->decrypt_btn);
  AdwClamp *clamp = ADW_CLAMP(adw_clamp_new());
  adw_clamp_set_child(clamp, box);
  adw_status_page_set_child(w->status, GTK_WIDGET(clamp));
  gtk_window_present(w->win);
}

static void on_open(GApplication *app, GFile **files, gint n, const gchar *hint, gpointer d) {
  (void)hint; (void)d;
  for (gint i = 0; i < n; i++) open_window(ADW_APPLICATION(app), files[i]);
}

static void on_activate(GApplication *app, gpointer d) {
  (void)d;
  GtkWidget *win = adw_application_window_new(GTK_APPLICATION(app));
  GtkWidget *sp = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(sp), "channel-secure-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(sp), "Nostr Seal");
  adw_status_page_set_description(ADW_STATUS_PAGE(sp),
    "Open a .nsealed file from Files, or use the nostr-seal command to seal one.");
  AdwToolbarView *tv = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
  adw_toolbar_view_add_top_bar(tv, adw_header_bar_new());
  adw_toolbar_view_set_content(tv, sp);
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(win), GTK_WIDGET(tv));
  gtk_window_set_default_size(GTK_WINDOW(win), 420, 360);
  gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char **argv) {
  g_autoptr(AdwApplication) app = adw_application_new("org.nostr.Seal", G_APPLICATION_HANDLES_OPEN);
  g_signal_connect(app, "open", G_CALLBACK(on_open), NULL);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
  return g_application_run(G_APPLICATION(app), argc, argv);
}
