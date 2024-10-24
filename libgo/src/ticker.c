#include "ticker.h"
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>

void *ticker_thread_func(void *arg) {
    Ticker *ticker = (Ticker *)arg;

    while (true) {
        nsync_mu_lock(&ticker->mutex); // Lock mutex
        if (ticker->stop) {
            nsync_mu_unlock(&ticker->mutex); // Unlock mutex before exiting
            break;
        }
        nsync_mu_unlock(&ticker->mutex); // Unlock mutex

        usleep(ticker->interval_ms * 1000); // Sleep for the specified interval

        // Send a tick (e.g., an empty signal) to the channel
        go_channel_send(ticker->c, NULL); // Just send a signal, no actual data
    }

    return NULL;
}

Ticker *create_ticker(size_t interval_ms) {
    Ticker *ticker = (Ticker *)malloc(sizeof(Ticker));
    ticker->interval_ms = interval_ms;
    ticker->c = go_channel_create(1); // Channel with capacity 1 for ticks
    ticker->stop = false;
    nsync_mu_init(&ticker->mutex); // Initialize mutex

    pthread_create(&ticker->thread, NULL, ticker_thread_func, ticker);
    return ticker;
}

void stop_ticker(Ticker *ticker) {
    if (ticker == NULL) {
        return; // Ensure ticker is not NULL
    }

    nsync_mu_lock(&ticker->mutex); // Lock mutex to safely update the stop flag
    ticker->stop = true;
    nsync_mu_unlock(&ticker->mutex); // Unlock mutex

    if (ticker->thread) {
        int result = pthread_join(ticker->thread, NULL);
        if (result == 0) {
            printf("Ticker thread joined successfully.\n");
        } else if (result == ESRCH) {
            printf("Ticker thread already terminated.\n");
        } else {
            printf("Error joining ticker thread: %d\n", result);
        }
    }

    if (ticker->c) {
        go_channel_free(ticker->c);  // Ensure ticker channel is freed only if valid
    }
    free(ticker); // Free the ticker structure
}
