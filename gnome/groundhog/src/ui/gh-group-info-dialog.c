#include "gh-group-info-dialog.h"

#include "gh-group-copy.h"
#include "gh-privacy-summary.h"
#include "gh-recipient.h"

#include <glib/gi18n.h>

/* ---- GhGroupMemberRow (data/ui/gh-group-member-row.blp) ---------------------------- */

#define GH_TYPE_GROUP_MEMBER_ROW (gh_group_member_row_get_type())
G_DECLARE_FINAL_TYPE(GhGroupMemberRow, gh_group_member_row, GH, GROUP_MEMBER_ROW, AdwActionRow)

struct _GhGroupMemberRow {
  AdwActionRow parent_instance;
  AdwAvatar *avatar;
  GtkBox *roles_box;
  GtkMenuButton *menu_button;
  gchar *pubkey;
};

G_DEFINE_FINAL_TYPE(GhGroupMemberRow, gh_group_member_row, ADW_TYPE_ACTION_ROW)

static void
member_change_role(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  (void)parameter;
  GhGroupMemberRow *self = GH_GROUP_MEMBER_ROW(widget);
  gtk_widget_activate_action(widget, "group.change-role", "s", self->pubkey);
}

static void
member_remove(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  (void)parameter;
  GhGroupMemberRow *self = GH_GROUP_MEMBER_ROW(widget);
  gtk_widget_activate_action(widget, "group.remove", "s", self->pubkey);
}

static void
gh_group_member_row_finalize(GObject *object)
{
  g_free(GH_GROUP_MEMBER_ROW(object)->pubkey);
  G_OBJECT_CLASS(gh_group_member_row_parent_class)->finalize(object);
}

static void
gh_group_member_row_class_init(GhGroupMemberRowClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  G_OBJECT_CLASS(klass)->finalize = gh_group_member_row_finalize;
  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-group-member-row.ui");
  gtk_widget_class_bind_template_child(widget_class, GhGroupMemberRow, avatar);
  gtk_widget_class_bind_template_child(widget_class, GhGroupMemberRow, roles_box);
  gtk_widget_class_bind_template_child(widget_class, GhGroupMemberRow, menu_button);
  gtk_widget_class_install_action(widget_class, "member.change-role", NULL, member_change_role);
  gtk_widget_class_install_action(widget_class, "member.remove", NULL, member_remove);
}

static void
gh_group_member_row_init(GhGroupMemberRow *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
}

/* name: what Groundhog shows for them; roles: the relay's (may be empty). */
static GhGroupMemberRow *
member_row_new(const gchar *pubkey, const gchar *name, gboolean is_you,
               const gchar *const *roles, gboolean can_change_role, gboolean can_remove)
{
  GhGroupMemberRow *self = g_object_new(GH_TYPE_GROUP_MEMBER_ROW, NULL);
  self->pubkey = g_strdup(pubkey);
  g_autofree gchar *npub = gh_recipient_npub_short(pubkey);
  /* TRANSLATORS: the account's own row in a group's member list. */
  g_autofree gchar *title = is_you ? g_strdup_printf(_("%s (You)"), name ? name : npub)
                                   : g_strdup(name ? name : npub);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self), title);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(self), name ? npub : "");
  adw_avatar_set_text(self->avatar, name ? name : npub);
  g_autoptr(GPtrArray) spoken = g_ptr_array_new();
  g_ptr_array_add(spoken, title);
  for (guint i = 0; roles && roles[i]; i++) {
    if (!*roles[i] || !g_utf8_validate(roles[i], -1, NULL))
      continue;
    GtkWidget *badge = gtk_label_new(roles[i]);
    gtk_label_set_use_markup(GTK_LABEL(badge), FALSE);
    gtk_widget_add_css_class(badge, "caption");
    gtk_widget_add_css_class(badge, "groundhog-role-badge");
    gtk_box_append(self->roles_box, badge);
    g_ptr_array_add(spoken, (gpointer)roles[i]);
  }
  g_ptr_array_add(spoken, NULL);
  g_autofree gchar *label = g_strjoinv(", ", (gchar **)spoken->pdata);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "member.change-role", can_change_role);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "member.remove", can_remove);
  gtk_widget_set_visible(GTK_WIDGET(self->menu_button), can_change_role || can_remove);
  return self;
}

/* ---- GhGroupInfoDialog --------------------------------------------------------------- */

/* An operation this dialog sent: what to say when the relay answers. */
typedef enum {
  INTENT_ADD,
  INTENT_ROLE,
  INTENT_REMOVE,
  INTENT_EDIT,
  INTENT_INVITE
} Intent;

typedef struct {
  Intent intent;
  gchar *code; /* INTENT_INVITE */
} Pending;

static void
pending_free(gpointer data)
{
  Pending *pending = data;
  g_free(pending->code);
  g_free(pending);
}

struct _GhGroupInfoDialog {
  AdwDialog parent_instance;
  AdwToastOverlay *toasts;
  AdwNavigationView *navigation;
  AdwAvatar *avatar;
  GtkLabel *title_label;
  GtkLabel *host_label;
  GtkLabel *about_label;
  AdwActionRow *privacy_row;
  AdwExpanderRow *visible_row;
  AdwExpanderRow *unprotected_row;
  AdwActionRow *membership_row;
  GtkImage *membership_icon;
  AdwActionRow *messages_row;
  AdwActionRow *details_row;
  AdwActionRow *settings_row;
  AdwActionRow *address_row;
  AdwPreferencesGroup *members_group;
  GtkWidget *add_member_button;
  AdwPreferencesGroup *admin_group;
  AdwActionRow *edit_row;
  AdwActionRow *invite_row;
  AdwActionRow *invite_code_row;
  AdwActionRow *join_row;
  AdwActionRow *leave_row;
  AdwActionRow *forget_row;
  AdwPreferencesGroup *roles_group;
  GtkWidget *role_save_button;
  AdwEntryRow *add_entry;
  GtkLabel *add_error_label;
  AdwEntryRow *name_entry;
  AdwEntryRow *about_entry;
  AdwSwitchRow *private_switch;
  AdwSwitchRow *closed_switch;
  AdwAlertDialog *leave_dialog;
  AdwAlertDialog *remove_dialog;
  AdwAlertDialog *forget_dialog;

  GhNip29Service *service;
  GhNip29Room *room;
  GhGroupNameFunc display_name;
  gpointer names_data;
  GPtrArray *member_rows;  /* GtkWidget, in members_group */
  GPtrArray *role_rows;    /* AdwActionRow, in roles_group */
  GPtrArray *role_checks;  /* GtkCheckButton, parallel to role_rows */
  GPtrArray *role_names;   /* gchar *, parallel to role_rows */
  gchar *role_target;
  gchar *remove_target;
  gchar *invite_code;      /* the newest code the relay accepted */
  GHashTable *pending;     /* GhNip29Op (ref) -> Pending */
  gboolean closing;
};

G_DEFINE_FINAL_TYPE(GhGroupInfoDialog, gh_group_info_dialog, ADW_TYPE_DIALOG)

static void
toast(GhGroupInfoDialog *self, const gchar *text)
{
  AdwToast *toast = adw_toast_new(text);
  adw_toast_set_use_markup(toast, FALSE); /* relay text */
  adw_toast_overlay_add_toast(self->toasts, toast);
}

static const gchar *
display_name(GhGroupInfoDialog *self, const gchar *pubkey)
{
  const gchar *name = self->display_name ? self->display_name(pubkey, self->names_data) : NULL;
  return name && *name && g_utf8_validate(name, -1, NULL) ? name : NULL;
}

static gchar *
relay_host(GhGroupInfoDialog *self)
{
  return gh_group_relay_host(gh_nip29_room_get_relay_url(self->room));
}

static gchar *
address(GhGroupInfoDialog *self, const gchar *code)
{
  return gh_group_format_address(gh_nip29_room_get_relay_url(self->room),
                                 gh_nip29_room_get_group_id(self->room), code);
}

/* ---- sync ------------------------------------------------------------------------------ */

static void
sync_header(GhGroupInfoDialog *self)
{
  const gchar *name = gh_nip29_room_get_name(self->room);
  const gchar *title = name && *name ? name : gh_nip29_room_get_group_id(self->room);
  g_autofree gchar *host = relay_host(self);
  gtk_label_set_text(self->title_label, title);
  adw_avatar_set_text(self->avatar, title);
  /* TRANSLATORS: where a relay group lives; %s is the relay's host name. */
  g_autofree gchar *where = g_strdup_printf(_("Relay group on %s"), host);
  gtk_label_set_text(self->host_label, where);
  const GhNip29Group *group = gh_nip29_room_get_group(self->room);
  g_autoptr(GhNip29Metadata) metadata = group ? gh_nip29_group_dup_metadata(group) : NULL;
  const gchar *about = metadata && metadata->about && *metadata->about &&
                       g_utf8_validate(metadata->about, -1, NULL) ? metadata->about : NULL;
  gtk_label_set_text(self->about_label, about ? about : "");
  gtk_widget_set_visible(GTK_WIDGET(self->about_label), about != NULL);
  if (metadata) {
    const gchar *join = metadata->is_closed ? _("Invite only") : _("Anyone can ask to join");
    const gchar *read = metadata->is_private ? _("only members can read")
                                             : _("anyone can read");
    /* TRANSLATORS: who can join, then who can read, e.g. "Invite only · only
     * members can read". */
    g_autofree gchar *settings = g_strdup_printf(_("%s · %s"), join, read);
    adw_action_row_set_subtitle(self->settings_row, settings);
  }
  gtk_widget_set_visible(GTK_WIDGET(self->settings_row), metadata != NULL);
  g_autofree gchar *shared = address(self, NULL);
  adw_action_row_set_subtitle(self->address_row, shared);
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
sync_privacy(GhGroupInfoDialog *self)
{
  g_autofree gchar *host = relay_host(self);
  GhPrivacyContext context = { .backend = GH_PRIVACY_BACKEND_NIP29, .relay_host = host };
  g_autoptr(GhPrivacySummary) summary = gh_privacy_summary_new(&context);
  if (!summary)
    return;
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->privacy_row), summary->heading);
  adw_action_row_set_subtitle(self->privacy_row, summary->encrypted);
  add_privacy_rows(self->visible_row, (const gchar *const *)summary->visible);
  add_privacy_rows(self->unprotected_row, (const gchar *const *)summary->unprotected);
}

static void
sync_status(GhGroupInfoDialog *self)
{
  GhNip29JoinState join = gh_nip29_room_get_join_state(self->room);
  /* "Already a member" answers a join request; here a member is a member. */
  g_autoptr(GhNip29Op) request = join == GH_NIP29_JOIN_MEMBER
                                   ? NULL : gh_nip29_room_dup_request_op(self->room);
  g_autofree gchar *host = relay_host(self);
  g_autoptr(GhGroupStateCopy) copy = gh_group_join_copy(join, request, FALSE,
                                                        gh_nip29_room_get_detail(self->room),
                                                        host);
  /* TRANSLATORS: a state's title, then its explanation. */
  g_autofree gchar *membership = g_strdup_printf(_("%s. %s"), copy->title, copy->description);
  adw_action_row_set_subtitle(self->membership_row, membership);
  gtk_image_set_from_icon_name(self->membership_icon, copy->icon_name);

  GhNip29ReadState read = gh_nip29_room_get_read_state(self->room);
  const gchar *reading = gh_group_read_copy(read);
  g_autofree gchar *reason = read == GH_NIP29_READ_REFUSED || read == GH_NIP29_READ_AUTH_REQUIRED
    ? gh_group_relay_reason(gh_nip29_room_get_detail(self->room)) : NULL;
  g_autofree gchar *messages = reason ? g_strdup_printf(_("%s. The relay said: “%s”"), reading,
                                                        reason)
                                      : g_strdup(reading);
  adw_action_row_set_subtitle(self->messages_row, messages);
  adw_action_row_set_subtitle(self->details_row,
                              gh_group_key_copy(gh_nip29_room_get_relay_key_state(self->room)));
}

static void
clear_member_rows(GhGroupInfoDialog *self)
{
  for (guint i = 0; i < self->member_rows->len; i++)
    adw_preferences_group_remove(self->members_group, g_ptr_array_index(self->member_rows, i));
  g_ptr_array_set_size(self->member_rows, 0);
}

static void
sync_members(GhGroupInfoDialog *self)
{
  clear_member_rows(self);
  const GhNip29Group *group = gh_nip29_room_get_group(self->room);
  const gchar *account = gh_nip29_service_get_account(self->service);
  GhGroupGate role_gate = gh_group_room_gate(self->room, NOSTR_PERMISSION_PUT_USER);
  GhGroupGate remove_gate = gh_group_room_gate(self->room, NOSTR_PERMISSION_REMOVE_USER);
  g_auto(GStrv) members = NULL;
  GhNip29MemberList state = group ? gh_nip29_group_dup_members(group, &members)
                                  : GH_NIP29_MEMBERS_UNAVAILABLE;
  adw_preferences_group_set_description(self->members_group, gh_group_members_copy(state));
  g_autoptr(GPtrArray) admins = group ? gh_nip29_group_dup_admins(group) : NULL;
  g_autoptr(GHashTable) shown = g_hash_table_new(g_str_hash, g_str_equal);
  /* Admins first, with the roles the relay gives them. */
  for (guint i = 0; admins && i < admins->len; i++) {
    const GhNip29Admin *admin = g_ptr_array_index(admins, i);
    if (g_hash_table_contains(shown, admin->pubkey))
      continue;
    g_hash_table_add(shown, admin->pubkey);
    gboolean you = g_strcmp0(admin->pubkey, account) == 0;
    GhGroupMemberRow *row = member_row_new(admin->pubkey, display_name(self, admin->pubkey), you,
                                           (const gchar *const *)admin->roles,
                                           !you && role_gate != GH_GROUP_GATE_HIDDEN,
                                           !you && remove_gate != GH_GROUP_GATE_HIDDEN);
    adw_preferences_group_add(self->members_group, GTK_WIDGET(row));
    g_ptr_array_add(self->member_rows, row);
  }
  for (guint i = 0; members && members[i]; i++) {
    if (g_hash_table_contains(shown, members[i]))
      continue;
    g_hash_table_add(shown, members[i]);
    gboolean you = g_strcmp0(members[i], account) == 0;
    GhGroupMemberRow *row = member_row_new(members[i], display_name(self, members[i]), you, NULL,
                                           !you && role_gate != GH_GROUP_GATE_HIDDEN,
                                           !you && remove_gate != GH_GROUP_GATE_HIDDEN);
    adw_preferences_group_add(self->members_group, GTK_WIDGET(row));
    g_ptr_array_add(self->member_rows, row);
  }
  gtk_widget_set_visible(self->add_member_button, role_gate != GH_GROUP_GATE_HIDDEN);
}

static void
sync_admin(GhGroupInfoDialog *self)
{
  GhGroupGate edit = gh_group_room_gate(self->room, NOSTR_PERMISSION_EDIT_METADATA);
  GhGroupGate invite = gh_group_room_gate(self->room, NOSTR_PERMISSION_CREATE_INVITE);
  GhGroupGate put = gh_group_room_gate(self->room, NOSTR_PERMISSION_PUT_USER);
  GhGroupGate remove = gh_group_room_gate(self->room, NOSTR_PERMISSION_REMOVE_USER);
  gtk_widget_set_visible(GTK_WIDGET(self->edit_row), edit != GH_GROUP_GATE_HIDDEN);
  gtk_widget_set_visible(GTK_WIDGET(self->invite_row), invite != GH_GROUP_GATE_HIDDEN);
  gtk_widget_set_visible(GTK_WIDGET(self->invite_code_row),
                         invite != GH_GROUP_GATE_HIDDEN && self->invite_code != NULL);
  gtk_widget_set_visible(GTK_WIDGET(self->admin_group),
                         edit != GH_GROUP_GATE_HIDDEN || invite != GH_GROUP_GATE_HIDDEN);
  gboolean relay_decides = edit == GH_GROUP_GATE_RELAY_DECIDES ||
                           invite == GH_GROUP_GATE_RELAY_DECIDES ||
                           put == GH_GROUP_GATE_RELAY_DECIDES ||
                           remove == GH_GROUP_GATE_RELAY_DECIDES;
  /* Charter §7.10: rights the relay did not publish are attempted; it says. */
  adw_preferences_group_set_description(self->admin_group,
                                        relay_decides ? _("The relay decides whether you’re "
                                                          "allowed.")
                                                      : NULL);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "group.edit", edit != GH_GROUP_GATE_HIDDEN);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "group.save-edit",
                                edit != GH_GROUP_GATE_HIDDEN);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "group.create-invite",
                                invite != GH_GROUP_GATE_HIDDEN);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "group.add-member",
                                put != GH_GROUP_GATE_HIDDEN);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "group.save-member",
                                put != GH_GROUP_GATE_HIDDEN);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "group.change-role",
                                put != GH_GROUP_GATE_HIDDEN);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "group.save-role",
                                put != GH_GROUP_GATE_HIDDEN);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "group.remove",
                                remove != GH_GROUP_GATE_HIDDEN);
}

static void
sync_actions(GhGroupInfoDialog *self)
{
  GhNip29JoinState join = gh_nip29_room_get_join_state(self->room);
  gboolean in = join == GH_NIP29_JOIN_MEMBER || join == GH_NIP29_JOIN_PENDING ||
                join == GH_NIP29_JOIN_REQUESTING;
  gboolean out = join == GH_NIP29_JOIN_NONE || join == GH_NIP29_JOIN_DENIED ||
                 join == GH_NIP29_JOIN_CLOSED || join == GH_NIP29_JOIN_NOT_SENT ||
                 join == GH_NIP29_JOIN_LEFT || join == GH_NIP29_JOIN_REMOVED;
  gtk_widget_set_visible(GTK_WIDGET(self->leave_row), in);
  gtk_widget_set_visible(GTK_WIDGET(self->join_row), out);
  gtk_widget_set_visible(GTK_WIDGET(self->forget_row), out);
  adw_action_row_set_subtitle(self->join_row,
                              join == GH_NIP29_JOIN_CLOSED || join == GH_NIP29_JOIN_DENIED
                                ? _("An invite code may be needed: use Join Group in the main "
                                    "menu to enter one.")
                                : _("Sends a new join request to the relay."));
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "group.leave", in);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "group.join", out);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "group.forget", out);
}

static void
sync_all(GhGroupInfoDialog *self)
{
  if (self->closing)
    return;
  sync_header(self);
  sync_status(self);
  sync_members(self);
  sync_admin(self);
  sync_actions(self);
}

/* ---- the relay's answers --------------------------------------------------------------- */

static const gchar *
accepted_text(Intent intent)
{
  switch (intent) {
  case INTENT_ADD:
    return _("The relay added them to the group");
  case INTENT_ROLE:
    return _("The relay changed their role");
  case INTENT_REMOVE:
    return _("The relay removed them from the group");
  case INTENT_EDIT:
    return _("The relay saved the group’s name and settings");
  case INTENT_INVITE:
  default:
    return _("The relay created the invite code");
  }
}

static void
on_op_changed(GhNip29Outbox *outbox, GhNip29Op *op, gpointer data)
{
  GhGroupInfoDialog *self = data;
  (void)outbox;
  Pending *pending = g_hash_table_lookup(self->pending, op);
  GhNip29OpResult result = gh_nip29_op_get_result(op);
  if (!pending || !gh_nip29_op_result_is_final(result))
    return;
  g_autofree gchar *reason = gh_group_relay_reason(gh_nip29_op_get_relay_message(op));
  g_autofree gchar *text = NULL;
  switch (result) {
  case GH_NIP29_OP_ACCEPTED:
  case GH_NIP29_OP_DUPLICATE:
    text = g_strdup(accepted_text(pending->intent));
    if (pending->intent == INTENT_INVITE && pending->code) {
      g_free(self->invite_code);
      self->invite_code = g_strdup(pending->code);
      g_autofree gchar *link = address(self, self->invite_code);
      adw_action_row_set_subtitle(self->invite_code_row, link);
    }
    break;
  case GH_NIP29_OP_REJECTED:
    text = reason ? g_strdup_printf(_("The relay didn’t allow it: “%s”"), reason)
                  : g_strdup(_("The relay didn’t allow it"));
    break;
  case GH_NIP29_OP_CANCELLED:
    text = g_strdup(_("Cancelled"));
    break;
  case GH_NIP29_OP_NOT_SENT:
  default:
    text = reason ? g_strdup_printf(_("Not sent: %s"), reason) : g_strdup(_("Not sent"));
    break;
  }
  g_hash_table_remove(self->pending, op);
  toast(self, text);
  gtk_accessible_announce(GTK_ACCESSIBLE(self), text, GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
  sync_admin(self);
}

/* Tracks op (transfer full; NULL: the error is shown). */
static void
track(GhGroupInfoDialog *self, GhNip29Op *op, Intent intent, const gchar *code,
      const GError *error)
{
  if (!op) {
    g_message("Groundhog could not send a group change: %s", error ? error->message : "?");
    toast(self, g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED)
                  ? _("You’re not allowed to do that in this group")
                  : _("This change couldn’t be sent"));
    return;
  }
  Pending *pending = g_new0(Pending, 1);
  pending->intent = intent;
  pending->code = g_strdup(code);
  g_hash_table_replace(self->pending, op, pending);
  on_op_changed(NULL, op, self); /* it may have been answered already */
}

/* ---- actions --------------------------------------------------------------------------- */

static void
copy_text(GhGroupInfoDialog *self, const gchar *text, const gchar *done)
{
  gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(self)), text);
  toast(self, done);
}

static void
action_copy_address(GtkWidget *widget, const gchar *name, GVariant *parameter)
{
  (void)name;
  (void)parameter;
  GhGroupInfoDialog *self = GH_GROUP_INFO_DIALOG(widget);
  if (!self->service || self->closing)
    return;
  g_autofree gchar *text = address(self, NULL);
  copy_text(self, text, _("Group address copied"));
}

static void
action_copy_invite(GtkWidget *widget, const gchar *name, GVariant *parameter)
{
  (void)name;
  (void)parameter;
  GhGroupInfoDialog *self = GH_GROUP_INFO_DIALOG(widget);
  if (!self->service || self->closing)
    return;
  if (!self->invite_code)
    return;
  g_autofree gchar *text = address(self, self->invite_code);
  copy_text(self, text, _("Invite link copied"));
}

static void
action_add_member(GtkWidget *widget, const gchar *name, GVariant *parameter)
{
  (void)name;
  (void)parameter;
  GhGroupInfoDialog *self = GH_GROUP_INFO_DIALOG(widget);
  if (!self->service || self->closing)
    return;
  gtk_editable_set_text(GTK_EDITABLE(self->add_entry), "");
  gtk_widget_set_visible(GTK_WIDGET(self->add_error_label), FALSE);
  adw_navigation_view_push_by_tag(self->navigation, "add");
  gtk_widget_grab_focus(GTK_WIDGET(self->add_entry));
}

static void
action_save_member(GtkWidget *widget, const gchar *name, GVariant *parameter)
{
  (void)name;
  (void)parameter;
  GhGroupInfoDialog *self = GH_GROUP_INFO_DIALOG(widget);
  if (!self->service || self->closing)
    return;
  g_autoptr(GhRecipientInput) input =
    gh_recipient_input_parse(gtk_editable_get_text(GTK_EDITABLE(self->add_entry)));
  if (input->kind != GH_RECIPIENT_INPUT_PUBKEY) {
    const gchar *message = input->kind == GH_RECIPIENT_INPUT_SECRET
      ? _("That’s a secret key. Never share it: paste their npub instead.")
      : _("That isn’t an npub. Paste the npub of the person to add.");
    gtk_label_set_text(self->add_error_label, message);
    gtk_widget_set_visible(GTK_WIDGET(self->add_error_label), TRUE);
    gtk_accessible_announce(GTK_ACCESSIBLE(self), message,
                            GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_HIGH);
    return;
  }
  g_autoptr(GError) error = NULL;
  GhNip29Op *op = gh_nip29_service_put_user(self->service, self->room, input->pubkey, NULL, NULL,
                                            &error);
  track(self, op, INTENT_ADD, NULL, error);
  adw_navigation_view_pop(self->navigation);
}

static void
clear_role_rows(GhGroupInfoDialog *self)
{
  for (guint i = 0; i < self->role_rows->len; i++)
    adw_preferences_group_remove(self->roles_group, g_ptr_array_index(self->role_rows, i));
  g_ptr_array_set_size(self->role_rows, 0);
  g_ptr_array_set_size(self->role_checks, 0);
  g_ptr_array_set_size(self->role_names, 0);
}

static void
action_change_role(GtkWidget *widget, const gchar *name, GVariant *parameter)
{
  (void)name;
  GhGroupInfoDialog *self = GH_GROUP_INFO_DIALOG(widget);
  if (!self->service || self->closing)
    return;
  const gchar *pubkey = g_variant_get_string(parameter, NULL);
  if (!gh_recipient_is_pubkey(pubkey))
    return;
  g_free(self->role_target);
  self->role_target = g_strdup(pubkey);
  clear_role_rows(self);
  const GhNip29Group *group = gh_nip29_room_get_group(self->room);
  g_autoptr(GPtrArray) roles = group ? gh_nip29_group_dup_roles(group) : NULL;
  g_auto(GStrv) held = group ? gh_nip29_group_dup_admin_roles(group, pubkey) : NULL;
  for (guint i = 0; roles && i < roles->len; i++) {
    const GhNip29Role *role = g_ptr_array_index(roles, i);
    if (!role->name || !g_utf8_validate(role->name, -1, NULL))
      continue;
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), role->name);
    if (role->description && g_utf8_validate(role->description, -1, NULL))
      adw_action_row_set_subtitle(ADW_ACTION_ROW(row), role->description);
    GtkWidget *check = gtk_check_button_new();
    gtk_widget_set_valign(check, GTK_ALIGN_CENTER);
    gtk_accessible_update_property(GTK_ACCESSIBLE(check), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   role->name, -1);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(check),
                                held && g_strv_contains((const gchar *const *)held, role->name));
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), check);
    adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), check);
    adw_preferences_group_add(self->roles_group, row);
    g_ptr_array_add(self->role_rows, row);
    g_ptr_array_add(self->role_checks, check);
    g_ptr_array_add(self->role_names, g_strdup(role->name));
  }
  const gchar *who = display_name(self, pubkey);
  g_autofree gchar *npub = gh_recipient_npub_short(pubkey);
  g_autofree gchar *title = roles && roles->len
    ? g_strdup_printf(_("Roles for %s"), who ? who : npub)
    : g_strdup(_("The relay hasn’t published any roles for this group."));
  adw_preferences_group_set_title(self->roles_group, title);
  gtk_widget_set_sensitive(self->role_save_button, roles && roles->len);
  adw_navigation_view_push_by_tag(self->navigation, "role");
}

static void
action_save_role(GtkWidget *widget, const gchar *name, GVariant *parameter)
{
  (void)name;
  (void)parameter;
  GhGroupInfoDialog *self = GH_GROUP_INFO_DIALOG(widget);
  if (!self->service || self->closing)
    return;
  if (!self->role_target)
    return;
  g_autoptr(GPtrArray) chosen = g_ptr_array_new();
  for (guint i = 0; i < self->role_checks->len; i++)
    if (gtk_check_button_get_active(g_ptr_array_index(self->role_checks, i)))
      g_ptr_array_add(chosen, g_ptr_array_index(self->role_names, i));
  g_ptr_array_add(chosen, NULL);
  g_autoptr(GError) error = NULL;
  GhNip29Op *op = gh_nip29_service_put_user(self->service, self->room, self->role_target,
                                            (const gchar *const *)chosen->pdata, NULL, &error);
  track(self, op, INTENT_ROLE, NULL, error);
  adw_navigation_view_pop(self->navigation);
}

static void
action_remove(GtkWidget *widget, const gchar *name, GVariant *parameter)
{
  (void)name;
  GhGroupInfoDialog *self = GH_GROUP_INFO_DIALOG(widget);
  if (!self->service || self->closing)
    return;
  const gchar *pubkey = g_variant_get_string(parameter, NULL);
  if (!gh_recipient_is_pubkey(pubkey))
    return;
  g_free(self->remove_target);
  self->remove_target = g_strdup(pubkey);
  const gchar *who = display_name(self, pubkey);
  g_autofree gchar *npub = gh_recipient_npub_short(pubkey);
  g_autofree gchar *body = g_strdup_printf(_("The relay removes %s from the group. They can ask "
                                             "to join again."),
                                           who ? who : npub);
  adw_alert_dialog_set_body(self->remove_dialog, body);
  adw_dialog_present(ADW_DIALOG(self->remove_dialog), GTK_WIDGET(self));
}

static void
on_remove_response(AdwAlertDialog *dialog, const gchar *response, GhGroupInfoDialog *self)
{
  (void)dialog;
  if (g_strcmp0(response, "remove-confirm") != 0 || !self->remove_target || !self->service)
    return;
  g_autoptr(GError) error = NULL;
  GhNip29Op *op = gh_nip29_service_remove_user(self->service, self->room, self->remove_target,
                                               NULL, &error);
  track(self, op, INTENT_REMOVE, NULL, error);
}

static void
action_edit(GtkWidget *widget, const gchar *name, GVariant *parameter)
{
  (void)name;
  (void)parameter;
  GhGroupInfoDialog *self = GH_GROUP_INFO_DIALOG(widget);
  if (!self->service || self->closing)
    return;
  const GhNip29Group *group = gh_nip29_room_get_group(self->room);
  g_autoptr(GhNip29Metadata) metadata = group ? gh_nip29_group_dup_metadata(group) : NULL;
  gtk_editable_set_text(GTK_EDITABLE(self->name_entry),
                        metadata && metadata->name ? metadata->name : "");
  gtk_editable_set_text(GTK_EDITABLE(self->about_entry),
                        metadata && metadata->about ? metadata->about : "");
  adw_switch_row_set_active(self->private_switch, metadata && metadata->is_private);
  adw_switch_row_set_active(self->closed_switch, metadata && metadata->is_closed);
  adw_navigation_view_push_by_tag(self->navigation, "edit");
}

static gchar *
entry_text(AdwEntryRow *entry)
{
  g_autofree gchar *text = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry))));
  return *text ? g_steal_pointer(&text) : NULL;
}

static void
action_save_edit(GtkWidget *widget, const gchar *name, GVariant *parameter)
{
  (void)name;
  (void)parameter;
  GhGroupInfoDialog *self = GH_GROUP_INFO_DIALOG(widget);
  if (!self->service || self->closing)
    return;
  const GhNip29Group *group = gh_nip29_room_get_group(self->room);
  /* NIP-29 edits replace the metadata: start from the relay's, so its
   * supported kinds, parent, children and unknown tags survive. */
  g_autoptr(GhNip29Metadata) metadata = group ? gh_nip29_group_dup_metadata(group) : NULL;
  if (!metadata)
    metadata = gh_nip29_metadata_new();
  g_free(metadata->name);
  metadata->name = entry_text(self->name_entry);
  g_free(metadata->about);
  metadata->about = entry_text(self->about_entry);
  metadata->is_private = adw_switch_row_get_active(self->private_switch);
  metadata->is_closed = adw_switch_row_get_active(self->closed_switch);
  g_autoptr(GError) error = NULL;
  GhNip29Op *op = gh_nip29_service_edit_metadata(self->service, self->room, metadata, NULL,
                                                 &error);
  track(self, op, INTENT_EDIT, NULL, error);
  adw_navigation_view_pop(self->navigation);
}

static void
action_create_invite(GtkWidget *widget, const gchar *name, GVariant *parameter)
{
  (void)name;
  (void)parameter;
  GhGroupInfoDialog *self = GH_GROUP_INFO_DIALOG(widget);
  if (!self->service || self->closing)
    return;
  g_autofree gchar *code = gh_nip29_new_group_id(); /* random, from the OS CSPRNG */
  g_autoptr(GError) error = NULL;
  GhNip29Op *op = gh_nip29_service_create_invite(self->service, self->room, code, NULL, &error);
  track(self, op, INTENT_INVITE, code, error);
}

static void
action_join(GtkWidget *widget, const gchar *name, GVariant *parameter)
{
  (void)name;
  (void)parameter;
  GhGroupInfoDialog *self = GH_GROUP_INFO_DIALOG(widget);
  if (!self->service || self->closing)
    return;
  g_autoptr(GError) error = NULL;
  g_autoptr(GhNip29Room) room = gh_nip29_service_join(self->service,
                                                      gh_nip29_room_get_relay_url(self->room),
                                                      gh_nip29_room_get_group_id(self->room),
                                                      NULL, NULL, &error);
  if (!room) {
    g_message("Groundhog could not ask to join a group: %s", error->message);
    toast(self, _("The join request couldn’t be sent"));
  }
}

static void
action_leave(GtkWidget *widget, const gchar *name, GVariant *parameter)
{
  (void)name;
  (void)parameter;
  GhGroupInfoDialog *self = GH_GROUP_INFO_DIALOG(widget);
  if (!self->service || self->closing)
    return;
  adw_dialog_present(ADW_DIALOG(self->leave_dialog), GTK_WIDGET(self));
}

static void
on_leave_response(AdwAlertDialog *dialog, const gchar *response, GhGroupInfoDialog *self)
{
  (void)dialog;
  if (g_strcmp0(response, "leave-confirm") != 0 || !self->service)
    return;
  g_autoptr(GError) error = NULL;
  g_autoptr(GhNip29Op) op = gh_nip29_service_leave(self->service, self->room, NULL, &error);
  if (!op) {
    g_message("Groundhog could not leave a group: %s", error->message);
    toast(self, _("The leave request couldn’t be sent"));
  }
}

static void
action_forget(GtkWidget *widget, const gchar *name, GVariant *parameter)
{
  (void)name;
  (void)parameter;
  GhGroupInfoDialog *self = GH_GROUP_INFO_DIALOG(widget);
  if (!self->service || self->closing)
    return;
  adw_dialog_present(ADW_DIALOG(self->forget_dialog), GTK_WIDGET(self));
}

static void
on_forget_response(AdwAlertDialog *dialog, const gchar *response, GhGroupInfoDialog *self)
{
  (void)dialog;
  if (g_strcmp0(response, "forget-confirm") != 0 || !self->service)
    return;
  g_autoptr(GError) error = NULL;
  if (!gh_nip29_service_forget(self->service, self->room, &error)) {
    g_message("Groundhog could not remove a group: %s", error->message);
    toast(self, _("This group couldn’t be removed"));
  }
  /* The room left the service: on_rooms_changed() closes the dialog. */
}

/* ---- lifetime -------------------------------------------------------------------------- */

static void
close_now(GhGroupInfoDialog *self)
{
  if (self->closing)
    return;
  self->closing = TRUE;
  adw_dialog_force_close(ADW_DIALOG(self));
}

/* The room left the service (forgotten; a new store after an account
 * switch has other room objects). */
static void
on_rooms_changed(GListModel *rooms, guint position, guint removed, guint added,
                 GhGroupInfoDialog *self)
{
  (void)position;
  (void)added;
  if (!removed)
    return;
  for (guint i = 0; i < g_list_model_get_n_items(rooms); i++) {
    g_autoptr(GhNip29Room) room = g_list_model_get_item(rooms, i);
    if (room == self->room)
      return;
  }
  close_now(self);
}

static void
on_service_gone(gpointer data, GObject *where)
{
  GhGroupInfoDialog *self = data;
  (void)where;
  self->service = NULL;
  close_now(self);
}

GhGroupInfoDialog *
gh_group_info_dialog_new(GhNip29Room *room, const GhGroupInfoConfig *config)
{
  g_return_val_if_fail(GH_IS_NIP29_ROOM(room), NULL);
  g_return_val_if_fail(config != NULL && GH_IS_NIP29_SERVICE(config->service), NULL);
  GhGroupInfoDialog *self = g_object_new(GH_TYPE_GROUP_INFO_DIALOG, NULL);
  self->room = g_object_ref(room);
  self->service = config->service;
  g_object_weak_ref(G_OBJECT(self->service), on_service_gone, self);
  self->display_name = config->display_name;
  self->names_data = config->names_data;
  g_signal_connect_object(room, "notify", G_CALLBACK(sync_all), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(room, "group-changed", G_CALLBACK(sync_all), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(self->service, "items-changed", G_CALLBACK(on_rooms_changed), self, 0);
  g_signal_connect_object(gh_nip29_service_get_outbox(self->service), "op-changed",
                          G_CALLBACK(on_op_changed), self, 0);
  sync_privacy(self);
  sync_all(self);
  return self;
}

GhNip29Room *
gh_group_info_dialog_get_room(GhGroupInfoDialog *self)
{
  g_return_val_if_fail(GH_IS_GROUP_INFO_DIALOG(self), NULL);
  return self->room;
}

guint
gh_group_info_dialog_get_pending_ops(GhGroupInfoDialog *self)
{
  g_return_val_if_fail(GH_IS_GROUP_INFO_DIALOG(self), 0);
  return g_hash_table_size(self->pending);
}

static void
gh_group_info_dialog_dispose(GObject *object)
{
  GhGroupInfoDialog *self = GH_GROUP_INFO_DIALOG(object);
  self->closing = TRUE;
  if (self->service) {
    g_object_weak_unref(G_OBJECT(self->service), on_service_gone, self);
    self->service = NULL;
  }
  if (self->pending)
    g_hash_table_remove_all(self->pending);
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_GROUP_INFO_DIALOG);
  g_clear_object(&self->room);
  G_OBJECT_CLASS(gh_group_info_dialog_parent_class)->dispose(object);
}

static void
gh_group_info_dialog_finalize(GObject *object)
{
  GhGroupInfoDialog *self = GH_GROUP_INFO_DIALOG(object);
  g_ptr_array_unref(self->member_rows);
  g_ptr_array_unref(self->role_rows);
  g_ptr_array_unref(self->role_checks);
  g_ptr_array_unref(self->role_names);
  g_hash_table_unref(self->pending);
  g_free(self->role_target);
  g_free(self->remove_target);
  g_free(self->invite_code);
  G_OBJECT_CLASS(gh_group_info_dialog_parent_class)->finalize(object);
}

static void
on_add_activated(GhGroupInfoDialog *self)
{
  gtk_widget_activate_action(GTK_WIDGET(self), "group.save-member", NULL);
}

static void
gh_group_info_dialog_class_init(GhGroupInfoDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  object_class->dispose = gh_group_info_dialog_dispose;
  object_class->finalize = gh_group_info_dialog_finalize;
  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-group-info-dialog.ui");
#define BIND(name) gtk_widget_class_bind_template_child(widget_class, GhGroupInfoDialog, name)
  BIND(toasts);
  BIND(navigation);
  BIND(avatar);
  BIND(title_label);
  BIND(host_label);
  BIND(about_label);
  BIND(privacy_row);
  BIND(visible_row);
  BIND(unprotected_row);
  BIND(membership_row);
  BIND(membership_icon);
  BIND(messages_row);
  BIND(details_row);
  BIND(settings_row);
  BIND(address_row);
  BIND(members_group);
  BIND(add_member_button);
  BIND(admin_group);
  BIND(edit_row);
  BIND(invite_row);
  BIND(invite_code_row);
  BIND(join_row);
  BIND(leave_row);
  BIND(forget_row);
  BIND(roles_group);
  BIND(role_save_button);
  BIND(add_entry);
  BIND(add_error_label);
  BIND(name_entry);
  BIND(about_entry);
  BIND(private_switch);
  BIND(closed_switch);
  BIND(leave_dialog);
  BIND(remove_dialog);
  BIND(forget_dialog);
#undef BIND
  gtk_widget_class_install_action(widget_class, "group.copy-address", NULL, action_copy_address);
  gtk_widget_class_install_action(widget_class, "group.copy-invite", NULL, action_copy_invite);
  gtk_widget_class_install_action(widget_class, "group.add-member", NULL, action_add_member);
  gtk_widget_class_install_action(widget_class, "group.save-member", NULL, action_save_member);
  gtk_widget_class_install_action(widget_class, "group.change-role", "s", action_change_role);
  gtk_widget_class_install_action(widget_class, "group.save-role", NULL, action_save_role);
  gtk_widget_class_install_action(widget_class, "group.remove", "s", action_remove);
  gtk_widget_class_install_action(widget_class, "group.edit", NULL, action_edit);
  gtk_widget_class_install_action(widget_class, "group.save-edit", NULL, action_save_edit);
  gtk_widget_class_install_action(widget_class, "group.create-invite", NULL,
                                  action_create_invite);
  gtk_widget_class_install_action(widget_class, "group.join", NULL, action_join);
  gtk_widget_class_install_action(widget_class, "group.leave", NULL, action_leave);
  gtk_widget_class_install_action(widget_class, "group.forget", NULL, action_forget);
}

static void
gh_group_info_dialog_init(GhGroupInfoDialog *self)
{
  g_type_ensure(GH_TYPE_GROUP_MEMBER_ROW);
  gtk_widget_init_template(GTK_WIDGET(self));
  self->member_rows = g_ptr_array_new();
  self->role_rows = g_ptr_array_new();
  self->role_checks = g_ptr_array_new();
  self->role_names = g_ptr_array_new_with_free_func(g_free);
  self->pending = g_hash_table_new_full(g_direct_hash, g_direct_equal, g_object_unref,
                                        pending_free);
  g_signal_connect(self->leave_dialog, "response", G_CALLBACK(on_leave_response), self);
  g_signal_connect(self->remove_dialog, "response", G_CALLBACK(on_remove_response), self);
  g_signal_connect(self->forget_dialog, "response", G_CALLBACK(on_forget_response), self);
  g_signal_connect_swapped(self->add_entry, "entry-activated", G_CALLBACK(on_add_activated),
                           self);
}
