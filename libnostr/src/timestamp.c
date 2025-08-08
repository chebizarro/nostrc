#include "nostr.h"

Timestamp Now(void) {
    return (Timestamp)time(NULL);
}

time_t TimestampToTime(Timestamp t) {
    return (time_t)t;
}