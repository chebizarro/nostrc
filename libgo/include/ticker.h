#ifndef GO_TICKER_H
#define GO_TICKER_H

#include "channel.h"
#include "stdbool.h"

typedef struct Ticker {
    GoChannel *channel;
    size_t interval_ms;
    pthread_t thread;
    bool stop;
} Ticker;

Ticker *create_ticker(size_t interval_ms);
void stop_ticker(Ticker *ticker);

#endif // GO_TICKER_H