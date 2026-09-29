#include "gh-conversation-actions.h"

static void
withdraw(GhNotifier *notifier, const gchar *room_id)
{
  if (notifier)
    gh_notifier_withdraw_conversation(notifier, room_id);
}

gboolean
gh_conversation_actions_mute(GhStoreConversations *conversations, GhNotifier *notifier,
                             const gchar *room_id, gint64 muted_until, GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_CONVERSATIONS(conversations), FALSE);
  g_return_val_if_fail(!notifier || GH_IS_NOTIFIER(notifier), FALSE);
  if (!gh_store_conversations_set_muted_until(conversations, room_id, muted_until, error))
    return FALSE;
  if (muted_until != 0)
    withdraw(notifier, room_id);
  return TRUE;
}

gboolean
gh_conversation_actions_block(GhStoreConversations *conversations, GhNotifier *notifier,
                              const gchar *room_id, gboolean blocked, GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_CONVERSATIONS(conversations), FALSE);
  g_return_val_if_fail(!notifier || GH_IS_NOTIFIER(notifier), FALSE);
  if (!gh_store_conversations_set_blocked(conversations, room_id, blocked, error))
    return FALSE;
  if (blocked)
    withdraw(notifier, room_id);
  return TRUE;
}

gboolean
gh_conversation_actions_forget(GhStoreConversations *conversations, GhNotifier *notifier,
                               const gchar *room_id, GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_CONVERSATIONS(conversations), FALSE);
  g_return_val_if_fail(!notifier || GH_IS_NOTIFIER(notifier), FALSE);
  if (!gh_store_conversations_forget(conversations, room_id, error))
    return FALSE;
  withdraw(notifier, room_id);
  return TRUE;
}
