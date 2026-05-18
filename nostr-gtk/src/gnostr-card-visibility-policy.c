#include "gnostr-card-visibility-policy.h"

struct _GnostrCardVisibilityPolicy {
  GObject parent_instance;

  char *viewer_pubkey_hex;
  GHashTable *followed_pubkeys;
};

G_DEFINE_TYPE(GnostrCardVisibilityPolicy, gnostr_card_visibility_policy, G_TYPE_OBJECT)

static void
add_followed_pubkey(gpointer data, gpointer user_data)
{
  const char *pubkey_hex = data;
  GHashTable *table = user_data;

  if (pubkey_hex && *pubkey_hex)
    g_hash_table_add(table, g_strdup(pubkey_hex));
}

static GHashTable *
build_follow_snapshot_table(const char * const *followed_pubkeys)
{
  GHashTable *table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  if (!followed_pubkeys)
    return table;

  for (gsize i = 0; followed_pubkeys[i] != NULL; i++)
    add_followed_pubkey((gpointer)followed_pubkeys[i], table);

  return table;
}

static gboolean
follow_snapshot_equal(GHashTable *a, GHashTable *b)
{
  if (a == b)
    return TRUE;
  if (!a || !b)
    return FALSE;
  if (g_hash_table_size(a) != g_hash_table_size(b))
    return FALSE;

  GHashTableIter iter;
  gpointer key = NULL;
  g_hash_table_iter_init(&iter, a);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    if (!g_hash_table_contains(b, key))
      return FALSE;
  }

  return TRUE;
}

static void
gnostr_card_visibility_policy_finalize(GObject *object)
{
  GnostrCardVisibilityPolicy *self = GNOSTR_CARD_VISIBILITY_POLICY(object);

  g_clear_pointer(&self->viewer_pubkey_hex, g_free);
  g_clear_pointer(&self->followed_pubkeys, g_hash_table_unref);

  G_OBJECT_CLASS(gnostr_card_visibility_policy_parent_class)->finalize(object);
}

static void
gnostr_card_visibility_policy_class_init(GnostrCardVisibilityPolicyClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = gnostr_card_visibility_policy_finalize;
}

static void
gnostr_card_visibility_policy_init(GnostrCardVisibilityPolicy *self)
{
  self->followed_pubkeys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

GnostrCardVisibilityPolicy *
gnostr_card_visibility_policy_new(void)
{
  return g_object_new(GNOSTR_TYPE_CARD_VISIBILITY_POLICY, NULL);
}

gboolean
gnostr_card_visibility_policy_replace_follow_snapshot(
    GnostrCardVisibilityPolicy *self,
    const char *viewer_pubkey_hex,
    const char * const *followed_pubkeys)
{
  g_return_val_if_fail(GNOSTR_IS_CARD_VISIBILITY_POLICY(self), FALSE);

  GHashTable *new_followed = build_follow_snapshot_table(followed_pubkeys);
  gboolean changed = (g_strcmp0(self->viewer_pubkey_hex, viewer_pubkey_hex) != 0) ||
                     !follow_snapshot_equal(self->followed_pubkeys, new_followed);

  if (changed) {
    g_free(self->viewer_pubkey_hex);
    self->viewer_pubkey_hex = g_strdup(viewer_pubkey_hex);

    g_hash_table_unref(self->followed_pubkeys);
    self->followed_pubkeys = g_steal_pointer(&new_followed);
  }

  g_clear_pointer(&new_followed, g_hash_table_unref);
  return changed;
}

gboolean
gnostr_card_visibility_policy_should_show(GnostrCardVisibilityPolicy *self,
                                           const char *author_pubkey_hex,
                                           gboolean has_profile,
                                           gboolean explicitly_loaded)
{
  if (explicitly_loaded || has_profile)
    return TRUE;

  if (!self)
    return FALSE;

  g_return_val_if_fail(GNOSTR_IS_CARD_VISIBILITY_POLICY(self), FALSE);

  if (!author_pubkey_hex || !*author_pubkey_hex || !self->followed_pubkeys)
    return FALSE;

  return g_hash_table_contains(self->followed_pubkeys, author_pubkey_hex);
}
