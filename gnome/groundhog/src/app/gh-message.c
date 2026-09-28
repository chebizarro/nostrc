#include "gh-message.h"

#include <nostr-event.h>
#include <nostr-tag.h>
#include <stdlib.h>
#include <string.h>

struct _GhMessage {
  GObject parent_instance;
  gchar *account;
  gchar *rumor_id;
  gchar *sender;
  GStrv recipients;
  GStrv participants;
  gchar *room_id;
  gint64 created_at;
  gchar *content;
  gchar *subject;
  gint64 expires_at;
  GhMessageStatus status;
  GPtrArray *relays; /* NULL-terminated */
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
  N_PROPS
};
static GParamSpec *props[N_PROPS];

G_DEFINE_FINAL_TYPE(GhMessage, gh_message, G_TYPE_OBJECT)

GType
gh_message_status_get_type(void)
{
  static gsize type = 0;
  if (g_once_init_enter(&type)) {
    static const GEnumValue values[] = {
      { GH_MESSAGE_STATUS_NONE, "GH_MESSAGE_STATUS_NONE", "none" },
      { GH_MESSAGE_STATUS_WAITING_FOR_SIGNER, "GH_MESSAGE_STATUS_WAITING_FOR_SIGNER",
        "waiting-for-signer" },
      { GH_MESSAGE_STATUS_QUEUED_OFFLINE, "GH_MESSAGE_STATUS_QUEUED_OFFLINE", "queued-offline" },
      { GH_MESSAGE_STATUS_SENDING, "GH_MESSAGE_STATUS_SENDING", "sending" },
      { GH_MESSAGE_STATUS_SENT, "GH_MESSAGE_STATUS_SENT", "sent" },
      { GH_MESSAGE_STATUS_PARTIALLY_SENT, "GH_MESSAGE_STATUS_PARTIALLY_SENT", "partially-sent" },
      { GH_MESSAGE_STATUS_RETRYING, "GH_MESSAGE_STATUS_RETRYING", "retrying" },
      { GH_MESSAGE_STATUS_NOT_SENT, "GH_MESSAGE_STATUS_NOT_SENT", "not-sent" },
      { GH_MESSAGE_STATUS_CANNOT_SEND_NO_INBOX, "GH_MESSAGE_STATUS_CANNOT_SEND_NO_INBOX",
        "cannot-send-no-inbox" },
      { 0, NULL, NULL }
    };
    g_once_init_leave(&type, g_enum_register_static(
      g_intern_static_string("GhMessageStatus"), values));
  }
  return type;
}

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
  else if (nostr_event_get_kind(rumor) != 14)
    reason = "not kind 14";
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

  GhMessage *self = g_object_new(GH_TYPE_MESSAGE, NULL);
  self->account = g_strdup(account_pubkey);
  self->rumor_id = g_strdup(id);
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
  return 14;
}

gint64
gh_message_get_expires_at(GhMessage *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE(self), 0);
  return self->expires_at;
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
  g_free(self->sender);
  g_strfreev(self->recipients);
  g_strfreev(self->participants);
  g_free(self->room_id);
  g_free(self->content);
  g_free(self->subject);
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
                                              0, G_MAXINT64, 0, ro);
  props[PROP_STATUS] = g_param_spec_enum("status", NULL, NULL, GH_TYPE_MESSAGE_STATUS,
                                         GH_MESSAGE_STATUS_NONE,
                                         ro | G_PARAM_EXPLICIT_NOTIFY);
  props[PROP_RELAYS] = g_param_spec_boxed("relays", NULL, NULL, G_TYPE_STRV,
                                          ro | G_PARAM_EXPLICIT_NOTIFY);
  g_object_class_install_properties(object_class, N_PROPS, props);
}

static void
gh_message_init(GhMessage *self)
{
  self->relays = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(self->relays, NULL);
}
