/* ns-dialog.c - libadwaita "Share to Nostr" window
 *
 * SPDX-License-Identifier: MIT
 *
 * One window per invocation (the app is NON_UNIQUE: every Open With is
 * its own share). Network work (signer, relay/server lookup, upload,
 * publish) runs in GTask threads; the widgets that edit the NsShare are
 * insensitive while a worker owns it.
 *
 * "Show the exact event JSON before signing": the expander always shows
 * the compact JSON the signer will receive, pretty-printed. If a Blossom
 * server returns a URL other than the predicted one, the upload step
 * stops, the JSON is refreshed and the user must press Publish again.
 *
 * Needs libadwaita >= 1.4 (AdwToolbarView, AdwSwitchRow).
 */
#include "ns-dialog.h"
#include "ns-strip.h"

#include <adwaita.h>

typedef enum {
  STATE_RESOLVING,
  STATE_READY,
  STATE_BUSY,
  STATE_DONE,
  STATE_FAILED_LOAD,
} DialogState;

typedef struct {
  AdwApplication *app;
  GtkWindow      *window;
  NsShare        *share;
  DialogState     state;
  int             exit_code;
  gboolean        resolve_ok;
  gchar          *resolve_error;
  gchar          *shown_json;    /* what the user reviewed */
  gint           *kinds;         /* picker index → kind */
  guint           n_kinds;
  gboolean        updating;      /* suppress signal recursion */

  AdwWindowTitle *title;
  GtkButton      *publish;
  GtkSpinner     *spinner;
  AdwBanner      *banner;
  AdwComboRow    *kind_row;
  AdwEntryRow    *to_row;
  AdwEntryRow    *title_row;
  GtkTextView    *text;
  GtkLabel       *text_label;
  AdwPreferencesGroup *meta_group;
  AdwSwitchRow   *keep_meta;
  AdwActionRow   *relays_row;
  AdwActionRow   *servers_row;
  GtkTextView    *json;
  GtkLabel       *status;
  GtkWidget      *editables;     /* box holding editable groups */
} Dialog;

static void refresh(Dialog *d);

/* ---- helpers ---- */

static void
set_status(Dialog *d, const gchar *msg, gboolean error)
{
  gtk_label_set_text(d->status, msg ? msg : "");
  if (error)
    gtk_widget_add_css_class(GTK_WIDGET(d->status), "error");
  else
    gtk_widget_remove_css_class(GTK_WIDGET(d->status), "error");
}

static gchar *
text_view_get(GtkTextView *tv)
{
  GtkTextBuffer *b = gtk_text_view_get_buffer(tv);
  GtkTextIter s, e;
  gtk_text_buffer_get_bounds(b, &s, &e);
  return gtk_text_buffer_get_text(b, &s, &e, FALSE);
}

static void
set_busy(Dialog *d, gboolean busy)
{
  gtk_spinner_set_spinning(d->spinner, busy);
  gtk_widget_set_visible(GTK_WIDGET(d->spinner), busy);
  gtk_widget_set_sensitive(d->editables, !busy && d->state != STATE_DONE);
}

static gboolean
media_blocked(Dialog *d)
{
  return ns_share_metadata_blocked(d->share, NULL) &&
         !adw_switch_row_get_active(d->keep_meta);
}

static void
update_publish_sensitivity(Dialog *d)
{
  gboolean ok = d->state == STATE_DONE ||
                (d->state == STATE_READY && d->resolve_ok && d->shown_json != NULL &&
                 !media_blocked(d));
  gtk_widget_set_sensitive(GTK_WIDGET(d->publish), ok);
}

/* Rebuild the event(s) from the current NsShare and show them. */
static void
refresh(Dialog *d)
{
  GError *err = NULL;
  g_clear_pointer(&d->shown_json, g_free);
  GtkTextBuffer *jb = gtk_text_view_get_buffer(d->json);
  if (ns_share_build(d->share, &err)) {
    d->shown_json = ns_share_preview_json(d->share, FALSE);
    gtk_text_buffer_set_text(jb, d->shown_json, -1);
    if (d->state == STATE_READY)
      set_status(d, d->resolve_ok ? "" : d->resolve_error, !d->resolve_ok);
  } else {
    gtk_text_buffer_set_text(jb, "", -1);
    if (d->state == STATE_READY)
      set_status(d, err->message, TRUE);
    g_clear_error(&err);
  }

  NsPost *p = g_ptr_array_index(d->share->posts, 0);
  gtk_widget_set_visible(GTK_WIDGET(d->title_row), p->action == NS_ACTION_ARTICLE);
  gboolean media = p->action == NS_ACTION_MEDIA_NOTE ||
                   p->action == NS_ACTION_FILE_METADATA;
  gtk_label_set_text(d->text_label, media ? "Caption" : "Text");

  g_autofree gchar *targets = ns_share_describe_targets(d->share);
  adw_action_row_set_subtitle(d->relays_row, targets);
  g_autofree gchar *servers = ns_share_describe_servers(d->share);
  adw_action_row_set_subtitle(d->servers_row, servers);
  gtk_widget_set_visible(GTK_WIDGET(d->servers_row), ns_share_needs_upload(d->share));

  gboolean blocked = ns_share_metadata_blocked(d->share, NULL);
  gtk_widget_set_visible(GTK_WIDGET(d->keep_meta), blocked);
  adw_banner_set_revealed(d->banner, blocked && !adw_switch_row_get_active(d->keep_meta));
  update_publish_sensitivity(d);
}

static void
fill_kind_picker(Dialog *d)
{
  d->updating = TRUE;
  gint kinds[3];
  guint n = ns_kind_choices(ns_share_primary_class(d->share), kinds);
  g_free(d->kinds);
  d->kinds = g_memdup2(kinds, sizeof(gint) * (n ? n : 1));
  d->n_kinds = n;
  GtkStringList *model = gtk_string_list_new(NULL);
  guint selected = 0;
  gint cur = ns_share_primary_kind(d->share);
  for (guint i = 0; i < n; i++) {
    gtk_string_list_append(model, ns_kind_label(kinds[i]));
    if (kinds[i] == cur)
      selected = i;
  }
  adw_combo_row_set_model(d->kind_row, G_LIST_MODEL(model));
  adw_combo_row_set_selected(d->kind_row, selected);
  g_object_unref(model);
  gtk_widget_set_visible(GTK_WIDGET(d->kind_row), n > 1);
  d->updating = FALSE;
}

/* ---- edits ---- */

static void
on_kind_selected(GObject *row, GParamSpec *pspec, gpointer user_data)
{
  (void)row; (void)pspec;
  Dialog *d = user_data;
  if (d->updating || d->state != STATE_READY)
    return;
  guint i = adw_combo_row_get_selected(d->kind_row);
  if (i >= d->n_kinds)
    return;
  GError *err = NULL;
  if (!ns_share_set_kind(d->share, d->kinds[i], &err)) {
    set_status(d, err->message, TRUE);
    g_clear_error(&err);
  }
  refresh(d);
}

static void
on_text_changed(GtkTextBuffer *buf, gpointer user_data)
{
  (void)buf;
  Dialog *d = user_data;
  if (d->updating || d->state != STATE_READY)
    return;
  g_autofree gchar *t = text_view_get(d->text);
  GError *err = NULL;
  if (!ns_share_set_text(d->share, t, &err)) {
    set_status(d, err->message, TRUE);
    g_clear_error(&err);
  }
  refresh(d);
}

static void
on_title_changed(GtkEditable *e, gpointer user_data)
{
  Dialog *d = user_data;
  if (d->updating || d->state != STATE_READY)
    return;
  g_free(d->share->title);
  const gchar *t = gtk_editable_get_text(e);
  d->share->title = *t ? g_strdup(t) : NULL;
  (void)ns_share_set_text(d->share, d->share->text, NULL);   /* replan */
  refresh(d);
}

static void
on_to_apply(AdwEntryRow *row, gpointer user_data)
{
  Dialog *d = user_data;
  if (d->state != STATE_READY)
    return;
  NsRecipient r;
  GError *err = NULL;
  if (!ns_recipient_parse(gtk_editable_get_text(GTK_EDITABLE(row)), &r, &err)) {
    set_status(d, err->message, TRUE);
    g_clear_error(&err);
    return;
  }
  gboolean group_changed = r.type == NS_RECIPIENT_GROUP ||
                           d->share->to.type == NS_RECIPIENT_GROUP;
  ns_recipient_clear(&d->share->to);
  d->share->to = r;
  if (group_changed) {
    /* Group posts go only to the group relay. Resolving that is local;
     * going back to the user's relays needs a network lookup, which the
     * Publish worker does (never on the UI thread). */
    ns_targets_clear(&d->share->targets);
    d->share->resolved = FALSE;
    if (r.type == NS_RECIPIENT_GROUP)
      (void)ns_resolve_targets(d->share->cfg, &d->share->net, d->share->pubkey_hex,
                               &d->share->to, &d->share->targets, NULL);
  }
  refresh(d);
}

static void
on_keep_meta(GObject *row, GParamSpec *pspec, gpointer user_data)
{
  (void)row; (void)pspec;
  Dialog *d = user_data;
  refresh(d);
}

/* ---- worker plumbing ---- */

typedef struct {
  Dialog *d;
  gchar  *msg;
} Progress;

static gboolean
progress_idle(gpointer data)
{
  Progress *p = data;
  set_status(p->d, p->msg, FALSE);
  g_free(p->msg);
  g_free(p);
  return G_SOURCE_REMOVE;
}

static void
progress_cb(const gchar *msg, gpointer user_data)
{
  Progress *p = g_new0(Progress, 1);
  p->d = user_data;
  p->msg = g_strdup(msg);
  g_main_context_invoke(NULL, progress_idle, p);
}

/* resolve */

static void
resolve_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
  (void)src; (void)c;
  Dialog *d = data;
  GError *err = NULL;
  if (!ns_share_connect(d->share, &err) || !ns_share_resolve(d->share, &err)) {
    g_task_return_error(task, err);
    return;
  }
  g_task_return_boolean(task, TRUE);
}

static void
resolve_done(GObject *src, GAsyncResult *res, gpointer data)
{
  (void)src;
  Dialog *d = data;
  GError *err = NULL;
  d->state = STATE_READY;
  d->resolve_ok = g_task_propagate_boolean(G_TASK(res), &err);
  g_clear_pointer(&d->resolve_error, g_free);
  if (!d->resolve_ok) {
    d->resolve_error = g_strdup(err->message);
    g_clear_error(&err);
  }
  if (d->share->pubkey_hex != NULL) {
    g_autofree gchar *sub = g_strdup_printf("as %.12s…", d->share->pubkey_hex);
    adw_window_title_set_subtitle(d->title, sub);
  } else {
    adw_window_title_set_subtitle(d->title, "no signer");
  }
  set_busy(d, FALSE);
  refresh(d);
  if (!d->resolve_ok)
    set_status(d, d->resolve_error, TRUE);
}

/* publish */

typedef enum { PUB_OK, PUB_REVIEW } PubOutcome;

static void
publish_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
  (void)src; (void)c;
  Dialog *d = data;
  GError *err = NULL;
  gboolean changed = FALSE;
  if (!ns_share_upload(d->share, &changed, progress_cb, d, &err)) {
    g_task_return_error(task, err);
    return;
  }
  if (!ns_share_build(d->share, &err)) {
    g_task_return_error(task, err);
    return;
  }
  g_autofree gchar *now = ns_share_preview_json(d->share, FALSE);
  if (changed || g_strcmp0(now, d->shown_json) != 0) {
    g_task_return_int(task, PUB_REVIEW);
    return;
  }
  if (!ns_share_publish(d->share, progress_cb, d, &err)) {
    g_task_return_error(task, err);
    return;
  }
  g_task_return_int(task, PUB_OK);
}

static void
publish_done(GObject *src, GAsyncResult *res, gpointer data)
{
  (void)src;
  Dialog *d = data;
  GError *err = NULL;
  gssize outcome = g_task_propagate_int(G_TASK(res), &err);
  if (err != NULL) {
    d->state = STATE_READY;
    set_busy(d, FALSE);
    set_status(d, err->message, TRUE);
    d->exit_code = 4;
    g_clear_error(&err);
    update_publish_sensitivity(d);
    return;
  }
  if (outcome == PUB_REVIEW) {
    d->state = STATE_READY;
    set_busy(d, FALSE);
    refresh(d);
    set_status(d, "The Blossom server returned a different URL. The event "
                  "below has been updated — review it and press Publish again.",
               FALSE);
    return;
  }

  d->state = STATE_DONE;
  d->exit_code = 0;
  set_busy(d, FALSE);
  GString *s = g_string_new("Shared.");
  for (guint i = 0; i < d->share->posts->len; i++) {
    NsPost *p = g_ptr_array_index(d->share->posts, i);
    if (p->result)
      g_string_append_printf(s, "\n%s", p->result);
  }
  set_status(d, s->str, FALSE);
  g_string_free(s, TRUE);
  g_autofree gchar *signed_json = ns_share_preview_json(d->share, TRUE);
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(d->json), signed_json, -1);
  gtk_button_set_label(d->publish, "Done");
  gtk_widget_remove_css_class(GTK_WIDGET(d->publish), "suggested-action");
  update_publish_sensitivity(d);
}

static void
on_publish(GtkButton *b, gpointer user_data)
{
  (void)b;
  Dialog *d = user_data;
  if (d->state == STATE_DONE || d->state == STATE_FAILED_LOAD) {
    gtk_window_close(d->window);
    return;
  }
  if (d->state != STATE_READY)
    return;
  d->share->keep_metadata = adw_switch_row_get_active(d->keep_meta);
  d->state = STATE_BUSY;
  set_busy(d, TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(d->publish), FALSE);
  set_status(d, "Publishing…", FALSE);
  GTask *task = g_task_new(NULL, NULL, publish_done, d);
  g_task_set_task_data(task, d, NULL);
  g_task_run_in_thread(task, publish_thread);
  g_object_unref(task);
}

static void
on_cancel(GtkButton *b, gpointer user_data)
{
  (void)b;
  Dialog *d = user_data;
  if (d->state == STATE_BUSY)
    return;    /* a signer prompt / upload is in flight */
  gtk_window_close(d->window);
}

/* ---- construction ---- */

static GtkWidget *
media_previews(Dialog *d)
{
  GtkWidget *flow = gtk_flow_box_new();
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 6);
  guint n = 0;
  for (guint i = 0; i < d->share->files->len; i++) {
    NsFile *f = g_ptr_array_index(d->share->files, i);
    if (f->cls != NS_CLASS_MEDIA && f->cls != NS_CLASS_OTHER_FILE)
      continue;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *thumb = NULL;
    if (f->cls == NS_CLASS_MEDIA && ns_strip_supported(f->blob.mime)) {
      g_autoptr(GdkTexture) tex = gdk_texture_new_from_bytes(f->bytes, NULL);
      if (tex != NULL) {
        thumb = gtk_picture_new_for_paintable(GDK_PAINTABLE(tex));
        gtk_picture_set_content_fit(GTK_PICTURE(thumb), GTK_CONTENT_FIT_COVER);
      }
    }
    if (thumb == NULL) {
      g_autofree gchar *ct = g_content_type_from_mime_type(f->blob.mime);
      g_autoptr(GIcon) icon = g_content_type_get_icon(ct ? ct : "application/octet-stream");
      thumb = gtk_image_new_from_gicon(icon);
      gtk_image_set_pixel_size(GTK_IMAGE(thumb), 64);
    }
    gtk_widget_set_size_request(thumb, 112, 112);
    gtk_widget_add_css_class(thumb, "card");
    gtk_box_append(GTK_BOX(box), thumb);
    GtkWidget *label = gtk_label_new(f->display_name);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 14);
    gtk_widget_add_css_class(label, "caption");
    gtk_box_append(GTK_BOX(box), label);
    gtk_flow_box_append(GTK_FLOW_BOX(flow), box);
    n++;
  }
  if (n == 0) {
    g_object_ref_sink(flow);
    g_object_unref(flow);
    return NULL;
  }
  return flow;
}

static void
add_metadata_rows(Dialog *d)
{
  guint n = 0;
  for (guint i = 0; i < d->share->files->len; i++) {
    NsFile *f = g_ptr_array_index(d->share->files, i);
    if (f->cls != NS_CLASS_MEDIA)
      continue;
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), f->display_name);
    g_autofree gchar *sub = f->stripped
      ? g_strdup_printf("Removed %u metadata block%s (EXIF/XMP/comments)",
                        f->n_meta_removed, f->n_meta_removed == 1 ? "" : "s")
      : g_strdup_printf("Metadata NOT removed: %s can carry GPS, camera and "
                        "time data that nostr-share cannot strip", f->blob.mime);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
    adw_preferences_group_add(d->meta_group, row);
    n++;
  }
  gtk_widget_set_visible(GTK_WIDGET(d->meta_group), n > 0);
}

static void
build_window(Dialog *d, const GError *load_error)
{
  AdwApplicationWindow *win = ADW_APPLICATION_WINDOW(
    adw_application_window_new(GTK_APPLICATION(d->app)));
  d->window = GTK_WINDOW(win);
  gtk_window_set_default_size(d->window, 560, 720);
  gtk_window_set_title(d->window, "Share to Nostr");

  GtkWidget *tv = adw_toolbar_view_new();
  GtkWidget *hb = adw_header_bar_new();
  adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(hb), FALSE);
  adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(hb), FALSE);
  d->title = ADW_WINDOW_TITLE(adw_window_title_new("Share to Nostr", ""));
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(hb), GTK_WIDGET(d->title));
  GtkWidget *cancel = gtk_button_new_with_label("Cancel");
  g_signal_connect(cancel, "clicked", G_CALLBACK(on_cancel), d);
  adw_header_bar_pack_start(ADW_HEADER_BAR(hb), cancel);
  d->publish = GTK_BUTTON(gtk_button_new_with_label("Publish"));
  gtk_widget_add_css_class(GTK_WIDGET(d->publish), "suggested-action");
  g_signal_connect(d->publish, "clicked", G_CALLBACK(on_publish), d);
  adw_header_bar_pack_end(ADW_HEADER_BAR(hb), GTK_WIDGET(d->publish));
  d->spinner = GTK_SPINNER(gtk_spinner_new());
  adw_header_bar_pack_end(ADW_HEADER_BAR(hb), GTK_WIDGET(d->spinner));
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), hb);

  d->banner = ADW_BANNER(adw_banner_new(
    "Some media keeps its metadata (location, device, time)"));
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), GTK_WIDGET(d->banner));

  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  GtkWidget *clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 640);
  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_margin_top(outer, 12);
  gtk_widget_set_margin_bottom(outer, 18);
  gtk_widget_set_margin_start(outer, 12);
  gtk_widget_set_margin_end(outer, 12);
  adw_clamp_set_child(ADW_CLAMP(clamp), outer);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), scroll);
  adw_application_window_set_content(win, tv);

  d->status = GTK_LABEL(gtk_label_new(""));
  gtk_label_set_wrap(d->status, TRUE);
  gtk_label_set_selectable(d->status, TRUE);
  gtk_label_set_xalign(d->status, 0);

  if (d->share == NULL) {
    d->state = STATE_FAILED_LOAD;
    gtk_box_append(GTK_BOX(outer), GTK_WIDGET(d->status));
    set_status(d, load_error ? load_error->message : "nothing to share", TRUE);
    gtk_button_set_label(d->publish, "Close");
    d->exit_code = 2;
    return;
  }

  d->editables = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  gtk_box_append(GTK_BOX(outer), d->editables);

  GtkWidget *previews = media_previews(d);
  if (previews != NULL)
    gtk_box_append(GTK_BOX(d->editables), previews);

  GtkWidget *post = adw_preferences_group_new();
  d->kind_row = ADW_COMBO_ROW(adw_combo_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(d->kind_row), "Publish as");
  g_signal_connect(d->kind_row, "notify::selected", G_CALLBACK(on_kind_selected), d);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(post), GTK_WIDGET(d->kind_row));
  d->title_row = ADW_ENTRY_ROW(adw_entry_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(d->title_row), "Title");
  if (d->share->title)
    gtk_editable_set_text(GTK_EDITABLE(d->title_row), d->share->title);
  g_signal_connect(d->title_row, "changed", G_CALLBACK(on_title_changed), d);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(post), GTK_WIDGET(d->title_row));
  d->to_row = ADW_ENTRY_ROW(adw_entry_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(d->to_row),
                                "Mention (public) or group host'id");
  adw_entry_row_set_show_apply_button(d->to_row, TRUE);
  if (d->share->to.type == NS_RECIPIENT_MENTION)
    gtk_editable_set_text(GTK_EDITABLE(d->to_row), d->share->to.npub);
  else if (d->share->to.type == NS_RECIPIENT_GROUP) {
    g_autofree gchar *g = g_strdup_printf("%s'%s", d->share->to.relay_url + 6,
                                          d->share->to.group_id);
    gtk_editable_set_text(GTK_EDITABLE(d->to_row), g);
  }
  g_signal_connect(d->to_row, "apply", G_CALLBACK(on_to_apply), d);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(post), GTK_WIDGET(d->to_row));
  gtk_box_append(GTK_BOX(d->editables), post);

  GtkWidget *tbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  d->text_label = GTK_LABEL(gtk_label_new("Text"));
  gtk_label_set_xalign(d->text_label, 0);
  gtk_widget_add_css_class(GTK_WIDGET(d->text_label), "heading");
  gtk_box_append(GTK_BOX(tbox), GTK_WIDGET(d->text_label));
  d->text = GTK_TEXT_VIEW(gtk_text_view_new());
  gtk_text_view_set_wrap_mode(d->text, GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_top_margin(d->text, 8);
  gtk_text_view_set_bottom_margin(d->text, 8);
  gtk_text_view_set_left_margin(d->text, 8);
  gtk_text_view_set_right_margin(d->text, 8);
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(d->text),
                           d->share->text ? d->share->text : "", -1);
  g_signal_connect(gtk_text_view_get_buffer(d->text), "changed",
                   G_CALLBACK(on_text_changed), d);
  GtkWidget *tscroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(tscroll), 140);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(tscroll), 320);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(tscroll), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tscroll), GTK_WIDGET(d->text));
  gtk_widget_add_css_class(tscroll, "card");
  gtk_box_append(GTK_BOX(tbox), tscroll);
  gtk_box_append(GTK_BOX(d->editables), tbox);

  d->meta_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(d->meta_group, "Privacy");
  d->keep_meta = ADW_SWITCH_ROW(adw_switch_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(d->keep_meta),
                                "Upload anyway, with metadata");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(d->keep_meta),
                              "Location, camera and timestamps may be published");
  adw_switch_row_set_active(d->keep_meta, d->share->keep_metadata);
  g_signal_connect(d->keep_meta, "notify::active", G_CALLBACK(on_keep_meta), d);
  adw_preferences_group_add(d->meta_group, GTK_WIDGET(d->keep_meta));
  add_metadata_rows(d);
  gtk_box_append(GTK_BOX(d->editables), GTK_WIDGET(d->meta_group));

  GtkWidget *dest = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(dest), "Destination");
  d->relays_row = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(d->relays_row), "Relays");
  adw_action_row_set_subtitle_selectable(d->relays_row, TRUE);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(dest), GTK_WIDGET(d->relays_row));
  d->servers_row = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(d->servers_row), "Blossom servers");
  adw_action_row_set_subtitle_selectable(d->servers_row, TRUE);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(dest), GTK_WIDGET(d->servers_row));

  GtkWidget *exp = adw_expander_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(exp), "Show event JSON");
  adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(exp),
                                "Exactly what the signer will be asked to sign");
  d->json = GTK_TEXT_VIEW(gtk_text_view_new());
  gtk_text_view_set_editable(d->json, FALSE);
  gtk_text_view_set_monospace(d->json, TRUE);
  gtk_text_view_set_wrap_mode(d->json, GTK_WRAP_CHAR);
  gtk_text_view_set_left_margin(d->json, 8);
  gtk_text_view_set_top_margin(d->json, 8);
  GtkWidget *jscroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(jscroll), 260);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(jscroll), GTK_WIDGET(d->json));
  adw_expander_row_add_row(ADW_EXPANDER_ROW(exp), jscroll);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(dest), exp);
  gtk_box_append(GTK_BOX(outer), dest);
  gtk_box_append(GTK_BOX(outer), GTK_WIDGET(d->status));

  fill_kind_picker(d);
}

static void
on_activate(GApplication *app, gpointer user_data)
{
  (void)app;
  Dialog *d = user_data;
  if (d->window != NULL) {
    gtk_window_present(d->window);
    return;
  }
  const GError *load_error = g_object_get_data(G_OBJECT(d->app), "ns-load-error");
  build_window(d, load_error);
  gtk_window_present(d->window);
  if (d->share == NULL)
    return;

  d->state = STATE_RESOLVING;
  set_busy(d, TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(d->publish), FALSE);
  refresh(d);   /* show the draft (without pubkey / URLs) immediately */
  set_status(d, "Connecting to your signer and looking up relays…", FALSE);
  GTask *task = g_task_new(NULL, NULL, resolve_done, d);
  g_task_set_task_data(task, d, NULL);
  g_task_run_in_thread(task, resolve_thread);
  g_object_unref(task);
}

int
ns_dialog_run(NsShare *share, const GError *load_error)
{
  Dialog d = { 0 };
  d.share = share;
  d.exit_code = 1;   /* closed without publishing */
  d.app = adw_application_new("org.nostr.Share", G_APPLICATION_NON_UNIQUE);
  if (load_error != NULL)
    g_object_set_data_full(G_OBJECT(d.app), "ns-load-error",
                           g_error_copy(load_error), (GDestroyNotify)g_error_free);
  g_signal_connect(d.app, "activate", G_CALLBACK(on_activate), &d);
  char *argv0[] = { (char *)"nostr-share", NULL };
  g_application_run(G_APPLICATION(d.app), 1, argv0);
  g_object_unref(d.app);
  ns_share_free(d.share);
  g_free(d.kinds);
  g_free(d.shown_json);
  g_free(d.resolve_error);
  return d.exit_code;
}
