/* SPDX-License-Identifier: MIT */
#include "grotto-profile-view.h"
#include <glib/gi18n-lib.h>
#include <string.h>

GrottoProfileInfo *grotto_profile_info_copy(const GrottoProfileInfo *info) {
  GrottoProfileInfo *copy = g_new0(GrottoProfileInfo, 1);
  for (guint i = 0; i < GROTTO_PROFILE_N_FIELDS; i++) copy->fields[i] = g_strdup(info && info->fields[i] ? info->fields[i] : "");
  if (info) {
    copy->created_at = info->created_at; copy->published = info->published; copy->cached = info->cached; copy->tor_blocked = info->tor_blocked;
    for (guint i = 0; i < 2; i++) if (info->images[i]) copy->images[i] = g_object_ref(info->images[i]);
  }
  return copy;
}
void grotto_profile_info_free(GrottoProfileInfo *info) {
  if (!info) return;
  for (guint i = 0; i < GROTTO_PROFILE_N_FIELDS; i++) g_free(info->fields[i]);
  for (guint i = 0; i < 2; i++) g_clear_object(&info->images[i]);
  g_free(info);
}
struct _GrottoProfileView {
  GtkBox parent_instance;
  GWeakRef panel;
  char *key, *label;
  GrottoProfileInfo *info, *draft;
  GCancellable *cancel;
  guint generation;
  gboolean editable, busy, fetched;
  AdwAvatar *avatar;
  GtkPicture *banner;
  GtkLabel *status;
  AdwActionRow *summary, *details[GROTTO_PROFILE_N_FIELDS];
  GtkTextView *about;
  GtkButton *edit, *refresh, *load_images, *stop;
  GtkSpinner *spinner, *dialog_spinner;
  AdwDialog *dialog;
  AdwEntryRow *entries[GROTTO_PROFILE_N_FIELDS];
  GtkButton *save, *choose[2];
  GtkPicture *previews[2];
  GtkLabel *error;
};
G_DEFINE_TYPE(GrottoProfileView, grotto_profile_view, GTK_TYPE_BOX)
static const char *const titles[] = {N_("Name"), N_("Display Name"), N_("About"), N_("Avatar URL"), N_("Banner URL"), N_("Nostr Address"), N_("Lightning Address"), N_("Lightning LNURL"), N_("Website")};
static void render(GrottoProfileView *self) {
  GrottoProfileInfo *p = self->info;
  g_autoptr(GrottoPanel) panel = g_weak_ref_get(&self->panel);
  adw_avatar_set_text(self->avatar, p && *p->fields[GROTTO_PROFILE_DISPLAY_NAME] ? p->fields[GROTTO_PROFILE_DISPLAY_NAME] : p && *p->fields[GROTTO_PROFILE_NAME] ? p->fields[GROTTO_PROFILE_NAME] : self->label);
  adw_avatar_set_custom_image(self->avatar, p ? GDK_PAINTABLE(p->images[0]) : NULL);
  gtk_picture_set_paintable(self->banner, p ? GDK_PAINTABLE(p->images[1]) : NULL);
  gtk_widget_set_visible(GTK_WIDGET(self->banner), p && p->images[1]);
  adw_action_row_set_subtitle(self->summary, p && p->published ? p->fields[GROTTO_PROFILE_ABOUT] : _("Not published yet"));
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->summary), p && *p->fields[GROTTO_PROFILE_DISPLAY_NAME] ? p->fields[GROTTO_PROFILE_DISPLAY_NAME] : p && *p->fields[GROTTO_PROFILE_NAME] ? p->fields[GROTTO_PROFILE_NAME] : _("Public Profile"));
  for (guint i = 0; i < GROTTO_PROFILE_N_FIELDS; i++) {
    gtk_widget_set_visible(GTK_WIDGET(self->details[i]), p && p->fields[i] && *p->fields[i]);
    adw_action_row_set_subtitle(self->details[i], p ? p->fields[i] : "");
  }
  gtk_widget_set_visible(GTK_WIDGET(self->edit), self->editable);
  gtk_widget_set_sensitive(GTK_WIDGET(self->edit), self->editable && !self->busy && self->fetched);
  gtk_widget_set_sensitive(GTK_WIDGET(self->refresh), !self->busy && panel && self->key && grotto_panel_has_profile_host(panel));
  gtk_widget_set_sensitive(GTK_WIDGET(self->load_images), !self->busy && self->info && panel && grotto_panel_has_profile_host(panel));
  gtk_spinner_set_spinning(self->spinner, self->busy);
  if (self->dialog) {
    gtk_spinner_set_spinning(self->dialog_spinner, self->busy);
    gtk_widget_set_sensitive(GTK_WIDGET(self->save), !self->busy && self->editable);
    for (guint i = 0; i < GROTTO_PROFILE_N_FIELDS; i++) if (self->entries[i]) gtk_widget_set_sensitive(GTK_WIDGET(self->entries[i]), !self->busy && self->editable);
    gtk_widget_set_sensitive(GTK_WIDGET(self->about), !self->busy && self->editable);
    for (guint i = 0; i < 2; i++) gtk_widget_set_sensitive(GTK_WIDGET(self->choose[i]), !self->busy && self->editable);
    adw_dialog_set_can_close(self->dialog, !self->busy);
    gtk_button_set_label(self->stop, self->busy ? _("Stop") : _("Cancel"));
    gtk_widget_set_sensitive(GTK_WIDGET(self->stop), TRUE);
  }
}
static GrottoProfileInfo *form(GrottoProfileView *self) {
  GrottoProfileInfo *p = grotto_profile_info_copy(self->draft ? self->draft : self->info);
  if (self->dialog) for (guint i = 0; i < GROTTO_PROFILE_N_FIELDS; i++) {
    if ((i == GROTTO_PROFILE_PICTURE || i == GROTTO_PROFILE_BANNER) && g_strcmp0(p->fields[i], gtk_editable_get_text(GTK_EDITABLE(self->entries[i]))))
      g_clear_object(&p->images[i == GROTTO_PROFILE_PICTURE ? 0 : 1]);
    g_free(p->fields[i]);
    if (i == GROTTO_PROFILE_ABOUT) {
      GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->about); GtkTextIter start, end;
      gtk_text_buffer_get_bounds(buffer, &start, &end); p->fields[i] = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    } else p->fields[i] = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->entries[i])));
  }
  return p;
}
typedef struct { GWeakRef view; guint generation; GrottoProfileAction action; } Pending;
static void done(GObject *source, GAsyncResult *result, gpointer data) {
  Pending *pending = data;
  g_autoptr(GrottoProfileView) self = g_weak_ref_get(&pending->view);
  g_autoptr(GError) error = NULL;
  g_autoptr(GrottoProfileInfo) info = grotto_panel_profile_finish(GROTTO_PANEL(source), result, &error);
  if (self && pending->generation == self->generation) {
    self->busy = FALSE;
    if (info) {
      gboolean upload = pending->action == GROTTO_PROFILE_CHOOSE_AVATAR || pending->action == GROTTO_PROFILE_CHOOSE_BANNER;
      if (upload) { grotto_profile_info_free(self->draft); self->draft = g_steal_pointer(&info); }
      else { grotto_profile_info_free(self->info); self->info = g_steal_pointer(&info); }
      self->fetched = TRUE;
      GrottoProfileInfo *display = upload ? self->draft : self->info;
      gtk_label_set_text(self->status, self->info->tor_blocked ? _("Tor mode — showing the saved profile without network access") : self->info->cached ? _("Offline — showing the saved profile") : self->editable ? _("Public information shared with others") : _("This identity's profile is read-only"));
      if (self->error) gtk_label_set_text(self->error, "");
      if (pending->action == GROTTO_PROFILE_SAVE && self->dialog) {
        adw_dialog_set_can_close(self->dialog, TRUE); adw_dialog_close(self->dialog);
      }
      else if (self->dialog) for (guint i = 0; i < GROTTO_PROFILE_N_FIELDS; i++) {
        if (i == GROTTO_PROFILE_ABOUT) gtk_text_buffer_set_text(gtk_text_view_get_buffer(self->about), display->fields[i], -1);
        else gtk_editable_set_text(GTK_EDITABLE(self->entries[i]), display->fields[i]);
      }
    } else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      const char *message = error ? error->message : _("Profile unavailable. Try again");
      gtk_label_set_text(self->dialog ? self->error : self->status, message);
    } else if (self->error) gtk_label_set_text(self->error, _("Stopped. Data may already have reached a server or relay. Refresh before saving again"));
    if (g_cancellable_is_cancelled(self->cancel)) { g_clear_object(&self->cancel); self->cancel = g_cancellable_new(); }
    if (self->dialog && self->draft) for (guint i = 0; i < 2; i++)
      gtk_picture_set_paintable(self->previews[i], GDK_PAINTABLE(self->draft->images[i]));
    render(self);
  }
  g_weak_ref_clear(&pending->view); g_free(pending);
}
static void request(GrottoProfileView *self, GrottoProfileAction action) {
  g_autoptr(GrottoPanel) panel = g_weak_ref_get(&self->panel);
  if (!panel || self->busy || !self->key || !grotto_panel_has_profile_host(panel) ||
      (action != GROTTO_PROFILE_FETCH && action != GROTTO_PROFILE_LOAD_IMAGES && !self->editable)) return;
  g_autoptr(GrottoProfileInfo) info = form(self);
  Pending *pending = g_new0(Pending, 1); g_weak_ref_init(&pending->view, self);
  pending->generation = self->generation; pending->action = action;
  self->busy = TRUE;
  const char *progress = action == GROTTO_PROFILE_FETCH ? _("Loading profile…") : action == GROTTO_PROFILE_SAVE ? _("Waiting for review and publishing…") : action == GROTTO_PROFILE_LOAD_IMAGES ? _("Loading images…") : _("Choosing and uploading image…");
  gtk_label_set_text(self->dialog ? self->error : self->status, progress);
  if (self->dialog) gtk_widget_grab_focus(GTK_WIDGET(self->error));
  render(self);
  grotto_panel_profile_async(panel, action, self->key, info, self->cancel, done, pending);
}
static void refresh(GtkButton *button, gpointer data) { (void)button; request(data, GROTTO_PROFILE_FETCH); }
static void load_images(GtkButton *button, gpointer data) { (void)button; request(data, GROTTO_PROFILE_LOAD_IMAGES); }
static void stop(GtkButton *button, gpointer data) {
  (void)button; GrottoProfileView *self = data;
  if (self->busy) { g_cancellable_cancel(self->cancel); if (self->error) gtk_label_set_text(self->error, _("Stopping… Data already sent cannot be recalled")); }
  else if (self->dialog) adw_dialog_close(self->dialog);
}
static void close_attempt(AdwDialog *dialog, gpointer data) { (void)dialog; stop(NULL, data); }
static void save(GtkButton *button, gpointer data) { (void)button; request(data, GROTTO_PROFILE_SAVE); }
static void choose(GtkButton *button, gpointer data) {
  GrottoProfileView *self = data;
  request(self, button == self->choose[0] ? GROTTO_PROFILE_CHOOSE_AVATAR : GROTTO_PROFILE_CHOOSE_BANNER);
}
static void closed(AdwDialog *dialog, gpointer data) {
  (void)dialog; GrottoProfileView *self = data;
  self->dialog = NULL; self->error = NULL; self->save = NULL; self->about = NULL; self->dialog_spinner = NULL; self->stop = NULL;
  g_clear_pointer(&self->draft, grotto_profile_info_free);
  memset(self->entries, 0, sizeof self->entries); memset(self->choose, 0, sizeof self->choose);
}
static void edit(GtkButton *button, gpointer data) {
  (void)button; GrottoProfileView *self = data;
  if (self->dialog || !self->editable || !self->info) return;
  self->draft = grotto_profile_info_copy(self->info);
  self->dialog = adw_dialog_new();
  adw_dialog_set_title(self->dialog, _("Edit Profile")); adw_dialog_set_content_width(self->dialog, 480); adw_dialog_set_content_height(self->dialog, 660);
  GtkWidget *toolbar = adw_toolbar_view_new(), *header = adw_header_bar_new(), *page = adw_preferences_page_new();
  self->stop = GTK_BUTTON(gtk_button_new_with_label(_("Cancel")));
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), GTK_WIDGET(self->stop));
  g_signal_connect(self->stop, "clicked", G_CALLBACK(stop), self);
  g_signal_connect_object(self->dialog, "close-attempt", G_CALLBACK(close_attempt), self, 0);
  self->save = GTK_BUTTON(gtk_button_new_with_label(_("Save")));
  gtk_widget_add_css_class(GTK_WIDGET(self->save), "suggested-action");
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), GTK_WIDGET(self->save));
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);
  GtkWidget *group = adw_preferences_group_new();
  adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group), _("Your profile is public. Saving asks you to review the final JSON and publication relays"));
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group));
  for (guint i = 0; i < GROTTO_PROFILE_N_FIELDS; i++) {
    if (i == GROTTO_PROFILE_ABOUT) {
      GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6), *label = gtk_label_new(_("About"));
      gtk_label_set_xalign(GTK_LABEL(label), 0);
      self->about = GTK_TEXT_VIEW(gtk_text_view_new()); gtk_text_view_set_wrap_mode(self->about, GTK_WRAP_WORD_CHAR);
      gtk_text_buffer_set_text(gtk_text_view_get_buffer(self->about), self->info->fields[i], -1);
      gtk_widget_set_size_request(GTK_WIDGET(self->about), -1, 100);
      gtk_accessible_update_property(GTK_ACCESSIBLE(self->about), GTK_ACCESSIBLE_PROPERTY_LABEL, _("About"), -1);
      gtk_box_append(GTK_BOX(box), label); gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->about));
      adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), box); continue;
    }
    self->entries[i] = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->entries[i]), _(titles[i]));
    gtk_editable_set_text(GTK_EDITABLE(self->entries[i]), self->info->fields[i]);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), GTK_WIDGET(self->entries[i]));
    if (i == GROTTO_PROFILE_PICTURE || i == GROTTO_PROFILE_BANNER) {
      guint index = i == GROTTO_PROFILE_PICTURE ? 0 : 1;
      self->choose[index] = GTK_BUTTON(gtk_button_new_from_icon_name("document-open-symbolic"));
      gtk_widget_set_valign(GTK_WIDGET(self->choose[index]), GTK_ALIGN_CENTER);
      gtk_widget_set_tooltip_text(GTK_WIDGET(self->choose[index]), index ? _("Choose Banner…") : _("Choose Avatar…"));
      self->previews[index] = GTK_PICTURE(gtk_picture_new_for_paintable(GDK_PAINTABLE(self->draft->images[index])));
      gtk_widget_set_size_request(GTK_WIDGET(self->previews[index]), 48, 48);
      gtk_picture_set_can_shrink(self->previews[index], TRUE);
      gtk_picture_set_alternative_text(self->previews[index], index ? _("Banner Preview") : _("Avatar Preview"));
      adw_entry_row_add_suffix(self->entries[i], GTK_WIDGET(self->previews[index]));
      adw_entry_row_add_suffix(self->entries[i], GTK_WIDGET(self->choose[index]));
      g_signal_connect(self->choose[index], "clicked", G_CALLBACK(choose), self);
    }
  }
  self->error = GTK_LABEL(gtk_label_new("")); gtk_label_set_wrap(self->error, TRUE); gtk_label_set_wrap_mode(self->error, PANGO_WRAP_WORD_CHAR);
  gtk_widget_set_focusable(GTK_WIDGET(self->error), TRUE);
  GtkWidget *progress = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  self->dialog_spinner = GTK_SPINNER(gtk_spinner_new());
  gtk_box_append(GTK_BOX(progress), GTK_WIDGET(self->dialog_spinner)); gtk_box_append(GTK_BOX(progress), GTK_WIDGET(self->error));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), progress);
  adw_dialog_set_child(self->dialog, toolbar);
  g_signal_connect(self->save, "clicked", G_CALLBACK(save), self);
  g_signal_connect_object(self->dialog, "closed", G_CALLBACK(closed), self, 0);
  adw_dialog_present(self->dialog, GTK_WIDGET(self)); render(self);
}
static void mapped(GtkWidget *widget, gpointer data) {
  (void)data; GrottoProfileView *self = GROTTO_PROFILE_VIEW(widget);
  if (!self->fetched) request(self, GROTTO_PROFILE_FETCH);
}
void grotto_profile_view_set_identity(GrottoProfileView *self, const char *key, const char *label, gboolean editable) {
  g_autoptr(GrottoPanel) panel = g_weak_ref_get(&self->panel);
  gboolean changed = g_strcmp0(self->key, key) != 0;
  gboolean was_editable = self->editable;
  self->editable = editable && panel && grotto_panel_has_profile_host(panel);
  if (was_editable != self->editable && self->info && !self->busy && !self->dialog)
    gtk_label_set_text(self->status, self->info->tor_blocked ? _("Tor mode — showing the saved profile without network access") : self->info->cached ? _("Offline — showing the saved profile") :
      self->editable ? _("Public information shared with others") : _("This identity's profile is read-only"));
  if (changed) {
    self->generation++; g_cancellable_cancel(self->cancel); g_clear_object(&self->cancel); self->cancel = g_cancellable_new();
    self->busy = FALSE; self->fetched = FALSE;
    if (self->dialog) { adw_dialog_set_can_close(self->dialog, TRUE); adw_dialog_force_close(self->dialog); }
    g_free(self->key); self->key = g_strdup(key);
    g_clear_pointer(&self->info, grotto_profile_info_free);
  }
  g_free(self->label); self->label = g_strdup(label ? label : "");
  if (!panel || !grotto_panel_has_profile_host(panel)) gtk_label_set_text(self->status, _("Open Grotto to view or edit this profile"));
  render(self);
  if (!self->fetched && gtk_widget_get_mapped(GTK_WIDGET(self))) request(self, GROTTO_PROFILE_FETCH);
}
static void dispose(GObject *object) {
  GrottoProfileView *self = GROTTO_PROFILE_VIEW(object);
  self->generation++; if (self->cancel) g_cancellable_cancel(self->cancel);
  if (self->dialog) { adw_dialog_set_can_close(self->dialog, TRUE); adw_dialog_force_close(self->dialog); }
  g_clear_object(&self->cancel); g_weak_ref_set(&self->panel, NULL);
  G_OBJECT_CLASS(grotto_profile_view_parent_class)->dispose(object);
}
static void finalize(GObject *object) {
  GrottoProfileView *self = GROTTO_PROFILE_VIEW(object);
  g_weak_ref_clear(&self->panel); g_free(self->key); g_free(self->label); grotto_profile_info_free(self->info); grotto_profile_info_free(self->draft);
  G_OBJECT_CLASS(grotto_profile_view_parent_class)->finalize(object);
}
static void grotto_profile_view_class_init(GrottoProfileViewClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = dispose; G_OBJECT_CLASS(klass)->finalize = finalize;
}
static void grotto_profile_view_init(GrottoProfileView *self) {
  g_weak_ref_init(&self->panel, NULL); self->cancel = g_cancellable_new();
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL); gtk_box_set_spacing(GTK_BOX(self), 12);
  self->banner = GTK_PICTURE(gtk_picture_new()); gtk_picture_set_content_fit(self->banner, GTK_CONTENT_FIT_COVER);
  gtk_picture_set_alternative_text(self->banner, _("Profile Banner"));
  gtk_widget_set_size_request(GTK_WIDGET(self->banner), -1, 120); gtk_widget_set_visible(GTK_WIDGET(self->banner), FALSE);
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->banner));
  self->avatar = ADW_AVATAR(adw_avatar_new(96, "", TRUE)); gtk_widget_set_halign(GTK_WIDGET(self->avatar), GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->avatar));
  GtkWidget *list = gtk_list_box_new(); gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(list, "boxed-list"); gtk_box_append(GTK_BOX(self), list);
  self->summary = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(self->summary), FALSE);
  adw_action_row_set_subtitle_lines(self->summary, 3); adw_action_row_set_subtitle_selectable(self->summary, TRUE);
  gtk_list_box_append(GTK_LIST_BOX(list), GTK_WIDGET(self->summary));
  for (guint i = 0; i < GROTTO_PROFILE_N_FIELDS; i++) {
    self->details[i] = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(self->details[i]), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->details[i]), _(titles[i]));
    adw_action_row_set_subtitle_selectable(self->details[i], TRUE); adw_action_row_set_subtitle_lines(self->details[i], 0);
    gtk_list_box_append(GTK_LIST_BOX(list), GTK_WIDGET(self->details[i]));
  }
  self->status = GTK_LABEL(gtk_label_new("")); gtk_label_set_wrap(self->status, TRUE); gtk_widget_add_css_class(GTK_WIDGET(self->status), "dim-label");
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->status));
  GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6); gtk_widget_set_halign(buttons, GTK_ALIGN_CENTER);
  self->edit = GTK_BUTTON(gtk_button_new_with_label(_("Edit Profile…"))); self->refresh = GTK_BUTTON(gtk_button_new_from_icon_name("view-refresh-symbolic"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->refresh), _("Refresh Profile"));
  self->load_images = GTK_BUTTON(gtk_button_new_with_label(_("Load Profile Images…")));
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->load_images));
  g_signal_connect(self->load_images, "clicked", G_CALLBACK(load_images), self);
  self->spinner = GTK_SPINNER(gtk_spinner_new());
  gtk_box_append(GTK_BOX(buttons), GTK_WIDGET(self->edit)); gtk_box_append(GTK_BOX(buttons), GTK_WIDGET(self->refresh)); gtk_box_append(GTK_BOX(buttons), GTK_WIDGET(self->spinner)); gtk_box_append(GTK_BOX(self), buttons);
  g_signal_connect(self->edit, "clicked", G_CALLBACK(edit), self); g_signal_connect(self->refresh, "clicked", G_CALLBACK(refresh), self); g_signal_connect(self, "map", G_CALLBACK(mapped), NULL);
}
GtkWidget *grotto_profile_view_new(GrottoPanel *panel) {
  GrottoProfileView *self = g_object_new(GROTTO_TYPE_PROFILE_VIEW, NULL); g_weak_ref_set(&self->panel, panel); return GTK_WIDGET(self);
}
