#include "gh-message.h"

#include <glib/gi18n.h>

#include <nostr-event.h>
#include <nostr-tag.h>
#include <nostr/nip19/nip19.h>
#include <stdlib.h>
#include <string.h>

GStrv
gh_message_extract_mentions(const gchar *content)
{
  g_autoptr(GStrvBuilder) names = g_strv_builder_new();
  g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  if (!content)
    return g_strv_builder_end(names);
  const gchar *p = content;
  while ((p = strstr(p, "nostr:npub1"))) {
    if (p > content && (g_ascii_isalnum(p[-1]) || p[-1] == ':')) {
      p++;
      continue;
    }
    const gchar *start = p + strlen("nostr:");
    const gchar *end = start;
    while (g_ascii_isalnum(*end))
      end++;
    g_autofree gchar *npub = g_strndup(start, end - start);
    guint8 key[32];
    if (nostr_nip19_decode_npub(npub, key) == 0) {
      gchar hex[65];
      for (guint i = 0; i < sizeof key; i++)
        g_snprintf(hex + 2 * i, sizeof hex - 2 * i, "%02x", key[i]);
      if (g_hash_table_add(seen, g_strdup(hex)))
        g_strv_builder_add(names, hex);
    }
    p = end;
  }
  return g_strv_builder_end(names);
}

struct _GhMessage {
  GObject parent_instance;
  gchar *account;
  gchar *rumor_id;
  gchar *rumor_json;
  gchar *sender;
  GStrv recipients;
  GStrv participants;
  gchar *room_id;
  gint64 created_at;
  gchar *content;
  gchar *subject;
  gint64 expires_at;
  guint64 seq;       /* local arrival order in its room (store-assigned) */
  GhMessageStatus status;
  GPtrArray *relays; /* NULL-terminated */
  /* NIP-29 group events only. */
  gboolean nip29;
  gboolean is_signed;
  gint kind;
  gchar *group_id;
  gchar *group_relay;
  /* MLS group messages only (group_id is then the hex MLS group id). */
  gboolean mls;
  gboolean has_mls_epoch;
  guint64 mls_epoch;
  GPtrArray *attachments;   /* GhMessageAttachment, MLS only; NULL: none */
  guint rejected_attachments;
  gboolean withdrawn;   /* nostrc-xrza: withdrawn by the group's convergence */
  /* nostrc-zjkv: NIP-29/MLS reply target (the first e-reply or q tag). */
  gchar *reply_to_id;
  gboolean legacy_nip04; /* W33: a NIP-04 (kind 4) DM, kept as a local rumor */
};

enum {
  PROP_0,
  PROP_RUMOR_ID,
  PROP_SENDER,
  PROP_BODY,
  PROP_IS_OUTGOING,
  PROP_CREATED_AT,
  PROP_KIND,
  PROP_SUBJECT,
  PROP_EXPIRES_AT,
  PROP_STATUS,
  PROP_RELAYS,
  PROP_WITHDRAWN,
  N_PROPS
};
static GParamSpec *props[N_PROPS];

G_DEFINE_FINAL_TYPE(GhMessage, gh_message, G_TYPE_OBJECT)

static gboolean
lower_hex64(const gchar *value)
{
  if (!value || strlen(value) != 64)
    return FALSE;
  for (const gchar *p = value; *p; p++)
    if (!g_ascii_isdigit(*p) && (*p < 'a' || *p > 'f'))
      return FALSE;
  return TRUE;
}

static gint
compare_strings(gconstpointer a, gconstpointer b)
{
  return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

static gboolean
invalid(GError **error, const gchar *reason)
{
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
              "Not a NIP-17 chat message: %s", reason);
  return FALSE;
}

/* Collects p recipients (unique, tag order) and the first subject tag. */
static gboolean
read_tags(GhMessage *self, const NostrEvent *rumor, GError **error)
{
  NostrTags *tags = nostr_event_get_tags(rumor);
  g_autoptr(GPtrArray) recipients = g_ptr_array_new_with_free_func(g_free);
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    const gchar *name = tag && nostr_tag_size(tag) >= 1 ? nostr_tag_get(tag, 0) : NULL;
    const gchar *value = tag && nostr_tag_size(tag) >= 2 ? nostr_tag_get(tag, 1) : NULL;
    if (g_strcmp0(name, "subject") == 0) {
      if (!self->subject)
        self->subject = g_strdup(value ? value : "");
      continue;
    }
    /* Local only (W33): a NIP-04 message stored as a kind-14 rumor. Never
     * published: Groundhog writes it on the rumors it makes from kind 4. */
    if (g_strcmp0(name, GH_MESSAGE_LEGACY_TAG) == 0) {
      self->legacy_nip04 = g_strcmp0(value, "nip04") == 0;
      continue;
    }
    if (g_strcmp0(name, "expiration") == 0) {
      gint64 expires_at = 0;
      if (!self->expires_at && value &&
          g_ascii_string_to_signed(value, 10, 1, G_MAXINT64, &expires_at, NULL))
        self->expires_at = expires_at;
      continue;
    }
    if (g_strcmp0(name, "p") != 0)
      continue;
    if (!lower_hex64(value))
      return invalid(error, "a p tag is not a lowercase hex pubkey");
    gboolean seen = FALSE;
    for (guint j = 0; j < recipients->len && !seen; j++)
      seen = g_str_equal(g_ptr_array_index(recipients, j), value);
    if (seen)
      continue;
    if (recipients->len >= GH_MESSAGE_MAX_RECIPIENTS)
      return invalid(error, "too many recipients");
    g_ptr_array_add(recipients, g_strdup(value));
  }
  if (recipients->len == 0)
    return invalid(error, "no recipient");

  /* Room members: author plus recipients, sorted and unique. */
  g_autoptr(GPtrArray) members = g_ptr_array_new();
  g_ptr_array_add(members, self->sender);
  for (guint i = 0; i < recipients->len; i++)
    if (!g_str_equal(g_ptr_array_index(recipients, i), self->sender))
      g_ptr_array_add(members, g_ptr_array_index(recipients, i));
  g_ptr_array_sort(members, compare_strings);
  self->participants = g_new0(gchar *, members->len + 1);
  for (guint i = 0; i < members->len; i++)
    self->participants[i] = g_strdup(g_ptr_array_index(members, i));
  self->room_id = g_strjoinv(",", self->participants);
  g_ptr_array_add(recipients, NULL);
  self->recipients = (GStrv)g_ptr_array_free(g_steal_pointer(&recipients), FALSE);
  return TRUE;
}

GhMessage *
gh_message_new_from_rumor(const gchar *account_pubkey, const gchar *rumor_json,
                          GError **error)
{
  if (!lower_hex64(account_pubkey)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "A lowercase hex account pubkey is required");
    return NULL;
  }
  if (!rumor_json || strnlen(rumor_json, GH_MESSAGE_MAX_RUMOR_JSON + 1) >
                       GH_MESSAGE_MAX_RUMOR_JSON) {
    invalid(error, "missing or too large");
    return NULL;
  }
  NostrEvent *rumor = nostr_event_new();
  if (!rumor) {
    invalid(error, "out of memory");
    return NULL;
  }
  gchar id[65] = { 0 };
  const gchar *reason = NULL;
  if (nostr_event_deserialize_unsigned(rumor, rumor_json, NULL) != NOSTR_EVENT_VALIDATION_OK)
    reason = "malformed";
  else if (rumor->sig)
    reason = "a rumor must not be signed";
  else if (nostr_event_get_kind(rumor) != 14 &&
           nostr_event_get_kind(rumor) != GH_NIP17_FILE_KIND &&
           nostr_event_get_kind(rumor) != GH_MESSAGE_MLS_POLL_KIND &&
           nostr_event_get_kind(rumor) != GH_MESSAGE_MLS_POLL_VOTE_KIND)
    reason = "not a supported private message kind";
  else if (nostr_event_get_created_at(rumor) <= 0)
    reason = "no created_at";
  else if (!lower_hex64(nostr_event_get_pubkey(rumor)))
    reason = "author is not a lowercase hex pubkey";
  else if (!nostr_event_get_content(rumor))
    reason = "no content";
  else if ((rumor->id ? nostr_event_validate_id(rumor, id)
                      : nostr_event_compute_id(rumor, id)) != NOSTR_EVENT_VALIDATION_OK)
    reason = "id does not match its content";
  if (reason) {
    nostr_event_free(rumor);
    invalid(error, reason);
    return NULL;
  }
  /* G21: a kind-15 file message only with complete, well-formed file tags
   * (gh-nip17-file.h); reading them fetches nothing. */
  if (nostr_event_get_kind(rumor) == GH_NIP17_FILE_KIND) {
    g_autoptr(GhNip17File) file = gh_nip17_file_from_rumor(rumor_json, NULL);
    if (!file) {
      nostr_event_free(rumor);
      invalid(error, "a kind-15 file message without valid file tags");
      return NULL;
    }
  }

  GhMessage *self = g_object_new(GH_TYPE_MESSAGE, NULL);
  self->kind = nostr_event_get_kind(rumor);
  self->account = g_strdup(account_pubkey);
  self->rumor_id = g_strdup(id);
  self->rumor_json = g_strdup(rumor_json);
  self->sender = g_strdup(nostr_event_get_pubkey(rumor));
  self->created_at = nostr_event_get_created_at(rumor);
  self->content = g_strdup(nostr_event_get_content(rumor));
  gboolean ok = read_tags(self, rumor, error);
  nostr_event_free(rumor);
  if (ok && !g_strv_contains((const gchar *const *)self->participants, self->account))
    ok = invalid(error, "the account is neither its author nor a recipient");
  if (!ok) {
    g_object_unref(self);
    return NULL;
  }
  return self;
}

const gchar *
gh_message_get_account(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), NULL);
  return self->account;
}

const gchar *
gh_message_get_rumor_id(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), NULL);
  return self->rumor_id;
}

const gchar *
gh_message_get_sender(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), NULL);
  return self->sender;
}

const gchar *const *
gh_message_get_recipients(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), NULL);
  return (const gchar *const *)self->recipients;
}

const gchar *const *
gh_message_get_participants(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), NULL);
  return (const gchar *const *)self->participants;
}

const gchar *
gh_message_get_room_id(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), NULL);
  return self->room_id;
}

gint64
gh_message_get_created_at(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), 0);
  return self->created_at;
}

const gchar *
gh_message_get_content(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), NULL);
  return self->content;
}

gboolean
gh_message_is_self(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), FALSE);
  return g_strcmp0(self->sender, self->account) == 0;
}

const gchar *
gh_message_get_subject(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), NULL);
  return self->subject;
}

gint
gh_message_get_kind(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), 0);
  return self->kind;
}

gchar *
gh_message_dup_display_text(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), NULL);
  g_autoptr(GhNip17File) file = gh_message_dup_file(self);
  if (file)
    /* TRANSLATORS: an encrypted file received or sent, before it is opened. */
    return g_strdup(gh_nip17_file_is_image(file) ? _("Photo") : _("File"));
  if (self->withdrawn)
    return g_strdup(gh_message_withdrawn_text());
  /* An encrypted group's files with no caption (W25): never a URL. */
  if ((!self->content || !*self->content) && self->attachments && self->attachments->len) {
    const GhMessageAttachment *first = g_ptr_array_index(self->attachments, 0);
    gboolean photo = first->media_type && g_str_has_prefix(first->media_type, "image/");
    guint n = self->attachments->len;
    if (n == 1)
      return g_strdup(photo ? _("Photo") : _("File"));
    /* TRANSLATORS: several encrypted files in one message, before they are opened. */
    return g_strdup_printf(g_dngettext(NULL, "%u file", "%u files", n), n);
  }
  /* An MLS poll shows "Poll: <question>" (charter: never the raw content
   * alone, so the list and notification say what it is); a vote is silent. */
  if (self->kind == GH_MESSAGE_MLS_POLL_KIND)
    /* TRANSLATORS: a poll in an encrypted group. %s is the question. */
    return g_strdup_printf(_("Poll: %s"), self->content ? self->content : "");
  if (self->kind == GH_MESSAGE_MLS_POLL_VOTE_KIND)
    /* TRANSLATORS: a vote on a poll in an encrypted group. */
    return g_strdup(_("Voted on a poll"));
  return g_strdup(self->content);
}

void
gh_message_attachment_free(GhMessageAttachment *attachment)
{
  if (!attachment)
    return;
  g_free(attachment->media_type);
  g_free(attachment->filename);
  g_free(attachment->file_id);
  g_free(attachment);
}

void
gh_message_set_attachments(GhMessage *self, GPtrArray *attachments, guint rejected)
{
  g_return_if_fail(GH_IS_MESSAGE(self) && self->mls);
  g_clear_pointer(&self->attachments, g_ptr_array_unref);
  if (attachments && attachments->len)
    self->attachments = g_ptr_array_ref(attachments);
  self->rejected_attachments = rejected;
}

guint
gh_message_get_n_attachments(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), 0);
  return self->attachments ? self->attachments->len : 0;
}

const GhMessageAttachment *
gh_message_get_attachment(GhMessage *self, guint index)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), NULL);
  return self->attachments && index < self->attachments->len
           ? g_ptr_array_index(self->attachments, index) : NULL;
}

guint
gh_message_get_rejected_attachments(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), 0);
  return self->rejected_attachments;
}

const gchar *
gh_message_withdrawn_text(void)
{
  /* TRANSLATORS: in place of an encrypted-group message the group withdrew
   * when two members' changes conflicted: other members never saw it. */
  return _("This message was withdrawn when the group resolved a conflict");
}

gboolean
gh_message_get_withdrawn(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), FALSE);
  return self->withdrawn;
}

void
gh_message_set_withdrawn(GhMessage *self, gboolean withdrawn)
{
  g_return_if_fail(GH_IS_MESSAGE(self));
  withdrawn = !!withdrawn;
  if (!self->mls || self->withdrawn == withdrawn)
    return;
  self->withdrawn = withdrawn;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_WITHDRAWN]);
}

GhNip17File *
gh_message_dup_file(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), NULL);
  if (self->nip29 || self->mls || self->kind != GH_NIP17_FILE_KIND)
    return NULL;
  return gh_nip17_file_from_rumor(self->rumor_json, NULL);
}

const gchar *
gh_message_get_rumor_json(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), NULL);
  return self->rumor_json;
}

gint64
gh_message_get_expires_at(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), 0);
  return self->expires_at;
}

void
gh_message_set_expires_at(GhMessage *self, gint64 expires_at)
{
  g_return_if_fail(GH_IS_MESSAGE(self));
  if (self->expires_at != 0 || expires_at <= 0)
    return;
  self->expires_at = expires_at;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_EXPIRES_AT]);
}

guint64
gh_message_get_seq(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), 0);
  return self->seq;
}

void
gh_message_set_seq(GhMessage *self, guint64 seq)
{
  g_return_if_fail(GH_IS_MESSAGE(self));
  self->seq = seq;
}

GhMessageStatus
gh_message_get_status(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), GH_MESSAGE_STATUS_NONE);
  return self->status;
}

void
gh_message_set_status(GhMessage *self, GhMessageStatus status)
{
  g_return_if_fail(GH_IS_MESSAGE(self));
  if (!gh_message_is_self(self) || self->status == status)
    return;
  self->status = status;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_STATUS]);
}

const gchar *const *
gh_message_get_relays(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), NULL);
  return (const gchar *const *)self->relays->pdata;
}

gboolean
gh_message_add_relay(GhMessage *self, const gchar *url)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), FALSE);
  if (!url || !*url || g_strv_contains(gh_message_get_relays(self), url))
    return FALSE;
  g_ptr_array_insert(self->relays, self->relays->len - 1, g_strdup(url));
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_RELAYS]);
  return TRUE;
}

/* ---- NIP-29 group events ------------------------------------------------ */

static gboolean
nip29_group_id_valid(const gchar *id)
{
  gsize length = id ? strnlen(id, GH_MESSAGE_MAX_GROUP_ID + 1) : 0;
  if (length == 0 || length > GH_MESSAGE_MAX_GROUP_ID)
    return FALSE;
  for (const gchar *p = id; *p; p++)
    if (!g_ascii_islower(*p) && !g_ascii_isdigit(*p) && *p != '-' && *p != '_')
      return FALSE;
  return TRUE;
}

/* A normalized ws(s) relay URL as gh_nip29_normalize_relay_url() makes it:
 * the scheme, a host, and no whitespace, control character or separator. */
static gboolean
nip29_relay_valid(const gchar *url)
{
  gsize length = url ? strnlen(url, 2049) : 0;
  if (length == 0 || length > 2048 ||
      !(g_str_has_prefix(url, "ws://") || g_str_has_prefix(url, "wss://")))
    return FALSE;
  const gchar *host = strstr(url, "://") + 3;
  if (!*host || *host == '/')
    return FALSE;
  for (const gchar *p = url; *p; p++)
    if ((guchar)*p <= 0x20 || *p == 0x7f)
      return FALSE;
  return TRUE;
}

gchar *
gh_message_nip29_room_id(const gchar *relay_url, const gchar *group_id)
{
  g_return_val_if_fail(relay_url != NULL && group_id != NULL, NULL);
  return g_strconcat(relay_url, GH_MESSAGE_NIP29_SEPARATOR, group_id, NULL);
}

gboolean
gh_message_nip29_room_split(const gchar *room_id, gchar **relay_url, gchar **group_id)
{
  const gchar *separator = room_id ? strchr(room_id, '\x1f') : NULL;
  if (!separator || strchr(separator + 1, '\x1f') || separator == room_id)
    return FALSE;
  g_autofree gchar *relay = g_strndup(room_id, separator - room_id);
  if (!nip29_relay_valid(relay) || !nip29_group_id_valid(separator + 1))
    return FALSE;
  if (relay_url)
    *relay_url = g_steal_pointer(&relay);
  if (group_id)
    *group_id = g_strdup(separator + 1);
  return TRUE;
}

static const gchar *
first_tag_value(const NostrEvent *event, const gchar *key, gboolean *present)
{
  NostrTags *tags = nostr_event_get_tags((NostrEvent *)event);
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (tag && nostr_tag_size(tag) >= 1 && g_strcmp0(nostr_tag_get(tag, 0), key) == 0) {
      if (present)
        *present = TRUE;
      return nostr_tag_size(tag) >= 2 ? nostr_tag_get(tag, 1) : NULL;
    }
  }
  if (present)
    *present = FALSE;
  return NULL;
}

/* nostrc-zjkv: extract the reply-target event id from the tags.
 * Priority: an e tag with marker "reply", then a q tag (quote), then the
 * last bare e tag (deprecated NIP-10 convention). Returns a newly allocated
 * lowercase hex id, or NULL when the event is not a reply/quote. */
static gchar *
read_reply_id(const NostrEvent *event)
{
  NostrTags *tags = nostr_event_get_tags((NostrEvent *)event);
  if (!tags)
    return NULL;
  const gchar *marked_reply = NULL;
  const gchar *quote = NULL;
  const gchar *last_e = NULL;
  for (size_t i = 0; i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (!tag || nostr_tag_size(tag) < 2)
      continue;
    const gchar *name = nostr_tag_get(tag, 0);
    const gchar *value = nostr_tag_get(tag, 1);
    if (!value || !lower_hex64(value))
      continue;
    if (g_strcmp0(name, "e") == 0) {
      /* Only genuinely unmarked e tags serve as the positional fallback;
       * a tag with marker "mention" is an inline reference, not a reply. */
      if (nostr_tag_size(tag) < 4 || !nostr_tag_get(tag, 3) ||
          *nostr_tag_get(tag, 3) == '\0')
        last_e = value;
      if (nostr_tag_size(tag) >= 4 && g_strcmp0(nostr_tag_get(tag, 3), "reply") == 0)
        marked_reply = value;
    } else if (g_strcmp0(name, "q") == 0 && !quote) {
      quote = value;
    }
  }
  const gchar *best = marked_reply ? marked_reply : quote ? quote : last_e;
  return best ? g_strdup(best) : NULL;
}

GhMessage *
gh_message_new_from_nip29_event(const gchar *account_pubkey, const gchar *relay_url,
                                const gchar *event_json, GError **error)
{
  if (!lower_hex64(account_pubkey) || !nip29_relay_valid(relay_url)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "A lowercase hex account and a normalized relay URL are required");
    return NULL;
  }
  const gchar *reason = NULL;
  if (!event_json || strnlen(event_json, GH_MESSAGE_MAX_RUMOR_JSON + 1) >
                       GH_MESSAGE_MAX_RUMOR_JSON)
    reason = "missing or too large";
  NostrEvent *event = reason ? NULL : nostr_event_new();
  gchar id[65] = { 0 };
  gboolean is_signed = FALSE;
  if (!reason && !event)
    reason = "out of memory";
  if (!reason) {
    if (nostr_event_deserialize_signed(event, event_json, NULL) == NOSTR_EVENT_VALIDATION_OK) {
      is_signed = TRUE;
      if (nostr_event_validate(event, id) != NOSTR_EVENT_VALIDATION_OK)
        reason = "the id or signature does not verify";
    } else {
      /* Only the account's own event may be unsigned: its local echo. */
      nostr_event_free(event);
      event = nostr_event_new();
      if (nostr_event_deserialize_unsigned(event, event_json, NULL) != NOSTR_EVENT_VALIDATION_OK)
        reason = "malformed";
      else if (g_strcmp0(nostr_event_get_pubkey(event), account_pubkey) != 0)
        reason = "an unsigned event that is not the account's own";
      else if ((event->id ? nostr_event_validate_id(event, id)
                          : nostr_event_compute_id(event, id)) != NOSTR_EVENT_VALIDATION_OK)
        reason = "id does not match its content";
    }
  }
  gint kind = event && !reason ? nostr_event_get_kind(event) : 0;
  const gchar *group_id = NULL;
  if (!reason && (kind < 9 || kind > 12))
    reason = "not a group message kind (9-12)";
  if (!reason && nostr_event_get_created_at(event) <= 0)
    reason = "no created_at";
  if (!reason && !lower_hex64(nostr_event_get_pubkey(event)))
    reason = "author is not a lowercase hex pubkey";
  if (!reason && !nostr_event_get_content(event))
    reason = "no content";
  if (!reason) {
    group_id = first_tag_value(event, "h", NULL);
    if (!nip29_group_id_valid(group_id))
      reason = "no valid h tag";
  }
  if (reason) {
    nostr_event_free(event);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Not a NIP-29 group message: %s", reason);
    return NULL;
  }

  GhMessage *self = g_object_new(GH_TYPE_MESSAGE, NULL);
  self->nip29 = TRUE;
  self->is_signed = is_signed;
  /* NIP-29 relays accept kind-9 group chat messages. A poll's NIP-88
   * definition/response tags live in that envelope with a poll marker. */
  const gchar *poll_kind = kind == 9 ? first_tag_value(event, "poll", NULL) : NULL;
  self->kind = g_strcmp0(poll_kind, "1068") == 0 ? GH_MESSAGE_MLS_POLL_KIND :
               g_strcmp0(poll_kind, "1018") == 0 ? GH_MESSAGE_MLS_POLL_VOTE_KIND : kind;
  self->account = g_strdup(account_pubkey);
  self->rumor_id = g_strdup(id);
  self->rumor_json = g_strdup(event_json);
  self->sender = g_strdup(nostr_event_get_pubkey(event));
  self->created_at = nostr_event_get_created_at(event);
  self->content = g_strdup(nostr_event_get_content(event));
  self->group_id = g_strdup(group_id);
  self->group_relay = g_strdup(relay_url);
  self->room_id = gh_message_nip29_room_id(relay_url, group_id);
  self->recipients = g_new0(gchar *, 1);
  self->participants = g_new0(gchar *, 2);
  self->participants[0] = g_strdup(account_pubkey);
  const gchar *expiration = first_tag_value(event, "expiration", NULL);
  gint64 expires_at = 0;
  if (expiration && g_ascii_string_to_signed(expiration, 10, 1, G_MAXINT64, &expires_at, NULL))
    self->expires_at = expires_at;
  self->reply_to_id = read_reply_id(event);
  nostr_event_free(event);
  return self;
}

gboolean
gh_message_is_nip29(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), FALSE);
  return self->nip29;
}

const gchar *
gh_message_get_group_id(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), NULL);
  return self->group_id;
}

const gchar *
gh_message_get_group_relay(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), NULL);
  return self->group_relay;
}

gboolean
gh_message_is_signed(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), FALSE);
  return self->is_signed;
}

/* ---- MLS group messages ------------------------------------------------- */

static gboolean
mls_group_hex_valid(const gchar *hex)
{
  gsize length = hex ? strnlen(hex, 2 * GH_MESSAGE_MAX_MLS_GROUP_ID + 1) : 0;
  if (length < 2 || length > 2 * GH_MESSAGE_MAX_MLS_GROUP_ID || length % 2)
    return FALSE;
  for (const gchar *p = hex; *p; p++)
    if (!g_ascii_isdigit(*p) && (*p < 'a' || *p > 'f'))
      return FALSE;
  return TRUE;
}

gchar *
gh_message_mls_room_id(const gchar *group_id_hex)
{
  if (!mls_group_hex_valid(group_id_hex))
    return NULL;
  return g_strconcat(GH_MESSAGE_MLS_ROOM_PREFIX, group_id_hex, NULL);
}

gboolean
gh_message_mls_room_split(const gchar *room_id, gchar **group_id_hex)
{
  if (!room_id || !g_str_has_prefix(room_id, GH_MESSAGE_MLS_ROOM_PREFIX))
    return FALSE;
  const gchar *hex = room_id + strlen(GH_MESSAGE_MLS_ROOM_PREFIX);
  if (!mls_group_hex_valid(hex))
    return FALSE;
  if (group_id_hex)
    *group_id_hex = g_strdup(hex);
  return TRUE;
}

GhMessage *
gh_message_new_from_mls(const gchar *account_pubkey, const gchar *group_id_hex,
                        const gchar *inner_event_json, GError **error)
{
  if (!lower_hex64(account_pubkey) || !mls_group_hex_valid(group_id_hex)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "A lowercase hex account and MLS group id are required");
    return NULL;
  }
  const gchar *reason = NULL;
  if (!inner_event_json || strnlen(inner_event_json, GH_MESSAGE_MAX_RUMOR_JSON + 1) >
                             GH_MESSAGE_MAX_RUMOR_JSON)
    reason = "missing or too large";
  NostrEvent *event = reason ? NULL : nostr_event_new();
  gchar id[65] = { 0 };
  if (!reason && !event)
    reason = "out of memory";
  if (!reason && nostr_event_deserialize_unsigned(event, inner_event_json, NULL) !=
                   NOSTR_EVENT_VALIDATION_OK)
    reason = "malformed";
  else if (!reason && event->sig)
    reason = "an inner event must not be signed";
  else if (!reason &&
           nostr_event_get_kind(event) != GH_MESSAGE_MLS_KIND &&
           nostr_event_get_kind(event) != GH_MESSAGE_MLS_POLL_KIND &&
           nostr_event_get_kind(event) != GH_MESSAGE_MLS_POLL_VOTE_KIND)
    reason = "unsupported inner event kind";
  else if (!reason && nostr_event_get_created_at(event) <= 0)
    reason = "no created_at";
  else if (!reason && !lower_hex64(nostr_event_get_pubkey(event)))
    reason = "author is not a lowercase hex pubkey";
  else if (!reason && !nostr_event_get_content(event))
    reason = "no content";
  else if (!reason && (event->id ? nostr_event_validate_id(event, id)
                                 : nostr_event_compute_id(event, id)) != NOSTR_EVENT_VALIDATION_OK)
    reason = "id does not match its content";
  if (reason) {
    if (event)
      nostr_event_free(event);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Not an encrypted group message: %s", reason);
    return NULL;
  }
  GhMessage *self = g_object_new(GH_TYPE_MESSAGE, NULL);
  self->mls = TRUE;
  self->kind = nostr_event_get_kind(event);
  self->account = g_strdup(account_pubkey);
  self->rumor_id = g_strdup(id);
  self->rumor_json = g_strdup(inner_event_json);
  self->sender = g_strdup(nostr_event_get_pubkey(event));
  self->created_at = nostr_event_get_created_at(event);
  self->content = g_strdup(nostr_event_get_content(event));
  self->group_id = g_strdup(group_id_hex);
  self->room_id = gh_message_mls_room_id(group_id_hex);
  self->recipients = g_new0(gchar *, 1);
  self->participants = g_new0(gchar *, 2);
  self->participants[0] = g_strdup(account_pubkey);
  const gchar *expiration = first_tag_value(event, "expiration", NULL);
  gint64 expires_at = 0;
  if (expiration && g_ascii_string_to_signed(expiration, 10, 1, G_MAXINT64, &expires_at, NULL))
    self->expires_at = expires_at;
  self->reply_to_id = read_reply_id(event);
  nostr_event_free(event);
  return self;
}

gboolean
gh_message_is_mls(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), FALSE);
  return self->mls;
}

void
gh_message_set_mls_epoch(GhMessage *self, guint64 source_epoch)
{
  g_return_if_fail(GH_IS_MESSAGE(self) && self->mls);
  self->mls_epoch = source_epoch;
  self->has_mls_epoch = TRUE;
}

gboolean
gh_message_get_mls_epoch(GhMessage *self, guint64 *out_source_epoch)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), FALSE);
  if (!self->mls || !self->has_mls_epoch)
    return FALSE;
  if (out_source_epoch)
    *out_source_epoch = self->mls_epoch;
  return TRUE;
}

const gchar *
gh_message_get_reply_to_id(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), NULL);
  return self->reply_to_id;
}

gint
gh_message_compare(GhMessage *a, GhMessage *b)
{
  if (a->created_at != b->created_at)
    return a->created_at < b->created_at ? -1 : 1;
  return strcmp(a->rumor_id, b->rumor_id);
}

static void
gh_message_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  GhMessage *self = GH_MESSAGE(object);
  switch (id) {
  case PROP_RUMOR_ID:
    g_value_set_string(value, self->rumor_id);
    break;
  case PROP_SENDER:
    g_value_set_string(value, self->sender);
    break;
  case PROP_BODY:
    g_value_set_string(value, self->content);
    break;
  case PROP_IS_OUTGOING:
    g_value_set_boolean(value, gh_message_is_self(self));
    break;
  case PROP_CREATED_AT:
    g_value_set_int64(value, self->created_at);
    break;
  case PROP_KIND:
    g_value_set_int(value, gh_message_get_kind(self));
    break;
  case PROP_SUBJECT:
    g_value_set_string(value, self->subject);
    break;
  case PROP_EXPIRES_AT:
    g_value_set_int64(value, self->expires_at);
    break;
  case PROP_STATUS:
    g_value_set_enum(value, self->status);
    break;
  case PROP_WITHDRAWN:
    g_value_set_boolean(value, self->withdrawn);
    break;
  case PROP_RELAYS:
    g_value_set_boxed(value, gh_message_get_relays(self));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gh_message_finalize(GObject *object)
{
  GhMessage *self = GH_MESSAGE(object);
  g_free(self->account);
  g_free(self->rumor_id);
  g_free(self->rumor_json);
  g_free(self->sender);
  g_strfreev(self->recipients);
  g_strfreev(self->participants);
  g_free(self->room_id);
  g_free(self->content);
  g_free(self->subject);
  g_free(self->group_id);
  g_free(self->group_relay);
  g_free(self->reply_to_id);
  g_clear_pointer(&self->attachments, g_ptr_array_unref);
  g_ptr_array_unref(self->relays);
  G_OBJECT_CLASS(gh_message_parent_class)->finalize(object);
}

static void
gh_message_class_init(GhMessageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->get_property = gh_message_get_property;
  object_class->finalize = gh_message_finalize;
  const GParamFlags ro = G_PARAM_READABLE | G_PARAM_STATIC_STRINGS;
  props[PROP_RUMOR_ID] = g_param_spec_string("rumor-id", NULL, NULL, NULL, ro);
  props[PROP_SENDER] = g_param_spec_string("sender", NULL, NULL, NULL, ro);
  props[PROP_BODY] = g_param_spec_string("body", NULL, NULL, NULL, ro);
  props[PROP_IS_OUTGOING] = g_param_spec_boolean("is-outgoing", NULL, NULL, FALSE, ro);
  props[PROP_CREATED_AT] = g_param_spec_int64("created-at", NULL, NULL,
                                              0, G_MAXINT64, 0, ro);
  props[PROP_KIND] = g_param_spec_int("kind", NULL, NULL, 0, G_MAXINT, 14, ro);
  props[PROP_SUBJECT] = g_param_spec_string("subject", NULL, NULL, NULL, ro);
  props[PROP_EXPIRES_AT] = g_param_spec_int64("expires-at", NULL, NULL,
                                              0, G_MAXINT64, 0,
                                              ro | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_STATUS] = g_param_spec_enum("status", NULL, NULL, GH_TYPE_MESSAGE_STATUS,
                                         GH_MESSAGE_STATUS_NONE,
                                         ro | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_RELAYS] = g_param_spec_boxed("relays", NULL, NULL, G_TYPE_STRV,
                                          ro | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_WITHDRAWN] = g_param_spec_boolean("withdrawn", NULL, NULL, FALSE,
                                               ro | G_PARAM_EXPLICIT_NOTIFY);
  g_object_class_install_properties(object_class, N_PROPS, props);
}

static void
gh_message_init(GhMessage *self)
{
  self->status = GH_MESSAGE_STATUS_NONE;
  self->kind = 14;
  self->relays = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(self->relays, NULL);
}

gboolean
gh_message_get_legacy_nip04(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), FALSE);
  return self->legacy_nip04;
}
