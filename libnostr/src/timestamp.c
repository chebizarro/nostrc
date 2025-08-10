#include "nostr.h"
#include "nostr-timestamp.h"

int64_t nostr_timestamp_now(void) {
    return (int64_t)time(NULL);
}

time_t nostr_timestamp_to_time(NostrTimestamp t) {
    return (time_t)t;
}
