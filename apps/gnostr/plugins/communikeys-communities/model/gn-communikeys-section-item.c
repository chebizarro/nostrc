#include "gn-communikeys-section-item.h"

typedef struct {
  int kind;
  gchar *subtype;
} Assignment;

struct _GnCommunikeysSectionItem {
  GObject parent_instance;
  gchar *community_pubkey;
  gchar *name;
  gchar *acl_publisher;
  gchar *acl_identifier;
  gchar *acl_relay;
  GArray *assignments;
  GPtrArray *badges;
  GPtrArray *members;
  GnCommunikeysAclState acl_state;
  gchar *acl_status;
};

G_DEFINE_TYPE(GnCommunikeysSectionItem, gn_communikeys_section_item,
              G_TYPE_OBJECT)

static void assignment_clear(gpointer data) {
  Assignment *assignment = data;
  g_free(assignment->subtype);
}

static gint string_compare(gconstpointer a, gconstpointer b) {
  const char * const *sa = a;
  const char * const *sb = b;
  return g_strcmp0(*sa, *sb);
}

static void gn_communikeys_section_item_finalize(GObject *object) {
  GnCommunikeysSectionItem *self = GN_COMMUNIKEYS_SECTION_ITEM(object);
  g_free(self->community_pubkey);
  g_free(self->name);
  g_free(self->acl_publisher);
  g_free(self->acl_identifier);
  g_free(self->acl_relay);
  g_clear_pointer(&self->assignments, g_array_unref);
  g_clear_pointer(&self->badges, g_ptr_array_unref);
  g_clear_pointer(&self->members, g_ptr_array_unref);
  g_free(self->acl_status);
  G_OBJECT_CLASS(gn_communikeys_section_item_parent_class)->finalize(object);
}

static void gn_communikeys_section_item_class_init(
    GnCommunikeysSectionItemClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = gn_communikeys_section_item_finalize;
}

static void gn_communikeys_section_item_init(
    GnCommunikeysSectionItem *self) {
  self->assignments = g_array_new(FALSE, FALSE, sizeof(Assignment));
  g_array_set_clear_func(self->assignments, assignment_clear);
  self->badges = g_ptr_array_new_with_free_func(g_free);
  self->members = g_ptr_array_new_with_free_func(g_free);
  self->acl_state = GN_COMMUNIKEYS_ACL_UNRESOLVED;
  self->acl_status = g_strdup("ACL not loaded — publishing disabled");
}

GnCommunikeysSectionItem *gn_communikeys_section_item_new(
    const char *community_pubkey,
    const nostr_communikeys_section_t *section) {
  g_return_val_if_fail(community_pubkey != NULL, NULL);
  g_return_val_if_fail(section != NULL, NULL);
  GnCommunikeysSectionItem *self =
    g_object_new(GN_TYPE_COMMUNIKEYS_SECTION_ITEM, NULL);
  self->community_pubkey = g_strdup(community_pubkey);
  self->name = g_strdup(section->name);
  self->acl_publisher = g_strdup(section->profile_list.pubkey);
  self->acl_identifier = g_strdup(section->profile_list.identifier);
  self->acl_relay = g_strdup(section->profile_list.relay);
  for (gsize i = 0; i < section->assignments_len; i++) {
    Assignment assignment = {
      .kind = section->assignments[i].kind,
      .subtype = g_strdup(section->assignments[i].subtype)
    };
    g_array_append_val(self->assignments, assignment);
  }
  for (gsize i = 0; i < section->badges_len; i++)
    g_ptr_array_add(self->badges, g_strdup(section->badges[i]));
  if (g_strcmp0(self->acl_publisher, community_pubkey) != 0)
    gn_communikeys_section_item_set_acl(
      self, GN_COMMUNIKEYS_ACL_UNTRUSTED_PUBLISHER,
      "Untrusted ACL publisher — policy requires the community key",
      NULL, 0);
  return self;
}

const char *gn_communikeys_section_item_get_community_pubkey(
    GnCommunikeysSectionItem *self) {
  g_return_val_if_fail(GN_IS_COMMUNIKEYS_SECTION_ITEM(self), NULL);
  return self->community_pubkey;
}
const char *gn_communikeys_section_item_get_name(
    GnCommunikeysSectionItem *self) {
  g_return_val_if_fail(GN_IS_COMMUNIKEYS_SECTION_ITEM(self), NULL);
  return self->name;
}
const char *gn_communikeys_section_item_get_acl_publisher(
    GnCommunikeysSectionItem *self) {
  g_return_val_if_fail(GN_IS_COMMUNIKEYS_SECTION_ITEM(self), NULL);
  return self->acl_publisher;
}
const char *gn_communikeys_section_item_get_acl_identifier(
    GnCommunikeysSectionItem *self) {
  g_return_val_if_fail(GN_IS_COMMUNIKEYS_SECTION_ITEM(self), NULL);
  return self->acl_identifier;
}
const char *gn_communikeys_section_item_get_acl_relay(
    GnCommunikeysSectionItem *self) {
  g_return_val_if_fail(GN_IS_COMMUNIKEYS_SECTION_ITEM(self), NULL);
  return self->acl_relay;
}
guint gn_communikeys_section_item_get_assignment_count(
    GnCommunikeysSectionItem *self) {
  g_return_val_if_fail(GN_IS_COMMUNIKEYS_SECTION_ITEM(self), 0);
  return self->assignments->len;
}
gboolean gn_communikeys_section_item_get_assignment(
    GnCommunikeysSectionItem *self, guint index, int *kind,
    const char **subtype) {
  g_return_val_if_fail(GN_IS_COMMUNIKEYS_SECTION_ITEM(self), FALSE);
  if (index >= self->assignments->len) return FALSE;
  Assignment *assignment = &g_array_index(self->assignments, Assignment, index);
  if (kind) *kind = assignment->kind;
  if (subtype) *subtype = assignment->subtype;
  return TRUE;
}
guint gn_communikeys_section_item_get_badge_count(
    GnCommunikeysSectionItem *self) {
  g_return_val_if_fail(GN_IS_COMMUNIKEYS_SECTION_ITEM(self), 0);
  return self->badges->len;
}
const char *gn_communikeys_section_item_get_badge(
    GnCommunikeysSectionItem *self, guint index) {
  g_return_val_if_fail(GN_IS_COMMUNIKEYS_SECTION_ITEM(self), NULL);
  return index < self->badges->len ? g_ptr_array_index(self->badges, index) : NULL;
}
GnCommunikeysAclState gn_communikeys_section_item_get_acl_state(
    GnCommunikeysSectionItem *self) {
  g_return_val_if_fail(GN_IS_COMMUNIKEYS_SECTION_ITEM(self),
                       GN_COMMUNIKEYS_ACL_INVALID);
  return self->acl_state;
}
const char *gn_communikeys_section_item_get_acl_status(
    GnCommunikeysSectionItem *self) {
  g_return_val_if_fail(GN_IS_COMMUNIKEYS_SECTION_ITEM(self), NULL);
  return self->acl_status;
}
guint gn_communikeys_section_item_get_member_count(
    GnCommunikeysSectionItem *self) {
  g_return_val_if_fail(GN_IS_COMMUNIKEYS_SECTION_ITEM(self), 0);
  return self->members->len;
}
const char *gn_communikeys_section_item_get_member(
    GnCommunikeysSectionItem *self, guint index) {
  g_return_val_if_fail(GN_IS_COMMUNIKEYS_SECTION_ITEM(self), NULL);
  return index < self->members->len ? g_ptr_array_index(self->members, index) : NULL;
}
gboolean gn_communikeys_section_item_has_member(
    GnCommunikeysSectionItem *self, const char *pubkey) {
  g_return_val_if_fail(GN_IS_COMMUNIKEYS_SECTION_ITEM(self), FALSE);
  for (guint i = 0; i < self->members->len; i++)
    if (g_strcmp0(g_ptr_array_index(self->members, i), pubkey) == 0)
      return TRUE;
  return FALSE;
}

void gn_communikeys_section_item_set_acl(
    GnCommunikeysSectionItem *self, GnCommunikeysAclState state,
    const char *status, char * const *members, gsize members_len) {
  g_return_if_fail(GN_IS_COMMUNIKEYS_SECTION_ITEM(self));
  self->acl_state = state;
  g_free(self->acl_status);
  self->acl_status = g_strdup(status ? status : "");
  g_ptr_array_set_size(self->members, 0);
  for (gsize i = 0; members && i < members_len; i++) {
    gboolean duplicate = FALSE;
    for (guint j = 0; j < self->members->len; j++)
      if (g_strcmp0(g_ptr_array_index(self->members, j), members[i]) == 0) {
        duplicate = TRUE;
        break;
      }
    if (!duplicate) g_ptr_array_add(self->members, g_strdup(members[i]));
  }
  g_ptr_array_sort(self->members, string_compare);
}
