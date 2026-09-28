#include "gh-inbox-resolver.h"

G_DEFINE_INTERFACE(GhInboxResolver, gh_inbox_resolver, G_TYPE_OBJECT)

static void
gh_inbox_resolver_default_init(GhInboxResolverInterface *iface)
{
  (void)iface;
}

GhInboxResult *
gh_inbox_result_copy(const GhInboxResult *result)
{
  g_return_val_if_fail(result != NULL, NULL);
  GhInboxResult *copy = g_new0(GhInboxResult, 1);
  *copy = *result;
  copy->recipient = g_strdup(result->recipient);
  copy->relays = g_strdupv(result->relays);
  copy->event_id = g_strdup(result->event_id);
  return copy;
}

void
gh_inbox_result_free(GhInboxResult *result)
{
  if (!result)
    return;
  g_free(result->recipient);
  g_strfreev(result->relays);
  g_free(result->event_id);
  g_free(result);
}

void
gh_inbox_resolver_resolve_async(GhInboxResolver *self, const gchar *pubkey_hex,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_INBOX_RESOLVER(self));
  GH_INBOX_RESOLVER_GET_IFACE(self)->resolve_async(self, pubkey_hex, cancellable,
                                                   callback, user_data);
}

GhInboxResult *
gh_inbox_resolver_resolve_finish(GhInboxResolver *self, GAsyncResult *result,
                                 GError **error)
{
  g_return_val_if_fail(GH_IS_INBOX_RESOLVER(self), NULL);
  return GH_INBOX_RESOLVER_GET_IFACE(self)->resolve_finish(self, result, error);
}

void
gh_inbox_resolver_forget(GhInboxResolver *self, const gchar *pubkey_hex)
{
  g_return_if_fail(GH_IS_INBOX_RESOLVER(self));
  GhInboxResolverInterface *iface = GH_INBOX_RESOLVER_GET_IFACE(self);
  if (iface->forget)
    iface->forget(self, pubkey_hex);
}
