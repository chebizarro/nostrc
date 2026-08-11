/* The Control Plane fold (CORD-04).
 *
 * Two things are deliberately kept apart here, because conflating them is the
 * classic way to get this wrong:
 *
 *   Ingest is about *provenance*. A wrap verifying at the plane's address
 *   proves only that some control_root holder published it (CORD-02 §2) — a
 *   spam gate, never a verdict. The seal inside names the real actor.
 *
 *   The fold is about *authority*. An edition is judged by its actor's rank
 *   in the owner-rooted Roster at fold time, not at arrival, because the
 *   Grant that authorizes it may land afterwards. Authority is rejection, not
 *   prevention: anyone with the write key can publish, everyone else drops
 *   what doesn't map to a qualifying rank.
 */

#include "gn-concord-control-plane.h"

#include <json-glib/json-glib.h>
#include <nip_concord.h>
#include <nostr-event.h>
#include <nostr-tag.h>

#include <stdlib.h>
#include <string.h>

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NostrEvent, nostr_event_free)

/* The Roster resolves outward from the owner, so a round can only ever seat
 * more authority than the last. Convergence is therefore monotone and this
 * bound is a backstop against a pathological chain, not an expected depth. */
#define CONCORD_ROSTER_ROUNDS 8
/* A Ban drops its target's authority actions, and dropping those can change
 * the Roster, which can change who may ban. Two passes settle it: the second
 * folds the Roster with the first pass's bans applied. */
#define CONCORD_BAN_ROUNDS 2
/* A Registry is member-folded state like every other entity, and its content
 * is attacker-shaped the moment a creator turns hostile: bound what one
 * creator can make every member hold. Not a protocol constant — CORD-05 §5
 * sets none — but a ceiling far above any real creator's live link count. */
#define CONCORD_MAX_LINKS_PER_REGISTRY 64

typedef struct {
  guint vsk;
  gchar *eid;
  guint64 version;
  gchar *prev;     /* NULL on a first edition */
  gchar *hash;     /* the frozen CORD-04 §1 identity */
  gchar *actor;    /* the seal's npub */
  gchar *rumor_id; /* the deterministic tiebreak */
  gchar *content;  /* the entity's new state, verbatim */
  gboolean has_vac;
  gchar *vac_eid;
  guint64 vac_version;
  gchar *vac_hash;
} ControlEdition;

typedef struct {
  gchar *role_id;
  gchar *name;
  guint32 position;
  guint64 permissions;
} ControlRole;

typedef struct {
  gchar *creator;
  GPtrArray *links; /* gchar*, the link signer pubkeys, as listed */
} ControlRegistry;

struct _GnConcordControlPlane {
  gchar *community_id;
  gchar *owner;
  GHashTable *entities; /* eid -> GPtrArray(ControlEdition*) */
  GHashTable *seen;     /* rumor id -> seen */
  gboolean dirty;
  /* Rumor ids parked awaiting the Grant they cite. A set rather than a
   * counter because the fold re-runs each entity across several rounds, and
   * one parked edition must not read as several. */
  GHashTable *parked;

  /* Folded state, valid while !dirty. */
  GHashTable *roles;  /* role id -> ControlRole* */
  GHashTable *grants; /* member -> GPtrArray(gchar*) role ids */
  GHashTable *banned; /* member -> banned */
  gchar *name;
  gchar *description;
  GPtrArray *relays;   /* gchar*, NULL-terminated */
  GPtrArray *channels; /* GnConcordControlChannel* */
  /* CORD-05 §5: the Registry fold. Per creator, plus the aggregate active-set
   * that is the Public/Private source of truth. */
  GHashTable *registries;   /* creator -> ControlRegistry* */
  GPtrArray *active_links;  /* gchar*, sorted, deduplicated */
};

/* ------------------------------------------------------------------ *
 * small helpers
 * ------------------------------------------------------------------ */

static void control_edition_free(gpointer data) {
  ControlEdition *edition = data;
  if (!edition) return;
  g_free(edition->eid);
  g_free(edition->prev);
  g_free(edition->hash);
  g_free(edition->actor);
  g_free(edition->rumor_id);
  g_free(edition->content);
  g_free(edition->vac_eid);
  g_free(edition->vac_hash);
  g_free(edition);
}

static void control_role_free(gpointer data) {
  ControlRole *role = data;
  if (!role) return;
  g_free(role->role_id);
  g_free(role->name);
  g_free(role);
}

static void control_registry_free(gpointer data) {
  ControlRegistry *registry = data;
  if (!registry) return;
  g_free(registry->creator);
  g_clear_pointer(&registry->links, g_ptr_array_unref);
  g_free(registry);
}

static void control_channel_free(gpointer data) {
  GnConcordControlChannel *channel = data;
  if (!channel) return;
  g_free(channel->channel_id);
  g_free(channel->name);
  g_free(channel);
}

static NostrTag *find_tag(const NostrEvent *event, const char *key) {
  NostrTags *tags = nostr_event_get_tags(event);
  if (!tags) return NULL;
  for (gsize i = 0; i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (tag && nostr_tag_size(tag) >= 2 &&
        g_strcmp0(nostr_tag_get(tag, 0), key) == 0)
      return tag;
  }
  return NULL;
}

static const char *tag_value(const NostrEvent *event, const char *key) {
  NostrTag *tag = find_tag(event, key);
  return tag ? nostr_tag_get(tag, 1) : NULL;
}

/* Tag values are canonical decimal: no sign, no leading zeros (CORD-01
 * "Encoding"). */
static gboolean parse_decimal(const char *value, guint64 *out) {
  if (!value || !*value) return FALSE;
  if (value[0] == '0' && value[1] != '\0') return FALSE;
  return g_ascii_string_to_unsigned(value, 10, 0, G_MAXUINT64, out, NULL);
}

static NostrEvent *parse_verified_event(const char *event_json) {
  if (!event_json) return NULL;
  NostrEvent *event = nostr_event_new();
  if (!event || !nostr_event_deserialize_compact(event, event_json, NULL) ||
      nostr_event_validate(event, NULL) != NOSTR_EVENT_VALIDATION_OK) {
    nostr_event_free(event);
    return NULL;
  }
  return event;
}

static const char *object_string(JsonObject *object, const char *member) {
  if (!object || !json_object_has_member(object, member)) return NULL;
  JsonNode *node = json_object_get_member(object, member);
  if (!node || json_node_get_value_type(node) != G_TYPE_STRING) return NULL;
  return json_node_get_string(node);
}

static JsonArray *object_array(JsonObject *object, const char *member) {
  if (!object || !json_object_has_member(object, member)) return NULL;
  JsonNode *node = json_object_get_member(object, member);
  return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}

/* Typed accessors rather than json_object_get_*_member: every edition here is
 * foreign input where a mistyped member is an expected shape, not a bug worth
 * a critical. */
static gboolean object_int(JsonObject *object, const char *member,
                          gint64 *out) {
  if (!object || !json_object_has_member(object, member)) return FALSE;
  JsonNode *node = json_object_get_member(object, member);
  if (!node || json_node_get_value_type(node) != G_TYPE_INT64) return FALSE;
  *out = json_node_get_int(node);
  return TRUE;
}

static gboolean object_bool(JsonObject *object, const char *member,
                           gboolean *out) {
  if (!object || !json_object_has_member(object, member)) return FALSE;
  JsonNode *node = json_object_get_member(object, member);
  if (!node || json_node_get_value_type(node) != G_TYPE_BOOLEAN) return FALSE;
  *out = json_node_get_boolean(node);
  return TRUE;
}

/* The entity's new state, parsed. Returns NULL unless it is a JSON object;
 * the caller keeps @parser alive for the returned borrow. */
static JsonObject *edition_content_object(const char *content,
                                          JsonParser *parser) {
  if (!content || !json_parser_load_from_data(parser, content, -1, NULL))
    return NULL;
  JsonNode *root = json_parser_get_root(parser);
  return root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root)
                                              : NULL;
}

/* ------------------------------------------------------------------ *
 * ingest: wrap -> plaintext seal -> edition rumor
 * ------------------------------------------------------------------ */

gboolean gn_concord_control_plane_ingest_wrap(GnConcordControlPlane *self,
                                              const uint8_t conv_key[32],
                                              const char *address_hex,
                                              const char *wrap_json) {
  g_return_val_if_fail(self != NULL, FALSE);
  if (!conv_key || !address_hex || !wrap_json) return FALSE;

  /* Every cleanup-attributed local is declared before the first `goto`: a
   * jump over such a declaration leaves its cleanup handler facing
   * uninitialized memory. */
  gboolean accepted = FALSE;
  char *seal_json = NULL;
  g_autoptr(NostrEvent) wrap = NULL;
  g_autoptr(NostrEvent) seal = NULL;
  g_autoptr(NostrEvent) rumor = NULL;
  g_autofree gchar *rumor_id = NULL;
  const char *rumor_json = NULL;
  const char *content = NULL;
  const char *eid = NULL;
  const char *prev = NULL;
  guint64 vsk = 0, version = 0;
  uint8_t eid_bytes[32], prev_bytes[32], hash_bytes[32];
  char hash_hex[65];
  NostrTag *vac = NULL;
  ControlEdition *edition = NULL;
  GPtrArray *entity = NULL;

  wrap = parse_verified_event(wrap_json);
  if (!wrap || nostr_event_get_kind(wrap) != CONCORD_STREAM_WRAP) goto done;
  /* Only a control_root holder can mint a wrap that verifies here; a
   * regular member cannot flood the one plane everyone syncs in full. */
  if (g_strcmp0(nostr_event_get_pubkey(wrap), address_hex) != 0) goto done;

  if (nostr_concord_stream_open(conv_key, nostr_event_get_content(wrap),
                                &seal_json) != NOSTR_CONCORD_OK)
    goto done;

  /* The Control Plane's seal MUST be plaintext (kind 20014): a compaction
   * re-wraps the signed edition into a new epoch, and a signature over
   * ciphertext could not survive that re-encryption (CORD-02 §5). */
  seal = parse_verified_event(seal_json);
  if (!seal || nostr_event_get_kind(seal) != CONCORD_SEAL_PLAINTEXT) goto done;

  /* Plaintext means exactly that: the seal's content is the rumor's
   * serialized JSON, carried byte-verbatim, not a NIP-44 payload. */
  rumor_json = nostr_event_get_content(seal);
  rumor = rumor_json ? nostr_event_new() : NULL;
  if (!rumor || !nostr_event_deserialize_compact(rumor, rumor_json, NULL))
    goto done;

  if (nostr_event_get_kind(rumor) != CONCORD_KIND_CONTROL_EDITION) goto done;
  /* The seal's signature is the author proof; a rumor claiming a different
   * actor than the seal that carried it is a forgery attempt. */
  if (g_strcmp0(nostr_event_get_pubkey(rumor),
                nostr_event_get_pubkey(seal)) != 0)
    goto done;

  if (!parse_decimal(tag_value(rumor, "vsk"), &vsk) || vsk > 11) goto done;
  eid = tag_value(rumor, "eid");
  if (!nostr_concord_is_lower_hex_32(eid)) goto done;
  prev = tag_value(rumor, "ep");
  if (prev && !nostr_concord_is_lower_hex_32(prev)) goto done;

  /* The dissolution tombstone is chainless and exempt from the version
   * discipline (CORD-02 §9); every other entity carries a version that only
   * ever climbs, starting at 1, with `prev` absent on the first alone. */
  if (vsk == CONCORD_VSK_DISSOLVED) {
    version = 1;
  } else {
    if (!parse_decimal(tag_value(rumor, "ev"), &version) || version == 0)
      goto done;
    if ((version == 1) != (prev == NULL)) goto done;
  }

  content = nostr_event_get_content(rumor);
  if (!content) content = "";

  if (!nostr_concord_hex_decode_32(eid, eid_bytes)) goto done;
  if (prev && !nostr_concord_hex_decode_32(prev, prev_bytes)) goto done;
  /* The edition's identity, over the frozen preimage: what the next
   * edition's `ep` cites, and what a `vac` citation pins. */
  if (nostr_concord_edition_hash(eid_bytes, version, prev ? prev_bytes : NULL,
                                 (const uint8_t *)content, strlen(content),
                                 hash_bytes) != NOSTR_CONCORD_OK)
    goto done;
  nostr_concord_hex_encode_32(hash_bytes, hash_hex);

  rumor_id = nostr_event_get_id(rumor);
  if (!rumor_id || g_hash_table_contains(self->seen, rumor_id)) goto done;

  edition = g_new0(ControlEdition, 1);
  edition->vsk = (guint)vsk;
  edition->eid = g_strdup(eid);
  edition->version = version;
  edition->prev = g_strdup(prev);
  edition->hash = g_strdup(hash_hex);
  edition->actor = g_strdup(nostr_event_get_pubkey(seal));
  edition->rumor_id = g_strdup(rumor_id);
  edition->content = g_strdup(content);

  /* The authority citation: the exact Grant the actor claims their rank
   * under, pinned by coordinate, version *and* content hash. A sync floor,
   * never the verdict (CORD-04 §5). */
  vac = find_tag(rumor, "vac");
  if (vac && nostr_tag_size(vac) >= 4) {
    const char *vac_eid = nostr_tag_get(vac, 1);
    guint64 vac_version = 0;
    const char *vac_hash = nostr_tag_get(vac, 3);
    if (nostr_concord_is_lower_hex_32(vac_eid) &&
        parse_decimal(nostr_tag_get(vac, 2), &vac_version) &&
        nostr_concord_is_lower_hex_32(vac_hash)) {
      edition->has_vac = TRUE;
      edition->vac_eid = g_strdup(vac_eid);
      edition->vac_version = vac_version;
      edition->vac_hash = g_strdup(vac_hash);
    } else {
      /* A malformed citation is not an absent one: dropping it would
       * promote the edition to an owner-style uncited action. */
      control_edition_free(edition);
      goto done;
    }
  }

  entity = g_hash_table_lookup(self->entities, edition->eid);
  if (!entity) {
    entity = g_ptr_array_new_with_free_func(control_edition_free);
    g_hash_table_insert(self->entities, g_strdup(edition->eid), entity);
  }
  g_ptr_array_add(entity, edition);
  g_hash_table_add(self->seen, g_strdup(rumor_id));
  self->dirty = TRUE;
  accepted = TRUE;

done:
  if (seal_json) {
    memset(seal_json, 0, strlen(seal_json));
    free(seal_json);
  }
  return accepted;
}

/* ------------------------------------------------------------------ *
 * the fold
 * ------------------------------------------------------------------ */

static guint32 member_position(GnConcordControlPlane *self,
                               const char *member) {
  if (g_strcmp0(member, self->owner) == 0) return CONCORD_POSITION_OWNER;
  GPtrArray *role_ids = g_hash_table_lookup(self->grants, member);
  guint32 best = CONCORD_POSITION_LAST;
  for (guint i = 0; role_ids && i < role_ids->len; i++) {
    ControlRole *role =
      g_hash_table_lookup(self->roles, g_ptr_array_index(role_ids, i));
    if (role && role->position < best) best = role->position;
  }
  return best;
}

static guint64 member_permissions(GnConcordControlPlane *self,
                                  const char *member) {
  /* The owner is proven by the community_id itself, occupies position 0, and
   * is supreme and unremovable — which is what keeps the fold non-circular:
   * their rank comes from outside it (CORD-02 §1, CORD-04 §2). */
  if (g_strcmp0(member, self->owner) == 0) return G_MAXUINT64;
  GPtrArray *role_ids = g_hash_table_lookup(self->grants, member);
  guint64 permissions = 0;
  for (guint i = 0; role_ids && i < role_ids->len; i++) {
    ControlRole *role =
      g_hash_table_lookup(self->roles, g_ptr_array_index(role_ids, i));
    if (role) permissions |= role->permissions;
  }
  return permissions;
}

/* CORD-04 §3's one hard rule: hold the required bit *and* strictly outrank
 * the target. Equal cannot act on equal — an admin cannot ban a peer admin. */
static gboolean actor_may(GnConcordControlPlane *self, const char *actor,
                          guint64 bit) {
  return (member_permissions(self, actor) & bit) != 0;
}

static gboolean edition_is_authorized(GnConcordControlPlane *self,
                                      const ControlEdition *edition) {
  /* Every event from a banned npub is dropped — message, reaction, edit, or
   * authority action — so a banned member vanishes entirely (CORD-04 §4). */
  if (g_hash_table_contains(self->banned, edition->actor)) return FALSE;
  if (g_strcmp0(edition->actor, self->owner) == 0) return TRUE;

  guint32 actor_rank = member_position(self, edition->actor);

  switch (edition->vsk) {
  case CONCORD_VSK_METADATA:
    return actor_may(self, edition->actor, CONCORD_PERM_MANAGE_METADATA);
  case CONCORD_VSK_CHANNEL:
    return actor_may(self, edition->actor, CONCORD_PERM_MANAGE_CHANNELS);
  case CONCORD_VSK_BANLIST:
    return actor_may(self, edition->actor, CONCORD_PERM_BAN);
  case CONCORD_VSK_INVITE_REGISTRY:
    /* A Registry is honored only while its author holds CREATE_INVITE
     * (CORD-05 §5). No rank comparison: a creator only ever edits their own
     * coordinate, which the fold verifies separately. */
    return actor_may(self, edition->actor, CONCORD_PERM_CREATE_INVITE);
  case CONCORD_VSK_ROLE: {
    if (!actor_may(self, edition->actor, CONCORD_PERM_MANAGE_ROLES))
      return FALSE;
    /* No edition may claim a position at or above its own signer, so nobody
     * can promote themselves toward the top — and that binds the owner too:
     * no Role may ever claim position 0, or an owner could create a peer
     * nobody outranks. */
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonObject *object = edition_content_object(edition->content, parser);
    gint64 position = 0;
    if (!object_int(object, "position", &position)) return FALSE;
    if (position <= 0 || position > CONCORD_POSITION_LAST) return FALSE;
    return actor_rank < (guint32)position;
  }
  case CONCORD_VSK_GRANT: {
    if (!actor_may(self, edition->actor, CONCORD_PERM_MANAGE_ROLES))
      return FALSE;
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonObject *object = edition_content_object(edition->content, parser);
    const char *member = object_string(object, "member");
    if (!nostr_concord_is_lower_hex_32(member)) return FALSE;
    /* Strictly outrank the target, and outrank every Role handed out: a
     * granter honored only if they outrank what they grant. */
    if (actor_rank >= member_position(self, member)) return FALSE;
    JsonArray *role_ids = object_array(object, "role_ids");
    for (guint i = 0; role_ids && i < json_array_get_length(role_ids); i++) {
      const char *role_id = json_array_get_string_element(role_ids, i);
      ControlRole *role = role_id ? g_hash_table_lookup(self->roles, role_id)
                                  : NULL;
      if (role && actor_rank >= role->position) return FALSE;
    }
    return TRUE;
  }
  default:
    /* An entity type this client does not fold is not silently honored. */
    return FALSE;
  }
}

/* A citation whose version is unsynced — or whose hash doesn't match the
 * edition held at that version — parks the action, and it parks only its own
 * author's, so an absurd citation griefs nobody but the actor (CORD-04 §5). */
static gboolean vac_is_satisfied(GnConcordControlPlane *self,
                                 const ControlEdition *edition) {
  if (!edition->has_vac) return TRUE;
  GPtrArray *cited = g_hash_table_lookup(self->entities, edition->vac_eid);
  for (guint i = 0; cited && i < cited->len; i++) {
    const ControlEdition *candidate = g_ptr_array_index(cited, i);
    if (candidate->version == edition->vac_version &&
        g_strcmp0(candidate->hash, edition->vac_hash) == 0)
      return TRUE;
  }
  return FALSE;
}

/* A compaction re-wraps each entity's current head under a new epoch, so that
 * head's `prev` cites an edition that no longer exists there. The rule is for
 * steady state and a Refounding resets the floor: a client holding nothing at
 * the preceding version accepts the head as its baseline, while one that does
 * hold that version requires the link (CORD-04 §1). */
static gboolean chain_is_intact(GnConcordControlPlane *self,
                                const ControlEdition *edition) {
  if (!edition->prev) return TRUE;
  GPtrArray *entity = g_hash_table_lookup(self->entities, edition->eid);
  gboolean holds_predecessor = FALSE;
  for (guint i = 0; entity && i < entity->len; i++) {
    const ControlEdition *candidate = g_ptr_array_index(entity, i);
    if (candidate->version != edition->version - 1) continue;
    holds_predecessor = TRUE;
    if (g_strcmp0(candidate->hash, edition->prev) == 0) return TRUE;
  }
  return !holds_predecessor;
}

/* The head of one entity: the highest version whose chain is intact and whose
 * actor qualifies. Two authorized editions tying on version resolve by the
 * lower rumor id, never the author-settable timestamp, so every client walks
 * the same chain and lands on the same head. */
static const ControlEdition *select_head(GnConcordControlPlane *self,
                                         GPtrArray *entity, guint vsk) {
  const ControlEdition *best = NULL;
  for (guint i = 0; i < entity->len; i++) {
    const ControlEdition *candidate = g_ptr_array_index(entity, i);
    if (candidate->vsk != vsk) continue;
    if (best && candidate->version < best->version) continue;
    if (!vac_is_satisfied(self, candidate)) {
      g_hash_table_add(self->parked, g_strdup(candidate->rumor_id));
      continue;
    }
    if (!edition_is_authorized(self, candidate)) continue;
    if (!chain_is_intact(self, candidate)) continue;
    if (!best || candidate->version > best->version ||
        g_strcmp0(candidate->rumor_id, best->rumor_id) < 0)
      best = candidate;
  }
  return best;
}

static void fold_roster(GnConcordControlPlane *self) {
  guint64 fingerprint = 0;
  for (guint round = 0; round < CONCORD_ROSTER_ROUNDS; round++) {
    GHashTable *roles = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                              control_role_free);
    GHashTable *grants = g_hash_table_new_full(
      g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
    guint64 next = 0;

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, self->entities);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
      const char *eid = key;
      GPtrArray *entity = value;

      const ControlEdition *head = select_head(self, entity, CONCORD_VSK_ROLE);
      if (head) {
        g_autoptr(JsonParser) parser = json_parser_new();
        JsonObject *object = edition_content_object(head->content, parser);
        /* The coordinate is the role_id: a content field disagreeing with the
         * entity it edits is not this Role. */
        const char *role_id = object_string(object, "role_id");
        gint64 position = 0;
        if (g_strcmp0(role_id, eid) == 0 &&
            object_int(object, "position", &position) && position > 0 &&
            position <= CONCORD_POSITION_LAST) {
          ControlRole *role = g_new0(ControlRole, 1);
          role->role_id = g_strdup(eid);
          role->position = (guint32)position;
          const char *name = object_string(object, "name");
          if (name && strlen(name) <= CONCORD_MAX_NAME_BYTES)
            role->name = g_strdup(name);
          /* Permissions ride as a decimal string, never a bare number — a
           * JSON number is a 64-bit float in JavaScript and corrupts past
           * 2^53. A reader accepts either form and always writes the string. */
          const char *permissions = object_string(object, "permissions");
          gint64 legacy = 0;
          if (permissions)
            nostr_concord_parse_permissions(permissions, &role->permissions);
          else if (object_int(object, "permissions", &legacy) && legacy >= 0)
            role->permissions = (guint64)legacy;
          g_hash_table_insert(roles, g_strdup(eid), role);
          next += head->version + 1;
        }
      }

      head = select_head(self, entity, CONCORD_VSK_GRANT);
      if (head) {
        g_autoptr(JsonParser) parser = json_parser_new();
        JsonObject *object = edition_content_object(head->content, parser);
        const char *member = object_string(object, "member");
        uint8_t community[32], member_bytes[32], expected[32];
        char expected_hex[65] = { 0 };
        if (nostr_concord_is_lower_hex_32(member) &&
            nostr_concord_hex_decode_32(self->community_id, community) &&
            nostr_concord_hex_decode_32(member, member_bytes) &&
            nostr_concord_grant_locator(community, member_bytes, expected) ==
              NOSTR_CONCORD_OK)
          nostr_concord_hex_encode_32(expected, expected_hex);
        /* Each member owns exactly their own coordinate: a Grant sitting
         * anywhere but grant_locator(community_id, member) is a forgery into
         * someone else's slot, whatever its signature says. */
        if (g_strcmp0(expected_hex, eid) == 0) {
          GPtrArray *role_ids = g_ptr_array_new_with_free_func(g_free);
          JsonArray *granted = object_array(object, "role_ids");
          guint n = granted ? json_array_get_length(granted) : 0;
          if (n > CONCORD_MAX_ROLES_PER_MEMBER)
            n = CONCORD_MAX_ROLES_PER_MEMBER;
          for (guint i = 0; i < n; i++) {
            const char *role_id = json_array_get_string_element(granted, i);
            if (nostr_concord_is_lower_hex_32(role_id))
              g_ptr_array_add(role_ids, g_strdup(role_id));
          }
          /* Empty role_ids is a revoke, and must still replace the prior
           * grant rather than being dropped as uninteresting. */
          g_hash_table_insert(grants, g_strdup(member), role_ids);
          next += head->version + 1;
        }
      }
    }

    g_hash_table_unref(self->roles);
    g_hash_table_unref(self->grants);
    self->roles = roles;
    self->grants = grants;
    if (next == fingerprint) break;
    fingerprint = next;
  }

  /* A Community carries at most 100 Roles: a client folds the 100 lowest
   * role_ids and ignores the rest, so every client keeps the same set. */
  if (g_hash_table_size(self->roles) > CONCORD_MAX_ROLES_IN_COMMUNITY) {
    GList *ids = g_list_sort(g_hash_table_get_keys(self->roles),
                             (GCompareFunc)g_strcmp0);
    GPtrArray *excess = g_ptr_array_new();
    guint index = 0;
    for (GList *l = ids; l; l = l->next, index++)
      if (index >= CONCORD_MAX_ROLES_IN_COMMUNITY)
        g_ptr_array_add(excess, l->data);
    g_list_free(ids);
    for (guint i = 0; i < excess->len; i++)
      g_hash_table_remove(self->roles, g_ptr_array_index(excess, i));
    g_ptr_array_unref(excess);
  }
}

static void fold_banlist(GnConcordControlPlane *self) {
  uint8_t community[32], expected[32];
  char expected_hex[65] = { 0 };
  if (nostr_concord_hex_decode_32(self->community_id, community) &&
      nostr_concord_banlist_locator(community, expected) == NOSTR_CONCORD_OK)
    nostr_concord_hex_encode_32(expected, expected_hex);

  GPtrArray *entity = g_hash_table_lookup(self->entities, expected_hex);
  g_hash_table_remove_all(self->banned);
  if (!entity) return;

  const ControlEdition *head = select_head(self, entity, CONCORD_VSK_BANLIST);
  if (!head) return;

  /* One Community-wide entity, its content the whole list, replaced entire on
   * every edit (CORD-04 §4). */
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, head->content, -1, NULL)) return;
  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_ARRAY(root)) return;
  JsonArray *list = json_node_get_array(root);
  guint n = json_array_get_length(list);
  if (n > CONCORD_MAX_BANLIST_ENTRIES) n = CONCORD_MAX_BANLIST_ENTRIES;
  for (guint i = 0; i < n; i++) {
    const char *pubkey = json_array_get_string_element(list, i);
    /* The owner is unremovable: a Ban naming them is not a Ban. */
    if (nostr_concord_is_lower_hex_32(pubkey) &&
        g_strcmp0(pubkey, self->owner) != 0)
      g_hash_table_add(self->banned, g_strdup(pubkey));
  }
}

/* CORD-05 §5: every creator's Registry, folded into one aggregate active-set.
 *
 * The entity is bound to the creator, so the coordinate is the identity here
 * exactly as it is for a Grant: a Registry sitting anywhere but
 * invite_registry_locator(community_id, actor) is a forgery into someone
 * else's list, whatever its signature says. */
static void fold_invite_registry(GnConcordControlPlane *self) {
  g_hash_table_remove_all(self->registries);
  g_ptr_array_set_size(self->active_links, 0);

  uint8_t community[32];
  if (!nostr_concord_hex_decode_32(self->community_id, community)) return;

  g_autoptr(GHashTable) seen =
    g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->entities);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    const char *eid = key;
    const ControlEdition *head =
      select_head(self, value, CONCORD_VSK_INVITE_REGISTRY);
    if (!head) continue;

    uint8_t creator[32], expected[32];
    char expected_hex[65] = { 0 };
    if (!nostr_concord_hex_decode_32(head->actor, creator) ||
        nostr_concord_invite_registry_locator(community, creator, expected) !=
          NOSTR_CONCORD_OK)
      continue;
    nostr_concord_hex_encode_32(expected, expected_hex);
    if (g_strcmp0(expected_hex, eid) != 0) continue;

    /* The content is the live links' *coordinates* only — never tokens, URLs,
     * or signing secrets — so members can see that links exist without being
     * able to use one. */
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, head->content, -1, NULL)) continue;
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_ARRAY(root)) continue;
    JsonArray *list = json_node_get_array(root);

    ControlRegistry *registry = g_new0(ControlRegistry, 1);
    registry->creator = g_strdup(head->actor);
    registry->links = g_ptr_array_new_with_free_func(g_free);
    guint n = json_array_get_length(list);
    if (n > CONCORD_MAX_LINKS_PER_REGISTRY)
      n = CONCORD_MAX_LINKS_PER_REGISTRY;
    for (guint i = 0; i < n; i++) {
      const char *signer = json_array_get_string_element(list, i);
      if (!nostr_concord_is_lower_hex_32(signer)) continue;
      g_ptr_array_add(registry->links, g_strdup(signer));
      /* Two creators listing one coordinate is nonsense, but a hostile one
         can mint it: the aggregate is a set, so it costs a duplicate, never a
         double count. */
      if (!g_hash_table_contains(seen, signer)) {
        g_hash_table_add(seen, g_strdup(signer));
        g_ptr_array_add(self->active_links, g_strdup(signer));
      }
    }
    /* An empty Registry is a retire, and must replace the prior one rather
     * than being dropped as uninteresting — emptying the aggregate is what
     * flips the Community back to Private. */
    g_hash_table_insert(self->registries, g_strdup(registry->creator),
                        registry);
  }

  g_ptr_array_sort_values(self->active_links, (GCompareFunc)g_strcmp0);
}

static void fold_metadata(GnConcordControlPlane *self) {
  GPtrArray *entity = g_hash_table_lookup(self->entities, self->community_id);
  g_clear_pointer(&self->name, g_free);
  g_clear_pointer(&self->description, g_free);
  g_clear_pointer(&self->relays, g_ptr_array_unref);
  if (!entity) return;

  const ControlEdition *head = select_head(self, entity, CONCORD_VSK_METADATA);
  if (!head) return;

  g_autoptr(JsonParser) parser = json_parser_new();
  JsonObject *object = edition_content_object(head->content, parser);
  if (!object) return;

  /* An over-cap field is refused at the content level, like an over-cap Pin
   * List: the edition still folds and chains, it just carries nothing there.
   * Refusing the edition would fork the version chain between clients. */
  const char *name = object_string(object, "name");
  if (name && strlen(name) <= CONCORD_MAX_NAME_BYTES)
    self->name = g_strdup(name);
  const char *description = object_string(object, "description");
  if (description && strlen(description) <= CONCORD_MAX_DESCRIPTION_BYTES)
    self->description = g_strdup(description);

  /* The relay list lives here so it can evolve; a client MAY truncate a
   * longer set, and the fold MUST stay usable when it does (CORD-02 §6). */
  JsonArray *relays = object_array(object, "relays");
  if (relays) {
    self->relays = g_ptr_array_new_with_free_func(g_free);
    guint n = json_array_get_length(relays);
    if (n > CONCORD_MAX_RELAYS_IN_BUNDLE) n = CONCORD_MAX_RELAYS_IN_BUNDLE;
    for (guint i = 0; i < n; i++) {
      const char *url = json_array_get_string_element(relays, i);
      if (url && *url) g_ptr_array_add(self->relays, g_strdup(url));
    }
    g_ptr_array_add(self->relays, NULL);
  }
}

static void fold_channels(GnConcordControlPlane *self) {
  g_ptr_array_set_size(self->channels, 0);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->entities);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    const char *eid = key;
    const ControlEdition *head =
      select_head(self, value, CONCORD_VSK_CHANNEL);
    if (!head) continue;

    g_autoptr(JsonParser) parser = json_parser_new();
    JsonObject *object = edition_content_object(head->content, parser);
    if (!object) continue;

    GnConcordControlChannel *channel = g_new0(GnConcordControlChannel, 1);
    /* The channel_id is the edition's eid, never a content field: the
     * coordinate is the identity (CORD-03 §2). */
    channel->channel_id = g_strdup(eid);
    const char *name = object_string(object, "name");
    if (name && strlen(name) <= CONCORD_MAX_NAME_BYTES)
      channel->name = g_strdup(name);
    object_bool(object, "private", &channel->is_private);
    /* Deletion is terminal: the id is never reused, and a client drops the
     * Channel from display (CORD-03 §2). */
    object_bool(object, "deleted", &channel->deleted);
    g_ptr_array_add(self->channels, channel);
  }
}

static void fold(GnConcordControlPlane *self) {
  if (!self->dirty) return;
  g_hash_table_remove_all(self->parked);
  g_hash_table_remove_all(self->banned);
  for (guint round = 0; round < CONCORD_BAN_ROUNDS; round++) {
    fold_roster(self);
    fold_banlist(self);
  }
  fold_metadata(self);
  fold_channels(self);
  fold_invite_registry(self);
  self->dirty = FALSE;
}

/* ------------------------------------------------------------------ *
 * lifecycle and accessors
 * ------------------------------------------------------------------ */

GnConcordControlPlane *gn_concord_control_plane_new(
    const char *community_id_hex, const char *owner_pubkey_hex) {
  g_return_val_if_fail(nostr_concord_is_lower_hex_32(community_id_hex), NULL);
  g_return_val_if_fail(nostr_concord_is_lower_hex_32(owner_pubkey_hex), NULL);

  GnConcordControlPlane *self = g_new0(GnConcordControlPlane, 1);
  self->community_id = g_strdup(community_id_hex);
  self->owner = g_strdup(owner_pubkey_hex);
  self->entities = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         (GDestroyNotify)g_ptr_array_unref);
  self->seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->parked = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->roles = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                      control_role_free);
  self->grants = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                       (GDestroyNotify)g_ptr_array_unref);
  self->banned = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->channels = g_ptr_array_new_with_free_func(control_channel_free);
  self->registries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                           control_registry_free);
  self->active_links = g_ptr_array_new_with_free_func(g_free);
  return self;
}

void gn_concord_control_plane_free(GnConcordControlPlane *self) {
  if (!self) return;
  g_free(self->community_id);
  g_free(self->owner);
  g_hash_table_unref(self->entities);
  g_hash_table_unref(self->seen);
  g_hash_table_unref(self->parked);
  g_hash_table_unref(self->roles);
  g_hash_table_unref(self->grants);
  g_hash_table_unref(self->banned);
  g_ptr_array_unref(self->channels);
  g_hash_table_unref(self->registries);
  g_ptr_array_unref(self->active_links);
  g_clear_pointer(&self->relays, g_ptr_array_unref);
  g_free(self->name);
  g_free(self->description);
  g_free(self);
}

const char *gn_concord_control_plane_get_name(GnConcordControlPlane *self) {
  g_return_val_if_fail(self != NULL, NULL);
  fold(self);
  return self->name;
}

const char *gn_concord_control_plane_get_description(
    GnConcordControlPlane *self) {
  g_return_val_if_fail(self != NULL, NULL);
  fold(self);
  return self->description;
}

const char *const *gn_concord_control_plane_get_relays(
    GnConcordControlPlane *self, guint *n_relays) {
  g_return_val_if_fail(self != NULL, NULL);
  fold(self);
  if (n_relays) *n_relays = self->relays ? self->relays->len - 1 : 0;
  return self->relays ? (const char *const *)self->relays->pdata : NULL;
}

GPtrArray *gn_concord_control_plane_get_channels(GnConcordControlPlane *self) {
  g_return_val_if_fail(self != NULL, NULL);
  fold(self);
  return self->channels;
}

gboolean gn_concord_control_plane_is_banned(GnConcordControlPlane *self,
                                            const char *pubkey_hex) {
  g_return_val_if_fail(self != NULL, FALSE);
  if (!pubkey_hex) return FALSE;
  fold(self);
  return g_hash_table_contains(self->banned, pubkey_hex);
}

guint64 gn_concord_control_plane_get_permissions(GnConcordControlPlane *self,
                                                 const char *pubkey_hex) {
  g_return_val_if_fail(self != NULL, 0);
  fold(self);
  return pubkey_hex ? member_permissions(self, pubkey_hex) : 0;
}

guint32 gn_concord_control_plane_get_position(GnConcordControlPlane *self,
                                              const char *pubkey_hex) {
  g_return_val_if_fail(self != NULL, CONCORD_POSITION_LAST);
  fold(self);
  return pubkey_hex ? member_position(self, pubkey_hex)
                    : CONCORD_POSITION_LAST;
}

GPtrArray *gn_concord_control_plane_get_invite_links(
    GnConcordControlPlane *self) {
  g_return_val_if_fail(self != NULL, NULL);
  fold(self);
  return self->active_links;
}

GPtrArray *gn_concord_control_plane_get_creator_invite_links(
    GnConcordControlPlane *self, const char *creator_hex) {
  g_return_val_if_fail(self != NULL, NULL);
  if (!creator_hex) return NULL;
  fold(self);
  ControlRegistry *registry = g_hash_table_lookup(self->registries,
                                                  creator_hex);
  return registry ? registry->links : NULL;
}

gboolean gn_concord_control_plane_is_public(GnConcordControlPlane *self) {
  g_return_val_if_fail(self != NULL, FALSE);
  fold(self);
  return self->active_links->len > 0;
}

guint64 gn_concord_control_plane_get_registry_head(
    GnConcordControlPlane *self, const char *creator_hex,
    const char **out_hash) {
  g_return_val_if_fail(self != NULL, 0);
  if (out_hash) *out_hash = NULL;
  if (!nostr_concord_is_lower_hex_32(creator_hex)) return 0;
  fold(self);

  uint8_t community[32], creator[32], expected[32];
  char expected_hex[65];
  if (!nostr_concord_hex_decode_32(self->community_id, community) ||
      !nostr_concord_hex_decode_32(creator_hex, creator) ||
      nostr_concord_invite_registry_locator(community, creator, expected) !=
        NOSTR_CONCORD_OK)
    return 0;
  nostr_concord_hex_encode_32(expected, expected_hex);

  GPtrArray *entity = g_hash_table_lookup(self->entities, expected_hex);
  if (!entity) return 0;
  const ControlEdition *head =
    select_head(self, entity, CONCORD_VSK_INVITE_REGISTRY);
  if (!head) return 0;
  if (out_hash) *out_hash = head->hash;
  return head->version;
}

guint64 gn_concord_control_plane_get_grant_head(GnConcordControlPlane *self,
                                               const char *member_hex,
                                               const char **out_hash) {
  g_return_val_if_fail(self != NULL, 0);
  if (out_hash) *out_hash = NULL;
  if (!nostr_concord_is_lower_hex_32(member_hex)) return 0;
  fold(self);

  uint8_t community[32], member[32], expected[32];
  char expected_hex[65];
  if (!nostr_concord_hex_decode_32(self->community_id, community) ||
      !nostr_concord_hex_decode_32(member_hex, member) ||
      nostr_concord_grant_locator(community, member, expected) !=
        NOSTR_CONCORD_OK)
    return 0;
  nostr_concord_hex_encode_32(expected, expected_hex);

  GPtrArray *entity = g_hash_table_lookup(self->entities, expected_hex);
  if (!entity) return 0;
  const ControlEdition *head = select_head(self, entity, CONCORD_VSK_GRANT);
  if (!head) return 0;
  if (out_hash) *out_hash = head->hash;
  return head->version;
}

guint gn_concord_control_plane_count_editions(GnConcordControlPlane *self) {
  g_return_val_if_fail(self != NULL, 0);
  return g_hash_table_size(self->seen);
}

guint gn_concord_control_plane_count_parked(GnConcordControlPlane *self) {
  g_return_val_if_fail(self != NULL, 0);
  fold(self);
  return g_hash_table_size(self->parked);
}
