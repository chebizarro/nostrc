#include "gn-communikeys-community-service.h"
#include "model/gn-communikeys-message-item.h"
#include "model/gn-communikeys-section-item.h"
#include "model/gn-communikeys-targeted-item.h"

#include <json-glib/json-glib.h>
#include <nostr-event.h>
#include <nostr-tag.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_CACHED_EVENTS 500

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NostrEvent, nostr_event_free)

typedef struct {
  NostrEvent *event;
  gchar *id;
  gint64 created_at;
} AclSlot;

/* One community BRANCH, keyed by its exact definition address
 * "32222:<owner>:<communityId>". Same-ID branches from different owners are
 * independent states with independent metadata, grants, and content. */
typedef struct {
  gchar *address;
  gchar *owner;
  gchar *community_id;
  NostrEvent *definition_event;
  nostr_communikeys_definition_t definition;
  gboolean has_definition;
  gchar *definition_id;
  gint64 definition_created_at;
  GnCommunikeysCommunityItem *item;
  AclSlot **acls; /* [section][profile-list reference] — multi-shard slots */
  GListStore *messages;
  GListStore *targets;
} CommunityState;

typedef struct {
  gchar *id;
  gchar *json;
} RawEvent;

struct _GnCommunikeysCommunityService {
  GObject parent_instance;
  GnostrPluginContext *context; /* host-owned */
  gchar *offline_user_pubkey;
  GListStore *communities;
  GHashTable *states;   /* definition address -> CommunityState */
  GHashTable *originals; /* event id/address -> NostrEvent */
  GPtrArray *raw_exclusive; /* RawEvent */
  GPtrArray *raw_targets;   /* RawEvent */
  guint64 definition_subscription;
  guint64 acl_subscription;
  guint64 content_subscription;
  gboolean shutting_down;
};

enum {
  COMMUNITY_UPDATED,
  ERROR_REPORTED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

G_DEFINE_TYPE(GnCommunikeysCommunityService,
              gn_communikeys_community_service, G_TYPE_OBJECT)

static gboolean process_exclusive_event(GnCommunikeysCommunityService *self,
                                        NostrEvent *event,
                                        const char *event_json,
                                        gboolean emit_update);
static gboolean process_target_event(GnCommunikeysCommunityService *self,
                                     NostrEvent *event,
                                     gboolean emit_update);
static void rebuild_authorized_content(GnCommunikeysCommunityService *self);

static void emit_error(GnCommunikeysCommunityService *self,
                       const char *message) {
  g_warning("Communikeys: %s", message);
  g_signal_emit(self, signals[ERROR_REPORTED], 0, message);
}

static void emit_update(GnCommunikeysCommunityService *self,
                        const char *address,
                        GnCommunikeysUpdateFlags flags) {
  g_signal_emit(self, signals[COMMUNITY_UPDATED], 0, address, (guint)flags);
}

static void acl_slot_clear(AclSlot *slot) {
  if (!slot) return;
  nostr_event_free(slot->event);
  g_free(slot->id);
  memset(slot, 0, sizeof(*slot));
}

static void community_state_clear_definition(CommunityState *state) {
  if (!state) return;
  if (state->acls && state->has_definition) {
    for (gsize i = 0; i < state->definition.sections_len; i++) {
      for (gsize j = 0; j < state->definition.sections[i].profile_lists_len; j++)
        acl_slot_clear(&state->acls[i][j]);
      g_free(state->acls[i]);
    }
  }
  g_clear_pointer(&state->acls, g_free);
  if (state->has_definition)
    nostr_communikeys_definition_clear(&state->definition);
  state->has_definition = FALSE;
  nostr_event_free(state->definition_event);
  state->definition_event = NULL;
  g_clear_pointer(&state->definition_id, g_free);
  g_clear_object(&state->item);
}

static void community_state_free(gpointer data) {
  CommunityState *state = data;
  if (!state) return;
  community_state_clear_definition(state);
  g_free(state->address);
  g_free(state->owner);
  g_free(state->community_id);
  g_clear_object(&state->messages);
  g_clear_object(&state->targets);
  g_free(state);
}

static CommunityState *community_state_new(const char *address,
                                           const char *owner,
                                           const char *community_id) {
  CommunityState *state = g_new0(CommunityState, 1);
  state->address = g_strdup(address);
  state->owner = g_strdup(owner);
  state->community_id = g_strdup(community_id);
  state->messages = g_list_store_new(GN_TYPE_COMMUNIKEYS_MESSAGE_ITEM);
  state->targets = g_list_store_new(GN_TYPE_COMMUNIKEYS_TARGETED_ITEM);
  return state;
}

static void raw_event_free(gpointer data) {
  RawEvent *raw = data;
  if (!raw) return;
  g_free(raw->id);
  g_free(raw->json);
  g_free(raw);
}

static gboolean raw_cache_add(GPtrArray *cache, const char *id,
                              const char *json) {
  for (guint i = 0; i < cache->len; i++) {
    RawEvent *raw = g_ptr_array_index(cache, i);
    if (g_strcmp0(raw->id, id) == 0) return FALSE;
  }
  RawEvent *raw = g_new0(RawEvent, 1);
  raw->id = g_strdup(id);
  raw->json = g_strdup(json);
  g_ptr_array_add(cache, raw);
  if (cache->len > MAX_CACHED_EVENTS)
    g_ptr_array_remove_index(cache, 0);
  return TRUE;
}

static CommunityState *find_state(GnCommunikeysCommunityService *self,
                                  const char *address) {
  return address ? g_hash_table_lookup(self->states, address) : NULL;
}

static gint find_community_index(GnCommunikeysCommunityService *self,
                                 const char *address) {
  guint n = g_list_model_get_n_items(G_LIST_MODEL(self->communities));
  for (guint i = 0; i < n; i++) {
    g_autoptr(GnCommunikeysCommunityItem) item =
      g_list_model_get_item(G_LIST_MODEL(self->communities), i);
    if (g_strcmp0(gn_communikeys_community_item_get_address(item), address) == 0)
      return (gint)i;
  }
  return -1;
}

static const char *event_tag_value(const NostrEvent *event,
                                   const char *key) {
  NostrTags *tags = nostr_event_get_tags(event);
  if (!tags) return NULL;
  for (gsize i = 0; i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (tag && nostr_tag_size(tag) >= 2 &&
        g_strcmp0(nostr_tag_get(tag, 0), key) == 0)
      return nostr_tag_get(tag, 1);
  }
  return NULL;
}

static gboolean event_is_newer(gint64 new_created_at, const char *new_id,
                               gint64 old_created_at, const char *old_id) {
  if (!old_id) return TRUE;
  if (new_created_at != old_created_at)
    return new_created_at > old_created_at;
  /* NIP-01 replaceable tie-break: the lexicographically lower id wins. */
  return g_strcmp0(new_id, old_id) < 0;
}

static gchar *build_filter(const int *kinds, gsize n_kinds,
                           const char *author,
                           const char *tag_name,
                           const char *tag_value,
                           guint limit) {
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);
  if (kinds && n_kinds) {
    json_builder_set_member_name(builder, "kinds");
    json_builder_begin_array(builder);
    for (gsize i = 0; i < n_kinds; i++)
      json_builder_add_int_value(builder, kinds[i]);
    json_builder_end_array(builder);
  }
  if (author) {
    json_builder_set_member_name(builder, "authors");
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, author);
    json_builder_end_array(builder);
  }
  if (tag_name && tag_value) {
    json_builder_set_member_name(builder, tag_name);
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, tag_value);
    json_builder_end_array(builder);
  }
  if (limit) {
    json_builder_set_member_name(builder, "limit");
    json_builder_add_int_value(builder, limit);
  }
  json_builder_end_object(builder);
  g_autoptr(JsonGenerator) generator = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(generator, root);
  gchar *json = json_generator_to_data(generator, NULL);
  json_node_free(root);
  return json;
}

static gboolean parse_verified_event(const char *event_json,
                                     NostrEvent **out_event,
                                     gchar **out_id) {
  g_return_val_if_fail(out_event != NULL, FALSE);
  *out_event = NULL;
  if (out_id) *out_id = NULL;
  if (!event_json) return FALSE;
  NostrEvent *event = nostr_event_new();
  if (!event || !nostr_event_deserialize_compact(event, event_json, NULL) ||
      nostr_event_validate(event, NULL) != NOSTR_EVENT_VALIDATION_OK) {
    nostr_event_free(event);
    return FALSE;
  }
  gchar *id = nostr_event_get_id(event);
  if (!id) {
    nostr_event_free(event);
    return FALSE;
  }
  *out_event = event;
  if (out_id) *out_id = id;
  else free(id);
  return TRUE;
}

static gboolean resolve_assignment(CommunityState *state, int kind,
                                   gsize *section_index,
                                   const char **subtype) {
  if (!state || !state->has_definition || !state->definition.valid)
    return FALSE;
  guint matches = 0;
  gsize found_section = 0;
  const char *found_subtype = NULL;
  for (gsize i = 0; i < state->definition.sections_len; i++) {
    nostr_communikeys_section_t *section = &state->definition.sections[i];
    for (gsize j = 0; j < section->assignments_len; j++) {
      if (section->assignments[j].kind == kind) {
        matches++;
        found_section = i;
        found_subtype = section->assignments[j].subtype;
      }
    }
  }
  if (matches != 1) return FALSE;
  if (section_index) *section_index = found_section;
  if (subtype) *subtype = found_subtype;
  return TRUE;
}

/* V2 grant evaluation: the union of current valid `p` grants across every
 * shard the section references, plus the owner's inherent authority and the
 * structural role of referenced delegated list authors — all evaluated by
 * nostr_communikeys_author_can_publish. */
static gboolean state_author_can_publish(CommunityState *state, int kind,
                                         const char *author_pubkey,
                                         gsize *section_index_out) {
  gsize section_index = 0;
  const char *subtype = NULL;
  if (!author_pubkey ||
      !resolve_assignment(state, kind, &section_index, &subtype) ||
      !state->acls)
    return FALSE;
  const nostr_communikeys_section_t *section =
    &state->definition.sections[section_index];
  const NostrEvent **events = NULL;
  gsize events_len = 0;
  if (section->profile_lists_len) {
    events = g_new0(const NostrEvent *, section->profile_lists_len);
    for (gsize i = 0; i < section->profile_lists_len; i++)
      if (state->acls[section_index][i].event)
        events[events_len++] = state->acls[section_index][i].event;
  }
  gboolean allowed = nostr_communikeys_author_can_publish(
    &state->definition, kind, subtype, events, events_len, author_pubkey);
  g_free(events);
  if (!allowed) return FALSE;
  if (section_index_out) *section_index_out = section_index;
  return TRUE;
}

static NostrEvent *clone_event(const NostrEvent *event) {
  char *json = nostr_event_serialize_compact(event);
  if (!json) return NULL;
  NostrEvent *copy = nostr_event_new();
  if (!copy || !nostr_event_deserialize_compact(copy, json, NULL)) {
    nostr_event_free(copy);
    copy = NULL;
  }
  free(json);
  return copy;
}

static void cache_original(GnCommunikeysCommunityService *self,
                           NostrEvent *event) {
  g_autofree gchar *id = nostr_event_get_id(event);
  if (!id) return;
  NostrEvent *existing = g_hash_table_lookup(self->originals, id);
  if (!existing || event_is_newer(nostr_event_get_created_at(event), id,
                                  nostr_event_get_created_at(existing), id))
    g_hash_table_replace(self->originals, g_strdup(id),
                         clone_event(event));

  int kind = nostr_event_get_kind(event);
  const char *d = event_tag_value(event, "d");
  const char *pubkey = nostr_event_get_pubkey(event);
  if (kind >= 30000 && kind < 40000 && d && pubkey) {
    g_autofree gchar *coordinate =
      g_strdup_printf("%d:%s:%s", kind, pubkey, d);
    existing = g_hash_table_lookup(self->originals, coordinate);
    g_autofree gchar *existing_id =
      existing ? nostr_event_get_id(existing) : NULL;
    if (!existing ||
        event_is_newer(nostr_event_get_created_at(event), id,
                       nostr_event_get_created_at(existing), existing_id))
      g_hash_table_replace(self->originals, g_strdup(coordinate),
                           clone_event(event));
  }
}

static NostrEvent *resolve_sourced_original(
    GnCommunikeysCommunityService *self,
    const nostr_communikeys_targeted_publication_t *publication) {
  NostrEvent *original =
    g_hash_table_lookup(self->originals, publication->reference);
  if (original || !self->context) return original;

  g_autoptr(GError) error = NULL;
  if (publication->reference_type == NOSTR_COMMUNIKEYS_REFERENCE_EVENT) {
    g_autofree gchar *json = gnostr_plugin_context_get_event_by_id(
      self->context, publication->reference, &error);
    g_autoptr(NostrEvent) parsed = NULL;
    if (json && parse_verified_event(json, &parsed, NULL))
      cache_original(self, parsed);
  } else {
    nostr_communikeys_coordinate_t coordinate;
    if (!nostr_communikeys_coordinate_parse(
          publication->reference, publication->reference_relay,
          &coordinate))
      return NULL;
    int kind = coordinate.kind;
    g_autofree gchar *filter = build_filter(
      &kind, 1, coordinate.pubkey, "#d", coordinate.identifier, 20);
    g_autoptr(GPtrArray) events = gnostr_plugin_context_query_events(
      self->context, filter, &error);
    if (events)
      for (guint i = 0; i < events->len; i++) {
        g_autoptr(NostrEvent) parsed = NULL;
        if (parse_verified_event(g_ptr_array_index(events, i), &parsed, NULL))
          cache_original(self, parsed);
      }
    nostr_communikeys_coordinate_clear(&coordinate);
  }
  return g_hash_table_lookup(self->originals, publication->reference);
}

/* Sourceless wrapper (V2): the original uses the wrapper `d` as its targeting
 * `h` and shares the wrapper author. */
static NostrEvent *resolve_sourceless_original(
    GnCommunikeysCommunityService *self,
    const NostrEvent *wrapper,
    const nostr_communikeys_targeted_publication_t *publication) {
  if (self->context) {
    int kind = publication->original_kind;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *filter = build_filter(
      &kind, 1, publication->curator, "#h", publication->identifier, 20);
    g_autoptr(GPtrArray) events = gnostr_plugin_context_query_events(
      self->context, filter, &error);
    if (events)
      for (guint i = 0; i < events->len; i++) {
        g_autoptr(NostrEvent) parsed = NULL;
        if (parse_verified_event(g_ptr_array_index(events, i), &parsed, NULL))
          cache_original(self, parsed);
      }
  }
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->originals);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    NostrEvent *candidate = value;
    if (!candidate) continue;
    if (nostr_event_get_kind(candidate) != publication->original_kind) continue;
    if (g_strcmp0(nostr_event_get_pubkey(candidate),
                  publication->curator) != 0) continue;
    if (nostr_communikeys_targeted_publication_validate(
          (NostrEvent *)wrapper, candidate) == NOSTR_COMMUNIKEYS_OK)
      return candidate;
  }
  return NULL;
}

static NostrEvent *resolve_original(
    GnCommunikeysCommunityService *self,
    const NostrEvent *wrapper,
    const nostr_communikeys_targeted_publication_t *publication) {
  return publication->has_source
    ? resolve_sourced_original(self, publication)
    : resolve_sourceless_original(self, wrapper, publication);
}

/* Recomputes one section's ACL presentation from its shard slots: the
 * effective member set is the union of current valid `p` grants across every
 * referenced coordinate. */
static void update_section_acl_view(CommunityState *state,
                                    gsize section_index) {
  if (!state->item || !state->acls) return;
  const nostr_communikeys_section_t *section =
    &state->definition.sections[section_index];
  g_autoptr(GnCommunikeysSectionItem) item =
    gn_communikeys_community_item_get_section(state->item, (guint)section_index);
  if (!item) return;
  if (section->profile_lists_len == 0) return; /* stays GRANT_FREE */

  GPtrArray *members = g_ptr_array_new_with_free_func(g_free);
  guint resolved = 0;
  for (gsize i = 0; i < section->profile_lists_len; i++) {
    AclSlot *slot = &state->acls[section_index][i];
    if (!slot->event) continue;
    nostr_communikeys_profile_list_t list;
    if (nostr_communikeys_profile_list_parse(
          slot->event,
          section->profile_lists[i].coordinate.pubkey,
          section->profile_lists[i].coordinate.identifier,
          &list) != NOSTR_COMMUNIKEYS_OK)
      continue;
    resolved++;
    for (gsize j = 0; j < list.members_len; j++)
      g_ptr_array_add(members, g_strdup(list.members[j]));
    nostr_communikeys_profile_list_clear(&list);
  }
  if (resolved == 0) {
    gn_communikeys_section_item_set_acl(
      item, GN_COMMUNIKEYS_ACL_UNRESOLVED,
      "ACL not loaded — publishing disabled", NULL, 0);
  } else {
    g_autofree gchar *status = g_strdup_printf(
      "Verified profile-list ACL (%u of %u shard%s, union of grants)",
      resolved, (guint)section->profile_lists_len,
      section->profile_lists_len == 1 ? "" : "s");
    gn_communikeys_section_item_set_acl(
      item, GN_COMMUNIKEYS_ACL_VERIFIED, status,
      (char * const *)members->pdata, members->len);
  }
  g_ptr_array_unref(members);
}

static gboolean ingest_definition(GnCommunikeysCommunityService *self,
                                  NostrEvent *event,
                                  const char *event_id) {
  nostr_communikeys_definition_t definition;
  if (!nostr_communikeys_definition_parse(event, &definition))
    return FALSE;
  if (!definition.valid) {
    g_autofree gchar *message = g_strdup_printf(
      "Ignored invalid community definition: %s",
      nostr_communikeys_status_string(definition.validation_status));
    emit_error(self, message);
    nostr_communikeys_definition_clear(&definition);
    return FALSE;
  }

  char *address_raw = nostr_communikeys_branch_format(&definition.branch);
  if (!address_raw) {
    nostr_communikeys_definition_clear(&definition);
    return FALSE;
  }
  g_autofree gchar *address = g_strdup(address_raw);
  free(address_raw);

  CommunityState *state = find_state(self, address);
  if (state &&
      !event_is_newer(nostr_event_get_created_at(event), event_id,
                      state->definition_created_at, state->definition_id)) {
    nostr_communikeys_definition_clear(&definition);
    return FALSE;
  }

  gint model_index = find_community_index(self, address);
  if (!state) {
    state = community_state_new(address, definition.branch.owner,
                                definition.branch.community_id);
    g_hash_table_insert(self->states, g_strdup(address), state);
  } else {
    community_state_clear_definition(state);
  }

  state->definition = definition;
  state->has_definition = TRUE;
  state->definition_event = clone_event(event);
  state->definition_id = g_strdup(event_id);
  state->definition_created_at = nostr_event_get_created_at(event);
  state->acls = g_new0(AclSlot *, definition.sections_len);
  for (gsize i = 0; i < definition.sections_len; i++)
    state->acls[i] = g_new0(AclSlot,
                            definition.sections[i].profile_lists_len
                              ? definition.sections[i].profile_lists_len : 1);
  state->item = gn_communikeys_community_item_new(
    &state->definition, state->address,
    state->definition_id, state->definition_created_at);

  if (model_index >= 0) {
    g_list_store_remove(self->communities, (guint)model_index);
    g_list_store_insert(self->communities, (guint)model_index, state->item);
  } else {
    g_list_store_append(self->communities, state->item);
  }

  rebuild_authorized_content(self);
  emit_update(self, state->address, GN_COMMUNIKEYS_UPDATE_DEFINITION);
  if (self->context)
    gn_communikeys_community_service_refresh_community(self, state->address);
  return TRUE;
}

static gboolean ingest_acl(GnCommunikeysCommunityService *self,
                           NostrEvent *event,
                           const char *event_id) {
  const char *publisher = nostr_event_get_pubkey(event);
  const char *identifier = event_tag_value(event, "d");
  gboolean accepted = FALSE;
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->states);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    CommunityState *state = value;
    if (!state->has_definition) continue;
    for (gsize i = 0; i < state->definition.sections_len; i++) {
      nostr_communikeys_section_t *section = &state->definition.sections[i];
      for (gsize j = 0; j < section->profile_lists_len; j++) {
        const nostr_communikeys_profile_list_ref_t *ref =
          &section->profile_lists[j];
        /* V2: list authors are ordinary delegated signers. Only the exact
         * referenced coordinate counts — placement is authoritative. */
        if (g_strcmp0(ref->coordinate.pubkey, publisher) != 0 ||
            g_strcmp0(ref->coordinate.identifier, identifier) != 0)
          continue;

        nostr_communikeys_profile_list_t list;
        nostr_communikeys_status_t status =
          nostr_communikeys_profile_list_parse(
            event, ref->coordinate.pubkey,
            ref->coordinate.identifier, &list);
        if (status != NOSTR_COMMUNIKEYS_OK) {
          if (!state->acls[i][j].event) {
            g_autoptr(GnCommunikeysSectionItem) item =
              gn_communikeys_community_item_get_section(state->item, (guint)i);
            if (item)
              gn_communikeys_section_item_set_acl(
                item, GN_COMMUNIKEYS_ACL_INVALID,
                nostr_communikeys_status_string(status), NULL, 0);
          }
          continue;
        }
        nostr_communikeys_profile_list_clear(&list);
        if (!event_is_newer(nostr_event_get_created_at(event), event_id,
                            state->acls[i][j].created_at,
                            state->acls[i][j].id))
          continue;

        acl_slot_clear(&state->acls[i][j]);
        state->acls[i][j].event = clone_event(event);
        state->acls[i][j].id = g_strdup(event_id);
        state->acls[i][j].created_at = nostr_event_get_created_at(event);
        update_section_acl_view(state, i);
        accepted = TRUE;
        emit_update(self, state->address, GN_COMMUNIKEYS_UPDATE_ACL);
      }
    }
  }
  if (accepted) rebuild_authorized_content(self);
  return accepted;
}

static gboolean message_store_has(GListStore *store, const char *id) {
  guint n = g_list_model_get_n_items(G_LIST_MODEL(store));
  for (guint i = 0; i < n; i++) {
    g_autoptr(GnCommunikeysMessageItem) item =
      g_list_model_get_item(G_LIST_MODEL(store), i);
    if (g_strcmp0(gn_communikeys_message_item_get_id(item), id) == 0)
      return TRUE;
  }
  return FALSE;
}

static void message_store_insert_sorted(GListStore *store,
                                        GnCommunikeysMessageItem *item) {
  guint n = g_list_model_get_n_items(G_LIST_MODEL(store));
  guint position = n;
  for (guint i = 0; i < n; i++) {
    g_autoptr(GnCommunikeysMessageItem) current =
      g_list_model_get_item(G_LIST_MODEL(store), i);
    gint64 a = gn_communikeys_message_item_get_created_at(item);
    gint64 b = gn_communikeys_message_item_get_created_at(current);
    if (a < b || (a == b &&
        g_strcmp0(gn_communikeys_message_item_get_id(item),
                  gn_communikeys_message_item_get_id(current)) < 0)) {
      position = i;
      break;
    }
  }
  g_list_store_insert(store, position, item);
}

static gboolean process_exclusive_event(GnCommunikeysCommunityService *self,
                                        NostrEvent *event,
                                        const char *event_json,
                                        gboolean emit_changed) {
  char community_id[65] = {0};
  if (nostr_communikeys_exclusive_validate(event, community_id) !=
      NOSTR_COMMUNIKEYS_OK)
    return FALSE;

  /* An h-only event does not identify a branch (V2 §Canonical Naddr): admit
   * it independently into every branch sharing the community ID whose own
   * authority admits the author — never merge or silently select. */
  gboolean accepted = FALSE;
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->states);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    CommunityState *state = value;
    if (g_strcmp0(state->community_id, community_id) != 0) continue;
    gsize section_index = 0;
    if (!state_author_can_publish(
          state, nostr_event_get_kind(event),
          nostr_event_get_pubkey(event), &section_index))
      continue;
    g_autofree gchar *id = nostr_event_get_id(event);
    if (!id || message_store_has(state->messages, id)) continue;
    const char *section_name =
      state->definition.sections[section_index].name;
    g_autoptr(GnCommunikeysMessageItem) item =
      gn_communikeys_message_item_new(
        id, event_json, nostr_event_get_created_at(event),
        nostr_event_get_kind(event), nostr_event_get_pubkey(event),
        nostr_event_get_content(event), section_name);
    message_store_insert_sorted(state->messages, item);
    accepted = TRUE;
    if (emit_changed)
      emit_update(self, state->address, GN_COMMUNIKEYS_UPDATE_EXCLUSIVE);
  }
  return accepted;
}

static gboolean targeted_store_insert(GListStore *store,
                                      GnCommunikeysTargetedItem *item) {
  guint n = g_list_model_get_n_items(G_LIST_MODEL(store));
  for (guint i = 0; i < n; i++) {
    g_autoptr(GnCommunikeysTargetedItem) current =
      g_list_model_get_item(G_LIST_MODEL(store), i);
    if (g_strcmp0(gn_communikeys_targeted_item_get_author(current),
                  gn_communikeys_targeted_item_get_author(item)) == 0 &&
        g_strcmp0(gn_communikeys_targeted_item_get_identifier(current),
                  gn_communikeys_targeted_item_get_identifier(item)) == 0) {
      if (!event_is_newer(
            gn_communikeys_targeted_item_get_created_at(item),
            gn_communikeys_targeted_item_get_id(item),
            gn_communikeys_targeted_item_get_created_at(current),
            gn_communikeys_targeted_item_get_id(current)))
        return FALSE;
      g_list_store_remove(store, i);
      break;
    }
  }

  n = g_list_model_get_n_items(G_LIST_MODEL(store));
  guint position = n;
  for (guint i = 0; i < n; i++) {
    g_autoptr(GnCommunikeysTargetedItem) current =
      g_list_model_get_item(G_LIST_MODEL(store), i);
    if (gn_communikeys_targeted_item_get_created_at(item) >
        gn_communikeys_targeted_item_get_created_at(current)) {
      position = i;
      break;
    }
  }
  g_list_store_insert(store, position, item);
  return TRUE;
}

static gboolean process_target_event(GnCommunikeysCommunityService *self,
                                     NostrEvent *event,
                                     gboolean emit_changed) {
  nostr_communikeys_targeted_publication_t publication;
  if (nostr_communikeys_targeted_publication_parse(event, &publication) !=
      NOSTR_COMMUNIKEYS_OK)
    return FALSE;
  if (publication.original_kind == 9 ||
      publication.original_kind == 11 ||
      publication.original_kind == CAS_TARGETED_PUBLICATION) {
    nostr_communikeys_targeted_publication_clear(&publication);
    return FALSE;
  }
  NostrEvent *original = resolve_original(self, event, &publication);
  if (!original ||
      nostr_communikeys_targeted_publication_validate(event, original) !=
        NOSTR_COMMUNIKEYS_OK) {
    nostr_communikeys_targeted_publication_clear(&publication);
    return FALSE;
  }

  /* Curator wrappers are valid: authorize the ORIGINAL author's grants in
   * each targeted branch, not the wrapper signer's. */
  const char *original_author = nostr_event_get_pubkey(original);
  g_autofree gchar *id = nostr_event_get_id(event);
  gboolean accepted = FALSE;
  for (gsize i = 0; i < publication.targets_len; i++) {
    char *address_raw =
      nostr_communikeys_branch_format(&publication.targets[i].branch);
    if (!address_raw) continue;
    CommunityState *state = find_state(self, address_raw);
    free(address_raw);
    gsize section_index = 0;
    if (!state || !state_author_can_publish(
          state, publication.original_kind,
          original_author, &section_index))
      continue;
    g_autoptr(GnCommunikeysTargetedItem) item =
      gn_communikeys_targeted_item_new(
        id, publication.identifier, nostr_event_get_created_at(event),
        publication.curator,
        publication.has_source ? publication.reference : "(sourceless)",
        publication.original_kind, nostr_event_get_content(original),
        state->definition.sections[section_index].name,
        (guint)publication.targets_len);
    if (targeted_store_insert(state->targets, item)) {
      accepted = TRUE;
      if (emit_changed)
        emit_update(self, state->address, GN_COMMUNIKEYS_UPDATE_TARGETED);
    }
  }
  nostr_communikeys_targeted_publication_clear(&publication);
  return accepted;
}

static void clear_store(GListStore *store) {
  while (g_list_model_get_n_items(G_LIST_MODEL(store)) > 0)
    g_list_store_remove(store, 0);
}

static void rebuild_authorized_content(GnCommunikeysCommunityService *self) {
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->states);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    CommunityState *state = value;
    clear_store(state->messages);
    clear_store(state->targets);
  }

  for (guint i = 0; i < self->raw_exclusive->len; i++) {
    RawEvent *raw = g_ptr_array_index(self->raw_exclusive, i);
    g_autoptr(NostrEvent) event = NULL;
    if (parse_verified_event(raw->json, &event, NULL))
      process_exclusive_event(self, event, raw->json, FALSE);
  }
  for (guint i = 0; i < self->raw_targets->len; i++) {
    RawEvent *raw = g_ptr_array_index(self->raw_targets, i);
    g_autoptr(NostrEvent) event = NULL;
    if (parse_verified_event(raw->json, &event, NULL))
      process_target_event(self, event, FALSE);
  }
}

gboolean gn_communikeys_community_service_ingest_event(
    GnCommunikeysCommunityService *self, const char *event_json) {
  g_return_val_if_fail(GN_IS_COMMUNIKEYS_COMMUNITY_SERVICE(self), FALSE);
  if (self->shutting_down) return FALSE;
  g_autoptr(NostrEvent) event = NULL;
  g_autofree gchar *event_id = NULL;
  if (!parse_verified_event(event_json, &event, &event_id))
    return FALSE;

  gboolean accepted = FALSE;
  switch (nostr_event_get_kind(event)) {
    case CAS_COMMUNITY_DEFINITION:
      accepted = ingest_definition(self, event, event_id);
      break;
    case NOSTR_COMMUNIKEYS_KIND_PROFILE_LIST:
      accepted = ingest_acl(self, event, event_id);
      cache_original(self, event);
      break;
    case 9:
    case 11:
      if (raw_cache_add(self->raw_exclusive, event_id, event_json))
        accepted = process_exclusive_event(self, event, event_json, TRUE);
      break;
    case CAS_TARGETED_PUBLICATION:
      raw_cache_add(self->raw_targets, event_id, event_json);
      /* Retry cached records on exact refresh: the NIP recommends
       * publishing kind 30222 before its referenced original. */
      accepted = process_target_event(self, event, TRUE);
      break;
    default:
      cache_original(self, event);
      rebuild_authorized_content(self);
      accepted = TRUE;
      break;
  }
  return accepted;
}

static void on_event(const char *event_json, gpointer user_data) {
  GnCommunikeysCommunityService *self =
    GN_COMMUNIKEYS_COMMUNITY_SERVICE(user_data);
  if (!self->shutting_down)
    gn_communikeys_community_service_ingest_event(self, event_json);
}

static void query_and_ingest(GnCommunikeysCommunityService *self,
                             const char *filter) {
  if (!self->context || self->shutting_down) return;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) events = gnostr_plugin_context_query_events(
    self->context, filter, &error);
  if (!events) {
    if (error) emit_error(self, error->message);
    return;
  }
  for (guint i = 0; i < events->len; i++)
    gn_communikeys_community_service_ingest_event(
      self, g_ptr_array_index(events, i));
}

void gn_communikeys_community_service_refresh_community(
    GnCommunikeysCommunityService *self, const char *definition_address) {
  g_return_if_fail(GN_IS_COMMUNIKEYS_COMMUNITY_SERVICE(self));
  CommunityState *state = find_state(self, definition_address);
  if (!state || !self->context || self->shutting_down) return;

  for (gsize i = 0; i < state->definition.sections_len; i++) {
    nostr_communikeys_section_t *section = &state->definition.sections[i];
    for (gsize j = 0; j < section->profile_lists_len; j++) {
      int kind = NOSTR_COMMUNIKEYS_KIND_PROFILE_LIST;
      g_autofree gchar *filter = build_filter(
        &kind, 1, section->profile_lists[j].coordinate.pubkey, "#d",
        section->profile_lists[j].coordinate.identifier, 20);
      query_and_ingest(self, filter);
    }
  }
  const int exclusive_kinds[] = {9, 11};
  g_autofree gchar *exclusive = build_filter(
    exclusive_kinds, G_N_ELEMENTS(exclusive_kinds), NULL,
    "#h", state->community_id, MAX_CACHED_EVENTS);
  query_and_ingest(self, exclusive);

  /* V2 wrappers are discovered by #h=<communityId>, never by #p. */
  int target_kind = CAS_TARGETED_PUBLICATION;
  g_autofree gchar *targets = build_filter(
    &target_kind, 1, NULL, "#h", state->community_id, MAX_CACHED_EVENTS);
  query_and_ingest(self, targets);
}

void gn_communikeys_community_service_refresh(
    GnCommunikeysCommunityService *self) {
  g_return_if_fail(GN_IS_COMMUNIKEYS_COMMUNITY_SERVICE(self));
  if (!self->context || self->shutting_down) return;
  int kind = CAS_COMMUNITY_DEFINITION;
  g_autofree gchar *filter = build_filter(&kind, 1, NULL, NULL, NULL, 200);
  query_and_ingest(self, filter);
}

static void subscribe(GnCommunikeysCommunityService *self) {
  self->definition_subscription = gnostr_plugin_context_subscribe_events(
    self->context, "{\"kinds\":[32222]}",
    G_CALLBACK(on_event), self, NULL);
  self->acl_subscription = gnostr_plugin_context_subscribe_events(
    self->context, "{\"kinds\":[30000]}",
    G_CALLBACK(on_event), self, NULL);
  self->content_subscription = gnostr_plugin_context_subscribe_events(
    self->context, "{\"kinds\":[9,11,30222]}",
    G_CALLBACK(on_event), self, NULL);
}

void gn_communikeys_community_service_shutdown(
    GnCommunikeysCommunityService *self) {
  g_return_if_fail(GN_IS_COMMUNIKEYS_COMMUNITY_SERVICE(self));
  if (self->shutting_down) return;
  self->shutting_down = TRUE;
  if (self->context) {
    if (self->definition_subscription)
      gnostr_plugin_context_unsubscribe_events(
        self->context, self->definition_subscription);
    if (self->acl_subscription)
      gnostr_plugin_context_unsubscribe_events(
        self->context, self->acl_subscription);
    if (self->content_subscription)
      gnostr_plugin_context_unsubscribe_events(
        self->context, self->content_subscription);
  }
  self->definition_subscription = 0;
  self->acl_subscription = 0;
  self->content_subscription = 0;
  /* The context is borrowed from the host and may be freed immediately after
   * deactivation. Late async completions must observe NULL and cancel. */
  self->context = NULL;
}

static void gn_communikeys_community_service_dispose(GObject *object) {
  GnCommunikeysCommunityService *self =
    GN_COMMUNIKEYS_COMMUNITY_SERVICE(object);
  gn_communikeys_community_service_shutdown(self);
  g_clear_object(&self->communities);
  g_clear_pointer(&self->states, g_hash_table_unref);
  g_clear_pointer(&self->originals, g_hash_table_unref);
  g_clear_pointer(&self->raw_exclusive, g_ptr_array_unref);
  g_clear_pointer(&self->raw_targets, g_ptr_array_unref);
  self->context = NULL;
  G_OBJECT_CLASS(gn_communikeys_community_service_parent_class)->dispose(object);
}

static void gn_communikeys_community_service_finalize(GObject *object) {
  GnCommunikeysCommunityService *self =
    GN_COMMUNIKEYS_COMMUNITY_SERVICE(object);
  g_free(self->offline_user_pubkey);
  G_OBJECT_CLASS(gn_communikeys_community_service_parent_class)->finalize(object);
}

static void gn_communikeys_community_service_class_init(
    GnCommunikeysCommunityServiceClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gn_communikeys_community_service_dispose;
  object_class->finalize = gn_communikeys_community_service_finalize;
  signals[COMMUNITY_UPDATED] = g_signal_new(
    "community-updated", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_UINT);
  signals[ERROR_REPORTED] = g_signal_new(
    "error-reported", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void gn_communikeys_community_service_init(
    GnCommunikeysCommunityService *self) {
  self->communities = g_list_store_new(GN_TYPE_COMMUNIKEYS_COMMUNITY_ITEM);
  self->states = g_hash_table_new_full(
    g_str_hash, g_str_equal, g_free, community_state_free);
  self->originals = g_hash_table_new_full(
    g_str_hash, g_str_equal, g_free, (GDestroyNotify)nostr_event_free);
  self->raw_exclusive = g_ptr_array_new_with_free_func(raw_event_free);
  self->raw_targets = g_ptr_array_new_with_free_func(raw_event_free);
}

GnCommunikeysCommunityService *gn_communikeys_community_service_new(
    GnostrPluginContext *context) {
  g_return_val_if_fail(context != NULL, NULL);
  GnCommunikeysCommunityService *self = g_object_new(
    GN_TYPE_COMMUNIKEYS_COMMUNITY_SERVICE, NULL);
  self->context = context;
  subscribe(self);
  gn_communikeys_community_service_refresh(self);
  return self;
}

GnCommunikeysCommunityService *
gn_communikeys_community_service_new_offline(const char *user_pubkey) {
  GnCommunikeysCommunityService *self = g_object_new(
    GN_TYPE_COMMUNIKEYS_COMMUNITY_SERVICE, NULL);
  self->offline_user_pubkey = g_strdup(user_pubkey);
  return self;
}

GListModel *gn_communikeys_community_service_get_model(
    GnCommunikeysCommunityService *self) {
  g_return_val_if_fail(GN_IS_COMMUNIKEYS_COMMUNITY_SERVICE(self), NULL);
  return G_LIST_MODEL(self->communities);
}

GnCommunikeysCommunityItem *
gn_communikeys_community_service_lookup_community(
    GnCommunikeysCommunityService *self, const char *definition_address) {
  g_return_val_if_fail(GN_IS_COMMUNIKEYS_COMMUNITY_SERVICE(self), NULL);
  CommunityState *state = find_state(self, definition_address);
  return state && state->item ? g_object_ref(state->item) : NULL;
}

GListModel *gn_communikeys_community_service_get_messages(
    GnCommunikeysCommunityService *self, const char *definition_address) {
  g_return_val_if_fail(GN_IS_COMMUNIKEYS_COMMUNITY_SERVICE(self), NULL);
  CommunityState *state = find_state(self, definition_address);
  return state ? G_LIST_MODEL(state->messages) : NULL;
}

GListModel *gn_communikeys_community_service_get_targets(
    GnCommunikeysCommunityService *self, const char *definition_address) {
  g_return_val_if_fail(GN_IS_COMMUNIKEYS_COMMUNITY_SERVICE(self), NULL);
  CommunityState *state = find_state(self, definition_address);
  return state ? G_LIST_MODEL(state->targets) : NULL;
}

const char *gn_communikeys_community_service_get_current_pubkey(
    GnCommunikeysCommunityService *self) {
  g_return_val_if_fail(GN_IS_COMMUNIKEYS_COMMUNITY_SERVICE(self), NULL);
  if (self->offline_user_pubkey) return self->offline_user_pubkey;
  return self->context
    ? gnostr_plugin_context_get_user_pubkey(self->context) : NULL;
}

gboolean gn_communikeys_community_service_author_can_publish(
    GnCommunikeysCommunityService *self, const char *definition_address,
    int kind, const char *author_pubkey) {
  g_return_val_if_fail(GN_IS_COMMUNIKEYS_COMMUNITY_SERVICE(self), FALSE);
  return state_author_can_publish(
    find_state(self, definition_address), kind, author_pubkey, NULL);
}

static gboolean validate_signed_publication(
    GnCommunikeysCommunityService *self, const char *signed_json,
    GError **error) {
  g_autoptr(NostrEvent) event = NULL;
  if (!parse_verified_event(signed_json, &event, NULL)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Signer returned an invalid event");
    return FALSE;
  }
  const char *current =
    gn_communikeys_community_service_get_current_pubkey(self);
  if (!current ||
      g_strcmp0(current, nostr_event_get_pubkey(event)) != 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                "Signer identity changed");
    return FALSE;
  }

  int kind = nostr_event_get_kind(event);
  if (kind == 9 || kind == 11) {
    char community_id[65] = {0};
    if (nostr_communikeys_exclusive_validate(event, community_id) !=
        NOSTR_COMMUNIKEYS_OK) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "Signer changed the community scope tag");
      return FALSE;
    }
    /* At least one branch sharing this community ID must still admit the
     * signer. */
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, self->states);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
      CommunityState *state = value;
      if (g_strcmp0(state->community_id, community_id) == 0 &&
          state_author_can_publish(state, kind, current, NULL))
        return TRUE;
    }
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                "Current ACL no longer permits this publication");
    return FALSE;
  }
  if (kind == CAS_TARGETED_PUBLICATION) {
    nostr_communikeys_targeted_publication_t publication;
    if (nostr_communikeys_targeted_publication_parse(event, &publication) !=
        NOSTR_COMMUNIKEYS_OK) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "Signer changed the targeting record");
      return FALSE;
    }
    gboolean nonexclusive =
      publication.original_kind != 9 &&
      publication.original_kind != 11 &&
      publication.original_kind != CAS_TARGETED_PUBLICATION;
    NostrEvent *original =
      nonexclusive ? resolve_original(self, event, &publication) : NULL;
    gboolean valid = original &&
      nostr_communikeys_targeted_publication_validate(event, original) ==
        NOSTR_COMMUNIKEYS_OK;
    /* Curator model: the ORIGINAL author's grants gate every target. */
    const char *original_author =
      original ? nostr_event_get_pubkey(original) : NULL;
    for (gsize i = 0; valid && i < publication.targets_len; i++) {
      char *address =
        nostr_communikeys_branch_format(&publication.targets[i].branch);
      valid = address != NULL &&
        gn_communikeys_community_service_author_can_publish(
          self, address, publication.original_kind, original_author);
      free(address);
    }
    nostr_communikeys_targeted_publication_clear(&publication);
    if (!valid) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                  "Current ACL no longer permits one or more targets");
      return FALSE;
    }
    return TRUE;
  }
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
              "Unexpected Communikeys publication kind");
  return FALSE;
}

static void on_publish_done(GObject *source, GAsyncResult *result,
                            gpointer user_data) {
  (void)source;
  GTask *task = G_TASK(user_data);
  GnCommunikeysCommunityService *self = g_task_get_source_object(task);
  if (self->shutting_down || !self->context) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            "Communikeys service was deactivated");
    g_object_unref(task);
    return;
  }
  g_autoptr(GError) error = NULL;
  if (!gnostr_plugin_context_publish_event_finish(
        self->context, result, &error))
    g_task_return_error(task, g_steal_pointer(&error));
  else
    g_task_return_boolean(task, TRUE);
  g_object_unref(task);
}

static void on_signed(GObject *source, GAsyncResult *result,
                      gpointer user_data) {
  (void)source;
  GTask *task = G_TASK(user_data);
  GnCommunikeysCommunityService *self = g_task_get_source_object(task);
  if (self->shutting_down || !self->context) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            "Communikeys service was deactivated");
    g_object_unref(task);
    return;
  }
  g_autoptr(GError) error = NULL;
  g_autofree gchar *signed_json =
    gnostr_plugin_context_request_sign_event_finish(
      self->context, result, &error);
  if (!signed_json ||
      !validate_signed_publication(self, signed_json, &error)) {
    if (error) g_task_return_error(task, g_steal_pointer(&error));
    else g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                 "Failed to sign Communikeys event");
    g_object_unref(task);
    return;
  }
  gnostr_plugin_context_publish_event_async(
    self->context, signed_json, g_task_get_cancellable(task),
    on_publish_done, task);
}

static void sign_and_publish(GnCommunikeysCommunityService *self,
                             NostrEvent *event, GCancellable *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer user_data) {
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  if (self->shutting_down || !self->context) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CLOSED,
                            "Communikeys service is not connected");
    g_object_unref(task);
    return;
  }
  char *unsigned_json = nostr_event_serialize_compact(event);
  if (!unsigned_json) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Failed to serialize Communikeys event");
    g_object_unref(task);
    return;
  }
  gnostr_plugin_context_request_sign_event(
    self->context, unsigned_json, cancellable, on_signed, task);
  free(unsigned_json);
}

static void return_publish_error(GnCommunikeysCommunityService *self,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data,
                                 GIOErrorEnum code,
                                 const char *message) {
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_return_new_error(task, G_IO_ERROR, code, "%s", message);
  g_object_unref(task);
}

void gn_communikeys_community_service_publish_exclusive_async(
    GnCommunikeysCommunityService *self, const char *definition_address,
    int kind, const char *content, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data) {
  g_return_if_fail(GN_IS_COMMUNIKEYS_COMMUNITY_SERVICE(self));
  const char *author =
    gn_communikeys_community_service_get_current_pubkey(self);
  CommunityState *state = find_state(self, definition_address);
  if (!author || !content || !*content || (kind != 9 && kind != 11) ||
      !state ||
      !gn_communikeys_community_service_author_can_publish(
        self, definition_address, kind, author)) {
    return_publish_error(
      self, cancellable, callback, user_data,
      G_IO_ERROR_PERMISSION_DENIED,
      "A verified section ACL does not permit this publication");
    return;
  }

  /* The branch address resolves authority; the event carries the OPAQUE
   * community ID as its exactly-one h tag. */
  g_autoptr(NostrEvent) event = nostr_event_new();
  nostr_event_set_kind(event, kind);
  nostr_event_set_pubkey(event, author);
  nostr_event_set_created_at(event, (gint64)time(NULL));
  nostr_event_set_content(event, content);
  if (!nostr_communikeys_exclusive_add_h(event, state->community_id)) {
    return_publish_error(self, cancellable, callback, user_data,
                         G_IO_ERROR_INVALID_ARGUMENT,
                         "Invalid Communikeys community identifier");
    return;
  }
  sign_and_publish(self, event, cancellable, callback, user_data);
}

gboolean gn_communikeys_community_service_publish_exclusive_finish(
    GnCommunikeysCommunityService *self, GAsyncResult *result,
    GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

void gn_communikeys_community_service_publish_target_async(
    GnCommunikeysCommunityService *self,
    const nostr_communikeys_targeted_publication_t *publication,
    GCancellable *cancellable, GAsyncReadyCallback callback,
    gpointer user_data) {
  g_return_if_fail(GN_IS_COMMUNIKEYS_COMMUNITY_SERVICE(self));
  const char *author =
    gn_communikeys_community_service_get_current_pubkey(self);
  /* The wrapper signer is the curator: it MAY differ from the original
   * author, but it must be the current user. */
  if (!publication || !author ||
      (publication->curator && g_strcmp0(publication->curator, author) != 0) ||
      publication->original_kind == 9 ||
      publication->original_kind == 11 ||
      publication->original_kind == CAS_TARGETED_PUBLICATION ||
      publication->targets_len == 0 ||
      publication->targets_len > NOSTR_COMMUNIKEYS_MAX_TARGETS) {
    return_publish_error(self, cancellable, callback, user_data,
                         G_IO_ERROR_INVALID_ARGUMENT,
                         "Invalid targeted-publication request");
    return;
  }

  /* Fill relay hints from each targeted branch's main relay and dedupe on
   * the exact definition address. */
  g_autofree nostr_communikeys_community_target_t *targets =
    g_new0(nostr_communikeys_community_target_t, publication->targets_len);
  GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  gboolean known = TRUE;
  for (gsize i = 0; i < publication->targets_len; i++) {
    char *address =
      nostr_communikeys_branch_format(&publication->targets[i].branch);
    CommunityState *state = address ? find_state(self, address) : NULL;
    if (!state || g_hash_table_contains(seen, address)) {
      free(address);
      known = FALSE;
      break;
    }
    g_hash_table_add(seen, g_strdup(address));
    free(address);
    targets[i] = publication->targets[i];
    if (!targets[i].relay)
      targets[i].relay = (char *)
        gn_communikeys_community_item_get_main_relay(state->item);
  }
  g_hash_table_unref(seen);
  if (!known) {
    return_publish_error(self, cancellable, callback, user_data,
                         G_IO_ERROR_INVALID_ARGUMENT,
                         "Unknown or duplicate community branch target");
    return;
  }

  nostr_communikeys_targeted_publication_t normalized = *publication;
  normalized.curator = (char *)author;
  normalized.targets = targets;
  g_autoptr(NostrEvent) probe =
    nostr_communikeys_targeted_publication_to_event(
      &normalized, (gint64)time(NULL));
  if (!probe) {
    return_publish_error(self, cancellable, callback, user_data,
                         G_IO_ERROR_INVALID_ARGUMENT,
                         "Invalid kind-30222 targeting record");
    return;
  }
  NostrEvent *original = resolve_original(self, probe, &normalized);
  if (!original ||
      nostr_communikeys_targeted_publication_validate(probe, original) !=
        NOSTR_COMMUNIKEYS_OK) {
    return_publish_error(self, cancellable, callback, user_data,
                         G_IO_ERROR_INVALID_DATA,
                         "The referenced original could not be verified");
    return;
  }

  /* Authorize the ORIGINAL author against every targeted branch. */
  const char *original_author = nostr_event_get_pubkey(original);
  for (gsize i = 0; i < publication->targets_len; i++) {
    char *address =
      nostr_communikeys_branch_format(&publication->targets[i].branch);
    gboolean permitted = address != NULL &&
      gn_communikeys_community_service_author_can_publish(
        self, address, publication->original_kind, original_author);
    free(address);
    if (!permitted) {
      return_publish_error(self, cancellable, callback, user_data,
                           G_IO_ERROR_PERMISSION_DENIED,
                           "A verified ACL does not permit one or more targets");
      return;
    }
  }

  sign_and_publish(self, probe, cancellable, callback, user_data);
}

gboolean gn_communikeys_community_service_publish_target_finish(
    GnCommunikeysCommunityService *self, GAsyncResult *result,
    GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}
