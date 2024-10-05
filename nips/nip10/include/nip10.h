#ifndef NOSTR_NIP10_H
#define NOSTR_NIP10_H

#include "event.h"

// Function prototypes for NIP-10
Tag* get_thread_root(Tags* tags);
Tag* get_immediate_reply(Tags* tags);

#endif // NOSTR_NIP10_H
