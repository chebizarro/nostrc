#ifndef NOSTR_NIP10_H
#define NOSTR_NIP10_H

#include "nostr-tag.h"

// Function prototypes for NIP-10
NostrTag* get_thread_root(NostrTags* tags);
NostrTag* get_immediate_reply(NostrTags* tags);

#endif // NOSTR_NIP10_H
