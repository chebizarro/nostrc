#include "gh-group-copy.h"

#include <glib/gi18n.h>
#include <nip29.h>
#include <string.h>

/* ---- References ---------------------------------------------------------------------- */

static gboolean
invalid(GError **error, const gchar *message)
{
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, message);
  return FALSE;
}

static gboolean
host_is_loopback(const gchar *host)
{
  if (!host)
    return FALSE;
  if (g_ascii_strcasecmp(host, "localhost") == 0)
    return TRUE;
  g_autoptr(GInetAddress) address = g_inet_address_new_from_string(host);
  return address && g_inet_address_get_is_loopback(address);
}

gchar *
gh_group_parse_relay(const gchar *text, GError **error)
{
  g_autofree gchar *trimmed = g_strstrip(g_strdup(text ? text : ""));
  if (!*trimmed) {
    invalid(error, _("Enter the group relay's address."));
    return NULL;
  }
  /* NIP-29 writes a group's relay as its host name: wss:// is meant. */
  g_autofree gchar *url = strstr(trimmed, "://") ? g_strdup(trimmed)
                                                 : g_strconcat("wss://", trimmed, NULL);
  g_autofree gchar *normalized = gh_nip29_normalize_relay_url(url, NULL);
  if (!normalized) {
    invalid(error, _("This isn't a relay address. It looks like groups.example.com."));
    return NULL;
  }
  /* ws:// only to a loopback test relay or a .onion service (Tor already
   * encrypts it); the relay transport then allows .onion only in Tor mode
   * (gh_net_relay_url_allowed, G09). */
  if (g_str_has_prefix(normalized, "ws://")) {
    g_autoptr(GUri) uri = g_uri_parse(normalized, G_URI_FLAGS_NONE, NULL);
    const gchar *host = uri ? g_uri_get_host(uri) : NULL;
    if (!host || !(host_is_loopback(host) || g_str_has_suffix(host, ".onion"))) {
      invalid(error, _("Groundhog only connects to group relays over a secure connection. "
                       "Use the relay’s wss address."));
      return NULL;
    }
  }
  return g_steal_pointer(&normalized);
}

gboolean
gh_group_parse_reference(const gchar *text, gchar **out_relay_url, gchar **out_group_id,
                         gchar **out_invite_code, GError **error)
{
  g_return_val_if_fail(out_relay_url && out_group_id, FALSE);
  *out_relay_url = NULL;
  *out_group_id = NULL;
  if (out_invite_code)
    *out_invite_code = NULL;
  g_autofree gchar *trimmed = g_strstrip(g_strdup(text ? text : ""));
  if (!*trimmed)
    return invalid(error, _("Enter a group address or paste a group link."));
  nostr_group_address_t address = { 0 };
  char *code = NULL;
  if (!nostr_group_reference_parse(trimmed, &address, &code))
    return invalid(error, _("This isn't a group address. It looks like "
                            "groups.example.com'group-id, or a nostr: link."));
  g_autoptr(GError) local = NULL;
  g_autofree gchar *relay = gh_group_parse_relay(address.relay, &local);
  g_autoptr(GhNip29GroupKey) key = relay ? gh_nip29_group_key_new(relay, address.id, NULL) : NULL;
  gboolean ok = key != NULL;
  if (!relay)
    g_propagate_error(error, g_steal_pointer(&local));
  else if (!key)
    invalid(error, _("This group address has an invalid group id."));
  if (ok) {
    *out_relay_url = g_steal_pointer(&relay);
    *out_group_id = g_strdup(address.id);
    if (out_invite_code && code && *code)
      *out_invite_code = g_strdup(code);
  }
  free(code);
  nostr_group_address_clear(&address);
  return ok;
}

gchar *
gh_group_relay_host(const gchar *relay_url)
{
  g_autoptr(GUri) uri = relay_url ? g_uri_parse(relay_url, G_URI_FLAGS_NONE, NULL) : NULL;
  const gchar *host = uri ? g_uri_get_host(uri) : NULL;
  if (!host || !*host)
    return g_strdup(relay_url ? relay_url : "");
  gint port = g_uri_get_port(uri);
  return port > 0 ? g_strdup_printf("%s:%d", host, port) : g_strdup(host);
}

gchar *
gh_group_format_address(const gchar *relay_url, const gchar *group_id, const gchar *invite_code)
{
  g_return_val_if_fail(relay_url && group_id, NULL);
  g_autoptr(GUri) uri = g_uri_parse(relay_url, G_URI_FLAGS_NONE, NULL);
  const gchar *path = uri ? g_uri_get_path(uri) : NULL;
  gboolean bare = uri && g_strcmp0(g_uri_get_scheme(uri), "wss") == 0 && (!path || !*path);
  g_autofree gchar *relay = bare ? gh_group_relay_host(relay_url) : g_strdup(relay_url);
  if (invite_code && *invite_code) {
    g_autofree gchar *escaped = g_uri_escape_string(invite_code, NULL, FALSE);
    return g_strdup_printf("%s'%s?invite=%s", relay, group_id, escaped);
  }
  return g_strdup_printf("%s'%s", relay, group_id);
}

gchar *
gh_group_relay_reason(const gchar *message)
{
  if (!message)
    return NULL;
  static const gchar *const prefixes[] = { "duplicate:", "pow:", "blocked:", "rate-limited:",
                                           "invalid:", "restricted:", "mute:", "error:",
                                           "auth-required:", NULL };
  const gchar *rest = message;
  for (guint i = 0; prefixes[i]; i++) {
    if (g_str_has_prefix(message, prefixes[i])) {
      rest = message + strlen(prefixes[i]);
      break;
    }
  }
  g_autofree gchar *trimmed = g_strstrip(g_strdup(rest));
  if (!*trimmed || !g_utf8_validate(trimmed, -1, NULL))
    return NULL;
  /* A relay's sentence may start lower-case after its prefix. */
  gunichar first = g_utf8_get_char(trimmed);
  g_autofree gchar *head = g_utf8_strup(trimmed, g_utf8_next_char(trimmed) - trimmed);
  if (g_unichar_islower(first))
    return g_strconcat(head, g_utf8_next_char(trimmed), NULL);
  return g_steal_pointer(&trimmed);
}

/* ---- States ---------------------------------------------------------------------------- */

void
gh_group_state_copy_free(GhGroupStateCopy *copy)
{
  if (!copy)
    return;
  g_free(copy->title);
  g_free(copy->description);
  g_free(copy);
}

static GhGroupStateCopy *
state_copy(GhGroupTone tone, const gchar *icon, const gchar *title, gchar *description)
{
  GhGroupStateCopy *copy = g_new0(GhGroupStateCopy, 1);
  copy->tone = tone;
  copy->icon_name = icon;
  copy->title = g_strdup(title);
  copy->description = description;
  return copy;
}

/* body, then the relay's own words when it gave any. */
static gchar *
with_reason(const gchar *body, const gchar *detail)
{
  g_autofree gchar *reason = gh_group_relay_reason(detail);
  if (!reason)
    return g_strdup(body);
  /* TRANSLATORS: a sentence, then what the relay answered, quoted. */
  return g_strdup_printf(_("%s The relay said: “%s”"), body, reason);
}

static gboolean
waiting_for_signer(GhNip29Op *request)
{
  return request && gh_nip29_op_get_result(request) == GH_NIP29_OP_WAITING_FOR_SIGNER;
}

GhGroupStateCopy *
gh_group_join_copy(GhNip29JoinState join, GhNip29Op *request, gboolean already_member,
                   const gchar *detail, const gchar *host)
{
  const gchar *relay = host && *host ? host : _("the group's relay");
  GhGroupStateCopy *copy = NULL;
  switch (join) {
  case GH_NIP29_JOIN_REQUESTING:
    if (waiting_for_signer(request))
      return state_copy(GH_GROUP_TONE_PROGRESS, "dialog-password-symbolic",
                        _("Waiting for Approval in Nostr Signer"),
                        g_strdup(_("Approve the join request in Nostr Signer to send it.")));
    return state_copy(GH_GROUP_TONE_PROGRESS, "network-transmit-receive-symbolic",
                      _("Asking to Join…"),
                      /* TRANSLATORS: %s is a relay's host name. */
                      g_strdup_printf(_("Waiting for %s to answer."), relay));
  case GH_NIP29_JOIN_PENDING: {
    g_autofree gchar *body = g_strdup_printf(
      /* TRANSLATORS: %s is a relay's host name. */
      _("A group admin has to approve your request. %s will let you in when they do."), relay);
    copy = state_copy(GH_GROUP_TONE_WARNING, "document-open-recent-symbolic",
                      _("Waiting for an Admin"), with_reason(body, detail));
    copy->can_open = TRUE;
    return copy;
  }
  case GH_NIP29_JOIN_MEMBER:
    if (already_member ||
        (request && gh_nip29_op_get_result(request) == GH_NIP29_OP_DUPLICATE))
      copy = state_copy(GH_GROUP_TONE_SUCCESS, "emblem-ok-symbolic", _("Already a Member"),
                        g_strdup(_("You're already in this group.")));
    else
      copy = state_copy(GH_GROUP_TONE_SUCCESS, "emblem-ok-symbolic", _("You're In"),
                        g_strdup_printf(_("%s let you into the group. Its operators can "
                                          "read the messages in it."),
                                        relay));
    copy->can_open = TRUE;
    return copy;
  case GH_NIP29_JOIN_DENIED:
    copy = state_copy(GH_GROUP_TONE_ERROR, "action-unavailable-symbolic", _("Request Declined"),
                      with_reason(_("The group didn't let you join."), detail));
    copy->needs_code = TRUE; /* an invite code may still open it */
    return copy;
  case GH_NIP29_JOIN_CLOSED:
    copy = state_copy(GH_GROUP_TONE_WARNING, "system-lock-screen-symbolic", _("Invite Only"),
                      with_reason(_("This group only lets people in with an invite code. Ask a "
                                    "group admin for one."),
                                  detail));
    copy->needs_code = TRUE;
    return copy;
  case GH_NIP29_JOIN_NOT_SENT:
    copy = state_copy(GH_GROUP_TONE_ERROR, "dialog-warning-symbolic", _("Request Not Sent"),
                      with_reason(_("Your join request couldn't be sent. Nothing was "
                                    "shared with the group."),
                                  detail));
    copy->can_retry = TRUE;
    return copy;
  case GH_NIP29_JOIN_LEAVING:
    return state_copy(GH_GROUP_TONE_PROGRESS, "network-transmit-receive-symbolic",
                      _("Leaving…"),
                      g_strdup_printf(_("Waiting for %s to answer."), relay));
  case GH_NIP29_JOIN_LEFT:
    copy = state_copy(GH_GROUP_TONE_NEUTRAL, "system-log-out-symbolic", _("You Left This Group"),
                      g_strdup(_("Messages you received before leaving stay on this device.")));
    copy->can_retry = TRUE;
    return copy;
  case GH_NIP29_JOIN_REMOVED:
    copy = state_copy(GH_GROUP_TONE_ERROR, "action-unavailable-symbolic",
                      _("Removed from Group"),
                      with_reason(_("A group admin removed you."), detail));
    return copy;
  case GH_NIP29_JOIN_CREATING:
    return state_copy(GH_GROUP_TONE_PROGRESS, "network-transmit-receive-symbolic",
                      _("Creating Group…"),
                      g_strdup_printf(_("Waiting for %s to create the group."), relay));
  case GH_NIP29_JOIN_NONE:
  default:
    copy = state_copy(GH_GROUP_TONE_NEUTRAL, "list-add-symbolic", _("Not Joined"),
                      with_reason(_("You haven't joined this group."), detail));
    copy->can_retry = TRUE;
    return copy;
  }
}

GhGroupStateCopy *
gh_group_create_copy(GhNip29JoinState join, GhNip29Op *request, gboolean has_state,
                     const gchar *detail, const gchar *host)
{
  const gchar *relay = host && *host ? host : _("the group's relay");
  GhGroupStateCopy *copy = NULL;
  switch (join) {
  case GH_NIP29_JOIN_CREATING:
    if (waiting_for_signer(request))
      return state_copy(GH_GROUP_TONE_PROGRESS, "dialog-password-symbolic",
                        _("Waiting for Approval in Nostr Signer"),
                        g_strdup(_("Approve the new group in Nostr Signer to send it.")));
    return gh_group_join_copy(join, request, FALSE, detail, host);
  case GH_NIP29_JOIN_MEMBER:
    copy = state_copy(has_state ? GH_GROUP_TONE_SUCCESS : GH_GROUP_TONE_PROGRESS,
                      has_state ? "emblem-ok-symbolic" : "network-transmit-receive-symbolic",
                      has_state ? _("Group Created") : _("Group Created, Waiting for Details"),
                      g_strdup_printf(has_state
                                        ? _("%s created the group and made you its admin. "
                                            "Its operators can read the messages in it.")
                                        : _("%s created the group. Its name and settings show "
                                            "once the relay publishes them."),
                                      relay));
    copy->can_open = TRUE;
    return copy;
  case GH_NIP29_JOIN_NONE:
  default: {
    g_autofree gchar *body = g_strdup_printf(
      _("%s didn't create the group. Not every relay lets people create groups."), relay);
    copy = state_copy(GH_GROUP_TONE_ERROR, "dialog-warning-symbolic", _("Group Not Created"),
                      with_reason(body, detail));
    copy->can_retry = TRUE;
    return copy;
  }
  }
}

gchar *
gh_group_send_reason(GhNip29JoinState join, gboolean restricted)
{
  switch (join) {
  case GH_NIP29_JOIN_MEMBER:
    return NULL;
  case GH_NIP29_JOIN_REQUESTING:
  case GH_NIP29_JOIN_PENDING:
    /* The relay decides whether a non-member may write (gh-nip29-service.h). */
    if (!restricted)
      return NULL;
    return g_strdup(join == GH_NIP29_JOIN_PENDING
                      ? _("Only members can write in this group. A group admin still has to "
                          "approve your join request.")
                      : _("Only members can write in this group. Waiting for the relay to "
                          "answer your join request."));
  case GH_NIP29_JOIN_CLOSED:
    return g_strdup(_("This group is closed to you: it only lets people in with an invite "
                      "code."));
  case GH_NIP29_JOIN_DENIED:
    return g_strdup(_("This group is closed to you: it declined your join request."));
  case GH_NIP29_JOIN_REMOVED:
    return g_strdup(_("This group is closed to you: a group admin removed you."));
  case GH_NIP29_JOIN_LEFT:
    return g_strdup(_("You left this group, so you can't write in it."));
  case GH_NIP29_JOIN_LEAVING:
    return g_strdup(_("Leaving this group…"));
  case GH_NIP29_JOIN_CREATING:
    return g_strdup(_("Waiting for the relay to create this group…"));
  case GH_NIP29_JOIN_NOT_SENT:
  case GH_NIP29_JOIN_NONE:
  default:
    return g_strdup(_("Join this group to write in it."));
  }
}

gchar *
gh_group_room_send_reason(GhNip29Service *service, const gchar *room_id)
{
  if (!service)
    return g_strdup(_("Groups aren’t available right now: message storage isn’t open."));
  g_autoptr(GhNip29Room) room = gh_nip29_service_lookup_room(service, room_id);
  if (!room)
    return g_strdup(_("This group isn’t in your list anymore, so you can’t write in it."));
  return gh_group_send_reason(gh_nip29_room_get_join_state(room),
                              gh_nip29_room_get_is_restricted(room));
}

GhNip29Op *
gh_group_find_message_op(GhNip29Service *service, const gchar *event_id)
{
  if (!service || !event_id)
    return NULL;
  g_autoptr(GPtrArray) ops = gh_nip29_outbox_dup_ops(gh_nip29_service_get_outbox(service));
  for (guint i = ops->len; i > 0; i--) {
    GhNip29Op *op = g_ptr_array_index(ops, i - 1);
    if (gh_nip29_op_get_message_id(op) > 0 &&
        g_strcmp0(gh_nip29_op_get_event_id(op), event_id) == 0)
      return g_object_ref(op);
  }
  return NULL;
}

gchar *
gh_group_op_outcome(GhNip29Op *op)
{
  g_return_val_if_fail(GH_IS_NIP29_OP(op), NULL);
  g_autofree gchar *reason = gh_group_relay_reason(gh_nip29_op_get_relay_message(op));
  switch (gh_nip29_op_get_result(op)) {
  case GH_NIP29_OP_ACCEPTED:
  case GH_NIP29_OP_DUPLICATE:
    return g_strdup(_("The group’s relay accepted it. It can’t tell whether anyone read it."));
  case GH_NIP29_OP_PENDING_APPROVAL:
    return g_strdup(_("The group’s relay holds it for review."));
  case GH_NIP29_OP_REJECTED:
    return reason ? g_strdup_printf(_("The group’s relay refused it: “%s”"), reason)
                  : g_strdup(_("The group’s relay refused it."));
  case GH_NIP29_OP_NOT_SENT:
    return reason ? g_strdup_printf(_("Not sent: %s"), reason) : g_strdup(_("Not sent."));
  case GH_NIP29_OP_RETRYING:
    return g_strdup(_("The group’s relay couldn’t be reached. Groundhog tries again."));
  case GH_NIP29_OP_WAITING_FOR_SIGNER:
    return g_strdup(_("Waiting for approval in Nostr Signer."));
  case GH_NIP29_OP_CANCELLED:
    return g_strdup(_("Cancelled."));
  case GH_NIP29_OP_SENDING:
  case GH_NIP29_OP_QUEUED:
  default:
    return g_strdup(_("Waiting for the group’s relay to answer."));
  }
}

GhGroupGate
gh_group_gate(GhNip29JoinState join, GhNip29Authz authz)
{
  if (join != GH_NIP29_JOIN_MEMBER)
    return GH_GROUP_GATE_HIDDEN;
  switch (authz) {
  case GH_NIP29_AUTHZ_ALLOWED:
    return GH_GROUP_GATE_ALLOWED;
  case GH_NIP29_AUTHZ_UNKNOWN_POLICY:
    return GH_GROUP_GATE_RELAY_DECIDES;
  case GH_NIP29_AUTHZ_UNKNOWN_NO_ADMINS: /* nothing says the account is an admin */
  case GH_NIP29_AUTHZ_DENIED_INVALID:
  case GH_NIP29_AUTHZ_DENIED_NOT_ADMIN:
  case GH_NIP29_AUTHZ_DENIED_UNADVERTISED_ROLES:
  case GH_NIP29_AUTHZ_DENIED_BY_POLICY:
  default:
    return GH_GROUP_GATE_HIDDEN;
  }
}

GhGroupGate
gh_group_room_gate(GhNip29Room *room, nostr_permission_t permission)
{
  g_return_val_if_fail(GH_IS_NIP29_ROOM(room), GH_GROUP_GATE_HIDDEN);
  return gh_group_gate(gh_nip29_room_get_join_state(room),
                       gh_nip29_room_check_permission(room, permission));
}

const gchar *
gh_group_read_copy(GhNip29ReadState read)
{
  switch (read) {
  case GH_NIP29_READ_SYNCING:
    return _("Catching up with the relay…");
  case GH_NIP29_READ_LIVE:
    return _("Up to date");
  case GH_NIP29_READ_AUTH_REQUIRED:
    return _("The relay wants you to sign in before it shares messages");
  case GH_NIP29_READ_REFUSED:
    return _("The relay refused to share this group's messages");
  case GH_NIP29_READ_DISCONNECTED:
    return _("Connection lost. Reconnecting…");
  case GH_NIP29_READ_IDLE:
  default:
    return _("Not connected");
  }
}

const gchar *
gh_group_key_copy(GhNip29RelayKeyState state)
{
  switch (state) {
  case GH_NIP29_RELAY_KEY_PINNED:
    return _("Signed by the relay’s key");
  case GH_NIP29_RELAY_KEY_UNAVAILABLE:
    return _("The relay doesn’t publish its key, so the group’s name, admins and members "
             "can’t be verified and aren’t shown");
  case GH_NIP29_RELAY_KEY_UNKNOWN:
  default:
    return _("Not checked with the relay yet");
  }
}

const gchar *
gh_group_members_copy(GhNip29MemberList members)
{
  return members == GH_NIP29_MEMBERS_PARTIAL
    ? _("Member list may be incomplete: relays may share only some members.")
    : _("Member list may be incomplete: the relay hasn’t shared one.");
}
