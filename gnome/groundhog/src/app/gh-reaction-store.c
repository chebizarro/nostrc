#include "gh-reaction-store.h"

#include <string.h>

/* Internal declarations for GhReactionSummary used by the store. */
GhReactionSummary *gh_reaction_summary_new(const gchar *account_pubkey);
void gh_reaction_summary_add(GhReactionSummary *self, GhReaction *reaction);
gboolean gh_reaction_summary_remove(GhReactionSummary *self, const gchar *reaction_rumor_id);

struct _GhReactionStore {
  GObject parent_instance;
  gchar *account_pubkey;

  /* target_rumor_id → GhReactionSummary (owned). */
  GHashTable *summaries;
  /* reaction_rumor_id → target_rumor_id (borrowed from the reaction). */
  GHashTable *reaction_to_target;
  /* W26 slice B review fix (F3): reaction_rumor_id → sender_pubkey. */
  GHashTable *reaction_to_sender;

  const GhReactionDelegate *delegate;
  gpointer delegate_data;
  GDestroyNotify delegate_destroy;

  /* W26 slice B review fix (F5): saved delegate during suspend. */
  const GhReactionDelegate *suspended_delegate;
  gpointer suspended_data;
};

enum {
  SIGNAL_REACTION_CHANGED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhReactionStore, gh_reaction_store, G_TYPE_OBJECT)

static void
clear_delegate(GhReactionStore *self)
{
  if (self->delegate_destroy && self->delegate_data)
    self->delegate_destroy(self->delegate_data);
  self->delegate = NULL;
  self->delegate_data = NULL;
  self->delegate_destroy = NULL;
}

static void
gh_reaction_store_finalize(GObject *object)
{
  GhReactionStore *self = GH_REACTION_STORE(object);
  clear_delegate(self);
  g_free(self->account_pubkey);
  g_hash_table_unref(self->summaries);
  g_hash_table_unref(self->reaction_to_target);
  g_hash_table_unref(self->reaction_to_sender);
  G_OBJECT_CLASS(gh_reaction_store_parent_class)->finalize(object);
}

static void
gh_reaction_store_class_init(GhReactionStoreClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = gh_reaction_store_finalize;

  signals[SIGNAL_REACTION_CHANGED] =
    g_signal_new("reaction-changed",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gh_reaction_store_init(GhReactionStore *self)
{
  self->summaries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  self->reaction_to_target = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  self->reaction_to_sender = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

GhReactionStore *
gh_reaction_store_new(void)
{
  return g_object_new(GH_TYPE_REACTION_STORE, NULL);
}

void
gh_reaction_store_set_account(GhReactionStore *self,
                              const gchar *account_pubkey,
                              const GhReactionDelegate *delegate,
                              gpointer delegate_data,
                              GDestroyNotify destroy)
{
  g_return_if_fail(GH_IS_REACTION_STORE(self));

  if (g_strcmp0(self->account_pubkey, account_pubkey) != 0) {
    g_hash_table_remove_all(self->summaries);
    g_hash_table_remove_all(self->reaction_to_target);
    g_hash_table_remove_all(self->reaction_to_sender);
  }

  clear_delegate(self);
  g_free(self->account_pubkey);
  self->account_pubkey = g_strdup(account_pubkey);
  self->delegate = delegate;
  self->delegate_data = delegate_data;
  self->delegate_destroy = destroy;
}

/* W26 slice B review fix (F5): temporarily NULL out the delegate so that
 * bulk admit() calls (the restore loop) skip the INSERT OR IGNORE. */
void
gh_reaction_store_suspend_delegate(GhReactionStore *self)
{
  g_return_if_fail(GH_IS_REACTION_STORE(self));
  self->suspended_delegate = self->delegate;
  self->suspended_data = self->delegate_data;
  self->delegate = NULL;
  self->delegate_data = NULL;
}

void
gh_reaction_store_resume_delegate(GhReactionStore *self)
{
  g_return_if_fail(GH_IS_REACTION_STORE(self));
  self->delegate = self->suspended_delegate;
  self->delegate_data = self->suspended_data;
  self->suspended_delegate = NULL;
  self->suspended_data = NULL;
}

static GhReactionSummary *
ensure_summary(GhReactionStore *self, const gchar *target_rumor_id)
{
  GhReactionSummary *summary = g_hash_table_lookup(self->summaries, target_rumor_id);
  if (!summary) {
    summary = gh_reaction_summary_new(self->account_pubkey);
    g_hash_table_insert(self->summaries, g_strdup(target_rumor_id), summary);
  }
  return summary;
}

gboolean
gh_reaction_store_admit(GhReactionStore *self, GhReaction *reaction, GError **error)
{
  g_return_val_if_fail(GH_IS_REACTION_STORE(self), FALSE);
  g_return_val_if_fail(GH_IS_REACTION(reaction), FALSE);

  const gchar *rid = gh_reaction_get_reaction_rumor_id(reaction);
  const gchar *tid = gh_reaction_get_target_rumor_id(reaction);

  /* Already known? */
  if (g_hash_table_contains(self->reaction_to_target, rid))
    return FALSE;

  /* Persist first (inside the caller's transaction). */
  if (self->delegate && self->delegate->admit) {
    if (!self->delegate->admit(self->delegate_data, reaction, error))
      return FALSE;
  }

  /* In-memory model. */
  GhReactionSummary *summary = ensure_summary(self, tid);
  gh_reaction_summary_add(summary, reaction);
  g_hash_table_insert(self->reaction_to_target, g_strdup(rid), g_strdup(tid));
  g_hash_table_insert(self->reaction_to_sender, g_strdup(rid),
                       g_strdup(gh_reaction_get_sender(reaction)));

  g_signal_emit(self, signals[SIGNAL_REACTION_CHANGED], 0, tid);
  return TRUE;
}

gboolean
gh_reaction_store_remove(GhReactionStore *self, const gchar *reaction_rumor_id,
                         GError **error)
{
  g_return_val_if_fail(GH_IS_REACTION_STORE(self), FALSE);
  g_return_val_if_fail(reaction_rumor_id != NULL, FALSE);

  gchar *target = g_strdup(g_hash_table_lookup(self->reaction_to_target, reaction_rumor_id));
  if (!target)
    return FALSE;

  /* Persist removal. */
  if (self->delegate && self->delegate->remove) {
    if (!self->delegate->remove(self->delegate_data, reaction_rumor_id, error)) {
      g_free(target);
      return FALSE;
    }
  }

  GhReactionSummary *summary = g_hash_table_lookup(self->summaries, target);
  if (summary)
    gh_reaction_summary_remove(summary, reaction_rumor_id);
  g_hash_table_remove(self->reaction_to_target, reaction_rumor_id);
  g_hash_table_remove(self->reaction_to_sender, reaction_rumor_id);

  g_signal_emit(self, signals[SIGNAL_REACTION_CHANGED], 0, target);
  g_free(target);
  return TRUE;
}

gchar *
gh_reaction_store_remove_own(GhReactionStore *self, const gchar *target_rumor_id,
                             const gchar *emoji, GError **error)
{
  g_return_val_if_fail(GH_IS_REACTION_STORE(self), NULL);
  g_return_val_if_fail(target_rumor_id != NULL, NULL);
  if (!self->account_pubkey)
    return NULL;

  GhReactionSummary *summary = g_hash_table_lookup(self->summaries, target_rumor_id);
  if (!summary)
    return NULL;

  const gchar *rid = gh_reaction_summary_own_reaction_id(summary, emoji);
  if (!rid)
    return NULL;

  gchar *result = g_strdup(rid);
  g_autoptr(GError) local_error = NULL;
  if (!gh_reaction_store_remove(self, result, error ? error : &local_error)) {
    g_free(result);
    return NULL;
  }
  return result;
}

GhReactionSummary *
gh_reaction_store_lookup(GhReactionStore *self, const gchar *target_rumor_id)
{
  g_return_val_if_fail(GH_IS_REACTION_STORE(self), NULL);
  g_return_val_if_fail(target_rumor_id != NULL, NULL);
  return ensure_summary(self, target_rumor_id);
}

/* W26 slice B review fix (F3): look up the sender pubkey of a reaction by
 * its rumor id, so kind-5 deletion handlers can verify author ownership. */
const gchar *
gh_reaction_store_get_sender(GhReactionStore *self, const gchar *reaction_rumor_id)
{
  g_return_val_if_fail(GH_IS_REACTION_STORE(self), NULL);
  g_return_val_if_fail(reaction_rumor_id != NULL, NULL);
  return g_hash_table_lookup(self->reaction_to_sender, reaction_rumor_id);
}
