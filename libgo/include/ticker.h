#ifndef GO_TICKER_H
#define GO_TICKER_H

#include "libgo-export.h"

#include "channel.h"
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <nsync.h>

typedef struct Ticker {
    GoChannel *c;
    size_t interval_ms;
    pthread_t thread;
    _Atomic bool stop;
    nsync_mu mutex;
} Ticker;

LIBGO_API Ticker *create_ticker(size_t interval_ms);
LIBGO_API void stop_ticker(Ticker *ticker);

#endif // GO_TICKER_H
