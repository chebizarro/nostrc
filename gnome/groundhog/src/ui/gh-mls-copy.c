#include "gh-mls-copy.h"

#include "gh-relay-scope.h"

#include <glib/gi18n.h>
#include <string.h>

const gchar *
gh_mls_invitee_copy(GhMlsInviteeState state)
{
  switch (state) {
  case GH_MLS_INVITEE_CHECKING:
    return _("Checking whether they can join…");
  case GH_MLS_INVITEE_READY:
    return _("Ready to invite");
  case GH_MLS_INVITEE_READY_UNPROVEN:
    return _("Ready to invite. Their app can’t prove their account, so others may see "
             "“Identity not verified”");
  case GH_MLS_INVITEE_NOT_SET_UP:
    return _("Hasn’t set up encrypted groups");
  case GH_MLS_INVITEE_NEEDS_UPDATE:
    return _("Their app can’t prove their account, and you only join groups where every "
             "member’s app proves their account");
  case GH_MLS_INVITEE_UNREACHABLE:
    return _("Couldn’t check: no relay answered");
  case GH_MLS_INVITEE_NO_RELAYS:
    return _("Couldn’t check: add a discovery relay in Preferences");
  case GH_MLS_INVITEE_FAILED:
  default:
    return _("Couldn’t check whether they can join");
  }
}

gboolean
gh_mls_invitee_can_invite(GhMlsInviteeState state)
{
  return state == GH_MLS_INVITEE_READY || state == GH_MLS_INVITEE_READY_UNPROVEN;
}

GhMlsIdentityCopy
gh_mls_identity_copy(GhMlsIdentityState state)
{
  GhMlsIdentityCopy copy = { NULL, NULL, FALSE, FALSE, FALSE };
  switch (state) {
  case GH_MLS_IDENTITY_NOT_REQUIRED:
  case GH_MLS_IDENTITY_ENROLLED:
    copy.ready = TRUE;
    break;
  case GH_MLS_IDENTITY_NONE:
    copy.title = _("Encrypted groups start once you’re online");
    copy.description = _("They work only for the active account while it’s connected.");
    break;
  case GH_MLS_IDENTITY_WAITING:
    copy.title = _("Waiting for approval in Nostr Signer…");
    copy.description = _("Approve the request so this device can take part in encrypted groups "
                         "as you. It is asked once each time Groundhog starts.");
    copy.busy = TRUE;
    break;
  case GH_MLS_IDENTITY_DECLINED:
    copy.title = _("Declined in Nostr Signer");
    copy.description = _("Encrypted groups on this device need your approval in Nostr Signer.");
    copy.can_retry = TRUE;
    break;
  case GH_MLS_IDENTITY_FAILED:
  default:
    copy.title = _("Nostr Signer couldn’t approve this device");
    copy.description = _("It failed or returned something Groundhog didn’t ask for.");
    copy.can_retry = TRUE;
    break;
  }
  return copy;
}

gchar *
gh_mls_error_copy(const GError *error)
{
  if (!error)
    return g_strdup(_("Something went wrong."));
  if (error->domain == GH_MLS_SERVICE_ERROR) {
    switch ((GhMlsServiceError)error->code) {
    case GH_MLS_SERVICE_ERROR_NO_CONSENT:
      return g_strdup(_("Only people you’ve accepted as contacts can be invited."));
    case GH_MLS_SERVICE_ERROR_NO_KEY_PACKAGE:
      return g_strdup(_("Someone you invited hasn’t set up encrypted groups. Nothing was "
                        "changed."));
    case GH_MLS_SERVICE_ERROR_NOT_ADMIN:
      return g_strdup(_("Only a group admin can do that."));
    case GH_MLS_SERVICE_ERROR_BUSY:
      return g_strdup(_("Another change to this group is still being sent. Try again once "
                        "it’s done."));
    case GH_MLS_SERVICE_ERROR_REFUSED:
      return g_strdup(_("The group’s relays refused the change. Nothing was changed."));
    case GH_MLS_SERVICE_ERROR_SUPERSEDED:
      return g_strdup(_("Another member changed the group at the same time. Try again."));
    case GH_MLS_SERVICE_ERROR_NO_RELAYS:
      return g_strdup(_("A group needs 1 to 16 secure relay addresses."));
    case GH_MLS_SERVICE_ERROR_INACTIVE:
      return g_strdup(_("Encrypted groups work only for the active account while it’s "
                        "online."));
    case GH_MLS_SERVICE_ERROR_NOT_ENROLLED:
      return g_strdup(_("Approve this device in Nostr Signer first."));
    case GH_MLS_SERVICE_ERROR_NEEDS_UPDATE:
      return g_strdup(_("Someone uses an app that can’t prove their account, and you only join "
                        "groups where every member’s app proves their account. Nothing was "
                        "changed."));
    case GH_MLS_SERVICE_ERROR_FORGED_IDENTITY:
      return g_strdup(_("Someone’s account proof is forged or broken, so Groundhog refused it. "
                        "Nothing was changed."));
    default:
      break;
    }
  }
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT))
    return g_strdup(_("Check the name, the relays and the people you chose."));
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return g_strdup(_("The account changed, so nothing was sent."));
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE))
    return g_strdup(_("No relay answered. Nothing was changed."));
  return g_strdup(_("The change couldn’t be made. Nothing was sent."));
}

gchar *
gh_mls_send_reason(GhMlsService *service, GhMlsGroup *group, const gchar *remover)
{
  if (!service || !group)
    return g_strdup(_("Encrypted groups aren’t running for this account."));
  if (gh_mls_group_get_end(group) == GH_MLS_GROUP_END_REMOVED)
    return remover && *remover
             /* TRANSLATORS: %s is the group admin's name or short npub. */
             ? g_strdup_printf(_("You were removed from this group by %s."), remover)
             : g_strdup(_("You were removed from this group."));
  if (gh_mls_group_get_end(group) == GH_MLS_GROUP_END_UNKNOWN)
    return g_strdup(_("This group has ended on this device."));
  if (!gh_mls_group_get_active(group))
    return g_strdup(_("You left this group."));
  if (gh_mls_group_get_leaving(group))
    return g_strdup(_("You’re leaving this group, so nothing more can be sent to it."));
  if (gh_mls_group_get_read_state(group) == GH_MLS_READ_IDLE)
    return g_strdup(_("Encrypted groups send only while you’re online."));
  return NULL;
}

gchar *
gh_mls_end_copy(GhMlsGroupEnd end, const gchar *remover)
{
  switch (end) {
  case GH_MLS_GROUP_END_REMOVED:
    if (remover && *remover)
      /* TRANSLATORS: %s is the group admin's name or short npub. */
      return g_strdup_printf(_("You were removed from this group by %s. Its messages stay on "
                               "this device."), remover);
    return g_strdup(_("You were removed from this group. Its messages stay on this device."));
  case GH_MLS_GROUP_END_LEFT:
    return g_strdup(_("You left this group. Its messages stay on this device."));
  case GH_MLS_GROUP_END_LEFT_DEVICE:
    /* Only this device stopped (nostrc-2um6): the others weren't told. */
    return g_strdup(_("You left this group on this device. Its other members still count you "
                      "as a member until an admin removes you. Its messages stay on this "
                      "device."));
  case GH_MLS_GROUP_END_UNKNOWN:
    /* Its record can't be read: say that it ended, not why (review N3). */
    return g_strdup(_("This group has ended on this device. Its messages stay on this "
                      "device."));
  case GH_MLS_GROUP_END_NONE:
  default:
    return NULL;
  }
}

const gchar *
gh_mls_leave_copy(GhMlsLeave kind)
{
  switch (kind) {
  case GH_MLS_LEAVE_EVERYONE:
    return _("The other members are told that you left. Once one of them confirms it, this "
             "group stops on this device too. Messages you have stay on this device.");
  case GH_MLS_LEAVE_ADMINS:
    return _("The group’s admins are asked to remove you. Once one of them does, this group "
             "stops on this device too; until then the others still count you. Messages you "
             "have stay on this device.");
  case GH_MLS_LEAVE_DEVICE_ADMIN:
    return _("You’re an admin, so you can’t leave for everyone yet: make someone else an admin "
             "and step down first. Leaving now stops this group on this device only, and the "
             "other members keep counting you until an admin removes you.");
  case GH_MLS_LEAVE_DEVICE_UNSUPPORTED:
    return _("Someone in this group uses an app that can’t process a member leaving. Leaving "
             "now stops this group on this device only, and the other members keep counting "
             "you until an admin removes you.");
  case GH_MLS_LEAVE_DEVICE_WAITING:
    return _("No member has confirmed your leave yet. Stop waiting? This group stops on this "
             "device now, and the other members keep counting you until one of them confirms "
             "it.");
  case GH_MLS_LEAVE_DEVICE:
  default:
    return _("This group stops on this device only, and the other members keep counting you "
             "until an admin removes you.");
  }
}

gchar *
gh_mls_member_left_copy(const gchar *member)
{
  /* TRANSLATORS: %s is the member's name or short npub. */
  return g_strdup_printf(_("%s left the group"), member && *member ? member : _("Someone"));
}

const gchar *
gh_mls_role_copy(GhMlsRole role)
{
  switch (role) {
  case GH_MLS_ROLE_OWNER:
    return _("Owner");
  case GH_MLS_ROLE_ADMIN:
    return _("Admin");
  case GH_MLS_ROLE_MEMBER:
  default:
    return NULL;
  }
}

const gchar *
gh_mls_read_copy(GhMlsReadState read)
{
  switch (read) {
  case GH_MLS_READ_SYNCING:
    return _("Catching up with the group’s relays…");
  case GH_MLS_READ_LIVE:
    return _("Up to date");
  case GH_MLS_READ_DISCONNECTED:
    return _("Can’t reach the group’s relays; trying again");
  case GH_MLS_READ_IDLE:
  default:
    return _("Not reading: offline, another account, or you left");
  }
}

gchar *
gh_mls_invite_subtitle(const gchar *inviter_npub_short, const gchar *inviter_name,
                       gboolean contact, guint member_count)
{
  g_autofree gchar *members = g_strdup_printf(
    g_dngettext(NULL, "%u member", "%u members", member_count), member_count);
  if (contact && inviter_name && *inviter_name)
    /* TRANSLATORS: who invited you, then the member count. */
    return g_strdup_printf(_("From %s · %s"), inviter_name, members);
  if (contact)
    return g_strdup_printf(_("From %s · %s"), inviter_npub_short, members);
  /* PD-8: a stranger's invitation is shown like a request: their npub only. */
  return g_strdup_printf(_("From %s, not in your contacts · %s"), inviter_npub_short, members);
}

gchar *
gh_mls_parse_relay(const gchar *text, GError **error)
{
  g_autofree gchar *trimmed = g_strstrip(g_strdup(text ? text : ""));
  if (!*trimmed) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        _("Enter a relay address, like relay.example.com."));
    return NULL;
  }
  g_autofree gchar *url = strstr(trimmed, "://") ? g_strdup(trimmed)
                                                 : g_strconcat("wss://", trimmed, NULL);
  g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
  const gchar *host = uri ? g_uri_get_host(uri) : NULL;
  gboolean local = host && (g_str_equal(host, "127.0.0.1") || g_str_equal(host, "::1") ||
                            g_ascii_strcasecmp(host, "localhost") == 0 ||
                            g_str_has_suffix(host, ".onion"));
  if (g_str_has_prefix(url, "ws://") && !local) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        _("Group relays need a secure connection. Use the relay’s wss "
                          "address."));
    return NULL;
  }
  if (!host || !gh_relay_url_validate(url, NULL)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        _("This isn’t a relay address. It looks like relay.example.com."));
    return NULL;
  }
  return g_steal_pointer(&url);
}

GhMlsMemberCopy
gh_mls_member_copy(GhMlsMemberIdentity identity, const gchar *added_by, gboolean added_by_self)
{
  GhMlsMemberCopy copy = { NULL, NULL, NULL };
  switch (identity) {
  case GH_MLS_MEMBER_CHECKING:
    copy.badge = _("Checking identity…");
    copy.explanation = g_strdup(_("Groundhog is asking relays for a key this person published."));
    break;
  case GH_MLS_MEMBER_UNVERIFIED:
    copy.badge = _("Identity not verified");
    if (added_by_self)
      copy.explanation = g_strdup(_("They added this device themselves. Groundhog couldn’t "
                                    "confirm this account owns this device."));
    else if (added_by && *added_by)
      /* TRANSLATORS: %s is the group admin's name or short npub. */
      copy.explanation = g_strdup_printf(_("Added by %s. Groundhog couldn’t confirm this "
                                           "account owns this device."), added_by);
    else
      /* Common for older apps, and so for an MDK group's creator (owkh). */
      copy.explanation = g_strdup(_("Their app can’t prove their account, as with many older "
                                    "apps, and Groundhog has no key they published to compare."));
    break;
  case GH_MLS_MEMBER_PROVEN:
  case GH_MLS_MEMBER_VERIFIED:
  default:
    return copy;
  }
  copy.accessible = g_strdup_printf("%s. %s", copy.badge, copy.explanation);
  return copy;
}

void
gh_mls_member_copy_clear(GhMlsMemberCopy *copy)
{
  g_clear_pointer(&copy->explanation, g_free);
  g_clear_pointer(&copy->accessible, g_free);
}

const gchar *
gh_mls_refused_copy(GhMlsRefusal refusal)
{
  switch (refusal) {
  case GH_MLS_REFUSAL_UNPROVEN:
    return _("Groundhog refused a change to this group: it brings in someone whose app can’t "
             "prove their account, or whose proof doesn’t check out, and you only join groups "
             "where every member’s app proves their account. New messages here can’t be read. "
             "Turning that preference off tries the change again.");
  case GH_MLS_REFUSAL_BROKEN_PROOF:
    return _("Groundhog refused a change to this group: an account proof in it doesn’t check "
             "out. If other members accepted it, new messages here can’t be read until the "
             "group moves past that change.");
  case GH_MLS_REFUSAL_UNFOLLOWABLE:
    return _("Groundhog refused a change to this group: Groundhog can’t follow it, or it "
             "breaks the group’s rules. If other members accepted it, new messages here can’t "
             "be read until the group moves past that change.");
  case GH_MLS_REFUSAL_NONE:
  default:
    return NULL;
  }
}

gchar *
gh_mls_verify_prompt(const gchar *name)
{
  /* TRANSLATORS: %s is the member's name or short npub. */
  return g_strdup_printf(_("Groundhog will ask relays for keys %s published and compare them "
                           "with this device. Those relays can see whom you look up, but not "
                           "this group."), name);
}

gchar *
gh_mls_verify_result_copy(GhMlsMemberIdentity identity, const GError *error, const gchar *name)
{
  if (error) {
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE))
      return g_strdup(_("No relay answered, so nothing was checked."));
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT))
      return g_strdup(_("Add a discovery relay in Preferences to verify identities."));
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
      return g_strdup(_("This person’s relays are the group’s relays, so checking would "
                        "reveal this group."));
    return gh_mls_error_copy(error);
  }
  if (identity == GH_MLS_MEMBER_VERIFIED || identity == GH_MLS_MEMBER_PROVEN)
    /* TRANSLATORS: %s is the member's name or short npub. */
    return g_strdup_printf(_("Verified: %s published this device’s key."), name);
  /* TRANSLATORS: %s is the member's name or short npub. */
  return g_strdup_printf(_("%s hasn’t published a key for this device. Their identity stays "
                           "unverified."), name);
}
