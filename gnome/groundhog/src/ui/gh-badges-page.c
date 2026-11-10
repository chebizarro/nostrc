#include "gh-badges-page.h"
#include <glib/gi18n.h>
#include <nostr/nip19/nip19.h>
#include <stdlib.h>
#include <string.h>

/* ---- shared copy ---------------------------------------------------------------- */

static gchar *
short_npub(const gchar *pubkey)
{
  guint8 bytes[32];
  if (!pubkey || strlen(pubkey) != 64) return g_strdup("");
  for (guint i = 0; i < 32; i++) {
    gchar byte[] = { pubkey[2 * i], pubkey[2 * i + 1], 0 };
    bytes[i] = (guint8)strtoul(byte, NULL, 16);
  }
  char *npub = NULL;
  if (nostr_nip19_encode_npub(bytes, &npub) != 0 || !npub) return g_strdup("");
  gsize length = strlen(npub);
  gchar *out = length > 16 ? g_strdup_printf("%.10s…%s", npub, npub + length - 4) : g_strdup(npub);
  free(npub);
  return out;
}
gchar *
gh_badge_describe_issuer(const GhBadge *badge)
{
  g_autofree gchar *issuer = short_npub(badge->issuer);
  /* TRANSLATORS: who issued a badge; never "verified". */
  return g_strdup_printf(_("Awarded by %s"), issuer);
}
const gchar *
gh_badge_display_name(const GhBadge *badge)
{
  return badge->name && *badge->name ? badge->name : badge->identifier;
}
static gchar *
join_lines(GStrv lines)
{
  return lines && lines[0] ? g_strjoinv("\n", lines) : g_strdup(_("none configured"));
}

/* ---- the page -------------------------------------------------------------------- */

struct _GhBadgesPage {
  AdwNavigationPage parent_instance;
  GhBadgesBackend backend;
  guint64 generation;
  AdwToastOverlay *toasts;
  AdwPreferencesGroup *shown_group;
  AdwPreferencesGroup *awards_group;
  GtkButton *publish_button;
  GtkButton *lookup_button;
  GPtrArray *selection; /* GhBadge, the order shown */
  GPtrArray *awards;    /* GhBadge, or NULL before a lookup */
  GhBadgeSelectionSource source;
  GCancellable *cancel;
  guint rows; /* widgets added to the groups, for removal */
  GPtrArray *row_widgets;
  gboolean busy;
};
G_DEFINE_FINAL_TYPE(GhBadgesPage, gh_badges_page, ADW_TYPE_NAVIGATION_PAGE)

static gboolean
current(GhBadgesPage *self)
{
  return self->backend.owner && self->generation == self->backend.generation(self->backend.owner) &&
    self->backend.account(self->backend.owner) != NULL;
}
static void
toast(GhBadgesPage *self, const gchar *text)
{
  AdwToast *t = adw_toast_new(text);
  adw_toast_set_use_markup(t, FALSE);
  adw_toast_overlay_add_toast(self->toasts, t);
}
static void refresh(GhBadgesPage *self);

/* ---- artwork (consented) ---- */

typedef struct { GhBadgesPage *page; GtkImage *image; } Artwork;
static void
artwork_free(gpointer data)
{
  Artwork *a = data;
  if (a->page) g_object_remove_weak_pointer(G_OBJECT(a->page), (gpointer *)&a->page);
  g_clear_object(&a->image); g_free(a);
}
static void
artwork_loaded(GObject *source, GAsyncResult *result, gpointer data)
{
  Artwork *a = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GdkTexture) texture = a->page && a->page->backend.owner
    ? a->page->backend.artwork_finish(G_OBJECT(source), result, &error) : NULL;
  /* Malformed or refused artwork: the text row stands as it is. */
  if (texture && a->page && gtk_widget_get_root(GTK_WIDGET(a->image)))
    gtk_image_set_from_paintable(a->image, GDK_PAINTABLE(texture));
  artwork_free(a);
}
static GtkWidget *
artwork_for(GhBadgesPage *self, const GhBadge *badge)
{
  GtkWidget *image = gtk_image_new_from_icon_name("emblem-default-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(image), 32);
  gtk_widget_set_valign(image, GTK_ALIGN_CENTER);
  gtk_accessible_update_property(GTK_ACCESSIBLE(image), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 gh_badge_display_name(badge), -1);
  const gchar *url = badge->thumb_url ? badge->thumb_url : badge->image_url;
  /* Nothing is fetched unless Load Remote Images is on (PD-2); a text-only
   * badge never asks. */
  if (url && self->backend.artwork_enabled && self->backend.artwork_enabled(self->backend.owner)) {
    Artwork *a = g_new0(Artwork, 1);
    a->page = self; g_object_add_weak_pointer(G_OBJECT(self), (gpointer *)&a->page);
    a->image = g_object_ref(GTK_IMAGE(image));
    self->backend.artwork_async(self->backend.owner, url, self->cancel, artwork_loaded, a);
  }
  return image;
}

/* ---- rows ---- */

static void
clear_rows(GhBadgesPage *self)
{
  for (guint i = 0; i < self->row_widgets->len; i++) {
    GtkWidget *row = g_ptr_array_index(self->row_widgets, i);
    GtkWidget *group = gtk_widget_get_ancestor(row, ADW_TYPE_PREFERENCES_GROUP);
    if (group) adw_preferences_group_remove(ADW_PREFERENCES_GROUP(group), row);
  }
  g_ptr_array_set_size(self->row_widgets, 0);
}
static gboolean
selected_has(GhBadgesPage *self, const GhBadge *badge)
{
  for (guint i = 0; i < self->selection->len; i++) {
    GhBadge *b = g_ptr_array_index(self->selection, i);
    if (!g_strcmp0(b->address, badge->address) && !g_strcmp0(b->award_id, badge->award_id)) return TRUE;
  }
  return FALSE;
}
static void
save_selection(GhBadgesPage *self)
{
  g_autoptr(GError) error = NULL;
  if (!current(self) || !self->backend.set_selection(self->backend.owner, self->selection, &error)) {
    g_message("Groundhog could not save the profile badges: %s", error ? error->message : "no account");
    toast(self, _("Groundhog couldn’t save this change"));
  }
  refresh(self);
}
/* badges.remove / move-up / move-down (u: the row's index), from each
 * chosen row's one menu button (a boxed-list row holds at most two controls). */
static void
on_remove(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  (void)action;
  GhBadgesPage *self = data;
  guint index = g_variant_get_uint32(parameter);
  if (index < self->selection->len) g_ptr_array_remove_index(self->selection, index);
  save_selection(self);
}
static void
move(GhBadgesPage *self, guint index, gint delta)
{
  guint target = index + delta;
  if (index >= self->selection->len || target >= self->selection->len) return;
  GhBadge *badge = g_ptr_array_steal_index(self->selection, index);
  g_ptr_array_insert(self->selection, target, badge);
  save_selection(self);
}
static void
on_move_up(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  (void)action; move(data, g_variant_get_uint32(parameter), -1);
}
static void
on_move_down(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  (void)action; move(data, g_variant_get_uint32(parameter), 1);
}
static void
on_add(GtkButton *button, GhBadgesPage *self)
{
  guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "index"));
  if (!self->awards || index >= self->awards->len) return;
  GhBadge *badge = g_ptr_array_index(self->awards, index);
  if (!selected_has(self, badge)) g_ptr_array_add(self->selection, gh_badge_copy(badge));
  save_selection(self);
}
static GtkWidget *
icon_button(const gchar *icon, const gchar *tooltip, guint index, GCallback callback, GhBadgesPage *self)
{
  GtkWidget *button = gtk_button_new_from_icon_name(icon);
  gtk_widget_set_tooltip_text(button, tooltip);
  gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(button, "flat");
  g_object_set_data(G_OBJECT(button), "index", GUINT_TO_POINTER(index));
  g_signal_connect(button, "clicked", callback, self);
  return button;
}
static GtkWidget *
row_menu(guint index, guint count)
{
  GtkWidget *button = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "view-more-symbolic");
  gtk_widget_set_tooltip_text(button, _("Badge Actions"));
  gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(button, "flat");
  g_autoptr(GMenu) menu = g_menu_new();
  const gchar *labels[] = { _("Move _Up"), _("Move _Down"), _("_Remove from Profile") };
  const gchar *actions[] = { "badges.move-up", "badges.move-down", "badges.remove" };
  for (guint i = 0; i < G_N_ELEMENTS(actions); i++) {
    if ((i == 0 && index == 0) || (i == 1 && index + 1 >= count)) continue;
    g_autoptr(GMenuItem) item = g_menu_item_new(labels[i], NULL);
    g_menu_item_set_action_and_target_value(item, actions[i], g_variant_new_uint32(index));
    g_menu_append_item(menu, item);
  }
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(button), G_MENU_MODEL(menu));
  return button;
}
static GtkWidget *
badge_row(GhBadgesPage *self, const GhBadge *badge)
{
  GtkWidget *row = adw_action_row_new();
  /* A badge's name and description come from its issuer: text, never markup. */
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), gh_badge_display_name(badge));
  /* A chosen pair whose award and definition are not known is never
   * attributed: it says so until a lookup verifies it. */
  g_autofree gchar *issuer = badge->verified ? gh_badge_describe_issuer(badge)
    : g_strdup(_("Not checked yet — look up your awards to verify this badge"));
  g_autofree gchar *subtitle = badge->verified && badge->description && *badge->description
    ? g_strdup_printf("%s\n%s", issuer, badge->description) : g_strdup(issuer);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
  adw_action_row_add_prefix(ADW_ACTION_ROW(row), artwork_for(self, badge));
  return row;
}
static void
add_row(GhBadgesPage *self, AdwPreferencesGroup *group, GtkWidget *row)
{
  adw_preferences_group_add(group, row);
  g_ptr_array_add(self->row_widgets, row);
}
static GtkWidget *
note_row(const gchar *title)
{
  GtkWidget *row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  gtk_widget_add_css_class(row, "dim-label");
  return row;
}
static void
refresh(GhBadgesPage *self)
{
  clear_rows(self);
  gboolean live = current(self);
  gtk_widget_set_sensitive(GTK_WIDGET(self), live);
  g_clear_pointer(&self->selection, g_ptr_array_unref);
  g_clear_pointer(&self->awards, g_ptr_array_unref);
  self->source = GH_BADGE_SELECTION_EMPTY;
  if (live) {
    self->selection = self->backend.selection(self->backend.owner, &self->source, NULL);
    self->awards = self->backend.awards(self->backend.owner);
  }
  if (!self->selection) self->selection = g_ptr_array_new_with_free_func((GDestroyNotify)gh_badge_free);
  const gchar *description = self->source == GH_BADGE_SELECTION_LEGACY
    ? _("From your older profile badges list. Publishing replaces it with the current format; the old list is not deleted.")
    : self->source == GH_BADGE_SELECTION_CANONICAL
    ? _("From your published profile badges. Changes stay on this device until you publish.")
    : _("The badges your profile shows, in this order. Changes stay on this device until you publish.");
  adw_preferences_group_set_description(self->shown_group, description);
  for (guint i = 0; i < self->selection->len; i++) {
    GhBadge *badge = g_ptr_array_index(self->selection, i);
    GtkWidget *row = badge_row(self, badge);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), row_menu(i, self->selection->len));
    add_row(self, self->shown_group, row);
  }
  if (!self->selection->len) add_row(self, self->shown_group, note_row(_("No badges shown")));
  guint offered = 0;
  for (guint i = 0; self->awards && i < self->awards->len; i++) {
    GhBadge *badge = g_ptr_array_index(self->awards, i);
    if (selected_has(self, badge)) continue;
    GtkWidget *row = badge_row(self, badge);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), icon_button("list-add-symbolic", _("Show on Profile"), i, G_CALLBACK(on_add), self));
    add_row(self, self->awards_group, row);
    offered++;
  }
  if (!offered)
    add_row(self, self->awards_group, note_row(self->awards ? _("No other awards found") : _("Not looked up yet")));
  gtk_widget_set_sensitive(GTK_WIDGET(self->publish_button), live && !self->busy);
  gtk_widget_set_sensitive(GTK_WIDGET(self->lookup_button), live && !self->busy);
}
static void
owner_changed(GObject *owner, GhBadgesPage *self)
{
  (void)owner;
  if (!current(self)) {
    GtkWidget *dialog = gtk_widget_get_ancestor(GTK_WIDGET(self), ADW_TYPE_PREFERENCES_DIALOG);
    if (dialog) adw_preferences_dialog_pop_subpage(ADW_PREFERENCES_DIALOG(dialog));
    return;
  }
  refresh(self);
}

/* ---- publish (explicit) ---- */

static void
published(GObject *source, GAsyncResult *result, gpointer data)
{
  GhBadgesPage *self = data;
  g_autoptr(GError) error = NULL;
  gboolean ok = self->backend.owner && self->backend.publish_finish(G_OBJECT(source), result, &error);
  self->busy = FALSE;
  if (gtk_widget_get_root(GTK_WIDGET(self))) {
    toast(self, ok ? _("Profile badges published") : _("Profile badges not published — try again when your write relays answer"));
    refresh(self);
  }
  g_object_unref(self);
}
static void
on_publish_response(AdwAlertDialog *dialog, const gchar *response, GhBadgesPage *self)
{
  (void)dialog;
  if (g_strcmp0(response, "publish") || !current(self) || self->busy) return;
  self->busy = TRUE;
  refresh(self);
  self->backend.publish_async(self->backend.owner, self->cancel, published, g_object_ref(self));
}
static void
on_publish(GtkButton *button, GhBadgesPage *self)
{
  (void)button;
  if (!current(self)) return;
  g_auto(GStrv) targets = self->backend.dup_targets(self->backend.owner);
  g_autofree gchar *where = join_lines(targets);
  g_autofree gchar *body = g_strdup_printf(
    _("Your profile badge list (%u badges) is published to your write relays:\n%s\n\nAnyone can read it there; accepted events cannot be recalled."),
    self->selection->len, where);
  AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(_("Publish Profile Badges?"), NULL));
  adw_alert_dialog_set_body(dialog, body);
  adw_alert_dialog_add_responses(dialog, "cancel", _("_Cancel"), "publish", _("_Publish"), NULL);
  adw_alert_dialog_set_response_appearance(dialog, "publish", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response(dialog, "cancel");
  adw_alert_dialog_set_close_response(dialog, "cancel");
  if (!targets || !targets[0]) adw_alert_dialog_set_response_enabled(dialog, "publish", FALSE);
  g_signal_connect_object(dialog, "response", G_CALLBACK(on_publish_response), self, 0);
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(self));
}

/* ---- look up awards (explicit) ---- */

static void
looked_up(GObject *source, GAsyncResult *result, gpointer data)
{
  GhBadgesPage *self = data;
  g_autoptr(GError) error = NULL;
  gboolean ok = self->backend.owner && self->backend.lookup_finish(G_OBJECT(source), result, &error);
  self->busy = FALSE;
  if (gtk_widget_get_root(GTK_WIDGET(self))) {
    if (!ok) toast(self, _("Badge lookup failed — check your discovery relays"));
    refresh(self);
  }
  g_object_unref(self);
}
static void
on_lookup_response(AdwAlertDialog *dialog, const gchar *response, GhBadgesPage *self)
{
  (void)dialog;
  if (g_strcmp0(response, "lookup") || !current(self) || self->busy) return;
  self->busy = TRUE;
  refresh(self);
  self->backend.lookup_async(self->backend.owner, self->backend.account(self->backend.owner), NULL,
                             self->cancel, looked_up, g_object_ref(self));
}
static void
on_lookup(GtkButton *button, GhBadgesPage *self)
{
  (void)button;
  if (!current(self)) return;
  g_auto(GStrv) sources = self->backend.dup_sources(self->backend.owner);
  g_autofree gchar *where = join_lines(sources);
  g_autofree gchar *body = g_strdup_printf(
    _("Groundhog asks your discovery relays for badges awarded to this account and checks each award against its issuer:\n%s\n\nThose relays learn that you asked."), where);
  AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(_("Look Up Awards?"), NULL));
  adw_alert_dialog_set_body(dialog, body);
  adw_alert_dialog_add_responses(dialog, "cancel", _("_Cancel"), "lookup", _("_Look Up"), NULL);
  adw_alert_dialog_set_response_appearance(dialog, "lookup", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response(dialog, "cancel");
  adw_alert_dialog_set_close_response(dialog, "cancel");
  if (!sources || !sources[0]) adw_alert_dialog_set_response_enabled(dialog, "lookup", FALSE);
  g_signal_connect_object(dialog, "response", G_CALLBACK(on_lookup_response), self, 0);
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(self));
}

/* ---- construction ---- */

GhBadgesPage *
gh_badges_page_new(const GhBadgesBackend *backend)
{
  g_return_val_if_fail(backend != NULL && G_IS_OBJECT(backend->owner), NULL);
  GhBadgesPage *self = g_object_new(GH_TYPE_BADGES_PAGE, NULL);
  self->backend = *backend;
  self->backend.owner = g_object_ref(backend->owner);
  self->generation = backend->generation(backend->owner);
  g_signal_connect(self->backend.owner, "changed", G_CALLBACK(owner_changed), self);
  refresh(self);
  return self;
}
static void
gh_badges_page_dispose(GObject *object)
{
  GhBadgesPage *self = GH_BADGES_PAGE(object);
  g_cancellable_cancel(self->cancel);
  if (self->backend.owner) {
    g_signal_handlers_disconnect_by_data(self->backend.owner, self);
    g_clear_object(&self->backend.owner);
  }
  G_OBJECT_CLASS(gh_badges_page_parent_class)->dispose(object);
}
static void
gh_badges_page_finalize(GObject *object)
{
  GhBadgesPage *self = GH_BADGES_PAGE(object);
  g_clear_object(&self->cancel);
  g_clear_pointer(&self->selection, g_ptr_array_unref);
  g_clear_pointer(&self->awards, g_ptr_array_unref);
  g_ptr_array_unref(self->row_widgets);
  G_OBJECT_CLASS(gh_badges_page_parent_class)->finalize(object);
}
static void
gh_badges_page_class_init(GhBadgesPageClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gh_badges_page_dispose;
  G_OBJECT_CLASS(klass)->finalize = gh_badges_page_finalize;
}
static GtkWidget *
header_button(const gchar *label, const gchar *css, GCallback callback, GhBadgesPage *self)
{
  GtkWidget *button = gtk_button_new_with_mnemonic(label);
  gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
  if (css) gtk_widget_add_css_class(button, css);
  g_signal_connect(button, "clicked", callback, self);
  return button;
}
static void
gh_badges_page_init(GhBadgesPage *self)
{
  self->cancel = g_cancellable_new();
  self->row_widgets = g_ptr_array_new();
  g_autoptr(GSimpleActionGroup) group = g_simple_action_group_new();
  const GActionEntry entries[] = {
    { "move-up", on_move_up, "u", NULL, NULL, { 0 } },
    { "move-down", on_move_down, "u", NULL, NULL, { 0 } },
    { "remove", on_remove, "u", NULL, NULL, { 0 } },
  };
  g_action_map_add_action_entries(G_ACTION_MAP(group), entries, G_N_ELEMENTS(entries), self);
  gtk_widget_insert_action_group(GTK_WIDGET(self), "badges", G_ACTION_GROUP(group));
  adw_navigation_page_set_title(ADW_NAVIGATION_PAGE(self), _("Profile Badges"));
  adw_navigation_page_set_tag(ADW_NAVIGATION_PAGE(self), "badges");
  GtkWidget *toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
  self->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
  GtkWidget *page = adw_preferences_page_new();
  adw_preferences_page_set_description(ADW_PREFERENCES_PAGE(page),
    _("Badges are awards other accounts gave you. Nothing is looked up or published without asking."));
  self->shown_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(self->shown_group, _("Shown on Your Profile"));
  self->publish_button = GTK_BUTTON(header_button(_("_Publish Profile Badges…"), "suggested-action", G_CALLBACK(on_publish), self));
  adw_preferences_group_set_header_suffix(self->shown_group, GTK_WIDGET(self->publish_button));
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), self->shown_group);
  self->awards_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(self->awards_group, _("Received Awards"));
  adw_preferences_group_set_description(self->awards_group,
    _("Awards whose issuer and definition checked out. Add one to show it on your profile."));
  self->lookup_button = GTK_BUTTON(header_button(_("_Look Up Awards…"), NULL, G_CALLBACK(on_lookup), self));
  adw_preferences_group_set_header_suffix(self->awards_group, GTK_WIDGET(self->lookup_button));
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), self->awards_group);
  adw_toast_overlay_set_child(self->toasts, page);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), GTK_WIDGET(self->toasts));
  adw_navigation_page_set_child(ADW_NAVIGATION_PAGE(self), toolbar);
}

/* ---- app.look-up-badges ------------------------------------------------------------ */

#define BADGES_CONTEXT "groundhog-badges-ui"
/* Reference-counted: every pending confirmation and lookup holds one, so an
 * uninstall (account teardown) only deactivates it and nothing pending
 * touches freed memory. */
typedef struct {
  gint refs;
  GtkApplication *app; /* valid while active */
  GhBadgesBackend backend;
  GCancellable *cancel;
  gboolean active;
} BadgesUI;
static BadgesUI *ui_ref(BadgesUI *ui) { g_atomic_int_inc(&ui->refs); return ui; }
static void
ui_unref(BadgesUI *ui)
{
  if (!g_atomic_int_dec_and_test(&ui->refs)) return;
  g_clear_object(&ui->backend.owner); g_clear_object(&ui->cancel); g_free(ui);
}
/* One Look Up Badges… from a person's menu: bound to the account and
 * generation it was asked under, so a confirmation answered after an
 * account switch starts nothing. */
typedef struct { BadgesUI *ui; gchar *account, *pubkey; guint64 generation; GStrv approved; } LookupRequest;
static void
request_free(LookupRequest *r)
{
  ui_unref(r->ui); g_free(r->account); g_free(r->pubkey); g_strfreev(r->approved); g_free(r);
}
static gboolean
request_current(LookupRequest *r)
{
  BadgesUI *ui = r->ui;
  return ui->active && ui->backend.owner && ui->backend.generation(ui->backend.owner) == r->generation &&
    !g_strcmp0(ui->backend.account(ui->backend.owner), r->account);
}
static void
present_alert(BadgesUI *ui, AdwAlertDialog *dialog)
{
  GtkWindow *window = gtk_application_get_active_window(ui->app);
  adw_dialog_present(ADW_DIALOG(dialog), window ? GTK_WIDGET(window) : NULL);
}
static void on_hints_response(AdwAlertDialog *dialog, const gchar *response, gpointer data);
static void
ui_looked_up(GObject *source, GAsyncResult *result, gpointer data)
{
  LookupRequest *r = data;
  BadgesUI *ui = r->ui;
  g_autoptr(GError) error = NULL;
  gboolean ok = ui->backend.owner && ui->backend.lookup_finish(G_OBJECT(source), result, &error);
  if (!request_current(r)) { request_free(r); return; }
  if (!ok) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(_("Badges Not Looked Up"), NULL));
      adw_alert_dialog_set_body(dialog, _("Your discovery relays did not answer. Check them in Preferences › Network."));
      adw_alert_dialog_add_responses(dialog, "close", _("_Close"), NULL);
      present_alert(ui, dialog);
    }
    request_free(r); return;
  }
  /* A relay hint is information, not permission (§3.12): the user approves
   * each hinted relay before it is contacted, once per lookup. */
  g_auto(GStrv) hints = r->approved ? NULL : ui->backend.dup_hints(ui->backend.owner, r->pubkey);
  if (!hints || !hints[0]) { request_free(r); return; }
  g_autofree gchar *where = g_strjoinv("\n", hints);
  g_autofree gchar *body = g_strdup_printf(
    _("Their badge list points at relays your discovery relays did not include:\n%s\n\nAsk them too? They learn you asked about this account."), where);
  AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(_("Also Ask Hinted Relays?"), NULL));
  adw_alert_dialog_set_body(dialog, body);
  adw_alert_dialog_add_responses(dialog, "cancel", _("_Cancel"), "approve", _("_Ask Them"), NULL);
  adw_alert_dialog_set_default_response(dialog, "cancel");
  adw_alert_dialog_set_close_response(dialog, "cancel");
  r->approved = g_steal_pointer(&hints);
  g_signal_connect(dialog, "response", G_CALLBACK(on_hints_response), r);
  present_alert(ui, dialog);
}
static void
start_lookup(LookupRequest *r)
{
  BadgesUI *ui = r->ui;
  ui->backend.lookup_async(ui->backend.owner, r->pubkey, (const gchar *const *)r->approved, ui->cancel,
                           ui_looked_up, r);
}
static void
on_hints_response(AdwAlertDialog *dialog, const gchar *response, gpointer data)
{
  (void)dialog;
  LookupRequest *r = data;
  if (g_strcmp0(response, "approve") || !request_current(r)) { request_free(r); return; }
  start_lookup(r);
}
static void
on_confirm_response(AdwAlertDialog *dialog, const gchar *response, gpointer data)
{
  (void)dialog;
  LookupRequest *r = data;
  if (g_strcmp0(response, "lookup") || !request_current(r)) { request_free(r); return; }
  start_lookup(r);
}
static void
activate_lookup(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  (void)action;
  BadgesUI *ui = data;
  const gchar *account = NULL, *pubkey = NULL;
  g_variant_get(parameter, "(&s&s)", &account, &pubkey);
  if (!ui->active || !ui->backend.owner || g_strcmp0(account, ui->backend.account(ui->backend.owner)) ||
      !pubkey || !*pubkey)
    return;
  g_auto(GStrv) sources = ui->backend.dup_sources(ui->backend.owner);
  g_autofree gchar *where = join_lines(sources);
  g_autofree gchar *body = g_strdup_printf(
    _("Groundhog asks your discovery relays for the badges this account displays and checks each award against its issuer:\n%s\n\nThose relays learn that you asked about this account."), where);
  AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(_("Look Up Badges?"), NULL));
  adw_alert_dialog_set_body(dialog, body);
  adw_alert_dialog_add_responses(dialog, "cancel", _("_Cancel"), "lookup", _("_Look Up"), NULL);
  adw_alert_dialog_set_response_appearance(dialog, "lookup", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response(dialog, "cancel");
  adw_alert_dialog_set_close_response(dialog, "cancel");
  if (!sources || !sources[0]) adw_alert_dialog_set_response_enabled(dialog, "lookup", FALSE);
  LookupRequest *r = g_new0(LookupRequest, 1);
  r->ui = ui_ref(ui); r->account = g_strdup(account); r->pubkey = g_strdup(pubkey);
  r->generation = ui->backend.generation(ui->backend.owner);
  g_signal_connect(dialog, "response", G_CALLBACK(on_confirm_response), r);
  present_alert(ui, dialog);
}
static void
ui_free(gpointer data)
{
  BadgesUI *ui = data;
  ui->active = FALSE;
  g_cancellable_cancel(ui->cancel);
  g_action_map_remove_action(G_ACTION_MAP(ui->app), "look-up-badges");
  ui->app = NULL;
  ui_unref(ui);
}
void
gh_badges_ui_uninstall(GtkApplication *app)
{
  g_object_set_data(G_OBJECT(app), BADGES_CONTEXT, NULL);
}
void
gh_badges_ui_install(GtkApplication *app, const GhBadgesBackend *backend)
{
  g_return_if_fail(GTK_IS_APPLICATION(app) && backend && G_IS_OBJECT(backend->owner));
  gh_badges_ui_uninstall(app);
  BadgesUI *ui = g_new0(BadgesUI, 1);
  ui->refs = 1; ui->active = TRUE;
  ui->app = app;
  ui->backend = *backend;
  ui->backend.owner = g_object_ref(backend->owner);
  ui->cancel = g_cancellable_new();
  GSimpleAction *action = g_simple_action_new("look-up-badges", G_VARIANT_TYPE("(ss)"));
  g_signal_connect(action, "activate", G_CALLBACK(activate_lookup), ui);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(action));
  g_object_unref(action);
  g_object_set_data_full(G_OBJECT(app), BADGES_CONTEXT, ui, ui_free);
}