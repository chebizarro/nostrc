#ifndef NIP55L_SIGNER_PERMISSION_POLICY_H
#define NIP55L_SIGNER_PERMISSION_POLICY_H
#include <nostr-kinds.h>

/* Owner-approved consent defaults. Explicit permissions always take precedence.
 * These kinds require a scoped first grant; presets alone do not grant them. */
static const guint32 signer_public_publishing_kinds[] = {
  NOSTR_KIND_TEXT_NOTE, NOSTR_KIND_REPOST, NOSTR_KIND_GENERIC_REPOST,
  NOSTR_KIND_REACTION, NOSTR_KIND_ARTICLE, NOSTR_KIND_COMMENT
};
static const guint32 signer_default_ask_by_default_kinds[] = {
  NOSTR_KIND_CLIENT_AUTHENTICATION, NOSTR_KIND_DELETION, NOSTR_KIND_ZAP_REQUEST,
  NOSTR_KIND_ENCRYPTED_DIRECT_MESSAGE, NOSTR_KIND_SEAL, NOSTR_KIND_DIRECT_MESSAGE,
  NOSTR_KIND_FILE_MESSAGE, NOSTR_KIND_GIFT_WRAP, NOSTR_KIND_FOLLOW_LIST,
  NOSTR_KIND_RELAY_LIST_METADATA, NOSTR_KIND_PROFILE_METADATA
};
#endif
