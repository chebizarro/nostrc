#include "ticker.h"
#include <pthread.h>
#include <unistd.h>

void *ticker_thread_func(void *arg) {
    Ticker *ticker = (Ticker *)arg;

    while (!ticker->stop) {
        usleep(ticker->interval_ms * 1000); // Sleep for the specified interval
        if (ticker->stop)
            break;

        // Send a tick (e.g., an empty signal) to the channel
        go_channel_send(ticker->c, NULL); // Just send a signal, no actual data
    }

    return NULL;
}

// Create a ticker that sends a tick every interval_ms milliseconds
Ticker *create_ticker(size_t interval_ms) {
    Ticker *ticker = (Ticker *)malloc(sizeof(Ticker));
    ticker->interval_ms = interval_ms;
    ticker->c = go_channel_create(1); // Channel with capacity 1 for ticks
    ticker->stop = false;

    pthread_create(&ticker->thread, NULL, ticker_thread_func, ticker);
    return ticker;
}

// Stop the ticker and clean up resources
void stop_ticker(Ticker *ticker) {
    ticker->stop = true;
    pthread_join(ticker->thread, NULL); // Wait for the thread to finish
    go_channel_free(ticker->c);
    free(ticker);
}
