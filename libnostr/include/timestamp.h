#ifndef TIMESTAMP_H
#define TIMESTAMP_H

#include <stdint.h>
#include <time.h>

typedef int64_t Timestamp;

Timestamp Now();
time_t TimestampToTime(Timestamp t);

#endif // TIMESTAMP_H