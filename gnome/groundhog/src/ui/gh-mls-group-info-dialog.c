#include "gh-mls-group-info-dialog.h"
#include "../app/gh-test-async-control.h"

#include "gh-mls-attachments.h"

#include "gh-mls-copy.h"
#include "gh-mls-invitee-picker.h"
#include "gh-mls-new-group-page.h"
#include "gh-privacy-summary.h"
#include "gh-recipient.h"

#include <glib/gi18n.h>

/* ---- GhMlsMemberRow (data/ui/gh-mls-member-row.blp) ---------------------------------------- */

#define GH_TYPE_MLS_MEMBER_ROW (gh_mls_member_row_get_type())
G_DECLARE_FINAL_TYPE(GhMlsMemberRow, gh_mls_member_row, GH, MLS_MEMBER_ROW, AdwActionRow)

struct _GhMlsMemberRow {
  AdwActionRow parent_instance;
  AdwAvatar *avatar;
  GtkLabel *identity_badge;
  GtkLabel *role_badge;
  GtkWidget *verify_button;
  GtkWidget *remove_button;
  gchar *pubkey;
  GhMlsRole role;
  GhMlsMemberIdentity identity;
};

G_DEFINE_FINAL_TYPE(GhMlsMemberRow, gh_mls_member_row, ADW_TYPE_ACTION_ROW)

static void
member_remove(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  (void)parameter;
  GhMlsMemberRow *self = GH_MLS_MEMBER_ROW(widget);
  gtk_widget_activate_action(widget, "mls-group.remove", "s", self->pubkey);
}

static void
member_verify(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  (void)parameter;
  GhMlsMemberRow *self = GH_MLS_MEMBER_ROW(widget);
  gtk_widget_activate_action(widget, "mls-group.verify", "s", self->pubkey);
}

static void
gh_mls_member_row_finalize(GObject *object)
{
  g_free(GH_MLS_MEMBER_ROW(object)->pubkey);
  G_OBJECT_CLASS(gh_mls_member_row_parent_class)->finalize(object);
}

static void
gh_mls_member_row_class_init(GhMlsMemberRowClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  G_OBJECT_CLASS(klass)->finalize = gh_mls_member_row_finalize;
  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-mls-member-row.ui");
  gtk_widget_class_bind_template_child(widget_class, GhMlsMemberRow, avatar);
  gtk_widget_class_bind_template_child(widget_class, GhMlsMemberRow, identity_badge);
  gtk_widget_class_bind_template_child(widget_class, GhMlsMemberRow, verify_button);
  gtk_widget_class_bind_template_child(widget_class, GhMlsMemberRow, role_badge);
  gtk_widget_class_bind_template_child(widget_class, GhMlsMemberRow, remove_button);
  gtk_widget_class_install_action(widget_class, "member.remove", NULL, member_remove);
  gtk_widget_class_install_action(widget_class, "member.verify", NULL, member_verify);
}

static void
gh_mls_member_row_init(GhMlsMemberRow *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
}

/* identity/added_by: what GhMlsService knows of who the member is
 * (nostrc-6ukh); added_by: the admin's name or short npub, or NULL;
 * added_by_self: the member added this device. verifiable: offer Verify
 * (an UNVERIFIED member, the service running). */
static GhMlsMemberRow *
member_row_new(const gchar *pubkey, const gchar *name, gboolean is_you, GhMlsRole role,
               gboolean removable, GhMlsMemberIdentity identity, const gchar *added_by,
               gboolean added_by_self, gboolean verifiable)
{
  GhMlsMemberRow *self = g_object_new(GH_TYPE_MLS_MEMBER_ROW, NULL);
  self->pubkey = g_strdup(pubkey);
  self->role = role;
  self->identity = identity;
  g_autofree gchar *npub = gh_recipient_npub_short(pubkey);
  const gchar *shown = name && *name ? name : npub;
  g_autofree gchar *title = is_you ? g_strdup_printf(_("%s (You)"), shown) : g_strdup(shown);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self), title);
  GhMlsMemberCopy copy = gh_mls_member_copy(identity, added_by, added_by_self);
  if (copy.badge) {
    g_autofree gchar *subtitle = name && *name
      ? g_strdup_printf("%s\n%s", npub, copy.explanation) : g_strdup(copy.explanation);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(self), subtitle);
    adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(self), 0);
    gtk_label_set_text(self->identity_badge, copy.badge);
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->identity_badge), copy.explanation);
    gtk_accessible_update_property(GTK_ACCESSIBLE(self), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
                                   copy.accessible, -1);
  } else {
    adw_action_row_set_subtitle(ADW_ACTION_ROW(self), name && *name ? npub : "");
  }
  gtk_widget_set_visible(GTK_WIDGET(self->identity_badge), copy.badge != NULL);
  gh_mls_member_copy_clear(&copy);
  gboolean verify = verifiable && identity == GH_MLS_MEMBER_UNVERIFIED;
  gtk_widget_set_visible(self->verify_button, verify);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "member.verify", verify);
  if (verify) {
    g_autofree gchar *label = g_strdup_printf(_("Verify %s’s Identity"), shown);
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->verify_button),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  }
  adw_avatar_set_text(self->avatar, shown);
  const gchar *badge = gh_mls_role_copy(role);
  gtk_label_set_text(self->role_badge, badge ? badge : "");
  gtk_widget_set_visible(GTK_WIDGET(self->role_badge), badge != NULL);
  gtk_widget_set_visible(self->remove_button, removable);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "member.remove", removable);
  if (removable) {
    g_autofree gchar *label = g_strdup_printf(_("Remove %s from Group"), shown);
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->remove_button),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  }
  return self;
}

/* ---- GhMlsGroupInfoDialog --------------------------------------------------------------- */

struct _GhMlsGroupInfoDialog {
  AdwDialog parent_instance;
  AdwToastOverlay *toasts;
  AdwNavigationView *navigation;
  AdwAvatar *avatar;
  GtkLabel *title_label;
  GtkLabel *subtitle_label;
  GtkLabel *about_label;
  AdwActionRow *privacy_row;
  AdwExpanderRow *visible_row;
  AdwExpanderRow *unprotected_row;
  AdwActionRow *messages_row;
  AdwActionRow *pending_row;
  GtkSpinner *pending_spinner;
  AdwPreferencesGroup *members_group;
  GtkWidget *add_member_button;
  AdwPreferencesGroup *admin_group;
  AdwPreferencesGroup *media_group;        /* nostrc-46k7: file storage */
  AdwActionRow *media_policy_row;
  GtkWidget *update_media_button;
  AdwPreferencesGroup *relays_group;
  GtkWidget *leave_button;
  GhMlsInviteePicker *add_picker;
  GtkLabel *add_reason;
  AdwEntryRow *name_row;
  AdwEntryRow *description_row;
  AdwAlertDialog *leave_dialog;
  AdwAlertDialog *remove_dialog;
  AdwAlertDialog *verify_dialog;
  AdwPreferencesGroup *picture_group;
  AdwActionRow *picture_row;
  GtkSpinner *picture_spinner;
  GtkWidget *show_picture_button;
  GtkWidget *set_picture_button;
  GtkWidget *remove_picture_button;
  AdwAlertDialog *set_picture_dialog;
  AdwAlertDialog *remove_picture_dialog;

  GhMlsGroup *group;
  GhMlsUiContext context;   /* objects referenced; service weak-watched */
  GPtrArray *member_rows;   /* GtkWidget in members_group */
  GPtrArray *relay_rows;    /* GtkWidget in relays_group */
  gchar *removing;          /* the pubkey the remove confirmation asks about */
  GhMlsLeave leave_kind;    /* what the shown leave confirmation said (nostrc-2um6) */
  gchar *verifying;         /* the pubkey the verify confirmation asks about */
  gchar *last_toast;
  guint pending;            /* changes started, not finished */
  GhMlsAttachments *files;  /* the window's group files (W25), or NULL */
  GCancellable *picture_op; /* fetching or setting the picture */
  GCancellable *media_op;   /* updating the group file servers */
  gboolean disposing;
  gint picture_state;       /* GhMlsPictureState shown, -1 before */
  gboolean picture_shown;   /* the avatar shows the decrypted picture */
  GBytes *offered_picture;  /* chosen, awaiting the upload's confirmation */
  gchar *offered_mime;
  GStrv offered_hosts;      /* the hosts that confirmation names */
};

G_DEFINE_FINAL_TYPE(GhMlsGroupInfoDialog, gh_mls_group_info_dialog, ADW_TYPE_DIALOG)

static void
toast(GhMlsGroupInfoDialog *self, const gchar *text)
{
  g_free(self->last_toast);
  self->last_toast = g_strdup(text);
  adw_toast_overlay_add_toast(self->toasts, adw_toast_new(text));
}

static const gchar *
display_name(GhMlsGroupInfoDialog *self, const gchar *pubkey)
{
  return self->context.display_name ? self->context.display_name(pubkey,
                                                                  self->context.names_data)
                                    : NULL;
}

static gboolean
can_manage(GhMlsGroupInfoDialog *self)
{
  /* Leaving (nostrc-2um6): nothing but the leave is sent any more. */
  return self->context.service && gh_mls_group_get_active(self->group) &&
         !gh_mls_group_get_leaving(self->group) && gh_mls_group_get_is_admin(self->group);
}

static void
sync_header(GhMlsGroupInfoDialog *self)
{
  const gchar *name = gh_mls_group_get_name(self->group);
  const gchar *title = name && *name ? name : _("Unnamed Group");
  gtk_label_set_text(self->title_label, title);
  adw_avatar_set_text(self->avatar, title);
  g_auto(GStrv) members = gh_mls_group_dup_members(self->group);
  GhPrivacyContext context = { .backend = GH_PRIVACY_BACKEND_MLS,
                               .n_people = members ? g_strv_length(members) : 0 };
  g_autofree gchar *subtitle = gh_privacy_summary_dup_subtitle(&context);
  gtk_label_set_text(self->subtitle_label, subtitle);
  const gchar *about = gh_mls_group_get_description(self->group);
  gtk_label_set_text(self->about_label, about ? about : "");
  gtk_widget_set_visible(GTK_WIDGET(self->about_label), about && *about);
}

static void
add_privacy_rows(AdwExpanderRow *expander, const gchar *const *lines)
{
  for (guint i = 0; lines && lines[i]; i++) {
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), lines[i]);
    adw_action_row_set_title_lines(ADW_ACTION_ROW(row), 0);
    adw_expander_row_add_row(expander, row);
  }
}

static void
fill_privacy(GhMlsGroupInfoDialog *self)
{
  g_auto(GStrv) members = gh_mls_group_dup_members(self->group);
  GhPrivacyContext context = { .backend = GH_PRIVACY_BACKEND_MLS,
                               .n_people = members ? g_strv_length(members) : 0 };
  g_autoptr(GhPrivacySummary) summary = gh_privacy_summary_new(&context);
  if (!summary)
    return;
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->privacy_row), summary->heading);
  adw_action_row_set_subtitle(self->privacy_row, summary->encrypted);
  add_privacy_rows(self->visible_row, (const gchar *const *)summary->visible);
  add_privacy_rows(self->unprotected_row, (const gchar *const *)summary->unprotected);
}

/* nostrc-46k7: show the group's media policy (which Blossom servers store its
 * files). Admins can update it from their own blossom-servers setting. */
static void
sync_media_policy(GhMlsGroupInfoDialog *self)
{
  gboolean adopted = gh_mls_group_get_adopted(self->group);
  if (!adopted || !self->context.service) {
    gtk_widget_set_visible(GTK_WIDGET(self->media_group), FALSE);
    return;
  }
  MarmotGroupComponents mc;
  memset(&mc, 0, sizeof mc);
  GError *error = NULL;
  if (!gh_mls_service_get_components(self->context.service, self->group, &mc, &error)) {
    g_clear_error(&error);
    gtk_widget_set_visible(GTK_WIDGET(self->media_group), FALSE);
    return;
  }
  if (!mc.has_media_policy || mc.media_policy.default_blob_endpoint_count == 0) {
    marmot_group_components_clear(&mc);
    /* Show the group when admin can add one. */
    gboolean manage = can_manage(self);
    if (manage) {
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->media_policy_row),
                                    _("No file servers configured"));
      adw_action_row_set_subtitle(self->media_policy_row,
                                  _("Pictures and files can't be shared until an admin sets the servers."));
      gtk_widget_set_visible(GTK_WIDGET(self->media_group), TRUE);
      gtk_widget_set_visible(self->update_media_button, TRUE);
    } else {
      gtk_widget_set_visible(GTK_WIDGET(self->media_group), FALSE);
    }
    return;
  }
  g_autoptr(GString) list = g_string_new(NULL);
  for (size_t i = 0; i < mc.media_policy.default_blob_endpoint_count; i++) {
    if (list->len > 0)
      g_string_append(list, ", ");
    g_string_append(list, mc.media_policy.default_blob_endpoints[i].base_url);
  }
  guint n = (guint)mc.media_policy.default_blob_endpoint_count;
  g_autofree gchar *title = g_strdup_printf(
    ngettext("Files stored on %u server", "Files stored on %u servers", n), n);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->media_policy_row), title);
  adw_action_row_set_subtitle(self->media_policy_row, list->str);
  gtk_widget_set_visible(GTK_WIDGET(self->media_group), TRUE);
  gtk_widget_set_visible(self->update_media_button, can_manage(self));
  marmot_group_components_clear(&mc);
}

typedef struct {
  GWeakRef dialog;
  GCancellable *cancellable;
} MediaUpdate;

static void
media_updated(GObject *source, GAsyncResult *result, gpointer data)
{
  if (gh_test_async_defer_result("group-info-service", "media_updated", source, result,
                                  media_updated, data))
    return;
  MediaUpdate *update = data;
  g_autoptr(GError) error = NULL;
  gboolean ok = gh_mls_service_change_finish(GH_MLS_SERVICE(source), result, &error);
  g_autoptr(GhMlsGroupInfoDialog) self = g_weak_ref_get(&update->dialog);
  if (self && self->media_op == update->cancellable) {
    g_clear_object(&self->media_op);
    self->pending--;
    if (!self->disposing && !gtk_widget_in_destruction(GTK_WIDGET(self))) {
      if (ok)
        toast(self, _("File servers updated"));
      else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_autofree gchar *words = gh_mls_error_copy(error);
        toast(self, words);
      }
      sync_media_policy(self);
    }
  }
  g_weak_ref_clear(&update->dialog);
  g_object_unref(update->cancellable);
  g_free(update);
}

static void
action_update_media(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  (void)parameter;
  GhMlsGroupInfoDialog *self = GH_MLS_GROUP_INFO_DIALOG(widget);
  if (!can_manage(self))
    return;
  GSettings *settings = self->context.settings;
  if (!settings)
    return;
  g_auto(GStrv) servers = g_settings_get_strv(settings, "blossom-servers");
  guint n = servers ? g_strv_length(servers) : 0;
  if (n == 0) {
    toast(self, _("No Blossom servers configured in your settings"));
    return;
  }
  if (self->media_op)
    return;
  MediaUpdate *update = g_new0(MediaUpdate, 1);
  g_weak_ref_init(&update->dialog, self);
  update->cancellable = g_cancellable_new();
  self->media_op = g_object_ref(update->cancellable);
  self->pending++;
  gh_mls_service_set_media_policy_async(self->context.service, self->group,
                                        (const gchar *const *)servers, update->cancellable,
                                        media_updated, update);
}

static void
sync_status(GhMlsGroupInfoDialog *self)
{
  gboolean active = gh_mls_group_get_active(self->group);
  const gchar *by = gh_mls_group_get_removed_by(self->group);
  g_autofree gchar *by_npub = by ? gh_recipient_npub_short(by) : NULL;
  const gchar *by_name = by ? display_name(self, by) : NULL;
  g_autofree gchar *ended = gh_mls_end_copy(gh_mls_group_get_end(self->group),
                                            by_name && *by_name ? by_name : by_npub);
  const gchar *read = gh_mls_read_copy(gh_mls_group_get_read_state(self->group));
  if (active && gh_mls_group_get_change_refused(self->group))
    /* An admin's change refused for good, by its cause (nostrc-prrl, L4). */
    read = gh_mls_refused_copy(gh_mls_group_get_refusal(self->group));
  adw_action_row_set_subtitle(self->messages_row, active || !ended ? read : ended);
  gboolean pending = gh_mls_group_get_pending_commit(self->group);
  guint unsent = gh_mls_group_get_unsent_welcomes(self->group);
  g_autofree gchar *words = NULL;
  if (gh_mls_group_get_leaving(self->group))
    words = g_strdup(gh_mls_group_get_leave_via_admin(self->group)
                       ? _("You’re leaving. Waiting for an admin to remove you.")
                       : _("You’re leaving. Waiting for another member to confirm it."));
  else if (gh_mls_group_get_leave_failure(self->group) == GH_MLS_LEAVE_FAILURE_NOT_PROCESSED)
    words = g_strdup(_("Your leave request wasn’t processed by the group admin. You can leave "
                       "on this device only, or ask an admin to remove you."));
  else if (gh_mls_group_get_leave_failed(self->group))
    words = g_strdup(_("Your leave couldn’t continue after the group changed, so you’re still "
                       "a member and can send again. You can leave again."));
  else if (pending)
    words = g_strdup(_("A change to the group is waiting for a relay to accept it."));
  else if (unsent > 0)
    words = g_strdup_printf(g_dngettext(NULL,
                                        "%u invitation hasn’t reached its person’s inbox yet.",
                                        "%u invitations haven’t reached their people’s inboxes "
                                        "yet.", unsent), unsent);
  gtk_widget_set_visible(GTK_WIDGET(self->pending_row), words != NULL);
  gtk_spinner_set_spinning(self->pending_spinner,
                           words != NULL && !gh_mls_group_get_leave_failed(self->group));
  adw_action_row_set_subtitle(self->pending_row, words ? words : "");
  gboolean manage = can_manage(self);
  gtk_widget_set_visible(self->add_member_button, manage);
  gtk_widget_set_visible(GTK_WIDGET(self->admin_group), manage);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "mls-group.add-members", manage);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "mls-group.rename", manage);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "mls-group.save-rename", manage);
  sync_media_policy(self);
  gtk_widget_set_visible(self->leave_button, active);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "mls-group.leave", active);
}

static void
clear_rows(AdwPreferencesGroup *group, GPtrArray *rows)
{
  for (guint i = 0; i < rows->len; i++)
    adw_preferences_group_remove(group, g_ptr_array_index(rows, i));
  g_ptr_array_set_size(rows, 0);
}

static gint
role_rank(GhMlsRole role)
{
  return role == GH_MLS_ROLE_OWNER ? 0 : role == GH_MLS_ROLE_ADMIN ? 1 : 2;
}

static void
sync_members(GhMlsGroupInfoDialog *self)
{
  if (!self->context.service)   /* going: the account switched */
    return;
  clear_rows(self->members_group, self->member_rows);
  g_auto(GStrv) members = gh_mls_group_dup_members(self->group);
  g_auto(GStrv) admins = gh_mls_group_dup_ordered_admins(self->context.service, self->group);
  const gchar *account = gh_mls_service_get_account(self->context.service);
  gboolean manage = can_manage(self);
  for (gint rank = 0; rank < 3; rank++) {
    for (guint i = 0; members && members[i]; i++) {
      GhMlsRole role = gh_mls_role_of((const gchar *const *)admins, members[i]);
      if (role_rank(role) != rank)
        continue;
      gboolean you = g_strcmp0(members[i], account) == 0;
      g_autofree gchar *added_by = NULL;
      GhMlsMemberIdentity identity = gh_mls_group_get_member_identity(self->group, members[i],
                                                                      &added_by);
      gboolean self_added = g_strcmp0(added_by, members[i]) == 0;
      const gchar *adder_name = added_by ? display_name(self, added_by) : NULL;
      g_autofree gchar *adder_npub = added_by ? gh_recipient_npub_short(added_by) : NULL;
      GhMlsMemberRow *row = member_row_new(members[i], display_name(self, members[i]), you,
                                           role, manage && !you, identity,
                                           adder_name && *adder_name ? adder_name : adder_npub,
                                           self_added,
                                           !you && gh_mls_group_get_active(self->group));
      adw_preferences_group_add(self->members_group, GTK_WIDGET(row));
      g_ptr_array_add(self->member_rows, row);
    }
  }
  sync_header(self);
}

static void
sync_relays(GhMlsGroupInfoDialog *self)
{
  clear_rows(self->relays_group, self->relay_rows);
  g_auto(GStrv) relays = gh_mls_group_dup_relays(self->group);
  for (guint i = 0; relays && relays[i]; i++) {
    GhMlsRelayRow *row = gh_mls_relay_row_new(relays[i], FALSE);
    adw_preferences_group_add(self->relays_group, GTK_WIDGET(row));
    g_ptr_array_add(self->relay_rows, row);
  }
}

/* ---- the picture (W25, nostrc-m6tp) ------------------------------------------------- */

static gchar *
picture_text(GhMlsGroupInfoDialog *self, GhMlsPictureState state, const gchar *const *hosts)
{
  g_autofree gchar *where = gh_mls_describe_hosts(hosts);
  guint n = hosts ? g_strv_length((gchar **)hosts) : 0;
  switch (state) {
  case GH_MLS_PICTURE_UNSUPPORTED:
    /* W25 review L2: such a group may have a picture others see. */
    return g_strdup(_("Groundhog can’t show or change the picture of this older kind of "
                      "group."));
  case GH_MLS_PICTURE_WEB:
    return g_strdup(_("The group’s picture is a web address. Groundhog doesn’t load pictures "
                      "from the web, so it shows the group’s initials."));
  case GH_MLS_PICTURE_WEB_UNVERIFIED:
    return g_strdup(_("The group’s picture is a web address Groundhog can’t check, so it’s "
                      "never loaded."));
  case GH_MLS_PICTURE_AVAILABLE:
    if (!where)
      return g_strdup(_("An encrypted picture. Show Picture downloads it."));
    /* W25 review L3: every server that may be asked, in order. */
    if (n == 1)
      return gh_mls_attachments_get_tor(self->files)
        ? g_strdup_printf(_("An encrypted picture. Show Picture downloads it from %s through "
                            "Tor."), where)
        : g_strdup_printf(_("An encrypted picture. Show Picture downloads it from %s, which can "
                            "see your IP address."), where);
    return gh_mls_attachments_get_tor(self->files)
      ? g_strdup_printf(_("An encrypted picture. Show Picture downloads it from the first of %s "
                          "that has it, through Tor."), where)
      : g_strdup_printf(_("An encrypted picture. Show Picture downloads it from the first of %s "
                          "that has it; each server asked can see your IP address."), where);
  case GH_MLS_PICTURE_NO_SERVER:
    return g_strdup(_("An encrypted picture, but the group doesn’t say which server keeps it, "
                      "so it can’t be shown."));
  case GH_MLS_PICTURE_READY:
    return g_strdup(_("An encrypted picture, kept on this device."));
  case GH_MLS_PICTURE_NONE:
  default:
    return g_strdup(_("No picture"));
  }
}

static void
sync_picture(GhMlsGroupInfoDialog *self)
{
  if (!self->files || !self->context.service) {
    gtk_widget_set_visible(GTK_WIDGET(self->picture_group), FALSE);
    return;
  }
  g_autoptr(GBytes) picture = NULL;
  g_auto(GStrv) hosts = NULL;
  GhMlsPictureState state = gh_mls_attachments_get_picture(self->files, self->group, &picture,
                                                           &hosts);
  /* Shown only after the decode guard, with GTK's own loaders. */
  g_autoptr(GdkTexture) texture = NULL;
  if (picture && gh_attachment_check_preview(picture, NULL, NULL, NULL, NULL))
    texture = gdk_texture_new_from_bytes(picture, NULL);
  adw_avatar_set_custom_image(self->avatar, texture ? GDK_PAINTABLE(texture) : NULL);
  self->picture_shown = texture != NULL;
  self->picture_state = state;
  g_autofree gchar *text = picture_text(self, state, (const gchar *const *)hosts);
  adw_action_row_set_subtitle(self->picture_row, text);
  gboolean busy = self->picture_op != NULL;
  gtk_widget_set_visible(GTK_WIDGET(self->picture_spinner), busy);
  gtk_spinner_set_spinning(self->picture_spinner, busy);
  gtk_widget_set_visible(self->show_picture_button, state == GH_MLS_PICTURE_AVAILABLE && !busy);
  gboolean admin = can_manage(self) && state != GH_MLS_PICTURE_UNSUPPORTED;
  gboolean has = state != GH_MLS_PICTURE_NONE && state != GH_MLS_PICTURE_UNSUPPORTED;
  gtk_widget_set_visible(self->set_picture_button, admin && !busy);
  gtk_widget_set_visible(self->remove_picture_button, admin && has && !busy);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "mls-group.show-picture",
                                state == GH_MLS_PICTURE_AVAILABLE && !busy);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "mls-group.set-picture", admin && !busy);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "mls-group.remove-picture",
                                admin && has && !busy);
  gtk_widget_set_visible(GTK_WIDGET(self->picture_group), TRUE);
}

static void
sync_all(GhMlsGroupInfoDialog *self)
{
  sync_header(self);
  sync_status(self);
  sync_members(self);
  sync_relays(self);
  sync_picture(self);
}

/* ---- changes ----------------------------------------------------------------------------- */

typedef struct {
  GhMlsGroupInfoDialog *self; /* a reference */
  gchar *done;               /* the toast when it worked */
} Change;

static void
change_done(GObject *source, GAsyncResult *result, gpointer data)
{
  if (gh_test_async_defer_result("group-info-service", "change_done", source, result,
                                  change_done, data))
    return;
  Change *change = data;
  GhMlsGroupInfoDialog *self = change->self;
  g_autoptr(GError) error = NULL;
  gboolean ok = gh_mls_service_change_finish(GH_MLS_SERVICE(source), result, &error);
  self->pending--;
  if (!self->disposing && !gtk_widget_in_destruction(GTK_WIDGET(self))) {
    if (ok) {
      toast(self, change->done);
    } else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_message("Groundhog could not change an encrypted group: %s", error->message);
      g_autofree gchar *words = gh_mls_error_copy(error);
      toast(self, words);
    }
    sync_all(self);
  }
  g_object_unref(self);
  g_free(change->done);
  g_free(change);
}

static Change *
change_new(GhMlsGroupInfoDialog *self, const gchar *done)
{
  Change *change = g_new0(Change, 1);
  change->self = g_object_ref(self);
  change->done = g_strdup(done);
  self->pending++;
  return change;
}

static void
sync_add_reason(GhMlsGroupInfoDialog *self)
{
  const gchar *reason = NULL;
  if (gh_mls_invitee_picker_get_n_selected(self->add_picker) == 0)
    reason = gh_mls_invitee_picker_get_n_listed(self->add_picker) == 0
      ? _("Every accepted contact is already in the group.")
      : _("Choose at least one person.");
  else if (gh_mls_invitee_picker_get_checking(self->add_picker))
    reason = _("Checking whether everyone you chose can join…");
  else if (!gh_mls_invitee_picker_get_ready(self->add_picker))
    reason = _("Someone you chose can’t be invited yet. Remove them to continue.");
  else {
    /* Only the group's own format can join it (nostrc-lf62). */
    gboolean adopted = gh_mls_group_get_adopted(self->group);
    g_auto(GStrv) people = gh_mls_invitee_picker_dup_selected(self->add_picker);
    for (guint i = 0; people && people[i] && !reason; i++)
      if (!gh_mls_invitee_can_join(gh_mls_invitee_picker_get_state(self->add_picker, people[i]),
                                   adopted))
        reason = adopted
          ? _("Someone you chose uses an older app version that can’t join this group. "
              "Remove them to continue.")
          : _("Someone you chose uses an app that joins only newer-format groups, and this "
              "group uses the older format. Remove them to continue.");
  }
  gtk_label_set_text(self->add_reason, reason ? reason : "");
  gtk_widget_set_visible(GTK_WIDGET(self->add_reason), reason != NULL);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "mls-group.save-add",
                                reason == NULL && can_manage(self));
}

static void
action_add_members(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  (void)parameter;
  GhMlsGroupInfoDialog *self = GH_MLS_GROUP_INFO_DIALOG(widget);
  if (!can_manage(self))
    return;
  g_auto(GStrv) members = gh_mls_group_dup_members(self->group);
  gh_mls_invitee_picker_setup(self->add_picker, &self->context,
                              (const gchar *const *)members);
  /* Skip the group's relays when checking invitees (nostrc-c0yo). */
  g_auto(GStrv) group_relays = gh_mls_group_dup_read_relays(self->group);
  gh_mls_invitee_picker_set_group_relays(self->add_picker,
                                         (const gchar *const *)group_relays);
  sync_add_reason(self);
  adw_navigation_view_push_by_tag(self->navigation, "add");
}

static void
action_save_add(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  (void)parameter;
  GhMlsGroupInfoDialog *self = GH_MLS_GROUP_INFO_DIALOG(widget);
  if (!can_manage(self) || !gh_mls_invitee_picker_get_ready(self->add_picker))
    return;
  g_auto(GStrv) people = gh_mls_invitee_picker_dup_selected(self->add_picker);
  guint n = g_strv_length(people);
  g_autofree gchar *done = g_strdup_printf(
    g_dngettext(NULL, "Invited %u person. They join once they accept.",
                "Invited %u people. They join once they accept.", n), n);
  gh_mls_service_add_members_async(self->context.service, self->group,
                                   (const gchar *const *)people, NULL, change_done,
                                   change_new(self, done));
  toast(self, _("Sending the invitation…"));
  adw_navigation_view_pop_to_tag(self->navigation, "main");
  sync_status(self);
}

static void
action_remove(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  GhMlsGroupInfoDialog *self = GH_MLS_GROUP_INFO_DIALOG(widget);
  const gchar *pubkey = g_variant_get_string(parameter, NULL);
  if (!self->context.service || !can_manage(self) || g_strcmp0(pubkey, gh_mls_service_get_account(self->context.service)) == 0)
    return;
  g_free(self->removing);
  self->removing = g_strdup(pubkey);
  g_autofree gchar *npub = gh_recipient_npub_short(pubkey);
  const gchar *name = display_name(self, pubkey);
  g_autofree gchar *body = g_strdup_printf(
    _("%s can’t read anything sent to the group after this. What they already have stays "
      "with them."), name && *name ? name : npub);
  adw_alert_dialog_set_body(self->remove_dialog, body);
  adw_dialog_present(ADW_DIALOG(self->remove_dialog), GTK_WIDGET(self));
}

static void
on_remove_response(AdwAlertDialog *dialog, const gchar *response, gpointer data)
{
  (void)dialog;
  GhMlsGroupInfoDialog *self = data;
  g_autofree gchar *pubkey = g_steal_pointer(&self->removing);
  if (!pubkey || g_strcmp0(response, "remove-confirm") != 0 || !can_manage(self))
    return;
  const gchar *members[] = { pubkey, NULL };
  gh_mls_service_remove_members_async(self->context.service, self->group, members,
                                      NULL, change_done,
                                      change_new(self, _("Removed from the group")));
  toast(self, _("Removing…"));
  sync_status(self);
}

/* Verify (W24 review H1): only after the user agreed to ask relays. */
static void
action_verify(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  GhMlsGroupInfoDialog *self = GH_MLS_GROUP_INFO_DIALOG(widget);
  const gchar *pubkey = g_variant_get_string(parameter, NULL);
  if (!self->context.service ||
      gh_mls_group_get_member_identity(self->group, pubkey, NULL) != GH_MLS_MEMBER_UNVERIFIED)
    return;
  g_free(self->verifying);
  self->verifying = g_strdup(pubkey);
  g_autofree gchar *npub = gh_recipient_npub_short(pubkey);
  const gchar *name = display_name(self, pubkey);
  g_autofree gchar *body = gh_mls_verify_prompt(name && *name ? name : npub);
  adw_alert_dialog_set_body(self->verify_dialog, body);
  adw_dialog_present(ADW_DIALOG(self->verify_dialog), GTK_WIDGET(self));
}

typedef struct {
  GhMlsGroupInfoDialog *self;   /* a reference */
  gchar *name;
} Verify;

static void
verify_finished(GObject *source, GAsyncResult *result, gpointer data)
{
  if (gh_test_async_defer_result("group-info-service", "verify_finished", source, result,
                                  verify_finished, data))
    return;
  Verify *verify = data;
  GhMlsGroupInfoDialog *self = verify->self;
  g_autoptr(GError) error = NULL;
  GhMlsMemberIdentity identity =
    gh_mls_service_verify_member_finish(GH_MLS_SERVICE(source), result, &error);
  self->pending--;
  if (!self->disposing && !gtk_widget_in_destruction(GTK_WIDGET(self)) &&
      !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    g_autofree gchar *words = gh_mls_verify_result_copy(identity, error, verify->name);
    toast(self, words);
    sync_all(self);
  }
  g_object_unref(self);
  g_free(verify->name);
  g_free(verify);
}

static void
on_verify_response(AdwAlertDialog *dialog, const gchar *response, gpointer data)
{
  (void)dialog;
  GhMlsGroupInfoDialog *self = data;
  g_autofree gchar *pubkey = g_steal_pointer(&self->verifying);
  if (!pubkey || g_strcmp0(response, "verify-confirm") != 0 || !self->context.service)
    return;
  Verify *verify = g_new0(Verify, 1);
  verify->self = g_object_ref(self);
  g_autofree gchar *npub = gh_recipient_npub_short(pubkey);
  const gchar *name = display_name(self, pubkey);
  verify->name = g_strdup(name && *name ? name : npub);
  self->pending++;
  gh_mls_service_verify_member_async(self->context.service, self->group, pubkey, NULL,
                                     verify_finished, verify);
}

static void
action_rename(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  (void)parameter;
  GhMlsGroupInfoDialog *self = GH_MLS_GROUP_INFO_DIALOG(widget);
  if (!can_manage(self))
    return;
  const gchar *name = gh_mls_group_get_name(self->group);
  const gchar *about = gh_mls_group_get_description(self->group);
  gtk_editable_set_text(GTK_EDITABLE(self->name_row), name ? name : "");
  gtk_editable_set_text(GTK_EDITABLE(self->description_row), about ? about : "");
  adw_navigation_view_push_by_tag(self->navigation, "rename");
}

static void
action_save_rename(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  (void)parameter;
  GhMlsGroupInfoDialog *self = GH_MLS_GROUP_INFO_DIALOG(widget);
  if (!can_manage(self))
    return;
  g_autofree gchar *name = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->name_row))));
  g_autofree gchar *about = g_strstrip(
    g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->description_row))));
  if (!*name) {
    toast(self, _("Give the group a name."));
    gtk_widget_grab_focus(GTK_WIDGET(self->name_row));
    return;
  }
  gh_mls_service_update_metadata_async(self->context.service, self->group, name, about,
                                       NULL, change_done,
                                       change_new(self, _("Name and description saved")));
  toast(self, _("Saving…"));
  adw_navigation_view_pop_to_tag(self->navigation, "main");
  sync_status(self);
}

static void
picture_finished(GhMlsGroupInfoDialog *self, const GError *error, gboolean upload,
                 const gchar *done)
{
  g_clear_object(&self->picture_op);
  self->pending--;
  if (self->disposing || gtk_widget_in_destruction(GTK_WIDGET(self)))
    return;
  if (!error) {
    if (done)
      toast(self, done);
  } else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    /* The error may name a server: debug only (PD-10). */
    g_debug("Group picture: %s", error->message);
    g_autofree gchar *words =
      error->domain == GH_MLS_SERVICE_ERROR && !upload
        ? gh_mls_error_copy(error)
        : gh_mls_attachments_describe(self->files, error,
                                      upload ? GH_ATTACHMENTS_UPLOAD : GH_ATTACHMENTS_DOWNLOAD,
                                      upload ? "group" : NULL, NULL);
    toast(self, words);
  }
  sync_picture(self);
}

static void
on_picture_fetched(GObject *source, GAsyncResult *result, gpointer data)
{
  if (gh_test_async_defer_result("media-attachment", "on_picture_fetched", source, result,
                                  on_picture_fetched, data))
    return;
  GhMlsGroupInfoDialog *self = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) picture =
    gh_mls_attachments_fetch_picture_finish(GH_MLS_ATTACHMENTS(source), result, &error);
  picture_finished(self, picture ? NULL : error, FALSE, NULL);
  /* W25 review N4: said, not only shown. */
  if (picture && !self->disposing && !gtk_widget_in_destruction(GTK_WIDGET(self)) &&
      gtk_widget_get_mapped(GTK_WIDGET(self)))
    gtk_accessible_announce(GTK_ACCESSIBLE(self), _("The group’s picture is shown"),
                            GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
  g_object_unref(self);
}

/* The only fetch of the picture: the user asked. */
static void
action_show_picture(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  GhMlsGroupInfoDialog *self = GH_MLS_GROUP_INFO_DIALOG(widget);
  (void)action;
  (void)parameter;
  if (!self->files || self->picture_op || self->picture_state != GH_MLS_PICTURE_AVAILABLE)
    return;
  self->picture_op = g_cancellable_new();
  self->pending++;
  gh_mls_attachments_fetch_picture_async(self->files, self->group, self->picture_op,
                                         on_picture_fetched, g_object_ref(self));
  sync_picture(self);
}

/* A picture being set: kept to ask again if the servers changed. */
typedef struct {
  GhMlsGroupInfoDialog *self;  /* a reference */
  GBytes *file;
  gchar *mime;
} SetCall;

static void
set_call_free(SetCall *call)
{
  g_object_unref(call->self);
  g_bytes_unref(call->file);
  g_free(call->mime);
  g_free(call);
}

static void
on_picture_set(GObject *source, GAsyncResult *result, gpointer data)
{
  if (gh_test_async_defer_result("media-attachment", "on_picture_set", source, result,
                                  on_picture_set, data))
    return;
  SetCall *call = data;
  GhMlsGroupInfoDialog *self = call->self;
  g_autoptr(GError) error = NULL;
  gboolean ok = gh_mls_attachments_set_picture_finish(GH_MLS_ATTACHMENTS(source), result,
                                                      &error);
  /* W25 re-review R4: the group's servers changed while the admin was
   * asked; nothing was uploaded, so ask again, naming the new ones. */
  gboolean ask_again = !ok && g_error_matches(error, GH_MLS_SERVICE_ERROR,
                                              GH_MLS_SERVICE_ERROR_SERVERS_CHANGED);
  picture_finished(self, ok || ask_again ? NULL : error, TRUE,
                   ok ? _("The group’s picture was changed") : NULL);
  if (ask_again && !self->disposing && !gtk_widget_in_destruction(GTK_WIDGET(self)))
    gh_mls_group_info_dialog_set_picture(self, call->file, call->mime);
  set_call_free(call);
}

static void
on_picture_removed(GObject *source, GAsyncResult *result, gpointer data)
{
  if (gh_test_async_defer_result("media-attachment", "on_picture_removed", source, result,
                                  on_picture_removed, data))
    return;
  GhMlsGroupInfoDialog *self = data;
  g_autoptr(GError) error = NULL;
  gboolean ok = GH_IS_MLS_SERVICE(source)
    ? gh_mls_service_change_finish(GH_MLS_SERVICE(source), result, &error)
    : gh_mls_attachments_set_picture_finish(GH_MLS_ATTACHMENTS(source), result, &error);
  picture_finished(self, ok ? NULL : error, FALSE, _("The group’s picture was removed"));
  g_object_unref(self);
}

static void
start_set_picture(GhMlsGroupInfoDialog *self, GBytes *file, const gchar *mime,
                  const gchar *const *hosts)
{
  if (!self->files || self->picture_op || !can_manage(self))
    return;
  self->picture_op = g_cancellable_new();
  self->pending++;
  SetCall *call = g_new0(SetCall, 1);
  call->self = g_object_ref(self);
  call->file = g_bytes_ref(file);
  call->mime = g_strdup(mime);
  gh_mls_attachments_set_picture_async(self->files, self->group, file, mime, hosts,
                                       self->picture_op, on_picture_set, call);
  sync_picture(self);
}

/* W25 review M1: nothing is uploaded before the admin saw where it goes. */
void
gh_mls_group_info_dialog_set_picture(GhMlsGroupInfoDialog *self, GBytes *file,
                                     const gchar *mime)
{
  g_return_if_fail(GH_IS_MLS_GROUP_INFO_DIALOG(self));
  g_return_if_fail(file != NULL);
  if (!self->files || self->picture_op || !can_manage(self))
    return;
  g_auto(GStrv) hosts = gh_mls_attachments_dup_picture_upload_hosts(self->files, self->group);
  g_autofree gchar *note = gh_mls_picture_upload_note(
    (const gchar *const *)hosts, gh_mls_attachments_get_tor(self->files));
  if (!note) {
    toast(self, _("This group doesn’t name a server for its pictures, so one can’t be set "
                  "here."));
    return;
  }
  g_clear_pointer(&self->offered_picture, g_bytes_unref);
  g_clear_pointer(&self->offered_mime, g_free);
  g_clear_pointer(&self->offered_hosts, g_strfreev);
  self->offered_picture = g_bytes_ref(file);
  self->offered_mime = g_strdup(mime);
  self->offered_hosts = g_steal_pointer(&hosts);
  adw_alert_dialog_set_body(self->set_picture_dialog, note);
  adw_dialog_present(ADW_DIALOG(self->set_picture_dialog), GTK_WIDGET(self));
}

static void
on_set_picture_response(AdwAlertDialog *dialog, const gchar *response, gpointer data)
{
  (void)dialog;
  GhMlsGroupInfoDialog *self = data;
  g_autoptr(GBytes) file = g_steal_pointer(&self->offered_picture);
  g_autofree gchar *mime = g_steal_pointer(&self->offered_mime);
  g_auto(GStrv) hosts = g_steal_pointer(&self->offered_hosts);
  /* Exactly the servers the confirmation named (W25 re-review R4). */
  if (file && hosts && g_strcmp0(response, "set-picture-confirm") == 0)
    start_set_picture(self, file, mime, (const gchar *const *)hosts);
}

AdwAlertDialog *
gh_mls_group_info_dialog_get_set_picture_dialog(GhMlsGroupInfoDialog *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP_INFO_DIALOG(self), NULL);
  return self->set_picture_dialog;
}

AdwAlertDialog *
gh_mls_group_info_dialog_get_remove_picture_dialog(GhMlsGroupInfoDialog *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP_INFO_DIALOG(self), NULL);
  return self->remove_picture_dialog;
}

static void
on_picture_read(GObject *source, GAsyncResult *result, gpointer data)
{
  if (gh_test_async_defer_result("media-attachment", "on_picture_read", source, result,
                                  on_picture_read, data))
    return;
  GhMlsGroupInfoDialog *self = data;
  (void)source;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *type = NULL;
  g_autoptr(GBytes) bytes = gh_mls_media_read_file_finish(result, NULL, &type, &error);
  self->pending--;
  if (!self->disposing && !gtk_widget_in_destruction(GTK_WIDGET(self))) {
    if (bytes) {
      gh_mls_group_info_dialog_set_picture(self, bytes, type);
    } else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_autofree gchar *words =
        gh_mls_attachments_describe(self->files, error, GH_ATTACHMENTS_UPLOAD, NULL, NULL);
      toast(self, words);
    }
  }
  g_object_unref(self);
}

static void
on_picture_chosen(GObject *source, GAsyncResult *result, gpointer data)
{
  if (gh_test_async_defer_result("media-attachment", "on_picture_chosen", source, result,
                                  on_picture_chosen, data))
    return;
  GhMlsGroupInfoDialog *self = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
  if (file && self->files && !self->disposing &&
      !gtk_widget_in_destruction(GTK_WIDGET(self))) {
    /* A file on this device only, refused over the limit before it is read
     * (gh-mls-media.h step 1). */
    self->pending++;
    gh_mls_media_read_file_async(file, GH_MLS_MEDIA_PICTURE_MAX, NULL, on_picture_read,
                                 g_object_ref(self));
  }
  g_object_unref(self);
}

static void
action_set_picture(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  GhMlsGroupInfoDialog *self = GH_MLS_GROUP_INFO_DIALOG(widget);
  (void)action;
  (void)parameter;
  if (!self->files || self->picture_op || !can_manage(self))
    return;
  g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, _("Choose a Group Picture"));
  gtk_file_dialog_set_accept_label(dialog, _("_Choose"));
  gtk_file_dialog_set_modal(dialog, TRUE);
  g_autoptr(GtkFileFilter) filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, _("Photos (JPEG, PNG)"));
  gtk_file_filter_add_mime_type(filter, "image/jpeg");
  gtk_file_filter_add_mime_type(filter, "image/png");
  g_autoptr(GListStore) filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
  g_list_store_append(filters, filter);
  gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
  GtkRoot *root = gtk_widget_get_root(widget);
  gtk_file_dialog_open(dialog, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL, NULL,
                       on_picture_chosen, g_object_ref(self));
}

/* W25 review L5: for every member, so confirmed first. */
static void
action_remove_picture(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  GhMlsGroupInfoDialog *self = GH_MLS_GROUP_INFO_DIALOG(widget);
  (void)action;
  (void)parameter;
  if (!self->files || self->picture_op || !can_manage(self))
    return;
  adw_dialog_present(ADW_DIALOG(self->remove_picture_dialog), widget);
}

static void
on_remove_picture_response(AdwAlertDialog *dialog, const gchar *response, gpointer data)
{
  (void)dialog;
  GhMlsGroupInfoDialog *self = data;
  if (g_strcmp0(response, "remove-picture-confirm") != 0 || !self->files ||
      self->picture_op || !can_manage(self))
    return;
  self->picture_op = g_cancellable_new();
  self->pending++;
  /* A web address wins over an encrypted picture: removing what shows. */
  if (self->picture_state == GH_MLS_PICTURE_WEB ||
      self->picture_state == GH_MLS_PICTURE_WEB_UNVERIFIED)
    gh_mls_service_clear_avatar_url_async(self->context.service, self->group, self->picture_op,
                                          on_picture_removed, g_object_ref(self));
  else
    gh_mls_attachments_set_picture_async(self->files, self->group, NULL, NULL, NULL,
                                         self->picture_op, on_picture_removed,
                                         g_object_ref(self));
  sync_picture(self);
}

static void
action_leave(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  (void)parameter;
  GhMlsGroupInfoDialog *self = GH_MLS_GROUP_INFO_DIALOG(widget);
  if (!gh_mls_group_get_active(self->group) || !self->context.service)
    return;
  /* Say what Leave will do: for everyone, or on this device only and why
   * (nostrc-2um6). */
  self->leave_kind = gh_mls_service_leave_kind(self->context.service, self->group);
  adw_alert_dialog_set_body(self->leave_dialog, gh_mls_leave_copy(self->leave_kind));
  adw_dialog_present(ADW_DIALOG(self->leave_dialog), GTK_WIDGET(self));
}

static void
on_leave_response(AdwAlertDialog *dialog, const gchar *response, gpointer data)
{
  (void)dialog;
  GhMlsGroupInfoDialog *self = data;
  if (g_strcmp0(response, "leave-confirm") != 0 || !self->context.service ||
      !gh_mls_group_get_active(self->group))
    return;
  g_autoptr(GError) error = NULL;
  if (!gh_mls_service_leave(self->context.service, self->group, &error)) {
    g_message("Groundhog could not leave an encrypted group: %s", error->message);
    g_autofree gchar *words = gh_mls_error_copy(error);
    toast(self, words);
    return;
  }
  toast(self, self->leave_kind == GH_MLS_LEAVE_EVERYONE || self->leave_kind == GH_MLS_LEAVE_ADMINS
                ? _("Leaving the group…")
                : _("You left the group on this device"));
  sync_all(self);
}

/* nostrc-2um6: a member who asked to leave is out. */
static void
on_member_left(GhMlsGroup *group, const gchar *pubkey, gpointer data)
{
  (void)group;
  GhMlsGroupInfoDialog *self = data;
  const gchar *name = display_name(self, pubkey);
  g_autofree gchar *npub = gh_recipient_npub_short(pubkey);
  g_autofree gchar *words = gh_mls_member_left_copy(name && *name ? name : npub);
  toast(self, words);
}

static void
on_rename_activated(GhMlsGroupInfoDialog *self)
{
  gtk_widget_activate_action(GTK_WIDGET(self), "mls-group.save-rename", NULL);
}

/* ---- lifetime ----------------------------------------------------------------------------- */

static void
on_service_gone(gpointer data, GObject *where)
{
  GhMlsGroupInfoDialog *self = data;
  (void)where;
  self->context.service = NULL;
  adw_dialog_force_close(ADW_DIALOG(self));
}

GhMlsGroupInfoDialog *
gh_mls_group_info_dialog_new(GhMlsGroup *group, const GhMlsUiContext *context)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP(group), NULL);
  g_return_val_if_fail(context != NULL && GH_IS_MLS_SERVICE(context->service), NULL);
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(context->accounts), NULL);
  g_return_val_if_fail(GH_IS_CONVERSATION_STORE(context->model), NULL);
  GhMlsGroupInfoDialog *self = g_object_new(GH_TYPE_MLS_GROUP_INFO_DIALOG, NULL);
  self->group = g_object_ref(group);
  self->context = *context;
  self->context.default_relays = NULL;
  self->files = context->files ? g_object_ref(context->files) : NULL;
  self->context.files = NULL;
  /* The service is watched, not held: an account switch disposes it. */
  g_object_weak_ref(G_OBJECT(context->service), on_service_gone, self);
  g_object_ref(context->accounts);
  g_object_ref(context->model);
  if (context->settings)
    g_object_ref(context->settings);
  g_signal_connect_object(group, "notify", G_CALLBACK(sync_status), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(group, "notify::name", G_CALLBACK(sync_header), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(group, "notify::description", G_CALLBACK(sync_header), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(group, "notify::is-admin", G_CALLBACK(sync_members), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(group, "notify::active", G_CALLBACK(sync_members), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(group, "members-changed", G_CALLBACK(sync_all), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(group, "member-left", G_CALLBACK(on_member_left), self, 0);
  /* Every Commit may change the picture's components. */
  g_signal_connect_object(group, "notify::epoch", G_CALLBACK(sync_picture), self,
                          G_CONNECT_SWAPPED);
  fill_privacy(self);
  sync_all(self);
  return self;
}

GhMlsGroup *
gh_mls_group_info_dialog_get_group(GhMlsGroupInfoDialog *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP_INFO_DIALOG(self), NULL);
  return self->group;
}

guint
gh_mls_group_info_dialog_get_pending(GhMlsGroupInfoDialog *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP_INFO_DIALOG(self), 0);
  return self->pending;
}

const gchar *
gh_mls_group_info_dialog_get_last_toast(GhMlsGroupInfoDialog *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP_INFO_DIALOG(self), NULL);
  return self->last_toast;
}

GhMlsInviteePicker *
gh_mls_group_info_dialog_get_add_picker(GhMlsGroupInfoDialog *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP_INFO_DIALOG(self), NULL);
  return self->add_picker;
}

const gchar *
gh_mls_group_info_dialog_get_member(GhMlsGroupInfoDialog *self, const gchar *pubkey,
                                    gboolean *removable)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP_INFO_DIALOG(self), NULL);
  for (guint i = 0; i < self->member_rows->len; i++) {
    GhMlsMemberRow *row = g_ptr_array_index(self->member_rows, i);
    if (g_strcmp0(row->pubkey, pubkey) != 0)
      continue;
    if (removable)
      *removable = gtk_widget_get_visible(row->remove_button);
    return gtk_label_get_text(row->role_badge);
  }
  return NULL;
}

const gchar *
gh_mls_group_info_dialog_get_member_identity(GhMlsGroupInfoDialog *self, const gchar *pubkey,
                                             const gchar **out_explanation)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP_INFO_DIALOG(self), NULL);
  if (out_explanation)
    *out_explanation = NULL;
  for (guint i = 0; i < self->member_rows->len; i++) {
    GhMlsMemberRow *row = g_ptr_array_index(self->member_rows, i);
    if (g_strcmp0(row->pubkey, pubkey) != 0)
      continue;
    if (!gtk_widget_get_visible(GTK_WIDGET(row->identity_badge)))
      return "";
    if (out_explanation)
      *out_explanation = gtk_widget_get_tooltip_text(GTK_WIDGET(row->identity_badge));
    return gtk_label_get_text(row->identity_badge);
  }
  return NULL;
}

gboolean
gh_mls_group_info_dialog_get_member_verifiable(GhMlsGroupInfoDialog *self, const gchar *pubkey)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP_INFO_DIALOG(self), FALSE);
  for (guint i = 0; i < self->member_rows->len; i++) {
    GhMlsMemberRow *row = g_ptr_array_index(self->member_rows, i);
    if (g_strcmp0(row->pubkey, pubkey) == 0)
      return gtk_widget_get_visible(row->verify_button);
  }
  return FALSE;
}

AdwAlertDialog *
gh_mls_group_info_dialog_get_verify_dialog(GhMlsGroupInfoDialog *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP_INFO_DIALOG(self), NULL);
  return self->verify_dialog;
}

const gchar *
gh_mls_group_info_dialog_get_messages_status(GhMlsGroupInfoDialog *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP_INFO_DIALOG(self), NULL);
  return adw_action_row_get_subtitle(self->messages_row);
}

AdwAlertDialog *
gh_mls_group_info_dialog_get_leave_dialog(GhMlsGroupInfoDialog *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP_INFO_DIALOG(self), NULL);
  return self->leave_dialog;
}

AdwAlertDialog *
gh_mls_group_info_dialog_get_remove_dialog(GhMlsGroupInfoDialog *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP_INFO_DIALOG(self), NULL);
  return self->remove_dialog;
}

const gchar *
gh_mls_group_info_dialog_get_picture_status(GhMlsGroupInfoDialog *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP_INFO_DIALOG(self), NULL);
  return gtk_widget_get_visible(GTK_WIDGET(self->picture_group))
           ? adw_action_row_get_subtitle(self->picture_row) : NULL;
}

gboolean
gh_mls_group_info_dialog_get_picture_shown(GhMlsGroupInfoDialog *self)
{
  g_return_val_if_fail(GH_IS_MLS_GROUP_INFO_DIALOG(self), FALSE);
  return self->picture_shown;
}

void
gh_mls_group_info_dialog_set_rename(GhMlsGroupInfoDialog *self, const gchar *name,
                                    const gchar *description)
{
  g_return_if_fail(GH_IS_MLS_GROUP_INFO_DIALOG(self));
  gtk_editable_set_text(GTK_EDITABLE(self->name_row), name ? name : "");
  gtk_editable_set_text(GTK_EDITABLE(self->description_row), description ? description : "");
}

static void
gh_mls_group_info_dialog_dispose(GObject *object)
{
  GhMlsGroupInfoDialog *self = GH_MLS_GROUP_INFO_DIALOG(object);
  self->disposing = TRUE;
  if (self->media_op)
    g_cancellable_cancel(self->media_op);
  g_clear_object(&self->media_op);
  if (self->context.service) {
    g_object_weak_unref(G_OBJECT(self->context.service), on_service_gone, self);
    self->context.service = NULL;
  }
  g_clear_object(&self->context.accounts);
  g_clear_object(&self->context.model);
  g_clear_object(&self->context.settings);
  if (self->picture_op)
    g_cancellable_cancel(self->picture_op);
  g_clear_object(&self->picture_op);
  g_clear_object(&self->files);
  g_clear_object(&self->group);
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_MLS_GROUP_INFO_DIALOG);
  G_OBJECT_CLASS(gh_mls_group_info_dialog_parent_class)->dispose(object);
}

static void
gh_mls_group_info_dialog_finalize(GObject *object)
{
  GhMlsGroupInfoDialog *self = GH_MLS_GROUP_INFO_DIALOG(object);
  g_ptr_array_unref(self->member_rows);
  g_ptr_array_unref(self->relay_rows);
  g_free(self->removing);
  g_free(self->verifying);
  g_free(self->last_toast);
  g_clear_pointer(&self->offered_picture, g_bytes_unref);
  g_free(self->offered_mime);
  g_strfreev(self->offered_hosts);
  G_OBJECT_CLASS(gh_mls_group_info_dialog_parent_class)->finalize(object);
}

static void
gh_mls_group_info_dialog_class_init(GhMlsGroupInfoDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  object_class->dispose = gh_mls_group_info_dialog_dispose;
  object_class->finalize = gh_mls_group_info_dialog_finalize;
  g_type_ensure(GH_TYPE_MLS_INVITEE_PICKER);
  gtk_widget_class_set_template_from_resource(
    widget_class, "/org/nostr/Groundhog/ui/gh-mls-group-info-dialog.ui");
#define BIND(name) gtk_widget_class_bind_template_child(widget_class, GhMlsGroupInfoDialog, name)
  BIND(toasts);
  BIND(navigation);
  BIND(avatar);
  BIND(title_label);
  BIND(subtitle_label);
  BIND(about_label);
  BIND(privacy_row);
  BIND(visible_row);
  BIND(unprotected_row);
  BIND(messages_row);
  BIND(pending_row);
  BIND(pending_spinner);
  BIND(members_group);
  BIND(add_member_button);
  BIND(admin_group);
  BIND(media_group);
  BIND(media_policy_row);
  BIND(update_media_button);
  BIND(relays_group);
  BIND(leave_button);
  BIND(add_picker);
  BIND(add_reason);
  BIND(name_row);
  BIND(description_row);
  BIND(leave_dialog);
  BIND(remove_dialog);
  BIND(verify_dialog);
  BIND(picture_group);
  BIND(picture_row);
  BIND(picture_spinner);
  BIND(show_picture_button);
  BIND(set_picture_button);
  BIND(remove_picture_button);
  BIND(set_picture_dialog);
  BIND(remove_picture_dialog);
#undef BIND
  gtk_widget_class_install_action(widget_class, "mls-group.add-members", NULL,
                                  action_add_members);
  gtk_widget_class_install_action(widget_class, "mls-group.save-add", NULL, action_save_add);
  gtk_widget_class_install_action(widget_class, "mls-group.remove", "s", action_remove);
  gtk_widget_class_install_action(widget_class, "mls-group.verify", "s", action_verify);
  gtk_widget_class_install_action(widget_class, "mls-group.rename", NULL, action_rename);
  gtk_widget_class_install_action(widget_class, "mls-group.save-rename", NULL,
                                  action_save_rename);
  gtk_widget_class_install_action(widget_class, "mls-group.leave", NULL, action_leave);
  gtk_widget_class_install_action(widget_class, "mls-group.show-picture", NULL,
                                  action_show_picture);
  gtk_widget_class_install_action(widget_class, "mls-group.set-picture", NULL,
                                  action_set_picture);
  gtk_widget_class_install_action(widget_class, "mls-group.remove-picture", NULL,
                                  action_remove_picture);
  gtk_widget_class_install_action(widget_class, "mls-group.update-media", NULL,
                                  action_update_media);
}

static void
gh_mls_group_info_dialog_init(GhMlsGroupInfoDialog *self)
{
  self->member_rows = g_ptr_array_new();
  self->relay_rows = g_ptr_array_new();
  self->picture_state = -1;
  gtk_widget_init_template(GTK_WIDGET(self));
  g_signal_connect(self->leave_dialog, "response", G_CALLBACK(on_leave_response), self);
  g_signal_connect(self->remove_dialog, "response", G_CALLBACK(on_remove_response), self);
  g_signal_connect(self->set_picture_dialog, "response", G_CALLBACK(on_set_picture_response),
                   self);
  g_signal_connect(self->remove_picture_dialog, "response",
                   G_CALLBACK(on_remove_picture_response), self);
  g_signal_connect(self->verify_dialog, "response", G_CALLBACK(on_verify_response), self);
  g_signal_connect_swapped(self->add_picker, "changed", G_CALLBACK(sync_add_reason), self);
  g_signal_connect_swapped(self->name_row, "entry-activated", G_CALLBACK(on_rename_activated),
                           self);
  g_signal_connect_swapped(self->description_row, "entry-activated",
                           G_CALLBACK(on_rename_activated), self);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "mls-group.save-add", FALSE);
}
