#ifndef NOSTR_TIMESTAMP_H
#define NOSTR_TIMESTAMP_H

#include <stdint.h>
#include <time.h>

typedef int64_t Timestamp;

Timestamp Now();
time_t TimestampToTime(Timestamp t);

#endif // NOSTR_TIMESTAMP_H