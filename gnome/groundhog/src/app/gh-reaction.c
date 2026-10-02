#include "gh-reaction.h"

#include <string.h>

/* ---- Quick-reaction set ---------------------------------------------------- */

const gchar *const gh_reaction_quick_set[GH_REACTION_QUICK_SET_SIZE] = {
  "👍", "❤️", "😂", "😮", "😢", "🙏"
};

/* ---- GhReaction ------------------------------------------------------------ */

struct _GhReaction {
  GObject parent_instance;
  gchar *target_rumor_id;
  gchar *reaction_rumor_id;
  gchar *sender_pubkey;
  gchar *emoji;
  gint64 created_at;
  gchar *room_id;
};

enum {
  PROP_REACTION_0,
  PROP_REACTION_TARGET_RUMOR_ID,
  PROP_REACTION_REACTION_RUMOR_ID,
  PROP_REACTION_SENDER,
  PROP_REACTION_EMOJI,
  PROP_REACTION_CREATED_AT,
  PROP_REACTION_ROOM_ID,
  N_REACTION_PROPS
};
static GParamSpec *reaction_props[N_REACTION_PROPS];

G_DEFINE_FINAL_TYPE(GhReaction, gh_reaction, G_TYPE_OBJECT)

static void
gh_reaction_finalize(GObject *object)
{
  GhReaction *self = GH_REACTION(object);
  g_free(self->target_rumor_id);
  g_free(self->reaction_rumor_id);
  g_free(self->sender_pubkey);
  g_free(self->emoji);
  g_free(self->room_id);
  G_OBJECT_CLASS(gh_reaction_parent_class)->finalize(object);
}

static void
gh_reaction_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  GhReaction *self = GH_REACTION(object);
  switch (prop_id) {
  case PROP_REACTION_TARGET_RUMOR_ID:
    g_value_set_string(value, self->target_rumor_id);
    break;
  case PROP_REACTION_REACTION_RUMOR_ID:
    g_value_set_string(value, self->reaction_rumor_id);
    break;
  case PROP_REACTION_SENDER:
    g_value_set_string(value, self->sender_pubkey);
    break;
  case PROP_REACTION_EMOJI:
    g_value_set_string(value, self->emoji);
    break;
  case PROP_REACTION_CREATED_AT:
    g_value_set_int64(value, self->created_at);
    break;
  case PROP_REACTION_ROOM_ID:
    g_value_set_string(value, self->room_id);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
gh_reaction_class_init(GhReactionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = gh_reaction_finalize;
  object_class->get_property = gh_reaction_get_property;

  reaction_props[PROP_REACTION_TARGET_RUMOR_ID] =
    g_param_spec_string("target-rumor-id", NULL, NULL, NULL,
                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  reaction_props[PROP_REACTION_REACTION_RUMOR_ID] =
    g_param_spec_string("reaction-rumor-id", NULL, NULL, NULL,
                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  reaction_props[PROP_REACTION_SENDER] =
    g_param_spec_string("sender", NULL, NULL, NULL,
                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  reaction_props[PROP_REACTION_EMOJI] =
    g_param_spec_string("emoji", NULL, NULL, NULL,
                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  reaction_props[PROP_REACTION_CREATED_AT] =
    g_param_spec_int64("created-at", NULL, NULL, G_MININT64, G_MAXINT64, 0,
                       G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  reaction_props[PROP_REACTION_ROOM_ID] =
    g_param_spec_string("room-id", NULL, NULL, NULL,
                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(object_class, N_REACTION_PROPS, reaction_props);
}

static void
gh_reaction_init(GhReaction *self)
{
  (void)self;
}

GhReaction *
gh_reaction_new(const gchar *target_rumor_id, const gchar *reaction_rumor_id,
                const gchar *sender_pubkey, const gchar *emoji,
                gint64 created_at, const gchar *room_id)
{
  g_return_val_if_fail(target_rumor_id != NULL, NULL);
  g_return_val_if_fail(reaction_rumor_id != NULL, NULL);
  g_return_val_if_fail(sender_pubkey != NULL, NULL);
  g_return_val_if_fail(room_id != NULL, NULL);

  const gchar *content = (emoji && *emoji) ? emoji : "+";
  if (strlen(content) > GH_REACTION_MAX_EMOJI)
    return NULL;

  GhReaction *self = g_object_new(GH_TYPE_REACTION, NULL);
  self->target_rumor_id = g_strdup(target_rumor_id);
  self->reaction_rumor_id = g_strdup(reaction_rumor_id);
  self->sender_pubkey = g_strdup(sender_pubkey);
  self->emoji = g_strdup(content);
  self->created_at = created_at;
  self->room_id = g_strdup(room_id);
  return self;
}

const gchar *gh_reaction_get_target_rumor_id(GhReaction *self)
{ g_return_val_if_fail(GH_IS_REACTION(self), NULL); return self->target_rumor_id; }
const gchar *gh_reaction_get_reaction_rumor_id(GhReaction *self)
{ g_return_val_if_fail(GH_IS_REACTION(self), NULL); return self->reaction_rumor_id; }
const gchar *gh_reaction_get_sender(GhReaction *self)
{ g_return_val_if_fail(GH_IS_REACTION(self), NULL); return self->sender_pubkey; }
const gchar *gh_reaction_get_emoji(GhReaction *self)
{ g_return_val_if_fail(GH_IS_REACTION(self), NULL); return self->emoji; }
gint64 gh_reaction_get_created_at(GhReaction *self)
{ g_return_val_if_fail(GH_IS_REACTION(self), 0); return self->created_at; }
const gchar *gh_reaction_get_room_id(GhReaction *self)
{ g_return_val_if_fail(GH_IS_REACTION(self), NULL); return self->room_id; }

/* ---- GhReactionChip -------------------------------------------------------- */

void
gh_reaction_chip_free(GhReactionChip *chip)
{
  if (!chip)
    return;
  g_free(chip->emoji);
  if (chip->reactors)
    g_ptr_array_unref(chip->reactors);
  g_free(chip);
}

/* ---- GhReactionSummary ----------------------------------------------------- */

struct _GhReactionSummary {
  GObject parent_instance;
  gchar *account_pubkey;     /* the active account, for is_own */
  GPtrArray *reactions;      /* (element-type GhReaction) all reactions */
  GPtrArray *chips;          /* (element-type GhReactionChip) aggregated; rebuilt on change */
  guint total_count;
  gboolean has_own;
  gboolean chips_dirty;
};

enum {
  PROP_SUMMARY_0,
  PROP_SUMMARY_TOTAL_COUNT,
  PROP_SUMMARY_HAS_OWN,
  N_SUMMARY_PROPS
};
static GParamSpec *summary_props[N_SUMMARY_PROPS];

G_DEFINE_FINAL_TYPE(GhReactionSummary, gh_reaction_summary, G_TYPE_OBJECT)

static void
gh_reaction_summary_finalize(GObject *object)
{
  GhReactionSummary *self = GH_REACTION_SUMMARY(object);
  g_free(self->account_pubkey);
  g_ptr_array_unref(self->reactions);
  if (self->chips)
    g_ptr_array_unref(self->chips);
  G_OBJECT_CLASS(gh_reaction_summary_parent_class)->finalize(object);
}

static void
gh_reaction_summary_get_property(GObject *object, guint prop_id, GValue *value,
                                 GParamSpec *pspec)
{
  GhReactionSummary *self = GH_REACTION_SUMMARY(object);
  switch (prop_id) {
  case PROP_SUMMARY_TOTAL_COUNT:
    g_value_set_uint(value, self->total_count);
    break;
  case PROP_SUMMARY_HAS_OWN:
    g_value_set_boolean(value, self->has_own);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
gh_reaction_summary_class_init(GhReactionSummaryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = gh_reaction_summary_finalize;
  object_class->get_property = gh_reaction_summary_get_property;

  summary_props[PROP_SUMMARY_TOTAL_COUNT] =
    g_param_spec_uint("total-count", NULL, NULL, 0, G_MAXUINT, 0,
                      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  summary_props[PROP_SUMMARY_HAS_OWN] =
    g_param_spec_boolean("has-own", NULL, NULL, FALSE,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(object_class, N_SUMMARY_PROPS, summary_props);
}

static void
gh_reaction_summary_init(GhReactionSummary *self)
{
  self->reactions = g_ptr_array_new_with_free_func(g_object_unref);
}

/* Internal: create a summary for one target message of one account. */
GhReactionSummary *
gh_reaction_summary_new(const gchar *account_pubkey)
{
  GhReactionSummary *self = g_object_new(GH_TYPE_REACTION_SUMMARY, NULL);
  self->account_pubkey = g_strdup(account_pubkey);
  return self;
}

static void
rebuild_chips(GhReactionSummary *self)
{
  if (!self->chips_dirty)
    return;
  self->chips_dirty = FALSE;

  if (self->chips)
    g_ptr_array_unref(self->chips);
  self->chips = g_ptr_array_new_with_free_func((GDestroyNotify)gh_reaction_chip_free);

  /* Aggregate by emoji, preserving first-seen order. */
  GHashTable *emoji_to_idx = g_hash_table_new(g_str_hash, g_str_equal);
  for (guint i = 0; i < self->reactions->len; i++) {
    GhReaction *r = g_ptr_array_index(self->reactions, i);
    const gchar *emoji = gh_reaction_get_emoji(r);
    gpointer val;
    if (g_hash_table_lookup_extended(emoji_to_idx, emoji, NULL, &val)) {
      guint idx = GPOINTER_TO_UINT(val);
      GhReactionChip *chip = g_ptr_array_index(self->chips, idx);
      chip->count++;
      if (!chip->is_own && self->account_pubkey &&
          g_strcmp0(gh_reaction_get_sender(r), self->account_pubkey) == 0)
        chip->is_own = TRUE;
      g_ptr_array_add(chip->reactors, g_strdup(gh_reaction_get_sender(r)));
    } else {
      GhReactionChip *chip = g_new0(GhReactionChip, 1);
      chip->emoji = g_strdup(emoji);
      chip->count = 1;
      chip->is_own = self->account_pubkey &&
                     g_strcmp0(gh_reaction_get_sender(r), self->account_pubkey) == 0;
      chip->reactors = g_ptr_array_new_with_free_func(g_free);
      g_ptr_array_add(chip->reactors, g_strdup(gh_reaction_get_sender(r)));
      g_hash_table_insert(emoji_to_idx, (gpointer)emoji, GUINT_TO_POINTER(self->chips->len));
      g_ptr_array_add(self->chips, chip);
    }
  }
  g_hash_table_unref(emoji_to_idx);
}

static void
recount(GhReactionSummary *self)
{
  guint old_count = self->total_count;
  gboolean old_own = self->has_own;

  self->total_count = self->reactions->len;
  self->has_own = FALSE;
  for (guint i = 0; i < self->reactions->len; i++) {
    GhReaction *r = g_ptr_array_index(self->reactions, i);
    if (self->account_pubkey &&
        g_strcmp0(gh_reaction_get_sender(r), self->account_pubkey) == 0) {
      self->has_own = TRUE;
      break;
    }
  }
  self->chips_dirty = TRUE;

  if (old_count != self->total_count)
    g_object_notify_by_pspec(G_OBJECT(self), summary_props[PROP_SUMMARY_TOTAL_COUNT]);
  if (old_own != self->has_own)
    g_object_notify_by_pspec(G_OBJECT(self), summary_props[PROP_SUMMARY_HAS_OWN]);
}

/* Internal: add a reaction to this summary. */
void
gh_reaction_summary_add(GhReactionSummary *self, GhReaction *reaction)
{
  g_return_if_fail(GH_IS_REACTION_SUMMARY(self));
  g_return_if_fail(GH_IS_REACTION(reaction));

  /* Dedup by reaction_rumor_id. */
  const gchar *rid = gh_reaction_get_reaction_rumor_id(reaction);
  for (guint i = 0; i < self->reactions->len; i++) {
    GhReaction *existing = g_ptr_array_index(self->reactions, i);
    if (g_strcmp0(gh_reaction_get_reaction_rumor_id(existing), rid) == 0)
      return;
  }

  g_ptr_array_add(self->reactions, g_object_ref(reaction));
  recount(self);
}

/* Internal: remove a reaction by its rumor id. Returns TRUE if found. */
gboolean
gh_reaction_summary_remove(GhReactionSummary *self, const gchar *reaction_rumor_id)
{
  g_return_val_if_fail(GH_IS_REACTION_SUMMARY(self), FALSE);
  for (guint i = 0; i < self->reactions->len; i++) {
    GhReaction *r = g_ptr_array_index(self->reactions, i);
    if (g_strcmp0(gh_reaction_get_reaction_rumor_id(r), reaction_rumor_id) == 0) {
      g_ptr_array_remove_index(self->reactions, i);
      recount(self);
      return TRUE;
    }
  }
  return FALSE;
}

guint
gh_reaction_summary_get_total_count(GhReactionSummary *self)
{
  g_return_val_if_fail(GH_IS_REACTION_SUMMARY(self), 0);
  return self->total_count;
}

gboolean
gh_reaction_summary_get_has_own(GhReactionSummary *self)
{
  g_return_val_if_fail(GH_IS_REACTION_SUMMARY(self), FALSE);
  return self->has_own;
}

const GPtrArray *
gh_reaction_summary_get_chips(GhReactionSummary *self)
{
  g_return_val_if_fail(GH_IS_REACTION_SUMMARY(self), NULL);
  rebuild_chips(self);
  return self->chips->len > 0 ? self->chips : NULL;
}

const gchar *
gh_reaction_summary_own_reaction_id(GhReactionSummary *self, const gchar *emoji)
{
  g_return_val_if_fail(GH_IS_REACTION_SUMMARY(self), NULL);
  if (!self->account_pubkey)
    return NULL;
  for (guint i = 0; i < self->reactions->len; i++) {
    GhReaction *r = g_ptr_array_index(self->reactions, i);
    if (g_strcmp0(gh_reaction_get_sender(r), self->account_pubkey) == 0 &&
        g_strcmp0(gh_reaction_get_emoji(r), emoji) == 0)
      return gh_reaction_get_reaction_rumor_id(r);
  }
  return NULL;
}
