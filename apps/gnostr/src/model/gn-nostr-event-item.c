#include "gn-nostr-event-item.h"
#include "../storage_ndb.h"
#include <string.h>
#include "nostr_json.h"
#include <json.h>

struct _GnNostrEventItem {
  GObject parent_instance;

  /* Primary identifier - nostrdb note key */
  uint64_t note_key;

  /* Cached values for sorting and display (avoid repeated txn opens) */
  gint64 created_at;
  gint kind;

  /* Lazy-loaded cached strings (populated on first access) */
  char *cached_event_id;
  char *cached_pubkey;
  char *cached_content;
  char *cached_tags_json;  /* NIP-92 imeta support */
  char **cached_hashtags;  /* "t" tags extracted directly */

  /* Profile object */
  GnNostrProfile *profile;

  /* Thread info (stored, not fetched from nostrdb) */
  char *thread_root_id;
  char *parent_id;
  guint reply_depth;

  gboolean is_root;
  gboolean is_reply;
  gboolean is_repost;
  gboolean is_muted;

  /* nostrc-7o7: Skip animation for notes added outside visible viewport */
  gboolean skip_animation;

  /* nostrc-0hp Phase 3: Reveal animation state for "New Notes" button */
  gboolean revealing;

  /* NIP-25: Reaction count (likes) */
  guint like_count;
  gboolean is_liked;  /* Whether current user has liked this event */

  /* NIP-18: Repost count */
  guint repost_count;

  /* NIP-57: Zap stats */
  guint zap_count;
  gint64 zap_total_msat;

  /* NIP-40: Expiration timestamp (cached) */
  gint64 expiration;
  gboolean expiration_loaded;
};

G_DEFINE_TYPE(GnNostrEventItem, gn_nostr_event_item, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_EVENT_ID,
  PROP_PUBKEY,
  PROP_CREATED_AT,
  PROP_CONTENT,
  PROP_KIND,
  PROP_PROFILE,
  PROP_THREAD_ROOT_ID,
  PROP_PARENT_ID,
  PROP_REPLY_DEPTH,
  PROP_IS_ROOT,
  PROP_IS_REPLY,
  PROP_IS_REPOST,
  PROP_IS_MUTED,
  PROP_SKIP_ANIMATION,
  PROP_REVEALING,
  PROP_LIKE_COUNT,
  PROP_IS_LIKED,
  PROP_REPOST_COUNT,
  PROP_ZAP_COUNT,
  PROP_ZAP_TOTAL_MSAT,
  PROP_EXPIRATION,
  PROP_IS_EXPIRED,
  N_PROPS
};

static GParamSpec *properties[N_PROPS];

/* Helper: Load note data from nostrdb and cache it */
static gboolean ensure_note_loaded(GnNostrEventItem *self)
{
  if (self->note_key == 0) return FALSE;
  if (self->cached_event_id != NULL) return TRUE;  /* Already loaded */

  /* Non-blocking: try once, no retries with sleep.  This runs on the GTK
   * main thread during property access (GtkListView bind).  Blocking here
   * with retry+sleep stalls the entire UI.  If NDB is busy the data will
   * be populated later when the item is re-bound or metadata batch runs. */
  void *txn = NULL;
  if (storage_ndb_begin_query(&txn) != 0 || !txn) {
    return FALSE;
  }

  storage_ndb_note *note = storage_ndb_get_note_ptr(txn, self->note_key);
  if (!note) {
    g_warning("[ITEM] ensure_note_loaded: note not found in DB for key %lu", (unsigned long)self->note_key);
  }
  if (note) {
    /* Cache event ID */
    const unsigned char *id = storage_ndb_note_id(note);
    if (id) {
      self->cached_event_id = g_malloc(65);
      storage_ndb_hex_encode(id, self->cached_event_id);
    }

    /* Cache pubkey */
    const unsigned char *pk = storage_ndb_note_pubkey(note);
    if (pk) {
      self->cached_pubkey = g_malloc(65);
      storage_ndb_hex_encode(pk, self->cached_pubkey);
    }

    /* Cache content */
    const char *content = storage_ndb_note_content(note);
    if (content) {
      uint32_t len = storage_ndb_note_content_length(note);
      self->cached_content = g_strndup(content, len);
    }

    /* Cache kind and created_at */
    self->kind = (gint)storage_ndb_note_kind(note);
    self->created_at = (gint64)storage_ndb_note_created_at(note);
    self->is_repost = (self->kind == 6);

    /* Cache tags JSON for NIP-92 imeta support */
    /* DISABLED: Tag JSON generation causes heap corruption with malformed events */
    self->cached_tags_json = NULL; // storage_ndb_note_tags_json(note);

    /* Extract hashtags directly (avoids the corruption from full tags_json) */
    self->cached_hashtags = storage_ndb_note_get_hashtags(note);

    /* NIP-40: Cache expiration timestamp */
    self->expiration = storage_ndb_note_get_expiration(note);
    self->expiration_loaded = TRUE;
  }

  storage_ndb_end_query(txn);
  return (self->cached_event_id != NULL);
}

/* Helper: Ensure expiration is loaded (may load full note if needed) */
static void ensure_expiration_loaded(GnNostrEventItem *self)
{
  if (self->expiration_loaded) return;

  /* Try to load just the expiration without full note load */
  if (self->note_key == 0) {
    self->expiration_loaded = TRUE;
    return;
  }

  /* Load full note data which includes expiration */
  ensure_note_loaded(self);
}

/* nostrc-slot: Populate item data from an existing note pointer.
 * This is called during batch processing while the transaction is still open,
 * avoiding the need to open a new transaction later in ensure_note_loaded().
 * This is the key optimization to prevent LMDB reader slot exhaustion. */
void gn_nostr_event_item_populate_from_note(GnNostrEventItem *self, struct ndb_note *note)
{
  g_return_if_fail(GN_IS_NOSTR_EVENT_ITEM(self));
  g_return_if_fail(note != NULL);

  /* If already loaded, don't overwrite */
  if (self->cached_event_id != NULL) return;

  /* Cache event ID */
  const unsigned char *id = storage_ndb_note_id((storage_ndb_note *)note);
  if (id) {
    self->cached_event_id = g_malloc(65);
    storage_ndb_hex_encode(id, self->cached_event_id);
  }

  /* Cache pubkey */
  const unsigned char *pk = storage_ndb_note_pubkey((storage_ndb_note *)note);
  if (pk) {
    self->cached_pubkey = g_malloc(65);
    storage_ndb_hex_encode(pk, self->cached_pubkey);
  }

  /* Cache content */
  const char *content = storage_ndb_note_content((storage_ndb_note *)note);
  if (content) {
    uint32_t len = storage_ndb_note_content_length((storage_ndb_note *)note);
    self->cached_content = g_strndup(content, len);
  }

  /* Cache kind and created_at */
  self->kind = (gint)storage_ndb_note_kind((storage_ndb_note *)note);
  self->created_at = (gint64)storage_ndb_note_created_at((storage_ndb_note *)note);
  self->is_repost = (self->kind == 6);

  /* Cache tags JSON for NIP-92 imeta support */
  /* DISABLED: Tag JSON generation causes heap corruption with malformed events */
  self->cached_tags_json = NULL;

  /* Extract hashtags directly */
  self->cached_hashtags = storage_ndb_note_get_hashtags((storage_ndb_note *)note);

  /* NIP-40: Cache expiration timestamp */
  self->expiration = storage_ndb_note_get_expiration((storage_ndb_note *)note);
  self->expiration_loaded = TRUE;
}

static void gn_nostr_event_item_finalize(GObject *object) {
  GnNostrEventItem *self = GN_NOSTR_EVENT_ITEM(object);

  g_free(self->cached_event_id);
  g_free(self->cached_pubkey);
  g_free(self->cached_content);
  g_free(self->cached_tags_json);
  g_strfreev(self->cached_hashtags);
  g_free(self->thread_root_id);
  g_free(self->parent_id);

  g_clear_object(&self->profile);

  G_OBJECT_CLASS(gn_nostr_event_item_parent_class)->finalize(object);
}

static void gn_nostr_event_item_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {
  GnNostrEventItem *self = GN_NOSTR_EVENT_ITEM(object);

  /* Ensure data is loaded for content-dependent properties */
  if (prop_id == PROP_EVENT_ID || prop_id == PROP_PUBKEY ||
      prop_id == PROP_CONTENT || prop_id == PROP_KIND) {
    ensure_note_loaded(self);
  }

  switch (prop_id) {
    case PROP_EVENT_ID:
      g_value_set_string(value, self->cached_event_id);
      break;
    case PROP_PUBKEY:
      g_value_set_string(value, self->cached_pubkey);
      break;
    case PROP_CREATED_AT:
      g_value_set_int64(value, self->created_at);
      break;
    case PROP_CONTENT:
      g_value_set_string(value, self->cached_content);
      break;
    case PROP_KIND:
      g_value_set_int(value, self->kind);
      break;
    case PROP_PROFILE:
      g_value_set_object(value, self->profile);
      break;
    case PROP_THREAD_ROOT_ID:
      g_value_set_string(value, self->thread_root_id);
      break;
    case PROP_PARENT_ID:
      g_value_set_string(value, self->parent_id);
      break;
    case PROP_REPLY_DEPTH:
      g_value_set_uint(value, self->reply_depth);
      break;
    case PROP_IS_ROOT:
      g_value_set_boolean(value, self->is_root);
      break;
    case PROP_IS_REPLY:
      g_value_set_boolean(value, self->is_reply);
      break;
    case PROP_IS_REPOST:
      g_value_set_boolean(value, self->is_repost);
      break;
    case PROP_IS_MUTED:
      g_value_set_boolean(value, self->is_muted);
      break;
    case PROP_SKIP_ANIMATION:
      g_value_set_boolean(value, self->skip_animation);
      break;
    case PROP_REVEALING:
      g_value_set_boolean(value, self->revealing);
      break;
    case PROP_LIKE_COUNT:
      g_value_set_uint(value, self->like_count);
      break;
    case PROP_IS_LIKED:
      g_value_set_boolean(value, self->is_liked);
      break;
    case PROP_REPOST_COUNT:
      g_value_set_uint(value, self->repost_count);
      break;
    case PROP_ZAP_COUNT:
      g_value_set_uint(value, self->zap_count);
      break;
    case PROP_ZAP_TOTAL_MSAT:
      g_value_set_int64(value, self->zap_total_msat);
      break;
    case PROP_EXPIRATION:
      ensure_expiration_loaded(self);
      g_value_set_int64(value, self->expiration);
      break;
    case PROP_IS_EXPIRED:
      ensure_expiration_loaded(self);
      if (self->expiration == 0) {
        g_value_set_boolean(value, FALSE);
      } else {
        gint64 now = g_get_real_time() / G_USEC_PER_SEC;
        g_value_set_boolean(value, self->expiration < now);
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void gn_nostr_event_item_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec) {
  GnNostrEventItem *self = GN_NOSTR_EVENT_ITEM(object);

  switch (prop_id) {
    case PROP_EVENT_ID:
      g_free(self->cached_event_id);
      self->cached_event_id = g_value_dup_string(value);
      break;
    case PROP_SKIP_ANIMATION:
      self->skip_animation = g_value_get_boolean(value);
      break;
    case PROP_REVEALING:
      self->revealing = g_value_get_boolean(value);
      break;
    case PROP_LIKE_COUNT:
      self->like_count = g_value_get_uint(value);
      break;
    case PROP_IS_LIKED:
      self->is_liked = g_value_get_boolean(value);
      break;
    case PROP_REPOST_COUNT:
      self->repost_count = g_value_get_uint(value);
      break;
    case PROP_ZAP_COUNT:
      self->zap_count = g_value_get_uint(value);
      break;
    case PROP_ZAP_TOTAL_MSAT:
      self->zap_total_msat = g_value_get_int64(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void gn_nostr_event_item_class_init(GnNostrEventItemClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->finalize = gn_nostr_event_item_finalize;
  object_class->get_property = gn_nostr_event_item_get_property;
  object_class->set_property = gn_nostr_event_item_set_property;

  properties[PROP_EVENT_ID] = g_param_spec_string("event-id", "Event ID", "Event ID hex", NULL,
                                                   G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  properties[PROP_PUBKEY] = g_param_spec_string("pubkey", "Pubkey", "Author pubkey", NULL,
                                                 G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_CREATED_AT] = g_param_spec_int64("created-at", "Created At", "Unix timestamp", 0, G_MAXINT64, 0,
                                                    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_CONTENT] = g_param_spec_string("content", "Content", "Event content", NULL,
                                                  G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_KIND] = g_param_spec_int("kind", "Kind", "Event kind", 0, G_MAXINT, 1,
                                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_PROFILE] = g_param_spec_object("profile", "Profile", "Author profile", GN_TYPE_NOSTR_PROFILE,
                                                  G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_THREAD_ROOT_ID] = g_param_spec_string("thread-root-id", "Thread Root ID", "Root event ID", NULL,
                                                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_PARENT_ID] = g_param_spec_string("parent-id", "Parent ID", "Parent event ID", NULL,
                                                    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_REPLY_DEPTH] = g_param_spec_uint("reply-depth", "Reply Depth", "Thread depth", 0, G_MAXUINT, 0,
                                                    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_IS_ROOT] = g_param_spec_boolean("is-root", "Is Root", "Is thread root", FALSE,
                                                   G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_IS_REPLY] = g_param_spec_boolean("is-reply", "Is Reply", "Is reply", FALSE,
                                                    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_IS_REPOST] = g_param_spec_boolean("is-repost", "Is Repost", "Is repost", FALSE,
                                                     G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_IS_MUTED] = g_param_spec_boolean("is-muted", "Is Muted", "Is muted", FALSE,
                                                    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_SKIP_ANIMATION] = g_param_spec_boolean("skip-animation", "Skip Animation",
                                                          "Skip fade-in animation (nostrc-7o7)", FALSE,
                                                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_REVEALING] = g_param_spec_boolean("revealing", "Revealing",
                                                     "Item is being revealed with animation (nostrc-0hp)", FALSE,
                                                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_LIKE_COUNT] = g_param_spec_uint("like-count", "Like Count",
                                                   "NIP-25 reaction count", 0, G_MAXUINT, 0,
                                                   G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_IS_LIKED] = g_param_spec_boolean("is-liked", "Is Liked",
                                                    "Whether current user has liked this event", FALSE,
                                                    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_REPOST_COUNT] = g_param_spec_uint("repost-count", "Repost Count",
                                                    "NIP-18 repost count", 0, G_MAXUINT, 0,
                                                    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_ZAP_COUNT] = g_param_spec_uint("zap-count", "Zap Count",
                                                  "NIP-57 zap receipt count", 0, G_MAXUINT, 0,
                                                  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_ZAP_TOTAL_MSAT] = g_param_spec_int64("zap-total-msat", "Zap Total Millisats",
                                                        "Total zap amount in millisatoshis", 0, G_MAXINT64, 0,
                                                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_EXPIRATION] = g_param_spec_int64("expiration", "Expiration",
                                                    "NIP-40 expiration Unix timestamp (0 if none)", 0, G_MAXINT64, 0,
                                                    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_IS_EXPIRED] = g_param_spec_boolean("is-expired", "Is Expired",
                                                      "Whether the event has expired (NIP-40)", FALSE,
                                                      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void gn_nostr_event_item_init(GnNostrEventItem *self) {
  self->kind = 1;
}

/* New preferred constructor - from nostrdb note key */
GnNostrEventItem *gn_nostr_event_item_new_from_key(uint64_t note_key, gint64 created_at) {
  GnNostrEventItem *self = g_object_new(GN_TYPE_NOSTR_EVENT_ITEM, NULL);
  self->note_key = note_key;
  self->created_at = created_at;
  return self;
}

uint64_t gn_nostr_event_item_get_note_key(GnNostrEventItem *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_ITEM(self), 0);
  return self->note_key;
}

/* Legacy constructor - from hex event ID */
GnNostrEventItem *gn_nostr_event_item_new(const char *event_id) {
  return g_object_new(GN_TYPE_NOSTR_EVENT_ITEM, "event-id", event_id, NULL);
}

const char *gn_nostr_event_item_get_event_id(GnNostrEventItem *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_ITEM(self), NULL);
  ensure_note_loaded(self);
  return self->cached_event_id;
}

const char *gn_nostr_event_item_get_pubkey(GnNostrEventItem *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_ITEM(self), NULL);
  ensure_note_loaded(self);
  return self->cached_pubkey;
}

gint64 gn_nostr_event_item_get_created_at(GnNostrEventItem *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_ITEM(self), 0);
  return self->created_at;
}

const char *gn_nostr_event_item_get_content(GnNostrEventItem *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_ITEM(self), NULL);
  ensure_note_loaded(self);
  return self->cached_content;
}

const char *gn_nostr_event_item_get_tags_json(GnNostrEventItem *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_ITEM(self), NULL);
  ensure_note_loaded(self);
  return self->cached_tags_json;
}

const char * const *gn_nostr_event_item_get_hashtags(GnNostrEventItem *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_ITEM(self), NULL);
  ensure_note_loaded(self);
  return (const char * const *)self->cached_hashtags;
}

gint gn_nostr_event_item_get_kind(GnNostrEventItem *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_ITEM(self), 0);
  ensure_note_loaded(self);
  return self->kind;
}

GnNostrProfile *gn_nostr_event_item_get_profile(GnNostrEventItem *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_ITEM(self), NULL);
  return self->profile;
}

const char *gn_nostr_event_item_get_thread_root_id(GnNostrEventItem *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_ITEM(self), NULL);
  return self->thread_root_id;
}

const char *gn_nostr_event_item_get_parent_id(GnNostrEventItem *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_ITEM(self), NULL);
  return self->parent_id;
}

guint gn_nostr_event_item_get_reply_depth(GnNostrEventItem *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_ITEM(self), 0);
  return self->reply_depth;
}

gboolean gn_nostr_event_item_get_is_root(GnNostrEventItem *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_ITEM(self), FALSE);
  return self->is_root;
}

gboolean gn_nostr_event_item_get_is_reply(GnNostrEventItem *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_ITEM(self), FALSE);
  return self->is_reply;
}

gboolean gn_nostr_event_item_get_is_repost(GnNostrEventItem *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_ITEM(self), FALSE);
  ensure_note_loaded(self);
  return self->is_repost;
}

gboolean gn_nostr_event_item_get_is_muted(GnNostrEventItem *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_ITEM(self), FALSE);
  return self->is_muted;
}

void gn_nostr_event_item_set_profile(GnNostrEventItem *self, GnNostrProfile *profile) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_ITEM(self));

  /* Always notify when setting profile, even if it's the same object pointer.
   * The profile object is reused and updated in place by profile_cache_update_from_content,
   * so g_set_object would return FALSE (no pointer change) and skip notification.
   * This caused timeline profile display to not update when profiles arrived. */
  g_set_object(&self->profile, profile);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_PROFILE]);
}

void gn_nostr_event_item_set_thread_info(GnNostrEventItem *self,
                                          const char *root_id,
                                          const char *parent_id,
                                          guint depth) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_ITEM(self));

  gboolean changed = FALSE;

  if (g_strcmp0(self->thread_root_id, root_id) != 0) {
    g_free(self->thread_root_id);
    self->thread_root_id = g_strdup(root_id);
    changed = TRUE;
  }

  if (g_strcmp0(self->parent_id, parent_id) != 0) {
    g_free(self->parent_id);
    self->parent_id = g_strdup(parent_id);
    changed = TRUE;
  }

  if (self->reply_depth != depth) {
    self->reply_depth = depth;
    changed = TRUE;
  }

  /* Ensure event_id is loaded for comparison */
  ensure_note_loaded(self);

  gboolean new_is_root = (root_id == NULL || g_strcmp0(self->cached_event_id, root_id) == 0);
  gboolean new_is_reply = (parent_id != NULL);

  if (self->is_root != new_is_root) {
    self->is_root = new_is_root;
    changed = TRUE;
  }

  if (self->is_reply != new_is_reply) {
    self->is_reply = new_is_reply;
    changed = TRUE;
  }

  if (changed) {
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_THREAD_ROOT_ID]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_PARENT_ID]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_REPLY_DEPTH]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_IS_ROOT]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_IS_REPLY]);
  }
}

/* Legacy function - for backward compatibility during migration */
void gn_nostr_event_item_update_from_event(GnNostrEventItem *self,
                                            const char *pubkey,
                                            gint64 created_at,
                                            const char *content,
                                            gint kind) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_ITEM(self));

  /* For items created from note_key, this is a no-op since data comes from nostrdb */
  if (self->note_key != 0) return;

  gboolean changed = FALSE;

  if (g_strcmp0(self->cached_pubkey, pubkey) != 0) {
    g_free(self->cached_pubkey);
    self->cached_pubkey = g_strdup(pubkey);
    changed = TRUE;
  }

  if (self->created_at != created_at) {
    self->created_at = created_at;
    changed = TRUE;
  }

  if (g_strcmp0(self->cached_content, content) != 0) {
    g_free(self->cached_content);
    self->cached_content = g_strdup(content);
    changed = TRUE;
  }

  if (self->kind != kind) {
    self->kind = kind;
    self->is_repost = (kind == 6);
    changed = TRUE;
  }

  if (changed) {
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_PUBKEY]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_CREATED_AT]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_CONTENT]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_KIND]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_IS_REPOST]);
  }
}

/* nostrc-7o7: Animation control for notes added outside visible viewport */
gboolean gn_nostr_event_item_get_skip_animation(GnNostrEventItem *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_ITEM(self), FALSE);
  return self->skip_animation;
}

void gn_nostr_event_item_set_skip_animation(GnNostrEventItem *self, gboolean skip) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_ITEM(self));
  if (self->skip_animation != skip) {
    self->skip_animation = skip;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_SKIP_ANIMATION]);
  }
}

/* nostrc-0hp Phase 3: Reveal animation state for "New Notes" button */
gboolean gn_nostr_event_item_get_revealing(GnNostrEventItem *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_ITEM(self), FALSE);
  return self->revealing;
}

void gn_nostr_event_item_set_revealing(GnNostrEventItem *self, gboolean revealing) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_ITEM(self));
  if (self->revealing != revealing) {
    self->revealing = revealing;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_REVEALING]);
  }
}

/* NIP-25: Reaction count support */
guint gn_nostr_event_item_get_like_count(GnNostrEventItem *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_ITEM(self), 0);
  return self->like_count;
}

void gn_nostr_event_item_set_like_count(GnNostrEventItem *self, guint count) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_ITEM(self));
  if (self->like_count != count) {
    self->like_count = count;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_LIKE_COUNT]);
  }
}

gboolean gn_nostr_event_item_get_is_liked(GnNostrEventItem *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_ITEM(self), FALSE);
  return self->is_liked;
}

void gn_nostr_event_item_set_is_liked(GnNostrEventItem *self, gboolean is_liked) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_ITEM(self));
  if (self->is_liked != is_liked) {
    self->is_liked = is_liked;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_IS_LIKED]);
  }
}

/* NIP-18: Repost count support */
guint gn_nostr_event_item_get_repost_count(GnNostrEventItem *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_ITEM(self), 0);
  return self->repost_count;
}

void gn_nostr_event_item_set_repost_count(GnNostrEventItem *self, guint count) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_ITEM(self));
  if (self->repost_count != count) {
    self->repost_count = count;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_REPOST_COUNT]);
  }
}

/* NIP-57: Zap stats support */
guint gn_nostr_event_item_get_zap_count(GnNostrEventItem *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_ITEM(self), 0);
  return self->zap_count;
}

void gn_nostr_event_item_set_zap_count(GnNostrEventItem *self, guint count) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_ITEM(self));
  if (self->zap_count != count) {
    self->zap_count = count;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_ZAP_COUNT]);
  }
}

gint64 gn_nostr_event_item_get_zap_total_msat(GnNostrEventItem *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_ITEM(self), 0);
  return self->zap_total_msat;
}

void gn_nostr_event_item_set_zap_total_msat(GnNostrEventItem *self, gint64 total_msat) {
  g_return_if_fail(GN_IS_NOSTR_EVENT_ITEM(self));
  if (self->zap_total_msat != total_msat) {
    self->zap_total_msat = total_msat;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_ZAP_TOTAL_MSAT]);
  }
}

/* NIP-40: Expiration timestamp support */
gint64 gn_nostr_event_item_get_expiration(GnNostrEventItem *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_ITEM(self), 0);
  ensure_expiration_loaded(self);
  return self->expiration;
}

gboolean gn_nostr_event_item_get_is_expired(GnNostrEventItem *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_ITEM(self), FALSE);
  ensure_expiration_loaded(self);
  if (self->expiration == 0) return FALSE;
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  return (self->expiration < now);
}

/* nostrc-3nj: Callback for iterating tags array to find "e" tag */
typedef struct {
  char *result;
} RepostTagIterData;

static gboolean repost_tag_iter_cb(gsize index, const gchar *element_json, gpointer user_data) {
  RepostTagIterData *data = (RepostTagIterData *)user_data;
  if (data->result) return false;  /* Already found, stop */

  /* Each tag is an array like ["e", "<event_id>", ...] */
  size_t arr_len = 0;
  arr_len = gnostr_json_get_array_length(element_json, NULL, NULL);
  if (arr_len < 0 || arr_len < 2) {
    return true;  /* Continue to next tag */
  }

  /* Get tag type (first element) */
  char *tag_type = NULL;
  tag_type = gnostr_json_get_array_string(element_json, NULL, 0, NULL);
  if (!tag_type) {
    return true;
  }

  if (strcmp(tag_type, "e") != 0) {
    free(tag_type);
    return true;
  }
  free(tag_type);

  /* Get event ID (second element) */
  char *event_id = NULL;
  event_id = gnostr_json_get_array_string(element_json, NULL, 1, NULL);
  if (event_id) {
    if (strlen(event_id) == 64) {
      data->result = event_id;  /* Transfer ownership */
      return false;  /* Stop iteration */
    }
    free(event_id);
  }

  return true;  /* Continue */
}

/* NIP-18: Extract referenced event ID from kind 6 repost tags.
 * Parses the cached tags JSON to find the first "e" tag and returns its value.
 * Returns newly allocated hex string or NULL if not found.
 * nostrc-3nj: Migrated from json-glib to NostrJsonInterface */
char *gn_nostr_event_item_get_reposted_event_id(GnNostrEventItem *self) {
  g_return_val_if_fail(GN_IS_NOSTR_EVENT_ITEM(self), NULL);

  ensure_note_loaded(self);

  /* Only kind 6 events are reposts */
  if (self->kind != 6) return NULL;

  /* Need tags_json to parse */
  if (!self->cached_tags_json || !*self->cached_tags_json) return NULL;

  /* Use NostrJsonInterface to iterate over tags array */
  RepostTagIterData data = { .result = NULL };
  gnostr_json_array_foreach_root(self->cached_tags_json, repost_tag_iter_cb, &data);

  /* data.result is already allocated by nostr_json_get_array_string; convert to glib-allocated */
  if (data.result) {
    char *glib_result = g_strdup(data.result);
    free(data.result);
    return glib_result;
  }
  return NULL;
}
