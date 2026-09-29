#include "gh-conversation-info-dialog.h"
#include "gh-conversation-actions.h"
#include "gh-conversation-view.h"
#include "gh-expiry.h"
#include "gh-notifier.h"
#include "gh-privacy-summary.h"
#include "gh-store-contacts.h"
#include "gh-store-conversations.h"

#include <glib/gi18n.h>
#include <nostr-utils.h>
#include <nostr/nip19/nip19.h>
#include <stdlib.h>
#include <string.h>

/* "npub1…" for a lowercase hex pubkey; abbreviated "npub1abcde…wxyz". */
static gchar *
npub_of(const gchar *pubkey_hex, gboolean abbreviated)
{
  guint8 bytes[32];
  char *npub = NULL;
  if (!pubkey_hex || strlen(pubkey_hex) != 64 || !nostr_hex2bin(bytes, pubkey_hex, sizeof bytes) ||
      nostr_nip19_encode_npub(bytes, &npub) != 0 || !npub)
    return g_strdup(pubkey_hex ? pubkey_hex : "");
  gsize length = strlen(npub);
  gchar *out = abbreviated && length > 16
                 ? g_strdup_printf("%.10s…%s", npub, npub + length - 4)
                 : g_strdup(npub);
  free(npub);
  return out;
}

/* ---- GhConversationInfoPerson (gh-conversation-info-person.blp) ------------------ */

#define GH_TYPE_CONVERSATION_INFO_PERSON (gh_conversation_info_person_get_type())
G_DECLARE_FINAL_TYPE(GhConversationInfoPerson, gh_conversation_info_person, GH,
                     CONVERSATION_INFO_PERSON, AdwActionRow)

struct _GhConversationInfoPerson {
  AdwActionRow parent_instance;
  AdwAvatar *avatar;
  GtkImage *verified_icon;
  GtkButton *copy_button;
  gchar *pubkey;
  gchar *npub;
  gchar *name;
  gchar *nip05;
};

G_DEFINE_FINAL_TYPE(GhConversationInfoPerson, gh_conversation_info_person, ADW_TYPE_ACTION_ROW)

static void
gh_conversation_info_person_dispose(GObject *object)
{
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_CONVERSATION_INFO_PERSON);
  G_OBJECT_CLASS(gh_conversation_info_person_parent_class)->dispose(object);
}

static void
gh_conversation_info_person_finalize(GObject *object)
{
  GhConversationInfoPerson *self = GH_CONVERSATION_INFO_PERSON(object);
  g_free(self->pubkey);
  g_free(self->npub);
  g_free(self->name);
  g_free(self->nip05);
  G_OBJECT_CLASS(gh_conversation_info_person_parent_class)->finalize(object);
}

static void
gh_conversation_info_person_class_init(GhConversationInfoPersonClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  G_OBJECT_CLASS(klass)->dispose = gh_conversation_info_person_dispose;
  G_OBJECT_CLASS(klass)->finalize = gh_conversation_info_person_finalize;
  gtk_widget_class_set_template_from_resource(
    widget_class, "/org/nostr/Groundhog/ui/gh-conversation-info-person.ui");
  gtk_widget_class_bind_template_child(widget_class, GhConversationInfoPerson, avatar);
  gtk_widget_class_bind_template_child(widget_class, GhConversationInfoPerson, verified_icon);
  gtk_widget_class_bind_template_child(widget_class, GhConversationInfoPerson, copy_button);
}

static void
gh_conversation_info_person_init(GhConversationInfoPerson *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
}

/* The row's text: the name, then the short npub, the claimed NIP-05 and the
 * local verification record. */
static void
person_update(GhConversationInfoPerson *self, gboolean verified)
{
  g_autofree gchar *short_npub = npub_of(self->pubkey, TRUE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self), self->name ? self->name : short_npub);
  adw_avatar_set_text(self->avatar, self->name ? self->name : short_npub);
  g_autoptr(GString) subtitle = g_string_new(self->name ? short_npub : NULL);
  if (self->nip05) {
    if (subtitle->len)
      g_string_append_c(subtitle, '\n');
    /* PD-2: NIP-05 is never looked up, so it is only what they claim. */
    g_string_append_printf(subtitle, _("Says they are %s (not checked)"), self->nip05);
  }
  if (verified) {
    if (subtitle->len)
      g_string_append_c(subtitle, '\n');
    g_string_append(subtitle, _("You marked this key verified"));
  }
  adw_action_row_set_subtitle(ADW_ACTION_ROW(self), subtitle->str);
  gtk_widget_set_visible(GTK_WIDGET(self->verified_icon), verified);
}

static GhConversationInfoPerson *
person_new(const gchar *pubkey, const gchar *name, const gchar *nip05)
{
  GhConversationInfoPerson *self = g_object_new(GH_TYPE_CONVERSATION_INFO_PERSON, NULL);
  self->pubkey = g_strdup(pubkey);
  self->npub = npub_of(pubkey, FALSE);
  self->name = name && *name ? g_strdup(name) : NULL;
  self->nip05 = nip05 && *nip05 ? g_strdup(nip05) : NULL;
  gtk_actionable_set_action_target(GTK_ACTIONABLE(self->copy_button), "s", self->npub);
  return self;
}

/* ---- GhConversationInfoDialog -------------------------------------------------------- */

/* The privacy summary's backends are the conversation model's (and the
 * store schema's) values. */
G_STATIC_ASSERT((gint)GH_PRIVACY_BACKEND_NIP17 == (gint)GH_CONVERSATION_BACKEND_NIP17);
G_STATIC_ASSERT((gint)GH_PRIVACY_BACKEND_NIP29 == (gint)GH_CONVERSATION_BACKEND_NIP29);
G_STATIC_ASSERT((gint)GH_PRIVACY_BACKEND_MLS == (gint)GH_CONVERSATION_BACKEND_MLS);

/* Timer row positions (data/ui/gh-conversation-info-dialog.blp). */
static const gint64 timer_choices[] = {
  GH_EXPIRY_TIMER_OFF, GH_EXPIRY_TIMER_DAY, GH_EXPIRY_TIMER_WEEK, GH_EXPIRY_TIMER_FOUR_WEEKS,
};

struct _GhConversationInfoDialog {
  AdwDialog parent_instance;

  AdwToastOverlay *toasts;
  AdwNavigationView *navigation;
  AdwAvatar *avatar;
  GtkLabel *title_label;
  GtkLabel *subtitle_label;
  AdwPreferencesGroup *people_group;
  AdwActionRow *privacy_row;
  GtkImage *privacy_icon;
  AdwExpanderRow *visible_row;
  AdwExpanderRow *unprotected_row;
  AdwActionRow *storage_row;
  AdwActionRow *mute_row;
  GtkMenuButton *mute_button;
  AdwComboRow *timer_row;
  AdwActionRow *block_row;
  AdwActionRow *forget_row;
  AdwPreferencesGroup *their_group;
  AdwActionRow *their_key_row;
  GtkButton *their_copy_button;
  AdwPreferencesGroup *code_group;
  GtkLabel *their_code_title;
  GtkLabel *their_code_label;
  GtkLabel *your_code_label;
  AdwActionRow *your_key_row;
  GtkButton *your_copy_button;
  AdwActionRow *verified_row;
  GtkButton *verify_button;
  AdwAlertDialog *block_dialog;
  AdwAlertDialog *forget_dialog;

  GhConversation *conversation;
  gchar *room_id;
  gchar *title;
  GhConversationStore *model;
  GhStoreConversations *conversations;
  GhStore *store; /* borrowed while the conversation is listed */
  GhExpiry *expiry;
  GhNotifier *notifier;
  GPtrArray *people;     /* GhConversationInfoPerson, not owned (rows of people_group) */
  gchar *verify_pubkey;  /* whom the verify page shows */
  gboolean syncing_timer;
};

G_DEFINE_FINAL_TYPE(GhConversationInfoDialog, gh_conversation_info_dialog, ADW_TYPE_DIALOG)

static void
toast(GhConversationInfoDialog *self, const gchar *title)
{
  AdwToast *toast = adw_toast_new(title);
  adw_toast_set_use_markup(toast, FALSE);
  adw_toast_overlay_add_toast(self->toasts, toast);
}

/* A store failure: the reason goes to the log (it never names the room or a
 * key), the user gets plain words. */
static void
report_failure(GhConversationInfoDialog *self, const gchar *what, GError *error)
{
  g_message("Groundhog could not %s: %s", what, error ? error->message : "no message store");
  toast(self, _("Groundhog couldn’t save this change"));
}

static gint64
now_unix(GhConversationInfoDialog *self)
{
  return self->store ? gh_clock_get_unix(gh_store_get_clock(self->store))
                     : g_get_real_time() / G_USEC_PER_SEC;
}

static const gchar *
unavailable_reason(void)
{
  return _("Unavailable while Groundhog isn’t saving messages on this device");
}

static GhConversationInfoPerson *
find_person(GhConversationInfoDialog *self, const gchar *pubkey)
{
  for (guint i = 0; i < self->people->len; i++) {
    GhConversationInfoPerson *person = g_ptr_array_index(self->people, i);
    if (g_strcmp0(person->pubkey, pubkey) == 0)
      return person;
  }
  return NULL;
}

static gint64
verified_at(GhConversationInfoDialog *self, const gchar *pubkey)
{
  gint64 at = 0;
  g_autoptr(GError) error = NULL;
  if (self->store && !gh_store_contacts_get_verified(self->store, pubkey, &at, &error))
    g_message("Groundhog could not read a verification mark: %s", error->message);
  return at;
}

/* ---- detaching ------------------------------------------------------------------------ */

/* The conversation left the model (forgotten, blocked, account switch): the
 * store it belongs to may close next, so nothing of it is used again. */
static void
detach(GhConversationInfoDialog *self)
{
  if (self->model)
    g_signal_handlers_disconnect_by_data(self->model, self);
  g_clear_object(&self->model);
  g_clear_object(&self->conversations);
  g_clear_object(&self->expiry);
  self->store = NULL;
}

static void
on_model_changed(GhConversationInfoDialog *self)
{
  if (gh_conversation_store_lookup(self->model, self->room_id) == self->conversation)
    return;
  detach(self);
  adw_dialog_force_close(ADW_DIALOG(self));
}

/* ---- mute ----------------------------------------------------------------------------- */

static gchar *
format_until(gint64 until, gint64 now)
{
  g_autoptr(GDateTime) when = g_date_time_new_from_unix_local(until);
  g_autoptr(GDateTime) today = g_date_time_new_from_unix_local(now);
  if (!when || !today)
    return g_strdup("");
  if (g_date_time_get_year(when) == g_date_time_get_year(today) &&
      g_date_time_get_day_of_year(when) == g_date_time_get_day_of_year(today))
    return g_date_time_format(when, "%H:%M");
  if (until - now < 6 * 24 * 3600)
    /* TRANSLATORS: g_date_time_format() format, e.g. "Tuesday 14:30". */
    return g_date_time_format(when, _("%A %H:%M"));
  return g_date_time_format(when, "%x");
}

static void
sync_mute(GhConversationInfoDialog *self)
{
  GtkWidget *row = GTK_WIDGET(self->mute_row);
  if (!self->conversations) {
    gtk_widget_set_sensitive(row, FALSE);
    adw_action_row_set_subtitle(self->mute_row, unavailable_reason());
    gtk_widget_action_set_enabled(GTK_WIDGET(self), "info.unmute", FALSE);
    return;
  }
  GhStoreNotifyState state = { 0 };
  g_autoptr(GError) error = NULL;
  if (!gh_store_conversations_get_notify_state(self->conversations, self->room_id, &state,
                                               &error))
    g_message("Groundhog could not read a mute: %s", error->message);
  gint64 now = now_unix(self);
  gboolean muted = state.muted_until == GH_STORE_CONVERSATIONS_MUTED_ALWAYS ||
                   state.muted_until > now;
  g_autofree gchar *subtitle = NULL;
  if (state.muted_until == GH_STORE_CONVERSATIONS_MUTED_ALWAYS) {
    subtitle = g_strdup(_("Muted until you turn notifications back on"));
  } else if (muted) {
    g_autofree gchar *until = format_until(state.muted_until, now);
    subtitle = g_strdup_printf(_("Muted until %s"), until);
  } else {
    subtitle = g_strdup(_("Notifications are on"));
  }
  adw_action_row_set_subtitle(self->mute_row, subtitle);
  gtk_menu_button_set_label(self->mute_button, muted ? _("Change") : _("Mute"));
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "info.unmute", muted);
}

static void
set_mute(GhConversationInfoDialog *self, gint64 muted_until)
{
  g_autoptr(GError) error = NULL;
  /* N3: what shows for a muted conversation goes now. */
  if (!self->conversations ||
      !gh_conversation_actions_mute(self->conversations, self->notifier, self->room_id,
                                    muted_until, &error)) {
    report_failure(self, "change a mute", error);
    return;
  }
  sync_mute(self);
}

static void
action_mute(GtkWidget *widget, const char *name, GVariant *parameter)
{
  GhConversationInfoDialog *self = GH_CONVERSATION_INFO_DIALOG(widget);
  (void)name;
  const gchar *choice = g_variant_get_string(parameter, NULL);
  if (g_str_equal(choice, "always")) {
    set_mute(self, GH_STORE_CONVERSATIONS_MUTED_ALWAYS);
    return;
  }
  gchar *end = NULL;
  gint64 seconds = g_ascii_strtoll(choice, &end, 10);
  if (!end || *end || seconds <= 0 || seconds > GH_EXPIRY_TIMER_FOUR_WEEKS) {
    g_warning("Groundhog ignored an unknown mute duration");
    return;
  }
  set_mute(self, now_unix(self) + seconds);
}

static void
action_unmute(GtkWidget *widget, const char *name, GVariant *parameter)
{
  (void)name;
  (void)parameter;
  set_mute(GH_CONVERSATION_INFO_DIALOG(widget), 0);
}

/* ---- disappearing timer --------------------------------------------------------------- */

static void
sync_timer(GhConversationInfoDialog *self)
{
  self->syncing_timer = TRUE;
  gint64 seconds = 0;
  g_autoptr(GError) error = NULL;
  gboolean known = self->expiry &&
                   gh_expiry_get_timer(self->expiry, self->room_id, &seconds, &error);
  if (self->expiry && !known)
    g_message("Groundhog could not read a disappearing timer: %s", error->message);
  guint position = GTK_INVALID_LIST_POSITION;
  for (guint i = 0; known && i < G_N_ELEMENTS(timer_choices); i++)
    if (timer_choices[i] == seconds)
      position = i;
  adw_combo_row_set_selected(self->timer_row, position);
  gtk_widget_set_sensitive(GTK_WIDGET(self->timer_row), known);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(self->timer_row),
                              known ? _("New messages from you; received ones keep their "
                                        "sender’s timer")
                                    : unavailable_reason());
  self->syncing_timer = FALSE;
}

static void
on_timer_selected(GhConversationInfoDialog *self)
{
  if (self->syncing_timer)
    return;
  guint position = adw_combo_row_get_selected(self->timer_row);
  if (position >= G_N_ELEMENTS(timer_choices))
    return;
  g_autoptr(GError) error = NULL;
  if (!self->expiry ||
      !gh_expiry_set_timer(self->expiry, self->room_id, timer_choices[position], &error))
    report_failure(self, "change a disappearing timer", error);
  sync_timer(self);
}

/* ---- block and forget ---------------------------------------------------------------- */

/* The window the dialog shows in, for the toast that outlives it. */
static AdwToastOverlay *
window_toasts(GhConversationInfoDialog *self)
{
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  return GH_IS_WINDOW(root) ? gh_window_get_toasts(GH_WINDOW(root)) : NULL;
}

typedef struct {
  GhStoreConversations *conversations;
  gchar *room_id;
} Unblock;

static void
unblock_free(gpointer data)
{
  Unblock *unblock = data;
  g_object_unref(unblock->conversations);
  g_free(unblock->room_id);
  g_free(unblock);
}

static void
on_undo_block(AdwToast *toast, Unblock *unblock)
{
  (void)toast;
  g_autoptr(GError) error = NULL;
  if (!gh_conversation_actions_block(unblock->conversations, NULL, unblock->room_id, FALSE,
                                     &error))
    g_message("Groundhog could not unblock a conversation: %s", error->message);
}

static void
action_block(GtkWidget *widget, const char *name, GVariant *parameter)
{
  GhConversationInfoDialog *self = GH_CONVERSATION_INFO_DIALOG(widget);
  (void)name;
  (void)parameter;
  adw_dialog_present(ADW_DIALOG(self->block_dialog), widget);
}

static void
on_block_response(AdwAlertDialog *dialog, const gchar *response, GhConversationInfoDialog *self)
{
  (void)dialog;
  if (g_strcmp0(response, "block-confirm") != 0 || !self->conversations)
    return;
  AdwToastOverlay *toasts = window_toasts(self);
  g_autoptr(GhStoreConversations) conversations = g_object_ref(self->conversations);
  g_autofree gchar *room_id = g_strdup(self->room_id);
  g_autoptr(GhNotifier) notifier = self->notifier ? g_object_ref(self->notifier) : NULL;
  g_autoptr(GError) error = NULL;
  /* The model drops the room, which closes this dialog (on_model_changed):
   * only the references taken above are used after this. */
  if (!gh_conversation_actions_block(conversations, notifier, room_id, TRUE, &error)) {
    report_failure(self, "block a conversation", error);
    return;
  }
  if (toasts) {
    AdwToast *done = adw_toast_new(_("Conversation blocked"));
    adw_toast_set_use_markup(done, FALSE);
    adw_toast_set_button_label(done, _("_Undo"));
    Unblock *unblock = g_new0(Unblock, 1);
    unblock->conversations = g_steal_pointer(&conversations);
    unblock->room_id = g_steal_pointer(&room_id);
    g_signal_connect_data(done, "button-clicked", G_CALLBACK(on_undo_block), unblock,
                          (GClosureNotify)(void (*)(void))unblock_free, 0);
    adw_toast_overlay_add_toast(toasts, done);
  }
}

static void
action_forget(GtkWidget *widget, const char *name, GVariant *parameter)
{
  GhConversationInfoDialog *self = GH_CONVERSATION_INFO_DIALOG(widget);
  (void)name;
  (void)parameter;
  adw_dialog_present(ADW_DIALOG(self->forget_dialog), widget);
}

static void
on_forget_response(AdwAlertDialog *dialog, const gchar *response, GhConversationInfoDialog *self)
{
  (void)dialog;
  if (g_strcmp0(response, "forget-confirm") != 0 || !self->conversations)
    return;
  AdwToastOverlay *toasts = window_toasts(self);
  g_autoptr(GhStoreConversations) conversations = g_object_ref(self->conversations);
  g_autofree gchar *room_id = g_strdup(self->room_id);
  g_autoptr(GhNotifier) notifier = self->notifier ? g_object_ref(self->notifier) : NULL;
  g_autoptr(GError) error = NULL;
  /* ST-9: the rows go, the tombstone refuses older backfill, and the model
   * drops the room, which closes this dialog: only the references taken
   * above are used after this. */
  if (!gh_conversation_actions_forget(conversations, notifier, room_id, &error)) {
    report_failure(self, "forget a conversation", error);
    return;
  }
  if (toasts) {
    AdwToast *done = adw_toast_new(_("Conversation forgotten on this device"));
    adw_toast_set_use_markup(done, FALSE);
    adw_toast_overlay_add_toast(toasts, done);
  }
}

/* ---- verify --------------------------------------------------------------------------- */

static void
sync_verified(GhConversationInfoDialog *self)
{
  if (!self->verify_pubkey)
    return;
  gint64 at = verified_at(self, self->verify_pubkey);
  gtk_widget_set_visible(GTK_WIDGET(self->verified_row), at > 0);
  gtk_widget_set_visible(GTK_WIDGET(self->verify_button), at == 0);
  gtk_widget_set_sensitive(GTK_WIDGET(self->verify_button), self->store != NULL);
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->verify_button),
                              self->store ? NULL : unavailable_reason());
  if (at > 0) {
    g_autoptr(GDateTime) when = g_date_time_new_from_unix_local(at);
    g_autofree gchar *date = when ? g_date_time_format(when, "%x") : g_strdup("");
    g_autofree gchar *subtitle = g_strdup_printf(_("On %s, on this device"), date);
    adw_action_row_set_subtitle(self->verified_row, subtitle);
  }
  GhConversationInfoPerson *person = find_person(self, self->verify_pubkey);
  if (person)
    person_update(person, at > 0);
}

static void
action_verify(GtkWidget *widget, const char *name, GVariant *parameter)
{
  GhConversationInfoDialog *self = GH_CONVERSATION_INFO_DIALOG(widget);
  (void)name;
  const gchar *pubkey = g_variant_get_string(parameter, NULL);
  GhConversationInfoPerson *person = find_person(self, pubkey);
  const gchar *account = gh_conversation_get_account(self->conversation);
  /* One code per person, each from that person's key alone (W15 B2). */
  g_autofree gchar *their_code = gh_privacy_fingerprint(pubkey);
  g_autofree gchar *your_code = gh_privacy_fingerprint(account);
  if (!person || !their_code || !your_code)
    return;
  g_free(self->verify_pubkey);
  self->verify_pubkey = g_strdup(pubkey);

  /* A group title is markup (AdwPreferencesGroup): the name is escaped. */
  g_autofree gchar *short_npub = npub_of(pubkey, TRUE);
  g_autofree gchar *escaped = g_markup_escape_text(person->name ? person->name : short_npub, -1);
  g_autofree gchar *their_title = g_strdup_printf(_("%s’s Key"), escaped);
  adw_preferences_group_set_title(self->their_group, their_title);
  g_autofree gchar *their_key = gh_privacy_format_key(person->npub);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->their_key_row), their_key);
  gtk_actionable_set_action_target(GTK_ACTIONABLE(self->their_copy_button), "s", person->npub);

  /* Plain text: the name is not markup here. */
  g_autofree gchar *their_code_title = g_strdup_printf(_("%s’s code"),
                                                       person->name ? person->name : short_npub);
  gtk_label_set_text(self->their_code_title, their_code_title);
  gtk_label_set_text(self->their_code_label, their_code);
  gtk_label_set_text(self->your_code_label, your_code);
  /* Read digit by digit, as they are compared. */
  g_autofree gchar *their_spoken = gh_privacy_fingerprint_spoken(their_code);
  g_autofree gchar *your_spoken = gh_privacy_fingerprint_spoken(your_code);
  g_autofree gchar *their_label = g_strdup_printf(_("%s: %s"), their_code_title, their_spoken);
  g_autofree gchar *your_label = g_strdup_printf(_("Your code: %s"), your_spoken);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->their_code_label),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, their_label, -1);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->your_code_label),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, your_label, -1);

  g_autofree gchar *own_npub = npub_of(account, FALSE);
  g_autofree gchar *own_key = gh_privacy_format_key(own_npub);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->your_key_row), own_key);
  gtk_actionable_set_action_target(GTK_ACTIONABLE(self->your_copy_button), "s", own_npub);

  sync_verified(self);
  if (adw_navigation_view_get_visible_page(self->navigation) !=
      adw_navigation_view_find_page(self->navigation, "verify"))
    adw_navigation_view_push_by_tag(self->navigation, "verify");
}

static void
set_verified(GhConversationInfoDialog *self, gint64 at)
{
  g_autoptr(GError) error = NULL;
  if (!self->verify_pubkey || !self->store ||
      !gh_store_contacts_set_verified(self->store, self->verify_pubkey, at, &error)) {
    report_failure(self, "change a verification mark", error);
    return;
  }
  sync_verified(self);
  toast(self, at > 0 ? _("Marked as verified on this device") : _("Verification mark removed"));
}

static void
action_mark_verified(GtkWidget *widget, const char *name, GVariant *parameter)
{
  GhConversationInfoDialog *self = GH_CONVERSATION_INFO_DIALOG(widget);
  (void)name;
  (void)parameter;
  set_verified(self, MAX(now_unix(self), 1));
}

static void
action_clear_verified(GtkWidget *widget, const char *name, GVariant *parameter)
{
  (void)name;
  (void)parameter;
  set_verified(GH_CONVERSATION_INFO_DIALOG(widget), 0);
}

static void
action_copy_key(GtkWidget *widget, const char *name, GVariant *parameter)
{
  GhConversationInfoDialog *self = GH_CONVERSATION_INFO_DIALOG(widget);
  (void)name;
  const gchar *text = g_variant_get_string(parameter, NULL);
  if (!*text)
    return;
  gdk_clipboard_set_text(gtk_widget_get_clipboard(widget), text);
  toast(self, _("Copied"));
}

static void
on_person_activated(GhConversationInfoPerson *person, GhConversationInfoDialog *self)
{
  gtk_widget_activate_action(GTK_WIDGET(self), "info.verify", "s", person->pubkey);
}

/* ---- filling ---------------------------------------------------------------------------- */

static void
add_text_rows(AdwExpanderRow *expander, GStrv lines)
{
  for (guint i = 0; lines && lines[i]; i++) {
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), lines[i]);
    adw_expander_row_add_row(expander, row);
  }
}

static void
fill(GhConversationInfoDialog *self, const GhConversationInfoServices *services)
{
  const gchar *const *peers = gh_conversation_get_peers(self->conversation);
  guint n_peers = g_strv_length((gchar **)peers);
  for (guint i = 0; i < n_peers; i++) {
    GhConversationInfoProfile profile = { 0 };
    if (services->profile)
      services->profile(peers[i], &profile, services->profile_data);
    GhConversationInfoPerson *person = person_new(peers[i], profile.name, profile.nip05);
    g_signal_connect_object(person, "activated", G_CALLBACK(on_person_activated), self, 0);
    adw_preferences_group_add(self->people_group, GTK_WIDGET(person));
    g_ptr_array_add(self->people, person);
    person_update(person, verified_at(self, peers[i]) > 0);
  }
  gtk_widget_set_visible(GTK_WIDGET(self->people_group), n_peers > 0);

  /* One other person: their name; otherwise the conversation's title. */
  GhConversationInfoPerson *only = n_peers == 1 ? g_ptr_array_index(self->people, 0) : NULL;
  g_autofree gchar *only_short = only ? npub_of(only->pubkey, TRUE) : NULL;
  self->title = g_strdup(only ? (only->name ? only->name : only_short)
                              : gh_conversation_get_title(self->conversation));
  gtk_label_set_text(self->title_label, self->title);
  adw_avatar_set_text(self->avatar, self->title);

  GhPrivacyContext context = {
    .backend = (GhPrivacyBackend)gh_conversation_get_backend(self->conversation),
    .peer_name = only ? self->title : NULL,
    .n_people = n_peers,
    .is_request = gh_conversation_get_is_request(self->conversation),
    .subject = gh_conversation_get_subject(self->conversation),
    .storage = !self->store                      ? GH_PRIVACY_STORAGE_NONE
               : gh_store_is_ephemeral(self->store) ? GH_PRIVACY_STORAGE_MEMORY
                                                    : GH_PRIVACY_STORAGE_SAVED,
  };
  g_autoptr(GhPrivacySummary) summary = gh_privacy_summary_new(&context);
  if (summary) {
    gtk_label_set_text(self->subtitle_label, summary->subtitle);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->privacy_row), summary->heading);
    adw_action_row_set_subtitle(self->privacy_row, summary->encrypted);
    gtk_image_set_from_icon_name(self->privacy_icon, summary->icon_name);
    add_text_rows(self->visible_row, summary->visible);
    add_text_rows(self->unprotected_row, summary->unprotected);
    adw_action_row_set_subtitle(self->storage_row, summary->storage);
  }

  /* Block and Forget name the person; their copy is plain text. */
  g_autofree gchar *block_heading = only ? g_strdup_printf(_("Block %s?"), self->title)
                                         : g_strdup(_("Block This Conversation?"));
  adw_alert_dialog_set_heading(self->block_dialog, block_heading);
  g_autofree gchar *unblock = only
    ? g_strdup_printf(_("To unblock it, choose Undo, or later start a new message to %s."),
                      self->title)
    : g_strdup(_("To unblock it, choose Undo, or later start a new message to the same people."));
  g_autofree gchar *block_body = g_strconcat(
    _("The conversation leaves your list and Groundhog stops notifying you about it. "
      "Messages sent to it while it is blocked are not saved on this device.\n\nNobody is told. "
      "Relays can still deliver their messages to your message relays."), "\n\n", unblock, NULL);
  adw_alert_dialog_set_body(self->block_dialog, block_body);
  g_autofree gchar *forget_body = only
    ? g_strdup_printf(_("Its messages and draft are deleted from this device, and older messages "
                        "won't come back from relays. A new message starts it again.\n\n%s still "
                        "has their copy, and encrypted copies may remain on relays."), self->title)
    : g_strdup(_("Its messages and draft are deleted from this device, and older messages won't "
                 "come back from relays. A new message starts it again.\n\nThe others still "
                 "have their copies, and encrypted copies may remain on relays."));
  adw_alert_dialog_set_body(self->forget_dialog, forget_body);

  gboolean stored = self->conversations != NULL;
  gtk_widget_set_sensitive(GTK_WIDGET(self->block_row), stored);
  gtk_widget_set_sensitive(GTK_WIDGET(self->forget_row), stored);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "info.block", stored);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "info.forget", stored);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "info.mute", stored);
  if (!stored) {
    adw_action_row_set_subtitle(self->block_row, unavailable_reason());
    adw_action_row_set_subtitle(self->forget_row, unavailable_reason());
  }
  sync_mute(self);
  sync_timer(self);
}

GhConversationInfoDialog *
gh_conversation_info_dialog_new(GhConversation *conversation,
                                const GhConversationInfoServices *services)
{
  g_return_val_if_fail(GH_IS_CONVERSATION(conversation), NULL);
  g_return_val_if_fail(services != NULL && GH_IS_CONVERSATION_STORE(services->model), NULL);
  g_return_val_if_fail(!services->conversations ||
                       GH_IS_STORE_CONVERSATIONS(services->conversations), NULL);
  g_return_val_if_fail((services->conversations == NULL) == (services->store == NULL), NULL);
  GhConversationInfoDialog *self = g_object_new(GH_TYPE_CONVERSATION_INFO_DIALOG, NULL);
  self->conversation = g_object_ref(conversation);
  self->room_id = g_strdup(gh_conversation_get_room_id(conversation));
  self->model = g_object_ref(services->model);
  self->conversations = services->conversations ? g_object_ref(services->conversations) : NULL;
  self->store = services->store;
  self->expiry = services->expiry ? g_object_ref(services->expiry) : NULL;
  self->notifier = services->notifier ? g_object_ref(services->notifier) : NULL;
  g_signal_connect_swapped(self->model, "items-changed", G_CALLBACK(on_model_changed), self);
  fill(self, services);
  return self;
}

void
gh_conversation_info_dialog_focus_timer(GhConversationInfoDialog *self)
{
  g_return_if_fail(GH_IS_CONVERSATION_INFO_DIALOG(self));
  adw_dialog_set_focus(ADW_DIALOG(self), GTK_WIDGET(self->timer_row));
}

GhConversation *
gh_conversation_info_dialog_get_conversation(GhConversationInfoDialog *self)
{
  g_return_val_if_fail(GH_IS_CONVERSATION_INFO_DIALOG(self), NULL);
  return self->conversation;
}

static void
gh_conversation_info_dialog_dispose(GObject *object)
{
  GhConversationInfoDialog *self = GH_CONVERSATION_INFO_DIALOG(object);
  detach(self);
  if (self->block_dialog)
    g_signal_handlers_disconnect_by_data(self->block_dialog, self);
  if (self->forget_dialog)
    g_signal_handlers_disconnect_by_data(self->forget_dialog, self);
  g_clear_object(&self->notifier);
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_CONVERSATION_INFO_DIALOG);
  G_OBJECT_CLASS(gh_conversation_info_dialog_parent_class)->dispose(object);
}

static void
gh_conversation_info_dialog_finalize(GObject *object)
{
  GhConversationInfoDialog *self = GH_CONVERSATION_INFO_DIALOG(object);
  g_clear_object(&self->conversation);
  g_free(self->room_id);
  g_free(self->title);
  g_free(self->verify_pubkey);
  g_ptr_array_unref(self->people);
  G_OBJECT_CLASS(gh_conversation_info_dialog_parent_class)->finalize(object);
}

static void
gh_conversation_info_dialog_class_init(GhConversationInfoDialogClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  G_OBJECT_CLASS(klass)->dispose = gh_conversation_info_dialog_dispose;
  G_OBJECT_CLASS(klass)->finalize = gh_conversation_info_dialog_finalize;
  gtk_widget_class_set_template_from_resource(
    widget_class, "/org/nostr/Groundhog/ui/gh-conversation-info-dialog.ui");
#define BIND(name) gtk_widget_class_bind_template_child(widget_class, GhConversationInfoDialog, name)
  BIND(toasts);
  BIND(navigation);
  BIND(avatar);
  BIND(title_label);
  BIND(subtitle_label);
  BIND(people_group);
  BIND(privacy_row);
  BIND(privacy_icon);
  BIND(visible_row);
  BIND(unprotected_row);
  BIND(storage_row);
  BIND(mute_row);
  BIND(mute_button);
  BIND(timer_row);
  BIND(block_row);
  BIND(forget_row);
  BIND(their_group);
  BIND(their_key_row);
  BIND(their_copy_button);
  BIND(code_group);
  BIND(their_code_title);
  BIND(their_code_label);
  BIND(your_code_label);
  BIND(your_key_row);
  BIND(your_copy_button);
  BIND(verified_row);
  BIND(verify_button);
  BIND(block_dialog);
  BIND(forget_dialog);
#undef BIND
  gtk_widget_class_install_action(widget_class, "info.verify", "s", action_verify);
  gtk_widget_class_install_action(widget_class, "info.mark-verified", NULL, action_mark_verified);
  gtk_widget_class_install_action(widget_class, "info.clear-verified", NULL,
                                  action_clear_verified);
  gtk_widget_class_install_action(widget_class, "info.copy-key", "s", action_copy_key);
  gtk_widget_class_install_action(widget_class, "info.mute", "s", action_mute);
  gtk_widget_class_install_action(widget_class, "info.unmute", NULL, action_unmute);
  gtk_widget_class_install_action(widget_class, "info.block", NULL, action_block);
  gtk_widget_class_install_action(widget_class, "info.forget", NULL, action_forget);
}

static void
gh_conversation_info_dialog_init(GhConversationInfoDialog *self)
{
  g_type_ensure(GH_TYPE_CONVERSATION_INFO_PERSON);
  gtk_widget_init_template(GTK_WIDGET(self));
  self->people = g_ptr_array_new();
  g_signal_connect_swapped(self->timer_row, "notify::selected", G_CALLBACK(on_timer_selected),
                           self);
  g_signal_connect(self->block_dialog, "response", G_CALLBACK(on_block_response), self);
  g_signal_connect(self->forget_dialog, "response", G_CALLBACK(on_forget_response), self);
}

/* ---- window glue -------------------------------------------------------------------------- */

#define ATTACH_DATA "groundhog-conversation-info"

typedef struct {
  GhWindow *window; /* not owned: the struct is its data */
  GSimpleAction *action;
  GhConversationInfoServicesFunc services_func;
  gpointer user_data;
  GDestroyNotify destroy;
  GhConversationInfoGroupFunc group_func; /* G20b: relay groups' own dialog */
  gpointer group_data;
} InfoAttach;

static void
attach_free(gpointer data)
{
  InfoAttach *attach = data;
  if (attach->destroy)
    attach->destroy(attach->user_data);
  g_clear_object(&attach->action);
  g_free(attach);
}

/* The shown conversation, when this dialog is for it: a private (NIP-17)
 * conversation only. Its Block, Forget, Mute and copy are NIP-17's; a relay
 * group gets its own info dialog (G20b, gh-group-info-dialog) through the
 * group handler (W15 review non-blocking #2). */
static GhConversation *
shown_any(GhWindow *window)
{
  GhContentPage *content = gh_window_get_content(window);
  GtkWidget *view = gh_content_page_get_view(content);
  if (!gh_content_page_get_conversation_shown(content) || !GH_IS_CONVERSATION_VIEW(view))
    return NULL;
  return gh_conversation_view_get_conversation(GH_CONVERSATION_VIEW(view));
}

static GhConversation *
shown_group(InfoAttach *attach)
{
  GhConversation *conversation = attach->group_func ? shown_any(attach->window) : NULL;
  return conversation &&
         gh_conversation_get_backend(conversation) == GH_CONVERSATION_BACKEND_NIP29
    ? conversation : NULL;
}

static GhConversation *
shown_conversation(GhWindow *window)
{
  GhConversation *conversation = shown_any(window);
  return conversation &&
         gh_conversation_get_backend(conversation) == GH_CONVERSATION_BACKEND_NIP17
    ? conversation : NULL;
}

static void
sync_action(InfoAttach *attach)
{
  g_simple_action_set_enabled(attach->action, shown_conversation(attach->window) != NULL ||
                                                shown_group(attach) != NULL);
}

static void
on_conversation_info(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  InfoAttach *attach = data;
  (void)action;
  (void)parameter;
  GhConversation *group = shown_group(attach);
  if (group) {
    attach->group_func(attach->window, group, attach->group_data);
    return;
  }
  GhConversation *conversation = shown_conversation(attach->window);
  GhConversationInfoServices services = { 0 };
  if (!conversation || !attach->services_func(&services, attach->user_data) || !services.model ||
      gh_conversation_store_lookup(services.model, gh_conversation_get_room_id(conversation)) !=
        conversation)
    return;
  GhConversationInfoDialog *dialog = gh_conversation_info_dialog_new(conversation, &services);
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(attach->window));
}

void
gh_conversation_info_attach(GhWindow *window, GhConversationInfoServicesFunc services_func,
                            gpointer user_data, GDestroyNotify destroy)
{
  g_return_if_fail(GH_IS_WINDOW(window));
  g_return_if_fail(services_func != NULL);
  g_return_if_fail(g_object_get_data(G_OBJECT(window), ATTACH_DATA) == NULL);
  InfoAttach *attach = g_new0(InfoAttach, 1);
  attach->window = window;
  attach->services_func = services_func;
  attach->user_data = user_data;
  attach->destroy = destroy;
  attach->action = g_simple_action_new("conversation-info", NULL);
  g_signal_connect(attach->action, "activate", G_CALLBACK(on_conversation_info), attach);
  g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(attach->action));
  g_object_set_data_full(G_OBJECT(window), ATTACH_DATA, attach, attach_free);
  GhContentPage *content = gh_window_get_content(window);
  GtkStack *stack = gh_content_page_get_stack(content);
  g_signal_connect_swapped(stack, "notify::visible-child-name", G_CALLBACK(sync_action), attach);
  /* Another conversation in the same view (a group after a DM) too. */
  GtkWidget *view = gh_content_page_get_view(content);
  if (GH_IS_CONVERSATION_VIEW(view))
    g_signal_connect_swapped(view, "notify::conversation", G_CALLBACK(sync_action), attach);
  sync_action(attach);
}

void
gh_conversation_info_set_group_handler(GhWindow *window, GhConversationInfoGroupFunc func,
                                       gpointer user_data)
{
  g_return_if_fail(GH_IS_WINDOW(window));
  InfoAttach *attach = g_object_get_data(G_OBJECT(window), ATTACH_DATA);
  g_return_if_fail(attach != NULL);
  attach->group_func = func;
  attach->group_data = func ? user_data : NULL;
  sync_action(attach);
}
