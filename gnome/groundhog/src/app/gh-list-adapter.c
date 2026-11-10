#include "gh-list-adapter.h"

struct _GhListAdapter {
  GhStore *store;
  gchar *account;
  guint64 generation;
};

GhListAdapter *
gh_list_adapter_new(GhStore *store, guint64 generation)
{
  GhListAdapter *self = g_new0(GhListAdapter, 1);
  self->store = store;
  self->account = g_strdup(gh_store_get_account_pubkey(store));
  self->generation = generation;
  return self;
}

void
gh_list_adapter_revoke(GhListAdapter *self)
{
  self->store = NULL;
}

void
gh_list_adapter_free(GhListAdapter *self)
{
  g_free(self->account);
  g_free(self);
}

static gboolean
generation_valid(gpointer data, const char *account, guint64 generation)
{
  GhListAdapter *self = data;
  return self->store && self->generation == generation && !g_strcmp0(self->account, account);
}

static gchar *
load(gpointer data, gint kind, const gchar *identifier, GError **error)
{
  GhListAdapter *self = data;
  if (!self->store) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Account list storage closed");
    return NULL;
  }
  return gh_store_social_load(self->store, kind, identifier, error);
}

static gboolean
commit(gpointer data, GNostrListDocument *snapshot, const gchar *state, GError **error)
{
  GhListAdapter *self = data;
  if (!self->store) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Account list storage closed");
    return FALSE;
  }
  return gh_store_social_commit(self->store, snapshot, state, error);
}

static gint64
now(gpointer data)
{
  GhListAdapter *self = data;
  return self->store ? gh_clock_get_unix(gh_store_get_clock(self->store)) : 0;
}

const GNostrListAdapter *
gh_list_adapter_get_callbacks(void)
{
  static const GNostrListAdapter callbacks = {
    .generation_valid = generation_valid, .load = load,
    .commit_snapshot = commit, .now = now,
  };
  return &callbacks;
}
