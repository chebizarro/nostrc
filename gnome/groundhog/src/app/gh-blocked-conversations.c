#include "gh-blocked-conversations.h"
#include "gh-conversation-actions.h"

#include <nostr-utils.h>
#include <nostr/nip19/nip19.h>
#include <stdlib.h>

static gchar *
npub_of(const gchar *pubkey_hex)
{
  guint8 bytes[32];
  char *npub = NULL;
  if (!nostr_hex2bin(bytes, pubkey_hex, sizeof bytes) ||
      nostr_nip19_encode_npub(bytes, &npub) != 0 || !npub)
    return g_strdup(pubkey_hex);
  gchar *out = g_strdup(npub);
  free(npub);
  return out;
}

GPtrArray *
gh_blocked_conversations_list(GhStoreConversations *conversations, const gchar *account_pubkey,
                              GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_CONVERSATIONS(conversations), NULL);
  g_return_val_if_fail(account_pubkey != NULL, NULL);
  g_autoptr(GPtrArray) rooms = gh_store_conversations_list_blocked(conversations, error);
  if (!rooms)
    return NULL;
  GPtrArray *blocked =
    g_ptr_array_new_with_free_func((GDestroyNotify)gh_blocked_conversation_free);
  for (guint i = 0; i < rooms->len; i++) {
    const GhStoreBlockedRoom *room = g_ptr_array_index(rooms, i);
    g_auto(GStrv) participants = g_strsplit(room->room_id, ",", -1);
    g_autoptr(GStrvBuilder) npubs = g_strv_builder_new();
    for (guint p = 0; participants[p]; p++) {
      if (g_ascii_strcasecmp(participants[p], account_pubkey) == 0)
        continue;
      g_autofree gchar *npub = npub_of(participants[p]);
      g_strv_builder_add(npubs, npub);
    }
    GhBlockedConversation *conversation = g_new0(GhBlockedConversation, 1);
    conversation->room_id = g_strdup(room->room_id);
    conversation->npubs = g_strv_builder_end(npubs);
    conversation->has_messages = room->has_messages;
    g_ptr_array_add(blocked, conversation);
  }
  return blocked;
}

gboolean
gh_blocked_conversations_unblock(GhStoreConversations *conversations, const gchar *room_id,
                                 GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_CONVERSATIONS(conversations), FALSE);
  /* Unblocking withdraws nothing: there is no notifier to tell. */
  return gh_conversation_actions_block(conversations, NULL, room_id, FALSE, error);
}
