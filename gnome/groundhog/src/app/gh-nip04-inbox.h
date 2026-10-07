#pragma once
#include "gh-account-controller.h"
#include "gh-account-relays.h"
#include "gh-conversation-store.h"
#include "gh-dm-inbox.h"
#include "gh-relay-scope.h"

G_BEGIN_DECLS

/* Older NIP-04 direct messages (kind 4), shown read-only (W33, owner
 * decision 2026-10-07). Many people still use clients that send them
 * (Primal, older Damus and Iris); without this, a conversation with them
 * shows only your half.
 *
 * What it does. While the NIP-17 inbox runs (an account active, its store
 * open), it asks the account's own NIP-65 read and write relays for kind 4
 * events addressed to the account (#p) and written by it (authors), from
 * 30 days back. Each event is verified; one already stored is skipped before
 * any signer call; the rest are decrypted one at a time through the signer
 * (Grotto asks, once per app with "remember"). A decrypted message enters
 * the same 1:1 conversation as NIP-17 messages, as a local kind-14 rumor
 * carrying [GH_MESSAGE_LEGACY_TAG, "nip04", <event id>] (never published),
 * and is shown marked as less private.
 *
 * What it never does. It never sends NIP-04: replies are NIP-17. It never
 * decrypts a message twice (the kind-4 id is the store's dedup key). A
 * denied decryption pauses it for the session. */
#define GH_TYPE_NIP04_INBOX (gh_nip04_inbox_get_type())
G_DECLARE_FINAL_TYPE(GhNip04Inbox, gh_nip04_inbox, GH, NIP04_INBOX, GObject)

#define GH_NIP04_INBOX_BACKFILL ((gint64)30 * 24 * 3600)

GhNip04Inbox *gh_nip04_inbox_new(GhAccountController *accounts, GhAccountRelays *relays,
                                 GhConversationStore *store, GhDmInbox *dm_inbox,
                                 const GhRelayTransport *transport, gpointer transport_data);
/* Messages admitted, and kind-4 events seen, this session (tests, About). */
guint gh_nip04_inbox_get_admitted(GhNip04Inbox *self);

G_END_DECLS
