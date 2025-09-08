#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include "ticker.h"
#include <pthread.h>

void *ticker_thread_func(void *arg) {
    Ticker *ticker = (Ticker *)arg;

    while (true) {
        if (atomic_load_explicit(&ticker->stop, memory_order_acquire)) {
            break;
        }

        struct timespec ts;
        ts.tv_sec = ticker->interval_ms / 1000;
        ts.tv_nsec = (long)(ticker->interval_ms % 1000) * 1000000L;
        nanosleep(&ts, NULL); // Sleep for the specified interval

        // Non-blocking send; drop tick if channel is full
        (void)go_channel_try_send(ticker->c, NULL);
    }

    return NULL;
}

Ticker *create_ticker(size_t interval_ms) {
    Ticker *ticker = (Ticker *)malloc(sizeof(Ticker));
    ticker->interval_ms = interval_ms;
    ticker->c = go_channel_create(1); // Channel with capacity 1 for ticks
    atomic_store_explicit(&ticker->stop, false, memory_order_relaxed);
    nsync_mu_init(&ticker->mutex); // retained for struct compatibility

    pthread_create(&ticker->thread, NULL, ticker_thread_func, ticker);
    return ticker;
}

void stop_ticker(Ticker *ticker) {
    if (ticker == NULL) {
        return; // Ensure ticker is not NULL
    }

    atomic_store_explicit(&ticker->stop, true, memory_order_release);

    if (ticker->thread) {
        (void)pthread_join(ticker->thread, NULL);
    }

    if (ticker->c) {
        go_channel_free(ticker->c);  // Ensure ticker channel is freed only if valid
    }
    free(ticker); // Free the ticker structure
}
