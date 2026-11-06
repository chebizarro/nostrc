/* SPDX-License-Identifier: MIT */
#include "grotto-profile-view.h"
#include <glib/gi18n-lib.h>
#include <string.h>

GrottoProfileInfo *grotto_profile_info_copy(const GrottoProfileInfo *info) {
  GrottoProfileInfo *copy = g_new0(GrottoProfileInfo, 1);
  for (guint i = 0; i < GROTTO_PROFILE_N_FIELDS; i++) copy->fields[i] = g_strdup(info && info->fields[i] ? info->fields[i] : "");
  if (info) {
    copy->created_at = info->created_at; copy->published = info->published; copy->cached = info->cached; copy->tor_blocked = info->tor_blocked;
    copy->bot = info->bot; copy->no_relays = info->no_relays; copy->nip05_status = info->nip05_status;
    copy->bootstrap_used = info->bootstrap_used; copy->discovery_relays = g_strdupv(info->discovery_relays); copy->publish_relays = g_strdupv(info->publish_relays); copy->auth_required_relays = g_strdupv(info->auth_required_relays); copy->bootstrap_relays = g_strdupv(info->bootstrap_relays);
    for (guint i = 0; i < 2; i++) if (info->images[i]) copy->images[i] = g_object_ref(info->images[i]);
  }
  return copy;
}
void grotto_profile_info_free(GrottoProfileInfo *info) {
  if (!info) return;
  for (guint i = 0; i < GROTTO_PROFILE_N_FIELDS; i++) g_free(info->fields[i]);
  for (guint i = 0; i < 2; i++) g_clear_object(&info->images[i]);
  g_strfreev(info->discovery_relays); g_strfreev(info->publish_relays); g_strfreev(info->auth_required_relays); g_strfreev(info->bootstrap_relays); g_free(info);
}
struct _GrottoProfileView {
  GtkBox parent_instance;
  GWeakRef panel;
  char *key, *label;
  GrottoProfileInfo *info, *draft;
  GCancellable *cancel;
  guint generation;
  gboolean editable, busy, fetched, fetch_failed;
  AdwAvatar *avatar;
  GtkPicture *banner;
  GtkLabel *status, *address;
  AdwActionRow *summary, *details[GROTTO_PROFILE_N_FIELDS];
  GtkTextView *about;
  GtkButton *edit, *refresh, *stop;
  GtkSpinner *spinner, *dialog_spinner;
  AdwDialog *dialog;
  AdwEntryRow *entries[GROTTO_PROFILE_N_FIELDS];
  GtkButton *save, *choose[2];
  GtkPicture *previews[2];
  GtkLabel *error, *verification, *about_placeholder;
  AdwActionRow *publication, *discovery;
  AdwSwitchRow *bot;
  AdwActionRow *discard;
  GtkButton *verify;
  AdwAvatar *card_avatar;
  GtkPicture *card_banner;
  GtkLabel *card_name, *card_username, *card_about, *card_links, *card_nip05;
  gboolean updating;
};
G_DEFINE_TYPE(GrottoProfileView, grotto_profile_view, GTK_TYPE_BOX)
static const char *const titles[] = {N_("Name"), N_("Display Name"), N_("About"), N_("Avatar URL"), N_("Banner URL"), N_("Nostr Address"), N_("Lightning Address"), N_("Lightning LNURL"), N_("Website")};
static const char *nip05_status(const GrottoProfileInfo *p) {
  if (!p || !*p->fields[GROTTO_PROFILE_NIP05]) return _("No Nostr address");
  switch (p->nip05_status) {
    case GROTTO_NIP05_VERIFIED: return _("Address matches this public key");
    case GROTTO_NIP05_MISMATCH: return _("Address does not match this public key");
    case GROTTO_NIP05_INVALID: return _("Invalid Nostr address");
    case GROTTO_NIP05_UNAVAILABLE: return _("Could not check address — offline or server unavailable");
    default: return _("Address not checked");
  }
}
static void render_card(GrottoProfileView *self, GrottoProfileInfo *p) {
  if (!self->dialog || !self->card_name || !p) return;
  const char *name = *p->fields[GROTTO_PROFILE_DISPLAY_NAME] ? p->fields[GROTTO_PROFILE_DISPLAY_NAME] : p->fields[GROTTO_PROFILE_NAME];
  gtk_label_set_text(self->card_name, *name ? name : _("Your Name"));
  g_autofree char *username = g_strdup_printf("@%s%s", p->fields[GROTTO_PROFILE_NAME], p->bot ? _(" · Bot") : "");
  gtk_label_set_text(self->card_username, username); gtk_widget_set_visible(GTK_WIDGET(self->card_username), *p->fields[GROTTO_PROFILE_NAME] || p->bot);
  gtk_label_set_text(self->card_about, p->fields[GROTTO_PROFILE_ABOUT]);
  g_autofree char *links = g_strdup_printf("%s%s%s%s%s", p->fields[GROTTO_PROFILE_WEBSITE], *p->fields[GROTTO_PROFILE_WEBSITE] ? "\n" : "",
    *p->fields[GROTTO_PROFILE_LUD16] ? p->fields[GROTTO_PROFILE_LUD16] : p->fields[GROTTO_PROFILE_LUD06], *p->fields[GROTTO_PROFILE_NIP05] ? "\n" : "", p->fields[GROTTO_PROFILE_NIP05]);
  gtk_label_set_text(self->card_links, links); gtk_label_set_text(self->card_nip05, nip05_status(p));
  gtk_widget_set_visible(GTK_WIDGET(self->card_nip05), *p->fields[GROTTO_PROFILE_NIP05] != '\0');
  adw_avatar_set_text(self->card_avatar, name); adw_avatar_set_custom_image(self->card_avatar, GDK_PAINTABLE(p->images[0]));
  gtk_picture_set_paintable(self->card_banner, GDK_PAINTABLE(p->images[1])); gtk_widget_set_visible(GTK_WIDGET(self->card_banner), p->images[1] != NULL);
  gtk_label_set_text(self->verification, nip05_status(p));
}
static void render(GrottoProfileView *self) {
  GrottoProfileInfo *p = self->info;
  g_autoptr(GrottoPanel) panel = g_weak_ref_get(&self->panel);
  adw_avatar_set_text(self->avatar, p && *p->fields[GROTTO_PROFILE_DISPLAY_NAME] ? p->fields[GROTTO_PROFILE_DISPLAY_NAME] : p && *p->fields[GROTTO_PROFILE_NAME] ? p->fields[GROTTO_PROFILE_NAME] : self->label);
  adw_avatar_set_custom_image(self->avatar, p ? GDK_PAINTABLE(p->images[0]) : NULL);
  gtk_picture_set_paintable(self->banner, p ? GDK_PAINTABLE(p->images[1]) : NULL);
  gtk_widget_set_visible(GTK_WIDGET(self->banner), p && p->images[1]);
  adw_action_row_set_subtitle(self->summary, p && p->published ? p->fields[GROTTO_PROFILE_ABOUT] : self->fetched && !self->fetch_failed ? _("No profile published yet") : _("Profile not loaded"));
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->summary), p && *p->fields[GROTTO_PROFILE_DISPLAY_NAME] ? p->fields[GROTTO_PROFILE_DISPLAY_NAME] : p && *p->fields[GROTTO_PROFILE_NAME] ? p->fields[GROTTO_PROFILE_NAME] : _("Public Profile"));
  for (guint i = 0; i < GROTTO_PROFILE_N_FIELDS; i++) {
    gtk_widget_set_visible(GTK_WIDGET(self->details[i]), p && p->fields[i] && *p->fields[i]);
    adw_action_row_set_subtitle(self->details[i], p ? p->fields[i] : "");
  }
  g_autoptr(GDateTime) date = p && p->published ? g_date_time_new_from_unix_local(p->created_at) : NULL;
  g_autofree char *time = date ? g_date_time_format(date, "%Y-%m-%d %H:%M %Z") : NULL;
  g_autofree char *relays = p && p->publish_relays ? g_strjoinv("\n", p->publish_relays) : g_strdup("");
  g_autofree char *published = g_strdup_printf(_("Last published: %s\nOutbox / publication relays:\n%s"), time ? time : _("Nothing published"), *relays ? relays : _("No write relays"));
  adw_action_row_set_subtitle(self->publication, published);
  g_autofree char *discovery = p && p->discovery_relays ? g_strjoinv("\n", p->discovery_relays) : g_strdup("");
  g_autofree char *source = g_strdup_printf("%s\n%s", p && p->bootstrap_used ? _("Discovery-only bootstrap used, then the identity's outbox when available. Never used for publishing.") : _("Profile discovery relays"), *discovery ? discovery : _("No relays contacted"));
  g_autofree char *skipped = p && p->auth_required_relays ? g_strjoinv("\n", p->auth_required_relays) : NULL;
  g_autofree char *discovery_status = skipped && *skipped ? g_strdup_printf(_("%s\n\nSkipped — relay requires authentication (bootstrap never signs or sends AUTH):\n%s"), source, skipped) : g_strdup(source);
  adw_action_row_set_subtitle(self->discovery, discovery_status);
  g_autofree char *address = p ? g_strdup_printf("%s — %s", p->fields[GROTTO_PROFILE_NIP05], nip05_status(p)) : g_strdup("");
  gtk_label_set_text(self->address, address); gtk_widget_set_visible(GTK_WIDGET(self->address), p && *p->fields[GROTTO_PROFILE_NIP05]);
  gtk_widget_set_visible(GTK_WIDGET(self->edit), self->editable);
  gtk_widget_set_sensitive(GTK_WIDGET(self->edit), self->editable && !self->busy && self->fetched);
  gtk_widget_set_sensitive(GTK_WIDGET(self->refresh), !self->busy && panel && self->key && grotto_panel_has_profile_host(panel));
  if (self->fetch_failed) gtk_button_set_label(self->refresh, _("Retry"));
  else gtk_button_set_icon_name(self->refresh, "view-refresh-symbolic");
  gtk_spinner_set_spinning(self->spinner, self->busy);
  gtk_widget_set_visible(GTK_WIDGET(self->spinner), self->busy);
  if (self->dialog) {
    gtk_spinner_set_spinning(self->dialog_spinner, self->busy);
    gtk_widget_set_sensitive(GTK_WIDGET(self->save), !self->busy && self->editable);
    gtk_widget_set_sensitive(GTK_WIDGET(self->discard), !self->busy);
    gtk_widget_set_sensitive(GTK_WIDGET(self->verify), !self->busy);
    gtk_widget_set_sensitive(GTK_WIDGET(self->bot), !self->busy && self->editable);
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
  if (self->dialog) p->bot = adw_switch_row_get_active(self->bot);
  if (self->draft && g_strcmp0(p->fields[GROTTO_PROFILE_NIP05], self->draft->fields[GROTTO_PROFILE_NIP05])) p->nip05_status = GROTTO_NIP05_UNCHECKED;
  return p;
}
static void form_changed(gpointer widget, gpointer data) {
  GrottoProfileView *self = data; if (self->updating || !self->dialog) return;
  /* A closing dialog can outlive its replacement until its final frame. */
  if (GTK_IS_WIDGET(widget) && !gtk_widget_is_ancestor(GTK_WIDGET(widget), GTK_WIDGET(self->dialog))) return;
  if (GTK_IS_TEXT_BUFFER(widget) && (!self->about || widget != gtk_text_view_get_buffer(self->about))) return;
  g_autoptr(GrottoProfileInfo) p = form(self);
  g_clear_pointer(&self->draft, grotto_profile_info_free); self->draft = g_steal_pointer(&p); render_card(self, self->draft);
  for (guint i = 0; i < 2; i++) gtk_picture_set_paintable(self->previews[i], GDK_PAINTABLE(self->draft->images[i]));
}
static void bot_changed(GObject *object, GParamSpec *spec, gpointer data) { (void)spec; form_changed(object, data); }
typedef struct { GWeakRef view; guint generation; GrottoProfileAction action; } Pending;
static void done(GObject *source, GAsyncResult *result, gpointer data) {
  Pending *pending = data;
  g_autoptr(GrottoProfileView) self = g_weak_ref_get(&pending->view);
  g_autoptr(GError) error = NULL;
  g_autoptr(GrottoProfileInfo) info = grotto_panel_profile_finish(GROTTO_PANEL(source), result, &error);
  if (self && pending->generation == self->generation) {
    self->busy = FALSE;
    if (info) {
      gboolean upload = self->dialog && (pending->action == GROTTO_PROFILE_CHOOSE_AVATAR || pending->action == GROTTO_PROFILE_CHOOSE_BANNER || pending->action == GROTTO_PROFILE_VERIFY_NIP05);
      self->updating = TRUE;
      if (upload) { grotto_profile_info_free(self->draft); self->draft = g_steal_pointer(&info); }
      else { grotto_profile_info_free(self->info); self->info = g_steal_pointer(&info); }
      if (pending->action == GROTTO_PROFILE_FETCH || pending->action == GROTTO_PROFILE_SAVE) {
        self->fetched = TRUE; self->fetch_failed = FALSE;
        gtk_label_set_text(self->status, self->info->tor_blocked ? _("Tor mode — showing the saved profile without network access") : self->info->cached ? _("Offline — showing the saved profile") : self->info->no_relays ? _("No relays — enable discovery or configure this identity's relays") : self->editable ? _("Public information shared with others") : _("This identity's profile is read-only"));
      }
      GrottoProfileInfo *display = upload ? self->draft : self->info;
      if (self->error) gtk_label_set_text(self->error, "");
      if (pending->action == GROTTO_PROFILE_SAVE && self->dialog) {
        adw_dialog_set_can_close(self->dialog, TRUE); adw_dialog_close(self->dialog);
      }
      else if (self->dialog) for (guint i = 0; i < GROTTO_PROFILE_N_FIELDS; i++) {
        if (i == GROTTO_PROFILE_ABOUT) gtk_text_buffer_set_text(gtk_text_view_get_buffer(self->about), display->fields[i], -1);
        else gtk_editable_set_text(GTK_EDITABLE(self->entries[i]), display->fields[i]);
      }
      if (self->dialog) { adw_switch_row_set_active(self->bot, display->bot); render_card(self, display); }
      self->updating = FALSE;
    } else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      if (pending->action == GROTTO_PROFILE_FETCH) {
        self->fetched = TRUE;
        self->fetch_failed = TRUE;
        if (!self->info) self->info = grotto_profile_info_copy(NULL);
      }
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
      (action != GROTTO_PROFILE_FETCH && action != GROTTO_PROFILE_VERIFY_NIP05 && !self->editable)) return;
  g_autoptr(GrottoProfileInfo) info = form(self);
  Pending *pending = g_new0(Pending, 1); g_weak_ref_init(&pending->view, self);
  pending->generation = self->generation; pending->action = action;
  self->busy = TRUE;
  const char *progress = action == GROTTO_PROFILE_FETCH ? _("Loading profile…") : action == GROTTO_PROFILE_SAVE ? _("Waiting for review and publishing…") : action == GROTTO_PROFILE_VERIFY_NIP05 ? _("Checking Nostr address…") : _("Choosing and uploading image…");
  gtk_label_set_text(self->dialog ? self->error : self->status, progress);
  if (self->dialog) gtk_widget_grab_focus(GTK_WIDGET(self->error));
  render(self);
  grotto_panel_profile_async(panel, action, self->key, info, self->cancel, done, pending);
}
static void refresh(GtkButton *button, gpointer data) { (void)button; request(data, GROTTO_PROFILE_FETCH); }
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
static void verify(GtkButton *button, gpointer data) { (void)button; request(data, GROTTO_PROFILE_VERIFY_NIP05); }
static void discard(AdwActionRow *row, gpointer data) {
  (void)row; GrottoProfileView *self = data;
  if (self->busy || !self->dialog) return;
  self->updating = TRUE; g_clear_pointer(&self->draft, grotto_profile_info_free); self->draft = grotto_profile_info_copy(self->info);
  for (guint i = 0; i < GROTTO_PROFILE_N_FIELDS; i++) {
    if (i == GROTTO_PROFILE_ABOUT) gtk_text_buffer_set_text(gtk_text_view_get_buffer(self->about), self->draft->fields[i], -1);
    else gtk_editable_set_text(GTK_EDITABLE(self->entries[i]), self->draft->fields[i]);
  }
  adw_switch_row_set_active(self->bot, self->draft->bot);
  for (guint i = 0; i < 2; i++) gtk_picture_set_paintable(self->previews[i], GDK_PAINTABLE(self->draft->images[i]));
  self->updating = FALSE; render_card(self, self->draft); render(self);
  gtk_label_set_text(self->error, _("Local changes discarded. Already uploaded images remain public"));
}
static AdwActionRow *detail_row(const char *title) {
  AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_action_row_set_subtitle_selectable(row, TRUE);
  adw_action_row_set_subtitle_lines(row, 0);
  return row;
}
static void banner_visibility(GObject *banner, GParamSpec *spec, gpointer data) {
  (void)spec;
  GtkWidget *space = data;
  int size = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(space), "avatar-size"));
  gtk_widget_set_size_request(space, -1, gtk_widget_get_visible(GTK_WIDGET(banner)) ? size / 2 : size);
}
static GtkWidget *profile_header(GtkPicture *banner, AdwAvatar *avatar) {
  GtkWidget *overlay = gtk_overlay_new(), *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *space = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  g_object_set_data(G_OBJECT(space), "avatar-size", GINT_TO_POINTER(adw_avatar_get_size(avatar)));
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(banner)); gtk_box_append(GTK_BOX(box), space);
  gtk_overlay_set_child(GTK_OVERLAY(overlay), box);
  gtk_widget_set_halign(GTK_WIDGET(avatar), GTK_ALIGN_CENTER);
  gtk_widget_set_valign(GTK_WIDGET(avatar), GTK_ALIGN_END);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), GTK_WIDGET(avatar));
  /* Reserve the lower half outside the banner, or the whole avatar without one. */
  g_signal_connect_object(banner, "notify::visible", G_CALLBACK(banner_visibility), space, 0);
  banner_visibility(G_OBJECT(banner), NULL, space);
  return overlay;
}
static void about_changed(GtkTextBuffer *buffer, gpointer data) {
  gtk_widget_set_visible(GTK_WIDGET(data), gtk_text_buffer_get_char_count(buffer) == 0);
}
static GtkLabel *card_label(GtkBox *box, const char *style) {
  GtkLabel *label = GTK_LABEL(gtk_label_new("")); gtk_label_set_wrap(label, TRUE); gtk_label_set_wrap_mode(label, PANGO_WRAP_WORD_CHAR); gtk_label_set_xalign(label, 0);
  if (style) gtk_widget_add_css_class(GTK_WIDGET(label), style);
  gtk_box_append(box, GTK_WIDGET(label)); return label;
}
static void closed(AdwDialog *dialog, gpointer data) {
  GrottoProfileView *self = data;
  if (self->dialog != dialog) return;
  for (guint i = 0; i < GROTTO_PROFILE_N_FIELDS; i++)
    if (self->entries[i]) g_signal_handlers_disconnect_by_data(self->entries[i], self);
  if (self->about) g_signal_handlers_disconnect_by_data(gtk_text_view_get_buffer(self->about), self);
  if (self->bot) g_signal_handlers_disconnect_by_data(self->bot, self);
  self->dialog = NULL; self->error = NULL; self->save = NULL; self->about = NULL; self->dialog_spinner = NULL; self->stop = NULL;
  self->card_name = NULL; self->card_avatar = NULL; self->card_banner = NULL; self->verification = NULL; self->about_placeholder = NULL;
  g_clear_pointer(&self->draft, grotto_profile_info_free);
  memset(self->entries, 0, sizeof self->entries); memset(self->choose, 0, sizeof self->choose);
}
static void edit(GtkButton *button, gpointer data) {
  (void)button; GrottoProfileView *self = data;
  if (self->dialog || !self->editable || !self->info) return;
  self->draft = grotto_profile_info_copy(self->info); self->updating = TRUE;
  self->dialog = adw_dialog_new();
  adw_dialog_set_title(self->dialog, _("Edit Profile")); adw_dialog_set_content_width(self->dialog, 540); adw_dialog_set_content_height(self->dialog, 660);
  GtkWidget *toolbar = adw_toolbar_view_new(), *header = adw_header_bar_new(), *page = adw_preferences_page_new();
  adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
  adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
  self->stop = GTK_BUTTON(gtk_button_new_with_label(_("Cancel")));
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), GTK_WIDGET(self->stop));
  g_signal_connect(self->stop, "clicked", G_CALLBACK(stop), self);
  g_signal_connect_object(self->dialog, "close-attempt", G_CALLBACK(close_attempt), self, 0);
  self->save = GTK_BUTTON(gtk_button_new_with_label(_("Save")));
  gtk_widget_add_css_class(GTK_WIDGET(self->save), "suggested-action");
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), GTK_WIDGET(self->save));
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);
  GtkWidget *preview_group = adw_preferences_group_new();
  AdwExpanderRow *preview = ADW_EXPANDER_ROW(adw_expander_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(preview), _("Profile Preview"));
  adw_expander_row_set_subtitle(preview, _("A client-style preview of your unsaved changes"));
  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start(card, 12); gtk_widget_set_margin_end(card, 12); gtk_widget_set_margin_top(card, 12); gtk_widget_set_margin_bottom(card, 12);
  self->card_banner = GTK_PICTURE(gtk_picture_new()); gtk_picture_set_content_fit(self->card_banner, GTK_CONTENT_FIT_COVER);
  gtk_picture_set_alternative_text(self->card_banner, _("Preview Banner")); gtk_widget_set_size_request(GTK_WIDGET(self->card_banner), -1, 90);
  self->card_avatar = ADW_AVATAR(adw_avatar_new(64, "", TRUE));
  gtk_box_append(GTK_BOX(card), profile_header(self->card_banner, self->card_avatar));
  self->card_name = card_label(GTK_BOX(card), "title-2"); self->card_username = card_label(GTK_BOX(card), "dim-label");
  self->card_about = card_label(GTK_BOX(card), NULL); self->card_links = card_label(GTK_BOX(card), NULL); self->card_nip05 = card_label(GTK_BOX(card), "dim-label");
  adw_expander_row_add_row(preview, card);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(preview_group), GTK_WIDGET(preview)); adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(preview_group));
  GtkWidget *group = adw_preferences_group_new(), *images = adw_preferences_group_new(), *advanced = adw_preferences_group_new();
  AdwExpanderRow *extra = ADW_EXPANDER_ROW(adw_expander_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(extra), _("Links, Addresses and More"));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(advanced), GTK_WIDGET(extra));
  adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group), _("Your profile is public. Saving asks you to review the final JSON and publication relays"));
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group));
  for (guint i = 0; i < GROTTO_PROFILE_N_FIELDS; i++) {
    if (i == GROTTO_PROFILE_ABOUT) {
      GtkWidget *row = adw_preferences_row_new(), *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), _("About"));
      GtkWidget *label = gtk_label_new(_("About")), *overlay = gtk_overlay_new();
      gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
      gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
      gtk_widget_set_focusable(row, FALSE);
      gtk_widget_set_margin_start(box, 12); gtk_widget_set_margin_end(box, 12);
      gtk_widget_set_margin_top(box, 12); gtk_widget_set_margin_bottom(box, 12);
      gtk_label_set_xalign(GTK_LABEL(label), 0);
      gtk_widget_add_css_class(label, "caption"); gtk_widget_add_css_class(label, "dim-label");
      self->about = GTK_TEXT_VIEW(gtk_text_view_new()); gtk_text_view_set_wrap_mode(self->about, GTK_WRAP_WORD_CHAR);
      gtk_text_buffer_set_text(gtk_text_view_get_buffer(self->about), self->info->fields[i], -1);
      gtk_widget_set_size_request(GTK_WIDGET(self->about), -1, 100);
      gtk_text_view_set_accepts_tab(self->about, FALSE);
      gtk_text_view_set_top_margin(self->about, 6); gtk_text_view_set_bottom_margin(self->about, 6);
      self->about_placeholder = GTK_LABEL(gtk_label_new(_("Write something about yourself…")));
      gtk_label_set_wrap(self->about_placeholder, TRUE); gtk_label_set_xalign(self->about_placeholder, 0);
      gtk_widget_set_valign(GTK_WIDGET(self->about_placeholder), GTK_ALIGN_START);
      gtk_widget_set_margin_top(GTK_WIDGET(self->about_placeholder), 6);
      gtk_widget_add_css_class(GTK_WIDGET(self->about_placeholder), "dim-label");
      gtk_widget_set_can_target(GTK_WIDGET(self->about_placeholder), FALSE);
      gtk_accessible_update_state(GTK_ACCESSIBLE(self->about_placeholder), GTK_ACCESSIBLE_STATE_HIDDEN, TRUE, -1);
      gtk_overlay_set_child(GTK_OVERLAY(overlay), GTK_WIDGET(self->about));
      gtk_overlay_add_overlay(GTK_OVERLAY(overlay), GTK_WIDGET(self->about_placeholder));
      g_signal_connect_object(gtk_text_view_get_buffer(self->about), "changed", G_CALLBACK(about_changed), self->about_placeholder, 0);
      about_changed(gtk_text_view_get_buffer(self->about), self->about_placeholder);
      gtk_accessible_update_property(GTK_ACCESSIBLE(self->about), GTK_ACCESSIBLE_PROPERTY_LABEL, _("About"), -1);
      gtk_box_append(GTK_BOX(box), label); gtk_box_append(GTK_BOX(box), overlay);
      gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
      adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row); continue;
    }
    self->entries[i] = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->entries[i]), _(titles[i]));
    gtk_editable_set_text(GTK_EDITABLE(self->entries[i]), self->info->fields[i]);
    if (i < GROTTO_PROFILE_PICTURE) adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), GTK_WIDGET(self->entries[i]));
    else adw_expander_row_add_row(extra, GTK_WIDGET(self->entries[i]));
    if (i == GROTTO_PROFILE_NIP05) {
      self->verify = GTK_BUTTON(gtk_button_new_with_label(_("Check"))); gtk_widget_set_valign(GTK_WIDGET(self->verify), GTK_ALIGN_CENTER);
      gtk_widget_set_tooltip_text(GTK_WIDGET(self->verify), _("Check this address using HTTPS at its domain"));
      adw_entry_row_add_suffix(self->entries[i], GTK_WIDGET(self->verify)); g_signal_connect(self->verify, "clicked", G_CALLBACK(verify), self);
    }
    if (i == GROTTO_PROFILE_PICTURE || i == GROTTO_PROFILE_BANNER) {
      guint index = i == GROTTO_PROFILE_PICTURE ? 0 : 1;
      self->choose[index] = GTK_BUTTON(gtk_button_new_from_icon_name("document-open-symbolic"));
      gtk_widget_set_valign(GTK_WIDGET(self->choose[index]), GTK_ALIGN_CENTER);
      gtk_widget_set_tooltip_text(GTK_WIDGET(self->choose[index]), index ? _("Choose Banner…") : _("Choose Avatar…"));
      self->previews[index] = GTK_PICTURE(gtk_picture_new_for_paintable(GDK_PAINTABLE(self->draft->images[index])));
      gtk_widget_set_size_request(GTK_WIDGET(self->previews[index]), 48, 48);
      gtk_picture_set_can_shrink(self->previews[index], TRUE);
      gtk_picture_set_alternative_text(self->previews[index], index ? _("Banner Preview") : _("Avatar Preview"));
      AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), index ? _("Banner") : _("Avatar"));
      adw_action_row_set_subtitle(row, index ? _("Crop to 3∶1 · Up to 1500 × 500") : _("Square crop · Up to 512 × 512"));
      adw_action_row_add_prefix(row, GTK_WIDGET(self->previews[index]));
      adw_action_row_add_suffix(row, GTK_WIDGET(self->choose[index])); adw_preferences_group_add(ADW_PREFERENCES_GROUP(images), GTK_WIDGET(row));
      g_signal_connect(self->choose[index], "clicked", G_CALLBACK(choose), self);
    }
  }
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(images));
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(advanced));
  self->bot = ADW_SWITCH_ROW(adw_switch_row_new()); adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->bot), _("Automated Account (Bot)"));
  adw_switch_row_set_active(self->bot, self->draft->bot); adw_expander_row_add_row(extra, GTK_WIDGET(self->bot));
  self->verification = GTK_LABEL(gtk_label_new(nip05_status(self->draft))); gtk_label_set_wrap(self->verification, TRUE); adw_expander_row_add_row(extra, GTK_WIDGET(self->verification));
  AdwExpanderRow *provenance = ADW_EXPANDER_ROW(adw_expander_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(provenance), _("Publication and Discovery Details"));
  GtkWidget *published_at = GTK_WIDGET(detail_row(_("Publication"))), *discovered_at = GTK_WIDGET(detail_row(_("Discovery")));
  g_object_bind_property(self->publication, "subtitle", published_at, "subtitle", G_BINDING_SYNC_CREATE);
  g_object_bind_property(self->discovery, "subtitle", discovered_at, "subtitle", G_BINDING_SYNC_CREATE);
  adw_expander_row_add_row(provenance, published_at); adw_expander_row_add_row(provenance, discovered_at);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(advanced), GTK_WIDGET(provenance));
  for (guint i = 0; i < GROTTO_PROFILE_N_FIELDS; i++) if (self->entries[i]) g_signal_connect_object(self->entries[i], "changed", G_CALLBACK(form_changed), self, 0);
  g_signal_connect_object(gtk_text_view_get_buffer(self->about), "changed", G_CALLBACK(form_changed), self, 0);
  g_signal_connect_object(self->bot, "notify::active", G_CALLBACK(bot_changed), self, 0);
  GtkWidget *actions = adw_preferences_group_new(); adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(actions));
  self->discard = detail_row(_("Discard Local Changes"));
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(self->discard), TRUE);
  adw_action_row_add_suffix(self->discard, gtk_image_new_from_icon_name("edit-undo-symbolic"));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(actions), GTK_WIDGET(self->discard));
  g_signal_connect(self->discard, "activated", G_CALLBACK(discard), self);
  self->error = GTK_LABEL(gtk_label_new("")); gtk_label_set_wrap(self->error, TRUE); gtk_label_set_wrap_mode(self->error, PANGO_WRAP_WORD_CHAR);
  gtk_widget_set_focusable(GTK_WIDGET(self->error), TRUE);
  GtkWidget *progress = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  self->dialog_spinner = GTK_SPINNER(gtk_spinner_new());
  gtk_box_append(GTK_BOX(progress), GTK_WIDGET(self->dialog_spinner)); gtk_box_append(GTK_BOX(progress), GTK_WIDGET(self->error));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(actions), progress);
  adw_dialog_set_child(self->dialog, toolbar);
  g_signal_connect(self->save, "clicked", G_CALLBACK(save), self);
  g_signal_connect_object(self->dialog, "closed", G_CALLBACK(closed), self, 0);
  self->updating = FALSE; render_card(self, self->draft);
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
  if (was_editable != self->editable && self->info && !self->fetch_failed && !self->busy && !self->dialog)
    gtk_label_set_text(self->status, self->info->tor_blocked ? _("Tor mode — showing the saved profile without network access") : self->info->cached ? _("Offline — showing the saved profile") :
      self->editable ? _("Public information shared with others") : _("This identity's profile is read-only"));
  if (changed) {
    self->generation++; g_cancellable_cancel(self->cancel); g_clear_object(&self->cancel); self->cancel = g_cancellable_new();
    self->busy = FALSE; self->fetched = FALSE; self->fetch_failed = FALSE;
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
  self->avatar = ADW_AVATAR(adw_avatar_new(96, "", TRUE));
  gtk_box_append(GTK_BOX(self), profile_header(self->banner, self->avatar));
  GtkWidget *list = gtk_list_box_new(); gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(list, "boxed-list"); gtk_box_append(GTK_BOX(self), list);
  self->summary = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(self->summary), FALSE);
  adw_action_row_set_subtitle_lines(self->summary, 3); adw_action_row_set_subtitle_selectable(self->summary, TRUE);
  gtk_list_box_append(GTK_LIST_BOX(list), GTK_WIDGET(self->summary));
  AdwExpanderRow *details = ADW_EXPANDER_ROW(adw_expander_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(details), _("Profile Details")); gtk_list_box_append(GTK_LIST_BOX(list), GTK_WIDGET(details));
  self->publication = detail_row(_("Publication")); self->discovery = detail_row(_("Discovery"));
  adw_expander_row_add_row(details, GTK_WIDGET(self->publication)); adw_expander_row_add_row(details, GTK_WIDGET(self->discovery));
  for (guint i = 0; i < GROTTO_PROFILE_N_FIELDS; i++) {
    self->details[i] = detail_row(_(titles[i]));
    adw_expander_row_add_row(details, GTK_WIDGET(self->details[i]));
  }
  self->address = GTK_LABEL(gtk_label_new("")); gtk_label_set_wrap(self->address, TRUE); gtk_label_set_wrap_mode(self->address, PANGO_WRAP_WORD_CHAR);
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->address));
  self->status = GTK_LABEL(gtk_label_new("")); gtk_label_set_wrap(self->status, TRUE); gtk_widget_add_css_class(GTK_WIDGET(self->status), "dim-label");
  GtkWidget *progress = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign(progress, GTK_ALIGN_CENTER);
  self->spinner = GTK_SPINNER(gtk_spinner_new());
  gtk_box_append(GTK_BOX(progress), GTK_WIDGET(self->spinner));
  gtk_box_append(GTK_BOX(progress), GTK_WIDGET(self->status));
  gtk_box_append(GTK_BOX(self), progress);
  GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6); gtk_widget_set_halign(buttons, GTK_ALIGN_CENTER);
  self->edit = GTK_BUTTON(gtk_button_new_with_label(_("Edit Profile…"))); self->refresh = GTK_BUTTON(gtk_button_new_from_icon_name("view-refresh-symbolic"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->refresh), _("Refresh Profile"));
  gtk_box_append(GTK_BOX(buttons), GTK_WIDGET(self->edit)); gtk_box_append(GTK_BOX(buttons), GTK_WIDGET(self->refresh)); gtk_box_append(GTK_BOX(self), buttons);
  g_signal_connect(self->edit, "clicked", G_CALLBACK(edit), self); g_signal_connect(self->refresh, "clicked", G_CALLBACK(refresh), self); g_signal_connect(self, "map", G_CALLBACK(mapped), NULL);
}
GtkWidget *grotto_profile_view_new(GrottoPanel *panel) {
  GrottoProfileView *self = g_object_new(GROTTO_TYPE_PROFILE_VIEW, NULL); g_weak_ref_set(&self->panel, panel); return GTK_WIDGET(self);
}
