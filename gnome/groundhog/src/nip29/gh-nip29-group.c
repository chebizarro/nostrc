#include "gh-nip29-group.h"

#include <nostr-kinds.h>
#include <stdlib.h>
#include <string.h>

G_DEFINE_QUARK(gh-nip29-error-quark, gh_nip29_error)

enum {
  SNAPSHOT_METADATA,
  SNAPSHOT_ADMINS,
  SNAPSHOT_MEMBERS,
  SNAPSHOT_ROLES,
  SNAPSHOT_COUNT
};

typedef struct {
  gboolean present;
  gint64 created_at;
  gchar id[65];
} GhNip29Snapshot;

struct _GhNip29GroupKey {
  gchar *relay_url;
  gchar *group_id;
};

struct _GhNip29Group {
  GhNip29GroupKey *key;
  gchar *relay_pubkey;
  /* Parsed snapshot fields. nips/nip29 owns the tag parsing; this layer owns
   * provenance (signature, relay key, group id) and replacement order. */
  nostr_group_t *state;
  GhNip29Snapshot snapshots[SNAPSHOT_COUNT];
  /* kind:39000 fields nips/nip29 does not model. */
  gboolean has_livekit;
  gboolean has_supported_kinds;
  GArray *supported_kinds;
};

struct _GhNip29RolePolicy {
  GHashTable *roles; /* role name -> permission bit mask */
};

gboolean
gh_nip29_is_hex64(const gchar *value)
{
  if (!value)
    return FALSE;
  for (gsize i = 0; i < 64; i++) {
    gchar c = value[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return FALSE;
  }
  return value[64] == '\0';
}

/* RFC 3986 6.2.2: escaped unreserved characters are decoded and remaining
 * percent-triplets use uppercase hex, so equivalent paths compare equal. */
static gboolean
append_normalized_path(GString *out, const gchar *path)
{
  for (const gchar *p = path; *p; p++) {
    if (*p != '%') {
      g_string_append_c(out, *p);
      continue;
    }
    gint hi = g_ascii_xdigit_value(p[1]);
    gint lo = hi < 0 ? -1 : g_ascii_xdigit_value(p[2]);
    if (hi < 0 || lo < 0)
      return FALSE;
    gchar c = (gchar)((hi << 4) | lo);
    if (g_ascii_isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~')
      g_string_append_c(out, c);
    else
      g_string_append_printf(out, "%%%02X", (guint)(guchar)c);
    p += 2;
  }
  return TRUE;
}

gchar *
gh_nip29_normalize_relay_url(const gchar *url, GError **error)
{
  g_autofree gchar *trimmed = url ? g_strstrip(g_strdup(url)) : NULL;
  g_autoptr(GUri) uri = trimmed ? g_uri_parse(trimmed, G_URI_FLAGS_ENCODED, NULL) : NULL;
  const gchar *scheme = uri ? g_uri_get_scheme(uri) : NULL;
  const gchar *host = uri ? g_uri_get_host(uri) : NULL;
  gboolean secure = g_strcmp0(scheme, "wss") == 0;
  if (!uri || (!secure && g_strcmp0(scheme, "ws") != 0) || !host || !*host) {
    g_set_error_literal(error, GH_NIP29_ERROR, GH_NIP29_ERROR_INVALID_RELAY_URL,
                        "relay URL must be ws:// or wss:// with a host");
    return NULL;
  }
  if (g_uri_get_userinfo(uri) || g_uri_get_query(uri) || g_uri_get_fragment(uri)) {
    g_set_error_literal(error, GH_NIP29_ERROR, GH_NIP29_ERROR_INVALID_RELAY_URL,
                        "relay URL must not carry credentials, a query or a fragment");
    return NULL;
  }
  gint port = g_uri_get_port(uri);
  if (port == 0) {
    g_set_error_literal(error, GH_NIP29_ERROR, GH_NIP29_ERROR_INVALID_RELAY_URL,
                        "relay URL port must be non-zero");
    return NULL;
  }
  if (port == (secure ? 443 : 80))
    port = -1;

  /* IDN hosts compare in their ASCII (punycode) form; IPv6 literals as-is. */
  gboolean ipv6 = strchr(host, ':') != NULL;
  g_autofree gchar *ascii_host = ipv6 ? g_strdup(host) : g_hostname_to_ascii(host);
  g_autoptr(GString) path = g_string_new(NULL);
  if (!ascii_host || !append_normalized_path(path, g_uri_get_path(uri))) {
    g_set_error_literal(error, GH_NIP29_ERROR, GH_NIP29_ERROR_INVALID_RELAY_URL,
                        "relay URL host or path is not valid");
    return NULL;
  }
  while (path->len > 0 && path->str[path->len - 1] == '/')
    g_string_truncate(path, path->len - 1);

  g_autofree gchar *lower_host = g_ascii_strdown(ascii_host, -1);
  GString *out = g_string_new(scheme);
  g_string_append(out, "://");
  if (ipv6)
    g_string_append_printf(out, "[%s]", lower_host);
  else
    g_string_append(out, lower_host);
  if (port > 0)
    g_string_append_printf(out, ":%d", port);
  g_string_append_len(out, path->str, (gssize)path->len);
  return g_string_free(out, FALSE);
}

/* ---- Identity ------------------------------------------------------------ */

static gboolean
group_id_is_supported(const gchar *relay_url, const gchar *group_id)
{
  if (!group_id)
    return FALSE;
  nostr_group_address_t address = {
    .relay = (char *)relay_url,
    .id = (char *)group_id,
  };
  return nostr_group_address_is_valid(&address);
}

GhNip29GroupKey *
gh_nip29_group_key_new(const gchar *relay_url, const gchar *group_id, GError **error)
{
  g_autofree gchar *normalized = gh_nip29_normalize_relay_url(relay_url, error);
  if (!normalized)
    return NULL;
  if (!group_id_is_supported(normalized, group_id)) {
    g_set_error_literal(error, GH_NIP29_ERROR, GH_NIP29_ERROR_INVALID_GROUP_ID,
                        "group id must be non-empty and use only a-z, 0-9, '-' or '_'");
    return NULL;
  }
  GhNip29GroupKey *key = g_new0(GhNip29GroupKey, 1);
  key->relay_url = g_steal_pointer(&normalized);
  key->group_id = g_strdup(group_id);
  return key;
}

GhNip29GroupKey *
gh_nip29_group_key_copy(const GhNip29GroupKey *key)
{
  g_return_val_if_fail(key != NULL, NULL);
  GhNip29GroupKey *copy = g_new0(GhNip29GroupKey, 1);
  copy->relay_url = g_strdup(key->relay_url);
  copy->group_id = g_strdup(key->group_id);
  return copy;
}

void
gh_nip29_group_key_free(GhNip29GroupKey *key)
{
  if (!key)
    return;
  g_free(key->relay_url);
  g_free(key->group_id);
  g_free(key);
}

const gchar *
gh_nip29_group_key_get_relay_url(const GhNip29GroupKey *key)
{
  g_return_val_if_fail(key != NULL, NULL);
  return key->relay_url;
}

const gchar *
gh_nip29_group_key_get_group_id(const GhNip29GroupKey *key)
{
  g_return_val_if_fail(key != NULL, NULL);
  return key->group_id;
}

guint
gh_nip29_group_key_hash(gconstpointer key)
{
  const GhNip29GroupKey *k = key;
  return g_str_hash(k->relay_url) * 31u + g_str_hash(k->group_id);
}

gboolean
gh_nip29_group_key_equal(gconstpointer a, gconstpointer b)
{
  const GhNip29GroupKey *ka = a;
  const GhNip29GroupKey *kb = b;
  return g_str_equal(ka->relay_url, kb->relay_url) &&
         g_str_equal(ka->group_id, kb->group_id);
}

/* ---- Group state --------------------------------------------------------- */

GhNip29Group *
gh_nip29_group_new(const GhNip29GroupKey *key, const gchar *relay_pubkey, GError **error)
{
  g_return_val_if_fail(key != NULL, NULL);
  g_autofree gchar *pubkey = relay_pubkey ? g_ascii_strdown(relay_pubkey, -1) : NULL;
  if (!gh_nip29_is_hex64(pubkey)) {
    g_set_error_literal(error, GH_NIP29_ERROR, GH_NIP29_ERROR_INVALID_PUBKEY,
                        "relay pubkey must be 64 hexadecimal characters");
    return NULL;
  }

  /* nostr_new_group() only accepts the legacy "host'id" string; build the
   * public struct directly so any normalized URL is representable. The
   * library releases these fields with free(). */
  nostr_group_t *state = calloc(1, sizeof(*state));
  if (state) {
    state->address.relay = strdup(key->relay_url);
    state->address.id = strdup(key->group_id);
  }
  if (!state || !state->address.relay || !state->address.id) {
    nostr_free_group(state);
    g_set_error_literal(error, GH_NIP29_ERROR, GH_NIP29_ERROR_FAILED,
                        "could not allocate NIP-29 group state");
    return NULL;
  }

  GhNip29Group *group = g_new0(GhNip29Group, 1);
  group->key = gh_nip29_group_key_copy(key);
  group->relay_pubkey = g_steal_pointer(&pubkey);
  group->state = state;
  group->supported_kinds = g_array_new(FALSE, FALSE, sizeof(gint));
  return group;
}

void
gh_nip29_group_free(GhNip29Group *group)
{
  if (!group)
    return;
  gh_nip29_group_key_free(group->key);
  g_free(group->relay_pubkey);
  nostr_free_group(group->state);
  g_array_unref(group->supported_kinds);
  g_free(group);
}

const GhNip29GroupKey *
gh_nip29_group_get_key(const GhNip29Group *group)
{
  g_return_val_if_fail(group != NULL, NULL);
  return group->key;
}

const gchar *
gh_nip29_group_get_relay_pubkey(const GhNip29Group *group)
{
  g_return_val_if_fail(group != NULL, NULL);
  return group->relay_pubkey;
}

static gint
snapshot_slot(gint kind)
{
  switch (kind) {
  case NOSTR_KIND_SIMPLE_GROUP_METADATA:
    return SNAPSHOT_METADATA;
  case NOSTR_KIND_SIMPLE_GROUP_ADMINS:
    return SNAPSHOT_ADMINS;
  case NOSTR_KIND_SIMPLE_GROUP_MEMBERS:
    return SNAPSHOT_MEMBERS;
  case NOSTR_KIND_SIMPLE_GROUP_ROLES:
    return SNAPSHOT_ROLES;
  default:
    return -1;
  }
}

/* First tag named @name; NIP-01 addressable events use the first `d`, which
 * is also what nips/nip29's merge helpers match. */
static const NostrTag *
first_tag(const NostrEvent *event, const gchar *name)
{
  const NostrTags *tags = nostr_event_get_tags(event);
  for (gsize i = 0; tags && i < nostr_tags_size(tags); i++) {
    const NostrTag *tag = nostr_tags_get(tags, i);
    const gchar *key = nostr_tag_get_key(tag);
    if (key && strcmp(key, name) == 0)
      return tag;
  }
  return NULL;
}

static const gchar *
first_tag_value(const NostrEvent *event, const gchar *name)
{
  const NostrTag *tag = first_tag(event, name);
  return tag && nostr_tag_size(tag) > 1 ? nostr_tag_get(tag, 1) : NULL;
}

static gboolean
parse_kind(const gchar *text, gint *out)
{
  guint64 value = 0;
  if (!text || !g_ascii_string_to_unsigned(text, 10, 0, 65535, &value, NULL))
    return FALSE;
  *out = (gint)value;
  return TRUE;
}

/* Returns whether a supported_kinds tag is present. Unparseable entries are
 * skipped, which only narrows what the UI offers. */
static gboolean
parse_supported_kinds(const NostrEvent *event, GArray *out)
{
  const NostrTag *tag = first_tag(event, "supported_kinds");
  if (!tag)
    return FALSE;
  for (gsize i = 1; i < nostr_tag_size(tag); i++) {
    gint kind = 0;
    if (!parse_kind(nostr_tag_get(tag, i), &kind))
      continue;
    gboolean seen = FALSE;
    for (guint j = 0; j < out->len && !seen; j++)
      seen = g_array_index(out, gint, j) == kind;
    if (!seen)
      g_array_append_val(out, kind);
  }
  return TRUE;
}

static gboolean
merge_snapshot(GhNip29Group *group, gint slot, const NostrEvent *event)
{
  switch (slot) {
  case SNAPSHOT_METADATA: {
    GArray *kinds = g_array_new(FALSE, FALSE, sizeof(gint));
    gboolean has_kinds = parse_supported_kinds(event, kinds);
    if (!nostr_group_merge_in_metadata_event(group->state, event)) {
      g_array_unref(kinds);
      return FALSE;
    }
    group->has_livekit = first_tag(event, "livekit") != NULL;
    group->has_supported_kinds = has_kinds;
    g_array_unref(group->supported_kinds);
    group->supported_kinds = kinds;
    return TRUE;
  }
  case SNAPSHOT_ADMINS:
    return nostr_group_merge_in_admins_event(group->state, event);
  case SNAPSHOT_MEMBERS:
    return nostr_group_merge_in_members_event(group->state, event);
  case SNAPSHOT_ROLES:
    return nostr_group_merge_in_roles_event(group->state, event);
  default:
    return FALSE;
  }
}

GhNip29Admission
gh_nip29_group_admit(GhNip29Group *group, const NostrEvent *event)
{
  g_return_val_if_fail(group != NULL, GH_NIP29_ADMISSION_FAILED);
  if (!event)
    return GH_NIP29_ADMISSION_MALFORMED;
  gint slot = snapshot_slot(nostr_event_get_kind(event));
  if (slot < 0)
    return GH_NIP29_ADMISSION_UNSUPPORTED_KIND;
  if (!event->id || !*event->id || !event->sig || !*event->sig)
    return GH_NIP29_ADMISSION_UNSIGNED;

  gchar canonical_id[65];
  switch (nostr_event_validate(event, canonical_id)) {
  case NOSTR_EVENT_VALIDATION_OK:
    break;
  case NOSTR_EVENT_VALIDATION_CANONICAL_ID_MISMATCH:
  case NOSTR_EVENT_VALIDATION_SIGNATURE_INVALID:
    return GH_NIP29_ADMISSION_FORGED;
  default:
    return GH_NIP29_ADMISSION_MALFORMED;
  }
  if (g_strcmp0(nostr_event_get_pubkey(event), group->relay_pubkey) != 0)
    return GH_NIP29_ADMISSION_WRONG_AUTHOR;
  if (g_strcmp0(first_tag_value(event, "d"), group->key->group_id) != 0)
    return GH_NIP29_ADMISSION_WRONG_GROUP;
  gint64 created_at = nostr_event_get_created_at(event);
  if (created_at < 0)
    return GH_NIP29_ADMISSION_MALFORMED;

  /* Newest created_at wins per kind; on a tie NIP-01 keeps the lowest id.
   * nips/nip29 accepts equal timestamps, so the order is enforced here. */
  GhNip29Snapshot *current = &group->snapshots[slot];
  if (current->present) {
    if (created_at < current->created_at)
      return GH_NIP29_ADMISSION_STALE;
    if (created_at == current->created_at) {
      gint order = strcmp(canonical_id, current->id);
      if (order == 0)
        return GH_NIP29_ADMISSION_DUPLICATE;
      if (order > 0)
        return GH_NIP29_ADMISSION_STALE;
    }
  }

  if (!merge_snapshot(group, slot, event))
    return GH_NIP29_ADMISSION_FAILED;
  current->present = TRUE;
  current->created_at = created_at;
  memcpy(current->id, canonical_id, sizeof(current->id));
  return GH_NIP29_ADMISSION_ACCEPTED;
}

const gchar *
gh_nip29_admission_to_string(GhNip29Admission admission)
{
  switch (admission) {
  case GH_NIP29_ADMISSION_ACCEPTED:
    return "accepted";
  case GH_NIP29_ADMISSION_DUPLICATE:
    return "duplicate of the current snapshot";
  case GH_NIP29_ADMISSION_UNSUPPORTED_KIND:
    return "not a relay group snapshot kind (39000-39003)";
  case GH_NIP29_ADMISSION_UNSIGNED:
    return "unsigned event";
  case GH_NIP29_ADMISSION_MALFORMED:
    return "malformed event";
  case GH_NIP29_ADMISSION_FORGED:
    return "event id or signature does not verify";
  case GH_NIP29_ADMISSION_WRONG_AUTHOR:
    return "not signed by the group's relay key";
  case GH_NIP29_ADMISSION_WRONG_GROUP:
    return "d tag does not name this group";
  case GH_NIP29_ADMISSION_STALE:
    return "older than the current snapshot";
  case GH_NIP29_ADMISSION_FAILED:
  default:
    return "could not apply snapshot";
  }
}

const gchar *
gh_nip29_group_get_snapshot_id(const GhNip29Group *group, gint kind, gint64 *out_created_at)
{
  g_return_val_if_fail(group != NULL, NULL);
  gint slot = snapshot_slot(kind);
  if (slot < 0 || !group->snapshots[slot].present)
    return NULL;
  if (out_created_at)
    *out_created_at = group->snapshots[slot].created_at;
  return group->snapshots[slot].id;
}

/* ---- Metadata ------------------------------------------------------------ */

GhNip29Metadata *
gh_nip29_metadata_new(void)
{
  return g_new0(GhNip29Metadata, 1);
}

static gint *
copy_kinds(const gint *kinds, gsize n)
{
  if (!kinds || n == 0)
    return NULL;
  gint *copy = g_new(gint, n);
  memcpy(copy, kinds, n * sizeof(gint));
  return copy;
}

GhNip29Metadata *
gh_nip29_metadata_copy(const GhNip29Metadata *metadata)
{
  g_return_val_if_fail(metadata != NULL, NULL);
  GhNip29Metadata *copy = gh_nip29_metadata_new();
  copy->name = g_strdup(metadata->name);
  copy->about = g_strdup(metadata->about);
  copy->picture = g_strdup(metadata->picture);
  copy->banner = g_strdup(metadata->banner);
  copy->is_private = metadata->is_private;
  copy->is_restricted = metadata->is_restricted;
  copy->is_hidden = metadata->is_hidden;
  copy->is_closed = metadata->is_closed;
  copy->has_livekit = metadata->has_livekit;
  copy->has_supported_kinds = metadata->has_supported_kinds;
  copy->supported_kinds = copy_kinds(metadata->supported_kinds, metadata->n_supported_kinds);
  copy->n_supported_kinds = copy->supported_kinds ? metadata->n_supported_kinds : 0;
  copy->parent = g_strdup(metadata->parent);
  copy->children = g_strdupv(metadata->children);
  return copy;
}

void
gh_nip29_metadata_free(GhNip29Metadata *metadata)
{
  if (!metadata)
    return;
  g_free(metadata->name);
  g_free(metadata->about);
  g_free(metadata->picture);
  g_free(metadata->banner);
  g_free(metadata->supported_kinds);
  g_free(metadata->parent);
  g_strfreev(metadata->children);
  g_free(metadata);
}

gboolean
gh_nip29_metadata_supports_kind(const GhNip29Metadata *metadata, gint kind)
{
  g_return_val_if_fail(metadata != NULL, FALSE);
  if (!metadata->has_supported_kinds)
    return TRUE;
  for (gsize i = 0; i < metadata->n_supported_kinds; i++) {
    if (metadata->supported_kinds[i] == kind)
      return TRUE;
  }
  return FALSE;
}

GhNip29Metadata *
gh_nip29_group_dup_metadata(const GhNip29Group *group)
{
  g_return_val_if_fail(group != NULL, NULL);
  if (!group->snapshots[SNAPSHOT_METADATA].present)
    return NULL;
  const nostr_group_t *state = group->state;
  GhNip29Metadata *metadata = gh_nip29_metadata_new();
  metadata->name = g_strdup(state->name);
  metadata->about = g_strdup(state->about);
  metadata->picture = g_strdup(state->picture);
  metadata->banner = g_strdup(state->banner);
  metadata->is_private = state->is_private;
  metadata->is_restricted = state->is_restricted;
  metadata->is_hidden = state->is_hidden;
  metadata->is_closed = state->is_closed;
  metadata->has_livekit = group->has_livekit;
  metadata->has_supported_kinds = group->has_supported_kinds;
  metadata->n_supported_kinds = group->supported_kinds->len;
  metadata->supported_kinds = copy_kinds((const gint *)(gpointer)group->supported_kinds->data,
                                         group->supported_kinds->len);
  metadata->parent = g_strdup(state->parent);
  metadata->children = g_new0(gchar *, state->children_len + 1);
  for (gsize i = 0; i < state->children_len; i++)
    metadata->children[i] = g_strdup(state->children[i]);
  return metadata;
}

/* ---- Admins and roles ---------------------------------------------------- */

void
gh_nip29_admin_free(GhNip29Admin *admin)
{
  if (!admin)
    return;
  g_free(admin->pubkey);
  g_strfreev(admin->roles);
  g_free(admin);
}

void
gh_nip29_role_free(GhNip29Role *role)
{
  if (!role)
    return;
  g_free(role->name);
  g_free(role->description);
  g_free(role);
}

static GStrv
admin_roles_strv(const nostr_group_admin_t *admin)
{
  GStrv roles = g_new0(gchar *, admin->roles_len + 1);
  gsize n = 0;
  for (gsize i = 0; i < admin->roles_len; i++) {
    if (admin->roles[i] && admin->roles[i][0])
      roles[n++] = g_strdup(admin->roles[i]);
  }
  return roles;
}

GPtrArray *
gh_nip29_group_dup_admins(const GhNip29Group *group)
{
  g_return_val_if_fail(group != NULL, NULL);
  if (!group->snapshots[SNAPSHOT_ADMINS].present)
    return NULL;
  GPtrArray *admins = g_ptr_array_new_with_free_func((GDestroyNotify)gh_nip29_admin_free);
  for (gsize i = 0; i < group->state->admins_len; i++) {
    const nostr_group_admin_t *entry = &group->state->admins[i];
    if (!gh_nip29_is_hex64(entry->pubkey))
      continue;
    GhNip29Admin *admin = g_new0(GhNip29Admin, 1);
    admin->pubkey = g_strdup(entry->pubkey);
    admin->roles = admin_roles_strv(entry);
    g_ptr_array_add(admins, admin);
  }
  return admins;
}

GStrv
gh_nip29_group_dup_admin_roles(const GhNip29Group *group, const gchar *pubkey)
{
  g_return_val_if_fail(group != NULL, NULL);
  if (!group->snapshots[SNAPSHOT_ADMINS].present || !gh_nip29_is_hex64(pubkey))
    return NULL;
  const nostr_group_admin_t *admin = nostr_group_get_admin(group->state, pubkey);
  return admin ? admin_roles_strv(admin) : NULL;
}

GPtrArray *
gh_nip29_group_dup_roles(const GhNip29Group *group)
{
  g_return_val_if_fail(group != NULL, NULL);
  if (!group->snapshots[SNAPSHOT_ROLES].present)
    return NULL;
  GPtrArray *roles = g_ptr_array_new_with_free_func((GDestroyNotify)gh_nip29_role_free);
  for (gsize i = 0; i < group->state->roles_len; i++) {
    const nostr_group_role_t *entry = &group->state->roles[i];
    GhNip29Role *role = g_new0(GhNip29Role, 1);
    role->name = g_strdup(entry->name);
    role->description = g_strdup(entry->description);
    g_ptr_array_add(roles, role);
  }
  return roles;
}

/* ---- Members ------------------------------------------------------------- */

GhNip29MemberList
gh_nip29_group_dup_members(const GhNip29Group *group, GStrv *out_pubkeys)
{
  if (out_pubkeys)
    *out_pubkeys = NULL;
  g_return_val_if_fail(group != NULL, GH_NIP29_MEMBERS_UNAVAILABLE);
  if (!group->snapshots[SNAPSHOT_MEMBERS].present)
    return GH_NIP29_MEMBERS_UNAVAILABLE;
  if (out_pubkeys) {
    GStrv pubkeys = g_new0(gchar *, group->state->members_len + 1);
    gsize n = 0;
    for (gsize i = 0; i < group->state->members_len; i++) {
      if (gh_nip29_is_hex64(group->state->members[i].pubkey))
        pubkeys[n++] = g_strdup(group->state->members[i].pubkey);
    }
    *out_pubkeys = pubkeys;
  }
  return GH_NIP29_MEMBERS_PARTIAL;
}

GhNip29Membership
gh_nip29_group_lookup_member(const GhNip29Group *group, const gchar *pubkey)
{
  g_return_val_if_fail(group != NULL, GH_NIP29_MEMBERSHIP_UNKNOWN);
  if (group->snapshots[SNAPSHOT_MEMBERS].present && gh_nip29_is_hex64(pubkey) &&
      nostr_group_get_member(group->state, pubkey))
    return GH_NIP29_MEMBERSHIP_LISTED;
  return GH_NIP29_MEMBERSHIP_UNKNOWN;
}

/* ---- Authorization ------------------------------------------------------- */

/* Permissions are stored as bits of a guint mask. */
G_STATIC_ASSERT(NOSTR_PERMISSION_UPDATE_PIN_LIST < 32);

static gboolean
permission_is_valid(nostr_permission_t permission)
{
  return nostr_permission_to_string(permission) != NULL;
}

GhNip29RolePolicy *
gh_nip29_role_policy_new(void)
{
  GhNip29RolePolicy *policy = g_new0(GhNip29RolePolicy, 1);
  policy->roles = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  return policy;
}

void
gh_nip29_role_policy_free(GhNip29RolePolicy *policy)
{
  if (!policy)
    return;
  g_hash_table_unref(policy->roles);
  g_free(policy);
}

gboolean
gh_nip29_role_policy_set_role(GhNip29RolePolicy *policy, const gchar *role,
                              const nostr_permission_t *permissions, gsize n_permissions,
                              GError **error)
{
  g_return_val_if_fail(policy != NULL, FALSE);
  if (!role || !*role || !g_utf8_validate(role, -1, NULL) ||
      (n_permissions > 0 && !permissions)) {
    g_set_error_literal(error, GH_NIP29_ERROR, GH_NIP29_ERROR_INVALID_ARGUMENT,
                        "role policy needs a non-empty UTF-8 role name");
    return FALSE;
  }
  guint mask = 0;
  for (gsize i = 0; i < n_permissions; i++) {
    if (!permission_is_valid(permissions[i])) {
      g_set_error(error, GH_NIP29_ERROR, GH_NIP29_ERROR_INVALID_ARGUMENT,
                  "unknown NIP-29 permission %d", (gint)permissions[i]);
      return FALSE;
    }
    mask |= 1u << permissions[i];
  }
  g_hash_table_insert(policy->roles, g_strdup(role), GUINT_TO_POINTER(mask));
  return TRUE;
}

GhNip29Authz
gh_nip29_group_check_permission(const GhNip29Group *group, const GhNip29RolePolicy *policy,
                                const gchar *pubkey, nostr_permission_t permission)
{
  g_return_val_if_fail(group != NULL, GH_NIP29_AUTHZ_DENIED_INVALID);
  if (!gh_nip29_is_hex64(pubkey) || !permission_is_valid(permission))
    return GH_NIP29_AUTHZ_DENIED_INVALID;
  if (!group->snapshots[SNAPSHOT_ADMINS].present)
    return GH_NIP29_AUTHZ_UNKNOWN_NO_ADMINS;
  const nostr_group_admin_t *admin = nostr_group_get_admin(group->state, pubkey);
  if (!admin)
    return GH_NIP29_AUTHZ_DENIED_NOT_ADMIN;

  /* With a relay-signed 39003, only advertised roles carry privileges; the
   * relay may accept other role names and "just not do anything with them". */
  gboolean roles_advertised = group->snapshots[SNAPSHOT_ROLES].present;
  gboolean any_candidate = FALSE;
  gboolean any_unknown = FALSE;
  for (gsize i = 0; i < admin->roles_len; i++) {
    const gchar *role = admin->roles[i];
    if (!role || !*role)
      continue;
    if (roles_advertised && !nostr_group_get_role(group->state, role))
      continue;
    any_candidate = TRUE;
    gpointer mask = NULL;
    if (!policy || !g_hash_table_lookup_extended(policy->roles, role, NULL, &mask)) {
      any_unknown = TRUE;
      continue;
    }
    if (GPOINTER_TO_UINT(mask) & (1u << permission))
      return GH_NIP29_AUTHZ_ALLOWED;
  }
  if (!any_candidate && admin->roles_len > 0 && roles_advertised)
    return GH_NIP29_AUTHZ_DENIED_UNADVERTISED_ROLES;
  if (!any_candidate || any_unknown)
    return GH_NIP29_AUTHZ_UNKNOWN_POLICY;
  return GH_NIP29_AUTHZ_DENIED_BY_POLICY;
}

gboolean
gh_nip29_group_can(const GhNip29Group *group, const GhNip29RolePolicy *policy,
                   const gchar *pubkey, nostr_permission_t permission)
{
  return gh_nip29_group_check_permission(group, policy, pubkey, permission) ==
         GH_NIP29_AUTHZ_ALLOWED;
}

const gchar *
gh_nip29_authz_to_string(GhNip29Authz authz)
{
  switch (authz) {
  case GH_NIP29_AUTHZ_ALLOWED:
    return "allowed by the relay role policy";
  case GH_NIP29_AUTHZ_DENIED_INVALID:
    return "invalid pubkey or permission";
  case GH_NIP29_AUTHZ_DENIED_NOT_ADMIN:
    return "not listed as a group admin";
  case GH_NIP29_AUTHZ_DENIED_UNADVERTISED_ROLES:
    return "roles are not advertised by the relay";
  case GH_NIP29_AUTHZ_DENIED_BY_POLICY:
    return "roles do not grant this permission";
  case GH_NIP29_AUTHZ_UNKNOWN_NO_ADMINS:
    return "group admins are not available";
  case GH_NIP29_AUTHZ_UNKNOWN_POLICY:
  default:
    return "role permissions are unknown";
  }
}
